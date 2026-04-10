# System Design: Google Drive

---

## 1. Requirement Clarifications

### Functional Requirements

1. **File Upload & Download** — Users can upload files of any type (documents, images, videos, binaries) up to 5 GB per file. Downloads must support partial/range requests for resumability.
2. **File Organization** — Users can create folders, move files, rename files, and build arbitrary directory hierarchies.
3. **Sync Across Devices** — Changes made on one device (desktop client, mobile app, web) propagate to all other signed-in devices within seconds for small files, minutes for large files.
4. **File Versioning** — The system retains the last 100 versions of each file (30 days for free tier, unlimited for paid tier). Users can restore any prior version.
5. **Sharing & Permissions** — Files/folders can be shared with specific users (view, comment, edit roles) or via public link. Permissions cascade from parent folder to children.
6. **Collaborative Editing** — Google Docs/Sheets/Slides (first-party editors) support real-time co-editing. Third-party files use lock-based conflict resolution.
7. **Offline Mode** — Desktop and mobile clients cache selected files locally. Offline edits are queued and applied via sync when connectivity resumes.
8. **Search** — Full-text search across file names, document content (OCR for images/PDFs), and metadata tags.
9. **Trash / Soft Delete** — Deleted files move to trash, retained for 30 days before permanent deletion.
10. **Storage Quota** — Per-user quota (15 GB free, up to 2 TB paid). Quota is shared across Gmail, Drive, and Photos.

### Non-Functional Requirements

| Property | Target |
|---|---|
| Availability | 99.99% (< 52 min/year downtime) |
| Durability | 99.999999999% (11 nines) — files must never be lost |
| Read latency (metadata) | p99 < 200 ms |
| Read latency (file download, first byte) | p99 < 500 ms |
| Upload throughput | Saturate available client bandwidth |
| Sync propagation latency | p99 < 5 s for files < 1 MB |
| Consistency | Strong consistency for metadata; eventual consistency acceptable for file bytes |
| Scale | 1 billion users, 15 exabytes total storage |
| Security | Encryption in transit (TLS 1.3) and at rest (AES-256). Customer-managed keys for enterprise. |

### Out of Scope

- Real-time collaborative editing engine internals (Operational Transformation / CRDT) — treated as a black box Google Docs service
- Billing and payment processing
- Mobile OS-level sync APIs (iOS Files app integration, Android Storage Access Framework)
- Admin console for Google Workspace organizations
- Third-party OAuth app integrations (Drive API for external apps)

---

## 2. Users & Scale

### User Types

| User Type | Description | Behavior Pattern |
|---|---|---|
| **Active Free User** | Personal Gmail account, 15 GB quota | Light uploads, heavy reads, web + mobile |
| **Paid Consumer** | Google One subscriber (100 GB–2 TB) | Moderate uploads, photo/video heavy |
| **Workspace User** | Enterprise/Education seat | Heavy document collaboration, Shared Drives |
| **Power User** | Content creator, developer | Large file uploads, API usage |
| **Read-Only Viewer** | Recipient of shared link | Download only, no authentication required |

### Traffic Estimates

**Assumptions:**
- 1 billion registered users; 20% daily active (200 M DAU)
- Average user performs 10 file operations/day (mix of reads, writes, syncs)
- 5% of operations are uploads; 20% downloads; 75% metadata reads
- Average uploaded file size: 2 MB (mix of docs ~50 KB, images ~3 MB, videos ~50 MB)
- Average downloaded file size: 3 MB (reads skew toward larger media)

| Metric | Calculation | Result |
|---|---|---|
| Total daily operations | 200 M DAU × 10 ops | 2 B ops/day |
| Upload operations/day | 2 B × 5% | 100 M uploads/day |
| Download operations/day | 2 B × 20% | 400 M downloads/day |
| Metadata reads/day | 2 B × 75% | 1.5 B metadata reads/day |
| Upload ops/second (avg) | 100 M / 86,400 | ~1,157 uploads/s |
| Upload ops/second (peak 3×) | 1,157 × 3 | ~3,500 uploads/s |
| Download ops/second (peak) | 400 M / 86,400 × 3 | ~13,900 downloads/s |
| Metadata reads/second (peak) | 1.5 B / 86,400 × 3 | ~52,000 reads/s |

### Latency Requirements

| Operation | p50 Target | p99 Target | Rationale |
|---|---|---|---|
| Folder listing (< 1000 items) | 30 ms | 100 ms | Interactive UI, perceived snappiness |
| File metadata fetch | 10 ms | 50 ms | Sync client polls this frequently |
| Upload chunk ACK | 50 ms | 200 ms | Client needs fast feedback to pipeline chunks |
| Download first byte | 100 ms | 500 ms | Resumable; user tolerates slight delay |
| Search query | 200 ms | 800 ms | Async indexing; search is best-effort real-time |
| Sync notification delivery | 100 ms | 2 s | Push channel; 5 s end-to-end budget |

### Storage Estimates

| Item | Calculation | Result |
|---|---|---|
| New data uploaded/day | 100 M uploads × 2 MB avg | 200 TB/day |
| Replication factor (3 copies) | 200 TB × 3 | 600 TB raw/day |
| 10-year accumulation (raw) | 600 TB × 365 × 10 | ~2.19 EB raw |
| Deduplication savings (~20%) | 2.19 EB × 0.8 | ~1.75 EB effective |
| Versioning overhead (~15%) | 1.75 EB × 1.15 | ~2.01 EB total |
| Current Google Drive total | Public: ~15 EB (assumption) | ~15 EB |

### Bandwidth Estimates

| Direction | Calculation | Result |
|---|---|---|
| Inbound (upload, avg) | 1,157 uploads/s × 2 MB | ~2.3 GB/s = ~18.4 Gbps |
| Inbound (upload, peak) | 3,500 × 2 MB | ~7 GB/s = ~56 Gbps |
| Outbound (download, avg) | 4,630 downloads/s × 3 MB | ~13.9 GB/s = ~111 Gbps |
| Outbound (download, peak) | 13,900 × 3 MB | ~41.7 GB/s = ~334 Gbps |
| CDN offload target (~70%) | 334 Gbps × 0.3 | ~100 Gbps origin egress at peak |

---

## 3. High-Level Architecture

```
┌────────────────────────────────────────────────────────────────────────────────┐
│                              CLIENT LAYER                                      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │  Web Browser │  │ Desktop App  │  │  Mobile App  │  │  Drive API       │  │
│  │  (HTTPS)     │  │ (macOS/Win)  │  │  (iOS/Droid) │  │  (OAuth 2.0)     │  │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘  │
└─────────┼─────────────────┼─────────────────┼───────────────────┼────────────┘
          │                 │                 │                   │
          ▼                 ▼                 ▼                   ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           EDGE / API GATEWAY                                │
│   Global Load Balancer → Anycast IPs → Regional API Gateways               │
│   (TLS termination, auth token validation, rate limiting, request routing) │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
              ┌────────────────────────┼──────────────────────────┐
              ▼                        ▼                           ▼
┌─────────────────────┐  ┌────────────────────────┐  ┌────────────────────────┐
│   METADATA SERVICE  │  │   UPLOAD SERVICE        │  │   NOTIFICATION SERVICE │
│                     │  │                         │  │                        │
│  - File/folder tree │  │  - Chunk ingestion      │  │  - WebSocket / SSE     │
│  - Permissions ACL  │  │  - Resumable session    │  │  - Change feed fanout  │
│  - Version catalog  │  │  - Virus scan trigger   │  │  - Push (FCM/APNs)     │
│  - Quota tracking   │  │  - Dedup check          │  │  - Polling fallback    │
└──────────┬──────────┘  └───────────┬─────────────┘  └────────────────────────┘
           │                         │
           ▼                         ▼
┌──────────────────────┐  ┌──────────────────────────────────────────────────┐
│   METADATA STORE     │  │               BLOB STORAGE LAYER                 │
│                       │  │                                                  │
│  Spanner (global     │  │  ┌──────────────────┐  ┌──────────────────────┐  │
│  strong consistency) │  │  │  Chunk Store     │  │  CDN (Cloud CDN /    │  │
│                       │  │  │  (GCS / Colossus)│  │  Cloudflare)         │  │
│  MySQL (per-region   │  │  │  Content-addressed│  │  Edge caches for     │  │
│  replica for reads)  │  │  │  SHA-256 keyed   │  │  popular files       │  │
└──────────────────────┘  │  └──────────────────┘  └──────────────────────┘  │
                           └──────────────────────────────────────────────────┘
           │
           ▼
┌──────────────────────────────────────────┐
│          SUPPORTING SERVICES             │
│                                          │
│  ┌────────────────┐  ┌────────────────┐  │
│  │  Search Index  │  │  Virus Scanner │  │
│  │  (Meilisearch/ │  │  (async queue) │  │
│  │   custom FTS)  │  └────────────────┘  │
│  └────────────────┘                      │
│  ┌────────────────┐  ┌────────────────┐  │
│  │  Thumbnail /   │  │  Audit Log     │  │
│  │  Preview Svc   │  │  (Append-only  │  │
│  └────────────────┘  │   immutable)   │  │
│                       └────────────────┘  │
└──────────────────────────────────────────┘
```

**Component Roles:**

- **Global Load Balancer / Anycast**: Routes clients to nearest regional cluster, performs health checks, provides DDoS protection via rate limiting at the edge.
- **API Gateway**: Handles TLS termination, JWT/OAuth token validation, per-user rate limiting (1000 req/min default), and routes to correct microservice.
- **Metadata Service**: The core of Drive. Manages the virtual file-system tree (files, folders, symlinks), enforces ACL/permission checks, tracks version history, deducts quota. Stateless horizontally scalable service backed by Spanner.
- **Upload Service**: Manages the stateful upload session — assigns a session ID, accepts chunks, verifies checksums, stores to blob store, then commits to metadata store atomically. Handles resumable uploads per the Google Resumable Upload Protocol.
- **Notification Service**: Maintains persistent connections (WebSocket, Server-Sent Events) with connected clients. Consumes change events from a Kafka topic and fans out to relevant subscriber connections. For mobile offline, delegates to FCM/APNs.
- **Metadata Store (Spanner)**: Globally distributed relational database providing external consistency (TrueTime-based). Chosen because file hierarchy operations require ACID transactions spanning multiple rows (e.g., atomic move of a subtree).
- **Chunk/Blob Store (Colossus/GCS)**: Google's internal distributed file system (Colossus) or GCS for public. Files are split into 256 KB–4 MB chunks, each stored as a content-addressed blob keyed by SHA-256. Provides byte-range reads for streaming.
- **CDN**: Caches public/shared files at edge PoPs. Cache key is `(sha256_hash, byte_range)`. Dramatically reduces origin egress for popular shared documents.
- **Search Index**: Async pipeline ingests file content (text extraction, OCR), builds inverted index for full-text search. Separate from metadata store to avoid write amplification.
- **Virus Scanner**: Async worker pool consumes newly uploaded file events, scans chunks with ClamAV + proprietary ML scanner, marks files clean/quarantined in metadata.

**Primary Use-Case Data Flow (File Upload):**

1. Client splits file into 256 KB chunks, computes SHA-256 of each chunk and the full file.
2. Client calls `POST /upload/initiate` with full-file hash, size, MIME type → server returns `sessionId`.
3. For each chunk, client checks dedup: `GET /chunks/{sha256}` → if 200, skip upload (chunk exists). If 404, upload with `PUT /upload/{sessionId}/chunk/{index}`.
4. After all chunks acknowledged, client calls `POST /upload/{sessionId}/commit` → metadata service creates file record atomically, quota is decremented, version record created.
5. Notification service publishes `FILE_CREATED` event to Kafka → all connected devices of the owner (and collaborators) receive a sync notification.
6. Async: virus scan runs; thumbnail/preview generated; search indexer ingests content.

---

## 4. Data Model

### Entities & Schema

```sql
-- Users and quota
CREATE TABLE users (
    user_id         UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    email           TEXT        NOT NULL UNIQUE,
    display_name    TEXT,
    quota_bytes     BIGINT      NOT NULL DEFAULT 16106127360, -- 15 GB
    used_bytes      BIGINT      NOT NULL DEFAULT 0,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Every node in the virtual file system (file or folder)
CREATE TABLE fs_nodes (
    node_id         UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id        UUID        NOT NULL REFERENCES users(user_id),
    parent_id       UUID        REFERENCES fs_nodes(node_id), -- NULL for root
    name            TEXT        NOT NULL,
    node_type       TEXT        NOT NULL CHECK (node_type IN ('file', 'folder', 'shortcut')),
    is_trashed      BOOLEAN     NOT NULL DEFAULT FALSE,
    trashed_at      TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    modified_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- For shortcuts only
    target_node_id  UUID        REFERENCES fs_nodes(node_id),
    -- Denormalized path for fast subtree queries (materialized path)
    path_tokens     TEXT[],     -- e.g., ['root_uuid', 'folder_uuid', 'this_uuid']
    UNIQUE (parent_id, name, owner_id) -- name unique within folder per owner
);
CREATE INDEX idx_fs_nodes_parent    ON fs_nodes(parent_id) WHERE NOT is_trashed;
CREATE INDEX idx_fs_nodes_owner     ON fs_nodes(owner_id, modified_at DESC);
CREATE INDEX idx_fs_nodes_path      ON fs_nodes USING GIN(path_tokens);

-- File versions — each write creates a new version row
CREATE TABLE file_versions (
    version_id      UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    node_id         UUID        NOT NULL REFERENCES fs_nodes(node_id),
    version_number  INT         NOT NULL,
    size_bytes      BIGINT      NOT NULL,
    mime_type       TEXT,
    content_hash    BYTEA       NOT NULL, -- SHA-256 of full file
    chunk_manifest  JSONB       NOT NULL, -- ordered list of chunk SHA-256s
    -- e.g., [{"index":0,"sha256":"abc...","size":262144}, ...]
    storage_class   TEXT        NOT NULL DEFAULT 'STANDARD',
    created_by      UUID        REFERENCES users(user_id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    is_head         BOOLEAN     NOT NULL DEFAULT TRUE,
    UNIQUE (node_id, version_number)
);
CREATE INDEX idx_file_versions_node ON file_versions(node_id, version_number DESC);
CREATE INDEX idx_file_versions_hash ON file_versions(content_hash);

-- Content-addressed chunk registry (deduplication table)
CREATE TABLE chunks (
    sha256          BYTEA       PRIMARY KEY, -- 32-byte SHA-256
    size_bytes      INT         NOT NULL,
    storage_backend TEXT        NOT NULL DEFAULT 'gcs',
    bucket          TEXT        NOT NULL,
    object_key      TEXT        NOT NULL,  -- e.g., "chunks/ab/cd/abcd1234..."
    ref_count       BIGINT      NOT NULL DEFAULT 1, -- number of versions referencing this chunk
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Access control entries
CREATE TABLE acl_entries (
    acl_id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    node_id         UUID        NOT NULL REFERENCES fs_nodes(node_id),
    principal_type  TEXT        NOT NULL CHECK (principal_type IN ('user', 'group', 'domain', 'anyone')),
    principal_id    UUID,       -- user_id or group_id; NULL for domain/anyone
    role            TEXT        NOT NULL CHECK (role IN ('viewer', 'commenter', 'editor', 'owner')),
    inherited       BOOLEAN     NOT NULL DEFAULT FALSE, -- TRUE if propagated from parent
    link_share      BOOLEAN     NOT NULL DEFAULT FALSE, -- TRUE for link-based sharing
    link_token      TEXT        UNIQUE,                 -- random token for link shares
    expires_at      TIMESTAMPTZ,
    created_by      UUID        NOT NULL REFERENCES users(user_id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_acl_node        ON acl_entries(node_id);
CREATE INDEX idx_acl_principal   ON acl_entries(principal_id) WHERE principal_id IS NOT NULL;
CREATE INDEX idx_acl_link_token  ON acl_entries(link_token) WHERE link_token IS NOT NULL;

-- Change/event log for sync
CREATE TABLE change_log (
    change_id       BIGSERIAL   PRIMARY KEY, -- monotonically increasing per user
    user_id         UUID        NOT NULL REFERENCES users(user_id),
    node_id         UUID        NOT NULL REFERENCES fs_nodes(node_id),
    change_type     TEXT        NOT NULL CHECK (change_type IN
                                  ('created','modified','moved','renamed','deleted','restored','shared')),
    change_payload  JSONB,      -- diff details (old_name, new_name, old_parent, etc.)
    change_token    TEXT        NOT NULL, -- opaque cursor for client polling
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_change_log_user    ON change_log(user_id, change_id);
CREATE INDEX idx_change_log_token   ON change_log(change_token);

-- Resumable upload sessions
CREATE TABLE upload_sessions (
    session_id      UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID        NOT NULL REFERENCES users(user_id),
    target_node_id  UUID        REFERENCES fs_nodes(node_id), -- NULL for new file
    parent_id       UUID        REFERENCES fs_nodes(node_id),
    filename        TEXT        NOT NULL,
    total_size      BIGINT      NOT NULL,
    expected_hash   BYTEA       NOT NULL, -- SHA-256 of complete file
    chunks_received INT[]       NOT NULL DEFAULT '{}', -- bitmask of received chunk indices
    status          TEXT        NOT NULL DEFAULT 'in_progress'
                                CHECK (status IN ('in_progress','committed','failed','expired')),
    expires_at      TIMESTAMPTZ NOT NULL DEFAULT now() + INTERVAL '7 days',
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

### Database Choice

**Candidates:**

| Database | Pros | Cons | Fit for Drive |
|---|---|---|---|
| **Google Spanner** | Globally distributed, external consistency, SQL, horizontal scale, TrueTime | Complex to operate, expensive, Google-proprietary | Excellent: ACID across shards needed for quota+file commit atomicity |
| **PostgreSQL** | Mature, full SQL, excellent indices, familiar | Single-region primary bottleneck, sharding is manual | Good for single-region; fails at global scale |
| **CockroachDB** | Distributed SQL, Postgres-compatible, multi-region | Higher latency than Spanner for co-located queries | Good open-source alternative to Spanner |
| **DynamoDB** | Massive scale, predictable latency, serverless | No joins, limited query patterns, no strong multi-item ACID (without transactions API) | Poor: hierarchical queries require multiple round trips |
| **Cassandra** | Extremely high write throughput, tunable consistency | No joins, eventual consistency by default, range scans painful | Good only for the change_log (append-only, high write throughput) |

**Selected: Spanner (primary) + Cassandra (change_log)**

Justification:
- **Spanner** is selected for `users`, `fs_nodes`, `file_versions`, `chunks`, `acl_entries`, `upload_sessions` because: (a) atomic cross-row transactions needed when committing an upload (decrement quota + create version + mark session committed); (b) strong consistency required so two devices never see conflicting file states; (c) global distribution ensures low-latency metadata reads from any region; (d) SQL schema enables complex hierarchical queries (path traversal, permission inheritance).
- **Cassandra** is selected for `change_log` because: (a) insert-only append workload fits Cassandra's LSM-tree perfectly; (b) queries are always `WHERE user_id = X AND change_id > cursor` — single partition key; (c) 1.5 B metadata reads/day at peak would stress Spanner; (d) eventual consistency is acceptable for the sync log since clients handle gaps gracefully.
- **Redis Cluster** is used as L1 cache for hot metadata (folder listings, ACL entries) with TTL of 60 seconds. Reduces Spanner load by ~80% for popular shared folders.

---

## 5. API Design

All endpoints require `Authorization: Bearer <OAuth2_access_token>` except public link access.
Rate limits: 1000 requests/minute/user; 10 concurrent uploads/user. Headers: `X-RateLimit-Remaining`, `X-RateLimit-Reset`.

### File & Folder Operations

```
GET  /drive/v3/files
  Query: q (search query), pageSize (default 100, max 1000), pageToken, orderBy, fields
  Response: { files: [FileResource], nextPageToken }

GET  /drive/v3/files/{fileId}
  Query: fields (field mask)
  Response: FileResource { id, name, mimeType, size, md5Checksum, parents,
                           modifiedTime, createdTime, trashed, owners, permissions,
                           headRevisionId, quotaBytesUsed }

POST /drive/v3/files
  Body: { name, mimeType, parents: [folderId], description }
  Response: FileResource (201)
  -- Creates empty file or folder; use upload endpoint for file with content

PATCH /drive/v3/files/{fileId}
  Body: { name?, description?, trashed?, parents? (for move) }
  Query: addParents, removeParents (for move)
  Response: FileResource

DELETE /drive/v3/files/{fileId}
  Response: 204 No Content  -- moves to trash; hard delete requires trash=true param

POST /drive/v3/files/{fileId}/copy
  Body: { name, parents }
  Response: FileResource (201)
```

### Upload Endpoints

```
POST /upload/drive/v3/files?uploadType=resumable
  Headers: X-Upload-Content-Type, X-Upload-Content-Length, X-Upload-Content-SHA256
  Body: FileResource metadata JSON
  Response: 200, Header: Location: https://www.googleapis.com/upload/drive/v3/files?upload_id=<sessionId>
  -- Initiates resumable upload session

PUT  /upload/drive/v3/files?upload_id={sessionId}
  Headers: Content-Range: bytes {start}-{end}/{total}
  Body: chunk bytes
  Response:
    308 Resume Incomplete (Range: bytes=0-{last_received}) -- more chunks needed
    200 FileResource  -- upload complete

GET  /upload/drive/v3/files?upload_id={sessionId}
  Headers: Content-Range: bytes */{total}
  Response: 308 with Range header showing how many bytes received so far
  -- Used by client to recover from network failure and resume

POST /upload/drive/v3/files?uploadType=multipart
  Headers: Content-Type: multipart/related; boundary=...
  Body: multipart body (metadata part + media part)
  Response: FileResource (200)
  -- For small files < 5 MB, single-request upload
```

### Sharing & Permissions

```
GET  /drive/v3/files/{fileId}/permissions
  Response: { permissions: [Permission] }

POST /drive/v3/files/{fileId}/permissions
  Body: { type: "user"|"group"|"domain"|"anyone",
          role: "viewer"|"commenter"|"editor",
          emailAddress?, domain?, expirationTime?,
          sendNotificationEmail: bool }
  Response: Permission (201)
  Rate limit: 10 share operations/user/minute (anti-spam)

PATCH /drive/v3/files/{fileId}/permissions/{permissionId}
  Body: { role, expirationTime }
  Response: Permission

DELETE /drive/v3/files/{fileId}/permissions/{permissionId}
  Response: 204

-- Generate a shareable link
POST /drive/v3/files/{fileId}/permissions
  Body: { type: "anyone", role: "reader", allowFileDiscovery: false }
  Response: Permission { webViewLink: "https://drive.google.com/file/d/{fileId}/view?usp=sharing" }
```

### Versioning & Revisions

```
GET  /drive/v3/files/{fileId}/revisions
  Query: pageSize, pageToken
  Response: { revisions: [Revision { id, modifiedTime, size, md5Checksum,
                                      keepForever, published }], nextPageToken }

GET  /drive/v3/files/{fileId}/revisions/{revisionId}
  Response: Revision

PATCH /drive/v3/files/{fileId}/revisions/{revisionId}
  Body: { keepForever: bool }
  Response: Revision  -- pin a version to prevent auto-deletion

DELETE /drive/v3/files/{fileId}/revisions/{revisionId}
  Response: 204  -- cannot delete head revision
```

### Change Tracking (Sync Protocol)

```
GET  /drive/v3/changes/startPageToken
  Response: { startPageToken }  -- call once to get initial cursor

GET  /drive/v3/changes
  Query: pageToken (required), pageSize (default 100, max 1000), spaces, driveId
  Response: { changes: [Change { fileId, time, removed, file: FileResource }],
              nextPageToken, newStartPageToken }
  -- newStartPageToken present only on last page; client saves this for next poll

-- Push notifications (webhook)
POST /drive/v3/files/watch
  Body: { id: "channel_id", type: "web_hook",
          address: "https://client.example.com/notifications",
          expiration: <epoch_ms> }
  Response: Channel { id, resourceId, expiration }
  -- Drive POSTs to address when changes occur; client then polls /changes
```

---

## 6. Deep Dive: Core Components

### 6.1 File Chunking & Deduplication

**Problem it solves:**
Raw file uploads over HTTP are fragile — a 2 GB file upload that fails at 99% must restart from scratch without chunking. Additionally, at billion-user scale, many users store identical files (same PDF textbook, same meme image). Without deduplication, storage costs multiply unnecessarily. Chunking also enables parallel upload (higher throughput) and efficient delta-sync (only changed chunks need re-upload on file modification).

**Approaches Comparison:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Single-request upload** | File sent in one HTTP POST | Simple, no session state | Fails completely on network drop; no parallelism; full re-upload on retry |
| **Fixed-size chunking** | File split into equal-sized blocks (e.g., 256 KB) | Simple split logic; easy to parallelize | Poor dedup: single-byte insert shifts all subsequent chunks |
| **Content-defined chunking (CDC)** | Rabin fingerprinting to find natural chunk boundaries | Excellent dedup across similar files (rolling hash is insertion-resilient) | More complex; variable chunk sizes add indexing complexity |
| **File-level dedup only** | SHA-256 of entire file; skip upload if hash exists | Zero upload for exact duplicates | Any modification → full re-upload; no partial dedup |
| **Block-level dedup (CDC + hash)** | CDC chunks + per-chunk SHA-256 dedup table | Best storage savings; only novel chunks transmitted | Client CPU cost for hashing; chunk manifest management |

**Selected Approach: Fixed-size chunking (4 MB) + SHA-256 block-level dedup**

Justification:
- **Fixed-size chosen over CDC** for Google Drive because: (a) Drive stores office documents, PDFs, images — these rarely have long shared prefixes with near-identical files (unlike source code). CDC's main win is on files that are edited incrementally; (b) fixed-size is simpler to implement, reason about, and audit; (c) 4 MB chunk size amortizes per-chunk HTTP overhead while keeping chunk granularity fine enough for delta sync.
- **Block-level dedup chosen over file-level** because: even slightly modified files share most of their 4 MB chunks with their predecessor version, so delta uploads re-use existing chunks. This compresses versioning storage dramatically.
- **SHA-256 chosen over MD5/SHA-1** because: collision resistance matters — a malicious user crafting a hash collision could access another user's data; SHA-256 with 256-bit output space has no known practical collisions.

**Implementation Detail:**

Client-side chunking algorithm (pseudo-code):
```
function prepareUpload(file):
    chunks = []
    for offset in range(0, file.size, CHUNK_SIZE=4MB):
        chunk_data = file.read(offset, CHUNK_SIZE)
        chunk_hash = sha256(chunk_data)
        chunks.append({index: offset/CHUNK_SIZE, hash: chunk_hash, size: len(chunk_data)})
    file_hash = sha256(file.read_all())
    return (file_hash, chunks)

function uploadFile(file):
    (file_hash, chunks) = prepareUpload(file)

    # Check if file already exists (file-level dedup)
    if server.fileExists(file_hash):
        server.createFileRecord(file_hash)  # zero-byte upload
        return

    # Initiate session
    session_id = server.initiateUpload(filename, file.size, file_hash, chunks)

    # Check chunk existence in parallel (batch API)
    existing = server.checkChunks([c.hash for c in chunks])  # returns set of known hashes

    # Upload only novel chunks, in parallel (max 5 concurrent)
    novel_chunks = [c for c in chunks if c.hash not in existing]
    parallel_upload(novel_chunks, session_id, concurrency=5)

    # Commit
    server.commitUpload(session_id)
```

Server-side chunk store organization:
```
GCS bucket: drive-chunks-{region}
Object key: chunks/{sha256[0:2]}/{sha256[2:4]}/{sha256}
-- Two-level directory sharding prevents single prefix hotspot.
-- Example: chunks/ab/cd/abcdef1234...
```

Deduplication reference counting: When a new version references a chunk, `ref_count` in the `chunks` table is incremented atomically. When a version is deleted, ref_counts are decremented. A background GC job deletes chunks with `ref_count = 0` after a 7-day grace period (to handle soft-delete/restore).

**Interviewer Q&As:**

Q1: What happens if the client computes the wrong SHA-256 due to a bug or corruption?
A1: The server independently verifies the SHA-256 of each received chunk. On the commit step, the server also reassembles the chunk manifest and verifies the full-file hash matches what the client declared. If there is a mismatch, the upload session is marked failed and the client must restart. No corrupted data is ever committed to the metadata store.

Q2: How do you handle very small files efficiently? Chunking a 1 KB text file into 4 MB chunks is wasteful.
A2: Files under 256 KB are uploaded in a single multipart request with no chunking. Files between 256 KB and 4 MB use a single chunk. The chunk size is adaptive: min(4 MB, ceil(file_size / 1)) with a floor of 256 KB. For tiny files (< 1 KB), the content can be stored inline in the metadata record itself (in a `inline_content` BYTEA field on `file_versions`), eliminating blob store round-trips entirely.

Q3: A popular file (e.g., a widely-shared PDF) has its SHA-256 known. Could a malicious user reference someone else's chunk to access their data?
A3: No. The chunk store is not directly accessible to users. Chunks have no inherent access control — they are opaque byte blobs. Access is gated entirely by the metadata service's ACL check. A user requesting a download gets a signed GCS URL with a 15-minute TTL, generated server-side only after ACL verification. Knowing a chunk hash doesn't enable any access.

Q4: How does this handle versioning storage costs? If I upload a 1 GB file 100 times with tiny changes, do you store 100 GB?
A4: With block-level dedup, each version's chunk manifest references the same existing chunks for unchanged portions. Only changed 4 MB blocks generate new chunk objects. A 1 GB file edited once (changing 100 KB in the middle) creates only ~1 new 4 MB chunk object; the other 255 chunks are shared via ref_count. The storage overhead for 100 sequential edits is proportional to the total changed bytes across all edits, not the total file size multiplied by version count.

Q5: How do you prevent a dedup table that becomes a hot spot? 100 M uploads/day all writing to the same `chunks` table.
A5: The `chunks` table is sharded by `sha256` prefix in Spanner. Because SHA-256 output is uniformly distributed, sharding by first N bytes gives perfect even distribution. Additionally, the `checkChunks` API accepts batches of up to 1000 hashes in a single RPC, reducing per-chunk round trips. For extreme hot chunks (e.g., a ubiquitous JavaScript library), a Redis cache in front of the chunks table handles existence checks with < 1 ms latency.

---

### 6.2 Sync Protocol & Conflict Resolution

**Problem it solves:**
Drive clients on multiple devices must stay in sync. The challenge is threefold: (1) efficiently detecting what changed since the last sync without scanning all files; (2) determining the authoritative state when two devices edit the same file concurrently while offline; (3) doing this at scale across 200 M DAU with billions of files.

**Approaches Comparison:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Full re-scan** | Client lists all files on every poll | Simple; no server state | O(N) per sync; catastrophic at 1000 files/user |
| **Timestamp-based delta** | Poll `/changes?since={timestamp}` | Simple; no cursor state | Clock skew causes missed or duplicate changes; not safe |
| **Opaque cursor / change token** | Server-assigned monotonic cursor; `/changes?pageToken={cursor}` | Clock-independent; no missed events; server controls ordering | Server must maintain change log per user |
| **Long-polling / WebSocket** | Client holds open connection; server pushes events | Lowest latency; real-time feel | High server connection state; doesn't work offline; reconnect storms |
| **Hybrid: push trigger + pull fetch** | Server pushes lightweight "something changed" signal; client pulls `/changes` for details | Best of both: low latency + no missed events; server is stateless for change content | Slightly more complex client logic |

**Selected Approach: Hybrid push + cursor-based pull**

Justification:
- Pure push (WebSocket) requires the server to buffer events for offline clients and manage millions of persistent connections. At 200 M DAU, even 10% online simultaneously = 20 M WebSocket connections — expensive.
- Cursor-based pull solves offline perfectly: the client saves its cursor durably, resumes polling from the same point after any outage, and never misses events.
- The hybrid adds a cheap push signal (a single "you have changes" WebSocket/SSE message or FCM push notification) to trigger an immediate poll, achieving real-time latency without requiring the server to buffer payloads per connection.

**Conflict Resolution:**

Drive's conflict resolution philosophy: **last-write-wins for non-collaborative file types; server wins for concurrent offline edits of binary files; automatic merge for Google Docs (OT-based).**

```
Conflict detection:
  Client sends PATCH/upload with: If-Match: "{last_known_version_id}"
  Server checks: IF current head version_id != last_known_version_id → CONFLICT

Resolution strategies by file type:
  1. Google Docs/Sheets/Slides  → OT engine merges in real-time; no conflict
  2. Text files (non-Google)    → Server creates a "conflict copy" with both versions;
                                  user resolves manually (similar to Dropbox behavior)
  3. Binary files (images, PDFs)→ Server wins: current server version is kept;
                                  client upload is stored as "conflicted copy of {name} (Device - Date)"
  4. Folder operations           → Serialized at metadata service; last write wins for renames;
                                  concurrent deletes are idempotent
```

**Implementation Detail — Change Log Cursor:**

```sql
-- Client polls with cursor
SELECT change_id, node_id, change_type, change_payload, occurred_at
FROM change_log
WHERE user_id = $1
  AND change_id > $2  -- $2 is client's saved cursor
ORDER BY change_id ASC
LIMIT 1000;
-- Client saves last change_id as new cursor
```

Cursor is stored on the client in a local SQLite database alongside a local file-system snapshot. On reconnect, the client fetches changes since cursor, diffs with local state, applies non-conflicting changes, and surfaces conflicts.

**Interviewer Q&As:**

Q1: What if two users simultaneously rename the same folder to different names while offline?
A1: Folder rename is serialized by the metadata service using optimistic locking. The first write succeeds. The second write carries `If-Match: {old_version}` which will fail with HTTP 412. The second client then re-fetches the current state (sees the first rename), and since folder renames don't have semantic conflict (both just want to name the folder), the second client re-applies its rename on top of the current name. If both want different names, both are surfaced to the user as a prompt.

Q2: How does the change log not grow infinitely?
A2: The change_log is a Cassandra table partitioned by `user_id`. Entries older than 90 days are tombstoned via Cassandra's TTL feature. Clients that have been offline for > 90 days receive a special `FULL_SYNC_REQUIRED` response, forcing them to re-download the full file list (using the standard listing API with pagination). This is a deliberate trade-off: 90-day cursor validity covers virtually all realistic offline periods.

Q3: How do you scale the push notification channel to 200 M active users?
A3: WebSocket connections are maintained in a stateless notification tier that uses a Kafka consumer group. Each WebSocket server node subscribes to partitions of the `changes` Kafka topic, and maintains an in-memory map of `user_id → [connection]`. When a change event arrives, the node routes it to any connected client for that user_id. Consistent hashing ensures clients for the same user tend to land on the same node, but cross-node routing via Redis Pub/Sub handles cases where a user has clients on different nodes.

Q4: Explain the sync flow for a file that's 10 GB — too large to hold fully in memory on a mobile device.
A4: Mobile clients use selective sync: users choose which folders to sync locally. For large files not locally synced, the client has a metadata-only record (shows in the file tree) but no local bytes. Streaming download uses HTTP range requests, allowing the file to be streamed directly to a consuming application without full local buffering. For offline access, users must explicitly pin a file; the app then downloads it in 4 MB chunks to local storage, resuming across app restarts.

Q5: How does sync work for a Shared Drive (previously Google Team Drives) where multiple users collaborate?
A5: Shared Drives have their own change log per drive (not per user). Members subscribe to the shared drive's change feed. The change_log table has a `drive_id` column (NULL for personal drives, UUID for shared drives). When a member opens Drive, they receive changes from both their personal change log and all shared drives they belong to. Permissions changes to a shared drive trigger a full re-sync of the ACL cache for all members.

---

### 6.3 Permission & ACL Enforcement

**Problem it solves:**
Permissions in Google Drive are hierarchical (folder permissions cascade to children), shared (same file can be shared to thousands of users simultaneously), and read on nearly every API call. A naive implementation would require N database lookups per request to walk the folder tree. At 52,000 metadata reads/second, this is unacceptable.

**Approaches Comparison:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Walk tree on every read** | For each request, walk parent chain to find ACL | Always accurate | O(depth) DB reads per request; depth can be 20+; 52K RPS × 20 = 1M reads/s |
| **Materialized ACL per node** | On share, propagate ACL entry to all descendants | O(1) check at read time | Expensive write amplification: sharing a top-level folder with 1M files → 1M writes |
| **Path-encoded ACL + cache** | Store ACL at share point; encode path; cache resolved ACL | Fast reads with warm cache; writes only at share point | Cache invalidation complexity on re-share/un-share |
| **Zanzibar-style ReBAC** | Google's actual system: relation tuples + recursive check service | Correct, scalable, supports complex group hierarchies | Complex to build and operate; Zanzibar is a separate paper |

**Selected Approach: Zanzibar-style Relationship-Based Access Control (ReBAC) + Redis cache**

Justification: This is Google's actual approach (published in the Zanzibar paper, 2019). Relation tuples (`user:alice can view folder:X`) are stored in Spanner. The Check API evaluates `can user U perform action A on object O?` by traversing relation tuples. Results are cached in Redis with a 60-second TTL (`acl:{user_id}:{node_id}:{action}` → bool). On permission change, the affected cache keys are invalidated. The 60-second staleness is acceptable because permission revocation in enterprise collaboration is not a sub-second requirement.

**Implementation Detail:**

```
Relation tuple format (stored in acl_entries table):
  (node_id, principal_type, principal_id, role)

Check algorithm:
  1. Check cache: GET acl:{user_id}:{node_id}:{action}
     → HIT: return cached result
  2. Direct ACL check: Does acl_entries have (node_id, user, user_id, role >= required)?
     → YES: cache TRUE, return ALLOW
  3. Group membership check: Is user_id in any group that has ACL on node_id?
  4. Path traversal: Walk path_tokens[] of node upward; for each ancestor:
       Check if user has ACL on ancestor with inherit=true
     → FOUND: cache TRUE, return ALLOW
  5. Cache FALSE, return DENY
```

Invalidation on ACL change:
```
On INSERT/DELETE/UPDATE to acl_entries for node_id:
  1. Find all nodes in subtree (using path_tokens array query)
  2. For each affected node, publish invalidation events to Redis channel
  3. All API server instances subscribed to channel flush their local L1 ACL cache
  Time complexity: O(subtree_size) for write, but writes are rare vs reads
```

---

## 7. Scaling

### Horizontal Scaling

- **API Gateway / Metadata Service**: Stateless; add instances behind a load balancer. Route by consistent hash of `user_id` to improve cache locality — requests for the same user land on the same instance, improving in-process ACL cache hit rate.
- **Upload Service**: Stateless except for session affinity — HTTP uploads should go to the same Upload Service instance that created the session (sticky sessions via `session_id` hash routing). Alternatively, session state in Redis allows any instance to resume.
- **Notification Service**: Stateful (holds WebSocket connections). Scaled by sharding users by `user_id % N` across N notification server pods. New pods announced via service registry; clients reconnect transparently via exponential backoff.
- **Blob Storage (GCS)**: GCS is inherently distributed and scales transparently. Internally, Colossus uses consistent hashing to distribute chunk objects across storage nodes.

### DB Sharding

- **Spanner**: Natively shards by key range. `fs_nodes` sharded by `node_id` UUID (uniform distribution). `file_versions` sharded by `node_id` prefix (co-locates versions with their node). Spanner automatically splits and moves hot ranges (hotspot detection built-in).
- **Cassandra (change_log)**: Partitioned by `user_id`; clustering key `change_id`. Each Cassandra node owns a token range. Replication factor = 3 with NetworkTopologyStrategy across 2 data centers.

### Replication

- **Spanner**: Paxos-based replication across 5 replicas in 3 geographic zones (quorum write requires 3 of 5). RPO = 0 (synchronous replication). RTO < 30 seconds (automatic leader failover).
- **GCS (blob store)**: Each chunk stored with 3× replication within a region, plus optional cross-region replication for paid tiers. Erasure coding (6+3) used for cold storage (files not accessed in 90 days) to reduce storage cost by 50% vs full 3× replication.
- **Redis**: Redis Cluster with 3 primary shards × 1 replica each, using `WAIT 1 0` for semi-synchronous replication of critical ACL cache writes.

### Caching Strategy

| Layer | What's Cached | TTL | Eviction |
|---|---|---|---|
| API instance in-process (LRU) | ACL decisions per user×node | 10 s | LRU, max 10k entries |
| Redis Cluster (L1) | Folder listings, file metadata, ACL decisions | 60 s | Explicit invalidation on write |
| Redis Cluster (L2) | Change log cursors, quota remaining | 30 s | Write-through |
| CDN (Cloud CDN) | Public/shared file bytes | 1 hour (or until invalidated) | Cache-Control headers; explicit purge on version create |

### CDN Strategy

Public shared files and files accessed via shareable link are served via CDN. Cache key: `sha256_hash` (not file path) — the same content at different paths hits the same CDN cache entry. Cache-Control: `public, max-age=3600, immutable` for versioned URLs (URL contains version hash). On file update, the URL changes (new sha256), so old CDN entries naturally expire. Explicit purge only needed for ACL changes (file made private after being public).

### Interviewer Q&As

Q1: How do you handle a thundering herd when a popular file is suddenly shared with 10 million users?
A1: The CDN bears the majority of download load since the file bytes are immutable (content-addressed). The metadata service only sees the initial permission check per user; after the first ACL evaluation, results are cached per (user, node, action) for 60 seconds. To prevent cache stampede, we use probabilistic early expiration: re-compute the cache entry when TTL < random(0, 30s) rather than exactly at TTL=0. The Notification Service fans out the share event to recipients via Kafka with controlled throughput (rate-limited publish per share operation).

Q2: How do you handle quota enforcement at scale without a central bottleneck?
A2: Quota is stored per user in Spanner (`users.used_bytes`), updated transactionally on each upload commit. To avoid per-upload Spanner writes at 3,500 uploads/second, we use a **deferred quota accounting** model: upload commits update a per-user quota accumulator in Redis (fast, atomic `INCRBY`). A background job flushes Redis accumulators to Spanner every 5 seconds. Quota checks (reject if used > quota) use the Redis value + a 5% buffer to account for un-flushed updates. This reduces Spanner write load by ~100× while maintaining < 0.1% quota overage risk.

Q3: How do you scale the search indexing pipeline for 100 M uploads/day?
A3: New file events are published to a Kafka topic. A fleet of indexing workers (horizontally scalable) consumes from this topic. Each worker: (1) fetches file content from GCS, (2) runs text extraction (Apache Tika for docs, Tesseract OCR for images), (3) writes to an Elasticsearch cluster sharded by `user_id`. The worker fleet auto-scales based on Kafka consumer group lag. Full-text search queries are routed to the Elasticsearch shard(s) for the querying user. At 100 M uploads/day, peak indexing throughput is ~3,500 files/second — achievable with ~500 worker instances processing an average of 7 files/second each.

Q4: How do you handle large enterprise customers with millions of files in a single Shared Drive?
A4: Shared Drive metadata is stored identically to personal drive metadata but under a `shared_drive_id` shard. For listing very large folders (100,000+ children), we enforce a max page size of 1,000 and require cursor-based pagination. Folder size is cached (pre-computed `child_count` on `fs_nodes` with transactional increment/decrement). For bulk operations (shared with new member → inherit ACL on all files), we use an async background job that batches ACL insertions in 1,000-row transactions to avoid long-running Spanner transactions.

Q5: How do you prevent hot partitions in the chunk store when a viral file is downloaded by millions simultaneously?
A5: The CDN handles this: popular chunks are cached at CDN edge PoPs. A chunk accessed by > 1,000 unique users/hour is automatically promoted to CDN by the Cache-Control policy. The GCS origin is only hit on CDN misses. For extreme virality (government press release, etc.), we pre-warm CDN by explicitly pushing the chunk to top-50 CDN PoPs upon detecting sustained traffic. Internally, GCS has its own caching layer that further reduces disk reads for hot objects.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Upload Service instance crash mid-upload | Session appears incomplete | Client times out waiting for ACK | Client resumes from last confirmed chunk via `GET /upload?upload_id=X`; session state in Redis survives instance crash |
| Spanner region outage | Metadata reads/writes fail for affected region | Health checks, error rate spike | Spanner uses 5-replica Paxos; survives single zone loss; multi-region config survives full region loss |
| GCS chunk write failure | Upload fails to persist | HTTP 5xx from GCS | Retry with exponential backoff (3 retries); upload sessions have 7-day TTL so resumable |
| Notification Service crash | Connected clients miss real-time push | Client detects WebSocket disconnect | Client falls back to polling `/changes` with saved cursor; no events missed due to cursor durability |
| Redis cache cluster failure | ACL/metadata cache miss | Cache error counter spikes | All operations fall back to Spanner reads; performance degrades gracefully; cache auto-heals on node restart |
| Kafka topic partition leader failure | Change log publishing delayed | Consumer lag spike | Kafka auto-elects new partition leader in < 30 s; upload commits succeed (Kafka publish is async); notifications delayed ≤ 30 s |
| Full region loss | All services in region unavailable | Global health check fails | Traffic rerouted to nearest healthy region via Global Load Balancer DNS update; RTO < 5 minutes |
| Corrupt chunk (bit rot) | Downloaded file content incorrect | SHA-256 mismatch on read | GCS reads are checksummed; on mismatch, GCS auto-repairs from replica; application layer re-verifies SHA-256 on download |
| Duplicate upload commit (network retry) | File version created twice | Idempotency key collision | `upload_sessions.session_id` is idempotency key; second commit attempt returns the already-committed FileResource (HTTP 200 with `X-Upload-Status: already-committed`) |

### Retries & Idempotency

- All mutating API calls include client-generated idempotency keys in `X-Idempotency-Key` header (UUID). Server stores (idempotency_key → response) in Redis for 24 hours.
- Chunk upload: PUT to `/upload/{sessionId}/chunk/{index}` is idempotent — same chunk_index with same bytes → server returns 200 without re-storing.
- Exponential backoff: initial 1 s, max 64 s, jitter ±25%. Applies to all internal RPCs and client-server calls.

### Circuit Breaker

Implemented at the API Gateway using the token bucket algorithm per downstream service. If error rate for Spanner calls exceeds 10% over a 30-second window, the circuit opens for 60 seconds: requests return HTTP 503 with `Retry-After: 60`. This prevents cascade failures where metadata service overloads Spanner with retries.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Tool |
|---|---|---|---|
| Upload success rate | Counter ratio | < 99.9% over 5 min | Datadog / Cloud Monitoring |
| Upload p99 latency | Histogram | > 10 s | Cloud Monitoring |
| Download p99 TTFB | Histogram | > 1 s | Cloud Monitoring |
| Metadata RPC latency (Spanner) | Histogram | p99 > 100 ms | Spanner dashboard |
| Kafka consumer lag (change_log) | Gauge | > 10,000 events | Kafka UI / Prometheus |
| ACL cache hit rate | Counter ratio | < 90% | Redis INFO, Prometheus |
| Quota errors per minute | Counter | > 100/min | Alert on spike |
| Active WebSocket connections | Gauge | — (for capacity planning) | Prometheus |
| GCS chunk error rate | Counter ratio | > 0.01% | Cloud Monitoring |
| CDN hit ratio | Counter ratio | < 85% | CDN analytics dashboard |

### Distributed Tracing

Every request carries a `X-Trace-Id` (W3C Trace Context format). Spans emitted for: API Gateway parse, ACL check (with cache hit/miss tag), Spanner query execution, GCS chunk write, Kafka publish. Traces sampled at 1% uniform + 100% for errors. Stored in Cloud Trace (Google) or Jaeger. Use trace to diagnose why a specific upload took > 5 seconds (usually: Spanner hot key or GCS retry).

### Logging

- **Access logs**: Every API request logged with `user_id` (hashed for PII), `operation`, `file_id`, `status_code`, `latency_ms`, `bytes_transferred`. Shipped to BigQuery for compliance audit.
- **Audit logs**: Immutable append-only log (Cloud Audit Logs) of all file access, sharing, deletion events. Retained 7 years for enterprise customers.
- **Error logs**: Structured JSON with stack trace, `trace_id`, `user_id` (hashed). Centralized in Cloud Logging with log-based alerts.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|---|---|---|---|
| Chunk size | 4 MB fixed | CDC variable | Simpler; Drive workload doesn't benefit much from CDC; fixed size easier to audit |
| Metadata DB | Spanner | PostgreSQL | Spanner provides global consistency and horizontal scale without manual sharding |
| Conflict resolution | Conflict copy for binary files | Auto-merge | Safe default; users lose no data; Google Docs handles collaborative types separately |
| ACL architecture | ReBAC (Zanzibar-style) | Inherited ACL materialized on all nodes | Write amplification on materialized is too severe for large shared drives |
| Sync protocol | Hybrid push+pull (cursor) | Pure WebSocket push | Cursor survives offline; push keeps latency low without server buffering payload state |
| Quota accounting | Redis deferred flush to Spanner | Synchronous Spanner write per upload | Reduces Spanner write load 100× at cost of ≤ 0.1% temporary overage |
| Chunk dedup | SHA-256 keyed content-addressed store | Per-user chunk isolation | Global dedup reduces total storage by ~20%; SHA-256 collision risk is negligible |
| CDN cache key | SHA-256 hash (content-addressed) | File path + version | Content-addressed URLs are immutable; CDN can cache aggressively with no invalidation on renames |
| Versioning storage | Block-level dedup with ref_count | Full copy per version | Dramatically reduces storage cost for versioned files |
| Push notifications | Kafka → WebSocket/FCM hybrid | Direct Kafka → client | Kafka decouples producers from consumers; FCM handles mobile offline |

---

## 11. Follow-up Interview Questions

Q1: How would you design the real-time collaborative editing feature (Google Docs) at a high level?
A1: Use Operational Transformation (OT) or CRDT. Each editing session has a server-side session coordinator (backed by a stateful service, one per document). All clients send operations (insert char at position X, delete range Y) to the coordinator. The coordinator serializes operations using OT: when concurrent ops arrive, it transforms op2 against op1 before applying, ensuring convergence. The coordinator broadcasts the transformed op to all other clients. State is checkpointed to Spanner every N operations. For scale, documents are sharded by `doc_id`; the coordinator for a doc lives on the shard responsible for that doc_id.

Q2: How would you implement the offline mode cache on a mobile device?
A2: The mobile app maintains a local SQLite database mirroring a subset of the cloud state (the files/folders the user has pinned for offline). On every successful sync, the SQLite DB is updated. File bytes are stored in the app's sandboxed file system. An offline edit queue (also SQLite table) records mutations made while offline. On reconnect: (1) fetch changes from server since last cursor, (2) apply non-conflicting server changes locally, (3) replay queued local mutations to server with conflict detection headers.

Q3: How do you handle GDPR right-to-erasure requests? A user deletes their account.
A3: Account deletion triggers an async deletion pipeline: (1) user record marked `deletion_pending`; (2) all ACL entries for the user removed (others lose access to files they shared); (3) all `fs_nodes` owned by user marked `is_trashed=TRUE`; (4) after 30-day grace period (in case user reactivates), all nodes hard-deleted; (5) `file_versions` for those nodes deleted; (6) chunk ref_counts decremented; (7) GC cleans up orphaned chunks. Audit logs retain only anonymized records. Total purge time: ≤ 30 days.

Q4: How would you implement file preview generation (PDF thumbnails, video still frames)?
A4: Async pipeline triggered on upload commit: event to Kafka topic `file.uploaded`. Preview worker consumes event, downloads file from GCS, generates thumbnails at multiple resolutions (64×64, 256×256, 1080p for video) using ImageMagick/FFmpeg, stores thumbnails in a separate GCS bucket (`drive-previews-{region}`), updates the `file_versions` record with `preview_urls`. Workers are containerized (GKE), auto-scaled based on queue depth. Results cached at CDN.

Q5: How would you prevent abuse — a user storing TB of data under a free 15 GB account?
A5: Multiple enforcement layers: (1) Quota pre-check at upload initiation (before session created) — reject if `used_bytes + upload_size > quota_bytes`; (2) Quota enforced again at commit (prevents race condition); (3) Storage class downgrade — files not accessed in 1 year moved to Nearline storage (cheaper, but same quota counting); (4) Anti-abuse ML model flags accounts uploading unusual patterns (many unique large files, no reads); (5) Rate limiting: max 100 upload sessions/user/hour.

Q6: How would you handle a request to list all files shared with me across all folders?
A6: This requires an inverted index on `acl_entries`: `INDEX idx_acl_principal ON acl_entries(principal_id)`. A dedicated `shared_with_me` view pre-computes this by materializing all `node_id`s where the user is a non-owner ACL principal, stored in a Redis sorted set (sorted by `shared_at` timestamp) per user, updated on every ACL change. The API `GET /drive/v3/files?q=sharedWithMe` queries this Redis set and fetches metadata for the top-N results.

Q7: What is the data model for Shared Drives (Team Drives) vs personal drives?
A7: Shared Drives are modeled as a first-class entity with a `drive_id`. `fs_nodes` have a nullable `drive_id` foreign key (NULL for personal). Permissions in Shared Drives are managed at the drive level (all members inherit access to all files) rather than per-file. A `shared_drive_members` table stores `(drive_id, user_id, role)`. This simplifies ACL checks for Shared Drive content: check `shared_drive_members` for the drive_id, no tree traversal needed.

Q8: How would you implement file search with full-text content indexing?
A8: Async pipeline: upload event → content extraction worker (Tika for Office/PDF, Tesseract for image OCR) → Elasticsearch index. Document schema: `{file_id, user_id, title, content_text, mime_type, owner, last_modified}`. Shard Elasticsearch by `user_id` hash for query isolation. Search query: `GET /drive/v3/files?q={term}` → Elasticsearch query filtered by user's accessible files (using Elasticsearch's percolate/document-level security or pre-filtering by user's ACL). Update index on every file version creation; delete on file deletion.

Q9: How do you handle file moves efficiently in the virtual file system?
A9: Moving a file updates `fs_nodes.parent_id` and `path_tokens[]` for the moved node and all its descendants (for subtree moves). For a single file, this is O(1). For a folder with N descendants, it's O(N) path_token updates. To avoid long-running transactions for large subtrees, path_tokens is lazily recomputed: update `parent_id` immediately (O(1)), and re-index path_tokens in a background job. Queries that need path traversal use the closure table pattern as a fallback while the background job runs.

Q10: What consistency model does Drive expose to clients?
A10: Drive guarantees **read-your-writes consistency**: after a successful upload commit, the client that committed immediately sees the new version on any subsequent read (using Spanner's strong consistency). For other clients, Drive provides **eventual consistency with bounded staleness**: changes propagate via the change log within seconds. This is intentional — it maps to the user mental model (I saved the file; my device shows the new version immediately; my colleague's device updates within a few seconds).

Q11: How would you design the trash/restore feature?
A11: Soft delete: `UPDATE fs_nodes SET is_trashed=TRUE, trashed_at=now() WHERE node_id=X`. All queries filter `WHERE NOT is_trashed`. The trash view queries `WHERE is_trashed=TRUE AND owner_id=current_user`. Restore: `UPDATE SET is_trashed=FALSE`. Permanent deletion: a background job runs nightly and hard-deletes records where `trashed_at < now() - INTERVAL '30 days'`, then decrements chunk ref_counts. Subtree delete (trashing a folder) marks all children via a recursive CTE in Spanner, wrapped in a single transaction.

Q12: How would you handle a scenario where a file's SHA-256 causes a collision in the dedup store?
A12: SHA-256 has 2^256 possible values. The probability of a collision among 10^18 chunks (exabyte-scale) is astronomically small (birthday paradox gives ~10^-58). Practically, this will never happen. However, defensively: if a `PUT /chunks/{sha256}` arrives with different bytes than stored, the server detects the mismatch via size check + re-hash and stores the new chunk under a slightly modified key (SHA-256 + a 1-byte collision discriminator). This has never been triggered in practice.

Q13: How would you implement rate limiting for the Drive API?
A13: Token bucket per `(user_id, operation_type)` stored in Redis. Buckets: 1000 req/min for reads, 100 req/min for writes, 10 req/min for shares. Implemented at API Gateway: on each request, `EVAL` a Lua script (atomic in Redis): `tokens = DECR bucket; if tokens < 0: reject; reset bucket at next minute boundary`. Response headers: `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`. Enterprise customers have elevated limits configurable per Workspace domain.

Q14: How does Google Drive handle files larger than 5 GB (video files for Google Workspace)?
A14: The 5 GB limit is a user-facing policy constraint, not a technical one. Internally, Drive supports arbitrary file sizes via chunked upload. The metadata service allows `file_versions.size_bytes` up to 2^63-1. The chunk manifest in JSONB can reference up to ~200,000 chunks (at 4 MB each → ~800 GB max). For video, the chunk store and CDN handle large files natively via HTTP range requests. The 5 GB limit is enforced at the API Gateway for consumer accounts; Workspace admins can configure higher limits.

Q15: How would you design the notification system to handle 1 million users simultaneously coming online (e.g., start of business hours)?
A15: The reconnection storm is mitigated by: (1) exponential backoff with jitter on the client side (clients that disconnected don't all reconnect at exactly the same time); (2) the Notification Service is pre-scaled before peak hours using predictive autoscaling based on historical connection patterns; (3) WebSocket handshakes are rate-limited at the load balancer (max N new connections/second/backend pod); (4) on reconnection, clients don't immediately flush all pending changes — they send a single `GET /changes?pageToken={cursor}` which is a normal metadata read handled by the stateless Metadata Service, not the Notification Service.

---

## 12. References & Further Reading

- **Google Zanzibar: Google's Consistent, Global Authorization System** — Pang et al., USENIX ATC 2019. https://research.google/pubs/pub48190/
- **Spanner: Google's Globally-Distributed Database** — Corbett et al., OSDI 2012. https://research.google/pubs/pub39966/
- **Google Drive API Documentation** — https://developers.google.com/drive/api/guides/about-files
- **Google Resumable Upload Protocol** — https://developers.google.com/drive/api/guides/manage-uploads#resumable
- **Colossus: Successor to the Google File System** — Referenced in various Google infrastructure talks; overview at https://cloud.google.com/blog/products/storage-data-transfer/a-peek-behind-colossus-googles-file-system
- **The Google File System** — Ghemawat et al., SOSP 2003. https://research.google/pubs/pub51/
- **Designing Data-Intensive Applications** — Martin Kleppmann, O'Reilly, 2017. Chapters 5 (Replication), 6 (Partitioning), 9 (Consistency).
- **Building Microservices** — Sam Newman, O'Reilly, 2021. Chapter on event-driven architecture (relevant to change log / Kafka design).
- **Google Cloud Storage documentation (consistency model)** — https://cloud.google.com/storage/docs/consistency
- **Bigtable: A Distributed Storage System for Structured Data** — Chang et al., OSDI 2006. https://research.google/pubs/pub27898/ (relevant to understanding how Spanner's predecessor handled large-scale storage)
