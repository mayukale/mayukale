# System Design: Consistent Hashing

---

## 1. Requirement Clarifications

### Functional Requirements
- Distribute a keyspace across N physical nodes such that each node is responsible for a contiguous range of the hash ring
- When a node is added: only the keys in the new node's "predecessor range" are migrated; all other keys remain
- When a node is removed: only the keys owned by the removed node are migrated to its successor; all other keys remain
- Support weighted nodes: a node with twice the capacity should receive twice the key assignments
- Provide a lookup function: given a key, return the node responsible for it in O(log N) time
- Support virtual nodes (vnodes): each physical node is represented by multiple points on the ring to improve distribution uniformity
- Support hotspot mitigation: detect and remedy skewed key distribution
- Client-side and server-side ring topology support

### Non-Functional Requirements
- Minimal key movement on topology change: adding/removing a node should move 1/N of all keys (theoretical minimum)
- Deterministic routing: all clients with the same ring view must route to the same node for the same key
- Low lookup latency: O(log N) key lookup in a ring of N vnodes
- Balanced load distribution: standard deviation of key count across nodes < 10% of mean (achievable with 150+ vnodes per node)
- Ring state consistency: ring topology changes must be propagated to all clients within a bounded time window (eventual consistency with max 5s propagation delay)
- Support for heterogeneous node capacities (weighted consistent hashing)

### Out of Scope
- Data replication and consistency (consistent hashing is a routing mechanism, not a replication protocol)
- Consensus on ring membership (use gossip protocol or central coordinator separately)
- Exactly-once key migration semantics (handled by the key store, not the router)
- Sub-millisecond topology update propagation (gossip-based propagation has bounded but non-zero delay)

---

## 2. Users & Scale

### User Types
| Actor | Behavior |
|---|---|
| Cache client | Calls `get_node(key)` to determine which cache server to contact |
| Database shard router | Routes DB queries to the correct shard node |
| Object storage gateway | Routes object PUT/GET to the correct storage node |
| Ring manager | Adds/removes nodes; distributes updated ring configuration |
| Monitoring service | Reads ring state; detects load imbalance |

### Traffic Estimates (calculations shown)

**Assumptions:**
- 1 M RPS total across all clients hitting the consistent hash router
- 20 physical nodes in the ring
- 150 virtual nodes per physical node → 3,000 total vnodes
- Average key length: 64 bytes (e.g., `user:12345:session`)
- Ring lookup is in-memory (no network call); cost is CPU-bound

Ring lookups/sec = 1 M/sec  
Binary search in sorted array of 3,000 vnodes: log2(3,000) ≈ 12 comparisons  
At 10 ns per comparison: 12 × 10 ns = 120 ns per lookup  
CPU cores needed for lookups only: 1 M × 120 ns / (1 sec / core) = 0.12 cores — **trivial**

The ring itself: 3,000 vnodes × (8 bytes hash + 8 bytes node pointer) = 48 KB — fits in L1/L2 cache entirely. Lookup is effectively free compared to the network call that follows.

### Latency Requirements
| Operation | P50 | P99 | Notes |
|---|---|---|---|
| `get_node(key)` in-process | 100 ns | 500 ns | Binary search in sorted array |
| `get_node(key)` via proxy | 0.1 ms | 1 ms | Network call to routing proxy |
| Ring update propagation | < 100 ms | < 5 s | Gossip-based propagation |
| Node add migration | O(keys/N) | — | Proportional to key count |
| Node remove migration | O(keys/N) | — | Proportional to key count |

### Storage Estimates

Ring state per client:
- 3,000 vnodes × (16 bytes: 8-byte hash + 8-byte node_id reference) = **48 KB per ring**
- Physical node metadata: 20 nodes × 200 bytes = 4 KB
- Total ring state in memory: **< 100 KB** — negligible

For a system with 100 clients (app servers), total ring memory: 100 × 100 KB = 10 MB.

### Bandwidth Estimates

Ring update broadcast (adding/removing one node):
- Ring delta: 150 vnodes × 16 bytes = 2.4 KB message
- Broadcast to 100 clients: 100 × 2.4 KB = 240 KB per topology change
- If topology changes happen 10 times/hour: 10 × 240 KB = 2.4 MB/hour — negligible

---

## 3. High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Client Application                            │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │             Consistent Hash Router (in-process)           │   │
│  │                                                          │   │
│  │  Ring: sorted array of (hash_value, vnode_id, node_ref) │   │
│  │                                                          │   │
│  │  get_node(key):                                          │   │
│  │    h = hash(key)                 (MurmurHash3 / xxHash)  │   │
│  │    idx = binary_search(ring, h)  (O(log N))              │   │
│  │    return ring[idx].node_ref      (wrap-around at end)   │   │
│  │                                                          │   │
│  │  ring_state: updated by Ring Manager via gossip/watch    │   │
│  └──────────────────────────────────────────────────────────┘   │
└────────────────────────────┬────────────────────────────────────┘
                             │ Routes requests to
           ┌─────────────────┼──────────────────┐
           ▼                 ▼                  ▼
    ┌─────────────┐   ┌─────────────┐   ┌─────────────┐
    │  Node A     │   │  Node B     │   │  Node C     │
    │  Cache /    │   │  Cache /    │   │  Cache /    │
    │  DB Shard   │   │  DB Shard   │   │  DB Shard   │
    │             │   │             │   │             │
    │ Hash range: │   │ Hash range: │   │ Hash range: │
    │ [0, 2^32/3) │   │ [2^32/3,    │   │ [2×2^32/3,  │
    │             │   │  2×2^32/3)  │   │   2^32)     │
    └─────────────┘   └─────────────┘   └─────────────┘

Hash Ring (conceptual):
                    0 / 2^32
                    ┌──────┐
               N  ──┤  A   ├──  (vnode a1)
              ╱    └──────┘
             ╱      ┌──────┐
            ╱  ─────┤  C   ├─ (vnode c1)
           │  ╱    └──────┘
           │ ╱      ┌──────┐
           ╱  ──────┤  B   ├─ (vnode b1)
          ╱╲       └──────┘
         ╱  ╲
        ╱    ╲  ┌──────┐
       S      ╲─┤  A   ├── (vnode a2)
               ╲└──────┘
                ┌──────┐
          ──────┤  C   ├── (vnode c2)
               └──────┘
   (Ring continues clockwise with more vnodes interspersed)

┌──────────────────────────────────────────────────────────────────┐
│                    Ring Manager Service                           │
│                                                                   │
│  - Maintains authoritative ring configuration                    │
│  - Stored in etcd / ZooKeeper (consistent, replicated)          │
│  - Publishes ring changes via gossip or Pub/Sub                  │
│  - Coordinates key migration when ring changes                   │
│  - Detects hotspot nodes and triggers vnode rebalancing          │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                 Gossip Protocol Layer                             │
│                                                                   │
│  - Each node periodically gossips ring state to K random peers   │
│  - Ring change propagates in O(log N) gossip rounds              │
│  - Uses vector clocks or version numbers to detect stale state   │
└──────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **Consistent Hash Router (in-process):** A sorted array of `(hash_position, vnode_id, physical_node_reference)` tuples. Lookup is a binary search on the hash_position array. All client libraries embed this component. Zero-cost if the ring fits in L2 cache (< 100 KB).
- **Physical Node:** A cache server, database shard, or storage node. Owns the key range from its hash position to the next vnode's hash position on the ring (clockwise).
- **Virtual Node (vnode):** A logical entry on the ring mapping to a physical node. Each physical node has V vnodes spread across the ring. V controls distribution uniformity. Higher V = better distribution but more migration work per topology change.
- **Ring Manager Service:** Authoritative source of ring topology. Uses etcd for durable, consistent storage. Propagates changes via gossip or Pub/Sub. Coordinates key migration.
- **Gossip Layer:** Peer-to-peer propagation of ring state changes. Each node gossips to K random peers every T milliseconds. Changes propagate in O(log N) rounds (expected).

**Primary Use-Case Data Flow (Key Lookup):**
1. Client application has key `user:12345:session`
2. Router computes `h = xxHash64("user:12345:session") mod 2^32 = 0xA3B4C5D6`
3. Router binary searches sorted vnode array for first position ≥ 0xA3B4C5D6
4. Finds vnode `b7` at position 0xA4000000 → owned by physical node `Node-B-IP:6379`
5. Client sends `GET user:12345:session` to `Node-B-IP:6379`
6. If Node B is down: client moves to next vnode (replication) or waits for ring update

---

## 4. Data Model

### Entities & Schema

```
-- In-Memory Ring Representation (per client) --

struct VNode {
    hash_position : uint32    // position on the ring (0 to 2^32 - 1)
    vnode_id      : string    // e.g., "node-b:7" (node b, vnode index 7)
    physical_node : NodeRef   // pointer to physical node info
}

struct NodeRef {
    node_id      : string     // stable unique identifier, e.g., "node-b"
    address      : string     // "10.0.1.5:6379"
    weight       : float      // relative capacity (1.0 = standard; 2.0 = double capacity)
    vnode_count  : int        // number of vnodes for this node (= round(weight * V_base))
    status       : enum       // ACTIVE | LEAVING | JOINING | DOWN
}

-- Sorted ring structure --
ring: VNode[]  // sorted ascending by hash_position
               // length = sum(vnode_count for all physical nodes)
               // e.g., 20 nodes × 150 vnodes = 3,000 entries

-- Binary search lookup --
function get_node(key: string) -> NodeRef:
    h = xxHash64(key) & 0xFFFFFFFF  // 32-bit hash
    // Binary search for first vnode with hash_position >= h
    lo, hi = 0, len(ring) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if ring[mid].hash_position < h:
            lo = mid + 1
        else:
            hi = mid
    // Wrap around: if h > all positions, use ring[0]
    idx = lo % len(ring)
    return ring[idx].physical_node

-- Persistent Ring Configuration (stored in etcd) --

Key: /ring/v1/config
Value: {
  "version": 42,               // monotonically increasing version number
  "nodes": [
    {
      "node_id": "node-a",
      "address": "10.0.1.1:6379",
      "weight": 1.0,
      "vnodes": [              // pre-computed vnode positions
        {"vnode_id": "node-a:0", "position": 123456789},
        {"vnode_id": "node-a:1", "position": 456789012},
        ...                    // 150 entries for node-a
      ]
    },
    ...
  ]
}

-- Vnode Position Computation --
// Deterministic: given a fixed node_id and vnode index, always produces the same position
function compute_vnode_position(node_id: string, vnode_index: int) -> uint32:
    seed = f"{node_id}:{vnode_index}"
    return xxHash64(seed) & 0xFFFFFFFF

// This ensures any client with the same ring config produces the same ring,
// and node positions are stable across restarts.

-- Hotspot Tracker (per Ring Manager) --
Key: /ring/v1/load_stats
Value: {
  "node_id": "node-b",
  "key_count": 1450000,
  "ops_per_sec": 125000,
  "expected_ops_per_sec": 50000,  // based on equal share of total
  "load_factor": 2.5,             // 2.5× expected → hotspot
  "timestamp": "2024-01-02T02:15:00Z"
}
```

### Database Choice

| Component | Option | Justification |
|---|---|---|
| Ring state (in-client) | Sorted array in memory | O(log N) lookup; fits in CPU cache |
| Ring configuration (persistent) | etcd | Raft consensus; Watch API for push updates; strong consistency needed for ring topology |
| Load statistics | Prometheus / time-series DB | High write rate (per-second per node); aggregation for hotspot detection |
| Migration state | PostgreSQL | Transactional tracking of key migrations in progress |

---

## 5. API Design

Consistent hashing is primarily a library, not a service. The API is the library interface plus the Ring Manager service API.

```python
# Client Library API (Python example; pattern applies to all languages)

class ConsistentHashRing:
    def __init__(self, nodes: list[Node], virtual_nodes_per_node: int = 150,
                 hash_fn: callable = xxhash64):
        """Initialize ring with given nodes and virtual node count."""

    def get_node(self, key: str) -> Node:
        """Return the primary node responsible for this key. O(log N)."""

    def get_nodes(self, key: str, count: int) -> list[Node]:
        """Return the N nodes responsible for this key (for replication).
           Returns the N distinct physical nodes clockwise from key's position."""

    def add_node(self, node: Node) -> MigrationPlan:
        """Add a node to the ring. Returns keys that must be migrated TO this node."""

    def remove_node(self, node_id: str) -> MigrationPlan:
        """Remove a node from the ring. Returns keys that must be migrated FROM this node."""

    def rebalance(self) -> MigrationPlan:
        """Rebalance virtual nodes to equalize load. Returns migration plan."""

    def get_ring_stats(self) -> RingStats:
        """Return load distribution statistics."""


class MigrationPlan:
    source_node: Node
    target_node: Node
    key_range: tuple[int, int]  # (start_hash, end_hash) of keys to migrate
    estimated_key_count: int


class RingStats:
    total_vnodes: int
    per_node: list[NodeStats]  # {node_id, vnode_count, hash_coverage_pct, key_count}
    std_dev_keys_pct: float    # distribution uniformity metric
    hotspot_nodes: list[str]   # nodes with load > 2× expected


# Ring Manager Service REST API

POST /v1/ring/nodes
  Auth: admin token
  Body: {"node_id": "node-e", "address": "10.0.1.9:6379", "weight": 2.0}
  Response: 202 Accepted {
    "migration_plan": {
      "keys_to_migrate": 150000,
      "source_nodes": ["node-b", "node-d"],
      "estimated_duration_seconds": 45
    },
    "ring_version": 43
  }

DELETE /v1/ring/nodes/{node_id}
  Auth: admin token
  Query: ?graceful=true&drain_timeout_seconds=300
  Response: 202 Accepted {"migration_plan": {...}, "ring_version": 44}

GET /v1/ring/config
  Response: 200 {"version": 43, "nodes": [...]}

GET /v1/ring/stats
  Response: 200 {
    "version": 43,
    "nodes": [
      {"node_id": "node-a", "vnode_count": 150, "key_coverage_pct": 5.2,
       "ops_per_sec": 52000, "expected_ops_per_sec": 50000, "load_factor": 1.04}
    ],
    "cluster_std_dev_pct": 3.2
  }

PUT /v1/ring/nodes/{node_id}/weight
  Body: {"weight": 2.0}  -- increase node weight; adds vnodes
  Response: 200 {"additional_vnodes": 150, "migration_plan": {...}}

POST /v1/ring/nodes/{node_id}/mark_down
  -- Immediately removes node from routing without migration (emergency)
  Response: 200 {"keys_now_unavailable": 150000, "ring_version": 45}

GET /v1/ring/key/{key}/node
  -- Debug: show which node currently owns this key
  Response: 200 {"key": "user:12345", "hash": "0xA3B4C5D6",
                  "node_id": "node-b", "vnode_id": "node-b:7",
                  "vnode_position": "0xA4000000"}
```

---

## 6. Deep Dive: Core Components

### 6.1 Virtual Nodes: Distribution Uniformity

**Problem it solves:** With only one ring position per physical node, N nodes divide the ring into N ranges of unequal size (because hash positions are random). One node might own 20% of the ring while another owns 3%. This causes significant load imbalance. Virtual nodes solve this by placing each physical node at multiple positions, making the distribution converge to uniform as the number of vnodes increases.

**Mathematical Analysis:**

For N physical nodes with no vnodes (1 position each):
- Expected key fraction per node: 1/N = 5% for N=20
- Standard deviation of fraction: high — with small N, the variance is O(1/N²) but the positions are random

With V vnodes per physical node:
- Total ring points = N × V
- By the law of large numbers, as V → ∞, each physical node's share → 1/N
- Standard deviation of key fraction: σ ≈ 1/(N × √V) × √(1/N)

**Simulation results for N=20 nodes:**

| Vnodes per node (V) | Total ring entries | Std dev of key distribution | Max/min ratio |
|---|---|---|---|
| 1 | 20 | ~20% | 8:1 |
| 10 | 200 | ~10% | 3:1 |
| 50 | 1,000 | ~5% | 2:1 |
| 100 | 2,000 | ~3.5% | 1.5:1 |
| **150** | **3,000** | **~2.8%** | **1.3:1** |
| 300 | 6,000 | ~2% | 1.2:1 |
| 1,000 | 20,000 | ~1% | 1.07:1 |

**Selected: 150 vnodes per node (standard in production Redis Cluster: 16,384 slots / 8 nodes ≈ 2,048 slots/node; Cassandra default: 256 vnodes/node)**

Justification: 150 vnodes provides < 3% standard deviation in key distribution while keeping the ring small enough (3,000 entries for 20 nodes) to fit in L2 cache. Increasing to 1,000 vnodes improves uniformity marginally but triples memory and migration overhead. The marginal return diminishes rapidly beyond 150.

**Vnode Position Computation Algorithm:**
```python
import xxhash

def compute_ring(nodes: list[Node], vnodes_per_node: int = 150) -> list[VNode]:
    ring = []
    for node in nodes:
        actual_vnodes = round(node.weight * vnodes_per_node)
        for i in range(actual_vnodes):
            # Deterministic, stable hash of "node_id:vnode_index"
            seed = f"{node.node_id}:{i}"
            position = xxhash.xxh32(seed).intdigest()  # 32-bit position
            ring.append(VNode(
                hash_position=position,
                vnode_id=f"{node.node_id}:{i}",
                physical_node=node
            ))
    
    # Sort by hash position for binary search
    ring.sort(key=lambda v: v.hash_position)
    return ring

def get_node(ring: list[VNode], key: str) -> Node:
    h = xxhash.xxh32(key).intdigest()
    
    # Binary search: find first vnode with position >= h
    lo, hi = 0, len(ring) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if ring[mid].hash_position < h:
            lo = mid + 1
        else:
            hi = mid
    
    # Wrap around: if h is greater than all positions, use ring[0]
    return ring[lo % len(ring)].physical_node

def get_nodes_for_replication(ring: list[VNode], key: str, n: int) -> list[Node]:
    """Return n distinct physical nodes clockwise from key's position."""
    h = xxhash.xxh32(key).intdigest()
    
    start_idx = binary_search_first_ge(ring, h)
    seen_nodes = set()
    result = []
    
    for offset in range(len(ring)):
        idx = (start_idx + offset) % len(ring)
        node = ring[idx].physical_node
        if node.node_id not in seen_nodes:
            seen_nodes.add(node.node_id)
            result.append(node)
        if len(result) == n:
            break
    
    return result
```

**Interviewer Q&As:**

Q: Why does consistent hashing move only 1/N keys when a node is added, and how does this compare to modular hashing?  
A: With modular hashing (`node = hash(key) % N`), adding a node changes N to N+1, which changes the assignment for approximately `N/(N+1) ≈ 1 - 1/(N+1) ≈ 100%` of keys (almost all keys move). For N=10, adding one node moves 10/11 ≈ 91% of keys. With consistent hashing, adding a node inserts its V vnodes into the ring. Each vnode "steals" some key range from its clockwise predecessor node. The total stolen range = 1/N of the full ring (since each new node takes 1/N of the total). Only keys in those stolen ranges need migration. Expected migration = 1/N of all keys — the theoretical minimum.

Q: How does the binary search work for ring lookup, and what is the edge case at the ring boundary?  
A: The ring is a sorted array of (hash_position, node) tuples. For key hash `h`, binary search finds the first entry with `hash_position >= h`. If `h` is greater than all positions (e.g., h = 0xFFFFFFFF and the maximum ring position is 0xF0000000), the search would return an out-of-bounds index. Handle this by wrapping: `idx = lo % len(ring)`. The key is then assigned to `ring[0]` (the node with the smallest hash position), which corresponds to "going past midnight on the clock and wrapping back to the start." This wrapping is what makes consistent hashing a "ring" rather than a line.

Q: What is the effect of using a poor hash function (e.g., MD5 on integer keys)?  
A: Hash function quality affects distribution uniformity. Requirements: (1) uniform distribution across the output space; (2) avalanche effect (small key change = completely different hash); (3) determinism. MD5 is cryptographically designed but slower than needed (hash functions like xxHash, MurmurHash3 are designed for distribution quality and speed). Using a hash function with clustering (certain input patterns produce similar outputs) creates a bimodal distribution: some vnodes receive far more keys than expected. Prefer xxHash3/xxHash32 or MurmurHash3 for consistent hashing; they are faster than MD5/SHA and have better distribution on short strings.

Q: How do virtual nodes affect migration when adding a new node?  
A: When adding a new node with V vnodes, each vnode is inserted at a unique position in the ring. For each new vnode `v_new` at position `P_new`, the keys in the range `(P_prev, P_new]` (where `P_prev` is the preceding vnode's position) must migrate from the preceding vnode's physical node to the new node. With 150 vnodes, 150 small key ranges are migrated from 150 different predecessor nodes. Typically these predecessors are different physical nodes (due to distribution), so migration is parallelized across many sources — much faster than a single large migration. Total keys migrated remains 1/N.

Q: How do you verify that the ring is correctly balanced after adding/removing nodes?  
A: Calculate the coefficient of variation (CV = σ/μ) of key counts per physical node. Acceptable: CV < 0.05 (5%). If higher, either the vnode count is too low or the hash function is producing clusters. Also verify by computing the hash coverage percentage per node: `sum(vnode_range_sizes_for_node) / 2^32`. Ideal: each node covers `100% / N` of the space. Tool: `GET /v1/ring/stats` returns per-node coverage percentages and the cluster-wide standard deviation.

---

### 6.2 Node Addition & Removal: Key Migration

**Problem it solves:** When the ring topology changes (node added or removed), keys must be moved to their new responsible nodes. This migration must happen without service disruption, must be bounded in scope (only affected keys move), and must handle in-flight requests correctly during the transition.

**Approaches Comparison:**

| Approach | Mechanism | Service Impact | Complexity |
|---|---|---|---|
| **Two-phase: add vnode (shadowing) + migrate + switch** | New node shadows old; clients use old until migration done; atomic switch | Near-zero | Medium |
| **Write-ahead: read from both, write to new** | During migration, writes go to new node; reads check both | Near-zero | Medium |
| **Stop-the-world migrate** | Stop writes, migrate all keys, resume | Downtime proportional to dataset size | Low |
| **Lazy migration (read-repair)** | Keys migrate on first access to new node | Gradual; eventual consistency | Low complexity, delayed convergence |
| **Double-write during migration** | Writes go to both old and new node | Increased write load; no read disruption | Medium |

**Selected: Double-write during migration + read from new, fallback to old**

**Node Addition Protocol:**
```
Phase 1: PREPARE (ring manager announces new node, no routing change yet)
  1. Ring Manager adds node-E to ring config with status=JOINING
  2. Broadcasts ring version N+1 with JOINING flag to all clients
  3. Clients add node-E to ring but do not route to it yet (still route to predecessor)
  4. Application double-writes: for every write to a key that will belong to node-E,
     also write to node-E. This ensures node-E gets up-to-date data as it joins.

Phase 2: MIGRATE (bulk copy of existing keys)
  5. Migration daemon on node-E pulls keys in its target hash ranges from predecessor nodes
  6. For each vnode being transferred: stream all keys with hash in that range
     source_node.scan(min_hash, max_hash) → target_node.bulk_write()
  7. Migration is throttled to avoid impacting live traffic
     (e.g., max 50 MB/sec migration bandwidth per node)

Phase 3: SWITCH (atomic cutover)
  8. Migration daemon signals "migration complete" for each vnode range
  9. Ring Manager atomically updates ring config: status=JOINING → ACTIVE for all node-E vnodes
  10. Broadcasts ring version N+2 with ACTIVE flag
  11. Clients update routing: now route to node-E for its key ranges
  12. Reads from node-E may still miss keys not yet migrated (race condition window)
      → Fall back to predecessor if miss (for a brief window post-switch)
  13. After 2× max replication lag time, stop double-writing and reading from predecessor

Phase 4: CLEANUP
  14. Predecessor nodes delete keys that have migrated to node-E
      (can be done lazily over time; old keys are stale but not harmful if expired)

def migrate_vnode_range(source_node, target_node, min_hash, max_hash, batch_size=1000):
    cursor = None
    total_migrated = 0
    
    while True:
        # SCAN is non-blocking (unlike KEYS); won't block source node event loop
        keys, cursor = source_node.scan_by_hash_range(min_hash, max_hash,
                                                        cursor=cursor,
                                                        count=batch_size)
        if not keys:
            break
        
        # Fetch values and TTLs from source
        pipe = source_node.pipeline()
        for key in keys:
            pipe.dump(key)   # serialize with TTL
        serialized = pipe.execute()
        
        # Restore on target
        restore_pipe = target_node.pipeline()
        for key, data in zip(keys, serialized):
            ttl = source_node.pttl(key)
            if ttl > 0:  # only migrate non-expired keys
                restore_pipe.restore(key, ttl, data, replace=True)
        restore_pipe.execute()
        
        total_migrated += len([d for d in serialized if d is not None])
        
        if cursor == 0:
            break  # scan complete
    
    return total_migrated
```

**Node Removal Protocol:**
```
Phase 1: DRAIN
  1. Ring Manager marks node-X as LEAVING in ring config
  2. Clients stop sending new requests to node-X for key ranges being migrated
     (but may still read from node-X for keys not yet migrated)
  3. New writes for node-X's key ranges go to its successor node(s) on the ring

Phase 2: MIGRATE
  4. Migration daemon copies node-X's keys to successor nodes (reverse of addition)
  5. Rate-limited bulk copy

Phase 3: REMOVE
  6. Ring Manager removes node-X from ring: ring version N+3
  7. Clients update ring to exclude node-X completely
  8. node-X goes offline safely

# Double-write implementation (during addition/removal):
class DoubleWriteRouter:
    def set(self, key, value, ttl):
        primary_node = ring.get_node(key)
        migration_node = migration_tracker.get_migration_target(key)
        
        primary_node.set(key, value, ttl)
        
        if migration_node and migration_node != primary_node:
            # Best-effort double-write; failure is acceptable
            # (migration daemon will catch up)
            try:
                migration_node.set(key, value, ttl)
            except Exception:
                pass  # non-critical; migration will reconcile

    def get(self, key):
        primary_node = ring.get_node(key)
        result = primary_node.get(key)
        
        if result is None and migration_tracker.is_migration_in_progress(key):
            # Fall back to old node during migration window
            old_node = migration_tracker.get_migration_source(key)
            if old_node:
                result = old_node.get(key)
        
        return result
```

**Interviewer Q&As:**

Q: How many keys are migrated when adding a node to a cluster of 20 nodes?  
A: In theory: 1/N of all keys = 1/21 × total_keys ≈ 4.8% of keys (with 21 nodes after addition). With 100M total keys: 4.8M keys migrated. At 50MB/sec migration bandwidth and 1KB average value: 50,000 keys/sec migrated → 4.8M / 50,000 = **96 seconds** of migration time. With 150 vnodes, migration is parallelized across ~150 predecessor vnode sources, but each source is throttled. In practice, Redis Cluster migration using `MIGRATE` command achieves this in 1–5 minutes for a typical cache dataset.

Q: What is the "fallback read" pattern and when should it be disabled?  
A: During migration (node being added), some keys may not yet be present on the new node. If a client reads a key and gets a miss on the new node, it should fall back to reading from the old (predecessor) node. This "fallback read" window should close once migration is complete AND a grace period has elapsed (e.g., 2× the maximum migration lag). After the grace period, all reads go to the new node without fallback. Disable fallback too early → cache misses during migration (acceptable for caches, bad for authoritative stores). Disable fallback too late → unnecessary double-reads consuming bandwidth.

Q: How do you handle a node addition where the new node's migration connection to the source is slow?  
A: Set a migration SLA: if migration isn't complete within T minutes, alert ops. Accelerate by increasing migration threads or bandwidth limit temporarily. If the source node is under heavy live traffic, use the time-of-low-traffic (off-peak) for migration. For Redis specifically, the `MIGRATE` command is synchronous — use asynchronous migration with explicit batching and pipeline to avoid head-of-line blocking. The new node can start serving fresh writes immediately; only old keys that haven't been migrated yet need the fallback path.

Q: How does consistent hashing relate to Dynamo-style databases (Cassandra, DynamoDB)?  
A: Amazon Dynamo (2007 SOSP paper) popularized consistent hashing for database partitioning. Cassandra uses consistent hashing with vnodes (introduced in Cassandra 1.2): each node gets 256 vnodes by default. The ring maps to token ranges, and data is placed on the node owning the token for the partition key. DynamoDB uses consistent hashing internally for partitioning but abstracts this from users behind a managed service. The key extensions Dynamo added over basic consistent hashing: preference lists (N-node replication with clockwise successors), sloppy quorum (hinted handoff), and vector clocks for conflict resolution.

Q: What is token-based consistent hashing (Cassandra vnodes model) vs range-based partitioning?  
A: Token-based (consistent hashing): each row's primary key is hashed to a token (position on ring); the node owning that token range stores the row. Vnodes spread each physical node across multiple token ranges. Range-based (e.g., HBase, MongoDB chunk-based): the keyspace is divided into contiguous ranges by value (not hash); a lookup table maps ranges to nodes. Range-based allows range scans (give me all users with IDs 1000–2000) but causes hotspots on sequential keys. Consistent hashing distributes uniformly but makes range scans across nodes expensive (must query all nodes). Choose based on access pattern: use consistent hashing for even distribution of point lookups; use range partitioning when ordered range scans are critical.

---

### 6.3 Hotspot Mitigation & Weighted Consistent Hashing

**Problem it solves:** Even with perfect hash function and adequate vnodes, real-world workloads have "celebrity" keys — a small number of keys receive disproportionately high traffic (Zipf distribution). Additionally, nodes may have different capacities (different machine sizes), requiring weighted distribution.

**Hotspot Types:**

| Type | Cause | Example | Mitigation |
|---|---|---|---|
| **Hot key (single key)** | Extreme traffic on one key | Viral post, popular product ID | Key replication, local cache |
| **Hot vnode (hash bucket)** | Cluster of keys hashing to same region | Sequential keys, poor hash function | Hash function change, key scrambling |
| **Hot node (capacity mismatch)** | Node has fewer vnodes than its capacity warrants | Smaller node receiving 1/N traffic | Weighted vnodes based on capacity |
| **Temporal hotspot** | Traffic spikes at specific times | End-of-day report trigger | Pre-warming, autoscaling |

**Weighted Consistent Hashing:**
```python
def build_weighted_ring(nodes: list[Node], base_vnodes: int = 150) -> list[VNode]:
    """
    Allocate vnodes proportional to node weight (capacity).
    
    Example:
    - node-a: weight=1.0 → 150 vnodes → covers ~1/3 of ring
    - node-b: weight=1.0 → 150 vnodes → covers ~1/3 of ring
    - node-c: weight=2.0 → 300 vnodes → covers ~2/3×(1/total_weight) of ring
    
    Total weight = 1.0 + 1.0 + 2.0 = 4.0
    Expected coverage:
    - node-a: 1.0/4.0 = 25%
    - node-b: 1.0/4.0 = 25%
    - node-c: 2.0/4.0 = 50%
    """
    ring = []
    total_weight = sum(n.weight for n in nodes)
    
    for node in nodes:
        # Vnodes proportional to weight
        vnode_count = round(node.weight / total_weight * base_vnodes * len(nodes))
        node.actual_vnode_count = vnode_count
        
        for i in range(vnode_count):
            position = xxhash.xxh32(f"{node.node_id}:{i}").intdigest()
            ring.append(VNode(position, node))
    
    ring.sort(key=lambda v: v.hash_position)
    return ring

# Hot Key Detection and Mitigation:
class HotKeyMitigator:
    def detect_hot_keys(self, ops_per_key: dict) -> list[str]:
        """Identify keys receiving > 10× the average ops rate."""
        total_ops = sum(ops_per_key.values())
        avg_ops = total_ops / len(ops_per_key) if ops_per_key else 0
        hot_threshold = avg_ops * 10
        return [key for key, ops in ops_per_key.items() if ops > hot_threshold]

    def mitigate_hot_key(self, key: str, strategy: str):
        if strategy == "LOCAL_CACHE":
            # L1 cache in each app server with short TTL (5-30s)
            # Effective for read-heavy hot keys
            # Drawback: all app servers cache it separately (N × memory)
            return LocalCacheStrategy(key, ttl=10)

        elif strategy == "SCATTER_READ":
            # Replicate key to K random ring positions (key + ":shard_0" through ":shard_K-1")
            # Reads choose a shard randomly: hash(key + random_int(0, K)) → different node
            # Writes update all K shards atomically
            K = 10
            shards = [f"{key}:shard_{i}" for i in range(K)]
            return ScatterReadStrategy(shards)

        elif strategy == "DYNAMIC_VNODE_SPLIT":
            # Move the hot vnode to a dedicated micro-node
            # Only applicable for server-managed sharding (not client-side)
            hot_vnode = ring.get_vnode_for_key(key)
            micro_node = provision_new_node(small_instance=True)
            ring.transfer_vnode(hot_vnode, micro_node)
            return VnodeSplitStrategy(hot_vnode, micro_node)

# Jump Consistent Hash (alternative algorithm):
def jump_consistent_hash(key: str | int, num_buckets: int) -> int:
    """
    Jump Consistent Hash (Lamping & Veach, 2014).
    Maps key to bucket in [0, num_buckets) with minimal key movement on resize.
    O(ln N) time, O(1) space — no ring data structure needed.
    
    Key property: when num_buckets increases by 1, only 1/num_buckets keys move.
    Limitation: only supports adding nodes at the END (buckets numbered sequentially).
                Cannot remove arbitrary nodes (only the last bucket).
    """
    if isinstance(key, str):
        key = int(xxhash.xxh64(key).intdigest())
    
    b, j = -1, 0
    while j < num_buckets:
        b = j
        key = ((key * 2862933555777941757) + 1) & 0xFFFFFFFFFFFFFFFF  # LCG step
        j = int((b + 1) * (1 << 31) / ((key >> 33) + 1))
    return b

# Rendezvous Hashing (Highest Random Weight):
def rendezvous_hash(key: str, nodes: list[Node]) -> Node:
    """
    For each node, compute hash(key + node_id). Pick node with highest hash.
    Property: when a node is removed, only keys assigned to it are remapped (1/N keys).
    O(N) time — not suitable for large N (N > 1000), but excellent for small N.
    No ring needed; deterministic without shared state.
    """
    scores = {}
    for node in nodes:
        seed = f"{key}:{node.node_id}"
        scores[node] = xxhash.xxh64(seed).intdigest()
    return max(scores, key=lambda n: scores[n])
```

**Interviewer Q&As:**

Q: When would you choose Jump Consistent Hash over ring-based consistent hashing?  
A: Jump Consistent Hash is ideal when: (1) nodes are always added in sequence (never arbitrary removals — only the highest-numbered bucket can be removed); (2) all nodes have equal capacity; (3) you want O(1) space and O(ln N) time with zero shared state (no ring to maintain). Use cases: sharding a background job across N identical workers, partitioning analytics buckets across N processing threads, any system where nodes are added sequentially and removals are rare. Use ring-based consistent hashing when: arbitrary node removals are needed, nodes have different capacities (weighted), or a custom vnode layout is required.

Q: How does rendezvous hashing compare to ring-based consistent hashing?  
A: Rendezvous hashing (Highest Random Weight) assigns a key to the node with the highest `hash(key + node_id)`. On node removal, only keys on the removed node are remapped (same 1/N property as ring-based). No ring data structure needed — just the list of nodes. Drawback: O(N) per lookup (must compute hash for all nodes). Suitable for small N (< 100 nodes). Ring-based consistent hashing achieves O(log N) per lookup, suitable for large N (thousands of vnodes). For a distributed cache with 20 nodes, rendezvous hashing is perfectly practical.

Q: Explain how Cassandra's token-range allocation with vnodes differs from the original Dynamo approach.  
A: Original Dynamo used one token per node (no vnodes), where each node was assigned a single random token. This caused unequal range distribution (one node might own 30% of the ring, another only 5%). When adding a node, a single large migration from one predecessor occurred. Cassandra 1.2+ introduced vnodes: each node gets 256 random tokens (virtual nodes), distributing its ownership across 256 ring segments. This provides better load balance and smaller, more parallel migration on topology change. The tradeoff: 256 vnodes per node creates more metadata (tokens to track) and more Gossip overhead. Newer Cassandra versions allow configuring vnodes_per_node.

Q: How do you handle a hotspot caused by sequential keys (e.g., timestamps)?  
A: Sequential keys (timestamps, auto-increment IDs) cluster in one region of the hash space under a poor hash function or range partitioning. With consistent hashing using a good hash function (xxHash, MurmurHash3), sequential keys should distribute well because the hash function produces uniformly distributed outputs. Verify: if `hash(timestamp_1)` and `hash(timestamp_1 + 1)` are in completely different ring regions (good avalanche effect), sequential keys distribute uniformly. If using range partitioning instead of consistent hashing, prefix keys with a random shard prefix: `{random_prefix}_{timestamp}` to scatter.

Q: What is the "split-brain" problem for consistent hashing when clients have different ring views?  
A: If some clients have ring version N (with old topology) and others have ring version N+1 (with new topology, after adding a node), they route the same key to different nodes. This is acceptable for a cache (miss on the new node → fallback to predecessor → cache miss, not data corruption). For a database shard, split-brain routing causes data inconsistency: a write from client A goes to Node-X (old ring), a write from client B for the same key goes to Node-Y (new ring), now two nodes have different values. Mitigation: use a two-phase ring update (write-ahead: make new node a secondary before routing, promote only after migration complete); or use coordinator-side routing (all requests go through a proxy with a single ring view).

---

## 7. Scaling

### Horizontal Scaling
Consistent hashing IS the horizontal scaling mechanism. Adding nodes:
- Increases total capacity by 1/N (if all nodes equal)
- Migrates 1/N of keys from predecessors
- Done live without cluster-wide restart

Scaling from 20 to 40 nodes: doubles storage capacity, halves per-node load. Each addition migrates ~2.5% of keys.

### Ring Size and Lookup Scalability

Ring lookup time = O(log(N × V)) where N=physical nodes, V=vnodes per node.

| Cluster size | Vnodes per node | Total ring entries | Lookup time (comparisons) |
|---|---|---|---|
| 10 nodes | 150 | 1,500 | log2(1500) ≈ 11 |
| 100 nodes | 150 | 15,000 | log2(15000) ≈ 14 |
| 1,000 nodes | 150 | 150,000 | log2(150000) ≈ 17 |
| 10,000 nodes | 150 | 1,500,000 | log2(1.5M) ≈ 21 |

At 21 comparisons × 10 ns = 210 ns per lookup — still negligible. The ring can scale to 10,000 physical nodes with acceptable lookup cost. Memory: 1.5M entries × 16 bytes = 24 MB — fits in L3 cache.

### Replication
For each key, use `get_nodes(key, n=3)` to get the 3 clockwise distinct physical nodes. This provides `n-1 = 2` redundant copies. The primary node (first in the list) handles writes; replicas receive async copies. On primary node failure, the second node in the preference list takes over.

### Caching Ring State
The ring is already in-memory (it IS the cache routing layer). Ring updates are pushed via gossip. For ring manager HA: store ring config in etcd (Raft-replicated), serve via gRPC with Watch for push notifications to clients.

### Interviewer Q&As (Scaling)

Q: How does consistent hashing handle a 10× sudden traffic spike on one node?  
A: Consistent hashing itself doesn't handle it — it's a routing algorithm, not a load balancer. Mitigations at higher layers: (1) Local in-process cache (L1) on each app server reduces load on the hot node. (2) Move some of the hot node's vnodes to other nodes (dynamic vnode migration). (3) Replicate hot keys to multiple nodes and randomly select one for reads. (4) Use adaptive routing: monitor per-node RPS; if a node exceeds a threshold, route a fraction of requests to the next clockwise node (at the cost of slightly worse hit rate for consistency). (5) Provision a temporary node and migrate hot vnodes to it.

Q: How do you propagate ring changes to 10,000 clients in a large microservices cluster?  
A: Three approaches: (1) **Gossip protocol:** each client gossips with K=3 random peers; change propagates in O(log N) rounds ≈ 10–15 rounds for 10,000 clients → propagation time ≈ 15 × gossip_interval (typically 1s each) = 15 seconds. (2) **etcd Watch:** all clients subscribe to a Watch on `/ring/config`. etcd pushes ring updates via HTTP2 streaming; all 10,000 clients receive the update within milliseconds. Scales to tens of thousands of watchers. (3) **Versioned pull:** clients periodically poll (every 5 seconds) for ring version; if version changed, fetch updated ring. Simpler but has up to 5-second staleness. Best for large deployments: etcd Watch for immediacy, gossip for resilience.

Q: What is the relationship between consistent hashing and the CAP theorem?  
A: Consistent hashing is a routing algorithm, not a consistency model — it doesn't directly interact with CAP. However, the systems built on consistent hashing make CAP trade-offs: Dynamo/Cassandra (AP): during partitions, accept stale reads and eventual consistency; consistent hashing ensures minimal data movement but doesn't guarantee linearizability. DynamoDB (now CP for individual items via conditional writes): uses consistent hashing for partitioning but adds single-master per partition for strong consistency. The choice of CAP trade-off is made at the replication and read/write protocol layer, not the routing layer.

Q: How do you test consistent hashing implementation for correctness?  
A: (1) **Distribution uniformity test:** insert 1M random keys, count keys per physical node; assert standard deviation < 5% of mean. (2) **Minimal migration test:** take a snapshot of 10K key-to-node assignments; add one node; verify exactly 1/N of keys moved to the new node (within statistical tolerance). (3) **Wrap-around test:** generate key that hashes to the maximum uint32 value (0xFFFFFFFF); verify it routes to ring[0]. (4) **Determinism test:** rebuild ring from scratch with same input nodes; verify identical vnode positions. (5) **Weighted distribution test:** add a node with weight=2.0; verify it receives ~twice the keys of a weight=1.0 node.

Q: How does Amazon Dynamo handle "hot nodes" with consistent hashing?  
A: The original Dynamo paper acknowledges that physical node heterogeneity (different machine capacities) and hotspot keys both challenge pure consistent hashing. Solutions described: (1) Virtual nodes: multiple tokens per physical node, with the count proportional to machine capacity. (2) Preference lists: for replication, walk clockwise on the ring to find N distinct physical nodes (skipping vnodes on the same physical node). (3) Sloppy quorum + hinted handoff: if a ring node is temporarily down, route to the next healthy node with a "hint" that the data belongs elsewhere; the hinted node delivers the data back when the target recovers.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Detection | Recovery |
|---|---|---|---|
| Ring client has stale ring view | Routes to wrong node (old primary) | Response from wrong node: NOT_OWNER error | Client refreshes ring from Ring Manager; retries request |
| Physical node crash (no replication) | All keys in its range unavailable | Health check / connection refused | Ring update removes node; keys rehomed to successor (cache miss storm) |
| Physical node crash (with replication) | Primary unavailable; reads/writes to secondary | Health check | Ring update promotes secondary; no data loss |
| Ring Manager crash | Clients cannot learn of ring changes | Ring Manager health check | HA Ring Manager (3 nodes via etcd); clients continue with cached ring |
| Split ring view (network partition) | Some clients route differently than others | Metrics: unexpected NOT_OWNER responses | Ensure ring updates use versioned atomic updates; wait for all clients to converge |
| vnode collision (two nodes hash to same position) | One vnode's range drops to zero | Ring build check: assert no duplicate positions | xxHash32 on 32-bit space has ~50% collision probability at 65K vnodes (birthday paradox); for 3K vnodes, collision probability ≈ (3000²)/(2 × 2^32) ≈ 0.1% — acceptable; detect and regenerate affected vnodes |
| Migration failure (source crash mid-migration) | Some keys not yet copied to target | Migration status tracker | Resume migration from last checkpoint; fallback reads still work |

**Ring Update Consistency Protocol:**
```
Ring Manager:
1. Write new ring config to etcd (version N+1) atomically
2. All etcd Watch subscribers receive push notification immediately
3. Client library validates version > current_version before applying
4. Client applies new ring atomically (swap pointer to new ring object)
5. Old ring object GC'd after all in-flight requests complete (grace period)

Version validation prevents applying stale updates:
if new_ring_version <= current_ring_version:
    discard new update (already up to date or replay)
```

---

## 9. Monitoring & Observability

| Metric | Source | Alert Threshold | Meaning |
|---|---|---|---|
| `keys_per_node` (distribution) | Ring stats API | Std dev > 10% of mean | Ring imbalance; increase vnodes |
| `ops_per_node` (request rate) | Per-node metrics | Any node > 2× average | Hot node; investigate key distribution |
| `ring_version_lag` per client | Client metrics | Any client > 2 versions behind | Ring update not propagating; check gossip |
| `not_owner_errors` per node | Node error logs | > 0.1% of requests | Client has stale ring; routing to wrong node |
| `migration_progress` | Migration tracker | < 10% progress after 1h | Migration stalled; check node health |
| `cache_miss_rate` spike | Cache metrics | > 2× baseline miss rate | Node added/removed recently; expected transient |
| `vnode_coverage_std_dev` | Ring stats | > 5% | Uneven vnode distribution; possible hash function issue |
| `hot_key_ops_per_sec` | LFU key stats | > 10× average key ops rate | Hot key detected; apply local cache or scatter |

**Distributed Tracing:**
- Embed `ring_version` in request headers; log at target node
- Detect when request arrives at a node that is NOT the current owner (ring_version mismatch)
- Trace full routing path: client → ring lookup → node → (if NOT_OWNER) → retry with refresh

**Logging:**
- Ring Manager: log every ring version change with diff (nodes added/removed, vnode count changes)
- Client library: log ring version updates at DEBUG; log NOT_OWNER redirects at INFO
- Migration daemon: log per-vnode migration start/complete/failed with key counts and duration

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Alternative | Reason |
|---|---|---|---|
| Ring representation | Sorted array + binary search | Binary search tree / skip list | Array has better cache locality; O(log N) same asymptotically; simpler |
| Hash function | xxHash32/64 | MD5, SHA-1, MurmurHash3 | Fastest with excellent distribution; no cryptographic overhead needed |
| Vnode count | 150 per node | 1 (no vnodes) / 1000 | 150 gives < 3% std dev with 20 nodes; diminishing returns beyond 300 |
| Vnode position computation | Deterministic (hash of node_id:vnode_idx) | Random (assigned at ring join time) | Deterministic: any client rebuilds identical ring from same config; testable |
| Ring state storage | etcd with Watch API | Gossip only | etcd provides authoritative, consistent ring state; gossip provides propagation |
| Node addition protocol | Double-write + fallback read | Stop-the-world migration | Zero downtime; handles in-flight requests during migration |
| Weighted nodes | Proportional vnode count | Fixed equal vnodes | Supports heterogeneous hardware without waste |
| Hotspot mitigation | Local L1 cache + scatter read | Dynamic vnode split | L1 cache is universal and free; scatter read adds write overhead but is effective |
| Jump consistent hash | Ring-based chosen | Jump consistent hash | Ring supports arbitrary node removal; jump only supports removing the last node |
| Replication routing | Clockwise N-next distinct nodes | Random N nodes | Locality-preserving; predictable failover path |

---

## 11. Follow-up Interview Questions

Q1: What is consistent hashing and why was it invented?  
A: Consistent hashing (Karger et al., 1997) was invented to solve the distributed web caching problem: how to route requests to N cache servers such that adding or removing a server doesn't invalidate the entire cache. Before consistent hashing, modular hashing (`hash(key) % N`) was used, but changing N (adding/removing a server) remapped almost every key, causing a cache miss storm. Consistent hashing maps both keys and servers to a shared hash ring and routes each key to the nearest clockwise server, ensuring only 1/N keys move when N changes.

Q2: Explain how consistent hashing is used in DynamoDB.  
A: DynamoDB partitions data using consistent hashing on the partition key. Internally, DynamoDB's storage nodes form a ring; each partition (unit of data storage) is assigned to a token range on the ring. Multiple partition replicas are placed on clockwise nodes. When a storage node is added or removed, DynamoDB migrates only the affected key ranges. The system is abstracted from users (no user-visible shard keys or partition counts) — DynamoDB's control plane automatically manages the ring topology and rebalancing.

Q3: How does Cassandra use consistent hashing and how does it differ from this design?  
A: Cassandra uses consistent hashing with configurable vnodes (default 256 per node). Each node is assigned 256 random tokens (positions on the ring). Data is placed based on the partition key's Murmur3 hash. Cassandra's ring is maintained via a gossip protocol (no central Ring Manager). Each node knows the full ring topology. Replication: for a replication factor of 3, the row is written to the first 3 distinct nodes clockwise from the key's position. Differences from this design: Cassandra is decentralized (gossip-only, no etcd); it has a built-in tunable replication factor and consistency level per operation.

Q4: What are the limitations of consistent hashing?  
A: (1) **No global ordering:** consistent hashing distributes keys uniformly but doesn't preserve key ordering. Range scans across all nodes require querying all nodes. (2) **Hot keys still happen:** consistent hashing distributes keys uniformly across nodes, but if one key receives 100× more traffic than average, that key's node is overloaded regardless of ring balance. (3) **Ring state must be consistent:** all clients must have the same ring view, or they route to different nodes for the same key. Managing ring state consistency at scale requires infrastructure. (4) **No awareness of data:** consistent hashing doesn't know if a key is "large" (1GB) or "small" (100 bytes); it routes all keys equally, potentially creating storage imbalance if value sizes vary wildly.

Q5: How do you handle the case where a key should be served by the nearest node geographically, not just the next clockwise node?  
A: Build a geography-aware routing layer on top of consistent hashing. Option 1: maintain separate rings per geographic region; route requests to the local ring first (local node), fall back to the global ring on miss. Option 2: use geospatial hashing (Geohash) as the key prefix for location-sensitive data; nodes in a geographic region own a contiguous Geohash prefix, and the ring is organized around geographic ranges rather than random hash positions. Option 3: use latency-based routing at the load balancer (Anycast) to direct requests to the nearest datacenter; within the datacenter, use standard consistent hashing.

Q6: What happens to in-flight requests during a ring update?  
A: In-flight requests that started routing before the ring update complete against the old ring topology. The target node may return "NOT OWNER" if it no longer owns the key (migration complete, ring updated). The client should handle this by refreshing its ring view and retrying the request against the new correct node. This is a brief inconsistency window — typically < 100ms — during which some requests may require a retry. For the migration window (potentially minutes), the fallback read pattern handles requests that hit the new node before migration is complete.

Q7: How does consistent hashing relate to sharding in relational databases?  
A: Database sharding is a form of horizontal partitioning where rows of a table are distributed across multiple DB instances based on a shard key. Consistent hashing is one sharding strategy: `shard = consistent_hash(primary_key) % num_shards`. Alternatives: range-based sharding (shard 1 owns rows with ID 1–1M, shard 2 owns 1M–2M), directory-based sharding (a lookup table maps each entity to its shard). Consistent hashing provides better rebalancing (1/N movement) vs range sharding (potentially large range moves) but loses the ability to do ordered range scans within a shard.

Q8: What is the "hot shard" problem and how do you address it in a database context?  
A: A hot shard occurs when a specific shard receives disproportionate query volume. Causes: popular entities (a celebrity's user row), time-series hotspots (current-time rows), or business events (end-of-quarter report shard). Unlike a cache where a hot key is tolerable (just a latency issue), a hot DB shard can cause write bottlenecks, lock contention, and replica lag. Solutions: (1) Re-shard the hot entity to its own dedicated shard ("micro-shard"). (2) Use in-memory caching for read-heavy hot entities. (3) Denormalize hot data to reduce write amplification. (4) Redesign the access pattern to avoid concentrating traffic on one shard.

Q9: How would you implement consistent hashing in a language without built-in sorted data structures?  
A: Implement a binary search on a sorted array manually. Sort the ring array by hash_position after any modification. Binary search: `while lo < hi: mid = (lo+hi)//2; if ring[mid].hash < h: lo=mid+1; else: hi=mid`. Alternatively, use a sorted list data structure (e.g., Java TreeMap, Go's sort.Search, Python's bisect module). The key operations are: (1) insert a new vnode at the correct sorted position (O(log N) for trees, O(N) for arrays — acceptable since ring updates are rare), (2) lookup the successor of a hash value (O(log N) binary search).

Q10: Explain the "chord" distributed hash table algorithm and how it relates to consistent hashing.  
A: Chord (Stoica et al., 2001) is a structured P2P network based on consistent hashing. It organizes N nodes in a ring (each with a unique ID = SHA-1 of its IP), uses consistent hashing to partition the keyspace, and adds a "finger table" (routing table with O(log N) entries) at each node for O(log N)-hop routing. Unlike a centralized consistent hash ring (where a client library holds the full ring), Chord routes queries hop-by-hop through the P2P network — each node knows only O(log N) other nodes but can still reach any node in O(log N) hops. Chord is the theoretical basis for many P2P systems (BitTorrent DHT, Kademlia); practical distributed caches use centralized ring management for better control and simpler implementation.

Q11: How does consistent hashing compare to sharding by user_id range (range-based partitioning)?  
A: Range-based: shard by `user_id` range (shard 1: 0–1M, shard 2: 1M–2M, etc.). Pros: range scans within shard are efficient; easy to reason about data locality. Cons: sequential inserts create hotspots (all new users go to the latest shard); adding a shard requires splitting an existing range (complex). Consistent hashing: shard by `hash(user_id)`. Pros: uniform distribution regardless of key patterns; adding a shard moves only 1/N keys. Cons: no range scans across shards; harder to reason about where a specific key lives. Hybrid: shard by hash(user_id) at the storage layer; for range queries, use a secondary index or fan-out across all shards.

Q12: What is "ketama" consistent hashing used in Memcached clients?  
A: Ketama (libketama) is the consistent hashing implementation standardized in Memcached client libraries (Java spymemcached, PHP memcached extension). It creates 160 vnodes per server using SHA-1: for each server, generate 40 hashes of `{server_ip}:{server_port}-{replica_index}`, each producing 4 bytes = 160 ring points. Ring lookup: MD5/SHA-1 of the key, binary search for successor. Ketama is the de-facto standard for Memcached client-side consistent hashing. Modern systems prefer xxHash/MurmurHash3 over SHA-1 for better performance, but Ketama's compatibility mode is important for cross-language client interoperability.

Q13: How does consistent hashing interact with database transactions?  
A: Single-key transactions: straightforward — route to the node owning the key; execute transaction locally. Multi-key transactions where all keys are on the same node (using hash tags / colocating): also straightforward. Multi-key transactions across different nodes: requires distributed transactions (2PC, Saga, or CRDT-based merging). Consistent hashing complicates cross-shard transactions because it doesn't guarantee any two keys are on the same node. Design pattern: route related entities to the same shard using a "tenant" or "aggregate root" consistent hash key: all data for user_12345 uses hash(user_12345) for routing, ensuring all user data is on one shard.

Q14: What is "virtual bucket" consistent hashing and why do some systems (Redis Cluster) prefer a fixed number of slots?  
A: Redis Cluster uses 16,384 fixed hash slots rather than a ring with arbitrary node positions. This is a variant of consistent hashing with a "virtual bucket" approach. Every key maps to `slot = CRC16(key) % 16384`. Slot-to-node mapping is a lookup table (not a ring). When a node is added, N slots are reassigned to the new node. This provides: (1) deterministic, simple mapping (no binary search needed); (2) easy migration (move slot X from node A to node B); (3) fixed number of migration units (16,384) regardless of key count. The downside: 16,384 is fixed — you can't have more than 16,384 nodes (1 slot minimum per node). For practical clusters (< 1,000 nodes), this is not a limitation.

Q15: How would you design a consistent hashing solution for a geo-distributed system (multiple regions)?  
A: Option 1: region-local rings. Each region has an independent ring. Write to local region; async replicate cross-region. Reads served locally (possibly stale). Option 2: global ring with region-aware placement. Assign vnodes to specific regions: `node: {region}-{node_id}`. When routing, prefer nodes in the client's region (using region-aware ring view). Fall back to other regions if local node is down. Option 3: hierarchical consistent hashing. First-level hash: map key to a region (based on user geography or data sovereignty rules). Second-level hash: within the region, use consistent hashing to map to a specific node. Write crosses regions (dual-write for disaster recovery); reads are region-local. For latency-sensitive systems, option 3 is recommended as it minimizes cross-region hops.

---

## 12. References & Further Reading

- **Karger et al. — "Consistent Hashing and Random Trees" (1997 ACM STOC):** https://www.cs.princeton.edu/courses/archive/fall09/cos518/papers/chash.pdf
- **Amazon Dynamo — "Dynamo: Amazon's Highly Available Key-value Store" (SOSP 2007):** https://www.allthingsdistributed.com/files/amazon-dynamo-sosp2007.pdf
- **Lamping & Veach — "A Fast, Minimal Memory, Consistent Hash Algorithm" (Jump Consistent Hash, 2014):** https://arxiv.org/abs/1406.2294
- **Rendezvous Hashing (HRW) — Thaler & Ravishankar (1998):** https://www.eecs.umich.edu/techreports/cse/96/CSE-TR-316-96.pdf
- **Chord: A Scalable Peer-to-peer Lookup Service — Stoica et al. (2001 SIGCOMM):** https://pdos.csail.mit.edu/papers/chord:sigcomm01/chord_sigcomm.pdf
- **Cassandra Documentation — Virtual Nodes:** https://cassandra.apache.org/doc/latest/cassandra/architecture/dynamo.html
- **Redis Cluster Specification — Hash Slots:** https://redis.io/docs/reference/cluster-spec/#hash-slots
- **libketama — Consistent Hashing for Memcached clients:** https://github.com/RJ/ketama
- **Designing Data-Intensive Applications — Kleppmann, Chapter 6 (Partitioning):** O'Reilly, 2017
- **MIT 6.824 Distributed Systems — Consistent Hashing Lecture Notes:** https://pdos.csail.mit.edu/6.824/
- **Discord Engineering — How Discord Scaled Elixir to 5 Million Concurrent Users (consistent hashing in routing):** https://discord.com/blog/how-discord-scaled-elixir-to-5-000-000-concurrent-users
- **Cloudflare — Load Balancing with Consistent Hashing:** https://blog.cloudflare.com/cloudflares-new-load-balancers-pt-2/
- **xxHash — Extremely Fast Hash Algorithm:** https://github.com/Cyan4973/xxHash
- **MurmurHash3 — Austin Appleby:** https://github.com/aappleby/smhasher
- **Apache Kafka — KIP-32 Add timestamps to Kafka message (consistent hash slot allocation in Kafka Streams):** https://cwiki.apache.org/confluence/display/KAFKA/KIP-32
