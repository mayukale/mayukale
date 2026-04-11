# Problem-Specific Design — Distributed Systems (14_distributed_systems)

## Distributed Cache (Redis Cluster)

### Unique Functional Requirements
- Sub-millisecond read (P99 < 1 ms) / write (P99 < 2 ms) latency
- 1 million ops/sec cluster-wide; 500 M users × 20 ops/day = 350K peak ops/sec → 700K provisioned
- 200 GB hot dataset (500M users × 10% active × 2 KB); 8 primaries × 32 GB = 256 GB usable
- Data structures: strings, hashes, lists, sets, sorted sets
- Pub/Sub messaging; Lua scripting for atomic multi-step ops
- RDB snapshots + AOF append-only log persistence (configurable)
- Cluster mode: automatic sharding across 8 primary + 8 replica nodes (16 total)

### Unique Components / Services
- **Cluster Router**: reads CLUSTER SLOTS map; routes to correct shard; handles MOVED (permanent slot reassign) and ASK (mid-migration redirect) responses; no external coordinator
- **Cluster Bus (Gossip)**: port = primary_port + 10000; heartbeat every 1 s; propagates slot assignments, PFAIL/FAIL votes, node join/leave in O(log N) gossip rounds; heartbeat payload includes 2 KB slot assignment bitmap (16,384 bits)
- **Eviction Manager**: approximate LFU using 8-bit Morris counter (logarithmic increment: P(increment) = 1/(freq × base + 1)); decay by lfu_decay_time minutes; samples 5 random keys, evicts min-frequency; `allkeys-lfu` for hot-data caches, `volatile-lru` for TTL-tagged session stores
- **Persistence Layer**: RDB (point-in-time fork + BGSAVE); AOF (log every write, `appendfsync everysec` default — at most 1 second loss); AOF rewrite via BGREWRITEAOF to compact log
- **Live Resharding Engine**: CLUSTER SETSLOT MIGRATING/IMPORTING; iterative MIGRATE per key (batch 100); MOVED gossip propagates final assignment; keys in-flight handled by ASK redirect during migration

### Unique Data Model
- **Hash slot assignment**: CRC16(key) % 16384 → hash slot → primary node; slot bitmap = 2 KB per gossip message
- **Hash tags**: `{user.12345}.session` and `{user.12345}.cart` hash only `user.12345` → same slot → enables atomic MULTI/EXEC across related keys
- **Key layout**: `session:{user_id}` (string, 500 B avg, 50 GB total), `feed:{user_id}:{page}` (list, 5 KB avg, 80 GB total), `rl:{user_id}:{window}` (string counter, 64 B avg, 10 GB total), `lb:{game_id}` (sorted set, 20 GB total)
- **LFU counter**: upper 8 bits = last-decrement time (minutes); lower 8 bits = log-frequency (max 255); stored in repurposed lru_clock field; new keys initialized to 5 (grace period)
- **Replication**: async primary → replica via partial resync (PSYNC2 with replication offset); WAIT 1 0 for near-synchronous confirmation

### Algorithms

**Approximate LFU (Morris Counter) — Eviction:**
```
P(increment) = 1 / (freq × base_probability + 1)
decay: every lfu_decay_time minutes without access, decrement freq by 1
evict: sample 5 random keys; evict min(decay_lfu_counter(k)) among candidates
```

**PER (Probabilistic Early Reexpiration) — Cache Stampede Prevention:**
```python
def get_with_stampede_prevention(key, beta=1.0):
    value, expiry = cache.get_with_expiry(key)
    ttl = expiry - time.now()
    # Early recalculation probability increases as TTL approaches 0
    if ttl - beta * math.log(random.random()) < 0:
        value = recompute_and_cache(key)
    return value
```

**Live Resharding (Zero-Downtime):**
- Mark source MIGRATING + target IMPORTING
- Batch MIGRATE keys (100 per round) with REPLACE flag
- CLUSTER SETSLOT NODE broadcasts final assignment via gossip
- MOVED: permanent, client updates slot map; ASK: temporary during migration, client sends ASKING first

### Scale Numbers
- 8 primaries + 8 replicas = 16 nodes; 32 GB RAM each; 256 GB total capacity
- Peak: 350,000 ops/sec; provisioned for 700,000 ops/sec (50% utilization headroom)
- Read throughput: 274 MB/sec; write: 68 MB/sec; replication bandwidth: 43 MB/sec per node
- Failover: majority (N/2+1) of masters vote `FAILOVER_AUTH_ACK`; split-brain minority stops writes

### Key Differentiator
Distributed Cache's uniqueness is its **cluster hash slot gossip with approximate LFU**: CRC16 % 16384 hash slots (2 KB gossip bitmap), zero-coordinator live resharding via MIGRATING/IMPORTING/MOVED/ASK redirect protocol, approximate LFU Morris counter (8-bit log-frequency with time-decay) as the eviction policy for Zipf-distributed access patterns, and PER (probabilistic early reexpiration) for cache stampede prevention — designed as a memory-first tiered cache, not a persistent database.

---

## Distributed Message Queue (Kafka)

### Unique Functional Requirements
- 10 GB/sec write throughput cluster-wide; P99 end-to-end < 10 ms
- 500 M events/day; 30,000 peak events/sec; avg 1 KB/message
- Ordered delivery within partition; replay from any offset within retention
- Consumer groups: each message to exactly one consumer per group; fan-out across unlimited groups
- Exactly-once semantics: idempotent producer + transactions (EOS)
- Log compaction: retain only latest value per key (cleanup.policy=compact)
- Schema registry: Avro/Protobuf schemas with compatibility checks

### Unique Components / Services
- **Broker (Kafka)**: 20 brokers; 500 partitions (50 topics × 10 partitions); NVMe SSDs; sequential disk writes via OS page cache; leader handles all reads/writes for a partition
- **ISR (In-Sync Replicas)**: each partition has RF=3 replicas; ISR = set of replicas within `replica.lag.time.max.ms` of leader LEO; `min.insync.replicas=2` means at least 2 must ACK before leader responds (`acks=all`); message only visible to consumers after HW (High Watermark) advances to include it
- **KRaft (Kafka Raft Metadata)**: replaces ZooKeeper for cluster metadata (broker configs, partition assignments, leader elections); built-in Raft controller quorum; removes ZooKeeper operational dependency
- **Idempotent Producer**: each producer assigned PID (Producer ID) by broker; per-partition sequence number starts at 0; broker maintains sliding dedup window of last 5 in-flight sequences per (PID, partition); retried messages with same sequence are deduplicated
- **Transactions**: atomic multi-partition write; `beginTransaction()` → produce to multiple partitions → `commitTransaction()`; coordinator stores transaction state in `__transaction_state` (hash of transactional_id % 50 partitions); consumers with `isolation.level=read_committed` skip uncommitted records
- **Log Compaction**: background cleaner thread; retains latest record per key; tombstone (null value) marks deletion; `cleanup.policy=compact` per topic; compaction lag bounded by `log.cleaner.min.cleanable.ratio`
- **Consumer Group Coordinator**: manages group membership; triggers rebalance on join/leave/heartbeat timeout; partition assignment via Range/RoundRobin/Sticky assignors; rebalance P99 < 30 s; static membership (`group.instance.id`) reduces rebalances on rolling restarts

### Unique Data Model
- **RecordBatch v2**: magic byte 2; batch header (base_offset, batch_length, partition_leader_epoch, attributes, last_offset_delta, base_timestamp, max_timestamp, producer_id, producer_epoch, base_sequence); records compressed as a batch (lz4/snappy/zstd)
- **Offset index**: sparse; one entry per 4 KB of log data; maps logical offset → physical byte position; binary search for random-access reads
- **`__consumer_offsets`** (50 partitions): `(group_id, topic, partition)` → committed offset; compacted topic; consumer lag = partition LEO - committed offset
- **`__transaction_state`** (50 partitions): transactional_id → transaction metadata (PID, epoch, state: ONGOING/PREPARE_COMMIT/COMPLETE); compacted

### Scale Numbers
- 20 brokers; 10.5 TB cluster storage (500 GB/day × 3× replication × 7 days)
- 525 GB per broker; ~700 GB–1 TB provisioned (30% headroom)
- Peak write: 30 MB/sec inbound; with replication: 90 MB/sec disk writes cluster-wide
- Consumer groups: 3 groups × 30 MB/sec = 90 MB/sec outbound; per-broker: ~12 MB/sec

### Key Differentiator
Distributed Message Queue's uniqueness is its **ISR-based replication with exactly-once EOS**: High Watermark / LEO / ISR tracking for durability guarantees, idempotent producer (PID + per-partition sequence dedup window) + transaction coordinator for cross-partition atomic writes, KRaft replacing ZooKeeper for metadata consensus, log compaction (`cleanup.policy=compact`) for event-sourcing topics, and RecordBatch v2 compressed batch format — designed for durable, ordered, replayable event streaming at 10 GB/sec, not an ephemeral message broker.

---

## Distributed Lock (etcd-backed)

### Unique Functional Requirements
- Exclusive locks on named resources; at most one holder at any time (safety)
- TTL-based lease expiry; auto-renew at 2/3 of TTL to prevent expiry during active hold
- Fencing tokens: monotonically increasing integer issued on every acquisition; resource guard rejects token ≤ last_seen
- Wait-with-timeout: client waits up to N ms for lock, then gives up
- Audit log: every acquire/release/expire event logged with client_id, lock_name, fencing_token, timestamp
- 10,000 service instances; 1,667 ops/sec; 83 concurrent holders; 9 MB total state

### Unique Components / Services
- **Lock Client Library**: handles acquire/release/renew via gRPC; retry with exponential backoff; auto-renew thread monitors lease expiry (renew at 2/3 of TTL, e.g., 20 s for 30 s TTL); passes fencing token to all downstream resource calls
- **Lock Service API Layer**: stateless; validates JWT/mTLS client identity; rate-limits per client; translates to etcd CAS transactions; assigns and returns fencing tokens
- **etcd Backend** (3 or 5 nodes): Raft consensus; linearizable reads (served by leader); atomic CAS transactions `(compare version('/locks/name') == 0 → put '/locks/name' with lease)`; Watch API for waiters to receive lock-released notification without polling
- **Fencing Token Service**: fencing token = etcd cluster revision after successful lock write (globally unique, monotonically increasing); no separate counter needed — etcd's own Raft revision serves as the token
- **Resource Guard**: tracks `last_seen_token` per resource_name; rejects any operation with fencing_token ≤ last_seen; sole enforcement point for zombie detection

### Unique Data Model
- **etcd lock key**: `/locks/{lock_name}` → `{"holder_id": client_id, "acquired_at": time_ms()}`; attached to lease (TTL); key deleted automatically when lease expires
- **Fencing token**: `txn_response.header.revision` from etcd after successful CAS write; revision increments monotonically with every etcd write cluster-wide; range 0 to ~2^63
- **Audit log**: append-only Kafka topic or Kinesis stream; fields: client_id, lock_name, fencing_token, timestamp, action (ACQUIRED/RELEASED/EXPIRED/RENEWED)

### Algorithms

**etcd Atomic Lock Acquire:**
```python
txn_response = etcd.transaction(
    compare=[etcd.transactions.version('/locks/' + lock_name) == 0],  # not exists
    success=[etcd.transactions.put('/locks/' + lock_name, holder_json, lease=lease)],
    failure=[]
)
fencing_token = txn_response.header.revision  # etcd revision = monotonic token
```

**Redlock Critique (Why Not Used):**
- Requires 5 independent Redis nodes; quorum = 3
- Clock drift vulnerability: if Node 3's clock jumps forward, it expires the key prematurely → two clients simultaneously hold quorum (Martin Kleppmann's critique, 2016)
- No fencing tokens: Redlock uses random value for release safety, but does not issue a monotonically increasing token → cannot detect zombie writes at resource
- Conclusion: Redlock is **unsafe for storage mutations** requiring fencing; etcd Raft provides linearizable guarantees without clock dependency

### Scale Numbers
- 3 or 5 etcd nodes; 9 MB total lock state; 1,667 acquire+release ops/sec average; 8,335 ops/sec peak (5× burst)
- Lock acquisition P99 < 5 ms (no contention), < 50 ms (wait queue); release P99 < 3 ms
- Audit log: 1,667 × 86,400 × 7 × 200 B ≈ 201 GB per week (append-only)

### Key Differentiator
Distributed Lock's uniqueness is its **etcd-revision fencing token design**: the lock's acquisition revision in etcd Raft is the fencing token (no separate counter), the Resource Guard at the protected resource validates token ≥ last_seen (not the lock service), auto-renewal at 2/3 of TTL prevents GC-pause expiry, and the Watch API replaces polling for waiters — contrasted with Redlock (clock-drift unsafe, no fencing tokens) and ZooKeeper (ephemeral znodes as alternative) as rejected alternatives.

---

## Distributed Job Scheduler

### Unique Functional Requirements
- Exactly-once execution per trigger across multiple scheduler nodes
- Cron jobs (RFC 5321 cron expression) and one-time scheduled jobs
- Priority levels 0–9; DAG dependencies (job B fires only after job A completes)
- Failure handling: retry with exponential backoff; dead letter queue (DLQ) after max_retries
- 100,000 cron jobs; 139 jobs/sec peak fire rate; 2,000 concurrent worker processes
- Execution history: 90-day retention; 1.08 TB total (139 jobs/sec × 86,400 × 90 × avg 1 KB)

### Unique Components / Services
- **Trigger Engine** (multi-node, no leader required): polls `scheduled_triggers` every 1 s; claims due rows with `FOR UPDATE SKIP LOCKED`; atomically sets status=CLAIMED; creates execution record + fencing token; enqueues to Redis job queue
- **Redis Job Queue**: sorted set per priority (ZADD score=scheduled_time); ZPOPMIN for worker dequeue; sub-millisecond enqueue/dequeue; workers claim via atomic ZPOPMIN → execute → complete/fail
- **Worker Pool**: stateless workers; heartbeat to API every 30 s (signals liveness + progress); receives cancel signal via heartbeat response; Kubernetes HPA autoscales based on queue depth (target: depth/workers ratio < 0.8)
- **Status Manager**: updates job_executions on heartbeat, completion, failure; detects timeout: `last_heartbeat < NOW() - timeout_seconds`; marks as FAILED and re-enqueues if max_retries not exhausted
- **DAG Dependency Engine**: stores edges in job_dependencies table; on job completion, checks dependency_fulfillments; when all parent executions complete, inserts trigger for child job; cycle detection via Kahn's algorithm at job registration time
- **Dead Letter Queue (DLQ)**: separate Redis sorted set; executions moved here after max_retries; admin API for force-retry; alerting on DLQ depth

### Unique Data Model
- **job_definitions**: job_id, name, handler, cron_expression, timezone, payload JSONB, priority (0-9), max_retries, retry_backoff_seconds, timeout_seconds, idempotency_key, status
- **scheduled_triggers**: trigger_id, job_id, scheduled_time, status (PENDING/CLAIMED/FIRED), claimed_by, claimed_at; indexed on (scheduled_time, status) for trigger engine poll
- **job_executions**: execution_id, job_id, trigger_id, status (QUEUED/RUNNING/COMPLETED/FAILED/TIMED_OUT), worker_id, fencing_token, started_at, completed_at, last_heartbeat, attempt_number, output JSONB, error_message; partitioned by `created_at` monthly
- **job_dependencies**: dependent_job_id → depends_on_job_id; DAG edges
- **dependency_fulfillments**: execution_id, dependency_job_id; tracks which parents have completed for a given execution

### Algorithms

**Exactly-Once Trigger Firing (FOR UPDATE SKIP LOCKED):**
```sql
SELECT t.trigger_id, t.job_id, t.scheduled_time, ...
FROM scheduled_triggers t JOIN job_definitions j ON t.job_id = j.job_id
WHERE t.scheduled_time <= NOW() + INTERVAL '1 second'
  AND t.status = 'PENDING' AND j.status = 'ACTIVE'
ORDER BY t.scheduled_time ASC, j.priority ASC
LIMIT 1000
FOR UPDATE OF t SKIP LOCKED
```
Multiple scheduler nodes run this query simultaneously; PostgreSQL row locks ensure each trigger row is claimed by exactly one node.

**Retry Backoff Formula:**
```python
delay = min(max_delay_seconds, retry_backoff_seconds * 2 ** attempt) + random(0, jitter_ms / 1000)
# Default: base=300s, max=3600s, jitter=30s
```

**DAG Cycle Detection (Kahn's Algorithm) at registration:**
```python
in_degree = {job: 0 for job in all_jobs}
for dep in dependencies: in_degree[dep.dependent] += 1
queue = [j for j in all_jobs if in_degree[j] == 0]
visited = 0
while queue:
    j = queue.pop(); visited += 1
    for child in children[j]:
        in_degree[child] -= 1
        if in_degree[child] == 0: queue.append(child)
if visited != len(all_jobs): raise CyclicDependencyError
```

**Fencing Token for Zombie Detection:**
- Each execution record has a unique fencing_token assigned at claim time
- Worker sends `X-Fencing-Token` header on heartbeat and complete requests
- `POST /v1/internal/executions/{id}/complete` responds 409 CONFLICT if token ≠ execution.fencing_token → zombie write rejected

### Scale Numbers
- 100,000 cron jobs; 139 jobs/sec peak; 2,000 worker processes
- PostgreSQL: 1.08 TB execution history over 90 days; monthly range partitions
- Redis queue: sub-millisecond enqueue/dequeue; priority levels 0–9 as separate sorted sets
- Kubernetes HPA: autoscales worker pool based on queue depth; target utilization 62%

### Key Differentiator
Distributed Job Scheduler's uniqueness is its **PostgreSQL FOR UPDATE SKIP LOCKED multi-node trigger claiming**: no leader election needed — multiple scheduler nodes compete for trigger rows atomically via PostgreSQL row locks, guaranteeing exactly-once firing; combined with DAG dependency engine (Kahn's cycle detection at registration, dependency_fulfillments tracking at runtime), fencing tokens for zombie worker detection, and exponential backoff + DLQ for failure handling — designed as a persistent, exactly-once cron-and-DAG scheduler, not a task queue.

---

## Consistent Hashing

### Unique Functional Requirements
- O(log N) key lookup; minimal key movement on topology change (1/N keys)
- 150 virtual nodes per physical node → standard deviation < 3% of mean across 20 nodes
- 1M RPS; 20 physical nodes; 48 KB ring state per client (fits in L2 cache)
- Ring topology changes propagated to all clients within < 5 s
- Weighted nodes: a 2× capacity node receives 2× vnodes (proportional assignment)

### Unique Components / Services
- **Consistent Hash Router** (in-process client library): sorted array of (hash_position, vnode_id, node_ref); binary search `get_node(key)`: O(log N) = 12 comparisons for 3,000 vnodes = ~120 ns; ring state updated by Ring Manager via etcd Watch; no network hop per lookup
- **Ring Manager Service**: maintains authoritative ring config in etcd; coordinates key migration on node add/remove; detects load imbalance (CV > 5%); publishes ring delta (150 vnodes × 16 bytes = 2.4 KB) on topology change
- **Gossip Protocol Layer**: distributes ring updates to all clients; O(log N) propagation rounds; bounded < 5 s end-to-end
- **Migration Daemon** (per new node): pulls key ranges from predecessor nodes using SCAN + DUMP/RESTORE pipeline; throttled to 50 MB/sec to avoid impacting live traffic; reports migration progress to Ring Manager

### Algorithms

**Vnode Position Computation (xxHash32):**
```python
import xxhash
def compute_ring(nodes, vnodes_per_node=150):
    ring = []
    for node in nodes:
        actual_vnodes = round(node.weight * vnodes_per_node)
        for i in range(actual_vnodes):
            seed = f"{node.node_id}:{i}"
            position = xxhash.xxh32(seed).intdigest()  # 32-bit ring
            ring.append(VNode(position, f"{node.node_id}:{i}", node))
    ring.sort(key=lambda v: v.hash_position)
    return ring

def get_node(ring, key):
    h = xxhash.xxh32(key).intdigest()
    lo, hi = 0, len(ring) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if ring[mid].hash_position < h: lo = mid + 1
        else: hi = mid
    return ring[lo % len(ring)].physical_node  # wrap-around at ring boundary
```

**Double-Write Migration Protocol:**
- Phase 1 PREPARE: announce new node (status=JOINING); clients double-write to both old and new nodes for keys in new node's target range
- Phase 2 MIGRATE: bulk copy existing keys from predecessor via SCAN + DUMP/RESTORE (throttled 50 MB/sec)
- Phase 3 SWITCH: atomic update status=JOINING → ACTIVE; clients route to new node; reads fall back to predecessor on cache miss for brief window
- Phase 4 CLEANUP: predecessors lazily delete migrated keys after 2× max replication lag

**Node Addition Key Movement (Proof):**
- New node with V vnodes inserts V points into ring; each point "steals" key range from clockwise predecessor
- Total stolen range = V / (N×V) = 1/N of ring → exactly 1/N of all keys migrate; theoretical minimum

**Alternatives Rejected:**
| Algorithm | Mechanism | Why Rejected |
|---|---|---|
| **Modular hashing** | `hash(key) % N` | Moves ~100% of keys on node add/remove |
| **Jump Consistent Hash** | `ch(key, N)` O(log N) computation, no ring structure | No weighted nodes; requires contiguous node IDs; poor for node removal |
| **Rendezvous Hashing** | Score `H(key, node)` per node; pick max; O(N) lookup | O(N) lookup too slow at N=20,000 nodes; recompute all scores per request |

### Scale Numbers
- 20 nodes × 150 vnodes = 3,000 total; 48 KB ring per client (3,000 × 16 B)
- Ring lookup: 12 binary search comparisons × 10 ns = 120 ns; 0.12 cores for 1M RPS
- Ring update broadcast: 150 vnodes × 16 B = 2.4 KB; 100 clients × 2.4 KB = 240 KB per topology change
- Standard deviation at 150 vnodes: ~2.8%; max/min ratio 1.3:1 (vs 8:1 with 1 vnode)

### Key Differentiator
Consistent Hashing's uniqueness is its **xxHash32 vnode ring with mathematical distribution guarantees**: 150 vnodes per node achieving < 3% std dev in key distribution (vs 20% with 1 vnode), O(log N) binary search in 48 KB sorted array (fits in L2 cache, ~120 ns), double-write zero-downtime migration protocol (write to both old and new nodes during migration phase), and rigorous comparison of alternatives (Jump Consistent Hash: no weighted nodes; Rendezvous Hashing: O(N) lookup) — designed as a deterministic routing algorithm for cache/shard tier, not a data store.
