# Problem-Specific Notes — Storage Systems (09_storage_systems)

## distributed_file_system

### Unique Purpose
Petabyte-scale file storage for analytics, ML training, and cold backup. Supports both HDFS (Hadoop ecosystem) and Ceph (object/block/file) deployment models.

### Unique Architecture
- **HDFS path**: NameNode (single master, in-memory namespace) + 200 DataNodes; 128 MB blocks; 3× replication default
- **Ceph path**: MON cluster (3–5 nodes) + OSD cluster (200 nodes × 12 HDD × 12 TB = 28.8 PB raw) + MDS (for CephFS); BlueStore on each OSD
- **HDFS HA**: Active + Standby NameNode; JournalNodes (3+) for shared EditLog; AvatarNode fast failover < 30 s
- **Clients**: HDFS clients via `DFSInputStream` with `getBlockLocations` RPC; Ceph clients via `librados` or FUSE

### Key Schema / Data Structures
```
-- HDFS NameNode in-memory (serialized to fsimage + EditLog)
INode {
  inode_id:    UINT64    -- monotonically increasing
  name:        STRING
  parent_id:   UINT64
  permissions: UINT16
  replication: UINT8
  mtime:       INT64
  blocks:      LIST<BlockInfo>
}
BlockInfo {
  block_id:    UINT64
  length:      INT64
  locations:   LIST<DatanodeID>   -- in-memory only, rebuilt on restart
}

-- Ceph BlueStore per-OSD RocksDB metadata
RADOS object → placement group (PG) → OSD set
PG_ID = hash(pool_id, object_name) % num_pgs
```

### Unique Algorithm: CRUSH Placement (Ceph)
```
# CRUSH straw2 weighted placement
straw_length(child, r) = hash(x, child, r) / ln(weight_child / total_weight)
# Each OSD assigned straw; highest straw wins for each replica
# 3× replication: 3 independent draws with different r values
# EC 4+2: 6 draws; shards placed on 6 different failure domains
```

### Unique Algorithm: HDFS Block Placement
```
Replica 1: same rack as writer (or random DataNode)
Replica 2: different rack
Replica 3: same rack as replica 2, different DataNode
→ Rack-awareness: at most ⌊(N_replicas + 1) / 2⌋ replicas per rack
```

### Capacity Math
| Model | Raw | Overhead | Usable |
|-------|-----|----------|--------|
| HDFS 3× replication | 28.8 PB | 3× | 9.6 PB |
| Ceph EC 4+2 | 28.8 PB | 1.5× | 19.2 PB |

### Unique NFRs
- Write throughput: 500 GB/s aggregate (200 nodes × 2.5 GB/s)
- NameNode memory: ~1 GB per 1M files; 300M files → ~300 GB RAM
- Block report cycle: every 6 hours; incremental every 5 s
- Rebalancer: `hdfs balancer -threshold 10` runs off-peak

### Failure Recovery
- DataNode failure: NameNode detects via missed heartbeat (10 min), triggers under-replicated block re-replication
- NameNode failure (HA): Standby replays EditLog from JournalNodes; failover < 30 s
- Ceph OSD down: CRUSH recalculates affected PGs; recovery I/O throttled by `osd_recovery_max_active`

---

## distributed_object_storage

### Unique Purpose
S3-compatible exabyte-scale object storage: 10 billion objects, 100 PB total, 17,400 PUTs/sec peak, 11 nines durability.

### Unique Architecture
- **Front door**: API Gateway → Metadata Service (MySQL) → Data Node cluster (consistent hash placement)
- **Erasure coding**: RS(10,4) — 10 data shards + 4 parity shards = 14 total; placed across 3 AZs (max 4–5 per AZ)
- **Change feed**: every object write → Kafka `object-events` topic → Lifecycle Processor (async)
- **Cross-region replication**: async via Kafka + remote cluster PUT worker

### Key Schema
```sql
CREATE TABLE objects (
  bucket_id        BIGINT       NOT NULL,
  object_key       VARCHAR(1024) NOT NULL,
  version_id       CHAR(32)     NOT NULL DEFAULT (UUID()),
  size_bytes       BIGINT       NOT NULL,
  etag             CHAR(32)     NOT NULL,   -- MD5 of content
  storage_class    ENUM('STANDARD','IA','ARCHIVE') NOT NULL DEFAULT 'STANDARD',
  deleted          BOOLEAN      NOT NULL DEFAULT FALSE,
  created_at       DATETIME(6)  NOT NULL,
  chunk_manifest   JSON         NOT NULL,   -- shard locations: [{node_id, shard_idx, offset}×14]
  PRIMARY KEY (bucket_id, object_key, version_id DESC),
  INDEX idx_bucket_prefix (bucket_id, object_key(128)),
  INDEX idx_lifecycle (storage_class, created_at) WHERE deleted = FALSE
);

CREATE TABLE buckets (
  bucket_id        BIGINT       PRIMARY KEY AUTO_INCREMENT,
  bucket_name      VARCHAR(63)  UNIQUE NOT NULL,
  owner_id         BIGINT       NOT NULL,
  versioning       ENUM('DISABLED','ENABLED','SUSPENDED') DEFAULT 'DISABLED',
  lifecycle_rules  JSON,
  acl              JSON,
  created_at       DATETIME(6)  NOT NULL
);
```

### Unique Algorithm: AZ-Aware Shard Placement
```python
# RS(10,4) placement: 14 shards, max 4-5 per AZ (3 AZs)
def place_shards(object_key, nodes_by_az):
    ring = ConsistentHashRing(nodes_by_az)
    placements = []
    az_count = defaultdict(int)
    for shard_idx in range(14):
        node = ring.next_node(object_key + str(shard_idx))
        while az_count[node.az] >= 5:
            node = ring.next_node(node.id)  # skip overloaded AZ
        placements.append((shard_idx, node))
        az_count[node.az] += 1
    return placements  # tolerates loss of any 4 shards (1 full AZ = 4-5 shards < parity=4)
```

### Presigned URL Flow
```
Client → API Gateway: GET /presign?bucket=b&key=k&expires=3600
API Gateway → Auth: validate IAM policy
API Gateway → sign: HMAC-SHA256(method+bucket+key+expiry, secret)
Return: https://gw.example.com/b/k?X-Amz-Expires=3600&X-Amz-Signature=<sig>
Client → API Gateway: direct GET with presigned URL (no auth header needed)
API Gateway: re-verify HMAC + expiry → serve object
```

### Unique NFRs
- Durability: 11 nines (RS(10,4) across 3 AZs; tolerate full AZ loss + 1 node)
- Availability read: 99.99%; write: 99.9%
- PUT latency P99: < 200 ms for objects < 5 MB
- Max object size: 5 TB (multipart upload required > 100 MB; 10,000 parts × 500 MB each)
- Lifecycle transition SLA: < 24 h after rule triggers

---

## elasticsearch_at_scale

### Unique Purpose
Full-text search and log analytics: 2.3 TB/day ingest, 500 K docs/sec, 544 TB total, < 500 ms P99 search, 1-year retention with hot/warm/cold tiering.

### Unique Architecture
- **61-node cluster**: 22 hot (NVMe, 30-day) + 9 warm (SSD, 90-day) + 22 cold (HDD, 1-year) + 3 dedicated masters + 5 coordinating nodes
- **Ingest path**: Kafka → Logstash/ingest pipelines → bulk API → coordinating node → shard routing
- **ILM**: automates rollover (50 GB or 7 days) and tier migration; force-merge on warm (1 segment/shard)

### Unique Algorithm: Shard Sizing
```
# Target shard size: 40 GB for warm/cold; 10-50 GB for hot
daily_GB = 2,300 GB
rollover_days = 7
primary_shards = ceil(daily_GB × rollover_days / 40 GB) = ceil(16,100 / 40) = ceil(402.5) = 58 → round to 60

# Per-node shard count rule: max 20 shards per GB heap
# Coordinating nodes: 32 GB heap → max 640 total shards per node
# Hot nodes: 64 GB heap → 1,280 shards/node; 22 nodes → 28,160 capacity
```

### ILM Policy
```json
{
  "hot":  { "actions": { "rollover": { "max_size": "50gb", "max_age": "7d" },
                          "set_priority": { "priority": 100 } } },
  "warm": { "min_age": "30d", "actions": {
              "shrink": { "number_of_shards": 1 },
              "forcemerge": { "max_num_segments": 1 },
              "allocate": { "require": { "data": "warm" } },
              "set_priority": { "priority": 50 } } },
  "cold": { "min_age": "90d", "actions": {
              "allocate": { "require": { "data": "cold" } },
              "freeze": {},
              "set_priority": { "priority": 0 } } },
  "delete": { "min_age": "365d", "actions": { "delete": {} } }
}
```

### Unique Index Settings
```json
{
  "index.number_of_shards": 10,
  "index.number_of_replicas": 1,
  "index.translog.durability": "async",
  "index.translog.sync_interval": "5s",
  "index.codec": "best_compression",
  "index.refresh_interval": "30s"
}
```

### Unique NFRs
- Ingest: 500 K docs/sec (bulk API, batches of 5,000 docs)
- Search P99: < 500 ms across hot tier; < 2 s across cold tier
- Cluster size: 61 nodes; 544 TB total storage
- Master heap: 32 GB (master-eligible nodes run no data)
- Snapshot: daily to S3 via SLM; < 2 h for full snapshot

---

## mysql_at_scale

### Unique Purpose
Multi-tenant cloud infrastructure metadata store: 10 shards × 10 TB = 100 TB total, 200 K reads/sec, 50 K writes/sec, < 10 ms P99 read, RPO = 0.

### Unique Architecture
- **Vitess layer**: VTGate (SQL proxy, routing) + VTTablet (per-MySQL sidecar) + global topology in etcd
- **Per shard**: 1 primary + 2 replicas (semi-sync: at least 1 ACK before primary commits)
- **Connection pooling**: HikariCP 10 connections/pod; 1,000 pods × 10 = 10,000 total connections; ProxySQL multiplexing at shard level
- **Semi-sync config**: `rpl_semi_sync_master_wait_for_slave_count = 1`; wait point `AFTER_SYNC` (binlog flushed + replica ACK before client ACK)

### Key Schema
```sql
CREATE TABLE tenants (
  tenant_id   BIGINT PRIMARY KEY AUTO_INCREMENT,
  name        VARCHAR(255) UNIQUE NOT NULL,
  plan        ENUM('free','pro','enterprise') NOT NULL,
  created_at  DATETIME(6) NOT NULL,
  status      ENUM('active','suspended','deleted') NOT NULL DEFAULT 'active',
  KEY idx_status (status)
) ENGINE=InnoDB;

CREATE TABLE instances (
  instance_id  BIGINT PRIMARY KEY AUTO_INCREMENT,
  tenant_id    BIGINT NOT NULL,
  region       VARCHAR(32) NOT NULL,
  instance_type VARCHAR(32) NOT NULL,
  status       ENUM('pending','running','stopped','terminated') NOT NULL,
  created_at   DATETIME(6) NOT NULL,
  KEY idx_tenant_status (tenant_id, status),
  KEY idx_created (created_at),
  FOREIGN KEY (tenant_id) REFERENCES tenants(tenant_id)
) ENGINE=InnoDB;

CREATE TABLE volumes (
  volume_id    BIGINT PRIMARY KEY AUTO_INCREMENT,
  instance_id  BIGINT,
  tenant_id    BIGINT NOT NULL,
  size_gb      INT NOT NULL,
  volume_type  ENUM('ssd','hdd','nvme') NOT NULL,
  iops         INT,
  status       ENUM('available','in-use','deleting') NOT NULL,
  KEY idx_tenant (tenant_id),
  KEY idx_instance (instance_id)
) ENGINE=InnoDB;
```

### Unique Algorithm: Vitess Shard Routing
```sql
-- Shard key: tenant_id (range-based sharding)
-- VTGate routes based on keyspace ID = hash(tenant_id)
-- 10 shards: [0x00, 0x1A), [0x1A, 0x33), ... [0xE6, 0xFF]

-- Cross-shard scatter query (fan-out)
SELECT * FROM instances WHERE created_at > NOW() - INTERVAL 1 DAY;
-- VTGate fans out to all 10 shards, merges results

-- Single-shard query (routed)  
SELECT * FROM instances WHERE tenant_id = 12345 AND status = 'running';
-- VTGate routes to shard containing tenant_id=12345
```

### Replication Config
```sql
-- Semi-sync: RPO = 0
SET GLOBAL rpl_semi_sync_master_enabled = 1;
SET GLOBAL rpl_semi_sync_master_wait_for_slave_count = 1;
SET GLOBAL rpl_semi_sync_master_wait_point = 'AFTER_SYNC';
-- AFTER_SYNC: write to binlog, flush, wait for replica ACK, THEN engine commit
-- Even if primary crashes after flush but before ACK, replica has the data

-- GTID-based replication
SET GLOBAL gtid_mode = 'ON';
SET GLOBAL enforce_gtid_consistency = 'ON';
```

### Unique NFRs
- Read: 200 K/sec (ProxySQL distributes across 2 replicas per shard)
- Write: 50 K/sec (5 K/shard × 10 shards)
- P99 read: < 10 ms; P99 write: < 20 ms
- RPO = 0 (semi-sync AFTER_SYNC); RTO < 30 s (VTTablet failover)
- Backup: `xtrabackup` daily; point-in-time recovery via GTID

---

## time_series_database

### Unique Purpose
Prometheus-compatible TSDB for infrastructure metrics: 12.5M active series, 833 K samples/sec avg (2.5 M peak), < 1 s P99 range query, 1.5 TB local + 1.8 TB long-term.

### Unique Architecture
- **Write path**: Prometheus scrapes → WAL (sequential write) → in-memory head block (2 h window) → flush to persistent block (local SSD)
- **Long-term path**: Thanos Sidecar uploads completed 2-h blocks to S3; Thanos Query Gateway federates local + remote queries
- **Read path**: recent (< 2 h) from head block; recent blocks from local SSD; old data via Thanos Store Gateway (S3)
- **Downsampling**: Thanos Compactor: 5-min resolution after 90 days; 1-hr resolution after 1 year

### Unique Algorithm: Gorilla Compression
```
# Delta-of-delta for timestamps
t[0]: stored as full 64-bit
delta[1] = t[1] - t[0]          # first delta (64-bit)
for i >= 2:
  dod = (t[i] - t[i-1]) - delta[i-1]  # delta of delta
  if dod == 0:        encode as '0'       (1 bit)
  elif -63 < dod < 64: encode as '10' + 7-bit value  (9 bits)
  elif -255 < dod < 256: encode as '110' + 9-bit value (12 bits)
  else:               encode as '1110' + 12-bit value (16 bits)
  # Regular 15s scrapes: dod ≈ 0 → 1 bit per timestamp

# XOR for values
v_xor = current_float64 XOR previous_float64
if v_xor == 0:     encode as '0'              (1 bit) — same value
else:
  leading zeros + trailing zeros + meaningful bits
  # Reuse previous block if same leading/trailing count (2 bits + significant bits)
  # Otherwise: 5-bit leading count + 6-bit block length + significant bits

# Net result: 1.37 bytes/sample avg (vs 16 bytes raw = 8B timestamp + 8B value)
# Counter metrics (monotonic): ≈ 0.5 bytes/sample (XOR ≈ 0, dod ≈ 0)
```

### WAL + Block Lifecycle
```
Scrape interval: 15s
Head block: 2h in memory (2h × 3600/15 × 12.5M series × 1.37B = ~30 GB compressed)
WAL: sequential writes; ~96 GB uncompressed for 2h window (pre-compression)

On crash: replay WAL → reconstruct head block
On flush (every 2h): head block → persistent block on local SSD
  persistent block = chunks/ + index/ + tombstones/
  index: label pair → posting list → series IDs → chunk refs

Block retention: local SSD holds 15 days = ~1.5 TB
Thanos upload: completed blocks → S3 within 5 min of flush
Compaction: overlapping blocks merged; downsampling applied
```

### Thanos Query Flow
```
User query: sum(rate(http_requests_total[5m])) by (service)
  ↓
Thanos Query (fan-out)
  ├── Prometheus (local): head block + recent blocks (< 15 days)
  └── Thanos Store Gateway: S3 blocks (> 15 days)
        → downloads index from S3 (not full data)
        → binary search posting list for matching series
        → stream matching chunks only

Deduplication: Thanos deduplicates overlapping blocks from multiple Prometheus replicas
Merge: sorted merge of chunk iterators; consistent timestamp alignment
```

### Capacity Math
| Metric | Value |
|--------|-------|
| Active series | 12.5 M |
| Sample rate (avg) | 833 K samples/sec |
| Sample rate (peak) | 2.5 M samples/sec |
| Bytes per sample (Gorilla) | 1.37 |
| Local retention (15 days) | 1.5 TB |
| Long-term (Thanos S3) | 1.8 TB |
| Label index size | ~150 GB (≈10% of data) |
| Head block memory | ~30 GB compressed |

### Unique NFRs
- Ingest: 2.5 M samples/sec peak (no sample loss after WAL ACK)
- Query P99: < 1 s for 24-hour range queries across 12.5 M series
- Local storage: 1.5 TB; Thanos: 1.8 TB
- WAL segment: 128 MB; checkpoint every 2 h
- Series churn: < 5% per scrape cycle (high churn degrades head block memory)
