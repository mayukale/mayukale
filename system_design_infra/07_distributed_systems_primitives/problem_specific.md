# Problem-Specific Design — Distributed Systems Primitives (07_distributed_systems_primitives)

## Consistent Hashing System

### Unique Functional Requirements
- Map keys to nodes in O(log N); minimal disruption (only K/N keys remapped) on node add/remove
- 150 virtual nodes per physical node by default; weighted nodes for heterogeneous hardware
- Load balance std dev < 10% of mean per node; ring convergence < 30 s
- Ring metadata < 100 KB for 1,000 nodes with 150 vnodes

### Unique Components / Services
- **Hash Ring**: sorted array of (hash_position: UINT64, node_id) pairs; binary search returns next vnode ≥ hash(key); wrap-around: if hash > ring[-1][0] → return index 0
- **Virtual Node Manager**: `vnodes = default_vnodes × (weight / 100)` — weight=200 → 2× vnodes; higher-capacity nodes get proportionally more ring positions
- **Migration Coordinator**: on node add/remove, migrates only keys in the affected arc (1/N of total keys); background streaming minimizes impact
- **Gossip Protocol**: delta gossip distributes ring updates; 2.4 KB per node add vs full ring broadcast; convergence < 30 s for 1,000 nodes
- **Zone-Aware Replication**: walk ring clockwise collecting N distinct physical nodes; skip duplicate physical nodes; prefer different zones for replicas

### Unique Data Model
- **PhysicalNode**: node_id STRING PK, address IP:port, weight INT DEFAULT 100, zone STRING, vnode_count INT, joined_at TIMESTAMP
- **VirtualNode**: hash_position UINT64 PK, physical_node STRING FK, vnode_index INT, replica_group INT
- **HashRing**: ring_version UINT64, ring_positions SORTED_ARRAY<(UINT64,STRING)>, node_count INT, total_vnodes INT; total size: ~5 MB (2.4 MB positions + 2.4 MB array + 256 KB metadata)

### Algorithms

**Binary Search Lookup:**
```python
lo, hi = 0, len(ring) - 1
if hash > ring[-1][0]: return 0  # wrap around
while lo < hi:
    mid = (lo + hi) // 2
    if ring[mid][0] < hash: lo = mid + 1
    else: hi = mid
return lo  # O(log N)
```

### Key Differentiator
Consistent Hashing's uniqueness is its **weighted virtual nodes + zone-aware replication + delta gossip**: `vnodes = default_vnodes × (weight/100)` proportionally distributes load for heterogeneous hardware without manual resharding; 150 vnodes per node reduces load imbalance to < 10% std dev (vs. ~50% with 1 vnode); zone-aware replica placement walks the ring collecting from different zones — ensures replicas survive AZ failure; delta gossip (2.4 KB per event) propagates changes in < 30 s without full ring broadcast.

---

## Distributed Configuration Management

### Unique Functional Requirements
- Hierarchical KV with prefix namespacing: `/config/prod/nova-api/db_pool_size`
- Every change creates new version; rollback to any of 100 historical versions per key
- vault:// secret references: AES-256-GCM encrypted at rest; zero-downtime rotation with 300 s dual-read grace period
- Atomic multi-key transactions; config templating with variable resolution at read time
- 1 M config keys; 10 K concurrent watchers; watch notification < 100 ms from write

### Unique Components / Services
- **etcd**: primary KV store; Raft-replicated; Watch API with revision-based guarantees; `watch_from(start_revision=last_seen + 1)` — no missed events even if disconnected
- **Vault Integration**: secret storage with AES-256-GCM; dynamic secret generation; TTL-based expiry; lease renewal
- **Version Manager**: every PUT creates new version; stores diff for rollback; 100 versions/key = 5 GB total version history
- **Template Engine**: resolves `{{.env}}` variables at read time; environment inheritance (base → env → service overlays)
- **Zero-Downtime Secret Rotation**: (1) generate new secret version N+1; (2) keep old version N valid for 300 s grace period; (3) notify all watchers; (4) invalidate old after grace period; no service restart needed
- **Schema Validator**: JSON Schema validation before write commit; rejects on schema violation

### Unique Data Model
- **Config (etcd)**: key (hierarchical path), value (1 KB avg), version (Raft revision)
- **Secret (Vault)**: path, ttl INT, encrypted_data (AES-256-GCM); dynamic: username/password generated per request
- **AuditLog (MySQL)**: timestamp, actor, operation ENUM(PUT/DELETE/ROTATE), old_value, new_value, diff, reason; 256 B/event; 1-year retention
- **VersionHistory**: version INT, key_path, value, changed_by, changed_at, reason; 100 versions/key

### Algorithms

**Watch with Revision Guarantee:**
```python
watch_from(start_revision=last_seen_revision + 1)
# etcd caches all revisions; reconnect resumes from last seen
# guarantees zero missed events
```

### Key Differentiator
Config Management's uniqueness is its **etcd Watch revision-based no-miss guarantee + Vault AES-256-GCM secret rotation**: `watch_from(last_seen + 1)` guarantees no config change is missed even across disconnections (etcd caches revisions); secret rotation 300 s dual-read grace period enables zero-downtime rotation — both old and new secrets valid simultaneously; config templating resolves `{{.env}}` variables at read time enabling environment-specific config without storing duplicate values; atomic multi-key transactions prevent half-applied config states (e.g., DB host + port updated together).

---

## Distributed Lock Service

### Unique Functional Requirements
- Exactly one EXCLUSIVE holder at any time; SHARED (read) locks allow multiple concurrent holders
- Fencing tokens: monotonically increasing per lock key; protected resources reject stale tokens
- FIFO + writer-preference: once EXCLUSIVE waiter queued, no new SHARED waiters admitted
- 50,000 lock ops/sec; acquisition P99 < 50 ms; 500 K+ named locks
- TTL = 15 s; keep-alive every 5 s (TTL/3); session death releases all held locks

### Unique Components / Services
- **Raft State Machine**: every lock/unlock operation is a Raft proposal; linearizable by design; deterministic FSM applied identically on all replicas
- **Fencing Token Generator**: `global_fence_counter` per lock key; `fence_token = global_fence += 1` on each grant; CAS-updated atomically in Raft state machine
- **Session Tracker**: maps session_id → held locks; heartbeat every TTL/3 (5 s); missed 3 heartbeats → session expired → all locks released atomically
- **FIFO Wait Queue**: `wait_queue: LIST<WaitEntry>` per lock key; EXCLUSIVE waiters block subsequent SHARED grants; prevents writer starvation
- **Client SDK**: wraps gRPC; manages sessions; provides Lock / TryLock (with timeout) / Unlock / Watch APIs

### Unique Data Model
- **Lock**: key VARCHAR(512) PK, lock_type ENUM(EXCLUSIVE/SHARED/REENTRANT), holders LIST<SessionRef>, fence_token BIGINT, wait_queue LIST<WaitEntry> FIFO
- **Session**: session_id UUID PK, client_addr VARCHAR(64), ttl_seconds INT DEFAULT 15, last_renewed TIMESTAMP, held_locks SET<LockKey>, metadata MAP
- **WaitEntry**: session_id, lock_type, deadline TIMESTAMP (for try-lock timeout), priority
- Storage: ~25 GB per node (4 MB lock state + 20 GB Raft log); WAL + periodic snapshots

### Algorithms

**FIFO Lock Grant:**
```python
if lock_type == EXCLUSIVE:
    grant only if holders is empty and wait_queue empty
elif lock_type == SHARED:
    grant all consecutive SHARED waiters at head of queue
    stop when EXCLUSIVE waiter encountered (writer-preference)
    # prevents reader monopoly starving writers
fence_token = global_fence_counter[key] += 1  # atomic CAS in Raft
```

**Keep-Alive:**
```
keep_alive_interval = TTL / 3  # e.g., 15s TTL → heartbeat every 5s
Tolerates 1 full missed heartbeat (2 consecutive misses = expired)
```

### Key Differentiator
Distributed Lock's uniqueness is its **Raft state machine fencing tokens + FIFO writer-preference**: Raft proposals make every lock op linearizable — no two clients can both believe they hold an EXCLUSIVE lock simultaneously; `fence_token = global_fence++` (Raft CAS) solves the paused-client problem when the storage layer enforces `reject if token < last_seen`; FIFO wait queue with writer-preference prevents EXCLUSIVE waiters from starving while SHARED holders accumulate; `keep_alive = TTL/3` tolerates one missed heartbeat before expiry (balances safety vs. liveness).

---

## Distributed Rate Limiter

### Unique Functional Requirements
- Sidecar + Redis hybrid: sidecar absorbs local bursts, batch-syncs to Redis every 100 ms (10–100× fewer Redis ops)
- 4 algorithms: token bucket, sliding window counter, fixed window, leaky bucket; per-policy selection
- Multi-tier: per-sec + per-min + per-hour + per-day limits enforced simultaneously
- 1 M rate-limit checks/sec; P50 < 1 ms (sidecar), P99 < 5 ms (Redis); accuracy ±1% over 1-min window

### Unique Components / Services
- **Sidecar (L1)**: in-process token bucket; absorbs bursts without Redis round-trip; periodic Redis sync; fail-open on Redis unavailability
- **Algorithm Engine**: token bucket (default) + sliding window (fixes boundary problem) + fixed window + leaky bucket; selected per `RateLimitPolicy.algorithm`
- **Redis Lua Script (Atomic)**: `EVALSHA` for atomic token bucket update; prevents race conditions between increment and check
- **Policy Engine**: loads `RateLimitPolicy` from config service; hot reload via watch; evaluates which policy applies per request (match_criteria JSON)

### Unique Data Model
- **RateLimitPolicy**: policy_id VARCHAR(128) PK, match_criteria JSON, algorithm ENUM(TOKEN_BUCKET/SLIDING_WINDOW/FIXED_WINDOW/LEAKY_BUCKET), limits LIST<LimitRule>, burst_size INT, fail_open BOOLEAN, priority INT
- **LimitRule**: window_size DURATION (1s/1m/1h/1d), max_requests BIGINT, cost_per_req INT DEFAULT 1
- **Redis (token bucket)**: `{key}:tokens FLOAT`, `{key}:last_refill TIMESTAMP`; 1 KB per key; 24 MB for 1 M keys
- **Redis (sliding window)**: `{key}:current_count`, `{key}:previous_count`; 200 MB–8 GB per Redis node

### Algorithms

**Token Bucket (Redis Lua):**
```lua
refill_rate = max_tokens / ttl  -- tokens per millisecond
elapsed_ms = now - last_refill
tokens = min(max_tokens, tokens + elapsed_ms * refill_rate)
if tokens >= requested:
    tokens -= requested; allowed = true
else:
    retry_after_ms = ceil((requested - tokens) / refill_rate * 1000)
```

**Sliding Window Counter (Weighted Average):**
```
weighted = current_count + (previous_count × previous_weight)
previous_weight = 1.0 - (elapsed_in_current / window_ms)
if weighted + cost > max_requests: DENY
# Fixes fixed-window boundary problem (2× burst at minute boundary)
```

### Key Differentiator
Rate Limiter's uniqueness is its **sidecar L1 + Redis L2 hybrid + sliding window counter**: sidecar absorbs local bursts (sub-millisecond, no network) and batch-syncs to Redis every 100 ms — 10–100× fewer Redis operations at 1 M checks/sec; sliding window `weighted = current + (previous × (1 - elapsed/window))` eliminates the 2× burst problem at fixed-window boundaries; Lua EVALSHA atomic update prevents race condition between token read and decrement under concurrent requests.

---

## Distributed Transaction Coordinator

### Unique Functional Requirements
- Saga orchestration (central coordinator) AND choreography (event-driven, no central coordinator)
- Compensating transactions in reverse order on failure; idempotency keys prevent duplicate execution
- 10,000 transactions/sec; < 5 s for 3-step saga happy path; 5% compensation rate
- Transaction log survives failures; 345.6 M log entries/day (30-day retention = 7.8 TB)

### Unique Components / Services
- **Saga Engine**: executes saga definition step-by-step; publishes command to Kafka → awaits response; on failure: starts COMPENSATION in reverse order
- **Compensation Manager**: executes compensating transactions in reverse step order; tracks `step.is_compensatable`; handles partial compensation failures (compensate must be idempotent too)
- **Transaction Log (MySQL)**: persistent SagaInstance + SagaStep records; enables recovery after coordinator crash (resume from last COMPLETED step)
- **2PC Engine**: for synchronous tightly-coupled operations (prepare → commit/abort); used rarely (< 1% of transactions)
- **Idempotency Key**: `idempotency_key = f"{saga_id}-{step_index}"`; participant checks DB before executing — returns existing result if already done

### Unique Data Model
- **SagaInstance**: saga_id STRING PK, saga_type STRING, status ENUM(STARTED/COMPLETED/COMPENSATING/FAILED), input_data JSON, timeout_at TIMESTAMP, idempotency_key STRING
- **SagaStep**: step_id STRING PK, step_index INT, step_name, participant STRING, status ENUM(PENDING/EXECUTING/COMPLETED/FAILED/COMPENSATING/COMPENSATED), command_data JSON, response_data JSON, retry_count INT, max_retries INT, is_compensatable BOOLEAN
- **SagaDefinition**: saga_type, default_timeout_seconds, steps[]; **StepDefinition**: step_name, participant, forward_action, compensate_action, timeout_seconds, max_retries
- **Kafka topics**: `saga.commands` (step dispatch), `saga.events` (step completion), `saga.dlq` (permanent failures)

### Algorithms

**Saga Orchestration:**
```python
# Create SagaInstance in DB (status=STARTED)
for step in definition.steps:
    publish command to Kafka (participant.commands)
    await response on (saga_id, step_id)
    if success: mark COMPLETED, continue
    elif retry_count < max_retries: retry
    else: start COMPENSATION

# Compensation (reverse order)
for step in completed_steps reversed:
    publish compensate_action to Kafka
    mark COMPENSATING → COMPENSATED
```

### Key Differentiator
Transaction Coordinator's uniqueness is its **orchestration saga with Kafka async steps + reverse-order compensation + idempotency keys**: persistent transaction log (MySQL SagaInstance + SagaStep) enables coordinator crash recovery — resumes from last COMPLETED step; `idempotency_key = "{saga_id}-{step_index}"` prevents duplicate participant actions on Kafka retry; compensation in reverse order respects dependency ordering (e.g., undo payment before releasing inventory); Kafka DLQ captures permanently failed sagas for manual intervention.

---

## Leader Election Service

### Unique Functional Requirements
- Exactly one leader at all times (never two); split-brain prevention via Raft quorum
- Lease TTL default 15 s; failover < 10 s (death to new leader operational)
- Fencing tokens reject stale leader operations; voluntary step-down for rolling upgrades
- 1,000+ election groups; leader discovery < 100 ms; lease renewal < 5 ms P99

### Unique Components / Services
- **Election Client SDK**: manages campaign (blocks until elected), lease renewal (TTL/3), resign (voluntary step-down), observe (non-candidate discovery)
- **Lease Manager**: creates etcd lease with TTL; atomic create `/elections/{name}/leader` (If-Not-Exists); if key exists → watch key; retry when key deleted
- **Fencing Token Generator**: CAS on `/elections/{name}/fencing_token`; `new = current + 1`; retry on CAS failure; returned to new leader on election
- **Observer API**: non-candidates GET current leader without participating in election; used by load balancers and clients for routing

### Unique Data Model
- **etcd keys**: `/elections/{name}/leader` (lease-attached, value = candidate_id); `/elections/{name}/fencing_token` (monotonic BIGINT counter)
- **ElectionGroup**: election_name STRING PK, namespace STRING, lease_ttl INT DEFAULT 15, fencing_token BIGINT
- **Candidate**: candidate_id STRING, lease_id UUID, is_leader BOOLEAN, fencing_token BIGINT

### Algorithms

**Campaign (Become Leader):**
```python
lease = create_lease(ttl=15s)
loop:
    if etcd.create_if_not_exists("/elections/{name}/leader", candidate_id, lease):
        fencing_token = cas_increment("/elections/{name}/fencing_token")
        start_keepalive(lease, interval=ttl/3)
        return ELECTED(fencing_token)
    else:
        watch "/elections/{name}/leader" until deleted
        # retry atomically when key disappears
```

### Key Differentiator
Leader Election's uniqueness is its **etcd lease If-Not-Exists atomic create + CAS fencing token + Observer API**: atomic `create_if_not_exists` on lease-attached key guarantees exactly-one leader — if key already exists, candidate watches and retries on deletion (no polling); CAS fencing_token increment is atomic with election (prevents token reuse if two candidates race); Observer API decouples leadership discovery from election participation — routers learn current leader without holding a lease; voluntary `resign()` transfers leadership immediately for zero-downtime rolling upgrades.

---

## Service Discovery System

### Unique Functional Requirements
- Hybrid health checking: server-side active (HTTP/TCP/gRPC/script) + client TTL heartbeat
- Health check propagation: < 5 s from failure to deregistration; 100 K instances, 10 K services
- Self-preservation mode: if renewal rate < 85% of expected for 15 min → stop expiring instances (stale > nothing)
- DNS interface: A/AAAA/SRV records for legacy compatibility; gRPC + REST for rich queries
- 500 K lookups/sec; lookup < 1 ms (cached), < 5 ms (uncached)

### Unique Components / Services
- **Consistent Hash-Based Health Check Assignment**: `ring.get_node(instance.instance_id)` — each service discovery node checks a deterministic subset; no duplicate checks, no missed instances; rebalances automatically on node join/leave
- **Self-Preservation Mode**: if `renewal_rate < 85%` of expected for 15 min → enter preservation mode → do NOT expire instances; assumes network partition, not mass failure; exits when renewals recover
- **Threshold-Based Health State**: `consecutive_failures >= failure_threshold (3)` → CRITICAL; `consecutive_passes >= success_threshold (2)` → PASSING; prevents flapping from single transient failure
- **DNS Server**: serves A/AAAA/SRV records; short TTL for fast update propagation; compatible with legacy DNS clients; caches for < 1 ms resolution

### Unique Data Model
- **ServiceInstance**: instance_id STRING PK, address IP, port INT, service_name STRING FK, tags LIST<STRING>, metadata MAP<STRING,STRING>, health_status ENUM(PASSING/WARNING/CRITICAL), weight INT DEFAULT 100, zone STRING, canary BOOLEAN, last_check_time TIMESTAMP, last_heartbeat TIMESTAMP
- **HealthCheckConfig**: type ENUM(HTTP/TCP/GRPC/SCRIPT/TTL), interval DURATION, timeout DURATION, endpoint STRING, failure_threshold INT DEFAULT 3, success_threshold INT DEFAULT 2
- **Service**: service_name STRING PK, namespace STRING, instances LIST<ServiceInstance>

### Algorithms

**Self-Preservation:**
```
if renewal_rate < 0.85 × expected_renewals for 15 min:
    enter SELF_PRESERVATION
    do NOT expire instances (return stale > return nothing)
    # assume network partition, not mass failure
```

**Distributed Health Check Assignment:**
```python
ring = ConsistentHashRing(cluster_nodes)
for instance in all_instances:
    responsible_node = ring.get_node(instance.instance_id)
    if responsible_node == my_node_id:
        perform_health_check(instance)  # no duplicates, full coverage
```

### Key Differentiator
Service Discovery's uniqueness is its **self-preservation mode + consistent hash health check assignment + threshold flap prevention**: self-preservation (renewal rate < 85% for 15 min → stop expiring) prevents mass deregistration during network partition — the right default for AP systems; consistent hash assignment distributes health checks without coordination — each node independently determines its responsibility set; threshold logic (3 consecutive failures) + success_threshold (2 consecutive passes) prevents transient failures from causing traffic floods during recovery.
