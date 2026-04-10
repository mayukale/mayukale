# System Design: Distributed File System (HDFS/Ceph-like)

> **Relevance to role:** Cloud infrastructure platform engineers operating bare-metal IaaS must design and manage distributed file systems that underpin big-data pipelines (HDFS for MapReduce/Spark), block storage for VMs (Ceph RBD), container persistent volumes (CephFS/CSI), and object storage gateways. Understanding CRUSH placement, NameNode scalability, and POSIX vs relaxed consistency is essential for capacity planning, failure handling, and performance tuning at scale.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|-------------|--------|
| FR-1 | File CRUD | Create, read, write, delete, rename files |
| FR-2 | Directory hierarchy | Hierarchical namespace with directory listing |
| FR-3 | Large file support | Files up to tens of TB; 128 MB block/chunk size |
| FR-4 | Concurrent readers | Multiple clients read the same file simultaneously |
| FR-5 | Append-only writes | HDFS-style: write-once-read-many (or Ceph: random write) |
| FR-6 | Snapshots | Point-in-time, space-efficient snapshots |
| FR-7 | Quotas | Per-directory and per-user storage quotas |
| FR-8 | Block/Object/File interfaces | Ceph: RBD (block), CephFS (file), RGW (object) |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Durability | 11+ nines |
| NFR-2 | Availability | 99.99% read, 99.9% write |
| NFR-3 | Throughput | Saturate 25 Gbps per node (sequential reads) |
| NFR-4 | Latency | p99 < 10 ms metadata ops; p99 < 50 ms first byte (data) |
| NFR-5 | Scalability | Petabytes of storage, billions of files |
| NFR-6 | POSIX compliance | Full for CephFS; relaxed for HDFS |
| NFR-7 | Rack/failure domain awareness | No single rack failure loses data |

### Constraints & Assumptions
- Bare-metal servers: 12× 12 TB HDD + 2× 1.6 TB NVMe per storage node
- Network: 25 Gbps leaf-spine CLOS, 3 racks minimum
- Mixed workloads: big-data (HDFS), VM images (RBD), shared file storage (CephFS)
- Kubernetes cluster needs dynamic persistent volumes via CSI driver
- Java and Python clients (HDFS Java API, libcephfs, librbd)

### Out of Scope
- Object storage gateway (covered in distributed_object_storage.md)
- Real-time streaming file systems (FUSE performance optimization)
- NFS gateway implementation details
- Deduplication

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Total raw capacity | 200 storage nodes × 12 HDD × 12 TB | 28.8 PB raw |
| Usable capacity (3× replication) | 28.8 PB / 3 | 9.6 PB usable |
| Usable capacity (EC 4+2) | 28.8 PB / 1.5 | 19.2 PB usable |
| Total files (HDFS workload) | 500M files × 2 blocks avg | 1B blocks |
| Metadata per file | ~300 bytes (inode) | 500M × 300 B = 150 GB |
| Block metadata per block | ~150 bytes (locations) | 1B × 150 B = 150 GB |
| Total metadata in memory (HDFS NameNode) | 150 GB + 150 GB | ~300 GB |
| Concurrent MapReduce/Spark jobs | 50 jobs × 1000 tasks | 50K concurrent readers |
| Read throughput demand | 50K tasks × 100 MB/s each (limited by task) | 5 TB/s aggregate |
| Write throughput demand | 10K concurrent writers × 50 MB/s | 500 GB/s aggregate |

### Latency Requirements

| Operation | p50 | p99 |
|-----------|-----|-----|
| File open (metadata) | 2 ms | 10 ms |
| Sequential read (per block) | 5 ms (first byte) | 30 ms |
| Sequential write (per block) | 10 ms | 50 ms |
| Directory listing (1000 entries) | 5 ms | 20 ms |
| Create file | 5 ms | 20 ms |
| Rename | 3 ms | 15 ms |

### Storage Estimates

| Component | Calculation | Value |
|-----------|-------------|-------|
| NameNode heap (HDFS) | 300 GB metadata + JVM overhead | 384 GB RAM |
| Ceph MON database (RocksDB) | ~1 GB per million PGs | ~10 GB |
| Ceph MDS cache | 4 GB per MDS for 100M files | 4-16 GB |
| OSD journal (NVMe) | 2× NVMe SSDs per node for WAL/DB | 3.2 TB NVMe per node |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| Client read aggregate | 50K readers × 100 MB/s | ~5 TB/s |
| Client write aggregate | 10K writers × 50 MB/s | ~500 GB/s |
| Replication traffic (3×) | 500 GB/s × 2 (2 extra copies) | ~1 TB/s internal |
| Recovery/rebalance | 200 nodes × 100 MB/s throttle | ~20 GB/s |

---

## 3. High Level Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                        CLIENT LAYER                               │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐ │
│  │HDFS Java │  │ librbd   │  │ libcephfs│  │ Kubernetes CSI   │ │
│  │ Client   │  │ (block)  │  │ (POSIX)  │  │ Driver           │ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────┬───────────┘ │
└───────┼──────────────┼──────────────┼───────────────┼────────────┘
        │              │              │               │
┌───────┼──────────────┼──────────────┼───────────────┼────────────┐
│       │         METADATA LAYER      │               │            │
│  ┌────▼─────┐  ┌────▼──────────────▼───┐  ┌───────▼──────────┐ │
│  │ HDFS     │  │ Ceph MON Cluster       │  │ Ceph MDS Cluster │ │
│  │ NameNode │  │ (Paxos, CRUSH map,     │  │ (CephFS metadata,│ │
│  │ (Active/ │  │  OSD map, PG map)      │  │  inode cache,    │ │
│  │  Standby)│  │ [3 or 5 monitors]      │  │  directory tree) │ │
│  └────┬─────┘  └────┬──────────────────┘  └───────┬──────────┘ │
│       │              │                              │            │
└───────┼──────────────┼──────────────────────────────┼────────────┘
        │              │                              │
┌───────┼──────────────┼──────────────────────────────┼────────────┐
│       │         DATA / STORAGE LAYER                │            │
│  ┌────▼─────┐  ┌────▼─────┐  ┌──────────┐  ┌──────▼─────┐     │
│  │ HDFS     │  │ Ceph OSD │  │ Ceph OSD │  │ Ceph OSD   │     │
│  │ DataNode │  │ (BlueStore│  │ (BlueStore│  │ (BlueStore │     │
│  │ (block   │  │  on HDD)  │  │  on HDD)  │  │  on NVMe)  │     │
│  │  storage)│  └──────────┘  └──────────┘  └────────────┘     │
│  └──────────┘                                                    │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Rack 1           Rack 2           Rack 3                │   │
│  │  [Node 1..66]     [Node 67..133]   [Node 134..200]       │   │
│  └──────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **HDFS NameNode** | Central metadata server for HDFS; stores file→block mapping, namespace tree, block locations. Active/Standby HA via ZooKeeper + JournalNodes. |
| **HDFS DataNode** | Stores 128 MB blocks on local disks; sends heartbeats and block reports to NameNode; serves read/write pipelines. |
| **Ceph MON (Monitor)** | Maintains cluster map (OSD map, PG map, CRUSH map) via Paxos consensus; serves map updates to clients and OSDs. |
| **Ceph MDS (Metadata Server)** | Manages CephFS namespace (directories, inodes, permissions); caches hot metadata in memory; supports dynamic subtree partitioning for scalability. |
| **Ceph OSD (Object Storage Daemon)** | One per disk; manages local data using BlueStore (direct block device access); handles replication, recovery, scrubbing. |
| **BlueStore** | Ceph's storage backend; bypasses filesystem (no ext4/XFS); writes data directly to raw block device with RocksDB for metadata. |
| **Kubernetes CSI Driver** | Provisions and mounts Ceph RBD (block) or CephFS (file) volumes dynamically for pods. |

### Data Flows

**HDFS Write Path:**
1. Client contacts NameNode: `create(/user/data/file.parquet)`
2. NameNode allocates block ID, returns DataNode pipeline (3 nodes, rack-aware)
3. Client streams data to first DataNode (DN1)
4. DN1 writes to local disk and forwards to DN2; DN2 forwards to DN3 (pipeline replication)
5. ACKs propagate back: DN3→DN2→DN1→Client
6. Client requests next block allocation; repeat until file complete
7. Client calls `close()` → NameNode marks file as complete

**Ceph Write Path (RADOS):**
1. Client computes PG = `hash(object_name) mod num_PGs`
2. Client consults CRUSH map: PG → OSD set (e.g., [OSD.12, OSD.45, OSD.78])
3. Client sends write to primary OSD (OSD.12)
4. Primary writes to BlueStore (WAL → data → commit)
5. Primary forwards to replicas (OSD.45, OSD.78)
6. Replicas ACK to primary; primary ACKs to client
7. For erasure-coded pools: primary encodes and sends coded chunks to OSD set

---

## 4. Data Model

### Core Entities & Schema

**HDFS Metadata (in-memory on NameNode):**

```
INode (file or directory):
├── id: long (inode number)
├── name: byte[] (UTF-8 file name)
├── parent: INode reference
├── permission: FsPermission (owner, group, mode)
├── modificationTime: long
├── accessTime: long
├── [File-specific]:
│   ├── blocks: BlockInfo[] (ordered list of blocks)
│   ├── replication: short (default 3)
│   ├── blockSize: long (default 128 MB)
│   └── fileSize: long
└── [Directory-specific]:
    ├── children: INode[] (sorted)
    └── quota: {namespace: long, storage: long}

BlockInfo:
├── blockId: long
├── generationStamp: long
├── numBytes: long
└── datanodes: DatanodeDescriptor[] (locations, updated via block reports)
```

**Ceph Object (RADOS):**

```
RADOS Object:
├── oid: string (object ID, max 256 bytes)
├── pool: int (pool ID — determines replication/EC policy)
├── pg: int (placement group — hash(oid) mod num_PGs)
├── data: byte[] (object data, up to configured max, default ~100 MB)
├── xattrs: map<string, byte[]> (extended attributes)
└── omap: map<string, byte[]> (key-value metadata, stored in RocksDB)

CephFS Inode (MDS):
├── ino: uint64 (inode number)
├── mode: uint32 (permissions)
├── uid/gid: uint32
├── size: uint64
├── mtime/ctime/atime: timespec
├── nlink: uint32
├── layout: {pool, stripe_unit, stripe_count, object_size}
└── xattrs: map<string, byte[]>
```

### Database/Storage Selection

| Component | Technology | Rationale |
|-----------|-----------|-----------|
| HDFS metadata | In-memory (Java heap) on NameNode | Sub-ms lookups; persisted to EditLog + FsImage |
| HDFS edit log | JournalNode quorum (QJM) | HA; shared edit log between Active/Standby NN |
| Ceph cluster map | Paxos (MON) | Consensus across monitors; clients cache map |
| Ceph OSD metadata | RocksDB (inside BlueStore) | LSM-tree for per-object metadata, allocations |
| Ceph OSD data | Raw block device (BlueStore) | No filesystem overhead; direct I/O control |
| CephFS metadata | Ceph RADOS pool (MDS journals + metadata objects) | Distributed, durable; MDS caches hot metadata in RAM |

### Indexing Strategy

- **HDFS:** In-memory B-tree (INodeDirectory.children sorted array); no disk-based index
- **Ceph CRUSH:** Algorithmic placement — no lookup table; O(log N) CRUSH computation
- **Ceph RocksDB (per OSD):** LSM-tree indexes object metadata by OID within each PG prefix
- **CephFS MDS:** In-memory inode cache with LRU eviction; directory fragments indexed by name hash

---

## 5. API Design

### File System APIs

**HDFS Java API:**
```java
// Configuration and FileSystem instantiation
Configuration conf = new Configuration();
conf.set("fs.defaultFS", "hdfs://nameservice1");
FileSystem fs = FileSystem.get(conf);

// Write
FSDataOutputStream out = fs.create(new Path("/data/file.parquet"),
    (short) 3,   // replication factor
    134217728);   // 128 MB block size
out.write(buffer);
out.close();

// Read
FSDataInputStream in = fs.open(new Path("/data/file.parquet"));
in.seek(offset);
in.readFully(buffer);
in.close();

// Directory operations
FileStatus[] listing = fs.listStatus(new Path("/data/"));
fs.mkdirs(new Path("/data/output/"));
fs.rename(new Path("/data/tmp"), new Path("/data/final"));
fs.delete(new Path("/data/old"), true);  // recursive

// Snapshots
fs.createSnapshot(new Path("/data"), "snap1");
fs.deleteSnapshot(new Path("/data"), "snap1");
```

**Ceph librados (Python):**
```python
import rados

cluster = rados.Rados(conffile='/etc/ceph/ceph.conf')
cluster.connect()

# RADOS object operations
ioctx = cluster.open_ioctx('mypool')
ioctx.write_full('myobject', b'data content')
data = ioctx.read('myobject')
ioctx.remove_object('myobject')

# Listing
for obj in ioctx.list_objects():
    print(obj.key)

ioctx.close()
cluster.shutdown()
```

**Ceph RBD (block device):**
```python
import rbd

# Create and map a block device
ioctx = cluster.open_ioctx('rbd_pool')
rbd_inst = rbd.RBD()
rbd_inst.create(ioctx, 'myvolume', 100 * 1024**3)  # 100 GB

# Open image for I/O
image = rbd.Image(ioctx, 'myvolume')
image.write(b'data', offset=0)
data = image.read(0, 4096)
image.create_snap('snap1')
image.close()
```

**Kubernetes CSI (YAML):**
```yaml
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: ceph-rbd-pvc
spec:
  accessModes: [ReadWriteOnce]
  storageClassName: ceph-rbd
  resources:
    requests:
      storage: 50Gi
---
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: ceph-rbd
provisioner: rbd.csi.ceph.com
parameters:
  clusterID: ceph-cluster-1
  pool: kubernetes
  csi.storage.k8s.io/fstype: ext4
reclaimPolicy: Delete
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: CRUSH Algorithm (Ceph Placement)

**Why it's hard:** Placing data across hundreds of storage devices while respecting fault domains (racks, hosts, DCs), handling heterogeneous capacities, and minimizing data movement during cluster changes requires a deterministic, scalable algorithm. Traditional hash-mod approaches cause massive reshuffling when nodes are added/removed.

**Approaches:**

| Approach | Data Movement on Change | Fault Domain Aware | Heterogeneous Capacity | Lookup Cost |
|----------|------------------------|--------------------|-----------------------|------------|
| Hash-mod (hash % N) | ~100% on N change | No | No | O(1) |
| Consistent hashing | ~1/N | No (without enhancement) | Via virtual nodes | O(log N) |
| Random slicing | Minimal | Configurable | Yes | O(1) |
| CRUSH (Controlled Replication Under Scalable Hashing) | Minimal, tunable | Yes (tree hierarchy) | Yes (weights) | O(log N) |
| RUSH (Replication Under Scalable Hashing) | Minimal | Limited | Yes | O(N) |

**Selected approach:** CRUSH algorithm (Ceph's native placement).

**Justification:**
- Deterministic: all clients independently compute the same placement without centralized lookup
- Hierarchy-aware: models racks, rows, DCs as a tree; placement rules enforce fault domain separation
- Weight-based: larger disks get proportionally more data
- Minimal data movement: adding a node moves only ~1/N of data
- Battle-tested: production-proven at exabyte scale

**Implementation Detail:**

```python
class CRUSHMap:
    """Simplified CRUSH algorithm implementation."""

    def __init__(self):
        self.buckets = {}    # id → CRUSHBucket (rack, host, root)
        self.devices = {}    # id → CRUSHDevice (OSD)
        self.rules = {}      # rule_id → list of CRUSHStep

    def place(self, pg_id: int, rule_id: int, num_replicas: int) -> list[int]:
        """Compute OSD set for a placement group using CRUSH rules."""
        rule = self.rules[rule_id]
        result = []
        x = pg_id  # Input to CRUSH hash

        for step in rule:
            if step.op == 'take':
                # Start from a specific bucket (e.g., root "default")
                current_bucket = self.buckets[step.bucket_id]

            elif step.op == 'chooseleaf_firstn':
                # Choose N items from current bucket, each from a different
                # leaf subtree (fault domain)
                result = self._choose_leaf(
                    x, current_bucket, step.num or num_replicas,
                    step.type)  # type = 'rack', 'host', etc.

            elif step.op == 'emit':
                pass  # Output the result

        return result

    def _choose_leaf(self, x: int, bucket, num: int,
                     fault_domain_type: str) -> list[int]:
        """
        Descend the CRUSH tree, choosing one item per fault domain.
        Uses straw2 bucket algorithm for weighted selection.
        """
        selected = []
        used_domains = set()

        for rep in range(num):
            # Hash with replica index for independence
            r = rep
            attempts = 0
            max_attempts = 50  # Prevent infinite loop

            while attempts < max_attempts:
                # Straw2 algorithm: each child draws a "straw" weighted
                # by its weight. Longest straw wins.
                candidate = self._straw2_select(bucket, x, r)

                # Descend to leaf (OSD)
                leaf = self._descend_to_leaf(candidate, x, r)

                # Check fault domain constraint
                domain = self._get_fault_domain(leaf, fault_domain_type)
                if domain not in used_domains and leaf not in selected:
                    if self._is_device_alive(leaf):
                        selected.append(leaf)
                        used_domains.add(domain)
                        break

                r += 1  # Retry with different hash
                attempts += 1

        return selected

    def _straw2_select(self, bucket, x: int, r: int) -> int:
        """
        Straw2 bucket type: each child draws a straw.
        straw_length = hash(x, child_id, r) / ln(weight/total_weight)
        The child with the longest straw is selected.
        """
        best_straw = -1
        best_child = None

        for child in bucket.children:
            # Jenkins hash for determinism
            draw = crush_hash(x, child.id, r)
            # Straw2: adjust by ln(weight) to handle weight changes gracefully
            straw = draw / (-math.log(child.weight / bucket.total_weight))

            if straw > best_straw:
                best_straw = straw
                best_child = child

        return best_child

    def _descend_to_leaf(self, node, x: int, r: int) -> int:
        """Recursively descend from node to a leaf OSD."""
        if node.is_device:
            return node.id

        child = self._straw2_select(node, x, r)
        return self._descend_to_leaf(child, x, r + 1)

    def _get_fault_domain(self, osd_id: int, domain_type: str) -> str:
        """Walk up the tree to find the fault domain of the given type."""
        node = self.devices[osd_id]
        while node.parent is not None:
            node = node.parent
            if node.type == domain_type:
                return node.id
        return None


# Example CRUSH rule for 3× replication across racks
example_rule = [
    CRUSHStep(op='take', bucket_id='root_default'),
    CRUSHStep(op='chooseleaf_firstn', num=3, type='rack'),
    CRUSHStep(op='emit')
]
# This ensures each replica lands on a different rack.
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| OSD failure | PGs on that OSD degrade | CRUSH recalculates; data re-replicated from surviving copies |
| Rack failure | All OSDs in rack unavailable | CRUSH rule ensures replicas on other racks; PGs degrade but remain available |
| CRUSH map out of sync | Client places data on wrong OSDs | MONs distribute map via Paxos; clients refresh on every operation |
| Straw2 hash collision | Two replicas on same fault domain | Retry loop with incremented hash seed; max_attempts guard |
| Weight misconfiguration | Uneven data distribution | `ceph osd reweight` to adjust; CRUSH auto-tuning |
| Thundering herd on OSD addition | All PGs with data on new OSD rebalance simultaneously | `osd_max_backfills` limit (default 1 per OSD) |

**Interviewer Q&As:**

**Q1: Why is CRUSH better than consistent hashing for Ceph?**
A: Consistent hashing doesn't natively model fault domain hierarchy (racks, hosts, DCs). CRUSH encodes the physical topology as a tree and enforces placement rules at each level. It also handles heterogeneous weights natively (larger disks get more data) without virtual nodes.

**Q2: What happens to data placement when you add a new rack?**
A: CRUSH recomputes placement for all PGs. Only PGs whose optimal placement includes the new rack's OSDs will move data. With proper weights, roughly 1/(N+1) of data moves, where N is the number of racks. The `osd_max_backfills` setting throttles rebalancing to prevent network saturation.

**Q3: How does CRUSH handle an OSD that's temporarily down vs permanently removed?**
A: Temporarily down: OSD is marked `out` after a timeout (default 600s). PGs are remapped to other OSDs. When it comes back, PGs rebalance back (if the data is still valid). Permanently removed: OSD is removed from CRUSH map; data is permanently remapped.

**Q4: What is the PG (Placement Group) and why is it needed?**
A: PGs are an intermediate mapping between objects and OSDs. Without PGs, CRUSH would compute placement per-object (billions of computations). PGs group objects (hash(oid) mod num_PGs) so CRUSH only computes placement for thousands of PGs. PG count is configured per pool (recommended: ~100 PGs per OSD).

**Q5: How does Straw2 improve over Straw1?**
A: Straw1 recomputed all straws when any weight changed, causing unnecessary data movement. Straw2 uses `ln(weight)` adjustment so that changing one OSD's weight only affects data going to/from that OSD — other OSDs' placements are stable.

**Q6: Can CRUSH be used for erasure-coded pools?**
A: Yes. The CRUSH rule specifies `chooseleaf_firstn` with `num = k + m` (e.g., 6+2=8). Each chunk (data or parity) is placed on a different fault domain. The primary OSD receives the write, encodes, and distributes chunks.

---

### Deep Dive 2: HDFS NameNode Scalability

**Why it's hard:** HDFS NameNode stores the entire filesystem namespace in memory. At 300 bytes per inode + 150 bytes per block, a cluster with 500M files and 1B blocks needs ~300 GB of heap. This creates single-point-of-failure risks, garbage collection issues, and startup time problems (loading the FsImage).

**Approaches:**

| Approach | Max Files | HA | Memory | Startup Time | Complexity |
|----------|-----------|-----|--------|-------------|-----------|
| Single NameNode | ~300M (with 256 GB heap) | Standby NN | High | 10-30 min | Low |
| HDFS Federation | Billions (namespace sharding) | Per-NN Standby | Distributed | Per-NN: 5-15 min | Medium |
| HDFS Observer NameNode | Same as above | Read scaling | Same | Same | Medium |
| NameNode on persistent memory (PMEM) | More (faster restart) | Standby | PMEM-backed | Seconds | Medium |
| Replace with Ceph MDS | Unlimited (dynamic subtree) | Active-active | Moderate | Seconds | High (different system) |

**Selected approach:** HDFS Federation with Observer NameNodes for read scaling.

**Justification:**
- Federation splits namespace into independent namespaces (e.g., `/user` on NN1, `/data` on NN2), each with its own NN
- Each NN's memory is bounded; total cluster supports billions of files
- Observer NameNodes handle read-heavy metadata operations (listStatus, getFileInfo) without loading the active NN
- Maintains backward compatibility with existing HDFS clients

**Implementation Detail:**

```java
/**
 * HDFS Federation: multiple independent NameNodes share the same DataNodes.
 * Each NameNode manages a "namespace volume" — a portion of the directory tree.
 * Block Pool: each NameNode has its own block pool ID; DataNodes store blocks
 * from multiple pools on the same disks.
 */
public class FederatedNameNodeConfig {

    // hdfs-site.xml configuration for Federation
    public static Configuration buildFederationConfig() {
        Configuration conf = new Configuration();

        // Define nameservices
        conf.set("dfs.nameservices", "ns1,ns2");

        // Namespace 1: handles /user, /tmp
        conf.set("dfs.ha.namenodes.ns1", "nn1,nn2");
        conf.set("dfs.namenode.rpc-address.ns1.nn1", "nn1-host:8020");
        conf.set("dfs.namenode.rpc-address.ns1.nn2", "nn2-host:8020");

        // Namespace 2: handles /data, /hbase
        conf.set("dfs.ha.namenodes.ns2", "nn3,nn4");
        conf.set("dfs.namenode.rpc-address.ns2.nn3", "nn3-host:8020");
        conf.set("dfs.namenode.rpc-address.ns2.nn4", "nn4-host:8020");

        // ViewFS mount table (client-side routing)
        conf.set("fs.defaultFS", "viewfs://cluster1");
        conf.set("fs.viewfs.mounttable.cluster1.link./user", "hdfs://ns1/user");
        conf.set("fs.viewfs.mounttable.cluster1.link./tmp", "hdfs://ns1/tmp");
        conf.set("fs.viewfs.mounttable.cluster1.link./data", "hdfs://ns2/data");
        conf.set("fs.viewfs.mounttable.cluster1.link./hbase", "hdfs://ns2/hbase");

        // Observer NameNode (HDFS 2.9+)
        conf.set("dfs.ha.namenodes.ns1", "nn1,nn2,nn1-observer");
        conf.set("dfs.namenode.state.context.enabled", "true");

        return conf;
    }
}

/**
 * NameNode memory management and GC optimization.
 * Critical for large heaps (256-384 GB).
 */
public class NameNodeJVMConfig {

    public static String[] getJVMFlags(int heapGB) {
        return new String[] {
            "-Xms" + heapGB + "g",
            "-Xmx" + heapGB + "g",

            // Use G1GC for large heaps (better than CMS for > 32 GB)
            "-XX:+UseG1GC",
            "-XX:G1HeapRegionSize=32m",

            // Target max GC pause 200ms
            "-XX:MaxGCPauseMillis=200",

            // Pre-allocate heap to avoid fragmentation
            "-XX:+AlwaysPreTouch",

            // Reduce GC overhead for metadata-heavy workload
            "-XX:InitiatingHeapOccupancyPercent=45",
            "-XX:G1MixedGCLiveThresholdPercent=85",

            // Large page support (if OS configured)
            "-XX:+UseLargePages",
            "-XX:LargePageSizeInBytes=2m",

            // GC logging
            "-Xlog:gc*:file=/var/log/hadoop/nn-gc.log:time,uptime,tags:filecount=10,filesize=100m"
        };
    }
}

/**
 * DataNode block pool management for Federation.
 * A single DataNode stores blocks from multiple NameNode block pools.
 */
public class DataNodeBlockPoolManager {

    private final Map<String, BlockPoolSlice> blockPools = new ConcurrentHashMap<>();

    /**
     * Each block pool is a separate directory structure on the DataNode.
     * Layout:
     *   /data/dn/current/
     *     BP-1234-nn1/current/finalized/subdir0/subdir0/blk_xxx
     *     BP-5678-nn2/current/finalized/subdir0/subdir0/blk_yyy
     */
    public void registerBlockPool(String blockPoolId, String nameserviceId) {
        BlockPoolSlice slice = new BlockPoolSlice(blockPoolId);
        slice.initialize();
        blockPools.put(blockPoolId, slice);

        // Start heartbeat thread for this block pool's NameNode
        heartbeatManager.addNameService(nameserviceId, blockPoolId);

        // Send initial block report
        sendBlockReport(blockPoolId);
    }

    /**
     * Block report: DataNode sends full list of blocks to each NameNode.
     * For 100K blocks per pool, this is ~15 MB; sent every 6 hours.
     * Incremental block reports (IBR) sent on every block add/delete.
     */
    public void sendBlockReport(String blockPoolId) {
        BlockPoolSlice slice = blockPools.get(blockPoolId);
        List<BlockReport> reports = slice.generateBlockReport();

        // Full block report — can be large; use incremental for steady state
        nameNodeProxy.blockReport(blockPoolId, reports);
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Active NameNode crash | Writes blocked until failover | Standby NN promotes via ZooKeeper ZKFC; < 30s failover |
| Both Active+Standby NN fail | Namespace unavailable | JournalNodes preserve edit log; cold start from FsImage + editlog |
| NameNode OOM | Crash; long restart (loading FsImage) | Proper heap sizing; Federation to split load; monitoring heap usage |
| GC pause > 30s | DataNodes think NN is dead; trigger false failover | G1GC tuning; ZK timeout > max expected GC pause |
| EditLog corruption | Metadata loss | QJM (quorum journal manager) — majority of JournalNodes must ACK |
| FsImage corruption | Cannot restart NN | Keep multiple checkpoint images; Standby NN creates checkpoints |

**Interviewer Q&As:**

**Q1: How does ViewFS work in Federation?**
A: ViewFS is a client-side mount table. The client translates paths like `/data/file.txt` to `hdfs://ns2/data/file.txt` using the mount table configuration. It's similar to Unix mount points but implemented entirely in the HDFS client library. No server-side routing.

**Q2: What's the maximum number of files a single NameNode can handle?**
A: Rule of thumb: 1 million files per GB of NameNode heap. With 256 GB heap, ~250M files (with some overhead for blocks). Beyond that, either add more heap (384 GB requires careful GC tuning) or use Federation to split namespaces.

**Q3: How does Observer NameNode maintain consistency?**
A: Observer NN tails the EditLog from JournalNodes and applies edits to its in-memory state. It's always slightly behind the Active NN (milliseconds). For strong consistency, the client can include a "state context" (txid) from a prior write; the Observer won't serve the read until it has caught up to that txid.

**Q4: What happens during NameNode failover to running Spark/MapReduce jobs?**
A: Clients retry RPC calls with exponential backoff. The DFSClient is configured with `dfs.client.failover.max.attempts` (default 15). Jobs typically survive a < 30s failover without failing. Longer outages may cause task timeouts and retries at the application level.

**Q5: How does the NameNode handle fsck (filesystem check)?**
A: `hdfs fsck /` traverses the in-memory namespace and checks: (1) under-replicated blocks, (2) corrupt blocks (checksums failed on all replicas), (3) missing blocks. It's a read-only operation on the NameNode's in-memory state, so it doesn't require disk I/O, but it holds the NameNode read lock, which can affect performance during the scan.

**Q6: Why not just use a database (MySQL, PostgreSQL) for NameNode metadata?**
A: Latency. The NameNode processes ~100K metadata operations per second. Each operation would require a database round trip (~1-5ms), creating a bottleneck. In-memory processing gives sub-millisecond latency. The EditLog (write-ahead log to JournalNodes) provides durability without per-operation database overhead.

---

### Deep Dive 3: Ceph BlueStore and I/O Path

**Why it's hard:** Traditional Ceph used FileStore (XFS/ext4 on top of block devices), which caused double-write penalty: data written to XFS journal, then to actual location. BlueStore eliminates this by managing the block device directly, but must implement space allocation, checksumming, compression, and metadata storage (RocksDB) without a filesystem.

**Approaches:**

| Approach | Write Amplification | Latency | Checksums | Compression | Complexity |
|----------|-------------------|---------|-----------|-------------|-----------|
| FileStore (XFS) | 2-3× (journal + data) | High | None (relies on XFS) | None | Low |
| BlueStore | 1-1.5× | Low | Per-block CRC32C | Snappy/zstd inline | High |
| SeaStore (future) | ~1× (seastar framework) | Very low | Yes | Yes | Very high |
| Raw RADOS on ZFS | 1.5-2× (COW) | Medium | ZFS checksums | ZFS compression | Medium |

**Selected approach:** BlueStore (Ceph default since Luminous 12.x).

**Implementation Detail:**

```python
class BlueStoreWritePath:
    """
    Simplified BlueStore write path.
    
    BlueStore layout on a raw block device:
    ┌──────────────────────────────────────────────────┐
    │  Superblock (4KB)                                │
    ├──────────────────────────────────────────────────┤
    │  BlueFS (small filesystem for RocksDB)           │
    │  ┌─────────────────────────────────────────────┐ │
    │  │  RocksDB: object metadata, PG metadata,     │ │
    │  │  allocation bitmap, omap data               │ │
    │  └─────────────────────────────────────────────┘ │
    ├──────────────────────────────────────────────────┤
    │  Data region (bulk of the device)                │
    │  ┌─────────────────────────────────────────────┐ │
    │  │  Object data extents (variable size)        │ │
    │  └─────────────────────────────────────────────┘ │
    └──────────────────────────────────────────────────┘
    
    Two write strategies:
    1. Deferred write: small writes (< min_alloc_size) go to WAL on NVMe,
       then merged later
    2. Simple write: large writes go directly to allocated extent on HDD
    """

    def __init__(self, block_device, wal_device, db_device):
        self.data_device = block_device       # HDD for data
        self.wal_device = wal_device           # NVMe for WAL
        self.db_device = db_device             # NVMe for RocksDB
        self.allocator = StupidAllocator(block_device.size)  # bitmap allocator
        self.rocksdb = RocksDB(db_device)
        self.min_alloc_size = 64 * 1024        # 64 KB for HDD

    def write_object(self, oid: str, data: bytes, offset: int):
        """Write data to a RADOS object."""
        
        obj_meta = self.rocksdb.get(f"O:{oid}")  # Get existing metadata
        
        if len(data) >= self.min_alloc_size:
            # SIMPLE WRITE: allocate new extent, write data directly
            return self._simple_write(oid, obj_meta, data, offset)
        else:
            # DEFERRED WRITE: write to WAL first, merge later
            return self._deferred_write(oid, obj_meta, data, offset)

    def _simple_write(self, oid, obj_meta, data, offset):
        """
        Large write path:
        1. Allocate new extent on data device
        2. Write data to new extent (direct I/O)
        3. Update RocksDB metadata atomically
        """
        # Step 1: Allocate
        extent = self.allocator.allocate(len(data))
        
        # Step 2: Write data (checksum before write)
        checksum = crc32c(data)
        self.data_device.write(extent.offset, data)
        
        # Step 3: Atomic metadata update in RocksDB
        # (old extent freed after RocksDB commit)
        old_extents = obj_meta.get_extents(offset, len(data))
        new_extent_map = obj_meta.extent_map.replace(
            offset, len(data), extent, checksum)
        
        batch = RocksDBWriteBatch()
        batch.put(f"O:{oid}", new_extent_map.serialize())
        batch.put(f"C:{oid}:{extent.offset}", checksum.to_bytes())
        self.rocksdb.write(batch)
        
        # Free old extents
        for old_ext in old_extents:
            self.allocator.release(old_ext)

    def _deferred_write(self, oid, obj_meta, data, offset):
        """
        Small write path (avoids HDD random I/O):
        1. Write data + intent to WAL (NVMe — fast)
        2. Update RocksDB metadata to point to WAL location
        3. Background thread later copies WAL data to data device
        """
        # Step 1: Write to WAL (NVMe)
        wal_offset = self.wal_device.append(data)
        
        # Step 2: Record deferred IO in RocksDB
        deferred_io = DeferredIO(oid=oid, offset=offset,
                                  length=len(data),
                                  wal_offset=wal_offset)
        batch = RocksDBWriteBatch()
        batch.put(f"D:{oid}:{offset}", deferred_io.serialize())
        self.rocksdb.write(batch)
        
        # Step 3: ACK to client immediately
        # Background thread handles cleanup
        return WriteResult(success=True, deferred=True)

    def read_object(self, oid: str, offset: int, length: int) -> bytes:
        """Read data, checking for deferred writes in WAL."""
        obj_meta = self.rocksdb.get(f"O:{oid}")
        
        # Check for any deferred (not-yet-migrated) writes overlapping this range
        deferred = self.rocksdb.range_scan(
            f"D:{oid}:{offset}", f"D:{oid}:{offset + length}")
        
        # Read base data from data device
        result = bytearray(length)
        for extent in obj_meta.get_extents(offset, length):
            chunk = self.data_device.read(extent.offset, extent.length)
            # Verify checksum
            expected_crc = self.rocksdb.get(f"C:{oid}:{extent.offset}")
            if crc32c(chunk) != expected_crc:
                raise ChecksumError(f"Corruption in {oid} at {extent.offset}")
            result[extent.obj_offset:extent.obj_offset + extent.length] = chunk
        
        # Overlay any deferred writes
        for d in deferred:
            wal_data = self.wal_device.read(d.wal_offset, d.length)
            result[d.offset - offset:d.offset - offset + d.length] = wal_data
        
        return bytes(result)
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| NVMe WAL device failure | Deferred writes lost; in-flight data at risk | Replicated at RADOS level (primary ACKs only after replicas confirm) |
| HDD data device failure | OSD marks itself down; data served from replicas | Automatic OSD down detection; PG remapping |
| RocksDB corruption | OSD cannot read metadata; data inaccessible | RocksDB WAL + checkpoints; OSD can rebuild from other replicas |
| Checksum mismatch on read | Silent data corruption detected | Return error; read from replica OSD; trigger scrub on corrupted OSD |
| Allocator bitmap corruption | Space leak or double allocation | Periodic `ceph-bluestore-tool fsck` validates allocation bitmap |
| Power loss during simple write | Partial write (data written, metadata not) | On restart, RocksDB replays WAL; orphaned extents reclaimed by fsck |

**Interviewer Q&As:**

**Q1: Why does BlueStore use RocksDB instead of a simpler key-value store?**
A: RocksDB provides: (1) atomic batch writes (critical for updating extent maps + checksums atomically), (2) efficient range scans (needed for omap data), (3) compression (metadata is highly compressible), (4) well-tested crash recovery via its own WAL. The overhead of running RocksDB is small compared to the I/O savings from eliminating the double-write problem.

**Q2: What is BlueFS and why is it needed?**
A: BlueFS is a minimal filesystem that provides the POSIX-like interface RocksDB requires (open, read, write, sync). It runs on NVMe SSDs and manages the WAL and SST files for RocksDB. It avoids using a full filesystem (ext4/XFS) on NVMe, which would add unnecessary overhead.

**Q3: How does BlueStore handle inline compression?**
A: When compression is enabled, BlueStore compresses data at write time (using Snappy or zstd). The extent map records compressed length vs logical length. On read, the compressed extent is read from disk and decompressed. Compression is transparent to RADOS clients. It works best for data pools with compressible data (text, logs); not recommended for already-compressed data (media).

**Q4: What is the min_alloc_size and why does it differ for HDD vs SSD?**
A: `min_alloc_size` is the minimum allocation unit. For HDD: 64 KB (matches seek granularity; smaller allocations waste seek time). For NVMe SSD: 4 KB (NVMe handles small random I/O efficiently). Writes smaller than min_alloc_size use the deferred write path to avoid wasting large allocations.

**Q5: How does BlueStore's checksumming compare to ZFS?**
A: Both provide per-block checksums (CRC32C). BlueStore stores checksums in RocksDB; ZFS stores them in the block pointer tree. Key difference: BlueStore checksums are at the RADOS object level, so Ceph can automatically repair from replicas. ZFS can only self-heal with RAID-Z or mirrors.

**Q6: What is the "deep scrub" process?**
A: Deep scrub reads all data from disk and verifies checksums (vs "scrub" which only checks metadata). It runs as a background process on each OSD, cycling through all PGs. It detects silent data corruption (bit rot). If corruption is found, the primary OSD repairs from a healthy replica. Default interval: once per week.

---

### Deep Dive 4: Network Topology Awareness and Rack-Aware Replication

**Why it's hard:** In a multi-rack deployment, placing all replicas on the same rack means a rack failure (power, ToR switch) loses all copies. Placing replicas on different racks increases cross-rack traffic. The optimal strategy balances durability against network efficiency.

**Selected approach:** HDFS default: 2 replicas on one rack, 1 on another. Ceph: CRUSH rule `chooseleaf_firstn` across racks.

**Implementation Detail:**

```java
/**
 * HDFS NetworkTopology: models the cluster as a tree.
 *
 *         /default-rack
 *        /      \
 *    /rack1    /rack2    /rack3
 *    /    \    /    \    /    \
 *  dn1   dn2 dn3  dn4 dn5  dn6
 *
 * BlockPlacementPolicyDefault:
 * - 1st replica: on the writer's node (or random if external client)
 * - 2nd replica: on a different rack (cross-rack durability)
 * - 3rd replica: same rack as 2nd (avoid 3 cross-rack transfers)
 *
 * This gives:
 * - Rack fault tolerance: survives any single rack failure
 * - Write: 1 cross-rack transfer (2nd replica); 3rd is local to rack
 * - Read: can read from local rack if data is on writer's rack
 */
public class RackAwarePlacementPolicy {

    private final NetworkTopology topology;

    public DatanodeDescriptor[] chooseTargets(String srcNode, int numReplicas,
                                               long blockSize) {
        DatanodeDescriptor[] targets = new DatanodeDescriptor[numReplicas];

        // 1st replica: local node (or random if client is external)
        DatanodeDescriptor first;
        if (topology.contains(srcNode)) {
            first = topology.getNode(srcNode);
        } else {
            first = chooseRandom(null, null);
        }
        targets[0] = first;

        // 2nd replica: different rack from first
        DatanodeDescriptor second = chooseRandom(
            excludeRack(first.getNetworkLocation()),
            exclude(first));
        targets[1] = second;

        // 3rd replica: same rack as second, different node
        DatanodeDescriptor third = chooseRandom(
            sameRack(second.getNetworkLocation()),
            exclude(first, second));
        targets[2] = third;

        return targets;
    }

    /**
     * Pipeline order: data flows client → dn1 → dn2 → dn3
     * Sort to minimize network hops: put nodes on same rack adjacent.
     */
    public DatanodeDescriptor[] sortPipeline(DatanodeDescriptor[] targets,
                                              DatanodeDescriptor client) {
        // Sort by network distance from client
        Arrays.sort(targets, (a, b) -> {
            int distA = topology.getDistance(client, a);
            int distB = topology.getDistance(client, b);
            return Integer.compare(distA, distB);
        });
        return targets;
    }
}
```

**Interviewer Q&As:**

**Q1: Why doesn't HDFS put all 3 replicas on different racks?**
A: Cost vs durability. Putting all 3 on different racks would require 2 cross-rack transfers (instead of 1). Cross-rack bandwidth is often a bottleneck. The 2-rack policy (2 on one rack, 1 on another) survives any single rack failure while minimizing cross-rack traffic by 33%.

**Q2: How does Ceph's approach differ from HDFS's?**
A: Ceph CRUSH puts one replica per rack (for replication factor 3, across 3 racks). This is more durable (survives 2 rack failures vs 1) but costs more cross-rack bandwidth. Ceph's primary OSD handles all writes, so the replication traffic pattern is: primary → replica1 (cross-rack) + primary → replica2 (cross-rack).

**Q3: How do you handle a misaligned topology (some racks full, some empty)?**
A: Both HDFS and Ceph handle this. HDFS's placement policy falls back to other racks if the preferred rack is full. Ceph's CRUSH weights handle uneven racks — OSDs on larger racks get more data proportionally.

**Q4: What if the cluster has only 2 racks?**
A: With replication factor 3 and only 2 racks, one rack holds 2 replicas. A single rack failure degrades data but doesn't lose it. For better durability, consider erasure coding (EC) or expanding to 3+ racks.

**Q5: How does topology awareness affect read latency?**
A: HDFS clients prefer reading from the closest replica (measured by network distance). Intra-rack reads avoid the ToR→spine→ToR path, saving ~0.5ms and reducing spine switch load. Ceph clients always read from the primary OSD, which may be on any rack.

**Q6: How do you model a multi-datacenter topology?**
A: Extend the tree: `/dc1/rack1/node1`, `/dc2/rack1/node2`. CRUSH rules can enforce: 1 replica per DC, or 2 replicas in local DC + 1 in remote DC. HDFS supports custom topology scripts that map hostnames to rack paths, which can include DC prefixes.

---

## 7. Scaling Strategy

**Dimension scaling:**

| Dimension | HDFS Approach | Ceph Approach |
|-----------|--------------|---------------|
| Storage capacity | Add DataNodes; NameNode limits at ~500M files | Add OSDs; CRUSH auto-rebalances |
| Metadata capacity | HDFS Federation (multiple NameNodes) | Add MDS for CephFS; MON scales to 5-7 |
| Read throughput | Observer NameNode + more DataNodes | More OSDs + primary affinity |
| Write throughput | More DataNodes (pipeline replication) | More OSDs (primary distributes writes) |
| File count | Federation (namespace sharding) | PG splitting (auto in recent Ceph) |

**Interviewer Q&As:**

**Q1: How do you handle a 10× growth in data over 2 years?**
A: For HDFS: add DataNodes and ensure NameNode heap is sufficient (or federate). For Ceph: add OSDs incrementally; CRUSH auto-rebalances. Both: plan for 3× replication overhead or switch to EC for cold data. Capacity planning: monitor `dfs.used%` and `ceph df` weekly; alert at 70% utilization.

**Q2: What's the maximum cluster size you've seen in production?**
A: HDFS: Yahoo ran 4500+ DataNodes, 600 PB, 1M+ files. Meta's HDFS clusters exceed an exabyte. Ceph: CERN runs multi-petabyte Ceph clusters with 10K+ OSDs. The practical limits are network (spine bandwidth) and operational (rolling upgrades across thousands of nodes).

**Q3: How do you handle mixed workloads (batch + low-latency) on the same cluster?**
A: HDFS: use YARN queues with resource isolation; separate NameNode namespaces for latency-sensitive vs batch. Ceph: use separate pools with different CRUSH rules (NVMe pool for low-latency, HDD pool for batch). QoS at the OSD level via `osd_mclock_scheduler`.

**Q4: How do you migrate data between storage tiers (HDD → SSD)?**
A: HDFS: use HDFS Storage Policies (HOT, WARM, COLD, ALL_SSD). The Mover tool relocates blocks between storage types. Ceph: use CRUSH rules to assign different pools to different device classes (HDD, SSD, NVMe). Move objects between pools using `rados cppool` or tiering.

**Q5: How do you decommission a rack?**
A: HDFS: `hdfs dfsadmin -decommission dn1,dn2,...` — NameNode replicates blocks off decommissioning nodes before they're shut down. Ceph: `ceph osd out osd.X` for each OSD in the rack — CRUSH redistributes PGs. Both processes are bandwidth-limited to avoid disruption.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO | RPO |
|---|---------|-----------|--------|----------|-----|-----|
| 1 | Single OSD/DataNode failure | Heartbeat timeout (30s HDFS, 120s Ceph) | Degraded replicas for affected blocks | Auto-replication from surviving copies | 0 (reads from other replicas) | 0 |
| 2 | Rack failure (power/ToR) | Multiple heartbeat timeouts | Many blocks lose 1-2 replicas | Priority re-replication; can survive with remaining replicas | 0 (degraded) | 0 |
| 3 | HDFS NameNode Active crash | ZooKeeper session timeout (30-45s) | Metadata writes blocked | Standby NN fences Active, takes over | < 45s | 0 (shared EditLog) |
| 4 | Ceph MON leader failure | Paxos lease timeout | Map updates blocked briefly | New leader elected by Paxos | < 10s | 0 |
| 5 | CephFS MDS failure | MON detects heartbeat loss | CephFS clients hang (caps expire) | Standby MDS takes over; replays journal | < 30s | 0 |
| 6 | Network partition (rack isolated) | BGP/LACP failure detection | Isolated rack's nodes unreachable | Fencing; re-replicate blocks from reachable nodes | < 60s | 0 |
| 7 | Silent disk corruption | Scrub/deep-scrub (daily/weekly) | Corrupted block returned to client | Detect via checksum; repair from replica | 0 (at scrub time) | 0 |
| 8 | Full cluster network outage | All monitoring | Complete outage | Wait for network; data is on disk | Network recovery time | 0 |

### Data Durability and Replication

**HDFS replication:**
- Default replication factor: 3
- Block report every 6 hours (full), incremental on every change
- Under-replication detected within 1 block report cycle (or immediately via incremental)
- Re-replication prioritized by: (1) blocks with 0 replicas (critical), (2) blocks with 1 replica, (3) under-replicated
- Replication throttle: `dfs.namenode.replication.max-streams` (default 2 per DataNode)

**Ceph replication:**
- Default: `size=3`, `min_size=2` (can serve reads with 2/3 replicas)
- Scrubbing: light scrub daily (metadata), deep scrub weekly (data + checksums)
- Recovery: automatic when OSD is marked `out`; throttled by `osd_max_backfills`
- Durability calculation: with 3 replicas, AFR 2%, 30-min repair time, P(data loss) ≈ 10⁻¹¹

---

## 9. Security

| Layer | HDFS | Ceph |
|-------|------|------|
| **Authentication** | Kerberos (SPNEGO for WebHDFS) | CephX (shared-secret authentication) |
| **Authorization** | POSIX permissions + HDFS ACLs + Ranger/Sentry | POSIX permissions (CephFS) + CephX capabilities |
| **Encryption at rest** | HDFS Transparent Encryption (KMS + EDEK per file) | dm-crypt/LUKS per OSD disk; or Ceph-level encryption (Pacific+) |
| **Encryption in transit** | SASL + TLS (data transfer); TLS (WebHDFS) | msgr2 protocol with on-wire encryption (AES-128-GCM) |
| **Network isolation** | Separate HDFS ports from client network | Separate public (client) and cluster (replication) networks |
| **Audit** | HDFS audit log (log4j, every operation) | Ceph audit log (MON/MDS operations) |
| **Multi-tenancy** | HDFS encryption zones per tenant; quotas | Separate pools per tenant; CephX key per tenant |

---

## 10. Incremental Rollout Strategy

| Phase | Scope | Duration | Validation |
|-------|-------|----------|-----------|
| 1 | Single-rack Ceph (3 OSD nodes, 1 MON, 1 MDS) | 2 weeks | Basic I/O, CRUSH placement, scrubbing |
| 2 | Multi-rack Ceph (3 racks, 9 nodes) | 3 weeks | Rack-failure simulation, recovery time |
| 3 | CephFS + CSI driver for Kubernetes | 3 weeks | Pod mount/unmount, dynamic provisioning, failover |
| 4 | RBD for VM images (OpenStack integration) | 3 weeks | Live migration, snapshot, clone |
| 5 | HDFS cluster for big-data workloads | 4 weeks | NameNode HA failover, Federation, Spark jobs |
| 6 | Production migration (10% → 50% → 100%) | 6 weeks | SLA monitoring, performance benchmarks |

**Rollout Q&As:**

**Q1: How do you validate CRUSH placement correctness before production data?**
A: Use `crushtool --test` to simulate placement for all PGs without any real data. Verify that (1) replicas land on different racks, (2) data distribution matches weight ratios within 5%, (3) no OSD is overloaded.

**Q2: How do you benchmark storage performance before migration?**
A: Use `rados bench` (Ceph) or `TestDFSIO` (HDFS) to measure: sequential write/read throughput, random IOPS, metadata ops/sec. Compare against SLA requirements. Run with production-like workload profiles (mixed read/write, varying object sizes).

**Q3: How do you handle the Ceph "recovery storm" when first expanding the cluster?**
A: When adding new OSDs, CRUSH rebalances data to them. Throttle with: `osd_max_backfills=1`, `osd_recovery_max_active=1`, `osd_recovery_sleep=0.1`. Monitor OSD latency during rebalance; pause if p99 exceeds SLA.

**Q4: How do you ensure Kubernetes CSI driver reliability?**
A: Test failure scenarios: (1) OSD failure during pod mount, (2) MDS failure during file I/O, (3) node failure with mounted volume, (4) CSI driver pod crash. Validate that `ReadWriteOnce` fencing works correctly (no split-brain).

**Q5: What's the rollback plan if Ceph MDS has stability issues?**
A: CephFS is independent of RBD. If MDS is unstable, switch CephFS workloads back to NFS (with NFS-Ganesha on old storage) while debugging. RBD workloads are unaffected by MDS issues. Keep the old NFS/GPFS infrastructure operational for 30 days as fallback.

---

## 11. Trade-offs & Decision Log

| # | Decision | Alternatives | Rationale |
|---|----------|-------------|-----------|
| 1 | Ceph for unified storage (block + file + object) over separate systems | HDFS-only, GlusterFS, separate SAN + NAS | Unified management; single team; proven at scale; supports all access patterns |
| 2 | HDFS retained for big-data workloads alongside Ceph | Move everything to Ceph | HDFS has deeper Hadoop/Spark ecosystem integration; NameNode performance for MapReduce metadata access patterns |
| 3 | BlueStore over FileStore | FileStore (XFS), ZFS | Eliminates double-write; better latency; checksumming; Ceph's recommended backend since Luminous |
| 4 | 3× replication for hot data; EC for cold | All EC, all replication | Hot data needs fast reads (single copy); cold data benefits from EC's storage efficiency |
| 5 | CRUSH over consistent hashing | Consistent hashing, static partitioning | Topology awareness; weighted placement; deterministic (no central lookup) |
| 6 | NVMe for WAL/DB, HDD for data | All HDD, All NVMe, tiered SSD+HDD | NVMe handles metadata I/O (RocksDB) and small writes (WAL); HDD handles bulk data at lower cost |
| 7 | HDFS Federation over single massive NameNode | Single NN with 512 GB heap, Apache Ozone | Federation is operationally simpler; no GC risk; Ozone not mature enough |
| 8 | CephX over Kerberos for Ceph | Kerberos, mTLS | CephX is native to Ceph; simpler deployment; sufficient for internal infrastructure |

---

## 12. Agentic AI Integration

| Use Case | Agentic AI Application | Implementation |
|----------|----------------------|----------------|
| **Predictive disk failure** | Agent monitors SMART data, temperature, error rates; predicts disk failure 24-48h in advance | Time-series model (LSTM) on SMART metrics; agent pre-migrates data off predicted-failing disks before failure occurs |
| **Intelligent rebalancing** | Agent schedules rebalancing during low-traffic windows to minimize impact | Traffic pattern model identifies daily/weekly troughs; agent triggers rebalance during off-peak; pauses if latency rises |
| **Capacity forecasting** | Agent predicts cluster growth and auto-provisions bare-metal nodes | Linear regression on daily storage growth; agent generates provisioning tickets when 30-day projection exceeds capacity |
| **Performance anomaly detection** | Agent detects slow OSDs (outliers) and proactively addresses them | Statistical model on per-OSD latency distribution; agent marks slow OSDs as "nearfull" or triggers disk replacement |
| **Workload-aware placement** | Agent adjusts CRUSH weights based on workload heat maps | Monitor per-OSD IOPS; agent reduces CRUSH weight of hot OSDs to shed load; increases for cold OSDs |
| **Automated incident resolution** | Agent handles common failures (OSD down, MDS crash, NameNode GC) | Runbook-as-code: agent detects failure, executes diagnostic commands, takes corrective action (restart, failover, reweight), escalates only if automated fix fails |

**Example: Predictive Disk Failure Agent**

```python
class DiskFailurePredictionAgent:
    """
    Monitors SMART data from all OSDs and predicts failures.
    Proactively migrates data before failure occurs.
    """

    def __init__(self, ceph_client, smart_collector, ml_model, alert_client):
        self.ceph = ceph_client
        self.smart = smart_collector
        self.model = ml_model  # Pre-trained LSTM on historical SMART → failure
        self.alerts = alert_client

    async def evaluate_disks(self):
        """Run every hour."""
        for osd in await self.ceph.list_osds():
            smart_data = await self.smart.get_metrics(osd.device,
                lookback_hours=168)  # 7 days

            # Features: reallocated sectors, pending sectors, temperature,
            #           uncorrectable errors, power-on hours, seek errors
            features = self.extract_features(smart_data)
            failure_prob = self.model.predict_failure_probability(features)

            if failure_prob > 0.8:
                await self.handle_imminent_failure(osd, failure_prob)
            elif failure_prob > 0.5:
                await self.alerts.warn(
                    f"OSD {osd.id} disk {osd.device} failure probability "
                    f"{failure_prob:.0%} — monitoring closely")

    async def handle_imminent_failure(self, osd, probability):
        """Proactively drain data from failing disk."""
        await self.alerts.critical(
            f"OSD {osd.id} predicted to fail within 48h "
            f"(probability={probability:.0%}). Initiating proactive drain.")

        # Mark OSD as "out" to trigger data migration
        await self.ceph.osd_out(osd.id)

        # Set reduced weight to gradually shift data away
        await self.ceph.osd_crush_reweight(osd.id, 0.0)

        # Monitor migration progress
        while True:
            pgs_remaining = await self.ceph.get_osd_pg_count(osd.id)
            if pgs_remaining == 0:
                await self.alerts.info(
                    f"OSD {osd.id} fully drained. Safe to replace disk.")
                break
            await asyncio.sleep(60)

    def extract_features(self, smart_data):
        return {
            'reallocated_sector_count': smart_data.get(5, 0),
            'current_pending_sector': smart_data.get(197, 0),
            'offline_uncorrectable': smart_data.get(198, 0),
            'temperature_celsius': smart_data.get(194, 35),
            'power_on_hours': smart_data.get(9, 0),
            'seek_error_rate_trend': self.compute_trend(smart_data, 7),
            'reallocated_growth_rate': self.compute_growth(smart_data, 5),
        }
```

---

## 13. Complete Interviewer Q&A Bank

**Architecture:**

**Q1: Compare HDFS and Ceph — when would you choose each?**
A: HDFS: when running Hadoop/Spark ecosystem; append-only workloads; deeply integrated with YARN. Ceph: when you need unified storage (block for VMs, file for shared access, object for S3); random I/O; Kubernetes persistent volumes. Many orgs run both: HDFS for big-data, Ceph for everything else.

**Q2: How does Ceph's "no single point of failure" design work?**
A: Every component is replicated: MONs (3-5, Paxos consensus), MDSs (active + standby), OSDs (each PG replicated across OSDs). Clients cache the cluster map and compute placement locally (CRUSH), so even if all MONs are temporarily unreachable, existing I/O continues. No NameNode-equivalent single point.

**Q3: What are the trade-offs of POSIX semantics for a distributed file system?**
A: POSIX requires strong consistency (read-your-writes), atomic rename, locking (flock/fcntl), and coherent client caching. This is expensive in distributed systems: CephFS uses capabilities (caps) — leases that grant clients permission to cache. Revoking caps on conflict (e.g., two clients writing the same file) adds latency. HDFS relaxes POSIX: write-once, no random writes, no flock.

**Q4: How does Ceph handle erasure coding vs replication?**
A: EC pools are configured per-pool (e.g., `erasure-code-profile k=4 m=2`). CRUSH places k+m chunks on separate OSDs/racks. The primary OSD encodes and distributes chunks. Reads require at least k chunks. EC reduces storage overhead (50% for 4+2 vs 200% for 3× replication) but increases read latency (decode overhead) and prohibits random writes (partial stripe updates require read-modify-write).

**Consistency & Correctness:**

**Q5: How does HDFS guarantee no data loss during NameNode failover?**
A: The EditLog (journal) is written to a quorum of JournalNodes (QJM). The Active NN writes every metadata mutation to QJM before ACKing to the client. The Standby NN continuously tails the EditLog and applies changes. On failover, the Standby replays any remaining edits from QJM, ensuring no committed mutations are lost.

**Q6: What happens if a Ceph client has a stale CRUSH map?**
A: The client sends the write to what it believes is the primary OSD. If the CRUSH map is stale, the OSD will reject the request and return the current map version. The client fetches the updated map from a MON and retries. This is transparent to the application.

**Q7: How does CephFS handle concurrent writes to the same file?**
A: CephFS uses capabilities (caps). A client with exclusive write cap can buffer writes. If another client opens the file for writing, the MDS revokes the exclusive cap. Both clients then write in synchronous mode (every write goes to OSD directly). The MDS serializes overlapping writes at the inode level.

**Performance:**

**Q8: How do you optimize HDFS for small files?**
A: Small files (< block size) waste NameNode memory (1 inode per file). Solutions: (1) HAR (Hadoop Archive): pack small files into a single archive file, (2) SequenceFile: key-value container for many small records, (3) CombineFileInputFormat: merge small file splits for MapReduce, (4) Ozone (separate system designed for small objects).

**Q9: What are the performance implications of Ceph's primary-copy replication?**
A: All writes go through the primary OSD, which can be a bottleneck for write-heavy workloads. The primary serializes writes, computes checksums, and forwards to replicas. Mitigation: (1) more PGs distribute primaries across more OSDs, (2) primary affinity can balance primary distribution, (3) EC pools distribute encoding work.

**Q10: How do you tune Ceph for all-NVMe deployments?**
A: (1) Increase PG count (NVMe handles more IOPS), (2) Set `min_alloc_size=4k` (NVMe handles small random I/O), (3) Enable `bluestore_prefer_deferred_size=0` (bypass deferred writes; NVMe doesn't need them), (4) Increase `osd_op_threads`, (5) Use `mclock` I/O scheduler for QoS, (6) Separate BlueFS onto same device (no benefit to separate WAL/DB on all-NVMe).

**Operations:**

**Q11: How do you perform a rolling upgrade of Ceph OSDs?**
A: (1) Upgrade MONs first (backward compatible), (2) Upgrade OSDs one at a time: set `noout` flag, restart OSD with new binary, verify it rejoins, repeat. (3) After all OSDs upgraded, enable new features. The `noout` flag prevents CRUSH from redistributing data while OSDs are temporarily down for restart.

**Q12: How do you handle a "split-brain" in HDFS NameNode HA?**
A: Fencing. Before the Standby NN takes over, it must fence the old Active to prevent split-brain. Fencing methods: (1) SSH fencing: kill the old NN process via SSH, (2) sshfence: more robust variant, (3) shell fencing: run custom script (e.g., STONITH — power off the server). The Standby will not become Active until fencing succeeds.

**Q13: What is PG autoscaling in Ceph and when should you use it?**
A: PG autoscaler (enabled by default since Nautilus) automatically adjusts the number of PGs per pool based on the amount of data and number of OSDs. It targets ~100 PGs per OSD. Use it when you don't want to manually calculate PG count. Disable it for pools where you need precise control (e.g., very latency-sensitive workloads where PG splitting might cause brief stalls).

**Q14: How do you monitor HDFS DataNode health?**
A: (1) NameNode Web UI: shows dead/decommissioning DataNodes, under-replicated blocks. (2) `hdfs dfsadmin -report`: disk usage per DataNode. (3) JMX metrics: heartbeat latency, block report time, I/O errors. (4) DataNode logs: disk errors, pipeline failures. (5) SMART monitoring for proactive disk failure detection.

**Q15: How do you handle Ceph's "slow ops" problem?**
A: Slow ops (operations taking > 30s) indicate OSD performance issues. Diagnosis: (1) Check `ceph daemon osd.X dump_ops_in_flight`, (2) Look for slow disks (high await in iostat), (3) Check for network issues (packet drops), (4) Verify no PG is in recovery (recovery competes with client I/O), (5) Check OSD memory and CPU. Fix: replace slow disk, throttle recovery, increase OSD resources.

**Q16: How does Ceph handle full OSDs?**
A: (1) At `nearfull_ratio` (default 85%): warning, no new data directed to near-full OSDs. (2) At `full_ratio` (default 95%): OSD stops accepting writes; cluster goes read-only for affected PGs. (3) At `backfillfull_ratio` (default 90%): OSD stops accepting backfill/recovery data. Prevention: capacity monitoring, alerts at 70%, automatic rebalancing.

---

## 14. References

| # | Resource | Relevance |
|---|----------|-----------|
| 1 | [Ceph: A Scalable, High-Performance Distributed File System (Weil et al., 2006)](https://ceph.com/assets/pdfs/weil-ceph-osdi06.pdf) | Original Ceph paper |
| 2 | [CRUSH: Controlled, Scalable, Decentralized Placement of Replicated Data](https://ceph.com/assets/pdfs/weil-crush-sc06.pdf) | CRUSH algorithm paper |
| 3 | [HDFS Architecture Guide](https://hadoop.apache.org/docs/stable/hadoop-project-dist/hadoop-hdfs/HdfsDesign.html) | HDFS design |
| 4 | [BlueStore: A New, Faster Storage Backend for Ceph (Aghayev et al., 2019)](https://www.usenix.org/conference/fast19/presentation/aghayev) | BlueStore internals |
| 5 | [HDFS Federation](https://hadoop.apache.org/docs/stable/hadoop-project-dist/hadoop-hdfs/Federation.html) | NameNode scalability |
| 6 | [Ceph Documentation](https://docs.ceph.com/) | Operational reference |
| 7 | [HDFS Observer NameNode](https://hadoop.apache.org/docs/stable/hadoop-project-dist/hadoop-hdfs/ObserverNameNode.html) | Read scaling |
| 8 | [Facebook's Tectonic: A Distributed File System for Exabyte-Scale](https://www.usenix.org/conference/fast21/presentation/pan) | Next-generation DFS at scale |
