# System Design: Distributed Cache

---

## 1. Requirement Clarifications

### Functional Requirements
- Store and retrieve key-value pairs with sub-millisecond read latency
- Support TTL (time-to-live) expiration per key
- Support common data structures: strings, hashes, lists, sets, sorted sets
- Evict keys when memory pressure is reached (configurable eviction policy)
- Replication: primary/replica topology for read scaling and HA
- Pub/Sub messaging: publish messages to channels; subscribers receive in real time
- Persistence options: snapshotting (RDB) and append-only log (AOF)
- Cluster mode: automatic data sharding across nodes using hash slots
- Atomic operations and Lua scripting for multi-key transactions
- Key introspection: TTL query, key scan, type inspection

### Non-Functional Requirements
- P99 read latency < 1 ms; P99 write latency < 2 ms
- Availability: 99.99% (< 52 min downtime/year)
- Durability: configurable — from in-memory-only to fsync-every-write
- Throughput: sustain 1 million operations/second across the cluster
- Linear horizontal scalability by adding shard nodes
- Cluster can tolerate loss of minority of nodes without data loss (with replication factor >= 2)
- Partition tolerance: continue serving cached reads during network split (AP for cache tier)
- Operational: rolling upgrades, live resharding without downtime

### Out of Scope
- Full SQL query processing
- Strong ACID multi-key transactions spanning multiple shards (no distributed 2PC)
- Disk-first storage (this is a memory-first system)
- Global geo-replication with conflict resolution (active-active multi-region)
- Client-side caching invalidation protocol details

---

## 2. Users & Scale

### User Types
| Actor | Behavior |
|---|---|
| Application service | Read/write hot data (session tokens, rate-limit counters, leaderboards) |
| Background worker | Read/write job queues, deduplication sets |
| Analytics service | Read counters and sorted sets for dashboards |
| Admin/ops tooling | Monitor memory usage, eviction rate, replication lag |

### Traffic Estimates (calculations shown)

**Assumptions:**
- 500 M active users/day
- Average 20 cache operations per user per day (reads + writes, typical web app: session check, feed cache, rate limit)
- Read:Write ratio = 80:20

Total ops/day = 500 M × 20 = 10 B ops/day  
Peak-to-average ratio = 3×  
Average ops/sec = 10 B / 86,400 ≈ 115,740 ops/sec  
Peak ops/sec = 115,740 × 3 ≈ **350,000 ops/sec**

At cluster level, target headroom at 50% utilization → provision for **700,000 ops/sec** cluster capacity.

Each node (e.g., r6g.xlarge: 32 GB RAM, 4 vCPUs) handles ~100,000–150,000 ops/sec in Redis benchmark.  
Nodes required for throughput = 700,000 / 125,000 ≈ **6 primary nodes** (rounded to 8 for headroom + even hash slot distribution).

### Latency Requirements
| Operation | P50 | P99 | P999 |
|---|---|---|---|
| GET (single key) | 0.1 ms | 0.8 ms | 2 ms |
| SET (single key) | 0.1 ms | 1.5 ms | 3 ms |
| ZADD / ZRANGE (sorted set) | 0.2 ms | 1.5 ms | 4 ms |
| Lua script (multi-op) | 0.3 ms | 2 ms | 5 ms |
| Pub/Sub publish | 0.1 ms | 1 ms | 3 ms |

### Storage Estimates

**Assumptions:**
- Hot dataset = 10% of 500 M users × avg 2 KB of cached data per user
- 500 M × 0.10 × 2 KB = 100 M × 2 KB = **200 GB hot data**
- Overhead for Redis internal structures ≈ 30% → 260 GB usable memory needed
- Each node: 32 GB RAM → 260 GB / 32 GB ≈ 9 nodes (use 8 primaries with 32 GB = 256 GB, accept ~98% of dataset fits; or upsize to r6g.2xlarge with 64 GB → 4 primaries cover 256 GB)

| Component | Raw Data | With Overhead | Nodes (32 GB each) |
|---|---|---|---|
| Session keys (avg 500 B) | 50 GB | 65 GB | 3 |
| Feed/content cache (avg 5 KB) | 80 GB | 104 GB | 4 |
| Rate-limit counters (avg 64 B) | 10 GB | 13 GB | 1 |
| Leaderboards / sorted sets | 20 GB | 26 GB | 1 |
| **Total** | **160 GB** | **208 GB** | **8 primaries** |

With replication factor 1 (one replica per primary), total cluster RAM = 8 × 2 × 32 GB = **512 GB across 16 nodes**.

### Bandwidth Estimates

Average value size = 1 KB  
Peak read throughput = 280,000 reads/sec × 1 KB = **274 MB/sec read**  
Peak write throughput = 70,000 writes/sec × 1 KB = **68 MB/sec write**  
Replication bandwidth (primary → replica, same writes) = 68 MB/sec per replica  
Total egress per primary node = (274 + 68) / 8 ≈ **43 MB/sec per node** — well within 10 Gbps NIC (1,250 MB/sec).

---

## 3. High-Level Architecture

```
                         ┌─────────────────────────────────────┐
                         │           Client Applications        │
                         │  (App Servers, Workers, Services)    │
                         └──────────┬──────────────────────────┘
                                    │  Smart Client (cluster-aware)
                                    │  or Proxy (e.g., Twemproxy/Envoy)
                         ┌──────────▼──────────────────────────┐
                         │         Cluster Router / Proxy       │
                         │  - Reads CLUSTER SLOTS map           │
                         │  - Routes to correct shard           │
                         │  - Handles MOVED/ASK redirects       │
                         └──────┬──────────┬──────────┬────────┘
                                │          │          │
              ┌─────────────────▼──┐  ┌────▼────┐  ┌─▼───────────────┐
              │  Shard 1 (Primary) │  │ Shard 2 │  │  Shard N        │
              │  Slots: 0–2730     │  │ Slots:  │  │  Slots: ...     │
              │  ┌──────────────┐  │  │2731–5460│  │                 │
              │  │ In-Memory    │  │  └────┬────┘  └──┬──────────────┘
              │  │ Hash Table   │  │       │           │
              │  │ + Eviction   │  │  [Replica 2]  [Replica N]
              │  │   Manager    │  │
              │  └──────────────┘  │
              │  ┌──────────────┐  │
              │  │ AOF / RDB    │  │
              │  │ Persistence  │  │
              │  └──────────────┘  │
              └────────┬───────────┘
                       │ Async replication
              ┌────────▼───────────┐
              │  Shard 1 Replica   │
              │  (Serves reads,    │
              │   failover target) │
              └────────────────────┘

              ┌─────────────────────────────────┐
              │       Cluster Bus (gossip)       │
              │  Port: primary_port + 10000      │
              │  - Heartbeat / failure detection │
              │  - Slot assignment propagation   │
              │  - PFAIL / FAIL voting           │
              └─────────────────────────────────┘

              ┌──────────────────┐    ┌──────────────────┐
              │  Sentinel Node 1 │    │  Sentinel Node 2 │
              │  (Used in        │    │  (Quorum-based   │
              │  non-cluster HA) │    │  failover mgmt)  │
              └──────────────────┘    └──────────────────┘

              ┌────────────────────────────────────────────┐
              │        Monitoring & Ops Layer               │
              │  - Prometheus exporters (redis_exporter)    │
              │  - Grafana dashboards                       │
              │  - PagerDuty alerts on eviction rate spike  │
              └────────────────────────────────────────────┘
```

**Component Roles:**
- **Shard (Primary node):** Owns a subset of 16,384 hash slots. Handles all writes and optionally reads for its slot range. Maintains in-memory data structures and persistence.
- **Replica node:** Async copy of primary. Handles read-only queries when `replica-read-only yes`. Promoted to primary by cluster vote on primary failure.
- **Cluster Bus:** Each node runs a gossip protocol on `port+10000`. Propagates node state, hash slot assignments, and failure detection messages.
- **Cluster Router / Smart Client:** Maintains a local copy of the slot map. Routes commands directly to the responsible shard. Handles `MOVED` (slot migrated) and `ASK` (migration in progress) errors by re-issuing the command to the correct node.
- **Sentinel (non-cluster mode):** Monitors a standalone primary/replica group. Issues `FAILOVER` when quorum of sentinels marks primary as `ODOWN`. Used for smaller deployments not using cluster mode.
- **AOF/RDB Persistence:** Runs within each primary process. RDB = point-in-time snapshot (fork + copy-on-write). AOF = append-only log of write commands.

**Primary Use-Case Data Flow (Cache Read):**
1. Client hashes key: `slot = CRC16(key) % 16384`
2. Client looks up slot in local slot map → gets primary node address
3. Client sends `GET key` to that node over TCP
4. Node checks memory hash table → cache hit returns value immediately
5. On cache miss: client falls back to origin DB; on successful fetch, issues `SET key value EX <ttl>` to cache
6. Node updates LRU/LFU clock on access

---

## 4. Data Model

### Entities & Schema

**Key Naming Convention:** `<namespace>:<entity>:<id>[:<field>]`  
Examples: `session:user:uid_12345`, `ratelimit:api:uid_12345:2024010115`, `leaderboard:game:weekly`

```
Key-Value Entry (internal representation per shard):
┌───────────────────────────────────────────────────────────────┐
│  key        : string (up to 512 MB, practical limit ~1 KB)   │
│  value      : one of: String, List, Hash, Set, ZSet, Stream   │
│  encoding   : int / embstr / raw / ziplist / listpack /       │
│               quicklist / skiplist / hashtable / intset       │
│  lru_clock  : 24-bit LRU clock (seconds granularity)         │
│  refcount   : integer (shared object optimization)           │
│  expire_at  : Unix timestamp in ms (0 = no expiry)           │
│  type_flags : 4-bit type identifier                          │
└───────────────────────────────────────────────────────────────┘

Session Cache Entry (stored as Hash):
  HSET session:user:12345
       token       "eyJhbGci..."
       created_at  "1704067200"
       ip          "203.0.113.42"
       user_agent  "Mozilla/5.0..."
  EXPIRE session:user:12345 86400   -- 24h TTL

Rate Limit Counter (stored as String):
  SET ratelimit:api:uid_12345:hour_bucket "47" EX 3600

Leaderboard (stored as Sorted Set):
  ZADD leaderboard:game:weekly <score> <user_id>
  ZRANGE leaderboard:game:weekly 0 9 WITHSCORES REV  -- top 10

Job Deduplication (stored as Set):
  SADD dedup:jobs:batch_20240101 "job_id_abc"
  EXPIRE dedup:jobs:batch_20240101 86400
```

**Cluster Slot Assignment Table (excerpt):**
```
Node           | Slots         | Primary IP      | Replica IP
---------------|---------------|-----------------|------------------
node-1         | 0–2047        | 10.0.1.1:6379   | 10.0.1.2:6379
node-2         | 2048–4095     | 10.0.1.3:6379   | 10.0.1.4:6379
...            | ...           | ...             | ...
node-8         | 14336–16383   | 10.0.1.15:6379  | 10.0.1.16:6379
```

### Database Choice

| Option | Pros | Cons | Fit |
|---|---|---|---|
| **Redis (OSS / Redis Stack)** | Sub-ms latency, rich data structures, mature cluster mode, Lua scripting, pub/sub, active community | Single-threaded command processing (I/O-threaded in v6+), memory-only primary tier, limited query capability | **Selected** |
| Memcached | Extremely simple, multi-threaded, low overhead | No persistence, no rich types, no replication, no cluster (only consistent hashing at client), no pub/sub | Not selected |
| Apache Ignite | In-memory + disk, SQL support, distributed transactions | High operational complexity, JVM overhead, higher latency tail | Not selected |
| Hazelcast | Near-cache, distributed data structures, IMDG | JVM heap GC pressure, complex licensing, less ecosystem than Redis | Not selected |
| DragonflyDB | Multi-threaded Redis-compatible, higher throughput on single node | Younger project, cluster mode less battle-tested, fewer managed offerings | Future consideration |

**Selected: Redis Cluster (OSS)**  
Justification: Redis is the de-facto standard for in-memory caching at scale. Its single-threaded event loop (with I/O thread offloading since v6) guarantees command atomicity without locking overhead. The built-in cluster mode with 16,384 hash slots provides deterministic sharding, live resharding via `CLUSTER SETSLOT`, and autonomous failover via gossip-based leader election — all without an external coordinator. The rich data type system (sorted sets, streams, HyperLogLog) eliminates the need for separate specialized stores for most use cases.

---

## 5. API Design

All operations use the Redis Serialization Protocol (RESP3) over TCP. For HTTP-facing services, a REST adapter is shown.

**Authentication:** Redis 6+ AUTH with ACL system. ACL rules per user: `ACL SETUSER appuser on >password ~session:* &* +@read +@write +@string`  
**Rate Limiting:** Enforced at the proxy layer (e.g., Envoy) — 10,000 RPS per client IP; at cluster level, backpressure via `maxmemory-policy` and connection limits (`maxclients 65536`).

```
-- Core Key-Value Operations --

GET <key>
  Returns: bulk string or nil
  Complexity: O(1)
  Auth: read ACL

SET <key> <value> [EX seconds | PX milliseconds] [NX | XX] [GET]
  Returns: OK | nil (if NX/XX condition not met) | old value (if GET)
  Complexity: O(1)
  Auth: write ACL
  Notes: NX = set only if not exists (used for distributed locking)
         XX = set only if exists

DEL <key> [key ...]
  Returns: integer count of deleted keys
  Complexity: O(N) N=number of keys
  Auth: write ACL

EXPIRE <key> <seconds>
  Returns: 1 if set, 0 if key does not exist
  Complexity: O(1)

TTL <key>
  Returns: remaining TTL in seconds, -1 if no expiry, -2 if key missing
  Complexity: O(1)

-- Hash Operations --

HSET <key> <field> <value> [field value ...]
HGET <key> <field>
HMGET <key> <field> [field ...]
HGETALL <key>
HDEL <key> <field> [field ...]

-- Sorted Set Operations --

ZADD <key> [NX|GT|LT] <score> <member> [score member ...]
  Complexity: O(log N) per element
ZRANGE <key> <start> <stop> [BYSCORE|BYLEX] [REV] [LIMIT offset count] [WITHSCORES]
  Complexity: O(log N + M) M=elements returned
ZRANK <key> <member> [WITHSCORE]
ZREM <key> <member> [member ...]

-- Pub/Sub --

PUBLISH <channel> <message>
  Returns: integer (number of subscribers that received)
  Complexity: O(N+M) N=subscribing clients, M=subscription patterns
  Auth: pubsub ACL category

SUBSCRIBE <channel> [channel ...]
PSUBSCRIBE <pattern> [pattern ...]
UNSUBSCRIBE [channel ...]

-- Atomic / Scripting --

INCR <key>          -- atomic increment, used for counters, rate limits
INCRBY <key> <amount>
GETSET <key> <value>  -- atomic get-then-set (deprecated; use SET ... GET)

EVAL <script> <numkeys> [key ...] [arg ...]
  Example: Rate-limit check-and-increment in one round trip:
  EVAL "
    local current = redis.call('INCR', KEYS[1])
    if current == 1 then
      redis.call('EXPIRE', KEYS[1], ARGV[1])
    end
    return current
  " 1 ratelimit:uid:12345 3600

-- Cluster Management --

CLUSTER INFO          -- cluster state, slots assigned, known nodes
CLUSTER NODES         -- full node list with slot ranges
CLUSTER SLOTS         -- slot-to-node mapping
CLUSTER KEYSLOT <key> -- which slot a key belongs to
CLUSTER SETSLOT <slot> MIGRATING|IMPORTING|NODE|STABLE <node-id>

-- REST Adapter (optional HTTP gateway) --

GET    /cache/v1/keys/{key}
  Response: 200 {"value": "...", "ttl": 3540} | 404 {"error": "key not found"}
  Headers: Authorization: Bearer <service-token>
  Rate limit: 5000 req/min per token

PUT    /cache/v1/keys/{key}
  Body: {"value": "...", "ttl_seconds": 3600}
  Response: 204 No Content
  Rate limit: 2000 req/min per token

DELETE /cache/v1/keys/{key}
  Response: 204 No Content | 404

GET    /cache/v1/keys?pattern=session:user:*&cursor=0&count=100
  Response: {"cursor": "next_cursor", "keys": [...]}
  Note: Uses SCAN under the hood, never KEYS (KEYS blocks event loop)
```

---

## 6. Deep Dive: Core Components

### 6.1 Memory Management & Eviction Policies

**Problem it solves:** Memory is finite. When `used_memory` approaches `maxmemory`, the cache must decide which keys to remove to make room for new writes. The wrong policy causes either cache thrashing (evicting hot keys) or stale data accumulation.

**Approaches Comparison:**

| Policy | Algorithm | Best For | Worst Case |
|---|---|---|---|
| `noeviction` | Reject writes when full | Durability-critical data | OOM on full cache |
| `allkeys-lru` | Approximate LRU across all keys | General-purpose cache (unknown TTLs) | Cold keys may linger if accessed once |
| `volatile-lru` | LRU only on keys with TTL | Mixed TTL/non-TTL data | Non-TTL keys never evicted |
| `allkeys-lfu` | Approximate LFU across all keys | Skewed access (Zipf distribution) | New hot keys start with low freq |
| `volatile-lfu` | LFU only on keys with TTL | Workloads with TTL-tagged hot data | Same new-key problem |
| `allkeys-random` | Random eviction across all keys | Uniform access distribution | Evicts hot keys equally |
| `volatile-random` | Random eviction on TTL keys | TTL-tagged cache with flat distribution | Same issue |
| `volatile-ttl` | Evict key with smallest remaining TTL | Short-lived cache with TTL semantics | Evicts nearly-expired useful keys |

**Selected approach: `allkeys-lfu` for general hot-data cache; `volatile-lru` for session/TTL-tagged stores.**

Reasoning: Real-world web traffic follows a Zipf distribution — a small percentage of keys account for the majority of accesses. LFU maintains a frequency counter per key and evicts the least-frequently-used key, preserving hot keys even if they weren't accessed in the last few seconds. LRU would evict a key that was accessed 10 million times simply because it wasn't the most recent access. Redis implements approximate LFU using a Morris counter (logarithmic 8-bit counter) with time-based decay to handle keys that were once hot but cooled off.

**Implementation Detail — Approximate LFU (Morris Counter):**

```
-- Frequency counter stored in lru_clock field (repurposed) --
-- 8-bit counter: upper 4 bits = last-decrement time, lower 4 = log frequency --

function increment_lfu_counter(current_counter, base_probability=10):
    # Extract frequency (lower 8 bits, max 255)
    freq = current_counter & 0xFF
    
    # Logarithmic increment: probability decreases as freq grows
    # P(increment) = 1 / (freq * base_probability + 1)
    if freq < 255:
        rand = random_uniform(0, 1)
        probability = 1.0 / (freq * base_probability + 1)
        if rand < probability:
            freq += 1
    return (current_counter & ~0xFF) | freq

function decay_lfu_counter(current_counter, lfu_decay_time):
    # Decay: every lfu_decay_time minutes without access, decrement by 1
    now_minutes = unix_time_seconds() / 60
    last_decrement_time = (current_counter >> 8) & 0xFF  # upper bits
    
    num_periods = now_minutes - last_decrement_time
    decay_amount = num_periods / lfu_decay_time
    
    freq = current_counter & 0xFF
    freq = max(0, freq - decay_amount)
    
    new_upper = now_minutes & 0xFF
    return (new_upper << 8) | freq

function evict_lfu_key(sample_size=5):
    # Sample N random keys; evict the one with lowest freq after decay
    candidates = sample_random_keys(sample_size)
    return min(candidates, key=lambda k: decay_lfu_counter(k.lru_clock))
```

**Interviewer Q&As:**

Q: Why does Redis use approximate LRU instead of a true LRU linked list?  
A: A true LRU doubly-linked list requires O(1) pointer updates on every access — fine for single-threaded operation, but it adds memory overhead (two 8-byte pointers per key) and CPU cache pressure. Redis's approximate LRU samples a configurable number of keys (default 5, tunable with `maxmemory-samples`) and evicts the one with the oldest clock, achieving 97%+ accuracy of true LRU at a fraction of the memory cost. The pool-based sampler in Redis 3.0+ further improves accuracy by maintaining a candidate eviction pool across samples.

Q: How does LFU handle the "new key" problem — a brand-new hot key starts with frequency 0 and might be immediately evicted?  
A: Redis initializes new keys with an LFU counter of 5 (configurable via `lfu-log-factor` and the initialization value), not 0. This gives new keys a grace period before they accumulate real frequency data. Additionally, the decay mechanism lowers counters for old keys over time, so new keys with even modest access quickly overtake stale high-frequency keys.

Q: What is the difference between `volatile-lru` and `allkeys-lru`?  
A: `volatile-lru` only considers keys that have an expiry set (via `EXPIRE`/`EX`). Keys without TTL are never evicted, preserving "permanent" cache entries like configuration data or static lookup tables. `allkeys-lru` considers every key, including those without TTL, which provides more eviction candidates but risks evicting data that was intentionally marked as permanent.

Q: When would you use `volatile-ttl` over `volatile-lru`?  
A: `volatile-ttl` is appropriate when the TTL itself encodes priority — for example, when shorter TTL means "less valuable soon." A CDN edge cache storing content with TTLs set by Cache-Control headers benefits from `volatile-ttl` because evicting near-expired content first minimizes stale-serving. `volatile-lru` would be better when recency of access matters more than remaining TTL, such as API response caches.

Q: How do you handle a workload where eviction rate is spiking and cache hit rate is dropping?  
A: First, increase `maxmemory` by scaling to larger nodes or adding shards (live resharding). Second, profile the keyspace with `redis-cli --hotkeys` (LFU mode) and `OBJECT FREQ <key>` to identify unexpectedly high-frequency keys and consider application-level changes (request coalescing, dog-pile prevention). Third, tune `maxmemory-samples` upward (to 10–20) to improve eviction candidate quality. Fourth, examine key TTL distribution — keys with no TTL and low access frequency should be tagged with TTLs in the application layer to make them eviction-eligible.

---

### 6.2 Cluster Mode: Hash Slots & Resharding

**Problem it solves:** A single Redis instance is bounded by the RAM and CPU of one machine. Cluster mode shards the keyspace across N primary nodes, allowing the dataset to exceed single-node memory and distributing write throughput.

**Approaches Comparison:**

| Approach | Mechanism | Pros | Cons |
|---|---|---|---|
| **Hash slots (Redis Cluster)** | CRC16(key) % 16384 → slot → node | No coordinator needed, deterministic, gossip-based | 16K slot count is fixed; no cross-slot transactions |
| Client-side consistent hashing | Client hashes key to node | Zero server-side overhead | No server awareness, all coordination in client |
| Proxy-based sharding (Twemproxy) | Proxy routes to backends | Legacy clients unmodified | Proxy is SPOF, extra hop latency |
| Range partitioning | Slot 0–N to node 1, N+1–2N to node 2 | Simple | Hotspots on sequential keys |
| Directory server | Central mapping service | Flexible remapping | Extra RTT, coordinator SPOF |

**Selected: Redis Cluster hash slots (16,384 slots)**

Justification: The 16,384 slot count was chosen to keep the slot assignment bitmap small enough to be included in heartbeat gossip messages (16,384 bits = 2 KB) while providing enough granularity for up to ~1,000 nodes (16 slots/node minimum). The CRC16 polynomial provides good distribution uniformity. The design requires no external coordinator — slot assignments propagate via gossip, and `MOVED`/`ASK` responses allow clients to self-heal their slot maps.

**Hash Tags for Multi-Key Operations:**  
Keys enclosed in `{}` hash only the content inside braces: `{user.12345}.session` and `{user.12345}.cart` always map to the same slot, enabling atomic `MULTI`/`EXEC` transactions across logically related keys.

**Live Resharding Algorithm (CLUSTER SETSLOT):**

```
function migrate_slot(slot, source_node, target_node):
    # Step 1: Mark target as IMPORTING
    target_node.execute("CLUSTER SETSLOT", slot, "IMPORTING", source_node.id)
    
    # Step 2: Mark source as MIGRATING
    source_node.execute("CLUSTER SETSLOT", slot, "MIGRATING", target_node.id)
    
    # Step 3: Iteratively move keys
    while True:
        keys = source_node.execute("CLUSTER GETKEYSINSLOT", slot, 100)  # batch of 100
        if not keys:
            break
        for key in keys:
            # MIGRATE is atomic: DEL on source, SET on target, returns on error
            source_node.execute(
                "MIGRATE", target_node.host, target_node.port,
                key, 0, 5000,  # db 0, timeout 5000ms
                "REPLACE"      # overwrite if exists on target
            )
    
    # Step 4: Update slot assignment in cluster (propagated via gossip)
    source_node.execute("CLUSTER SETSLOT", slot, "NODE", target_node.id)
    target_node.execute("CLUSTER SETSLOT", slot, "NODE", target_node.id)
    # Gossip propagates to all other nodes within O(log N) rounds

# During migration: source serves keys still present (returns value)
# If key already migrated to target: source returns ASK redirect
# Client handles ASK: sends ASKING to target, then re-issues command
# MOVED redirect: slot permanently moved, client updates its slot map
```

**Interviewer Q&As:**

Q: Why 16,384 hash slots specifically — why not 65,536 or 1,000,000?  
A: The gossip heartbeat packet includes the full slot-assignment bitmap. At 16,384 slots: 16,384 / 8 = 2,048 bytes = 2 KB per heartbeat message. At 65,536 slots that becomes 8 KB per heartbeat, which is significant when each node gossips with every other node every second. 16,384 also provides sufficient granularity for up to ~1,000 nodes while keeping rebalancing work small (each slot migrates its keys atomically). The Redis author (Salvatore Sanfilippo) noted this was a practical engineering choice, not a theoretical optimum.

Q: What is the difference between a MOVED and an ASK redirect?  
A: `MOVED slot <ip:port>` means the slot has permanently moved to the new node — the client must update its local slot map and all future requests for that slot should go to the new node. `ASK slot <ip:port>` is a temporary redirect during migration: the key has already been migrated, but the client must send an `ASKING` command to the target before its real command (to bypass the "slot is in IMPORTING state" guard on the target). The client should NOT update its slot map for an ASK, because once migration finishes a MOVED will confirm the final assignment.

Q: How does Redis Cluster handle a split-brain scenario?  
A: Redis Cluster uses a majority-based failover. When a primary becomes unreachable, replicas start a failover election. A replica wins if it receives `FAILOVER_AUTH_ACK` from more than half the masters in the cluster (`N/2 + 1`). If the cluster is partitioned into two halves of equal size, neither side has quorum and no failover occurs — the minority partition stops serving writes (keys in that partition are marked as failing). This is a deliberate CP-over-A choice for writes, while reads can continue from replicas in both partitions.

Q: How do hash tags affect data distribution?  
A: Hash tags `{...}` allow grouping keys onto the same slot for atomic multi-key operations. The risk is hotspot creation: if all application keys use the same tag prefix (e.g., `{user}`), all keys land on the same slot and one node bears 100% of the load. Best practice is to make the tag granular (e.g., `{uid_12345}` rather than `{user}`) so that each user's keys colocate but users distribute across slots. Monitor with `CLUSTER KEYSLOT` and `CLUSTER INFO` slot distribution.

Q: How would you handle a node failure during resharding?  
A: If the source node fails mid-migration, the slot remains in `MIGRATING` state on the crashed node's metadata, and the target remains in `IMPORTING`. The cluster will not route requests normally until the situation is resolved. Recovery: restart the source node (it will still know which slot was migrating and re-serve keys), or if permanently lost, use `CLUSTER SETSLOT <slot> NODE <target-id>` to force-assign the slot to the target, then recover data from AOF/RDB or accept partial data loss for the in-flight keys.

---

### 6.3 Persistence: RDB vs AOF

**Problem it solves:** An in-memory store loses all data on process restart or node failure. Persistence options allow recovery to a recent state without replaying from the origin database, dramatically reducing cold-start time and protecting against data loss.

**Approaches Comparison:**

| Mechanism | Description | Recovery Time | Data Loss Risk | Write Overhead |
|---|---|---|---|---|
| **No persistence** | Data only in RAM | Full rebuild from DB | 100% on restart | None |
| **RDB snapshot** | Fork process, serialize whole dataset to `.rdb` file | Fast (load binary) | Since last snapshot (minutes) | Fork cost spikes periodically |
| **AOF (appendonly)** | Log every write command to `.aof` file | Slow (replay commands) | Since last fsync | fsync overhead per write |
| **AOF + RDB hybrid** | RDB base + AOF tail since snapshot | Fast (load RDB, replay short AOF tail) | Since last fsync | Combined overhead |
| **Replication only** | Rely on replica for durability | Replica promotion | Zero if replica up-to-date | Replication bandwidth |

**Selected: AOF with `appendfsync everysec` + periodic RDB for backup; Hybrid persistence in production.**

Reasoning:
- `appendfsync always`: fsync on every write — maximum durability (at most 1 command lost), but write throughput drops to disk IOPS limit (~10K ops/sec for spinning disks, ~100K for NVMe). Unsuitable for 350K ops/sec target.
- `appendfsync everysec`: fsync every 1 second in background — at most 1 second of data loss. Write throughput is near-unimpeded (AOF write goes to OS page cache; background thread fsyncs). **This is the standard production choice.**
- `appendfsync no`: never fsync — OS decides when to flush. Best performance, worst durability.

**AOF Rewrite (log compaction):**
```
function aof_rewrite():
    # Problem: AOF grows unboundedly. Rewrite compacts it.
    # Redis forks a child process.
    
    child_process:
        # Walk in-memory dataset
        for key in all_keys:
            if key.has_expiry and key.expiry < now():
                continue  # skip already-expired keys
            # Emit the minimal command to recreate current state
            emit_command(key)
            # e.g., "SET foo bar EX 3540"
            # e.g., "HSET session:user:123 token abc created_at 1704067200"
        write_to_temp_aof_file()
    
    parent_process:
        # While child rewrites, buffer new commands in aof_rewrite_buf
        # When child finishes:
        #   1. Append aof_rewrite_buf to new AOF file
        #   2. Atomically rename new AOF over old AOF (rename syscall is atomic)
        atomic_rename(temp_aof, aof_file)
    
    # Trigger: auto-aof-rewrite-percentage 100 (double in size since last rewrite)
    #          auto-aof-rewrite-min-size 64mb
```

**Interviewer Q&As:**

Q: What is the fork overhead problem with RDB/AOF rewrite, and how do you mitigate it?  
A: When Redis forks for RDB save or AOF rewrite, the OS uses copy-on-write (COW) semantics. The fork itself is O(1) (kernel just copies page table entries). But as the parent process handles writes, it triggers COW page copies for every modified memory page. In the worst case (all keys modified during snapshot), RSS doubles. On a 32 GB Redis instance during a write-heavy workload, fork can pause the process for 1–10 seconds while the kernel copies page tables, causing latency spikes. Mitigations: use Linux `Transparent Huge Pages: disabled` (THP creates 2 MB pages, making COW copies larger), use `replica-lazy-flush yes`, schedule RDB snapshots during low-traffic periods, and use dedicated replica nodes for persistence (avoid forking on primaries).

Q: What happens if the AOF file becomes corrupted mid-write due to a power failure?  
A: If the last write to AOF was incomplete (truncated), Redis detects this on startup and either refuses to start (if `aof-use-rdb-preamble no` and strict mode) or truncates the file at the last valid command and continues loading. The `redis-check-aof --fix` tool can automatically find the truncation point and trim the file. For hybrid RDB+AOF, the RDB preamble is loaded first (fast), then the valid portion of the AOF tail is replayed.

Q: How does Redis handle AOF and RDB in cluster mode across multiple nodes?  
A: Each node manages its own persistence independently. There is no coordinated cluster-wide snapshot. For cluster backups, operators either snapshot all primaries simultaneously (approximate consistency), or rely on WAIT command to ensure replicas are in sync before snapshotting replicas, or use an external backup tool (e.g., redis-dump, RDB diff tools) that understands the cluster key distribution. For recovery, each node's RDB/AOF is restored independently, then the cluster is re-formed.

Q: When would you disable persistence entirely?  
A: For pure cache tiers where the origin database is the source of truth and cache misses are acceptable. Disabling persistence eliminates fork overhead, fsync latency spikes, and disk I/O contention — improving write throughput and lowering latency tail. The tradeoff is that node restarts cause cold cache (cache miss storm). To mitigate warm-up time, a pre-warming process can be run on restart, reading the most-accessed keys from the DB using a hot-key list from application metrics.

Q: What is the "Redis + replication" durability model — can you lose data even with a replica?  
A: Yes. Redis replication is asynchronous. After a primary acknowledges a write (`EXEC` returns OK), the command is queued for async propagation to replicas. If the primary crashes before the replica receives that command, the write is lost permanently — the replica is promoted as new primary without that write. The `WAIT <numreplicas> <timeout>` command provides a synchronous replication fence: `WAIT 1 100` blocks until at least 1 replica acknowledges the last write or 100ms passes. Using `WAIT 1 0` (infinite timeout) on critical writes approaches synchronous replication durability at the cost of write latency increase.

---

## 7. Scaling

### Horizontal Scaling Strategy

**Read Scaling:** Add replicas per shard. Redis Cluster supports up to N replicas per primary. Read replicas serve `GET`, `HGET`, `ZRANGE` etc. from application read replicas with `READONLY` mode. Use `replica-lazy-flush yes` to avoid blocking flushes on replica restart.

**Write Scaling:** Add primary shards. Each new shard receives a portion of the 16,384 hash slots via live resharding (`CLUSTER REBALANCE`). Adding 2 new nodes to an 8-node cluster: each of the 8 existing primaries migrates ~1/10 of their slots (approximately 2048 slots × 2 new nodes / 10 total nodes ≈ 410 slots each) — done live with `CLUSTER REBALANCE --pipeline 10`.

**Vertical Scaling:** Upgrade instance type. Prefer r6g (Graviton) instances for better memory/dollar ratio. AWS ElastiCache and GCP Memorystore support online instance type upgrade with < 1 min failover.

### DB Sharding
Redis Cluster handles sharding natively via hash slots. Application concern: avoid cross-slot multi-key commands unless hash tags are used. For multi-key operations across different slots, the application must pipeline commands and accept non-atomic semantics, or use Lua scripts with hash-tagged keys.

### Replication
- Async replication: primary streams commands to replicas over persistent TCP connections using the Redis Replication Protocol (RDB bulk sync on first connection, then command stream).
- Replication buffer: `client-output-buffer-limit replica 256mb 64mb 60` — if a replica falls too far behind, it is disconnected and must do a full resync.
- Replica read: `replica-read-only yes` (default). Applications can load-balance reads across replicas for additional throughput, but reads may return slightly stale data (replication lag typically < 10 ms).
- Synchronous replication: Use `WAIT 1 100` for critical writes where replication lag is unacceptable.

### Caching Strategy (L1/L2)
```
L1: Application-side in-process cache (e.g., Caffeine, LRU dict)
    - Size: ~100 MB per app server JVM heap
    - TTL: 5–30 seconds
    - Hit rate: 40–60% for hottest keys
    - Eliminates network RTT entirely

L2: Redis Cluster (this system)
    - Size: 200 GB+ across cluster
    - TTL: minutes to hours
    - Hit rate: 85–95% for warm dataset

L3: Origin DB (Postgres, DynamoDB)
    - Only accessed on L1+L2 miss
    - Protected by cache stampede prevention (PER algorithm or mutex lock)
```

**Cache Stampede Prevention (Probabilistic Early Recomputation — PER):**
```python
def get_with_per(key, recompute_fn, ttl, beta=1.0):
    value, expiry = cache.get_with_expiry(key)
    if value is None or (time.time() - beta * math.log(random.random()) > expiry):
        # PER: recompute before expiry with probability that increases near expiry
        new_value = recompute_fn()
        cache.set(key, new_value, ttl)
        return new_value
    return value
```

### Interviewer Q&As (Scaling)

Q: How do you handle a hot key (a single key receiving disproportionate traffic, e.g., a viral post)?  
A: Several strategies: (1) Local in-process cache with short TTL absorbs reads on each app server, multiplying effective read capacity by the number of app servers. (2) Key replication at application level: write the same value to `hotkey:N` for N = 0..99, reads randomly select one. (3) Redis 7.0 client-side caching (tracking invalidation) caches key in client memory with server-push invalidation. (4) Read replicas: route all reads for that key to replicas. Hot key identification: `redis-cli --hotkeys` (requires LFU maxmemory policy) or `MONITOR` command (development only — extremely expensive in production).

Q: How do you scale Pub/Sub to millions of subscribers?  
A: Redis Pub/Sub fans out a published message to all subscribers on the same node. For cluster mode, `PUBLISH` is forwarded to all nodes in the cluster (O(N nodes)), and each node fans out to its local subscribers. This scales well for fan-out within a datacenter but creates O(N_nodes × M_subscribers_per_node) work. For massive fan-out (push notifications, sports scores), a dedicated Pub/Sub broker (Kafka + WebSocket gateway, or NATS) is more appropriate. Redis Streams (`XADD`/`XREAD`) with consumer groups are a better fit for durable, replay-able fan-out.

Q: What is Redis Sentinel and when do you use it instead of Cluster?  
A: Sentinel provides HA for a standalone primary/replica setup without data sharding. Three or more Sentinel processes monitor the primary; when a quorum marks it as `ODOWN` (objectively down), one Sentinel triggers a failover: it reconfigures the most up-to-date replica as new primary and updates all other replicas and clients. Use Sentinel when: (a) dataset fits in a single node, (b) you want simplicity without cluster overhead, (c) clients support Sentinel discovery (most Redis clients do). Use Cluster when: data exceeds single-node memory, write throughput exceeds single-node capacity, or you want built-in sharding without a proxy.

Q: How does Redis handle memory fragmentation, and how do you combat it?  
A: The allocator (jemalloc by default) maintains memory in size-class bins. As keys are created and deleted at different sizes, freed blocks of one size cannot be reused for different-size allocations, leading to fragmentation. Fragmentation ratio = `mem_rss / used_memory`. Healthy: < 1.5. Problematic: > 1.5. Redis 4.0+ supports `MEMORY PURGE` (calls `jemalloc_purge`) and active defragmentation (`activedefrag yes`) which rewrites live keys to contiguous memory in the background at the cost of some CPU. Also: avoid mixing many different value sizes within the same instance; separate workloads by data shape across instances.

Q: How would you implement a rate limiter using Redis at 100K RPS?  
A: Use a sliding window log or fixed window counter. For fixed window with per-user limit of 1000 req/min: key = `ratelimit:{uid}:{minute_bucket}`, value = counter. Lua script ensures atomicity: `INCR key; if current == 1 then EXPIRE key 60 end; return current`. This is one round trip, processed atomically, achieving ~100K ops/sec easily. For sliding window: use a Sorted Set with timestamp as score, prune entries older than 1 minute with `ZREMRANGEBYSCORE`, count remaining entries. This is more accurate but more expensive per request (O(log N) ZADD + O(M) ZREMRANGE).

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Detection | Recovery |
|---|---|---|---|
| Primary node crash | Writes fail for affected slots; reads fail until failover | Gossip PFAIL → FAIL within cluster_node_timeout (default 15s) | Replica election; failover in ~15–30s; data loss = async lag |
| Replica node crash | Reduced read capacity; no data loss | Gossip PFAIL detection | Primary continues serving; replace replica node |
| Network partition (minority side) | Minority primary stops accepting writes after timeout | cluster_node_timeout | Minority primary demotes itself; majority elects new primary |
| AOF corruption | Node cannot start | Startup check | `redis-check-aof --fix`; restore from RDB backup |
| Memory full (maxmemory reached) | Writes return OOM error (noeviction) or eviction begins | `used_memory` metric alert | Scale memory, tune eviction, reduce data volume |
| High replication lag | Replica data is stale; failover target may be behind | `replication_lag` metric | Alert ops; use `WAIT` for critical writes; auto-failover picks best replica |
| Client connection storm | Connection queue overflows; clients timeout | `connected_clients` metric; `blocked_clients` | Connection pooling (min 10, max 100 per app server); backpressure at load balancer |
| Lua script timeout | Script blocked for > `lua-time-limit` (5000ms default) | Slow log; server logs `BUSY` | `SCRIPT KILL` if script has not written; restart if it has |
| Key expiry mass eviction | Cache hit rate drops suddenly on restart or TTL batch expiry | `expired_keys` rate spike | Stagger TTLs with jitter: `TTL = base + random(0, 10% of base)` |

**Failover Flow:**
```
t=0:    Primary-1 becomes unreachable (network partition or crash)
t=1s:   Other nodes set Primary-1 to PFAIL (probable fail) in gossip
t=15s:  Enough nodes have PFAIL → cluster marks Primary-1 as FAIL
t=15s:  Replica-1a starts failover: sends FAILOVER_AUTH_REQUEST to all masters
t=15s:  Masters check: Is this replica's replication offset up to date?
                       Does this master have a vote token for this epoch?
t=16s:  Replica-1a receives majority ACK (e.g., 5/8 masters voted)
t=16s:  Replica-1a promotes itself: CONFIG SET replica-priority ... 
                                     SLAVEOF NO ONE (now primary)
t=16s:  Broadcasts new epoch and slot ownership via gossip
t=17s:  All other nodes and clients receive MOVED redirects to Replica-1a (new Primary-1)
        Total failover time: ~17 seconds
```

**Retries & Idempotency:**
- `SET key value NX EX 60`: naturally idempotent — safe to retry
- `INCR key`: NOT idempotent — retry causes double-increment. Solution: use a client-generated idempotency token as a separate key: `SET idempotency:{request_id} "processed" NX EX 300` before INCR; if SET returns nil, skip INCR.
- RESP3 connection: clients use exponential backoff (50ms, 100ms, 200ms, 400ms, max 5 retries) on `CONNRESET` or `CLUSTER DOWN` errors.
- Circuit breaker: after 5 consecutive failures within 10 seconds, open circuit and return cache-miss (fall back to DB) without attempting Redis for 30 seconds.

---

## 9. Monitoring & Observability

| Metric | Tool | Alert Threshold | Meaning |
|---|---|---|---|
| `used_memory` / `maxmemory` | `INFO memory` → Prometheus | > 85% | Approaching eviction zone |
| `evicted_keys` per second | `INFO stats` | > 100/sec | Eviction storm; consider scaling |
| `keyspace_hits` / `keyspace_misses` | `INFO stats` | Hit rate < 80% | Cache is under-sized or poorly populated |
| `replication_lag` per replica | `INFO replication` → `master_repl_offset - replica_repl_offset` | > 1 MB | Replica falling behind; network or disk issue |
| `connected_clients` | `INFO clients` | > 80% of `maxclients` | Connection pool exhaustion |
| `blocked_clients` | `INFO clients` | > 0 for extended period | BLPOP/BRPOP consumers not draining |
| `aof_pending_rewrite_buffer_length` | `INFO persistence` | > 100 MB | AOF rewrite falling behind; disk slow |
| `instantaneous_ops_per_sec` | `INFO stats` | Below expected baseline | Request drop; upstream issue |
| `latency_ms` (p99) | `LATENCY HISTORY event` | > 2 ms | Slow commands, fork event, network |
| `mem_fragmentation_ratio` | `INFO memory` | > 1.5 | Fragmentation; enable activedefrag |
| `cluster_state` | `CLUSTER INFO` | `!= ok` | Cluster has failed or missing slots |
| `total_commands_processed` | `INFO stats` | Delta drops suddenly | Node or network issue |

**Distributed Tracing:**
- Instrument Redis client library to inject trace context into a shadow key or use OpenTelemetry Redis instrumentation.
- Trace spans: `redis.GET`, `redis.SET`, `redis.EVAL` with attributes: `db.redis.key`, `db.redis.database_index`, duration, error type.
- Correlate high-latency Redis spans with upstream service spans in Jaeger/Zipkin/AWS X-Ray.

**Logging:**
- Redis slow log: `SLOWLOG GET 10` — shows commands taking > `slowlog-log-slower-than` microseconds (default 10,000 = 10 ms). Log entries include: command, args, latency, client IP.
- Application logs: log cache miss events with key pattern (NOT key value) and fallback duration to detect patterns.
- Keyspace notifications: `notify-keyspace-events "Ex"` — publishes to `__keyevent@0__:expired` channel when keys expire. Useful for downstream cache invalidation chains.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Alternative | Reason |
|---|---|---|---|
| Data structure | Redis Cluster | Memcached cluster | Rich types, replication, persistence, Lua |
| Eviction policy | allkeys-lfu | allkeys-lru | Zipf distribution; LFU preserves true hot keys |
| Persistence | Hybrid RDB+AOF, fsync everysec | No persistence | Balance: fast recovery + acceptable 1s data loss |
| Sharding | Redis hash slots (16,384) | Client-side consistent hash | No coordinator, live resharding, gossip HA |
| HA for shards | Cluster-internal failover | Sentinel | Cluster handles sharding + HA together |
| Read scaling | Read replicas + L1 app cache | More primaries | Cost-efficient; replicas are cheaper than new shards |
| Rate limiter | Lua script (INCR atomic) | Sliding window ZSet | Lower latency (O(1) vs O(log N)); fixed-window accuracy acceptable |
| Pub/Sub | Redis Pub/Sub for low-fan-out | Kafka for large fan-out | Sub-ms delivery for small subscriber counts; Kafka for durable replay |
| Memory fragmentation | activedefrag + jemalloc | restart + reload | Zero-downtime defrag |
| Cache stampede | PER algorithm | Mutex/lock | No blocking; probabilistic works well for TTL-expiry scenarios |

---

## 11. Follow-up Interview Questions

Q1: How does Redis ensure command atomicity in a single-threaded model?  
A: Redis processes all commands in a single event loop thread. No command can be interleaved with another. `MULTI`/`EXEC` queues commands that execute as an uninterruptible block. Lua scripts are also atomic — the event loop does not process other commands while a script runs. This eliminates the need for internal locking, making Redis extremely fast but also meaning long-running scripts block all other clients.

Q2: What is the difference between MULTI/EXEC and Lua scripting?  
A: `MULTI`/`EXEC` is a client-side transaction: commands are queued on the server and executed atomically when EXEC is called. But the client cannot branch logic on intermediate results (no conditionals based on GET return value within MULTI). Lua scripts run server-side and can use full Lua control flow (`if/else`, loops) based on `redis.call()` return values. Lua is preferred for conditional atomic operations like check-and-set, rate limiting, and distributed lock acquisition.

Q3: How would you implement a distributed semaphore (allow N concurrent holders) using Redis?  
A: Use a sorted set: score = expiry timestamp, member = holder_id. Acquire: `ZREMRANGEBYSCORE key -inf <now>` (prune expired) → `ZCARD key` (count holders) → if count < N: `ZADD key <now+ttl> <holder_id>`. Wrap in a Lua script for atomicity. Release: `ZREM key <holder_id>`. This supports N > 1 holders and handles expiry-based auto-release.

Q4: Explain the Redis memory model — why are small integers and short strings handled differently?  
A: Redis uses a shared integer object pool for integers 0–9999: all keys referencing the integer 42 point to the same object (refcount incremented), saving memory. Strings ≤ 44 bytes use `embstr` encoding: string object and data allocated in one contiguous 64-byte allocation (fits one CPU cache line). Strings > 44 bytes use `raw` encoding with a separate `sdshdr` + data allocation. Lists, hashes, and sets use compact `listpack` (previously `ziplist`) encoding when they are small (configurable `hash-max-listpack-entries 128`), converting to hash table or skiplist only when they exceed the threshold.

Q5: What is the Redis keyspace notification feature and what are its use cases?  
A: Redis can publish events to special channels when key operations occur (`notify-keyspace-events "KExg"`). A subscriber on `__keyevent@0__:expired` receives the key name when it expires. Use cases: cache invalidation cascades (expire parent key → invalidate dependent application caches), audit logging, triggering downstream workflows on expiry. Caution: high notification volume can overwhelm pub/sub subscribers; events are not durable (missed if subscriber is down).

Q6: How does Redis handle big keys and how do you find them?  
A: Big keys (e.g., a hash with 10 M fields, or a 100 MB string) cause: (a) serialization latency spikes when the key is read/written, (b) replication lag bursts when the big key is serialized to the replica stream, (c) memory fragmentation. Find them with `redis-cli --bigkeys` (scans keyspace, reports max key per type) or `OBJECT ENCODING <key>` + `MEMORY USAGE <key>`. Mitigation: split large hashes into sub-keys by field range (e.g., `hash:large:{bucket_0..N}`), use client-side aggregation.

Q7: What is the Redis Stream data type and how does it differ from a simple List?  
A: `Stream` is a log-like data structure (analogous to Kafka topics). Each entry has an auto-generated ID (`<timestamp>-<seq>`) and a field-value map. Features: consumer groups with per-consumer offset tracking (`XREADGROUP`), acknowledgment (`XACK`), pending entry list (PEL) for unacknowledged messages, range queries by ID, trim by length or time. Unlike List, streams are append-only, support multiple independent consumer groups reading the same data, and entries are not deleted on read (only on XDEL or XTRIM).

Q8: How would you implement a leaderboard with real-time rank updates for 100 M users?  
A: Use a Redis Sorted Set: `ZADD leaderboard:weekly <score> <user_id>` on each score update. `ZRANK leaderboard:weekly <user_id>` returns rank in O(log N). Problem: 100 M users × 8 bytes (score) + 8 bytes (member) = 1.6 GB — fits in a single large Redis instance. For write throughput, the key must live in one slot (uses one shard). Mitigation: shard by user range (leaderboard:weekly:shard:{0..N}), compute global rank by summing counts from shards with smaller scores. Or use Redis Cluster with hash tags on a dedicated leaderboard node.

Q9: What is the `WAIT` command and when do you use it?  
A: `WAIT <numreplicas> <timeout>` blocks the client until `numreplicas` replicas have acknowledged all previous write commands, or until timeout. Returns the number of replicas that actually acknowledged. Use it for: (a) critical writes where data loss is unacceptable (financial counters, deduplication keys), (b) read-your-writes consistency when reading from replicas, (c) safe failover preparation before maintenance. Note: it only guarantees replication up to that moment; subsequent writes before `WAIT` are not included.

Q10: Explain how Redis handles clock drift for key expiry in a distributed setting.  
A: Redis computes expiry using the server's local monotonic/wall clock. In a cluster, each node manages expiry independently. If clocks drift between nodes, a key's TTL seen via `TTL` on different nodes may differ. Redis uses the local `unix_time_in_milliseconds()` for `PEXPIRE` and `PTTL`. Mitigation: synchronize all node clocks with NTP or PTP; max acceptable drift is a few milliseconds. A 100ms clock drift on a key with `EX 1` could cause it to expire 10% sooner than expected on one node.

Q11: How would you implement a pub/sub system that survives Redis restarts?  
A: Standard Redis Pub/Sub is ephemeral — messages published while a subscriber is offline are lost. For durable pub/sub, use Redis Streams: producers `XADD stream_name * field value`; consumers use `XREADGROUP GROUP mygroup consumer1 COUNT 10 STREAMS stream_name >` to read new messages. Consumer groups maintain per-consumer offsets. If a consumer crashes, unacknowledged messages stay in the Pending Entry List (PEL) and can be claimed by another consumer with `XCLAIM`. Streams persist to AOF/RDB, surviving restarts.

Q12: What is Redlock and what are its limitations?  
A: Redlock is a distributed lock algorithm across N independent Redis instances (typically 5). To acquire: set a key with `SET key token NX PX ttl` on each instance simultaneously; lock is acquired if > N/2 succeed. The elapsed time must be less than the TTL. Main criticism (Martin Kleppmann): Redlock is unsafe under clock drift, GC pauses, or network delays — a client can believe it holds the lock past its expiry due to a long GC pause, while another client has already acquired it. For storage systems, use fencing tokens (monotonically increasing numbers from a lock service) instead.

Q13: How do you perform a zero-downtime Redis cluster upgrade?  
A: Rolling upgrade: upgrade replicas first (no traffic impact), then failover each shard (promote replica, upgrade former primary, add back as replica). For major version upgrades: test with `redis-server --test-memory`; verify RDB compatibility; use `DEBUG RELOAD` to test persistence load. If downtime is required: use a blue-green cluster approach (spin up new cluster, populate with a migration tool, cut over application config, drain old cluster).

Q14: What is the difference between a cache-aside pattern and a write-through pattern?  
A: **Cache-aside (lazy loading):** Application checks cache first; on miss, reads from DB and populates cache. Cache is eventually consistent; a write to DB does not automatically update cache. **Write-through:** Every DB write is mirrored to the cache synchronously. Ensures consistency but doubles write latency and writes cold data that may never be read. **Write-behind (write-back):** Writes go to cache immediately; DB write is asynchronous. Fastest write path but risks data loss if cache fails before DB flush. For most web applications, cache-aside with explicit cache invalidation on write is the standard pattern.

Q15: How would you test Redis cluster failover in a chaos engineering setup?  
A: Use Chaos Monkey / Gremlin / Pumba to: (1) Kill a primary node process — measure failover time (target < 30s), validate no data corruption, verify clients transparently reconnect. (2) Introduce 200ms network latency on the primary-to-replica link — measure replication lag, verify `WAIT 1 500` correctly times out. (3) Fill node to maxmemory — verify eviction policy activates and no OOM errors propagate to clients. (4) Run `DEBUG SLEEP 30` on a primary (simulates GC pause) — verify cluster marks it FAIL and failsover. Automate these as CI tests with `redis-cli --cluster check` assertions.

---

## 12. References & Further Reading

- **Redis Documentation — Cluster Specification:** https://redis.io/docs/reference/cluster-spec/
- **Redis Documentation — Persistence (RDB and AOF):** https://redis.io/docs/manual/persistence/
- **Redis Documentation — Memory Optimization:** https://redis.io/docs/manual/config/#memory-optimization
- **Redis Documentation — LFU and LRU Eviction:** https://redis.io/docs/manual/eviction/
- **Salvatore Sanfilippo — "Redis Cluster: How it works":** https://redis.io/topics/cluster-tutorial
- **Martin Kleppmann — "How to do distributed locking" (critique of Redlock):** https://martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html
- **Antirez — Response to Kleppmann on Redlock:** http://antirez.com/news/101
- **Designing Data-Intensive Applications — Martin Kleppmann, Chapter 5 (Replication), Chapter 6 (Partitioning):** O'Reilly, 2017
- **Facebook Engineering — Scaling Memcache at Facebook (NSDI 2013):** https://www.usenix.org/system/files/conference/nsdi13/nsdi13-final170_update.pdf
- **Twitter Engineering — Caching at Twitter Scale:** https://blog.twitter.com/engineering/en_us/topics/infrastructure/2020/the-infrastructure-behind-twitter-scale
- **jemalloc — Memory Allocator:** https://github.com/jemalloc/jemalloc
- **Probabilistic Early Recomputation (PER) — Vattani et al., 2015:** http://cseweb.ucsd.edu/~avattani/papers/cache_stampede.pdf
- **AWS ElastiCache for Redis — Best Practices:** https://docs.aws.amazon.com/AmazonElastiCache/latest/red-ug/best-practices.html
- **Redis University — RU101 (Introduction to Redis Data Structures):** https://university.redis.com/courses/ru101/
