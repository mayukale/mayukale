# Common Patterns — Distributed Systems Primitives (07_distributed_systems_primitives)

## Common Components

### etcd + Raft Consensus as Coordination Backbone
- 5 of 7 systems use etcd (or a Raft-based store) for linearizable coordination; the fundamental building block
- distributed_configuration_management: primary KV store; Watch API with revision-based guarantees; `write < 20 ms` (Raft consensus)
- distributed_lock_service: Raft state machine stores lock state, sessions, fencing counters; every lock op is a Raft proposal
- leader_election_service: `/elections/{name}/leader` key held with etcd lease; `/elections/{name}/fencing_token` monotonic counter
- service_discovery_system: service registry Raft-replicated across 3–5 nodes; Watch for real-time change notification
- consistent_hashing_system: ring metadata distributed via gossip (not Raft), but uses version numbers for convergence

### Lease-Based TTL with TTL/3 Keep-Alive Interval
- 5 of 7 systems use lease + periodic keep-alive; `keep_alive_interval = TTL / 3` tolerates one missed heartbeat before expiry
- distributed_lock_service: Session TTL default 15 s; heartbeat every 5 s; session death → all held locks released
- leader_election_service: Lease TTL default 15 s; leader renews every 5 s; TTL expiry → etcd deletes leader key → all watchers notified
- service_discovery_system: TTL-based health check (client must heartbeat); missed TTL → mark CRITICAL
- distributed_configuration_management: secret TTL-based expiry; rotation with 300 s grace period
- distributed_rate_limiter: policy TTL; hot reload via watch

### Fencing Tokens (Monotonically Increasing, Reject Stale Operations)
- 4 of 7 systems issue monotonically increasing fencing tokens on grant/election; protected resources reject `token < last_seen`
- distributed_lock_service: `fence_token = global_fence += 1` on each lock grant; storage layer rejects writes with stale token
- leader_election_service: `fencing_token` CAS-incremented on leadership change; old leader's writes rejected at resource layer
- consistent_hashing_system: `ring_version UINT64` incremented on topology change; stale ring clients detect via version mismatch

### Watch / Subscribe API (Streaming with Revision-Based Guarantee)
- 5 of 7 systems provide streaming change notification; clients resume from `last_seen_revision + 1` — no missed events
- distributed_configuration_management: `watch_from(start_revision=last_seen + 1)`; etcd caches all revisions
- distributed_lock_service: Watch lock state changes; waiters receive notification when lock released
- leader_election_service: watchers on `/elections/{name}/leader` notified when key deleted (leader dead)
- service_discovery_system: gRPC streaming watch; new/down instances propagated within < 5 s
- distributed_rate_limiter: policy watch for hot reload; sidecar picks up new limits without restart

### FIFO Wait Queue + Writer-Preference for Locks
- distributed_lock_service: explicit `wait_queue: LIST<WaitEntry>` per lock key; FIFO grant order; once EXCLUSIVE waiter queued, no new SHARED waiters admitted (writer-preference prevents writer starvation)
- leader_election_service: ZooKeeper variant uses ephemeral sequential znodes — lowest sequence number is leader; others watch the next-lower node
- consistent_hashing_system: virtual nodes placed in sorted order (UINT64 ring_positions SORTED_ARRAY); O(log N) binary search for next responsible node

### Health Check Threshold Logic (Prevent Flapping)
- service_discovery_system + distributed_lock_service + leader_election_service use consecutive threshold logic:
  - Mark CRITICAL only after `consecutive_failures >= failure_threshold` (default 3)
  - Mark PASSING only after `consecutive_passes >= success_threshold` (default 2)
  - Prevents single transient failure from triggering cascading deregistration

## Common Databases

### etcd
- 5 of 7; linearizable KV store; Watch API; Raft consensus; lease-based keys with TTL; P99 write < 25 ms (SSD)

### MySQL
- 3 of 7 (config_management audit, transaction_coordinator log, lock_service WAL); audit trails; version history; transaction logs; append-only; 1-year retention

### Redis Cluster
- 2 of 7 (rate_limiter primary, config_management cache); atomic Lua scripts for counter operations; 6+ nodes; 200 MB–8 GB per node depending on algorithm

### Raft WAL + Snapshots (on-disk)
- 3 of 7 (lock_service, leader_election, service_discovery); ~25 GB per node (4 MB active state + 20 GB Raft log); periodic snapshots prevent unbounded log growth

## Common Communication Patterns

### gRPC + Protobuf
- 6 of 7 (all except consistent_hashing which uses gossip); low-latency typed RPC; streaming for Watch API; bidirectional for keep-alive

### Gossip Protocol for Metadata Distribution
- consistent_hashing_system: delta gossip distributes ring updates — 2.4 KB per node add vs. full ring broadcast; convergence < 30 s for 1,000 nodes

### Kafka for Async Coordination
- distributed_transaction_coordinator: `saga.commands`, `saga.events`, `saga.dlq` topics; async step execution; at-least-once + idempotency keys = effectively-once

## Common Scalability Techniques

### Local Cache + Batch Sync to Backend
- distributed_rate_limiter: sidecar maintains local token bucket; batch sync to Redis every 100 ms; reduces Redis ops by 10–100×
- distributed_configuration_management: client-side config cache; watch for invalidation; cache miss hits etcd
- service_discovery_system: CoreDNS caches A/SRV records; < 1 ms cached lookup vs. < 5 ms uncached

### Consistent Hash-Based Work Distribution
- service_discovery_system: health checker assignment via consistent hashing — each node checks a deterministic subset of instances; no duplicate checks
- distributed_rate_limiter: per-key Redis shard via consistent hash; avoids hot shards on single key

### Multi-Tier Architecture (Gateway → Sidecar → Centralized)
- distributed_rate_limiter: API Gateway (coarse per-path) → Sidecar (per-request, local) → Redis (distributed enforcement); fail-open at each tier

## Common Deep Dive Questions

### How do fencing tokens prevent the "paused client" problem in distributed locks?
Answer: Client A holds a lock, pauses (GC, network partition), lock TTL expires, Client B acquires the lock (gets token N+1). Client A resumes, still thinks it holds the lock, attempts to write with token N. The protected storage layer checks: `if token < last_seen_token: reject`. Token N < N+1 → write rejected. This requires the storage layer to participate in the fencing protocol, not just the lock service. Without fencing tokens, Client A's paused write would corrupt data held by Client B.
Present in: distributed_lock_service, leader_election_service

### Why use etcd leases instead of just a TTL on a Redis key?
Answer: etcd Lease provides two guarantees Redis TTL cannot: (1) Lease is tied to all keys created under it — when the lease expires, ALL associated keys are atomically deleted in one Raft proposal (no partial cleanup); (2) etcd Watch API is revision-based — watchers subscribe from `last_seen_revision + 1`, so a key deletion during a disconnect is never missed. Redis keyspace notifications can be missed under load, and Redis SETNX + TTL is two separate operations (non-atomic under failures).
Present in: distributed_lock_service, leader_election_service, service_discovery_system

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.99% for all 7 systems (infrastructure primitives must be more available than services using them) |
| **Latency P50** | < 1 ms cached (config, service discovery), < 5 ms (lock acquisition, leader discovery) |
| **Latency P99** | < 50 ms (Raft consensus path), < 100 ms (cross-AZ) |
| **Throughput** | 50 K lock ops/s, 500 K service lookups/s, 1 M rate-limit checks/s |
| **Consistency** | Linearizable for coordination operations (Raft); eventual/AP for data plane reads |
| **Failover** | < 10 s leader re-election; < 30 s ring convergence; < 5 s service health propagation |
| **Scalability** | 500 K locks, 1 M config keys, 100 K service instances, 1,000 election groups |
