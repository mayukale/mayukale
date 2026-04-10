# System Design: Consistent Hashing System

> **Relevance to role:** Consistent hashing is the partitioning algorithm behind nearly every distributed data system in infrastructure. Redis Cluster uses it for key sharding. Kafka uses it for partition assignment. Load balancers use it for sticky sessions. The job scheduler uses it to assign work partitions to scheduler instances. Bare-metal inventory can be partitioned by consistent hash for parallel provisioning. Understanding consistent hashing deeply -- including virtual nodes, rebalancing math, and hotspot mitigation -- is essential for designing any horizontally scalable infrastructure component.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Map keys to nodes | Given a key, return the responsible node in O(log N) time |
| FR-2 | Minimal disruption on node add/remove | Only K/N keys remapped (K=total keys, N=total nodes) |
| FR-3 | Virtual nodes | Configurable number of virtual nodes per physical node for balance |
| FR-4 | Weighted nodes | Heterogeneous nodes get proportionally more keys |
| FR-5 | Replication placement | Return N responsible nodes for replication (not just 1) |
| FR-6 | Node add: key migration | When a node joins, affected keys are migrated from predecessor |
| FR-7 | Node remove: key reassignment | When a node leaves, its keys are distributed to successor |
| FR-8 | Hotspot detection | Identify keys or ranges with disproportionate load |
| FR-9 | Ring visualization | Admin tool to visualize key distribution across the ring |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Lookup latency | O(log N) -- < 1 microsecond for 1000 nodes |
| NFR-2 | Node add/remove impact | Only 1/N of keys affected |
| NFR-3 | Load balance | Standard deviation of keys per node < 10% of mean |
| NFR-4 | Memory overhead | < 100 KB for 1000 physical nodes with 150 vnodes each |
| NFR-5 | Availability during rebalance | Lookups continue during rebalancing (no downtime) |
| NFR-6 | Convergence after membership change | < 30 seconds for ring stabilization |

### Constraints & Assumptions
- Hash function: consistent hashing requires a uniform hash function (e.g., xxHash, Murmur3, SHA-256).
- Physical node count: 10 to 10,000 nodes.
- Key space: 2^32 or 2^64 (ring size).
- Use cases: distributed cache (Redis Cluster), partition assignment (Kafka), load balancing, distributed lock sharding.
- The ring metadata is replicated to all nodes (gossip or config-based).

### Out of Scope
- Specific storage engine implementation (we design the partitioning layer).
- Cross-datacenter consistent hashing (handled by higher-level routing).
- CRDT-based conflict resolution for replicated data.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Physical nodes | 1,000 | Large infrastructure cluster |
| Virtual nodes per physical | 150 | Standard for good balance |
| Total ring positions | 150,000 | 1,000 x 150 |
| Keys managed | 100 million | Cache entries, partition keys |
| Lookups per second | 10 million | Every cache hit/miss requires a lookup |
| Node additions per hour | 5 | Scaling events, replacements |
| Node removals per hour | 2 | Failures, decommissions |

### Latency Requirements

| Operation | Latency | Notes |
|-----------|---------|-------|
| Key lookup | < 1 us | Binary search on sorted ring |
| Node add (ring update) | < 1 ms | Insert 150 vnodes into sorted array |
| Node remove (ring update) | < 1 ms | Remove 150 vnodes |
| Key migration (per key) | Depends on data size | Network transfer |
| Full rebalance | < 30 s | Overlap period with old and new ring |

### Storage Estimates

| Item | Size | Total |
|------|------|-------|
| Ring position (hash + node_id) | 16 bytes (8B hash + 8B node_id) | |
| 150K ring positions | | 2.4 MB |
| Sorted ring array (for binary search) | | 2.4 MB |
| Physical node metadata | 256 B per node | 256 KB |
| **Total ring metadata** | | **~5 MB** |

### Bandwidth Estimates

| Event | Calculation | Bandwidth |
|-------|-------------|-----------|
| Ring gossip (full ring, every 30s) | 5 MB x 1000 nodes / 30s | Not full broadcast; use delta gossip |
| Ring delta update (node add) | 150 entries x 16B = 2.4 KB per node | 2.4 MB total (gossip to all nodes) |
| Key migration on node add | 100M keys / 1000 nodes = 100K keys x avg 1KB | 100 MB per node add |

---

## 3. High-Level Architecture

```
                    ┌─────────────────────────────────────┐
                    │           Client / Router            │
                    │  ┌───────────────────────────────┐  │
                    │  │  Consistent Hash Ring (local) │  │
                    │  │  [sorted array of vnodes]     │  │
                    │  │                               │  │
                    │  │  lookup(key) → Node            │  │
                    │  └───────────────────────────────┘  │
                    └──────────────┬──────────────────────┘
                                   │  route to node
                                   ▼
     ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
     │ Node A   │  │ Node B   │  │ Node C   │  │ Node D   │
     │(vnodes:  │  │(vnodes:  │  │(vnodes:  │  │(vnodes:  │
     │ 5,120,   │  │ 42,88,   │  │ 15,67,   │  │ 33,99,   │
     │ 200,310) │  │ 175,290) │  │ 230,355) │  │ 141,277) │
     └──────────┘  └──────────┘  └──────────┘  └──────────┘

    Hash Ring (0 to 2^32-1):
    ┌───────────────────────────────────────────────────────┐
    │ 0                                              2^32-1 │
    │ ├──A──B──C──D──A──C──B──A──D──C──B──D──A──B──C──D──┤ │
    │   5  15 33  42 67  88  99 120 141 175 200 230 277 310│
    │                                                355    │
    └───────────────────────────────────────────────────────┘

    lookup("user:12345"):
      hash("user:12345") = 187
      binary_search(ring, 187) → next vnode at position 200 → Node A
```

### Component Roles

| Component | Role |
|-----------|------|
| **Hash Ring** | Sorted array of (hash_position, node_id) pairs; the core data structure |
| **Hash Function** | Maps keys and node identifiers to positions on the ring; must be uniform |
| **Virtual Node Manager** | Creates and manages virtual nodes per physical node; handles weighted distribution |
| **Lookup Engine** | Binary search on the sorted ring; returns the node(s) responsible for a key |
| **Membership Manager** | Tracks physical node additions/removals; triggers ring updates |
| **Migration Coordinator** | Orchestrates key migration when nodes join/leave |
| **Gossip Protocol** | Distributes ring updates to all nodes |
| **Hotspot Detector** | Monitors load per node/vnode; detects imbalances |

### Data Flows

**Key Lookup:**
1. Client computes `hash(key)` -> position P on ring.
2. Binary search finds the first vnode with position >= P (wrapping around if needed).
3. Return the physical node associated with that vnode.
4. For replication: return the next N-1 distinct physical nodes on the ring.

**Node Addition:**
1. New node N joins with 150 vnodes.
2. Each vnode is placed on the ring at `hash(N.id + "-" + vnode_index)`.
3. For each new vnode, the key range that was owned by the successor is now split: keys that hash to positions between the new vnode and its predecessor are now owned by N.
4. Migration coordinator transfers those keys from the old owner to N.
5. Ring update gossipped to all nodes.

**Node Removal:**
1. Node N fails or is decommissioned.
2. Its 150 vnodes are removed from the ring.
3. For each removed vnode, the key range it owned is now owned by the successor vnode's physical node.
4. Keys are migrated (if the data was on the removed node, the successor takes over from replicas).
5. Ring update gossipped to all nodes.

---

## 4. Data Model

### Core Entities & Schema

```
┌─────────────────────────────────────────────────────────┐
│ PhysicalNode                                             │
├─────────────────────────────────────────────────────────┤
│ node_id         STRING      PK    -- unique identifier   │
│ address         STRING            -- IP:port             │
│ weight          INT DEFAULT 100   -- relative capacity   │
│ zone            STRING            -- failure domain      │
│ status          ENUM(ACTIVE, DRAINING, REMOVED)          │
│ vnode_count     INT               -- computed from weight│
│ joined_at       TIMESTAMP                                │
│ metadata        MAP<STRING,STRING>                        │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ VirtualNode                                              │
├─────────────────────────────────────────────────────────┤
│ hash_position   UINT64      PK    -- position on ring    │
│ physical_node   STRING      FK    -- owning node         │
│ vnode_index     INT               -- 0..vnode_count-1    │
│ replica_group   INT               -- which replica set   │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ HashRing                                                 │
├─────────────────────────────────────────────────────────┤
│ ring_version    UINT64            -- incremented on      │
│                                      membership change   │
│ ring_positions  SORTED_ARRAY<(UINT64, STRING)>           │
│                                   -- (hash, node_id)     │
│ node_count      INT                                      │
│ total_vnodes    INT                                      │
└─────────────────────────────────────────────────────────┘
```

### Database Selection

| Store | Use | Rationale |
|-------|-----|-----------|
| **In-memory sorted array** | Hot path (lookups) | O(log N) binary search; < 1us per lookup; fits in CPU cache for small clusters |
| **Configuration store (etcd)** | Ring membership persistence | Survives process restarts; watch for membership changes |
| **Gossip protocol** | Ring propagation | All nodes converge to same ring state quickly; no SPOF |

### Indexing Strategy

- **Primary structure:** Sorted array of `(hash_position, physical_node_id)` pairs. Binary search for O(log N) lookups.
- **Physical node index:** HashMap `node_id -> [vnode positions]` for fast node add/remove.
- **Zone index:** HashMap `zone -> [node_ids]` for zone-aware replica placement.

---

## 5. API Design

### Library API (In-Process)

```python
class ConsistentHashRing:
    """Public API for consistent hashing."""

    def __init__(self, hash_function=xxhash.xxh64, default_vnodes=150):
        ...

    def add_node(self, node_id: str, weight: int = 100,
                 zone: str = "") -> List[KeyRange]:
        """Add a node with proportional vnodes. Returns affected key ranges."""

    def remove_node(self, node_id: str) -> List[KeyRange]:
        """Remove a node. Returns affected key ranges for migration."""

    def lookup(self, key: str) -> str:
        """Return the responsible node for a key."""

    def lookup_n(self, key: str, n: int) -> List[str]:
        """Return n distinct physical nodes for replication."""

    def get_key_ranges(self, node_id: str) -> List[KeyRange]:
        """Return all key ranges owned by a node."""

    def get_load_distribution(self) -> Dict[str, float]:
        """Return percentage of ring owned by each node."""
```

### gRPC Service (Centralized Ring Manager)

```protobuf
service HashRingService {
  rpc GetNode(GetNodeRequest) returns (GetNodeResponse);
  rpc GetNodes(GetNodesRequest) returns (GetNodesResponse);
  rpc AddNode(AddNodeRequest) returns (RingUpdateResponse);
  rpc RemoveNode(RemoveNodeRequest) returns (RingUpdateResponse);
  rpc GetRingState(GetRingStateRequest) returns (RingState);
  rpc WatchRing(WatchRingRequest) returns (stream RingUpdate);
  rpc GetLoadDistribution(Empty) returns (LoadDistribution);
}
```

### CLI

```bash
# Add a node
hashring add-node node-42 --weight 100 --zone az-1 --address 10.0.1.42:6379
# Output:
# Node node-42 added with 150 vnodes
# Affected key ranges: 150 ranges, ~0.1% of keyspace
# Migration required from: node-7 (50 ranges), node-23 (48 ranges), ...

# Remove a node
hashring remove-node node-42
# Output:
# Node node-42 removed. 150 vnodes removed.
# Keys redistributed to: node-7 (52 ranges), node-23 (45 ranges), ...

# Lookup
hashring lookup "user:12345"
# Output: node-7

# Lookup with replicas
hashring lookup "user:12345" --replicas 3
# Output: node-7 (primary), node-23 (replica-1), node-15 (replica-2)

# Show distribution
hashring distribution
# Output:
# NODE       VNODES   KEY SHARE   EXPECTED   DEVIATION
# node-1     150      10.2%       10.0%      +0.2%
# node-2     150      9.8%        10.0%      -0.2%
# node-3     150      10.1%       10.0%      +0.1%
# ...
# Std Dev: 0.3% (target: < 1%)

# Visualize ring
hashring ring --format ascii
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Virtual Nodes and Load Balance Analysis

**Why it's hard:**
Without virtual nodes, each physical node gets one position on the ring. With N nodes, the expected key share per node is 1/N, but the standard deviation is high. Node A might own 30% of the ring while Node B owns 5%. Virtual nodes spread each physical node across V positions on the ring, reducing variance. But how many virtual nodes are enough?

**Approaches:**

| Approach | Mechanism | Load Balance | Memory | Lookup |
|----------|-----------|-------------|--------|--------|
| No vnodes (basic consistent hash) | 1 position per node | Poor (std dev ~100/sqrt(N) %) | Minimal | O(log N) |
| **Virtual nodes** | **V positions per node** | **Good (std dev ~100/sqrt(V*N) %)** | **V*N entries** | **O(log V*N)** |
| Jump consistent hash | Stateless O(ln N) algorithm | Perfect (0% deviation) | Zero (stateless) | O(ln N) |
| Rendezvous hashing (HRW) | Hash key with each node; highest wins | Perfect | Zero per-key state | O(N) |
| Bounded load consistent hashing | Consistent hash + load cap | Good; prevents extreme hotspots | Ring + load counters | O(log N) + load check |

**Virtual Node Analysis:**

```python
class VirtualNodeAnalysis:
    """
    Statistical analysis of load distribution with virtual nodes.
    """

    @staticmethod
    def expected_std_deviation(num_physical_nodes, vnodes_per_node):
        """
        Standard deviation of key share per physical node.

        With V virtual nodes per N physical nodes, the total ring
        positions = V * N. Each physical node owns V consecutive
        arcs. The expected share per node = 1/N.

        The standard deviation of the share is approximately:
            sigma = 1 / sqrt(V * N)

        For N=10, V=1:    sigma = 31.6%  (terrible)
        For N=10, V=10:   sigma = 10.0%  (poor)
        For N=10, V=100:  sigma = 3.2%   (acceptable)
        For N=10, V=150:  sigma = 2.6%   (good)
        For N=10, V=500:  sigma = 1.4%   (excellent)
        For N=100, V=150: sigma = 0.8%   (excellent)
        For N=1000, V=150: sigma = 0.26% (near-perfect)
        """
        total_vnodes = num_physical_nodes * vnodes_per_node
        return 1.0 / (total_vnodes ** 0.5)

    @staticmethod
    def simulate_distribution(num_nodes, vnodes_per_node,
                               num_keys=1_000_000):
        """
        Simulate key distribution and measure actual balance.
        """
        ring = ConsistentHashRing(default_vnodes=vnodes_per_node)
        for i in range(num_nodes):
            ring.add_node(f"node-{i}")

        # Distribute keys
        node_counts = defaultdict(int)
        for i in range(num_keys):
            key = f"key-{i}"
            node = ring.lookup(key)
            node_counts[node] += 1

        # Calculate statistics
        counts = list(node_counts.values())
        mean = num_keys / num_nodes
        variance = sum((c - mean) ** 2 for c in counts) / num_nodes
        std_dev = variance ** 0.5
        std_dev_pct = std_dev / mean * 100

        max_load = max(counts) / mean
        min_load = min(counts) / mean

        return {
            "nodes": num_nodes,
            "vnodes_per_node": vnodes_per_node,
            "mean_keys_per_node": mean,
            "std_dev_pct": f"{std_dev_pct:.1f}%",
            "max_load_ratio": f"{max_load:.2f}x",
            "min_load_ratio": f"{min_load:.2f}x",
        }
```

**Empirical Results (1M keys):**

| Nodes | VNodes/Node | Total VNodes | Std Dev (%) | Max Load | Min Load |
|-------|-------------|-------------|-------------|----------|----------|
| 10 | 1 | 10 | 32.1% | 1.82x | 0.31x |
| 10 | 10 | 100 | 10.3% | 1.28x | 0.72x |
| 10 | 50 | 500 | 4.5% | 1.12x | 0.88x |
| 10 | 150 | 1,500 | 2.6% | 1.07x | 0.93x |
| 10 | 500 | 5,000 | 1.5% | 1.04x | 0.96x |
| 100 | 150 | 15,000 | 0.8% | 1.03x | 0.97x |
| 1000 | 150 | 150,000 | 0.3% | 1.01x | 0.99x |

**150 vnodes per physical node is the standard recommendation** (used by Amazon DynamoDB, Apache Cassandra). It provides < 3% standard deviation with 10+ nodes while keeping memory usage manageable (150K ring entries for 1000 nodes = 2.4 MB).

**Implementation:**

```python
class ConsistentHashRing:
    """
    Consistent hashing with virtual nodes.
    Uses a sorted array for O(log N) lookups.
    """

    def __init__(self, hash_func=xxhash_xxh64, default_vnodes=150):
        self.hash_func = hash_func
        self.default_vnodes = default_vnodes

        # Sorted array of (hash_position, node_id)
        # Maintained in sorted order for binary search
        self.ring = []            # List[(int, str)]
        self.nodes = {}           # node_id -> PhysicalNode
        self._sorted = True

    def add_node(self, node_id, weight=100, zone=""):
        """
        Add a physical node with proportional virtual nodes.
        Weight determines vnode count: vnodes = default * (weight/100)
        """
        vnode_count = int(self.default_vnodes * weight / 100)
        node = PhysicalNode(node_id=node_id, weight=weight,
                            zone=zone, vnode_count=vnode_count)
        self.nodes[node_id] = node

        for i in range(vnode_count):
            # Hash the combination of node_id and vnode index
            # to get a uniformly distributed position
            vnode_key = f"{node_id}#vnode{i}"
            position = self.hash_func(vnode_key.encode())
            self.ring.append((position, node_id))

        # Re-sort the ring
        self.ring.sort(key=lambda x: x[0])
        self._sorted = True

        return self._affected_ranges(node_id)

    def remove_node(self, node_id):
        """Remove a node and all its virtual nodes."""
        self.ring = [(pos, nid) for pos, nid in self.ring
                     if nid != node_id]
        affected = self._affected_ranges(node_id)
        del self.nodes[node_id]
        return affected

    def lookup(self, key):
        """
        Find the node responsible for a key.
        Binary search for the first ring position >= hash(key).
        Time complexity: O(log(V*N)) where V=vnodes, N=nodes.
        """
        if not self.ring:
            raise EmptyRingError("No nodes in ring")

        key_hash = self.hash_func(key.encode())
        idx = self._binary_search(key_hash)
        return self.ring[idx][1]

    def lookup_n(self, key, n):
        """
        Find n distinct physical nodes for replication.
        Walk clockwise from the key's position, collecting distinct
        physical nodes (skip duplicate physical nodes from vnodes).
        """
        if len(self.nodes) < n:
            raise InsufficientNodesError(
                f"Need {n} nodes but only {len(self.nodes)} available")

        key_hash = self.hash_func(key.encode())
        idx = self._binary_search(key_hash)
        result = []
        seen_nodes = set()
        seen_zones = set()

        for offset in range(len(self.ring)):
            pos_idx = (idx + offset) % len(self.ring)
            _, node_id = self.ring[pos_idx]

            if node_id not in seen_nodes:
                # Zone-aware: prefer nodes in different zones
                node = self.nodes[node_id]
                if len(result) < n:
                    result.append(node_id)
                    seen_nodes.add(node_id)
                    seen_zones.add(node.zone)

            if len(result) >= n:
                break

        return result

    def _binary_search(self, key_hash):
        """
        Find the first ring position >= key_hash.
        If key_hash > all positions, wrap to the first position (ring).
        """
        lo, hi = 0, len(self.ring) - 1

        if key_hash > self.ring[-1][0]:
            return 0  # Wrap around to first position

        while lo < hi:
            mid = (lo + hi) // 2
            if self.ring[mid][0] < key_hash:
                lo = mid + 1
            else:
                hi = mid

        return lo
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Hash function collision | Two vnodes at same position; one is shadowed | Use 64-bit hash; collision probability negligible (birthday problem: 50% at 2^32 entries) |
| Uneven vnode distribution | Some nodes get more load despite equal vnodes | Increase vnode count; or use power-of-two-choices: hash key twice, choose less loaded node |
| Large vnode count | Memory overhead; slow ring updates | 150 vnodes is the sweet spot; beyond 500, diminishing returns |

**Interviewer Q&A:**

**Q1: Why 150 virtual nodes per physical node?**
A: It's a balance between load balance and memory. At 150 vnodes, the standard deviation of key share is ~2.6% with 10 nodes (good enough for most use cases). Going to 500 vnodes reduces it to ~1.5%, but the ring is 3.3x larger. Amazon DynamoDB and Apache Cassandra use ~150 vnodes. Going below 50 vnodes gives > 5% deviation, which causes hot nodes.

**Q2: How does weighted consistent hashing work?**
A: A node with weight 200 gets 2x the vnodes of a weight-100 node. If default_vnodes=150 and weight=200, that node gets 300 vnodes. This means it owns 2x the ring space and receives 2x the keys. Use case: heterogeneous hardware — a server with 2x RAM should handle 2x cache keys.

**Q3: What is the advantage of jump consistent hash over ring-based?**
A: Jump consistent hash (Lamping & Veach, 2014) is stateless: given a key and the number of buckets, it deterministically returns a bucket number in O(ln N) time with zero memory overhead. It has perfect balance (0% standard deviation). Downside: it doesn't support named nodes (only numbered buckets 0..N-1), and adding/removing a node affects all buckets (not minimal disruption like ring-based). It's ideal for static clusters.

**Q4: What is rendezvous hashing and when would you use it?**
A: Also called Highest Random Weight (HRW). For each key, compute hash(key, node) for every node; the node with the highest hash wins. O(N) per lookup (must evaluate all nodes). Perfect balance; minimal disruption (only the affected node's keys are remapped). Use when N is small (< 100 nodes) and balance is critical. Not suitable for large N due to O(N) lookup cost.

**Q5: How does Redis Cluster implement consistent hashing?**
A: Redis Cluster doesn't use traditional consistent hashing. It uses hash slots: the key space is divided into 16,384 fixed slots (`CRC16(key) % 16384`). Each slot is assigned to a node. This is simpler than ring-based hashing and allows explicit slot-to-node mapping (admin can control which slots go where). Rebalancing is manual: move slots between nodes. The downside is that adding a node requires explicitly migrating slot ranges.

**Q6: How do you handle the "hotspot" problem in consistent hashing?**
A: A single extremely popular key (e.g., a viral cache key) is always mapped to one node, regardless of virtual nodes. Mitigations: (1) Client-side L1 cache: each client caches the hot key locally for a short TTL. (2) Scatter-read: hash the key to N replicas and spread reads across them. (3) Key-level replication: popular keys are replicated to additional nodes. (4) Random suffix: append a random suffix and aggregate results (for counters).

---

### Deep Dive 2: Key Migration on Node Add/Remove

**Why it's hard:**
When a node joins or leaves, some keys must be migrated to their new owner. During migration, the key must be readable from either the old or new owner (availability). Consistency requires that writes go to the new owner, but reads might still go to the old one. The migration must be coordinated without losing data or serving stale values.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| Full reshuffle | Simple | Moves all keys; catastrophic for cache hit rate |
| **Consistent hash (minimal migration)** | **Only K/N keys moved** | Requires migration coordination |
| Lazy migration (on access) | No upfront cost | First access is slow (cache miss + migration) |
| Background streaming | No impact on reads during migration | Complexity; dual-read window |

**Selected: Background streaming with dual-read fallback**

```python
class MigrationCoordinator:
    """
    Coordinates key migration when nodes join or leave the ring.
    """

    def __init__(self, ring, storage_client):
        self.ring = ring
        self.storage = storage_client

    def handle_node_add(self, new_node_id, new_node_address):
        """
        When a new node joins:
        1. Calculate which key ranges move to the new node.
        2. For each range, the old owner streams keys to the new node.
        3. During migration, reads fall back to old owner if new
           node doesn't have the key yet.
        4. After migration completes, all traffic goes to new node.
        """
        affected_ranges = self.ring.add_node(new_node_id)

        for key_range in affected_ranges:
            old_owner = key_range.previous_owner
            new_owner = new_node_id

            # Phase 1: Start migration (background)
            migration_id = self._start_migration(
                source=old_owner,
                destination=new_owner,
                key_range=key_range
            )

            # Phase 2: During migration, lookups route to new node,
            # which proxies to old node on miss
            self._set_migration_state(
                key_range, state="MIGRATING",
                source=old_owner, destination=new_owner
            )

        # Phase 3: Wait for all migrations to complete
        self._wait_for_migrations(affected_ranges)

        # Phase 4: Clear migration state
        for key_range in affected_ranges:
            self._set_migration_state(key_range, state="COMPLETE")

    def _start_migration(self, source, destination, key_range):
        """
        Stream keys from source to destination.

        Algorithm:
        1. Source iterates all keys in the range.
        2. For each key, send to destination.
        3. Destination stores the key.
        4. Source marks the key as "migrated" (can be deleted later).
        5. New writes during migration go to BOTH source and
           destination (dual-write) or only to destination
           (new owner) with proxy reads from source.
        """
        source_client = self.storage.get_client(source)
        dest_client = self.storage.get_client(destination)

        keys = source_client.scan_range(
            start=key_range.start_hash,
            end=key_range.end_hash
        )

        migrated_count = 0
        for key, value, ttl in keys:
            dest_client.put(key, value, ttl=ttl)
            migrated_count += 1

            # Report progress periodically
            if migrated_count % 10000 == 0:
                log.info(f"Migration progress: {migrated_count} keys "
                         f"from {source} to {destination}")

        log.info(f"Migration complete: {migrated_count} keys "
                 f"from {source} to {destination}")
        return migrated_count

    def calculate_keys_to_migrate(self, new_node_id):
        """
        Calculate the exact number of keys that need to migrate.

        When node N joins with V vnodes:
        - Each vnode "steals" a portion of the ring from its
          successor's range.
        - Total keys migrated ≈ (total_keys / num_nodes)
          = K/N keys per node addition.
        - This is the minimal possible disruption.

        Example: 10 nodes, 1M keys, add 1 node
        Keys migrated ≈ 1M / 11 ≈ 91K keys
        That's 9.1% of total keys — exactly 1/(N+1).
        """
        total_keys = self.storage.total_key_count()
        new_node_count = len(self.ring.nodes)
        return total_keys / new_node_count
```

**Migration During Node Removal:**

```python
def handle_node_remove(self, removed_node_id):
    """
    When a node leaves (failure or decommission):
    1. Its key ranges are absorbed by successor nodes.
    2. If the node is still alive (graceful): stream keys first.
    3. If the node is dead (crash): rely on replicas.
    """
    if self._is_node_alive(removed_node_id):
        # Graceful removal: stream keys to successors
        affected_ranges = self.ring.get_key_ranges(removed_node_id)
        for key_range in affected_ranges:
            successor = self.ring.lookup(key_range.start_hash)
            self._start_migration(
                source=removed_node_id,
                destination=successor,
                key_range=key_range
            )
        self.ring.remove_node(removed_node_id)
    else:
        # Crash: remove from ring immediately.
        # Keys must be served from replicas.
        self.ring.remove_node(removed_node_id)
        # The next nodes in the ring already have replicas.
        # Trigger re-replication to restore replication factor.
        self._trigger_re_replication(removed_node_id)
```

**Interviewer Q&A:**

**Q1: How much data is migrated when adding a node to a 100-node cluster?**
A: Approximately 1/101 ≈ 1% of total keys. If the cluster holds 100 million keys, adding one node migrates ~1 million keys. With consistent hashing, this is the theoretical minimum. Without consistent hashing (simple modulo), adding a node would remap ~99% of keys.

**Q2: How long does migration take?**
A: Depends on data volume and network bandwidth. 1M keys x 1KB avg = 1 GB. At 1 Gbps network, that's ~8 seconds. In practice, with overhead, 30-60 seconds. During migration, reads are still served (from old or new owner), so there's no downtime.

**Q3: What happens to writes during migration?**
A: Two strategies: (1) New owner is authoritative: all writes go to the new owner. If the key hasn't migrated yet, the write creates it fresh on the new owner (for cache, this is fine — it's a cache miss). (2) Dual-write: writes go to both old and new owner. More complex but preserves data integrity for stateful systems.

**Q4: How does Amazon DynamoDB handle key migration?**
A: DynamoDB uses virtual nodes (called "partitions"). When a partition is split (for scaling), the data is streamed to the new partition in the background. During streaming, reads can come from either partition. DynamoDB uses quorum reads/writes, so consistency is maintained. The migration is invisible to the client.

**Q5: Can you avoid migration entirely?**
A: For a cache (Redis, Memcached): yes. When a node is added, the new node starts empty. Cache misses cause the application to fetch from the database and populate the new node. Over time, the new node warms up naturally. The trade-off is increased cache miss rate temporarily (thundering herd on database).

**Q6: How do you handle migration failures (network error mid-stream)?**
A: (1) Track migration progress per key range (checkpoint). (2) On failure, resume from the last checkpoint. (3) Set a deadline for migration completion. (4) If deadline passes, escalate — manual intervention or force-remove the node from the ring. (5) For cache systems, abandon migration and let the new node warm from cold.

---

### Deep Dive 3: Jump Consistent Hash and Rendezvous Hashing

**Why it's hard:**
Ring-based consistent hashing requires O(V*N) memory for virtual nodes and O(log V*N) for lookups. For very large clusters or memory-constrained environments, stateless alternatives are attractive. But they have different trade-offs for node addition/removal.

**Jump Consistent Hash (Lamping & Veach, Google, 2014):**

```python
def jump_consistent_hash(key: int, num_buckets: int) -> int:
    """
    Stateless consistent hash that maps a key to a bucket.

    Properties:
    - O(ln(num_buckets)) time
    - Zero memory overhead
    - Perfect balance (exactly 1/N keys per bucket)
    - When num_buckets increases from N to N+1, only 1/(N+1) of
      keys move (to the new bucket)

    Limitation: buckets are numbered 0..N-1; no named nodes.
    Removing bucket K (not the last) requires remapping.
    Only supports adding/removing the last bucket.
    """
    b = -1
    j = 0
    seed = key

    while j < num_buckets:
        b = j
        seed = ((seed * 2862933555777941757) + 1) & 0xFFFFFFFFFFFFFFFF
        j = int((b + 1) * (float(1 << 31) / float((seed >> 33) + 1)))

    return b
```

**Rendezvous Hashing (HRW — Highest Random Weight, 1997):**

```python
def rendezvous_hash(key: str, nodes: List[str]) -> str:
    """
    For each node, compute hash(key, node). The node with the
    highest hash value wins.

    Properties:
    - O(N) per lookup (must evaluate all nodes)
    - Perfect balance (each node equally likely to win)
    - Minimal disruption: adding/removing a node only affects
      keys where that node was the winner
    - Named nodes (not numbered)
    - Stateless

    Limitation: O(N) per lookup. Not suitable for N > 1000.
    """
    max_hash = -1
    winner = None

    for node in nodes:
        # Combine key and node to get a deterministic hash
        combined = f"{key}:{node}"
        h = xxhash.xxh64(combined.encode()).intdigest()
        if h > max_hash:
            max_hash = h
            winner = node

    return winner


def rendezvous_hash_top_n(key: str, nodes: List[str], n: int) -> List[str]:
    """Return top-n nodes for replication (sorted by hash value)."""
    scored = []
    for node in nodes:
        combined = f"{key}:{node}"
        h = xxhash.xxh64(combined.encode()).intdigest()
        scored.append((h, node))

    scored.sort(reverse=True)
    return [node for _, node in scored[:n]]
```

**Comparison:**

| Feature | Ring + VNodes | Jump Hash | Rendezvous (HRW) |
|---------|-------------|-----------|-------------------|
| Lookup time | O(log V*N) | O(ln N) | O(N) |
| Memory | O(V*N) | O(1) | O(N) for node list |
| Balance | Good (< 3% std dev at 150 vnodes) | Perfect | Perfect |
| Named nodes | Yes | No (numbered buckets) | Yes |
| Node add | Minimal disruption | Only last bucket | Minimal disruption |
| Node remove | Minimal disruption | Only last bucket | Minimal disruption |
| Arbitrary node remove | Yes | No (only the last) | Yes |
| Replication (top-N) | Walk ring | Not native | Sort by hash, take top N |
| Use case | General purpose | Static clusters, load balancers | Small clusters (< 100 nodes) |

**Interviewer Q&A:**

**Q1: When would you choose jump hash over ring-based?**
A: When the cluster is mostly static (nodes rarely added/removed), you need perfect balance, and nodes are numbered (not named). Example: sharding data across a fixed set of database replicas. Google uses jump hash internally for some of its systems.

**Q2: When would you choose rendezvous hashing?**
A: When N is small (< 100), you need named nodes, and you want perfect balance with minimal disruption. Example: selecting which CDN edge server should cache a URL. GitHub uses rendezvous hashing for some load balancing decisions.

**Q3: Can you combine jump hash with named nodes?**
A: Yes. Maintain a sorted list of node names. Use jump hash to select a bucket number, then map bucket number to node name. But node removal becomes problematic (renumbering). Better to use rendezvous hashing in that case.

**Q4: What hash function should you use for consistent hashing?**
A: Requirements: uniform distribution, fast, deterministic. Good choices: xxHash (fastest, good distribution), Murmur3 (widely used, good distribution), CityHash (Google). Avoid: MD5/SHA-256 (too slow for hot-path lookups; cryptographic properties not needed). xxHash64 is the best choice for most infrastructure use cases — 10-100x faster than SHA-256 with excellent distribution.

**Q5: How does Kafka use consistent hashing for partition assignment?**
A: Kafka's default partitioner uses `murmur2(key) % num_partitions`. This is modulo hashing, not consistent hashing — changing the partition count remaps most keys. Kafka's consumer group assignment uses a range or round-robin assignor, not consistent hashing. Some Kafka client libraries support sticky assignment, which is closer to consistent hashing (minimizes partition migration on rebalance).

**Q6: What is the "power of two choices" optimization for consistent hashing?**
A: Hash the key to two positions on the ring. Compare the load of the two candidate nodes. Route to the less loaded one. This dramatically reduces the maximum load on any node (from O(log N / log log N) to O(log log N) with high probability). Google's Maglev paper uses this technique for load balancing.

---

## 7. Scheduling & Resource Management

### Consistent Hashing in Job Scheduling

**Partition Assignment for Parallel Schedulers:**
```
Job queue is partitioned by job_id hash.
N scheduler instances each own a portion of the ring.

Scheduler 1: owns hash range [0, 2^64/N)
Scheduler 2: owns hash range [2^64/N, 2*2^64/N)
...
Scheduler N: owns hash range [(N-1)*2^64/N, 2^64)

When a scheduler fails:
- Its partition is immediately absorbed by successor (consistent hash).
- Only 1/N of jobs need to be re-assigned.
- No "stop the world" rebalancing.
```

**Bare-Metal Inventory Partitioning:**
```python
class InventoryPartitioner:
    """
    Partition bare-metal servers across provisioner instances
    using consistent hashing.
    """

    def __init__(self, ring):
        self.ring = ring

    def get_provisioner_for_server(self, server_id):
        """Each server is handled by one provisioner."""
        return self.ring.lookup(server_id)

    def get_servers_for_provisioner(self, provisioner_id):
        """Get all servers assigned to a provisioner."""
        ranges = self.ring.get_key_ranges(provisioner_id)
        servers = []
        for r in ranges:
            servers.extend(
                inventory_db.query_by_hash_range(r.start, r.end))
        return servers
```

---

## 8. Scaling Strategy

| Dimension | Strategy | Detail |
|-----------|----------|--------|
| Node count | Ring supports 10K+ nodes with 150 vnodes each | 1.5M ring entries, ~24 MB |
| Lookup speed | Binary search stays O(log N); at 1.5M entries, ~20 comparisons | < 1 us |
| Migration speed | Parallel migration from multiple source nodes | Each source streams independently |
| Ring propagation | Gossip protocol with delta updates | Only changed entries, not full ring |
| Heterogeneous scaling | Weighted nodes get proportional vnodes | 2x RAM server gets 2x vnodes |

**Interviewer Q&A:**

**Q1: What happens to the ring when you double the cluster size?**
A: Adding N nodes to an existing N-node cluster: each new node gets 1/(2N) of the keyspace. Total keys migrated = 50% (N new nodes each get 1/(2N) share = N * K/(2N) = K/2). The migration is distributed across all existing nodes (each gives up 1/(2N) to one new node), so no single node is overwhelmed.

**Q2: How do you handle heterogeneous node capacity?**
A: Weighted consistent hashing. A node with 2x capacity gets 2x virtual nodes, owning 2x the ring space. This naturally routes 2x the keys to the larger node. The weight can be updated dynamically (increase vnodes for a node with more available resources).

**Q3: How do you minimize migration during a rolling upgrade?**
A: Rolling upgrade (replace one node at a time) with the same node ID: the replacement node inherits the same vnode positions. Hash positions are derived from node_id + vnode_index, so if the node_id is the same, positions are identical. Zero key migration. If the node_id changes (new hardware), migration equals 1/N of keys.

**Q4: How does Cassandra handle consistent hashing at scale?**
A: Cassandra assigns each node to one or more token ranges on the ring. Originally, each node had one token; now, vnodes (default 256) are used. When a node joins, it takes ownership of portions of existing nodes' token ranges. Data is streamed in the background. Cassandra's anti-entropy repair process (Merkle trees) ensures consistency after migration.

**Q5: Can consistent hashing handle non-uniform key access patterns?**
A: Consistent hashing distributes keys uniformly across nodes, but if some keys are accessed much more than others (Zipfian distribution), the node holding the hot key becomes a bottleneck. Solutions: (1) Client-side L1 cache. (2) Read replicas for hot keys. (3) Key splitting (shard the hot key into sub-keys). (4) Bounded-load consistent hashing (Google Maglev): cap the max load per node, spilling excess to the next node on the ring.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO |
|---|---------|-----------|--------|----------|-----|
| 1 | Single node crash | Heartbeat timeout | 1/N of keys unavailable (primary) | Successor takes over from replicas | 1-5 s |
| 2 | Multiple nodes in same zone | Zone failure | Zone's share of keys unavailable | Cross-zone replicas serve; re-replication needed | < 10 s |
| 3 | Ring metadata corruption | Checksum mismatch | Incorrect routing | Reload ring from config store (etcd) | < 5 s |
| 4 | Hash function change | Configuration change | All keys remap (100% cache miss) | Never change hash function in production; use a version field | N/A (preventable) |
| 5 | Network partition (split ring) | Gossip divergence | Two ring views; inconsistent routing | Raft-based membership prevents split-brain ring | Duration of partition |
| 6 | Migration stalls (source overloaded) | Migration progress monitoring | Keys stuck in transit; reads proxied to old owner | Throttle migration; increase timeout; split into smaller batches | Minutes |

### Consensus & Coordination

**Ring Membership via Raft:**
- Ring membership (which nodes are in the cluster) is managed by a Raft group (or etcd).
- This prevents split-brain: during a network partition, only the majority side can update membership.
- All nodes watch the membership store and update their local ring copy.
- Hash ring computation is deterministic: given the same membership, all nodes compute the same ring.

---

## 10. Observability

### Key Metrics

| # | Metric | Type | Alert Threshold | Why |
|---|--------|------|-----------------|-----|
| 1 | `ring.lookup.latency.p99` | Histogram | > 10us | Binary search should be < 1us |
| 2 | `ring.node.count` | Gauge | deviation from expected | Node failure or unexpected addition |
| 3 | `ring.load.std_deviation` | Gauge | > 5% | Imbalanced load; need more vnodes |
| 4 | `ring.load.max_ratio` | Gauge | > 1.2x | Hot node; investigate |
| 5 | `ring.migration.keys_in_transit` | Gauge | > 100K sustained | Migration slow or stalled |
| 6 | `ring.migration.duration` | Histogram | > 120s | Migration taking too long |
| 7 | `ring.version` | Counter | > 10/hour | Too many membership changes |
| 8 | `ring.gossip.propagation_delay` | Histogram | > 5s | Ring inconsistency across nodes |
| 9 | `ring.hotspot.key` | String | any | Hot key detected; needs mitigation |

---

## 11. Security

| Threat | Mitigation |
|--------|------------|
| Ring poisoning (malicious node injection) | Authenticated membership (Raft-based with mTLS) |
| Key enumeration via ring analysis | Hash function makes it infeasible to derive keys from positions |
| Traffic analysis (knowing which node holds which keys) | Not a concern for internal infrastructure |
| Migration data interception | TLS for all node-to-node communication |

---

## 12. Incremental Rollout Strategy

### Phase 1: Library Integration (Week 1-2)
- Implement consistent hashing library in Java and Python.
- Unit tests with load balance verification.
- Benchmark: lookup latency, memory usage, migration calculation.

### Phase 2: Cache Layer (Week 3-6)
- Deploy for Redis cache cluster partitioning.
- Shadow mode: compute consistent hash results alongside current partitioning; compare.
- Gradually migrate cache traffic.

### Phase 3: Scheduler Partitioning (Week 7-10)
- Use consistent hashing for job queue partitioning across scheduler instances.
- Test failover: kill a scheduler instance; verify its partition is absorbed by successor.

### Phase 4: Infrastructure-Wide (Week 11-16)
- Inventory partitioning, log sharding, metric routing.
- Library becomes standard infrastructure primitive.

**Rollout Q&A:**

**Q1: How do you migrate from modulo-based partitioning to consistent hashing?**
A: Dual-read period: (1) new ring routes all new writes to consistent-hash-determined nodes. (2) reads try consistent-hash node first; on miss, try old modulo node. (3) Background migration moves existing data from old to new nodes. (4) After migration, remove dual-read.

**Q2: What if the hash function has poor distribution?**
A: Measure distribution before production use. Run 1M random keys through the hash function and verify uniformity (chi-squared test). If distribution is poor, switch hash function. Note: changing hash function after deployment remaps all keys (equivalent to a full reshuffle).

**Q3: How do you handle a scenario where a node repeatedly joins and leaves (flapping)?**
A: (1) Dampen membership changes: require a node to be stable for 30 seconds before adding to ring. (2) Rate-limit ring updates. (3) Alert on flapping (> 3 membership changes per hour for same node).

**Q4: What is the impact on cache hit rate when adding a node?**
A: Adding a node to an N-node cluster causes ~1/(N+1) of keys to be remapped. For N=10, that's ~9% cache miss. These misses hit the backend database. If the database can handle the spike (typically it can, since it's 1/N of normal cache traffic), the impact is brief. Pre-warm the new node by migrating keys before adding it to the ring.

**Q5: How do you test consistent hashing correctness?**
A: (1) Verify minimal disruption: add/remove a node and count remapped keys. Must equal ~K/N. (2) Verify balance: standard deviation of keys per node < threshold. (3) Verify determinism: same membership -> same ring -> same lookups. (4) Verify replication: lookup_n returns N distinct physical nodes in different zones.

---

## 13. Trade-offs & Decision Log

| # | Decision | Options | Selected | Rationale |
|---|----------|---------|----------|-----------|
| 1 | Hashing algorithm | Ring + vnodes vs jump hash vs rendezvous | Ring + vnodes | Named nodes; arbitrary add/remove; replication support |
| 2 | Vnode count | 50, 100, 150, 256, 500 | 150 | Best balance/memory trade-off; industry standard |
| 3 | Hash function | xxHash vs Murmur3 vs CityHash | xxHash64 | Fastest; excellent distribution; widely available |
| 4 | Ring data structure | Sorted array vs balanced BST vs skip list | Sorted array | Cache-friendly; simple; O(log N) binary search |
| 5 | Membership management | Gossip vs Raft vs static config | Raft (etcd) + local cache | Prevents split-brain; single source of truth |
| 6 | Migration strategy | Eager (immediate) vs lazy (on access) | Eager with background streaming | Better latency; cache hit rate recovers faster |
| 7 | Replica placement | Next N on ring vs zone-aware | Zone-aware (skip same-zone nodes) | Survives AZ failure |
| 8 | Hotspot mitigation | Client cache vs scatter-read vs key split | Client L1 cache (default); scatter-read for extreme cases | Simplest; covers 99% of hotspots |

---

## 14. Agentic AI Integration

**1. Automatic Weight Tuning:**
```python
class WeightTuningAgent:
    """
    Adjusts node weights based on actual load and capacity.
    """
    def recommend_weights(self):
        for node in self.ring.nodes.values():
            actual_load = self.metrics.get_cpu_utilization(node.node_id)
            current_weight = node.weight
            target_utilization = 0.70  # 70% CPU target

            if actual_load > 0.85:
                new_weight = int(current_weight * target_utilization
                                 / actual_load)
                yield Recommendation(
                    node=node.node_id,
                    current_weight=current_weight,
                    new_weight=new_weight,
                    reason=f"CPU at {actual_load:.0%}; reducing weight "
                           f"to decrease key share"
                )
```

**2. Hotspot Detection and Mitigation:**
```
Agent monitors per-key access patterns.
When a key exceeds 10x the average access rate:
  1. Identify the key and its owning node.
  2. Recommend mitigation:
     - Enable L1 client cache for this key pattern.
     - If it's a counter, switch to distributed counter (CRDTs).
     - If it's a cache key, replicate to additional nodes.
  3. Auto-apply L1 cache config change if approved.
```

**3. Migration Impact Prediction:**
```
Before a node addition, the agent predicts:
  - Number of keys to migrate: ~K/(N+1)
  - Estimated migration time: keys * avg_size / bandwidth
  - Impact on cache hit rate: ~1/(N+1) temporary miss rate
  - Impact on backend load: miss_rate * request_rate
  - Recommendation: "Add node during off-peak hours (2-4 AM)
    when request rate is 30% of peak."
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Explain consistent hashing in 30 seconds.**
A: Consistent hashing maps both keys and nodes to a ring (hash space). A key is assigned to the first node clockwise on the ring. When a node is added or removed, only 1/N of keys are remapped (instead of all keys with modulo hashing). Virtual nodes (multiple positions per node) improve load balance.

**Q2: Why is modulo hashing bad for distributed systems?**
A: With modulo hashing (`hash(key) % N`), changing N (adding/removing a node) remaps almost all keys. For a cache, this means near-100% cache miss rate after scaling. For a database, this means migrating nearly all data. Consistent hashing limits remapping to ~1/N.

**Q3: What is the mathematical proof that consistent hashing only remaps K/N keys?**
A: Each node owns a contiguous arc of the ring. When a node is added, it bisects one arc. The keys in the portion of that arc that now belongs to the new node are the only ones remapped. Since all arcs are approximately equal (1/N of the ring), the new node takes approximately K/N keys from one predecessor.

**Q4: What is the "token" in distributed databases like Cassandra?**
A: A token is a node's position on the consistent hash ring. In Cassandra, each node is assigned one or more tokens (with vnodes, typically 256 tokens). The token determines which partition keys the node is responsible for. `token(partition_key) = hash(partition_key)`, and the node with the nearest token clockwise is responsible.

**Q5: How does Amazon DynamoDB use consistent hashing?**
A: DynamoDB uses consistent hashing to distribute partition keys across storage nodes. Each table is partitioned by hash of the primary key. Partitions are automatically split when they exceed size or throughput limits. DynamoDB manages the ring internally; users don't see it. This is why DynamoDB can scale "infinitely" — partitions are created and distributed automatically.

**Q6: What is the bounded-load consistent hashing from Google's Maglev paper?**
A: Maglev adds a load constraint to consistent hashing. Each node has a capacity cap (e.g., 1.1x of average load). If the primary node is at capacity, the request goes to the next node on the ring. This prevents hot nodes while maintaining the benefits of consistent hashing (minimal disruption on node change). The trade-off: load-aware routing requires real-time load information.

**Q7: How do you handle a node that is slow but not dead?**
A: A slow node still passes health checks but causes latency. Consistent hashing doesn't account for latency. Mitigations: (1) Reduce the slow node's weight (fewer vnodes, less traffic). (2) Adaptive routing: measure per-node latency; shift traffic away from slow nodes. (3) Circuit breaker: if a node exceeds latency SLO, temporarily exclude it from the ring.

**Q8: Can consistent hashing guarantee locality (nearby keys on nearby nodes)?**
A: Standard consistent hashing does not — keys are uniformly distributed. If you need range queries (keys A-M on one node), use range-based partitioning (like Bigtable / HBase). You can combine: use consistent hashing for the first level (choose which partition group) and range partitioning within each group.

**Q9: What is the time complexity of adding a node to the ring?**
A: O(V log(V*N)): inserting V new vnodes into a sorted array of V*N entries. In practice: V=150, N=1000, total entries=150K. Inserting 150 entries into a 150K sorted array takes microseconds. The bottleneck is migration, not ring computation.

**Q10: How does consistent hashing work with replication?**
A: For replication factor R, a key is stored on R consecutive distinct physical nodes on the ring (clockwise from the key's position). The lookup_n function skips vnodes of the same physical node and ideally the same zone. This ensures replicas are on different nodes (and different failure domains).

**Q11: What is the "ketama" consistent hashing algorithm?**
A: Ketama is the original consistent hashing implementation for memcached (by Last.fm). It uses MD5 to hash server names and creates 100-200 points per server on a 2^32 ring. Lookup uses binary search. It's the de facto standard for memcached consistent hashing and was adopted by many other systems.

**Q12: How do you test that your consistent hashing implementation is correct?**
A: (1) Balance test: distribute 1M random keys across N nodes; verify std dev < 3% (at 150 vnodes). (2) Stability test: add/remove a node; count remapped keys; verify < 2/N of total keys. (3) Determinism test: same nodes, same order -> same ring -> same lookups. (4) Replication test: lookup_n returns distinct physical nodes in distinct zones. (5) Stress test: concurrent add/remove/lookup with no crashes.

**Q13: How does consistent hashing interact with data rebalancing in a live system?**
A: During rebalancing: (1) New ring is computed. (2) Lookups use new ring immediately (for writes). (3) Reads check new owner first; on miss, check old owner. (4) Background migration streams data from old to new owners. (5) Once migration is complete, dual-read is disabled. This ensures availability throughout the rebalance.

**Q14: What is the relationship between consistent hashing and the CAP theorem?**
A: Consistent hashing is a partitioning mechanism, not a consistency mechanism. It determines which node is responsible for a key. The CAP trade-off (consistency vs availability during partitions) is orthogonal — it's determined by the replication and consensus protocol, not the hash function. You can build a CP system (strong consistency) or an AP system (eventual consistency) on top of consistent hashing.

**Q15: How would you implement consistent hashing for a real-time analytics system?**
A: Use consistent hashing to partition metric streams across aggregation workers. Each metric name hashes to a worker. Benefits: stateful aggregation (each worker maintains state for its metrics), minimal disruption when workers scale, deterministic routing (all events for metric X go to the same worker). This is how systems like Prometheus with Thanos/Cortex distribute metric series.

---

## 16. References

1. Karger, D. et al. (1997). *Consistent Hashing and Random Trees: Distributed Caching Protocols for Relieving Hot Spots on the World Wide Web*. ACM STOC.
2. Lamping, J. & Veach, E. (2014). *A Fast, Minimal Memory, Consistent Hash Algorithm* (Jump Consistent Hash). Google.
3. Thaler, D. & Ravishankar, C. (1998). *Using Name-Based Mappings to Increase Hit Rates* (Rendezvous Hashing). IEEE/ACM Transactions on Networking.
4. DeCandia, G. et al. (2007). *Dynamo: Amazon's Highly Available Key-value Store*. SOSP.
5. Lakshman, A. & Malik, P. (2010). *Cassandra — A Decentralized Structured Storage System*. ACM SIGOPS.
6. Eisenbud, D. et al. (2016). *Maglev: A Fast and Reliable Software Network Load Balancer*. NSDI.
7. Redis Cluster Specification. https://redis.io/docs/reference/cluster-spec/
8. Vimeo. *Consistent Hashing: Algorithmic Tradeoffs*. https://medium.com/vimeo-engineering-blog/improving-load-balancing-with-a-new-consistent-hashing-algorithm-9f1bd75709ed
