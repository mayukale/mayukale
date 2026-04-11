# Common Patterns — Storage (06_storage)

## Common Components

### Content-Addressed Blob Store (SHA-256 as Key)
- All four problems key blobs by their SHA-256 hash, making storage inherently deduplicated
- In google_drive: 256 KB–4 MB chunks stored in GCS/Colossus; key = SHA-256(chunk_bytes); dedup check is `GET /chunks/{sha256}` before upload
- In dropbox: 4 MB fixed blocks stored in S3/Magic Pocket; key = SHA-256(block); Block Server manages reference counts per hash
- In s3_object_store: ETag = SHA-256 (or MD5) of stored content; objects stored by placement service, not CAS-keyed by hash — but object identity and integrity tracked via ETag
- In photo_storage: blob store key = sha256(photo_bytes); dedup check before writing to GCS/S3/Custom CAS store

### Separation of Metadata Layer from Blob/Storage Layer
- All four explicitly separate the fast, small, transactional metadata plane from the large, bulk blob storage plane
- In google_drive: Metadata Service (Spanner — globally consistent) vs. Blob Store (GCS/Colossus — content-addressed)
- In dropbox: Metadata Server (MySQL sharded) vs. Block Store (S3/Magic Pocket); metadata ops are tiny + strong consistency; block ops are large + eventual
- In s3_object_store: Index/Metadata Layer (distributed hash table, strong consistency) vs. Storage Layer (storage nodes with erasure-coded shards)
- In photo_storage: Photo Metadata DB (Cassandra) vs. Content-Addressed Blob Store (GCS/S3/Custom); search index (Elasticsearch) is a third layer

### Presigned URLs for Direct Client-to-Storage Access
- All four generate time-limited presigned URLs so clients upload/download directly to object storage without proxying bytes through API servers
- In google_drive: Google Resumable Upload Protocol; session tokens valid 24h; chunk PUT directly to GCS
- In dropbox: Block Server generates presigned S3 PUT URLs for upload; clients upload blocks directly to S3/Magic Pocket
- In s3_object_store: core product feature — HMAC-signed URLs allowing unauthenticated access to a specific object for a time-limited window
- In photo_storage: Read/Delivery Service generates presigned CDN URLs for photo download; upload also uses presigned PUT

### CDN for Photo/File Delivery
- All four cache immutable blobs at CDN edge PoPs; cache key based on SHA-256 or object key
- In google_drive: Cloud CDN / Cloudflare; cache key = `(sha256_hash, byte_range)`; public/shared files served from edge; segments cached long-TTL (immutable)
- In dropbox: AWS CloudFront; reduced origin egress for popular shared files
- In s3_object_store: CloudFront integration; ~80% CDN offload assumed; reduces origin GET to ~9 Tbps at peak
- In photo_storage: Cloudflare / Cloud CDN; thumbnails cached at edge with immutable URLs (SHA-256-based path); CDN hit rate ~90%; p99 thumbnail delivery < 30 ms CDN hit

### Kafka for Async Post-Upload Event Pipeline
- All four publish an event to Kafka (or equivalent) after a file is durably committed, triggering async downstream processing
- In google_drive: Kafka `file.uploaded` topic → virus scanner, thumbnail/preview generator, search indexer, notification fan-out
- In dropbox: Kafka change events → notification server fan-out; downstream analytics pipeline
- In s3_object_store: transactional outbox pattern — notification record written atomically with metadata; worker publishes to SNS/SQS/Lambda; event types: `s3:ObjectCreated`, `s3:ObjectDeleted`, etc.
- In photo_storage: Kafka `photo.uploaded` topic → Thumbnail Generator, Face Detection, Scene Labeling, EXIF Extractor, Geo Tagger (all in parallel)

### Redis for Hot-Path Metadata Caching
- All four use Redis for frequently-accessed hot state that would otherwise hit the primary metadata DB on every request
- In google_drive: ACL cache (60 s TTL), quota cache (30 s TTL), deferred quota accounting via Redis accumulators (avoid per-operation Spanner write)
- In dropbox: session tokens, rate limit buckets, LAN peer registry (Redis Sets per user)
- In s3_object_store: IAM policy cache refreshed asynchronously (avoids calling IAM service per request); per-account/per-bucket rate limit counters
- In photo_storage: hot photo metadata (recently viewed, recently uploaded), ACL check results (permission cache per user-photo pair)

### Async Post-Upload Processing Workers
- All four offload heavy processing (thumbnail generation, virus scanning, ML inference) to async worker pools consuming the Kafka event stream
- In google_drive: virus scan (ClamAV + ML scanner), thumbnail/preview generator, full-text search indexer (OCR for images/PDFs)
- In dropbox: virus scanner (async), preview generator (images, PDFs, code)
- In s3_object_store: lifecycle manager (periodic transitions Standard → Glacier → Deep Archive), cross-region replication (CRR), event notification publisher
- In photo_storage: Thumbnail Generator (libvips, 3 variants: 64×64 / 360×360 / 1080p), Face Detection (MTCNN + FaceNet), Scene Labeling (ResNet-50 / ViT), EXIF Extractor (ExifTool / libexif), Geo Tagger (GPS → place name reverse geocode), Face Clusterer (DBSCAN / ANN)

### Resumable / Multipart Upload Protocol
- All four support uploading large files in chunks to handle network interruptions gracefully
- In google_drive: Google Resumable Upload Protocol — client initiates session (`POST /upload/initiate`), receives `sessionId`; uploads chunks with `PUT /upload/{sessionId}/chunk/{index}`; server tracks received chunks
- In dropbox: Block-level protocol — client uploads each 4 MB block separately via presigned PUT URLs; blocks are individually atomic; resume = re-identify which blocks are missing
- In s3_object_store: Multipart Upload API — up to 10,000 parts (5 MB–5 GB each); parallel or sequential part upload; atomic `CompleteMultipartUpload` assembles final object; manifest-based assembly
- In photo_storage: multipart upload for large RAW files (up to 50 MB); presigned multipart PUT URLs; upload service commits metadata after all parts received

### Soft Delete + Versioning
- All four preserve deleted/overwritten files for a configurable retention window before physical deletion
- In google_drive: files moved to Trash on delete; retained 30 days; last 100 versions retained (30 days for free tier, unlimited for paid)
- In dropbox: conflicted copies rather than data loss on concurrent edits; file versions retained 180 days (Plus) or unlimited (Business Advanced)
- In s3_object_store: versioning enabled per bucket; DELETE creates a delete marker (version is preserved); hard delete requires specifying version ID; Lifecycle policies expire delete markers and non-current versions
- In photo_storage: photos moved to Trash on delete, auto-purged after 60 days

### Strong Consistency for Metadata; Eventual for Blob Bytes
- All four separate consistency guarantees by layer: metadata writes are strongly consistent; blob data delivery is eventually consistent (CDN caching acceptable)
- In google_drive: Spanner provides external consistency (TrueTime-based) for all metadata; GCS blobs are eventually consistent for CDN serving
- In dropbox: MySQL with synchronous replication for metadata (strong consistency); S3 blocks are eventually consistent once uploaded
- In s3_object_store: strong read-after-write consistency since Nov 2020 (for both object data and metadata); list operations are strongly consistent
- In photo_storage: Cassandra with QUORUM writes for photo metadata (strong consistency within a DC); CDN serves stale bytes for up to TTL (eventual)

## Common Databases

### Relational DB for Metadata (MySQL / PostgreSQL / Spanner)
- All four store file/object metadata in a relational or relational-like strongly-consistent store
- In google_drive: Spanner (globally distributed, externally consistent) for file hierarchy, permissions, versions, quotas
- In dropbox: MySQL sharded by namespace_id (64 shards); ZooKeeper for shard routing and leader election; read replicas for read offload
- In s3_object_store: custom distributed hash table (DynamoDB-like, sharded by consistent hash of bucket+key) for object index; ZooKeeper for bucket metadata
- In photo_storage: Cassandra for photo timeline (partition by user_id, cluster by upload_time DESC, TWCS); PostgreSQL for albums, ACL, face labels

### Object Storage (S3 / GCS / Magic Pocket)
- All four use S3-compatible object storage as the physical blob store
- In google_drive: GCS (Google Cloud Storage) or internal Colossus distributed file system
- In dropbox: AWS S3 (external) + Magic Pocket (Dropbox's proprietary block storage; erasure coding 14+3 = 17 shards; 90% cost reduction vs S3)
- In s3_object_store: the system itself; custom storage nodes with local RocksDB-like KV + erasure coding 6+3 Reed-Solomon
- In photo_storage: GCS / S3 / Custom CAS store (content-addressed, SHA-256 keyed)

### Cache (Memcached / Redis)
- All four add a cache tier to absorb read traffic from the metadata DB
- In google_drive: Redis for ACL + quota caches; Spanner's built-in read cache
- In dropbox: Memcached for 90% metadata read offload (file metadata, namespace state, dedup lookups); Redis for session tokens + rate limits
- In s3_object_store: IAM policy local cache on API Gateway tier; per-prefix rate limit counters in Redis
- In photo_storage: Redis for hot photo metadata + ACL check cache

## Common Queues / Event Streams

### Kafka (Post-Upload Event Bus)
- All four use Kafka as the durable event bus connecting the upload commit point to downstream async workers
- See Common Components above

## Common Communication Patterns

### Separation of API Plane and Data Plane
- All four route metadata API requests through application servers (rate limiting, auth, quota checks) but deliver/receive blob bytes directly from object storage via presigned URLs
- API servers never proxy blob bytes — this keeps API servers lightweight and stateless

### REST over HTTPS with OAuth 2.0
- All four use REST APIs over HTTPS with OAuth 2.0 / JWT Bearer token authentication
- In s3_object_store: AWS Signature V4 (HMAC-SHA256) instead of OAuth; but same REST-over-HTTPS pattern

### Long-Poll / SSE / WebSocket for Change Notifications
- Three of the four push file change notifications to connected clients in real-time
- In google_drive: WebSocket or Server-Sent Events (SSE) for connected clients; FCM/APNs for mobile offline
- In dropbox: Long-poll HTTP or SSE; Notification Server consumes Kafka and pushes to affected user sessions
- In s3_object_store: SNS/SQS/Lambda event notifications (push to subscribers); no persistent connection to browser
- In photo_storage: not a primary real-time sync requirement (upload → async processing; client polls or receives push notification on completion)

## Common Scalability Techniques

### Block-Level / Chunk-Level Deduplication (SHA-256 + Ref Count)
- All four deduplicate stored blobs using SHA-256 existence checks before uploading novel content
- Dedup check: client (or server) looks up SHA-256 in the metadata store; if found, skip upload; if not found, upload the bytes
- In google_drive: client checks `GET /chunks/{sha256}` before each chunk upload; ~20% dedup savings claimed
- In dropbox: client compares block hashes against server-known blocks; ~30% savings; Block Server manages ref counts per block hash
- In s3_object_store: ETag-based dedup at application level (S3 itself doesn't deduplicate; dedup is client responsibility)
- In photo_storage: Upload Service computes SHA-256, checks CAS store; ~15% exact duplicate savings

### Erasure Coding for Storage Efficiency and Durability
- Three of the four use erasure coding (rather than 3× replication) for the blob storage layer to achieve 11-nine durability at 1.5× overhead instead of 3× overhead
- In google_drive: 3× replication (Google Colossus uses RS codes internally, but design describes RF=3)
- In dropbox (Magic Pocket): erasure coding 14+3 = 17 shards; any 14 of 17 shards can reconstruct the block; ~1.21× storage overhead vs 3×
- In s3_object_store: erasure coding 6+3 Reed-Solomon; 9 shards across 9 storage nodes; any 6 sufficient for reconstruction; 1.5× overhead
- In photo_storage: erasure coded 6+3 (1.5×); same Reed-Solomon scheme as S3

### Consistent Hashing for Storage Node Assignment
- Two of the four use consistent hashing to distribute objects across storage nodes
- In s3_object_store: consistent hashing + 128 virtual nodes; Placement Service uses ring to assign shards to storage nodes; topology-aware (rack/AZ)
- In dropbox: ZooKeeper-based shard routing; MySQL shards assigned by namespace_id; not strict consistent hashing but consistent namespace routing
- In photo_storage: Cassandra uses consistent hashing with virtual nodes (vnodes) internally for partition placement

### Horizontal Sharding of Metadata DB
- All four shard or distribute their metadata store to handle the scale of billions of files/objects
- In google_drive: Spanner auto-shards globally by key range; no manual sharding
- In dropbox: MySQL sharded by namespace_id; 64 shards; ZooKeeper manages shard routing table
- In s3_object_store: metadata layer sharded by consistent hash of (bucket, key); each shard is an independent strongly-consistent replica group
- In photo_storage: Cassandra distributes partitions by user_id consistent hash; Elasticsearch sharded by index

### Deferred / Batched Quota Accounting
- Two of the four avoid per-operation quota writes by deferring quota accounting to periodic flushes
- In google_drive: Redis accumulators track quota deltas; periodically flushed to Spanner (avoids a Spanner write per chunk uploaded)
- In dropbox: quota updated on file commit (MySQL write); read quota from Memcached (cached read-path)
- In s3_object_store: storage metering is asynchronous (billing pipeline reads usage logs, not per-PUT accounting)

## Common Deep Dive Questions

### How do you achieve 11-nine durability without 3× replication overhead?
Answer: Use erasure coding instead of full replication. Reed-Solomon 6+3 stores 9 shards (6 data + 3 parity) across 9 independent storage nodes (different racks/AZs). Any 6 of 9 shards suffice for reconstruction, tolerating up to 3 simultaneous failures. Storage overhead is 1.5× instead of 3×. For 300 EB of data, this saves 450 EB of raw storage (equivalent of billions of dollars/year at cloud pricing). Magic Pocket uses 14+3 for even better efficiency.
Present in: dropbox (14+3), s3_object_store (6+3 RS), photo_storage (6+3 RS)

### How do you handle deduplication without scanning every stored object?
Answer: Content-addressed storage with SHA-256. Before uploading any block/chunk, the client computes SHA-256 of the content and checks if that hash exists in the metadata store (a single key lookup). If found, reference count is incremented; no byte transfer occurs. If not found, bytes are uploaded and a new entry is created. This makes dedup O(1) per block regardless of total storage size. Savings: 15–30% depending on workload (more for photo storage with camera duplicates, less for unique binary artifacts).
Present in: google_drive, dropbox, photo_storage

### How do you make large file uploads resumable?
Answer: Split the file into fixed-size chunks/blocks (4 MB for Dropbox, 256 KB–4 MB for Drive, 5 MB minimum for S3 multipart). Each chunk is independently uploaded and ACKed. The server tracks which chunks have been received (via chunk index in metadata or ETag list for multipart). On network failure, client resumes from the last ACKed chunk. Session state stored in Redis (TTL 24h) or persistent metadata DB. Client uses HEAD request to discover resume offset.
Present in: google_drive, dropbox, s3_object_store, photo_storage

### How do you propagate file changes to all connected devices in near real-time?
Answer: Event-driven push via Kafka + persistent client connections. On metadata commit (file created/changed/deleted), the metadata service publishes an event to Kafka. A Notification Server consumes the event and pushes to all connected sessions of the affected user (and shared folder members) via WebSocket/SSE/long-poll. For mobile clients that are not connected, FCM/APNs push triggers the app to sync on next foreground. End-to-end latency < 5 s for files under 1 MB.
Present in: google_drive, dropbox

### How do you enforce access control without a DB read on every file access?
Answer: Cache ACL checks in Redis (60 s TTL) keyed by (user_id, resource_id). On cache miss, evaluate the permission model (Zanzibar ReBAC graph for Google Drive, simple owner/share table for Dropbox/Photo Storage). For S3: IAM policy evaluation is cached locally on API Gateway nodes and refreshed asynchronously from the IAM service. For photo storage: ACL check results cached in Redis per (user_id, photo_id) pair. Cache invalidation on permission change is done by deleting or TTL-expiring the relevant cache entry.
Present in: google_drive (Zanzibar ReBAC), dropbox (ACL table), s3_object_store (IAM policy cache), photo_storage (Redis ACL cache)

## Common NFRs

- **Durability**: 11 nines (99.999999999%) across all four — files must never be lost
- **Availability**: 99.99% for metadata and blob serving; transcoding/processing pipeline can tolerate brief outages with retry queues
- **Upload latency**: p99 < 200–500 ms to durable metadata commit; blob bytes transfer depends on bandwidth
- **Metadata read latency**: p99 < 100–200 ms for folder listings and file metadata
- **CDN-delivered read latency**: p99 < 30–100 ms for thumbnails/small blobs (CDN hit); < 500 ms on CDN miss
- **Consistency**: strong for metadata, eventual for CDN-served blob bytes
- **Security**: TLS 1.3 in transit; AES-256 at rest; presigned URLs for time-limited access; zero-knowledge optional (Dropbox Business)
- **Deduplication**: block/chunk-level SHA-256 dedup reduces physical storage 15–30%
