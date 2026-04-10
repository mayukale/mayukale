# System Design: S3 Object Store

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Put Object** — Store an arbitrary blob (0 bytes to 5 TB) identified by a bucket name + key string. Returns ETag (MD5 or SHA-256) of stored content.
2. **Get Object** — Retrieve a full or partial (byte range) object by bucket + key. Supports conditional GETs (`If-None-Match`, `If-Modified-Since`).
3. **Delete Object** — Remove a specific object. With versioning enabled, creates a delete marker; permanently removes a specific version ID when specified.
4. **List Objects** — List objects in a bucket with prefix/delimiter filtering, paginated via continuation tokens. Lexicographic ordering.
5. **Multipart Upload** — Split large objects into up to 10,000 parts (5 MB to 5 GB each), upload parts in parallel or sequentially, then atomically complete. Required for objects > 5 GB.
6. **Object Versioning** — When enabled on a bucket, every PUT creates a new version; GET returns latest by default; specific versions retrievable by version ID.
7. **Presigned URLs** — Generate time-limited URLs allowing unauthenticated access (upload or download) to a specific object, signed with HMAC using the requester's credentials.
8. **Bucket Policies & ACLs** — Define access policies on buckets (IAM-style: Allow/Deny, principal, action, resource, condition).
9. **Lifecycle Policies** — Automatically transition objects to cheaper storage classes (Standard → Standard-IA → Glacier → Deep Archive) after specified time, or expire/delete them.
10. **Storage Classes** — Multiple tiers: Standard (high availability, high cost), Standard-IA (infrequent access), One Zone-IA, Glacier (archival, minutes retrieval), Deep Archive (hours retrieval).
11. **Object Encryption** — Server-Side Encryption: SSE-S3 (managed keys), SSE-KMS (customer-managed in KMS), SSE-C (customer-provided key per request). Client-side encryption also supported.
12. **Cross-Region Replication (CRR)** — Async replication of new objects to a bucket in another region for disaster recovery or latency reduction.
13. **Event Notifications** — Trigger SNS/SQS/Lambda events on object creation, deletion, restore completion.

### Non-Functional Requirements

| Property | Target |
|---|---|
| Durability | 99.999999999% (11 nines) — equivalent to losing 1 object per 10,000 years per billion objects |
| Availability (Standard) | 99.99% (reads), 99.99% (writes) |
| Read latency | p99 < 200 ms for objects < 1 MB |
| Write latency | p99 < 500 ms for objects < 1 MB (first byte in) to durable commit |
| Scale | Trillions of objects; exabytes of data |
| Throughput | 3,500 PUTs/second/prefix, 5,500 GETs/second/prefix (S3 published limits) |
| Security | All data encrypted at rest (AES-256); TLS 1.2+ in transit |
| Consistency | Strong read-after-write consistency (post Nov 2020 S3 announcement) |

### Out of Scope

- IAM service internals (credential issuance, STS token service)
- KMS (Key Management Service) internals
- Glacier archival retrieval job internals (Glacier is treated as a cold storage backend)
- S3 Select (SQL-over-objects query engine)
- S3 Object Lambda (transform objects on read)
- CloudFront CDN integration specifics
- Billing and metering service

---

## 2. Users & Scale

### User Types

| User Type | Description | Behavior Pattern |
|---|---|---|
| **Application Backend** | Microservices storing user-generated content | Mix of read/write; unpredictable spikes |
| **Data Lake Consumer** | Analytics pipeline reading TB-scale datasets | High sequential read; large objects; prefix-based listing |
| **Backup Service** | Incremental backup of databases/servers | Write-heavy; irregular schedule; Glacier transition |
| **Static Website Host** | Serving JS/CSS/HTML assets via S3 + CloudFront | Extreme read-heavy; tiny objects; high concurrency |
| **ML Training Pipeline** | Reading large model artifacts and datasets | Massive parallel reads; 100 GB+ objects |
| **Human (Console User)** | AWS Console / CLI for ad-hoc operations | Low volume; management operations |

### Traffic Estimates

**Assumptions (modeling a large S3-like system):**
- Total stored objects: 300 trillion (300 × 10^12) — AWS disclosed 100+ trillion; assume growth to 300T
- Total data stored: 300 EB (assumption based on public AWS scale statements)
- Daily PUT rate: 100 billion PUTs/day (assumption)
- Daily GET rate: 500 billion GETs/day (reads heavily outweigh writes; cached reads excluded)
- Average object size: 1 MB (mix of tiny metadata objects + large media; geometric mean)
- Peak multiplier: 3× average

| Metric | Calculation | Result |
|---|---|---|
| PUT ops/second (avg) | 100 B / 86,400 | ~1.16 M PUTs/s |
| PUT ops/second (peak) | 1.16 M × 3 | ~3.5 M PUTs/s |
| GET ops/second (avg) | 500 B / 86,400 | ~5.79 M GETs/s |
| GET ops/second (peak) | 5.79 M × 3 | ~17.4 M GETs/s |
| Inbound bandwidth (avg) | 1.16 M × 1 MB | ~9.3 Tbps |
| Inbound bandwidth (peak) | 3.5 M × 1 MB | ~28 Tbps |
| Outbound bandwidth (avg) | 5.79 M × 1 MB | ~46 Tbps |
| Outbound bandwidth (peak) | 17.4 M × 1 MB | ~139 Tbps |
| New data/day | 100 B × 1 MB | ~100 EB/day |
| Storage growth/year (raw 3×) | 100 EB × 365 × 3 | Not sustainable; justifies erasure coding |

### Latency Requirements

| Operation | p50 Target | p99 Target | Notes |
|---|---|---|---|
| PUT object (< 1 MB) | 20 ms | 150 ms | First byte durable |
| GET object (< 1 MB, cache miss) | 15 ms | 120 ms | From storage node to edge |
| GET object (> 100 MB) | 50 ms TTFB | 300 ms TTFB | Then streaming at line rate |
| DELETE object | 10 ms | 50 ms | Async physical deletion |
| List objects (1000 items) | 30 ms | 200 ms | Index scan |
| Multipart complete | 200 ms | 1 s | Atomic reassembly |
| Presigned URL generate | 5 ms | 20 ms | Crypto operation only |

### Storage Estimates

| Item | Calculation | Result |
|---|---|---|
| Total objects | 300 trillion | 3 × 10^14 |
| Avg object size | 1 MB | — |
| Total raw data | 300 T × 1 MB | 300 EB |
| Erasure coding overhead (e.g., 6+3 = 1.5×) | 300 EB × 1.5 | 450 EB physical |
| Metadata per object (~1 KB) | 300 T × 1 KB | 300 PB metadata |
| Daily ingress (new data) | 100 B × 1 MB | ~100 PB/day |

### Bandwidth Estimates

| Direction | Avg | Peak |
|---|---|---|
| Inbound (PUT) | ~9.3 Tbps | ~28 Tbps |
| Outbound (GET) | ~46 Tbps | ~139 Tbps |
| Internal (replication) | 3× inbound | ~84 Tbps |
| CDN offload (assume 80%) | Reduces origin GET to ~9 Tbps | ~27 Tbps at origin |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              CLIENTS                                         │
│  AWS SDK (boto3, aws-sdk-java, ...)  │  AWS CLI  │  REST/HTTP directly       │
└────────────────────────────┬─────────────────────────────────────────────────┘
                             │ HTTPS (TLS 1.3)
                             ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                       EDGE NETWORK (AWS Global Network)                     │
│         Route 53 Anycast DNS → Regional Edge PoPs → API Fleet              │
└────────────────────────────────────────┬────────────────────────────────────┘
                                         │
                                         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                             S3 FRONTEND TIER                                │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  Request Router / API Gateway                                        │   │
│  │  - HTTP/S3 protocol parsing (REST, presigned URL validation)        │   │
│  │  - AuthN: Signature V4 verification (HMAC-SHA256)                   │   │
│  │  - AuthZ: IAM policy evaluation (local cache of policy engine)      │   │
│  │  - Rate limiting: per-account, per-bucket, per-prefix               │   │
│  │  - Routes to: Index Layer (metadata) or Storage Layer (data)        │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────┬────────────────────────────────────┘
                                         │
              ┌──────────────────────────┼───────────────────────────┐
              ▼                          ▼                           ▼
┌─────────────────────┐    ┌─────────────────────────┐   ┌──────────────────────┐
│   INDEX / METADATA  │    │    STORAGE LAYER         │   │  SUPPORTING SERVICES │
│       LAYER         │    │                          │   │                      │
│                     │    │  ┌───────────────────┐   │   │  ┌────────────────┐  │
│  Object Index:      │    │  │ Storage Nodes      │   │   │  │ Replication    │  │
│  Bucket+Key →       │    │  │ (Data Servers)     │   │   │  │ Service (CRR)  │  │
│  {version, storage_ │    │  │                    │   │   │  └────────────────┘  │
│   location, ETag,   │    │  │ - Hold erasure-    │   │   │  ┌────────────────┐  │
│   size, metadata}   │    │  │   coded data shards│   │   │  │ Lifecycle Mgr  │  │
│                     │    │  │ - Serve byte-range │   │   │  └────────────────┘  │
│  Metadata DB:       │    │  │   GET requests     │   │   │  ┌────────────────┐  │
│  DynamoDB-like      │    │  │ - Accept PUT data  │   │   │  │ Event Notif.   │  │
│  (sharded by        │    │  └───────────────────┘   │   │  │ (SNS/SQS/Lambda│  │
│   consistent hash   │    │                          │   │  └────────────────┘  │
│   of bucket+key)    │    │  ┌───────────────────┐   │   │  ┌────────────────┐  │
│                     │    │  │ Placement Service  │   │   │  │ Monitoring     │  │
│  Version Catalog:   │    │  │ (Consistent Hash   │   │   │  │ (CloudWatch)   │  │
│  Append-only list   │    │  │  Ring)             │   │   │  └────────────────┘  │
│  of versions per    │    │  └───────────────────┘   │   └──────────────────────┘
│  object             │    └──────────────────────────┘
└─────────────────────┘
         │
         ▼
┌────────────────────────────────────────────────┐
│               PHYSICAL STORAGE CLUSTER         │
│                                                │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐        │
│  │  Storage │ │  Storage │ │  Storage │  ...    │
│  │  Node    │ │  Node    │ │  Node    │  N nodes│
│  │ (HDD/SSD)│ │ (HDD/SSD)│ │ (HDD/SSD)│        │
│  └──────────┘ └──────────┘ └──────────┘        │
│  Erasure coded across nodes: 6 data + 3 parity │
└────────────────────────────────────────────────┘
```

**Component Roles:**

- **Request Router / API Gateway**: Parses the S3 REST protocol; validates AWS Signature V4 (HMAC-SHA256 of canonical request + credential scope). Evaluates IAM bucket policies (cached locally; refreshed from IAM service asynchronously). Routes PUT requests to storage nodes after determining placement; routes GET requests to storage nodes after looking up object location from the Index Layer.
- **Index / Metadata Layer**: Maps (bucket, key, version_id) → (storage location vector, ETag, size, content-type, user metadata, system metadata). This is the "brain" of the object store — all object discovery goes through here. Implemented as a distributed hash table with strong consistency (more below in §6.1).
- **Placement Service**: Given a new object's (bucket, key) and size, determines which storage nodes will hold its erasure-coded shards, using consistent hashing on the object's key to distribute objects evenly. Returns a write plan: `[node_1: shard_0, node_2: shard_1, ..., node_9: shard_8]`.
- **Storage Nodes**: Commodity servers with large HDDs (for cost efficiency; hot objects on SSDs). Each node manages a local key-value store of shard_id → bytes. Serve byte-range reads efficiently with kernel sendfile. Handle local checksum verification on read (detect bit rot).
- **Replication Service (CRR)**: Consumes replication events (new object created/deleted in bucket with CRR enabled). Reads object from source region, writes to destination bucket in destination region. Implements exponential backoff for transient failures; tracks replication lag per bucket.
- **Lifecycle Manager**: Runs periodic scans of the metadata index to find objects whose lifecycle policy criteria are met (age, object prefix, tag conditions). Triggers storage class transitions (update metadata, copy data to cheaper backend) and expiration (insert delete marker or hard delete).
- **Event Notifications**: Transactional outbox pattern — on object mutation, a notification record is written atomically with the metadata change. A notification worker reads the outbox and publishes to SNS/SQS/Lambda asynchronously. Exactly-once delivery via idempotency key.

**Primary Use-Case Data Flow (PutObject):**

1. Client sends `PUT /bucket/key` with body bytes and `Content-MD5` header to API Gateway (nearest edge endpoint).
2. API Gateway: parse request, extract credentials from `Authorization: AWS4-HMAC-SHA256` header, verify Signature V4.
3. API Gateway: query IAM policy cache — does this principal have `s3:PutObject` permission on `arn:aws:s3:::bucket/key`?
4. API Gateway: check bucket exists and is writable; check object size against account limits.
5. API Gateway → Placement Service: "I have a new object (bucket=X, key=Y, size=Z). Where does it go?" Returns storage node list.
6. API Gateway → Storage Nodes: streams body bytes to write coordinator (one of the storage nodes in the write plan). Coordinator writes erasure-coded shards to peer storage nodes.
7. Storage Nodes: all 6 data shards + 3 parity shards written and acknowledged (or configurable quorum).
8. Write coordinator → API Gateway: "Write committed. Storage location vector = {node_ids, shard_keys}."
9. API Gateway → Index Layer: atomically create/update metadata record `{bucket, key, version_id=uuid, etag=md5(body), size, location_vector, content_type, user_metadata, last_modified=now}`.
10. Index Layer: commit. Returns success.
11. API Gateway → Client: HTTP 200 with `ETag` header and (if versioning enabled) `x-amz-version-id` header.
12. Async: notification outbox processed → SNS/SQS event published; CRR queue gets new entry if bucket has replication rule.

---

## 4. Data Model

### Entities & Schema

The S3 metadata layer is not a relational database — it is a distributed key-value store with secondary indices. Represented here in a pseudo-schema for clarity:

```
-- OBJECT METADATA (primary index: bucket_name + object_key + version_id)
-- Implemented as: DynamoDB-style distributed hash map
-- Partition key: hash(bucket_name + object_key) → determines storage node
-- Sort key (for versioned objects): version_id (UUID, lexicographic = chronological with UUIDv7)

object_metadata {
    bucket_name:     string       -- max 63 chars, DNS-compliant
    object_key:      string       -- max 1024 bytes UTF-8
    version_id:      string       -- UUID; "null" if versioning disabled
    is_delete_marker: bool
    etag:            string       -- MD5 of object (or MD5 of MD5s for multipart)
    size_bytes:      uint64       -- 0 for delete markers
    content_type:    string       -- MIME type
    content_encoding: string
    last_modified:   timestamp
    storage_class:   enum {STANDARD, STANDARD_IA, ONE_ZONE_IA, GLACIER, DEEP_ARCHIVE}
    encryption_type: enum {NONE, SSE_S3, SSE_KMS, SSE_C}
    kms_key_id:      string       -- if SSE_KMS
    user_metadata:   map<string,string>  -- x-amz-meta-* headers, max 2 KB total
    location_vector: list<ShardLocation>
        -- e.g., [{node_id: "n001", shard_index: 0, shard_key: "sha256_of_shard"},
        --         {node_id: "n002", shard_index: 1, shard_key: "sha256_of_shard"},
        --         ...9 entries total]
    checksum_sha256: string       -- if client requested checksum
    replication_status: enum {null, PENDING, COMPLETED, FAILED, REPLICA}
    lock_mode:       enum {null, GOVERNANCE, COMPLIANCE}  -- Object Lock
    lock_retain_until: timestamp
    legal_hold:      bool
    owner_account_id: string
    acl:             list<AclGrant>
    tag_set:         list<Tag>    -- up to 10 tags per object
}

-- BUCKET METADATA (stored separately; low cardinality — billions of buckets max, but practically millions)
bucket_metadata {
    bucket_name:       string PRIMARY KEY
    owner_account_id:  string
    region:            string
    creation_date:     timestamp
    versioning_status: enum {DISABLED, ENABLED, SUSPENDED}
    lifecycle_rules:   list<LifecycleRule>
    cors_rules:        list<CORSRule>
    logging_config:    LoggingConfig
    notification_config: NotificationConfig
    replication_config: ReplicationConfig
    encryption_default: EncryptionConfig
    public_access_block: PublicAccessBlock
    policy:            string (JSON)    -- IAM-style bucket policy
    request_payment:   enum {BucketOwner, Requester}
    object_lock_config: ObjectLockConfig
    tags:              list<Tag>
}

-- MULTIPART UPLOAD TRACKING
multipart_upload {
    upload_id:       string PRIMARY KEY   -- UUID assigned on CreateMultipartUpload
    bucket_name:     string
    object_key:      string
    created_at:      timestamp
    expires_at:      timestamp            -- auto-abort after 7 days by default
    initiator:       string               -- account/role ARN
    storage_class:   enum
    encryption_type: enum
    user_metadata:   map<string,string>
    parts:           map<int, PartInfo>   -- part_number → {etag, size, upload_time}
    status:          enum {IN_PROGRESS, COMPLETING, COMPLETED, ABORTED}
}

-- PART METADATA (child of multipart upload)
-- part_number: 1..10000
-- etag: MD5 of part bytes
-- size_bytes: 5MB..5GB per part (except last which can be 0..5GB)
-- location_vector: where part data shards are stored (same as object_metadata.location_vector)

-- LIFECYCLE RULE DEFINITION
lifecycle_rule {
    id:           string
    status:       enum {ENABLED, DISABLED}
    filter:       {prefix?, tag_set?, object_size_gt?, object_size_lt?}
    transitions:  [{days, storage_class}]     -- e.g., [{days:30, class:STANDARD_IA}, {days:90, class:GLACIER}]
    expiration:   {days?, date?, expired_delete_marker?}
    noncurrent_version_transitions: [{noncurrent_days, storage_class}]
    noncurrent_version_expiration: {noncurrent_days}
    abort_incomplete_multipart_upload: {days_after_initiation}
}

-- OBJECT LOCK CONFIGURATION (WORM: Write Once Read Many)
-- When object lock is enabled on a bucket, objects can be made immutable.
-- COMPLIANCE mode: no one (not even root) can delete/overwrite before retain_until
-- GOVERNANCE mode: privileged users with s3:BypassGovernanceRetention can delete

-- INVENTORY MANIFEST (output of S3 Inventory feature)
-- Daily or weekly CSV/ORC/Parquet file listing all objects in a bucket with their metadata
-- Written to a separate "inventory destination" bucket
-- Used for: audits, lifecycle analysis, compliance checks
```

### Database Choice

**Candidates:**

| Database | Pros | Cons | Role |
|---|---|---|---|
| **Custom distributed KV store** | Optimal for object metadata access patterns; tuned for S3's exact workload; no impedance mismatch | Requires massive engineering investment | Selected for primary object index |
| **DynamoDB (or similar)** | Proven at scale; single-digit ms reads; flexible schema | Scan-heavy list operations are expensive; proprietary | Used as model/inspiration |
| **Cassandra** | Excellent write throughput; LSM-tree suits append-heavy version metadata | Tombstone issues for delete-heavy workloads; eventual consistency | Secondary consideration |
| **PostgreSQL / MySQL** | Full SQL; ACID | Cannot scale to trillions of objects on a single cluster without extreme sharding | Not viable at S3 scale |
| **LevelDB / RocksDB per node** | Embedded KV; excellent for sorted-key scans (important for list_objects) | Single-node only; requires custom distributed layer above | Used within each metadata shard node |

**Selected Architecture: Custom sharded metadata store (per-shard RocksDB) + consistent hash ring for routing**

Justification:
- **RocksDB per shard**: RocksDB provides sorted key iteration (critical for `ListObjects` which requires lexicographic traversal of keys with prefix filtering), fast point lookups, and excellent write throughput via LSM-tree. Compaction policies can be tuned per storage class.
- **Consistent hashing for shard assignment**: The partition key `hash(bucket_name + object_key)` is mapped to a shard via a consistent hash ring. This ensures even distribution across shards and enables shard addition/removal with minimal key migration (only adjacent shard boundaries move).
- **Version chain**: For versioned objects, all versions share the same partition hash but differ by `version_id`. RocksDB's sorted key iteration efficiently retrieves all versions of an object (range scan on `{bucket}:{key}:` prefix).
- **Bucket metadata**: Stored in a separate, simpler distributed store (lower cardinality). ZooKeeper-backed for consistent reads with strong linearizability guarantees needed for bucket create/delete atomicity.

---

## 5. API Design

Authentication: AWS Signature Version 4 (SigV4) — HMAC-SHA256 signed canonical request. All endpoints support both path-style (`s3.amazonaws.com/bucket/key`) and virtual-hosted-style (`bucket.s3.amazonaws.com/key`). HTTPS only in production.

### Object Operations

```
-- PutObject
PUT /{bucket}/{key}
  Headers:
    Content-Type: application/octet-stream
    Content-Length: {bytes}
    Content-MD5: {base64(md5(body))}          -- optional; server validates
    x-amz-content-sha256: {sha256(body)}      -- required for SigV4 streaming
    x-amz-storage-class: STANDARD | STANDARD_IA | GLACIER | ...
    x-amz-server-side-encryption: AES256 | aws:kms
    x-amz-server-side-encryption-aws-kms-key-id: {kms_arn}
    x-amz-meta-{name}: {value}                -- user metadata (x-amz-meta- prefix)
    x-amz-tagging: key1=val1&key2=val2
    x-amz-object-lock-mode: COMPLIANCE | GOVERNANCE
    x-amz-object-lock-retain-until-date: {ISO8601}
    If-None-Match: *                           -- conditional PUT (fail if key exists)
  Body: object bytes
  Response:
    200 OK
    Headers: ETag: "{md5_hex}", x-amz-version-id: {uuid}, x-amz-server-side-encryption: AES256
  Errors: 403 AccessDenied, 409 Conflict (conditional PUT), 507 InsufficientStorage

-- GetObject
GET /{bucket}/{key}?versionId={versionId}
  Headers:
    Range: bytes={start}-{end}                 -- partial content
    If-None-Match: "{etag}"                    -- conditional: return 304 if unchanged
    If-Modified-Since: {HTTP-date}
    x-amz-server-side-encryption-customer-algorithm: AES256  -- SSE-C
    x-amz-server-side-encryption-customer-key: {base64 key} -- SSE-C
  Response:
    200 OK (full content) or 206 Partial Content (Range request)
    Headers: Content-Type, Content-Length, ETag, Last-Modified, x-amz-version-id,
             x-amz-restore (if restored from Glacier), x-amz-storage-class
  Errors: 404 NoSuchKey, 403 AccessDenied, 412 PreconditionFailed

-- HeadObject (metadata only, no body)
HEAD /{bucket}/{key}?versionId={versionId}
  Response: Same headers as GetObject, no body
  -- Used to check existence, size, ETag without downloading content

-- DeleteObject
DELETE /{bucket}/{key}?versionId={versionId}
  Response:
    204 No Content (always; even if key doesn't exist — idempotent)
    Headers: x-amz-delete-marker: true (if versioning inserts delete marker),
             x-amz-version-id: {new_delete_marker_version_id}

-- DeleteObjects (batch delete up to 1000)
POST /{bucket}?delete
  Body (XML):
    <Delete>
      <Object><Key>key1</Key></Object>
      <Object><Key>key2</Key><VersionId>v2</VersionId></Object>
      ...
    </Delete>
  Response (XML): <DeleteResult> with <Deleted> and <Error> lists

-- CopyObject
PUT /{dest_bucket}/{dest_key}
  Headers:
    x-amz-copy-source: /{source_bucket}/{source_key}
    x-amz-copy-source-version-id: {versionId}
    x-amz-metadata-directive: COPY | REPLACE
    x-amz-copy-source-if-match: {etag}         -- conditional copy
  Response: 200 OK, XML body with ETag and LastModified
  -- Server-side copy: no data traverses client; internal copy between storage nodes
```

### Bucket Operations

```
-- CreateBucket
PUT /{bucket}
  Headers: x-amz-bucket-object-lock-enabled: true
  Body (XML, optional):
    <CreateBucketConfiguration>
      <LocationConstraint>eu-west-1</LocationConstraint>
    </CreateBucketConfiguration>
  Response: 200 OK, Location: /{bucket}
  Errors: 409 BucketAlreadyExists, 409 BucketAlreadyOwnedByYou

-- ListObjectsV2
GET /{bucket}?list-type=2&prefix={prefix}&delimiter={delim}&max-keys={n}&continuation-token={token}
  Response (XML):
    <ListBucketResult>
      <Name>{bucket}</Name>
      <Prefix>{prefix}</Prefix>
      <KeyCount>{n}</KeyCount>
      <MaxKeys>{max_keys}</MaxKeys>
      <IsTruncated>true|false</IsTruncated>
      <NextContinuationToken>{token}</NextContinuationToken>
      <Contents>
        <Key>...</Key><LastModified>...</LastModified><ETag>...</ETag><Size>...</Size>
        <StorageClass>STANDARD</StorageClass>
      </Contents>
      <CommonPrefixes><Prefix>{common_prefix}/</Prefix></CommonPrefixes>
    </ListBucketResult>

-- ListObjectVersions
GET /{bucket}?versions&prefix={prefix}&key-marker={key}&version-id-marker={vid}
  -- Returns Versions and DeleteMarkers for all keys, paginated

-- PutBucketPolicy
PUT /{bucket}?policy
  Body: JSON IAM policy document
  Response: 204 No Content

-- PutBucketVersioning
PUT /{bucket}?versioning
  Body (XML): <VersioningConfiguration><Status>Enabled|Suspended</Status></VersioningConfiguration>
  Response: 200 OK

-- PutBucketLifecycleConfiguration
PUT /{bucket}?lifecycle
  Body (XML): <LifecycleConfiguration><Rule>...</Rule></LifecycleConfiguration>
  Response: 200 OK

-- PutBucketReplication
PUT /{bucket}?replication
  Body (XML): <ReplicationConfiguration> with role, rules, destination bucket
  Response: 200 OK
```

### Multipart Upload

```
-- Step 1: Initiate
POST /{bucket}/{key}?uploads
  Headers: x-amz-storage-class, x-amz-server-side-encryption, x-amz-meta-*
  Response (XML): <InitiateMultipartUploadResult>
                    <Bucket>...<Key>...<UploadId>{uuid}</UploadId>
                  </InitiateMultipartUploadResult>

-- Step 2: Upload part (repeat for each part, parallel allowed)
PUT /{bucket}/{key}?partNumber={1..10000}&uploadId={uuid}
  Headers: Content-Length, Content-MD5
  Body: part bytes (5 MB to 5 GB each; last part can be smaller)
  Response: 200 OK, ETag: "{part_md5}"

-- Step 3: Complete upload (atomic assembly)
POST /{bucket}/{key}?uploadId={uuid}
  Body (XML): <CompleteMultipartUpload>
                <Part><PartNumber>1</PartNumber><ETag>"{md5}"</ETag></Part>
                ...
              </CompleteMultipartUpload>
  Response: 200 OK (XML with final ETag = MD5 of MD5s + "-{num_parts}")

-- Abort upload (clean up incomplete parts)
DELETE /{bucket}/{key}?uploadId={uuid}
  Response: 204 No Content

-- List parts of an in-progress upload
GET /{bucket}/{key}?uploadId={uuid}&part-number-marker={n}
  Response (XML): list of Part {PartNumber, LastModified, ETag, Size}

-- List all in-progress multipart uploads
GET /{bucket}?uploads&prefix={prefix}&key-marker={key}&upload-id-marker={uid}
  Response (XML): list of in-progress uploads
```

### Presigned URLs

```
-- Generate presigned GET URL (client-side SDK operation, no server call)
-- Client SDK computes HMAC-SHA256 signed URL:
-- https://{bucket}.s3.{region}.amazonaws.com/{key}
--   ?X-Amz-Algorithm=AWS4-HMAC-SHA256
--   &X-Amz-Credential={access_key}/{date}/{region}/s3/aws4_request
--   &X-Amz-Date={timestamp}
--   &X-Amz-Expires={seconds}      -- max 604800 (7 days)
--   &X-Amz-SignedHeaders=host
--   &X-Amz-Signature={hmac_sha256}

-- Presigned PUT URL (for direct upload from browser/mobile)
-- Same construction but method=PUT; includes Content-Type in signed headers
-- Optionally includes Content-Length-Range condition (POST policy for browser form upload)

-- POST presigned upload (browser form upload via HTML <form>)
POST https://{bucket}.s3.amazonaws.com/
  Body: multipart/form-data with:
    key: {object_key}
    Content-Type: {mime_type}
    X-Amz-Credential: ...
    X-Amz-Algorithm: AWS4-HMAC-SHA256
    X-Amz-Date: ...
    Policy: {base64(policy_json)}  -- conditions: bucket, key prefix, size range, content-type
    X-Amz-Signature: {hmac_of_policy}
    file: {binary content}
  Response: 204 No Content (redirect to success_action_redirect if specified)
```

---

## 6. Deep Dive: Core Components

### 6.1 Object Placement & Consistent Hashing

**Problem it solves:**
With trillions of objects and thousands of storage nodes, the system must efficiently determine: (a) which nodes hold a new object's shards (write path), and (b) which nodes to contact to reassemble an object on read (lookup path). The placement scheme must distribute load evenly, minimize data movement when nodes are added/removed, and survive node failures without data loss.

**Approaches Comparison:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Static hash (modulo)** | `node = hash(key) % N` | Simple | Adding/removing node → rehash ~100% of keys; catastrophic for petabyte-scale |
| **Lookup table (directory)** | Central directory maps key → node list | Flexible, any mapping | Central bottleneck; single point of failure; table size = object count (trillion rows) |
| **Consistent hashing (ring)** | Hash space 0..2^32; nodes placed at ring positions; key maps to nearest clockwise node | O(K/N) keys move when node added/removed; no central dir | Hot spots if nodes unevenly distributed |
| **Consistent hashing + virtual nodes** | Each physical node owns V virtual ring positions (vnodes) | Even distribution; fine-grained load rebalancing | More complex routing table; V×N ring entries |
| **Rendezvous (highest random weight) hashing** | For each candidate node, compute score = hash(key+node_id); top-N nodes win | Simple; naturally handles node removal (only affected keys move) | O(N) computation per lookup unless caching |
| **CRUSH (Controlled Replication Under Scalable Hashing)** | Deterministic placement algorithm used by Ceph; takes cluster topology as input | Placement is computable (no lookup table); topology-aware (rack/AZ spreading) | Complex algorithm; requires cluster topology map |

**Selected Approach: Consistent hashing with virtual nodes (128 vnodes per physical node) + topology-aware placement rules**

Justification:
- **Consistent hashing** is selected over lookup table because at the scale of trillions of objects, a central lookup table becomes a bottleneck (trillion-row directory). Consistent hashing is purely computational — no external lookup.
- **Virtual nodes (vnodes)** solve the "hot spot" problem of basic consistent hashing: with 128 vnodes per physical node, the key space is divided into 128N segments, and each physical node owns N statistically equal-sized segments. When a node fails, its 128 segments are redistributed across all healthy nodes, spreading the recovery load.
- **Topology-aware placement** ensures that for a 6+3 erasure coded object, no two shards land on the same rack or availability zone. This guarantees that a full rack failure (power or switch) doesn't cause data loss — the 3 parity shards can recover from losing all 3 nodes of a failed rack.

**Implementation Detail:**

```
Consistent hash ring construction:
  - Ring space: 0 .. 2^64 - 1 (64-bit hash space for large clusters)
  - For each physical node P_i with 128 vnodes:
      for v in range(128):
          vnode_token = SHA256(f"{P_i.node_id}:{v}") % 2^64
          ring.insert(vnode_token, P_i)
  - ring is sorted by token; stored in memory on all coordinator nodes

Object placement (PUT):
  1. Compute ring_position = SHA256(f"{bucket}/{key}") % 2^64
  2. Walk clockwise on ring starting from ring_position
  3. Collect first 9 unique physical nodes (not vnodes) with topology constraints:
       - Max 2 nodes per rack
       - At least 1 node per AZ (for 3 AZs: 3+3+3 distribution)
       → {n1, n2, ..., n9}: 6 data shards + 3 parity shards
  4. Shard assignment: node_i holds shard_i (shard_0..5 = data, shard_6..8 = parity)

Object lookup (GET):
  - Same deterministic algorithm: recompute placement from ring state
  - No metadata lookup for location needed at this level
  - BUT: ring state can change (node added/removed) → must consult metadata
    for the committed location_vector (stored at index commit time)
  Note: in practice, the location_vector in metadata is the authoritative source.
  The ring algorithm is used for NEW writes; existing objects use the stored location_vector.

Node addition:
  - New node inserted at its 128 vnode ring positions
  - Background job migrates keys from adjacent successors to new node
  - Data movement: 1/N fraction of total data (where N = node count)

Node removal / failure:
  - Node's 128 vnodes removed from ring
  - Objects on failed node reconstructed from parity shards (erasure coding)
  - Reconstructed shards written to new nodes as determined by updated ring
```

**Interviewer Q&As:**

Q1: How do you handle the placement service becoming a single point of failure?
A1: The consistent hash ring state is not held by a central service — it is a deterministic algorithm applied to the cluster topology map. The cluster topology map (which nodes exist, their health status, ring positions) is stored in ZooKeeper (strongly consistent, replicated across 5 ZK nodes). Every coordinator node locally caches the topology map and recomputes placements independently. A ZooKeeper failure means topology changes cannot be committed, but existing placements remain fully functional. For reads and writes to already-placed objects, the stored location_vector in the metadata index is used — the ring algorithm is only invoked for new writes.

Q2: What happens during a network partition where some storage nodes can't reach others?
A2: S3's write consistency model requires a quorum write. For a 6+3 erasure coded object, the write coordinator requires all 9 shards to be acknowledged before returning success. If fewer than 9 nodes are reachable, the write fails with a 503 (retried by client). This is a CP (Consistency + Partition-tolerance) choice for writes: availability is sacrificed during partition. For reads, the coordinator only needs 6 of 9 shards — any 6 data shards suffice to reconstruct the object. If 3 nodes are partitioned away, reads still succeed as long as at least 6 remain reachable.

Q3: How does the metadata index stay consistent with the physical placement?
A3: The PUT operation is two-phase: (1) write data to storage nodes (erasure coded shards) with a 2-phase commit protocol; (2) commit metadata to the index layer. If step 1 succeeds but step 2 fails (index write fails), the storage shards are orphaned. The system handles this via a garbage collection process: storage nodes maintain a recent-writes log (last 24 hours). A background reconciler cross-references the storage log against the metadata index and cleans up orphaned shards. The client receives a write failure (index commit failed) and retries, resulting in new shards being written (possibly to same or different nodes) with the successful metadata commit.

Q4: How do you prevent hot partitions on the consistent hash ring when a specific bucket has extremely high request rate?
A4: S3's approach (documented in AWS re:Invent talks): (1) **Prefix diversity**: if all keys start with `images/`, all keys hash to a narrow range of the ring. AWS recommends randomizing key prefixes (e.g., include a UUID prefix or reverse the timestamp). Starting in 2018, S3 added automatic request rate scaling per prefix — a single prefix can sustain 3,500 PUT/s and 5,500 GET/s before the system auto-splits the prefix into sub-partitions; (2) **Auto-partition splitting**: when a ring segment exceeds a throughput threshold, the metadata index shard for that key range is automatically split. This is analogous to DynamoDB's adaptive capacity; (3) **Request shaping at API Gateway**: per-account and per-bucket request rate limiting prevents a single noisy neighbor from monopolizing ring segments.

Q5: How does erasure coding reconstruction work when a storage node fails?
A5: With Reed-Solomon 6+3 erasure coding: the object is split into 6 equal data shards; 3 parity shards are computed (each parity shard = XOR-based combination of data shards, per Reed-Solomon math). To reconstruct: any 6 of the 9 shards are sufficient. When a node fails, the background repair service: (1) detects failure via heartbeat monitor; (2) for each object with shards on the failed node: reads 6 of the remaining 8 shards from healthy nodes; (3) runs Reed-Solomon decode to reconstruct all 9 shards; (4) places the reconstructed shard on a healthy replacement node. Repair priority: objects with 2+ failed shards (already degraded) are repaired first (risk of data loss if one more shard fails).

---

### 6.2 Durability: Erasure Coding vs Replication

**Problem it solves:**
At 300 EB of data, the storage overhead cost is enormous. Full 3× replication uses 3× the raw storage (300 EB → 900 EB physical). Erasure coding achieves the same or better durability with far less overhead. The trade-off is read/write amplification (multiple shard reads required for reconstruction vs a single replica read).

**Approaches Comparison:**

| Approach | Durability | Storage Overhead | Read Overhead | Write Overhead | Recovery |
|---|---|---|---|---|---|
| **1× (no redundancy)** | Very low (~99%) | 1× | 1 read | 1 write | Manual; complete loss on failure |
| **3× replication** | 99.999999999% (11 nines) with 3 AZ | 3× | 1 read (nearest replica) | 3 synchronous writes | Simple: use any surviving replica |
| **2× replication + 1 parity** | 11 nines | 2× | 1 read | 2 writes + 1 parity | One failure: read from 2 copies |
| **Reed-Solomon 6+3** | > 11 nines | 1.5× | 6 shard reads (reconstruct) or 1 data shard if healthy | 9 writes (9 shards across 9 nodes) | Can reconstruct from any 6 of 9 |
| **Reed-Solomon 14+3 (Magic Pocket)** | > 11 nines | 1.21× | 14 shard reads or direct | 17 writes | Tolerates 3 simultaneous failures |
| **Reed-Solomon 10+4** | > 11 nines | 1.4× | 10 shard reads | 14 writes | Tolerates 4 failures |

**Selected: Reed-Solomon 6+3 for Standard storage class; 3× replication for high-frequency hot objects (small objects < 128 KB)**

Justification:
- **6+3 chosen over 3× replication** for objects > 128 KB: 1.5× overhead vs 3× overhead → 50% storage cost reduction. At 300 EB scale, this saves 150 EB = approximately $3 billion/year (at $0.02/GB/month). The 9-shard write overhead is acceptable because write throughput is I/O-bound at storage nodes, not network-bound.
- **3× replication for small objects** (< 128 KB): Reed-Solomon overhead per object is a fixed ~3 KB of computation + shard metadata. For a 1 KB object split into 6+3 shards, each shard is ~170 bytes — the shard management overhead exceeds the data overhead. Small objects are replicated 3× to avoid this per-object overhead. In practice, AWS S3 uses this hybrid approach (small objects replicated, large objects erasure coded).
- **Durability calculation (6+3 RS)**: Annual disk failure rate ~1%. P(data loss) = P(4+ of 9 nodes fail within reconstruction window). P(single node failure) = 0.01/365 per day. P(4+ fail before 1 repaired) with MTTR=4hrs ≈ 10^-15 per object per year. This exceeds the 11-nines (10^-11) target by 4 orders of magnitude.

**Implementation of Erasure Coding:**

```python
import pyeclib  # or custom Reed-Solomon implementation

EC_K = 6   # data shards
EC_M = 3   # parity shards
EC_SEGMENT_SIZE = 1024 * 1024  # 1 MB segments for large objects

def write_object(key, data):
    ec_driver = pyeclib.ECDriver(k=EC_K, m=EC_M, ec_type='rs_vand')
    # Pad data to be divisible by EC_K
    padded = data + b'\x00' * ((-len(data)) % EC_K)
    # Encode: returns list of EC_K+EC_M shards of equal size
    shards = ec_driver.encode(padded)
    assert len(shards) == EC_K + EC_M

    # Write each shard to its assigned storage node (determined by placement)
    for i, shard in enumerate(shards):
        node = placement_nodes[i]
        node.write_shard(shard_key=f"{key}:{i}", data=shard)
    return [sha256(s) for s in shards]  # shard checksums for verification

def read_object(key, placement_nodes, size):
    ec_driver = pyeclib.ECDriver(k=EC_K, m=EC_M, ec_type='rs_vand')
    # Try to read all 9 shards; tolerate up to 3 failures
    shards = [None] * (EC_K + EC_M)
    for i, node in enumerate(placement_nodes):
        try:
            shards[i] = node.read_shard(shard_key=f"{key}:{i}")
        except NodeUnavailable:
            pass  # leave as None
    # Reconstruct from any 6 available shards
    reconstructed = ec_driver.decode(shards)
    return reconstructed[:size]  # remove padding
```

**Interviewer Q&As:**

Q1: What is the read amplification penalty of erasure coding vs replication for hot objects?
A1: With 3× replication, a GET reads 1 replica (the nearest). With 6+3 RS, the system reads all 6 data shards in parallel (if no node failures); each shard is 1/6 the object size, so 6 × (size/6) = 1× the data transferred — same as replication for data bytes. However, there's overhead: 9 parallel network connections (vs 1), and the final reassembly step. For very small objects (< 1 KB), this overhead dominates. That's why small objects use 3× replication. For large objects (> 1 MB), the 6 parallel reads are actually faster than a single serial read (parallelism wins), and the per-connection overhead is amortized.

Q2: How does S3 achieve 11 nines of durability? Walk through the math.
A2: S3's durability claim: 99.999999999% (11 nines) means P(data loss) < 10^-11 per object per year. Model: assume annual disk failure rate = 2%, MTTR (mean time to repair = reconstruct shard to new disk) = 4 hours, 6+3 erasure coding. P(a single disk fails) = 0.02/365/24 ≈ 2.28×10^-6 per hour. For data loss, need 4 of 9 disks to fail within the 4-hour MTTR window. Using a Markov chain model (memoryless failures): P(4 failures in 4 hours | 1 failure just occurred) ≈ C(8,3) × (2.28×10^-6)^3 × (4 hours)^3 ≈ 56 × 1.19×10^-17 × 64 ≈ 4.3×10^-14 per 4-hour window. Annualized: ×2190 (4-hour windows/year) ≈ 9.4×10^-11. With geographic replication (3 AZs), multiply by P(all AZ copies failing) → well below 10^-11. In practice, S3 also uses scrubbing (background integrity checks) to proactively repair bit rot before failures compound.

Q3: How does the repair process prioritize which objects to repair first?
A3: Repair priority is based on degradation level: (1) **Critical (1 good shard remaining above threshold)**: objects with only EC_K healthy shards (all parity consumed) — one more failure = data loss. These get immediate high-priority repair. (2) **Warning (1 shard lost)**: objects with 1 shard unavailable — can afford 2 more failures. Repaired within 4 hours. (3) **Degraded (2 shards lost)**: repaired within 1 hour. The repair scheduler maintains a priority queue (min-heap by health score = available_shards - EC_K). Repair workers pull from the highest-priority queue.

Q4: How does Glacier (archival storage) differ in its durability model from S3 Standard?
A4: Glacier uses the same 11-nines durability target but achieves it with different economics: objects are stored on tape or high-density HDD with higher-latency access. Glacier uses deeper erasure coding (e.g., RS 24+8 or similar) to get ultra-low storage overhead (1.33×). The tradeoff is retrieval time (minutes to hours for tape-based storage) vs Standard's milliseconds. Metadata is stored with the same metadata layer as Standard objects; only the storage backend differs. The lifecycle policy engine automates the Standard → Glacier transition: after N days, the metadata's `storage_class` field is updated, and the bytes are migrated from hot HDD nodes to Glacier-class nodes (or tape library).

Q5: What is "bit rot" and how does S3 detect and repair it?
A5: Bit rot is spontaneous data corruption in storage media (cosmic ray bit flips, magnetic degradation). S3 mitigates it via: (1) **Write verification**: when a shard is written, its SHA-256 is stored in the metadata. On every read, the storage node re-computes SHA-256 and compares. (2) **Scrubbing**: a background process continuously reads every shard and verifies checksums — reads every object ~monthly. When checksum mismatch is detected, the corrupt shard is reconstructed from the other 8 shards (6+3 RS allows reconstruction from any 6 good shards). (3) **Storage node-level**: SSDs use ECC (Error Correcting Code) for single-bit flip correction in NAND flash. HDDs have their own sector-level CRC. Multi-layer defense.

---

### 6.3 Presigned URLs & Authentication (Signature V4)

**Problem it solves:**
S3's primary consumers are applications that need to allow end-users to upload or download files directly — without routing bytes through the application backend (which would add latency and cost). Presigned URLs solve this: the backend generates a time-limited, scoped URL that the client uses directly against S3. The backend never sees the bytes. The challenge is security: the URL must be unforgeable without the AWS secret key, yet verifiable by S3 without a central key store lookup on every request.

**Approaches Comparison:**

| Approach | Security | Performance | Scalability |
|---|---|---|---|
| **Proxy all traffic through app** | Full control | High latency; backend bandwidth cost | Does not scale; backend bottleneck |
| **Shared secret (API key)** | Weak: key never expires; any key leak = full access | Fast | Not scalable; key rotation painful |
| **JWT token** | Strong: stateless, signed | Fast | Excellent; stateless verification |
| **AWS Signature V4 (HMAC-SHA256)** | Strong: scoped to specific operation + resource + time window | Fast (pure crypto; no DB lookup) | Excellent; fully stateless verification |
| **OAuth2 token exchange** | Strong: standard protocol | Slightly higher overhead (token introspection) | Good; requires token endpoint |

**Selected: AWS Signature V4 (HMAC-SHA256)**

Justification: S3's actual protocol. SigV4 is stateless (no central key lookup on the S3 verification path), time-scoped (X-Amz-Expires limits validity), operation-scoped (method + canonical URI + canonical query string are signed), and forgery-resistant (HMAC-SHA256 with 256-bit output). Unlike JWT, SigV4 binds the signature to the full request parameters — a URL intercepted in transit cannot be modified without invalidating the signature.

**Implementation Detail:**

```
Signature V4 Construction (for presigned GET URL):

1. Create canonical request:
   canonical_request = "\n".join([
       "GET",                                      # Method
       "/my-bucket/my-key",                        # Canonical URI (URL-encoded)
       "X-Amz-Algorithm=AWS4-HMAC-SHA256&"         # Canonical query string (sorted)
       "X-Amz-Credential=AKID%2F20260409%2Fus-east-1%2Fs3%2Faws4_request&"
       "X-Amz-Date=20260409T120000Z&"
       "X-Amz-Expires=3600&"
       "X-Amz-SignedHeaders=host",
       "host:my-bucket.s3.us-east-1.amazonaws.com",  # Signed headers
       "",                                           # Empty line
       "host",                                       # Header names
       "UNSIGNED-PAYLOAD"                            # Payload hash (for presigned, use this literal)
   ])

2. Create string to sign:
   credential_scope = "20260409/us-east-1/s3/aws4_request"
   string_to_sign = "\n".join([
       "AWS4-HMAC-SHA256",
       "20260409T120000Z",           # X-Amz-Date
       credential_scope,
       sha256(canonical_request)    # hex-encoded
   ])

3. Derive signing key (key derivation function):
   signing_key = HMAC(
       HMAC(
           HMAC(
               HMAC("AWS4" + secret_access_key, "20260409"),
               "us-east-1"
           ),
           "s3"
       ),
       "aws4_request"
   )
   # Key is re-derived per (date, region, service) — changes daily

4. Compute signature:
   signature = HMAC(signing_key, string_to_sign).hexdigest()

5. Append to URL:
   presigned_url = f"https://my-bucket.s3.us-east-1.amazonaws.com/my-key?"
                   f"X-Amz-Algorithm=AWS4-HMAC-SHA256"
                   f"&X-Amz-Credential=AKID%2F{credential_scope}"
                   f"&X-Amz-Date=20260409T120000Z"
                   f"&X-Amz-Expires=3600"
                   f"&X-Amz-SignedHeaders=host"
                   f"&X-Amz-Signature={signature}"

S3 verification (stateless):
  - Extract access key ID from X-Amz-Credential
  - Look up secret key from IAM service (cached locally, ~10ms on cache miss)
  - Re-derive signing key, re-compute canonical request from URL, re-compute signature
  - Compare computed signature to X-Amz-Signature → match: proceed; mismatch: 403
  - Check X-Amz-Expires: if current_time > X-Amz-Date + X-Amz-Expires → 403 ExpiredToken
  - Check IAM policy: does this access key have s3:GetObject on this resource?
```

**Interviewer Q&As:**

Q1: Can a presigned URL be used to upload an arbitrarily large file, or can the uploader change the Content-Type?
A1: No — the signed headers are explicit. For a presigned PUT URL, the Content-Type and Content-Length-Range (via POST policy) are typically included in the signed headers. If the uploader changes Content-Type, S3 recomputes the canonical request with the new value and the signature won't match (403 SignatureDoesNotMatch). For browser upload via POST policy, the backend specifies exact conditions in the policy document (max file size, allowed MIME types, key prefix) — S3 enforces these conditions server-side.

Q2: How are presigned URLs revoked? The signing key can't be revoked per-URL.
A2: Presigned URL revocation options: (1) **Short expiry**: set X-Amz-Expires to a small value (e.g., 300 seconds for a download, 3600 for an upload). Expired URLs automatically stop working. (2) **Credential rotation**: the URL is bound to the access key ID. Rotating (deleting) the access key immediately invalidates all presigned URLs signed with that key. (3) **Bucket policy deny**: add a bucket policy condition that denies requests with a specific condition (e.g., `aws:SourceIP not in approved_ranges`) — this can effectively block URLs even if not expired. (4) **Block public access**: if the bucket has block public access enabled, presigned URLs from non-bucket-owner IAM principals are rejected. True per-URL revocation is not supported without a URL tracking database, which breaks the stateless model.

Q3: How does S3 handle clock skew between the client and S3's servers?
A3: SigV4 includes a timestamp in X-Amz-Date. S3 allows a ±15-minute clock skew window. If the request timestamp differs from S3's system clock by more than 15 minutes, S3 rejects with `RequestTimeTooSkewed`. This prevents replay attacks: an attacker intercepting a signed request cannot replay it more than 15 minutes later (for regular signed requests). For presigned URLs, the window is the full X-Amz-Expires duration — they're designed for asynchronous use over longer periods, so the 15-minute window doesn't apply to presigned URLs; instead, the expiry time is the binding constraint.

---

## 7. Scaling

### Horizontal Scaling

- **Frontend (API Gateway) Tier**: Stateless; scale horizontally by region. Route 53 Anycast routes requests to the nearest regional fleet. 100s of API servers per region.
- **Metadata Index Shards**: Each shard is a leader + 2 followers (Raft-based replication). New shards added by splitting high-traffic existing shards (range split at median key). Number of shards: scale from thousands to millions as object count grows.
- **Storage Nodes**: Add new nodes; consistent hash ring automatically starts routing new writes to the new node. Existing data migrates in the background.
- **Placement Service**: Stateless (computes placement from in-memory ring state); scales trivially.

### DB Sharding

Object metadata is sharded by `hash(bucket + key)` using consistent hashing. Key insight: this distribution is even across keys but can be hot for specific buckets with high request rate. S3 mitigates bucket-level hotness by sub-partitioning by key prefix within a bucket: if `images/` prefix gets 10,000 GETs/second and the shard's capacity is 5,000/second, the shard is automatically split at `images/m` (the median key), spreading load across 2 shards. This split is recorded in a routing table overlay on top of consistent hashing.

### Replication

- Within a region: 6+3 erasure coded across 9 nodes in different racks/AZs. Synchronous write (all 9 shards acked before returning success). RPO = 0. RTO = minutes (repair in background).
- Cross-region (CRR): Asynchronous. After successful PUT, an event is published to an internal replication queue. Replication worker reads object, makes PUT to destination region. Typical replication lag: < 15 minutes for 99% of objects. Not suitable as primary RPO=0 DR mechanism alone.
- `x-amz-replication-status` header shows: PENDING (queued), COMPLETED, FAILED, REPLICA (this is the copy).

### Caching Strategy

| Layer | What | TTL | Notes |
|---|---|---|---|
| API server in-process | IAM policy evaluation result | 5 min | Refreshed on IAM policy update event |
| API server in-process | Bucket metadata (versioning, lifecycle, policy) | 60 s | Low write rate; safe to cache |
| Metadata shard (RocksDB block cache) | Hot object metadata pages | Eviction-based | Prevents disk I/O for hot keys |
| Storage node (page cache) | Hot shard data (OS page cache on Linux) | OS-managed | Small objects fully in RAM; large objects streaming |
| CloudFront (CDN) | Object bytes for public/presigned URLs | Configured per bucket | Cache-Control header from object metadata |

### CDN Strategy

CloudFront is S3's CDN. S3 serves as origin. Cache key: bucket + key (+ version ID for versioned objects). When CloudFront misses: fetches from S3 origin. Invalidation: explicit via CloudFront API (path-level); or natural expiry via Cache-Control max-age. S3 Transfer Acceleration: uses CloudFront's edge PoPs to accelerate PUT operations — client PUTs to nearest edge PoP, which routes over AWS private backbone (faster than public internet) to the destination S3 region.

### Interviewer Q&As

Q1: How does S3 handle the "hot key" problem where millions of requests per second all access the same object (e.g., a viral video)?
A1: For GET-heavy hot objects: (1) CloudFront CDN absorbs the vast majority of traffic; the S3 origin only sees CDN misses. A properly configured CDN (Cache-Control: max-age=3600) means only 1 request per hour per CDN edge PoP hits S3 origin; (2) for requests that do hit S3 origin, the object's shards are spread across 9 storage nodes — reads are parallelized across all 9 nodes (or whichever 6 data shards are needed for reconstruction). A single hot object cannot bottleneck more than 9 storage nodes. S3 has published that popular objects see automatic replication to additional nodes for load distribution.

Q2: How does ListObjects scale to handle buckets with trillions of objects?
A2: ListObjects with prefix filtering is implemented as a range scan on the sorted metadata index. The metadata shards store keys in lexicographic order (RocksDB's sorted key ordering). A list request with prefix `images/` becomes: `seek(f"{bucket}:{prefix}")`, then iterate forward until key no longer starts with the prefix. This scan is bounded by the page size (max 1000 keys per response). For very large result sets, the continuation token encodes the last key seen, allowing the next request to resume from exactly that position. The scan is distributed across multiple shards if the prefix spans shard boundaries — parallel sub-scans merged at the API layer.

Q3: How does S3 ensure strong read-after-write consistency? What changed from eventual consistency?
A3: Before November 2020, S3 used an eventual consistency model: a PUT to an existing key might read stale data immediately after. The root cause was a cache layer between the metadata index and API servers — a read might serve a stale cached version. Post-2020, AWS implemented two changes: (1) The metadata index now uses a linearizable store — writes are synchronously replicated before ack; (2) The API server's local cache was made write-through + invalidation-consistent: on a PUT to key K, all API server caches for K are synchronously invalidated before the PUT response is returned. This guarantees that any subsequent GET to K (on any API server) reads from the metadata index, not a stale cache.

Q4: How would you implement S3's automatic storage class tiering (lifecycle transitions)?
A4: Lifecycle Manager runs nightly batch jobs per bucket. For each active lifecycle rule: (1) scan the bucket's metadata index for objects matching rule's filter (prefix, tags, size conditions); (2) for objects where `age > transition_days`: enqueue a transition job; (3) transition workers: copy object bytes from Standard storage nodes to Standard-IA / Glacier backend; update metadata record's `storage_class` field; decrement reference on Standard storage nodes; (4) atomic metadata commit: the storage_class update and storage reference change are committed in a single metadata transaction, preventing split-brain. Idempotent: re-running transition on already-transitioned object is a no-op (detected by checking current storage_class).

Q5: How does S3's multipart upload enable parallel uploads, and how does CompleteMultipartUpload work atomically?
A5: Multipart upload: (1) each part is stored as an independent, incomplete object in a staging area (separate from the main namespace); (2) parts can be uploaded in any order and in parallel by multiple threads/machines (e.g., a 100 GB file split into 100 × 1 GB parts, uploaded by 10 parallel clients simultaneously → 10× throughput); (3) `CompleteMultipartUpload` takes the ordered list of (part_number, ETag) pairs, validates all parts exist and their ETags match, and atomically: computes the final ETag (MD5 of MD5s: `MD5(MD5_1 + MD5_2 + ... + MD5_N)-N`), creates a new metadata record pointing to all parts' storage locations as a logical chain, and removes the parts from staging. The final object is not physically assembled — it is stored as a manifest pointing to its constituent part shards. On GET, the object is streamed by reading parts in order.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Storage node failure (hardware) | 1 shard of affected objects unavailable | Heartbeat miss (30 s); RAID controller alert | Objects still readable (6+3: can reconstruct from 6 of remaining 8); background repair writes missing shard to new node |
| Metadata shard leader failure | Reads/writes to that shard fail | Raft heartbeat miss; latency spike | Raft leader election in < 5 s; followers promote; temporary 503 on affected keys |
| Network partition between AZs | Cross-AZ shard writes fail | Internal health checks | Each AZ can read locally (3+ shards per AZ in 3-AZ placement); writes quorum may fail → retry |
| Corrupt metadata record | GET returns wrong data or 500 | Checksum mismatch on shard read | Metadata validated with CRC32 on every read; corrupt record triggers repair from replica metadata shards |
| Multipart upload left incomplete | Orphaned part shards consume storage | n/a (background audit) | Lifecycle rule: `AbortIncompleteMultipartUploads` after N days automatically cleans up; also manual abort API |
| Presigned URL used after expiry | 403 response to user | n/a | Client should generate new presigned URL; application should implement retry logic |
| Region-wide failure | Full service unavailability in region | Global health checks | CRR-replicated data accessible in backup region; DNS failover to backup region; RTO varies by application |
| IAM service unavailability | All authenticated requests fail | Health check | API servers cache IAM policy evaluations (5 min TTL); short outages handled by cache; extended outage → degrade to deny-all or allow-cached |

### Retries & Idempotency

- **PUT Object**: Idempotent by definition (same key + same bytes = same result; versioning creates a new version but that's correct per-design). Client SDKs retry on 5xx with exponential backoff + jitter. AWS SDK default: 3 retries, 0.5-3 s delays.
- **DELETE Object**: Idempotent (deleting a non-existent key returns 204 — no error).
- **Multipart Upload Part**: Idempotent (PUT same part_number with same bytes returns same ETag; duplicate upload just overwrites the staged part).
- **CompleteMultipartUpload**: Not idempotent — calling twice with same upload_id returns 200 on first call, 404 (NoSuchUpload) on second call (upload already removed from staging). Client must handle this gracefully.
- Internal RPCs use idempotency keys (UUID generated at the API layer, propagated to storage and metadata layers). Retry of internal operations checks for completed flag before re-executing.

### Circuit Breaker

Each API server maintains circuit breakers per downstream: Metadata Index, Storage Layer, IAM Cache. Thresholds (configurable): open at 10% error rate over 30s, or p99 latency > 5× baseline. Half-open probing after 30s with 1 test request. Customers see: HTTP 503 with `Retry-After: 5` during open circuit state.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Tool |
|---|---|---|---|
| PUT success rate (per region) | Counter ratio | < 99.9% / 1 min | CloudWatch |
| GET success rate (per region) | Counter ratio | < 99.95% / 1 min | CloudWatch |
| PUT p99 latency | Histogram | > 500 ms | CloudWatch |
| GET p99 latency | Histogram | > 200 ms | CloudWatch |
| Metadata shard leader election events | Counter | > 1/hour per shard | CloudWatch Alarm |
| Storage node disk fill % | Gauge | > 80% | Internal health dashboard |
| Erasure coding repair queue depth | Gauge | > 10,000 objects | PagerDuty P2 |
| Objects with degraded durability (< 7 of 9 shards) | Gauge | > 0 | PagerDuty P1 |
| CRR replication lag (p99) | Histogram | > 15 min | CloudWatch |
| Request signature verification failures | Counter | Spike > 1000/min | Security alert |

### Distributed Tracing

AWS X-Ray integration: every S3 API request creates an X-Ray trace. Spans: RequestRouter (auth + routing), IndexLookup (metadata read), StorageRead/Write (per shard), IAMEval. Sampling: 5% of all requests; 100% of requests with > 5 s latency; 100% of 5xx responses. Used to identify slow metadata shards (hotspot detection) and stragglers in erasure coded writes (tail latency caused by a slow storage node).

### Logging

- **S3 Server Access Logs**: Optional per-bucket logging. Fields: requester, bucket, key, request-URI, status, error code, bytes-sent, time, referrer, user-agent, version-id, host-id. Delivered to a logging destination bucket. Used for audit and analytics.
- **AWS CloudTrail**: Management-plane API calls (CreateBucket, PutBucketPolicy, etc.) automatically logged. Data-plane events (GetObject, PutObject) optionally logged (high volume; priced separately). 90-day CloudTrail event history; archive to S3 for longer retention.
- **Internal structured logs**: JSON with trace_id, account_id (masked), bucket, key_hash (SHA256 of key for privacy), operation, duration_ms, shard_count, erasure_coding_recovery. Log to internal Splunk/Elasticsearch.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|---|---|---|---|
| Durability mechanism | Erasure coding 6+3 (Standard) / 3× replication (small objects) | Pure 3× replication for all objects | 50% storage cost savings at 300 EB scale; same 11-nines durability |
| Object placement | Consistent hashing + vnodes + topology-aware rules | Central directory | Avoids billion-entry lookup table; purely computational placement |
| Metadata store | Custom distributed KV (RocksDB per shard) | DynamoDB / Cassandra | Sorted key iteration required for ListObjects; complete control over compaction and caching |
| Read consistency | Strong (linearizable) read-after-write | Eventual consistency | Eliminated an entire class of user bugs (reading stale data after write) |
| Auth mechanism | Signature V4 (HMAC-SHA256) | OAuth2/JWT | Stateless verification; no central token store lookup on hot path |
| Presigned URL scope | Bound to specific method + resource + time | Broad token (any action) | Principle of least privilege; prevents scope creep in URL sharing |
| Multipart assembly | Manifest-based (logical chain of parts) | Physical reassembly into single object | Avoids copying 5 GB at CompleteMultipartUpload time; read path handles assembly transparently |
| Small object storage | 3× replication (< 128 KB) | Erasure coding for all sizes | EC per-object overhead exceeds data size for tiny objects |
| Versioning model | Append-only versions with delete markers | In-place overwrite | Immutability: enables compliance (WORM), easy rollback, no data loss on accidental overwrite |
| Lifecycle transitions | Async background job | Synchronous on-write | Write hot path not blocked by lifecycle logic; lifecycle is best-effort within SLA |

---

## 11. Follow-up Interview Questions

Q1: How would you implement S3 Object Lock (WORM — Write Once Read Many)?
A1: Object Lock prevents deletion or overwriting of objects for a specified period. Two modes: GOVERNANCE (privileged users with `s3:BypassGovernanceRetention` can override) and COMPLIANCE (no one, not even root, can delete). Implementation: when an object is PUT with Object Lock metadata, the metadata record includes `lock_mode` and `lock_retain_until`. DELETE and overwrite requests check these fields: if `lock_mode=COMPLIANCE` and `current_time < lock_retain_until`, reject with HTTP 403 (`ObjectLockRetentionPeriodError`). This check happens in the metadata commit path, before any physical deletion. The compliance guarantee requires that the underlying metadata store itself prevents retroactive modification — achieved via an append-only metadata log where lock records cannot be overwritten.

Q2: How does S3 Select work — querying data within objects without downloading them?
A2: S3 Select pushes down a SQL-like query (SELECT, WHERE, LIMIT on CSV/JSON/Parquet) to the storage layer. Instead of the API server downloading the full object and filtering, the storage node itself runs a lightweight query engine on the raw bytes. Only matching rows are returned to the API server and client. Implementation: when the API server receives a SelectObjectContent request, it routes to the storage nodes holding the object's shards; each storage node runs the query engine on its data shard and returns only matching records. Results are assembled and streamed to the client. For Parquet, column pruning eliminates reading non-referenced columns entirely. Bandwidth savings: 99% reduction for selective queries on large datasets.

Q3: How would you implement cross-account access to an S3 bucket?
A3: S3 resource-based policies (bucket policies) allow cross-account access. A bucket policy can specify: `"Principal": {"AWS": "arn:aws:iam::ACCOUNT_B_ID:root"}` with `"Effect": "Allow"`. When Account B's IAM identity calls GetObject on Account A's bucket: (1) API server evaluates Account B's IAM identity policy (must have `s3:GetObject`); (2) API server evaluates Account A's bucket policy (must also allow Account B). **Both** the requester's identity policy and the resource's bucket policy must allow the action. This dual-evaluation prevents a compromised account from accessing other accounts' buckets via their own IAM policies alone.

Q4: How does S3 handle concurrent writes to the same key?
A4: S3 uses a last-writer-wins model for non-versioned buckets: the last PUT to arrive at the metadata index wins. For very closely timed concurrent PUTs, S3 makes no ordering guarantee — one of the two writes will be the final state, but which one is non-deterministic. With versioning enabled, both PUTs create new versions (different version IDs); no data is lost — the "winner" is just the version returned by default on GET (highest version ID = latest). Applications requiring atomic conditional writes can use the `If-None-Match: *` header (only write if key doesn't exist) or rely on object versioning + conditional copy operations.

Q5: Explain the data flow for a 10 GB multipart upload with 100 parallel parts.
A5: (1) `CreateMultipartUpload` → upload_id returned; (2) 100 clients each upload 100 MB parts (parts 1–100) in parallel to S3 API servers. Each API server: authenticates, determines placement for that part via consistent hashing (using `upload_id:part_number` as key), streams 100 MB to 9 storage nodes (erasure coded); returns ETag = MD5(part_bytes); (3) Final client assembles part ETags, calls `CompleteMultipartUpload` with ordered list; (4) API server: verifies all 100 parts' ETags match stored values; creates final object metadata record as a manifest (part_1_location, part_2_location, ..., part_100_location); atomically commits to metadata index; (5) Returns 200 with final ETag = MD5(MD5_1+...+MD5_100)+"-100". The final 10 GB is now stored as 100 independent 100 MB erasure-coded objects linked by a manifest. No reassembly copy occurs.

Q6: How does S3 handle object encryption with customer-provided keys (SSE-C)?
A6: SSE-C: the client provides the encryption key in the request header (base64-encoded 256-bit AES key). S3 uses this key to encrypt the object bytes before writing to storage nodes, then **immediately discards the key** — S3 never stores the customer's key. S3 stores only an HMAC of the key (for verification on subsequent GET requests). On GET, the client must provide the same key again; S3 computes HMAC and compares to stored value to verify the correct key was provided, then decrypts. If the client loses the key, the object is permanently unrecoverable. SSE-C shifts the key management burden entirely to the customer. It differs from SSE-KMS (where AWS KMS stores and manages the key) in that there is zero AWS key custody.

Q7: How would you design S3 Inventory — generating a daily list of all objects in a bucket?
A7: S3 Inventory generates a flat file (CSV/ORC/Parquet) listing all objects in a bucket with configurable metadata fields. Implementation: (1) A daily job is triggered per bucket with inventory enabled; (2) The job performs a full sorted scan of the bucket's metadata shards (similar to ListObjects but unbounded — no pagination limit); (3) For efficiency, each metadata shard generates its own partial inventory file (sorted by key); (4) The partial files are merged-sorted (external sort merge, since total may be billions of rows); (5) The merged file is written to the inventory destination bucket in the specified format; (6) A manifest JSON file is written alongside, listing the inventory file's location, schema, and creation time. Total time: depends on bucket size — for buckets with 100 billion objects, this may take several hours, which is why daily (not real-time) is the finest granularity.

Q8: How does S3 handle bucket namespace management? Can two accounts create a bucket with the same name?
A8: Bucket names are **globally unique** across all AWS accounts and regions. Implementation: bucket names are registered in a global atomic namespace service (similar to a distributed DNS registry — likely DynamoDB Global Tables or similar). `CreateBucket` first checks global name availability, then atomically claims the name. If two accounts simultaneously try to create `my-bucket`: one wins (gets HTTP 200), the other loses (HTTP 409 BucketAlreadyExists). The global namespace service is strongly consistent (linearizable) to prevent two accounts from claiming the same bucket name. This global uniqueness simplifies DNS: `my-bucket.s3.amazonaws.com` unambiguously resolves to one bucket.

Q9: What is S3's approach to eventual consistency in replicated metadata, and how was it fixed?
A9: Pre-2020: S3 used a distributed metadata cache where individual API servers cached object metadata locally with a TTL. This enabled the infamous "read-after-write" inconsistency: you PUT object K, but a subsequent GET from a different API server returned 404 (cache still had a "not exists" entry). The fix (2020): S3's metadata index moved to a strongly consistent replicated log (Raft-based or equivalent). All API servers now query the authoritative leader for metadata reads, or use a coherent invalidation protocol. The local cache was made write-through: on PUT, all API servers flush their cache entry for key K before returning success to the client. This guarantees any subsequent GET, even on a different API server, reads the updated metadata.

Q10: How does S3 implement server-side copy (CopyObject) efficiently?
A10: CopyObject is a server-side copy — bytes never travel to/from the client. Implementation: (1) Source object metadata read from metadata index (get location_vector); (2) Destination placement determined (same or different bucket/region); (3) Storage-layer copy: coordinator reads all 9 source shards in parallel, re-encodes and writes 9 new destination shards. For same-region copy: optimized by reading source shard bytes and directly writing to destination storage nodes (avoiding full decode/re-encode for same storage class); (4) New metadata record created for destination key; (5) Response returned to client with new ETag and version ID. For cross-region copy: source region API server streams the decoded object bytes to the destination region API server, which re-encodes and writes locally. Bandwidth is AWS backbone, not public internet.

Q11: How would you implement S3 Object Tagging and use it in lifecycle policies?
A11: Object tags are stored as key-value pairs (max 10, total 2 KB) in the object's metadata record. Tags are indexed separately for tag-based lifecycle filtering: a secondary index `(bucket, tag_key, tag_value) → set<object_key>` enables efficient queries like "find all objects in bucket X with tag env=production." Lifecycle rules with tag-based filters query this secondary index during the nightly lifecycle scan. Tags can be updated independently of the object content (`PutObjectTagging`, `DeleteObjectTagging`) — these operations only update the metadata, not the physical bytes, and create entries in the change log for lifecycle re-evaluation. Tag updates are charged separately (minor operation) and propagated to the tag index within 24 hours.

Q12: How does S3 prevent billing for unauthorized data transfer (data exfiltration)?
A12: Multiple controls: (1) Bucket policies and IAM policies restrict who can call GetObject — unauthorized requesters get 403 (no data transferred, no charge); (2) VPC Endpoint policies: in a VPC, an S3 endpoint policy can restrict access to specific buckets (prevent data exfiltration to attacker-controlled buckets); (3) `aws:SourceVpc` and `aws:SourceVpce` conditions in bucket policies restrict access to traffic originating from specific VPCs; (4) CloudTrail logging of all data plane events (GetObject) enables post-hoc detection of suspicious access patterns (high-volume downloads from unusual IPs); (5) Macie (ML-based data discovery) can detect sensitive data being stored or exfiltrated. Billing for data transfer is tied to the AWS account responsible for the request (requestor pays configuration can shift charges to the downloader).

Q13: How would you design the delete flow to ensure durability guarantees even after deletion?
A13: Soft delete flow for versioned buckets: DELETE creates a delete marker (a metadata-only record, no bytes, marking the key as logically deleted). All previous version bytes remain intact. Hard delete (specifying a version ID) removes: (1) metadata record for that version; (2) decrements storage shard ref_counts; (3) when ref_count reaches 0 (no other version references those shards), shards are scheduled for garbage collection after a 7-day grace period. The 7-day grace period allows: (a) admin accidentally deleting to restore; (b) in-flight reads to complete; (c) CRR to propagate the deletion to replicas. Physical shard deletion is a low-priority background job (does not affect performance of active reads/writes).

Q14: Explain S3 Transfer Acceleration and when you'd use it.
A14: Transfer Acceleration routes upload/download traffic through CloudFront edge PoPs instead of directly to the S3 regional endpoint. Client → nearest CloudFront PoP (low latency over public internet, geographically close) → CloudFront PoP → S3 region endpoint (over AWS private backbone, ultra-low latency). Benefit: for users far from the S3 region (e.g., user in Tokyo uploading to us-east-1), the public internet portion is minimized to the nearest PoP; the long-haul is over AWS's private network (lower loss, lower latency, higher bandwidth than public internet). Use when: (1) global users uploading to a single centralized bucket; (2) large objects (> 1 GB) where even a 20% throughput improvement matters; (3) don't use for S3 buckets and users in the same region (no benefit; adds overhead).

Q15: How would you build an audit system that proves a specific object existed at a specific point in time?
A15: S3 Object Lock + CloudTrail provides audit-grade proof: (1) Object Lock in COMPLIANCE mode with a long retain_until date makes the object immutable and undeletable; (2) CloudTrail (with data event logging enabled) creates a tamper-evident log entry for every PutObject, including the ETag (MD5 of content) and timestamp; (3) CloudTrail logs are themselves stored in S3 with Object Lock (immutable audit trail); (4) S3 inventory snapshots (daily) provide point-in-time listings of all objects with their ETags; (5) For legal-grade proof: CloudTrail events can be shipped to an immutable log service (AWS QLDB or equivalent) that provides cryptographic proof of integrity (hash chaining). Combination of Object Lock + CloudTrail + QLDB provides: proof that object K with content-hash H existed at time T, and that it has not been modified since.

---

## 12. References & Further Reading

- **Amazon S3 Documentation** — https://docs.aws.amazon.com/s3/index.html
- **AWS Signature Version 4 Signing Process** — https://docs.aws.amazon.com/general/latest/gr/signature-version-4.html
- **Amazon S3 Strong Consistency (2020 announcement)** — https://aws.amazon.com/blogs/aws/amazon-s3-update-strong-read-after-write-consistency/
- **Dynamo: Amazon's Highly Available Key-Value Store** — DeCandia et al., SOSP 2007. https://www.allthingsdistributed.com/files/amazon-dynamo-sosp2007.pdf (foundational for consistent hashing used in S3)
- **Erasure Coding in Windows Azure Storage** — Huang et al., USENIX ATC 2012. https://www.usenix.org/conference/atc12/technical-sessions/presentation/huang
- **Reed-Solomon Codes for Cloud Storage** — Plank et al. http://web.eecs.utk.edu/~jplank/plank/papers/
- **Raft Consensus Algorithm** — Ongaro and Ousterhout, USENIX ATC 2014. https://raft.github.io/
- **Consistent Hashing and Random Trees** — Karger et al., STOC 1997. https://dl.acm.org/doi/10.1145/258533.258660
- **CRUSH: Controlled, Scalable, Decentralized Placement of Replicated Data** — Weil et al., SC 2006. https://ceph.com/assets/pdfs/weil-crush-sc06.pdf
- **S3 Multipart Upload documentation** — https://docs.aws.amazon.com/AmazonS3/latest/userguide/mpuoverview.html
- **S3 Object Lock (WORM) documentation** — https://docs.aws.amazon.com/AmazonS3/latest/userguide/object-lock.html
- **AWS re:Invent 2019: Best Practices for Amazon S3** (video) — https://www.youtube.com/watch?v=N_3IaOVcIO0
- **Designing Data-Intensive Applications** — Martin Kleppmann, O'Reilly, 2017. Chapters 3, 5, 6.
