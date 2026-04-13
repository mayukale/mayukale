# INTERVIEW GUIDE — Infra-09: Storage Systems

Reading Infra Pattern 9: Storage Systems — 5 problems, 6 shared components

**Problems covered:** Distributed File System (HDFS/Ceph), Distributed Object Storage (S3-like), Elasticsearch at Scale, MySQL at Scale, Time Series Database (Prometheus/Thanos)

**Shared components:** Write-Ahead Log, Replication, Inverted/Secondary Index, Hot/Warm/Cold Tiering, Horizontal Sharding/Partitioning, Connection Pooling

---

## STEP 1 — OVERVIEW

Storage Systems is one of the highest-signal topic areas in infrastructure interviews. Every problem in this cluster asks a fundamentally similar question: given massive scale, how do you store, retrieve, and durably persist data while keeping latency low and cost sane? The five problems differ in their access patterns (file vs. object vs. search vs. relational vs. time-series), but they reuse the same arsenal of techniques.

Here is a quick map of the five problems and what makes each one live:

- **Distributed File System** — POSIX-ish file storage for big-data and VM workloads. The hard part is the metadata bottleneck (HDFS NameNode) and intelligent data placement across failure domains (Ceph CRUSH).
- **Distributed Object Storage** — Immutable blob storage at exabyte scale with 11-nines durability. The hard part is erasure coding math, AZ-aware shard placement, and strong read-after-write consistency in a sharded metadata store.
- **Elasticsearch at Scale** — Full-text search and log analytics for hundreds of TB of time-series logs. The hard part is shard sizing (set at index creation, cannot be undone), ILM-driven hot/warm/cold migration, and sustaining 500K docs/sec ingest without blowing up heap.
- **MySQL at Scale** — Relational OLTP backing multi-tenant cloud infra metadata. The hard part is horizontal sharding with Vitess, semi-sync replication for RPO=0, and connection pool explosion prevention with ProxySQL and HikariCP.
- **Time Series Database** — High-frequency metric ingestion (millions of samples/sec) with PromQL query serving. The hard part is cardinality management, Gorilla compression to squeeze 16-byte samples into 1.4 bytes, and long-term retention via Thanos on object storage.

---

## STEP 2 — MENTAL MODEL

### The Core Idea

Every storage system in this cluster is solving the same fundamental tension: **writes want to be sequential and local, reads want to be random and distributed.** Sequential writes are fast (HDDs, NVMe SSDs love sequential I/O). Random reads across distributed nodes are slow and expensive. Every clever design here is a mechanism to honor that tension — buffer writes sequentially (WAL, head block, translog), then restructure for reads (compaction, indexing, erasure coding, sharding).

### The Real-World Analogy

Think of a major airport baggage system. When bags arrive, they are all thrown on a single moving belt — sequential intake, one direction, no stops (this is your WAL). Then they get sorted into holding areas by flight (hot/warm/cold tiering, sharding). The bags are tagged and entered into a lookup database so the airline can find any bag quickly (secondary index, inverted index). Bags get replicated — one set goes to the plane, one set to a backup hold (replication). When a plane is delayed, you don't want to search the entire airport — you go to that flight's holding area (shard/index routing). The whole system falls apart if baggage handlers can't find the sort database (metadata bottleneck — the HDFS NameNode problem).

### Why It Is Hard

Three things make storage systems genuinely difficult at scale:

1. **The metadata problem.** At tens of billions of objects or files, the directory of "what lives where" becomes its own bottleneck. HDFS's NameNode needs 300 GB of RAM just to hold 500M files in memory. An S3-like system needs MySQL shards to hold metadata for 10 billion objects. The data tier scales horizontally; the metadata tier often does not. Every good design has an explicit answer to "how does metadata scale."

2. **The durability math.** Promising 11 nines of durability sounds easy until you compute it: for 10 billion objects stored for a year, you can lose at most 0.001 objects — that is one object every 1000 years on average. Achieving this requires erasure coding (RS(10,4) tolerating 4 simultaneous node failures), AZ-aware placement, scrubbing, and repair pipelines. One wrong assumption about correlated failure modes and the math collapses.

3. **The write/read amplification tradeoff.** Anything that speeds up reads tends to increase write amplification and vice versa. Erasure coding saves 50% storage over 3x replication but requires collecting 10 shards on read. Elasticsearch's inverted index makes searches sub-second but makes indexing slower and heap-heavier. Force-merging Lucene segments to 1 per shard speeds up warm-tier search but is a CPU-intensive offline operation. Interviewers probe whether you understand these tradeoffs on both sides.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before drawing a single box, ask these questions. Each answer changes the design significantly.

**Q1: What is the access pattern — write-once-read-many, read-heavy random, or write-heavy time-series?**
If write-once-read-many (HDFS/S3), you can optimize heavily for sequential read throughput and use erasure coding. If read-heavy random (MySQL, search), you invest in indexes and replicas. If write-heavy append (TSDB, logs), you prioritize WAL throughput and batch compaction.
*What changes:* write-once favors erasure coding; random read favors B-tree indexes and replica read routing; time-series write-heavy favors WAL + compaction architecture.

**Q2: What are the durability and consistency requirements?**
"11 nines" (S3-level) means RS erasure coding across AZs. "RPO=0" (MySQL) means semi-sync replication with AFTER_SYNC wait point. "No sample loss after ACK" (Prometheus) means WAL durability before any acknowledgment.
*What changes:* durability target directly determines the replication/erasure strategy; consistency target determines whether you need synchronous commit waits.

**Q3: What is the expected scale — object count, data volume, QPS?**
Small-scale answers look very different. 10B objects at 256 KB average requires 2.5 PB raw data and ~5 TB of metadata. 12.5 million active time-series at 15s scrape interval is 833K samples/sec. These numbers determine sharding strategy, metadata store choice, and capacity estimates.
*What changes:* a single well-tuned Postgres can handle 1 TB metadata; at 10 TB you need Vitess sharding; at 100 TB you need different sharding keys.

**Q4: Is the data mutable or immutable? Does it need versioning?**
Immutable blobs (S3 objects) allow erasure coding and aggressive caching. Mutable records (MySQL, Elasticsearch documents with updates) require more careful consistency management. Versioning (S3 bucket versioning) requires keeping all version chains and delete markers.
*What changes:* immutability enables erasure coding and content-addressable storage; mutability requires MVCC, version chains, and more complex cache invalidation.

**Q5: What are the retention and tiering requirements?**
"15 days local, 1 year archival" (Prometheus) demands a Thanos-style remote write tier. "30-day hot, 90-day warm, 1-year cold" (Elasticsearch) demands ILM-driven tier migration. Flat retention (MySQL, no tiering) simplifies the design.
*What changes:* tiering requirements drive the entire tier architecture, hardware BOM (NVMe vs SSD vs HDD), and background lifecycle management pipelines.

**Red flags to watch for in the interview:**
- Jumping to implementation before clarifying access patterns — the single worst habit
- Designing one system when the interviewer wants another (object storage vs. file system look different; don't conflate them)
- Ignoring the metadata tier — treating storage as just "lots of disks"

---

### 3b. Functional Requirements

#### Core Requirements (common across storage systems)

- **Ingest data:** write files/objects/documents/metrics into the system with a durable acknowledgment
- **Retrieve data:** read by key, by range, by query (full-text, PromQL, SQL), or by path
- **Delete data:** remove data with optional soft-delete and lifecycle-driven hard delete
- **List/enumerate data:** list files in a directory, objects by prefix, time-series by label set
- **Survive failures:** recover from node, disk, rack, or AZ failure without data loss

#### Problem-Specific Scope

For **Distributed File System:** add POSIX semantics (rename, directory listing, HDFS-style block operations), snapshot support, and quota management.

For **Object Storage:** add versioning, multipart upload for files >5 GB, presigned URL generation, lifecycle policies (STANDARD → IA → ARCHIVE → delete), and cross-region replication.

For **Elasticsearch:** add full-text search with scoring, structured aggregations (terms, date_histogram, percentile), ILM (hot/warm/cold/delete), ingest pipelines (grok parsing, enrichment), and cross-cluster search.

For **MySQL at Scale:** add ACID transactional semantics, JOIN queries across tables, online DDL (schema change without downtime), read/write splitting, and point-in-time recovery.

For **Time Series Database:** add label-based filtering (PromQL label matchers), recording rules (pre-computed aggregations stored as new series), alerting rule evaluation, downsampling (1m → 5m → 1h), and remote write/read for long-term retention.

**Clear statement of scope:** Design a storage system that handles [X TB/day ingest or X QPS] with [Y nines durability], [Z ms p99 latency], [N days/year retention], and survives single-node and single-rack failures.

---

### 3c. Non-Functional Requirements

Always derive NFRs from the problem constraints, not from thin air. Here is the full picture across all five systems.

**Durability:**
- Object Storage: 11 nines (99.999999999%) — RS(10,4) across 3 AZs, scrubbing, repair pipelines
- MySQL: RPO = 0 — semi-sync replication with AFTER_SYNC wait point; at least 1 replica ACKs before client gets success
- TSDB: no sample loss after WAL ACK — sequential WAL write is the commit point
- DFS: 99.999%+ — 3x replication (HDFS default) or EC 4+2 (Ceph) with rack-awareness
- Elasticsearch: per-shard 1 replica; translog for crash recovery

**Availability:**
- Object Storage read path: 99.99% — load balanced API gateway, multiple data node replicas
- MySQL: 99.99% — Vitess automated failover <30s on primary failure
- Elasticsearch search: 99.9% — coordinating nodes, replica shards serve queries
- TSDB ingestion: 99.9% — WAL write is local; scrape failures are non-fatal per target

**Write Throughput:**
- DFS: 500 GB/s aggregate (200 nodes × 2.5 GB/s each)
- Object Storage: ~17,400 PUTs/sec peak (3x average)
- Elasticsearch: 500K docs/sec (50 KB avg = 25 GB/s raw ingest)
- MySQL: 50K writes/sec across 10 shards = 5K/shard
- TSDB: 2.5M samples/sec peak

**Read Latency p99:**
- DFS metadata ops: < 10 ms; first data byte: < 50 ms
- Object Storage GET (small object): < 200 ms
- Elasticsearch search: < 500 ms (hot tier); < 2s (cold tier)
- MySQL point read by PK: < 2 ms; range read: < 10 ms
- TSDB instant query: < 50 ms; range query 1h: < 1s

**Key trade-offs to surface explicitly:**
- ✅ Erasure coding RS(10,4): 1.4x storage overhead vs ❌ 3x replication: 3x storage overhead
- ✅ Eventual consistency on replicas enables cheap read scaling vs ❌ reading from primary only for strict consistency adds load to the write path
- ✅ Async translog in Elasticsearch gives 500K docs/sec throughput vs ❌ sync translog would cut throughput by 3-5x
- ✅ In-memory NameNode gives sub-ms HDFS metadata ops vs ❌ a DB-backed metadata store would limit to 100K ops/sec at 10x the latency

---

### 3d. Capacity Estimation

**The formula approach:** always start from "how much data arrives per second" and work outward to storage, then back inward to metadata, then to network.

**Anchor numbers across all five systems:**

| System | Write Rate | Storage (Active) | Storage (Total) | Metadata |
|---|---|---|---|---|
| DFS (HDFS) | 500 GB/s aggregate | 9.6 PB usable (3x replication) | 28.8 PB raw | 300 GB RAM (NameNode heap) |
| Object Storage | 4.5 GB/s peak ingest | ~2.5 PB raw data | ~3.5 PB with EC | ~10 TB metadata (MySQL) |
| Elasticsearch | 25 GB/s raw ingest | 69 TB hot tier | 544 TB total | ~10 GB cluster state in master heap |
| MySQL | ~225 MB/s client traffic | 10 TB per shard | 100 TB across 10 shards | etcd topology store (tiny) |
| TSDB | ~20 MB/s samples (encoded) | 1.5 TB local (15 days) | 1.8 TB long-term (Thanos) | ~150 GB label index |

**Step-by-step for TSDB (example to show in interview):**
1. Sources: 25,000 nodes × 500 metrics each = 12.5M active time series
2. Scrape interval: 15s → samples/sec = 12.5M / 15 = 833K samples/sec
3. Raw bytes: 833K × 16 bytes (8B timestamp + 8B float64) = ~13 MB/s raw
4. After Gorilla compression (1.37 bytes/sample): 833K × 1.37 = 1.14 MB/s compressed
5. 15-day local storage: 1.14 MB/s × 86400 × 15 = ~1.5 TB — fits on NVMe
6. WAL (2h uncompressed): 833K × 7200 × 16 = ~96 GB — must fit in fast NVMe
7. Architecture implication: a single Prometheus server can handle this; above ~5M series or if you need HA, add Thanos

**Step-by-step for Object Storage (example):**
1. 10B objects × 256 KB average = 2.5 PB data
2. RS(10,4): 14/10 overhead = 3.5 PB physical
3. Metadata: 10B × 500 bytes/object = 5 TB metadata, doubled for indexes = ~10 TB in MySQL
4. Daily PUTs: 500M/day = 5,800/sec average; 3x peak = 17,400 PUTs/sec
5. Architecture implication: MySQL metadata needs sharding (10 TB > single-instance limit); 41 data nodes for physical storage at 60% utilization

**Time to estimate in an interview:** aim for 5-7 minutes. Do not go beyond 10 minutes or you have stolen time from HLD and deep dives.

---

### 3e. High-Level Design

**The four to six components every storage system shares:**

1. **Front-door / Access Layer:** Client library (HDFS Java API, librados, S3 REST gateway) or API Gateway. Handles auth, rate limiting, request routing, TLS termination. Stateless, horizontally scalable.

2. **Metadata Layer:** Knows "where does this data live?" and "does this data exist?" The most architecturally critical component. HDFS uses in-memory NameNode (fast, single-node bottleneck). Object Storage uses sharded MySQL + Redis cache. Elasticsearch uses Lucene inverted index per shard (distributed). TSDB uses a per-block inverted index for label → series ID → chunk offset.

3. **Data / Storage Layer:** Where bytes actually live. HDFS DataNodes (128 MB blocks on local disks). S3-like data nodes (erasure-coded chunks on local HDDs/NVMe). Elasticsearch data nodes (Lucene segments, per-shard). MySQL primary + replicas (InnoDB B-tree pages). TSDB persistent blocks (immutable chunk + index files on NVMe).

4. **Replication / Redundancy Layer:** Ensures no single failure loses data. HDFS pipeline replication (3x, rack-aware). Object storage RS(10,4) across AZs. Elasticsearch 1 replica shard per primary. MySQL semi-sync binlog replication. TSDB WAL + Thanos object storage upload.

5. **Lifecycle / Background Management:** Handles what happens after data ages. ILM in Elasticsearch (rollover, migrate, force-merge, delete). Lifecycle worker in Object Storage (STANDARD → IA → ARCHIVE → delete). Compactor in TSDB (2h → 8h → 48h blocks; Thanos downsampling). HDFS balancer. Ceph scrubbing and OSD recovery.

6. **Query / Retrieval Engine (optional, problem-specific):** PromQL engine in TSDB. Lucene scatter-gather in Elasticsearch. VTGate SQL router in MySQL. DFS block location lookup via NameNode.

**Data flow to trace in the interview — always trace both the write path and the read path:**

For Object Storage PUT:
Client → Load Balancer → API Gateway (auth, validate) → Placement Service (consistent hash → 14 node set) → Data Nodes (erasure-encode → write 14 chunks) → all ACKs received → Metadata Service (write MySQL, invalidate Redis) → 200 OK to client → Kafka event for async replication/lifecycle.

For Object Storage GET:
Client → Load Balancer → API Gateway (auth) → Metadata Service (Redis cache hit or MySQL) → Placement Service (resolve chunk locations) → Data Nodes (read any 10 of 14 chunks in parallel) → erasure-decode → stream to client.

**Key decisions to call out unprompted on the whiteboard:**

- Why in-memory metadata for HDFS NameNode (not a database): sub-ms lookup; a DB round-trip at 100K ops/sec is a 5ms bottleneck — ten seconds of delay in aggregate
- Why delete-on-write (not set-on-write) for Redis cache invalidation: prevents the race condition where two concurrent writers leave a stale cache entry
- Why RS(10,4) not 3x replication: saves 53% physical storage (1.4x vs 3.0x overhead); at 2.5 PB that is 4 PB saved
- Why Kafka between log producers and Elasticsearch: decouples producers from ES downtime; enables replay; absorbs burst traffic
- Why semi-sync AFTER_SYNC not AFTER_COMMIT for MySQL: AFTER_COMMIT can lose data if primary crashes after commit but before replica ACK; AFTER_SYNC guarantees the replica has the transaction before the engine commits

**Whiteboard order:**
1. Draw client(s) and access layer first
2. Draw metadata layer second (this is where interviewers probe hardest)
3. Draw data/storage layer third
4. Add replication arrows (this is where durability story lives)
5. Add lifecycle/background workers last
6. Label key numbers on the arrows (throughput, latency targets)

---

### 3f. Deep Dive Areas

The three areas interviewers probe most in this cluster:

#### Deep Dive 1: Metadata Bottleneck and Scalability

**The problem:** Every storage system has a "where is the data?" service. At scale, this becomes the system's Achilles heel. HDFS's NameNode is famously limited — 300 bytes per inode means 500M files requires 150 GB just for file metadata, plus 150 GB for block location data = 300 GB heap on a single JVM. Above 300M files, you hit GC pause problems, startup time issues (loading FsImage takes 10-30 minutes), and single-point-of-failure anxiety.

**Solutions and trade-offs:**

For HDFS: **HDFS Federation** splits the namespace across multiple NameNodes (e.g., `/user` → NN1, `/data` → NN2). Each NameNode's heap is bounded. Clients use ViewFS (a client-side mount table) to route paths to the right NameNode. Observer NameNodes can serve read-heavy metadata operations without loading the Active NameNode.

For Object Storage: **Sharded MySQL via Vitess** (shard key = bucket_id hash) with Redis as a write-through cache. The key insight is that all operations for a given object stay within one bucket, which maps to one MySQL shard — no cross-shard transactions needed. Cache invalidation uses delete-on-write (not set-on-write) to avoid stale entry race conditions.

For TSDB: **Per-block inverted index** — the label → series ID → chunk reference mapping is stored as an immutable index file per 2-hour block. There is no global metadata server; each query engine reads directly from the block's index file. This scales to billions of samples because index lookups never touch a central coordinator.

**Unprompted trade-offs to mention:**
- ✅ In-memory NameNode: sub-ms metadata latency — ❌ heap sizing is painful; GC pauses at 256+ GB heaps; requires G1GC tuning
- ✅ MySQL sharded metadata: scales to 10B objects — ❌ requires Vitess resharding as data grows; cross-shard queries become scatter-gather
- ✅ Per-block immutable indexes: horizontally scalable, no coordinator — ❌ queries touching many blocks must read many index files; cold tier queries are slow

#### Deep Dive 2: Durability Without Writing to All Replicas Synchronously

**The problem:** If you require all replicas to ACK before telling the client "done," you are as slow as your slowest replica. If you only require one ACK, you risk data loss when that one node fails. The WAL + semi-synchronous replication pattern threads this needle.

**The pattern (consistent across all five systems):**
1. Append to a local, sequential, durable write-ahead log (WAL, binlog, translog, Ceph journal)
2. Tell the client "done" only after the WAL write is confirmed (fast, local, sequential)
3. Propagate to replicas/additional nodes asynchronously (can lag behind without affecting client latency)
4. On crash: replay the WAL to reconstruct state

**System-specific implementations:**

For MySQL AFTER_SYNC semi-sync: the binlog is flushed to disk and at least one replica ACKs receipt of the binlog entry *before* the InnoDB storage engine commits. This means: even if the primary crashes the instant after the replica ACKs, the replica has the transaction. RPO = 0. The cost is +1-2ms per write for the replica round trip.

For Elasticsearch translog: every index operation writes to the translog (default async, synced every 5 seconds). On node restart, the translog is replayed to recover in-flight operations that Lucene had not yet flushed to disk. The trade-off: `async` translog gives 500K docs/sec; `request` translog (sync on every operation) would cut throughput to ~50K docs/sec.

For Prometheus WAL: every sample is written to the WAL (on NVMe) before being added to the in-memory head block. On crash, WAL replay reconstructs the head block. The WAL is never needed after the head block is flushed to a persistent block (every 2 hours), so old WAL segments are deleted.

**Unprompted trade-offs to mention:**
- ✅ Async translog (ES): 10x throughput — ❌ up to 5 seconds of data loss on node crash
- ✅ Semi-sync AFTER_SYNC (MySQL): RPO=0 — ❌ +1-2ms latency per write; risk of fallback to async if no replica ACKs within 1 second
- ✅ Ceph EC over RADOS: data survives 4 simultaneous failures — ❌ repair requires reading 10 chunks and re-encoding 4; expensive for large failures

#### Deep Dive 3: Write Amplification and Compaction

**The problem:** All the systems in this cluster write data in one format optimized for ingestion (sequential, append-only) and later reorganize it into a format optimized for reads (indexed, merged, compressed). This reorganization process — compaction — consumes I/O, CPU, and can cause latency spikes if not carefully managed.

**TSDB block compaction:** Prometheus writes 2-hour blocks when the head block flushes. The compactor merges adjacent 2-hour blocks into 8-hour blocks, then into 48-hour blocks. This reduces the number of index files a query must touch. The compaction is fully offline (immutable blocks), but it does generate significant I/O (~50 MB/s burst). Thanos adds a second compaction layer: it merges blocks from multiple Prometheus replicas and generates downsampled versions (5-minute resolution after 90 days, 1-hour after 1 year).

**Elasticsearch force-merge:** Lucene accumulates many small segments per shard during active indexing. Each segment has its own inverted index; searching across many segments multiplies work. On transition to the warm tier, ILM triggers a force-merge to 1 segment per shard. This dramatically reduces search latency on warm/cold data but is a long-running, CPU and I/O-intensive operation. Best practice: run force-merge only on read-only indices, during off-peak hours.

**Ceph BlueStore:** replaces the older FileStore (which wrote to XFS on top of a block device). BlueStore manages the block device directly, using RocksDB for object metadata. The key win: no double-write penalty (FileStore wrote to XFS journal, then to data location). BlueStore writes data directly with a single pass. The cost: operational complexity — you are managing raw block devices without a filesystem.

**Unprompted trade-offs:**
- ✅ TSDB compaction reduces query I/O by 10x for historical data — ❌ compaction bursts can cause I/O contention with scrape writes during peak hours
- ✅ Force-merge in ES cuts warm-tier search latency in half — ❌ cannot force-merge while the index is still being written to; must wait for ILM rollover
- ✅ BlueStore eliminates double-write — ❌ debugging raw block device issues is significantly harder than debugging a filesystem

---

### 3g. Failure Scenarios

Interviewers at senior and staff level focus on failure modes, not happy paths. The framing that distinguishes senior answers: state the failure, state the blast radius, state the recovery mechanism, then state the monitoring signal.

**Common failure modes and senior framing:**

**Failure 1 — NameNode GC pause causes DataNode false death detection**
The HDFS NameNode is JVM-based. A full GC pause at 256+ GB heap can last 30+ seconds. DataNodes send heartbeats to the NameNode every 3 seconds; if 10 heartbeats are missed, the NameNode declares the DataNode dead and starts re-replicating its blocks. When the NameNode comes back from GC, it sees re-replication in progress and the cluster is destabilized.
Mitigation: tune G1GC with `-XX:MaxGCPauseMillis=200`; set ZooKeeper session timeout to be longer than the maximum expected GC pause; use Observer NameNodes to offload read-heavy metadata operations; monitor `NameNode_MemHeapUsedMB` and alert at 80%.

**Failure 2 — Semi-sync fallback to async in MySQL**
When MySQL semi-sync replication cannot get a replica ACK within the timeout (default 10s, we set 1s), it falls back to async replication. During the fallback window, writes proceed without replica durability. If the primary crashes during this window, those writes are lost.
Monitoring signal: `Rpl_semi_sync_master_no_tx` counter increments. Alert immediately. Common causes: replica disk I/O spike, network issue, replica OOM. Mitigation: maintain 2+ replicas (only 1 ACK needed, so another replica is available even if one is slow); set `rpl_semi_sync_master_wait_for_slave_count = 1` (not 2) to reduce fallback risk.

**Failure 3 — Elasticsearch split-brain master election**
Elasticsearch uses Raft (since 7.x, previously Zen discovery) for master election. If the cluster is partitioned and two groups each believe they have a quorum, two master nodes can form. This leads to conflicting index mappings and shard routing.
Prevention: always deploy an odd number of master-eligible nodes (3 or 5); set `discovery.seed_hosts` explicitly; set `cluster.initial_master_nodes` for bootstrap only. With 3 master-eligible nodes, a partition requires one side to have 2+ nodes for a quorum — no split brain.

**Failure 4 — Consistent hash ring split-brain in Object Storage**
If the placement service loses consistency (etcd partition, stale cache), two API gateway instances might compute different shard placements for the same object. Object data goes to the wrong node set; metadata points to a different node set. The object becomes unreadable.
Mitigation: the placement ring is rebuilt from etcd on startup and versioned. All ring updates are linearizable through etcd. API gateways validate their ring version before every placement decision.

**Failure 5 — Ceph OSD cascade failure during recovery**
When an OSD fails, Ceph triggers recovery — moving data from the failed OSD to surviving OSDs. If recovery I/O is not throttled, surviving OSDs get saturated and begin timing out, causing the cluster to declare more OSDs dead, triggering more recovery, causing a cascade.
Mitigation: set `osd_recovery_max_active = 1` per OSD (limits concurrent recovery operations); set `osd_recovery_max_backfills = 1`; monitor `ceph health detail` for `recovery` status; set alert at degraded PG count > 5% of total PGs.

**Failure 6 — Elasticsearch repair storm after large failure (correlated with failure 5)**
If multiple hot data nodes fail simultaneously (power outage to a rack), all their primary shards must be re-elected and all their replica shards must be re-indexed from surviving replicas. With 22 hot nodes × ~79 primary shards each, losing 3-4 nodes triggers thousands of shard recoveries concurrently.
Mitigation: set `cluster.routing.allocation.node_concurrent_recoveries = 4` (limit per-node recovery); monitor `_cluster/health` for `yellow` status; set `index.unassigned.node_left.delayed_timeout = 5m` to delay allocation for brief node restarts (avoids unnecessary recovery on rolling restarts).

---

## STEP 4 — COMMON COMPONENTS

These are the six components present in some or all of the five problems. Know all of them cold.

---

### Component 1: Write-Ahead Log (WAL)

**What it is and why every storage system uses it:**
The WAL (also called editlog, translog, binlog, or just "journal" depending on the system) is an append-only file written before any mutation takes effect on the primary data structure. Its only job is to make sure that if the process crashes mid-write, the system can replay the log on restart and reconstruct exactly the state it had at the moment of the crash.

The WAL is the fundamental reason you can promise "no data loss after ACK." When the system says "done," it means "the WAL write completed." The in-memory state (head block in Prometheus, buffer pool in MySQL, in-memory Lucene segments in Elasticsearch) might be lost on crash — but the WAL is not.

**Where each system uses it:**
- HDFS: the **EditLog** written by the Active NameNode, read by the Standby NameNode via JournalNodes (3+ for quorum). Every namespace mutation (create, delete, rename) is appended to the EditLog before taking effect in memory. The JournalNode quorum ensures the EditLog is durable even if one JournalNode dies.
- Ceph: each OSD has a **per-OSD journal** on NVMe (2x NVMe SSDs per node in BlueStore). Write hits the journal first; then the OSD applies it to the HDD data area and RocksDB metadata. BlueStore's journal eliminates the double-write that FileStore suffered.
- Object Storage: **Kafka** functions as the WAL for the event stream. Every successful object write publishes to the `object-events` Kafka topic with `acks=all` and `min.insync.replicas=2`. Lifecycle and replication workers consume this feed — if they fall behind or crash, they replay from their Kafka offset.
- Elasticsearch: the **translog** is written on every indexing operation before the Lucene index is updated. `durability: async` with `sync_interval: 5s` gives up to 5 seconds of potential loss in exchange for 10x throughput. `durability: request` syncs on every operation — use this when you cannot tolerate any data loss.
- MySQL: the **InnoDB redo log** (crash recovery) plus the **binary log** (replication). The `AFTER_SYNC` wait point in semi-sync means the binlog is flushed *and* one replica ACKs before the engine commit — the replication and crash recovery guarantee are unified.
- TSDB: **WAL segments** on NVMe, up to 128 MB each. Every incoming sample is written to the WAL before being added to the in-memory head block. On crash, the WAL is replayed to reconstruct the head block. WAL segments older than the last head block flush are deleted.

**Key configuration:**
- HDFS: `dfs.journalnode.edits.dir` → fast local disk for JournalNode; `dfs.ha.automatic-failover.enabled=true`
- Elasticsearch: `index.translog.durability=async`, `index.translog.sync_interval=5s` for write-heavy; `request` for high-value data
- MySQL: `sync_binlog=1` (flush binlog on every commit); `innodb_flush_log_at_trx_commit=1` (flush redo log on every commit)
- TSDB: WAL segment size 128 MB; checkpoint written every 2 hours when head block flushes

**What happens without it:** on process crash, any in-memory state that had not been flushed to the primary data structure is permanently lost. At 833K samples/sec in TSDB, a crash without WAL could lose 2 hours × 3600 × 833K = ~6 billion samples. At 50K writes/sec in MySQL, a crash without redo log could corrupt the InnoDB tablespace.

---

### Component 2: Replication for Durability

**Why every system replicates:**
A single node can fail. A single disk can fail (HDDs fail at ~1-3% annual rate; at 200 nodes × 12 drives = 2,400 drives, expect 24-72 drive failures per year). Replication ensures that data is never solely on one physical component.

**The three replication models across these systems:**

**Model A — Pipeline replication (HDFS):** the client writes to DataNode1, DN1 forwards to DN2, DN2 forwards to DN3, ACKs propagate back. Rack-aware placement: replica 1 on writer's rack, replica 2 on a different rack, replica 3 on the same rack as replica 2 but different node. This means at most ⌊(replicas + 1) / 2⌋ replicas per rack.

**Model B — Erasure coding (Object Storage, Ceph EC):** instead of storing 3 copies (3x overhead), the system encodes data into k data shards + m parity shards. Any k shards can reconstruct the object. RS(10,4): 10 data + 4 parity = 14 shards total, 1.4x overhead. With 14 shards across 3 AZs (max 4-5 per AZ), a full AZ failure leaves 9-10 shards — still enough to reconstruct. Trade-off: read requires collecting 10 shards (more network hops than 1 replica read); repair after failure is expensive (read 10, re-encode 4).

**Model C — Primary-replica with binlog/translog (MySQL, Elasticsearch):** writes go to a single authoritative primary; changes propagate to one or more replicas via change log. MySQL uses semi-sync (`rpl_semi_sync_master_wait_for_slave_count=1`) for RPO=0. Elasticsearch uses a replica shard that receives every index operation from the primary shard. Both use the replica for read scaling.

**Key configuration:**
- HDFS 3x replication: `dfs.replication=3` (per-file or per-cluster default)
- RS(10,4): 10 data shards + 4 parity shards; max 4-5 per AZ; tolerate 4 failures
- MySQL semi-sync: `rpl_semi_sync_master_wait_point=AFTER_SYNC`, `rpl_semi_sync_master_wait_for_slave_count=1`, `rpl_semi_sync_master_timeout=1000` (1s before fallback to async)
- Elasticsearch: `index.number_of_replicas=1`; replica shards receive all index operations in parallel with primary

**What happens without it:** a single node failure causes permanent data loss for all data stored only on that node.

---

### Component 3: Inverted / Secondary Index

**Why all five systems need it:**
Without an index, finding data requires a full scan — O(N) in the number of records or bytes. At billions of objects or 500M files, that is unusable. Each system builds a specialized index structure optimized for its access pattern.

**How each system implements it:**

- **HDFS NameNode:** in-memory B-tree-like structure (INodeDirectory.children as a sorted array). File path → INode → BlockInfo list → DataNode locations. All held in JVM heap. Lookup is O(path depth) — effectively O(1) for fixed-depth paths.

- **Ceph OSD (RocksDB per OSD):** each OSD runs a RocksDB instance that maps object ID to placement group → data location on the raw block device. RocksDB's LSM-tree structure is optimized for writes (sequential, batched) with acceptable random read performance.

- **Object Storage (MySQL secondary indexes):** `INDEX idx_bucket_key_version (bucket_id, object_key, version_id DESC)` for latest-version retrieval. `INDEX idx_bucket_prefix (bucket_id, object_key(128))` for prefix-based LIST operations. The Redis cache key `obj:{bucket_id}:{sha256(object_key)}` covers point lookups for hot objects.

- **Elasticsearch (Lucene inverted index):** for each field, maps term → posting list → document IDs. A query like `level=ERROR AND service=api-gateway` intersects the posting lists for both terms. Aggregations use **doc values** (column-oriented store, disk-backed) rather than the inverted index. The `stacktrace` field has `index: false` — stored for display but not indexed, saving significant disk and heap.

- **TSDB (per-block inverted index):** maps label_name=label_value → posting list → series IDs → chunk references. A PromQL query like `http_requests_total{method="GET", status="200"}` intersects two posting lists to get the matching series IDs, then loads only the chunk files for those series. Index size is ~10% of data (150 GB for 1.5 TB).

**Key insight to state in the interview:** inverted indexes and doc values serve different purposes in Elasticsearch. The inverted index (per-field, on disk) supports search (WHERE). Doc values (column-oriented, per-field) support aggregations (GROUP BY). A field can have both. The `stacktrace` field has neither doc_values nor index — it is stored raw for display only.

**What happens without it:** full table/shard scans on every query. At 500K docs/sec ingest rate in Elasticsearch, a full-index scan query would take minutes instead of milliseconds and saturate all data node I/O.

---

### Component 4: Hot/Warm/Cold Tiering

**Why 4 of 5 systems use tiering:**
Storage cost differences between NVMe (expensive, fast), SSD (medium), and HDD (cheap, slow) are roughly 10x per GB from NVMe to HDD. For workloads where data is accessed most intensively when fresh and rarely when old, tiering is a straightforward cost optimization. Recent data lives on fast, expensive storage; old data lives on slow, cheap storage.

**How each system implements tiering:**

- **Elasticsearch ILM (hot/warm/cold/delete):** the most complete tiering implementation in this cluster. Hot tier: 22 NVMe nodes, last 30 days, active indexing. Warm tier: 9 SSD nodes, 30-90 days, read-only, force-merged to 1 segment per shard. Cold tier: 22 HDD nodes, 90 days to 1 year, read-only, compressed with `best_compression` codec. ILM policy automates transitions: `rollover` on hot (50 GB or 7 days), `forcemerge` + `allocate` on warm, `freeze` + `allocate` on cold, `delete` at 365 days. Total 544 TB with this policy.

- **TSDB (head → persistent → Thanos on S3):** head block in memory (most recent ~2 hours, ~30 GB compressed). Persistent blocks on local NVMe (last 15 days = 1.5 TB). Thanos Sidecar uploads completed 2-hour blocks to S3 within 5 minutes of flush. Thanos Compactor downsamps: raw resolution for first 90 days, 5-minute resolution from 90 days to 1 year, 1-hour resolution beyond 1 year. Total long-term 1.8 TB at 5-minute resolution.

- **Object Storage lifecycle policies:** storage class field per object (`STANDARD`, `IA`, `ARCHIVE`). Lifecycle rules fire after configurable days: `transition_days=30 to IA`, `transition_days=90 to ARCHIVE`, `expiration_days=365`. A Python/Celery lifecycle worker evaluates rules via a daily scan. Transitions update the `storage_class` field in MySQL and potentially move data to cheaper storage nodes.

- **Distributed File System (Ceph CRUSH storage classes):** CRUSH rules can route new writes to NVMe OSDs (a "ssd" storage class). Older pools use HDD OSDs. Ceph does not auto-migrate data between tiers by age (unlike ILM or Thanos) — you must explicitly move data between pools. HDFS has a similar mechanism via storage policies (`HOT`, `WARM`, `COLD`, `ONE_SSD`, `ALL_SSD`).

**Key configuration:**
- Elasticsearch: node attribute `node.attr.data: hot/warm/cold`; index setting `index.routing.allocation.require.data: hot`; ILM `allocate` action with `require` filters
- Thanos: `--retention.resolution-raw=90d`, `--retention.resolution-5m=1y`; compactor runs continuously in object storage
- Object Storage: `storage_class ENUM` on the objects table; lifecycle worker query: `WHERE storage_class='STANDARD' AND created_at < NOW() - INTERVAL 30 DAY`

**What happens without it:** everything lives on the most expensive storage tier. For 544 TB of Elasticsearch data entirely on NVMe, the hardware cost would be 5-10x higher than the hot+warm+cold split.

---

### Component 5: Horizontal Sharding / Partitioning

**Why all five systems shard:**
No single machine can store petabytes or handle 500K writes/sec. Sharding distributes both storage and write throughput across multiple nodes. The challenge is choosing a shard key that keeps related data together (avoiding cross-shard joins), distributes load evenly (avoiding hot shards), and allows resharding without downtime as the dataset grows.

**How each system shards:**

- **HDFS (block distribution):** blocks (128 MB each) are distributed across 200 DataNodes using rack-aware placement. No explicit shard key — blocks are placed to maximize sequential read throughput and fault tolerance, not to colocate files. The NameNode tracks block → DataNode mapping.

- **Ceph (CRUSH placement groups):** objects are hashed to placement groups (`PG = hash(pool_id, object_name) % num_PGs`), and PGs are mapped to OSD sets via CRUSH. Recommended: ~100 PGs per OSD. CRUSH handles heterogeneous capacity (weights), rack awareness, and minimal data movement on cluster changes.

- **Object Storage (consistent hash ring):** objects are hashed to a position on the ring; 14 data nodes are selected clockwise from that position respecting AZ constraints (max 4-5 per AZ). Consistent hashing means adding a new node moves only ~1/N of objects instead of all objects.

- **Elasticsearch (Lucene shards):** primary shards are set at index creation (cannot be changed without reindex or shrink). Shard count formula: `ceil(daily_ingest_GB × rollover_days / target_shard_size_GB)`. At 2.3 TB/day with 1-day rollover at 40 GB/shard = 58 primary shards. Custom routing by `tenant_id` colocates all documents for one tenant on the same shard — efficient single-tenant queries.

- **MySQL (Vitess hash sharding):** shard key is `tenant_id` (hash vindex). VTGate hashes tenant_id to a keyspace range and routes the query to the corresponding VTTablet. 10 shards × 10 TB each = 100 TB total. Online resharding via VReplication (no downtime, no app changes) — this is Vitess's killer feature.

**Key configuration:**
- Elasticsearch: `index.number_of_shards: 10` at template level; never change after index creation
- Vitess: `vschema.json` defines the hash vindex per table; `vt ctl PlannedReparent` for failover; `vt ctl Reshard` for online resharding
- CRUSH: `ceph osd crush reweight osd.X <new_weight>` to balance capacity; `ceph osd crush rule create-replicated` to define placement policy

**What happens without it:** a single MySQL instance caps at ~10 TB of data and ~20K writes/sec before performance degrades. A single Elasticsearch node cannot hold 544 TB. A single TSDB node cannot scrape 25,000 targets at 15s intervals.

---

### Component 6: Connection Pooling

**Why 3 of 5 systems need it explicitly:**
Every Java/Python service that connects to MySQL, Elasticsearch, or an Object Storage API gateway needs connections. Without pooling, each request spawns a new TCP connection + TLS handshake + auth sequence. At 1,000 pods × 200 reads/sec = 200K requests/sec, that would require 200K new connections/sec. MySQL's connection limit is ~10K. The pooling layer is a critical multiplexing layer that keeps connection counts manageable.

**How each system implements it:**

- **MySQL (HikariCP + ProxySQL):** HikariCP (Java JDBC pool) maintains 10 pre-warmed connections per pod. 1,000 pods × 10 = 10,000 total connections — within MySQL's limit. ProxySQL at the shard level further multiplexes: multiple app connections to ProxySQL share fewer connections to MySQL primary/replicas. ProxySQL also performs read/write splitting — queries with `readOnly=true` go to replicas; writes go to primary. Key HikariCP settings: `maximum-pool-size=10`, `connection-timeout=3000ms`, `idle-timeout=300000ms`, `max-lifetime=1800000ms`.

- **Elasticsearch (Apache HttpAsyncClient via RestClient):** the Java Elasticsearch client pools HTTP connections to coordinating nodes. `setMaxConnTotal=100` (total), `setMaxConnPerRoute=30` (per coordinating node). Keep-alive headers prevent TCP teardown between bulk requests. The BulkProcessor additionally batches individual index operations (1000 docs or 5 MB or 5 seconds, whichever comes first) into single HTTP requests.

- **Object Storage API Gateway:** the gateway pools connections to data nodes and to the metadata service. Kafka producers batch events (batching reduces the number of produce RPCs).

**Key configuration:**
- HikariCP: `maximum-pool-size=10` per pod (not too large — each connection consumes MySQL memory); `leak-detection-threshold=30000ms` to catch leaked connections
- ProxySQL: `max_replication_lag=5` (don't route reads to replicas lagging more than 5 seconds); `hostgroup_id=10` for writes, `20` for reads
- ES RestClient: `setMaxConnTotal` should be at least `num_coordinating_nodes × max_concurrent_bulk_requests`

**What happens without it:** 1,000 pods each opening 200 connections/sec to MySQL = 200K new connections/sec. MySQL caps at 10K concurrent connections. The result is a connection storm: 190K connections rejected, cascading service failures, `Too many connections` errors across every microservice simultaneously.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Distributed File System (HDFS/Ceph)

**What is unique:**
The only problem in this cluster with a POSIX-like file system interface — hierarchical namespaces, directory listing, file rename, and file permissions. The central architectural challenge is the **NameNode bottleneck** in HDFS: the entire namespace lives in memory on a single (or active/standby pair of) JVM with up to 384 GB heap. This limits a single NameNode to ~300M files. The solution is HDFS Federation (multiple independent NameNodes, each owning a namespace slice) with ViewFS for client-side path routing. Ceph takes a completely different approach: the **CRUSH algorithm** computes OSD placement deterministically from first principles (no lookup table), supporting heterogeneous capacities and fault domain hierarchies with O(log N) cost. Ceph also uniquely offers three interfaces from a single cluster: block (RBD for VMs), file (CephFS for POSIX), and object (RGW for S3 compatibility).

**Two-sentence differentiator:** Distributed file systems are the only storage type here where clients need to traverse a path hierarchy and where metadata (directory tree + block locations) must be served with sub-10ms latency despite representing billions of files. The critical design question is always "where does the metadata live and how does it scale" — HDFS answers with a big-heap in-memory NameNode plus Federation, while Ceph answers with a distributed MON cluster for cluster maps and MDS nodes for CephFS namespace with dynamic subtree partitioning.

---

### Distributed Object Storage (S3-like)

**What is unique:**
The only problem in this cluster targeting 11-nines durability and exabyte scale. Objects are **immutable** (updates create new versions) — this is what enables erasure coding with RS(10,4). Immutability also means there is no file locking, no partial write, no rename. The unique architectural challenges are: (1) **erasure coding math** — RS(10,4) requires collecting exactly 10 of 14 shards to reconstruct; placement must ensure no AZ holds more than 4 shards so an AZ loss leaves 10+ surviving; (2) **strong read-after-write consistency** — after a successful PUT, any GET must see the latest version; achieved by synchronous Redis cache invalidation (delete-on-write) before returning 200 OK; (3) **presigned URL** — a time-limited HMAC-SHA256-signed URL that grants access without credentials, generated by the API gateway.

**Two-sentence differentiator:** Distributed object storage is the only system here where the client never talks directly to the storage nodes — everything is brokered through an API gateway that handles placement, erasure coding, and metadata atomically. The single most important design decision is the choice between RS(10,4) erasure coding (1.4x overhead, 10-shard read, tolerate 4 failures) versus 3x replication (3x overhead, 1-shard read, tolerate 2 failures) — and the standard answer is RS(10,4) for all cold/standard storage with replication only for the first 24 hours while data is hot.

---

### Elasticsearch at Scale

**What is unique:**
The only problem in this cluster centered on **full-text search** and **log analytics** rather than raw data storage. The unique challenges are: (1) **shard count is immutable** — set at index creation, cannot be changed (without reindex or shrink); getting it wrong is painful; the formula is `ceil(daily_ingest_GB × rollover_days / 40)` and you must validate it against the heap budget (max 20 shards per GB heap); (2) **hot/warm/cold transitions require data locality changes** — moving an index from hot to warm nodes is a live shard relocation, not just a metadata update; ILM orchestrates this automatically but it consumes significant network I/O; (3) **cardinality explosion** — having a high-cardinality keyword field (like `trace_id` or `user_agent`) in doc_values can exhaust heap on aggregation nodes; mark fields as `keyword` only when needed for aggregations, not for every string field.

**Two-sentence differentiator:** Elasticsearch is the only system here where the write schema (mapping) is as architecturally important as the write throughput — a poorly designed mapping (wrong field types, `dynamic: true`, high-cardinality doc_values fields) can destroy query performance and cause OOM on coordinating nodes even with correct capacity. The central operational tension is shard sizing: too few shards = hot spots and slow recovery; too many shards = heap exhaustion (each shard's segment metadata costs ~10 MB of heap).

---

### MySQL at Scale

**What is unique:**
The only problem in this cluster with **full ACID semantics**, **complex JOINs across tables**, and **structured relational schema**. Every other system in this cluster uses some form of eventual consistency, relaxed consistency, or no cross-document transactions. MySQL's uniqueness is RPO=0 (semi-sync AFTER_SYNC) combined with horizontal scalability through Vitess. The key unique challenges are: (1) **connection explosion** — Java microservices with naive connection management will exhaust MySQL's connection limit; HikariCP + ProxySQL are required, not optional; (2) **N+1 query problem** — JPA's lazy loading causes 1 query per child entity in a list; `@EntityGraph` or explicit JOIN FETCHes are required; (3) **cross-shard scatter-gather** — any query without the shard key (tenant_id) must fan out to all 10 shards; these queries are expensive and should be avoided in hot paths; (4) **online DDL** — adding an index to a 1 TB table without pt-online-schema-change or MySQL 8.0's online DDL algorithm will lock the table for hours.

**Two-sentence differentiator:** MySQL at scale is the only system here that requires maintaining ACID transactional guarantees while distributing writes across 10 shards — Vitess is the enabler, but the design must ensure that transactions never cross shard boundaries (all tables sharing tenant_id as a foreign key must be co-sharded on tenant_id). The hardest operational question is not "how do you scale reads" (ProxySQL + replicas) but "how do you reshard without downtime as the dataset grows" — Vitess VReplication handles this, but the application must be tested to tolerate brief read-only windows during cutover.

---

### Time Series Database (Prometheus/Thanos)

**What is unique:**
The only problem in this cluster optimized for **monotonically increasing timestamps** and **uniform scrape intervals**. These properties enable Gorilla compression (delta-of-delta for timestamps + XOR for values), which achieves 1.37 bytes per sample versus 16 bytes raw — an 11.7x compression ratio. No other storage system in this cluster exploits such domain-specific compression. The unique challenges are: (1) **cardinality management** — each unique combination of metric name + label values creates a new time series; a label like `request_id` or `pod_name` with millions of values causes cardinality explosion that exhausts the in-memory head block; high-cardinality labels must be removed at ingestion; (2) **long-term retention without query regression** — Prometheus's local storage is limited to what fits on NVMe; Thanos extends retention to years by uploading blocks to S3 and serving historical queries from the Store Gateway; (3) **the head block memory budget** — 12.5M active series × 2 hours × 1.37 bytes/sample = ~30 GB compressed in memory; this must fit in the Prometheus server's RAM.

**Two-sentence differentiator:** TSDB is the only system here where compression is not optional but load-bearing — without Gorilla encoding, 833K samples/sec would require 13 MB/s of storage writes and 1.5 TB would balloon to 11.5 TB in 15 days; with it, storage is 11.7x smaller and fits comfortably on a single NVMe. The unique operational failure mode is cardinality explosion: a misbehaving application that generates unique labels (request IDs, user IDs, generated pod names) can create millions of new time series in seconds, exhausting the head block's memory and crashing Prometheus.

---

## STEP 6 — Q&A BANK

---

### Tier 1 — Surface Questions (interviewers ask these to baseline you)

**Q1: What is the difference between a distributed file system and distributed object storage?**

A distributed file system (HDFS, CephFS) provides hierarchical namespace semantics — directories, subdirectories, POSIX-style permissions, rename operations, and random-access file I/O. Clients see a tree of paths. Object storage (S3-like) provides a flat namespace within buckets — you store objects by key and retrieve them by key; there is no true directory concept (prefixes simulate it), no rename, and no random-access writes (objects are immutable). File systems are the right choice for applications that need POSIX APIs (MapReduce, NFS mounts, VM images via block interface). Object storage is the right choice for bulk, immutable data at exabyte scale where cost matters more than POSIX semantics. The key technical difference is that file systems maintain a namespace metadata service (NameNode, MDS) while object storage uses flat key-value metadata (usually sharded MySQL or DynamoDB).

**Q2: Why does Elasticsearch use an inverted index instead of a B-tree like MySQL?**

MySQL's B-tree index is optimized for exact-match lookups and range scans on a single field (e.g., `WHERE tenant_id = 12345`). It stores data in sorted order and supports efficient prefix range scans. An inverted index is optimized for full-text search: it maps each term (word or token) to the list of documents containing it. For a query like "find all logs where message contains 'OutOfMemoryError'," a B-tree cannot help — you would need a full table scan. The inverted index precomputes term → document_id mappings so the query is O(1) on the posting list lookup + O(result set size) for merge. The trade-off is that maintaining an inverted index is expensive on writes (every new document must tokenize all text fields and update all affected posting lists), which is why Elasticsearch uses immutable Lucene segments and merges them in the background rather than updating in place.

**Q3: What is Gorilla compression and why does it matter for a TSDB?**

Gorilla (named after Facebook's in-memory TSDB paper from 2015, used by Prometheus) is a compression scheme that exploits two properties of time-series data. First, timestamps: since metrics are scraped at regular intervals (e.g., every 15 seconds), the delta between successive timestamps is nearly constant. Instead of storing deltas, Gorilla stores the delta-of-delta (DoD) — which is often zero. A zero DoD encodes as a single bit. Second, float values: Prometheus metrics like CPU percentage change slowly. XOR-ing successive float64 values leaves mostly leading and trailing zeros — Gorilla encodes only the meaningful bits. The result is 1.37 bytes per sample versus 16 bytes raw (11.7x compression). At 833K samples/sec this difference means 1.5 TB for 15 days instead of 17.5 TB. Without Gorilla, a single-server Prometheus deployment would need 12x the disk.

**Q4: What does "semi-sync replication with AFTER_SYNC" mean and why is it critical for MySQL durability?**

In standard async MySQL replication, the primary commits a transaction to its storage engine and sends the change to replicas in the background. If the primary crashes before the replica receives the change, that transaction is permanently lost. Semi-sync replication adds a wait: the primary delays its acknowledgment to the client until at least one replica confirms receipt of the binlog event. The `AFTER_SYNC` wait point specifically means the primary waits after flushing the binlog to disk and receiving the replica ACK, but before committing to the InnoDB storage engine. This means that even if the primary crashes the instant after the ACK, the replica has the transaction and can be promoted without data loss. RPO = 0. The cost is roughly +1-2ms per write for the replica round trip. The risk is fallback to async if no replica ACKs within the timeout (1 second in our setup) — monitor `Rpl_semi_sync_master_no_tx` and alert immediately.

**Q5: What is ILM in Elasticsearch and how does it prevent the cluster from running out of disk?**

ILM (Index Lifecycle Management) is Elasticsearch's built-in policy engine that automates the lifecycle of time-based indices. A typical ILM policy has four phases: hot (active indexing, rollover when primary shard hits 40 GB or 1 day), warm (transition to slower nodes, force-merge to 1 segment, read-only), cold (transition to HDD nodes, compress, freeze), delete (remove after 365 days). Without ILM, indices accumulate indefinitely and the cluster runs out of disk. ILM also handles the shrink operation (reducing shard count from 10 to 2 for warm indices to reduce heap overhead) and force-merge (collapsing many small Lucene segments into one per shard to speed up searches on archived data). The key thing interviewers want to hear is that ILM is triggered by conditions (size, age), not a cron job, and that transitions between tiers involve live shard reallocation — which consumes network I/O.

**Q6: How does Thanos extend Prometheus retention beyond local disk?**

Prometheus is designed to be lightweight and local — it stores 15 days of data on NVMe. For longer retention, Thanos adds three components alongside each Prometheus instance. The Thanos Sidecar runs colocated with Prometheus and uploads completed 2-hour blocks to S3-compatible object storage within 5 minutes of each flush. The Thanos Store Gateway reads from S3 and serves historical data via the Thanos StoreAPI (gRPC). The Thanos Query component federates queries: when you ask for a 30-day range, it fans out to local Prometheus (last 15 days) and the Store Gateway (older data), merges the results, and deduplicates overlapping blocks from HA Prometheus pairs. The Thanos Compactor runs offline in object storage, merging and downsampling blocks (raw → 5-minute resolution after 90 days → 1-hour resolution after 1 year) to reduce query cost for historical data. Total long-term storage at 5-minute resolution: 1.8 TB for 1 year versus 26 TB for raw 1-year retention.

**Q7: What is a placement group (PG) in Ceph and why does the indirection layer matter?**

Without PGs, Ceph would need to run CRUSH (the placement algorithm) for every individual RADOS object. With 1 billion objects, that is 1 billion CRUSH computations on every cluster map change (OSD added, removed, or reweighted). PGs solve this by adding an intermediate grouping: objects are hashed to PGs (`PG = hash(object_name) mod num_PGs`), and CRUSH maps PGs to OSD sets. With ~100 PGs per OSD and 200 OSDs, there are ~20,000 PGs total. When a cluster change occurs, CRUSH recomputes placement for 20,000 PGs — not 1 billion objects. The recommended ratio is 100 PGs per OSD. Too few PGs means uneven data distribution (some OSDs overloaded). Too many PGs means overhead — each PG requires memory and thread management on every OSD.

---

### Tier 2 — Deep Dive Questions (interviewers ask these to probe system understanding)

**Q1: How does RS(10,4) erasure coding guarantee 11-nines durability, and what are its operational costs?**

RS(10,4) splits each object into 10 data shards + 4 parity shards = 14 total, placed across 3 AZs with a maximum of 4-5 shards per AZ. The system can reconstruct the full object from any 10 of the 14 shards — so it tolerates any 4 simultaneous failures. With 14 shards spread so that no AZ holds more than 4, losing an entire AZ (worst case 5 shards) still leaves 9 shards — the minimum needed is 10, so placement must ensure max 4 per AZ to handle a full AZ outage. The math for 11 nines: assume individual disk annual failure rate of 0.5% and independent failures. The probability that 5 or more of 14 disks fail simultaneously in a 10-minute window approaches the right order of magnitude for 11-nines. Operational costs: (1) read amplification — you must collect 10 shard responses to decode; hedged reads (read all 14, use first 10) reduce tail latency at 40% extra I/O; (2) repair cost — when a node fails, the repair worker must read 10 surviving shards, run Galois field GF(2^8) arithmetic to re-encode 4 missing shards, and write them to a new node; for a 1 TB node, that is 1 TB of read I/O plus ~400 GB of write I/O per repair event; (3) encoding bug risk — Cauchy matrices over GF(2^8) are used precisely because all k×k submatrices are guaranteed non-singular (any k shards can decode), unlike Vandermonde matrices which lack this guarantee.

**Q2: Why does the HDFS NameNode use in-memory metadata instead of a database, and what are the limits?**

The NameNode processes 100K+ metadata operations per second (file opens, block allocations, directory listings, renames). A database round trip is 1-5ms; at 100K ops/sec that would require 100K concurrent database connections and introduce 1-5ms per-operation latency — 10-50x the 0.1ms latency achievable in-memory. The in-memory model achieves sub-millisecond operations by keeping the full namespace (INode tree + BlockInfo list) in JVM heap. The limits: (1) memory — 300 bytes per INode × 500M files = 150 GB for file metadata alone; plus 150 bytes per block × 1B blocks = 150 GB for block locations = 300 GB minimum heap, requiring 384 GB physical RAM; (2) GC pauses — at 256+ GB heap, even G1GC can pause for 500ms-2s during major GC; tune with `-XX:MaxGCPauseMillis=200`, `-XX:G1HeapRegionSize=32m`, and alarm on ZooKeeper timeout if GC pause exceeds ZK session timeout; (3) startup time — loading the FsImage checkpoint + replaying EditLog at startup takes 10-30 minutes for a large cluster; this is why NameNode HA failover (not restart) is the normal recovery mechanism; (4) scale ceiling — a single NameNode maxes out at ~300M files with 256 GB heap; beyond that, HDFS Federation is required.

**Q3: How does Vitess online resharding work without downtime?**

Vitess uses VReplication, a CDC-like (change data capture) mechanism. The process: (1) Create new target shards with the new shard boundaries. (2) Start VReplication streams: for each source shard, a VReplication stream captures binlog events and applies them to the appropriate target shard. (3) Initial copy phase: simultaneously, a bulk copy of existing rows from source to target shards runs in the background. (4) Lag catches up: VReplication monitors replication lag between source and target. When lag drops below a few seconds, prepare for cutover. (5) Cutover: briefly pause writes to the source shards (a few seconds), wait for VReplication lag to reach zero, reroute VTGate to the new shards, then release writes. The application sees a brief read-only window (a few seconds) with no data loss and no data inconsistency. The key insight is that VTGate hides the topology change — applications connect to VTGate with a stable MySQL-compatible endpoint and never need to know which physical shard they are talking to.

**Q4: Explain the split-query/fetch phase in Elasticsearch searches and why it matters for performance.**

Elasticsearch search executes in two phases. Phase 1 (query phase): the coordinating node sends the query to all relevant shards (all primaries or replicas for the targeted indices). Each shard executes the query locally, scores matching documents, and returns the top-N document IDs and their scores — not the full documents. The coordinating node merges these lists, re-ranks globally, and identifies the final top-N document IDs. Phase 2 (fetch phase): the coordinating node fetches only the full documents for the final top-N results from the shards that hold them. Why it matters: if you search 100 shards and need top 10 results, phase 1 transfers 100 × 10 = 1,000 document IDs (tiny) instead of 1,000 full documents. Phase 2 transfers only 10 full documents. Without this split, deep pagination (`from=10000, size=10`) would require every shard to transfer 10,010 full documents to the coordinator — this is why `from+size > 10,000` is rejected by default. For deep pagination, use `search_after` (keyset pagination) with the last document's sort values as the cursor.

**Q5: What happens to Prometheus data during a PromQL range query that spans local and remote (Thanos) data?**

The query goes to Thanos Query, which fans it out to two sources: the local Prometheus instance (for data within the local 15-day retention window via its HTTP API) and one or more Thanos Store Gateways (for data in S3 beyond 15 days). The Store Gateway does not load full block data into memory — it fetches only the block index from S3 (which lists label → posting list → chunk file offsets), performs the label matcher intersection in the index, and then streams only the matching chunk files from S3 for the requested time range. Chunks are streamed as gRPC stream responses. Thanos Query merges the two result streams, applies PromQL functions (rate, sum, etc.), and deduplicates any overlapping blocks (Prometheus HA pairs write the same data; Thanos deduplicates by the `replica` label). At the downsampled tier (5-minute or 1-hour resolution), each data point represents a pre-aggregated window, which drastically reduces the number of samples to transfer for long historical queries.

**Q6: How do you prevent connection pool exhaustion in a microservices environment with HikariCP and MySQL?**

Connection pool exhaustion happens when the sum of `max_pool_size × num_pods` exceeds MySQL's `max_connections`. With 1,000 pods × 10 connections/pool = 10,000 connections, you are at MySQL's practical limit (~10-16K connections depending on RAM). The layered defense: (1) **HikariCP sizing** — keep `maximum-pool-size` small (10) and set `connection-timeout=3000ms`; if the pool is exhausted, HikariCP throws a `SQLTimeoutException` immediately (fails fast) rather than queuing requests indefinitely; (2) **ProxySQL connection multiplexing** — ProxySQL maintains connection pools to MySQL primaries/replicas and multiplexes thousands of app connections into fewer MySQL connections; ProxySQL can multiplex 10K app connections into 500 MySQL connections using connection reuse between queries; (3) **`max_replication_lag` guard** — ProxySQL monitors replica lag and stops routing reads to replicas lagging more than 5 seconds, preventing stale reads during lag spikes; (4) **leak detection** — `leak-detection-threshold=30000ms` in HikariCP logs a warning if a connection is held for more than 30 seconds, identifying slow queries or code bugs that hold transactions open unnecessarily.

**Q7: How does Ceph's CRUSH algorithm differ from consistent hashing, and when would you choose one over the other?**

Consistent hashing (used in Object Storage placement ring): maps objects to a ring; nodes occupy positions on the ring; an object maps to the first N nodes clockwise from its hash position. Adding a node moves only ~1/N of data. However, standard consistent hashing has no concept of fault domain hierarchy — two replicas might land on nodes in the same rack if virtual node positions happen to cluster there. It also requires a lookup table (the ring) to be distributed to all clients.

CRUSH (Ceph): a deterministic algorithm that traverses a hierarchy tree (root → region → rack → host → OSD), using a weighted random selection (straw2) at each level. It natively enforces fault domain separation (one replica per rack, one per host), handles heterogeneous capacities (larger disks get higher weights), and requires no lookup table — clients compute placement from the CRUSH map. Adding a node causes minimal data movement (~1/N). The cost: O(log N) computation per placement vs O(log N) ring lookup.

Choose consistent hashing when you have a homogeneous cluster and the client-side computation logic is simple. Choose CRUSH when you need to enforce physical topology constraints (rack/AZ awareness), handle heterogeneous capacities, or need deterministic placement that any client can compute independently without a central service.

---

### Tier 3 — Staff+ Stress Tests (reason aloud, show system-level thinking)

**Q1: You are the on-call engineer. Your 61-node Elasticsearch cluster is at 95% heap usage on hot data nodes and new indexing is being rejected with "circuit breaker tripped" errors. It is 2 AM. Walk through your investigation and remediation.**

Start by quantifying the blast radius: check `_cluster/health` for `status: red/yellow` and count unassigned shards. If red, search is degraded — prioritize. Check `_cat/nodes?v` for heap usage per node and identify which nodes are critical. Check `_cat/shards?v` to see if hot nodes have more shards than expected.

Root causes, in order of likelihood: (1) shard count explosion — an ILM policy failure might have created many small indices that were never rolled over; check `_cat/indices?v&h=index,docs.count,store.size,pri` sorted by index count; (2) mapping explosion — a misbehaving log shipper using `dynamic: true` mapping might have added hundreds of new fields; each new field consumes heap for ordinals; check `_cat/fielddata?v` for field data usage; (3) aggregation heap pressure — a heavy aggregation query is holding large amounts of doc values in memory; check `_nodes/stats/jvm,indices/fielddata` for field data evictions.

Immediate remediation: (1) clear fielddata cache: `POST /_cache/clear?fielddata=true`; (2) if shard count explosion, manually trigger ILM rollover on overloaded indices: `POST /<alias>/_rollover`; (3) increase coordinating node timeout to prevent retry storms adding more pressure; (4) if all else fails, set `index.blocks.read_only_allow_delete: true` on the oldest cold indices to prevent further writes while you diagnose.

Root cause fix: adjust ILM policy rollover conditions (max size 40 GB, not max age 7 days — age-based rollover can leave shards far below target size when ingest rate drops); add `dynamic: false` or `dynamic: strict` to mappings to prevent field explosion; add a heap utilization alert at 75%.

**Q2: Your Object Storage system is serving 11-nines durability SLA. A datacenter fire destroys one entire AZ containing 35% of your nodes. Walk through what happens automatically, what you must do manually, and what your durability math looks like during recovery.**

What happens automatically: the Placement Service detects node heartbeat failures within 30 seconds. For each affected object, it determines how many chunks are lost: with RS(10,4) and a max of 4-5 shards per AZ, losing an AZ with 35% of nodes means losing ~5 shards per object on average. Objects with 9 or more surviving shards (14-5 = 9) are still readable (need 10 to decode — objects with exactly 9 surviving shards are temporarily unreadable). The Placement Service enqueues repair jobs ordered by priority: objects with fewer than 12 surviving shards get highest priority (approaching the failure threshold).

During the recovery window: durability is reduced. An object that had 14 shards and now has 9 surviving shards can tolerate zero additional failures. Before you finish repair, you are temporarily running with degraded durability. This is expected and survivable for hours, not days. Calculate repair time: AZ had 35% of nodes = ~14 nodes × 12 TB = ~168 TB to repair. Repair rate throttled to 500 MB/s per node to avoid saturating surviving nodes. Time to repair: 168 TB / (26 surviving nodes × 500 MB/s) = ~3.5 hours. During those 3.5 hours, durability is reduced but data is readable.

Manual actions: (1) immediately disable writes to the failed AZ nodes in the placement service to prevent new objects from being placed there; (2) declare a severity-1 incident and track the number of objects below 12 surviving shards on a live dashboard; (3) set repair priority queue to process objects with fewest surviving shards first; (4) verify that new objects being written during recovery are placed with AZ-awareness — no new shards should go to the failed AZ even for partial nodes that might still be accessible.

**Q3: Your MySQL cluster is running 10 shards. One shard is receiving 80% of all write traffic because a single large customer's tenant_id hashes to that shard. P99 write latency on that shard is 200ms; all others are < 10ms. Semi-sync replication timeout is 1 second. What has happened, what are the risks right now, and what are your short-term and long-term solutions?**

What has happened: Vitess hash sharding distributes tenants across shards using a hash of tenant_id. Hash functions do not guarantee uniform distribution — a single high-volume tenant or a cluster of tenants whose hashed tenant_ids fall in the same keyspace range can overwhelm one shard. This is a "hot shard" — a fundamental sharding problem.

Risks right now: the overloaded shard's primary is at high load. Its replica(s) are likely lagging. If replica lag exceeds 1 second, semi-sync times out and falls back to async — RPO is no longer 0 on this shard. Check `Rpl_semi_sync_master_no_tx` immediately. If write latency is 200ms p99 and transactions are holding locks, long-running transactions are accumulating, potentially causing deadlocks.

Short-term remediation: (1) route the large tenant's read traffic exclusively to replicas via a `readOnly` connection pool to remove read pressure from the primary; (2) if available, add a third replica to the overloaded shard to absorb more read traffic; (3) consider throttling the large tenant at the API gateway level to buy time; (4) check for and kill any long-running transactions that are holding locks.

Long-term solutions: (1) Vitess shard split — use `vt ctl Reshard` to split the overloaded shard into two; VReplication handles the live migration; no app downtime; (2) dedicated shard for large tenant — create a dedicated keyspace for the high-volume tenant, route their traffic there; (3) application-level sharding for the largest tenant — if one tenant is >10% of total load, they should be on dedicated infrastructure; (4) review the sharding key — if tenant_id cardinality is low and load is highly skewed, consider a composite shard key or a lookup-based sharding scheme instead of hash-based.

---

## STEP 7 — MNEMONICS

### Mnemonic 1: "WARS-CT" — The Six Shared Components

**W**rite-Ahead Log (WAL, translog, binlog, editlog — all the same idea)
**A**synchronous-to-synchronous replication (WAL + semi-sync threads this needle)
**R**eplica reads for scale (read replicas in MySQL, replica shards in ES, Thanos Store Gateway)
**S**harding for write scale (Vitess for MySQL, Lucene shards for ES, CRUSH for Ceph, consistent hash for object storage)
**C**onnection pooling (HikariCP + ProxySQL — without these, you get a connection storm)
**T**iering for cost (hot/warm/cold in ES, head/block/Thanos in TSDB, STANDARD/IA/ARCHIVE in object storage)

Remember WARS-CT by thinking: "WAR is SCARY without CT scans" (a forced mnemonic, but it sticks).

### Mnemonic 2: "MPD" — The Three Deep Dive Areas That Always Come Up

**M**etadata bottleneck — "where does the metadata live and how does it scale?"
**P**ersistence without synchronous commit to all nodes — "WAL + semi-sync"
**D**ata movement / compaction / amplification — "what happens in the background?"

Every deep-dive question in this cluster is a variation on one of M, P, or D.

### Opening One-Liner

When the interviewer says "design a distributed storage system," begin with:

> "Before I start drawing boxes, I need to understand the access pattern — write-once-read-many like an object store, random reads and writes like a relational DB, or high-frequency append-only like a time-series DB — because each one has a completely different metadata architecture and durability strategy."

This one question immediately signals that you have pattern-matched across the five problems in this cluster and are not going to draw the same generic "load balancer → servers → database" box for every storage problem.

---

## STEP 8 — CRITIQUE

### Well-Covered Areas

The source material is exceptionally thorough in several areas:

- **Erasure coding mathematics:** RS(10,4) Galois field GF(2^8) arithmetic, Cauchy matrix properties, and the AZ placement constraint (max 4 shards per AZ) are explained with correctness and depth. The trade-off against RS(6,3) is explicit.
- **MySQL semi-sync replication:** the AFTER_SYNC vs AFTER_COMMIT distinction is correctly explained with concrete failure scenarios. The GTID-based failover logic in Vitess is accurate.
- **HDFS NameNode internals:** JVM heap sizing, GC tuning (G1GC parameters), Federation with ViewFS, Observer NameNode for read scaling — all covered at a level that would satisfy a staff interview.
- **Elasticsearch shard sizing formula:** the `ceil(daily_GB × rollover_days / 40)` formula with the heap validation step (max 20 shards per GB heap) is the exact heuristic production ES operators use.
- **Gorilla compression:** the delta-of-delta timestamp encoding and XOR float encoding are correctly described with the empirical 1.37 bytes/sample figure.

### Missing or Shallow Coverage

**1. Cross-AZ network cost and latency impact on erasure coding reads.**
The source material notes that RS(10,4) read requires collecting 10 shards but does not deeply address what happens when those 10 shards are spread across 3 AZs with 20-40ms inter-AZ latency. The hedged-read strategy (read all 14, use first 10) is mentioned but the latency model is incomplete. Senior interviewers will probe: "Your RS(10,4) object has shards in us-east-1a, 1b, and 1c. What is your p99 GET latency budget, and how does the shard placement strategy affect it?"

**2. MySQL GTID replication gaps and recovery procedures.**
The source covers GTID for failover but does not address what to do when a replica has a "GTID hole" — a missing transaction that prevents it from catching up. This requires manually restoring the replica from a backup and replaying from a specific GTID position. This is a real operational failure mode that senior MySQL engineers encounter.

**3. Elasticsearch mapping explosion and dynamic mapping.**
The source mentions `dynamic: strict` on the index template but does not explain what happens when a misbehaving log shipper sends a JSON document with 500 new field names. Each new field triggers a mapping update (cluster state update propagated to all nodes), which can cause a "mapping explosion" that: (a) fills the master node's heap with cluster state, (b) slows cluster state distribution, (c) causes coordinating node OOM on aggregations. This is a common production incident that interviewers at companies running large ES clusters will probe.

**4. Prometheus cardinality explosion recovery procedures.**
The source notes cardinality as a risk but does not explain how to recover once a cardinality explosion has occurred. You cannot delete individual time series from Prometheus while it is running without using the admin API (`POST /api/v1/admin/tsdb/delete_series`). The correct procedure: identify the high-cardinality labels using `tsdb analyze`; fix the instrumentation at the source; optionally use a Prometheus relabeling rule to drop or replace the offending label; then use `POST /api/v1/admin/tsdb/clean_tombstones` after deleting series to reclaim disk space.

**5. Object storage consistency edge cases with versioning.**
The source covers read-after-write consistency for the latest version but does not address what happens when bucket versioning is enabled and two clients simultaneously write to the same key. S3 semantics guarantee that the last write wins (one version_id is created per write), but the "last write" is determined by the metadata service commit order in MySQL, not by client wall-clock time. If two clients both GET the same key concurrently after two conflicting PUTs, they may see different version_ids transiently until cache invalidation propagates.

### Senior Probes to Be Ready For

These are questions that a staff-level or principal-level interviewer would add after your HLD:

- "You chose RS(10,4) for standard storage. What do you do for objects under 1 KB where the 14 shard headers cost more than the data itself?" (Answer: replicate small objects 3x instead of erasure-coding them; use a threshold, e.g., objects under 128 KB use 3x replication, above use RS(10,4).)

- "Your MySQL Vitess cluster is hitting the resharding limit — even after splitting to 20 shards, one customer is still generating 40% of write load. What is your architectural escalation path?" (Answer: dedicated MySQL cluster for the customer with a separate Vitess keyspace; application-level routing by tenant_id to different database backends.)

- "Explain what a Ceph PG peering event is and when it blocks I/O." (Answer: when a PG's acting set changes — due to OSD failure or addition — the PGs in the affected acting set must "peer": exchange PG logs, agree on the authoritative set of objects, and resolve any divergent state. During peering, that PG is unavailable for I/O. With 20,000 PGs and proper configuration, peering is brief (seconds) and limited to the affected PGs.)

- "Your TSDB has 12.5M active series. A developer pushes a metrics change that adds `request_id` as a label, creating 10M new time series in 5 minutes. What happens and how do you detect it before Prometheus crashes?" (Answer: the head block's memory grows explosively — each new series allocates a chunk buffer. Monitor `prometheus_tsdb_head_series` and alert if it grows more than 10% per minute. The fix: add a Prometheus relabeling rule to drop the `request_id` label before ingestion: `action: labeldrop, regex: request_id`.)

- "For your HDFS cluster with 200 DataNodes, a RAID failure corrupts 3 DataNodes simultaneously. Walk through the HDFS recovery sequence." (Answer: DataNodes that cannot serve blocks send error reports to the NameNode on the next heartbeat. NameNode marks those blocks as under-replicated. The block replication monitor schedules re-replication for each under-replicated block using the source pipeline from surviving replicas. Prioritize blocks with 0 or 1 surviving copies. Monitor via `hdfs fsck / -list-corruptfileblocks`.)

### Traps to Avoid

- **Trap 1: Saying "just use OFFSET/LIMIT" for paginating MySQL or Elasticsearch.** `OFFSET 100000 LIMIT 10` requires MySQL to read and discard 100,000 rows. Use keyset pagination (`WHERE created_at < cursor ORDER BY created_at DESC LIMIT 10`). In Elasticsearch, `from + size > 10,000` is rejected by default — use `search_after`.

- **Trap 2: Setting Elasticsearch `number_of_replicas=0` in production.** Zero replicas means a single node failure takes that shard offline. Always 1 replica minimum for production. Zero replicas is acceptable only during initial bulk load with an explicit plan to set replicas back to 1 before the index is searchable.

- **Trap 3: Mixing up `AFTER_SYNC` and `AFTER_COMMIT` in MySQL semi-sync.** Many candidates say "semi-sync replication" without knowing which wait point. AFTER_COMMIT is the pre-5.7.2 behavior; AFTER_SYNC is correct for RPO=0. Interviewers who know MySQL will catch this immediately.

- **Trap 4: Treating Prometheus as horizontally scalable out of the box.** Prometheus is a single-server design. It does not natively federate for ingestion. Horizontal scaling requires either functional sharding (different Prometheus instances scrape different target sets) or Thanos/Cortex. Saying "just add more Prometheus servers to a load balancer" is incorrect.

- **Trap 5: Ignoring the metadata tier when asked about "scaling object storage."** The most common scaling bottleneck is not the data tier (add data nodes) but the metadata tier (MySQL shards). At 10B objects, metadata is 5 TB in MySQL, requiring Vitess sharding. Missing this shows a shallow understanding of the architecture.

- **Trap 6: Conflating "sharding" and "partitioning" in the Elasticsearch context.** In MySQL/Vitess, "shard" means a separate MySQL instance with its own data. In Elasticsearch, "shard" means a Lucene index living on a data node. They are different concepts. An Elasticsearch "index" is roughly analogous to a MySQL "table"; an Elasticsearch "shard" is analogous to a MySQL "shard." Do not confuse the interviewer by mixing these vocabularies.

---

*End of INTERVIEW_GUIDE.md — Infra-09: Storage Systems*
