# Common Patterns — Distributed Systems (14_distributed_systems)

## Common Components

### Stateless API / Service Layer + Persistent Backend
- All five designs split into a stateless application layer (horizontally scalable) and a persistent backend (durable, replicated)
- distributed_cache: Cluster Router / Proxy (stateless) → Redis Shard Primaries (persistent)
- distributed_message_queue: Kafka Client Library (stateless) → Broker replicas (durable log on NVMe)
- distributed_lock: Lock Service API Layer (stateless, JWT/mTLS auth) → etcd cluster (Raft-replicated)
- distributed_job_scheduler: API/Control Plane (stateless REST) → PostgreSQL + Redis job queue (persistent)
- consistent_hashing: Consistent Hash Router (in-process, stateless) → Physical Nodes + etcd Ring Manager (persistent ring config)

### etcd / Raft-Based Coordination
- Three of five use etcd (or equivalent Raft consensus) for strongly-consistent coordination
- distributed_lock: etcd is the primary lock store — atomic CAS transaction, TTL-based leases, Watch API for waiters; Raft ensures linearizable reads; /locks/{lock_name} key format
- distributed_job_scheduler: leader election via etcd for trigger engine (optional approach); fencing tokens issued per execution to detect zombie workers
- consistent_hashing: etcd stores authoritative ring configuration; Watch API used by clients to receive ring topology changes within < 5 s

### Primary / Replica Replication
- All five replicate state for availability; replication strategy differs per system
- distributed_cache: async primary → replica replication; `WAIT 1 0` for near-synchronous; replica serves reads; failover via quorum vote (N/2+1 masters must acknowledge `FAILOVER_AUTH_ACK`)
- distributed_message_queue: ISR (In-Sync Replicas); replication factor 3; min.insync.replicas=2; leader tracks LEO (Log End Offset) per follower; message visible to consumers only after HW (High Watermark) advances
- distributed_lock: etcd runs 3 or 5 nodes; Raft write quorum = majority; read is linearizable (served by leader)
- distributed_job_scheduler: PostgreSQL primary + read replicas; Redis primary + replica per shard
- consistent_hashing: replication across N nodes is a concern of the underlying key store (Dynamo-style: get_nodes_for_replication returns N distinct physical nodes clockwise)

### TTL / Lease-Based Expiry
- All five use time-based expiry as a safety mechanism to prevent indefinite resource hold
- distributed_cache: per-key TTL via EXPIRE/EX; eviction policies (allkeys-lfu, volatile-lru) when maxmemory reached
- distributed_message_queue: topic-level retention by time (default 7 days) or size (100 GB); key TTL for log-compacted topics (tombstone with null value)
- distributed_lock: mandatory lease TTL on every lock; auto-renew at 2/3 of TTL to prevent expiry while holder is alive; max TTL cap (e.g., 60 s) enforced by lock service
- distributed_job_scheduler: job execution timeout_seconds; heartbeat-based liveness detection; job times out and enters DLQ if no heartbeat within timeout window
- consistent_hashing: no TTL on ring entries; etcd lease TTL used for ephemeral node membership (node announces itself with a lease; if it crashes, lease expires and ring manager removes it)

### Retry with Exponential Backoff + Jitter
- All five require retry logic on the client path; exponential backoff + jitter prevents thundering herd
- distributed_cache: client retries on MOVED/ASK redirects; backoff on connection errors
- distributed_message_queue: producer retry on LEADER_NOT_AVAILABLE; consumer retry on offset commit failure; `retry.backoff.ms` config
- distributed_lock: lock client retries acquisition with `jitter = random(0, base_delay)` to avoid synchronized stampede on lock release
- distributed_job_scheduler: worker retry backoff formula: `delay = min(max_delay, base_delay * 2^attempt) + random(0, jitter_ms)`; max_retries configurable per job; exponential backoff 300s base
- consistent_hashing: client retries on not_owner error with brief delay; ring refresh if stale view detected

### Idempotency Mechanisms
- Four of five implement idempotency to prevent duplicate effects from retries or duplicate deliveries
- distributed_cache: SETNX (SET if Not eXists) for atomic conditional writes; GET → compare → SET with Lua for check-and-set atomicity
- distributed_message_queue: idempotent producer with PID (Producer ID) + monotonic sequence number per partition; broker deduplicates within sliding window of last 5 in-flight requests; transactions for cross-partition exactly-once
- distributed_lock: fencing token (etcd cluster revision) uniquely identifies each lock acquisition; resource guard rejects operations with token ≤ last_seen; prevents zombie write duplication
- distributed_job_scheduler: idempotency_key field on job definitions; unique index prevents duplicate job registration; fencing token on each execution record; complete endpoint rejects if `X-Fencing-Token` ≠ execution's token
- consistent_hashing: double-write protocol ensures key migration is idempotent (RESTORE with replace=True)

### In-Memory Data Structures for Hot-Path Operations
- All five optimize the critical path using in-memory structures to avoid disk I/O or network round-trips
- distributed_cache: Redis in-memory hash table; eviction pool for approximate LFU sampling; cluster slot assignment in 2 KB bitmap in gossip heartbeat
- distributed_message_queue: page cache (OS-managed) for sequential log reads; offset index (sparse: every 4 KB of log data) in memory; consumer offset cache in `__consumer_offsets` topic (memory-mapped)
- distributed_lock: etcd stores all lock entries in Raft-replicated in-memory store (BoltDB backed); 9 MB total state — negligible
- distributed_job_scheduler: Redis sorted sets for active job dispatch queue; sub-millisecond enqueue/dequeue; PostgreSQL for durable trigger state
- consistent_hashing: ring = sorted array of (hash_position, vnode_id, node_ref); 48 KB for 3,000 vnodes on 20 nodes; fits in L2 cache; O(log N) binary search = 120 ns per lookup at 1M RPS

### Admin API for Runtime Configuration
- All five expose admin APIs for hot-reconfiguration without service restart
- distributed_cache: `CLUSTER SETSLOT` for live resharding; `CONFIG SET maxmemory-policy allkeys-lfu` for hot eviction policy change; `CLUSTER FAILOVER` for manual failover
- distributed_message_queue: `POST /v1/admin/topics` (create/delete); `GET /v1/admin/consumer-groups/{group}/offsets` (lag monitoring); `POST /v1/admin/topics/{topic}/partitions` (increase partitions)
- distributed_lock: `GET /v1/locks/{name}` (inspect lock state); `DELETE /v1/locks/{name}` (admin force-release); `PUT /v1/locks/{name}/ttl` (update default TTL)
- distributed_job_scheduler: `POST/PUT/DELETE /v1/cron-jobs`; `POST /v1/cron-jobs/{id}/pause` / resume; `POST /v1/executions/{id}/retry` (DLQ force-retry); all within 30s propagation
- consistent_hashing: `POST /v1/ring/nodes` (add node); `DELETE /v1/ring/nodes/{id}` (graceful remove); `POST /v1/ring/nodes/{id}/mark_down` (emergency); ring change propagated to clients via etcd Watch

## Common Databases

### etcd
- distributed_lock (primary lock store), consistent_hashing (authoritative ring config); Raft consensus; linearizable reads; Watch API for change notification; lease-based TTL

### PostgreSQL
- distributed_job_scheduler (job_definitions, scheduled_triggers, job_executions, job_dependencies tables); ACID transactions; `FOR UPDATE SKIP LOCKED` for multi-node exactly-once trigger claiming; JSONB for flexible payload; time-range partitioning for execution history

### Redis
- distributed_cache (primary data store); distributed_job_scheduler (active job dispatch queue); consistent hashing in-memory store (for cache tier); sub-millisecond ops; Lua scripting for atomicity

### Kafka
- distributed_message_queue (primary system); distributed_job_scheduler uses Kafka-style async event log for audit; all systems emit operational events to Kafka for downstream monitoring

## Common Communication Patterns

### Gossip Protocol for Cluster Membership
- distributed_cache: Redis Cluster Bus on port (primary_port + 10000); heartbeat every 1s; propagates slot assignments, PFAIL/FAIL votes, node join/leave in O(log N) gossip rounds
- consistent_hashing: gossip layer propagates ring delta (150 vnodes × 16 bytes = 2.4 KB per change) to all clients; bounded propagation < 5 s

### gRPC for Internal Service Communication
- distributed_lock: Lock Client → Lock Service API Layer via gRPC (AcquireLock, ReleaseLock, RenewLock, WatchLock)
- distributed_job_scheduler: API ↔ workers via gRPC internally; worker heartbeat every 30 s; cancel signal via heartbeat response

## Common Scalability Techniques

### Horizontal Scaling via Stateless API Layer
- All five scale the request-handling layer horizontally; no session state in API tier; state externalized to Redis/PostgreSQL/etcd

### Partitioning / Sharding by Key
- distributed_cache: CRC16(key) % 16384 hash slots → shard nodes; hash tags {user.12345} pin related keys to same slot
- distributed_message_queue: partition by producer key (e.g., user_id or order_id) for ordering within partition
- distributed_job_scheduler: PostgreSQL job_executions table partitioned by time range (monthly) for query performance on historical data
- consistent_hashing: ring partitions keyspace by hash range; vnodes provide uniform distribution

## Common Deep Dive Questions

### How do you guarantee exactly-once semantics in a distributed system?
Answer: Three distinct mechanisms across these problems. (1) Idempotent producer (Kafka): broker deduplicates using producer PID + per-partition sequence number; works for single-partition exactly-once. (2) Transactions (Kafka): multi-partition atomic write with two-phase commit; coordinator manages transaction state in __transaction_state topic. (3) Database row locking (job scheduler): `FOR UPDATE SKIP LOCKED` atomically claims a trigger row; only one scheduler node claims each row regardless of how many are running. All three require a monotonically increasing token (sequence number, etcd revision, or transaction ID) to detect and reject duplicates.
Present in: distributed_message_queue, distributed_job_scheduler, distributed_lock

### How do you handle node failure and recovery without data loss?
Answer: Pattern is consistent: durable write acknowledgment before success response, plus replication before ACK. In Kafka: `acks=all` requires all ISR members to acknowledge before producer receives success; if leader fails, a new leader is elected from ISR (guaranteed to have all committed messages). In Redis Cluster: `WAIT 1 0` can enforce sync replica acknowledgment; without it, async replication means ~5 ms window of potential data loss on failover. In etcd: Raft majority quorum; write committed only after majority of nodes persist — single node failure cannot lose committed data.
Present in: distributed_cache, distributed_message_queue, distributed_lock

## Common NFRs

- **Throughput**: 1M ops/sec (cache), 10 GB/sec (queue), 1,667 ops/sec (lock), 139 jobs/sec peak (scheduler), 1M RPS lookups (consistent hashing)
- **Latency**: P99 < 1 ms read (cache), P99 < 10 ms end-to-end (queue), P99 < 10 ms acquire (lock), P99 < 120 ns lookup (consistent hashing)
- **Availability**: 99.99% across all five; fail-open or leader election on node failure
- **Durability**: configurable (cache: AOF fsync everysec vs. in-memory); guaranteed once ACKed (queue, lock, scheduler)
- **Scalability**: horizontal by adding nodes/shards; ring/slot rebalancing migrates minimum keys (1/N fraction)
- **Idempotency**: all systems handle retried requests without duplicate side effects
