# Pattern 14: Distributed Systems — Interview Study Guide

Reading Pattern 14: Distributed Systems — 5 problems, 8 shared components

---

## STEP 1 — PATTERN OVERVIEW

This pattern covers five canonical distributed systems problems that appear across virtually every senior and staff-level system design interview at FAANG and beyond. The five problems are: **Consistent Hashing**, **Distributed Cache**, **Distributed Lock**, **Distributed Message Queue**, and **Distributed Job Scheduler**. They are grouped together because they all share a common set of architectural concerns: how do you coordinate state across multiple machines reliably, with low latency, and without a single point of failure?

Each of the five is a building block that shows up as a sub-component in larger systems. If you get asked to design a URL shortener, the caching layer is a distributed cache. If you get asked to design a payments system, the idempotency check on write is backed by a distributed lock. If you get asked to design a ride-sharing platform, the driver-location fan-out is a message queue. Knowing these five cold means you can handle both standalone "design a distributed X" prompts and also slot these components intelligently into any larger design.

**The 8 shared components across all five problems:**
1. Stateless API / Service Layer + Persistent Backend
2. etcd / Raft-Based Coordination
3. Primary / Replica Replication
4. TTL / Lease-Based Expiry
5. Retry with Exponential Backoff + Jitter
6. Idempotency Mechanisms
7. In-Memory Data Structures for Hot-Path Operations
8. Admin API for Runtime Configuration

---

## STEP 2 — MENTAL MODEL

**The core idea:** Every distributed system problem reduces to the same fundamental tension — you have multiple machines that need to agree on something (which node owns this key, who holds this lock, which message has been processed, when this job should fire), and the network is unreliable, clocks drift, and processes crash at the worst possible moment. The entire design craft of distributed systems is about choosing which of the three — consistency, availability, and partition tolerance — to sacrifice at each layer, and then building the compensating mechanisms (retries, TTLs, fencing tokens, consensus protocols) for the gaps you left.

**Real-world analogy:** Think of distributed systems like a relay race across a stadium where the runners can't see each other and the baton sometimes disappears mid-hand-off. Consistent hashing is the lane assignment system — it tells each runner which lane to run in. The distributed cache is the water station — everyone knows roughly where it is, but sometimes the water has been drunk already (cache miss) or it's slightly stale (replication lag). The distributed lock is the starting pistol — only one runner fires at a time, but if the pistol misfires (lock holder crashes), you need the gun to reset automatically (TTL expiry) rather than freezing the whole race. The message queue is the race official's clipboard — every baton hand-off is written down in order, and late officials (consumers) can catch up by reading the clipboard from any point. The job scheduler is the stadium clock — it fires events at precise times regardless of which official happens to be watching at that moment.

**Why this is genuinely hard:**

Three compounding problems make distributed systems hard in a way that's easy to underestimate in an interview:

First, **partial failure** — in a distributed system, any component can fail independently, including the network between two healthy components. Unlike a single-process crash where everything stops together, partial failures leave the system in an ambiguous "did it or didn't it" state. A producer that never gets an acknowledgment doesn't know if the broker crashed after writing the message or before — so it must retry, which means duplicates, which means you need idempotency.

Second, **clock unsynchronization** — every machine's clock drifts. NTP corrections can cause a clock to jump forward or backward. This means that any algorithm that relies on wall-clock timestamps for ordering or for deciding "this lock has expired" is inherently unsafe. This is precisely why Redlock (Redis-based distributed locking) is considered unsafe by the academic community — it relies on synchronized clocks for its TTL guarantees — and why etcd uses its own Raft-committed revision number as a fencing token instead of a timestamp.

Third, **the split-brain dilemma** — when a network partition splits your cluster into two groups that can't see each other, both groups are alive and healthy from their own perspective. If both sides continue to accept writes, you get conflicting state that's hard to reconcile. If both sides stop accepting writes, you've sacrificed availability. The choice you make here (Redis Cluster stops writes on the minority side; Kafka prefers availability with `unclean.leader.election.enable=true`) defines the operational character of your system.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before drawing a single box, ask these questions. Each one changes the design in a material way.

**The four questions you must always ask:**

1. **"What is the consistency requirement — can clients see slightly stale data, or do all reads need to reflect the latest write?"**
   - Changes everything: stale-OK → async replication + read replicas is fine; strong consistency → Raft/Paxos and linearizable reads only, no read replicas without WAIT.
   - For cache: usually stale-OK. For lock service: must be strongly consistent. For job scheduler trigger claiming: must be atomic.

2. **"What is the failure model — do we need to tolerate node failures, network partitions, or both? What is the acceptable window for unavailability on failure?"**
   - Node failure only: primary/replica with fast failover works. Network partitions too: you need a consensus protocol (Raft, ZAB) or a deliberate CP/AP choice.
   - Acceptable unavailability window determines whether you need a hot standby (seconds) or just eventual recovery (minutes).

3. **"What are the throughput and latency requirements — specifically the peak QPS and P99 latency target?"**
   - This drives the number of nodes, whether in-process routing is needed (consistent hashing ring in L2 cache), whether you can afford a synchronous fsync (Kafka `acks=all` vs `acks=1`), and whether you need a proxy tier.

4. **"What does 'exactly-once' mean in this context — exactly-once delivery, exactly-once processing, or exactly-once side effects?"**
   - Exactly-once delivery: Kafka EOS with idempotent producer. Exactly-once processing: at-least-once delivery + idempotent consumer with dedup key. Exactly-once side effects (e.g., charge a credit card once): requires distributed transaction or outbox pattern.

**Fifth question for senior/staff roles:**

5. **"Is this a single-region or multi-region deployment, and if multi-region, is active-active or active-passive acceptable?"**
   - Multi-region active-active requires conflict resolution strategy (last-write-wins, CRDTs, or application-level merge). Most distributed lock and job scheduler designs break under active-active multi-region — this is a known hard problem worth flagging proactively.

**What changes based on answers:**
- Strong consistency required: switch from Redis to etcd. Use `WAIT 1 0` in Redis for near-sync. Use `acks=all` + `min.insync.replicas=2` in Kafka.
- Very high throughput (>1M ops/sec): add in-process caching tier (L1 Caffeine + L2 Redis). Consider consistent hashing for stateless routing.
- Sub-millisecond latency: in-memory only, no disk persistence, no synchronous replication. Accept the durability trade-off.
- Low throughput + correctness-critical: take the etcd/PostgreSQL path for everything. Don't over-engineer.

**Red flags (things that tell you the candidate is struggling):**
- Jumping straight to "I'll use Kafka" or "I'll use Redis" without asking what consistency/latency is needed. Both are wrong default answers — Redis is AP and loses data on failover without WAIT; Kafka has seconds-level end-to-end latency for low-throughput topics.
- Not asking about failure scenarios. A distributed system that only works when all nodes are up is just a regular system that happens to run on multiple machines.
- Conflating the lock service with the resource it protects. The lock service tells you who holds the lock; it cannot stop a zombie from acting. Only a fencing token validated at the resource can do that.

---

### 3b. Functional Requirements

**Consistent Hashing:**
- Core: Given a key, return the node responsible for it in O(log N). When a node is added or removed, only 1/N keys change assignment.
- Scope: This is a routing algorithm, not a data store. It doesn't handle replication, consistency, or the actual data — those are concerns of the system using the ring.
- Clear statement: "Design a consistent hash ring that distributes a keyspace across N nodes such that adding or removing a node moves the minimum possible number of keys, supports weighted nodes, and propagates ring topology changes to all clients within 5 seconds."

**Distributed Cache:**
- Core: Sub-millisecond get/set with TTL expiration. Support common data types (strings, hashes, sorted sets). Shard data across nodes. Tolerate node failure.
- Scope: Memory-first, not a database. Durability is configurable. Not strongly consistent across shards for multi-key transactions.
- Clear statement: "Design a distributed in-memory cache serving 1M ops/sec at P99 < 1ms read latency, supporting 200 GB hot dataset, with automatic sharding, replication, and configurable persistence."

**Distributed Lock:**
- Core: Exclusive, named locks. Only one holder at a time (safety). Automatic expiry on holder crash (liveness). Fencing tokens to prevent zombie writes.
- Scope: Advisory locks only. Single-datacenter. No read/write locks in base design.
- Clear statement: "Design a distributed lock service for 10,000 microservice instances that provides mutual exclusion with TTL-based lease expiry, fencing tokens, and wait-with-timeout semantics."

**Distributed Message Queue:**
- Core: Producers publish to topics, consumers read in order. Durable storage replicated across brokers. Consumer groups for parallel consumption. Offset-based replay.
- Scope: Ordered delivery within a partition. Not a task queue (no priority). Fan-out across unlimited consumer groups.
- Clear statement: "Design a distributed message queue handling 10 GB/sec write throughput with ordered delivery per partition, 7-day retention, consumer group offset management, and P99 end-to-end latency under 10ms."

**Distributed Job Scheduler:**
- Core: Cron and one-time job scheduling. Exactly-once firing per trigger across multiple scheduler nodes. Job status tracking. Retry with backoff and DLQ.
- Scope: Not a workflow engine (no complex branching beyond DAG). Not sub-second scheduling. Not dynamic code upload.
- Clear statement: "Design a distributed job scheduler for 100,000 cron jobs that guarantees exactly-once execution per trigger, supports priority levels and DAG dependencies, and handles worker failure with configurable retry."

---

### 3c. Non-Functional Requirements

NFRs are not a list to recite — they are derived from the problem and each one creates a trade-off you should name out loud.

**Throughput derivations (show your work):**
- Cache: 500M users × 20 ops/day = 10B ops/day ÷ 86,400 ≈ 115K ops/sec average. At 3× peak = 350K ops/sec. Provision 700K ops/sec (50% headroom). → Need 6–8 primary nodes at 100K ops/sec each.
- Message Queue: 500M events/day = 5,800 events/sec average. At 5× peak = 30K events/sec = 30 MB/sec. With 3× replication = 90 MB/sec cluster disk writes. → Need 20 brokers at ~5 MB/sec each, well within NVMe capacity.
- Lock: 10K instances × 10 acquires/min = 1,667 ops/sec. Trivially handled by any in-memory system. The constraint here is latency, not throughput.
- Job Scheduler: 100K jobs ÷ 60 ≈ 1,667 jobs/min average. Midnight spike: 10K jobs in 5 seconds = 2,000 jobs/sec burst. Queue depth handles the burst.

**Key trade-offs to call out explicitly:**

✅ **Durability vs Latency**: `acks=all` in Kafka gives you no data loss on leader failure but costs 2–5ms of extra latency. `acks=1` is faster but loses messages if the leader fails before replicating. `appendfsync everysec` in Redis risks 1 second of data loss but keeps write throughput at full speed. `appendfsync always` gives zero data loss but caps throughput at disk IOPS (~10K-100K ops/sec depending on disk type).

❌ **Consistency vs Availability** (CAP): Redis Cluster is AP — the minority partition stops accepting writes but the majority continues. This means writes never block indefinitely on partition, but you might temporarily serve stale reads from replicas. etcd is CP — it refuses writes if it can't reach quorum. Choose based on whether your use case can tolerate occasional duplicate execution (AP is fine) or requires mutual exclusion guarantees (CP is required).

✅ **Scalability vs Operational Complexity**: Adding more Kafka partitions increases throughput linearly but also increases the cost of consumer group rebalances (up to 30 seconds of stopped consumption per rebalance). Adding more Redis shards requires live resharding which is operationally risky if not planned carefully. More etcd nodes improve read throughput but add write latency (more nodes to reach quorum).

❌ **Exactly-once vs Throughput**: Kafka exactly-once (EOS) uses 2-phase commit between producer and broker, adding ~5-10ms overhead per transaction. For most workloads, at-least-once delivery + idempotent consumers achieves the same effective result at full throughput.

---

### 3d. Capacity Estimation

**Anchor numbers worth memorizing:**

| System | Key Number | Why It Matters |
|---|---|---|
| Redis node | 100K–150K ops/sec per node (4 vCPU, 32 GB) | Sizing shard count |
| Redis cluster | 16,384 hash slots, 2 KB gossip bitmap | Fixed; cannot increase without full reshard |
| Kafka broker | 1–3 GB/sec sequential disk write (NVMe) | Bottleneck is network, not disk |
| Kafka partition | 1 partition = 1 ordered stream, 1 consumer per group | Partition count = max parallelism |
| etcd | 3 or 5 nodes; handles thousands of ops/sec; 9 MB for 10K locks | Size of lock state is negligible |
| Consistent hash ring | 150 vnodes/node × 20 nodes = 3,000 entries × 16 bytes = 48 KB | Fits in L2 cache; lookup = 120 ns |
| Job Scheduler | 100K cron jobs × 2 KB = 200 MB definitions; 139 jobs/sec × 1 KB × 86,400 × 90 days = 1.08 TB history | History is the storage challenge |

**Capacity estimation formula — cache example (show this sequence):**

```
Users: 500M active, 10% active daily = 50M
Avg cached data per user: 2 KB → Hot dataset = 100M × 2 KB = 200 GB raw
Redis overhead (~30%): 200 GB × 1.3 = 260 GB needed
Node size: 32 GB RAM → 260/32 ≈ 9 nodes → use 8 primaries (256 GB)
  or use r6g.2xlarge (64 GB) → 4 primaries (256 GB)

Peak OPS: 500M users × 20 ops/day = 10B ops/day
Peak OPS/sec: 10B / 86,400 × 3 (peak multiplier) = 350K ops/sec
Provisioned: 350K / 0.5 (50% headroom) = 700K ops/sec
Nodes needed: 700K / 125K per node = 6 → use 8 primaries
Result: 8 primaries + 8 replicas = 16 nodes

Bandwidth:
Peak reads: 280K reads/sec × 1 KB = 274 MB/sec
Peak writes: 70K writes/sec × 1 KB = 68 MB/sec
Per node: (274 + 68) / 8 ≈ 43 MB/sec — well within 10 Gbps NIC
```

**Architecture implications of the numbers:**
- 260 GB hot dataset → cannot fit in one machine → sharding is mandatory.
- 43 MB/sec per node → well within NIC capacity → bandwidth is not the bottleneck.
- 120 ns ring lookup → at 1M RPS, ring lookups consume 0.12 CPU cores → in-process ring is effectively free.
- 1.08 TB execution history → must partition the PostgreSQL table by time range (monthly) or it becomes a table scan nightmare for history queries.

**Time allocation in interview (45 min):**
- Clarifying questions: 5 min
- Requirements + NFRs: 5 min
- Capacity estimation: 5–8 min (get numbers, state implications, move on — don't get stuck)
- HLD + data flow: 12–15 min
- Deep dive on most interesting component: 10–12 min
- Failure scenarios + trade-offs: 5 min

---

### 3e. High-Level Design

**The four-to-six components that appear in almost every distributed systems HLD:**

1. **Stateless API Layer**: Horizontally scaled request handler. Validates, authenticates, rate-limits. Holds no state — all state is externalized. Routes to the appropriate backend. In cache: Cluster Router or Proxy. In lock: Lock Service API gRPC servers. In message queue: Kafka Producer/Consumer clients. In job scheduler: REST/gRPC Control Plane.

2. **Consensus / Coordination Layer**: The brain that decides who's in charge. In cache: Redis Cluster Bus (gossip-based, AP). In lock: etcd (Raft, CP). In message queue: KRaft controller quorum. In job scheduler: PostgreSQL row locks (no separate consensus service needed — the DB transaction IS the consensus).

3. **Storage / Data Layer**: Where the actual data lives. In cache: Redis in-memory hash table with optional AOF/RDB persistence. In message queue: Append-only log on NVMe SSDs. In job scheduler: PostgreSQL for durable job state, Redis sorted sets for active dispatch queue. In consistent hashing: the physical nodes (cache servers, DB shards) are the storage; the ring is just routing.

4. **Replication Layer**: The mechanism for durability and read scaling. In cache: async primary → replica with optional `WAIT` for sync. In message queue: ISR-based replication with `acks=all` and `min.insync.replicas=2`. In lock: etcd Raft majority quorum write.

5. **Failure Detection / Expiry Layer**: How the system handles crashed nodes and stuck processes. TTL/lease expiry is the universal answer. etcd leases delete keys automatically. Redis key TTL expires stale locks. Kafka ISR timeout removes lagging replicas. Job scheduler heartbeat timeout marks executions as failed.

6. **Monitoring + Observability Layer**: Prometheus exporters, Grafana dashboards, alerting. Every production distributed system needs metrics on: replication lag, queue depth, error rate, latency percentiles (P50/P99/P999), and the specific "canary" metric for that system (ISR shrink rate for Kafka, eviction rate for Redis, DLQ depth for job scheduler, fencing token gap for lock service).

**Whiteboard order (draw in this sequence):**

1. Draw client(s) on the left → stateless API layer in the middle → storage backend on the right.
2. Add the consensus/coordination layer (etcd, KRaft, PostgreSQL row locks) connected to the API layer.
3. Show primary/replica relationship in the storage layer with an arrow labeled "async replication" (or "Raft quorum write" for etcd).
4. Add the data flow arrows: label each arrow with the key operation (GET, SET, ACQUIRE, PRODUCE, FIRE TRIGGER).
5. Add failure detection components: TTL/lease expiry on the lock, ISR tracker on the message queue, heartbeat monitor on the job scheduler.
6. Add monitoring box in the corner (Prometheus + Grafana) — this shows operational maturity.

**Key decisions to verbalize as you draw:**
- "I'm choosing Redis Cluster over standalone Redis because this dataset doesn't fit on one node."
- "I'm using etcd over ZooKeeper for the lock backend because etcd's native lease API handles TTL expiry at the consensus layer, and its Go binary has lower operational overhead."
- "I'm using `FOR UPDATE SKIP LOCKED` in PostgreSQL rather than leader election for the job scheduler trigger engine because it allows multiple scheduler nodes to run simultaneously without coordination overhead."
- "I'm choosing async replication for the cache because we're in an AP use case — cache misses are tolerable, but blocking every write on replica acknowledgment would double our P99 write latency."

---

### 3f. Deep Dive Areas

The three areas interviewers probe most heavily in distributed systems interviews:

**1. Exactly-once semantics (appears in 3 of 5 problems)**

The problem: network failures cause retries; retries cause duplicates; duplicates cause incorrect state (double-charging, duplicate job execution, duplicate cache invalidation).

Three distinct mechanisms — know all three:
- **Kafka idempotent producer**: Each producer gets a PID (Producer ID) from the broker. Each partition gets a monotonically increasing sequence number. The broker keeps a sliding dedup window of the last 5 in-flight sequences per (PID, partition). Retried batches with the same sequence are dropped silently. This gives exactly-once delivery to a single partition within a producer session.
- **Kafka transactions**: Extends idempotency to atomic writes across multiple partitions. `beginTransaction()` → produce to N partitions → `commitTransaction()` triggers a 2-phase commit via the transaction coordinator. Consumers with `isolation.level=read_committed` skip uncommitted records. Cost: ~5-10ms per transaction.
- **PostgreSQL `FOR UPDATE SKIP LOCKED`**: For the job scheduler, multiple scheduler nodes simultaneously run: `SELECT ... WHERE status='PENDING' AND scheduled_time <= NOW() FOR UPDATE SKIP LOCKED`. PostgreSQL row-level locking ensures each trigger row is claimed by exactly one node. No leader election, no external consensus service — the database transaction IS the distributed coordination.

Trade-offs: Kafka EOS adds latency and requires the transactional API. `FOR UPDATE SKIP LOCKED` requires PostgreSQL and adds lock contention at high throughput (>10K TPS). Application-level idempotency (dedup key in target DB) is the most general solution but requires database support and adds a round trip per processed message.

**2. Fencing tokens and the zombie problem (central to lock service, relevant to job scheduler)**

The problem: A lock holder pauses for longer than its TTL (JVM GC stop-the-world, OS scheduling, network hiccup). During that pause, the TTL expires, another client acquires the lock, and now both clients believe they hold it. The first client resumes from its pause with no awareness that its lease expired.

This is the most misunderstood aspect of distributed locking. The fix is NOT to make the TTL longer (GC pauses can be seconds; you cannot make TTL arbitrarily long). The fix is a **fencing token** — a monotonically increasing integer issued by the lock service on every successful acquisition.

How it works: etcd's global revision number (which increments with every write in the cluster) serves as the fencing token. Client A acquires lock, gets revision 42. Client A's lease expires. Client B acquires lock, gets revision 43. Client A resumes and tries to write to the database with token 42. The database (or a proxy in front of it) tracks the highest token it has ever seen. It sees 42 < 43 (already seen), and rejects the write. Client A is a zombie and is correctly blocked.

Critical insight: the validation must happen at the resource, not at the lock service. The lock service issues the token; only the resource can enforce it, because only the resource sees both the token and the actual operation. If the database doesn't support fencing tokens (most don't natively), you have several options: wrap it with a proxy that does, use optimistic locking (`UPDATE WHERE version = ?`), use STONITH (kill the zombie machine), or accept the risk and use short TTLs with idempotent operations.

Unprompted trade-off to mention: Redlock (multi-node Redis quorum) doesn't have fencing tokens. Its "random value" safety mechanism prevents one client from releasing another's lock, but does nothing to prevent a zombie from acting on an expired belief. Redlock is acceptable for advisory efficiency locks (preventing duplicate cron execution where an occasional duplicate is tolerable) but is NOT safe for storage-protecting locks where data correctness is paramount. This distinction (Martin Kleppmann's 2016 critique) is a genuine debate in the community and naming it earns points.

**3. ISR (In-Sync Replicas) and the High Watermark (Kafka-specific)**

The problem: with 3 brokers holding replicas of a partition, what exactly does "durably written" mean? If the leader crashes after writing but before all followers replicate, which messages survive?

The ISR is the set of replicas that are caught up to within `replica.lag.time.max.ms` (default 30 seconds) of the leader's Log End Offset (LEO). With `acks=all`, the leader only acknowledges a write after all current ISR members have confirmed they received it. The High Watermark (HW) is the highest offset that ALL ISR members have confirmed. Consumers can only read up to the HW — never beyond it.

Why the HW restriction exists: if a consumer could read beyond the HW, and then the leader crashed before those messages were replicated to the new leader, the consumer would have read a message that the system later "forgot." By restricting reads to HW, Kafka guarantees read-after-write consistency from any leader.

The dangerous edge case: if all replicas fall out of ISR (all lag), the ISR shrinks to just the leader. With `min.insync.replicas=2`, the leader refuses writes (`NotEnoughReplicasException`). This is correct behavior for durability. If you set `min.insync.replicas=1`, the leader accepts writes alone — which means a leader crash loses those writes. Alert on `IsrShrinksPerSec > 0` in production.

`unclean.leader.election.enable=true` allows a lagging replica to become leader when all ISR members are dead. This recovers availability at the cost of message loss and possible reordering. Default is `false`. Only enable it for topics where data loss is acceptable (e.g., application metrics, click events) and the alternative (partition offline) is worse.

---

### 3g. Failure Scenarios

**The failure modes you must describe unprompted for a senior-level pass:**

**Single node failure:**
- Cache (Redis): The replica detects the primary is gone via heartbeat timeout. The cluster holds a failover election — the replica that receives `FAILOVER_AUTH_ACK` from majority (`N/2 + 1`) of masters is promoted. During the election window (~5-30s), the affected slots are unavailable. After promotion, clients receive `MOVED` redirects and update their slot maps. Risk: any writes ACKed by the primary but not yet replicated to the replica are lost (async replication gap).
- Kafka: Controller detects broker failure via heartbeat timeout (default `session.timeout.ms=30s`). For each partition where the failed broker was leader, controller selects a new leader from the ISR. Leader epoch is incremented — stale produce requests from the old leader (zombie) are rejected. Replication of the failed broker's partitions to the new leader is automatic once the broker rejoins.
- Lock (etcd): etcd's Raft protocol handles node failure transparently. The remaining majority (2 of 3, or 3 of 5) continues accepting writes. If the failed node was the current Raft leader, the remaining nodes elect a new leader within one election timeout (default `heartbeat-interval * 10` ≈ 1 second). In-flight lock operations that were in progress may timeout and need to be retried by the client library.

**Senior framing — the questions that separate principal-level from senior-level responses:**

A senior says: "If the Redis primary crashes, we might lose some writes due to async replication."

A principal says: "If the Redis primary crashes, we might lose writes — the window is the replication lag, typically 5-10ms. We can bound this with `WAIT 1 100` on critical writes (blocks until at least 1 replica acknowledges or 100ms passes), accepting the write latency trade-off. For our session cache use case, a 5ms window of potential loss is acceptable because the consequence is a re-login, not data corruption. For rate-limit counters, it means we might occasionally allow a few extra requests — also acceptable. If we were caching financial data that is the source of truth, I would use a CP system (etcd or Postgres) rather than Redis for that tier."

This framing — "here is the failure mode, here is its window, here are the consequences, here is whether those consequences are acceptable for this specific use case, and here is the alternative if they aren't" — is the senior/principal difference.

**Failure scenario checklist per system:**

| System | Failure Mode | Detection | Recovery | Data Loss Risk |
|---|---|---|---|---|
| Redis Cache | Primary crash | Gossip PFAIL/FAIL vote | Replica promotion (5-30s) | Async replication gap (~5ms) |
| Redis Cache | Network partition | Gossip timeout | Minority stops writes (AP) | No loss on majority side |
| Kafka | Broker crash | Heartbeat timeout (30s) | ISR leader election | Zero if ISR > 1 and acks=all |
| Kafka | ISR shrinks to 1 | Controller ISR shrink event | Reject writes (if min.insync.replicas=2) | Potential if allowed to continue |
| etcd/Lock | Node crash (minority) | Raft heartbeat | Majority continues, no disruption | Zero (Raft committed) |
| etcd/Lock | Leader crash | Raft election | New leader in ~1 election timeout | Zero (Raft committed) |
| Job Scheduler | Worker crash mid-job | Heartbeat timeout | Status Manager marks FAILED, re-enqueues | None (job reruns) |
| Job Scheduler | Scheduler node crash | PostgreSQL connection timeout | Other scheduler nodes claim unclaimed triggers | None (FOR UPDATE SKIP LOCKED) |
| Consistent Hash | Node added to ring | Ring Manager announces JOINING | Double-write + migrate + SWITCH | None (double-write protocol) |
| Consistent Hash | Node removed | Ring Manager announces LEAVING | Migrate keys to successor, remove | None (migration before removal) |

---

## STEP 4 — COMMON COMPONENTS

These eight components appear across all five problems. Know them cold — an interviewer can ask about any of them in the context of any problem.

---

### Component 1: Stateless API / Service Layer + Persistent Backend

**Why it's used:** The API layer handles incoming requests, validates them, enforces authentication and rate limits, and translates high-level semantics ("acquire lock") to backend operations (etcd CAS transaction). Making it stateless means it can be horizontally scaled by adding instances behind a load balancer — any instance can handle any request because state lives in the backend, not in the API process.

**Key config:**
- Authentication: JWT Bearer tokens (REST) or mTLS client certificates (gRPC) — verify before every operation.
- Rate limiting: enforced at the API layer, not the backend. The backend shouldn't need to care about per-client limits.
- The API layer is thin — it should do minimal computation. Heavy logic (key migration coordination, lock contention management) belongs in dedicated background services.

**What happens without it:** If the API layer holds state (e.g., in-memory lock state), you can't add instances freely — requests for the same lock name must go to the same instance. This creates a sticky-routing requirement, which is operationally fragile and limits horizontal scalability.

---

### Component 2: etcd / Raft-Based Coordination

**Why it's used:** Any operation that requires "exactly one winner" or "consistent view of current state" across multiple processes needs a consensus protocol. etcd provides linearizable reads and writes via Raft — a write that succeeds is guaranteed to be visible to all future reads from any node. The native lease/TTL API handles automatic key expiry at the consensus layer. The Watch API provides efficient change notification without polling.

**Key config:**
- 3 nodes (tolerates 1 failure) or 5 nodes (tolerates 2 failures). Most production deployments use 3.
- `heartbeat-interval`: 100ms default. `election-timeout`: 1000ms default (10× heartbeat).
- `--auto-compaction-retention`: compact Raft log periodically to control disk usage.
- etcd's revision number is a free, globally monotonic fencing token — use it instead of implementing your own counter.

**What happens without it:** Without consensus, you can't guarantee mutual exclusion across processes. You can use `SET NX PX` on a single Redis instance for mutual exclusion within one node, but single-node Redis is a SPOF and Redis is AP (not linearizable). Any scenario where two concurrent clients both believe they hold the lock simultaneously is possible under network partition or failover with an AP store.

---

### Component 3: Primary / Replica Replication

**Why it's used:** A single machine can fail. Replication distributes the data to additional machines so that a failure of any one machine doesn't lose data. Replicas also serve read traffic, increasing read throughput beyond what a single node can handle.

**Key config:**
- **Redis**: Async replication. `WAIT 1 0` for near-synchronous on critical writes. Replication buffer: `client-output-buffer-limit replica 256mb 64mb 60` — if a replica falls too far behind, it is disconnected and must do a full resync.
- **Kafka**: ISR-based with `acks=all` and `min.insync.replicas=2`. The ISR set is maintained by the controller; replicas that fall behind `replica.lag.time.max.ms` (30s) are removed from the ISR.
- **etcd**: Raft quorum write — the write is committed only after a majority of nodes (⌊N/2⌋ + 1) persist it. This is synchronous by definition; there is no "async" option in Raft.

**What happens without it:** Single node = single point of failure. Any hardware failure, kernel panic, or NIC dropout takes the entire service down. For a cache, this causes a thundering herd of cache misses against the origin DB. For a lock service, it makes the locking infrastructure unavailable. For a message queue, you lose messages that were in-flight.

---

### Component 4: TTL / Lease-Based Expiry

**Why it's used:** Processes crash. Network partitions happen. If a lock holder crashes while holding a lock, without TTL the lock is held forever and no other client can ever acquire it — the system deadlocks. TTL/lease expiry is the self-healing mechanism: after a bounded time, stale state is automatically cleaned up even if the holder never explicitly releases it.

**Key config:**
- **Lock TTL**: Short enough that a crashed holder releases quickly (30s is common); long enough that a legitimate operation completes without needing frequent renewals. Auto-renew at 2/3 of TTL.
- **Cache key TTL**: Matched to the staleness tolerance of the data. Session tokens: 24h. Rate-limit counters: per-window (1 hour). Feed caches: 5-60 minutes.
- **Kafka retention**: 7 days by time or 100 GB by size, whichever is first. Log-compacted topics have no time-based TTL — they retain the latest value per key indefinitely.
- **Job execution timeout**: Per-job `timeout_seconds` (default 300s). Detected by heartbeat absence: `last_heartbeat < NOW() - timeout_seconds`.

**What happens without it:** Leaked resources accumulate. A crashed lock holder blocks all waiters indefinitely. Expired cached data is never evicted, consuming memory and eventually causing the cache to fill up and reject new writes. A hung job execution holds a worker slot forever, starving other jobs.

---

### Component 5: Retry with Exponential Backoff + Jitter

**Why it's used:** Transient failures (network blip, node restarting) should not cause permanent failures. Retry handles transient failures. Exponential backoff prevents the retrying client from hammering a recovering system. Jitter randomizes the retry time so that if 1,000 clients all fail at the same moment and retry, they don't all retry at the same moment (the "thundering herd" or "retry storm").

**Key config:**
```python
delay = min(max_delay, base_delay * 2**attempt) + random(0, jitter_range)
# Example: base=1s, max=60s, jitter=0-1s
# attempt=0: 1s + jitter
# attempt=1: 2s + jitter
# attempt=2: 4s + jitter
# attempt=3: 8s + jitter
# attempt=4: 16s + jitter
# attempt=5: 32s + jitter
# attempt=6+: 60s + jitter (capped)
```
- Kafka: `retry.backoff.ms=100`, `retries=2147483647` (essentially infinite) for producer.
- Job Scheduler: `base=300s, max=3600s, jitter=30s` — much longer because job execution may need minutes to recover.
- Lock Client: `base=50ms, max=2s, jitter=50ms` — fast retries because lock hold durations are short.

**What happens without it:** Without retry, any transient failure becomes a permanent failure from the client's perspective. Without backoff, retrying clients overwhelm a recovering server (retry storm). Without jitter, synchronized retries (all clients back off for exactly 2 seconds and retry simultaneously) recreate the spike that caused the failure.

---

### Component 6: Idempotency Mechanisms

**Why it's used:** In a distributed system, the client cannot know if a failed request was actually processed by the server before the failure. To avoid the "did it or didn't it" problem, all mutating operations should be idempotent — applying them twice has the same effect as applying them once. Idempotency allows unlimited retries without fear of side effects.

**Key implementations:**
- **Kafka idempotent producer**: PID + per-partition sequence number. Broker deduplicates within a sliding window of the last 5 in-flight sequences. `enable.idempotence=true`.
- **etcd CAS (Compare-And-Swap)**: `PUT /locks/name IF version == 0`. Only succeeds if the key does not already exist. Re-trying a failed PUT either creates the key (first attempt succeeded on server but ACK was lost) or fails the CAS (first attempt was genuinely processed), with no double-write.
- **Job Scheduler**: `idempotency_key` field with a unique index. `INSERT INTO job_definitions ... ON CONFLICT (idempotency_key) DO NOTHING`. Second submission with the same key is silently ignored.
- **Redis**: `SET key value NX EX ttl` — set only if not exists. Safe to retry.
- **Fencing token**: the ultimate idempotency mechanism for distributed systems — the resource validates that each operation's fencing token is strictly greater than the last seen, rejecting any re-submissions from stale clients.

**What happens without it:** Retried requests cause double effects. A retried job submission registers the job twice, causing duplicate executions. A retried lock acquire creates two lock entries (on two different nodes), giving two clients quorum-based "locks" simultaneously. A retried Kafka produce duplicates messages in the log.

---

### Component 7: In-Memory Data Structures for Hot-Path Operations

**Why it's used:** Disk I/O and network round-trips are 1,000–1,000,000× slower than in-memory operations. The critical path (the operation that must complete in sub-millisecond time) should never go to disk or over the network if it can be avoided. Placing routing tables, slot maps, ring state, and job queues in memory ensures the hot path stays fast.

**Key examples:**
- **Consistent hash ring**: Sorted array of 3,000 `(hash_position, vnode_id, node_ref)` structs = 48 KB. Fits entirely in L2 CPU cache. Binary search = 12 comparisons × 10 ns = 120 ns per lookup. At 1M RPS, this consumes 0.12 CPU cores — effectively free.
- **Redis slot map**: The 16,384-slot assignment bitmap = 2 KB, included in every gossip heartbeat. Every Redis client keeps a local copy in memory and updates it on `MOVED` responses.
- **Kafka page cache**: Sequential log reads are cached by the OS filesystem page cache. Consumer reads that are close to the write head are served from RAM at memory speed, not from disk.
- **Job dispatch queue**: Redis sorted set (score = scheduled_time) for the active job queue. `ZADD` and `ZPOPMIN` are O(log N) — sub-millisecond for tens of thousands of pending jobs.
- **etcd lock state**: 10,000 locks × 900 bytes = 9 MB. The entire lock state fits in RAM multiple times over. etcd's BoltDB storage provides durability with O(1) in-memory lookup.

**What happens without it:** Every request goes to disk or network. A lock acquire that requires a database round-trip adds 1-10ms of latency. A ring lookup that requires a network call to a routing service adds 0.1-1ms. These seem small but compound at 1M RPS: 10ms × 1M = 10,000 CPU-seconds per second of serving load — completely untenable.

---

### Component 8: Admin API for Runtime Configuration

**Why it's used:** Distributed systems need operational control without service restarts. Adding a node to a Redis cluster, changing an eviction policy, pausing a cron job, increasing Kafka partition count — all of these must be doable on a live system. Admin APIs provide these hot-reconfiguration capabilities with audit trails.

**Key operations per system:**
- **Cache**: `CLUSTER SETSLOT` (live resharding), `CONFIG SET maxmemory-policy allkeys-lfu` (hot eviction policy change), `CLUSTER FAILOVER` (manual failover for planned maintenance).
- **Message Queue**: `POST /admin/topics` (create/delete topics), `GET /admin/consumer-groups/{group}/offsets` (monitor lag), `POST /admin/topics/{topic}/partitions` (increase partition count — note: can only increase, never decrease).
- **Lock**: `DELETE /v1/locks/{name}` (admin force-release for stuck locks), `PUT /v1/locks/{name}/ttl` (update default TTL), `GET /v1/metrics/contention` (see high-contention locks).
- **Job Scheduler**: `POST /v1/cron-jobs/{id}/pause` / `resume` (stop/restart job firing), `POST /v1/executions/{id}/retry` (force-retry from DLQ), `GET /v1/metrics/overview` (queue depth, worker utilization).
- **Consistent Hash**: `POST /v1/ring/nodes` (add node + get migration plan), `DELETE /v1/ring/nodes/{id}` (graceful drain + remove), `POST /v1/ring/nodes/{id}/mark_down` (emergency remove without migration — accept temporary key unavailability).

**What happens without it:** Operational changes require service restarts (downtime) or direct database manipulation (risky, no audit trail). In production, this means you can't dynamically scale the cluster, respond to hotspots, or fix stuck states without taking the service offline. Every on-call engineer who has ever had to SSH into a production Redis node at 3am and run `DEBUG RELOAD` would prefer an admin API.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

Each of the five problems has a unique design decision that distinguishes it from the others. Know these two-sentence differentiators cold.

---

### Consistent Hashing

**What makes it unique:** The xxHash32 vnode ring is a pure routing algorithm — it is stateless on the hot path (48 KB sorted array in L2 cache, O(log N) binary search = 120 ns) and completely separate from the data it routes. The double-write migration protocol (write to both old and new nodes during topology change, bulk migrate existing keys, atomic SWITCH) achieves zero-downtime node addition/removal while guaranteeing that only 1/N keys move — the mathematical minimum.

**Different design decision:** Unlike modular hashing (`hash(key) % N`) which moves ~100% of keys when N changes, consistent hashing moves only 1/N. Unlike Rendezvous hashing which requires O(N) comparisons per lookup (untenable at N=20,000 nodes), consistent hashing uses O(log N) binary search. Unlike Jump Consistent Hash which cannot handle weighted nodes or arbitrary node removal, consistent hashing handles both via the vnode weight system.

**Two-sentence differentiator:** Consistent hashing is not a data store — it is a deterministic key-to-node routing function implemented as a sorted array with O(log N) lookup. Its core innovation is replacing modular arithmetic (catastrophic key movement on topology change) with virtual nodes on a hash ring (minimum 1/N key movement), and its operational challenge is coordinating ring topology changes across all clients within a bounded propagation window.

---

### Distributed Cache

**What makes it unique:** The Redis Cluster hash slot gossip protocol (CRC16(key) % 16,384 → 2 KB bitmap in every heartbeat, zero-coordinator live resharding via MIGRATING/IMPORTING/MOVED/ASK) and the approximate LFU eviction policy (8-bit Morris counter with time-decay for Zipf-distributed real-world access patterns) are the two design elements that have no counterpart in the other four systems.

**Different design decision:** The cache is the only AP system in the group that deliberately accepts data loss on failover (async replication gap ~5ms). Every other system (lock, message queue, job scheduler) uses CP mechanisms for their primary data paths. The cache makes this trade-off because a cache miss is recoverable — the origin database is the source of truth — while a missed job fire or a lost lock acquisition has business consequences.

**Two-sentence differentiator:** The distributed cache's defining design challenge is the triangle of latency (sub-ms), capacity (200 GB hot dataset requiring sharding), and consistency (AP with async replication) — and its answer is the Redis Cluster gossip protocol with 16,384 hash slots, live resharding via the MOVED/ASK protocol, and approximate LFU eviction to keep the hottest keys alive under Zipf-distributed access patterns. PER (Probabilistic Early Reexpiration) prevents cache stampedes, and the AOF `appendfsync everysec` configuration provides crash recovery within a 1-second data loss window.

---

### Distributed Lock

**What makes it unique:** The fencing token design (etcd's global Raft revision = the lock token, validated at the protected resource, not at the lock service) and the rigorous comparison of alternatives (Redlock: unsafe due to clock drift and no fencing tokens; ZooKeeper sequential znodes: no herd effect but JVM overhead; single Redis: SPOF) make the lock service the most conceptually rich of the five problems.

**Different design decision:** The distributed lock is the only CP problem in the group by definition — a lock that sometimes grants mutual exclusion and sometimes doesn't is useless. This forces the etcd/Raft choice (linearizable, majority quorum write) rather than the Redis/Gossip choice used by the cache. The operational cost is higher (etcd is more complex than Redis) and write latency is higher (Raft consensus vs single-node in-memory), but these are acceptable for the lock's workload (1,667 ops/sec, not 1M ops/sec).

**Two-sentence differentiator:** The distributed lock's critical insight is that the lock service alone cannot guarantee safety — a zombie (process paused longer than its TTL) can still act on an expired belief, so the protected resource must validate a monotonically increasing fencing token (etcd's Raft revision) and reject any operation with a token it has already seen. Redlock is explicitly rejected because it relies on synchronized clocks for TTL and issues no fencing token, making it unsafe for storage mutations even though it's acceptable for advisory efficiency locks.

---

### Distributed Message Queue

**What makes it unique:** The ISR (In-Sync Replicas) / High Watermark / LEO (Log End Offset) trifecta for durability, exactly-once semantics via idempotent producer (PID + per-partition sequence dedup) + transaction coordinator (2PC across partitions), KRaft replacing ZooKeeper for metadata consensus, and log compaction (`cleanup.policy=compact`) for event-sourcing topics have no analogues in the other four problems.

**Different design decision:** The message queue is the only system in the group that stores data on disk as its primary storage medium (sequential append to NVMe log files). All other systems use in-memory structures as the primary hot path (Redis RAM, etcd BoltDB, PostgreSQL buffer pool). This disk-primary design is what enables 7-day retention on 10 GB/sec write throughput (10.5 TB total cluster storage) — a dataset size that would be prohibitively expensive to store entirely in RAM.

**Two-sentence differentiator:** The distributed message queue's design center is durable, ordered, replayable event streaming at 10 GB/sec — achieved by sequential append to NVMe SSDs (1-3 GB/sec throughput, OS page cache serving consumer reads near the write head) plus ISR-based replication (`acks=all`, `min.insync.replicas=2`) ensuring no acknowledged message is lost on broker failure. The exactly-once semantics layer (PID + sequence dedup for single-partition, 2PC transaction coordinator for multi-partition) is a surgical addition on top of the at-least-once foundation, accepted only when the consumer side effect cannot be made idempotent at the application layer.

---

### Distributed Job Scheduler

**What makes it unique:** The `FOR UPDATE SKIP LOCKED` PostgreSQL pattern for exactly-once trigger claiming (no leader election, no external coordination, multiple scheduler nodes compete atomically for trigger rows), DAG dependency engine (Kahn's cycle detection at registration, dependency_fulfillments counter at runtime), and fencing tokens on job executions (zombie workers rejected at the `POST /complete` endpoint) distinguish the job scheduler from all other problems.

**Different design decision:** The distributed job scheduler is the only problem that uses PostgreSQL as the coordination layer rather than etcd, Raft, or gossip. `FOR UPDATE SKIP LOCKED` achieves exactly-once trigger claiming using database-level row locks — this is an underrated pattern that avoids adding an external consensus service. The trade-off is that PostgreSQL's write throughput ceiling (~10K TPS) limits the trigger firing rate, but at 139 jobs/sec peak, the system operates at ~1% of that limit with enormous headroom.

**Two-sentence differentiator:** The distributed job scheduler's defining technique is PostgreSQL's `FOR UPDATE SKIP LOCKED` as a distributed coordination primitive — multiple scheduler nodes race atomically to claim trigger rows, and the database lock ensures exactly-once firing without leader election overhead. The DAG dependency engine (Kahn's algorithm at registration, `dependency_fulfillments` counter at runtime) and fencing tokens on each execution record (rejected by the `POST /complete` endpoint if mismatched) complete the safety guarantees for a concurrent, multi-node scheduling system.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (expect these in first 15 minutes)

**Q1: What is consistent hashing and why is it better than modular hashing for distributing cache keys across servers?**

**KEY PHRASE: "moves only 1/N of keys."** Modular hashing assigns keys via `hash(key) % N`. When you add a server (N becomes N+1), almost every key maps to a different server — roughly `N/(N+1) ≈ 100%` of keys need to move, causing a massive migration and cache miss storm. Consistent hashing places both keys and servers on a conceptual ring (0 to 2^32). Each key is assigned to the first server clockwise from its hash position. Adding a server inserts new points on the ring, "stealing" key ranges only from neighboring servers — so only 1/N keys move on average. Virtual nodes (150 per physical node) distribute the ring positions evenly, keeping load variance under 3%.

**Q2: What is the CAP theorem and which side does Redis Cluster land on?**

**KEY PHRASE: "minority partition stops writes."** CAP states that a distributed system can guarantee at most two of: Consistency (all nodes see the same data at the same time), Availability (every request gets a response), and Partition Tolerance (the system continues operating when network partitions occur). Redis Cluster chooses AP — during a network partition, the majority partition continues serving reads and writes, while the minority partition stops accepting writes (to avoid split-brain). This means a client on the minority side gets errors rather than stale data, and writes to the majority continue. The cost: if the primary and its replica end up on opposite sides of a partition and the replica is promoted, writes acknowledged by the old primary but not yet replicated are lost.

**Q3: What is the High Watermark in Kafka and why can't consumers read beyond it?**

**KEY PHRASE: "all ISR members have confirmed."** The High Watermark (HW) is the highest offset where ALL In-Sync Replicas have confirmed receipt. Consumers are restricted to reading up to the HW (not the Log End Offset). The reason: if a consumer could read beyond the HW, and then the leader crashed before replicating those messages to any follower, a new leader would be elected without those messages. The consumer would have read data that the cluster later "forgot," violating the guarantee that committed messages are permanently available. By gating reads at HW, Kafka ensures any consumed message survives leader failover.

**Q4: What is a fencing token and why is TTL alone not enough for distributed locks?**

**KEY PHRASE: "zombie still thinks it holds the lock."** A process can be paused for longer than the lock's TTL — JVM GC stop-the-world pauses can last seconds, OS scheduling can suspend a process, and network hiccups can delay heartbeats. If the pause exceeds the TTL, the lock expires and another client acquires it. When the paused process resumes, it still believes it holds the lock and attempts to write to the protected resource — this is the "zombie" problem. A fencing token is a monotonically increasing integer (in etcd, this is the Raft revision number) issued on every lock acquisition. The protected resource tracks the highest token it has ever seen and rejects any operation with a lower-or-equal token. The zombie's operation, carrying a stale token, is rejected regardless of what the lock service says.

**Q5: What is `FOR UPDATE SKIP LOCKED` and how does the job scheduler use it for exactly-once job firing?**

**KEY PHRASE: "atomic row claim without coordination."** `FOR UPDATE SKIP LOCKED` is a PostgreSQL (and MySQL 8+) feature that, when combined with a `SELECT` statement, acquires a row-level exclusive lock on each qualifying row AND skips any rows that are already locked by another transaction (rather than waiting). This allows multiple concurrent processes to each claim a distinct subset of pending trigger rows without any application-level coordination. In the job scheduler, multiple scheduler nodes run the same query simultaneously: `SELECT ... FROM scheduled_triggers WHERE status='PENDING' AND scheduled_time <= NOW() FOR UPDATE SKIP LOCKED`. PostgreSQL guarantees each trigger row is claimed by exactly one node. No etcd leader election is needed — the database transaction IS the distributed coordination mechanism.

**Q6: What is the difference between at-least-once, at-most-once, and exactly-once delivery in a message queue?**

**KEY PHRASE: "idempotent consumer is the practical answer."** At-most-once: the producer sends once and doesn't retry, and the consumer commits its offset before processing. If either crashes, the message is lost. Used for metrics and logging where some loss is acceptable. At-least-once: the producer retries on failure (`acks=all`, `retries=MAX`), and the consumer commits after processing. Guaranteed delivery but possible duplicate processing. Used for most business events. Exactly-once: Kafka's EOS combines idempotent producer (PID + sequence dedup) with transactions (2PC across partitions). Adds ~5-10ms latency. For most use cases, at-least-once delivery with an idempotent consumer (check dedup key in the target DB before processing) achieves effectively-exactly-once behavior at lower cost.

**Q7: What is a dead letter queue (DLQ) and when should a job go to it?**

**KEY PHRASE: "max retries exhausted, not temporary."** A dead letter queue is a separate queue (or database table) where jobs are moved when they have permanently failed — specifically, when they have exhausted their configured maximum retry count. The DLQ exists because you don't want permanently failing jobs to block the main queue indefinitely by retrying forever. DLQ entries should trigger an alert (DLQ depth > 0 is an anomaly requiring investigation). The admin interface should support manual force-retry from DLQ once the underlying issue is fixed, with a new fencing token to reject any zombie workers from the previous attempts.

---

### Tier 2 — Deep Dive Questions (expect these after the HLD)

**Q1: Why does Redis use 16,384 hash slots rather than, say, 65,536?**

**KEY PHRASE: "gossip heartbeat size."** The slot assignment bitmap is included in every gossip heartbeat message between nodes. At 16,384 slots: `16,384 / 8 = 2,048 bytes = 2 KB` per heartbeat. At 65,536 slots: `65,536 / 8 = 8,192 bytes = 8 KB`. With N nodes each gossiping to every other node every second, the bandwidth cost is `N² × message_size`. Going from 2 KB to 8 KB quadruples gossip bandwidth. 16,384 also provides sufficient granularity for up to ~1,000 nodes (minimum ~16 slots per node) and was chosen by the Redis author as the practical optimum. It's a deliberate engineering trade-off, not a theoretical maximum.

**Q2: Explain the difference between MOVED and ASK redirects in Redis Cluster during a resharding operation.**

**KEY PHRASE: "MOVED is permanent, ASK is temporary."** During a live resharding operation, a slot transitions from a source node (status: MIGRATING) to a target node (status: IMPORTING). `MOVED slot ip:port` is returned when the slot has permanently moved to a new node. The client must update its local slot map — all future requests for that slot go to the new node. `ASK slot ip:port` is returned during migration: the specific key being requested has already been migrated to the target, but the slot assignment hasn't been finalized yet. The client must send an `ASKING` command to the target before the actual command (to bypass the IMPORTING guard), but should NOT update its slot map — that update happens when `MOVED` confirms the final assignment.

**Q3: Walk me through the Kleppmann critique of Redlock and explain when Redlock is safe to use.**

**KEY PHRASE: "clock drift and no fencing tokens."** Martin Kleppmann's 2016 critique identified two problems. First: Redlock relies on wall-clock TTL on each Redis node. If one node's clock drifts forward (due to NTP correction or virtualization), it will expire the lock key prematurely. The quorum then changes — the "expired" node can be part of a new client's quorum while the original client still believes it has quorum. Two clients simultaneously hold quorum. Second: Redlock uses a random value as a lock identifier, which prevents another client from releasing your lock, but does not issue a monotonically increasing token. Without a monotonic token, the protected resource cannot distinguish a zombie from a legitimate holder. The Redis author (Antirez) responded that clock drift > 1ms/sec is rare in practice with NTP, and GC pauses beyond TTL are the application's problem. The practical recommendation: Redlock is safe for advisory efficiency locks (preventing duplicate cron execution, rate limiting) where an occasional duplicate is tolerable. It is NOT safe for storage-protecting locks where data correctness is paramount — use etcd or ZooKeeper with fencing tokens for those.

**Q4: What is log compaction in Kafka and when would you use `cleanup.policy=compact` vs `cleanup.policy=delete`?**

**KEY PHRASE: "retain latest value per key."** `cleanup.policy=delete` (default) retains messages by time (e.g., 7 days) or size (e.g., 100 GB). Old segments are deleted when they exceed the threshold. `cleanup.policy=compact` retains only the most recent record for each key, regardless of time. The background cleaner thread periodically scans the log and removes older records for keys that have newer entries. A tombstone (record with a null value) marks a key for deletion — the tombstone itself is retained for `delete.retention.ms` to allow consumers to observe the deletion. Use compaction for: database CDC topics (consumer rebuilds current DB state by replaying the topic from the beginning), user profile changelog topics, and any use case where "current state per entity" is what matters, not the full history. Use delete for: event streams, logs, metrics — where history is bounded by time/size, not by key cardinality.

**Q5: How does the trigger engine in the job scheduler achieve exactly-once firing without a single leader?**

**KEY PHRASE: "`FOR UPDATE SKIP LOCKED` is the coordination primitive."** Multiple scheduler nodes run the trigger loop simultaneously. Each loop iteration runs a PostgreSQL transaction: `SELECT ... FROM scheduled_triggers WHERE status='PENDING' AND scheduled_time <= NOW() ORDER BY scheduled_time LIMIT 1000 FOR UPDATE SKIP LOCKED`. PostgreSQL acquires a row-level exclusive lock on each returned row and skips rows already locked by other concurrent transactions. Each scheduler node gets a disjoint set of trigger rows. The node then updates those rows to `status='CLAIMED'`, creates execution records with fencing tokens, and enqueues to Redis. The transaction is committed atomically — if the scheduler crashes before committing, the rows remain PENDING and will be picked up by another node on the next poll cycle. No etcd, no Zookeeper, no leader election — PostgreSQL's row-level MVCC is the consistency mechanism.

**Q6: What is `unclean.leader.election.enable` in Kafka and what's the cost of enabling it?**

**KEY PHRASE: "availability vs. durability, and which topic deserves which."** By default (`unclean.leader.election.enable=false`), Kafka will not elect a leader from outside the ISR. If all ISR members die, the partition is offline — no reads or writes — until an ISR member recovers. This ensures no data loss. With `unclean.leader.election.enable=true`, Kafka elects any available replica as leader, even one that is behind the old leader's LEO. This recovers availability at the cost of: (1) message loss — messages the old leader committed that the new (out-of-ISR) leader never received are gone; (2) possible consumer confusion — offsets may now point to different messages. Enable it only on topics where availability is strictly more valuable than data integrity: clickstream, application logs, metrics. Never enable it for financial events, order events, or audit logs.

**Q7: How does the DAG dependency engine in the job scheduler prevent cycles and trigger dependent jobs?**

**KEY PHRASE: "Kahn's algorithm at registration, counter decrement at runtime."** Cycle detection happens at job registration time using Kahn's topological sort algorithm: build an in-degree map for all jobs, initialize a queue with jobs with in-degree=0, process jobs by decrementing the in-degree of their children, and flag a cycle if not all jobs are processed (some have non-zero in-degree remaining). This is O(V+E) where V = jobs, E = dependencies. At runtime, when a job completes, the dependency engine queries `job_dependencies` for that job's children, then checks `dependency_fulfillments` for each child — if all of a child's parents have completed (counter reaches 0), the child is inserted into `scheduled_triggers` with `scheduled_time = NOW()`. This is event-driven (triggered on job completion) rather than polling-based.

---

### Tier 3 — Staff+ Stress Test Questions (reason aloud, no single right answer)

**Q1: Your distributed cache is experiencing a "thundering herd" on cache miss — a popular key expires and 10,000 concurrent requests all hit the origin database simultaneously. How do you fix this, and what are the trade-offs of each approach?**

**KEY PHRASE: "single writer, early refresh, or probabilistic recomputation."** Three approaches at increasing sophistication:

(1) **Mutex / single-writer**: When a key is missing, one thread acquires a lock (Redis `SET lock_key ... NX EX 5`) and refreshes the cache; other threads wait or serve a stale value. Trade-off: requires a lock, waiters are blocked, and if the refreshing thread crashes, the lock TTL must expire before others can try.

(2) **Probabilistic Early Reexpiration (PER)**: When reading a key, probabilistically trigger a background refresh before the TTL expires. The probability increases as the TTL approaches 0: `if ttl - beta * log(random()) < 0: background_refresh()`. This staggers the refresh workload across multiple requests rather than all hitting at once. Trade-off: slightly stale reads during the refresh window; works best when the refresh is fast.

(3) **Cache-Aside with coalescing (request collapsing)**: Multiple concurrent requests for the same missing key are coalesced into a single origin DB request. The first requester fetches and caches; subsequent requesters wait for the first and receive the cached result. Trade-off: requires application-level coordination (a shared promise/future per key), adds complexity.

For very hot keys (viral content, breaking news), none of these fully solves the problem — consider pre-warming (proactively populate before expiry) or serving a hardcoded default for the brief window.

**Q2: Your Kafka consumer lag is growing despite adding more consumer instances. Describe your debugging process and the possible root causes.**

**KEY PHRASE: "partition count is the parallelism ceiling."** First, establish the facts: what is the lag per partition? If it is growing uniformly across all partitions, the consumer processing rate is slower than the producer rate system-wide. If only some partitions are lagging, there is a hot partition (uneven key distribution) or a slow consumer on those specific assignments. 

Root cause hierarchy:
- **Partition count**: In Kafka, a consumer group can have at most one consumer per partition. If you have 6 partitions and 10 consumer instances, 4 are idle. Adding more instances beyond partition count does nothing. Fix: increase partition count (only possible upward in Kafka, never downward).
- **Slow consumer processing**: Each message takes too long to process. Profile the consumer handler. Consider async processing (commit offset after starting processing rather than after completing), or parallelize within the consumer.
- **Hot partition**: Most messages hash to the same partition (e.g., all events have the same key). Fix: add randomness to the key, use a hash tag that spreads better, or override the partitioner.
- **Large messages**: Large messages cause more data per poll, more GC pressure, more deserialization time. Profile message size; consider compression or splitting.
- **`isolation.level=read_committed` + long transaction**: If producers have a long-running transaction, LSO falls behind HW, and `read_committed` consumers appear lagged even though data exists. Short-circuit by checking LSO lag vs HW lag separately.

**Q3: You need to design a distributed lock service that works across two geographically separated datacenters in active-active mode — both datacenters serve traffic. How do you approach this?**

**KEY PHRASE: "you can't have linearizable locks across datacenters and network latency under 100ms — pick one."** This is a genuine tension that has no clean solution. Reason through it:

For a lock to be safe (mutual exclusion), it must use a consensus system. A consensus system's write latency is bounded below by the round-trip time between the quorum members. If your quorum spans two datacenters with 50ms cross-DC RTT, every lock acquire costs at minimum 50ms. That is often unacceptable for high-frequency locks.

Options, in order of increasing complexity:

(1) **Single-region lock service**: Accept that locks are served from one region. The other region calls across the WAN to acquire locks. Simple, safe, but adds 50ms to all lock operations for the remote region and introduces a cross-DC dependency.

(2) **Geo-partitioned locks**: Split the lock namespace by region. Locks for European users are served from the European DC; locks for US users from the US DC. No cross-DC coordination for same-region locks. Breaks down when a resource (e.g., a shared inventory counter) spans regions.

(3) **Fencing with optimistic concurrency at the resource**: Use short-TTL advisory locks per region (fast, no cross-DC) combined with the resource itself performing the final consistency check via optimistic concurrency (version numbers in the database). The "lock" is just a hint that reduces contention; the database is the true arbiter. Requires the resource to support CAS.

(4) **Clock-based leases (Martin Fowler's "leases")**: Use GPS-synchronized clocks (AWS Time Sync, TrueTime in Spanner) to issue time-bounded leases. The holder is safe to act until `lease_expires - uncertainty_bound`. This requires hardware-level time synchronization not available on commodity clouds.

The correct answer for the interview: state the trade-off honestly — "linearizable cross-datacenter locks are fundamentally slow; the right answer depends on whether we truly need mutual exclusion across DCs or whether we can use a combination of local locks and idempotent operations at the resource to achieve the same safety property at lower latency."

**Q4: A senior engineer proposes replacing Kafka with a simple Redis pub/sub for your message queue. How do you respond?**

**KEY PHRASE: "Redis pub/sub is fire-and-forget; Kafka is durable and replayable."** This is a valid architecture discussion, not a gotcha question. Redis pub/sub has genuine advantages: sub-millisecond latency, zero operational overhead, and simplicity. But it has fundamental limitations that make it unsuitable for most production message queue use cases:

Redis pub/sub is **not durable** — if no subscriber is listening when a message is published, the message is gone forever. There is no retention, no replay, no offset management. If a consumer crashes and restarts, it has missed all messages published during the outage.

Redis pub/sub **doesn't support consumer groups** — every subscriber receives every message. For load-balanced processing (one consumer in the group processes each message), you'd need application-level coordination.

Redis pub/sub **doesn't support ordered delivery** — there's no partition concept, so ordering is only preserved within a single-client sequential publish rate.

**When Redis pub/sub IS appropriate**: real-time chat room presence (who's online), live sports score updates, real-time dashboard refresh signals — use cases where missing a message is tolerable, no replay is needed, and the event is ephemeral.

**When Kafka IS required**: payments, orders, audit events, anything where "did this event happen" is a business fact that must survive consumer downtime and be replayable — use Kafka.

A middle ground: **Redis Streams** (XADD/XREAD with consumer groups) provides persistence, consumer groups, and offset tracking with Redis-level latency. It is a reasonable middle ground for moderate throughput (millions of events/day) without Kafka's operational overhead.

---

## STEP 7 — MNEMONICS

### Mnemonic 1: "CRISP" — The five distributed systems problems

**C** — Consistent Hashing (key routing)
**R** — Replicated Cache (in-memory hot data)
**I** — idempotent locks (mutual exclusion with fencing)
**S** — Streaming queue (Kafka, durable fan-out)
**P** — Periodic scheduler (cron + DAG)

### Mnemonic 2: "TIRED" — The 8 shared components

**T** — TTL and lease expiry (safety valve for crashed processes)
**I** — Idempotency (retry without double effects)
**R** — Replication primary/replica (durability + read scale)
**E** — etcd / Raft consensus (exactly-one-winner coordination)
**D** — Data in memory for hot path (ring in L2, slot map, job queue)

(The remaining three — Stateless API Layer, Retry+Backoff, Admin API — are universal engineering fundamentals that apply everywhere.)

### Opening one-liner for any distributed systems question:

"The fundamental challenge in all distributed systems design is that we have multiple machines that need to agree on something, and we can't simultaneously have it be fast, always-available, and strongly consistent — so let me start by understanding which of those properties this specific use case cares most about."

This one sentence signals to the interviewer that you understand the CAP-triangle trade-off, you're going to drive the conversation with clarifying questions, and you're thinking architecturally rather than pattern-matching to a tool name.

---

## STEP 8 — CRITIQUE

### Well-covered by the source materials

The source files are unusually thorough in several areas:
- **Mathematical rigor on consistent hashing**: the vnode distribution math (standard deviation formula, simulation table showing 2.8% std dev at 150 vnodes), the proof that 1/N keys move on node addition, and the O(log N) binary search implementation are all solid and interview-ready.
- **Kafka internals depth**: ISR/HW/LEO, leader epoch, idempotent producer + transactions, log compaction, KRaft vs ZooKeeper — this covers everything up to principal-engineer depth.
- **Fencing token design**: the etcd revision-as-fencing-token insight, the zombie illustration, the Redlock critique, and the ZooKeeper alternative are all correctly explained and show genuine distributed systems expertise.
- **Capacity estimation methodology**: the source files show the full derivation chain (users → ops/day → peak ops/sec → nodes needed → bandwidth), which is exactly what interviewers want to see.

### Missing, shallow, or potentially wrong

**Missing:**
- **Multi-region and geo-distribution** is listed as out-of-scope in most files but not explained with enough depth for principal-level interviews. The cross-datacenter distributed lock question (Tier 3, Q3) partially addresses this but the source material doesn't have a section on it. Study Google Spanner's TrueTime and CockroachDB's hybrid logical clocks as supplementary material.
- **Observability depth**: the source mentions Prometheus and Grafana but doesn't specify what the critical alert thresholds are (e.g., ISR shrinks per second > 0 → page immediately; Redis eviction rate > 1000/sec → investigate; job DLQ depth > 0 → alert). Interviewers at staff level often probe on observability.
- **Backpressure mechanisms**: what happens when the system is overwhelmed? Kafka has `fetch.max.bytes` and consumer throttle delays. Redis has `maxmemory-policy noeviction` that rejects writes. The job scheduler has a finite worker pool. The lock service has rate limiting per client. Knowing how each system signals saturation and what the client is supposed to do about it (circuit breaker, exponential backoff, drop) is a gap in the source material.

**Shallow:**
- **Consistent hashing hotspot handling**: the source mentions detecting hotspots (CV > 5%) and triggering vnode rebalancing but doesn't detail the rebalancing algorithm. In practice, hotspot handling in consistent hashing is hard and often requires application-level workarounds (key sharding, adding a random suffix to hot keys). This is worth knowing.
- **Redis Cluster split-brain**: the source correctly says the minority partition stops writes, but doesn't explain what happens to in-flight client requests that were routed to nodes that got partitioned. Clients should receive `CLUSTERDOWN` errors and their retry behavior matters.

**Potentially oversimplified:**
- **"Fencing token at the resource" is presented as the clean solution** — it is, but most resources (vanilla PostgreSQL, S3, third-party APIs) don't support fencing tokens natively. The source acknowledges this briefly but the workarounds (proxy, optimistic locking, STONITH) deserve more depth than a bullet point.
- **Job scheduler trigger precision of 5 seconds** is presented as an NFR without explaining the practical sources of jitter: database polling interval (1s), queue dequeue delay, worker pool availability, and network RTT together can easily sum to 3-5s in a loaded system.

### Senior/staff probes that candidates frequently miss

1. **"What happens to the ring during a network partition in consistent hashing?"** — The ring is just a routing table. Different clients with different views of the ring will route the same key to different nodes. During a partition, you get split-brain routing, which means reads return stale data and writes may go to the wrong node. The source material doesn't address this. Answer: clients need a version check on the ring; if two clients have different ring versions, the one with the higher etcd revision is correct; the stale client will miss-route until it receives the ring update.

2. **"How do you handle the case where the Kafka `__consumer_offsets` topic itself becomes unavailable?"** — Consumer groups cannot commit offsets, so on restart they'll either start from `auto.offset.reset=earliest` (reprocess everything since last known committed offset) or `latest` (skip everything processed since the offset topic went down). This is a rare but real operational scenario that separates experienced Kafka engineers from novices.

3. **"Why does etcd have a default max key size of 1.5 MB and what does that tell you about what etcd is designed for?"** — etcd is designed for small, frequently-read coordination data (configuration, service discovery, locks), not for large payloads. If your design puts large objects in etcd, you're misusing it. This catches candidates who try to store job payloads or cache values in etcd rather than only using it for coordination metadata.

4. **"Your Redis Cluster has 8 shards. A key write pattern causes 80% of writes to go to shard 3. What are your options?"** — This is the hot-shard problem. Options: (1) Hash tags — if the key structure allows it, force a more even distribution. (2) Client-side spreading — add a random suffix (`key:0` through `key:9`) and fan-out writes to all suffixed keys; reads check all 10. (3) Add vnodes (not applicable to Redis Cluster which uses fixed hash slots, not vnodes). (4) Application redesign — is the hot key truly singular (e.g., a global counter) or artificially singular (a design flaw)? This is a great discussion about the limits of pure hash-based sharding.

### Common interview traps

- **Saying "I'll use Zookeeper" for a lock service without knowing that ZooKeeper has been deprecated in modern Kafka** (replaced by KRaft) and is considered operationally heavy by most teams. Say etcd for new designs, acknowledge ZooKeeper as the battle-tested alternative.
- **Confusing partition count with node count in Kafka**. You can have 100 partitions on 3 brokers (each broker stores ~33 partitions as leader or replica). Partition count determines consumer parallelism. Node count determines storage and throughput capacity.
- **Saying "consistent hashing solves data distribution"** without clarifying that it solves key routing only. The actual data distribution guarantees (replication, consistency, durability) are responsibilities of the underlying data store, not the ring.
- **Proposing Redis as a distributed lock backend without discussing fencing tokens**. A Redis-based lock without fencing tokens is only safe for advisory/efficiency locks. Never propose it for protecting a database write without explicitly acknowledging the zombie problem and your mitigation.
- **Not knowing the numbers**: 16,384 hash slots, 150 vnodes per node, `acks=all` + `min.insync.replicas=2`, 1,667 lock ops/sec, 139 jobs/sec peak. These are the anchor numbers that demonstrate you've actually studied these systems rather than just read a blog post about them.

---

*End of INTERVIEW_GUIDE.md — Pattern 14: Distributed Systems*
