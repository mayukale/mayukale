# Pattern 6: Storage — Interview Guide

Reading Pattern 6: Storage — 4 problems, 9 shared components

---

## STEP 1 — PATTERN OVERVIEW

This pattern covers systems that store, retrieve, organize, and deliver user files at massive scale. All four problems in this folder sit on the same foundational stack, but each adds a distinct layer of complexity on top.

**The four problems:**

1. **Google Drive** — A general-purpose cloud file system with a hierarchical folder tree, real-time collaborative editing, cascading permissions, and global strong consistency. The defining challenge is expressing complex inheritance-based access control and ensuring atomic operations across a globally distributed file hierarchy.

2. **Dropbox** — A desktop sync-first product focused on minimizing bandwidth usage. Its defining technical challenge is block-level delta sync: only the changed 4 MB blocks of a modified file are uploaded, not the whole file. LAN sync (peer-to-peer on a local network) is a major differentiator.

3. **S3 Object Store** — A flat-namespace, API-driven object storage service used by applications rather than end users. The defining challenges are operating at astronomical scale (trillions of objects, exabytes of data), supporting multiple storage tiers with automatic lifecycle transitions, and providing a stateless, HMAC-signed authentication model that needs no central lookup on each request.

4. **Photo Storage** — An upload-heavy, read-even-heavier media storage system that runs a rich ML pipeline on every uploaded photo: thumbnail generation, face detection, face clustering, scene labeling, GPS reverse geocoding. The defining challenge is the multi-stage async ML pipeline that must complete within 60 seconds of upload.

**Why these four problems appear together:** They all require solving the same core set of storage problems — chunking/deduplication, metadata separation, presigned URL delivery, CDN caching, resumable uploads, and durability via erasure coding. Learning the pattern once and then noting where each problem diverges is the most efficient way to handle any of them in an interview.

---

## STEP 2 — MENTAL MODEL

**Core idea:** Separate your system into two completely independent planes — a small, fast, strongly-consistent **metadata plane** that describes what exists and who can access it, and a large, cheap, eventually-consistent **data plane** that holds the raw bytes. Every good storage system design flows from keeping these planes independent.

**Real-world analogy:** Think of a library. The card catalog (metadata plane) is a small, organized, immediately queryable index: it tells you which books exist, where they are shelved, and who has checked them out. The shelves of books (data plane) are large, physical, and slow to search by browsing — but once the card catalog tells you exactly where to look, retrieval is direct. You never redesign the physical shelves when you reorganize the catalog, and you never put catalog cards on the shelves. They are separate systems that talk through a well-defined interface (the call number / the object key / the SHA-256 hash).

**Why this is hard:** Three things compound together to make storage systems genuinely difficult:

First, **durability requirements are unforgiving**. Eleven nines (99.999999999%) means you can lose at most one object per ten thousand years per billion objects. Achieving this economically — without triple-replicating every byte — requires erasure coding, which introduces read/write amplification and a reconstruction step that must handle partial node failures gracefully.

Second, **consistency is layered and non-obvious**. Users expect that after they upload a file, they can immediately download it (strong read-after-write). But they also expect that a CDN edge node in Tokyo serves their file at low latency, which implies caching, which implies eventual consistency for the bytes. The metadata must be strongly consistent; the bytes can be eventually consistent. Getting this split right — and knowing which layer is which — is what separates strong candidates from average ones.

Third, **scale creates emergent problems that don't exist at small size**. A 4 MB block dedup table with a billion entries becomes a hot-spot when millions of users are checking the same popular blocks per second. A single namespace with a billion files makes simple operations like folder listing catastrophically slow. A consistent hash ring for object placement becomes vulnerable to prefix-based hot spots when application teams accidentally create write-heavy prefixes. These aren't theoretical — every interviewer who has worked in this space has seen them in production.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

These are the questions you must ask before drawing a single box. Each one materially changes the architecture.

**Question 1: Is this primarily a sync product (files need to appear on all a user's devices) or a storage-and-retrieve product (files are uploaded by one process and downloaded by another)?**

What changes: Sync products (Drive, Dropbox) need a change log, a cursor-based notification protocol, and a desktop client with local block-state caching. Storage products (S3, photo storage) don't need any of this — they just need a fast read/write API. If the interviewer says sync, you immediately need to discuss the notification service, long-polling or WebSocket connections, and conflict resolution. If they say storage/retrieve, you skip all of that.

**Question 2: What is the maximum file (or object) size, and do we expect many large files?**

What changes: Objects over 5 GB require a multipart upload API. Large files make the chunk size choice consequential — 256 KB chunks vs 4 MB chunks have very different per-chunk overhead profiles. Large files also mean your API servers cannot buffer them in memory; you need to stream bytes or use presigned URLs for direct client-to-storage upload.

**Question 3: Do users need to share files with other users, and do permissions need to cascade from folders to their contents?**

What changes: Sharing with link access is simple (a random token in a table). Sharing with cascading permissions (user A can view folder X and therefore all 10,000 files inside it) requires either a graph-based ACL (Zanzibar-style) or materialized permission rows on every descendant, which creates write amplification on every share operation. The complexity difference between these two is enormous.

**Question 4: What consistency model is required for reads immediately after writes?**

What changes: Strong read-after-write (S3 post-2020, Google Drive) means the metadata layer must be the single serialization point — all reads route through it, no CDN or cache can bypass it for freshness. Eventual consistency (acceptable for CDN serving of file bytes) allows aggressive CDN caching with long TTLs and is the right choice for the data plane. Candidates who say "just make everything strongly consistent" have not accounted for the cost at exabyte scale.

**Question 5: Is deduplication a requirement, and should it be transparent to users?**

What changes: Transparent dedup (user still sees N copies of the same photo, but storage only has one) uses a content-addressed store with reference counting and is the right model for photo storage and Drive. Non-transparent dedup (the system refuses a second upload of identical bytes and tells the user) is simpler but changes the UX. If dedup is required, you immediately need to discuss SHA-256 as the object identity, reference counting for GC, and the privacy implication of sharing a byte store across users.

**Red flags to watch for in the interviewer's clarifications:**
- If they say "real-time collaborative editing," push back to treat it as a black box. OT/CRDT engines are a separate system design topic. Say you'd integrate with a collaborative editing service (like Google Docs backend) rather than build it from scratch.
- If they mention "exabyte scale," erasure coding becomes mandatory. Do not propose 3× replication as your durability strategy — the cost difference is enormous.
- If they say "mobile clients," you need to discuss the behavior when the device is offline, which immediately brings in local state caching and sync-on-reconnect.

---

### 3b. Functional Requirements

**Core requirements (appear in all four problems):**
- Upload a file (or object or photo) and receive a durable commit acknowledgment
- Download a file by its identifier, supporting byte-range requests for large files
- Soft-delete (trash) with configurable retention before permanent removal
- File versioning: retain N prior versions; restore to any version
- Sharing: share with specific users by email or via a public link

**Scope decisions:**
- Collaborative editing (OT/CRDT) — out of scope; black-box it
- Billing and payments — out of scope
- Video transcoding — out of scope for photo storage
- Admin consoles and enterprise SSO — out of scope

**Clear statement to open with:**

"We need to build a storage system that lets users upload files of up to [5 GB / 2 TB / 50 MB for photos] each, download them on any device, share them with other users, and retain prior versions for up to [30 days / 180 days / unlimited]. The system should serve [1 billion users / 100 M uploads/day / 300 trillion objects], with 11-nine durability — files must never be lost. Metadata reads must be strongly consistent; blob delivery can be eventually consistent via CDN."

---

### 3c. Non-Functional Requirements

**How to derive the NFRs from first principles:**

Start with the user's worst-case scenario for each property, then translate it into a concrete engineering target.

**Durability — 11 nines (99.999999999%):** The worst thing that can happen in a storage system is data loss. 11 nines is the industry standard for cloud storage services. This immediately implies erasure coding or multi-region replication, scrubbing (background checksum verification), and a specific repair priority scheme when storage nodes fail.

**Availability — 99.99% (< 52 minutes/year):** File storage is not a life-critical system, so 99.99% is the right target. This differs from durability: availability is "can the user access their file right now"; durability is "has the file's data survived." You can have 100% availability and 0% durability (serve blank bytes reliably). They are independent.

**Latency — metadata p99 < 100-200 ms; CDN-served thumbnails/blobs p99 < 30-100 ms:** Metadata latency drives the sync and folder-browsing UX. Blob delivery latency drives whether the app feels snappy. CDN edge hits should be under 30 ms. Origin misses (CDN miss → blob store) under 500 ms is acceptable.

**Consistency — strong for metadata, eventual for bytes:** Metadata (which file version is current, what are the permissions) must be strongly consistent so all clients agree on the canonical state. Byte delivery via CDN can be eventually consistent because content-addressed blobs are immutable — once a sha256 is committed, those bytes never change.

**Key trade-offs:**

✅ Erasure coding (6+3 Reed-Solomon) achieves 11-nine durability at 1.5× storage overhead — compared to 3× replication's 3× overhead. At 300 EB scale, this saves 150 EB = roughly $3 billion/year.

❌ Erasure coding requires reading 6 shards in parallel for every GET (vs 1 replica with replication), adding per-operation overhead that makes small object reads slightly more expensive. Solution: replicate small objects (< 128 KB) 3× instead.

✅ Strong metadata consistency prevents split-brain where two clients see different file states. Spanner/TrueTime gives this globally with low latency.

❌ Spanner is operationally complex and expensive to run. MySQL sharded by namespace/user is simpler and cheaper. Only choose Spanner if you need global multi-region strong consistency (Google Drive) — not if you can tolerate per-region strong consistency (Dropbox).

✅ CDN caching of content-addressed blobs is maximally efficient: the cache key IS the SHA-256 hash, so the same bytes from different users hit the same cache entry, and the entry never needs invalidation (immutable content).

❌ CDN caching only works for content-addressed (immutable) URLs. Mutable file paths ("my-resume.pdf") cannot be cached this way — you need to cache by (path + version) and invalidate on update.

---

### 3d. Capacity Estimation

**The formula you should internalize:**

```
Daily storage growth = (uploads/day) × (average file size) × (replication/erasure coding factor)
Operations/sec = (daily operations / 86,400) × peak multiplier (3×)
Bandwidth = (operations/sec) × (average transfer size)
```

**Anchor numbers by problem:**

| Problem | DAU / Scale | Uploads/day | Avg size | Peak upload OPS | Peak download OPS | Storage growth/day |
|---|---|---|---|---|---|---|
| Google Drive | 200 M DAU | 100 M | 2 MB | ~3,500/s | ~14,000/s | ~600 TB raw (3× RF) |
| Dropbox | 15 M DAU | 45 M | 5 MB (delta: 1 MB) | ~1,560/s | ~2,600/s | ~135 TB raw (3×) |
| S3 Object Store | 300 T objects total | 100 B/day | 1 MB | ~3.5 M/s | ~17.4 M/s | ~150 PB raw (1.5×) |
| Photo Storage | 200 M DAU | 100 M | 3 MB | ~3,471/s | ~34,700 views/s | ~421 TB raw (1.5×) |

**Architecture implications of the numbers:**

- 52,000 metadata reads/second (Google Drive peak) → You cannot serve this from a single database. You need Redis caching (60-second TTL for ACL and folder listings) absorbing 80% of reads, with Spanner handling only cache misses.

- 3.5 M PUT operations/second (S3) → A single metadata database shard cannot serialize this. You need the metadata layer sharded by consistent hash of (bucket + key), with thousands of shards.

- 3,471 photo uploads/second peak (photo storage) → Each needing 3 thumbnail variants = ~10,000 image processing operations/second. libvips (not ImageMagick) is mandatory at this rate.

- 100 M face detection jobs/day peak → You need a GPU inference fleet (Triton Inference Server), not CPU workers.

**Time yourself:** In an interview, capacity estimation should take 5-7 minutes. Walk through: DAU assumption → ops/day for each major operation type → divide by 86,400 for average ops/second → multiply by 3× for peak → convert to bandwidth → convert to storage growth. State your assumptions out loud.

---

### 3e. High-Level Design (HLD)

**Whiteboard order — draw these in this sequence:**

1. **Client layer** (web browser, desktop client, mobile app) — one box at the top. Note that the desktop client (for Dropbox/Drive) has a local sync engine; for S3/photo storage the client is an SDK or mobile app.

2. **API Gateway / Edge** (TLS termination, authentication, rate limiting, request routing) — one box. All requests go through this. Never draw direct client-to-database arrows.

3. **Metadata Service + Metadata Store** (the core) — two connected boxes. The metadata service handles the file tree, permissions, version catalog, quota tracking. The metadata store (Spanner, MySQL sharded, Cassandra, or custom KV) backs it. This is where the critical decisions live.

4. **Upload Service** — one box. Handles resumable upload sessions, chunk reception, dedup checks, presigned URL generation, committing to metadata store.

5. **Blob Store** (content-addressed, SHA-256 keyed) — one box. This is where the actual bytes live: GCS/Colossus for Drive, S3/Magic Pocket for Dropbox, custom storage nodes for S3 object store, GCS/custom CAS for photo storage.

6. **CDN** (Cloud CDN, Cloudflare, CloudFront) — attached to the blob store via arrows showing the read path. 80-90% of file reads should be CDN hits.

7. **Event Bus (Kafka)** + **Async Workers** — one Kafka box with worker boxes hanging off it: thumbnail generator, virus scanner, search indexer, face detection pipeline, notification fanout.

8. **Notification Service** (for Drive/Dropbox) — connects to Kafka on the input side and WebSocket/SSE/FCM on the output side.

**Key architectural decisions to call out explicitly (don't wait to be asked):**

- "I'm separating the metadata plane from the data plane. API servers never proxy blob bytes — clients upload/download directly to the blob store via presigned URLs."
- "I'm using content-addressed storage keyed by SHA-256. This gives us deduplication, immutable CDN URLs, and safe cache-forever headers, all for free."
- "For metadata I'm choosing [Spanner/MySQL/Cassandra] because [global strong consistency / ACID namespace operations / write-heavy time-series pattern]."
- "After a durable commit, I publish to Kafka. All async work — thumbnails, virus scanning, ML inference — happens downstream without blocking the upload response."

**Data flow for file upload (memorize this sequence):**

1. Client computes SHA-256 of file/chunks.
2. Client calls metadata service: "initiate upload session." Gets back session ID.
3. Client calls metadata service: "which of these chunk hashes are new?" Gets back the list of novel chunks.
4. Client uploads novel chunks directly to blob store via presigned PUT URLs.
5. Client calls metadata service: "commit upload session." Metadata record created atomically. Quota decremented.
6. Metadata service publishes event to Kafka.
7. Async workers process (thumbnails, face detection, virus scan, search indexing).
8. Notification service pushes "file ready" to connected clients of the user.

---

### 3f. Deep Dive Areas

The three areas interviewers probe most deeply in storage problems:

**Deep Dive 1: Deduplication and the chunk reference counting problem**

The problem: Content-addressed storage with SHA-256 dedup works perfectly when things are created. The hard part is deletion. When a user deletes file version V2 that shares 249 of its 250 chunks with V1, you must know that 249 chunks are still referenced by V1 and must NOT be deleted from the blob store. Reference counting solves this, but it creates its own challenges.

The solution: Each entry in the chunks/blocks table has a `ref_count` column. Every new version that references a chunk increments `ref_count`. Every version deletion decrements `ref_count`. When `ref_count` reaches zero, the blob is NOT immediately deleted — it enters a GC queue with a 7-day grace period (in case the version deletion was part of a rollback that gets re-applied). A background GC job runs nightly and physically deletes blobs with `ref_count = 0` and `last_ref_count_zero_at > 7 days ago`.

Unprompted trade-offs to mention:
- ✅ The grace period prevents data loss from race conditions between the ref_count decrement and the GC job.
- ❌ The grace period means storage is temporarily over-provisioned during bulk deletion sprees.
- ✅ Batching reference count updates (decrement 250 ref_counts in a single transaction on version delete) is faster than individual decrements.
- ❌ Long transactions on the chunks table can create lock contention at high write rates. Solution: use optimistic locking or a separate GC-queue table to decouple the main transaction from the GC work.

**Deep Dive 2: Sync protocol and cursor-based change notification**

The problem: With 200 M DAU, you cannot maintain a persistent connection to every client. But clients need to know about changes as quickly as possible. Polling every file's metadata is O(N) per user per poll — catastrophic.

The solution: A **monotonically increasing cursor per user/namespace** in an append-only change log table (partitioned by user_id in Cassandra, or by namespace_id in MySQL). Each change event (file created, modified, renamed, deleted) writes one row. The client saves its last cursor. On reconnect, it sends one query: "give me all changes since cursor X" — this is a single efficient range scan on (user_id, change_id > X). For real-time notification, a **hybrid push + pull** model: the server pushes a lightweight "you have changes" signal (WebSocket/SSE/FCM), which triggers the client to do a `list_changes?since=cursor` pull. The server never has to buffer change payloads per connection.

Unprompted trade-offs to mention:
- ✅ Cursor-based pull handles offline clients perfectly — they resume from their saved cursor after any outage, never missing events.
- ❌ Clients offline > 90 days receive a `FULL_SYNC_REQUIRED` response (the change log is only retained 90 days by TTL). Full re-sync is expensive but very rare.
- ✅ The push trigger + pull separation means the notification server is stateless about change content — it only maintains the connection, not the change data. This makes it easy to scale.
- ❌ There is inherent latency in the hybrid model: push signal arrival → client polls → response received → client renders change. End-to-end is typically 1-3 seconds, not milliseconds.

**Deep Dive 3: Erasure coding for durability at low storage overhead**

The problem: 11-nine durability with 3× replication costs 3× the raw storage. At 300 EB, that's 900 EB physical. Can we do better?

The solution: **Reed-Solomon erasure coding** (6+3 or 14+3). The object is split into 6 equal data shards. 3 parity shards are computed (each parity shard is a deterministic XOR-based combination of the data shards, per the RS polynomial math). All 9 shards are stored on 9 different storage nodes, with topology awareness: different racks, different AZs. To reconstruct the object, any 6 of the 9 shards suffice. This tolerates 3 simultaneous node failures (any combination). Storage overhead: 9/6 = 1.5× vs 3× for replication.

Walk through the durability math when asked: Annual disk failure rate ~1-2%. MTTR (time to reconstruct a shard after failure) = ~4 hours. P(data loss) = P(4 of 9 nodes fail within one MTTR window). With independent failure probabilities, this is roughly 10^-14 per object per year — 4 orders of magnitude better than the required 11 nines (10^-11).

Unprompted trade-offs to mention:
- ✅ 1.5× overhead vs 3× = 50% storage cost reduction. At Dropbox scale (Magic Pocket 14+3 = 1.21× overhead), the savings are even more dramatic — 90% cost reduction vs S3 3× replication.
- ❌ For hot reads, you must read 6 shards in parallel vs 1 replica. For small objects (< 128 KB), the per-shard overhead exceeds the data size. Solution: replicate small objects 3× and erasure-code only large objects.
- ❌ Reconstruction under failure requires 6 network reads before serving the object. This adds latency for degraded-mode reads. Detection: monitor per-object shard health; trigger background repair before clients notice.
- ✅ With 14+3 (Magic Pocket), you can tolerate 3 simultaneous failures out of 17 shards, with only 1.21× overhead — even better than 6+3.

---

### 3g. Failure Scenarios

**The five failure modes every interviewer expects you to address:**

**1. Storage node failure (a physical disk/server goes down)**

Detection: Heartbeat monitor detects missed heartbeat within 10 seconds.
Impact: Objects with shards on the failed node have 1 of 9 shards unavailable. Reads can still proceed (6 of 9 suffice). Writes can still proceed (coordinator routes around the failed node using the healthy nodes). Background repair starts immediately: for each affected object, reads 6 healthy shards, reconstructs the missing shard via RS decode, writes the reconstructed shard to a healthy replacement node.
Repair priority: objects with 2+ missing shards (consuming parity budget) are repaired first.

**2. Client network failure mid-upload**

Detection: Server-side upload session has a TTL (7 days). Client detects connection drop on its side.
Mitigation: Resumable upload protocol. Client sends `GET /upload?session_id=X` to query how many bytes/chunks the server has acknowledged. Resumes from the last ACKed position. No data re-uploaded. Session state stored in Redis (survives API server restarts) or in the metadata DB.

**3. Metadata service database outage**

Detection: Error rate spike on metadata service; health checks fail.
Impact: All operations requiring metadata (every upload, download that is a CDN miss, folder listing) fail. CDN-served files (cache hits) continue to work since they bypass the metadata service.
Mitigation: For Google Drive, Spanner replicates across 5 nodes with Paxos quorum — a single zone failure doesn't cause an outage. For Dropbox MySQL: ZooKeeper-managed failover promotes a read replica to primary within ~10 seconds. Circuit breaker at the API Gateway returns 503 with `Retry-After` during the failover window. Writes fail gracefully; clients surface "syncing..." state and retry with exponential backoff.

**4. Duplicate upload commit (network retry causes the same file to be committed twice)**

Detection: Idempotency key collision on the upload session.
Mitigation: Upload sessions have a client-generated UUID as the session ID. The commit endpoint is idempotent on session_id. The second commit attempt finds the session already in `committed` state and returns the already-committed file record (HTTP 200 with the same response) without creating a duplicate.

**5. Cache failure (Redis cluster goes down)**

Impact: All ACL checks and metadata reads fall back to the primary database (Spanner or MySQL). The system degrades gracefully — it still works, just at reduced performance. The primary database sees 5-10× more load.
Mitigation: Circuit breaker on the cache layer prevents cascade; the primary database is sized to handle moderate cache-miss storms. Redis auto-heals (Cluster failover < 30 seconds). Per-instance LRU in-process cache (second level) absorbs the most-hot entries even without Redis.

**Senior framing for failure scenarios:** Frame every failure answer with three parts: detection, isolation (how does the failure not cascade?), and recovery. Mention that failures in storage systems should be graceful degradation, not binary up/down. CDN hits surviving a metadata outage is a concrete example of graceful degradation that signals deep thinking.

---

## STEP 4 — COMMON COMPONENTS

Every component below appears across multiple (or all) problems. Know why it's there, how it's configured, and what breaks if you remove it.

---

### Content-Addressed Blob Store (SHA-256 as Key)

**Why used:** Storing blobs keyed by their SHA-256 hash gives you deduplication, integrity verification, and immutable CDN URLs all for free. Two users uploading the same photo? One blob stored, two metadata records pointing to it, reference count = 2. The CDN URL (`/blobs/{sha256}`) never changes for the same content, so you can set `Cache-Control: immutable, max-age=31536000` and never need to invalidate.

**Key config:** Blobs stored at path `blobs/{sha256[0:2]}/{sha256[2:4]}/{sha256}` — the two-level directory prefix prevents any single directory from having too many entries (avoids filesystem performance cliffs). Reference count in the metadata table tracks how many file versions reference each blob. GC runs after `ref_count` reaches zero and a 7-day grace period elapses.

**What breaks without it:** Without content-addressed storage, you need per-user per-file copies. Storage costs multiply by the duplicate rate (15-30% wasted). CDN URLs are mutable (need invalidation on file update). Integrity checks require a separate audit process rather than being intrinsic to the storage key.

**Appears in:** All four problems. Drive uses 256 KB–4 MB chunks keyed by SHA-256. Dropbox uses 4 MB blocks. S3 uses object ETags (MD5 or SHA-256). Photo storage uses full-file SHA-256 for photo blobs.

---

### Separation of Metadata Layer from Blob Storage Layer

**Why used:** Metadata operations (list a folder, check permissions, get file size) are tiny, frequent, and need strong consistency. Blob operations (upload 3 MB of bytes, download a video) are large, less frequent, and tolerant of eventual consistency via CDN. If you route blob bytes through your metadata service, you have created a giant bottleneck. Separating them lets each layer scale independently and be sized appropriately.

**Key config:** The metadata service stores location pointers (bucket + object key, or chunk manifest of SHA-256s) that tell clients where to retrieve bytes. The actual bytes never touch the metadata service. Presigned URLs are the bridge: the metadata service authorizes and generates the URL; the client uses it directly against the blob store.

**What breaks without it:** Your metadata service becomes the I/O bottleneck for all file transfers. At 334 Gbps peak egress (Google Drive), your metadata servers would need network cards larger than the entire datacenter. More importantly, blob bytes are inherently variable-size and unpredictable in rate; metadata requests are small and predictable. Mixing them makes both harder to reason about and scale.

---

### Presigned URLs for Direct Client-to-Storage Access

**Why used:** Clients upload and download bytes directly to/from the blob store (S3/GCS/Magic Pocket) without routing through your API servers. This keeps API servers stateless, lightweight, and scalable. API servers only generate and validate metadata; they never carry blob traffic.

**Key config:** The API server generates a presigned URL scoped to a specific operation (GET or PUT), a specific resource (bucket + key or sha256), and a time window (typically 15 minutes to 1 hour). The URL is signed with the server's secret key (HMAC-SHA256 for S3's SigV4, or a JWT-based signature for others). The blob store validates the signature on receipt — no callback to the API server.

**What breaks without it:** Without presigned URLs, your API servers must proxy all blob bytes. This creates a massive I/O bottleneck, adds latency (extra network hop), and dramatically increases your API fleet's networking costs. At 334 Gbps peak (Drive), you'd need hundreds of 25 Gbps servers just for proxying — a configuration that makes no sense given the tiny computation required.

---

### CDN for File and Photo Delivery

**Why used:** The majority of file reads are for the same popular content (a shared document sent to 10,000 people, a viral photo). CDN edge PoPs cache the bytes near users, delivering sub-30ms latency and avoiding origin egress costs. Because blobs are content-addressed (immutable), CDN caches can set `max-age=31536000` (1 year) with no risk of serving stale data.

**Key config:** Cache key is the SHA-256 hash of the content (or the versioned object key). `Cache-Control: public, max-age=31536000, immutable` for content-addressed blobs. For mutable access-controlled files, use short-TTL signed CDN URLs (15 min - 1 hour) that encode an authorization token — CDN serves the bytes but validates the signature. CDN offload target: 70-90% of reads served from edge.

**What breaks without it:** Without CDN, every download hits your origin blob store. At 34,700 photo views/second peak (photo storage), your origin would need to serve 55+ Gbps of thumbnail traffic 24/7 with p99 < 100 ms. This is expensive and fragile. CDN reduces origin egress to ~5 Gbps for the same workload.

---

### Kafka for Async Post-Upload Event Pipeline

**Why used:** After a file is durably committed to the metadata store, multiple downstream systems need to react: thumbnail generation, virus scanning, search indexing, face detection, notification fanout. You do not want to do these synchronously in the upload path because (a) they take too long, (b) failure in any one should not cause the upload to fail, (c) they scale at different rates.

**Key config:** One Kafka topic per major event type (`photo.uploaded`, `file.created`, `object.deleted`). Each downstream service is an independent consumer group — they make progress independently and can be scaled independently. Kafka retains events for 7 days, so slow consumers can catch up without data loss. Event payloads are small (photo_id, sha256, user_id, exif) — not the blob bytes.

**What breaks without it:** Synchronous processing of thumbnails, face detection, and virus scanning in the upload path would add 5-60 seconds to every upload response. Users would see spinning upload indicators. More importantly, a GPU inference job failing would cause the upload to fail, coupling infrastructure concerns to user-facing reliability in an unacceptable way.

---

### Redis for Hot-Path Metadata Caching

**Why used:** The metadata store (Spanner, MySQL) is the strong-consistency source of truth. But at 52,000 metadata reads/second peak (Drive), most of those reads are for the same popular resources: hot shared folders, frequently-accessed file metadata, ACL decisions for users with many active sessions. Redis absorbs 70-90% of these reads.

**Key config:** ACL cache: key = `acl:{user_id}:{resource_id}:{action}`, TTL = 60 seconds, value = ALLOW/DENY boolean. Folder listing cache: key = `listing:{folder_id}:{page_token}`, TTL = 30 seconds. Quota cache: key = `quota:{user_id}`, TTL = 30 seconds (or updated on write via write-through). On permission change, the affected ACL keys are explicitly invalidated (not TTL-expired — that 60-second staleness could mean a revoked user still has access for a minute, which is acceptable in practice but must be stated).

**What breaks without it:** Without Redis, every API call that touches a popular shared folder hits Spanner or MySQL directly. At 52,000 reads/second with p99 < 200 ms requirements, your metadata DB cluster would need to be ~10× larger. Worse, Spanner's cost scales with transaction rate — Redis reduces your Spanner bill by the cache hit rate (~80%).

---

### Async Post-Upload Processing Workers

**Why used:** All heavy processing that is not on the critical path of "user receives upload confirmation" goes here. The rule is: if the user doesn't need the result to confirm the upload succeeded, it's an async worker.

**Key config per problem:**
- Google Drive workers: virus scanner (ClamAV + ML scanner), thumbnail/preview generator, full-text search indexer (Tesseract OCR for images/PDFs, Apache Tika for documents).
- Dropbox workers: virus scanner (async), preview generator (images, PDFs, code).
- S3 workers: lifecycle manager (triggers storage class transitions Standard → Glacier), cross-region replication service, event notification publisher.
- Photo storage workers (the richest pipeline): Thumbnail Generator (libvips, 3 variants), Face Detection (MTCNN + FaceNet embeddings), Face Clusterer (DBSCAN + FAISS ANN per user), Scene Labeler (ResNet-50 or ViT), EXIF Extractor (ExifTool/libexif), Geo Tagger (GPS coordinates → reverse geocode → place name). All workers run in parallel from the same Kafka event. SLA: thumbnails < 5 seconds, face detection < 60 seconds.

**What breaks without it:** Synchronous virus scanning before a user can access their uploaded file would mean users wait 5-30 seconds staring at a spinner. OCR of a 100-page scanned PDF would timeout the HTTP connection. Face detection at 3,471 photos/second would require GPU capacity directly in the upload path, with no ability to scale it independently. Async separation is what makes each concern independently operable and independently scalable.

---

### Resumable / Multipart Upload Protocol

**Why used:** Large file uploads over a cellular connection that fails halfway through would be catastrophic without resumability. A 2 GB file at 10 Mbps takes 27 minutes to upload — too long to assume a perfect connection. Resumable upload means after reconnecting, you continue from where you left off.

**Key config:**
- Session initiation: client calls `POST /upload/initiate` with file hash, size, and chunk manifest. Server returns session ID. Session state stored in Redis with 7-day TTL.
- Chunk upload: client uploads chunks one at a time (or in parallel) with `PUT /upload/{session_id}/chunk/{index}`. Server ACKs each chunk. Server tracks received chunks bitmask.
- Recovery: client sends `GET /upload?session_id=X` to discover which chunks the server has. Resumes from first missing chunk.
- Commit: client calls `POST /upload/{session_id}/commit` after all chunks are ACKed. Server atomically creates the metadata record.

**What breaks without it:** Without resumable uploads, any network interruption on a large file forces a full re-upload from byte 0. For a 1 GB file on a 4G connection, even one dropped packet causes a restart. The user experience is terrible and bandwidth is wasted re-uploading bytes the server already has. You also can't achieve the dedup check ("which chunks do you already have?") without the pre-upload chunk manifest exchange that is part of the resumable protocol.

---

### Soft Delete + Versioning

**Why used:** Users frequently delete files accidentally. Versioning allows recovery of prior states. Soft delete (move to trash, retain for N days) prevents permanent data loss from accidental deletes. Together they are a non-negotiable user trust feature.

**Key config:**
- Soft delete: `is_trashed = TRUE` flag set on the metadata record, with `trashed_at` timestamp. A GC job runs nightly and permanently deletes metadata + decrements blob ref_counts for records where `trashed_at < now() - 30 days`.
- Versioning: each upload commit creates a new version row in the `file_versions` table. Only the latest version has `is_head = TRUE`. Clients can request a prior version by version_id. Old versions are pruned based on the retention policy (30 days for free tier, unlimited for paid, 180 days for Dropbox Business).
- Lifecycle policy (S3): `DELETE` without a version ID creates a delete marker (the object is "invisible" but all versions are still present). Specifying a version ID in `DELETE` permanently removes that version. Lifecycle rules can auto-expire non-current versions after N days.

**What breaks without it:** Without soft delete, any accidental delete is permanent data loss — and users will blame the storage system regardless of whose fault it was. Without versioning, overwriting a file overwrites it forever. These are baseline table-stakes features that every modern storage system must have, and interviewers will specifically ask how you handle them.

---

### Strong Consistency for Metadata; Eventual for Blob Bytes

**Why used:** This is the key architectural decision that makes large-scale storage systems economically viable. Metadata is tiny in size but must be authoritative — two clients must never see conflicting states about which version of a file is current or who has permission to access it. Blob bytes are large and can be cached aggressively because they are immutable (content-addressed).

**Key config:** Metadata writes go through a strongly-consistent transaction (Spanner external consistency, MySQL with synchronous replication, or Cassandra with QUORUM writes). Blob reads go through CDN with `Cache-Control: immutable` — the CDN serves bytes without consulting the metadata layer (as long as the URL is valid). For private files, short-TTL signed CDN URLs are the bridge: the metadata layer issues the signed URL only after an ACL check; the CDN delivers bytes without its own ACL check but validates the signature.

**What breaks without it:** If you apply strong consistency to blob delivery, you can never cache. Every read requires a consistency check against the metadata store. At 34,700 photo views/second, that means 34,700 metadata reads/second just for photo delivery — blowing your metadata store capacity instantly. Conversely, if you apply eventual consistency to metadata, clients on different devices can see different file states, causing split-brain and sync conflicts that are near-impossible to resolve after the fact.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Google Drive

**What makes it unique:**

Google Drive requires a **globally consistent file hierarchy with cascading permissions**. The folder tree can be arbitrarily deep; sharing a top-level folder with a user means that user implicitly has access to all contents, nested to any depth. This requires a permission model (Zanzibar ReBAC — relationship-based access control tuples) that is fundamentally different from the simple owner/ACL tables used by Dropbox, S3, and photo storage.

The second unique element is **Spanner as the metadata store**. While Dropbox uses MySQL sharded by namespace, and photo storage uses Cassandra for time-series photo metadata, Drive needs globally distributed, externally consistent (TrueTime-based) transactions because file system operations like "move a subtree of 10,000 files atomically" require ACID transactions spanning potentially many rows across potentially many regions simultaneously.

The third unique element is **deferred quota accounting via Redis accumulators**. Each 4 MB chunk upload would require a Spanner write to decrement the user's quota. At 3,500 uploads/second with multi-chunk files, that's tens of thousands of Spanner quota writes per second. Instead, quota deltas accumulate in Redis and are flushed to Spanner every 5-30 seconds in batches.

**Key differentiator in two sentences:**

Google Drive's defining architectural choice is **Spanner + Zanzibar**: the global file hierarchy with cascading permissions requires externally consistent distributed transactions (Spanner/TrueTime) and a relationship-based access control graph (Zanzibar) to express folder-level permissions that cascade to all descendants. Neither appears in the other three storage problems — they are the price of supporting true global strong consistency and expressive enterprise sharing semantics.

**Different decisions vs the other problems:**

- Uses Spanner instead of MySQL/Cassandra for metadata (most expensive but only correct option for global strong consistency + ACID subtree operations)
- Uses Zanzibar ReBAC instead of simple ACL table (necessary for hierarchical permission inheritance)
- Uses change tokens from Google's Changes API (opaque cursor) rather than exposing the internal sequence number to clients
- Supports first-party Google Docs/Sheets/Slides collaborative editing (OT-based, out of scope for the design itself but must be mentioned as a black-box integration)
- Quota shared across Drive + Gmail + Photos (requires cross-product quota accounting coordination)

---

### Dropbox

**What makes it unique:**

Dropbox is the only problem in this folder that focuses primarily on **bandwidth minimization**. Block-level delta sync is the core innovation: on a 100 MB file where 12 MB changed, only the 3 changed 4 MB blocks are uploaded — 88% bandwidth savings. The desktop client maintains a local SQLite database of block hashes from the previous sync, so it can detect changed blocks without a server round-trip.

The second unique element is **LAN sync**: on a local network, the desktop client uses mDNS to announce its presence and discover peers, then transfers needed blocks directly over TCP without touching Dropbox's servers. For an office with 50 employees all syncing the same 500 MB file, LAN sync reduces Dropbox's bandwidth cost from 25 GB to ~500 MB — a 98% reduction.

The third unique element is **Magic Pocket**, Dropbox's proprietary block storage system built in 2016 to replace AWS S3. Magic Pocket uses 14+3 erasure coding (17 shards, any 14 sufficient for reconstruction) for ~1.21× storage overhead — far better than S3's 3× replication. This reduced Dropbox's storage costs by ~90% at 500 PB+ scale. It is the most consequential infrastructure decision of any problem in this folder.

**Key differentiator in two sentences:**

Dropbox's defining technical choice is **block-level delta sync + Magic Pocket**: the 4 MB fixed block abstraction enables precise bandwidth-minimal uploads and the LAN peer-to-peer transfer capability; Magic Pocket is Dropbox's most strategically important infrastructure decision — replacing AWS S3 with a custom erasure-coded system (14+3 = 1.21× overhead vs 3× for replication) to reduce storage costs by 90%, a move that saved hundreds of millions of dollars at their 500 PB+ scale.

**Different decisions vs the other problems:**

- Desktop client has a local SQLite DB (`~/.dropbox/config.db`) with per-file block manifests — the only problem where significant intelligence lives on the client
- Fixed 4 MB block size (vs Drive's variable 256 KB–4 MB) for simplicity and consistency
- MySQL sharded by namespace_id (not Spanner, not Cassandra) — because namespace operations are naturally bounded to one namespace, and MySQL is simpler/cheaper at this scope
- Memcached (not Redis) as primary metadata cache — reflects Dropbox's historical technology choices; Memcached is slightly more efficient for pure get/set caching with no persistence requirement
- Conflict resolution creates a "conflicted copy" file in the same folder (named `filename (Bob's conflicted copy YYYY-MM-DD)`) — no merge attempted for binary files; simpler but more user-visible than Drive's approach

---

### S3 Object Store

**What makes it unique:**

S3 is not a file system and not a sync product — it is a **flat-namespace API-driven object store** used by applications as infrastructure, not by end users directly. There are no folders (only key prefixes that simulate them), no sync, no collaborative editing. The complexity instead comes from: operating at truly staggering scale (trillions of objects, 3.5 million PUTs/second), supporting multiple storage tiers with automatic lifecycle transitions, and providing a stateless authentication model (SigV4) that requires no central session lookup.

The second unique element is the **IAM + SigV4 authorization layer**. Unlike the other three problems (which serve known users with simple ACL checks or Zanzibar tuples), S3 must evaluate complex IAM policies with principal, action, resource, and condition elements on every single request, authenticate using HMAC-SHA256 (SigV4) with no session state, and support cross-account access where a user in one AWS account accesses a bucket owned by another account.

The third unique element is the **storage class lifecycle tiering**: Standard → Standard-IA → Glacier → Deep Archive, with automatic transitions based on object age, prefix, and tag conditions. This requires the metadata layer to track storage class per object and a Lifecycle Manager that continuously scans the metadata index for objects meeting transition criteria.

**Key differentiator in two sentences:**

S3's defining complexity is its **IAM + SigV4 authorization layer combined with storage class tiering**: unlike the other three problems which serve known users with ACL checks, S3 must evaluate complex IAM policies (principal, action, resource, condition) per request and authenticate via stateless HMAC signatures that need no central lookup, while simultaneously managing objects across multiple storage tiers (Standard through Deep Archive) with automatic lifecycle transitions — making it as much an identity and authorization system as a storage system.

**Different decisions vs the other problems:**

- No user concept at the storage layer — only IAM principals (users, roles, service accounts)
- SigV4 authentication instead of OAuth 2.0 / JWT — entirely stateless, no session table needed
- Flat namespace (bucket + key) with delimiter-based prefix simulation of folders — no actual tree structure
- Strong read-after-write consistency for ALL operations including list (since Nov 2020) — notably harder than eventual consistency
- Multipart upload allows up to 10,000 parts at 5 MB–5 GB each for objects up to 5 TB — the largest supported object size of any problem in this folder
- Object Lock (WORM compliance mode) — prevents deletion or overwrite even by the bucket owner for a specified retention period; mandatory for compliance use cases
- Event notifications (SNS/SQS/Lambda triggers on object events) — the only problem in this folder with first-class event-driven application integration built into the storage API

---

### Photo Storage

**What makes it unique:**

Photo Storage is the only problem in this folder with a **significant ML processing pipeline** as a first-class requirement. Every uploaded photo goes through up to 6 parallel async processing stages: thumbnail generation, face detection (MTCNN), face embedding (FaceNet, 128-dimensional vectors), face clustering (DBSCAN + FAISS ANN per user), scene classification (ResNet-50 or ViT for labels like "beach," "food," "sunset"), EXIF extraction (camera model, GPS, timestamp), and GPS reverse geocoding (coordinates → place name). All must complete within 60 seconds of upload. This requires a dedicated GPU inference fleet.

The second unique element is **FAISS per-user face index**. Face embeddings are deeply personal — you do not want cross-user face search. Each user's face embeddings are stored in a per-user FAISS index (Facebook AI Similarity Search) that supports approximate nearest neighbor search in 128-dimensional space. When a new photo is processed, its face embeddings are compared against the user's existing FAISS index; similar embeddings merge into existing "person clusters"; dissimilar embeddings start new clusters.

The third unique element is the **Cassandra TimeWindowCompactionStrategy** for the photo timeline. Photos are almost always queried by recency (scroll your photo library — you start at the newest). TWCS compacts SSTables within 7-day time windows, keeping recent data in hot SSTables and archiving old data into large, infrequently-touched SSTables. This prevents the write amplification that would occur with SizeTieredCompactionStrategy on a time-series workload.

**Key differentiator in two sentences:**

Photo Storage's defining complexity is its **multi-stage ML pipeline**: unlike the other three storage problems which focus on file transfer, sync, and durability, photo storage runs a parallel async ML pipeline on every uploaded photo — face detection (MTCNN), embedding (FaceNet, 128-dim), clustering (DBSCAN + FAISS per user), scene classification (ResNet-50/ViT), and GPS geocoding — requiring a dedicated GPU inference fleet (Triton Inference Server), a per-user vector index (FAISS), and specialized data models (128-dim embeddings in Cassandra, Elasticsearch for geospatial + label search) that no other problem in this folder has.

**Different decisions vs the other problems:**

- Cassandra (not Spanner, not MySQL) for photo metadata — write-heavy time-series (100 M photos/day = 1,157 writes/second steady) with partition-by-user and clustering-by-date fits Cassandra's model perfectly
- TWCS (TimeWindowCompactionStrategy) — critical config decision for time-series data in Cassandra; eliminates compaction amplification on insert-heavy workloads
- FAISS per-user vector index — not a global vector DB (privacy isolation), stored as a file in blob store, loaded on demand
- PostgreSQL for albums and sharing (not the main metadata store) — low cardinality, relational ACL queries benefit from SQL; photo metadata volume (4 trillion rows) makes PostgreSQL impractical for the main store
- libvips instead of ImageMagick for thumbnail generation — 10× less memory, 3-10× faster at high concurrency; mandatory at 10,000 thumbnail operations/second peak
- File-level SHA-256 dedup (not block-level) — photos don't share sub-block content meaningfully; block-level dedup adds metadata overhead with near-zero additional savings for photos

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (2-4 sentences each; surface these within the first 20 minutes)

**Q1: How do you handle large file uploads reliably?**

**KEY PHRASE: "resumable chunk upload with server-side session tracking"**

Split the file into fixed-size chunks (4 MB is the common choice). The client initiates an upload session with the server, which returns a session ID. For each chunk, the client sends a PUT with the session ID and chunk index; the server ACKs each chunk and tracks received indices in the session state. On network failure, the client queries the server for its resume offset and re-uploads only the missing chunks.

---

**Q2: How do you achieve 11-nine durability without 3× replication?**

**KEY PHRASE: "Reed-Solomon erasure coding at 1.5× overhead instead of 3×"**

Erasure coding splits each object into k data shards and m parity shards using Reed-Solomon polynomials. With 6+3 coding (6 data + 3 parity = 9 shards), any 6 of the 9 shards can reconstruct the full object, tolerating 3 simultaneous node failures. Shards are placed on different nodes in different racks and AZs for topology isolation. Storage overhead is 9/6 = 1.5× vs 3× for full replication — at 300 EB total data, this saves 150 EB of physical storage.

---

**Q3: How does deduplication work at scale?**

**KEY PHRASE: "content-addressed storage with SHA-256 hash as the storage key"**

Before uploading any chunk or photo, the client computes its SHA-256 hash and asks the server "does this hash exist?" — a single O(1) key lookup in the chunk metadata table. If the hash exists (the content is already stored), the client skips the byte upload entirely and just references the existing blob in the new file's version record. The reference count on the blob is incremented. This makes dedup constant-time regardless of total storage size, and reduces storage by 15-30% depending on workload.

---

**Q4: How do clients sync file changes across devices?**

**KEY PHRASE: "cursor-based change log with hybrid push + pull notification"**

The server maintains an append-only change log per user/namespace, where each file create/modify/delete/rename appends one row with a monotonically increasing sequence number. Clients save their last-seen sequence number as a cursor. On reconnect or when pushed a "you have changes" signal, the client fetches all changes since its cursor in a single range scan. The push signal (WebSocket/SSE/FCM) is lightweight ("something changed") — the actual change data is always pulled via the cursor API, not pushed. This handles offline clients perfectly: they always resume from their saved cursor.

---

**Q5: How does access control work for files shared with thousands of users?**

**KEY PHRASE: "ACL check cached in Redis with 60-second TTL, evaluated via permission table or Zanzibar graph"**

For simple cases (Dropbox, S3, photo storage), a single table with (resource_id, user_id, permission_level) rows is sufficient. For Google Drive's cascading folder permissions, a Zanzibar-style relationship-based graph is needed: permission tuples like "user:alice can view folder:X" are stored in Spanner, and a check for "can alice view file:Y inside folder:X" traverses the path from Y up to X. Both models cache the check result in Redis keyed by (user_id, resource_id, action) with a 60-second TTL, so the permission evaluation is done at most once per minute per user-resource pair.

---

**Q6: How do clients directly upload to blob storage without going through your API servers?**

**KEY PHRASE: "presigned URL — a time-limited, scope-limited, cryptographically signed URL"**

The API server generates a presigned URL after verifying the user has permission to write. The URL encodes the target bucket, key, expiration time, allowed operations, and is signed with HMAC-SHA256 using the server's secret key. The client sends the file bytes directly to the blob store endpoint using this URL. The blob store validates the HMAC signature on arrival without calling back to the API server — stateless validation. The URL expires after 15 minutes to 1 hour, limiting the blast radius of a leaked URL.

---

**Q7: How does CDN caching work for user files that may be private?**

**KEY PHRASE: "separate content-addressed immutable URLs (public) vs signed expiring CDN URLs (private)"**

Public shared files can use long-TTL content-addressed CDN URLs (`max-age=31536000, immutable`) because the cache key is the SHA-256 hash — the same bytes always map to the same URL and are served to anyone with the URL. Private files use signed CDN URLs with short TTLs (15 minutes to 1 hour) that encode an authorization token; the CDN validates the signature but does not check ACLs independently. When a file's access control changes from private to public (or vice versa), a CDN purge is issued to clear the old policy.

---

### Tier 2 — Deep Dive Questions (why + trade-offs; expect these after your HLD)

**Q8: Why did you choose Spanner over MySQL for Google Drive's metadata?**

**KEY PHRASE: "atomic cross-shard ACID transactions required for quota + hierarchy operations"**

Google Drive needs to atomically commit a file upload: decrement the user's quota, create the file_versions record, update the fs_nodes record, and mark the upload session committed — all in one transaction. With MySQL sharded by user_id, these operations likely hit multiple shards (quota is on one shard, the file being placed in a shared folder is on another). Cross-shard transactions in MySQL require a two-phase commit protocol that is complex, error-prone, and slow. Spanner handles distributed transactions natively with TrueTime-based external consistency. The trade-off is cost: Spanner is significantly more expensive than MySQL per transaction, and operationally complex. For Dropbox, where namespace-based sharding keeps all namespace operations on one shard, MySQL is the correct (and cheaper) choice.

---

**Q9: Walk me through exactly what happens when a storage node fails during a write.**

**KEY PHRASE: "write coordinator requires quorum; erasure coding allows reconstruction from any 6 of 9 shards"**

For a 6+3 erasure coded object, the write coordinator (one of the storage nodes in the write plan) sends shards to all 9 nodes simultaneously. If one node is unreachable, the coordinator fails the write to that node but continues if at least 6 + 1 (6 data + at least partial parity) acknowledgments are received. The write is committed to the metadata index with the available shards' locations. In the background, the repair service detects the missing shard (via the object's health score in its monitoring system), reads any 6 available shards, runs Reed-Solomon decode to reconstruct all 9, and writes the reconstructed shard to a healthy replacement node. Priority: objects with fewer healthy shards are repaired first to minimize data loss risk.

---

**Q10: How does block-level delta sync work in Dropbox when a 1-byte insertion at the start of a 100 MB file shifts all block boundaries?**

**KEY PHRASE: "fixed-size chunking worst case; mitigated in practice by file type and usage patterns"**

With fixed-size 4 MB blocks, a 1-byte insertion at the start shifts every subsequent block boundary, making all 25 blocks "new" — forcing a full re-upload. This is the fundamental disadvantage of fixed-size chunking compared to Content-Defined Chunking (CDC with Rabin fingerprinting), which finds natural chunk boundaries and is insertion-resilient. Dropbox chose fixed-size for simplicity and because the files where users make insertions (text files, code) are typically small (< 1 MB), making the full re-upload negligible. Large files (videos, archives) are either replaced wholesale or linearly appended — never byte-inserted at arbitrary offsets. For the adversarial case of large text files with insertions, Dropbox recommends using Dropbox Paper (a first-party collaborative editor) instead of the file sync system.

---

**Q11: How do you prevent the chunk reference counting table from becoming a bottleneck at 100 million uploads/day?**

**KEY PHRASE: "shard the chunks table by SHA-256 prefix; batch existence checks; Redis hot-chunk cache"**

The chunks/blocks table is sharded by the first 2 hex digits of the SHA-256 hash — this gives 256 shards with perfectly uniform distribution (SHA-256 output is uniformly distributed). The `checkChunks` API accepts up to 1,000 hashes per call, batching what would be 1,000 individual lookups into one RPC with a single IN query on the primary key. For extreme hot chunks (a ubiquitous file like a popular JavaScript library that thousands of users are all simultaneously checking), Redis caches existence lookups with a 60-second TTL — these checks return in under 1 ms without hitting the database. Reference count increments on commit are batched in the commit transaction: 250 ref_counts updated in one transaction rather than 250 individual transactions.

---

**Q12: How do you handle a user that has been offline for 3 months and tries to sync?**

**KEY PHRASE: "cursor expiry triggers full re-sync; change log has a configurable retention window (30-180 days)"**

The change log retains entries for a configurable window (90 days for Drive, 180 days for Dropbox Business). When a client presents a cursor older than this retention window, the server returns a special `FULL_SYNC_REQUIRED` response instead of a change delta. The client then fetches the complete file list via the standard paginated listing API and rebuilds its local state from scratch. This is expensive but very rare in practice — most users sync at least once a month. To reduce the pain, the full re-sync uses the cursor-based listing API (not a dump of all metadata at once) so it is paginated and resumable.

---

**Q13: How does S3 achieve strong read-after-write consistency for all operations since November 2020?**

**KEY PHRASE: "all reads route through the metadata layer as the single serialization point"**

Pre-2020, S3 was eventually consistent for overwrite PUTs and DELETEs because some reads could be served directly from storage nodes without consulting the metadata index. Post-2020, S3 made the metadata index the single serialization point for all operations: every GET request must consult the metadata index to retrieve the object's current storage location, which guarantees the read sees the latest committed version. There is no read path that bypasses the metadata index and serves stale data from storage nodes. The trade-off is latency: every read has an additional metadata lookup (which is cached, but still adds a potential latency step). This is why Amazon specifically calls out the Nov 2020 change — it required a non-trivial architectural change to the read path.

---

### Tier 3 — Staff+ Stress Test (reason aloud; expect these for L5+/Staff candidates)

**Q14: A viral piece of content is suddenly shared with 50 million users simultaneously. How does your system handle the traffic spike without falling over?**

The key insight is that this spike hits three different layers simultaneously: the metadata layer (50 million ACL checks), the blob store (50 million GET requests), and the CDN (50 million initial cache misses if the content is new). The CDN should absorb 90%+ of the read traffic once warmed. For the initial stampede (CDN cold), a CDN cache-stampede prevention technique is critical: probabilistic early expiration (re-populate the cache when TTL < random(0, 30s)) prevents the thundering herd where millions of CDN edge requests simultaneously hit origin. For ACL: cache the check result per (user_id, resource_id) in Redis with 60-second TTL — each user checks once, then 60 seconds of caching. For metadata read throughput: the Notification Service fanout uses Kafka-backed controlled rate publishing (the share event is published once; the fan-out to 50M recipients is rate-limited at the Kafka consumer level to avoid overwhelming the notification fleet). For the blob store origin: pre-warm CDN by explicitly pushing the object to the top-50 CDN PoPs upon detecting a share event to > 1M recipients.

---

**Q15: Walk me through the privacy implications of content-addressed storage (CAS). If user A and user B both upload the same photo, their blobs share a SHA-256. Does user A's deletion expose user B's photo? Can law enforcement get user B's data via a subpoena for user A?**

This requires careful separation of concerns. The CAS blob store is a pure byte store — it has no concept of users or ownership. The `blobs` table maps sha256 → physical storage location and a reference count. The `photos` (or `file_versions`) table maps user_id + photo_id → sha256 (pointing into the blob store). User A's deletion sets `is_deleted = true` on their `photos` record and queues a ref_count decrement after the 60-day grace period. If user B still has a `photos` record pointing to the same sha256, the ref_count stays above 0 and the blob is NOT deleted — user B's photo is unaffected. For law enforcement: a subpoena for user A's data returns the bytes associated with user A's `photos` records. The system does not reveal that user B also has a record pointing to the same blob — user isolation is enforced at the metadata layer, not the blob layer. The blob itself is just bytes identified by a hash; it carries no user identity. The legal access is scoped to the requesting user's metadata records.

---

**Q16: Design the face clustering system for photo storage at 2 billion users and 4 trillion photos. What are the scalability limits of per-user FAISS, and what's your fallback?**

Per-user FAISS is the right isolation model (no cross-user face search is possible, which is a hard privacy requirement). The scalability question is: how large does a single user's FAISS index get? A power photographer with 500,000 photos, averaging 3 faces per photo, has 1.5 million face embeddings × 128 floats × 4 bytes = ~768 MB per user. This fits in memory on a worker node and FAISS IVF (Inverted File Index) quantization can reduce it 4-8×. For users with tens of millions of photos (Google Photos power users), the FAISS index might be multi-GB. Mitigation: store the FAISS index as a file in blob store; load it on-demand into a worker node's memory for face processing. Use mmap to load only the relevant IVF partition. For extreme cases (> 5 million faces), switch from flat FAISS to IVF-PQ (Product Quantization), which reduces memory 32-64× with ~5% recall penalty. The fallback for truly extreme users is approximate clustering with periodic full re-clustering as a batch job rather than incremental addition.

---

**Q17: How would you design S3 to handle a customer whose bucket receives 100 million PUTs/second to keys with the same prefix (e.g., all keys start with "logs/2026/")?**

This is the classic S3 hot-prefix problem. The root cause: all keys with the same prefix hash to a narrow region of the consistent hash ring, overloading those ring segments. AWS's documented solution is a combination of three mechanisms. First, automatic prefix partition splitting: the metadata index shard for the hot prefix range is automatically split into sub-partitions when the request rate exceeds the threshold (3,500 PUTs/second per prefix per documented limit). This is analogous to DynamoDB's adaptive capacity — the hot partition is split into 10 or 100 sub-partitions, each handling a fraction of the load. Second, vnode rebalancing: the consistent hash ring's 128 vnodes per physical node allow fine-grained redistribution — hot vnodes are split and distributed to additional physical nodes. Third, client-side mitigation: randomize key prefixes. Instead of `logs/2026-04-12/event_0001`, use `a1b2/logs/2026-04-12/event_0001` where `a1b2` is a random 4-character prefix. This distributes keys uniformly across the entire ring, eliminating the hot-spot at the cost of less convenient lexicographic ordering. The architectural lesson: flat-namespace object stores must treat prefix-locality as a scalability concern, not just an organizational one.

---

## STEP 7 — MNEMONICS

**The storage design acronym: S.C.U.D.D.E.R.**

- **S**eparate metadata from blob storage (they scale differently and have different consistency requirements)
- **C**ontent-address blobs with SHA-256 (dedup, immutable CDN URLs, integrity verification, all for free)
- **U**pload chunked and resumable (4 MB chunks, session state in Redis, client resumes from last ACK)
- **D**urability via erasure coding 6+3 (1.5× overhead vs 3× for replication; tolerate 3 simultaneous failures)
- **D**ecouple heavy processing via Kafka (thumbnails, face detection, virus scan all async downstream)
- **E**vent-driven sync with cursor (append-only change log, cursor saved by client, hybrid push + pull)
- **R**ead via CDN with immutable cache headers (`max-age=31536000, immutable`; content hash as cache key)

**Opening one-liner for any storage interview:**

"The core design is two planes: a small, fast, strongly-consistent metadata plane that describes what exists and who can access it, and a large, cheap, eventually-consistent data plane that holds the bytes. Clients never route blob bytes through my API servers — they use presigned URLs to hit the blob store directly. Let me walk through how upload, download, sync, and durability work in that model."

This one-liner positions you as someone who has thought about this before, establishes the key architectural principle immediately, and sets up every subsequent discussion point naturally.

---

## STEP 8 — CRITIQUE

### Well-Covered Areas

The source material is exceptionally thorough in the following areas:

**Data models are production-grade.** The SQL schemas for Drive (Spanner), Dropbox (MySQL), S3 (custom KV pseudo-schema), and photo storage (Cassandra + PostgreSQL) are detailed enough to implement directly, including index choices, sharding keys, and constraint logic. Most interview guides give toy schemas; these are the real thing.

**Deep-dive Q&As are genuine.** The interviewer Q&A sections in each source file address the genuinely hard questions: what happens when a 1-byte insertion shifts all block boundaries, how SHA-256 hash collisions could theoretically enable unauthorized access (and why they can't in practice), the privacy implications of CAS across users, the durability math for erasure coding. These are the questions that separate Staff candidates from senior candidates.

**API design is complete.** The source material includes actual endpoint signatures, HTTP method choices, response formats, error codes, rate limiting headers, and presigned URL construction (including SigV4 canonical request construction). This level of detail lets you discuss API design with confidence.

**Technology choices are justified.** The database choice tables in each source file compare 4-6 options per problem and explain why the chosen option is right for that specific problem. Knowing that Dropbox uses Memcached (not Redis) for metadata caching and why is the kind of distinguishing detail that signals real expertise.

---

### Missing or Shallow Coverage

**Quota enforcement atomicity under concurrent uploads is underexplained.** The guide mentions deferred quota accounting via Redis accumulators, but does not fully address the race condition where a user's quota is 100 MB and they simultaneously initiate 5 uploads that together total 200 MB. The deferred model with a 5% buffer reduces but does not eliminate quota overages. Interviewers at Google/Dropbox will probe this. The complete answer involves: optimistic quota check at session initiation (using cached Redis value), deferred increment during upload, and a quota reconciliation job that charges back overages or cancels the final commit if the true quota would be exceeded.

**Exactly-once delivery for the async processing pipeline needs more depth.** The guide mentions Kafka and idempotency keys but does not fully address what happens when a thumbnail generator worker crashes after writing the thumbnail to blob store but before updating the `photos` table with the thumbnail URL. The answer requires: the Kafka offset is not committed until the worker completes the full processing step (at-least-once semantics from Kafka), and the thumbnail generation step is idempotent (computing the same thumbnail from the same SHA-256 always produces the same result — if the blob already exists in the CAS store, it's a no-op; just update the metadata).

**LAN sync security is underexplored.** The guide mentions mDNS discovery and direct TCP transfer but doesn't address the security model. On a shared WiFi network (e.g., an airport), any device can advertise itself as a Dropbox peer. The real answer: block transfers use the same block-hash-based integrity verification (SHA-256 of received bytes must match the requested block hash), so a malicious peer cannot serve corrupted data without detection. Authentication of the peer is done via a shared session token exchanged through the Dropbox server, not just mDNS — preventing man-in-the-middle block injection.

**Global secondary indexes for photo search have shallow coverage.** The guide mentions Elasticsearch for photo search but does not address how you maintain consistency between the Cassandra `photos` table and the Elasticsearch index. If the async search indexer fails after the photo metadata is written to Cassandra but before the Elasticsearch document is created, the photo is invisible in search. The complete answer requires a write-ahead log or an outbox pattern: write the photo metadata to Cassandra and simultaneously write a `search_index_pending` record; a separate indexer job reads from the pending table, indexes to Elasticsearch, and deletes the pending record. Idempotent on retry.

---

### Senior Probes (questions that will stump unprepared candidates)

1. "You said you cache ACL checks in Redis with a 60-second TTL. I revoke a user's access at time T. What is the worst-case window where they can still read the file? How would you reduce it to < 5 seconds without overloading your metadata database?"

2. "Your dedup table has a reference counting bug: sometimes ref_count goes negative. How would you detect this, and how would you fix the affected blobs without downtime?"

3. "Walk me through exactly how you would migrate Dropbox from AWS S3 to Magic Pocket for the existing 500 PB of data. You cannot have more than 1 hour of downtime."

4. "A user uploads a 4 TB file to S3. The upload succeeds and returns HTTP 200. Three days later the user does a GET and gets data corruption — some bytes are wrong. Walk me through every layer of defense that should have caught this, and explain why each layer might have failed."

5. "In photo storage, your face clustering uses DBSCAN. DBSCAN has O(n²) complexity in the worst case. A celebrity uploads 50,000 photos and has 500,000 face instances. Incremental addition of new faces to existing clusters avoids the full O(n²) re-run, but describe the failure mode where incremental assignment produces incorrect clusters and explain how you'd detect and repair it."

---

### Common Traps to Avoid

**Trap 1: Proposing 3× replication for blob durability at scale.**
Every interviewer who has worked in storage knows the math. The correct answer is erasure coding (6+3 or 14+3). If you say 3× replication, expect "how much does that cost at 300 EB?" — and the answer will be unflattering.

**Trap 2: Putting blob bytes through your API servers.**
"Clients upload to my API server, which then stores to S3" is wrong for any system that handles files > a few KB at scale. Presigned URLs and direct client-to-storage transfer is the correct pattern. Mentioning this proactively signals experience.

**Trap 3: Using a single SHA-256 check for deduplication without discussing race conditions.**
Two users simultaneously uploading the same new block: both check "does this hash exist?" → both get 404 → both upload. The second upload is a no-op (the bytes are already there; the INSERT in the chunks table uses ON CONFLICT DO UPDATE to increment ref_count atomically). This is fine in practice and you should mention it preemptively.

**Trap 4: Designing a global vector database for face search.**
Per-user FAISS is the right model for face clustering, not a global vector database. Cross-user face search is a privacy violation. A global vector DB also has scaling problems at 2 billion users × avg 50,000 faces each = 100 trillion face embeddings in a single index. Saying "I'd use Pinecone" without this reasoning signals you haven't thought through the privacy and scale implications.

**Trap 5: Forgetting soft delete and the GC grace period for blob reference counts.**
Interviewers always ask "what happens when a user deletes a file and then wants it back 3 days later?" The answer requires: soft delete (is_deleted flag, not physical delete), reference count NOT decremented until after the trash retention window, and a background GC job with a grace period before physical deletion. Candidates who say "just delete the blob when the ref_count hits zero" have forgotten that ref_count zero can be a transient state during rollback.

---

*End of Pattern 6: Storage Interview Guide*
