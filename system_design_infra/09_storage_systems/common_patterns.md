# Common Patterns — Storage Systems (09_storage_systems)

## Common Components

### Write-Ahead Log (WAL) for Crash Recovery
- All 5 systems use WAL or an equivalent durable append-only log to guarantee no data loss on crash
- distributed_file_system: HDFS JournalNodes (EditLog); Ceph per-OSD journal; replay on restart
- distributed_object_storage: Kafka as durable event log; object writes acknowledged only after Kafka commit
- elasticsearch_at_scale: translog (`index.translog.durability: async`, `sync_interval: 5s`); replay on shard recovery
- mysql_at_scale: InnoDB redo log + binlog (GTID-based); `AFTER_SYNC` semi-sync wait point ensures binlog flushed before ACK
- time_series_database: WAL on disk; 2-hour head block in memory; WAL replayed on crash; ~96 GB uncompressed for 2-hour window

### Replication for Durability (3× or Equivalent)
- All 5 systems replicate data across multiple nodes/AZs to survive failures
- distributed_file_system: HDFS 3× block replication (28.8 PB raw → 9.6 PB usable) OR Ceph EC 4+2 (28.8 PB → 19.2 PB usable)
- distributed_object_storage: RS(10,4) — 14 shards across 3 AZs, max 4–5 per AZ; 1.4× overhead vs 3× replication overhead
- elasticsearch_at_scale: 1 replica per shard; total shards = primary × 2
- mysql_at_scale: semi-synchronous replication — 1 replica must ACK before primary commits; `rpl_semi_sync_master_wait_for_slave_count = 1`; RPO = 0
- time_series_database: WAL persistence + Thanos uploads completed 2-hour blocks to S3; no sample loss after WAL ACK

### Inverted / Secondary Index for Fast Lookups
- All 5 systems maintain secondary indexes to avoid full data scans
- distributed_file_system: HDFS in-memory namespace tree (B-tree INode); Ceph RocksDB per OSD for object metadata; omap (RocksDB-backed)
- distributed_object_storage: MySQL `INDEX idx_bucket_key_version (bucket_id, object_key, version_id DESC)` and `INDEX idx_bucket_prefix (bucket_id, object_key(128))`
- elasticsearch_at_scale: Lucene inverted index per shard; term → posting list → document IDs; supports full-text + keyword queries
- mysql_at_scale: InnoDB clustered primary key + secondary indexes `KEY idx_tenant_status (tenant_id, status)`, `KEY idx_created (created_at)`
- time_series_database: label pair → posting list → series IDs → chunk references; `~10%` of data size = ~150 GB for 1.5 TB dataset

### Hot / Warm / Cold Tiering for Long-Term Retention
- 4 of 5 systems implement explicit tiering to control cost vs. query latency
- elasticsearch_at_scale: hot (22 NVMe nodes, 30-day), warm (9 SSD, 90-day, force-merged), cold (22 HDD, 1-year, compressed); ILM-automated rollover and migration; total 544 TB
- time_series_database: head block (in-memory, 2 h) → persistent blocks (local SSD) → Thanos object storage; 5-min downsampled at 90 days; 1-hr downsampled at 1 year; 1.5 TB local, 1.8 TB long-term
- distributed_object_storage: lifecycle policies: STANDARD → IA → ARCHIVE → delete; storage_class ENUM per object
- distributed_file_system: Ceph CRUSH storage class rules route new writes to NVMe; older data migrates to HDD pools

### Sharding / Horizontal Partitioning for Write Scale
- All 5 systems shard data horizontally for write throughput beyond single-node capacity
- distributed_object_storage: consistent hash placement maps object → 14 node set; AZ-aware: max 4–5 shards per AZ
- elasticsearch_at_scale: `number_of_shards: 10` per index; shard sizing: `ceil(daily_ingest_GB × rollover_days / 40 GB target)` = 58 primary shards for 2.3 TB/day
- mysql_at_scale: Vitess horizontal sharding; VTGate routes SQL by shard key; 10 shards × 10 TB = 100 TB total; 50 K writes/sec across 10 shards = 5 K/shard
- distributed_file_system: HDFS blocks (128 MB) distributed across 200 DataNodes; Ceph PGs (placement groups) distribute RADOS objects across OSDs

### Connection Pooling to Prevent Connection Explosion
- 3 of 5 systems explicitly implement connection pooling at application or proxy layer
- mysql_at_scale: HikariCP (10 connections per pod); 1,000 pods × 10 = 10 K total connections (manageable); ProxySQL multiplexing
- elasticsearch_at_scale: 5 coordinating nodes handle scatter-gather; each coordinating node pools connections to data nodes; bulk ingest batching
- distributed_object_storage: API Gateway pools connections to data nodes; Kafka producer batching reduces connection overhead

## Common Databases

### MySQL (Metadata Store)
- 4 of 5 (object_storage, mysql_at_scale, and implied in others); relational metadata (bucket, object, tenant, instance definitions); ACID guarantees; InnoDB B-tree storage

### RocksDB (Embedded Key-Value)
- 3 of 5 (file_system Ceph BlueStore, time_series TSDB label index, elasticsearch segment metadata); embedded KV for local metadata; LSM tree structure; write-optimized

### Kafka (Event Bus + Durable Log)
- 2 of 5 (object_storage change feed, elasticsearch ingest pipeline); ordered durable log; enables async replication and lifecycle processing

### S3 / Object Storage (Long-Term Archival)
- 3 of 5 (elasticsearch snapshots, time_series Thanos blocks, distributed_file_system Ceph backup); cost-effective durable store for cold data and snapshots; Parquet/block format

## Common Communication Patterns

### Async Replication via Event Feed
- distributed_object_storage: Kafka change feed → replication worker → remote cluster async PUT
- elasticsearch_at_scale: ILM transitions (hot → warm → cold) triggered by background ILM thread
- mysql_at_scale: semi-sync on primary write path; async binlog relay to additional replicas

### Master/Leader + Read Replicas
- All 5: writes go to single authoritative node/shard; reads distributed across replicas
- mysql_at_scale: ProxySQL `readOnly` flag routes reads to replicas; writes to primary; 200 K reads/sec across 2+ replicas per shard

## Common Scalability Techniques

### Compression for Storage Efficiency
- All 5 apply compression to cold/archived data
- time_series_database: Gorilla XOR + delta-of-delta encoding; **1.37 bytes/sample** vs 16 bytes raw (11.7× compression); counters ≈ 0.5 bytes/sample
- elasticsearch_at_scale: `best_compression` codec on warm/cold tiers; force-merge to 1 segment per shard (reduces segment metadata overhead)
- distributed_object_storage: RS(10,4) erasure coding; 1.4× overhead vs 3× replication (2.1× more efficient)
- distributed_file_system: EC 4+2 → 1.5× overhead (vs 3× replication); zstd compression for log files

### Read Replica Routing + Connection Multiplexing
- mysql_at_scale + elasticsearch_at_scale: reads routed to replicas/replica shards; ProxySQL enforces `max_replication_lag` to prevent stale reads; coordinating nodes do scatter-gather

## Common Deep Dive Questions

### How do you ensure durability without synchronously writing to all replicas?
Answer: The pattern is WAL + semi-synchronous replication: (1) Write to local WAL first (fast, sequential write); (2) Acknowledge client only after WAL is durable; (3) Apply asynchronously to all replicas. For MySQL: `AFTER_SYNC` wait point means the binlog is flushed + at least 1 replica has ACKed before the client sees success — RPO = 0. For Elasticsearch: translog is written on every indexing operation; on shard recovery, translog is replayed to restore in-memory Lucene state. For TSDB: WAL ensures no sample loss; head block flush to disk happens every 2 hours.
Present in: mysql_at_scale, elasticsearch_at_scale, time_series_database

### How do you scale reads beyond a single node's capacity?
Answer: Read replicas + routing: MySQL (ProxySQL routes all non-write queries to 2 replicas per shard), Elasticsearch (replica shards serve queries alongside primary shards; coordinating nodes do scatter-gather), TSDB (Thanos Store Gateways serve historical queries from S3-backed blocks, Prometheus serves recent). The key invariant: write path is single-primary (strong consistency), read path is distributed (eventual consistency acceptable for monitoring/analytics workloads).
Present in: mysql_at_scale, elasticsearch_at_scale, time_series_database

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Durability** | 11 nines (object storage), RPO=0 (MySQL), no sample loss (TSDB), 3× replication (DFS) |
| **Availability** | 99.99% (MySQL, Object Storage read path), 99.9% (Elasticsearch, TSDB) |
| **Write Throughput** | 500 GB/s aggregate (DFS), 17.4 K PUTs/sec (object storage), 500 K docs/sec (ES), 50 K writes/sec (MySQL), 2.5 M samples/sec peak (TSDB) |
| **Read Latency** | < 10 ms metadata (DFS), < 200 ms P99 GET (object storage), < 500 ms P99 search (ES), < 10 ms P99 read (MySQL), < 1 s P99 range query (TSDB) |
| **Retention** | Petabytes (DFS), exabytes (object storage), 544 TB / 1-year (ES), 100 TB (MySQL), 1.5 TB local + 1.8 TB long-term (TSDB) |
