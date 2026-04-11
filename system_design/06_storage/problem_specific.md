# Problem-Specific Design — Storage (06_storage)

## Google Drive

### Unique Functional Requirements
- Arbitrary directory hierarchy (folders, sub-folders, symlinks)
- Real-time collaborative editing via Google Docs/Sheets/Slides (Operational Transformation / CRDT — treated as black box)
- Lock-based conflict resolution for third-party file types
- Per-user quota shared across Gmail + Drive + Photos (15 GB free, up to 2 TB paid)
- Full-text search including OCR for images and PDFs
- Workspace (enterprise) features: Shared Drives, Team folders, organization-wide sharing

### Unique Components / Services
- **Spanner**: globally distributed relational DB with external consistency (TrueTime-based); chosen because file hierarchy operations (atomic subtree move, quota decrement, version creation) require ACID transactions spanning multiple rows across regions — no other problem in this folder uses Spanner
- **Zanzibar ReBAC (Relationship-Based Access Control)**: permission system where `user:alice can view file:X` is a tuple stored in a graph; supports cascading permissions from folder to children; Google's Zanzibar paper describes this exact model
- **Colossus**: Google's internal distributed file system (successor to GFS); backs GCS; provides byte-range reads, automatic data replication, and block-level erasure coding internally
- **Deferred Quota Accounting**: Redis accumulators accumulate quota deltas per user; flushed to Spanner in batches to avoid one Spanner write per uploaded chunk
- **Audit Log**: append-only immutable log of all file operations (access, share, delete); required for compliance in Google Workspace

### Unique Data Model
- **files** (Spanner): file_id, owner_id, name, parent_folder_id, content_hash, size_bytes, MIME type, version_count, trashed_at
- **file_versions** (Spanner): file_id, version_id, chunk_manifest (ordered list of SHA-256 chunk hashes), created_at, author_user_id
- **file_chunks** (Spanner or GCS metadata): sha256, size, ref_count, blob_path (GCS URI)
- **permissions** (Zanzibar tuples): `(user:X, role:viewer, resource:file:Y)` — evaluated via graph traversal for cascading folder → children
- **users** (Spanner): user_id, email, quota_used, quota_limit
- **Chunk size**: 256 KB–4 MB (variable, set by upload client based on file size)

### Unique Scalability / Design Decisions
- **Spanner over MySQL**: chosen for global strong consistency without manual sharding; other problems in this folder use MySQL or Cassandra (eventually consistent or regionally sharded); trade-off is cost and operational complexity
- **Zanzibar over simple ACL table**: permissions cascade from parent folder to children via graph traversal; Zanzibar supports `check(user, action, resource)` in O(graph depth) with caching; simpler ACL tables can't express nested sharing cleanly
- **Deferred quota via Redis accumulators**: avoids one Spanner write per 4 MB chunk (thousands of writes per large upload); Redis accumulates delta, periodic flush (every 30 s or on commit) writes final quota to Spanner
- **Chunk-level dedup at upload time**: client calls `GET /chunks/{sha256}` per chunk; if 404, uploads the chunk bytes; if 200, skips upload; deduplicated blocks save ~20% storage

### Key Differentiator
Google Drive's defining architectural choice is **Spanner + Zanzibar**: the global file hierarchy with cascading permissions requires externally consistent transactions (Spanner/TrueTime) and a relationship-based access control graph (Zanzibar) — neither of which appears in the other three storage problems. The trade-off is high cost and operational complexity for the benefit of true global consistency and expressive permission semantics.

---

## Dropbox

### Unique Functional Requirements
- Block-level delta sync: only changed 4 MB blocks uploaded on file modification (88% bandwidth savings for large files with small edits)
- LAN sync: devices on the same local network sync directly peer-to-peer without routing through Dropbox servers
- Selective sync: desktop client can exclude specific folders from local sync (files remain in cloud)
- Conflict resolution: creates a "conflicted copy" named `filename (Bob's conflicted copy YYYY-MM-DD)` in same folder (no merge attempted)
- Namespace journaling: each namespace (user's Dropbox root, shared folder) has a change journal that sync clients poll

### Unique Components / Services
- **Magic Pocket**: Dropbox's proprietary block storage system built in 2016 to replace AWS S3; erasure coding 14+3 (17 shards, any 14 sufficient for reconstruction); ~1.21× overhead; reduced storage cost by ~90% vs S3; S3-compatible API but custom hardware and data layout
- **Block Server**: stateless service that generates presigned S3/Magic Pocket PUT/GET URLs, verifies block checksums on receipt, manages per-block reference counts, and queues virus scanning — does NOT store block bytes itself
- **Metadata Server (MySQL + ZooKeeper)**: MySQL sharded by namespace_id (64 shards initially); ZooKeeper manages shard routing table and leader election (Patroni-like); read replicas per shard for read offload; historically "Edgestore" infrastructure
- **Memcached cluster**: 90% of metadata reads served from Memcached (file metadata, namespace state, dedup lookups); Redis is separate, used only for session tokens, rate limits, and LAN peer tables
- **LAN Sync**: clients broadcast presence via mDNS (`_dropbox._tcp.local`); when client A detects client B has needed blocks, fetches directly over local TCP (dynamically chosen port); Metadata Server knows which blocks each client has cached and can route peer-to-peer — bypasses Dropbox servers entirely
- **Desktop Client SQLite DB**: `~/.dropbox/config.db` maintains local snapshot of file state (block manifests, sync state, local file hashes); enables offline detection of changed blocks without server round-trip

### Unique Data Model
- **namespaces** (MySQL): namespace_id, owner_user_id, type (personal | shared_folder), cursor (sync pointer)
- **files** (MySQL, sharded by namespace_id): file_id, namespace_id, path, current_version_id, size, is_deleted
- **file_versions** (MySQL): version_id, file_id, block_manifest (ordered list of SHA-256 block hashes), created_at, author_device_id
- **blocks** (MySQL): sha256 (PK), size, ref_count, storage_path (S3/Magic Pocket URI), is_deleted
- **Block size**: fixed 4 MB (unlike Drive's variable 256 KB–4 MB)
- **Local SQLite**: mirrors per-file block manifest for change detection without server

### Unique Scalability / Design Decisions
- **Namespace-as-shard-key**: all files within a namespace (user root or shared folder) live on one MySQL shard; enables ACID operations on the namespace without cross-shard joins; shared folders shared by 1,000 users still live on one shard (owner's namespace)
- **Magic Pocket 14+3 vs S3**: 14+3 erasure coding gives ~1.21× storage overhead vs 3× replication; at Dropbox scale (500 PB+), saves hundreds of PB; trade-off is rebuilding infrastructure (network, hardware) instead of paying AWS for storage
- **Memcached as primary read cache (not Redis)**: Dropbox uses Memcached for metadata reads (simple get/set semantics sufficient); Redis is used only for mutable state (session, rate limits, LAN peers); reflects historical technology choices dating back to 2007–2010
- **LAN sync bypasses Dropbox servers**: on a local network, mDNS peer discovery + direct TCP transfer reduces Dropbox server bandwidth and latency significantly; relevant for offices where many employees share the same files (e.g., shared marketing assets)
- **Block-level delta sync (88% bandwidth savings)**: on a 100 MB file change where only 12 MB of blocks changed, only those 3 blocks (12 MB) are uploaded, not the full 100 MB; reference counting handles shared blocks across versions

### Key Differentiator
Dropbox's defining technical choice is **block-level delta sync + Magic Pocket**: the 4 MB block abstraction enables precise delta uploads and the LAN peer-to-peer transfer; Magic Pocket is Dropbox's most consequential infrastructure decision — replacing AWS S3 entirely with a custom erasure-coded system to reduce storage costs by 90%, a move no other problem in this folder makes.

---

## S3 Object Store

### Unique Functional Requirements
- Bucket-level namespace: objects identified by bucket name + key string (not a file hierarchy)
- Object versioning: every PUT creates a new version; GET returns latest by default; specific version retrievable by version ID
- Multiple storage classes with automatic lifecycle transitions: Standard → Standard-IA → Glacier → Deep Archive
- Bucket policies and ACLs: IAM-style (Allow/Deny, principal, action, resource, condition) for fine-grained access control
- Server-Side Encryption variants: SSE-S3 (S3-managed keys), SSE-KMS (customer-managed in KMS), SSE-C (customer-provided key per request)
- Cross-Region Replication (CRR): async replication of new objects to a bucket in another region
- Event Notifications: trigger SNS/SQS/Lambda on object creation, deletion, restore completion
- Strong read-after-write consistency for all operations (since Nov 2020)
- Object Lock (WORM): prevent object deletion or overwrite for a specified retention period

### Unique Components / Services
- **AWS Signature Version 4 (SigV4)**: HMAC-SHA256 authentication; canonical request = HTTP method + URI + query string + headers + body hash; signed with derived signing key (region + service + date + secret); stateless verification — no session; unique to S3 among this folder's problems
- **Placement Service**: given new object (bucket, key, size), determines which storage nodes will hold its erasure-coded shards using consistent hashing on (bucket, key) with 128 virtual nodes; returns write plan `[node_1: shard_0, …, node_9: shard_8]`; topology-aware (different racks/AZs per shard)
- **Storage Nodes**: commodity servers (large HDD for cost efficiency; hot data on SSD); each manages a local key-value store of shard_id → bytes; serve byte-range reads via kernel sendfile; perform local checksum verification on every read to detect bit rot
- **Lifecycle Manager**: periodic scans of metadata index to find objects meeting lifecycle policy criteria (age, prefix, tag conditions); triggers storage class transitions (metadata update + data copy to cheaper backend) and expiration (delete marker or hard delete)
- **Cross-Region Replication Service**: consumes `ObjectCreated` events per bucket with CRR enabled; reads object from source region, writes to destination; tracks replication lag per bucket; exponential backoff on failure
- **Transactional Outbox for Event Notifications**: on any object mutation, a notification record is written atomically with the metadata change in the same transaction; a notification worker reads the outbox and publishes to SNS/SQS/Lambda; idempotency key prevents duplicate delivery

### Unique Data Model
- **buckets** (ZooKeeper / globally distributed store): bucket_name (globally unique), owner_account_id, region, versioning_enabled, lifecycle_rules (JSON), replication_config, cors_config, encryption_config
- **object_index** (distributed hash table, sharded by consistent hash of bucket+key): (bucket, key, version_id) → {storage_location_vector, ETag, size_bytes, content_type, user_metadata, last_modified, storage_class}
- **multipart_uploads** (metadata store): upload_id, bucket, key, initiated_at, parts (list of {part_number, ETag, size}) — cleared on completion or abort
- **lifecycle_rules** (per bucket): {id, filter_prefix, expiration_days, transition_to_storage_class_after_days}
- **No concept of directories**: key strings can contain "/" to simulate hierarchy; `ListObjects` with `delimiter="/"` returns common prefixes simulating folder listing
- **Object size range**: 0 bytes to 5 TB; multipart required for > 5 GB parts

### Unique Scalability / Design Decisions
- **Consistent hashing + 128 vnodes + topology-aware placement**: object key hashed onto ring; 128 virtual nodes per physical node; Placement Service ensures each shard of an erasure-coded object lands on a different rack/AZ; supports hot-spot redistribution by splitting vnodes
- **Hot prefix problem and the solution**: S3's documented 3,500 PUTs/s and 5,500 GETs/s limits are per prefix; for hot workloads, customers add random prefixes (e.g., `{random_4_char}/{date}/{key}`) to distribute across more shards; S3 Auto-Scaling internally repartitions hot prefixes
- **Strong read-after-write consistency (since Nov 2020)**: achieved by making the metadata layer the single serializable point; all read requests go through the metadata layer to get the storage location, guaranteeing they see the latest committed version; before 2020, S3 was only eventually consistent for overwrite PUTs and DELETEs
- **Erasure coding 6+3 at storage layer only**: metadata layer uses strong consistency (synchronous replication to quorum); storage nodes use erasure coding for cost efficiency; separating the consistency model per layer is the key architectural insight
- **Multipart upload with manifest-based assembly**: `CompleteMultipartUpload` is atomic — it writes a manifest linking part ETags to the final object; no actual byte copying occurs; storage nodes serve parts as-is with the manifest providing ordering; makes "completing" a 5 TB upload near-instant

### Key Differentiator
S3's defining complexity is its **IAM + SigV4 authorization layer + storage class tiering**: unlike the other three problems (which serve known users with simple ACL checks), S3 must evaluate complex IAM policies (principal, action, resource, condition) per request, authenticate via stateless HMAC signatures, and manage objects across multiple storage tiers with automatic lifecycle transitions — making it as much an identity/authorization system as a storage system.

---

## Photo Storage (Instagram / Google Photos Scale)

### Unique Functional Requirements
- EXIF metadata extraction and indexing (camera model, GPS, capture timestamp, ISO, aperture, shutter speed)
- Face detection, embedding, and per-user clustering into "person" clusters with user-assigned labels
- AI scene labeling (beach, sunset, food, etc.) for search and smart albums
- Smart album auto-creation (e.g., "Italy 2024", "People: Alice") based on ML-derived metadata
- GPS reverse geocoding: coordinates → place name for location-based search
- Multi-format support: JPEG, PNG, HEIC, RAW (up to 50 MB per photo)
- Multiple thumbnail variants per photo: 64×64, 360×360, 1080p — generated async

### Unique Components / Services
- **Thumbnail Generator (libvips)**: high-performance image processing library (10× faster than ImageMagick for large images due to streaming pipeline); produces 3 thumbnail variants per uploaded photo; workers consume Kafka `photo.uploaded` events
- **Face Detection Pipeline**: MTCNN (Multi-task Cascaded Convolutional Network) for face detection in photos → FaceNet for 128-dimensional face embeddings → DBSCAN (density-based clustering) or ANN search for per-user face cluster assignment; entire pipeline async, completes within 60 s of upload
- **FAISS (per-user face vector index)**: Facebook AI Similarity Search; maintains one FAISS index per user containing 128-dim FaceNet embeddings for all faces in their photo library; ANN search on new face embedding → find nearest cluster → assign to existing cluster or create new cluster; supports GPU acceleration for large user libraries
- **Scene Labeling (ResNet-50 / ViT)**: convolutional or transformer model trained on scene categories (beach, mountain, food, sunset, etc.); produces tag embeddings stored in Elasticsearch for multi-label search; runs on Triton Inference Server or similar GPU inference fleet
- **Geo Tagger**: GPS coordinates from EXIF → reverse geocoding API (Google Maps / Nominatim / custom geocoder) → place name + city + country; stored in photo metadata; enables "photos from Italy" queries via Elasticsearch geo queries
- **EXIF Extractor (ExifTool / libexif)**: async worker extracts all EXIF fields; camera model, lens focal length, aperture, shutter speed, ISO, capture timestamp (authoritative, not upload timestamp), GPS lat/lon/alt, color space; all indexed in Elasticsearch

### Unique Data Model
- **photos** (Cassandra): PRIMARY KEY `(user_id, upload_time_bucket)`, clustering key `upload_time DESC` (TWCS — TimeWindowCompactionStrategy, weekly buckets); columns: photo_id, sha256, blob_path, original_filename, size_bytes, mime_type, exif_json, gps_lat, gps_lon, scene_labels (list), face_cluster_ids (list), is_deleted, trash_at
- **face_clusters** (Cassandra): PRIMARY KEY `(user_id, cluster_id)`; columns: cluster_id, label (user-assigned name), centroid_embedding (128-dim float32 array), photo_count, sample_photo_ids (list<10>)
- **face_instances** (Cassandra): PRIMARY KEY `(user_id, photo_id)`, clustering key `face_id`; columns: bounding_box_json, embedding (128-dim), cluster_id (foreign to face_clusters)
- **albums** (PostgreSQL): album_id, owner_user_id, title, is_smart_album, smart_album_type (manual | date_location | people), cover_photo_id, created_at
- **album_photos** (PostgreSQL): album_id, photo_id, added_at, added_by_user_id
- **photo_shares** (PostgreSQL): share_id, resource_type (photo | album), resource_id, shared_by, shared_with_user_id (nullable), share_link_token (nullable), permission (view | contribute)
- **Search Index (Elasticsearch)**: one document per photo; fields: photo_id, user_id, exif fields, gps_point (geo_point type), scene_labels (keyword array), camera_model, capture_date; access control enforced by user_id filter on all queries (never cross-user)

### Unique Scalability / Design Decisions
- **Cassandra TWCS for photo timeline**: time-series access pattern (user views recent photos most); TimeWindowCompactionStrategy compacts writes within weekly time windows; old photo windows become immutable and compact efficiently with no tombstone buildup; same pattern as Discord's messages table
- **FAISS per-user index (not a global vector DB)**: face embeddings are deeply personal (you don't want cross-user face search); per-user FAISS index keeps index size manageable (~128 dim × 4 bytes × num_faces_per_user); avoids privacy leakage; rebuilt incrementally as new photos arrive; GPU-accelerated for users with large libraries
- **libvips over ImageMagick for thumbnail generation**: libvips uses a streaming, demand-driven pipeline (never loads full image into memory); 10× less memory, 3–10× faster; critical at 100 M photos/day = 3,471 photos/s peak, each needing 3 thumbnails = ~10,000 thumbnail operations/s
- **Progressive JPEG delivery**: original photos stored as progressive JPEG where possible; client browser renders low-resolution scan first, then refines as more bytes arrive; improves perceived load time without changing infrastructure
- **ML pipeline parallelism**: all async workers (thumbnail, face detection, scene labeling, EXIF, geo) consume the same Kafka `photo.uploaded` event independently; no dependency chain; all complete in parallel within their respective SLAs (thumbnail < 5 s, face detection < 60 s); failure in one pipeline (e.g., scene labeling) does not block photo availability
- **Smart album auto-creation via ML signals**: "Italy 2024" album created by clustering photos by GPS place name + date range; "People: Alice" created when user labels a face cluster; no manual curation needed; all based on Elasticsearch aggregations + FAISS cluster metadata

### Key Differentiator
Photo Storage's defining complexity is its **ML processing pipeline**: unlike the other three storage problems (which focus on file transfer, sync, and durability), photo storage runs a multi-stage ML pipeline on every uploaded photo — face detection (MTCNN), embedding (FaceNet), clustering (DBSCAN + FAISS), scene classification (ResNet-50/ViT), and GPS geocoding — all running in parallel and completing within 60 seconds, requiring a dedicated GPU inference fleet (Triton Inference Server), a per-user vector index (FAISS), and specialized ML-oriented data models (128-dim embeddings in Cassandra/FAISS) that no other problem in this folder has.
