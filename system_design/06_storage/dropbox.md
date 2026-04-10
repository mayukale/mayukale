# System Design: Dropbox

---

## 1. Requirement Clarifications

### Functional Requirements

1. **File Sync** — Files placed in the Dropbox folder on any device are automatically synced to all other linked devices and accessible via the web. Sync is bidirectional.
2. **Block-Level Delta Sync** — On file modification, only changed 4 MB blocks are uploaded, not the full file. This minimizes bandwidth for large files with incremental edits.
3. **File Upload & Download** — Users can upload and download any file type up to 2 TB per file (Business plans). Downloads support range requests for resumability.
4. **File Versioning** — Retain file versions for 180 days (Plus) or unlimited (Business Advanced). Users can restore any prior version.
5. **File Sharing** — Share files/folders with other Dropbox users (view or edit permissions) or via public links. Shared folders appear in recipient's Dropbox.
6. **LAN Sync** — On a local network, devices sync directly peer-to-peer without routing through Dropbox servers, using mDNS discovery and direct TCP transfer.
7. **Selective Sync** — Desktop clients can exclude specific folders from local sync (they remain in cloud storage but not on disk).
8. **Conflict Resolution** — Conflicting edits create a "conflicted copy" named `filename (Bob's conflicted copy YYYY-MM-DD)` in the same folder.
9. **Offline Mode** — All locally synced files are available offline. Edits queue locally and sync on reconnect.
10. **Paper / Third-Party Integrations** — Dropbox Paper for collaborative documents; integrations with Slack, Zoom, etc. (out of scope for core design).
11. **File Search** — Search by filename and content (full-text) within the user's Dropbox.

### Non-Functional Requirements

| Property | Target |
|---|---|
| Availability | 99.99% (< 52 min/year downtime) |
| Durability | 99.999999999% (11 nines) — files never lost |
| Sync latency | p99 < 10 s for files < 1 MB on broadband |
| Upload throughput | Full available client bandwidth utilization |
| Metadata read latency | p99 < 100 ms |
| Scale | 700 M registered users; 15 M paying; 500 M files modified/day |
| Security | TLS 1.3 in transit; AES-256 at rest; optional zero-knowledge (Team plan) |
| Consistency | Strong consistency for metadata operations |

### Out of Scope

- Dropbox Paper (collaborative documents) real-time editing internals
- Admin console (Dropbox Business team management)
- Dropbox Sign (e-signature product)
- Third-party OAuth app integrations
- Dropbox Transfer (large file send product)

---

## 2. Users & Scale

### User Types

| User Type | Description | Behavior Pattern |
|---|---|---|
| **Free User (Basic)** | 2 GB storage, 3-device limit | Occasional uploads, web + 1 desktop |
| **Plus User** | 2 TB storage, unlimited devices | Heavy sync, photo backup, large files |
| **Business User** | 5+ TB, advanced sharing | Team collaboration, many shared folders |
| **Power Syncer** | Developer / content creator | Continuous large file sync (video editing) |
| **Read-Only Collaborator** | Shared folder recipient | Reads synced files, rare writes |

### Traffic Estimates

**Assumptions:**
- 700 M registered users; 100 M monthly active; 15 M daily active (DAU)
- Each DAU performs: 20 file reads (sync checks), 3 file uploads, 5 file downloads
- Average upload size: 5 MB (mix of docs, photos, code)
- Average download size: 5 MB
- Block-level delta sync reduces average upload bytes to 20% of file size (only changed blocks): effective upload per modification = 1 MB

| Metric | Calculation | Result |
|---|---|---|
| Upload operations/day | 15 M × 3 | 45 M uploads/day |
| Download operations/day | 15 M × 5 | 75 M downloads/day |
| Sync check (metadata read)/day | 15 M × 20 | 300 M metadata reads/day |
| Upload ops/sec (avg) | 45 M / 86,400 | ~520 uploads/s |
| Upload ops/sec (peak 3×) | 520 × 3 | ~1,560 uploads/s |
| Download ops/sec (peak) | 75 M / 86,400 × 3 | ~2,600 downloads/s |
| Metadata reads/sec (peak) | 300 M / 86,400 × 3 | ~10,400 reads/s |
| Inbound bandwidth (avg) | 520 × 1 MB delta | ~4.2 Gbps |
| Inbound bandwidth (peak) | 1,560 × 1 MB | ~12.5 Gbps |
| Outbound bandwidth (peak) | 2,600 × 5 MB | ~104 Gbps |

### Latency Requirements

| Operation | p50 Target | p99 Target | Rationale |
|---|---|---|---|
| Block existence check (dedup) | 5 ms | 20 ms | Called per block; high frequency; must be fast |
| Block upload ACK | 30 ms | 150 ms | Client pipelines multiple blocks |
| File commit (metadata write) | 50 ms | 200 ms | User-visible: "Syncing..." to "Up to date" |
| Folder listing / sync state fetch | 20 ms | 100 ms | Client polls this to detect changes |
| File download TTFB | 80 ms | 400 ms | Acceptable for background sync |
| LAN sync negotiation | 10 ms | 50 ms | Local network; must be sub-RTT |

### Storage Estimates

| Item | Calculation | Result |
|---|---|---|
| New data uploaded/day (raw) | 45 M × 5 MB avg | 225 TB/day |
| After delta dedup (only new blocks) | 225 TB × 20% | 45 TB net new/day |
| Replication factor (3×) | 45 TB × 3 | 135 TB raw/day |
| 5-year accumulation | 135 TB × 365 × 5 | ~246 PB |
| Total claimed Dropbox storage (public est.) | ~500 PB+ | ~500 PB |
| Dedup savings (file-level + block-level ~30%) | 246 PB × 0.7 | ~172 PB effective |

### Bandwidth Estimates

| Direction | Calculation | Result |
|---|---|---|
| Inbound (upload avg) | 520 uploads/s × 1 MB | ~4.2 Gbps |
| Inbound (upload peak) | 1,560 × 1 MB | ~12.5 Gbps |
| Outbound (download avg) | 867 downloads/s × 5 MB | ~34.7 Gbps |
| Outbound (download peak) | 2,600 × 5 MB | ~104 Gbps |
| LAN sync (network-local, no egress charge) | variable | counted but free |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              CLIENT LAYER                                    │
│  ┌────────────────┐  ┌────────────────┐  ┌─────────────────┐                │
│  │ Desktop Client │  │  Mobile Client │  │  Web Browser    │                │
│  │ (macOS/Win/Lin)│  │  (iOS/Android) │  │  (dropbox.com)  │                │
│  │                │  │                │  │                 │                │
│  │ - Local watcher│  │ - Camera upload│  │ - File manager  │                │
│  │ - Block engine │  │ - Selective     │  │ - Previews      │                │
│  │ - LAN sync     │  │   sync          │  │ - Sharing       │                │
│  └──────┬─────────┘  └──────┬──────── ┘  └────────┬────────┘                │
└─────────┼────────────────── ┼─────────────────────┼────────────────────────┘
          │ HTTPS + TLS 1.3   │                      │
          ▼                   ▼                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                     API GATEWAY / EDGE (AWS CloudFront)                     │
│            Auth (OAuth2 + PKCE), Rate limiting, TLS termination             │
└────────────────────────────────────┬────────────────────────────────────────┘
                                     │
          ┌──────────────────────────┼─────────────────────────┐
          ▼                          ▼                          ▼
┌──────────────────┐      ┌────────────────────┐    ┌───────────────────────┐
│  METADATA SERVER │      │   BLOCK SERVER      │    │  NOTIFICATION SERVER  │
│                  │      │                     │    │                       │
│ - File tree      │      │ - Block upload      │    │ - Long-poll / SSE     │
│ - Version index  │      │ - Block download    │    │ - Change event push   │
│ - Namespace mgmt │      │ - Dedup check       │    │ - LAN peer registry   │
│ - Quota          │      │ - Virus scan queue  │    │                       │
│ - ACL/sharing    │      │ - Presigned URLs    │    │                       │
└────────┬─────────┘      └─────────┬───────────┘    └───────────────────────┘
         │                          │
         ▼                          ▼
┌──────────────────┐      ┌─────────────────────────────────────────────────┐
│  METADATA STORE  │      │               BLOCK STORE                       │
│                  │      │                                                  │
│  MySQL (sharded) │      │  AWS S3 (primary) / Dropbox Magic Pocket        │
│  + read replicas │      │  Content-addressed: key = SHA-256(block)        │
│  + ZooKeeper for │      │  AES-256 encrypted at rest per block            │
│    leader elect  │      │  3× replication cross-AZ; cross-region for paid │
└──────────────────┘      └─────────────────────────────────────────────────┘
         │
         ▼
┌───────────────────────────────────────────────────────────────────────────┐
│                          CACHE LAYER                                       │
│   Memcached cluster (file metadata, namespace state, dedup lookups)       │
│   Redis (session tokens, rate limit buckets, LAN peer tables)             │
└───────────────────────────────────────────────────────────────────────────┘

                    ┌─────────────────────────────┐
                    │     SUPPORTING SERVICES      │
                    │                             │
                    │  Search (Elasticsearch)      │
                    │  Preview Generator           │
                    │  Virus Scanner (async)       │
                    │  Analytics Pipeline          │
                    └─────────────────────────────┘
```

**Component Roles:**

- **Desktop Client (Sync Engine)**: Monitors the local Dropbox folder using OS file-system events (FSEvents/inotify/ReadDirectoryChangesW). On change, computes block fingerprints, checks which blocks are novel, uploads only new blocks, then calls Metadata Server to commit. Maintains a local SQLite DB (`~/.dropbox/config.db`) with file state snapshots.
- **Metadata Server**: The single source of truth for the file namespace — which files exist, their versions, who owns them, sharing state. Returns the current block manifest for each file version. **Crucially separated from block storage** — metadata ops are tiny, frequent, and need strong consistency; block ops are large, less frequent, and tolerant of slight eventual consistency.
- **Block Server**: Stateless service that generates presigned S3 URLs for block upload/download, verifies block checksums on receipt, manages block reference counts, and triggers async virus scanning. Does NOT store blocks itself — that's S3/Magic Pocket.
- **Notification Server**: Clients maintain a long-poll HTTP connection (or SSE). When the Metadata Server commits a change for user X (or shared folder Y), it publishes to Kafka. Notification Server consumes and pushes to all connected sessions for affected users. Enables the "badge turns green / 'Up to date'" UX within seconds.
- **Metadata Store (MySQL)**: Sharded MySQL with a master per shard and multiple read replicas. ZooKeeper manages shard routing and leader election. MySQL chosen (historically) for ACID transactions on namespace operations, familiarity, and mature tooling. Dropbox famously moved from AWS RDS to their own bare-metal MySQL — "Edgestore" and later "Dropbox on Dropbox" infrastructure.
- **Block Store (S3 / Magic Pocket)**: Object storage keyed by SHA-256 of block contents. Dropbox built "Magic Pocket" (their own block storage system) in 2016 to reduce AWS S3 costs by 90%. Each block is stored as an S3-compatible object; Magic Pocket uses erasure coding (14+3 = 17 shards) for durability.
- **LAN Sync**: Clients broadcast their presence via mDNS (`_dropbox._tcp.local`). When client A detects client B has blocks it needs, it fetches directly from B over local TCP (port chosen dynamically). The Metadata Server knows which blocks each client has cached locally and can direct peer-to-peer transfers.

**Primary Use-Case Data Flow (Block-Level Delta Sync Upload):**

1. User modifies a 100 MB video file in their Dropbox folder.
2. Desktop client detects change via OS file-system event.
3. Client reads the modified file and computes SHA-256 of each 4 MB block (25 blocks total).
4. Client compares block hashes against its local SQLite state of the previous version — identifies 3 blocks that changed.
5. Client sends `POST /metadata/file_state` with new block manifest → Metadata Server checks which blocks it doesn't have → returns list of 3 novel block hashes.
6. Client requests presigned upload URLs for 3 blocks from Block Server: `POST /blocks/upload_urls`.
7. Client uploads 3 blocks (12 MB total) directly to S3/Magic Pocket via presigned PUT URLs. ~12 MB vs 100 MB — 88% bandwidth savings.
8. Client notifies Metadata Server: `POST /metadata/commit` with `{file_path, new_block_manifest, previous_version_id}`.
9. Metadata Server atomically creates new version record, updates namespace.
10. Notification Server pushes `FILE_CHANGED` event to all connected devices of the user and shared folder members.
11. Other devices receive the event, fetch the new block manifest, identify which of the 3 changed blocks they don't have locally, download only those blocks.

---

## 4. Data Model

### Entities & Schema

```sql
-- Accounts
CREATE TABLE accounts (
    account_id      BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    email           VARCHAR(255) NOT NULL,
    display_name    VARCHAR(255),
    plan            ENUM('basic','plus','professional','business','business_plus') NOT NULL DEFAULT 'basic',
    quota_bytes     BIGINT UNSIGNED NOT NULL DEFAULT 2147483648,  -- 2 GB default
    used_bytes      BIGINT UNSIGNED NOT NULL DEFAULT 0,
    created_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    INDEX idx_email (email)
) ENGINE=InnoDB;

-- Namespaces (personal or shared folder)
-- A namespace is the root of a sync scope
CREATE TABLE namespaces (
    namespace_id    BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    owner_id        BIGINT UNSIGNED NOT NULL,
    namespace_type  ENUM('personal','shared_folder','team_folder') NOT NULL,
    created_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (owner_id) REFERENCES accounts(account_id)
) ENGINE=InnoDB;

-- Every file and folder node in the virtual file tree
CREATE TABLE file_journal (
    journal_id      BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    namespace_id    BIGINT UNSIGNED NOT NULL,
    -- Stable file identifier (persists across renames/moves)
    file_id         BINARY(16) NOT NULL,           -- UUID stored as binary
    parent_file_id  BINARY(16),                    -- NULL for root children
    filename        VARCHAR(1024) NOT NULL,
    is_folder       TINYINT(1) NOT NULL DEFAULT 0,
    is_deleted      TINYINT(1) NOT NULL DEFAULT 0,
    -- Version tracking
    server_rev      BIGINT UNSIGNED NOT NULL,      -- monotonically increasing per namespace
    -- For files only
    size_bytes      BIGINT UNSIGNED,
    content_hash    BINARY(32),                    -- SHA-256 of entire file content
    block_manifest  JSON,
    -- e.g., [{"index":0,"sha256":"aabb...","size":4194304}, ...]
    client_mtime    DATETIME(3),                   -- mtime on client filesystem
    server_mtime    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    -- Sync metadata
    sync_signature  BINARY(20),                    -- identifies client that made last change
    FOREIGN KEY (namespace_id) REFERENCES namespaces(namespace_id),
    UNIQUE KEY uk_namespace_file_rev (namespace_id, file_id, server_rev),
    INDEX idx_parent (namespace_id, parent_file_id),
    INDEX idx_server_rev (namespace_id, server_rev)
) ENGINE=InnoDB;

-- Latest state per file (denormalized for fast reads)
-- Derived from file_journal; updated on each commit
CREATE TABLE file_current_state (
    namespace_id    BIGINT UNSIGNED NOT NULL,
    file_id         BINARY(16) NOT NULL,
    parent_file_id  BINARY(16),
    filename        VARCHAR(1024) NOT NULL,
    is_folder       TINYINT(1) NOT NULL DEFAULT 0,
    is_deleted      TINYINT(1) NOT NULL DEFAULT 0,
    server_rev      BIGINT UNSIGNED NOT NULL,
    size_bytes      BIGINT UNSIGNED,
    content_hash    BINARY(32),
    block_manifest  JSON,
    client_mtime    DATETIME(3),
    server_mtime    DATETIME(3),
    PRIMARY KEY (namespace_id, file_id),
    INDEX idx_parent_state (namespace_id, parent_file_id),
    FOREIGN KEY (namespace_id) REFERENCES namespaces(namespace_id)
) ENGINE=InnoDB;

-- Block / chunk registry (content-addressed)
CREATE TABLE blocks (
    block_hash      BINARY(32) PRIMARY KEY,        -- SHA-256
    size_bytes      INT UNSIGNED NOT NULL,
    storage_backend ENUM('s3','magic_pocket') NOT NULL DEFAULT 's3',
    storage_key     VARCHAR(512) NOT NULL,          -- e.g., "blocks/aa/bb/aabb1234..."
    ref_count       BIGINT UNSIGNED NOT NULL DEFAULT 1,
    scan_status     ENUM('pending','clean','quarantined') NOT NULL DEFAULT 'pending',
    created_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
) ENGINE=InnoDB;

-- Shared folder membership
CREATE TABLE shared_folder_members (
    namespace_id    BIGINT UNSIGNED NOT NULL,
    member_id       BIGINT UNSIGNED NOT NULL,
    role            ENUM('viewer','editor','owner') NOT NULL DEFAULT 'viewer',
    joined_at       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (namespace_id, member_id),
    FOREIGN KEY (namespace_id) REFERENCES namespaces(namespace_id),
    FOREIGN KEY (member_id) REFERENCES accounts(account_id)
) ENGINE=InnoDB;

-- Public / shared links
CREATE TABLE shared_links (
    link_id         BINARY(16) PRIMARY KEY,
    link_token      VARCHAR(64) NOT NULL UNIQUE,   -- random URL-safe token
    namespace_id    BIGINT UNSIGNED NOT NULL,
    file_id         BINARY(16) NOT NULL,
    created_by      BIGINT UNSIGNED NOT NULL,
    access_type     ENUM('view','edit') NOT NULL DEFAULT 'view',
    requires_password TINYINT(1) NOT NULL DEFAULT 0,
    password_hash   VARCHAR(128),
    expires_at      DATETIME(3),
    download_count  BIGINT UNSIGNED NOT NULL DEFAULT 0,
    created_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (namespace_id) REFERENCES namespaces(namespace_id)
) ENGINE=InnoDB;

-- Resumable upload sessions
CREATE TABLE upload_sessions (
    session_id      VARCHAR(64) PRIMARY KEY,       -- client-generated UUID
    account_id      BIGINT UNSIGNED NOT NULL,
    namespace_id    BIGINT UNSIGNED NOT NULL,
    target_path     VARCHAR(4096) NOT NULL,
    total_size      BIGINT UNSIGNED NOT NULL,
    expected_hash   BINARY(32) NOT NULL,
    blocks_committed JSON NOT NULL DEFAULT '[]',   -- array of committed block hashes
    status          ENUM('in_progress','committed','failed') NOT NULL DEFAULT 'in_progress',
    expires_at      DATETIME(3) NOT NULL,
    created_at      DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (account_id) REFERENCES accounts(account_id)
) ENGINE=InnoDB;
```

### Database Choice

**Candidates:**

| Database | Pros | Cons | Fit |
|---|---|---|---|
| **MySQL (sharded)** | Battle-tested ACID, full SQL, excellent read replica support, Dropbox has deep operational expertise | Manual sharding complexity, no automatic rebalancing, vertical scale limits on single shard | Selected: consistent with Dropbox's actual architecture (Edgestore) |
| **PostgreSQL** | Richer SQL features, JSONB native, better concurrency (MVCC) | Same sharding limitations as MySQL; Dropbox's team history is MySQL-focused | Good alternative; not selected due to operational familiarity |
| **CockroachDB / Spanner** | Distributed SQL, no manual sharding | Higher write latency (Paxos overhead), operational complexity | Overkill for metadata ops that are naturally per-namespace/per-user |
| **DynamoDB** | Predictable latency, serverless, auto-scale | Limited query patterns, no complex joins, eventual consistency by default | Poor fit: namespace queries require range scans and joins |
| **Cassandra** | Massive write throughput | No ACID transactions, no range queries on non-partition keys | Suitable only for the append-only journal, not current state |

**Selected: MySQL (sharded by `namespace_id`) + Memcached**

Justification:
- **MySQL** provides ACID for the critical commit path: inserting into `file_journal` and updating `file_current_state` must be atomic; MySQL transactions handle this correctly.
- **Sharding by `namespace_id`** works because almost all queries filter by namespace_id: listing a folder, fetching a file's state, getting changes since cursor. Cross-shard queries (rare: e.g., showing all files shared with a user) use a denormalized lookup table.
- **Memcached** (not Redis) for metadata caching because: (a) Dropbox's published architecture uses Memcached; (b) simple key-value cache with no persistence needed; (c) Memcached has slightly lower overhead per operation than Redis for pure caching workloads; (d) horizontal scaling via consistent hashing of keys.
- `file_journal` is append-only (new row per change) providing a natural change log for sync cursor-based fetching. `file_current_state` is the fast-path read table; denormalization eliminates joins.

---

## 5. API Design

All endpoints require `Authorization: Bearer <OAuth2_access_token>` unless noted.
Rate limits: 5,000 API calls/hour/user (default). Headers: `X-RateLimit-Limit`, `X-RateLimit-Remaining`.

### Core File Operations

```
-- List folder contents
POST /2/files/list_folder
  Body: { path: "/Photos",
          recursive: false,
          include_deleted: false,
          include_media_info: false,
          limit: 2000 }
  Response: { entries: [Metadata], cursor, has_more }
  -- Metadata union type: FileMetadata | FolderMetadata | DeletedMetadata

-- Continue listing (pagination)
POST /2/files/list_folder/continue
  Body: { cursor }
  Response: { entries: [Metadata], cursor, has_more }

-- Get latest changes (long-poll trigger)
POST /2/files/list_folder/longpoll
  Body: { cursor, timeout: 30 }  -- timeout in seconds (max 480)
  Response: { changes: bool, backoff: int }
  -- Blocks until there are changes or timeout. No auth required (cursor encodes auth).
  -- On changes: true, client calls list_folder/continue to fetch actual changes.

-- Get file metadata
POST /2/files/get_metadata
  Body: { path: "/document.pdf", include_media_info: true }
  Response: FileMetadata { id, name, path_lower, path_display, client_modified,
                           server_modified, rev, size, is_downloadable,
                           content_hash, media_info? }

-- Create folder
POST /2/files/create_folder_v2
  Body: { path: "/NewFolder", autorename: false }
  Response: { metadata: FolderMetadata }

-- Delete
POST /2/files/delete_v2
  Body: { path: "/old_file.txt" }
  Response: { metadata: DeletedMetadata }

-- Move
POST /2/files/move_v2
  Body: { from_path, to_path, allow_shared_folder: false, autorename: false }
  Response: { metadata: Metadata }

-- Copy
POST /2/files/copy_v2
  Body: { from_path, to_path, autorename: false }
  Response: { metadata: Metadata }

-- Restore version
POST /2/files/restore
  Body: { path: "/document.pdf", rev: "a1c2e4f80" }
  Response: FileMetadata
```

### Upload Endpoints

```
-- Single-request upload (files up to 150 MB)
POST /2/files/upload
  Headers:
    Dropbox-API-Arg: {"path":"/file.txt","mode":"add","autorename":true,
                      "mute":false,"strict_conflict":false}
    Content-Type: application/octet-stream
  Body: file bytes
  Response: FileMetadata

-- Start resumable/large-file upload session
POST /2/files/upload_session/start
  Headers:
    Dropbox-API-Arg: {"close":false}   -- close:true for single-request session
    Content-Type: application/octet-stream
  Body: first chunk bytes (optional)
  Response: { session_id }

-- Append chunk to session
POST /2/files/upload_session/append_v2
  Headers:
    Dropbox-API-Arg: {"cursor":{"session_id":"...","offset":4194304},"close":false}
    Content-Type: application/octet-stream
  Body: next chunk bytes
  Response: 200 OK (no body)

-- Finish upload and commit to namespace
POST /2/files/upload_session/finish
  Headers:
    Dropbox-API-Arg: {"cursor":{"session_id":"...","offset":total_size},
                      "commit":{"path":"/BigFile.mp4","mode":"overwrite"}}
    Content-Type: application/octet-stream
  Body: last chunk bytes (may be empty)
  Response: FileMetadata

-- Batch finish (commit multiple sessions atomically)
POST /2/files/upload_session/finish_batch_v2
  Body: { entries: [{ cursor, commit }] }
  Response: { async_job_id }  -- check with /upload_session/finish_batch/check
```

### Download Endpoints

```
-- Download file
POST /2/files/download
  Headers:
    Dropbox-API-Arg: {"path":"/file.pdf"}
    Range: bytes=0-1048575   -- optional range request
  Response: FileMetadata in Dropbox-API-Result header; file bytes in body

-- Get temporary direct link (for streaming)
POST /2/files/get_temporary_link
  Body: { path: "/video.mp4" }
  Response: { metadata: FileMetadata, link }  -- link valid for 4 hours
```

### Sharing

```
-- Create shared link
POST /2/sharing/create_shared_link_with_settings
  Body: { path: "/report.pdf",
          settings: { requested_visibility: "public",
                      audience: "public",
                      access: "viewer",
                      expires: "2026-12-31T00:00:00Z" } }
  Response: SharedLinkMetadata { url, id, name, expires, path_lower, link_permissions }

-- Share folder with another user
POST /2/sharing/add_folder_member
  Body: { shared_folder_id, members: [{ member: {".tag":"email","email":"bob@example.com"},
                                         access_level: "editor" }],
           quiet: false, custom_message: "Sharing project files" }
  Response: 200 OK

-- List shared folder members
POST /2/sharing/list_folder_members
  Body: { shared_folder_id }
  Response: { users: [UserMembershipInfo], groups: [], invitees: [], cursor }
```

### Sync Endpoint (Block-Level Internal Protocol)

```
-- NOTE: This is the internal sync protocol used by desktop clients,
-- not part of the public HTTP API v2. Documented here for completeness.

-- Phase 1: Compute delta
POST /internal/sync/compute_delta
  Body: { namespace_id, known_state: [{ file_id, server_rev }] }
  Response: { to_download: [FileMetadata], to_delete: [file_id], cursor }

-- Phase 2: Check block existence (dedup)
POST /internal/blocks/check
  Body: { block_hashes: ["sha256_1", "sha256_2", ...] }  -- up to 1000 hashes
  Response: { known: ["sha256_1"], unknown: ["sha256_2"] }

-- Phase 3: Get presigned upload URLs for novel blocks
POST /internal/blocks/upload_urls
  Body: { blocks: [{ hash, size }] }
  Response: { urls: [{ hash, upload_url, expires_in }] }
  -- Client uploads directly to S3/Magic Pocket via returned PUT URL

-- Phase 4: Commit new file version
POST /internal/sync/commit
  Body: { namespace_id, path, block_manifest: [...], content_hash,
          client_mtime, size_bytes, parent_rev, session_id }
  Response: FileMetadata with new server_rev, or ConflictError
```

---

## 6. Deep Dive: Core Components

### 6.1 Block-Level Delta Sync

**Problem it solves:**
File synchronization is fundamentally a bandwidth problem. Without delta sync, modifying one slide in a 500 MB PowerPoint requires uploading the entire 500 MB again. This is catastrophic on cellular connections and even significant on broadband for large files. Block-level delta sync transmits only the 4 MB blocks that actually changed, reducing sync bandwidth by 80–99% for large files with incremental edits.

**Approaches Comparison:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Full file re-upload** | Every change uploads entire file | Trivially simple | Catastrophic bandwidth; terrible UX on mobile |
| **File-level dedup (hash-then-skip)** | Skip upload if full file hash unchanged | Zero bandwidth for unchanged files | Any modification → full upload |
| **Fixed-size block dedup** | Split into equal 4 MB chunks; upload only changed chunks | Simple split; good for files where changes are isolated to specific regions | Single-byte insert shifts all subsequent blocks; poor dedup for insertions |
| **rsync algorithm** | Rolling checksum identifies matching regions; transmits diff | Minimal byte transfer; handles insertions well | High client CPU; server must hold previous version for comparison; complex protocol |
| **Content-Defined Chunking (CDC) + dedup** | Rabin fingerprint finds natural boundaries; SHA-256 per chunk | Best dedup for near-duplicate files; handles insertions | Variable chunk sizes; more complex manifest; index overhead |
| **Fixed-size block + local state (Dropbox approach)** | Client maintains per-block hash of previous version locally; only uploads blocks where hash changed | No server-side comparison needed; simple; deterministic | Single-byte insert invalidates ~2 blocks at boundary; 4 MB granularity limits dedup fidelity |

**Selected Approach: Fixed-size 4 MB blocks + SHA-256 per block + client-maintained local block state**

Justification:
- **Fixed-size chosen over CDC**: Dropbox's user base stores office files, photos, code, and videos. For most of these workloads, edits cluster in specific regions of the file (e.g., editing slide 5 of 50 changes a contiguous region). Fixed-size blocks handle this efficiently. CDC's advantage (insertion-resilience) matters for text files, but Dropbox serves a general consumer market where most large files (photos, videos, archives) are either replaced wholesale or appended to linearly.
- **Client local state** eliminates the need for the server to compute diffs: the client knows exactly which blocks it changed, because it hashes the file after every local modification and compares to its cached block state. This is O(file_size/block_size) CPU on the client but only O(changed_blocks) network.
- **4 MB block size** balances: (a) too small (e.g., 256 KB) → too many HTTP round trips per large file; (b) too large (e.g., 16 MB) → a small edit still uploads a large block. Dropbox's published architecture confirms 4 MB.
- **SHA-256** for block identity: collision resistant; enables global dedup across all users (same block hash → same block bytes → block already in storage → zero upload).

**Implementation Detail:**

Client-side block computation:
```python
BLOCK_SIZE = 4 * 1024 * 1024  # 4 MB

def compute_block_manifest(filepath):
    blocks = []
    with open(filepath, 'rb') as f:
        index = 0
        while True:
            data = f.read(BLOCK_SIZE)
            if not data:
                break
            block_hash = hashlib.sha256(data).digest()
            blocks.append({
                'index': index,
                'hash': block_hash.hex(),
                'size': len(data)
            })
            index += 1
    file_hash = compute_dropbox_content_hash(filepath)  # Dropbox's specific algorithm
    return blocks, file_hash

def compute_dropbox_content_hash(filepath):
    # Dropbox content_hash: SHA-256 of the concatenation of SHA-256s of each 4MB block
    block_hashes = []
    with open(filepath, 'rb') as f:
        while True:
            data = f.read(BLOCK_SIZE)
            if not data:
                break
            block_hashes.append(hashlib.sha256(data).digest())
    return hashlib.sha256(b''.join(block_hashes)).hexdigest()

def sync_file(filepath, local_db):
    new_blocks, new_content_hash = compute_block_manifest(filepath)
    old_blocks = local_db.get_blocks(filepath)  # cached from last sync

    # Identify changed blocks
    changed_blocks = []
    for new_block in new_blocks:
        old_block = old_blocks.get(new_block['index'])
        if old_block is None or old_block['hash'] != new_block['hash']:
            changed_blocks.append(new_block)

    # Check with server which changed blocks it already has (global dedup)
    unknown_blocks = server.check_blocks([b['hash'] for b in changed_blocks])

    # Upload only truly novel blocks
    upload_urls = server.get_upload_urls(unknown_blocks)
    for block in unknown_blocks:
        data = read_block(filepath, block['index'])
        http_put(upload_urls[block['hash']], data)

    # Commit
    server.commit_file(filepath, new_blocks, new_content_hash)
    local_db.update_blocks(filepath, new_blocks)
```

Server-side block reference counting (on commit):
```sql
-- Increment ref_count for blocks in new version
UPDATE blocks SET ref_count = ref_count + 1
WHERE block_hash IN (SELECT block_hash FROM new_manifest_blocks);

-- Decrement ref_count for blocks only in old version
UPDATE blocks SET ref_count = ref_count - 1
WHERE block_hash IN (SELECT block_hash FROM old_manifest_blocks)
  AND block_hash NOT IN (SELECT block_hash FROM new_manifest_blocks);

-- Schedule GC for ref_count = 0
DELETE FROM blocks WHERE ref_count = 0 AND created_at < NOW() - INTERVAL 7 DAY;
```

**Interviewer Q&As:**

Q1: How does block-level sync work when a 1-byte insertion at the beginning of a 1 GB file shifts all block boundaries?
A1: With fixed-size blocks, a 1-byte insertion at the start shifts every block boundary, making all blocks "new" and requiring a full re-upload — the worst case for fixed-size chunking. This is precisely where CDC (Content-Defined Chunking) excels. Dropbox partially mitigates this with a practical observation: the files where users make insertions (text files, code) are typically small enough that a full re-upload is < 1 MB. For large files (videos, archives), users don't insert bytes at the start; they either overwrite or append. For the truly adversarial case (large text file with insertion), Dropbox recommends using a Google Docs-style editor instead.

Q2: How does the server-side dedup table scale when there are 500 billion blocks stored?
A2: The `blocks` table is sharded by `block_hash` prefix. SHA-256 is uniformly distributed, so sharding by the first 2 hex digits gives 256 shards with even distribution. At 500B rows × ~100 bytes/row = ~50 TB of block metadata. Distributed across 256 shards = ~200 GB per shard — manageable on modern hardware. The `check_blocks` API accepts 1000 hashes per call and uses `IN` queries with the `block_hash` primary key; these resolve in < 10 ms with B-tree index.

Q3: What prevents a user from claiming their block has a specific SHA-256 to access another user's block?
A3: Block access is mediated through the Block Server, not through direct URL guessing. When a client requests a download URL, the Block Server verifies the requesting user's account has ACL access to the file that references the block (via Metadata Server). Only then is a presigned S3 URL issued with a 15-minute expiry. Knowing a block hash gives you nothing without the presigned URL issued by an authorized download flow.

Q4: How does the system ensure exactly-once semantics for block uploads? What if the client crashes after uploading a block but before committing?
A4: Block uploads are idempotent: `PUT /block/{sha256}` with the same bytes always succeeds (if hash matches bytes). Uncommitted blocks are stored in a staging area with a 7-day TTL. The upload session in `upload_sessions.blocks_committed` tracks which blocks have been acknowledged. On restart, the client fetches its session state and re-uploads any blocks not in `blocks_committed`. The commit step is the single atomic operation — before commit, blocks exist but no file version references them. After commit, the file version record and block ref_counts are updated atomically in a MySQL transaction.

Q5: How does Dropbox's content_hash differ from a simple SHA-256 of the file, and why use it?
A5: Dropbox's content_hash is SHA-256(SHA-256(block_0) || SHA-256(block_1) || ... || SHA-256(block_N)), where each block is 4 MB. This construction has two advantages: (a) it can be computed incrementally — each block's SHA-256 is computed as the file is chunked, and the final content_hash is one additional SHA-256 pass over the block hashes; (b) it can be verified in parallel on the server — each block's hash is independently verifiable, and the content_hash provides a global check. A simple SHA-256 of the entire file would require reading the entire file sequentially to verify.

---

### 6.2 Metadata Server & Namespace Architecture

**Problem it solves:**
The Metadata Server must handle 10,400 reads/second and 1,560 writes/second while maintaining ACID guarantees for namespace mutations. It must efficiently answer: "What changed in this namespace since cursor X?" (the core sync query) and "What is the current state of folder Y?" (the listing query). These two query patterns have opposing optimization pressures.

**Approaches Comparison:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Single append-only journal** | Every change is a row; queries always replay from beginning | Simple; audit trail built-in | Unbounded table growth; slow listing requires full scan |
| **Separate journal + current_state** | Journal for change history; current_state for fast reads (selected) | O(1) listing; O(cursor_offset) sync; clean separation | Write amplification (2 writes per change); eventual consistency between tables |
| **Event sourcing with snapshots** | Append-only events; periodic snapshots for fast reads | Clean model; replayable | Snapshot management complexity; snapshot staleness window |
| **CRDT-based state** | File state as CRDT; automatic merge | Natural conflict resolution | Complex; overkill for file metadata; poor for ACL |

**Selected: Dual-write (file_journal + file_current_state)**

Implementation Detail:

```sql
-- Core sync query: What changed since cursor X?
SELECT j.file_id, j.filename, j.is_deleted, j.is_folder,
       j.size_bytes, j.content_hash, j.block_manifest,
       j.server_rev, j.server_mtime
FROM file_journal j
WHERE j.namespace_id = ?
  AND j.server_rev > ?   -- cursor
ORDER BY j.server_rev ASC
LIMIT 500;
-- Returns at most 500 changes; client calls again if result has 500 rows.
-- Cursor = last seen server_rev (saved in client's local SQLite).

-- Commit a new file version (single transaction)
BEGIN;
  -- 1. Check for conflict
  SELECT server_rev FROM file_current_state
  WHERE namespace_id = ? AND file_id = ?
  FOR UPDATE;   -- row-level lock

  -- Conflict if current server_rev != parent_rev sent by client
  -- (application-level check after this SELECT)

  -- 2. Get next server_rev for namespace (atomic increment)
  UPDATE namespaces SET server_rev = server_rev + 1 WHERE namespace_id = ?;
  SELECT server_rev FROM namespaces WHERE namespace_id = ? FOR UPDATE;

  -- 3. Insert journal entry
  INSERT INTO file_journal (namespace_id, file_id, parent_file_id, filename, ...)
  VALUES (?, ?, ?, ?, ...);

  -- 4. Upsert current state
  INSERT INTO file_current_state (namespace_id, file_id, parent_file_id, filename, ...)
  VALUES (?, ?, ?, ?, ...)
  ON DUPLICATE KEY UPDATE
    filename = VALUES(filename),
    server_rev = VALUES(server_rev),
    block_manifest = VALUES(block_manifest),
    ...;

  -- 5. Update quota
  UPDATE accounts SET used_bytes = used_bytes + ?
  WHERE account_id = ? AND used_bytes + ? <= quota_bytes;
  -- If 0 rows updated: quota exceeded, ROLLBACK

COMMIT;
```

The `server_rev` is a namespace-scoped monotonically increasing counter. This is critical: clients store their last seen `server_rev` as their sync cursor. By ordering `file_journal` by `server_rev`, the server can return all changes since the client's last sync in a single efficient range scan on the index `(namespace_id, server_rev)`.

**Interviewer Q&As:**

Q1: Why not use a global change log cursor instead of per-namespace?
A1: Per-namespace cursor localizes the impact of a high-write namespace. A user with millions of files in a shared folder doesn't slow down cursor scans for other users. The namespace boundary also maps cleanly to MySQL shard boundaries (sharded by namespace_id), so the cursor query always hits a single shard. A global cursor would require a distributed sequence generator (complex, hot) and cross-shard queries for sync (expensive).

Q2: How do you handle the case where namespace_id sharding creates a hot shard? A hugely popular shared folder (e.g., a company-wide folder) might get millions of reads/writes.
A2: Mitigation layers: (1) Memcached caches `file_current_state` entries with 60-second TTL, absorbing ~90% of read traffic for popular namespaces; (2) MySQL read replicas serve the sync queries; (3) if a single namespace exceeds a read threshold (>1,000 RPS), it is promoted to its own dedicated MySQL instance; (4) the notification server rate-limits change fan-out per namespace (e.g., debounces rapid successive changes from a build pipeline into batched notifications).

Q3: How does the Metadata Server handle shard failures?
A3: Each shard has 1 primary and 2 read replicas. ZooKeeper monitors liveness. On primary failure: ZooKeeper detects missing heartbeat within 10 seconds, promotes a replica to primary (semi-synchronous replication ensures ≤ 1 transaction of data loss). Writes fail with 503 during the 10-second failover window. Clients retry with exponential backoff. SLA: 99.99% uptime allows ~4.4 minutes of downtime/month, which covers this window.

Q4: How is the `server_rev` counter kept consistent under concurrent writers?
A4: The `namespaces` table has a `server_rev` column updated with `server_rev = server_rev + 1` inside the commit transaction. MySQL's InnoDB row-level locking ensures this increment is serialized — concurrent transactions block on the row lock. This means namespace-level throughput is limited to how fast MySQL can serialize commits. For busy namespaces (company-wide Dropbox), this creates a bottleneck. Mitigation: batch writes (aggregate multiple file changes from a single client push into one transaction with one rev increment), reducing lock contention.

Q5: How do you handle the `file_journal` table growing to billions of rows over time?
A5: Journal entries older than 180 days (the version retention limit) are periodically archived to cold storage (S3 Glacier) and deleted from MySQL. A background job runs nightly, scanning for `file_journal` entries where `server_mtime < NOW() - INTERVAL 180 DAY` and archiving them. Active sync cursors are all within the 180-day retention window. The archive is still queryable for compliance/support purposes via a separate batch query interface. This keeps the hot MySQL tables bounded to ~180 days of history.

---

### 6.3 LAN Sync

**Problem it solves:**
In an office environment, 50 employees might all have Dropbox. When someone uploads a 500 MB file, without LAN sync, every one of the 50 laptops downloads that 500 MB from Dropbox servers — 50 × 500 MB = 25 GB of internet egress. With LAN sync, the first laptop to download fetches from the server; the rest fetch from that laptop directly over the local 1 Gbps network — reducing internet egress by 98%.

**Approaches Comparison:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **mDNS + direct TCP** | Announce presence via mDNS; transfer via direct TCP | No infrastructure needed; works on any LAN | mDNS doesn't cross subnets; NAT traversal issues |
| **BitTorrent-style P2P** | Full P2P protocol; track peers via DHT or tracker | Handles large-scale mesh well | Complex; overkill for small office LANs; legal perception issues |
| **Central LAN relay** | Designate one machine as local proxy | Simpler routing | Single point of failure; one machine must always be on |
| **mDNS + Dropbox server coordination** | mDNS for peer discovery; Dropbox server provides block-level peer map | Dropbox server knows which blocks each peer has; optimal routing | Requires server round-trip for peer-block mapping |

**Selected: mDNS discovery + direct TCP + server-assisted block map**

Implementation Detail:

```
LAN Sync Protocol:

1. Peer Discovery:
   Each Dropbox client broadcasts mDNS service:
     _dropbox._tcp.local  port=17500  TXT: {"version":"...", "account_id_hash":"..."}
   Clients on the same subnet see each other's mDNS announcements.
   account_id_hash (HMAC-SHA256 of account_id with a shared team secret) prevents
   strangers from participating in each other's LAN sync.

2. Peer Authentication:
   When client A wants to fetch a block from client B:
     A → B: TCP connect to mDNS-announced port
     A → B: "HELLO {nonce_a}"
     B → A: "HELLO_RESP {nonce_b, HMAC(nonce_a, shared_secret)}"
     A → B: Verifies HMAC; sends "AUTH_OK {HMAC(nonce_b, shared_secret)}"
   Shared_secret = per-team key derived from Dropbox account tokens.

3. Block Request:
   A → B: "GETBLOCK {sha256_hash}"
   B checks local block cache:
     If present: "BLOCK_DATA {size} {bytes}"
     If absent: "BLOCK_NOT_FOUND"
   
4. Dropbox Server Assistance:
   When client A gets a sync notification for file F with block_manifest M:
     A asks Dropbox server: "Which peers on my LAN have blocks from M?"
     Server maintains a peer-block index (ephemeral, in Redis): 
       key: "lan_blocks:{account_id_hash}:{block_hash}"  value: set of peer IPs
     Server responds: { "aabb1234...": ["192.168.1.5", "192.168.1.12"] }
     A fetches from local peers first; falls back to server for missing blocks.
   
5. Reporting back:
   After client A fetches a block (from cloud or LAN), it registers in the server's
   peer-block index (Redis SET with 1-hour TTL):
     "lan_blocks:{account_id_hash}:{block_hash}" → SADD "192.168.1.7"
   This makes client A a source for subsequent LAN peers.
```

**Interviewer Q&As:**

Q1: What if mDNS doesn't work because the office network blocks multicast?
A1: mDNS requires multicast UDP. If blocked, Dropbox falls back to: (1) unicast mDNS-style polling of known IP ranges (if the client can infer the local subnet); (2) checking the Dropbox server for known peers in the same account/team — the server provides the peer IP list directly when it knows the client's IP falls in a corporate IP range; (3) complete fallback to cloud-only sync. The server registers client IPs and groups them by /24 subnet as a heuristic for LAN co-location.

Q2: Is there a security concern with clients serving blocks directly to each other?
A2: Yes, and it's mitigated by the HMAC-based mutual authentication in the peer handshake. The shared_secret is never stored on disk in plaintext — it's derived from the account token which is stored in the system keychain. Additionally, only blocks belonging to namespaces the receiving peer is authorized to access can be requested — the server-provided peer-block map only returns block hashes from namespaces the requesting client is a member of. A malicious peer cannot request arbitrary blocks.

Q3: How does LAN sync interact with selective sync? Client B didn't sync a specific folder locally.
A3: The peer-block map on the server only registers blocks that a peer has **locally downloaded** and verified. If client B hasn't synced folder X (selective sync), it has no blocks from folder X in its local cache and won't be registered as a source for those blocks. Client A will simply not receive B as a peer for those blocks and falls back to cloud download.

---

## 7. Scaling

### Horizontal Scaling

- **Metadata Server**: Stateless application tier. Scale horizontally; route requests to shard via consistent hash of `namespace_id`. Each metadata server instance maintains a local Memcached connection for L1 cache.
- **Block Server**: Stateless; scales horizontally. Any instance can generate presigned S3 URLs. Load balanced round-robin.
- **Notification Server**: Stateful (long-poll connections). Scale by consistent hashing clients to servers by `account_id`. Use Kafka consumer groups so multiple notification servers process changes from the same topic.
- **Sync Workers (background jobs)**: Horizontally scalable consumer pool reading from Kafka. Scale by increasing consumer group size.

### DB Sharding

- MySQL sharded by `namespace_id % N_shards`. Default: 64 shards. Each shard: 1 primary + 2 read replicas.
- Shard map stored in ZooKeeper; cached in-process on Metadata Servers (refreshed every 30 seconds).
- Adding shards: consistent hashing minimizes key movement; a re-sharding job migrates ~1/N_new fraction of data.
- Cross-shard: when listing "all files shared with me", a `sharing_index` table in a separate low-write MySQL instance maintains `(member_id, namespace_id, file_id)` entries, updated asynchronously on ACL changes.

### Replication

- MySQL: semi-synchronous replication (primary waits for ACK from at least 1 replica before commit returns). Balances durability and latency. Replica lag target: < 100 ms.
- S3 / Magic Pocket: Erasure coded 14+3 across 17 storage nodes in a rack. Cross-AZ: 3 copies (one per AZ). Cross-region replication for Business Advanced plans.
- Memcached: No replication. Cache misses simply hit MySQL. Consistent hashing ensures failure of 1 Memcached node affects only 1/N fraction of keys.

### Caching Strategy

| Layer | What | TTL | Invalidation |
|---|---|---|---|
| Metadata Server in-process | Hot namespace shard map | 30 s | ZooKeeper watch |
| Memcached | `file_current_state` per (namespace, file_id) | 60 s | Deleted on every commit to that file |
| Memcached | Folder listing (serialized list) | 30 s | Deleted on any commit in that folder |
| Memcached | Block existence (sha256 → bool) | 24 hours | Never (immutable: if block exists, it always exists) |
| Redis | Presigned URL validity cache (short) | 14 min | TTL-based |
| CDN (CloudFront) | Public shared file bytes | 1 hour | Invalidated on new version commit |

### CDN Strategy

Shared link downloads and public folder access served via CloudFront. Cache key = SHA-256 of block (for block-level delivery). Files accessed via the main Dropbox app (authenticated) are not CDN-cached — they use presigned S3 URLs which bypass CDN. For large public files (> 100 MB), Dropbox uses byte-range CDN caching: CloudFront caches individual range-request responses.

### Interviewer Q&As

Q1: How do you handle a sync storm where 50,000 clients all come online simultaneously and each triggers a sync (e.g., after a company-wide Dropbox maintenance window)?
A1: Multiple mitigation layers: (1) The Notification Server uses a reconnection backoff policy — clients that disconnect get a random reconnection delay of 0–30 seconds (jittered exponential backoff), spreading the reconnection storm over 30 seconds; (2) the sync endpoint (`/list_folder/longpoll`) is served by a stateless tier that rate-limits per account_id (max 10 concurrent sync requests per account); (3) the Metadata Server's Memcached cache absorbs the read spike (folder listings cached for 30 s); (4) predictive autoscaling pre-scales the Metadata Server fleet before known maintenance windows.

Q2: How do you prevent the `blocks` dedup table from being a thundering-herd hot spot during a viral file upload?
A2: The `check_blocks` API is backed by Memcached with a 24-hour TTL for block existence (since blocks are immutable — once written, always present). The first check for a given block hash misses cache and hits MySQL; subsequent checks (from any client, globally) hit Memcached. For a viral file (e.g., a widely-shared ISO image), after the first few clients check its block hashes, all subsequent clients get cache hits. This pattern amplifies with popular content.

Q3: How do you scale the sync notification fan-out for a shared folder with 100,000 members?
A3: Fan-out to 100K users is an extreme case (similar to Twitter's celebrity problem). The Notification Server uses a tiered fan-out strategy: (a) for folders with > 1,000 members, changes are published to a Kafka partition per namespace_id; (b) a dedicated fan-out service consumes this partition and maintains the 100K member → connection mapping; (c) fan-out is rate-limited (max 10K notifications/second per namespace to avoid notification floods from high-frequency changes like a build pipeline); (d) for very high-velocity namespaces, notifications are debounced to 1/second with a "N files changed" aggregated notification.

Q4: How does Dropbox's Magic Pocket differ from S3 in terms of the storage system design?
A4: Magic Pocket (described in Dropbox's 2016 blog post) is a purpose-built object storage system designed to replace their AWS S3 usage for long-term block storage. Key differences: (1) Erasure coding (14+3): stores 14 data shards + 3 parity shards across 17 HDDs in a zone; recovers from any 3 simultaneous disk failures, using ~121% of raw data vs S3's ~300% (3× replication); (2) Optimized for sequential read/write (HDDs, not SSDs) since blocks are immutable after write; (3) Rack-aware placement (within a zone) + cross-zone replication for geo-durability; (4) No object versioning internally (Dropbox handles versioning at metadata layer). Result: ~90% cost reduction vs S3 at Dropbox's scale.

Q5: How would you handle multi-region deployment for Dropbox to serve European users with GDPR-compliant data residency?
A5: Dropbox supports EU data residency via geographic namespace isolation: (1) EU accounts have `namespace_id`s that route exclusively to EU-based MySQL shards and Magic Pocket zones (Frankfurt, Amsterdam); (2) the Metadata Server in EU regions only connects to EU DB shards; (3) edge API gateway routes EU-IP clients to EU regions based on GeoIP; (4) US-EU data migration via an async migration service that moves blocks and metadata to EU storage, atomically switches routing, then deletes US copies. Cross-region GDPR: client data (emails, display names) is further isolated in a separate EU-only `accounts` table shard.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Metadata Server crash mid-commit | Transaction rolled back by MySQL; client retries | Client gets TCP RST or timeout | Idempotent commit: client retries with same session_id; MySQL transaction ensures atomicity |
| Block Server crash after block stored but before ACK | Block in storage, client retries upload | Client gets no ACK | Block upload is idempotent: re-PUT same block → server returns 200; ref_count not double-incremented (check before increment) |
| MySQL primary failure | Writes fail; reads continue from replicas | ZooKeeper heartbeat miss (10 s) | ZooKeeper promotes replica; ~10 s write unavailability; client queues writes and retries |
| S3 / Magic Pocket zone failure | Block upload/download fails for affected zone | HTTP 5xx from S3; health check | Retry to second zone; erasure coding recovers blocks from remaining 14/17 shards |
| LAN switch failure | LAN sync stops; falls back to cloud sync | mDNS announcements stop being received | LAN sync failure is transparent; cloud sync continues; no data loss |
| Notification Server partition | Some clients don't receive push events | Client polls /longpoll and gets changes | Client's sync correctness doesn't depend on push; long-poll is a performance optimization |
| Corrupt block (bit rot in storage) | Client receives corrupted file bytes | SHA-256 mismatch on download | Block Server re-verifies SHA-256 on every download; corrupt block triggers repair from other erasure coding shards |
| User account quota race condition | Two concurrent uploads might both pass quota check | Post-commit quota > limit | Quota updated atomically in commit transaction; second commit fails if `used_bytes + size > quota_bytes` |

### Retries & Idempotency

- All client→server operations carry `X-Dropbox-Idempotency-Key` (session_id for uploads, UUID for metadata mutations). Server stores (key → result) in Redis for 24 hours.
- Block upload: PUT to presigned S3 URL — S3 PUT is inherently idempotent.
- Commit: guarded by `parent_rev` check — replaying the same commit returns the existing file version if content_hash matches, or a conflict error if the namespace advanced.
- Client retry policy: immediate retry × 2, then exponential backoff (2, 4, 8, 16, 32, 64 s), max 5 retries, then surface error to user.

### Circuit Breaker

Each service dependency (MySQL, Memcached, S3) is wrapped in a circuit breaker at the application layer (Hystrix pattern). Parameters: open if error rate > 50% in 20-second window; half-open after 30 seconds; reset on 5 consecutive successes. On circuit open: Metadata Server returns 503; Block Server returns 503; clients display "Dropbox is having trouble" in the tray icon.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Tool |
|---|---|---|---|
| Sync success rate | Counter ratio | < 99.5%/5 min | Datadog |
| Block upload p99 latency | Histogram | > 5 s | Datadog |
| Metadata commit p99 latency | Histogram | > 500 ms | Datadog |
| MySQL replication lag (per shard) | Gauge | > 500 ms | PMM (Percona Monitoring) |
| Memcached hit rate | Counter ratio | < 85% | Memcached stats |
| Kafka consumer lag (notifications) | Gauge | > 5,000 events | Burrow |
| Active long-poll connections | Gauge | — (capacity planning) | Prometheus |
| LAN sync success rate (opt-in telemetry) | Counter ratio | — | Internal dashboard |
| Quota enforcement failures/min | Counter | > 50/min | PagerDuty alert |
| Block corruption detections | Counter | > 0/hour | PagerDuty P1 |

### Distributed Tracing

OpenTelemetry instrumentation on all services. Trace propagated via `traceparent` HTTP header. Key spans: API Gateway parse → Auth check → Metadata Server dispatch → MySQL query → Memcached check → Block Server presign → S3 upload. Sampled at 0.1% for normal traffic; 100% for errors and slow requests (> 2× p99). Stored in Jaeger with 7-day retention. Used to diagnose outlier upload latency spikes (commonly caused by MySQL hot-shard write contention).

### Logging

- Structured JSON logs: `{"ts":..., "trace_id":..., "shard_id":..., "account_id_hash":..., "op":"commit_file", "duration_ms":42, "file_size":4096000, "blocks_uploaded":3}`
- PII: account emails never logged in plain text; only HMAC(email, log_key) for correlation.
- Audit log: separate immutable pipeline (write to Kafka → S3 → Glacier). Retention: 7 years (compliance). Includes all file access, sharing, deletion events.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|---|---|---|---|
| Chunking strategy | Fixed-size 4 MB blocks | CDC (Rabin fingerprinting) | Simpler; Dropbox's workload benefits minimally from CDC; better understood operationally |
| Metadata vs block store | Separate services | Unified service | Allows independent scaling; metadata is high-frequency small ops; blocks are low-frequency large ops |
| Metadata DB | MySQL (sharded) | Spanner / CockroachDB | Dropbox's operational history; MySQL well-understood at scale; namespace sharding makes joins rare |
| Sync cursor | Per-namespace `server_rev` (monotonic int) | Timestamp-based | Timestamps have clock skew; monotonic rev is deterministic and index-efficient |
| Conflict resolution | Conflict copy (both versions kept) | Last-write-wins | Users prefer not losing data; conflict copy is the safest default for non-real-time files |
| LAN sync | mDNS + direct TCP + server peer map | Cloud-only, or BitTorrent | Reduces egress cost in office environments; server-assisted adds only minimal coordination overhead |
| Block storage | Magic Pocket (erasure coding 14+3) | Pure S3 replication | 90% cost reduction at scale; erasure coding gives equivalent durability at lower overhead |
| Notification model | Long-poll + Kafka fan-out | WebSocket, polling | Long-poll scales well (stateless); Kafka decouples commit latency from notification latency |
| Quota accounting | Synchronous in commit transaction | Async deferred | Consistency: users should immediately see quota exhaustion; prevents over-quota race conditions |
| Dedup scope | Global (across all users) | Per-user only | Significantly higher dedup ratio; SHA-256 collision risk is negligible |

---

## 11. Follow-up Interview Questions

Q1: How would you design the Dropbox mobile camera upload feature to be bandwidth-efficient?
A1: Camera upload triggers on: new photo detected, WiFi connected (configurable to cellular), app in background. Each photo is chunked and checked for dedup first (many photos are duplicates of edits). The upload is low-priority (background QoS class) so it doesn't interfere with foreground app usage. On iOS, the app uses NSURLSession background transfer which persists across app restarts — the OS manages the upload and notifies the app on completion. Bandwidth throttling: camera uploads rate-limited to 50% of available bandwidth on cellular (when enabled), 100% on WiFi. Metadata-only mode: if quota is nearly exhausted, upload metadata and thumbnails, defer full resolution upload until space is available.

Q2: How does Dropbox handle a file that's being actively written by an application (e.g., a Word document being saved)?
A2: The desktop client's file watcher receives rapid successive inotify/FSEvents events during a save. The client uses a debounce window (typically 2 seconds): it waits for file system events to settle before starting the sync process. This prevents syncing partially-written files. For applications that write via atomic rename (write to temp file, then rename to target — as Word and Excel do), the rename event triggers sync, and by definition the file is complete at that point. The client also checks if the file is locked (Windows: CreateFile with SHARE_READ check; macOS: flock) and waits for the lock to be released before reading.

Q3: Explain Dropbox's approach to zero-knowledge encryption for enterprise teams.
A3: Dropbox Business "Extended" plan offers an option where blocks are encrypted with customer-managed keys (CMK) before leaving the client. The client generates a per-file AES-256 key, encrypts the block bytes before computing the SHA-256 (so SHA-256 is of ciphertext), and uploads ciphertext blocks to Magic Pocket. The per-file key is wrapped (encrypted) with the customer's CMK held in an HSM/KMS (e.g., AWS KMS). Dropbox servers only see ciphertext blocks and wrapped keys — they cannot decrypt without the customer's CMK. Trade-off: content-based dedup across users is **not possible** with CMK (different users encrypting the same file get different ciphertext); dedup only works within the same CMK scope (same team).

Q4: How would you design versioning with 180-day retention for 700 M users?
A4: `file_journal` is the version store. Each commit creates a new journal entry. The current version is the latest non-deleted `journal_id` for a given `file_id`. Older versions are accessible by querying `file_journal WHERE file_id = X AND server_mtime >= NOW() - INTERVAL 180 DAY`. A background garbage collection job runs nightly: (1) identify file_journal entries older than 180 days; (2) archive to S3 Glacier (for compliance/support access); (3) decrement block ref_counts for blocks exclusively referenced by expired versions; (4) delete block objects with ref_count = 0 (after 7-day grace period for soft-delete restore). The GC job is distributed (processes one MySQL shard at a time) to avoid lock contention.

Q5: How does selective sync work technically? If I un-sync a folder, what happens on the server?
A5: Selective sync is a **client-side** concept only. The server stores all files regardless of which clients have synced them locally. The desktop client maintains a `selective_sync_exclusions` list in its local SQLite DB. When processing a sync response, the client skips downloading files whose path prefix matches an exclusion. The server is unaware of this preference — it sends all changes. When the user adds a folder back to selective sync, the client processes the accumulated delta since it last synced that folder and downloads any new files. Folder exclusions are synced to Dropbox servers as part of the client configuration, so they are consistent across devices (exclusions on MacBook also exclude on Windows laptop).

Q6: How would you handle rate limiting fairly for Dropbox API while not impacting the sync client?
A6: Separate rate limit quotas for different API consumers: (1) Official sync client: no rate limit on core sync operations (list_folder, upload, download); rate-limited to 10,000 ops/hour on management APIs (create_folder, move, share); (2) Third-party apps (OAuth): 5,000 API calls/hour per app per user (configurable per developer tier); (3) Dropbox Paper and first-party features: separate high-priority quota; (4) Abuse detection: any single IP making > 100 auth failures/hour is blocked via WAF. Sync traffic uses separate connection pools from API traffic to prevent noisy-neighbor between developers testing apps and users syncing files.

Q7: What happens to in-progress syncs during a Dropbox platform deployment?
A7: Rolling deployments with no forced reconnects: (1) New application tier instances are brought up with the new code; (2) the load balancer routes new connections to new instances; (3) existing long-poll connections on old instances drain naturally (client re-connects every 8 minutes at most, or sooner if the server sends `changes: true`); (4) MySQL schema changes are backward-compatible (additive only; no column removals); (5) blue-green deployment for MySQL stored procedure changes (rare). Client-visible impact: none for transactional operations; possible 10–30 second delay in notifications while connections drain.

Q8: How does Dropbox's block-level sync interact with encrypted files (FileVault on macOS)?
A8: FileVault encrypts the entire disk at rest — it is transparent to applications. Dropbox reads file contents via normal file system APIs; it reads plaintext bytes regardless of whether the underlying disk is encrypted. The encryption/decryption happens in the kernel. Dropbox then optionally re-encrypts at the Dropbox level (server-side encryption with Dropbox-managed keys, or CMK for enterprise). The block SHA-256 is computed on plaintext bytes from Dropbox's perspective. FileVault does NOT enable zero-knowledge encryption with respect to Dropbox — Dropbox sees plaintext unless CMK is configured.

Q9: How would you detect and quarantine malware uploaded to Dropbox?
A9: Async pipeline: (1) Upload commit → publish event to Kafka `file.uploaded`; (2) Virus scanner fleet (ClamAV + proprietary ML malware scanner) consumes events, downloads file from Magic Pocket via internal presigned URL; (3) On detection: update `blocks.scan_status = 'quarantined'` for all blocks; set `file_current_state.is_quarantined = true`; publish `FILE_QUARANTINED` event; (4) Quarantined files: still stored (for forensics), but download requests return HTTP 403 with error code `MALWARE_DETECTED`; (5) User notified via email and in-app notification; (6) False positive appeal process: user can request re-scan; Dropbox security team reviews. Scanning is async (does not block upload commit) to avoid degrading upload latency.

Q10: How would you ensure consistency between `file_journal` and `file_current_state` when both are written in the same transaction but the process crashes between the two writes?
A10: MySQL InnoDB ACID transactions handle this: both writes are in a single `BEGIN...COMMIT` block. If the process crashes after the INSERT to `file_journal` but before the UPSERT to `file_current_state`, the entire transaction is rolled back by InnoDB's crash recovery (using the undo log). The client's idempotent retry then re-executes the full transaction. The journal and current_state are guaranteed to be consistent after crash recovery. This is why the dual-write approach works safely — the two tables are in the same MySQL shard (same database instance), allowing a single transaction to cover both.

Q11: How does Dropbox handle file paths with Unicode characters and case sensitivity across platforms?
A11: Dropbox normalizes file paths to Unicode NFC form and stores them case-preserved but case-insensitively unique (to match Windows/macOS default case-insensitive file systems). MySQL's `utf8mb4_unicode_ci` collation is used for the `filename` column, providing case-insensitive uniqueness enforcement via `UNIQUE KEY uk_namespace_file_rev`. On Linux clients (case-sensitive file system), Dropbox enforces the server's case-insensitive namespace by rejecting operations that would create two files differing only in case. Emoji filenames are supported (4-byte UTF-8, requiring `utf8mb4` charset).

Q12: What is the `Dropbox-API-Arg` header pattern and why use it instead of JSON request bodies?
A12: Dropbox's v2 API uses a content-split pattern: file bytes in the HTTP body + metadata in `Dropbox-API-Arg` header (JSON-encoded). This is necessary for the upload endpoint — you can't have both a JSON body and a binary body in the same HTTP request without multipart encoding overhead. Multipart adds ~200 bytes overhead and requires content-type parsing — fine for small files but wasteful for TB-scale transfers. The header-based approach lets the server parse metadata from the header before streaming the body, enabling early validation (quota check, path validation) without buffering the entire upload.

Q13: How would you implement Dropbox's Paper (collaborative documents) storage model differently from regular file sync?
A13: Paper documents are not stored as files in the block store — they are stored as operational transformation (OT) event logs in a separate database (Cassandra, append-only). Each edit is a tiny structured operation (insert/delete at position). The "file bytes" representation is always computed on-demand by replaying the event log (or from a cached snapshot every N operations). Paper documents appear in the Drive file tree as metadata-only entries with no block manifest; their content is fetched from the Paper service via a separate API. Version history in Paper is per-operation granularity (see every keystroke), not per-save.

Q14: How does quota work for shared folders — does each member's quota get charged?
A14: Shared folder quota is charged only to the folder owner, not to members. This is a critical UX design decision: if you share a 100 GB folder with 10 people, only your quota decreases. Members can contribute files to a shared folder (if they have edit access), and those contributions count against the contributor's quota (not the owner's). This requires per-file quota tracking: `file_current_state` records `creator_id` and quota is tracked per creator. The commit transaction decrements `accounts.used_bytes` for the user who is uploading the file, regardless of which namespace the file lands in.

Q15: How would you implement Dropbox Rewind — rewinding an entire account or folder to a point in time?
A15: Dropbox Rewind traverses the `file_journal` table for the target namespace: (1) User selects target timestamp T; (2) Server queries: `SELECT DISTINCT file_id, MAX(journal_id) as last_rev FROM file_journal WHERE namespace_id = X AND server_mtime <= T GROUP BY file_id` — this gives the last state of every file as of time T; (3) For each file: if the file existed at T but not now, it is restored; if it was modified, the version at T is restored; if it was created after T, it is deleted; (4) All these restores/deletions are applied as new journal entries (rewind doesn't rewrite history — it creates forward mutations that reproduce the historical state); (5) The operation is transactional per batch (1000 files/transaction) and resumable (progress tracked by the last applied `journal_id`).

---

## 12. References & Further Reading

- **Dropbox Infrastructure Blog: Rewriting the Heart of Our Sync Engine** (2020) — https://dropbox.tech/infrastructure/rewriting-the-heart-of-our-sync-engine
- **Dropbox Tech Blog: Magic Pocket — Building a Dedicated Storage System** (2016) — https://dropbox.tech/infrastructure/magic-pocket-internet-scale-blob-store
- **Dropbox Tech Blog: Scaling Dropbox's Block Store** — https://dropbox.tech/infrastructure/-llbb-
- **Dropbox API v2 Documentation** — https://www.dropbox.com/developers/documentation/http/documentation
- **Dropbox Content Hash Documentation** — https://www.dropbox.com/developers/reference/content-hash
- **rsync Algorithm Paper** — Andrew Tridgell, Paul Mackerras, 1996. https://rsync.samba.org/tech_report/
- **Designing Data-Intensive Applications** — Martin Kleppmann, O'Reilly, 2017. Chapters 5, 6, 11 (stream processing).
- **MySQL High Performance, 4th Edition** — Baron Schwartz et al., O'Reilly. (MySQL sharding and replication patterns)
- **The Chubby Lock Service for Loosely-Coupled Distributed Systems** — Burrows, OSDI 2006. https://research.google/pubs/pub27897/ (ZooKeeper is based on similar principles)
- **Dropbox Security Whitepaper** — https://www.dropbox.com/security/architecture (overview of encryption model)
- **mDNS / DNS-SD RFC** — RFC 6762 (mDNS), RFC 6763 (DNS-SD). https://tools.ietf.org/html/rfc6762
- **Erasure Coding for Distributed Storage Survey** — Plank et al., 2009. https://web.eecs.utk.edu/~jplank/plank/papers/CS-08-627.html
