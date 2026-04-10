# System Design: Distributed Lock Service

> **Relevance to role:** Distributed locks are the backbone of coordination in cloud infrastructure. Bare-metal provisioning needs single-writer guarantees for IPMI operations, job schedulers require leader election (built on locks), Kubernetes controllers use lease-based locks for controller-manager HA, and any control-plane that mutates shared state (OpenStack Nova scheduler, placement service) depends on correct lock semantics to prevent split-brain corruption.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Acquire lock | Client requests a named lock; service grants it to exactly one holder at a time |
| FR-2 | Release lock | Holder explicitly releases; other waiters are notified |
| FR-3 | Automatic expiry | If holder crashes, lock is released after a configurable TTL / session timeout |
| FR-4 | Lock types | Exclusive (write), shared (read), reentrant (same owner can re-acquire) |
| FR-5 | Fencing tokens | Each lock grant returns a monotonically increasing token; protected resources reject stale tokens |
| FR-6 | Lock queuing | Waiters are served in FIFO order (fair lock) |
| FR-7 | Try-lock with timeout | Client specifies max wait; returns failure if not acquired within deadline |
| FR-8 | Watch / notification | Clients subscribe to lock-state changes (etcd watch, ZK watcher) |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Availability | 99.99% (control-plane grade) |
| NFR-2 | Lock acquisition latency | p50 < 5 ms, p99 < 50 ms within same AZ |
| NFR-3 | Consistency | Linearizable — no two clients may hold the same exclusive lock simultaneously |
| NFR-4 | Durability | Lock state survives single-node failure |
| NFR-5 | Throughput | 50,000 lock operations/sec cluster-wide |
| NFR-6 | Scalability | Support 500K+ named locks |
| NFR-7 | Partition tolerance | Must not grant lock to two holders during network partition (CP system) |

### Constraints & Assumptions
- Deployment: 3 or 5 node cluster across availability zones in one region.
- Clients are Java/Python services, bare-metal provisioners, k8s controllers.
- Network RTT within region < 2 ms; cross-AZ < 5 ms.
- Clock skew between nodes bounded by NTP (< 100 ms); no reliance on synchronized clocks for correctness.
- Infrastructure runs on Linux bare-metal or VMs managed by the platform.

### Out of Scope
- Multi-region lock replication (use separate lock clusters per region).
- Distributed semaphore / counting locks (extension, not core).
- Application-level transaction coordination (covered in distributed_transaction_coordinator.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Services using locks | 500 | Platform microservices + controllers |
| Locks per service (avg) | 20 | Scheduler partitions, provisioner resources |
| Total named locks | 10,000 | 500 x 20 |
| Lock ops per service per sec | 100 | Acquire + release + heartbeat |
| Total lock ops/sec | 50,000 | 500 x 100 |
| Peak multiplier | 3x | Deployments, failovers |
| Peak lock ops/sec | 150,000 | 50,000 x 3 |
| Concurrent lock holders | 10,000 | Roughly equals named locks |
| Active sessions / leases | 500 | One per service instance |

### Latency Requirements

| Operation | p50 | p99 | Notes |
|-----------|-----|-----|-------|
| Lock acquire (no contention) | 2 ms | 10 ms | Single Raft round-trip |
| Lock acquire (contended, FIFO wait) | 50 ms | 500 ms | Depends on holder duration |
| Lock release | 1 ms | 5 ms | Raft commit |
| Lease renewal / heartbeat | 1 ms | 5 ms | Raft commit |
| Fencing token read | < 1 ms | 2 ms | Linearizable read from leader |

### Storage Estimates

| Item | Size | Total |
|------|------|-------|
| Lock key (path) | 256 B avg | |
| Lock value (owner, token, TTL) | 128 B | |
| Per lock record | ~400 B | |
| 10K locks | 4 MB | |
| Raft log (1 day, 50K ops/sec) | ~20 GB | 50K x 400B x 86400 / compaction |
| Raft snapshots (3 copies) | 12 MB | 4MB x 3 |
| **Total per node** | **~25 GB** | With WAL and snapshots |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Client -> Leader (ops) | 50K ops/sec x 512 B | 25 MB/s |
| Leader -> Followers (replication) | 50K ops/sec x 512 B x 2 followers | 50 MB/s |
| Heartbeats (Raft) | 10 heartbeats/sec x 64 B x 4 peers | negligible |
| Watch notifications | 5K events/sec x 256 B | 1.25 MB/s |

---

## 3. High-Level Architecture

```
                          ┌──────────────────────────────────────────────┐
                          │              Client Applications             │
                          │  (Java/Python services, k8s controllers,     │
                          │   bare-metal provisioners)                   │
                          └──────────────┬───────────────────────────────┘
                                         │ gRPC / HTTP
                          ┌──────────────▼───────────────────────────────┐
                          │           Client SDK / Library               │
                          │  ┌──────────┐ ┌────────────┐ ┌───────────┐  │
                          │  │Lock API  │ │Session Mgr │ │Watch Mgr  │  │
                          │  └──────────┘ └────────────┘ └───────────┘  │
                          └──────────────┬───────────────────────────────┘
                                         │
              ┌──────────────────────────┼──────────────────────────┐
              │                          │                          │
      ┌───────▼───────┐         ┌───────▼───────┐         ┌───────▼───────┐
      │   Node 1      │         │   Node 2      │         │   Node 3      │
      │   (Leader)    │◄───────►│  (Follower)   │◄───────►│  (Follower)   │
      │               │  Raft   │               │  Raft   │               │
      │ ┌───────────┐ │         │ ┌───────────┐ │         │ ┌───────────┐ │
      │ │ Lock Mgr  │ │         │ │ Lock Mgr  │ │         │ │ Lock Mgr  │ │
      │ ├───────────┤ │         │ ├───────────┤ │         │ ├───────────┤ │
      │ │ Session   │ │         │ │ Session   │ │         │ │ Session   │ │
      │ │ Tracker   │ │         │ │ Tracker   │ │         │ │ Tracker   │ │
      │ ├───────────┤ │         │ ├───────────┤ │         │ ├───────────┤ │
      │ │ Fencing   │ │         │ │ Fencing   │ │         │ │ Fencing   │ │
      │ │ Token Gen │ │         │ │ Token Gen │ │         │ │ Token Gen │ │
      │ ├───────────┤ │         │ ├───────────┤ │         │ ├───────────┤ │
      │ │Raft Engine│ │         │ │Raft Engine│ │         │ │Raft Engine│ │
      │ ├───────────┤ │         │ ├───────────┤ │         │ ├───────────┤ │
      │ │ State     │ │         │ │ State     │ │         │ │ State     │ │
      │ │ Machine   │ │         │ │ Machine   │ │         │ │ Machine   │ │
      │ ├───────────┤ │         │ ├───────────┤ │         │ ├───────────┤ │
      │ │ WAL +     │ │         │ │ WAL +     │ │         │ │ WAL +     │ │
      │ │ Snapshots │ │         │ │ Snapshots │ │         │ │ Snapshots │ │
      │ └───────────┘ │         │ └───────────┘ │         │ └───────────┘ │
      └───────────────┘         └───────────────┘         └───────────────┘
              AZ-1                      AZ-2                      AZ-3
```

### Component Roles

| Component | Role |
|-----------|------|
| **Client SDK** | Wraps gRPC calls; manages sessions (keep-alive heartbeats); auto-reconnects; provides `Lock`, `TryLock`, `Unlock`, `Watch` APIs |
| **Lock Manager** | Processes lock/unlock requests on the leader; maintains FIFO wait queue per lock key; issues fencing tokens |
| **Session Tracker** | Maps client sessions to held locks; expires sessions on missed heartbeats; releases all locks on session death |
| **Fencing Token Generator** | Monotonically increasing counter per lock key; persisted in Raft state machine; returned on every lock grant |
| **Raft Engine** | Consensus layer; replicates log entries; elects leader; guarantees linearizability |
| **State Machine** | Deterministic FSM applied identically on all nodes; stores lock state, sessions, fencing counters |
| **WAL + Snapshots** | Write-ahead log for durability; periodic snapshots for log compaction |

### Data Flows

**Lock Acquisition (happy path):**
1. Client SDK sends `Acquire(lock_key, session_id, lock_type, timeout)` to leader via gRPC.
2. Leader's Lock Manager checks if lock is free.
3. If free: Lock Manager creates a Raft proposal `{op: GRANT, key, session, fence_token: next_counter}`.
4. Raft Engine replicates to majority (2 of 3).
5. On commit, State Machine applies grant; Lock Manager returns `{granted: true, fence_token: N}`.
6. If not free: request is queued; client blocks (or times out). When current holder releases, next waiter is proposed.

**Session Keep-Alive:**
1. Client SDK sends periodic `KeepAlive(session_id)` every `TTL/3` seconds.
2. Leader refreshes session expiry in state machine via Raft.
3. If no keep-alive received within TTL, session expires; all held locks released.

**Fencing Token Usage:**
1. Client receives `fence_token = 42` with lock grant.
2. Client passes `fence_token = 42` to protected resource (e.g., storage API).
3. Protected resource rejects any request with `fence_token < last_seen_token`.

---

## 4. Data Model

### Core Entities & Schema

```
┌─────────────────────────────────────────────────────────┐
│ Lock                                                    │
├─────────────────────────────────────────────────────────┤
│ key           VARCHAR(512)    PK    -- e.g. /locks/bm/  │
│                                        server-42        │
│ lock_type     ENUM(EXCLUSIVE, SHARED)                   │
│ holders       LIST<SessionRef>  -- 1 for excl, N for    │
│                                    shared               │
│ fence_token   BIGINT           -- monotonic counter     │
│ wait_queue    LIST<WaitEntry>  -- FIFO ordered          │
│ created_at    TIMESTAMP                                 │
│ updated_at    TIMESTAMP                                 │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ Session                                                 │
├─────────────────────────────────────────────────────────┤
│ session_id    UUID             PK                       │
│ client_addr   VARCHAR(64)                               │
│ ttl_seconds   INT              -- e.g. 15               │
│ last_renewed  TIMESTAMP                                 │
│ held_locks    SET<LockKey>                              │
│ metadata      MAP<STRING,STRING>  -- client name, etc.  │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ WaitEntry                                               │
├─────────────────────────────────────────────────────────┤
│ session_id    UUID                                      │
│ lock_type     ENUM(EXCLUSIVE, SHARED)                   │
│ enqueued_at   TIMESTAMP                                 │
│ deadline      TIMESTAMP        -- try-lock timeout      │
└─────────────────────────────────────────────────────────┘
```

### Database Selection

| Option | Verdict | Rationale |
|--------|---------|-----------|
| **Embedded FSM (BoltDB / bbolt)** | **Selected** | etcd uses bbolt; all state is the Raft state machine. No external DB needed. B+ tree provides ordered key traversal. Single-writer model matches Raft leader semantics. |
| SQLite | Rejected | Adds SQL overhead; lock state is key-value shaped |
| External MySQL | Rejected | Introduces dependency cycle (lock service should have minimal deps) |
| In-memory only | Rejected | Loses durability; requires full re-sync on restart |

### Indexing Strategy

- **Primary index:** Lock key (B+ tree in bbolt, sorted byte order).
- **Secondary index:** Session ID -> set of held lock keys (in-memory map, rebuilt from snapshot on restart).
- **Fencing token index:** Per-key monotonic counter stored inline with lock record.

---

## 5. API Design

### gRPC Service Definition

```protobuf
service LockService {
  // Session management
  rpc CreateSession(CreateSessionRequest) returns (CreateSessionResponse);
  rpc KeepAlive(stream KeepAliveRequest) returns (stream KeepAliveResponse);
  rpc DestroySession(DestroySessionRequest) returns (DestroySessionResponse);

  // Lock operations
  rpc Acquire(AcquireRequest) returns (AcquireResponse);
  rpc TryAcquire(TryAcquireRequest) returns (TryAcquireResponse);
  rpc Release(ReleaseRequest) returns (ReleaseResponse);
  rpc GetLockInfo(GetLockInfoRequest) returns (LockInfo);

  // Watch
  rpc Watch(WatchRequest) returns (stream WatchEvent);
}

message AcquireRequest {
  string lock_key = 1;
  string session_id = 2;
  LockType lock_type = 3;          // EXCLUSIVE or SHARED
  int32 timeout_ms = 4;            // 0 = block forever
  bool reentrant = 5;              // allow same session re-acquire
}

message AcquireResponse {
  bool granted = 1;
  int64 fence_token = 2;           // monotonically increasing
  string error = 3;
}

message ReleaseRequest {
  string lock_key = 1;
  string session_id = 2;
  int64 fence_token = 3;           // must match current token
}

enum LockType {
  EXCLUSIVE = 0;
  SHARED = 1;
}
```

### REST Endpoints (HTTP gateway)

| Method | Path | Description |
|--------|------|-------------|
| POST | `/v1/sessions` | Create session with TTL |
| POST | `/v1/sessions/{id}/keepalive` | Renew session |
| DELETE | `/v1/sessions/{id}` | Destroy session, release all locks |
| POST | `/v1/locks/{key}/acquire` | Acquire lock (body: session_id, type, timeout) |
| POST | `/v1/locks/{key}/try-acquire` | Non-blocking acquire attempt |
| POST | `/v1/locks/{key}/release` | Release lock (body: session_id, fence_token) |
| GET | `/v1/locks/{key}` | Get lock info (holder, waiters, fence_token) |
| GET | `/v1/locks/{key}/watch` | SSE stream of lock state changes |

### CLI

```bash
# Create a session
lockctl session create --ttl 15s
# Output: session_id=a1b2c3d4

# Acquire exclusive lock
lockctl lock acquire /locks/bm/server-42 --session a1b2c3d4 --type exclusive --timeout 30s
# Output: granted=true fence_token=147

# Release
lockctl lock release /locks/bm/server-42 --session a1b2c3d4 --fence-token 147

# Inspect
lockctl lock info /locks/bm/server-42
# Output:
# Key:          /locks/bm/server-42
# Type:         EXCLUSIVE
# Holder:       session=a1b2c3d4 (provisioner-host-7)
# Fence Token:  147
# Waiters:      2
# Created:      2026-04-09T10:15:00Z

# Watch for changes
lockctl lock watch /locks/bm/server-42
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Lock Acquisition Algorithm (FIFO Fair Lock)

**Why it's hard:**
Multiple clients may request the same lock simultaneously. We need to guarantee mutual exclusion (only one exclusive holder), support shared (read) locks alongside exclusive (write) locks, maintain fairness (no starvation), and do all of this through a replicated state machine where the leader can change at any time.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| Simple CAS on leader | Low latency | No fairness; thundering herd on release |
| ZooKeeper ephemeral sequential znodes | Fair FIFO; well-proven | Requires sequential node creation + watch on predecessor |
| etcd lease + revision-based ordering | Fair; uses existing Raft revision | More complex client logic |
| Redis Redlock | Low latency; no Raft overhead | Not linearizable; clock-dependent; Kleppmann critique applies |
| **Raft-replicated FIFO queue** | **Linearizable + fair + fencing** | Slightly higher latency (Raft round-trip) |

**Selected: Raft-replicated FIFO queue with read-write lock semantics**

Justification: For infrastructure control-plane locks (bare-metal IPMI, scheduler partitions), correctness is paramount. Redis Redlock's reliance on wall-clock timing makes it unsuitable (Martin Kleppmann's analysis shows that process pauses, GC, or clock jumps can violate mutual exclusion). A Raft-replicated approach gives us linearizability by construction.

**Implementation:**

```python
class LockStateMachine:
    """
    Deterministic state machine applied identically on all Raft nodes.
    All mutations go through Raft log.
    """

    def __init__(self):
        self.locks = {}          # key -> LockState
        self.sessions = {}       # session_id -> SessionState
        self.global_fence = 0    # global monotonic counter

    def apply(self, entry: RaftLogEntry) -> Result:
        if entry.op == "ACQUIRE":
            return self._acquire(entry.key, entry.session_id,
                                 entry.lock_type, entry.deadline)
        elif entry.op == "RELEASE":
            return self._release(entry.key, entry.session_id,
                                 entry.fence_token)
        elif entry.op == "SESSION_EXPIRE":
            return self._expire_session(entry.session_id)

    def _acquire(self, key, session_id, lock_type, deadline):
        if key not in self.locks:
            self.locks[key] = LockState(key=key, holders=[],
                                         wait_queue=deque(),
                                         fence_token=0)
        lock = self.locks[key]

        # Reentrant check: if same session already holds it
        if session_id in [h.session_id for h in lock.holders]:
            return Result(granted=True, fence_token=lock.fence_token,
                          reentrant=True)

        # Can we grant immediately?
        if self._can_grant(lock, lock_type):
            return self._grant(lock, session_id, lock_type)
        else:
            # Enqueue waiter
            wait_entry = WaitEntry(session_id=session_id,
                                   lock_type=lock_type,
                                   deadline=deadline)
            lock.wait_queue.append(wait_entry)
            return Result(granted=False, queued=True,
                          position=len(lock.wait_queue))

    def _can_grant(self, lock, requested_type):
        """
        Grant rules:
        - EXCLUSIVE: only if no holders AND no queued exclusive waiters
          ahead (to prevent writer starvation)
        - SHARED: only if no exclusive holder AND no queued exclusive
          waiters ahead (writer-preference to prevent starvation)
        """
        if not lock.holders:
            return True
        if requested_type == LockType.SHARED:
            # Allow if all current holders are SHARED and no EXCLUSIVE
            # waiter is ahead in queue
            all_shared = all(h.lock_type == LockType.SHARED
                            for h in lock.holders)
            no_excl_waiter = not any(w.lock_type == LockType.EXCLUSIVE
                                     for w in lock.wait_queue)
            return all_shared and no_excl_waiter
        return False  # EXCLUSIVE requires empty holders

    def _grant(self, lock, session_id, lock_type):
        self.global_fence += 1
        lock.fence_token = self.global_fence
        lock.holders.append(Holder(session_id=session_id,
                                    lock_type=lock_type))
        self.sessions[session_id].held_locks.add(lock.key)
        return Result(granted=True, fence_token=lock.fence_token)

    def _release(self, key, session_id, fence_token):
        lock = self.locks.get(key)
        if not lock:
            return Result(error="LOCK_NOT_FOUND")

        # Validate fence token to prevent stale releases
        holder = next((h for h in lock.holders
                       if h.session_id == session_id), None)
        if not holder:
            return Result(error="NOT_HOLDER")

        lock.holders.remove(holder)
        self.sessions[session_id].held_locks.discard(key)

        # Try to grant to next waiter(s)
        self._process_wait_queue(lock)
        return Result(released=True)

    def _process_wait_queue(self, lock):
        """
        After a release, grant to as many compatible waiters as possible.
        - If next waiter is EXCLUSIVE: grant only if holders empty
        - If next waiter is SHARED: grant it and all subsequent SHARED
          waiters until an EXCLUSIVE waiter is found
        """
        while lock.wait_queue:
            # Remove expired waiters
            while (lock.wait_queue and
                   lock.wait_queue[0].deadline < current_time()):
                expired = lock.wait_queue.popleft()
                self._notify_waiter(expired, Result(error="TIMEOUT"))

            if not lock.wait_queue:
                break

            next_waiter = lock.wait_queue[0]

            if next_waiter.lock_type == LockType.EXCLUSIVE:
                if not lock.holders:
                    waiter = lock.wait_queue.popleft()
                    result = self._grant(lock, waiter.session_id,
                                         LockType.EXCLUSIVE)
                    self._notify_waiter(waiter, result)
                break  # Can't grant more after exclusive

            elif next_waiter.lock_type == LockType.SHARED:
                # Grant all consecutive shared waiters
                while (lock.wait_queue and
                       lock.wait_queue[0].lock_type == LockType.SHARED):
                    waiter = lock.wait_queue.popleft()
                    result = self._grant(lock, waiter.session_id,
                                         LockType.SHARED)
                    self._notify_waiter(waiter, result)
                break  # Stop at next exclusive waiter

    def _expire_session(self, session_id):
        """Called when session TTL expires (no keep-alive received)."""
        session = self.sessions.get(session_id)
        if not session:
            return
        for lock_key in list(session.held_locks):
            self._release(lock_key, session_id, fence_token=None)
        del self.sessions[session_id]
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Lock holder crashes | Lock held until session TTL expires | TTL should be short (10-15s); keep-alive interval = TTL/3 |
| Leader crashes mid-acquire | Client request fails | Client retries; new leader has committed state |
| Network partition (client isolated from leader) | Session expires, lock released | Client detects via keep-alive failure; must re-acquire |
| Raft split-brain (2 leaders temporarily) | Impossible with correct Raft | Old leader cannot commit (lacks quorum); new leader has all committed entries |

**Interviewer Q&A:**

**Q1: Why not use Redis Redlock for infrastructure locks?**
A: Martin Kleppmann's 2016 analysis ("How to do distributed locking") showed that Redlock depends on wall-clock timing assumptions. A GC pause, process suspension, or clock jump can cause two clients to hold the lock simultaneously. For infra control-plane (e.g., bare-metal IPMI where issuing two conflicting firmware updates could brick a server), we need linearizable locks backed by consensus, not timing.

**Q2: How does the fencing token prevent the "paused client" problem?**
A: Client A acquires the lock with fence_token=42, then pauses (GC, swap). The lock expires. Client B acquires with fence_token=43. Client A wakes up, still thinks it holds the lock, and sends a write with fence_token=42 to the storage layer. The storage layer rejects it because 42 < 43 (last seen). This requires the protected resource to be "fence-aware."

**Q3: What is the difference between ZooKeeper's ephemeral sequential znodes and etcd's lease-based approach?**
A: In ZooKeeper, each lock acquisition creates a sequential ephemeral znode (e.g., `/locks/mylock/lock-00000047`). The client watches its predecessor. When the predecessor is deleted, it checks if it's now the lowest — if so, it holds the lock. In etcd, a client creates a key with a lease (TTL). Lock ordering uses the key's create revision. The client watches all keys with revision lower than its own. Both achieve the same FIFO fairness.

**Q4: How do you handle deadlocks?**
A: Three strategies: (1) Lock ordering — all clients acquire locks in a canonical order (e.g., alphabetical by key), preventing circular waits. (2) Timeout-based — every acquire has a deadline; if not granted in time, the attempt fails and the client backs off. (3) Deadlock detection — build a wait-for graph and abort one participant. We prefer (1) + (2) because detection adds complexity and latency.

**Q5: Can shared (read) locks starve exclusive (write) lock waiters?**
A: Yes, if we naively grant shared locks whenever no exclusive holder exists. Our algorithm uses writer-preference: once an exclusive waiter is in the queue, no new shared locks are granted. This prevents write starvation while allowing concurrent readers when no writer is waiting.

**Q6: What happens if the Raft leader is overloaded with lock requests?**
A: Lock operations are Raft proposals, so the leader bears the replication cost. Mitigations: (1) Batch multiple lock proposals into a single Raft entry. (2) Use lock partitioning — shard locks across multiple independent Raft groups (e.g., by key hash). (3) For read-only operations (lock info, watch), serve from followers with linearizable reads (read-index protocol).

---

### Deep Dive 2: Session Management and Automatic Lock Expiry

**Why it's hard:**
If a lock holder crashes, the lock must be released automatically. But "crash detection" in distributed systems is fundamentally unreliable — we can't distinguish a crashed node from a slow one. Too-short TTLs cause false expirations (lock released while holder is alive but slow); too-long TTLs cause availability delays (dead holder's lock isn't released for minutes).

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| Fixed TTL, no keep-alive | Simple | Can't extend; holder must finish before TTL |
| **TTL + periodic keep-alive** | **Balances safety and liveness** | Keep-alive storms under high session count |
| Failure detector (phi accrual) | Adaptive to network conditions | Complex; probabilistic |
| TCP connection-based (ZK sessions) | Instant detection on connection drop | Not suitable for gRPC load-balanced connections |
| No TTL (explicit release only) | No false expirations | Lock stuck forever if holder crashes |

**Selected: TTL + periodic keep-alive (etcd lease model)**

**Implementation:**

```python
class SessionManager:
    """
    Manages client sessions with TTL-based expiry.
    All state changes go through Raft.
    """

    def __init__(self, raft, lock_manager, min_ttl=5, max_ttl=300):
        self.raft = raft
        self.lock_manager = lock_manager
        self.min_ttl = min_ttl      # seconds
        self.max_ttl = max_ttl      # seconds
        self.checker_interval = 1   # check every second

    def create_session(self, client_id, requested_ttl):
        """Create a new session with bounded TTL."""
        ttl = max(self.min_ttl, min(requested_ttl, self.max_ttl))
        session_id = generate_uuid()

        # Propose via Raft
        entry = RaftEntry(
            op="SESSION_CREATE",
            session_id=session_id,
            client_id=client_id,
            ttl=ttl,
            expires_at=now() + ttl
        )
        self.raft.propose(entry)
        return session_id, ttl

    def keep_alive(self, session_id):
        """
        Extend session expiry. Client should call this every TTL/3.

        Why TTL/3?
        - At TTL/3 interval, client has 2 more attempts before expiry
        - Accounts for 1 missed heartbeat (network blip) + 1 retry
        - Example: TTL=15s, heartbeat every 5s
          - Miss at t=5: next attempt at t=10
          - Miss at t=10: session expires at t=15
          - So client tolerates 1 full missed heartbeat
        """
        entry = RaftEntry(
            op="SESSION_RENEW",
            session_id=session_id,
            new_expires_at=now() + self.sessions[session_id].ttl
        )
        result = self.raft.propose(entry)
        return result.new_expires_at

    def check_expirations(self):
        """
        Leader periodically checks for expired sessions.
        Only the leader runs this to avoid duplicate proposals.
        """
        if not self.raft.is_leader():
            return

        current = now()
        for session_id, session in self.sessions.items():
            if session.expires_at <= current:
                # Propose session expiration through Raft
                entry = RaftEntry(
                    op="SESSION_EXPIRE",
                    session_id=session_id,
                    reason="TTL_EXPIRED"
                )
                self.raft.propose(entry)
                # This will trigger lock_manager._expire_session()
                # which releases all locks held by this session

    def on_leader_change(self):
        """
        When a new leader is elected, it must check all sessions
        because the old leader's expiration checker may have stopped.

        CRITICAL: The new leader must NOT immediately expire sessions
        that appear overdue. The session's keep-alive may have been
        in-flight during the election. Grant a grace period.
        """
        grace_period = 2  # seconds
        for session in self.sessions.values():
            session.expires_at = max(session.expires_at,
                                      now() + grace_period)
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Client network partition | Keep-alive fails; session expires; locks released | Client SDK detects and logs warning; must re-acquire locks after partition heals |
| Leader failover during keep-alive | One keep-alive lost | Client retries immediately to new leader; TTL/3 interval gives 2 extra attempts |
| Clock skew between leader and followers | Session may expire early/late on different nodes | All expiry decisions made by leader and replicated via Raft; only Raft-committed time matters |
| Keep-alive storm (500 sessions x TTL/3) | ~100 Raft proposals/sec for keep-alives alone | Batch keep-alives; use streaming gRPC for efficient transport |

**Interviewer Q&A:**

**Q1: What TTL would you recommend for bare-metal provisioning locks?**
A: 30-60 seconds. Bare-metal operations (IPMI commands, PXE boot) are slow. A 30s TTL with 10s heartbeat interval gives tolerance for 2 missed heartbeats. The provisioner should NOT hold the lock for the entire multi-minute provisioning — instead, hold it only during the critical IPMI mutation, then release and use an idempotency token for the rest.

**Q2: How does etcd implement leases internally?**
A: etcd leases are first-class Raft-replicated objects. A lease has an ID and a TTL. Keys are attached to leases. The leader runs a lease expiry checker; when a lease expires, it proposes a revoke through Raft, which deletes all attached keys. Keep-alives extend the lease TTL. Lease grants and revokes are Raft log entries; keep-alives are optimized to only require leader confirmation (not full Raft commit in newer versions).

**Q3: What if a client holds a lock but is actually hung (not crashed)?**
A: The keep-alive mechanism doesn't distinguish hung from healthy — if the client's keep-alive goroutine/thread is still running, the session stays alive. This is correct behavior: the lock service guarantees mutual exclusion, not progress. If you need to detect hung clients, implement application-level health checks and a separate "force-release" admin API.

**Q4: Can session TTL be dynamic?**
A: Yes. The client can request a new TTL on each keep-alive. The server bounds it between min_ttl and max_ttl. This is useful for clients that know they need the lock longer (e.g., a long-running bare-metal firmware update).

**Q5: How do you handle the "zombie session" problem?**
A: A zombie session is one where the client has crashed but the session hasn't expired yet. During this window, the lock is held but unused. The only safe approach is to wait for TTL expiry. Aggressive approaches (e.g., expiring on TCP disconnect) are unsafe because TCP RSTs can be spurious. Fencing tokens protect downstream resources even if a zombie session briefly overlaps with a new holder.

**Q6: What is ZooKeeper's approach vs etcd's approach to session management?**
A: ZooKeeper uses TCP session-based tracking. When the TCP connection drops, ZK starts a session timeout countdown. If the client reconnects within the timeout, the session is preserved. If not, ephemeral nodes are deleted. etcd uses explicit leases with keep-alive RPCs. etcd's approach is more network-topology-friendly (works through load balancers, proxies) because it doesn't rely on a persistent TCP connection.

---

### Deep Dive 3: Redis Redlock Analysis and Kleppmann Critique

**Why it's hard:**
Redis is ubiquitous, and many teams reach for `SETNX` / Redlock for distributed locking. Understanding why this is problematic for safety-critical infrastructure locks is essential interview material.

**Redlock Algorithm (Antirez, 2015):**

```
1. Get current time T1
2. Try to acquire lock on N independent Redis masters (e.g., N=5)
   with same key, random value, and TTL
3. Get current time T2
4. Lock is acquired if:
   a. Majority (N/2 + 1 = 3) masters granted the lock
   b. Total elapsed time (T2 - T1) < TTL
5. If acquired, effective TTL = original TTL - (T2 - T1)
6. If not acquired, release lock on all masters
```

**Kleppmann's Critique (2016):**

```
Timeline showing the flaw:

Client A        Lock Service (Redis)      Protected Resource
   |                                           |
   |--- Acquire lock (TTL=10s) ----------->    |
   |<-- Lock granted (no fence token) ---      |
   |                                           |
   |  [GC pause for 11 seconds]               |
   |                                           |
   |  (Lock expires after 10s)                |
   |                                           |
   |           Client B acquires lock -------> |
   |           Client B writes data ---------> |
   |                                           |
   |  [GC pause ends]                         |
   |--- Writes data (thinks it holds lock) --> |
   |                                           |
   VIOLATION: Both A and B wrote to the resource
```

**Why Redlock fails:**
1. **No fencing tokens:** Redis doesn't provide monotonically increasing tokens. Even if you add them, Redis replication is async — a failover can lose the token counter.
2. **Clock dependency:** Redlock assumes that `T2 - T1` accurately measures elapsed real time. Process pauses (GC, swap, kernel scheduling) can make this arbitrarily wrong.
3. **Async replication:** If a Redis master grants a lock and then crashes before replicating to its replica, the promoted replica doesn't know about the lock. With N=5 and a crash, you now only need 2 more grants — reducing fault tolerance.

**When Redlock IS acceptable:**
- Efficiency locks: preventing duplicate work (e.g., cache stampede). If two holders temporarily overlap, the only cost is wasted computation.
- NOT for correctness locks: anything where double-execution causes data corruption, financial inconsistency, or hardware damage (bare-metal IPMI).

**Interviewer Q&A:**

**Q1: Antirez (Redis creator) disagreed with Kleppmann. Who is right?**
A: Both are right within their respective contexts. Antirez argued that the timing assumptions are reasonable in practice for most workloads, and that GC pauses exceeding the lock TTL are rare. Kleppmann argued that for correctness (not just best-effort), you cannot rely on timing bounds in an asynchronous system. For infrastructure locks, Kleppmann's position is the right one — we cannot accept even rare safety violations when controlling bare-metal hardware.

**Q2: Can you add fencing tokens to Redis to fix it?**
A: You can implement a counter in Redis (`INCR`), but the fundamental problem remains: Redis replication is asynchronous. If the master holding the counter crashes, the replica may have a stale counter value. A fencing token is only as strong as the system guaranteeing its monotonicity — and only a consensus-based system (Raft/Paxos) can guarantee that.

**Q3: What about Redis with `WAIT` command for synchronous replication?**
A: `WAIT` blocks until N replicas acknowledge the write, which improves durability. But it doesn't solve the consensus problem: there's no leader election protocol guaranteeing that the promoted replica has the latest state, and there's no protection against split-brain during network partitions. `WAIT` is a durability mechanism, not a consensus mechanism.

**Q4: How does this relate to the CAP theorem?**
A: Redis Sentinel/Cluster is an AP system (prioritizes availability over consistency during partitions). Lock services must be CP (prioritize consistency — never grant a lock to two holders). etcd and ZooKeeper are CP systems by design. During a network partition, the minority side of an etcd cluster cannot perform writes (locks cannot be acquired), which is the correct behavior.

**Q5: In practice, at AWS/GCP, which lock service is used?**
A: AWS uses DynamoDB-based locking (with conditional writes providing linearizability within a partition) and an internal Paxos-based lock service. GCP uses Chubby (the system described in the original Google paper, based on Paxos). Both are consensus-based. Neither relies on timing for correctness.

**Q6: If a team insists on using Redis for locks, what's the minimum safe approach?**
A: Use a single Redis master (no Redlock) with `SET key value NX PX ttl`. Accept that master failure = brief lock unavailability. Implement fencing tokens separately in a linearizable store. Use short TTLs (< 5s). Add application-level idempotency to all operations protected by the lock. Document that this provides "best-effort" mutual exclusion, not guaranteed.

---

### Deep Dive 4: Fencing Tokens

**Why it's hard:**
Even with a perfect lock service, a client that acquires a lock can be paused (GC, swap, kernel preemption) and continue operating after the lock has expired and been granted to another client. Fencing tokens are the mechanism that prevents this from causing harm, but they require cooperation from the protected resource.

**Implementation:**

```python
class FencingTokenValidator:
    """
    Embedded in every protected resource (storage API, database proxy,
    IPMI controller, etc.).
    """

    def __init__(self):
        # Per-resource last-seen fence token
        self.last_token = {}  # resource_key -> highest fence_token seen

    def validate_and_execute(self, resource_key, fence_token, operation):
        """
        Reject operations with stale fence tokens.
        This is the CRITICAL safety mechanism.
        """
        current_max = self.last_token.get(resource_key, 0)

        if fence_token < current_max:
            raise StaleFenceTokenError(
                f"Fence token {fence_token} < last seen {current_max}. "
                f"Another lock holder has been granted access."
            )

        # Update high-water mark
        self.last_token[resource_key] = fence_token

        # Execute the protected operation
        return operation.execute()


class ProtectedStorageAPI:
    """
    Example: bare-metal provisioning API that accepts fencing tokens.
    """

    def __init__(self):
        self.fencing = FencingTokenValidator()

    def configure_server(self, server_id, config, fence_token):
        """
        Every mutation requires a fence token from the lock service.
        """
        return self.fencing.validate_and_execute(
            resource_key=f"server:{server_id}",
            fence_token=fence_token,
            operation=ConfigureServerOp(server_id, config)
        )
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Protected resource doesn't check fence token | Safety violation possible | Make fence-token-checking a mandatory middleware; fail open = policy violation |
| Fence token counter wraps around | Stale tokens appear valid | Use 64-bit counter; at 1M locks/sec, wraps in 292,000 years |
| Multiple protected resources with separate token trackers | Token valid on one, stale on another | Use a global fence token (single counter), not per-key. Or use per-resource token tracking. |

**Interviewer Q&A:**

**Q1: Does the protected resource need to persist the last-seen fence token?**
A: Yes, if it can crash and restart. Otherwise, after restart, it would accept stale tokens. Persist `last_token` atomically with the protected operation (e.g., in the same database transaction). Alternatively, store it in the lock service itself and query before each operation.

**Q2: What's the relationship between fencing tokens and optimistic concurrency control?**
A: Fencing tokens are effectively a form of optimistic concurrency control. They're analogous to version numbers / ETags. The key difference is that fencing tokens are issued by the lock service (not the data store), creating a causal ordering that spans the lock service and the protected resource.

**Q3: Can fencing tokens replace locks entirely?**
A: Not quite. Fencing tokens require the protected resource to reject stale operations, but they don't prevent the stale client from attempting the operation. If the operation has side effects before reaching the protected resource (e.g., sending an email), fencing tokens don't help. Locks + fencing tokens together provide defense in depth.

**Q4: How does Google's Chubby handle fencing?**
A: Chubby provides "sequencer" — a byte string containing the lock name, mode (exclusive/shared), and a sequence number. The client passes the sequencer to the protected resource, which validates it with Chubby. This is essentially the same as fencing tokens but with server-side validation.

**Q5: What if the client doesn't send the fence token?**
A: The protected resource should reject requests without fence tokens for lock-protected operations. This is an API design decision — make fence tokens a required parameter, not optional.

**Q6: How do you handle fence tokens across retries?**
A: The client uses the same fence token for all operations within a single lock hold. If the client releases and re-acquires, it gets a new (higher) fence token. Idempotency keys should be used for retry safety; fence tokens are for mutual exclusion.

---

## 7. Scheduling & Resource Management

### Lock Service in Job Scheduling Context

The distributed lock service is foundational to job scheduling infrastructure:

**Scheduler Leader Election:**
```
                ┌─────────────┐
                │  Lock:       │
                │  /scheduler/ │
                │  leader      │
                └──────┬──────┘
                       │
         ┌─────────────┼─────────────┐
         │             │             │
    Scheduler-1   Scheduler-2   Scheduler-3
    (LEADER)      (STANDBY)     (STANDBY)
    Holds lock    Watching       Watching
```

- Only the lock holder is the active scheduler.
- Standby instances watch the lock; on release (leader crash), one acquires and becomes leader.
- Fence token ensures that old leader's in-flight scheduling decisions are rejected.

**Resource Reservation Locks:**

```python
def reserve_bare_metal_server(server_id, job_id, scheduler_session):
    """
    Lock ensures only one scheduler reserves a given server.
    Critical for bare-metal where double-allocation = hardware conflict.
    """
    lock_key = f"/resources/bm/{server_id}"

    result = lock_service.acquire(
        lock_key,
        session_id=scheduler_session,
        lock_type=LockType.EXCLUSIVE,
        timeout_ms=5000
    )

    if not result.granted:
        raise ResourceContendedError(server_id)

    try:
        # Check server is still available (inside lock)
        server = inventory_db.get(server_id)
        if server.state != "AVAILABLE":
            raise ResourceUnavailableError(server_id)

        # Reserve with fence token
        inventory_db.reserve(
            server_id,
            job_id=job_id,
            fence_token=result.fence_token
        )
    finally:
        lock_service.release(lock_key, scheduler_session,
                              result.fence_token)
```

**Lock Partitioning for Scheduler Scaling:**

| Partition Strategy | Description | Trade-off |
|-------------------|-------------|-----------|
| Per-resource locks | One lock per bare-metal server | High concurrency but many locks |
| Per-rack locks | One lock per rack (16-48 servers) | Lower overhead but coarser granularity |
| Per-AZ locks | One lock per availability zone | Simple but limits parallelism |
| Consistent-hash partitioned | Scheduler instances own key ranges | Best balance; requires rebalancing on scheduler scale |

---

## 8. Scaling Strategy

### Scaling Dimensions

| Dimension | Strategy | Detail |
|-----------|----------|--------|
| Throughput | Multi-Raft (lock key sharding) | Partition lock namespace across N independent Raft groups; each handles a slice of keys |
| Read latency | Follower reads with read-index | Followers serve linearizable reads by confirming commit index with leader |
| Lock count | Horizontal state partitioning | Each Raft group manages ~10K locks; add groups as lock count grows |
| Client count | Connection pooling + gRPC multiplexing | Each client maintains 1 gRPC connection; multiplexes lock RPCs |
| Cross-region | Independent clusters + global lock proxy | No cross-region Raft (latency too high); proxy routes to regional cluster |

### Multi-Raft Architecture

```
Lock Key Space: [0, 2^64)
    │
    ├── Raft Group 1: [0, 2^62)           ── Nodes {A1, A2, A3}
    ├── Raft Group 2: [2^62, 2^63)        ── Nodes {B1, B2, B3}
    ├── Raft Group 3: [2^63, 3*2^62)      ── Nodes {C1, C2, C3}
    └── Raft Group 4: [3*2^62, 2^64)      ── Nodes {D1, D2, D3}

Client SDK routes lock key to correct Raft group via consistent hash.
```

**Interviewer Q&A:**

**Q1: At what scale does a single Raft group become a bottleneck?**
A: A single etcd cluster (one Raft group) handles ~10K-30K writes/sec depending on hardware (SSD, 10GbE). If your lock service needs 100K ops/sec, you need 4-10 Raft groups. The bottleneck is the leader's disk fsync (WAL write) and the Raft round-trip latency.

**Q2: How do you handle a hot lock (one lock accessed by thousands of clients)?**
A: (1) Redesign: is the lock too coarse? Can you partition the protected resource and use per-partition locks? (2) If the lock must be singular (e.g., global leader election), the Raft group handling it should be dedicated and not shared with other locks. (3) Use read-write locks: if most accesses are reads, shared locks allow concurrency.

**Q3: How does etcd handle scaling in production Kubernetes clusters?**
A: A single etcd cluster (3 or 5 nodes) serves the entire Kubernetes cluster. Kubernetes limits cluster size partly because of etcd's single-Raft-group scalability. At ~5,000 nodes, etcd becomes a bottleneck. Large clusters use etcd federation or multiple API servers with separate etcd clusters for events vs core resources.

**Q4: Can you use a lock service across multiple regions?**
A: Not with a single Raft group — cross-region RTT (50-200ms) makes Raft commits take 100-400ms, which is unacceptable for lock acquisition. Instead, deploy independent lock clusters per region. For global locks (rare), use a dedicated cross-region Raft group with the understanding that lock acquisition takes 200ms+.

**Q5: How does lock partitioning interact with deadlock detection?**
A: With multi-Raft partitioning, a global wait-for graph requires cross-group coordination. This is complex. Prefer timeout-based deadlock prevention (every acquire has a deadline). If you must detect deadlocks, maintain a centralized deadlock detector that aggregates wait-for edges from all Raft groups.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO |
|---|---------|-----------|--------|----------|-----|
| 1 | Single node crash | Raft heartbeat timeout (150-300ms) | Cluster continues if quorum intact | Node restarts, catches up from Raft log / snapshot | 0 (no downtime) |
| 2 | Leader crash | Followers detect missed heartbeats | New election in 300ms-2s | New leader elected; clients reconnect | 1-3s |
| 3 | Network partition (minority isolated) | Cannot reach quorum | Minority side read-only; clients on minority side cannot acquire locks | Partition heals; minority catches up | Duration of partition |
| 4 | Network partition (leader isolated) | Leader loses quorum | Leader steps down; new election on majority side | Old leader's clients reconnect to new leader | 2-5s |
| 5 | Disk failure on one node | I/O errors during WAL write | Node stops accepting writes | Replace disk; node recovers from peer snapshot | 10-30 min (manual) |
| 6 | All nodes crash simultaneously | Monitoring alerts | Total outage | Recover from persistent WAL + snapshots | 5-15 min |
| 7 | Byzantine failure (corrupted node) | Checksum mismatches | Raft log diverges | Remove corrupted node; add fresh node; rebuild from peers | 30 min |
| 8 | Client GC pause > lock TTL | Lock expires; session lost | Client loses locks; may cause spurious lock release | Client re-acquires; fencing token prevents stale writes | Session TTL |
| 9 | Slow disk (high fsync latency) | Raft commit latency spikes | Lock acquisition latency increases | Identify and replace slow disk; move leader to faster node | Minutes |

### Consensus & Coordination

**Raft Protocol in Lock Service Context:**

```
Leader Election Timeline:
─────────────────────────────────────────────────────────

Term 4:  Leader=Node1    ████████████████████████████████
         Followers:      Node2 ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
                         Node3 ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓

         [Node1 crashes]
                              │
Term 5:  Election:            │  Node2 requests votes ──►
                              │  Node3 grants vote   ──►
                              │  Node2 becomes leader
         Leader=Node2         │  ████████████████████████
         Follower:            │  Node3 ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓
         Down:                │  Node1 xxxxxxxxxxxxxxxxxx

Raft Properties That Matter for Locks:
┌─────────────────────────────────────────────────────┐
│ 1. Election Safety: at most one leader per term     │
│    → No two nodes think they're leader              │
│    → No split-brain lock grants                     │
│                                                     │
│ 2. Leader Completeness: committed entries survive    │
│    leader changes                                   │
│    → If lock grant was committed, new leader has it │
│                                                     │
│ 3. State Machine Safety: all nodes apply same log   │
│    → All nodes agree on who holds each lock         │
│                                                     │
│ 4. Linearizability: reads and writes appear to      │
│    occur at a single point in time                  │
│    → Implemented via Raft read-index protocol       │
└─────────────────────────────────────────────────────┘
```

**Raft Election Timeout Tuning:**

```
election_timeout = random(150ms, 300ms)   # Randomized to prevent split vote
heartbeat_interval = 50ms                 # Leader sends every 50ms
disk_fsync_latency = ~1ms (NVMe SSD)

Rule: heartbeat_interval << election_timeout << MTBF_of_nodes

Too low election_timeout:  Spurious elections during network jitter
Too high election_timeout: Slow leader failover → lock acquisition stalled
```

---

## 10. Observability

### Key Metrics

| # | Metric | Type | Alert Threshold | Why |
|---|--------|------|-----------------|-----|
| 1 | `lock.acquire.latency.p99` | Histogram | > 100ms | Lock contention or Raft issues |
| 2 | `lock.acquire.timeout_rate` | Counter | > 1% of requests | High contention or undersized cluster |
| 3 | `lock.active_count` | Gauge | > 50K | Approaching state machine size limits |
| 4 | `session.active_count` | Gauge | > 10K | High client count; check keep-alive load |
| 5 | `session.expiry_rate` | Counter | > 10/min | Clients crashing or network issues |
| 6 | `raft.commit.latency.p99` | Histogram | > 50ms | Disk or network bottleneck |
| 7 | `raft.leader.changes` | Counter | > 3/hour | Instability; check network, disk |
| 8 | `raft.proposal.failed_rate` | Counter | > 0.1% | Raft proposal failures; leader overloaded |
| 9 | `lock.waiters.count` | Gauge per key | > 100 | Hot lock; redesign needed |
| 10 | `lock.hold_duration.p99` | Histogram | > 30s | Clients holding locks too long |
| 11 | `fence_token.rejections` | Counter | > 0 | Stale clients writing to protected resources |
| 12 | `raft.snapshot.duration` | Histogram | > 30s | Snapshot too large; compact more often |
| 13 | `grpc.connection.active` | Gauge | > 5K | Connection pooling needed |
| 14 | `disk.wal.fsync.latency` | Histogram | > 10ms | Disk degradation |

### Dashboards

```
Lock Service Health Dashboard
├── Lock Operations
│   ├── Acquire rate (ops/sec) by result (granted, queued, timeout, error)
│   ├── Release rate (ops/sec)
│   ├── Acquire latency heatmap (p50, p90, p99)
│   └── Top 10 most-contended locks
├── Sessions
│   ├── Active session count over time
│   ├── Session creation/expiry rate
│   └── Keep-alive success rate
├── Raft Health
│   ├── Current leader (node ID)
│   ├── Term number
│   ├── Commit index lag (leader vs followers)
│   ├── WAL fsync latency
│   └── Snapshot frequency and duration
└── Resources
    ├── CPU / Memory / Disk per node
    ├── Network bytes in/out per node
    └── gRPC connection count
```

---

## 11. Security

### Threat Model

| Threat | Mitigation |
|--------|------------|
| Unauthorized lock acquisition | mTLS authentication + RBAC (e.g., etcd auth) |
| Lock key enumeration | Prefix-based ACLs (client X can only access `/locks/team-x/*`) |
| Denial of service (create millions of locks) | Per-client lock count limits; session creation rate limits |
| Man-in-the-middle | TLS for all client-server and peer-peer communication |
| Replay attack (replay old acquire request) | Request IDs + server-side deduplication; session binding |
| Admin abuse (force-release production locks) | Audit log for all admin operations; require MFA for force-release |
| Rogue node joins cluster | Peer certificate validation; cluster bootstrap requires pre-shared token |

### Authentication & Authorization

```
┌─────────────────────────────────────────────────────┐
│ RBAC Policy Example (etcd-style)                    │
├─────────────────────────────────────────────────────┤
│ Role: bare-metal-provisioner                        │
│   Permissions:                                      │
│     /locks/bm/*        : READ, WRITE               │
│     /locks/scheduler/* : READ only                  │
│     /locks/network/*   : DENY                       │
│                                                     │
│ Role: scheduler                                     │
│   Permissions:                                      │
│     /locks/scheduler/* : READ, WRITE                │
│     /locks/bm/*        : READ, WRITE               │
│                                                     │
│ Role: admin                                         │
│   Permissions:                                      │
│     /locks/**          : READ, WRITE, FORCE_RELEASE │
│     /sessions/**       : READ, WRITE, FORCE_EXPIRE  │
└─────────────────────────────────────────────────────┘
```

### Audit Logging

Every lock operation is logged with:
- Timestamp, client identity (TLS cert CN), session ID
- Lock key, operation (acquire/release/force-release), result
- Fence token issued
- Source IP, gRPC metadata

---

## 12. Incremental Rollout Strategy

### Phase 1: Shadow Mode (Week 1-2)
- Deploy lock service alongside existing lock mechanism (e.g., MySQL advisory locks).
- Application acquires locks from both old and new systems.
- Compare results; log discrepancies.
- No behavioral change in application.

### Phase 2: New System Primary, Old System Verification (Week 3-4)
- Application uses new lock service for decisions.
- Old system continues to be consulted; alerts on disagreement.
- Canary with 1-2 non-critical services (e.g., log rotation locks).

### Phase 3: Graduated Rollout (Week 5-8)
- 10% of services -> 25% -> 50% -> 100%.
- Priority order: least critical first (test locks, cache locks) -> moderately critical (scheduler partition locks) -> most critical (bare-metal provisioning locks).
- Feature flag per service to enable/disable.

### Phase 4: Decommission Old System (Week 9-12)
- Remove old lock system dependencies.
- Clean up dual-write code.
- Final validation under load.

**Rollout Q&A:**

**Q1: How do you test lock correctness during rollout?**
A: Run a continuous "lock checker" that spawns N clients all trying to acquire the same lock. Each holder writes its identity to a shared file/DB row. After the test, verify no two identities overlap in time. This is a linearizability checker (like Jepsen).

**Q2: What's the rollback plan if the new lock service has a bug?**
A: Feature flag immediately reverts to old system. All in-flight locks on the new system are abandoned (TTL expires). Applications re-acquire on old system. Data integrity is protected by fencing tokens (if resource layer checks them) or idempotency tokens.

**Q3: How do you handle the transition period where some services use old locks and others use new?**
A: During transition, both systems are active. Services using the new system acquire locks there; services still on old use the old. The critical constraint is that no lock is split across systems — all consumers of a given lock key must be on the same system simultaneously. Migrate lock keys atomically (all consumers of key X switch together).

**Q4: How do you validate performance during rollout?**
A: A/B comparison dashboards: lock acquisition latency, session expiry rate, Raft commit latency. Load test with 2x expected traffic before each rollout phase. Alert on p99 latency regression > 20%.

**Q5: What if a Raft cluster needs to be resized during rollout?**
A: etcd supports online member add/remove. Add the new node, wait for it to catch up (watch `raft.commit.index.lag`), then remove the old node. Never change more than one node at a time. Never go below 3 nodes.

---

## 13. Trade-offs & Decision Log

| # | Decision | Options Considered | Selected | Rationale |
|---|----------|-------------------|----------|-----------|
| 1 | Consensus protocol | Raft vs Paxos vs ZAB | Raft | Easier to implement and reason about; etcd/consul use it; equivalent safety guarantees to Paxos |
| 2 | Backend for lock services | ZooKeeper vs etcd vs custom | Custom (etcd-inspired) | ZK is Java (GC pauses); etcd is closest to our needs but we want tighter integration with our platform |
| 3 | Lock fairness | FIFO fair vs unfair (random wakeup) | FIFO fair | Prevents starvation; critical for long-running provisioning jobs that would otherwise never acquire |
| 4 | Session model | TCP-based (ZK) vs lease-based (etcd) | Lease-based | Works through load balancers and proxies; more robust in cloud environments |
| 5 | Fencing tokens | Global counter vs per-key counter | Global counter | Simpler; allows cross-key ordering; 64-bit counter never wraps |
| 6 | Storage engine | BoltDB (bbolt) vs RocksDB vs SQLite | bbolt | B+ tree with MVCC; used by etcd; single-writer matches Raft leader pattern |
| 7 | Read consistency | Stale reads OK vs linearizable reads always | Linearizable by default, option for stale | Lock state queries must be consistent; option for stale reads for monitoring/dashboard |
| 8 | Lock types | Exclusive only vs read-write | Read-write | Many infra use cases have read-heavy access patterns (checking lock state, reading config) |
| 9 | Deadlock handling | Detection vs prevention | Prevention (timeout + ordering) | Detection requires global state; prevention is simpler and sufficient for our workloads |
| 10 | Reentrant locks | Support vs reject | Support (opt-in) | Some services legitimately re-enter locked sections (nested function calls); opt-in avoids accidental misuse |

---

## 14. Agentic AI Integration

### AI-Driven Lock Management

**1. Anomaly Detection for Lock Contention:**
```
AI Agent monitors:
  - Lock hold duration distributions per key
  - Wait queue depth trends
  - Session expiry patterns

Detects:
  - "Lock /locks/bm/rack-7 hold duration increased 10x in last hour"
  - "Session expiry rate spiked 5x — possible network partition"
  - "Wait queue depth for /locks/scheduler/partition-3 growing linearly"

Actions:
  - Alert oncall with root-cause hypothesis
  - Suggest lock key redesign (e.g., split /locks/bm/rack-7 into
    per-server locks)
  - Correlate with infra events (deployment, network change)
```

**2. Predictive Lock TTL Tuning:**
```python
class AdaptiveTTLAgent:
    """
    AI agent that recommends lock TTL based on historical hold patterns.
    """

    def recommend_ttl(self, lock_key):
        # Analyze historical hold durations for this key
        durations = self.metrics.get_hold_durations(lock_key, window="7d")
        p99_duration = percentile(durations, 99)

        # TTL should be > p99 hold duration to avoid false expiry
        # but not so high that crash recovery is slow
        recommended_ttl = p99_duration * 1.5

        # Bound by safety limits
        recommended_ttl = max(5, min(recommended_ttl, 300))

        return {
            "lock_key": lock_key,
            "recommended_ttl": recommended_ttl,
            "rationale": f"p99 hold duration = {p99_duration}s; "
                         f"1.5x safety margin",
            "risk": "LOW" if recommended_ttl < 60 else "MEDIUM"
        }
```

**3. Automated Lock Health Remediation:**

```
Agent Workflow:
1. Detect: Lock service Raft commit latency > 50ms for 5 min
2. Diagnose:
   a. Check disk I/O latency on leader node
   b. Check Raft log size (needs compaction?)
   c. Check network latency between peers
   d. Check if a follower is lagging (slow disk on follower)
3. Act:
   a. If disk slow → trigger leader transfer to healthier node
   b. If log bloated → trigger manual compaction
   c. If follower lagging → alert and prepare replacement node
4. Verify: Commit latency returns to < 10ms
5. Report: Post incident summary to Slack/PagerDuty
```

**4. AI-Assisted Lock Design Review:**
```
When a developer creates a new lock key pattern:
- AI reviews the lock usage code for common mistakes:
  - Missing try/finally (lock not released on exception)
  - Holding lock across I/O operations (long hold time)
  - Missing fence token in protected resource calls
  - Lock key too coarse (locking entire table vs row)
- AI suggests improvements and creates a review comment
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through what happens when a lock holder's network is partitioned.**
A: The holder's keep-alive RPCs fail. After TTL expires (e.g., 15 seconds), the leader proposes session expiration through Raft. All locks held by that session are released, and waiters are granted. The partitioned client detects the failure when its keep-alive returns an error. After partition heals, the client must create a new session and re-acquire locks. Any in-flight operations from the old session are rejected by fencing tokens.

**Q2: Compare ZooKeeper and etcd for distributed locking.**
A: ZooKeeper uses ZAB (similar to Raft) for consensus. Locks use ephemeral sequential znodes — each client creates a node, watches its predecessor. etcd uses Raft with lease-based locks — clients create keys with leases and use create-revision for ordering. Key differences: ZK is Java (GC pauses are a real problem at scale); etcd is Go (lower tail latency). ZK has a richer primitive set (recipes); etcd has a simpler API but requires client-side logic. ZK sessions are TCP-based; etcd leases are RPC-based (better for load-balanced environments).

**Q3: How would you implement a read-write lock?**
A: State machine maintains a list of holders with their lock type. Shared (read) grant: allowed if no exclusive holder and no queued exclusive waiter (to prevent writer starvation). Exclusive (write) grant: allowed only if no holders at all. On release, process wait queue: if next waiter is exclusive, grant only it; if next is shared, grant all consecutive shared waiters until an exclusive waiter is found.

**Q4: What is the thundering herd problem in lock services?**
A: When a lock is released, all waiters are notified simultaneously. They all try to acquire, but only one succeeds; the rest retry. This wastes resources and causes latency spikes. Solution: FIFO queue — only the next waiter is notified (or in ZK, only the next sequential znode watches its predecessor). This converts O(N) notifications to O(1).

**Q5: How do you handle clock skew in a distributed lock service?**
A: The lock service should not depend on synchronized clocks for correctness. All time-dependent operations (TTL expiry) are decided by the Raft leader based on its local monotonic clock, and the decision is replicated through Raft. Clients and servers may have different wall clocks; this doesn't affect safety because the leader's expiration decision is the authoritative one.

**Q6: Can you build a distributed semaphore on top of this lock service?**
A: Yes. A semaphore with capacity N allows up to N holders. Implementation: the lock state machine tracks a counter instead of a boolean. Acquire increments; release decrements. Grant if counter < N. This generalizes exclusive lock (N=1) to counting semaphore (N>1). Useful for rate limiting concurrent bare-metal provisions per rack.

**Q7: What is the CAP theorem implication for our lock service?**
A: Our lock service is a CP system. During a network partition, the minority partition cannot perform writes (no lock acquisitions or releases). This is correct for a lock service — we prefer unavailability over inconsistency (two holders). The majority partition continues operating normally. When the partition heals, the minority nodes catch up from the Raft log.

**Q8: How does this relate to Kubernetes lease objects?**
A: Kubernetes uses Lease objects (in `coordination.k8s.io/v1`) for leader election of controllers (e.g., kube-controller-manager, kube-scheduler). A Lease stores: holderIdentity, leaseDurationSeconds, acquireTime, renewTime. The holder periodically updates the Lease. If the update stops, another controller acquires it. Under the hood, this relies on etcd's linearizable writes for correctness.

**Q9: What is the split-brain scenario and how do you prevent it?**
A: Split-brain occurs when a network partition causes two nodes to both believe they are the leader, potentially granting the same lock to two different clients. Raft prevents this: a leader requires a quorum (majority) to commit entries. With 3 nodes, a quorum is 2. A partition creates a majority side (2 nodes) and a minority side (1 node). Only the majority side can elect a leader and commit. The old leader on the minority side cannot commit (cannot reach quorum) and steps down.

**Q10: How do you test a distributed lock service for correctness?**
A: (1) Jepsen testing: inject network partitions, process pauses, clock skew, disk failures; verify linearizability of lock operations. (2) TLA+ model checking: formally specify the lock protocol and verify safety properties (mutual exclusion, no deadlock). (3) Linearizability checker: record all lock operations with timestamps; verify that the history is consistent with some sequential execution. (4) Chaos testing: kill nodes, drop packets, fill disks in production-like environment.

**Q11: What's the performance difference between ZooKeeper, etcd, and Redis for locks?**
A: Redis: ~100K ops/sec, p99 < 1ms (single node). But not linearizable. etcd: ~10-30K writes/sec, p99 < 10ms (3-node cluster). Linearizable. ZooKeeper: ~10-20K writes/sec, p99 < 15ms (3-node cluster). Linearizable but Java GC can cause p99.9 spikes to 100ms+. For infrastructure locks where correctness > performance, etcd or ZK is correct; Redis is only suitable for best-effort locks.

**Q12: How do you handle lock service upgrades without downtime?**
A: Rolling upgrade: update one node at a time. The Raft cluster tolerates one node being down. (1) Stop follower node. (2) Upgrade binary. (3) Start and verify it catches up. (4) Repeat for other follower. (5) Transfer leadership from old-binary leader to upgraded follower. (6) Upgrade old leader. Total unavailability: 0 (quorum maintained throughout).

**Q13: What is the "lock with intent" pattern?**
A: Used in database systems (SQL Server's IX, IS locks). A coarse-grained intent lock signals that a finer-grained lock will be taken inside. For infra: acquiring an intent lock on a rack signals that a specific server in that rack will be locked next. This prevents conflicting rack-level operations (e.g., rack power-cycle) from proceeding while server-level operations are in progress.

**Q14: How do you debug a lock that appears to be stuck?**
A: (1) `lockctl lock info /locks/stuck-key` — check holder identity, session, fence token. (2) Check if the holder's session is still alive (`lockctl session info <id>`). (3) If alive, the holder is slow, not crashed — check the holder's application logs. (4) If the session is alive but the application is hung, admin can force-expire the session (`lockctl session force-expire <id>` — requires admin role). (5) Check for deadlock: are there circular wait dependencies?

**Q15: Design a lock service that works across multiple data centers.**
A: Option 1: Global Raft group spanning DCs. Simple but high latency (100-200ms per lock op). Suitable for rarely-acquired global locks (e.g., global config change lock). Option 2: Per-DC lock clusters with a global coordinator. Each DC has local locks; cross-DC operations use a global lock acquired from the coordinator. Option 3: CRDTs for conflict-free operations where possible; locks only for true conflicts. For our infra role, Option 2 is most practical: bare-metal resources are DC-local, so locks are DC-local.

**Q16: What happens if the fencing token counter overflows?**
A: With a 64-bit unsigned integer, at 1 million lock grants per second, overflow occurs in 2^64 / 10^6 / 86400 / 365 = ~584,942 years. This is not a practical concern. If using a 32-bit counter (don't), overflow at 1M/sec happens in ~72 minutes — use 64-bit.

**Q17: How would you implement a distributed barrier (all N participants must arrive before any proceed)?**
A: Using the lock service: create N lock keys under a barrier prefix (e.g., `/barriers/job-123/participant-{1..N}`). Each participant creates its key. A watch on the prefix waits until all N keys exist. Alternatively, use a counter: each participant increments a Raft-replicated counter; when counter = N, all are released. This is useful for synchronized bare-metal fleet operations (e.g., firmware update all servers in a rack simultaneously).

---

## 16. References

1. Lamport, L. (1998). *The Part-Time Parliament* (Paxos). ACM TOCS.
2. Ongaro, D. & Ousterhout, J. (2014). *In Search of an Understandable Consensus Algorithm* (Raft). USENIX ATC.
3. Burrows, M. (2006). *The Chubby Lock Service for Loosely-Coupled Distributed Systems*. OSDI.
4. Hunt, P. et al. (2010). *ZooKeeper: Wait-free Coordination for Internet-scale Systems*. USENIX ATC.
5. Kleppmann, M. (2016). *How to do distributed locking*. https://martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html
6. Antirez (Sanfilippo, S.) (2016). *Is Redlock safe?* http://antirez.com/news/101
7. etcd documentation. https://etcd.io/docs/
8. Kingsbury, K. (Jepsen). *etcd analysis*. https://jepsen.io/analyses/etcd-3.4.3
9. Kleppmann, M. (2017). *Designing Data-Intensive Applications*. O'Reilly. Chapters 8-9.
10. Gray, J. & Lamport, L. (2006). *Consensus on Transaction Commit*. ACM TODS.
