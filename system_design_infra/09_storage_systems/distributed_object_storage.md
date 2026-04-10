# System Design: Distributed Object Storage (S3-like)

> **Relevance to role:** Cloud infrastructure platform engineers must understand how object storage underpins nearly every service — VM image storage, backup targets, data lake foundations, artifact repositories, and log archival. Designing and operating S3-compatible storage on bare metal requires deep knowledge of consistent hashing, erasure coding, metadata management, and multi-tenant performance isolation.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|-------------|--------|
| FR-1 | Bucket CRUD | Create, list, delete buckets per tenant |
| FR-2 | Object CRUD | PUT, GET, DELETE, HEAD, LIST objects (prefix-based) |
| FR-3 | Multipart upload | Parallel upload of large objects (>5 GB) in parts |
| FR-4 | Presigned URLs | Time-limited, credential-free access to objects |
| FR-5 | Versioning | Optional per-bucket; maintain all versions of an object |
| FR-6 | Lifecycle policies | Automatic tier/expiry rules (hot → warm → archive → delete) |
| FR-7 | Cross-region replication | Async replication of objects to remote clusters |
| FR-8 | Strong read-after-write consistency | Any GET after a successful PUT returns the latest version |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Durability | 11 nines (99.999999999%) annual |
| NFR-2 | Availability | 99.99% for reads, 99.9% for writes |
| NFR-3 | Latency | p50 < 50 ms GET (small objects), p99 < 200 ms |
| NFR-4 | Throughput | 100K+ requests/sec per cluster |
| NFR-5 | Object size | 1 byte to 5 TB |
| NFR-6 | Multi-tenancy | Strict isolation, per-tenant rate limiting |
| NFR-7 | Scalability | Linear horizontal scale to exabytes |

### Constraints & Assumptions
- Bare-metal servers with local NVMe/HDD (no SAN)
- Internal network: 25 Gbps per node, leaf-spine CLOS topology
- Objects are immutable once written (updates create new versions)
- Metadata must support billions of objects per bucket
- Java services for API gateway layer; Python for lifecycle/policy workers

### Out of Scope
- Block storage (EBS-like)
- File system semantics (POSIX)
- Inline data transformation (Lambda-on-read)
- Client-side encryption libraries

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Total objects | 10B objects across all tenants | 10 × 10⁹ |
| Average object size | Weighted avg (many small + fewer large) | ~256 KB |
| Total raw data | 10B × 256 KB | ~2.5 PB |
| With erasure coding RS(10,4) overhead | 2.5 PB × (14/10) = 2.5 PB × 1.4x | ~3.5 PB |
| Daily PUTs | 500M new objects/day | ~5,800 PUTs/sec avg |
| Daily GETs | 2B reads/day | ~23,000 GETs/sec avg |
| Peak multiplier | 3× average | ~17.4K PUTs/sec, ~69K GETs/sec |
| LIST requests | 200M/day | ~2,300/sec avg |

### Latency Requirements

| Operation | p50 | p99 |
|-----------|-----|-----|
| PUT (< 1 MB) | 30 ms | 150 ms |
| GET (< 1 MB) | 20 ms | 100 ms |
| GET (100 MB) | 200 ms | 800 ms |
| HEAD | 10 ms | 50 ms |
| LIST (1000 keys) | 50 ms | 200 ms |
| DELETE | 20 ms | 100 ms |

### Storage Estimates

| Component | Calculation | Value |
|-----------|-------------|-------|
| Metadata per object | key + etag + ACL + version ≈ 500 bytes | 500 B |
| Total metadata | 10B × 500 B | ~5 TB |
| Metadata index (MySQL) | 5 TB + indexes ≈ 2× | ~10 TB |
| Data nodes (12 TB HDD × 60% utilization) | 3.5 PB / (12 TB × 0.6) | ~486 drives |
| Servers (12 drives each) | 486 / 12 | ~41 data servers |
| Storage overhead vs 3x replication | RS(10,4)=1.4x vs replication=3.0x | 2.14x less storage = ~50% cost reduction |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| Ingest (peak) | 17.4K/sec × 256 KB | ~4.5 GB/s |
| Read (peak) | 69K/sec × 256 KB | ~17.6 GB/s |
| Internal replication | erasure coded parity rebuild | ~2 GB/s background |
| Cross-region replication | async, 10% of ingest | ~450 MB/s |

---

## 3. High Level Architecture

```
                          ┌────────────────────────┐
                          │     Load Balancer       │
                          │  (L4/L7, TLS termination)│
                          └───────────┬────────────┘
                                      │
                    ┌─────────────────┼─────────────────┐
                    │                 │                 │
             ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐
             │  API Gateway │  │  API Gateway │  │  API Gateway │
             │  (Java/Spring)│  │  (Java/Spring)│  │  (Java/Spring)│
             └──────┬──────┘  └──────┬──────┘  └──────┬──────┘
                    │                 │                 │
        ┌───────────┴─────────────────┴─────────────────┘
        │
        ├──────────────────────────────────────────────┐
        │                                              │
 ┌──────▼──────┐                               ┌──────▼──────┐
 │  Metadata   │                               │  Placement  │
 │  Service    │◄──────────────────────────────►│  Service    │
 │ (MySQL +    │                               │ (Consistent │
 │  cache)     │                               │  hashing)   │
 └──────┬──────┘                               └──────┬──────┘
        │                                              │
        │         ┌────────────────────────────────────┤
        │         │                                    │
 ┌──────▼──────┐  │  ┌──────────┐  ┌──────────┐  ┌───▼──────┐
 │  Data Node  │  │  │ Data Node│  │ Data Node│  │ Data Node│
 │  (HDD/NVMe) │  │  │          │  │          │  │          │
 │  + Erasure  │  │  │          │  │          │  │          │
 │    Codec    │  │  │          │  │          │  │          │
 └─────────────┘  │  └──────────┘  └──────────┘  └──────────┘
                  │
        ┌─────────▼─────────┐     ┌─────────────────┐
        │ Lifecycle Worker  │     │ Replication Wkr  │
        │ (Python/Celery)   │     │ (cross-region)   │
        └───────────────────┘     └─────────────────┘
                  │
        ┌─────────▼─────────┐
        │  Message Broker   │
        │  (Kafka/RabbitMQ) │
        └───────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Load Balancer** | L4/L7 routing, TLS termination, health checks, rate limiting |
| **API Gateway (Java)** | S3-compatible REST API, authentication (HMAC-SHA256), request validation, presigned URL generation/verification, multipart upload orchestration |
| **Metadata Service** | Stores bucket metadata, object metadata (key, etag, size, ACL, version chain), backed by sharded MySQL with Redis cache |
| **Placement Service** | Consistent hash ring mapping object → data node set; rack/fault-domain-aware placement |
| **Data Nodes** | Store erasure-coded chunks on local disks; serve read/write I/O; report disk health to placement service |
| **Lifecycle Worker (Python)** | Evaluates lifecycle rules (transition, expiration), enqueues transitions via Kafka |
| **Replication Worker** | Consumes change feed from Kafka, replicates to remote cluster via streaming PUT |
| **Message Broker (Kafka)** | Event bus for object change notifications, lifecycle events, replication feed |

### Data Flows

**PUT Object (small, < 5 MB):**
1. Client → LB → API Gateway (auth, validate)
2. API Gateway → Placement Service: get node set for `hash(bucket + key)`
3. API Gateway → Data Nodes: stream object data; erasure-encode into 6 data + 3 parity chunks; write to 9 nodes
4. All chunk ACKs received → API Gateway → Metadata Service: write object record (key, etag, chunk locations, version)
5. Metadata Service commits → API Gateway returns `200 OK` with ETag
6. Metadata write triggers Kafka event for replication/lifecycle

**GET Object:**
1. Client → LB → API Gateway (auth)
2. API Gateway → Metadata Service: lookup object metadata (Redis cache hit or MySQL)
3. API Gateway → Placement Service: resolve chunk locations
4. API Gateway → Data Nodes: read any 6 of 9 chunks in parallel
5. Erasure-decode → stream to client

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Metadata MySQL (sharded by bucket_id)

CREATE TABLE buckets (
    bucket_id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    tenant_id       BIGINT UNSIGNED NOT NULL,
    bucket_name     VARCHAR(63) NOT NULL,
    region          VARCHAR(32) NOT NULL,
    versioning      ENUM('disabled','enabled','suspended') DEFAULT 'disabled',
    created_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY idx_tenant_bucket (tenant_id, bucket_name)
) ENGINE=InnoDB;

CREATE TABLE objects (
    object_id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    bucket_id       BIGINT UNSIGNED NOT NULL,
    object_key      VARCHAR(1024) NOT NULL,
    version_id      BIGINT UNSIGNED NOT NULL DEFAULT 0,
    etag            CHAR(32) NOT NULL,            -- MD5 hex
    size_bytes      BIGINT UNSIGNED NOT NULL,
    content_type    VARCHAR(256),
    storage_class   ENUM('STANDARD','IA','ARCHIVE') DEFAULT 'STANDARD',
    is_delete_marker TINYINT(1) DEFAULT 0,
    chunk_manifest  JSON NOT NULL,                -- [{node_id, chunk_idx, disk_path}, ...]
    created_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_bucket_key_version (bucket_id, object_key, version_id DESC),
    INDEX idx_bucket_prefix (bucket_id, object_key(128))
) ENGINE=InnoDB;

CREATE TABLE multipart_uploads (
    upload_id       CHAR(36) PRIMARY KEY,         -- UUID
    bucket_id       BIGINT UNSIGNED NOT NULL,
    object_key      VARCHAR(1024) NOT NULL,
    part_count      INT UNSIGNED DEFAULT 0,
    status          ENUM('in_progress','completed','aborted') DEFAULT 'in_progress',
    created_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    INDEX idx_bucket_key (bucket_id, object_key)
) ENGINE=InnoDB;

CREATE TABLE multipart_parts (
    upload_id       CHAR(36) NOT NULL,
    part_number     INT UNSIGNED NOT NULL,
    etag            CHAR(32) NOT NULL,
    size_bytes      BIGINT UNSIGNED NOT NULL,
    chunk_manifest  JSON NOT NULL,
    PRIMARY KEY (upload_id, part_number)
) ENGINE=InnoDB;

CREATE TABLE lifecycle_rules (
    rule_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    bucket_id       BIGINT UNSIGNED NOT NULL,
    prefix_filter   VARCHAR(1024) DEFAULT '',
    transition_days INT UNSIGNED,
    transition_class ENUM('IA','ARCHIVE'),
    expiration_days INT UNSIGNED,
    enabled         TINYINT(1) DEFAULT 1,
    INDEX idx_bucket (bucket_id)
) ENGINE=InnoDB;
```

### Database/Storage Selection

| Layer | Technology | Rationale |
|-------|-----------|-----------|
| Object metadata | MySQL 8.0 (sharded by bucket_id hash) | ACID for consistency; well-understood ops; sharded via Vitess |
| Metadata cache | Redis Cluster | Sub-ms lookups for hot objects; cache-aside pattern |
| Data chunks | Local ext4/XFS on HDD/NVMe | Direct disk I/O; no distributed FS overhead |
| Event stream | Kafka | Ordered per-partition; durable; cross-region mirroring with MirrorMaker |
| Placement map | In-memory consistent hash ring | Rebuilt from node registry (MySQL or etcd) on startup |

### Indexing Strategy

- **Primary lookup:** `(bucket_id, object_key, version_id DESC)` — composite index for efficient latest-version retrieval
- **Prefix listing:** `(bucket_id, object_key(128))` prefix index; LIST with prefix/delimiter uses `LIKE 'prefix%'` with cursor-based pagination
- **Tenant isolation:** shard key is `bucket_id`; all queries scoped to a single shard
- **Redis cache key:** `obj:{bucket_id}:{sha256(object_key)}` → serialized metadata; TTL 60s; invalidated on write

---

## 5. API Design

### Storage APIs (S3-Compatible REST)

```
# Bucket operations
PUT    /{bucket}                              → CreateBucket
GET    /                                      → ListBuckets
DELETE /{bucket}                              → DeleteBucket (must be empty)
GET    /{bucket}?versioning                   → GetBucketVersioning
PUT    /{bucket}?versioning                   → SetBucketVersioning
PUT    /{bucket}?lifecycle                    → PutLifecycleConfiguration

# Object operations
PUT    /{bucket}/{key}                        → PutObject
GET    /{bucket}/{key}                        → GetObject
HEAD   /{bucket}/{key}                        → HeadObject
DELETE /{bucket}/{key}                        → DeleteObject
GET    /{bucket}?list-type=2&prefix=X&max-keys=1000  → ListObjectsV2

# Multipart upload
POST   /{bucket}/{key}?uploads                → InitiateMultipartUpload
PUT    /{bucket}/{key}?partNumber=N&uploadId=X → UploadPart
POST   /{bucket}/{key}?uploadId=X             → CompleteMultipartUpload
DELETE /{bucket}/{key}?uploadId=X             → AbortMultipartUpload

# Presigned URLs (generated client-side or by API gateway)
GET    /{bucket}/{key}?X-Amz-Algorithm=...&X-Amz-Signature=...
       → Presigned GET (time-limited, no credentials)
PUT    /{bucket}/{key}?X-Amz-Algorithm=...&X-Amz-Signature=...
       → Presigned PUT
```

**Authentication:** AWS Signature Version 4 (HMAC-SHA256 over canonical request).

**Rate Limiting:** Per-tenant token bucket at the API Gateway; 3,500 PUTs/sec and 5,500 GETs/sec per prefix (matching S3 defaults).

---

## 6. Core Component Deep Dives

### Deep Dive 1: Erasure Coding and Data Placement

**Why it's hard:** Achieving 11-nines durability while keeping storage overhead below 2× requires erasure coding with fault-domain-aware placement. Naive replication (3×) wastes 200% overhead; erasure coding RS(10,4) achieves superior durability at only 1.4x overhead, but complicates reads (must collect 10 shards to decode) and repairs (must reconstruct from k=10 surviving fragments).

**RS(10,4) specifics:**
- 10 data shards + 4 parity shards = 14 total shards per object
- Can reconstruct the full object from **any 10 of 14 shards** — tolerates 4 simultaneous failures
- Storage overhead: 14/10 = **1.4x** vs 3x for replication — 2.14x storage cost reduction
- For a 1 MB object: each shard is 102.4 KB (data and parity shards are the same size)
- Encoding: Galois Field GF(2^8) arithmetic; parity P_i = XOR-weighted linear combination of data shards
- Decoding: solve a linear system over GF(2^8) from any 10 available shards

**Approaches:**

| Approach | Storage Overhead | Durability | Read Latency | Repair Cost | Complexity |
|----------|-----------------|-----------|--------------|-------------|-----------|
| 3× replication | 3.0x (200% extra) | High (3 copies) | Low (any 1 copy) | Low (copy 1 shard) | Low |
| Reed-Solomon RS(10,4) | **1.4x** | Very high (tolerate 4 failures) | Medium (collect 10 of 14) | High (read 10, encode 4) | High |
| Reed-Solomon RS(6,3) | 1.5x | High (tolerate 3 failures) | Medium (collect 6 of 9) | Medium | Medium |
| LRC (Local Reconstruction Code) | ~1.5x | Very high | Low (local parity) | Low (local repair) | Very high |
| Hybrid: replicate hot, EC cold | Variable | High | Low for hot | Variable | Medium |

**Selected approach:** RS(10,4) for standard storage class; 3× replication for the first 24 hours (fast reads during peak access window), then background EC conversion.

**Justification:**
- 1.4x overhead vs 3.0x replication saves ~53% storage cost — at 2.5 PB raw data, saves 4 PB physical storage
- RS(10,4) tolerates 4 simultaneous node failures — with 14 shards placed across 3+ AZs, a full AZ failure (losing ~4-5 shards) is survivable
- Hybrid approach captures the "most reads happen shortly after write" pattern without sacrificing read latency for hot objects

**Implementation Detail:**

```python
import numpy as np
from galois import GF  # Galois field arithmetic

class ReedSolomonCodec:
    """Reed-Solomon RS(10,4) erasure coding over GF(2^8).
    10 data shards + 4 parity shards = 14 total. Tolerates any 4 failures.
    Storage overhead: 14/10 = 1.4x (vs 3x for replication).
    """

    def __init__(self, data_shards=10, parity_shards=4):
        self.k = data_shards
        self.m = parity_shards
        self.n = data_shards + parity_shards
        self.gf = GF(2**8)
        self.encoding_matrix = self._build_cauchy_matrix()

    def _build_cauchy_matrix(self):
        """Build systematic Cauchy encoding matrix over GF(2^8)."""
        # Top k×k is identity (systematic); bottom m×k is Cauchy parity
        matrix = np.eye(self.k, dtype=int)
        parity_rows = []
        for i in range(self.m):
            row = []
            for j in range(self.k):
                # Cauchy matrix: 1 / (x_i + y_j) in GF(2^8)
                x = self.gf(self.k + i)
                y = self.gf(j)
                row.append(int(self.gf(1) / (x + y)))
            parity_rows.append(row)
        return np.vstack([matrix, np.array(parity_rows)])

    def encode(self, data: bytes, chunk_size: int) -> list[bytes]:
        """Split data into k chunks, produce n = k + m chunks."""
        # Pad data to multiple of k * chunk_size
        padded = data.ljust(self.k * chunk_size, b'\x00')
        data_chunks = [padded[i*chunk_size:(i+1)*chunk_size] for i in range(self.k)]

        # Encode each byte position across chunks
        all_chunks = [bytearray(chunk_size) for _ in range(self.n)]
        for pos in range(chunk_size):
            data_vector = self.gf(np.array([c[pos] for c in data_chunks]))
            encoded = self.gf(self.encoding_matrix) @ data_vector
            for i in range(self.n):
                all_chunks[i][pos] = int(encoded[i])

        return [bytes(c) for c in all_chunks]

    def decode(self, chunks: dict[int, bytes], chunk_size: int) -> bytes:
        """Reconstruct from any k of n chunks. chunks = {index: data}."""
        if len(chunks) < self.k:
            raise ValueError(f"Need at least {self.k} chunks, got {len(chunks)}")

        # Select first k available chunks
        available = sorted(chunks.keys())[:self.k]
        sub_matrix = self.gf(self.encoding_matrix[available])
        inv_matrix = np.linalg.inv(sub_matrix)  # GF inversion

        result = bytearray(self.k * chunk_size)
        for pos in range(chunk_size):
            encoded_vector = self.gf(np.array([chunks[i][pos] for i in available]))
            decoded = inv_matrix @ encoded_vector
            for i in range(self.k):
                result[i * chunk_size + pos] = int(decoded[i])

        return bytes(result)


class PlacementService:
    """Consistent-hash-based placement with rack awareness."""

    def __init__(self, nodes: list[dict], virtual_nodes_per_physical=150):
        self.ring = SortedDict()  # hash_value → node_id
        self.node_info = {}       # node_id → {rack, zone, capacity, ...}
        for node in nodes:
            self.node_info[node['id']] = node
            for vn in range(virtual_nodes_per_physical):
                h = md5_hash(f"{node['id']}:{vn}")
                self.ring[h] = node['id']

    def get_placement(self, object_key: str, num_chunks: int = 14) -> list[str]:
        """Return num_chunks node IDs across distinct fault domains.
        For RS(10,4): num_chunks=14; spread across 3+ AZs so no single AZ
        holds more than 4 shards (ensuring AZ loss leaves 10+ shards intact).
        """
        h = md5_hash(object_key)
        candidates = []
        racks_used = set()

        # Walk ring clockwise from hash point
        start_idx = self.ring.bisect_left(h)
        ring_keys = list(self.ring.keys())
        ring_len = len(ring_keys)

        idx = start_idx
        while len(candidates) < num_chunks:
            wrapped_idx = idx % ring_len
            node_id = self.ring[ring_keys[wrapped_idx]]
            rack = self.node_info[node_id]['rack']

            # Ensure rack diversity: max 5 shards per AZ (14 shards / 3 AZs rounded up)
            # This ensures losing 1 AZ (5 shards) leaves 9 data + 4 parity = 13 >= 10
            rack_count = sum(1 for c in candidates
                           if self.node_info[c]['rack'] == rack)
            if rack_count < 5 and node_id not in candidates:
                candidates.append(node_id)
                racks_used.add(rack)

            idx += 1
            if idx - start_idx > ring_len:
                break  # Full ring traversal

        return candidates
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Single data node down | Reads still work (any 10 of 13 remaining shards) | Automatic repair from surviving chunks within 30 min |
| Up to 4 node failures simultaneously | Object still readable from remaining 10 shards | Trigger priority repair before 5th failure |
| Entire AZ failure (5 shards in worst case) | 14 - 5 = 9 shards remaining; if AZ holds only 4 shards, 10 survive | Placement rule: max 4 shards per AZ ensures AZ loss leaves 10+ shards |
| Disk corruption (silent) | Bad chunk returned | Per-chunk CRC32C checksums; detect on read |
| Encoding bug | Data loss at decode time | End-to-end checksum (full object MD5 stored in metadata) |
| Hash ring split brain | Chunks placed on wrong nodes | Etcd-backed ring with leader election; versioned ring |
| Repair storm after large failure | Network saturation | Repair bandwidth throttling; priority queue by object popularity |

**Interviewer Q&As:**

**Q1: Why RS(10,4) instead of RS(6,3)?**
A: RS(10,4) tolerates 4 simultaneous failures vs RS(6,3)'s 3 — but crucially, with 14 shards placed across 3 AZs (max 4-5 per AZ), RS(10,4) survives a full AZ outage even if that AZ holds 4 shards (10 remain). RS(6,3) with 9 shards across 3 AZs (3 per AZ) also survives AZ failure but leaves only 6 remaining — exactly at the minimum, with zero margin. RS(10,4)'s storage overhead is 1.4x vs RS(6,3)'s 1.5x — so RS(10,4) is actually cheaper AND more durable. The tradeoff: RS(10,4) requires collecting 10 network responses vs 6 for RS(6,3), increasing read parallelism requirements.

**Q2: How do you handle repair when a node permanently fails?**
A: The placement service detects the failure via heartbeat timeout. It enqueues all affected objects for repair. For each object, read any 10 surviving shards (from the remaining 13), re-encode the 4 missing shards using Reed-Solomon GF(2^8) arithmetic, place them on a new node respecting AZ constraints (max 4 shards per AZ). Repair is throttled to avoid saturating the network (e.g., 500 MB/s per node). Priority: objects with fewer than 12 surviving shards (risk zone approaching minimum) get highest repair priority.

**Q3: What's the read amplification cost of erasure coding vs replication?**
A: With 3x replication, you read 1 shard (the full object) from any 1 of 3 replicas. With RS(10,4), you must collect 10 shards in parallel (each shard is 1/10th the object size) and decode. Total bytes transferred: identical (1x object size in both cases). Latency: depends on the slowest of 10 parallel shard reads — higher tail latency than a single replica read. Mitigation: issue reads to all 14 shard locations simultaneously; use the first 10 to respond — this turns p99 shard latency into roughly p(1 - (1-p99)^14) ≈ p70 of shard latency.

**Q4: How do you handle the "tail-at-scale" problem when reading 10 shards in parallel?**
A: Issue reads to all 14 shard locations (10 data + 4 parity) simultaneously. Use the first 10 responses to decode. Cancel (abort) the remaining 4 slow requests. This hedged reads strategy: (a) eliminates waiting for stragglers, (b) automatically uses parity shards if data shards are slow, (c) adds 40% extra read I/O overhead but dramatically reduces p99 latency. For hot objects (accessed frequently), this is enabled by default.

**Q5: How does the consistent hash ring handle heterogeneous node capacities?**
A: Weight virtual nodes proportionally to disk capacity. A node with 24 TB gets 2× the virtual nodes of a 12 TB node, so it naturally receives ~2× the placement targets.

**Q6: What happens if the encoding matrix is not invertible for a given subset of surviving chunks?**
A: Cauchy matrices over GF(2^8) are guaranteed to have all submatrices invertible. This is a mathematical property of Cauchy matrices — any k×k submatrix of the n×k encoding matrix is non-singular. This is precisely why Cauchy matrices are chosen over Vandermonde.

---

### Deep Dive 2: Metadata Service and Strong Read-After-Write Consistency

**Why it's hard:** Metadata must support billions of objects with sub-10ms lookup, handle concurrent writes to the same key (versioning), and provide strong read-after-write consistency. Since metadata is sharded across MySQL instances, cross-shard consistency and cache invalidation are non-trivial.

**Approaches:**

| Approach | Consistency | Latency | Scalability | Operational Cost |
|----------|------------|---------|-------------|-----------------|
| Single MySQL primary | Strong | Low | Limited (vertical) | Low |
| Sharded MySQL (Vitess) | Strong per shard | Low | High | Medium |
| DynamoDB-style (eventual) | Eventual | Very low | Very high | Low |
| MySQL + synchronous cache invalidation | Strong | Low with cache | High | High |
| Raft-based custom metadata store | Strong (consensus) | Medium | High | Very high |

**Selected approach:** Sharded MySQL via Vitess with synchronous Redis cache invalidation.

**Justification:**
- MySQL gives ACID per shard; no cross-shard transactions needed (all object ops are within a single bucket, which maps to a single shard)
- Vitess provides online resharding as data grows
- Redis provides sub-ms reads for hot metadata; cache invalidation on every write guarantees no stale reads

**Implementation Detail:**

```java
@Service
public class MetadataService {

    private final VitessDataSource vitessDataSource;
    private final RedisTemplate<String, ObjectMetadata> redisTemplate;
    private final MeterRegistry meterRegistry;

    private static final Duration CACHE_TTL = Duration.ofSeconds(60);

    /**
     * Write-through metadata: write to MySQL first, then invalidate cache.
     * Strong read-after-write: cache invalidation is synchronous.
     */
    @Transactional(propagation = Propagation.REQUIRED,
                   isolation = Isolation.READ_COMMITTED)
    public ObjectMetadata putObjectMetadata(PutObjectRequest req) {
        long bucketId = req.getBucketId();
        String objectKey = req.getObjectKey();

        // Determine version_id
        long versionId = 0;
        if (isBucketVersioningEnabled(bucketId)) {
            versionId = snowflakeIdGenerator.nextId();
        }

        ObjectMetadata metadata = ObjectMetadata.builder()
            .bucketId(bucketId)
            .objectKey(objectKey)
            .versionId(versionId)
            .etag(req.getEtag())
            .sizeBytes(req.getSizeBytes())
            .contentType(req.getContentType())
            .storageClass(StorageClass.STANDARD)
            .chunkManifest(req.getChunkManifest())
            .createdAt(Instant.now())
            .build();

        // Upsert into MySQL (Vitess routes to correct shard by bucket_id)
        objectRepository.upsert(metadata);

        // Synchronous cache invalidation — ensures read-after-write consistency
        String cacheKey = cacheKey(bucketId, objectKey);
        redisTemplate.delete(cacheKey);  // Delete, not set — avoids race conditions

        // Publish event for async consumers (replication, lifecycle)
        kafkaTemplate.send("object-events", String.valueOf(bucketId),
            ObjectEvent.created(metadata));

        meterRegistry.counter("metadata.put.success").increment();
        return metadata;
    }

    /**
     * Read-through with cache-aside pattern.
     * On cache miss, read from MySQL, populate cache.
     */
    public Optional<ObjectMetadata> getObjectMetadata(long bucketId, String objectKey) {
        String cacheKey = cacheKey(bucketId, objectKey);

        // Try cache first
        ObjectMetadata cached = redisTemplate.opsForValue().get(cacheKey);
        if (cached != null) {
            meterRegistry.counter("metadata.cache.hit").increment();
            return Optional.of(cached);
        }

        meterRegistry.counter("metadata.cache.miss").increment();

        // Cache miss — read from MySQL (latest version)
        Optional<ObjectMetadata> fromDb = objectRepository
            .findLatestVersion(bucketId, objectKey);

        // Populate cache (only if found and not a delete marker)
        fromDb.filter(m -> !m.isDeleteMarker())
              .ifPresent(m -> redisTemplate.opsForValue()
                  .set(cacheKey, m, CACHE_TTL));

        return fromDb;
    }

    /**
     * Prefix-based listing with cursor pagination.
     * Cannot use cache — scans MySQL index directly.
     */
    public ListObjectsResponse listObjects(long bucketId, String prefix,
                                           String startAfter, int maxKeys) {
        List<ObjectMetadata> objects = objectRepository
            .listByPrefix(bucketId, prefix, startAfter, maxKeys + 1);

        boolean isTruncated = objects.size() > maxKeys;
        if (isTruncated) {
            objects = objects.subList(0, maxKeys);
        }

        return ListObjectsResponse.builder()
            .objects(objects)
            .isTruncated(isTruncated)
            .nextContinuationToken(isTruncated ?
                objects.get(objects.size() - 1).getObjectKey() : null)
            .build();
    }

    private String cacheKey(long bucketId, String objectKey) {
        return "obj:" + bucketId + ":" + Hashing.sha256()
            .hashString(objectKey, StandardCharsets.UTF_8).toString();
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Redis failure | All reads go to MySQL; latency spike | Redis Cluster with replicas; circuit breaker to MySQL direct |
| MySQL shard failure | Metadata writes fail for affected buckets | Semi-sync replication; Vitess automatic failover (< 30s) |
| Cache-DB inconsistency | Stale reads | Delete-on-write (not set-on-write); TTL as safety net |
| Kafka event loss | Replication/lifecycle delayed | Kafka `acks=all`, `min.insync.replicas=2` |
| Snowflake ID generator failure | Cannot assign version IDs | Pre-allocated ID ranges; fallback to timestamp-based |
| Vitess resharding in progress | Brief read-only window | Vitess handles this transparently; writes queue briefly |

**Interviewer Q&As:**

**Q1: Why delete-on-write for cache instead of set-on-write?**
A: Set-on-write creates a race condition: if two concurrent writes W1 and W2 both write to MySQL, W2 wins in MySQL but W1's cache set might execute after W2's, leaving stale data in cache. Delete-on-write ensures the next read fetches from MySQL (source of truth).

**Q2: How do you achieve strong read-after-write consistency when metadata is sharded?**
A: Each object belongs to exactly one bucket, which maps to exactly one MySQL shard. All reads and writes for that object hit the same shard's primary. There are no cross-shard reads. Cache invalidation is synchronous within the PUT path, so any subsequent GET will either miss the cache (and read from MySQL primary) or find the updated value.

**Q3: What if the cache delete succeeds but the Kafka publish fails?**
A: The Kafka publish is outside the MySQL transaction. If it fails, the object is stored correctly but replication/lifecycle is delayed. A reconciliation worker periodically scans for objects without corresponding Kafka events and re-publishes them.

**Q4: How does LIST handle billions of objects in a single bucket?**
A: The `(bucket_id, object_key)` index supports efficient range scans. LIST uses cursor-based pagination (`startAfter` parameter). The query is `WHERE bucket_id = ? AND object_key > ? AND object_key LIKE ? ORDER BY object_key LIMIT ?`. This leverages the B-tree index for both prefix filtering and pagination.

**Q5: How do you handle the NameNode memory bottleneck (like HDFS)?**
A: Unlike HDFS where all metadata is in memory, we use MySQL on disk with Redis as the hot cache. This allows metadata to scale to billions of objects limited only by disk, not RAM. Vitess sharding distributes the load across multiple MySQL instances.

**Q6: How do you prevent a thundering herd on cache miss?**
A: Use a distributed lock (Redis SETNX) so only one request populates the cache on a miss. Other concurrent requests wait briefly or fall through to MySQL with a rate limiter.

---

### Deep Dive 3: Multipart Upload

**Why it's hard:** Large files (up to 5 TB) cannot be uploaded atomically. Network failures mid-upload would waste all progress. Parts must be uploaded in parallel, potentially out of order, and assembled atomically. Orphaned incomplete uploads waste storage.

**Approaches:**

| Approach | Parallelism | Resume | Complexity | Storage Waste |
|----------|------------|--------|-----------|---------------|
| Single PUT, retry full | None | No | Low | None |
| Client-side chunking, single stream | None | Partial | Medium | Low |
| Server-side multipart (S3-style) | Full | Per-part | Medium | Managed by abort/lifecycle |
| Resumable upload (GCS-style) | None | Byte-level | Medium | Low |

**Selected approach:** S3-style multipart upload.

**Implementation Detail:**

```java
@RestController
@RequestMapping("/{bucket}/{key:.+}")
public class MultipartUploadController {

    @Autowired private MetadataService metadataService;
    @Autowired private PlacementService placementService;
    @Autowired private DataNodeClient dataNodeClient;
    @Autowired private ReedSolomonCodec codec;

    /**
     * POST /{bucket}/{key}?uploads → Initiate multipart upload
     */
    @PostMapping(params = "uploads")
    public InitiateMultipartUploadResponse initiate(
            @PathVariable String bucket,
            @PathVariable String key) {

        String uploadId = UUID.randomUUID().toString();
        metadataService.createMultipartUpload(bucket, key, uploadId);

        return new InitiateMultipartUploadResponse(bucket, key, uploadId);
    }

    /**
     * PUT /{bucket}/{key}?partNumber=N&uploadId=X → Upload part
     * Each part is independently erasure-coded and stored.
     * Parts: 5 MB to 5 GB each. Up to 10,000 parts.
     */
    @PutMapping(params = {"partNumber", "uploadId"})
    public UploadPartResponse uploadPart(
            @PathVariable String bucket,
            @PathVariable String key,
            @RequestParam int partNumber,
            @RequestParam String uploadId,
            InputStream body) {

        // Validate
        if (partNumber < 1 || partNumber > 10000) {
            throw new InvalidPartNumberException(partNumber);
        }

        // Read part data, compute ETag (MD5)
        byte[] partData = body.readAllBytes();
        String etag = DigestUtils.md5Hex(partData);

        // Erasure encode this part independently
        int chunkSize = (partData.length + codec.getK() - 1) / codec.getK();
        List<byte[]> chunks = codec.encode(partData, chunkSize);

        // Place chunks across fault domains
        String placementKey = uploadId + ":" + partNumber;
        List<String> nodeIds = placementService.getPlacement(placementKey, 9);

        // Write chunks in parallel
        List<CompletableFuture<ChunkWriteResult>> futures = new ArrayList<>();
        for (int i = 0; i < chunks.size(); i++) {
            final int chunkIdx = i;
            futures.add(dataNodeClient.writeChunkAsync(
                nodeIds.get(i), uploadId, partNumber, chunkIdx, chunks.get(i)));
        }

        // Wait for all 9 chunks (or at least 6+1 parity for durability)
        List<ChunkWriteResult> results = CompletableFuture.allOf(
            futures.toArray(new CompletableFuture[0]))
            .thenApply(v -> futures.stream()
                .map(CompletableFuture::join)
                .collect(Collectors.toList()))
            .get(30, TimeUnit.SECONDS);

        // Record part metadata
        metadataService.recordPart(uploadId, partNumber, etag,
            partData.length, buildChunkManifest(results));

        return new UploadPartResponse(etag);
    }

    /**
     * POST /{bucket}/{key}?uploadId=X → Complete multipart upload
     * Validates all parts, creates final object metadata atomically.
     */
    @PostMapping(params = "uploadId")
    public CompleteMultipartUploadResponse complete(
            @PathVariable String bucket,
            @PathVariable String key,
            @RequestParam String uploadId,
            @RequestBody CompleteMultipartUploadRequest request) {

        // Validate: all part numbers contiguous, ETags match
        List<PartSummary> parts = metadataService.listParts(uploadId);
        validatePartsMatch(request.getParts(), parts);

        // Compute composite ETag: MD5 of concatenated part MD5s + "-" + count
        String compositeEtag = computeCompositeEtag(parts);

        long totalSize = parts.stream()
            .mapToLong(PartSummary::getSizeBytes).sum();

        // Build combined chunk manifest (ordered list of per-part manifests)
        List<ChunkManifest> manifests = parts.stream()
            .map(PartSummary::getChunkManifest)
            .collect(Collectors.toList());

        // Atomic metadata commit: create object record, mark upload complete
        ObjectMetadata finalObject = metadataService.completeMultipartUpload(
            bucket, key, uploadId, compositeEtag, totalSize, manifests);

        return new CompleteMultipartUploadResponse(
            bucket, key, finalObject.getVersionId(), compositeEtag);
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Part upload fails mid-stream | Part not recorded; client retries that part | Idempotent part upload (same partNumber overwrites) |
| Complete called with missing parts | Object would be incomplete | Validate all part numbers present before commit |
| Upload abandoned (never completed) | Orphaned chunks waste storage | Lifecycle rule: abort incomplete uploads after 7 days |
| Network partition during complete | Partial metadata commit | MySQL transaction ensures atomicity |
| Part uploaded to wrong upload ID | Data corruption | Validate upload ID ownership on every operation |
| Concurrent complete requests | Double object creation | MySQL unique constraint on (bucket_id, key, version_id) |

**Interviewer Q&As:**

**Q1: How is the final object ETag computed for multipart uploads?**
A: It's the MD5 of the concatenation of all individual part MD5s (in binary), followed by a hyphen and the part count. For example, `"abc123def456-2"`. This differs from single-PUT ETags which are the direct MD5 of the object content.

**Q2: Can parts be uploaded out of order or in parallel?**
A: Yes. Each part is independently erasure-coded and stored. Part numbers from 1 to 10,000. The client can upload part 5 before part 1. The CompleteMultipartUpload request specifies the ordered list, and the server stitches the chunk manifests in that order.

**Q3: What's the minimum and maximum part size?**
A: Minimum 5 MB (except the last part), maximum 5 GB. This ensures each part is large enough to amortize the per-part metadata overhead while not exceeding memory buffers on the server.

**Q4: How do you garbage-collect orphaned parts from aborted uploads?**
A: A lifecycle worker runs daily, finds multipart uploads older than the configured TTL (default 7 days) that are still `in_progress`, marks them `aborted`, and enqueues chunk deletion for all associated parts.

**Q5: How does multipart upload interact with versioning?**
A: The version ID is assigned at CompleteMultipartUpload time, not at initiation. This ensures the version reflects the actual write time. If versioning is enabled, the completed object gets a new version ID; previous versions remain accessible.

**Q6: What if the same part number is uploaded twice with different data?**
A: The second upload overwrites the first. The metadata for that part number is updated, and the old chunks are marked for garbage collection. The ETag in the CompleteMultipartUpload request must match the latest upload for each part.

---

### Deep Dive 4: Presigned URLs

**Why it's hard:** Presigned URLs grant time-limited access to private objects without sharing credentials. They must be cryptographically secure, resistant to replay attacks beyond the expiry window, and must work without server-side state (stateless verification).

**Selected approach:** AWS Signature V4 compatible presigned URLs.

**Implementation Detail:**

```java
@Service
public class PresignedUrlService {

    private final Clock clock;
    private final CredentialStore credentialStore;

    /**
     * Generate a presigned URL for GET or PUT.
     * URL contains all auth info in query parameters.
     */
    public String generatePresignedUrl(PresignedUrlRequest req) {
        Instant now = clock.instant();
        String dateStamp = DateTimeFormatter.ofPattern("yyyyMMdd")
            .withZone(ZoneOffset.UTC).format(now);
        String amzDate = DateTimeFormatter.ofPattern("yyyyMMdd'T'HHmmss'Z'")
            .withZone(ZoneOffset.UTC).format(now);

        // Credential scope
        String credentialScope = String.join("/",
            dateStamp, req.getRegion(), "s3", "aws4_request");

        // Canonical query string (sorted)
        TreeMap<String, String> queryParams = new TreeMap<>();
        queryParams.put("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
        queryParams.put("X-Amz-Credential",
            req.getAccessKeyId() + "/" + credentialScope);
        queryParams.put("X-Amz-Date", amzDate);
        queryParams.put("X-Amz-Expires",
            String.valueOf(req.getExpirationSeconds()));  // max 604800 (7 days)
        queryParams.put("X-Amz-SignedHeaders", "host");

        String canonicalQueryString = queryParams.entrySet().stream()
            .map(e -> urlEncode(e.getKey()) + "=" + urlEncode(e.getValue()))
            .collect(Collectors.joining("&"));

        // Canonical request
        String canonicalRequest = String.join("\n",
            req.getHttpMethod(),                    // GET or PUT
            "/" + req.getBucket() + "/" + urlEncode(req.getKey()),
            canonicalQueryString,
            "host:" + req.getHost() + "\n",         // canonical headers
            "host",                                  // signed headers
            "UNSIGNED-PAYLOAD"                       // payload not signed for presigned
        );

        // String to sign
        String stringToSign = String.join("\n",
            "AWS4-HMAC-SHA256",
            amzDate,
            credentialScope,
            sha256Hex(canonicalRequest)
        );

        // Signing key (derived from secret key)
        byte[] signingKey = getSigningKey(
            req.getSecretAccessKey(), dateStamp, req.getRegion());

        // Signature
        String signature = hmacSha256Hex(signingKey, stringToSign);

        // Assemble URL
        return String.format("https://%s/%s/%s?%s&X-Amz-Signature=%s",
            req.getHost(), req.getBucket(), urlEncode(req.getKey()),
            canonicalQueryString, signature);
    }

    /**
     * Verify a presigned URL on incoming request.
     * Stateless: all info is in the URL.
     */
    public boolean verifyPresignedUrl(HttpServletRequest request) {
        String amzDate = request.getParameter("X-Amz-Date");
        int expires = Integer.parseInt(request.getParameter("X-Amz-Expires"));
        String signature = request.getParameter("X-Amz-Signature");

        // Check expiry
        Instant signTime = Instant.from(DateTimeFormatter
            .ofPattern("yyyyMMdd'T'HHmmss'Z'")
            .withZone(ZoneOffset.UTC).parse(amzDate));
        if (clock.instant().isAfter(signTime.plusSeconds(expires))) {
            return false;  // URL expired
        }

        // Reconstruct canonical request from incoming request and recompute
        String credential = request.getParameter("X-Amz-Credential");
        String accessKeyId = credential.split("/")[0];
        String secretKey = credentialStore.getSecretKey(accessKeyId);

        String recomputedSignature = recomputeSignature(request, secretKey);
        return MessageDigest.isEqual(
            signature.getBytes(), recomputedSignature.getBytes());
    }

    private byte[] getSigningKey(String secretKey, String dateStamp, String region) {
        byte[] kDate = hmacSha256(("AWS4" + secretKey).getBytes(), dateStamp);
        byte[] kRegion = hmacSha256(kDate, region);
        byte[] kService = hmacSha256(kRegion, "s3");
        return hmacSha256(kService, "aws4_request");
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Clock skew between URL generator and verifier | Valid URLs rejected or expired URLs accepted | NTP sync on all servers; allow 15-minute skew tolerance |
| Secret key compromised | All presigned URLs valid until key rotated | Key rotation; short expiry defaults (1 hour) |
| URL shared beyond intended scope | Unauthorized access | Short expiry; IP restriction (custom condition) |
| Replay within validity window | Allowed by design | Acceptable for S3 semantics; if not, add nonce |

**Interviewer Q&As:**

**Q1: Why is presigned URL verification stateless?**
A: All the information needed to verify the signature is in the URL itself (credential, date, expiry, signed headers). The server only needs the secret key (looked up by access key ID). No server-side session state is required, which means any API gateway node can verify.

**Q2: How do you revoke a presigned URL before it expires?**
A: You cannot revoke an individual URL. The options are: (1) rotate the access key, which invalidates all URLs signed with it, or (2) add a bucket policy that denies access after a certain date, or (3) delete the object.

**Q3: What's the maximum expiry for presigned URLs?**
A: 7 days (604,800 seconds) when signed with an access key directly. When using temporary credentials (STS), the maximum is the credential's remaining validity.

**Q4: Can presigned URLs be used for multipart upload?**
A: Yes. You generate a presigned URL for each UploadPart call (specifying partNumber and uploadId in query params). The client uploads each part using its presigned URL.

**Q5: How do you prevent presigned URL abuse for DDoS amplification?**
A: Rate limiting at the API gateway per access key ID. Also, the object must exist (for GET) and the bucket policy must allow the operation. If the object is large, bandwidth cost is the concern — monitor and alert on anomalous download patterns.

**Q6: How does presigned URL work with server-side encryption?**
A: If the bucket uses SSE-S3 (server-managed keys), it works transparently. For SSE-C (customer-provided keys), the client must include encryption headers in the presigned URL, which is impractical. For SSE-KMS, the presigned URL signer must have KMS permissions.

---

## 7. Scaling Strategy

**Horizontal scaling dimensions:**

| Dimension | Approach |
|-----------|---------|
| API Gateway | Stateless; add instances behind LB |
| Metadata (MySQL) | Vitess resharding: split hot shards |
| Cache (Redis) | Redis Cluster: add slots |
| Data Nodes | Add nodes to consistent hash ring; data rebalances gradually |
| Kafka | Add partitions and brokers |

**Prefix-based sharding to avoid hot partitions:**
S3-compatible storage historically had hot-partition problems when all keys shared a common prefix (e.g., date-based prefixes). Solution: internally hash the first component of the key to distribute across metadata partitions. Since 2020, S3 removed the need for random prefixes by automatically partitioning by key hash.

**Interviewer Q&As:**

**Q1: How do you handle a sudden 10× traffic spike to a single bucket?**
A: The bucket's metadata shard may become a hotspot. Mitigation: (1) Redis cache absorbs read spikes, (2) Vitess can split the shard further by object key range, (3) API gateway rate limiting prevents overload. For data reads, multiple data nodes serve chunks in parallel, so read bandwidth scales with node count.

**Q2: How do you add data nodes without disrupting reads?**
A: Add the new node to the consistent hash ring. Only objects that hash to the new node's range need to move. Virtual nodes ensure minimal data movement (~1/N of total data for 1 new node in an N-node ring). Reads for in-flight objects check both old and new locations during the transition window.

**Q3: What's the blast radius of a metadata shard failure?**
A: Only buckets mapped to that shard are affected. With Vitess semi-sync replication, failover takes < 30 seconds. During failover, affected buckets return 503 for writes but may serve cached reads.

**Q4: How do you handle cross-region replication lag?**
A: Cross-region replication is async, so lag is inherent (typically seconds to minutes). The source region's metadata is authoritative. If the destination region is queried for a not-yet-replicated object, it returns 404. Applications that require strong cross-region consistency must use a single-region approach or implement read-your-writes at the client.

**Q5: How do you scale LIST operations for buckets with billions of keys?**
A: LIST is inherently expensive. The `(bucket_id, object_key)` B-tree index makes it efficient for prefix scans with cursor pagination. For extreme scale (>1B keys), we partition the index by key prefix hash. Additionally, we set a max-keys limit (default 1000) and require cursor-based pagination.

**Q6: How do you prevent one tenant from monopolizing cluster resources?**
A: Per-tenant rate limiting at the API gateway (token bucket per access key). Per-tenant storage quotas enforced at the metadata layer. Per-tenant bandwidth limits enforced at the data node layer. QoS classes allow premium tenants higher limits.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO | RPO |
|---|---------|-----------|--------|----------|-----|-----|
| 1 | Single data node crash | Heartbeat timeout (30s) | Degraded reads (8/9 chunks available) | Auto-repair missing chunks | 0 (reads continue) | 0 |
| 2 | Full rack failure | Multiple heartbeat timeouts | 3 chunks per object unavailable; EC still serves | Emergency repair prioritized | 0 (reads continue) | 0 |
| 3 | MySQL metadata shard failure | Vitess health check | Writes fail for affected shard | Semi-sync replica promotion | < 30s | 0 (semi-sync) |
| 4 | Redis cluster partition | Sentinel detection | All reads go to MySQL; latency spike | Auto-heal or manual redis failover | 0 (degraded) | 0 |
| 5 | Kafka broker failure | ISR shrink alert | Event delivery delayed | Kafka controller reassigns partitions | < 60s | 0 (replicated) |
| 6 | Network partition (AZ split) | BGP/health probes | One AZ isolated; requests routed to healthy AZs | DNS/LB failover | < 60s | 0 |
| 7 | Silent data corruption | CRC check on read | Single chunk corrupted | Read from parity; repair corrupted chunk | 0 | 0 |
| 8 | Full cluster disaster | Monitoring/alerting | Complete outage | Cross-region failover (DNS) | Minutes | Seconds (async repl.) |

### Data Durability and Replication

**Durability calculation for Reed-Solomon 6+3:**
- Probability of single drive failure in a year: ~2% (AFR for enterprise HDD)
- Object survives if at most 3 of 9 chunks are lost
- P(object loss) = P(4+ of 9 chunks lost simultaneously before repair)
- With 30-minute repair time and 2% AFR: P(loss) ≈ 10⁻¹² (12 nines)
- Cross-region replication adds another layer: regional disaster requires both regions to lose the object

**Consistency model:**
- **Read-after-write:** Guaranteed. PUT completes only after metadata is committed to MySQL primary. GET reads from primary (or cache that was synchronously invalidated).
- **List-after-write:** Guaranteed. LIST queries the same MySQL primary.
- **Read-after-delete:** Guaranteed. DELETE invalidates cache synchronously.

---

## 9. Security

| Layer | Mechanism |
|-------|-----------|
| **Authentication** | AWS Signature V4 (HMAC-SHA256); IAM identity federation via OIDC/SAML |
| **Authorization** | Bucket policies (resource-based), IAM policies (identity-based), ACLs (legacy); evaluated using policy union with explicit deny override |
| **Encryption at rest** | AES-256; SSE-S3 (server-managed keys), SSE-KMS (KMS-managed envelope encryption), SSE-C (customer-provided keys) |
| **Encryption in transit** | TLS 1.3 mandatory for all API endpoints; mTLS between internal services |
| **Network isolation** | VPC endpoints for private access; bucket policies can restrict to source VPC/IP |
| **Audit** | All API calls logged to audit log (immutable, separate storage); S3 server access logging |
| **Data integrity** | Content-MD5 header verified on PUT; per-chunk CRC32C; ETag verification on GET |
| **Multi-tenancy isolation** | Namespace isolation (tenant ID prefix on all internal paths); separate encryption keys per tenant |
| **Compliance** | Object Lock (WORM) for regulatory compliance; legal hold; retention policies |

---

## 10. Incremental Rollout Strategy

| Phase | Scope | Duration | Validation |
|-------|-------|----------|-----------|
| 1 | Single-rack prototype (3 nodes, no EC) | 2 weeks | Functional tests, latency baseline |
| 2 | Add erasure coding (6+3); internal-only workloads | 4 weeks | Durability simulation (fault injection), throughput tests |
| 3 | Metadata sharding via Vitess; 1 tenant pilot | 4 weeks | Online resharding test; metadata consistency checks |
| 4 | Multi-tenant GA; 10% traffic migration | 4 weeks | Per-tenant SLA monitoring; canary comparison |
| 5 | Full production; cross-region replication | 6 weeks | Disaster recovery drill; compliance audit |

**Rollout Q&As:**

**Q1: How do you migrate data from an existing storage system without downtime?**
A: Dual-write approach: new objects go to both old and new systems. A background migration worker copies existing objects to the new system. Once all data is migrated, flip reads to the new system. Verify with checksum comparison. Roll back by flipping reads back to the old system.

**Q2: How do you validate durability before GA?**
A: Run a durability simulation: intentionally fail nodes and verify all objects are recoverable. Also, write N million test objects, kill random nodes, verify 100% recovery. Measure repair time and network cost.

**Q3: How do you handle the consistent hash ring rebalancing during initial load?**
A: Seed the ring with all planned nodes from the start, even if some are empty. This avoids massive rebalancing later. Alternatively, pre-split the ring into a fixed number of partitions (e.g., 4096) and assign partitions to nodes, similar to Cassandra's token ranges.

**Q4: How do you test the read-after-write consistency guarantee?**
A: Automated consistency checker: continuously PUT objects and immediately GET them. Verify that GET returns the exact bytes written. Run this across all metadata shards and data node groups. Alert if any inconsistency detected within 100ms of PUT completion.

**Q5: What's your rollback strategy if erasure coding introduces data corruption?**
A: Keep 3× replication as the default for the first month, with EC as opt-in. Run EC in shadow mode: encode and store EC chunks alongside replicas, but serve reads from replicas. Periodically decode EC chunks and compare to replicas. Only switch to EC-only after confidence is established.

**Q6: How do you handle schema migrations in the metadata MySQL without downtime?**
A: Use MySQL 8.0 instant ADD COLUMN for backward-compatible changes. For complex migrations, use Vitess's online DDL (gh-ost under the hood): creates a shadow table, backfills via binlog, then atomic rename. No downtime, no locks.

---

## 11. Trade-offs & Decision Log

| # | Decision | Alternatives Considered | Rationale |
|---|----------|------------------------|-----------|
| 1 | Reed-Solomon 6+3 over 3× replication | 3× replication, 4+2 EC, LRC | 50% overhead vs 200%; tolerates rack failure; LRC too complex for V1 |
| 2 | MySQL (Vitess) for metadata over DynamoDB-style | DynamoDB, Cassandra, custom Raft store | ACID guarantees simplify consistency; Vitess provides online resharding; team expertise |
| 3 | Cache-aside with delete-on-write over write-through | Write-through cache, read-through only, no cache | Avoids race conditions; simpler than write-through; TTL as safety net |
| 4 | Consistent hashing over static partitioning | Static hash partitioning, rendezvous hashing | Minimal data movement on node add/remove; virtual nodes handle heterogeneity |
| 5 | Kafka over RabbitMQ for event streaming | RabbitMQ, Redis Streams, Pulsar | Ordered per partition (needed for replication); high throughput; durable; ecosystem maturity |
| 6 | Stateless presigned URLs over token-based access | JWT tokens, session-based access | No server-side state; S3 API compatibility; simple client integration |
| 7 | Hybrid replication→EC over pure EC from day 1 | EC from write, permanent replication | Hot objects (first 24h) benefit from low-latency replicated reads; EC for cold saves storage |
| 8 | Synchronous cache invalidation over async | Async invalidation, TTL-only, write-through | Required for strong read-after-write guarantee; latency cost is minimal (~1ms for Redis delete) |

---

## 12. Agentic AI Integration

| Use Case | Agentic AI Application | Implementation |
|----------|----------------------|----------------|
| **Intelligent tiering** | ML agent analyzes access patterns per object prefix and automatically adjusts lifecycle policies | Time-series model trained on GET/PUT frequency per prefix; outputs recommended storage class transitions; agent applies via lifecycle API |
| **Anomaly detection** | Agent monitors per-tenant traffic patterns for abuse or compromise | Streaming anomaly detection on request rate, bandwidth, error rate per tenant; agent triggers rate limit adjustments or alerts |
| **Capacity planning** | Agent forecasts storage growth and pre-provisions data nodes | Prophet/ARIMA model on daily storage growth per region; agent submits bare-metal provisioning requests when capacity < 30-day projection |
| **Repair prioritization** | Agent prioritizes chunk repair based on object criticality and remaining redundancy | When multiple objects need repair, agent scores by: (1) number of surviving chunks (lower = higher priority), (2) access frequency, (3) tenant SLA tier |
| **Cost optimization** | Agent identifies objects with zero access in 90+ days and recommends archival | Batch analysis of access logs; agent generates lifecycle rule proposals; human approval workflow |
| **Automated incident response** | Agent detects cascading failures and takes corrective action | Monitors error rates, node health, queue depths; can automatically drain a node, reroute traffic, or trigger emergency scaling |

**Example: Intelligent Tiering Agent**

```python
class StorageTieringAgent:
    """Agentic AI for automated storage class optimization."""

    def __init__(self, metrics_client, lifecycle_client, llm_client):
        self.metrics = metrics_client
        self.lifecycle = lifecycle_client
        self.llm = llm_client

    async def evaluate_and_act(self):
        """Periodic evaluation loop (runs every 6 hours)."""
        # Gather: access patterns per prefix for all buckets
        prefixes = await self.metrics.get_prefix_access_patterns(
            lookback_days=30, granularity="daily")

        for prefix in prefixes:
            # Analyze: classify access pattern
            pattern = self.classify_pattern(prefix)

            if pattern == "cooling":
                # Object access declining; recommend transition to IA
                confidence = self.compute_confidence(prefix)
                if confidence > 0.85:
                    # Act: apply lifecycle rule
                    await self.lifecycle.add_transition_rule(
                        bucket=prefix.bucket,
                        prefix=prefix.prefix,
                        transition_to="INFREQUENT_ACCESS",
                        after_days=30,
                        reason=f"Agent: access declined {prefix.decline_rate:.0%} "
                               f"over 30 days (confidence={confidence:.2f})")

            elif pattern == "cold":
                # Near-zero access; recommend archive
                if prefix.last_access_days > 90:
                    await self.lifecycle.add_transition_rule(
                        bucket=prefix.bucket,
                        prefix=prefix.prefix,
                        transition_to="ARCHIVE",
                        after_days=90,
                        reason=f"Agent: no access in {prefix.last_access_days} days")

    def classify_pattern(self, prefix) -> str:
        """Classify access pattern: hot, cooling, cold, erratic."""
        daily_access = prefix.daily_access_counts  # last 30 days
        trend = np.polyfit(range(len(daily_access)), daily_access, 1)[0]

        if np.mean(daily_access[-7:]) > 100:
            return "hot"
        elif trend < -5:
            return "cooling"
        elif np.mean(daily_access) < 1:
            return "cold"
        else:
            return "erratic"
```

---

## 13. Complete Interviewer Q&A Bank

**Architecture & Design:**

**Q1: Why separate metadata from data nodes?**
A: Metadata is small, latency-sensitive, and requires ACID transactions. Data is large, throughput-sensitive, and benefits from erasure coding. Separating them allows independent scaling and technology choices (MySQL for metadata, raw disk for data).

**Q2: How does your system compare to Amazon S3's architecture?**
A: S3 uses a similar separation of metadata (backed by DynamoDB internally) and data (distributed across storage nodes). Our approach mirrors this with MySQL/Vitess for metadata and custom data nodes. S3 achieved strong consistency in 2020 by making metadata writes synchronous — we do the same.

**Q3: Why not use a distributed file system (HDFS/Ceph) as the data layer?**
A: HDFS has NameNode memory limitations, 3× replication overhead, and is optimized for large sequential reads (MapReduce), not random small-object access. Ceph RGW is viable but adds operational complexity. Custom data nodes with erasure coding give us control over placement, encoding, and repair.

**Q4: How do you handle object versioning with strong consistency?**
A: Each version gets a unique monotonically increasing version_id (Snowflake ID). The `(bucket_id, object_key, version_id DESC)` index ensures the latest version is always the first row. GET without a version_id returns the first result. DELETE with versioning enabled creates a delete marker (a version with `is_delete_marker=1`).

**Q5: What's your approach to handling the "small file problem"?**
A: Small objects (< 256 KB) have disproportionate metadata overhead. We batch small objects into larger "packed" files on data nodes (similar to Haystack/f4 at Meta). The chunk manifest points to an offset within a packed file. This dramatically improves disk utilization and IOPS efficiency.

**Consistency & Correctness:**

**Q6: Walk me through a scenario where read-after-write could fail and how you prevent it.**
A: Scenario: PUT writes to MySQL primary, then a GET is routed to a MySQL replica before replication completes. Prevention: (1) All metadata reads go to the MySQL primary (not replicas), (2) Redis cache is invalidated synchronously, so cache hits return fresh data, (3) Replicas are only used for analytics/backup, never for serving API reads.

**Q7: How do you handle concurrent PUT to the same key?**
A: With versioning enabled: both succeed, creating two versions. The later one (by MySQL commit order) has a higher version_id and becomes the "latest." Without versioning: last-writer-wins. MySQL's row-level locking on `(bucket_id, object_key)` serializes the metadata writes; the second PUT's data replaces the first.

**Q8: How do you ensure data integrity end-to-end?**
A: Four layers: (1) Client sends Content-MD5 header; API gateway verifies before storing. (2) Each erasure-coded chunk has a CRC32C checksum stored alongside it. (3) Object ETag (MD5) is stored in metadata. (4) Background scrubber periodically reads and verifies all chunks. Any corruption triggers automatic repair.

**Performance & Optimization:**

**Q9: How do you handle hot objects (viral content)?**
A: (1) Redis metadata cache eliminates metadata lookups, (2) Data node read cache (Linux page cache) serves hot chunks from RAM, (3) For extremely hot objects, we replicate the data to multiple read replicas (not just EC chunks) — a "popularity-based replication" strategy.

**Q10: What's the throughput bottleneck for large object uploads?**
A: Network bandwidth between client and API gateway, and between API gateway and data nodes. For a single large object upload, the bottleneck is the single-stream throughput. Multipart upload solves this by parallelizing across multiple streams. Internally, erasure encoding is CPU-bound but highly parallelizable (SIMD-optimized GF arithmetic).

**Q11: How do you optimize LIST for buckets with billions of objects?**
A: The B-tree index on `(bucket_id, object_key)` supports efficient range scans. We use cursor-based pagination (not offset-based). For count-star queries, we maintain approximate counts in a separate table updated asynchronously. For very large buckets, we offer S3 Inventory (batch export of bucket contents).

**Operations & Monitoring:**

**Q12: What metrics do you monitor?**
A: Per-API latency (p50/p95/p99), error rates (4xx/5xx), per-tenant request rates, storage utilization per node, erasure coding repair queue depth, replication lag (cross-region), Redis hit rate, MySQL query latency, Kafka consumer lag.

**Q13: How do you perform capacity planning?**
A: Track daily net storage growth (writes minus deletes minus lifecycle transitions) per region. Project 90-day forward capacity needs. Alert when projected capacity exceeds 80% of current provisioned capacity. Lead time for new bare-metal servers is ~2 weeks, so we need 30-day advance warning.

**Q14: How do you handle a data node decommission?**
A: Mark the node as "draining" in the placement service. Background worker reads all chunks from the draining node and re-places them on other nodes (respecting fault domains). Once all chunks are evacuated, remove the node from the hash ring. No client-facing impact.

**Q15: How do you test disaster recovery?**
A: Quarterly DR drills: (1) Simulate full AZ failure by blocking network to one AZ's nodes; verify all objects are still readable from remaining AZs. (2) Simulate full region failure; verify cross-region replica serves reads. (3) Simulate metadata shard failure; verify Vitess failover. All drills are documented with RTO/RPO measurements.

---

## 14. References

| # | Resource | Relevance |
|---|----------|-----------|
| 1 | [Amazon S3 Strong Consistency](https://aws.amazon.com/s3/consistency/) | S3's 2020 strong consistency model |
| 2 | [Reed-Solomon Error Correction (Plank, 2007)](http://web.eecs.utk.edu/~jplank/plank/papers/CS-07-593/) | Mathematical foundations of erasure coding |
| 3 | [Facebook f4: Warm BLOB Storage](https://www.usenix.org/conference/osdi14/technical-sessions/presentation/muralidhar) | Erasure coding for warm data at Meta |
| 4 | [Dynamo: Amazon's Highly Available Key-value Store](https://www.allthingsdistributed.com/files/amazon-dynamo-sosp2007.pdf) | Consistent hashing, virtual nodes |
| 5 | [Vitess: Scalable MySQL](https://vitess.io/docs/) | MySQL sharding, online resharding |
| 6 | [MinIO](https://min.io/docs/) | Open-source S3-compatible storage |
| 7 | [AWS Signature V4](https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-authenticating-requests.html) | Authentication protocol |
| 8 | [Haystack: Finding a Needle in Facebook's Photo Storage](https://www.usenix.org/legacy/event/osdi10/tech/full_papers/Beaver.pdf) | Small object optimization |
