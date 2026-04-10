# System Design: Collaborative Document Editor (Google Docs Scale)

---

## 1. Requirement Clarifications

### Functional Requirements
1. Multiple users can simultaneously edit the same document; all changes appear in real time on all connected clients (< 500 ms P99 propagation within a region).
2. Changes are merged without data loss regardless of edit ordering (conflict resolution).
3. Cursor presence: each collaborator's cursor position is visible to others with their name and a color-coded indicator, updated in real time.
4. Rich text support: bold, italic, underline, headings, lists, links, tables, inline images.
5. Complete revision history: every version of the document is stored; users can view and restore any historical version.
6. Offline editing: users can edit while disconnected; changes sync automatically on reconnect without data loss.
7. Document sharing: owner can share with view-only, comment, or edit permissions; share via link or specific user invitation.
8. Comments and suggestions: users can add inline comments (threaded replies); suggest edits (tracked changes mode).
9. Autocomplete and spell-check (client-side; out of scope for server design).
10. Export to PDF, DOCX, plain text.

### Non-Functional Requirements
1. Consistency: no data loss; every user's changes must eventually be reflected in the canonical document (strong eventual consistency via CRDT/OT).
2. Latency: local edits applied instantly on client; remote changes visible within 500 ms P99 within the same region.
3. Scalability: 1 B documents stored; 100 M daily active users; 10 M concurrent editing sessions.
4. Availability: 99.99% uptime (< 52 min/year); document access should degrade gracefully (read-only mode during writes outage).
5. Durability: zero document data loss; 11-nines storage durability.
6. Security: document contents are encrypted at rest and in transit; no unauthorized access.
7. Offline capability: clients must function offline and reconcile without server involvement until reconnection.
8. History: full revision history retained indefinitely; efficient storage (not full snapshot per edit).

### Out of Scope
- Real-time voice/video in document (Google Meet integration)
- Spreadsheet calculations (Sheets-style formula engine)
- Presentation-mode rendering (Slides equivalent)
- Full-text search across all documents (mentioned as extension)
- AI writing assistance (mentioned as extension)
- Native mobile application specifics

---

## 2. Users & Scale

### User Types
| Role | Description |
|---|---|
| Owner | Created the document; full admin rights; can transfer ownership |
| Editor | Full read/write access; can add/delete content and comments |
| Commenter | Can add/reply to comments; cannot edit document content |
| Viewer | Read-only access; sees live edits but cannot contribute |
| Anonymous | View-only via public link; limited functionality |

### Traffic Estimates

**Assumptions:**
- 1 B total documents; 100 M DAU; 10 M concurrent editing sessions
- Average collaborators per active document: 3 (many documents have 1 active user; few have 100+)
- Average characters typed per user per minute: 40 (typical typing speed)
- Each keystroke = 1 operation; approximate operation size: 50 bytes (op type, position, character, metadata)
- Concurrent editing sessions: 10 M sessions × 3 avg users = 30 M connected WebSocket clients
- Comments: 5 M new comments/day

| Metric | Calculation | Result |
|---|---|---|
| Concurrent WebSocket connections | 10 M sessions × 3 users/session | 30 M connections |
| Operation write rate | 30 M users × 40 chars/min / 60 | 20 M operations/s |
| Operation bytes/s | 20 M ops/s × 50 bytes | 1 GB/s (1 Gbps writes) |
| Operation fan-out (broadcast) | 20 M ops/s × 2 other users avg | 40 M deliveries/s |
| Presence updates | 30 M users × 1 cursor/s | 30 M presence events/s |
| REST API (document open/close) | 10 M sessions × 2 calls | 20 M calls/session day ÷ 86400 = 231 opens/s |
| Revision history writes | 20 M ops/s → batched snapshots 1/min → 10 M sessions × 1 snapshot/min | 167 k snapshots/s |
| Storage: operation log | 20 M ops/s × 50 bytes × 86400 s | ~86 TB/day raw (before compaction) |
| Storage: snapshots | 1 B docs × avg 100 KB | 100 PB (total document corpus) |
| Storage: compressed history | 100 PB × 0.3 compression ratio | 30 PB |

**Note**: In practice, not all 30 M users type simultaneously; the 20 M ops/s is a theoretical peak. Real-world measurement at Google Docs scale is closer to 1–5 M ops/s during business hours.

### Latency Requirements
| Operation | Target (P50) | Target (P99) | Notes |
|---|---|---|---|
| Local keystroke apply (client-side) | < 1 ms | < 5 ms | Immediate local application |
| Remote change propagation (same region) | < 50 ms | < 500 ms | Perceived collaboration quality |
| Document load (first meaningful render) | < 1 s | < 3 s | Includes snapshot fetch + recent ops |
| Revision history fetch | < 200 ms | < 1 s | Index lookup + snapshot fetch |
| Permission check | < 10 ms | < 50 ms | Cached in auth service |
| Comment submit | < 100 ms | < 300 ms | |
| Offline sync on reconnect | < 2 s for small divergence | < 10 s for large divergence | Depends on operation backlog size |

### Storage Estimates
| Data | Size | Volume | Total |
|---|---|---|---|
| Document snapshots (current state) | avg 100 KB compressed | 1 B documents | 100 TB (current versions) |
| Operation log (per document) | avg 50 bytes/op × 10 k ops/document lifetime | 1 B documents | 500 TB |
| Snapshots for history (one per hour of editing, compressed delta) | avg 10 KB/snapshot | 1 B docs × 50 editing hours average | 500 TB |
| Metadata (title, permissions, sharing settings) | 5 KB/document | 1 B documents | 5 TB |
| Comments (threaded, with reactions) | 2 KB/comment × 5 M/day × 365 days × 5 years | — | ~18 TB over 5 years |
| Presence data | ephemeral; not persisted | — | Redis only |
| **Total** | | | ~1.1 PB (manageable at Google scale) |

### Bandwidth Estimates
| Flow | Calculation | Result |
|---|---|---|
| Client → server (operations) | 5 M concurrent active typists × 40 chars/min/60 × 50 bytes × 8 | ~267 Mbps |
| Server → clients (fan-out) | 267 Mbps × 2× fan-out | ~534 Mbps |
| Document load (snapshot + recent ops) | 231 opens/s × 100 KB | ~185 Mbps |
| Presence updates | 30 M events/s × 30 bytes × 8 (aggressive; batched in practice) | ~7.2 Gbps (requires batching) |

---

## 3. High-Level Architecture

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│                        CLIENT (Browser / Mobile App)                              │
│                                                                                   │
│  ┌─────────────────────────────────────────────────────────────────────────────┐ │
│  │  CRDT Engine (Yjs / Automerge / custom)                                     │ │
│  │  - Local document state (CRDT data structure)                               │ │
│  │  - Applies local operations immediately (optimistic)                        │ │
│  │  - Queues operations for server sync                                        │ │
│  │  - Integrates remote operations from server                                 │ │
│  └──────────────────────┬────────────────────────────────────────────────────── ┘ │
│                         │ WebSocket (ops + presence)                             │
└─────────────────────────┼─────────────────────────────────────────────────────────┘
                          │
                          ▼
┌───────────────────────────────────────────────────────────────────────────────────┐
│                   API GATEWAY / LOAD BALANCER                                     │
│   TLS termination; JWT validation; rate limiting; sticky routing by document_id   │
└───────────────────────────────────────────────────────────────────────────────────┘
                          │
           ┌──────────────┼──────────────────────┐
           ▼              ▼                      ▼
┌─────────────────┐  ┌────────────────────┐  ┌──────────────────────────────────────┐
│  REST API       │  │  DOCUMENT SESSION  │  │  AUTH / PERMISSIONS SERVICE          │
│  Service        │  │  SERVERS           │  │                                      │
│  - Document     │  │                    │  │  JWT issuance + verification         │
│    CRUD         │  │  One session server│  │  Permission model (ACLs per doc)     │
│  - Sharing      │  │  per document;     │  │  Cached in Redis; persisted in PG    │
│  - Export       │  │  all collaborators │  │                                      │
│  - History list │  │  for a doc connect │  └──────────────────────────────────────┘
│  - Comments     │  │  to the SAME pod   │
└─────────────────┘  │  (consistent hash) │
                     │                    │
                     │  ┌──────────────┐  │
                     │  │ Session State│  │
                     │  │ - CRDT doc   │  │
                     │  │   (in RAM)   │  │
                     │  │ - Connected  │  │
                     │  │   clients    │  │
                     │  │ - Cursor     │  │
                     │  │   positions  │  │
                     │  └───────┬──────┘  │
                     └──────────┼──────────┘
                                │
              ┌─────────────────┼──────────────────────┐
              │                 │                      │
              ▼                 ▼                      ▼
┌─────────────────────┐ ┌──────────────────┐ ┌────────────────────────────────────┐
│   OPERATION LOG     │ │  DOCUMENT STORE  │ │  PRESENCE / AWARENESS SERVICE      │
│   SERVICE           │ │                  │ │                                    │
│                     │ │  Current state   │ │  Cursor positions, user colors     │
│  Kafka topic per    │ │  snapshots in    │ │  Stored in Redis (TTL 30s)         │
│  document (by hash  │ │  object store    │ │  Fan-out via pub/sub               │
│  ring)              │ │  (S3/GCS)        │ │  Batched: 100ms windows            │
│                     │ │                  │ │                                    │
│  Ops persisted in   │ │  Revision index  │ │                                    │
│  Cassandra for fast │ │  in PostgreSQL   │ │                                    │
│  replay             │ │                  │ │                                    │
└─────────────────────┘ └──────────────────┘ └────────────────────────────────────┘
              │
              ▼
┌──────────────────────────────────────────────────────────────────────────────────┐
│  BACKGROUND SERVICES                                                             │
│  ┌────────────────────┐  ┌────────────────────┐  ┌────────────────────────────┐ │
│  │  Snapshot Service  │  │  History Compactor │  │  Export Service            │ │
│  │  Periodically      │  │  Merges old ops    │  │  Renders doc to PDF/DOCX   │ │
│  │  writes full doc   │  │  into compressed   │  │  on demand                 │ │
│  │  snapshot to S3    │  │  snapshots; deletes│  │                            │ │
│  │  (every 5 min of   │  │  raw op log entries│  │                            │ │
│  │  editing activity) │  │  older than 30 days│  │                            │ │
│  └────────────────────┘  └────────────────────┘  └────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **Client CRDT Engine**: Applies edits locally (zero latency for the typing user), queues operations to send to server, and integrates remote operations from other users. This is the core of collaborative editing correctness.
- **API Gateway**: Routes WebSocket connections for a document to the correct session server (consistent hash by document_id). Routes REST calls to stateless API servers.
- **Document Session Server**: The authoritative in-memory CRDT state for one document. All clients editing a document connect to the same session server. Receives operations, integrates them into the CRDT, broadcasts to other clients, and writes to the operation log.
- **Operation Log (Kafka + Cassandra)**: Durable ordered log of all operations. Kafka for real-time delivery; Cassandra for queryable history.
- **Document Store (S3)**: Stores periodic snapshots of the full document state. Used for fast document loading (load latest snapshot + replay recent ops from Cassandra).
- **Presence Service**: Stores cursor positions and user awareness (typing indicator, selection range) in Redis with TTL. Decoupled from the operation pipeline to avoid cursor movement from slowing down text operations.
- **Snapshot Service**: Background worker that periodically creates a full document snapshot, reducing the number of operations that must be replayed on document load.

**Primary Data Flow (User A types a character, User B sees it):**
1. User A types "H". The client CRDT engine creates an INSERT operation `{type: INSERT, pos: 42, char: "H", author: user_A, clock: {user_A: 103}}` and applies it locally (User A sees the character immediately).
2. Client sends the operation over WebSocket to the document session server.
3. Session server receives the op, validates the user has edit permission (cached ACL check), integrates it into the server-side CRDT state, and broadcasts the op to all other connected clients (User B, User C).
4. Session server writes the op to Kafka topic `doc-ops-{shard}` (keyed by document_id for ordering).
5. User B's client receives the broadcasted op, feeds it into its CRDT engine, which determines the correct position (using vector clocks / CRDT merge semantics) and inserts "H" at the right position.
6. A Kafka consumer (Operation Log Service) persists the op to Cassandra.

---

## 4. Data Model

### Entities & Schema

```sql
-- Documents metadata
CREATE TABLE documents (
    doc_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id        UUID NOT NULL REFERENCES users(user_id),
    title           VARCHAR(500) NOT NULL DEFAULT 'Untitled Document',
    doc_type        VARCHAR(20) NOT NULL DEFAULT 'text',   -- 'text', 'spreadsheet', 'presentation'
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),    -- updated on every operation
    last_snapshot_seq BIGINT DEFAULT 0,                    -- sequence number of last S3 snapshot
    word_count      INTEGER DEFAULT 0,                     -- approximate, updated in background
    language        CHAR(5) DEFAULT 'en',
    is_trashed      BOOLEAN DEFAULT FALSE,
    trashed_at      TIMESTAMPTZ,
    storage_bytes   BIGINT DEFAULT 0                       -- approximate document size
);
CREATE INDEX idx_documents_owner ON documents(owner_id, updated_at DESC);
CREATE INDEX idx_documents_updated ON documents(updated_at DESC) WHERE is_trashed = FALSE;

-- Users
CREATE TABLE users (
    user_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email           VARCHAR(254) NOT NULL UNIQUE,
    display_name    VARCHAR(100) NOT NULL,
    avatar_url      TEXT,
    google_id       VARCHAR(50) UNIQUE,    -- OAuth provider ID
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Permissions (ACL)
CREATE TABLE document_permissions (
    doc_id          UUID NOT NULL REFERENCES documents(doc_id) ON DELETE CASCADE,
    principal_type  VARCHAR(10) NOT NULL,  -- 'user', 'link', 'domain'
    principal_id    UUID,                  -- user_id; NULL for link/domain sharing
    permission      VARCHAR(10) NOT NULL,  -- 'view', 'comment', 'edit', 'owner'
    created_by      UUID NOT NULL REFERENCES users(user_id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at      TIMESTAMPTZ,
    link_token      VARCHAR(64) UNIQUE,    -- for link-based sharing
    PRIMARY KEY (doc_id, principal_type, COALESCE(principal_id::text, link_token))
);
CREATE INDEX idx_permissions_user ON document_permissions(principal_id, doc_id) WHERE principal_type = 'user';

-- Revision index (pointer to snapshots + op sequence ranges)
CREATE TABLE document_revisions (
    doc_id          UUID NOT NULL REFERENCES documents(doc_id),
    revision_id     BIGSERIAL,
    op_seq_start    BIGINT NOT NULL,        -- first operation sequence in this revision
    op_seq_end      BIGINT NOT NULL,        -- last operation sequence in this revision
    snapshot_s3_key TEXT,                   -- S3 key of the snapshot (if this is a snapshot revision)
    author_id       UUID REFERENCES users(user_id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    summary         TEXT,                   -- auto-generated or user-provided
    PRIMARY KEY (doc_id, revision_id)
);
CREATE INDEX idx_revisions_doc_time ON document_revisions(doc_id, created_at DESC);

-- Operations log (Cassandra CQL — for queryable history)
/*
CREATE TABLE document_operations (
    doc_id      UUID,
    shard       INT,           -- (unix_ts / 3600) % 64  to bound partition size
    seq_num     BIGINT,        -- monotonically increasing per document
    op_type     TEXT,          -- 'insert', 'delete', 'format', 'comment'
    author_id   UUID,
    vector_clock TEXT,         -- JSON of Lamport/vector clock at time of op
    op_data     BLOB,          -- serialized CRDT operation (protobuf)
    client_ts   TIMESTAMP,     -- client's local timestamp
    server_ts   TIMESTAMP,     -- server receipt timestamp
    PRIMARY KEY ((doc_id, shard), seq_num)
) WITH CLUSTERING ORDER BY (seq_num ASC)
  AND default_time_to_live = 7776000;  -- 90-day TTL; older ops in snapshots
*/

-- Comments
CREATE TABLE comments (
    comment_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    doc_id          UUID NOT NULL REFERENCES documents(doc_id),
    author_id       UUID NOT NULL REFERENCES users(user_id),
    anchor_start    INTEGER NOT NULL,    -- character position in document
    anchor_end      INTEGER NOT NULL,
    anchor_text     TEXT,                -- copy of anchored text at time of comment
    body            TEXT NOT NULL,
    status          VARCHAR(20) DEFAULT 'open',  -- 'open', 'resolved'
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_comments_doc ON comments(doc_id, created_at DESC);

CREATE TABLE comment_replies (
    reply_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    comment_id      UUID NOT NULL REFERENCES comments(comment_id) ON DELETE CASCADE,
    author_id       UUID NOT NULL REFERENCES users(user_id),
    body            TEXT NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- S3 snapshot manifest (stored as document metadata, not in SQL)
-- S3 key format: docs/{doc_id}/snapshots/{seq_num}.bin
-- Object format: compressed (zstd) serialized CRDT state + metadata header
-- Additional: docs/{doc_id}/current.bin → points to latest snapshot (updated atomically)
```

**Redis Schema (ephemeral state):**
```
# Active session metadata
HSET doc:session:{doc_id}
    server_id  "session-pod-42"
    active_users  "3"
    last_op_seq  "10293"
    last_active_ts  "1712345678"
TTL: 300 s (refreshed on each operation)

# Cursor/presence per user per document
HSET doc:presence:{doc_id}:{user_id}
    display_name  "Alice"
    color         "#FF6B35"
    cursor_pos    "1052"           # character offset
    selection_start  "1052"
    selection_end    "1052"
    ts            "1712345678123"
TTL: 30 s (refreshed on each cursor movement; expires if user disconnects)

# Permission cache (avoid DB hit on every operation)
SET doc:perm:{doc_id}:{user_id}  "edit"
TTL: 60 s

# Operation sequence counter per document (atomic increment)
# Not in Redis — use Cassandra seq_num; Redis INCR too risky as source of truth
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| PostgreSQL | Document metadata, permissions, revision index, comments | ACID, rich queries, relational integrity for permissions | Not suited for high-volume operation log writes | **Selected** for metadata |
| Cassandra | Operation log (high-volume append writes) | Excellent append-only write throughput, TTL support, time-series natural fit, multi-DC replication | No secondary indexes suitable for full-text search | **Selected** for op log |
| S3 (object store) | Document snapshots, exported files | Infinite durability, cost-effective for large blobs, CDN-friendly for load | Eventual consistency (mitigated by conditional writes) | **Selected** for snapshots |
| Redis | Presence data, permission cache, session routing, pub/sub | Sub-millisecond, TTL native, pub/sub for presence fan-out | In-memory cost; not durable for critical data | **Selected** for ephemeral state |
| Kafka | Real-time operation streaming, fan-out to background services | Ordered per partition, durable, multi-consumer | Not a query engine | **Selected** as event bus |
| Elasticsearch | Full-text search across documents | Best-in-class text search | Operational complexity; eventual consistency | Not selected for core (extension) |
| DynamoDB | — | Serverless | No CRDT-friendly data types; no sorted range scans | Not selected |

---

## 5. API Design

### WebSocket Protocol (Real-Time Collaboration)

```
Connection: wss://docs.example.com/v1/ws/{doc_id}
  Auth: JWT in Sec-WebSocket-Protocol header or ?token= query param
  On connect: server validates permission (EDIT, COMMENT, or VIEW)

# Message format (MessagePack or JSON; MessagePack preferred for binary ops)
# All messages have: { "type": string, "v": 1, "ts": unix_ms }

# Client → Server

# Submit an operation (editor only)
{ "type": "OP",
  "op": {
    "id":      "uuid",              # client-generated op UUID (for dedup/ack)
    "type":    "insert",            # insert | delete | format | retain
    "pos":     1052,                # character position in document
    "text":    "H",                 # for insert operations
    "len":     1,                   # for delete operations
    "attrs":   {"bold": true},      # for format operations
    "clock":   {"userA": 103, "userB": 87}  # vector clock for ordering
  }
}

# Update cursor/selection (all non-viewers)
{ "type": "AWARENESS",
  "cursor":    1053,
  "sel_start": 1053,
  "sel_end":   1053
}

# Acknowledge received ops (for server to know client is in sync)
{ "type": "ACK", "seq": 10293 }

# Request history (document load)
{ "type": "SYNC_REQUEST", "since_seq": 10200 }

# Server → Client

# Broadcast operation from another user
{ "type": "OP",
  "seq":   10293,
  "op":    { ...same structure as above... },
  "author": { "user_id": "...", "display_name": "Alice" }
}

# Acknowledge client's operation (confirms server persistence)
{ "type": "OP_ACK",
  "op_id": "uuid",
  "seq":   10293     # server-assigned sequence number
}

# Batch ops since requested sequence (for reconnect)
{ "type": "SYNC_RESPONSE",
  "ops": [ ...array of OP messages... ],
  "current_seq": 10293
}

# Presence broadcast (batched every 100 ms)
{ "type": "AWARENESS_UPDATE",
  "users": [
    { "user_id": "...", "display_name": "Alice", "color": "#FF6B35",
      "cursor": 1053, "sel_start": 1053, "sel_end": 1053 }
  ]
}

# Session state (sent on connect, includes current users)
{ "type": "SESSION_STATE",
  "doc_id": "uuid",
  "seq":    10293,
  "users":  [...],
  "permissions": "edit"  # this user's permission level
}

Rate limits:
  - Operations: 100/s per user per document
  - Awareness: 10/s per user per document
  - Max document size: 1 M characters (soft limit; warning at 800 k)
```

### REST API

```
# Document Management
POST /api/v1/documents
  Auth: Bearer <jwt>
  Body: { "title": "New Document", "type": "text" }
  Response: 201 { doc_id, title, created_at, edit_url }

GET /api/v1/documents/{doc_id}
  Auth: Bearer <jwt> or link token
  Response: 200 { doc_id, title, owner, permissions, snapshot_url, current_seq }

DELETE /api/v1/documents/{doc_id}
  Auth: Bearer <jwt> (owner only)
  Response: 204

# Snapshot (for initial load — faster than WebSocket sync)
GET /api/v1/documents/{doc_id}/snapshot
  Auth: Bearer <jwt>
  Response: 200 {
    "seq": 10200,               # sequence number of this snapshot
    "content": "<base64 encoded CRDT state>",
    "content_type": "application/x-crdt-yjs",
    "size_bytes": 45234
  }
  Cache: CDN cacheable by (doc_id, seq) — immutable once written

# Sharing / Permissions
PUT /api/v1/documents/{doc_id}/permissions
  Auth: Bearer <jwt> (owner or editor with share permission)
  Body: { "principal_type": "user", "principal_id": "uuid", "permission": "edit" }
  Response: 200 { "share_link": null }

POST /api/v1/documents/{doc_id}/share-link
  Auth: Bearer <jwt> (owner only)
  Body: { "permission": "view" }
  Response: 200 { "link": "https://docs.example.com/d/{link_token}" }

# Revision History
GET /api/v1/documents/{doc_id}/revisions?limit=20&before=2024-04-01
  Auth: Bearer <jwt>
  Response: 200 { "revisions": [{ revision_id, created_at, author, summary }] }

GET /api/v1/documents/{doc_id}/revisions/{revision_id}/content
  Auth: Bearer <jwt>
  Response: 200 { "content": "...", "seq": 9000 }   # reconstructed state at that revision

POST /api/v1/documents/{doc_id}/revisions/{revision_id}/restore
  Auth: Bearer <jwt> (editor+ required)
  Response: 202 { "job_id": "uuid" }   # async restore; result via WebSocket

# Comments
POST /api/v1/documents/{doc_id}/comments
  Auth: Bearer <jwt> (commenter+ required)
  Body: { "anchor_start": 1000, "anchor_end": 1050, "body": "Good point here" }
  Response: 201 { comment_id, author, created_at }

GET /api/v1/documents/{doc_id}/comments
  Auth: Bearer <jwt>
  Response: 200 { "comments": [...] }

# Export
POST /api/v1/documents/{doc_id}/export
  Auth: Bearer <jwt>
  Body: { "format": "pdf" }     # pdf | docx | txt | html
  Response: 202 { "job_id": "uuid" }

GET /api/v1/export-jobs/{job_id}
  Auth: Bearer <jwt>
  Response: 200 { "status": "completed", "download_url": "https://..." }
```

---

## 6. Deep Dive: Core Components

### 6.1 CRDT vs. Operational Transformation (OT) for Conflict Resolution

**Problem it solves:**
Two users editing the same document simultaneously produce conflicting operations. User A inserts "H" at position 5; User B simultaneously deletes the character at position 5. The server receives both operations and must merge them in a way that: (a) both users' changes are preserved (no silent data loss), (b) both users end up with the same document state (convergence), and (c) the result makes semantic sense.

**Approaches Comparison:**

| Approach | Convergence | Intent Preservation | Offline Support | Complexity | Real-World Use |
|---|---|---|---|---|---|
| Last-Write-Wins (timestamp) | Yes | Poor (data loss) | Trivial | Very low | Not suitable for collaborative text |
| Operational Transformation (OT) | Yes | Good (with correct transform) | Difficult (server required for ordering) | Very high | Google Docs (original), OT with Jupiter algorithm |
| CRDT — WOOT | Yes | Good | Excellent | Medium | Academic; not production at scale |
| CRDT — RGA (Replicated Growable Array) | Yes | Excellent | Excellent | Medium-high | Yjs, Automerge basis |
| CRDT — Logoot/LSEQ | Yes | Good | Excellent | Medium | PeerPad |
| CRDT — Yjs (Y.Doc) | Yes | Excellent | Excellent | Low (library) | Notion, Linear, many modern editors |
| OT with CRDTs for tombstones | Yes | Excellent | Good | High | Hybrid approach |

**Selected: CRDT using Yjs (Y.Doc) for client library; server-side CRDT state maintained in session server.**

Yjs implements a variant of YATA (Yet Another Transformation Approach), which is CRDT-based but designed for practical performance. Key properties:
1. Each character insertion is tagged with a unique ID: `(client_id, clock)` — a Lamport clock per client.
2. Deletions are "tombstones" — the item is marked deleted but its ID remains in the tree, preventing position conflicts.
3. Insertions are ordered by: (a) left neighbor ID, (b) right neighbor ID, (c) client_id as tiebreaker. This is deterministic and commutative — applying operations in any order yields the same result.
4. No server round-trip needed for local operations — the client CRDT state is always valid.
5. Offline editing: operations accumulate locally; on reconnect, the client sends all pending ops to the server; the server merges them with the server state (which may have received other users' ops in the meantime). CRDT semantics guarantee convergence regardless of the order operations are received.

**Algorithm detail with pseudocode:**

```python
# Yjs-style CRDT Item
class Item:
    id:         (client_id: int, clock: int)   # unique identifier
    content:    str | None                      # None if deleted (tombstone)
    left_origin:  Item.id | None               # left neighbor at insertion time
    right_origin: Item.id | None               # right neighbor at insertion time
    is_deleted: bool = False

class YDoc:
    items: DoublyLinkedList[Item]               # the document array
    item_index: HashMap[id, Item]               # for O(1) lookup by id
    client_clocks: HashMap[client_id, int]      # last known clock per client
    pending_ops: List[Op]                        # ops waiting for dependencies

    def integrate(self, new_item: Item) -> None:
        """Insert new_item into the correct position using CRDT rules."""
        # Find left neighbor
        left = self.item_index.get(new_item.left_origin)
        right = self.item_index.get(new_item.right_origin)

        # Scan from left to find the correct insertion point
        # (handles concurrent insertions at the same position)
        o = left.right if left else self.items.head
        while o != right and o is not None:
            # Concurrent item: determine ordering by client_id
            o_left = self.item_index.get(o.left_origin)
            if o_left == left:
                # Both items have the same left neighbor: tiebreak by client_id
                if new_item.id.client_id < o.id.client_id:
                    break   # new_item goes before o
            o = o.right

        # Insert new_item before o
        self.items.insert_before(new_item, o)
        self.item_index[new_item.id] = new_item

        # Update client clock
        self.client_clocks[new_item.id.client_id] = max(
            self.client_clocks.get(new_item.id.client_id, 0),
            new_item.id.clock
        )

    def delete(self, item_id: tuple) -> None:
        item = self.item_index[item_id]
        item.is_deleted = True  # tombstone; item stays in tree

    def get_text(self) -> str:
        return ''.join(item.content for item in self.items if not item.is_deleted)

    def apply_op(self, op: Op) -> bool:
        """Returns False if op's dependencies aren't met yet (buffer it)."""
        # Check that the op's left_origin and right_origin are known
        if op.left_origin and op.left_origin not in self.item_index:
            self.pending_ops.append(op)
            return False

        if op.type == INSERT:
            new_item = Item(id=(op.client_id, op.clock), content=op.text,
                           left_origin=op.left_origin, right_origin=op.right_origin)
            self.integrate(new_item)
        elif op.type == DELETE:
            self.delete(op.target_id)

        # Try to apply buffered ops that may now have their dependencies satisfied
        self.flush_pending()
        return True

# Server session handler
class SessionServer:
    def __init__(self, doc_id):
        self.doc_id = doc_id
        self.ydoc = YDoc()
        self.clients = {}    # client_id → WebSocket connection
        self.seq = 0         # global operation sequence for this document

    async def handle_op(self, client_id, op):
        # 1. Validate permission
        if not has_permission(client_id, self.doc_id, 'edit'):
            return error(client_id, PERMISSION_DENIED)

        # 2. Apply to server CRDT state
        success = self.ydoc.apply_op(op)
        if not success:
            return  # buffered; will apply when dependencies arrive

        # 3. Assign sequence number
        self.seq += 1
        op.server_seq = self.seq

        # 4. Broadcast to all other connected clients
        for cid, conn in self.clients.items():
            if cid != client_id:
                await conn.send({"type": "OP", "seq": self.seq, "op": op})

        # 5. Acknowledge to sender
        await self.clients[client_id].send({"type": "OP_ACK", "op_id": op.id, "seq": self.seq})

        # 6. Write to Kafka (async, don't block the response path)
        kafka.produce_async("doc-ops", key=self.doc_id, value=serialize(op))
```

**Interviewer Q&A:**

Q1: Why CRDT over OT for a new system designed in 2024?
A1: Operational Transformation requires a central server for operation ordering — every client must first receive an acknowledgment from the server before applying their operation to the shared state (otherwise, transforming operations out of order causes divergence). This means: (a) offline editing is impossible with pure OT (you can't edit without server contact), (b) multi-server architectures are extremely complex (the "Jupiter" OT algorithm has known bugs in certain multi-server configurations), and (c) the OT transform functions must be implemented correctly for every pair of operation types (n² complexity for n operation types). CRDTs, specifically Yjs, solve all three: offline editing works natively (operations accumulate locally and merge on sync), multiple servers can all hold CRDT state and merge correctly, and the merge logic is centralized in the CRDT library. The trade-off is memory: CRDT tombstones (deleted items) accumulate; a heavily-edited document may have 10× the tombstones vs. live content. Periodic CRDT GC (garbage collection) resolves this.

Q2: How does CRDT handle the "last character deletion" problem where two users delete the same character?
A2: In CRDT (Yjs), each item has a unique ID. If User A and User B both delete item `(clientA, 5)`, they each send a DELETE op for the same item ID. When the server (or any client) receives both deletes, it marks the item as deleted. Since "set deleted = true" is idempotent, receiving the same delete twice is harmless. The document converges to the item being deleted (correct behavior) regardless of the order the two DELETE ops are processed. This is called "idempotent tombstone" behavior.

Q3: What happens when a user has been offline for 24 hours and returns with 10,000 pending operations?
A3: On reconnect: (1) The client sends `{ type: "SYNC_REQUEST", since_seq: last_server_seq_received }` to the session server; (2) The session server fetches all operations since `last_server_seq` from Cassandra (potentially 10 k operations from other users); (3) The session server sends these to the client in a `SYNC_RESPONSE` batch; (4) The client feeds all received ops into its local CRDT via `apply_op()` — CRDT semantics guarantee that applying these ops (which the client has never seen) in any order produces a correct result; (5) The client then sends its 10,000 pending ops to the server via the WebSocket; (6) The server integrates them into its CRDT state. The final document state is identical on server and client. No data is lost. This is the key advantage of CRDTs over OT for offline use cases.

Q4: How do you handle CRDT state growing very large from tombstones?
A4: Yjs implements "GC mode" (garbage collection): if ALL clients who have ever seen a document are known to have processed a deletion (their vector clock shows they have seen the delete), the tombstone can be safely removed. In practice, tracking "all clients" is hard. Alternative: periodic CRDT state compaction: (1) Take the current "live" text (ignoring tombstones); (2) Create a new CRDT document with only the live items (re-ID them from scratch); (3) Store this compacted state as a new snapshot in S3; (4) All new clients load from the compacted snapshot; (5) Old clients currently editing are migrated on their next reconnect. This is called "rebasing" the document and resets the tombstone accumulation. Done when `tombstone_count / live_count > 10` (tombstones are 10× the live content).

Q5: How do you prevent two concurrent inserts at the exact same position from producing different orderings on different clients?
A5: The Yjs YATA algorithm handles this deterministically: both concurrent insertions share the same `left_origin` (the character before the insertion point). The `integrate()` algorithm (shown in the pseudocode) scans rightward from the left_origin and uses the `client_id` as a tiebreaker — the insertion from the client with the lower `client_id` (numeric) comes first. This is deterministic across all clients because all clients apply the same comparison function. The choice of which client "wins" (goes first) is arbitrary but consistent — the document converges to the same state everywhere. Users may see their insertion end up on the left or right of a concurrent insertion, but this is expected behavior in concurrent editing (equivalent to "the other person typed at the same spot at the same millisecond").

---

### 6.2 Document Session Server: Routing and Consistency

**Problem it solves:**
With 10 M concurrent editing sessions and each session potentially served by a different server, we need to ensure: (a) all clients editing the same document connect to the same session server (for single-writer coordination), (b) the session server's in-memory CRDT state stays consistent with durable storage, (c) if the session server crashes, the document is recoverable.

**Approaches Comparison:**

| Approach | Consistency | Scalability | Failover Complexity |
|---|---|---|---|
| Single global server (sharded by doc_id) | Perfect | Medium (limited shards) | High (state transfer on crash) |
| Leader per document (Raft consensus) | Perfect | High | Automatic (Raft election) | 
| Consistent hash routing (stateless) + Redis for state | Good | Excellent | Medium (reload state from Redis) |
| Multi-master CRDT replication (all servers hold all docs) | Good (eventual) | Infinite | Low (any server can serve) |
| Actor model (one actor per document) | Perfect | Excellent | Medium (actor migration) |

**Selected: Consistent hash routing to a session server that owns the document + periodic CRDT state persistence to Redis/S3.**

**Routing algorithm:**
The API Gateway uses consistent hashing on `doc_id` to route all WebSocket connections for a document to the same session server pod. Hash ring with virtual nodes (150 virtual nodes per physical server) ensures even distribution and minimal remapping on scale changes.

**State persistence and recovery pseudocode:**
```python
class SessionServer:
    def __init__(self):
        self.docs = {}    # doc_id → DocSession
        self.checkpoint_interval_s = 30

    async def get_or_create_session(self, doc_id) -> DocSession:
        if doc_id not in self.docs:
            # Load from Redis (fast) or S3 (slower, fallback)
            session = await self.load_session(doc_id)
            self.docs[doc_id] = session
        return self.docs[doc_id]

    async def load_session(self, doc_id) -> DocSession:
        # Try Redis first (fast path: 1-2 ms)
        crdt_bytes = redis.get(f"doc:crdt:{doc_id}")
        if crdt_bytes:
            ydoc = YDoc.from_bytes(crdt_bytes)
            seq = int(redis.get(f"doc:seq:{doc_id}") or 0)
        else:
            # Cold start: load from S3 snapshot (slow path: 100-500 ms)
            snapshot_key = f"docs/{doc_id}/current.bin"
            snapshot = s3.get_object(snapshot_key)
            ydoc = YDoc.from_bytes(snapshot.body)
            seq = snapshot.metadata["seq"]
            # Replay ops from Cassandra since snapshot
            ops = cassandra.query(
                "SELECT * FROM document_operations WHERE doc_id=? AND seq_num > ? ORDER BY seq_num ASC",
                doc_id, seq
            )
            for op in ops:
                ydoc.apply_op(deserialize(op.op_data))
                seq = op.seq_num

        return DocSession(doc_id=doc_id, ydoc=ydoc, seq=seq)

    async def checkpoint_session(self, doc_id):
        session = self.docs.get(doc_id)
        if not session or not session.dirty:
            return
        crdt_bytes = session.ydoc.encode_state()
        # Write to Redis (fast, temporary)
        redis.setex(f"doc:crdt:{doc_id}", 3600, crdt_bytes)  # 1-hour TTL
        redis.set(f"doc:seq:{doc_id}", session.seq)
        # Write to S3 asynchronously (durable)
        s3_key = f"docs/{doc_id}/snapshots/{session.seq}.bin"
        await s3.put_object(s3_key, crdt_bytes, metadata={"seq": str(session.seq)})
        # Update current pointer (conditional write to prevent race)
        await s3.put_object(f"docs/{doc_id}/current.bin", crdt_bytes,
                           if_none_match="*",  # not quite right; use CAS-like atomic write
                           metadata={"seq": str(session.seq)})
        session.dirty = False

    async def handle_session_server_failure(self, failed_server_id, doc_ids_moved):
        # Called by the consistent hash ring manager on server removal
        for doc_id in doc_ids_moved:
            # Load session from Redis/S3 (already checkpointed by now or recovering)
            # The new assigned server will load on first connection
            # Clients will reconnect via WebSocket (server drop detected)
            pass
```

**Interviewer Q&A:**

Q1: What happens when the session server crashes with 50 unprocessed operations in memory?
A1: This is the critical durability question. Design: (1) Operations are written to Kafka synchronously before being applied to the CRDT state — `kafka.produce()` with `acks=all` completes before the server sends `OP_ACK` to the client; (2) So even if the session server crashes immediately after sending OP_ACK, the op is in Kafka; (3) The CRDT state at the time of crash may not include the last 30 s of ops (checkpoint interval); (4) On recovery (new session server for this doc_id): load last S3 snapshot, then replay all ops from Cassandra (which is fed by the Kafka consumer) since the snapshot seq; (5) If Kafka ops arrived but Cassandra persistence hadn't caught up: Kafka has 7-day retention; the recovery process can read directly from Kafka; (6) Clients that were connected will detect the WebSocket disconnect, reconnect, and receive a `SYNC_RESPONSE` with any ops they missed. Net result: zero data loss (Kafka durability is the guarantee).

Q2: How do you handle a "hot document" with 1,000 concurrent editors (viral document)?
A2: Single session server per document becomes a bottleneck at 1,000 concurrent WebSocket connections generating 40 k operations/s. Mitigations: (1) The session server is specialized for this: it uses async I/O (Go goroutines/goroutines, not threads); one goroutine per connection for receive; a single-threaded CRDT state machine for apply + broadcast; (2) The single-threaded CRDT engine processes 40 k ops/s — at 5 µs/op (CRDT integrate), that's 200 ms/s compute: the engine is saturated. Solution: batching — accumulate ops for 5 ms, apply all, broadcast all as a single batch packet; (3) If still insufficient, split the document into "sections" (pages), each with its own sub-document session — this is how Google Docs handles very long documents; (4) For the broadcast: with 1,000 connections on one server, broadcasting 40 k ops/s = 40 M messages/s × 200 bytes = 64 Gbps — exceeds a single NIC. This requires either limiting concurrent editors (realistic: 100 max in Google Docs) or offloading broadcast to a separate fan-out service.

Q3: How does a new client joining a live session get the current document state?
A3: Two-phase join: (1) REST call to `GET /api/v1/documents/{doc_id}/snapshot` fetches the latest S3 snapshot for this document (CDN cached by seq number — immutable). This gives the client the document state as of `seq=10200`; (2) WebSocket connect to the session server, sending `{ type: "SYNC_REQUEST", since_seq: 10200 }`; (3) Session server replies with `SYNC_RESPONSE` containing all ops from seq 10200 to current (10293). Client applies these ops to its local CRDT state, reaching current; (4) Normal operation resumes. This avoids sending the full document over WebSocket — the CDN-cached snapshot handles the bulk of the data transfer, and only the delta ops (likely < 100) go via WebSocket.

Q4: How do you route 10 M documents across a session server cluster?
A4: A 200-node session server cluster (each handling 50 k active documents at 1 user/doc average): 200 × 50 k = 10 M docs. Consistent hash ring: doc_id (UUID) is hashed to a 32-bit integer; mapped to a position on the 2^32 ring; the ring has 200 × 150 = 30,000 virtual nodes. The API Gateway has a copy of the ring (updated via etcd watch when nodes join/leave). When a WebSocket connection arrives for doc_id X, the gateway looks up the ring (O(log N) binary search on sorted virtual nodes) and routes to the assigned session server pod. If the session server is temporarily overloaded, it can reject the connection (503) and the gateway retries the next virtual node in the ring (overflow routing).

Q5: What is the consistency model for document reads? Can a user open a document and see a stale version?
A5: Documents are "read your own writes" consistent: when a user saves (or the auto-save fires), the write goes to the session server, which persists to S3 (async). If the user immediately opens the document in another tab, the REST snapshot may serve a slightly stale version (the S3 snapshot may be up to 30 s behind the WebSocket session state). Mitigation: the `GET /snapshot` response includes the snapshot `seq` number; the client then fetches ops from seq → current via WebSocket. The displayed document is eventually current within < 1 s. For "view the document right now" freshness (not collaborative real-time editing), this is acceptable. A "loading" indicator shows while ops are being applied.

---

### 6.3 Offline Editing and Sync

**Problem it solves:**
A user edits a document on a plane with no internet for 2 hours, making 5,000 changes. Another user simultaneously edits the same document online, making 1,000 changes. When the offline user reconnects, both sets of changes must be merged correctly without either user's edits being lost, and without requiring human conflict resolution.

**Implementation:**

```python
# Client-side offline queue
class OfflineQueue:
    def __init__(self):
        self.pending_ops = []    # operations made while offline
        self.last_server_seq = 0  # last seq received from server

    def apply_local_op(self, op):
        """Apply immediately to local CRDT; queue for server sync."""
        self.local_ydoc.apply_op(op)
        self.pending_ops.append(op)
        self.persist_to_indexeddb(op)  # persist in browser IndexedDB for crash recovery

    def on_reconnect(self, websocket):
        """Called when WebSocket connection is re-established."""
        # Step 1: Request missing server ops since last known seq
        websocket.send({ "type": "SYNC_REQUEST", "since_seq": self.last_server_seq })

    def on_sync_response(self, server_ops, current_seq, websocket):
        """Receive server ops made while we were offline."""
        # Step 2: Integrate remote ops into local CRDT
        # CRDT semantics: order doesn't matter; result is the same
        for op in server_ops:
            self.local_ydoc.apply_op(op)   # concurrent ops from other users
        self.last_server_seq = current_seq

        # Step 3: Send our pending local ops to server
        # These ops were made against our local CRDT state (which may diverge from server)
        # CRDT handles the merge; we just send all pending ops
        for pending_op in self.pending_ops:
            websocket.send({"type": "OP", "op": pending_op})

        # Step 4: Clear pending queue (ops now being processed by server)
        self.pending_ops.clear()
        self.clear_indexeddb_pending()

# Server-side handling of reconnect flood
class SessionServer:
    async def handle_sync_request(self, client_id, since_seq):
        # Fetch ops from Cassandra
        if since_seq < self.seq - CASSANDRA_RECENT_THRESHOLD:
            # Client is very far behind; suggest full snapshot reload
            snapshot_url = s3.get_presigned_url(f"docs/{self.doc_id}/current.bin")
            return {"type": "SYNC_SUGGEST_SNAPSHOT", "snapshot_url": snapshot_url,
                    "snapshot_seq": self.last_snapshot_seq}

        ops = cassandra.query(
            "SELECT op_data, seq_num FROM document_operations "
            "WHERE doc_id=? AND seq_num > ? ORDER BY seq_num ASC LIMIT 10000",
            self.doc_id, since_seq
        )
        return {"type": "SYNC_RESPONSE", "ops": ops, "current_seq": self.seq}
```

**Interviewer Q&A:**

Q1: What is the practical limit on offline edit divergence before sync becomes too expensive?
A1: With 5,000 local ops and 1,000 remote ops, the CRDT merge processes 6,000 ops. At 5 µs/op (Yjs performance benchmark): 30 ms — imperceptible. At the extreme: 100,000 local ops (12 hours of offline editing) × 5 µs = 500 ms — noticeable but acceptable (a spinner for < 1 s is fine). The network cost of sending 100,000 ops to the server: at 50 bytes/op = 5 MB — a few seconds on a slow connection. For truly large offline divergence (> 24 hours, document was edited by many users), we suggest a full document reload: server sends `SYNC_SUGGEST_SNAPSHOT`, client fetches the current snapshot from S3 CDN, and the local pending ops are applied on top. The user's local changes are preserved because they were made to the CRDT (which they send to the server), but they may see "jump" in the document state when integrating 10,000 remote ops.

Q2: How do you handle a situation where two offline users each rename the document title?
A2: Document title is metadata, not CRDT content. For metadata (title, permissions), we use a different conflict resolution strategy: **last-write-wins** with server timestamp. When two users set the title while offline and both sync, the server records both in the operation log, but the PostgreSQL `documents.title` field uses the most recently received update (timestamp-based LWW). This is acceptable because: (a) title changes are rare, (b) the "loss" of one user's title change is visible in the revision history and can be easily undone, (c) CRDT for metadata would be over-engineered. Users see the currently-live title in their UI after sync; if they disagree, they change it manually.

Q3: How does IndexedDB (browser storage) prevent losing offline edits on browser crash?
A3: Every local op is written to IndexedDB immediately (synchronously via a transaction) before being applied to the in-memory CRDT. IndexedDB uses a WAL-based storage engine in Chrome (LevelDB) and Firefox (SQLite), providing durability across browser crashes. On the next page load: (1) The client checks IndexedDB for pending (un-acked) ops for the document; (2) If found, loads the CRDT state from IndexedDB (also persisted periodically); (3) On WebSocket reconnect, sends the pending ops. This ensures that even if the browser crashes right after the user types text but before the WebSocket send, the text is recovered. The `OP_ACK` from the server is what removes an op from the IndexedDB pending queue.

---

## 7. Scaling

### Horizontal Scaling

| Component | Scaling Strategy | Notes |
|---|---|---|
| Session servers | Consistent hash ring; add nodes to expand ring | Each node handles ~50 k active sessions (50 MB RAM each × 50 k = 2.5 TB RAM/node; use 256 GB RAM nodes → 5 k sessions/node for safety; 10 M / 5 k = 2,000 nodes) |
| API servers (REST) | Stateless; horizontal scale behind L7 LB | Add nodes; CDN for snapshot GETs |
| WebSocket gateways | Scale with connection count | Each gateway handles 50 k connections; 30 M connections = 600 gateways |
| Kafka | Add brokers; increase topic partitions | Topic `doc-ops` partitioned by `doc_id % N_PARTITIONS`; scale to 1,024 partitions |
| Cassandra | Add nodes; auto-rebalance | `(doc_id, shard)` partition distributes load; target < 10 GB/partition |
| S3 | Infinite (managed) | Prefix sharding by `doc_id[0:2]` to distribute across S3's internal partitions |
| PostgreSQL | Read replicas; Citus shard by doc_id for metadata | Write volume is low; primary handles writes; replicas serve reads |
| Redis | Cluster; shard by `doc_id % N_SHARDS` | 16 shards; each shard holds ~625 k active doc presences |

### DB Sharding
- **Cassandra operations log**: Natural sharding via `(doc_id, shard)` partition key (shard = hour bucket). Consistent hashing distributes partitions across nodes. Adding nodes causes Cassandra to automatically migrate partitions.
- **PostgreSQL document metadata**: At 1 B documents × 5 KB = 5 TB, a single PG instance handles this. For 10× scale, shard by `doc_id % 16` using Citus; each shard holds 62.5 M docs.

### Replication
- PostgreSQL: RF=1 primary + 5 read replicas; Patroni failover.
- Cassandra: RF=3 per DC; two DCs (active-active); LOCAL_QUORUM reads and writes.
- S3: Cross-Region Replication (CRR) for DR; 11-nines durability native.
- Redis Cluster: 1 replica per primary; 3 AZs for replicas.
- Session servers: no replication (stateless recovery from Redis/S3).

### Caching
| Layer | Data | TTL | Strategy |
|---|---|---|---|
| CDN | Document snapshots (by doc_id + seq) | Immutable (86400 s) | Fetched on doc open; seq is immutable key |
| Redis | Document permission (doc_id + user_id) | 60 s | Refreshed on any permission change; invalidated on share |
| Redis | CRDT state (doc_id) | 1 hour | Session server warm restore; S3 is cold backup |
| API server in-process | User profile (for commenter display name) | 5 min | LRU |

**Interviewer Q&A — Scaling:**

Q1: The consistent hash ring for session server routing has a weakness: adding/removing nodes causes sessions to migrate. How do you handle this?
A1: When a node is added, consistent hashing moves only `1/N_nodes` fraction of sessions (much better than modulo hashing which remaps half). With virtual nodes (150 per physical), rebalancing is smooth. For each migrated document: (1) The old session server flushes its CRDT state to Redis (setex with 5-minute TTL); (2) On the first WebSocket connection to the new server for this doc, the new server loads state from Redis (fast, 1–2 ms); (3) Clients may experience a brief WebSocket reconnect when their connection is terminated during rebalancing — they auto-reconnect to the new server. The "drain period" for a node removal: stop accepting new connections 30 s before removal; checkpoint all active sessions to Redis; then remove from ring.

Q2: How do you handle the "birthday problem" for document IDs in consistent hashing?
A2: Document IDs are UUIDs v4 (128 bits, cryptographically random). The hash function (MurmurHash3 or xxHash) maps them to 32-bit integers for ring placement. With 1 B documents: 10^9 / 2^32 ≈ 0.23 load per ring slot — well below the collision threshold. With 150 virtual nodes × 2,000 physical servers = 300,000 virtual nodes on a 2^32 ring: average gap = 2^32 / 300,000 ≈ 14,316 — very fine-grained distribution. Collisions in the ring are handled by choosing the nearest clockwise virtual node.

Q3: How do you prevent a "thundering herd" when a popular document is shared to 10,000 users simultaneously?
A3: When a document is shared (e.g., a viral company-wide announcement), all 10,000 users open it within 60 seconds: (1) The S3 snapshot URL is CDN-cached (key = doc_id + seq); the CDN handles 10,000 concurrent GETs for the same object without hitting S3 origin more than once (request coalescing); (2) The session server receives 10,000 WebSocket connections within 60 s: 10,000 / 60 s = 167 new connections/s — manageable for a single pod (connection setup is O(1)); (3) The permission check for each connection queries Redis (cached); 10,000 × 0.1 ms = 1,000 ms total but these are parallel, so < 50 ms at steady state; (4) The session server sends each new client a `SYNC_RESPONSE` with recent ops (likely few if the document is just being viewed). The bottleneck is RAM: 10,000 WebSocket connections × 50 KB state = 500 MB RAM for connection buffers.

Q4: How do you scale the Cassandra operation log given 1 GB/s of writes?
A4: 1 GB/s writes across Cassandra: with RF=3, actual write amplification = 3 GB/s of SSTable writes. A 40-node Cassandra cluster with NVMe SSDs (each handling 500 MB/s write throughput) can handle: 40 × 500 MB/s / 3 = 6.7 GB/s write capacity — sufficient headroom. Key schema consideration: `(doc_id, shard)` partition key. With 10 M active documents and 1 k ops/s per active document: 10 M × 1 k = 10 B ops/s total theoretical; in practice, operations are bursty (typing speed × users). The `shard` column (hour bucket) ensures no partition exceeds ~3.6 M ops/hour × 50 bytes = 180 MB — well within Cassandra's 100 MB/partition guideline. Operations older than 90 days are automatically deleted (TTL = 7,776,000 s), preventing unbounded growth.

Q5: How would you handle 100 million documents being opened on Monday morning simultaneously?
A5: The spike at 9 AM Monday is the "document breakfast rush." Strategies: (1) CDN for snapshot GETs — 100 M document opens × 100 KB snapshot = 10 TB of data; CDN absorbs this with high hit rates if users open the same popular documents; (2) Session server cold start: each document open triggers a session load (Redis check + possible S3 fallback). Redis handles 1 M ops/s; 100 M docs / 60 s = 1.67 M Redis GETs/s during the ramp — requires 2 Redis clusters; (3) Pre-warm popular documents: a background service identifies documents with many followers (e.g., company wikis) and pre-loads their session state into Redis at 8:55 AM; (4) Session server autoscaling: triggered 15 minutes before expected peak based on historical patterns; (5) Connection rate limiting: API gateway enforces 1,000 new WebSocket connections/s per session server to prevent overload during the spike.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---|---|---|---|---|
| Session server crash | All editors for affected docs get WebSocket disconnect | k8s liveness probe (5 s) | k8s restarts pod; clients auto-reconnect; state reloaded from Redis/S3 | < 30 s reconnect |
| Redis node failure | Presence stale; permission cache miss (fallback to DB) | Redis Sentinel; health check | Replica promotion; permission check falls back to PostgreSQL | < 10 s |
| Kafka broker failure | Op persistence delayed | Broker health monitor | RF=3 ensures continuity; delayed but not lost | < 10 s |
| Cassandra node failure | Historical op queries may fail | Nodetool; health endpoint | RF=3, LOCAL_QUORUM: one node failure transparent | 0 impact |
| S3 outage (snapshot reads fail) | New document loads fail (cold start) | Health check | Serve from Redis CRDT cache for warm docs; queue new loads | Warm docs: unaffected; cold loads: degrade |
| PostgreSQL primary failure | Document metadata writes fail | Patroni monitor | Patroni promotes standby | < 30 s |
| Network partition between session server and Kafka | Op persistence fails | Kafka producer error callbacks | Circuit breaker: session server buffers in-memory (max 30 s); retries on reconnect | < 60 s buffer |
| Full datacenter failure | Regional service down | External health checks | Failover to secondary region via DNS; session servers in secondary region load state from S3 (replicated via CRR) | ~5 min |

### Retries & Idempotency
- **Client op delivery**: each op has a client-generated `op_id` (UUID). If the WebSocket drops before OP_ACK, the client re-sends the op on reconnect. The session server checks `if op.op_id in recently_seen_op_ids: skip` (deduplication window: 10 minutes, stored in Redis set with TTL).
- **Kafka produce** (session server → op log): `acks=all`, `enable.idempotence=true` (Kafka's built-in idempotent producer), `max.in.flight.requests=1` — guarantees exactly-once delivery to Kafka per partition.
- **S3 snapshot writes**: conditional writes using `If-Match` ETag (S3 optimistic concurrency) prevent two snapshot writers from overwriting each other. The session server holds the current ETag and provides it on every conditional PUT.

### Circuit Breaker
- **Session server → Kafka**: if Kafka produce error rate > 5% over 10 s, circuit opens. In-memory buffer activated (max 500 ops, ~30 s of typical typing). Alert fires immediately — Kafka outage is critical. OP_ACKs are still sent to clients (maintaining user experience) but with a warning log. If buffer fills (> 500 ops), the session server starts sending OP_ACK with `{ "persisted": false }` warning flag.
- **Session server → Redis** (checkpoint): if Redis latency > 50 ms P99, increase checkpoint interval from 30 s to 120 s. If Redis is unreachable, skip Redis checkpoints; persist only to S3 (every 5 min). Presence updates are dropped (acceptable — presence is ephemeral).
- **REST API → PostgreSQL**: if PG is slow (> 100 ms P99 for metadata reads), circuit opens; metadata served from Redis cache (stale-while-revalidate). Document creates/deletes queue in Kafka for retry.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Purpose |
|---|---|---|---|
| `session_server_active_docs` | Gauge | — | Capacity planning |
| `op_processing_latency_ms` | Histogram | P99 > 50 ms | CRDT performance |
| `op_propagation_latency_ms` (time from server receive to client receive) | Histogram | P99 > 500 ms | Real-time collaboration SLA |
| `ws_connections_per_session_server` | Gauge | > 40 k | Scaling trigger |
| `kafka_produce_error_rate` | Counter | > 0.01% | Durability risk |
| `cassandra_write_latency_ms` | Histogram | P99 > 10 ms | Persistence lag |
| `s3_snapshot_age_s` (time since last snapshot) | Gauge per doc | > 600 s (10 min for active docs) | Snapshot freshness |
| `offline_sync_ops_count` (ops sent by reconnecting client) | Histogram | > 1000 ops | Large offline divergence alert |
| `crdt_tombstone_ratio` | Gauge per doc | > 10 (10× live content) | GC needed |
| `permission_cache_hit_rate` | Gauge | < 90% | Redis performance |
| `document_load_time_ms` | Histogram | P99 > 3000 ms | User experience |
| `conflict_resolution_time_ms` | Histogram | P99 > 100 ms | CRDT merge performance |
| `session_server_memory_bytes` | Gauge | > 85% of pod limit | OOM risk |

### Distributed Tracing
- OpenTelemetry spans: `client_op_sent` → `gateway_received` → `session_server_applied` → `kafka_produced` → `client_broadcast`. Target: < 100 ms span end-to-end.
- Document load trace: `rest_snapshot_request` → `cdnlookup` → `crdt_load` → `websocket_sync_request` → `cassandra_op_replay` → `client_ready`. Target: < 2 s.
- Sampling: 100% for errors; 5% for normal ops (20 M ops/s × 5% = 1 M traces/s — still expensive; use tail-based sampling: always trace ops that take > 200 ms).

### Logging
- Session server: log document loads, session starts/ends, error events. Log op_id + doc_id for every OP_ACK (for debugging "why didn't my edit save?").
- Do NOT log op content (privacy — document contents are confidential). Log only op_type, op_id, doc_id, user_id, seq_num.
- Permission service: log every permission check outcome (ALLOW/DENY) with doc_id, user_id, required_permission, actual_permission.
- Offline sync: log when a reconnecting client sends > 100 pending ops (anomalous offline edit) with user_id and op count.
- Retention: access logs 90 days; op metadata (doc_id, user_id, seq, ts) 7 years (potential legal discovery); op content — never logged (only in Cassandra).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Alternative | Why Chosen |
|---|---|---|---|
| CRDT (Yjs) vs. OT | CRDT (Yjs) | Operational Transformation (Jupiter algorithm) | CRDT enables offline editing, simpler multi-server deployment, no transform function matrix; OT requires central sequencing server |
| Session server per document (stateful) | Single session server per doc via consistent hash | Stateless servers with all state in Redis | In-memory CRDT is 1,000× faster than Redis round-trips for each op; stateful pods are acceptable with fast recovery |
| Op persistence: Kafka + Cassandra | Kafka (streaming) + Cassandra (queryable history) | Direct Cassandra write | Kafka provides a durable buffer and decouples real-time from persistence; Cassandra provides queryable history |
| Snapshot format: CRDT binary | CRDT serialized state (Yjs binary) | JSON document | Yjs binary is 5-10× smaller than JSON for the same document content; client loads the same format it uses natively |
| Presence: Redis pub/sub with batching | Redis (TTL-based) with 100 ms batching | WebSocket relay through session server | Decouples presence from op processing; batching reduces cursor movement noise from 30 M events/s to 300 k messages/s |
| Revision history: op log + snapshots | Cassandra op log + periodic S3 snapshots | Full snapshot per edit (like git commits) | Full snapshot per edit = 20 M × 100 KB = 2 TB/s storage; op log is 20 M × 50 bytes = 1 GB/s — 2000× cheaper |
| Conflict resolution for concurrent edits | CRDT deterministic merge | Last-write-wins | LWW silently loses data; CRDT preserves all concurrent changes |
| Permission enforcement: at WebSocket connect + cached | Check at connect + 60 s Redis cache | Check every op | Per-op permission checks add 1 ms latency per op; at 20 M ops/s = 20 M Redis lookups/s — excessive. Connect-time check with 60 s cache is sufficient; permission revocation takes effect within 60 s |

---

## 11. Follow-up Interview Questions

Q1: How would you implement "Suggesting" mode (tracked changes)?
A1: Suggestions are CRDT operations tagged with `{ mode: "suggestion", suggestion_id: uuid }`. Instead of directly modifying the CRDT state, suggestions create a parallel "suggested state" layer: insertions are shown with a special formatting (green underline); deletions are shown as struck-through. The base CRDT document excludes suggested changes; a "suggestion applied" document includes them. Implementation: Yjs `Y.Awareness` protocol tracks which operations are suggestions; a separate Y.Map stores suggestion metadata (author, status: pending/accepted/rejected). When an editor accepts a suggestion, the suggestion's ops are re-applied to the base doc as normal ops; when rejected, the suggestion's ops are tombstoned. All collaborators see suggestion status changes in real time via the same WebSocket channel.

Q2: How do you implement Google Docs' "See revision history" and "Restore this version" features?
A2: The `document_revisions` table indexes the operation log into user-meaningful revision points: auto-created at each 1-hour editing window and on explicit manual save. To view a revision: (1) Fetch the latest S3 snapshot with `seq <= revision.op_seq_start`; (2) Replay all ops from Cassandra from that seq to `revision.op_seq_end`; (3) Render the document at that state. To restore: (4) The current document's CRDT state is set to the revision's state; (5) A new "restore" op is logged to the op log (the restore itself becomes part of history); (6) All connected clients receive the new CRDT state via a `FULL_STATE` message (full snapshot, not delta). The full restoration is done as a single atomic "revert" operation so revision history shows "Document restored to version from April 1, 2024."

Q3: How do you handle a document that grows to 1 million characters?
A3: (1) Yjs CRDT scales well to 1 M characters: the underlying linked list has O(1) amortized inserts and deletes. Memory: 1 M items × 50 bytes/item (id + content + left/right pointers) = 50 MB per session in RAM — acceptable; (2) Tombstones: with heavy editing, a 1 M character document might have 10 M tombstones. At 50 bytes each = 500 MB. CRDT GC is triggered when tombstones exceed 5× live content; (3) Loading: the S3 snapshot at 500 MB takes 2–5 s to download; mitigated by CDN caching and delta sync (most users don't start from zero); (4) Search: client-side search over 1 M characters takes < 50 ms (JavaScript string search); (5) Export to PDF/DOCX: async job; a 1 M character document = ~500 page PDF, rendering takes 10–30 s server-side (acceptable for async job).

Q4: How do you handle comments when the anchored text is later deleted?
A4: Comments store `anchor_start`, `anchor_end` (character offsets at creation time), and `anchor_text` (a copy of the text). When the CRDT processes deletions that overlap with a comment anchor, the anchor positions must be updated. Implementation: each comment anchor is a Yjs "relative position" — not an absolute integer offset but a reference to a specific CRDT Item ID. When items before the anchor are inserted or deleted, the absolute position changes but the relative position (anchored to a specific Item) remains valid. When the anchored item itself is deleted (tombstoned), the comment is marked "anchor deleted" and shown as an orphaned comment (still readable in history but no longer highlighted in the text). This is how Google Docs handles comments on deleted text.

Q5: What would change in your design if Google Docs needed to support 100 simultaneous editors of a single document?
A5: Current design supports 100 concurrent editors on one session server pod. Bottlenecks at 100 editors: (1) Fan-out: 100 users × 40 ops/min × 100 recipients = 6,667 messages/s from one session server — manageable; (2) Presence: 100 cursor positions, each updated at 1 Hz = 100 presence events/s × 100 viewers = 10,000 presence deliveries/s per document — manageable; (3) CRDT performance: concurrent edits at 100 users → more frequent `integrate()` calls with tiebreaking — performance is still O(log N) per op; (4) Real bottleneck: human cognitive load — 100 simultaneous edits creates a chaotic visual experience. Google Docs limits concurrent editors display to 100; beyond that, presence indicators are collapsed. Server changes: increase the session server's batch window from 5 ms to 10 ms when `active_users > 50`, reducing delivery rate but maintaining throughput.

Q6: How do you prevent unauthorized access to a shared link after the sharing is revoked?
A6: (1) The `document_permissions` table has a `link_token` column per link-based share; (2) On revocation, the row is deleted (or `expires_at` is set to now); (3) The Redis permission cache for `link_token` has a 60 s TTL — maximum stale window after revocation; (4) For immediate revocation: invalidate the Redis cache key `doc:perm:{doc_id}:{link_token}` synchronously as part of the revocation API call; (5) The JWT token issued to a link user has a short expiry (1 hour) and is not renewable after the link is revoked; (6) New WebSocket connections after revocation are rejected at connect time (permission check fails); (7) Existing WebSocket connections: the session server broadcasts a `PERMISSION_REVOKED` message to all connections authenticated with the revoked token; clients gracefully close.

Q7: How would you implement "Named Versions" (like git tags for documents)?
A7: Named versions are user-created revision snapshots. Implementation: (1) `POST /api/v1/documents/{doc_id}/named-versions` with `{ "name": "Final Draft" }` creates a row in a `named_versions` table pointing to the current `op_seq_end` and optionally triggering an S3 snapshot write immediately; (2) Named versions are listed in the revision history UI with special styling; (3) Fetching a named version is the same as fetching a numbered revision (load snapshot before that seq + replay ops to that seq); (4) Named versions are immutable references — deleting a named version only removes the label, not the underlying ops or snapshot; (5) Named versions are included in document export metadata (DOCX comment: "Final Draft, April 6 2024").

Q8: How do you design the permission model for very large organizations (50 k+ employees)?
A8: (1) Group-based permissions: add a `groups` table and `group_members` table; `document_permissions.principal_type = 'group'`; effective permission = max(direct_user_permissions, group_permissions); (2) Domain-based sharing: `principal_type = 'domain'`, `principal_id = 'example.com'` — anyone with that email domain gets the specified permission; (3) Inheritance: documents can inherit permissions from a folder/drive (recursive permission tree); (4) Permission evaluation uses a denormalized cache: a background job precomputes `effective_permission` for `(user_id, doc_id)` pairs and stores in Redis. On any permission change (to user, group, or inherited folder), invalidate affected cache entries; (5) For 50 k employees × 1 M shared documents: 50 B permission pairs — cache only hot (recently accessed) pairs; cold permissions evaluated on demand from the permission tree.

Q9: What is the biggest technical risk in your design?
A9: The single session server per document (stateful pod) is the biggest risk. If the consistent hashing routes all requests for a document to a server that's experiencing issues (high CPU, memory pressure from a memory leak in the CRDT state), all collaborators on that document are affected. Mitigations discussed above (circuit breaker, autoscaling, state persistence) reduce but don't eliminate this risk. The alternative — multi-master CRDT replication (any server can serve any document) — would eliminate this risk at the cost of significantly higher inter-server synchronization traffic. At Google scale, they have solved this with a custom consensus protocol per document (similar to Raft but optimized for CRDT operations). For a startup, the single-server-per-document model is significantly simpler and the right starting point.

Q10: How do you design the full-text search across all documents?
A10: (1) A Kafka consumer reads the operation log (`doc-ops` topic); for INSERT operations, it extracts the new text and sends to an Elasticsearch indexing pipeline; for DELETE operations (tombstones), it marks the text as removed; (2) The Elasticsearch index maps `doc_id → full_text` with per-user permission metadata; (3) At query time: `GET /api/v1/search?q=quarterly+report` sends the query to the Search service, which calls Elasticsearch with a filter: `{ "term": { "accessible_by": user_id }}` (permission-aware search); (4) Elasticsearch returns matching `doc_id` + highlight snippets; (5) Challenge: permission-aware search at 1 B documents × 50 M users is expensive to maintain. Optimization: index documents by owner and group; at search time, expand the user's group memberships (cached) and add them to the filter. (6) Indexing latency: ops are indexed within 30 s of being written (Kafka consumer lag). This is acceptable for search (users don't expect to find text they typed 5 s ago via full-text search).

Q11: How would you implement multi-language support with right-to-left (RTL) text?
A11: RTL support is primarily a client-side concern (text rendering direction). Server-side changes: (1) The CRDT `insert` operation is direction-agnostic — it works at the character level regardless of display direction; (2) RTL markers (Unicode RLM/LRM) are treated as regular characters by the CRDT; (3) Paragraphs have a `direction` attribute in the formatting spec (LTR/RTL/auto-detect); (4) `detect_direction(text)` runs client-side on paragraph text; server stores the detected direction as a document attribute; (5) Export to PDF/DOCX must handle bidirectional text (uses HarfBuzz for text shaping, pango for layout, or a commercial rendering library like PDFium); (6) The op log stores operations as character-level regardless of direction — no server changes needed for RTL support beyond validating that direction formatting attributes are part of the allowed attribute set.

Q12: How do you handle a document editor session that has been idle for 30 minutes?
A12: Idle session management: (1) The client sends presence updates (AWARENESS messages) at 1 Hz while the user is active and the document is focused. When the browser tab is backgrounded or the user is inactive, AWARENESS frequency drops to 1/30 s; (2) After 30 minutes of no AWARENESS messages, the session server marks the client as "idle" (not disconnected — WebSocket is still alive); (3) After 60 minutes of no AWARENESS, the session server sends a `{ type: "IDLE_TIMEOUT", message: "You've been idle for 1 hour. Your connection will be closed in 60 seconds." }` message to the client; (4) If the client doesn't respond with AWARENESS, the WebSocket is closed; (5) The client can reconnect at any time and resume editing (state is preserved in Redis/S3); (6) Session server unloads the document from RAM after all clients disconnect (checkpoint to Redis first), freeing memory for other documents.

Q13: What consistency guarantees does your design provide?
A13: The system provides **Strong Eventual Consistency (SEC)**: (1) All operations are eventually applied everywhere (Kafka ensures all consumers receive all ops); (2) All clients that have applied the same set of operations will have identical document states (CRDT convergence guarantee); (3) Operations are never lost (Kafka durability + Cassandra persistence); (4) Causality is preserved: if Op A happens before Op B on the same client (according to that client's vector clock), all other clients will also apply A before B. The system is NOT strongly consistent in the CP sense: during a network partition, a client can continue editing locally (availability) but the server may not receive those edits (partition tolerance), and different clients may temporarily see different document states (no linearizability). This is the correct trade-off for a collaborative editor: availability and offline support > strong consistency.

Q14: How do you detect and prevent a malicious client from injecting operations on behalf of another user?
A14: Every WebSocket connection is authenticated with a JWT. The JWT payload contains `{ user_id, session_token, doc_id, permission }`. Operations received on a WebSocket connection are attributed to the `user_id` in the JWT — the client cannot spoof another user_id. The connect_token includes a signature over the tuple `(user_id, doc_id, expiry)` using the server's HMAC key; the session server verifies this signature on every WebSocket upgrade. For additional protection: (1) Operations are validated for physical plausibility (can't insert 10,000 characters in one op — max op size = 1,000 characters); (2) Rate limiting per user_id prevents flood attacks; (3) Session tokens are short-lived (1 hour); the client refreshes via the `/auth/refresh` endpoint; (4) If a JWT is compromised, it can be revoked by adding the JWT ID to a Redis blocklist (checked on every WebSocket upgrade).

Q15: What is the operational cost of running this system at 100 M DAU?
A15: Rough monthly cost estimates: (1) Session servers: 2,000 pods × $0.50/hr each = ~$720 k/month; (2) Cassandra: 40 nodes × $2/hr = ~$58 k/month; (3) Kafka: 20 brokers × $1/hr = ~$14.4 k/month; (4) Redis Cluster: 16 nodes × $1/hr = ~$11.5 k/month; (5) PostgreSQL: 1 primary + 5 replicas × $2/hr = ~$8.6 k/month; (6) S3 (1.1 PB × $0.023/GB + transfer costs): ~$26 k/month; (7) CDN: 10 TB/day × $0.05/GB transfer = ~$15 k/month; (8) Compute (API servers, gateways): 200 pods × $0.20/hr = ~$29 k/month. Total: ~$900 k/month in infrastructure. Revenue model: 100 M DAU × $6/month (Google Workspace Individual / Enterprise allocation) = $600 M/month revenue — a 667:1 revenue-to-infra ratio at scale. Actual Google Docs margins are much higher due to custom hardware, on-premises data center amortization, and engineering at scale.

---

## 12. References & Further Reading

1. Yjs CRDT Documentation and Architecture: https://docs.yjs.dev/api/about-yjs
2. YATA: Yet Another Transformation Approach — Nicolaescu et al. (2016): https://arxiv.org/abs/1608.04168v1
3. Automerge CRDT (alternative to Yjs): https://automerge.org/docs/hello/
4. "Designing Real-Time Collaboration Applications" — Google I/O Talk (Wave/Docs): https://www.youtube.com/watch?v=3ykZYKCK7AM (archived GWave tech talk)
5. "OT FAQ" by Marc Shapiro: http://operational-transformation.github.io/
6. CRDTs: Consistency without concurrency control — Shapiro et al. (2011): https://arxiv.org/abs/0907.0929
7. Liveblocks Real-Time Collaboration Infrastructure Blog: https://liveblocks.io/blog
8. "Merging OT and CRDT Algorithms" — Seph Gentle: https://josephg.com/blog/crdts-go-brrr/
9. Figma's Approach to Real-Time Collaboration (OT-based): https://www.figma.com/blog/how-figmas-multiplayer-technology-works/
10. Notion's Move to CRDTs: https://www.notion.so/blog/real-time-collaboration
11. Apache Kafka Documentation (Idempotent Producers): https://kafka.apache.org/documentation/#producerconfigs
12. Cassandra: Designing for Performance (DataStax): https://docs.datastax.com/en/cassandra-oss/3.0/cassandra/dml/dmlWritePath.html
13. "Local-First Software" — Kleppmann et al. (2019): https://www.inkandswitch.com/local-first/
14. Redis Sorted Sets and their implementation: https://redis.io/docs/data-types/sorted-sets/
15. "Building Offline-First Apps" — Mozilla Developer Network: https://developer.mozilla.org/en-US/docs/Web/Progressive_web_apps/Guides/Offline_and_background_operation
