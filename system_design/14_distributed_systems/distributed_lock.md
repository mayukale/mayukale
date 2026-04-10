# System Design: Distributed Lock

---

## 1. Requirement Clarifications

### Functional Requirements
- Allow a client to acquire an exclusive lock on a named resource
- Lock acquisition must be atomic (test-and-set): only one client holds the lock at any time
- Locks must expire automatically after a configurable TTL (lease duration) to prevent indefinite blocking if a lock holder crashes
- Lock release must be safe: only the lock holder can release its own lock (not another client)
- Support for lock renewal: a running process can extend its lease before it expires
- Reentrant locking: same client can re-acquire a lock it already holds (optional, configurable)
- Fencing tokens: lock service issues a monotonically increasing token on each acquisition; clients pass this token to resources they protect to detect stale operations
- Support wait-with-timeout: a client can wait up to N milliseconds for a lock, then give up
- Advisory vs mandatory locking: locks are advisory — clients that don't use the lock can still access the resource (enforcement is at the application layer)

### Non-Functional Requirements
- Safety (mutual exclusion): at most one client holds a lock at any given time under normal conditions
- Liveness (progress): a client can always eventually acquire a lock (no deadlock) unless a lock holder is alive and not releasing
- Availability: lock service stays available during minority node failures
- Latency: lock acquisition P99 < 10 ms in the same datacenter
- Durability: a granted lock must survive a single node failure of the lock service
- Scalability: support 10,000 distinct lock names, 1,000 concurrent lock holders
- Audit: every acquire/release/expiry event is logged with client ID, timestamp, fencing token

### Out of Scope
- Distributed transactions (2PC) using locks
- Read/write locks (shared/exclusive) — only exclusive locks in base design
- Hierarchical lock namespaces (lock on `/a` implies lock on `/a/b`)
- Lock-based queue management (use a message queue instead)
- Cross-datacenter distributed locks (requires careful analysis; covered in deep dive)

---

## 2. Users & Scale

### User Types
| Actor | Behavior |
|---|---|
| Microservice instance | Acquires lock before executing a critical section (e.g., leader election, cron job deduplication) |
| Job scheduler | Acquires locks to ensure only one instance of a cron job runs at a time |
| Database migration runner | Acquires lock to prevent concurrent schema migrations |
| Resource allocator | Acquires lock on a resource ID before modifying shared state |
| Monitoring / ops | Reads lock state for debugging; does not modify |

### Traffic Estimates (calculations shown)

**Assumptions:**
- 500 distinct microservices × 20 instances each = 10,000 service instances
- Each instance acquires a lock on average 10 times/min (conservative: includes leader election heartbeats, job deduplication, resource locking)
- Lock hold duration: average 500 ms

Total lock operations/sec = 10,000 instances × 10 / 60 ≈ **1,667 acquire+release ops/sec**  
Peak (5× burst): **8,335 ops/sec** — trivially handled by any in-memory system  
Concurrent lock holders = 10,000 instances × (500ms hold / 60,000ms/min) ≈ **83 concurrent holders** at steady state  
Lock contention events (multiple clients waiting): estimated 5% of acquisitions = **83 contention events/sec** at peak

### Latency Requirements
| Operation | P50 | P99 | P999 |
|---|---|---|---|
| Lock acquire (no contention) | 1 ms | 5 ms | 15 ms |
| Lock acquire (with wait queue) | 5 ms | 50 ms | 500 ms |
| Lock release | 0.5 ms | 3 ms | 10 ms |
| Lease renewal | 0.5 ms | 3 ms | 10 ms |
| Fencing token query | 0.5 ms | 2 ms | 5 ms |

### Storage Estimates

Each lock entry:
- Key: 256 bytes (resource name)
- Value: client ID (128 bytes) + fencing token (8 bytes) + expiry timestamp (8 bytes) + metadata (256 bytes) = ~400 bytes
- Plus ZooKeeper/etcd node overhead: ~500 bytes

10,000 distinct locks × 900 bytes ≈ **9 MB total state** — entirely negligible; the lock service is I/O and latency bound, not storage bound.

| Component | Size | Notes |
|---|---|---|
| Active lock entries | 9 MB | 10,000 locks × 900 B |
| Audit log (7-day retention) | 1,667 ops/sec × 86,400 × 7 × 200 B ≈ 201 GB | Written to append-only log store |
| fencing_tokens table | < 1 MB | Monotonic counter per lock name |

### Bandwidth Estimates

1,667 ops/sec × ~1 KB per operation (request + response) = **1.67 MB/sec** — negligible network load. Even at 10× peak: 16.7 MB/sec per node, well within 1 Gbps NIC.

---

## 3. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                   Client Services (Callers)                  │
│  Service A (3 instances)   Service B (5 instances)  ...     │
│                                                              │
│  Lock Client Library:                                        │
│  - Handles acquire/release/renew via gRPC                   │
│  - Implements retry with exponential backoff                 │
│  - Passes fencing token to downstream resource calls        │
│  - Monitors lease expiry; auto-renews at 2/3 of TTL         │
└─────────────────────┬────────────────────────────────────────┘
                      │ gRPC / HTTP2
                      ▼
┌─────────────────────────────────────────────────────────────┐
│               Lock Service API Layer                         │
│  (Stateless application servers, horizontally scalable)      │
│                                                              │
│  Endpoints: Acquire / Release / Renew / Query / Wait        │
│  - Validates JWT / mTLS client identity                     │
│  - Rate limits per client                                    │
│  - Translates to backend lock store operations               │
│  - Assigns and returns fencing tokens                        │
└─────────────────────┬────────────────────────────────────────┘
                      │
          ┌───────────┴─────────────┐
          │                         │
          ▼                         ▼
┌─────────────────┐       ┌─────────────────────────────────┐
│  Backend Option │       │  Backend Option B:               │
│  A: etcd        │  OR   │  ZooKeeper                       │
│  (Raft-based)   │       │  (ZAB-based)                    │
│                 │       │                                   │
│  3 or 5 nodes   │       │  3 or 5 nodes                    │
│  - Leases (TTL) │       │  - Ephemeral znodes              │
│  - Watch API    │       │  - Watchers                      │
│  - Raft         │       │  - Sessions                      │
│    consensus    │       │  - ZAB protocol                  │
└─────────────────┘       └─────────────────────────────────┘
          │
          ▼
┌─────────────────────────────────────────────────────────────┐
│               Fencing Token Service                          │
│  - Monotonically increasing counter per lock name           │
│  - Stored in Raft-backed KV (same etcd cluster)             │
│  - Returned on every successful lock acquisition            │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                 Protected Resource (example)                 │
│  Database / Storage Service / External API                  │
│                                                              │
│  Resource Guard:                                             │
│  - Accepts operations only with fencing token >= last_seen  │
│  - Rejects stale operations from zombie lock holders        │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  Audit Log (append-only, Kafka topic or Kinesis stream)     │
│  Every acquire/release/expire/renew event with:            │
│  client_id, lock_name, fencing_token, timestamp, action    │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│  Monitoring (Prometheus + Grafana)                           │
│  - Lock acquisition latency P50/P99                         │
│  - Lock contention rate (queue depth per lock)              │
│  - Expired lock count (crash indicator)                     │
│  - Fencing token gaps (potential zombie activity)           │
└─────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **Lock Client Library:** Thin wrapper around the gRPC API. Automatically renews leases before expiry (at 2/3 of TTL), handles MOVED/retry errors, and passes fencing tokens to protected resources.
- **Lock Service API Layer:** Stateless gRPC servers. Translate high-level lock semantics to atomic KV operations on the backend. Enforce access control and rate limits.
- **etcd / ZooKeeper backend:** Provides Raft/ZAB consensus for atomic compare-and-set operations. Replicates lock state to a quorum of nodes. TTL/lease expiry is managed natively.
- **Fencing Token Service:** Issues strictly monotonic integers per lock name. Embedded in etcd (atomic `COMPARE-AND-SWAP` on a counter key). Prevents stale lock holders from acting on expired locks.
- **Protected Resource Guard:** Validates that the fencing token in an incoming request is >= the last token it has seen. Rejects stale requests. This is the final safety boundary.

**Primary Use-Case Data Flow (Lock Acquisition):**
1. Service A calls `AcquireLock(resource="job:export:weekly", ttl=30s, wait_timeout=5s)`
2. Lock Service computes lock key, checks current holder in etcd
3. If no holder: atomic `PUT /locks/job:export:weekly {holder: A, token: 42, expires: now+30s} IF NOT EXISTS`
4. etcd commits via Raft to quorum (3/5 nodes), returns success
5. Lock Service atomically increments fencing token counter (returns 42)
6. Returns `LockResponse{token: 42, granted: true, expiry: T+30s}` to client
7. Service A proceeds with protected operation, passing token=42 to the database
8. Database guard validates: 42 >= last_seen_token (41) → allows operation
9. Service A calls `ReleaseLock(resource="job:export:weekly", token=42)`
10. Lock Service verifies token matches current holder → deletes key from etcd
11. etcd commit propagates; waiting clients receive watch notification and retry

---

## 4. Data Model

### Entities & Schema

```
-- etcd key-value schema --

Lock Entry:
  Key:   /locks/{lock_name}
         e.g., /locks/job:cron:daily_report
  Value (JSON or Protobuf):
    {
      "holder_id":      "service-a-instance-7f3b",  // client identity
      "fencing_token":  42,                          // monotonic token issued at acquire
      "acquired_at":    1704067200000,               // Unix ms
      "expires_at":     1704067230000,               // Unix ms (acquired_at + TTL)
      "ttl_ms":         30000,
      "lock_type":      "exclusive",
      "metadata":       {"job_id": "daily_report_2024_01_01"}
    }
  etcd lease: expires_at is managed via etcd lease TTL
  Revision: etcd's internal revision number serves as the fencing token (see Deep Dive)

Fencing Token Counter (if not using etcd revision):
  Key:   /fencing_tokens/{lock_name}
  Value: integer (atomic increment on each acquisition)

Wait Queue Entry (for wait-with-timeout):
  Key:   /lock_waiters/{lock_name}/{waiter_priority}/{client_id}
         (sorted by priority then arrival time)
  Value:
    {
      "client_id":    "service-b-instance-4a2c",
      "enqueued_at":  1704067201000,
      "wait_timeout_at": 1704067206000   // give up at this time
    }
  etcd TTL: wait_timeout_at - now  (auto-removes expired waiters)

Audit Log Record (written to Kafka):
  {
    "event_id":      "uuid-v4",
    "timestamp_ms":  1704067200000,
    "event_type":    "ACQUIRED" | "RELEASED" | "EXPIRED" | "RENEWED" | "REJECTED",
    "lock_name":     "job:cron:daily_report",
    "client_id":     "service-a-instance-7f3b",
    "fencing_token": 42,
    "wait_duration_ms": 0,
    "hold_duration_ms": null  // filled on RELEASED/EXPIRED
  }
```

### Database Choice

| Option | Pros | Cons | Fit |
|---|---|---|---|
| **etcd** | Raft consensus, native leases/TTL, Watch API, linearizable reads, embedded in Kubernetes | Smaller ecosystem than ZooKeeper, watch storm risk at scale | **Selected** |
| ZooKeeper | Battle-tested, ephemeral znodes, sequential znodes for lock queues, ZAB consensus | No native TTL per node (use session expiry), JVM overhead, more complex ops | Strong alternative |
| Redis (single node) | Sub-ms latency, simple SET NX EX | Not linearizable (Lua is atomic but Redis is AP); single-node SPOF | Not suitable for critical locks |
| Redis Redlock | Multi-node, quorum-based | Clock drift unsafe (Kleppmann critique), no fencing tokens | Not suitable for storage-protecting locks |
| MySQL/Postgres advisory locks | Familiar, ACID | Not distributed; lock tied to DB connection lifetime | Single-region only; not scalable |
| Chubby (Google) | Designed for distributed locks, cell-based | Proprietary; not available | Inspiration only |

**Selected: etcd**  
Justification: etcd provides linearizable (not eventually consistent) reads and writes via Raft consensus, meaning a successful `PUT IF NOT EXISTS` (implemented as a Compare-And-Swap transaction) is guaranteed to be seen by all future reads across all nodes. The native lease API (with TTL) handles lock expiry at the etcd level — the lock key is automatically deleted when the lease expires, even if the API layer crashes. The Watch API allows waiters to be notified when a lock is released without polling. etcd's revision counter provides a built-in, monotonically increasing fencing token that is completely free.

---

## 5. API Design

**Protocol:** gRPC (Protocol Buffers) for client libraries; REST/JSON for admin and debugging.  
**Authentication:** mTLS client certificates per service, with service identity embedded in CN field. JWT Bearer tokens for REST endpoints.  
**Rate Limiting:** 100 acquire requests/sec per client_id; 1,000/sec global across all clients per lock service instance.

```protobuf
// lock_service.proto

service LockService {
  rpc AcquireLock(AcquireLockRequest) returns (AcquireLockResponse);
  rpc ReleaseLock(ReleaseLockRequest) returns (ReleaseLockResponse);
  rpc RenewLock(RenewLockRequest) returns (RenewLockResponse);
  rpc GetLockState(GetLockStateRequest) returns (GetLockStateResponse);
  rpc WatchLock(WatchLockRequest) returns (stream LockEvent);
}

message AcquireLockRequest {
  string lock_name        = 1;  // e.g., "job:export:weekly"
  string client_id        = 2;  // stable service instance ID
  int64  ttl_ms           = 3;  // lease duration, max 60000
  int64  wait_timeout_ms  = 4;  // 0 = non-blocking; -1 = wait forever
  bool   reentrant        = 5;  // allow same client to re-acquire
  map<string,string> metadata = 6;
}

message AcquireLockResponse {
  bool    granted        = 1;
  int64   fencing_token  = 2;  // monotonically increasing; use this for resource access
  int64   expires_at_ms  = 3;  // Unix ms when lock expires
  string  error_message  = 4;  // if not granted
  enum RejectReason {
    NONE = 0;
    TIMEOUT = 1;       // wait_timeout_ms exceeded
    RATE_LIMITED = 2;
    INVALID_REQUEST = 3;
  }
  RejectReason reject_reason = 5;
}

message ReleaseLockRequest {
  string lock_name       = 1;
  string client_id       = 2;
  int64  fencing_token   = 3;  // must match the token from AcquireLockResponse
}

message ReleaseLockResponse {
  bool success      = 1;
  string error      = 2;  // "NOT_LOCK_HOLDER" | "LOCK_EXPIRED" | "INVALID_TOKEN"
}

message RenewLockRequest {
  string lock_name      = 1;
  string client_id      = 2;
  int64  fencing_token  = 3;
  int64  new_ttl_ms     = 4;  // must be <= max_ttl; can't reduce below remaining TTL
}

message RenewLockResponse {
  bool  success       = 1;
  int64 new_expires_at_ms = 2;
  string error        = 3;  // "LOCK_EXPIRED" if lease already expired
}

message GetLockStateRequest {
  string lock_name = 1;
}

message GetLockStateResponse {
  bool    is_locked      = 1;
  string  holder_id      = 2;  // empty if not locked
  int64   fencing_token  = 3;
  int64   expires_at_ms  = 4;
  int64   acquired_at_ms = 5;
}

message WatchLockRequest {
  string lock_name = 1;
}

message LockEvent {
  enum EventType {
    ACQUIRED = 0;
    RELEASED = 1;
    EXPIRED  = 2;
    RENEWED  = 3;
  }
  EventType event_type   = 1;
  string    holder_id    = 2;
  int64     fencing_token = 3;
  int64     timestamp_ms  = 4;
}
```

```
-- REST API (admin / debug) --

GET /v1/locks/{lock_name}
  Response: 200 {"is_locked": true, "holder_id": "svc-a-7f3b", "fencing_token": 42, "expires_at_ms": 1704067230000}
            404 {"is_locked": false}
  Auth: Bearer admin-token

GET /v1/locks?pattern=job:cron:*
  Response: 200 {"locks": [...]}

DELETE /v1/locks/{lock_name}  (admin force-release)
  Body: {"admin_reason": "manual intervention"}
  Response: 204 No Content
  Auth: admin role only; creates audit log entry with admin_flag=true

GET /v1/metrics/contention
  Response: {"high_contention_locks": [{"lock_name": "...", "queue_depth": 15, "p99_wait_ms": 450}]}
```

---

## 6. Deep Dive: Core Components

### 6.1 Fencing Tokens: Making Locks Safe

**Problem it solves:** A lock holder can "die" and come back as a zombie — a process that was paused (GC pause, OS scheduling) for longer than the lock TTL, unaware that its lease expired and another client acquired the lock. Without fencing tokens, the zombie can still access the protected resource with a stale belief that it holds the lock, causing data corruption.

**Illustration of the problem:**
```
t=0:  Client A acquires lock, gets token=42, starts operation on DB
t=5:  Client A pauses for 35 seconds (JVM GC stop-the-world)
t=5:  Lock TTL (30s) expires; Client B acquires lock, gets token=43
t=10: Client B writes to DB with token=43
t=40: Client A resumes, still thinks it holds the lock
t=40: Client A writes to DB with token=42 → ZOMBIE! Without fencing: data corrupted.
      With fencing: DB sees token=42 < last_seen=43, REJECTS operation.
```

**Approaches Comparison:**

| Approach | Mechanism | Safety | Requirement |
|---|---|---|---|
| **Fencing tokens (monotonic int)** | Lock service issues token; resource validates `token >= last_seen` | Safe under GC pause, network delay | Resource must implement token check |
| Version-based locking | Client sends expected version; resource rejects if version changed | Safe if resource supports CAS | Resource must support CAS |
| Lease expiry only (no token) | Lock expires; client assumes it still holds it | Unsafe (zombie problem) | None |
| Heartbeat-based | Client heartbeats; resource disconnects if no heartbeat | Partially safe | Resource maintains connection per client |
| STONITH (fencing at machine level) | Kill the machine of the old lock holder | Very strong | Requires out-of-band infrastructure (IPMI, cloud API) |

**Selected: Fencing tokens using etcd's global revision number**

Justification: etcd maintains a cluster-wide, monotonically increasing revision number (`cluster_revision`) that increments on every key write. On lock acquisition, the Lock Service returns the etcd revision at the moment of the successful `PUT`. Since etcd's revision is Raft-committed and globally ordered, it is a perfect fencing token — it always increases, it cannot go backward, and it is issued by the consensus system itself (not by the application), making it tamper-proof.

**Fencing Token Implementation with etcd:**
```python
class LockService:
    def acquire_lock(self, lock_name, client_id, ttl_ms):
        lease = etcd.grant_lease(ttl=ttl_ms // 1000)
        
        # Atomic compare-and-swap: put key only if it doesn't exist
        # etcd Transaction:
        txn_response = etcd.transaction(
            compare=[
                # Condition: the lock key does not exist (version == 0)
                etcd.transactions.version('/locks/' + lock_name) == 0
            ],
            success=[
                # If condition met: create lock entry with lease
                etcd.transactions.put(
                    '/locks/' + lock_name,
                    json.dumps({"holder_id": client_id, "acquired_at": time_ms()}),
                    lease=lease
                )
            ],
            failure=[]
        )
        
        if txn_response.succeeded:
            # The fencing token IS the etcd cluster revision after the write
            fencing_token = txn_response.header.revision  # globally unique, monotonic
            return LockResponse(granted=True, token=fencing_token,
                                expires_at=time_ms() + ttl_ms)
        else:
            # Lock already held; optionally enqueue waiter
            lease.revoke()
            return LockResponse(granted=False)

    def release_lock(self, lock_name, client_id, fencing_token):
        # Read current lock entry
        current = etcd.get('/locks/' + lock_name)
        if current is None:
            return ReleaseLockResponse(success=False, error="LOCK_EXPIRED")
        
        holder = json.loads(current.value)
        if holder["holder_id"] != client_id:
            return ReleaseLockResponse(success=False, error="NOT_LOCK_HOLDER")
        
        # Atomic delete: only delete if the key's mod_revision matches fencing_token
        # This prevents accidental release of a re-acquired lock
        txn_response = etcd.transaction(
            compare=[
                # The key was created at fencing_token revision (our acquisition)
                etcd.transactions.mod_revision('/locks/' + lock_name) == fencing_token
            ],
            success=[
                etcd.transactions.delete('/locks/' + lock_name)
            ],
            failure=[]
        )
        
        return ReleaseLockResponse(success=txn_response.succeeded)

# Protected Resource Guard (e.g., in the database layer):
class ResourceGuard:
    def __init__(self):
        self.last_fencing_token = {}  # per resource_name

    def execute_operation(self, resource_name, fencing_token, operation):
        last = self.last_fencing_token.get(resource_name, -1)
        if fencing_token <= last:
            raise StaleOperationError(
                f"Fencing token {fencing_token} <= last seen {last}. "
                "Rejecting as possible zombie operation."
            )
        self.last_fencing_token[resource_name] = fencing_token
        return operation()
```

**Interviewer Q&As:**

Q: Why does the fencing token need to be validated at the resource, not at the lock service?  
A: The lock service only knows whether a lock is currently held — it cannot know if a zombie (with an expired token) has already sent a request to the resource. The resource is the only party that sees both the token and the actual operation. By storing the maximum token ever seen, the resource can detect and reject stale operations regardless of what the lock service says. The lock service is the token issuer; the resource is the enforcer.

Q: What if the protected resource (e.g., a database) doesn't support fencing tokens?  
A: This is the hard reality of most lock implementations. Options: (1) Wrap the resource with a guard proxy that tracks fencing tokens and intercepts requests. (2) Use optimistic locking at the application layer: read a version number before the critical section, pass it in the UPDATE WHERE version=? clause — the database itself enforces ordering. (3) Accept the race condition and use STONITH (shoot the other node in the head) — forcibly terminate the suspected zombie via its machine's power management API before granting a new lock. (4) Use very short TTLs (1–2s) so zombie windows are minimized, combined with idempotent operations.

Q: What is the maximum safe duration of a GC pause relative to the lock TTL?  
A: For safety without fencing tokens: GC pause must always be < TTL - network_RTT - processing_time. For a 30s TTL and 10ms network RTT, the maximum safe GC pause is ~29.99 seconds — theoretically fine but not guaranteed. JVM G1GC can pause for 200ms–5 seconds; ZGC targets < 1ms but isn't guaranteed. With fencing tokens, GC pauses of any duration are safe — the zombie's operation will be rejected by the resource guard. This is why fencing tokens are the correct solution and short TTLs alone are insufficient.

Q: Explain the difference between advisory locks and mandatory locks.  
A: Advisory locks require all participating processes to voluntarily check the lock before accessing the resource. A process that ignores the lock API can still access the resource freely. Most distributed lock systems (etcd, ZooKeeper, Redis-based) implement advisory locks — they rely on all clients respecting the locking protocol. Mandatory locks are enforced by the resource itself (e.g., file system-level locks where the OS refuses access to a locked file). For distributed systems, fencing-token-based resource guards are the closest equivalent to mandatory locking.

Q: How do you prevent a client from holding a lock indefinitely due to a bug (no release call)?  
A: The TTL/lease is the primary mechanism. Each lock has a mandatory expiry. The client library auto-renews the lease at 2/3 of TTL (e.g., renew at 20s for a 30s TTL), but only if the processing is still active. If the client process crashes or hangs, the lease is not renewed and expires automatically. Additionally: (1) Set a hard maximum TTL at the Lock Service (e.g., max 60 seconds; no single operation should take longer). (2) Monitor for locks held past a warning threshold (e.g., 80% of max TTL). (3) Alert on clients repeatedly renewing the same lock for > N minutes.

---

### 6.2 Redlock Analysis vs ZooKeeper-Based Locks

**Problem it solves:** Single-node Redis lock (`SET key NX EX`) has an obvious SPOF. Redlock attempts to provide distributed safety across N independent Redis instances. ZooKeeper provides a fundamentally different, consensus-based approach. This section compares them rigorously.

**Redlock Algorithm (5 nodes):**
```
function redlock_acquire(lock_name, client_id, ttl_ms):
    n = 5  # number of independent Redis instances
    quorum = n // 2 + 1  # = 3
    start_time = time_ms()
    acquired_count = 0
    acquired_nodes = []
    random_value = generate_random_token()  # prevents impersonation

    for redis_node in redis_instances:
        try:
            # SET NX PX: set if not exists, expire in ttl_ms
            success = redis_node.set(
                f"lock:{lock_name}", random_value,
                nx=True, px=ttl_ms,
                timeout=ttl_ms // (2 * n)  # don't block on slow nodes
            )
            if success:
                acquired_count += 1
                acquired_nodes.append(redis_node)
        except RedisException:
            pass  # node down; continue

    elapsed = time_ms() - start_time
    validity_time = ttl_ms - elapsed  # remaining validity

    if acquired_count >= quorum and validity_time > 0:
        return RedlockResult(success=True, token=random_value,
                             validity_time=validity_time)
    else:
        # Failed: release on all nodes where we acquired
        for node in acquired_nodes:
            lua_delete_if_value_matches(node, f"lock:{lock_name}", random_value)
        return RedlockResult(success=False)

# Safe release (Lua script — atomic check-and-delete):
RELEASE_SCRIPT = """
if redis.call("get",KEYS[1]) == ARGV[1] then
    return redis.call("del",KEYS[1])
else
    return 0
end
"""
```

**Martin Kleppmann's Critique of Redlock:**
```
Scenario 1: Clock drift attack
- Client 1 acquires Redlock; lock expires at T+30s
- Node 3's clock jumps forward 31 seconds (NTP correction or drift)
- Node 3 expires the lock key prematurely at T (incorrectly)
- Client 2 acquires: gets nodes 1,2,3 (node 3 released prematurely) = quorum
- Now both Client 1 and Client 2 believe they hold the lock

Scenario 2: GC pause (no fencing token)
- Client 1 acquires Redlock (token = random_value_A)
- Client 1 pauses for 35 seconds (JVM GC)
- All 5 Redis nodes expire the lock (TTL=30s)
- Client 2 acquires Redlock (token = random_value_B)
- Client 2 writes to DB
- Client 1 resumes, STILL BELIEVES it holds the lock
- Client 1 writes to DB → data corruption
- Redlock has no fencing token mechanism; cannot detect this

Antirez (Redis author) response:
- Clock drift > 1ms per second is unusual in practice with NTP
- GC pause > TTL is the application's bug, not Redlock's
- For most use cases (efficiency locks, not safety-critical), Redlock is sufficient

Verdict: Redlock is acceptable for advisory efficiency locks (preventing duplicate cron runs).
         Redlock is NOT safe for storage-protecting locks without additional fencing.
```

**ZooKeeper-Based Lock Algorithm (using sequential ephemeral znodes):**
```
function zookeeper_acquire(lock_name, client_id, wait_timeout_ms):
    # Create ephemeral sequential znode under lock directory
    # e.g., /locks/job_export/lock-0000000042
    my_path = zk.create(
        f"/locks/{lock_name}/lock-",
        data=client_id.encode(),
        ephemeral=True,   # deleted when ZK session expires (client crash)
        sequence=True     # ZK appends a monotonic 10-digit sequence number
    )
    # my_path = "/locks/job_export/lock-0000000042"
    my_seq = int(my_path.split("-")[-1])  # = 42

    start_time = time_ms()
    while True:
        children = sorted(zk.get_children(f"/locks/{lock_name}"))
        # children = ["lock-0000000040", "lock-0000000041", "lock-0000000042"]
        # (sorted lexicographically = sorted by sequence number)

        my_index = children.index(os.path.basename(my_path))
        
        if my_index == 0:
            # I am first → I hold the lock
            fencing_token = my_seq  # sequence number is monotonically increasing
            return LockAcquired(path=my_path, token=fencing_token)
        
        # Watch the node IMMEDIATELY BEFORE me (herd effect prevention)
        predecessor = children[my_index - 1]
        predecessor_path = f"/locks/{lock_name}/{predecessor}"
        
        event = zk.exists(predecessor_path, watch=True)
        if event is None:
            # Predecessor already deleted — retry (may now be first)
            continue
        
        # Wait for predecessor to be deleted (released or expired)
        elapsed = time_ms() - start_time
        remaining_timeout = wait_timeout_ms - elapsed
        if remaining_timeout <= 0:
            zk.delete(my_path)  # give up; clean up our znode
            return LockTimeout()
        
        event.wait(timeout=remaining_timeout / 1000)  # block until predecessor deleted

def release_zk_lock(my_path):
    zk.delete(my_path)
    # ZK session expiry (client crash) also triggers ephemeral node deletion
```

**Approaches Comparison:**

| Dimension | Redlock (5 Redis nodes) | ZooKeeper Sequential Znodes | etcd CAS |
|---|---|---|---|
| Consensus | None (quorum of SET NX) | ZAB protocol (linearizable) | Raft (linearizable) |
| Clock sensitivity | Vulnerable to clock drift | Session timeout (not wall-clock TTL) | Lease TTL uses server clock |
| Fencing token | No (random value is not monotonic) | Yes (sequence number is monotonic) | Yes (etcd revision is monotonic) |
| Herd effect | No wait queue; all clients poll | Watched predecessor — no herd | Watch on lock key — herd on release |
| Session expiry | Per-node TTL only | ZK session: all ephemeral nodes deleted atomically | Lease: all associated keys deleted |
| Safety under GC pause | Unsafe without fencing | Safe with fencing (seq number) | Safe with fencing (revision) |
| Latency | ~1 ms (in-memory) | ~5–10 ms (ZAB writes) | ~2–5 ms (Raft writes) |
| Operational complexity | 5 independent Redis instances | 3–5 node ZK ensemble, JVM | 3–5 node etcd cluster, Go binary |

**Selected for this design: etcd CAS with revision-based fencing tokens.**  
ZooKeeper is an equally valid choice with stronger sequential-node wait queue semantics.

**Interviewer Q&As:**

Q: What is the "herd effect" in distributed locking and how do ZooKeeper sequential znodes prevent it?  
A: When a popular lock is released, all waiting clients receive the release notification and simultaneously race to acquire the lock. This causes a "thundering herd" — N clients all attempt lock acquisition, all but one fail, wasting network and CPU. ZooKeeper sequential znodes solve this elegantly: each waiter watches only the znode immediately before it in the sorted sequence. When the lock holder deletes its znode, only the next-in-line is notified and wakes up. N-1 waiters remain asleep. This converts O(N) wake-ups to O(1) per release.

Q: What is the ZooKeeper session timeout and how is it different from a Redis TTL?  
A: A ZooKeeper session is a persistent connection between a ZK client and the ZK ensemble. The session has a configurable timeout. If the ensemble does not receive a heartbeat from the client within the timeout, it marks the session as expired and atomically deletes ALL ephemeral znodes owned by that session. This is safer than a Redis per-key TTL because: (1) it's atomic — all of a client's locks are released together on crash, and (2) it's session-level — the ZK client library automatically manages heartbeats. The risk: a long GC pause can cause a session to be declared expired even though the client is running; the client then loses all its locks and must re-acquire.

Q: Antirez argued that clock drift > 1ms/sec is unlikely, making Redlock safe in practice. Do you agree?  
A: The argument has merit for most operational environments with properly configured NTP (drift < 10ms/day typically). However, "unlikely" and "safe" are categorically different. In a distributed system, safety properties must hold under all conditions stated in the threat model. If clock drift is outside the threat model, Redlock is fine for advisory locks. If the threat model includes arbitrary clock drift, OS scheduling delays, or GC pauses, Redlock is not safe for protecting storage resources. The practical recommendation: use Redlock for efficiency-oriented locks (deduplicate cron jobs, rate limiting, soft mutual exclusion) where the consequence of occasional duplicate execution is tolerable. Use etcd/ZooKeeper + fencing tokens for safety-critical locks where data correctness is paramount.

Q: How would you implement a fair lock (FIFO ordering) with etcd?  
A: etcd does not natively support sequential nodes like ZooKeeper. Implement FIFO with etcd by: (1) Using a counter key (`/lock_queues/{lock_name}/counter`) with an atomic increment (`etcd transaction: GET counter, PUT counter+1, PUT /lock_queues/{lock_name}/waiter-{counter}`). (2) Watch the waiter znode with the predecessor's sequence number. (3) On lock release, only the waiter with the lowest sequence number can proceed. This replicates ZooKeeper's sequential ephemeral znodes in etcd.

Q: How do you handle lock holder crashes in ZooKeeper vs etcd vs Redis?  
A: **ZooKeeper:** The session expires after the session timeout. All ephemeral znodes for that session are deleted atomically. Other waiters are notified via watches. No data is left behind. **etcd:** The lease expires after the TTL. All keys associated with the lease are atomically deleted by the etcd server. The lock key disappears, and watchers are notified. **Redis (Redlock):** The per-key TTL expires on each individual Redis node. If the client set NX PX on all nodes simultaneously with the same TTL, they expire at approximately the same time (within clock drift). Watchers must poll (or use keyspace notifications). The difference: ZooKeeper/etcd have atomic session/lease deletion across all associated resources; Redis handles each key independently.

---

### 6.3 Lock Contention & Deadlock Prevention

**Problem it solves:** When multiple clients compete for the same lock, some must wait. Poorly designed waiting logic can cause deadlocks (circular dependency), starvation (some clients never acquire), or resource exhaustion (thousands of clients blocking on a single lock).

**Approaches Comparison:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Timeout-based acquisition** | Client gives up after wait_timeout_ms | Simple, prevents indefinite blocking | May miss lock briefly available after timeout |
| **FIFO queue (sequential znodes)** | Clients queue up in order | Fair; no starvation | More complex implementation |
| **Exponential backoff + jitter** | Client retries after random delay | Simple; natural load spreading | Unfair; high-priority tasks wait same as low |
| **Priority queue** | Higher-priority clients acquire first | Supports SLA tiers | Complex; risk of starvation for low-priority |
| **Lock hierarchy (ordering)** | All clients acquire locks in a consistent global order | Eliminates deadlock | Requires global knowledge of all lock names |

**Deadlock Prevention Algorithm (Lock Ordering):**
```python
# Deadlock scenario without ordering:
# Thread A: acquires lock(resource_1), then tries lock(resource_2)
# Thread B: acquires lock(resource_2), then tries lock(resource_1)
# → Circular dependency → deadlock

# Prevention: always acquire locks in canonical sorted order
# Thread A: acquires lock("resource_1"), lock("resource_2")  [sorted order]
# Thread B: acquires lock("resource_1"), lock("resource_2")  [same order]
# → Thread B waits for Thread A to release resource_1 → no deadlock

class DeadlockSafeLockClient:
    def acquire_multiple(self, lock_names: list[str], ttl_ms: int):
        # Sort lock names canonically to prevent deadlock
        sorted_names = sorted(lock_names)
        acquired = []
        try:
            for name in sorted_names:
                result = self.acquire(name, ttl_ms=ttl_ms, wait_timeout_ms=5000)
                if not result.granted:
                    raise LockAcquisitionError(f"Failed to acquire {name}")
                acquired.append((name, result.fencing_token))
            return acquired
        except Exception:
            # Release all acquired locks on failure
            for name, token in reversed(acquired):
                self.release(name, token)
            raise

# Lock contention monitor:
class ContentionMonitor:
    def on_lock_wait(self, lock_name, client_id, wait_start):
        # Track: how many clients are waiting per lock
        queue_depth = self.wait_queue.count(lock_name)
        if queue_depth > HIGH_CONTENTION_THRESHOLD:  # e.g., 10
            alert(f"High contention on {lock_name}: {queue_depth} waiters")
        
        # Detect potential deadlock: if a client has been waiting > 3x TTL
        # it may be in a deadlock
        wait_duration = time_ms() - wait_start
        if wait_duration > 3 * DEFAULT_TTL_MS:
            alert(f"Possible deadlock: {client_id} waiting {wait_duration}ms for {lock_name}")
```

**Interviewer Q&As:**

Q: How do you detect a deadlock in a distributed lock system?  
A: In a pure advisory lock system with TTLs, deadlocks self-resolve when TTLs expire — client A waiting for B's lock will eventually get it when B's lease expires. True indefinite deadlocks require a circular wait with no TTL expiry. Detection: maintain a wait-for graph (node = client, edge = "A is waiting for lock held by B"). A cycle in this graph means deadlock. This is expensive to maintain globally; instead, use timeout-based acquisition everywhere (wait_timeout_ms < lock TTL) so any would-be deadlock resolves within TTL. Pragmatic: set wait_timeout_ms = 2–3× the average operation duration, and have the lock client log and alert on acquisition timeouts.

Q: What is lock granularity and why does it matter for contention?  
A: Lock granularity is how "narrow" the protected resource is. Coarse-grained lock: `lock("all_orders")` — only one client can work on any order at a time. Fine-grained lock: `lock("order:12345")` — different orders can be processed in parallel. Finer granularity reduces contention (more parallelism) but increases the number of distinct lock names and management overhead. Rule: lock the smallest resource that guarantees correctness. For a row-level lock in a distributed system, use `lock(f"row:{table}:{primary_key}")`.

Q5: How would you design the lock system to survive a complete data center failure?  
A: For single-region etcd: deploy 5 nodes across 3 availability zones (2+2+1). Survive loss of any 1 AZ with quorum of 3/5. For multi-region: replicate etcd across regions with geo-distributed Raft. Read/write latency becomes cross-region (~50–100ms), which is acceptable for lock acquisition but not for millisecond-latency operations. Alternative: use region-scoped locks (each region has its own lock service; cross-region operations use a two-phase approach with higher-level coordination). For most systems, AZ-redundant single-region is sufficient and orders of magnitude simpler.

---

## 7. Scaling

### Horizontal Scaling
The Lock Service API layer (gRPC servers) is stateless — scale by adding instances behind a load balancer. All state is in etcd.

**etcd scaling:** etcd is write-latency sensitive to quorum size. 3-node cluster: survives 1 node failure; write quorum of 2/3. 5-node cluster: survives 2 node failures; write quorum of 3/5 but slightly higher write latency. 7-node: survives 3 failures but write latency increases further. For most production lock services, **5 nodes across 3 AZs** is the sweet spot.

**Lock namespace sharding:** If 10,000 distinct lock names creates hot spots (unlikely at this scale — etcd handles 10K writes/sec easily), shard by lock name prefix: `resource:orders:* → etcd cluster A`, `resource:users:* → etcd cluster B`. The Lock Service API layer routes to the appropriate cluster.

### Replication
etcd uses Raft: all writes go through the leader, followers replicate. Reads can be linearizable (via leader) or serializable (via follower, may be slightly stale). For lock operations, always use linearizable reads (`--consistency=l` in etcd) to avoid reading stale lock state from a follower that hasn't received the latest write.

### Caching
**Do NOT cache lock state.** A cached lock state can be stale — it might show a lock as "free" when it is actually held, causing incorrect acquisition. The lock service must always perform a linearizable read from etcd. Caching is appropriate only for advisory/informational reads (e.g., "show me the current state of all locks for the dashboard") with explicit staleness warnings.

### Interviewer Q&As (Scaling)

Q: How do you handle 100,000 distinct lock names across a distributed system?  
A: etcd handles 1M+ keys trivially; the concern is write throughput, not key count. 100K distinct locks with low contention (each acquired/released infrequently) is well within a 5-node etcd cluster's capacity (10K–30K writes/sec). If all 100K locks are being contended simultaneously — unusual — shard etcd clusters by lock prefix. For ultra-high-volume lock use cases (millions/sec), switch to a lock-like pattern in a single Redis primary per lock namespace (acceptable for advisory-only locks).

Q2: What is the maximum recommended size of an etcd cluster?  
A: The etcd documentation recommends no more than 7 nodes in a production cluster. Beyond 7 nodes, the Raft quorum write latency (waiting for majority confirmation) increases linearly with cluster size. For scaling beyond 7 nodes, either shard into multiple etcd clusters or use a tiered architecture (etcd for cluster metadata, application-level sharding above it). The majority threshold at 7 nodes is 4/7 — surviving 3 simultaneous failures while still accepting writes.

Q3: How does etcd's MVCC (Multi-Version Concurrency Control) model affect lock correctness?  
A: etcd keeps multiple versions of each key (by revision number). A lock acquisition transaction's `IF NOT EXISTS` condition is evaluated at a specific revision. If two clients simultaneously attempt acquisition, both see the same revision where the key doesn't exist, but only one can win the transaction — etcd's Raft serializes all writes at the leader, so only the first-to-arrive transaction succeeds. The losing transaction's compare condition fails (the key now exists with a higher revision), and it correctly reports failure. MVCC enables atomic transactions without blocking readers.

Q4: How would you implement a distributed read-write lock?  
A: Use two lock keys per resource: `{resource}:write` and `{resource}:read:{client_id}`. Write lock: exclusive — acquire only if no read or write locks exist. Read lock: shared — acquire only if no write lock exists; multiple read locks can coexist. Implementation: on write lock acquire, check no `/locks/{resource}/read:*` keys exist AND no `/locks/{resource}/write` key exists. On read lock acquire, check no `/locks/{resource}/write` key exists. Use etcd transactions for atomicity. This is complex; libraries like `go-etcd-lock` implement read-write semaphores on top of etcd.

Q5: How do you test a distributed lock implementation for correctness?  
A: (1) Linearizability testing with `Jepsen` — the industry standard for distributed systems safety testing. Jepsen injects network partitions, clock skew, and process kills while measuring lock invariants. (2) Unit tests with mocked etcd using deterministic Raft simulation. (3) Chaos testing: kill the etcd leader during an active lock acquisition; verify the client either gets the lock or gets a clear failure (no silent duplicate). (4) Concurrency stress test: 100 goroutines compete for the same lock; verify at most 1 holds it at any time using an atomic counter that must never exceed 1.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Detection | Recovery |
|---|---|---|---|
| etcd leader crash | Raft re-election (~150ms); writes briefly fail | Client receives `etcdserver: leader changed` error | Client retries with backoff; Raft elects new leader automatically |
| etcd minority node crash | No impact if quorum maintained (3/5 nodes) | etcd health check endpoint `/health` | Replace failed node; it resyncs from Raft log |
| etcd quorum lost (majority down) | Reads and writes fail completely | All `/health` checks fail | Restore quorum; if data lost, recover from etcd backup (snapshot) |
| Lock Service API server crash | Requests to that instance fail | Load balancer health check | LB routes to healthy instances; stateless — no data loss |
| Network partition between API server and etcd | Lock operations fail | Request timeout | Client retries to another API server that can reach etcd |
| Lock holder crashes without releasing | Lock held until TTL expiry | Lock `expires_at` monitor | TTL expires automatically; next waiter acquires lock |
| Lock holder GC pause > TTL | Lock expires; zombie may act | Fencing token validation at resource | Resource rejects zombie operations (fencing_token < last_seen) |
| Clock skew on etcd nodes | Raft election instability | NTP monitoring; etcd `--peer-auto-tls` cert expiry | Enforce NTP; etcd uses monotonic clocks internally for Raft timers |
| Thundering herd on lock release | All waiters wake simultaneously | Response latency spike | Use sequential-node wait-queue (watch predecessor) |

**Lock Renewal (keep-alive) Logic:**
```python
class LockKeepAlive:
    def __init__(self, lock_service, lock_name, token, ttl_ms):
        self.lock_service = lock_service
        self.lock_name = lock_name
        self.token = token
        self.ttl_ms = ttl_ms
        self.renewal_thread = None

    def start(self):
        self.renewal_thread = threading.Thread(target=self._renew_loop, daemon=True)
        self.renewal_thread.start()

    def _renew_loop(self):
        renewal_interval = self.ttl_ms * 2 / 3 / 1000  # renew at 2/3 of TTL
        while self.active:
            time.sleep(renewal_interval)
            try:
                response = self.lock_service.renew_lock(
                    self.lock_name, self.token,
                    new_ttl_ms=self.ttl_ms
                )
                if not response.success:
                    # Lock expired before renewal (our process was too slow)
                    self.on_lock_lost()
                    break
            except Exception as e:
                # Transient error: retry immediately once; then give up
                log.warning(f"Lock renewal failed: {e}")
                self.on_lock_lost()
                break

    def on_lock_lost(self):
        # CRITICAL: the process must stop its critical section immediately
        # Raise an exception in the business logic thread, trigger graceful shutdown,
        # or use a context variable that business logic checks periodically
        self.active = False
        signal_lock_lost()  # business logic must handle this
```

---

## 9. Monitoring & Observability

| Metric | Source | Alert Threshold | Meaning |
|---|---|---|---|
| `lock_acquisition_latency_p99` | gRPC server histogram | > 50 ms | Lock service overloaded or etcd slow |
| `lock_contention_queue_depth` | Per-lock gauge | > 10 waiters for > 30s | Severe contention; operation taking too long |
| `lock_expiry_rate` | Counter | > 1% of acquisitions | Processes crashing or running past TTL; investigate |
| `lock_release_success_rate` | Counter | < 99.9% | Clients failing to release; possible bug |
| `etcd_leader_changes_total` | etcd metrics | > 1 per hour | Cluster instability; check network, disk I/O |
| `etcd_disk_wal_fsync_duration_seconds_p99` | etcd metrics | > 10 ms | Disk I/O too slow for Raft WAL |
| `etcd_server_slow_apply_total` | etcd metrics | Any | etcd apply backlog; Raft falling behind |
| `fencing_token_gap` | Computed: token[n+1] - token[n] | > 1 for same lock | Possible zombie activity or audit gap |
| `zombie_operation_rejections` | Resource guard counter | > 0 | Zombie detected; investigate GC/network |
| `lock_holder_duration_p99` | Business metric | > 80% of max_ttl | Operations running too long; scale or optimize |

**Distributed Tracing:**
- Every lock acquire/release gRPC call carries W3C `traceparent` header
- Lock Service propagates trace to etcd client call (custom instrumentation)
- Trace spans: `lock.acquire` → `etcd.transaction` (shows Raft commit latency)
- Alert on `lock.acquire` spans with `etcd.transaction` taking > 10ms (indicates Raft issue)

**Logging:**
- Log every ACQUIRE/RELEASE/EXPIRE event at INFO with: lock_name, client_id, fencing_token, wait_duration_ms, hold_duration_ms (on release/expire)
- Log REJECTED (wrong token, wrong client) at WARN with full context
- Log ZOMBIE_REJECTED at ERROR with both token values
- Ship all logs to centralized logging (Datadog/Splunk) for post-incident analysis

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Alternative | Reason |
|---|---|---|---|
| Backend store | etcd (Raft) | ZooKeeper (ZAB) | Both valid; etcd is simpler (Go binary, no JVM), native K8s integration |
| Fencing token | etcd cluster revision | Application-managed counter | Free, tamper-proof, guaranteed monotonic by Raft |
| Lock expiry | Lease TTL (etcd native) | Application heartbeat | Atomic deletion on lease expiry; simpler than heartbeat protocol |
| Wait queue | Watch on lock key + retry (slight herd) | Sequential znodes (no herd) | Acceptable for low contention; sequential znodes if herd is observed |
| Exactly-correct acquire | Linearizable CAS transaction | Optimistic lock with retry | Safety-critical; no stale reads |
| Client library behavior | Auto-renew at 2/3 TTL | Manual renewal by caller | Prevents accidental expiry during long operations |
| Multi-lock deadlock prevention | Consistent sort order | Wait-for graph detection | Simpler; effective for known lock sets |
| Redlock rejection | etcd/ZK instead | Redlock (5 Redis nodes) | Redlock unsafe for storage-protecting locks; acceptable for advisory locks only |
| Lock granularity | Per-resource-instance (fine) | Per-resource-type (coarse) | Maximizes parallelism; reduces contention |
| Failure mode | Deny acquisition (safety over availability) | Allow acquisition with degraded guarantees | Lock correctness > availability |

---

## 11. Follow-up Interview Questions

Q1: How is a distributed lock different from a database row lock (SELECT FOR UPDATE)?  
A: `SELECT FOR UPDATE` is a database-level row lock within a single RDBMS. It's tied to a database transaction and is released when the transaction commits or rolls back. It's reliable, transactional, and handles failure atomically (connection drop = transaction rollback = lock release). A distributed lock spans multiple systems; the resource being protected is not necessarily a database. Use `SELECT FOR UPDATE` when protecting database rows within a single DB transaction. Use distributed locks when coordinating across services or protecting resources outside a database.

Q2: Describe a scenario where a distributed lock causes more harm than good.  
A: If the resource being protected is inherently idempotent (e.g., writing a file with a content hash), adding a distributed lock introduces failure modes (lock service unavailable) while not actually preventing concurrent writes (the file system handles atomicity). Locks should only be used when: (a) the operation is non-idempotent, (b) there is genuine contention, and (c) the lock's availability is higher than the operation's consistency requirements. Overuse of distributed locks is a common antipattern that creates unnecessary coordination overhead and latency.

Q3: How would you implement optimistic locking as an alternative to distributed locks?  
A: Optimistic locking assumes conflicts are rare. Each resource has a version number. To update: (1) Read the current version. (2) Perform computation. (3) Write with `UPDATE WHERE version = <read_version> AND SET version = version + 1`. If another process has updated the resource, the write fails (0 rows affected), and the caller retries. This eliminates the need for a lock service entirely — the database itself provides the serialization. Works excellently for low-contention workloads; for high-contention (many retries), a lock may have lower latency.

Q4: How do lock-free data structures relate to distributed locks?  
A: Lock-free data structures (CAS loops, compare-and-swap in hardware) eliminate locks entirely by using atomic CPU instructions. In distributed systems, CAS is implemented via the consensus protocol (etcd transactions, DynamoDB conditional writes). The distributed equivalent of a lock-free CAS is: `write if version == X; otherwise return current version and let caller retry`. This is optimistic concurrency control. True lock-free distributed algorithms exist (Michael-Scott queue, Herlihy's universal construction) but are complex to implement correctly.

Q5: What is Chubby and how did it inspire etcd and ZooKeeper?  
A: Chubby (described in the 2006 Google paper) is Google's distributed lock and small-file storage service, built on a Paxos-based consensus protocol. It introduced the concept of advisory distributed locks with sessions, lease-based TTLs, and a file-system-like namespace. ZooKeeper was built at Yahoo as an open-source equivalent. etcd was built by CoreOS as a simpler alternative for Kubernetes metadata storage. All three share the core insight: a strongly consistent, highly available coordination service is the right foundation for distributed locks, leader election, and configuration management.

Q6: How would you implement leader election using this distributed lock system?  
A: Leader election is a special case of lock acquisition: the "lock" is the "leader role," and the holder is the leader. Implementation: all candidates continuously attempt `AcquireLock("leader:service-X", ttl=15s, wait=true)`. The one that succeeds becomes the leader and continuously renews its lease. If it fails to renew (crash, network partition), the lock expires and another candidate wins. The fencing token serves as the leader's term: a stale leader with an old token is rejected by followers. This is exactly how etcd itself performs controller leader election in Kubernetes.

Q7: What is the relationship between distributed locks and distributed transactions?  
A: Distributed transactions (2PC) are a more powerful but more expensive primitive: they provide atomicity across multiple resources without requiring a lock held across the transaction duration. Locks can implement transactions (acquire all resource locks, modify, release), but this has limitations (hold time = transaction duration, deadlock risk). Modern systems prefer either (a) SAGA pattern (compensating transactions, no locks held across services), (b) optimistic concurrency control (no locks, retry on conflict), or (c) event sourcing (append-only log, no in-place mutation requiring locks).

Q8: How do you implement lock timeouts fairly without starvation?  
A: Without a FIFO queue, timeout-based lock acquisition can lead to starvation: a high-throughput client keeps re-acquiring the lock, and a low-priority client never gets a turn. Solutions: (1) FIFO queue (ZooKeeper sequential znodes) — serves waiters in arrival order. (2) Fair scheduling: track wait time per client; prioritize clients that have waited longest. (3) Lock admission control: limit the rate at which new waiters can join the queue for a specific lock (prevent the queue from growing unboundedly).

Q9: How would you handle a scenario where the lock service itself is down?  
A: Option 1 (fail-safe = deny): if the lock service is unreachable, deny all operations requiring the lock. This is the safest behavior for storage-protecting locks — better to refuse work than to allow concurrent unsafe operations. Option 2 (fail-open with local lease): if the lock service was last known to have granted the lock with TTL T, allow the client to continue operating for T seconds while the lock service is recovering (trusting the last granted lease). Dangerous — only for advisory locks. Option 3 (circuit breaker): after N consecutive failures to reach the lock service, open circuit and return a configurable failure mode (either deny or allow without lock). Always prefer Option 1 for critical sections.

Q10: Explain the CAP theorem implications for a distributed lock service.  
A: A distributed lock service requires CP (consistency and partition tolerance). If we choose AP (availability over consistency), during a network partition, two partitions could each believe they hold the lock — violating mutual exclusion. etcd and ZooKeeper explicitly choose CP: during a partition where the minority segment cannot form a quorum, the minority side stops accepting writes (lock acquisitions fail). This is the correct tradeoff for a lock service: it is better to return an error than to grant a lock that is not truly exclusive.

Q11: How do you implement a distributed counting semaphore (limit N concurrent holders)?  
A: Use an atomic counter in etcd. Acquire: `TRANSACTION: IF counter < N THEN INCR counter, STORE holder_record EL`. Release: `TRANSACTION: DEL holder_record, DECR counter`. Alternatively, use N separate "slot" locks: `lock:semaphore:X:slot:0` through `lock:semaphore:X:slot:N-1`. Client tries slots in random order; acquires whichever slot is available first. Slot-based approach avoids a counter hot spot and distributes load. Watch all N slot keys to be notified when any slot opens.

Q12: What are the trade-offs between using TTL-based locks vs heartbeat-based locks?  
A: **TTL-based:** Simple — set an expiry at acquisition time. If client dies, lock expires automatically after TTL. Downside: if the protected operation takes longer than TTL, the lock expires while the client is still working (requires careful TTL choice and renewal). **Heartbeat-based:** Client sends periodic keepalive signals; lock expires only if keepalive stops. More accurate to actual liveness. Downside: requires persistent connection or frequent RPCs; if keepalive message is delayed but client is alive, lock may incorrectly expire. In practice, TTL + renewal at 2/3 TTL is the standard approach (etcd leases work exactly this way).

Q13: How would you debug a situation where two services claim to hold the same lock simultaneously?  
A: (1) Check if both services are using the same lock service endpoint. (2) Examine audit logs: find the token values each service claims to hold — if they're different tokens, one was a zombie that acted after its token expired. (3) Confirm fencing token enforcement at the resource: if the resource is not checking fencing tokens, fix that immediately. (4) Check for clock skew: if Redis-based lock, a TTL expiry before expected indicates clock drift. (5) Check for partition healing: if etcd was partitioned and healed, a client on the minority side may have been granted a lock by a partitioned node that didn't form quorum.

Q14: What is "lock coarsening" and when is it useful?  
A: Lock coarsening merges multiple consecutive fine-grained lock acquisitions into a single coarser-grained lock, reducing locking overhead. Example: if a function acquires `lock(A)`, releases, then immediately acquires `lock(A)` again, merge them into one `lock(A)` acquisition covering both operations. This reduces round-trips to the lock service. JIT compilers (JVM) do this automatically for synchronized blocks. In distributed systems, merge sequential operations on the same resource into a single lock acquisition when the intermediate release is unnecessary.

Q15: How would you extend this lock service to support cross-datacenter mutual exclusion?  
A: Cross-DC locks require a globally consistent consensus protocol across DCs. Options: (1) **etcd with geo-distributed Raft:** a 5-node etcd cluster with nodes spread across DCs. Write quorum requires majority ACK across DCs — write latency = cross-DC RTT (~50–100ms). Acceptable for infrequent, long-held locks. (2) **Google Spanner TrueTime:** uses GPS and atomic clocks to bound clock uncertainty; allows globally consistent timestamps without cross-DC round trips for reads. Not available outside GCP. (3) **Consensus hierarchy:** use local locks (fast, intra-DC) for most operations; escalate to a cross-DC lock only for globally unique operations. (4) **Conflict-free by design:** redesign the system so global locks aren't needed (e.g., use sharding by geography — DC-A owns user IDs 0-500M, DC-B owns 500M-1B; no cross-DC locking needed).

---

## 12. References & Further Reading

- **Martin Kleppmann — "How to do distributed locking":** https://martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html
- **Antirez (Salvatore Sanfilippo) — "Is Redlock safe?":** http://antirez.com/news/101
- **Google Chubby Paper — "The Chubby lock service for loosely-coupled distributed systems" (OSDI 2006):** https://research.google.com/archive/chubby-osdi06.pdf
- **etcd Documentation — Distributed Locks:** https://etcd.io/docs/v3.5/dev-guide/interacting_v3/#distributed-locks
- **etcd Documentation — Transactions:** https://etcd.io/docs/v3.5/learning/api/#transaction
- **Apache ZooKeeper — Recipes and Solutions (Distributed Locks):** https://zookeeper.apache.org/doc/r3.7.1/recipes.html#sc_recipes_Locks
- **Redis Documentation — Distributed Locks with Redlock:** https://redis.io/docs/manual/patterns/distributed-locks/
- **Designing Data-Intensive Applications — Kleppmann, Chapter 8 (Trouble with Distributed Systems), Chapter 9 (Consistency and Consensus):** O'Reilly, 2017
- **Jepsen — Distributed Systems Safety Analysis:** https://jepsen.io/
- **FaunaDB Blog — Distributed Transactions via Calvin (strong consistency model):** https://fauna.com/blog/introducing-faunadb
- **go-etcd-lock — Go library for etcd-based distributed locks:** https://github.com/xordataexchange/crypt / etcd/clientv3/concurrency package
- **Herlihy — "Wait-free synchronization" (1991 ACM Transactions):** https://dl.acm.org/doi/10.1145/114005.102808
- **Leslie Lamport — "Time, Clocks, and the Ordering of Events in a Distributed System" (1978):** https://lamport.azurewebsites.net/pubs/time-clocks.pdf
