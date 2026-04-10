# System Design: Photo Storage (Instagram / Google Photos Scale)

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Photo Upload** — Users can upload photos (JPEG, PNG, HEIC, RAW) up to 50 MB each. Upload returns a stable photo ID immediately; processing (thumbnails, ML analysis) is async.
2. **Photo Download / Delivery** — Original and resized versions (thumbnails, web-optimized) served at low latency via CDN. Supports progressive JPEG streaming.
3. **Album Organization** — Users can create albums and add/remove photos. Photos can belong to multiple albums. System auto-creates smart albums (e.g., "Italy 2024", "People: Alice").
4. **Deduplication** — Content-addressed storage: uploading the same photo twice (same bytes) uses one storage slot. The user still sees two entries (logical deduplication is transparent).
5. **EXIF Metadata Extraction** — On upload, extract and index: camera model, lens, GPS coordinates, capture timestamp, ISO, aperture, shutter speed, white balance.
6. **Face Detection & Recognition** — ML pipeline detects faces in photos, clusters similar faces into "person" clusters, allows users to label person clusters (name → cluster).
7. **Search** — Search by: filename, date range, location (GPS-based), camera model, face (after user labels), AI-generated scene labels (beach, sunset, food, etc.).
8. **Sharing** — Share individual photos or albums via link or with specific users. Shared albums allow contribution (others can add photos).
9. **Storage Quota** — 15 GB free (Google Photos model); unlimited original quality for paid tier.
10. **Deletion & Recovery** — Photos moved to trash on delete, auto-purged after 60 days. Bulk delete supported.

### Non-Functional Requirements

| Property | Target |
|---|---|
| Availability | 99.99% (< 52 min/year) |
| Durability | 99.999999999% (11 nines) |
| Upload latency (to durable commit) | p99 < 3 s for photos < 10 MB on broadband |
| Thumbnail delivery latency | p99 < 100 ms (CDN-served) |
| Original photo TTFB | p99 < 500 ms (CDN miss) |
| Scale | 2 billion users; 4 trillion photos stored; 1.4 billion photos uploaded/day (assumption: 5 photos/user/month → ~2.3 B/month → ~75 M/day for active base; Instagram publishes 100M uploads/day) |
| Search latency | p99 < 1 s |
| Face clustering latency | Async; complete within 60 s of upload |
| Security | Photos private by default; TLS in transit; AES-256 at rest |

**Assumption on scale:** Using 100 M uploads/day (Instagram-scale) as the primary number. 4 trillion total photos stored (Google Photos scale).

### Out of Scope

- Video storage and transcoding (treated as a separate system with different pipeline)
- Stories / ephemeral content (different retention model)
- Comment / like social graph (covered by separate social graph system)
- Payments and subscription management
- Content moderation pipeline internals (treated as a black-box ML service)

---

## 2. Users & Scale

### User Types

| User Type | Description | Behavior Pattern |
|---|---|---|
| **Casual User** | Uploads vacation photos, family events | ~50 photos/month; more read than write |
| **Power Photographer** | Semi-pro; RAW files; organized albums | ~500 photos/month; large files; heavy organization |
| **Social Media Active** | Frequent poster; curated aesthetic | ~30 uploads/day; strong sharing; views own content heavily |
| **Archivist** | Bulk-uploads historical photos | Batch upload of 10,000+ photos; rare read after |
| **Viewer** | Views shared albums; no uploads | Read-only; mobile-heavy |

### Traffic Estimates

**Assumptions:**
- 2 billion registered users; 500 M monthly active (MAU); 200 M daily active (DAU)
- Upload rate: 100 M photos/day (Instagram-published figure)
- Read-to-write ratio: 10:1 (photo views heavily outweigh uploads)
- Average photo size: 3 MB (HEIC from iPhone ~3 MB, JPEG ~1.5 MB, some RAW ~25 MB; average ~3 MB)
- Thumbnail sizes generated per photo: 3 variants (64×64, 360×360, 1080p)
- Average thumbnail size: 10 KB (small), 50 KB (medium), 200 KB (large)

| Metric | Calculation | Result |
|---|---|---|
| Uploads/day | 100 M | 100 M |
| Uploads/second (avg) | 100 M / 86,400 | ~1,157 uploads/s |
| Uploads/second (peak 3×) | 1,157 × 3 | ~3,471 uploads/s |
| Photo views/day | 100 M × 10 | 1 B views/day |
| Photo view requests/second (peak) | 1 B / 86,400 × 3 | ~34,700 views/s |
| Inbound bandwidth (upload, avg) | 1,157 × 3 MB | ~27.8 Gbps |
| Inbound bandwidth (upload, peak) | 3,471 × 3 MB | ~83.3 Gbps |
| Outbound bandwidth (views, avg, thumbnail only) | 11,574 × 50 KB | ~4.6 Gbps |
| Outbound bandwidth (views, peak) | 34,700 × 50 KB | ~13.9 Gbps |
| Face detection jobs/day | 100 M (1 per upload) | 100 M ML jobs/day |
| Face detection jobs/second (peak) | 3,471 | ~3,471 ML inferences/s |

### Latency Requirements

| Operation | p50 Target | p99 Target | Rationale |
|---|---|---|---|
| Photo upload (commit to durable store) | 500 ms | 3 s | Async processing OK; user sees upload success immediately |
| Thumbnail delivery (CDN hit) | 5 ms | 30 ms | Perceived as instant; critical UX |
| Thumbnail delivery (CDN miss) | 50 ms | 200 ms | First view of a new photo |
| Original photo TTFB (CDN miss) | 100 ms | 500 ms | Full resolution; acceptable to take slightly longer |
| Album listing (50 photos) | 30 ms | 100 ms | Interactive; user scrolling gallery |
| Search results | 200 ms | 1 s | Search UX tolerance is ~1 s |
| Face recognition (async) | n/a | 60 s | Background processing; user not waiting |
| GPS reverse geocoding (async) | n/a | 30 s | Background; used for smart albums |

### Storage Estimates

| Item | Calculation | Result |
|---|---|---|
| New photos/day | 100 M | 100 M |
| Raw storage/day (originals) | 100 M × 3 MB | 300 TB/day |
| Thumbnails/day (3 variants) | 100 M × (10+50+200) KB | ~26 TB/day |
| Total new storage/day | 300 + 26 | ~326 TB/day |
| Dedup savings (~15% exact duplicates) | 300 TB × 0.85 | ~255 TB/day after dedup |
| Replication (erasure coded 6+3 = 1.5×) | 281 TB × 1.5 | ~421 TB raw physical/day |
| 10-year accumulation | 421 TB × 365 × 10 | ~1,537 PB = ~1.5 EB |
| Metadata per photo (~2 KB) | 4 T photos × 2 KB | ~8 PB metadata |
| Total 4 trillion photos (est.) | 4 T × 3 MB × 0.85 dedup × 1.5 EC | ~15.3 EB raw |

### Bandwidth Estimates

| Direction | Avg | Peak |
|---|---|---|
| Inbound (upload) | ~27.8 Gbps | ~83.3 Gbps |
| Outbound (views, all sizes) | ~18.5 Gbps | ~55.5 Gbps |
| Outbound after CDN offload (90%) | ~1.85 Gbps | ~5.55 Gbps origin |
| Internal (ML inference reads) | 3,471 × 3 MB = ~83 Gbps | ~83 Gbps peak |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              CLIENTS                                         │
│  Mobile (iOS/Android Camera Roll)  │  Web Browser  │  Desktop App            │
└───────────────────────────────┬───────────────────────────────────────────── ┘
                                │ HTTPS / TLS 1.3
                                ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│             EDGE NETWORK (Anycast DNS, Global CDN, PoPs)                     │
│  Photo reads (thumbnails, originals) served via CDN edge nodes              │
│  Uploads routed to nearest regional API cluster                             │
└────────────────────────────────────────┬─────────────────────────────────────┘
                                         │
                                         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                          API GATEWAY CLUSTER                                │
│  (Auth, rate limiting, TLS termination, request routing)                    │
└───────────┬──────────────────────────────────────────┬────────────────────── ┘
            │                                          │
            ▼                                          ▼
┌────────────────────────────┐          ┌──────────────────────────────────┐
│     UPLOAD SERVICE         │          │     READ / DELIVERY SERVICE      │
│                            │          │                                  │
│  1. Receive multipart      │          │  - Photo metadata lookup         │
│  2. Compute SHA-256        │          │  - Permission check (ACL)        │
│  3. Dedup check            │          │  - Presigned URL generation      │
│  4. Write to Blob Store    │          │  - CDN origin serve              │
│  5. Extract EXIF           │          │  - Byte-range support            │
│  6. Commit metadata        │          └──────────────────────────────────┘
│  7. Publish to event queue │
└──────────────┬─────────────┘
               │ Kafka event: photo.uploaded
               ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                      ASYNC PROCESSING PIPELINE                               │
│                                                                              │
│  ┌───────────────────────┐  ┌────────────────────┐  ┌─────────────────────┐ │
│  │  Thumbnail Generator  │  │  Face Detection    │  │  Scene Labeling     │ │
│  │  (ImageMagick/libvips)│  │  (ML: MTCNN,       │  │  (ML: ResNet-50 /   │ │
│  │  3 variants per photo │  │   FaceNet)          │  │   ViT for tags)     │ │
│  └───────────┬───────────┘  └─────────┬──────────┘  └──────────┬──────────┘ │
│              │                        │                         │            │
│  ┌───────────────────────┐  ┌────────────────────┐  ┌─────────────────────┐ │
│  │  EXIF Extractor       │  │  Face Clusterer    │  │  Geo Tagger         │ │
│  │  (ExifTool / libexif) │  │  (DBSCAN / ANN)    │  │  (Reverse geocode   │ │
│  │  GPS, camera, date    │  │  per-user clusters  │  │   GPS → place name) │ │
│  └───────────┬───────────┘  └─────────┬──────────┘  └──────────┬──────────┘ │
└──────────────┼────────────────────────┼──────────────────────── ┼────────────┘
               │                        │                         │
               └──────────────┬─────────┘─────────────────────────┘
                              ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                           DATA STORES                                        │
│                                                                              │
│  ┌─────────────────────┐  ┌──────────────────────┐  ┌────────────────────┐  │
│  │  Photo Metadata DB  │  │  Content-Addressed   │  │  Search Index      │  │
│  │  (Cassandra)        │  │  Blob Store          │  │  (Elasticsearch)   │  │
│  │  - photo_id, user,  │  │  (GCS / S3 /         │  │  - EXIF data       │  │
│  │    sha256, path,    │  │   Custom CAS store)  │  │  - Scene labels    │  │
│  │    metadata, tags   │  │  Key: sha256(photo)  │  │  - GPS index       │  │
│  └─────────────────────┘  └──────────────────────┘  └────────────────────┘  │
│  ┌─────────────────────┐  ┌──────────────────────┐  ┌────────────────────┐  │
│  │  Face Index         │  │  Album / Social DB   │  │  Redis Cache       │  │
│  │  (FAISS vector DB   │  │  (PostgreSQL)        │  │  - Hot photo meta  │  │
│  │   per user)         │  │  - Albums, shares    │  │  - ACL checks      │  │
│  └─────────────────────┘  └──────────────────────┘  └────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │ CDN (Cloudflare │
                    │  / Cloud CDN)   │
                    │  Photo delivery │
                    └─────────────────┘
```

**Component Roles:**

- **Upload Service**: Receives photo bytes, computes SHA-256, checks content-addressed store for existing blob (dedup), writes blob if novel, extracts basic EXIF, commits photo metadata record, publishes async events. Returns `photo_id` to client immediately after metadata commit.
- **Thumbnail Generator**: Consumes `photo.uploaded` Kafka events; reads original photo from blob store; generates 3 thumbnail variants using libvips (faster than ImageMagick for high-concurrency, lower memory); stores thumbnails as separate blobs in blob store; updates metadata record with thumbnail URLs.
- **Face Detection**: GPU-accelerated ML worker using MTCNN (Multi-task Cascaded CNN) for face detection + FaceNet for 128-d face embeddings. Stores detected face bounding boxes and embeddings in the Face Index per photo.
- **Face Clusterer**: DBSCAN or ANN (Approximate Nearest Neighbor using FAISS) clustering of face embeddings per user. Groups similar embeddings into "person clusters." Incremental: new photo's faces are compared against existing clusters; similar faces merge into existing cluster.
- **Scene Labeling**: ViT (Vision Transformer) or ResNet-based multi-label classifier. Labels each photo with scene/object tags (beach, food, mountains, pets, etc.) stored in the search index for semantic search.
- **Content-Addressed Blob Store (CAS Store)**: Object storage where each blob's key is its SHA-256 hash. Identical photos from different users map to the same blob. Reference-counted for garbage collection. Built on top of GCS/S3 or a custom Colossus-like distributed file system.
- **Photo Metadata DB (Cassandra)**: Stores per-photo structured metadata: owner, capture time, EXIF, thumbnail paths, ML labels, ACL. Cassandra chosen for write-heavy workload (100 M photos/day = 1,157 writes/s steady), time-series query pattern (get photos by user + date), and linear horizontal scale.
- **Face Index (FAISS per user)**: Per-user vector database of 128-d face embeddings. FAISS (Facebook AI Similarity Search) supports approximate nearest neighbor search in O(log N) with IVF (Inverted File Index) quantization. Each user's FAISS index stored as a file in blob store; loaded into memory on face search requests.
- **Album / Social DB (PostgreSQL)**: Stores albums, photo-album mappings, sharing relationships, permissions. Lower write rate (album ops vs photo ops); relational model appropriate for complex ACL queries.
- **CDN**: All thumbnail and original photo deliveries served via CDN. Cache key = sha256 (content-addressed → immutable; can cache with max-age=1 year). Cache hit rate target: 90%+ for thumbnails.

**Primary Use-Case Data Flow (Photo Upload + Async Processing):**

1. Mobile client uploads photo bytes via multipart HTTPS to Upload Service.
2. Upload Service: compute SHA-256 of photo bytes.
3. CAS Store dedup check: `GET /blobs/{sha256}` → 404 (novel) → `PUT /blobs/{sha256}` to store photo bytes.
4. Upload Service extracts EXIF synchronously (< 100 ms via libexif).
5. Upload Service commits metadata to Cassandra: `{photo_id=uuid, user_id, sha256, size_bytes, content_type, exif_json, upload_time, status='processing'}`.
6. Upload Service returns `{photo_id, status: "processing"}` to client (< 2 s total).
7. Upload Service publishes event to Kafka topic `photo.uploaded`: `{photo_id, user_id, sha256, exif}`.
8. Kafka consumers (workers) process in parallel:
   - Thumbnail Generator: generates 3 variants → stores in CAS store → updates metadata `thumbnail_urls`.
   - Face Detection: ML inference → stores face embeddings in Face Index → publishes `photo.faces_detected`.
   - Scene Labeler: ML inference → stores labels in metadata + search index.
   - Geo Tagger: reverse geocodes GPS from EXIF → stores place name → updates search index.
9. After thumbnails ready (typically < 30 s), metadata status updated to `'ready'`.
10. Client polls or receives push notification: photo is ready to display.

---

## 4. Data Model

### Entities & Schema

```sql
-- PHOTO METADATA (Cassandra schema)
-- Primary: (user_id) → partition; (upload_time DESC, photo_id) → clustering for timeline queries

CREATE TABLE photos (
    photo_id        UUID,
    user_id         UUID,
    sha256          TEXT,                   -- hex; content-address in blob store
    original_size   BIGINT,                 -- bytes
    width           INT,
    height          INT,
    content_type    TEXT,                   -- 'image/jpeg', 'image/heic', etc.
    status          TEXT,                   -- 'processing', 'ready', 'error', 'deleted'
    upload_time     TIMESTAMP,
    capture_time    TIMESTAMP,              -- from EXIF DateTimeOriginal
    -- EXIF metadata
    camera_make     TEXT,
    camera_model    TEXT,
    lens_model      TEXT,
    focal_length    FLOAT,
    aperture        FLOAT,                  -- f-number
    shutter_speed   TEXT,                   -- e.g., "1/500"
    iso             INT,
    gps_lat         DOUBLE,
    gps_lng         DOUBLE,
    gps_altitude    FLOAT,
    gps_place_name  TEXT,                   -- reverse-geocoded (async-filled)
    -- ML-generated
    scene_labels    LIST<TEXT>,             -- e.g., ['beach', 'sunset', 'ocean']
    -- Storage paths
    thumbnail_small TEXT,                   -- CDN URL for 64×64
    thumbnail_medium TEXT,                  -- CDN URL for 360×360
    thumbnail_large TEXT,                   -- CDN URL for 1080p
    original_url    TEXT,                   -- CDN URL for original
    -- Flags
    is_favorite     BOOLEAN,
    is_archived     BOOLEAN,
    is_deleted      BOOLEAN,
    deleted_at      TIMESTAMP,
    -- Primary key
    PRIMARY KEY ((user_id), upload_time, photo_id)
) WITH CLUSTERING ORDER BY (upload_time DESC, photo_id ASC)
  AND default_time_to_live = 0
  AND compaction = {'class': 'TimeWindowCompactionStrategy',
                    'compaction_window_unit': 'DAYS',
                    'compaction_window_size': 7};

-- Secondary index for sha256 lookup (for dedup check at photo record level)
CREATE INDEX idx_photos_sha256 ON photos (sha256);

-- ALBUMS (PostgreSQL)
CREATE TABLE albums (
    album_id        UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    owner_id        UUID        NOT NULL,
    title           VARCHAR(255) NOT NULL,
    description     TEXT,
    cover_photo_id  UUID,
    album_type      TEXT        NOT NULL DEFAULT 'manual'
                    CHECK (album_type IN ('manual', 'smart', 'shared', 'face', 'location', 'highlights')),
    smart_filter    JSONB,          -- criteria for smart albums (e.g., {date_range, place_name, person_id})
    is_shared       BOOLEAN     NOT NULL DEFAULT FALSE,
    share_link_token TEXT        UNIQUE,
    allow_contributions BOOLEAN NOT NULL DEFAULT FALSE,
    photo_count     INT         NOT NULL DEFAULT 0,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_albums_owner ON albums(owner_id, updated_at DESC);

-- ALBUM-PHOTO MAPPING (PostgreSQL)
CREATE TABLE album_photos (
    album_id        UUID        NOT NULL REFERENCES albums(album_id),
    photo_id        UUID        NOT NULL,
    user_id         UUID        NOT NULL,   -- denormalized for shard routing
    added_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    added_by        UUID        NOT NULL REFERENCES users(user_id),
    sort_order      INT,                    -- manual ordering within album
    PRIMARY KEY (album_id, photo_id)
);
CREATE INDEX idx_album_photos_album ON album_photos(album_id, sort_order, added_at DESC);

-- CONTENT-ADDRESSED BLOB REGISTRY (separate service, stored in distributed KV)
-- Conceptually:
CREATE TABLE blobs (
    sha256          TEXT        PRIMARY KEY,    -- 64-char hex
    size_bytes      BIGINT      NOT NULL,
    storage_bucket  TEXT        NOT NULL,
    storage_path    TEXT        NOT NULL,       -- e.g., "blobs/ab/cd/abcd1234..."
    ref_count       BIGINT      NOT NULL DEFAULT 1,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    mime_type       TEXT
);

-- FACE CLUSTERS (per user; stored in Cassandra for scale)
CREATE TABLE face_clusters (
    user_id         UUID,
    cluster_id      UUID,
    label           TEXT,           -- user-assigned name ("Mom", "Alice"), NULL if unlabeled
    cover_photo_id  UUID,           -- representative photo for this cluster
    photo_count     INT,
    created_at      TIMESTAMP,
    updated_at      TIMESTAMP,
    PRIMARY KEY ((user_id), cluster_id)
);

-- FACE DETECTIONS (per photo; stored in Cassandra)
CREATE TABLE face_detections (
    photo_id        UUID,
    face_id         UUID,           -- unique within photo
    cluster_id      UUID,           -- which person cluster this face belongs to
    user_id         UUID,           -- photo owner
    -- Bounding box (normalized 0.0–1.0)
    bbox_x          FLOAT,
    bbox_y          FLOAT,
    bbox_w          FLOAT,
    bbox_h          FLOAT,
    -- 128-d FaceNet embedding stored separately in FAISS index
    -- Stored here for reference/debugging
    embedding_version TEXT,         -- model version that generated this embedding
    confidence      FLOAT,
    detected_at     TIMESTAMP,
    PRIMARY KEY ((photo_id), face_id)
);
CREATE INDEX idx_face_detections_cluster ON face_detections (cluster_id);

-- SHARING ACL (PostgreSQL)
CREATE TABLE photo_shares (
    share_id        UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    resource_type   TEXT        NOT NULL CHECK (resource_type IN ('photo', 'album')),
    resource_id     UUID        NOT NULL,
    owner_id        UUID        NOT NULL,
    grantee_type    TEXT        NOT NULL CHECK (grantee_type IN ('user', 'anyone')),
    grantee_id      UUID,               -- NULL for 'anyone' (public link)
    link_token      TEXT        UNIQUE, -- for public links
    permission      TEXT        NOT NULL CHECK (permission IN ('view', 'contribute')),
    expires_at      TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_shares_resource ON photo_shares(resource_id, resource_type);
CREATE INDEX idx_shares_grantee  ON photo_shares(grantee_id) WHERE grantee_id IS NOT NULL;
```

### Database Choice

**Candidates:**

| Database | Pros | Cons | Role |
|---|---|---|---|
| **Cassandra** | 1,157 writes/s (photo uploads) → LSM-tree perfect; time-series queries (photos by user+date) map to partition+clustering keys; linear horizontal scale | Limited join support; no aggregation; eventual consistency; schema changes painful | Selected for photos table (write-heavy, time-series) and face_detections |
| **PostgreSQL** | Full relational queries; ACID; mature; great for albums/ACL which have complex joins | Cannot scale to 4 trillion rows on single cluster; sharding is manual | Selected for albums, sharing (low cardinality, relational) |
| **DynamoDB** | Serverless; predictable latency; scales automatically | Vendor lock-in; limited query patterns; expensive at sustained high throughput | Alternative to Cassandra |
| **MySQL** | Familiar; ACID; good tooling | Single-region vertical limits; less ideal for time-series vs Cassandra | Alternative for albums |
| **FAISS (vector DB)** | ANN search in 128-d space in < 10 ms for millions of vectors | Not a general-purpose DB; must be managed as files; no persistence out of the box | Face similarity search |
| **Elasticsearch** | Full-text + geospatial + numeric range queries; horizontal scale; near-real-time indexing | High memory overhead; not for primary storage; eventual consistency on index | Search index for EXIF, labels, GPS |

**Selected:**
- **Cassandra** for photos + face metadata: time-series writes, partition-by-user, clustering by date.
- **PostgreSQL** for albums + sharing: relational, low cardinality, ACID needed for album operations.
- **FAISS (per-user, stored in blob store, loaded on demand)** for face embeddings: ANN search for face clustering.
- **Elasticsearch** for search: geospatial queries (GPS bounding box), full-text scene labels, date range.
- **Redis** for hot metadata cache (popular photo metadata, ACL decisions).

Cassandra TimeWindowCompactionStrategy (TWCS) is critical for the photos table: photos are queried primarily by recency (timeline view). TWCS groups SSTables by time window (7-day windows), ensuring old data compacts into large, infrequently-accessed SSTables while new data remains in hot SSTables. This avoids compaction amplification that would kill write performance.

---

## 5. API Design

All endpoints require `Authorization: Bearer <JWT_access_token>` except public shared content.
Rate limits: 100 uploads/hour/user; 10,000 reads/hour/user.

### Upload & Management

```
-- Upload photo
POST /v1/photos
  Headers:
    Content-Type: multipart/form-data OR image/jpeg
    X-Upload-Content-SHA256: {sha256}   -- optional; for pre-verification
    X-Upload-Content-Length: {bytes}
  Body: photo bytes (for direct upload) OR multipart form
  Response (201):
    { "photo_id": "550e8400-e29b-41d4-a716-446655440000",
      "status": "processing",
      "upload_time": "2026-04-09T12:00:00Z",
      "thumbnails": null  -- null while processing
    }

-- Get photo metadata
GET /v1/photos/{photo_id}
  Response (200):
    { "photo_id", "user_id", "status", "capture_time", "upload_time",
      "width", "height", "size_bytes", "content_type",
      "exif": { "camera_make", "camera_model", "focal_length", "aperture",
                "iso", "gps_lat", "gps_lng", "gps_place_name" },
      "scene_labels": ["beach", "ocean", "sunset"],
      "thumbnails": {
        "small":  "https://cdn.example.com/t/64/{sha256}.jpg",
        "medium": "https://cdn.example.com/t/360/{sha256}.jpg",
        "large":  "https://cdn.example.com/t/1080/{sha256}.jpg"
      },
      "original_url": "https://cdn.example.com/o/{sha256}",
      "is_favorite": false,
      "faces": [{ "face_id", "cluster_id", "label", "bbox": {x,y,w,h} }]
    }

-- List photos (timeline)
GET /v1/photos?page_token={token}&limit=50&order=desc&before={iso8601}&after={iso8601}
  Response (200):
    { "photos": [PhotoSummary, ...],    -- PhotoSummary = photo_id, thumbnail_medium, capture_time
      "next_page_token": "{cursor}",
      "has_more": true }
  -- page_token encodes (last_seen_upload_time, last_seen_photo_id) for Cassandra cursor

-- Update photo (favorite, archive, etc.)
PATCH /v1/photos/{photo_id}
  Body: { "is_favorite"?: bool, "is_archived"?: bool }
  Response (200): PhotoMetadata

-- Delete photo (soft delete)
DELETE /v1/photos/{photo_id}
  Response (204)
  -- Sets is_deleted=true, deleted_at=now(); purged after 60 days by GC job

-- Bulk delete
POST /v1/photos/batch_delete
  Body: { "photo_ids": ["uuid1", "uuid2", ...] }  -- max 100
  Response (200): { "deleted": ["uuid1"], "errors": [] }

-- Get original photo (presigned URL or direct stream)
GET /v1/photos/{photo_id}/download
  Response (200): { "url": "https://cdn.example.com/o/{sha256}?expires=...", "expires_in": 3600 }
  -- Or stream directly:
GET /v1/photos/{photo_id}/original
  Headers: Range: bytes=0-1048575
  Response (200 / 206): photo bytes with Content-Type, Content-Length, ETag
```

### Albums

```
GET  /v1/albums?page_token={token}&limit=20
  Response: { "albums": [AlbumSummary], "next_page_token" }

POST /v1/albums
  Body: { "title", "description"?, "album_type": "manual" }
  Response (201): AlbumResource

GET  /v1/albums/{album_id}
  Response (200): AlbumResource with first 50 photos

GET  /v1/albums/{album_id}/photos?page_token={token}&limit=100
  Response (200): { "photos": [PhotoSummary], "next_page_token" }

POST /v1/albums/{album_id}/photos
  Body: { "photo_ids": ["uuid1", "uuid2"] }  -- add photos to album
  Response (200): { "added": 2 }

DELETE /v1/albums/{album_id}/photos/{photo_id}
  Response (204)

PATCH /v1/albums/{album_id}
  Body: { "title"?, "cover_photo_id"?, "allow_contributions"? }
  Response (200): AlbumResource

DELETE /v1/albums/{album_id}
  Response (204)  -- does not delete photos; only removes album
```

### Search

```
GET /v1/search?q={query}&type=all&page_token={token}&limit=50
  -- type: all | date | location | person | label | camera
  -- q examples:
  --   "beach 2024"           → full-text + date range
  --   "near:37.7749,-122.4194,10km"  → geo radius search
  --   "person:cluster_abc123"         → face cluster search
  --   "camera:iPhone 15"              → camera model search
  --   "label:food"                    → scene label search
  Response (200):
    { "results": [PhotoSummary], "total_count": 1234,
      "facets": { "dates": [...], "places": [...], "labels": [...] },
      "next_page_token" }

-- Semantic / AI search (natural language query)
POST /v1/search/semantic
  Body: { "query": "photos of my dog at the park", "limit": 20 }
  Response (200): { "results": [PhotoSummary] }
  -- Uses CLIP-style joint image-text embedding for similarity search
```

### Face Management

```
GET /v1/faces/clusters
  Response (200): { "clusters": [{ "cluster_id", "label", "photo_count", "cover_photo_id" }] }

PATCH /v1/faces/clusters/{cluster_id}
  Body: { "label": "Mom" }
  Response (200): FaceCluster  -- assigns human-readable name to cluster

POST /v1/faces/clusters/{cluster_id}/merge
  Body: { "merge_into_cluster_id": "uuid2" }
  Response (200): FaceCluster  -- merge two mistaken-identity clusters

GET /v1/faces/clusters/{cluster_id}/photos?page_token={token}
  Response (200): { "photos": [PhotoSummary], "next_page_token" }
```

### Sharing

```
POST /v1/photos/{photo_id}/share
  Body: { "grantee_type": "anyone" | "user", "grantee_email"?: "...",
          "permission": "view", "expires_in_days"?: 30 }
  Response (201): { "share_link": "https://photos.example.com/s/{link_token}",
                    "share_id", "expires_at" }

DELETE /v1/photos/{photo_id}/share/{share_id}
  Response (204)

-- Access shared photo (no auth required for 'anyone' shares)
GET /s/{link_token}
  Response: Redirects to photo view with temporary token; or returns photo metadata
```

---

## 6. Deep Dive: Core Components

### 6.1 Content-Addressable Storage (CAS) & Deduplication

**Problem it solves:**
Users frequently upload the same photo multiple times (re-downloads, multiple backup sources, family members uploading the same event photos). Without deduplication, 4 trillion photos × 3 MB = 12 EB of raw data; with ~15% exact duplicate rate, dedup saves ~1.8 EB. Additionally, CAS enables immutable, safely cacheable CDN URLs (the URL contains the content hash, so it never changes for the same content) and simplifies the thumbnail pipeline (generate thumbnails for a hash once; share across all users who have the same photo).

**Approaches Comparison:**

| Approach | Description | Dedup Level | Complexity | Storage Savings |
|---|---|---|---|---|
| **No dedup (per-user copy)** | Each upload creates a unique file | None | Trivial | 0% |
| **File-level hash dedup** | SHA-256 of full photo; skip upload if exists | File | Low | ~15% (exact duplicates) |
| **Block-level dedup** | Split photo into chunks; dedup at block level | Block | Medium | ~20-25% for photos (photos don't share sub-block content meaningfully) |
| **Perceptual hash dedup** | pHash / dHash — dedup near-identical photos (slight crop/filter) | Near-duplicate | High (ML pipeline) | ~25-30% but introduces false-positives risk |
| **CAS with file-level SHA-256 (selected)** | Store blob keyed by SHA-256; ref-count; single logical copy per unique content | File | Low-Medium | ~15% storage savings; +100% CDN cache efficiency |

**Selected: File-level SHA-256 CAS with reference counting**

Justification:
- **File-level is sufficient for photo storage**: unlike source code or office documents, photos don't meaningfully share sub-block content. A 3 MB JPEG from a phone has no 256 KB block that appears in another JPEG. Block-level dedup adds significant metadata overhead (millions of chunk records per TB) with near-zero additional savings for photos.
- **CAS enables immutable CDN URLs**: since the CDN cache key is the SHA-256 hash, and the photo's bytes never change (immutable blob), CDN can cache with `max-age=31536000` (1 year). No invalidation ever needed. This is a major CDN efficiency win.
- **Perceptual hash dedup rejected**: the false positive rate is unacceptable — deduplicating two slightly different photos from different users (near-identical but distinct) would violate user expectations and potentially expose data across user boundaries. CAS dedup is transparent: users see their photos; storage layer happens to share the same bytes.

**Implementation Detail:**

```python
# Upload flow (Upload Service)
def upload_photo(user_id, photo_bytes, filename):
    # Step 1: Content hash
    sha256 = hashlib.sha256(photo_bytes).hexdigest()

    # Step 2: Check CAS (dedup check)
    blob_exists = cas_store.exists(sha256)

    if not blob_exists:
        # Step 3a: Write to CAS blob store
        storage_path = f"blobs/{sha256[0:2]}/{sha256[2:4]}/{sha256}"
        cas_store.put(sha256, photo_bytes, storage_path)
        # Update blobs table: INSERT or ignore (idempotent)
        db.execute("""
            INSERT INTO blobs (sha256, size_bytes, storage_bucket, storage_path, ref_count)
            VALUES (?, ?, 'photos-primary', ?, 1)
            ON CONFLICT (sha256) DO UPDATE SET ref_count = blobs.ref_count + 1
        """, (sha256, len(photo_bytes), storage_path))
    else:
        # Step 3b: Dedup: just increment ref_count
        db.execute("""
            UPDATE blobs SET ref_count = ref_count + 1 WHERE sha256 = ?
        """, (sha256,))

    # Step 4: Extract EXIF
    exif = extract_exif(photo_bytes)

    # Step 5: Generate photo_id and commit metadata
    photo_id = str(uuid.uuid4())
    cassandra.execute("""
        INSERT INTO photos (photo_id, user_id, sha256, upload_time, capture_time,
                            original_size, status, camera_make, camera_model,
                            gps_lat, gps_lng, ...)
        VALUES (?, ?, ?, ?, ?, ?, 'processing', ?, ?, ?, ?, ...)
    """, (photo_id, user_id, sha256, now(), exif.capture_time, len(photo_bytes),
          exif.camera_make, exif.camera_model, exif.gps_lat, exif.gps_lng))

    # Step 6: Publish event
    kafka.publish('photo.uploaded', {
        'photo_id': photo_id,
        'user_id': user_id,
        'sha256': sha256,
        'size_bytes': len(photo_bytes),
        'exif': exif.to_dict()
    })

    return photo_id

# Deletion / GC flow
def delete_photo(photo_id, user_id):
    # Soft delete: mark is_deleted=true
    cassandra.execute("""
        UPDATE photos SET is_deleted=true, deleted_at=?
        WHERE user_id=? AND photo_id=?
    """, (now(), user_id, photo_id))
    # Note: ref_count NOT decremented here; decremented by GC job after 60-day grace period

# GC job (runs nightly)
def gc_deleted_photos():
    # Find photos deleted > 60 days ago
    stale = cassandra.execute("""
        SELECT photo_id, sha256 FROM photos
        WHERE is_deleted=true AND deleted_at < ?
        LIMIT 10000 ALLOW FILTERING
    """, (now() - timedelta(days=60),))  # In practice: secondary index on (is_deleted, deleted_at)

    for photo in stale:
        # Decrement ref_count
        result = db.execute("""
            UPDATE blobs SET ref_count = ref_count - 1
            WHERE sha256 = ? RETURNING ref_count
        """, (photo.sha256,))

        if result.ref_count == 0:
            # Schedule blob for physical deletion (7-day grace period)
            blob_gc_queue.enqueue(photo.sha256, execute_after=now() + timedelta(days=7))

        # Hard delete photo metadata
        cassandra.execute("""
            DELETE FROM photos WHERE user_id=? AND photo_id=?
        """, (photo.user_id, photo.photo_id))
```

**Thumbnail URL construction (immutable CDN URLs):**
```
Template: https://cdn.photos.example.com/{variant}/{sha256}.{format}
Examples:
  - Small (64×64):  https://cdn.photos.example.com/64/{sha256}.jpg
  - Medium (360px): https://cdn.photos.example.com/360/{sha256}.jpg
  - Large (1080px): https://cdn.photos.example.com/1080/{sha256}.jpg
  - Original:       https://cdn.photos.example.com/o/{sha256}

Cache-Control: public, max-age=31536000, immutable
-- Same sha256 = same bytes = same URL = same CDN cache entry
-- All users who share the same underlying photo hit the same CDN cache entry
-- Zero cache invalidation ever needed (immutable content)
```

**Interviewer Q&As:**

Q1: If two users upload the same photo, how do you prevent user A from accessing user B's photo through the shared CAS store?
A1: CAS dedup is entirely invisible to users. The blob store is not publicly accessible — it's an internal storage system. User-facing access is gated by the metadata layer: a GET request for photo_id X checks that the requesting user owns photo_id X (or has a share grant). Only then is a CDN URL returned. For public/shared CDN URLs, the URL contains the SHA-256 which is an opaque hash — a user cannot enumerate others' SHA-256 values. Even if they could (e.g., they receive a shared photo), knowing the SHA-256 only gives access to the bytes (which they already have, since the photo was shared with them). Private photos use signed CDN URLs with short TTLs.

Q2: What is the privacy implication of CAS dedup? If law enforcement requests a user's photo and it's stored as a shared blob, does that expose other users?
A2: This is a real legal/privacy concern. CAS dedup is a storage-layer implementation detail. From a legal standpoint: if law enforcement issues a subpoena for user A's photos, the system returns the bytes associated with user A's photo records — the fact that those bytes happen to be stored once rather than N times is irrelevant. The legal access is scoped to user A's metadata records. The blob itself does not identify any user — it is just bytes identified by a hash. The system does NOT reveal that user B also has the same photo. Each user's privacy is scoped to their metadata records.

Q3: How do you handle HEIC / RAW formats that browsers can't natively display?
A3: HEIC (from iPhone) and RAW (from DSLRs) are converted during the thumbnail generation pipeline. The Thumbnail Generator uses libheif (for HEIC decode) and dcraw/LibRaw (for RAW decode) to decode into a pixel buffer, then re-encode to JPEG or WebP using libvips. The original file is preserved exactly (CAS stores original bytes unmodified). Thumbnails are always served in web-compatible formats (JPEG, WebP based on Accept header). For download of originals, the user gets the original HEIC/RAW file. The conversion happens asynchronously after upload, so the thumbnail may not be immediately available (status: 'processing' until thumbnails are ready).

Q4: How do you handle upload deduplication when the same person uploads from multiple devices simultaneously?
A4: This is a write-write conflict scenario. Both uploads compute the same SHA-256. The CAS blob write is idempotent (the second writer sees the blob already exists, skips the write, just increments ref_count — using an atomic compare-and-swap or database transaction). At the Cassandra layer, both uploads attempt to INSERT into `photos` with different `photo_id` UUIDs — both succeed (different primary keys). The user ends up with two photo records pointing to the same SHA-256 blob. This is correct: the user uploaded "the same photo twice" — perhaps intentionally or accidentally. A subsequent dedup pass (which compares sha256 + capture_time + exif) can detect logical duplicates and offer to merge them, but this is a UX feature, not enforced automatically.

Q5: How do you handle partial uploads / interrupted uploads for large photos?
A5: For photos > 5 MB (e.g., RAW files), the API supports multipart uploads (similar to S3): (1) Client calls `POST /v1/uploads/init` → returns `upload_id`; (2) Client sends chunks `PUT /v1/uploads/{upload_id}/chunk/{index}` — each chunk stored as a temporary blob in CAS staging area (TTL: 7 days); (3) Client calls `POST /v1/uploads/{upload_id}/complete` → server assembles chunks, computes full-file SHA-256, runs dedup check, commits photo metadata. On network failure, client resumes with `GET /v1/uploads/{upload_id}` to determine which chunks were received, then uploads only missing chunks. This mirrors S3's multipart upload protocol.

---

### 6.2 Face Detection & Clustering Pipeline

**Problem it solves:**
Google Photos' "People" feature automatically groups photos by person without requiring user tagging. This requires: (1) detecting all faces in each photo (face detection), (2) generating a compact representation of each face (face embedding), (3) clustering similar embeddings per user (face clustering), and (4) incrementally updating clusters as new photos arrive. At 100 M photos/day with ~1.5 faces/photo on average = ~150 M face detections/day.

**Approaches Comparison:**

| Component | Option A | Option B | Selected |
|---|---|---|---|
| Face Detection | Haar Cascades (OpenCV) | MTCNN (CNN-based) | MTCNN: handles partial faces, multiple scales, varying poses; Haar is fast but too many false positives |
| Face Embedding | Eigenfaces (PCA) | FaceNet (deep CNN, 128-d) | FaceNet: 128-d embedding trained with triplet loss; L2 distance directly correlates with identity similarity |
| Face Clustering | K-Means (K unknown) | DBSCAN | DBSCAN: handles variable cluster sizes; doesn't require K; identifies noise/outliers (non-face detections) |
| ANN Search (new photo → existing cluster) | Linear scan (O(N)) | FAISS IVF (O(log N)) | FAISS: 1 ms search in 1M-vector index; linear is unacceptable for users with 100K+ photos |

**Selected: MTCNN → FaceNet (128-d) → FAISS IVF for ANN → DBSCAN for initial clustering + incremental assignment**

**Implementation Detail:**

```python
# Face Detection Worker (GPU node)
def process_photo_faces(photo_id, user_id, sha256):
    # Download photo from CAS store
    photo_bytes = cas_store.get(sha256)
    img = decode_image(photo_bytes)

    # Step 1: Face Detection with MTCNN
    # Returns: list of (bounding_box, landmarks, confidence)
    mtcnn = MTCNNDetector(min_face_size=20, thresholds=[0.6, 0.7, 0.7])
    detections = mtcnn.detect(img)

    if not detections:
        # No faces; update metadata
        cassandra.execute("UPDATE photos SET status='ready' WHERE ...")
        return

    face_records = []
    embeddings = []
    for det in detections:
        # Step 2: Align and crop face using landmarks (5-point: eyes, nose, mouth corners)
        aligned_face = align_face(img, det.landmarks, output_size=(160, 160))

        # Step 3: Generate 128-d FaceNet embedding
        facenet = FaceNetModel()  # loaded once per worker; GPU-resident
        embedding = facenet.embed(aligned_face)  # shape: (128,), normalized L2

        face_id = str(uuid.uuid4())
        face_records.append({
            'face_id': face_id,
            'photo_id': photo_id,
            'bbox': det.bounding_box,
            'confidence': det.confidence
        })
        embeddings.append((face_id, embedding))

    # Step 4: Match embeddings against user's FAISS index
    user_faiss = load_user_faiss_index(user_id)  # loaded from blob store
    cluster_assignments = []
    for face_id, embedding in embeddings:
        # ANN search: find nearest existing cluster centroid
        distances, cluster_ids = user_faiss.search(embedding, k=5)
        nearest_dist = distances[0]
        nearest_cluster = cluster_ids[0]

        SIMILARITY_THRESHOLD = 0.6  # L2 distance; tuned on validation set
        if nearest_dist < SIMILARITY_THRESHOLD and nearest_cluster != -1:
            # Assign to existing cluster
            cluster_id = nearest_cluster
        else:
            # Create new cluster
            cluster_id = str(uuid.uuid4())
            cassandra.execute("""
                INSERT INTO face_clusters (user_id, cluster_id, label, photo_count)
                VALUES (?, ?, null, 0)
            """, (user_id, cluster_id))

        # Add embedding to FAISS index
        user_faiss.add_with_ids(embedding, cluster_id)
        cluster_assignments.append((face_id, cluster_id))

    # Persist updated FAISS index to blob store
    save_user_faiss_index(user_id, user_faiss)

    # Step 5: Write face detections to Cassandra
    for face_record, (face_id, cluster_id) in zip(face_records, cluster_assignments):
        cassandra.execute("""
            INSERT INTO face_detections
            (photo_id, face_id, cluster_id, user_id, bbox_x, bbox_y, bbox_w, bbox_h, confidence)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (photo_id, face_id, cluster_id, user_id,
              face_record['bbox'].x, face_record['bbox'].y,
              face_record['bbox'].w, face_record['bbox'].h,
              face_record['confidence']))

        # Update cluster photo_count
        cassandra.execute("""
            UPDATE face_clusters SET photo_count = photo_count + 1
            WHERE user_id=? AND cluster_id=?
        """, (user_id, cluster_id))

    # Step 6: Mark photo as ready
    cassandra.execute("""
        UPDATE photos SET status='ready' WHERE user_id=? AND photo_id=?
    """, (user_id, photo_id))
```

**FAISS Index Management:**

Each user's face index is stored as a file in the CAS blob store: `faiss/{user_id}/index.bin`. The file is:
- Loaded into GPU memory on the face worker when processing a photo for that user
- Saved back after updates
- Cached in an LRU on each GPU worker node (hot users' indices stay in memory)

Index type: `IndexIVFFlat` (Inverted File Index): clusters the embedding space into `nlist=100` cells (Voronoi cells). Search: probe `nprobe=10` nearest cells. This gives O(nprobe × n/nlist) = O(10 × N/100) = O(N/10) per query, vs O(N) for linear scan. For a user with 10,000 faces (reasonable for a power user), linear scan is 10,000 distance computations; FAISS IVF is ~1,000. For a user with 100,000 faces, FAISS takes ~1 ms, linear takes ~10 ms. Beyond ~1 M faces per user (extreme case), switch to `IndexIVFPQ` (Product Quantization) for compressed embeddings.

**Interviewer Q&As:**

Q1: How do you handle the privacy of face data? Face embeddings are biometric data under GDPR.
A1: Face embeddings are treated as sensitive biometric data: (1) stored encrypted at rest (AES-256, user-specific key derived from user's account key); (2) not shared between users — each user's face index is completely isolated; (3) facial recognition is opt-in (must be explicitly enabled in settings); (4) users can delete all face data (triggers deletion of all `face_detections` records and the user's FAISS index); (5) embeddings are stored in a separate data store from photo metadata, subject to stricter access controls; (6) GDPR right-to-erasure for biometric data is processed within 30 days (same as general right-to-erasure).

Q2: How does the system handle a user with identical twins? Two people with very similar face embeddings will cluster together.
A2: This is a known limitation of any face clustering system. Mitigation: (1) The similarity threshold (0.6 L2 distance) is tuned to be somewhat conservative — identical twins may appear as separate clusters if the embedding model captures subtle differences (which FaceNet does to some degree); (2) Users can manually split a merged cluster (the UI shows all photos in a cluster; user can select a subset and move to a new cluster); (3) The `POST /v1/faces/clusters/{cluster_id}/merge` endpoint handles merges; a split endpoint handles the reverse; (4) When a user labels both "Alice" and "Bob" with photos from the same cluster, the system can suggest a split. Ultimately, manual correction is the fallback for edge cases.

Q3: How does face clustering scale as a user uploads more and more photos over years?
A3: The FAISS index grows with the number of face embeddings. For most users (< 10,000 photos, < 15,000 faces), `IndexIVFFlat` is sufficient. For power users (> 100,000 photos, > 150,000 faces): (1) Switch index type to `IndexIVFPQ` (Product Quantization reduces memory by 8-16×); (2) If user exceeds 1 M faces (a professional photographer's lifetime portfolio), introduce hierarchical clustering: first cluster at the DBSCAN level, then represent each cluster by its centroid rather than all member embeddings. The index size scales as O(active_clusters) rather than O(total_faces). Incremental clustering: new photos always go through ANN search against centroids, not all-pairs comparison.

Q4: What accuracy does FaceNet achieve, and what are the false positive / false negative rates?
A4: FaceNet (as published in the 2015 paper by Schroff et al.) achieves 99.63% accuracy on the LFW (Labeled Faces in the Wild) benchmark. In production, with the same model: false positive rate (two different people clustered together) ~0.1-1% depending on threshold tuning; false negative rate (same person in separate clusters) ~2-5%. These rates are acceptable because: (1) false clusters are surfaced to users who can merge them; (2) false separations are also surfaced (user sees "4 clusters" and can merge); (3) the system doesn't make any consequential decision from clustering — it's a UI convenience. The threshold is deliberately tuned to favor false negatives (separate clusters) over false positives (merged different people), because merging wrong people is more confusing to users.

Q5: How would you handle a new ML model version that generates different embeddings than the previous version? All existing embeddings are incompatible.
A5: This is a model migration problem. Strategy: (1) **Dual-model period**: deploy new model alongside old; all new photos processed with both models; old photos backfilled with new model (async batch job over weeks); (2) **Separate indices**: maintain `face_index_v1` and `face_index_v2` per user until migration complete; (3) **Batch re-embed**: for each user, re-run FaceNet v2 on all stored photos (read original from CAS store); this is O(total_photos) inference — at 4 trillion photos, infeasible to do at once. Prioritize active users; inactive users migrated lazily on next login; (4) **Post-migration**: new clustering run on v2 embeddings (DBSCAN re-cluster per user); user-assigned labels (cluster → name) are migrated using old cluster → photo membership → new cluster mapping.

---

### 6.3 Thumbnail Generation Pipeline

**Problem it solves:**
Photo viewers render thumbnails, not originals. A 3 MB original JPEG displayed in a 64×64 pixel grid cell wastes 99.9% of the transferred bytes. Generating thumbnails server-side allows the CDN to serve < 50 KB versions instead of 3 MB originals, reducing CDN egress cost by ~60× and dramatically improving mobile page load times. The challenge: 100 M photos/day = 100 M × 3 thumbnails = 300 M thumbnail generation jobs/day, needing to complete within ~30 seconds of upload (user expects to see their photo quickly).

**Approaches Comparison:**

| Approach | Throughput | Latency | Cost | Flexibility |
|---|---|---|---|---|
| **On-demand (generate on first GET)** | Scales with demand | High first-view latency | No wasted compute; only generate what's needed | Maximum flexibility; no pre-computation |
| **Pre-generate all variants at upload** | Limited by ingestion pipeline | Low first-view latency (after pipeline completes) | Compute cost even for photos never viewed | Consistent CDN cache population |
| **Progressive on-demand with pre-generate** | Hybrid | On-demand for first access; pre-generated for common sizes | Moderate | Best of both; complex to operate |
| **Client-side thumbnails** | No server cost | Zero (local) | Zero | Limited: different clients produce different quality; server can't cache |

**Selected: Pre-generate all 3 standard variants at upload time (async pipeline)**

Justification: 90%+ of uploaded photos are viewed at least once within 24 hours (social sharing, personal review). Pre-generating all 3 variants ensures the CDN is warm before users view the photo. The compute cost is well-defined (3 thumbnails × 100 M photos/day) and manageable with auto-scaled workers.

**Implementation Detail:**

```python
# Thumbnail Generator Worker
import pyvips  # libvips Python binding; 3-10× faster than ImageMagick; lower memory

THUMBNAIL_VARIANTS = [
    {'name': 'small',  'max_dim': 64,   'format': 'jpeg', 'quality': 80},
    {'name': 'medium', 'max_dim': 360,  'format': 'jpeg', 'quality': 85},
    {'name': 'large',  'max_dim': 1080, 'format': 'webp', 'quality': 85},
]

def generate_thumbnails(photo_id, user_id, sha256, original_content_type):
    # Download original from CAS store
    original_bytes = cas_store.get(sha256)

    # Decode with pyvips (handles JPEG, PNG, HEIC via libheif, WebP, TIFF, RAW via dcraw)
    # Use 'access=VIPS_ACCESS_SEQUENTIAL' for streaming decode (lower memory for large files)
    img = pyvips.Image.new_from_buffer(original_bytes, '', access='sequential')

    # Auto-rotate based on EXIF orientation flag (strips EXIF from thumbnail)
    img = img.autorot()

    thumbnail_urls = {}
    for variant in THUMBNAIL_VARIANTS:
        # Resize maintaining aspect ratio (max_dim = larger dimension)
        thumb = img.thumbnail_image(
            variant['max_dim'],
            height=variant['max_dim'],
            size=pyvips.Size.DOWN,     # never upscale
            crop=pyvips.Interesting.NONE  # preserve full image (no cropping)
        )

        # For social feed thumbnails (square crop), use:
        # crop=pyvips.Interesting.ATTENTION  # smart-crop focusing on salient region

        # Strip metadata (privacy: remove GPS, camera info from thumbnails)
        thumb = thumb.copy()

        # Encode to target format
        if variant['format'] == 'jpeg':
            thumb_bytes = thumb.jpegsave_buffer(Q=variant['quality'], strip=True,
                                                 optimize_coding=True, interlace=True)
            # interlace=True → progressive JPEG (better perceived loading)
        elif variant['format'] == 'webp':
            thumb_bytes = thumb.webpsave_buffer(Q=variant['quality'], strip=True,
                                                 reduction_effort=4)

        # Compute SHA-256 of thumbnail (for CAS dedup and CDN URL)
        thumb_sha256 = hashlib.sha256(thumb_bytes).hexdigest()
        thumb_key = f"thumbnails/{variant['max_dim']}/{thumb_sha256}"

        # Store thumbnail in CAS store
        if not cas_store.exists(thumb_sha256):
            cas_store.put(thumb_sha256, thumb_bytes,
                         f"thumbnails/{variant['max_dim']}/{thumb_sha256[0:2]}/{thumb_sha256[2:4]}/{thumb_sha256}")

        # Construct immutable CDN URL
        cdn_url = f"https://cdn.photos.example.com/{variant['max_dim']}/{thumb_sha256}"
        thumbnail_urls[variant['name']] = cdn_url

    # Update photo metadata with thumbnail URLs
    cassandra.execute("""
        UPDATE photos
        SET thumbnail_small=?, thumbnail_medium=?, thumbnail_large=?, status='ready'
        WHERE user_id=? AND photo_id=?
    """, (thumbnail_urls['small'], thumbnail_urls['medium'], thumbnail_urls['large'],
          user_id, photo_id))
```

Pipeline scaling:
- Workers are stateless containers on Kubernetes; scale based on Kafka `photo.uploaded` consumer group lag.
- Target: keep lag < 5 minutes (process 100 M photos/day = 1,157 photos/second).
- libvips benchmarks: ~500 ms/photo for all 3 variants on a single CPU core. At 1,157 photos/s: 1,157 / (1000ms/500ms) = 578 CPU cores needed. With 4 cores per worker: ~145 worker pods minimum; ~435 at peak (3×).
- GPU not needed for thumbnail generation (it's image processing, not ML inference).

**Interviewer Q&As:**

Q1: How do you handle thumbnail generation for a 50 MB RAW file from a professional camera?
A1: RAW files require decode via LibRaw or dcraw before pyvips can process them. LibRaw is integrated into pyvips as a load operation. RAW decode is CPU-intensive (~5-10 seconds for a 50 MB RAW file). For RAW files: (1) use a separate worker queue with longer timeout and higher memory allocation (2-4 GB per worker, as RAW decode can expand a 50 MB file to 200+ MB in memory); (2) use pyvips' embedded thumbnail extraction for RAW files that have a pre-computed JPEG thumbnail in the EXIF — this is a 100 ms operation vs 10 seconds for full decode; (3) the extracted EXIF thumbnail (usually 1-2 MP) is used as the source for smaller variants; full decode only for the 1080p variant. This hybrid approach achieves < 2 s thumbnail generation for most RAW files.

Q2: How do you ensure thumbnails are available when users immediately open the app after uploading?
A2: The upload flow returns immediately after metadata commit (status: 'processing'). The mobile client uses an optimistic UI: it shows a local preview of the photo (generated on-device before upload completes) in the gallery immediately. When thumbnails become available on the server (typically 5-30 seconds), a push notification (via FCM/APNs) or SSE event updates the status. The client then replaces the local preview with the server-generated CDN thumbnail. If the user navigates to the photo detail view before thumbnails are ready, the original photo is streamed directly (with a loading indicator for slow connections). This progressive enhancement strategy ensures zero perceived delay from the user's perspective.

Q3: How does progressive JPEG improve perceived loading speed?
A3: Standard JPEG is encoded top-to-bottom — the user sees a partial image from the top as it loads. Progressive JPEG encodes multiple passes at increasing quality: first pass = low quality full image (appears blurry but fully visible), subsequent passes add detail. At 50% download completion, the user sees a blurry but recognizable full image, vs standard JPEG which shows a sharp-but-cropped top half. This dramatically improves perceived loading speed on slow connections. libvips's `interlace=True` parameter enables progressive JPEG encoding with minimal overhead (+5-10% file size, sometimes smaller for natural images). WebP and AVIF support similar progressive modes.

---

## 7. Scaling

### Horizontal Scaling

- **Upload Service**: Stateless; scale horizontally. Consistent hash routing by `user_id` to improve CAS blob store cache locality per instance.
- **Read / Delivery Service**: Stateless; scale horizontally. CDN absorbs 90%+ of reads; origin tier scales based on CDN miss rate.
- **Thumbnail Generator Workers**: Horizontally scalable Kubernetes pods. Autoscale based on Kafka consumer lag metric (`photo.uploaded` topic).
- **Face Detection Workers**: GPU-enabled pods; scale based on `photo.faces_pending` Kafka topic lag. GPU scheduling via Kubernetes device plugins.
- **Cassandra Cluster**: Add nodes to increase throughput (Cassandra scales linearly with node count). Shard by `user_id` partition key.
- **PostgreSQL (albums)**: Single-writer + read replicas; auto-failover with Patroni. Read replicas for listing queries.
- **Elasticsearch**: Add data nodes to scale search indexing throughput and storage. Shard by `user_id` for query isolation.

### DB Sharding (Cassandra)

Cassandra partitions by `user_id` (hash partition key). Each physical partition is replicated across 3 nodes (replication factor 3) using `NetworkTopologyStrategy` across 3 AZs. Token-aware routing: the application driver sends requests directly to the node(s) responsible for the given `user_id` partition, avoiding unnecessary inter-node hops. Compaction: `TimeWindowCompactionStrategy` for the `photos` table (time-series access pattern).

### Replication

- **CAS Blob Store (GCS/S3)**: Erasure coded 6+3 within region; cross-region replication for paid storage tiers (photos of paid users replicated to a second region for higher durability).
- **Cassandra**: RF=3, `NetworkTopologyStrategy` (1 replica per AZ). Writes use `QUORUM` consistency (writes ≥ 2 AZs before ack). Reads use `LOCAL_QUORUM` (read from at least 2 nodes in local AZ).
- **PostgreSQL**: Synchronous streaming replication to 1 standby; async to 1 additional standby in second AZ.
- **Elasticsearch**: ES replica shards (1 replica per primary shard); cross-cluster replication for DR.

### Caching Strategy

| Layer | What | TTL | Invalidation |
|---|---|---|---|
| CDN | Thumbnails, originals | 1 year (immutable sha256 URLs) | Never (immutable) |
| CDN | Shared album listing pages | 5 minutes | Purge on album modification |
| Redis (L1) | Photo metadata (hot photos) | 300 s | Invalidated on update (PATCH photo) |
| Redis (L1) | ACL decisions for shared photos | 60 s | Invalidated on share grant/revoke |
| Redis (L1) | Album contents (first page) | 30 s | Invalidated on photo add/remove |
| Cassandra built-in | Row cache for hot partitions | LRU | Automatic eviction |
| GPU worker in-process | User's FAISS index (LRU) | LRU (100 users max per node) | Load on miss from blob store |

### CDN Strategy

CDN cache key = thumbnail URL = `https://cdn/{variant}/{sha256}`. Since URLs are content-addressed (sha256 = hash of thumbnail bytes), they are perfectly immutable. `Cache-Control: public, max-age=31536000, immutable`. No invalidation is ever needed for thumbnails or originals.

Exceptions requiring invalidation:
- **Privacy change**: user makes photo private after it was publicly shared → purge CDN entries for `{sha256}` variants. This is done via CDN tag-based invalidation (tag each CDN entry with the photo_id; invalidate by tag on privacy change).
- **Thumbnail regeneration** (model quality upgrade): CDN URLs change (new thumbnail has different bytes → different sha256 → new URL). No invalidation needed; old URL simply stops being linked from metadata; expires naturally after 1 year.

CDN geo-distribution: PoPs in all major regions (NA, EU, APAC, SA). Photos accessed frequently from a specific geography are pinned to that PoP's cache. For a viral photo (viewed by millions), the CDN serves all requests — origin sees < 0.1% of traffic.

### Interviewer Q&As

Q1: How would you handle a spike in uploads on New Year's Eve (everyone photographs the fireworks)?
A1: New Year's Eve spike prediction: 10×-20× normal rate for ~1 hour. Mitigation: (1) Pre-scale all upload-path services by 15× starting 30 minutes before midnight (predicted based on prior year telemetry); (2) Kafka's `photo.uploaded` topic buffers the spike — thumbnail generation and face detection workers can process asynchronously; upload path only needs to handle the PUT throughput; (3) Rate limiting: globally, if inbound exceeds 20× baseline, apply soft rate limits (return HTTP 429 with `Retry-After: 5` to non-premium users) — premium users are prioritized; (4) CAS blob store (GCS/S3) scales elastically; (5) Cassandra throughput = N_nodes × 10,000 writes/s/node — pre-scale cluster; (6) CDN pre-warms caches for popular photo types (fireworks photos will be shared widely within minutes).

Q2: How does the search index stay consistent with photos as they are deleted or privacy-changed?
A2: Photo updates (delete, privacy change, label addition) publish events to a Kafka `photo.updated` topic. Search indexer (Elasticsearch) consumes this topic and: (1) For delete: `DELETE /photos/_doc/{photo_id}` (Elasticsearch soft delete → actually removes on next segment merge); (2) For privacy change (private): remove from Elasticsearch or add an `is_private: true` field; all search queries include `is_private: false` filter; (3) For new label/EXIF addition: `POST /photos/_update/{photo_id}` with partial document. Elasticsearch is near-real-time (NRT): 1-second refresh interval means changes appear in search within 1 second of processing. For privacy changes, we force an immediate refresh (`POST /photos/_refresh`) to avoid privacy violations in search results.

Q3: How do you scale the EXIF/GPS indexing pipeline to support 100 M uploads/day?
A3: EXIF extraction is fast (< 100 ms in libexif) and done synchronously at upload time. GPS reverse geocoding (lat/lng → "Paris, France") requires an external API call or local gazetteer database. At 100 M photos/day × 30% with GPS = 30 M geocoding requests/day = 347/second. This is an async pipeline: GPS coordinates from EXIF are stored immediately; a Geo Tagger worker (consuming `photo.uploaded` Kafka events) performs reverse geocoding using a local offline gazetteer (Natural Earth data + OpenStreetMap via Nominatim or Google Geocoding API for high-accuracy). The Geo Tagger updates `photos.gps_place_name` and the Elasticsearch document with the resolved place name, enabling location-based search. Local gazetteer means no per-query external API cost at 347 req/s.

Q4: How would you design the "Memories" feature (e.g., "3 years ago today")?
A4: Memories scans the `photos` Cassandra table for each active user, filtering by `capture_time` date (month+day matching today's date, regardless of year). This is not a standard Cassandra query (Cassandra doesn't support arbitrary date-part filtering efficiently). Implementation: a nightly batch job (running at 2 AM local time per user timezone) pre-computes each user's memories for the next day. It queries: `SELECT * FROM photos WHERE user_id=? AND capture_time >= {N_years_ago - 1 day} AND capture_time <= {N_years_ago + 1 day}` (running for N=1,2,3,...,7) and stores the result in a `memories_feed` Cassandra table (keyed by `user_id + date`). When the user opens the app, the Memories section reads from `memories_feed` — a fast O(1) Cassandra point lookup. The batch job processes 200 M DAU / (24 × 60) = ~139 users/minute — easily parallelizable across hundreds of batch workers.

Q5: How would you handle photos uploaded over a slow 2G mobile connection with frequent disconnects?
A5: Client-side chunked upload with resumability: (1) SDK chunks the photo into 512 KB segments; (2) For each chunk, attempt HTTP PUT with a `X-Upload-Offset` header (inspired by tus.io resumable upload protocol); (3) If connection drops, client retries the last chunk (idempotent PUT); (4) Server tracks received chunks in `upload_sessions.chunks_received` bitmap; (5) Client can resume by first calling `HEAD /v1/uploads/{session_id}` to get the current `Upload-Offset`; (6) Upload session TTL: 30 days (enough for a user who uploads from a satellite connection with daily outages); (7) Chunk size is adaptive: client measures round-trip time and adjusts chunk size (larger chunks for fast connections, smaller for slow/lossy).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Upload Service crash mid-upload | Upload incomplete | Client timeout / TCP RST | Upload sessions table survives crash; client resumes from last committed chunk |
| Cassandra node failure | Some photo metadata reads/writes fail | Cassandra gossip protocol detects in 10 s | RF=3; QUORUM reads/writes tolerate 1 node failure; Cassandra auto-repairs hinted handoff |
| Thumbnail generation worker crash | Photo stuck in 'processing' status | Status check timeout (30 min) | Kafka consumer group auto-rebalances to surviving workers; message redelivered; idempotent (thumbnail generation for same sha256 is safe to retry) |
| Face detection worker failure | Face clusters incomplete | Kafka lag spike on face detection topic | Requeue failed events; isolated from photo serving (photo is 'ready' after thumbnails, independent of face detection) |
| CAS blob store (GCS) unavailable | Uploads fail; downloads fail (CDN miss) | GCS health API | CDN serves existing cached content; new uploads queued (client retries); upload_sessions preserve state |
| Redis cache cluster failure | Higher latency (cache miss → Cassandra reads) | Error rate spike on Redis | Graceful degradation: all reads fall back to Cassandra; performance degraded but functionally correct |
| Face FAISS index corruption | Face clustering stops for user | Error on load attempt | Rebuild from face_detections table (all embeddings stored per-detection); slow but recoverable |
| ML model container crash | Face detection / scene labeling delayed | Kafka lag spike; container restart alert | Kubernetes restarts container (pod liveness probe); failed events requeued in Kafka (max 3 retries, then DLQ) |
| Content delivery (CDN) outage | Photo viewing fails | CloudFront health checks | Multi-CDN failover (primary + backup CDN provider); URL construction includes CDN host; fallback to origin with additional caching |

### Retries & Idempotency

- **CAS blob write**: PUT with SHA-256 key is idempotent (same bytes, same key, same result). Safe to retry.
- **Photo metadata commit**: uses photo_id as idempotency key. Cassandra INSERT is idempotent (same primary key → upsert same data).
- **Thumbnail generation**: keyed by `(sha256, variant)` → same operation, same output. Safe to retry; second execution finds thumbnail already in CAS store, skips write.
- **Face detection**: keyed by `(photo_id, face_id)` for each detected face. Idempotent insert to `face_detections`. FAISS index update is not idempotent but duplicate embeddings are handled by checking if face_id already exists before FAISS add.
- All Kafka consumer workers use manual offset commit: offset committed only after successful processing. Failed processing causes re-delivery (at-least-once semantics). All downstream operations are idempotent, so re-delivery is safe.

### Circuit Breaker

External dependencies (Cassandra, Redis, Elasticsearch, GCS) wrapped in circuit breakers. Parameters: open at 15% error rate over 20s; half-open after 30s. On circuit open: (1) CAS store circuit open → upload returns 503; (2) Cassandra circuit open → serve from Redis cache where possible, else 503; (3) Elasticsearch circuit open → search returns empty results with `"degraded": true` flag.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Tool |
|---|---|---|---|
| Upload success rate | Counter ratio | < 99.5% / 5 min | Datadog |
| Upload p99 latency (to metadata commit) | Histogram | > 5 s | Datadog |
| Thumbnail generation lag (Kafka) | Gauge | > 5 min backlog | Prometheus |
| Face detection lag (Kafka) | Gauge | > 30 min backlog | Prometheus |
| CDN hit rate (thumbnails) | Counter ratio | < 90% | CDN analytics |
| CAS dedup ratio | Counter ratio | < 10% (unexpected low dedup) | Internal dashboard |
| Cassandra write p99 | Histogram | > 50 ms | Cassandra JMX metrics |
| FAISS index load failures | Counter | > 0/hour | PagerDuty |
| Photos stuck in 'processing' > 1 hour | Gauge | > 1,000 photos | PagerDuty P2 |
| GPU worker utilization | Gauge | > 90% for 5 min | Prometheus (DCGM) |

### Distributed Tracing

OpenTelemetry traces for the full upload flow: API Gateway → Upload Service (CAS check, blob write, metadata commit, Kafka publish) → Thumbnail Worker (decode, resize, CDN URL) → Face Detection Worker. Traces linked by `photo_id` as a trace baggage attribute. Sampling: 1% for happy-path uploads; 100% for errors and uploads > 10 s. Key spans: `cas_dedup_check`, `blob_write`, `cassandra_insert`, `kafka_publish`, `thumbnail_gen_{variant}`, `mtcnn_inference`, `facenet_embed`, `faiss_search`.

### Logging

- Structured JSON: `{"ts":..., "trace_id":..., "photo_id":..., "user_id_hash":..., "op":"thumbnail_gen", "variant":"medium", "duration_ms":120, "sha256_prefix":"abcd...", "result":"success"}`.
- PII: user_id logged as HMAC hash; GPS coordinates NOT logged (privacy-sensitive); photo content never logged.
- Access log: CDN access logs (anonymized IPs) → S3 → Athena for query analytics (CDN hit ratio by geography, popular photo types).
- Audit log: all share/unshare events, privacy changes, deletions. Immutable append-only pipeline (Kafka → S3 Parquet → Glacier).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|---|---|---|---|
| Dedup strategy | File-level SHA-256 CAS | Block-level or perceptual hash | Block-level has minimal additional savings for photos; perceptual hash has false-positive privacy risk |
| Primary metadata DB | Cassandra | DynamoDB or PostgreSQL | Time-series access pattern (user + date); linear scale for 4 trillion photos; TWCS for efficient compaction |
| Albums/shares DB | PostgreSQL | Cassandra | Relational model needed for complex joins; lower write rate; ACID for album ops |
| Thumbnail CDN strategy | Content-addressed immutable URLs | Per-photo versioned URLs | Immutable URLs never need invalidation; perfect CDN efficiency; simplifies operations |
| Thumbnail format | JPEG (small/medium), WebP (large) | PNG everywhere | JPEG 20-40% smaller than PNG for photos; WebP 25-35% smaller than JPEG for large variant |
| Face detection ML | MTCNN + FaceNet | Traditional Haar + Eigenfaces | Deep learning dramatically outperforms classical methods in recall and accuracy |
| Face clustering | FAISS per-user + DBSCAN | Global shared face index | Per-user isolation is privacy requirement; DBSCAN handles unknown cluster count |
| Thumbnail generation timing | Pre-generate at upload (async) | On-demand on first GET | 90%+ photos viewed within 24h; CDN pre-warming; eliminates TTFB spike on first view |
| CAS storage path | Two-level directory sharding (`{sha256[0:2]}/{sha256[2:4]}/`) | Flat or single-level | Avoids filesystem hot spots with millions of files per directory |
| Photo timeline query | Cassandra with (user_id, upload_time) clustering | Elasticsearch for all queries | Timeline is the most frequent query; Cassandra partition scan is O(1) for this; ES reserved for search |
| Soft delete retention | 60 days | Immediate hard delete | User recovery: 60 days allows accidental deletion recovery; reasonable storage cost |

---

## 11. Follow-up Interview Questions

Q1: How would you implement the "Similar Photos" feature (grouping near-duplicate photos)?
A1: Use perceptual hashing (pHash or dHash): compute a 64-bit perceptual hash of each photo's pixel content (after resizing to 32×32 grayscale). Two photos are "similar" if their Hamming distance < 10 (configurable threshold). Implementation: (1) compute pHash for each photo at upload time, store in Cassandra `photos.phash` column; (2) to find similar photos for a given photo, use a BK-tree (Burkhard-Keller tree) or VP-tree indexed on the pHash — nearest-neighbor search in Hamming space in O(log N). Alternative: store pHashes in a separate Redis set per user; find near-duplicates using Redis's `BITCOUNT` and XOR operations. Present duplicates as a cluster in the "Clean Up" UI section.

Q2: How would you handle location-based features (geo search, "Explore by Place" map view)?
A2: GPS data is indexed in Elasticsearch using its native `geo_point` type: `{"gps": {"lat": 48.8566, "lon": 2.3522}}`. Geo queries: (1) `geo_distance` filter for "photos within X km of current location"; (2) `geo_bounding_box` for photos in a map viewport (drag the map → fetch photos in bounding box); (3) `geo_grid` aggregation for clustering nearby photos into map pins (avoids rendering 10,000 individual pins on a zoomed-out map). Place names from reverse geocoding enable text search (type "Paris" → find photos with `gps_place_name` containing "Paris"). Smart album generation: group photos by geohash prefix (coarse level = city, fine level = neighborhood).

Q3: How would you implement a "Print-Quality Download" feature that watermarks the original before download?
A3: Original photos are stored without modification in CAS (SHA-256 of clean bytes). Watermarking is an on-demand server-side transformation: (1) user requests print download; (2) server fetches original from CAS; (3) watermark applied in memory using libvips (composites a semi-transparent logo at a specific position); (4) watermarked version returned directly (streamed to client); NOT stored in CAS (watermarked bytes are never persisted, to avoid storage waste). For performance: popular watermarked photos can be stored in a temporary cache (Redis or short-TTL CAS entry with a special key `wm_{sha256}_{watermark_version}`). Watermark TTL: 1 hour (enough for a download to complete).

Q4: How does Google Photos handle the transition from photo processing to "ready" state? What guarantees are made?
A4: The upload returns `status: "processing"` immediately. The status transitions are: `processing → ready` (after thumbnails done) or `processing → error` (if processing fails permanently after 3 retries). Clients poll `GET /v1/photos/{photo_id}` or subscribe to server-sent events (SSE) for status updates. Guarantee: the photo is durable (stored in CAS) as soon as the upload returns 201. Thumbnails are a "best effort" enhancement — if the thumbnail pipeline fails, the photo is still accessible (original can be downloaded; thumbnail shows a placeholder). `status: 'ready'` means thumbnails are available, but missing thumbnails do not mean the photo is lost. The client falls back to streaming the original if thumbnails are unavailable after 5 minutes.

Q5: How would you implement a shared album where multiple users contribute photos?
A5: Shared album model: (1) Album owner enables `allow_contributions=true`; (2) invited users (via `POST /v1/albums/{id}/share`) receive `'contribute'` permission; (3) contributors can `POST /v1/albums/{id}/photos` to add their own photos; (4) the album_photos join table records `added_by` (contributor's user_id) and the photo retains its original owner's user_id and storage quota; (5) album listing aggregates photos across multiple users' Cassandra partitions — potentially expensive. Optimization: album_photos table in PostgreSQL is the source of truth; it stores (album_id, photo_id, user_id, sort_order); a Cassandra materialized view `album_timeline` (partitioned by album_id, clustered by added_at) provides efficient album listing without cross-partition Cassandra reads. Notifications: all album members receive push notification when a photo is added (Kafka fan-out similar to Dropbox notification pattern).

Q6: How would you handle copyright detection for uploaded photos?
A6: Content-based fingerprinting using perceptual hashing (PhotoDNA or similar proprietary hash): (1) compute a robust perceptual hash (cryptographic-quality fingerprint resilient to crop/resize/filter) for each uploaded photo; (2) compare against a hash database of known-copyright content (licensed from Getty, Getty Images, major news agencies); (3) if match score > threshold: block upload and return HTTP 451 (Unavailable For Legal Reasons) with a DMCA notice; (4) this comparison runs in the upload path (synchronously before metadata commit, since violating content should not be stored). The hash comparison is fast (< 50 ms with a bloom filter pre-filter on the hash database); the hash database is periodically synced from copyright registries. This does NOT prevent storage of all copyrighted content (most photos are copyrighted by their photographer, who is also the uploader), only blocks fingerprinted content from known infringement databases.

Q7: How would you design the storage tier transitions for cold/old photos?
A7: Lifecycle policy: (1) Standard tier: photos accessed in last 90 days; stored on SSD-backed hot storage; (2) Standard-IA (Infrequent Access): photos not accessed in 90-365 days; moved to HDD-backed cold storage; lower storage cost, slight increase in retrieval latency (+50 ms); (3) Archive: photos > 1 year old and not marked as favorite; compressed to AVIF format (50% smaller than JPEG for equivalent quality) and stored on tape/deep archive; retrieval requires 5-30 minute restore job; (4) Immutable originals are re-stored in the cheaper tier without quality loss (AVIF lossless or at the same quality level as the JPEG original). Transition is triggered by the Lifecycle Manager (similar to S3 lifecycle policies). User-visible: photos in archive tier show a "Restoring..." indicator when accessed; available for download after restore completes.

Q8: How does the system handle photos that are part of an active investigation (legal hold)?
A8: Legal hold overrides all deletion policies: (1) photos under legal hold are flagged with `legal_hold=true` in metadata; (2) all delete operations (manual, soft-delete expiry, lifecycle transition, GC ref-count zero) check `legal_hold` before proceeding; (3) flagged photos are never deleted; quota is not decremented for held photos (fairness: user shouldn't be charged for court-ordered data retention); (4) legal hold is applied via an internal admin API, not user-facing; (5) hold can only be removed by authorized legal team members; all hold/release operations are immutably logged (append-only audit trail). Implementation mirrors S3 Object Lock COMPLIANCE mode.

Q9: How would you handle a user who requests export of all their photos (GDPR data portability)?
A9: Export flow: (1) User requests `POST /v1/account/export` → returns `job_id`; (2) Async export job: (a) query Cassandra for all non-deleted photos for the user (potentially millions of rows — paginated with ALLOW FILTERING on `is_deleted=false`); (b) for each photo, fetch original from CAS blob store; (c) package into a ZIP file (streaming, to avoid holding entire archive in memory); (d) upload ZIP to a user-private export bucket (with 7-day TTL); (e) send email notification with download link; (3) Export job runs in a low-priority background queue (doesn't compete with upload/serving traffic); (4) Typical SLA: deliver export within 48 hours of request; large accounts (millions of photos) may take longer; (5) Rate limit: 1 export request per 30 days per user.

Q10: How would you design AI-powered automatic photo editing suggestions (enhance brightness, contrast, etc.)?
A10: Async ML pipeline triggered after thumbnails are ready: (1) run a CNN-based image quality assessment model (predicts: underexposed, overexposed, blurry, too-high ISO noise, skewed horizon); (2) for each detected issue, compute suggested correction parameters (brightness delta, contrast delta, rotation angle, noise reduction strength); (3) store suggestions in photo metadata: `edit_suggestions: [{type: "brightness", value: +0.2}, {type: "rotation", value: -2.3}]`; (4) when user opens photo editor, pre-apply suggestions as default one-click "Auto Enhance"; (5) edited versions are generated on-demand with libvips (applying the parameters) and cached as new thumbnails with a `_enhanced` suffix in the URL. Original is never modified — non-destructive editing. Suggestions are opt-in and clearly labeled as AI-generated.

Q11: How would you implement a collaborative photo editing feature where multiple users can annotate the same photo?
A11: Annotations (text overlays, arrows, crop suggestions) are stored as structured data separate from the photo bytes: a `photo_annotations` table in PostgreSQL, with columns `(annotation_id, photo_id, user_id, annotation_type, geometry_json, text, created_at)`. Geometry is stored in PostGIS-compatible format (polygon, point, line). Real-time collaboration: (1) users viewing the same shared photo with annotation mode enabled connect via WebSocket (annotation-session-id = photo_id); (2) annotation changes broadcast via the session; (3) server persists annotations transactionally; (4) conflict resolution: last-write-wins per annotation_id (annotations are fine-grained enough that concurrent edits to the same annotation are rare). The original photo bytes are never modified; annotations are rendered as a compositing layer client-side (or server-side via libvips when exporting an annotated version).

Q12: How does the system handle HEIC photos uploaded from iPhone with iOS privacy features blurring faces?
A12: iOS 17+ can apply face/person "smart blur" to photos before sharing (distinct from the photo stored in Camera Roll). This is a client-side iOS privacy feature; the photo uploaded to our service has the blur baked into the JPEG/HEIC bytes. From our system's perspective, the uploaded photo contains blurred faces — face detection will find no faces (or low-confidence detections) on those pixels. This is expected behavior; our system respects whatever pixels are sent. For photos where the user uploads via our native iOS app from Camera Roll (with permission), we receive the original unblurred photo from Camera Roll (not the shared version). The distinction is in iOS's permission model: apps reading from Camera Roll with PHPhotoLibrary permission get originals; apps receiving photos via Share Sheet may receive processed versions.

Q13: How would you handle quota calculation for a user with 2 million photos?
A13: Per-photo quota accounting would require summing `size_bytes` across 2 million rows — too slow for real-time quota checks. Instead: (1) maintain a denormalized `users.used_bytes` counter, updated atomically at upload commit (`UPDATE users SET used_bytes = used_bytes + ? WHERE user_id = ?`); (2) quota check: compare `users.used_bytes + new_photo_size > users.quota_bytes` before allowing upload; (3) for the `users` table in PostgreSQL (not Cassandra), this is a fast single-row read; (4) eventual consistency acceptable: `used_bytes` cached in Redis (updated on write with Redis `INCRBY`), reconciled against the authoritative PostgreSQL value nightly (to catch any divergence due to failed transactions). Thumbnail sizes contribute to quota differently by plan: free tier charges for thumbnails; paid tier does not (incentive to pay).

Q14: What would you change if photos also needed to be searchable by the object in them (e.g., "photos with cats")?
A14: Semantic search via CLIP (Contrastive Language-Image Pre-Training, OpenAI 2021): (1) encode each photo into a 512-d vision embedding using the CLIP image encoder (ViT-B/32); (2) store the embedding in a vector database (Pinecone, Weaviate, or FAISS per-user for privacy); (3) at query time, encode the text query "photos with cats" using the CLIP text encoder (same 512-d space); (4) approximate nearest neighbor search in the joint embedding space returns photos semantically similar to the query; (5) CLIP embeddings are generated as part of the async ML pipeline (after thumbnail generation). This enables zero-shot search: you can search for any concept without training a specific classifier for it. Label-based search (current architecture) requires pre-defined classes; CLIP search handles arbitrary natural language queries.

Q15: How would you ensure photo delivery quality during a CDN outage?
A15: Multi-CDN strategy with automatic failover: (1) primary CDN (Cloudflare); backup CDN (Fastly or CloudFront); (2) photo URLs are constructed with a CDN hostname abstraction: `photos.cdn.example.com` which resolves to the active CDN via DNS; (3) health check monitors CDN hit rate and error rate every 30 seconds; (4) on CDN failure: DNS failover to backup CDN (TTL = 60 seconds; failover completes within ~60s); (5) backup CDN warms its cache on demand (CDN misses go to origin; origin is capable of serving additional traffic during failover due to pre-scaling); (6) photo URLs remain the same (CDN hostname abstraction hides which CDN is serving); (7) cold start on backup CDN: first few minutes after failover have higher origin load (CDN cache empty); origin is pre-scaled 2× during any CDN incident to absorb the miss traffic.

---

## 12. References & Further Reading

- **Google Photos: Inside the Machine (Google I/O 2015)** — https://www.youtube.com/watch?v=zcBIDNZOFo4
- **FaceNet: A Unified Embedding for Face Recognition and Clustering** — Schroff, Kalenichenko, Philbin. CVPR 2015. https://arxiv.org/abs/1503.03832
- **MTCNN: Joint Face Detection and Alignment using Multi-task Cascaded CNNs** — Zhang et al. 2016. https://arxiv.org/abs/1604.02878
- **FAISS: A Library for Efficient Similarity Search** — Johnson, Douze, Jégou. 2017. https://arxiv.org/abs/1702.08734
- **CLIP: Learning Transferable Visual Models From Natural Language Supervision** — Radford et al. OpenAI 2021. https://arxiv.org/abs/2103.00020
- **Facebook's Photo Storage System (Haystack)** — Beaver et al. OSDI 2010. https://www.usenix.org/legacy/event/osdi10/tech/full_papers/Beaver.pdf
- **Instagram Engineering: What Powers Instagram's Image Storage** — https://instagram-engineering.com/
- **libvips Documentation and Performance Benchmarks** — https://libvips.github.io/libvips/
- **Cassandra: A Decentralized Structured Storage System** — Lakshman & Malik. SIGOPS 2010. https://dl.acm.org/doi/10.1145/1773912.1773922
- **Cassandra TimeWindowCompactionStrategy** — https://cassandra.apache.org/doc/latest/cassandra/operating/compaction/twcs.html
- **DBSCAN: A Density-Based Algorithm for Discovering Clusters in Large Spatial Databases** — Ester et al. KDD 1996. https://www.aaai.org/Papers/KDD/1996/KDD96-037.pdf
- **Progressive JPEG specification and libvips progressive encoding** — https://libvips.github.io/libvips/API/current/VipsImage.html
- **PhotoDNA (Microsoft perceptual hash for content detection)** — https://www.microsoft.com/en-us/photodna
- **tus.io Resumable Upload Protocol** — https://tus.io/protocols/resumable-upload.html
- **Designing Data-Intensive Applications** — Martin Kleppmann, O'Reilly, 2017. Chapters 3, 10 (batch processing).
