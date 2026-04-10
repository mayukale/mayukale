# System Design: Artifact Registry

> **Relevance to role:** A cloud infrastructure platform engineer operates the artifact registry that stores and distributes every Docker image, Maven JAR, Python wheel, Helm chart, and raw binary across the organization. This is the critical dependency between "build" and "deploy" -- if the registry is down, no deployments happen. Interviewers expect deep knowledge of the Docker Distribution spec (manifests, layers, content-addressable storage), layer deduplication, vulnerability scanning integration (Trivy), retention policies, geo-replication for multi-region deployments, pull-through caching for public registries, RBAC, and bandwidth optimization (lazy loading, on-demand layer fetch).

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|-------------|
| FR-1 | Store and serve Docker/OCI images: push, pull, list tags, delete. |
| FR-2 | Store and serve Maven/Gradle JARs, Python wheels, npm packages, Helm charts. |
| FR-3 | Content-addressable storage: artifacts stored by SHA-256 digest. |
| FR-4 | Layer deduplication: shared layers across images stored only once. |
| FR-5 | Vulnerability scanning: Trivy integration to scan every pushed image. |
| FR-6 | Retention policies: keep last N versions, keep tagged releases, expire old snapshots. |
| FR-7 | Geo-replication: replicate artifacts to multiple regions for local pull. |
| FR-8 | Pull-through cache: proxy public registries (Docker Hub, Maven Central, PyPI) with local caching. |
| FR-9 | RBAC: control who can push/pull which repositories. |
| FR-10 | Bandwidth optimization: support lazy loading, on-demand layer fetch (Stargz/Nydus). |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Image pull latency (within region) | < 5 s for 200 MB image |
| NFR-2 | Image push latency | < 30 s for 200 MB image |
| NFR-3 | Availability | 99.99% (52 min downtime/year) |
| NFR-4 | Durability | 99.999999999% (11 nines, S3-backed) |
| NFR-5 | Concurrent image pulls | 10,000+ |
| NFR-6 | Storage capacity | 500 TB+ (S3 unlimited) |
| NFR-7 | Vulnerability scan latency | < 5 min after push |

### Constraints & Assumptions
- S3 (or S3-compatible object storage, e.g., MinIO) is the blob storage backend.
- MySQL is the metadata database.
- 3 regions: US-East, EU-West, AP-Southeast.
- Registry is deployed as a Kubernetes service.
- Docker Distribution API v2 is the standard for Docker/OCI images.

### Out of Scope
- Client-side build tooling (Docker CLI, Maven settings.xml).
- OS package repositories (apt, yum).
- Large binary asset storage (Git LFS replacement).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Docker images pushed/day | 15,500 pipeline runs x 60% produce image | ~9,300 |
| Maven/PyPI packages pushed/day | 15,500 x 30% (Java + Python libs) | ~4,650 |
| Docker image pulls/day | 9,300 pushed + 3x pull factor (staging, prod, multiple clusters) | ~37,000 |
| Peak pulls/hour | 3x average | ~4,600 |
| Concurrent pulls (peak) | 4,600/hr, avg pull 3s | ~4 concurrent per second |
| Layer blobs served/day | 37,000 pulls x avg 5 layers | 185,000 blobs |
| Unique layers/day (after dedup) | ~9,300 images x 2 unique layers avg | ~18,600 |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Docker pull (200 MB image, 5 layers, cached in region) | < 5 s |
| Docker push (200 MB image, 2 new layers, 3 cached) | < 15 s |
| Maven dependency resolution (500 artifacts) | < 10 s |
| Tag listing (GET /v2/{repo}/tags/list) | < 500 ms |
| Vulnerability scan completion | < 5 min |
| Manifest HEAD request (check digest) | < 100 ms |

### Storage Estimates

| Item | Calculation | Value |
|------|-------------|-------|
| New image layers/day | 18,600 unique layers x 30 MB avg compressed | 558 GB/day |
| Monthly (before retention) | 558 x 30 | ~16.7 TB/month |
| After retention (keep last 20 tags + releases) | ~30% of total | ~50 TB steady-state |
| Maven/PyPI packages | 4,650/day x 10 MB avg | 46.5 GB/day |
| Maven retention (last 5 snapshots, all releases) | ~5 TB steady-state |
| Total storage | Images + packages + metadata | ~60 TB |
| S3 cost | 60 TB x $0.023/GB/month | ~$1,380/month |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| Push bandwidth | 9,300 images x 60 MB new content avg | ~558 GB/day (~6.5 MB/s avg) |
| Pull bandwidth (within region) | 37,000 x 200 MB x layer cache hit 70% | ~2.2 TB/day (~25 MB/s avg) |
| Cross-region replication | 558 GB/day x 2 replicas | ~1.1 TB/day |
| Peak pull bandwidth | 3x average | ~75 MB/s |
| S3 transfer cost (within region) | Free (VPC endpoint) | $0 |
| S3 cross-region transfer | 1.1 TB/day x $0.02/GB | ~$22/day = ~$660/month |

---

## 3. High Level Architecture

```
                    +-------------------+
                    | Docker / Maven /  |
                    | pip clients       |
                    +--------+----------+
                             |
                    Push/Pull (HTTPS)
                             |
                    +--------v----------+
                    | Load Balancer     |
                    | (L7, TLS term)    |
                    +--------+----------+
                             |
              +--------------+--------------+
              |                             |
     +--------v--------+           +--------v--------+
     | Registry API     |           | Package API     |
     | (Docker Distrib. |           | (Maven/PyPI/npm |
     |  v2 spec)        |           |  endpoints)     |
     +--------+---------+           +--------+--------+
              |                             |
     +--------v-----------------------------v--------+
     |              Metadata Service                 |
     |   (MySQL: manifests, tags, repos, RBAC)       |
     +--------+-------------------------------------+
              |
     +--------v---------+
     |    Blob Store     |     Content-addressable storage
     |    (S3 backend)   |     Key: sha256:<digest>
     +--------+----------+
              |
     +--------v---------+        +-------------------+
     |  Replication      |------->| S3 (EU-West)     |
     |  Controller       |------->| S3 (AP-SE)       |
     +--------+----------+        +-------------------+
              |
     +--------v---------+
     | Vulnerability     |
     | Scanner (Trivy)   |
     +--------+----------+
              |
     +--------v---------+
     | Retention         |
     | Manager (cron)    |
     +-------------------+

     +-------------------+
     | Pull-Through      |    Proxies Docker Hub, Maven Central, PyPI
     | Cache             |
     +-------------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **Load Balancer** | L7 load balancer (Envoy/Nginx). TLS termination. Route: `/v2/*` to Docker Registry API, `/maven/*` to Maven API, `/pypi/*` to PyPI API. |
| **Registry API** | Implements Docker Distribution HTTP API v2. Handles: manifest push/pull, layer upload/download, tag management, catalog listing. Stateless; scales horizontally. |
| **Package API** | Implements Maven Repository API, PyPI Simple API, npm Registry API, Helm Chart Repository API. Stateless. |
| **Metadata Service** | MySQL database storing: manifest metadata, tag-to-digest mappings, repository list, access control policies, scan results. |
| **Blob Store (S3)** | Content-addressable object storage. Key = `sha256/<first 2 chars>/<digest>`. Stores layer blobs, config blobs, and package files. Deduplication is inherent (same content = same key). |
| **Replication Controller** | Watches for new blobs in the primary region. Replicates to secondary regions (S3 cross-region copy). Ensures all regions have the same content. |
| **Vulnerability Scanner** | Trivy instance that scans every pushed image. Triggered by push webhook. Results stored in metadata DB and displayed in UI/API. |
| **Retention Manager** | Cron job that applies retention policies: delete old tags, remove untagged manifests, garbage collect unreferenced blobs. |
| **Pull-Through Cache** | Proxies public registries. On pull, if the image is cached locally, serve it. If not, fetch from upstream, cache locally, serve. Reduces external bandwidth and provides resilience against upstream outages. |

### Data Flows

**Docker push flow:**
```
1. Client: POST /v2/{repo}/blobs/uploads/       -> Get upload URL
2. Client: PUT  /v2/{repo}/blobs/uploads/{id}   -> Upload layer blob (chunked)
   -> Registry validates digest, writes to S3 at sha256/<digest>
   -> If blob already exists (dedup), upload is a no-op (201 OK)
3. Client: PUT  /v2/{repo}/manifests/{tag}       -> Upload manifest
   -> Registry validates manifest, stores in MySQL + S3
   -> Tag -> digest mapping written to MySQL
4. Registry: webhook -> Trivy: scan image sha256:<digest>
5. Replication Controller: replicate new blobs to EU-West, AP-SE
```

**Docker pull flow:**
```
1. Client: GET  /v2/{repo}/manifests/{tag}
   -> Registry resolves tag to digest in MySQL
   -> Returns manifest JSON (list of layers)
2. Client: GET  /v2/{repo}/blobs/sha256:<digest>  (for each layer)
   -> Registry serves blob from S3 (or redirects to S3 pre-signed URL)
3. Client assembles image from layers locally
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Repository (namespace for images/packages)
CREATE TABLE repositories (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    name            VARCHAR(512) NOT NULL,              -- e.g., "org/order-service"
    repo_type       ENUM('docker', 'maven', 'pypi', 'npm', 'helm', 'raw') NOT NULL,
    visibility      ENUM('public', 'internal', 'private') NOT NULL DEFAULT 'internal',
    owner_team      VARCHAR(255),
    description     TEXT,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_name_type (name, repo_type),
    INDEX idx_owner (owner_team)
) ENGINE=InnoDB;

-- Docker/OCI manifest (image metadata)
CREATE TABLE manifests (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    repository_id   BIGINT NOT NULL,
    digest          CHAR(71) NOT NULL,                  -- sha256:hex (64 chars + "sha256:" prefix)
    media_type      VARCHAR(255) NOT NULL,              -- e.g., "application/vnd.oci.image.manifest.v1+json"
    schema_version  INT NOT NULL DEFAULT 2,
    size_bytes      BIGINT NOT NULL,
    config_digest   CHAR(71),                           -- config blob digest
    layer_count     INT NOT NULL,
    total_size_bytes BIGINT NOT NULL,                   -- sum of all layers (compressed)
    pushed_by       VARCHAR(255),
    pushed_at       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    scan_status     ENUM('pending', 'scanning', 'clean', 'warning', 'critical') NOT NULL DEFAULT 'pending',
    scan_completed_at DATETIME,
    FOREIGN KEY (repository_id) REFERENCES repositories(id),
    UNIQUE KEY uk_repo_digest (repository_id, digest),
    INDEX idx_digest (digest),
    INDEX idx_scan_status (scan_status)
) ENGINE=InnoDB;

-- Tags (mutable pointers to manifests)
CREATE TABLE tags (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    repository_id   BIGINT NOT NULL,
    tag_name        VARCHAR(255) NOT NULL,              -- e.g., "v1.2.4", "latest", "sha-abc123"
    manifest_id     BIGINT NOT NULL,
    pushed_at       DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (repository_id) REFERENCES repositories(id),
    FOREIGN KEY (manifest_id) REFERENCES manifests(id),
    UNIQUE KEY uk_repo_tag (repository_id, tag_name),
    INDEX idx_manifest (manifest_id)
) ENGINE=InnoDB;

-- Layer blobs (content-addressable)
CREATE TABLE blobs (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    digest          CHAR(71) NOT NULL UNIQUE,           -- sha256:hex
    size_bytes      BIGINT NOT NULL,
    media_type      VARCHAR(255),
    storage_path    VARCHAR(512) NOT NULL,              -- S3 key
    reference_count INT NOT NULL DEFAULT 0,             -- number of manifests referencing this blob
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_accessed_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_digest (digest),
    INDEX idx_reference_count (reference_count),
    INDEX idx_last_accessed (last_accessed_at)
) ENGINE=InnoDB;

-- Manifest-to-blob mapping (which blobs belong to which manifest)
CREATE TABLE manifest_layers (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    manifest_id     BIGINT NOT NULL,
    blob_id         BIGINT NOT NULL,
    layer_index     INT NOT NULL,                       -- order in the manifest
    FOREIGN KEY (manifest_id) REFERENCES manifests(id) ON DELETE CASCADE,
    FOREIGN KEY (blob_id) REFERENCES blobs(id),
    UNIQUE KEY uk_manifest_layer (manifest_id, layer_index)
) ENGINE=InnoDB;

-- Vulnerability scan results
CREATE TABLE scan_results (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    manifest_id     BIGINT NOT NULL,
    scanner         VARCHAR(50) NOT NULL DEFAULT 'trivy',
    scanner_version VARCHAR(50) NOT NULL,
    total_vulns     INT NOT NULL DEFAULT 0,
    critical_count  INT NOT NULL DEFAULT 0,
    high_count      INT NOT NULL DEFAULT 0,
    medium_count    INT NOT NULL DEFAULT 0,
    low_count       INT NOT NULL DEFAULT 0,
    report_url      VARCHAR(1024),                      -- S3 URL for full JSON report
    scanned_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (manifest_id) REFERENCES manifests(id),
    INDEX idx_manifest (manifest_id)
) ENGINE=InnoDB;

-- RBAC access policies
CREATE TABLE access_policies (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    repository_pattern VARCHAR(512) NOT NULL,           -- glob, e.g., "org/order-*"
    principal       VARCHAR(255) NOT NULL,              -- user, team, or service account
    principal_type  ENUM('user', 'team', 'service_account', 'ci_pipeline') NOT NULL,
    actions         JSON NOT NULL,                      -- ["push", "pull", "delete", "admin"]
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_repo_pattern (repository_pattern),
    INDEX idx_principal (principal)
) ENGINE=InnoDB;

-- Retention policies
CREATE TABLE retention_policies (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    repository_pattern VARCHAR(512) NOT NULL,
    rule_type       ENUM('keep_last_n_tags', 'keep_tagged_releases', 'expire_untagged',
                         'expire_older_than', 'keep_regex_match') NOT NULL,
    rule_value      VARCHAR(255) NOT NULL,              -- e.g., "20" for last 20 tags, "90d" for 90 days
    enabled         BOOLEAN NOT NULL DEFAULT TRUE,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_repo (repository_pattern)
) ENGINE=InnoDB;

-- Replication status per blob per region
CREATE TABLE replication_status (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    blob_digest     CHAR(71) NOT NULL,
    target_region   VARCHAR(50) NOT NULL,               -- e.g., "eu-west-1"
    status          ENUM('pending', 'replicating', 'completed', 'failed') NOT NULL DEFAULT 'pending',
    started_at      DATETIME,
    completed_at    DATETIME,
    retry_count     INT NOT NULL DEFAULT 0,
    INDEX idx_digest_region (blob_digest, target_region),
    INDEX idx_status (status)
) ENGINE=InnoDB;

-- Maven package metadata
CREATE TABLE maven_artifacts (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    repository_id   BIGINT NOT NULL,
    group_id        VARCHAR(255) NOT NULL,              -- e.g., "com.company.order"
    artifact_id     VARCHAR(255) NOT NULL,              -- e.g., "order-service"
    version         VARCHAR(128) NOT NULL,              -- e.g., "1.2.4" or "1.2.5-SNAPSHOT"
    classifier      VARCHAR(50),                        -- e.g., "sources", "javadoc"
    packaging       VARCHAR(20) NOT NULL DEFAULT 'jar',
    blob_digest     CHAR(71) NOT NULL,
    pom_digest      CHAR(71),                           -- POM file digest
    size_bytes      BIGINT NOT NULL,
    is_snapshot     BOOLEAN NOT NULL DEFAULT FALSE,
    published_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (repository_id) REFERENCES repositories(id),
    UNIQUE KEY uk_maven_gav (group_id, artifact_id, version, classifier, packaging),
    INDEX idx_group_artifact (group_id, artifact_id)
) ENGINE=InnoDB;
```

### Database Selection

| Store | Engine | Rationale |
|-------|--------|-----------|
| Metadata (repos, manifests, tags, ACLs) | MySQL 8.0 | Transactional consistency for tag updates (tag is a mutable pointer); moderate write volume; relational model fits metadata well. |
| Blob storage | S3 | Content-addressable, infinite scale, 11-nines durability, cheap ($0.023/GB/month). |
| Scan results (searchable) | Elasticsearch | Full-text search across CVE descriptions; aggregation for vulnerability dashboards. |
| Cache (frequently accessed manifests) | Redis | Sub-ms manifest resolution. Cache tag -> digest mapping. |
| Replication queue | Kafka | Durable, ordered queue for cross-region replication events. Replay on failure. |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| manifests | (repository_id, digest) UNIQUE | Manifest lookup by digest (most common operation) |
| manifests | (digest) | Cross-repo digest lookup (dedup check) |
| manifests | (scan_status) | "Show all images with critical vulnerabilities" |
| tags | (repository_id, tag_name) UNIQUE | Tag resolution: "what's the digest for tag v1.2.4?" |
| blobs | (digest) UNIQUE | Content-addressable blob lookup |
| blobs | (reference_count) | Garbage collection: find unreferenced blobs |
| access_policies | (repository_pattern) | RBAC: "can user X push to repo Y?" |

---

## 5. API Design

### REST Endpoints

**Docker Distribution API v2 (standard):**
```
# Image manifests
GET    /v2/{repo}/manifests/{reference}          # Pull manifest (by tag or digest)
PUT    /v2/{repo}/manifests/{reference}          # Push manifest
DELETE /v2/{repo}/manifests/{reference}          # Delete manifest
HEAD   /v2/{repo}/manifests/{reference}          # Check manifest existence

# Blob (layer) operations
GET    /v2/{repo}/blobs/{digest}                 # Pull blob
HEAD   /v2/{repo}/blobs/{digest}                 # Check blob existence
DELETE /v2/{repo}/blobs/{digest}                 # Delete blob
POST   /v2/{repo}/blobs/uploads/                 # Start blob upload
PATCH  /v2/{repo}/blobs/uploads/{id}             # Upload blob chunk
PUT    /v2/{repo}/blobs/uploads/{id}?digest=...  # Complete blob upload

# Tags
GET    /v2/{repo}/tags/list                      # List tags

# Catalog
GET    /v2/_catalog                              # List repositories
```

**Extended Registry Management API:**
```
# Repository management
POST   /api/v1/repositories                      # Create repository
GET    /api/v1/repositories                       # List repositories
GET    /api/v1/repositories/{name}                # Get repository details
DELETE /api/v1/repositories/{name}                # Delete repository

# Vulnerability scanning
GET    /api/v1/repositories/{name}/manifests/{digest}/scan  # Get scan results
POST   /api/v1/repositories/{name}/manifests/{digest}/rescan # Trigger rescan

# Retention
POST   /api/v1/repositories/{name}/retention       # Set retention policy
GET    /api/v1/repositories/{name}/retention        # Get retention policy
POST   /api/v1/retention/run                        # Trigger retention cleanup (manual)

# Replication
GET    /api/v1/replication/status                   # Get replication status
POST   /api/v1/replication/trigger                   # Force replication

# RBAC
POST   /api/v1/access-policies                      # Create access policy
GET    /api/v1/access-policies?repo={pattern}        # List policies for repo
DELETE /api/v1/access-policies/{id}                  # Delete policy

# Analytics
GET    /api/v1/analytics/storage                     # Storage usage by repo
GET    /api/v1/analytics/bandwidth                   # Pull/push bandwidth
GET    /api/v1/analytics/popular-images               # Most-pulled images
```

**Maven Repository API (Nexus-compatible):**
```
GET    /maven/{repo}/{group/path}/{artifact}/{version}/{file}    # Download artifact
PUT    /maven/{repo}/{group/path}/{artifact}/{version}/{file}    # Upload artifact
GET    /maven/{repo}/{group/path}/{artifact}/maven-metadata.xml  # Get version list
```

### CLI

```bash
# Docker operations (standard Docker CLI)
docker push registry.internal/org/order-svc:v1.2.4
docker pull registry.internal/org/order-svc:v1.2.4

# Registry management CLI
regctl repo list                                    # List repositories
regctl repo info org/order-svc                      # Repository details
regctl tag list org/order-svc                       # List tags
regctl tag delete org/order-svc:old-tag             # Delete a tag

# Vulnerability scanning
regctl scan org/order-svc:v1.2.4                    # View scan results
regctl scan org/order-svc:v1.2.4 --rescan           # Trigger rescan
regctl scan list --severity critical                 # List all critical vulns

# Retention
regctl retention set org/order-svc --keep-last 20 --keep-releases --expire-untagged 30d
regctl retention run --dry-run                       # Preview what would be deleted
regctl retention run                                 # Execute retention

# Replication
regctl replication status                            # Check replication lag
regctl replication trigger org/order-svc:v1.2.4      # Force replicate specific image

# Storage analytics
regctl storage usage --top 20                        # Top 20 repos by storage
regctl storage dedup-savings                         # Layer dedup effectiveness
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Content-Addressable Storage and Layer Deduplication

**Why it's hard:**
A typical organization has 1,500 services, each producing Docker images multiple times per day. Most images share common base layers (e.g., `eclipse-temurin:21-jre` is 200 MB and used by 80% of Java services). Without deduplication, storage grows linearly with image count. With deduplication, shared layers are stored once, but the system must efficiently track references, handle concurrent uploads of the same layer, and safely garbage collect unreferenced blobs.

**Approaches:**

| Approach | Dedup Rate | Complexity | Performance |
|----------|-----------|------------|-------------|
| **Store each image as a complete tarball** | 0% | Low | Slow (large files) |
| **Layer-level dedup (Docker Distribution)** | 60-80% | Medium | Good |
| **Chunk-level dedup (sub-layer, e.g., Casync)** | 85-95% | High | Complex |
| **Content-addressable with reference counting** | 60-80% | Medium | Good |

**Selected approach: Layer-level deduplication with content-addressable storage (Docker Distribution standard)**

**Justification:** The Docker Distribution specification already defines content-addressable storage by layer digest. Each layer blob is stored once in S3 at its SHA-256 digest key. Manifests reference layers by digest. This gives 60-80% dedup for typical organizations where most services share base images. Chunk-level dedup provides marginal improvement at much higher complexity.

**Implementation detail:**

```
S3 bucket structure:
s3://registry-blobs/
  sha256/
    ab/
      abcdef1234567890...   (layer blob, ~50 MB)
    cd/
      cdef5678abcd1234...   (config blob, ~5 KB)
    ef/
      efgh9012abcd5678...   (manifest blob, ~2 KB)

Two-char prefix directory prevents S3 listing bottleneck (max 1000 objects per prefix).
```

**Blob upload with dedup check:**

```python
# Registry API: blob upload handler

class BlobUploadHandler:
    def __init__(self, s3_client, db):
        self.s3 = s3_client
        self.db = db
    
    def complete_upload(self, repo: str, upload_id: str, expected_digest: str, data: bytes):
        """
        Complete a blob upload. If the blob already exists (by digest), skip the S3 write.
        """
        # Compute actual digest
        actual_digest = "sha256:" + hashlib.sha256(data).hexdigest()
        
        if actual_digest != expected_digest:
            raise ValueError(f"Digest mismatch: expected {expected_digest}, got {actual_digest}")
        
        # Check if blob already exists (deduplication)
        existing = self.db.query(
            "SELECT id FROM blobs WHERE digest = %s", (actual_digest,)
        )
        
        if existing:
            # Blob already exists; increment reference count
            self.db.execute(
                "UPDATE blobs SET reference_count = reference_count + 1, "
                "last_accessed_at = NOW() WHERE digest = %s",
                (actual_digest,)
            )
            return {"digest": actual_digest, "deduplicated": True}
        
        # New blob: write to S3
        s3_key = f"sha256/{actual_digest[7:9]}/{actual_digest[7:]}"
        self.s3.put_object(
            Bucket="registry-blobs",
            Key=s3_key,
            Body=data,
            ContentType="application/octet-stream",
            ContentLength=len(data),
            # S3 server-side checksum
            ChecksumAlgorithm="SHA256",
        )
        
        # Record in metadata DB
        self.db.execute(
            "INSERT INTO blobs (digest, size_bytes, storage_path, reference_count) "
            "VALUES (%s, %s, %s, 1)",
            (actual_digest, len(data), s3_key)
        )
        
        # Trigger replication
        self.replication_queue.publish({
            "type": "new_blob",
            "digest": actual_digest,
            "s3_key": s3_key,
            "size_bytes": len(data)
        })
        
        return {"digest": actual_digest, "deduplicated": False}
```

**Deduplication effectiveness:**

```
Example: 1,500 Java services, each with Docker image

Typical Java service image layers:
  Layer 1: eclipse-temurin:21-jre base    ~200 MB  (shared by 1,200 services)
  Layer 2: OS packages (apt-get)          ~50 MB   (shared by ~500 services)
  Layer 3: Application dependencies       ~80 MB   (shared by ~50 services)
  Layer 4: Application JAR                ~30 MB   (unique per service per version)

Without dedup: 1,500 x (200+50+80+30) = 540 GB per image set
With dedup:    200 + 50*3 + 80*30 + 30*1,500 = 200 + 150 + 2,400 + 45,000 = ~47.8 GB

Dedup savings: ~91% for a single version
Over 20 tags (versions), unique layers grow but base layers are still shared: ~65% savings.
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| S3 write fails mid-upload | Incomplete blob | Upload is atomic (S3 PutObject); on failure, no blob is stored. Client retries. |
| Concurrent uploads of same blob | Both try to write to same S3 key | S3 PutObject is idempotent (same content = same key = last write wins, all identical). MySQL INSERT uses `ON DUPLICATE KEY UPDATE reference_count = reference_count + 1`. |
| Digest mismatch (data corruption) | Wrong content served | SHA-256 verified on upload and on pull. S3 checksums provide additional integrity. |
| Orphaned blobs (referenced by no manifest) | Wasted storage | Garbage collector (see Retention Manager) periodically scans for blobs with `reference_count = 0` and deletes after a grace period. |

**Interviewer Q&As:**

**Q1: How does content-addressable storage work in the Docker Registry?**
A: Every blob (layer, config, manifest) is stored at a key derived from its SHA-256 hash. When pushing, the client computes the digest and sends it with the blob. The registry verifies the digest matches the content and stores it. On pull, the client requests the blob by digest and verifies the content. This makes blobs immutable (changing content changes the digest) and enables deduplication (same content = same digest = stored once).

**Q2: What happens if two services push the same base image layer simultaneously?**
A: Both uploads compute the same SHA-256 digest. Both try to write to the same S3 key. S3 PutObject is idempotent -- the second write overwrites the first with identical content. In MySQL, we use `INSERT ... ON DUPLICATE KEY UPDATE` to increment the reference count. The race is benign because the content is identical.

**Q3: How do you handle blob deletion safely (garbage collection)?**
A: We never delete blobs immediately. Garbage collection runs in two phases: (1) Mark: find blobs with `reference_count = 0` (no manifest references them). (2) Sweep: after a 7-day grace period (to handle in-flight pushes), delete from S3 and MySQL. The grace period prevents deleting a blob that's part of an in-progress multi-layer push.

**Q4: How does Docker layer caching reduce build time?**
A: When BuildKit builds an image, it pushes each layer to the registry. If a layer already exists (same digest), the push is a no-op (registry returns 201 immediately). On the next build, BuildKit checks which layers already exist in the registry and skips re-uploading them. For a Spring Boot app, the dependency layer (~80 MB) changes only when pom.xml changes, so it's cached across builds.

**Q5: How do you handle multi-architecture images?**
A: Multi-arch images use an OCI Image Index (formerly Docker Manifest List). The Index is a manifest that points to per-architecture manifests (amd64, arm64). Each architecture manifest points to its own layers. The client's Docker daemon sends its platform (e.g., `linux/amd64`) and the registry resolves the correct manifest. Storage-wise, common layers across architectures are deduplicated.

**Q6: What's the storage overhead of keeping 20 tags per service?**
A: If 20 tags represent 20 versions of a service, and each version changes only the app JAR layer (~30 MB): 20 versions x 30 MB unique layer = 600 MB per service. 1,500 services x 600 MB = 900 GB. The base layers are shared across all versions and services (stored once). Total: ~900 GB unique + ~2 GB shared = ~902 GB. Without dedup, it would be 1,500 x 20 x 360 MB = 10.8 TB.

---

### Deep Dive 2: Geo-Replication

**Why it's hard:**
Deploying to clusters in US-East, EU-West, and AP-Southeast requires pulling images from a local registry (cross-region S3 pull adds 200-500ms per layer). Replication must be fast (image available in all regions within minutes of push), consistent (all regions serve the same digest), and efficient (don't replicate unchanged layers).

**Approaches:**

| Approach | Latency | Consistency | Bandwidth |
|----------|---------|-------------|-----------|
| **Single registry, pull cross-region** | High (200-500ms/layer x-region) | Perfect | High x-region |
| **S3 Cross-Region Replication (CRR)** | ~15 min (async) | Eventually consistent | Efficient (S3-managed) |
| **Active replication (push-based)** | < 5 min | Eventually consistent | Controlled |
| **Pull-through cache per region** | First-pull slow, then cached | Eventually consistent | Bandwidth on-demand |
| **Active replication + pull-through fallback** | < 5 min (replicated), first-pull (fallback) | Eventually consistent | Best mix |

**Selected approach: Active push-based replication + pull-through fallback**

**Justification:** Active replication ensures all artifacts pushed by CI/CD (which needs them for deployment) are available in all regions within minutes. Pull-through fallback handles edge cases (image referenced before replication completes, images from public registries). S3 CRR is too slow (15 min) for deployment pipelines.

**Implementation detail:**

```python
# Replication Controller

class ReplicationController:
    def __init__(self, s3_clients: dict, kafka_consumer, db):
        """
        s3_clients: {"us-east-1": s3_client, "eu-west-1": s3_client, ...}
        """
        self.s3_clients = s3_clients
        self.kafka = kafka_consumer
        self.db = db
        self.primary_region = "us-east-1"
        self.replica_regions = ["eu-west-1", "ap-southeast-1"]
    
    def run(self):
        """Consume new_blob events from Kafka and replicate."""
        for event in self.kafka.consume("registry.new_blob"):
            self.replicate_blob(event)
    
    def replicate_blob(self, event: dict):
        digest = event["digest"]
        s3_key = event["s3_key"]
        size = event["size_bytes"]
        
        for region in self.replica_regions:
            # Check if already replicated (idempotent)
            status = self.db.get_replication_status(digest, region)
            if status == "completed":
                continue
            
            try:
                self.db.set_replication_status(digest, region, "replicating")
                
                # Copy from primary to replica region
                source_s3 = self.s3_clients[self.primary_region]
                target_s3 = self.s3_clients[region]
                
                # For large blobs (> 100 MB), use multipart copy
                if size > 100 * 1024 * 1024:
                    self._multipart_copy(source_s3, target_s3, s3_key, size)
                else:
                    # Direct copy
                    obj = source_s3.get_object(Bucket="registry-blobs", Key=s3_key)
                    target_s3.put_object(
                        Bucket="registry-blobs",
                        Key=s3_key,
                        Body=obj["Body"].read(),
                    )
                
                self.db.set_replication_status(digest, region, "completed")
                
            except Exception as e:
                self.db.set_replication_status(digest, region, "failed", retry_count_incr=True)
                log.error(f"Replication failed for {digest} to {region}: {e}")
                # Will be retried by the retry loop
    
    def _multipart_copy(self, source_s3, target_s3, s3_key, size):
        """Multipart copy for large blobs."""
        part_size = 100 * 1024 * 1024  # 100 MB parts
        upload = target_s3.create_multipart_upload(Bucket="registry-blobs", Key=s3_key)
        parts = []
        
        for i, offset in enumerate(range(0, size, part_size)):
            end = min(offset + part_size - 1, size - 1)
            part = source_s3.get_object(
                Bucket="registry-blobs", Key=s3_key,
                Range=f"bytes={offset}-{end}"
            )
            resp = target_s3.upload_part(
                Bucket="registry-blobs", Key=s3_key,
                UploadId=upload["UploadId"],
                PartNumber=i + 1,
                Body=part["Body"].read()
            )
            parts.append({"PartNumber": i + 1, "ETag": resp["ETag"]})
        
        target_s3.complete_multipart_upload(
            Bucket="registry-blobs", Key=s3_key,
            UploadId=upload["UploadId"],
            MultipartUpload={"Parts": parts}
        )
```

**Pull-through fallback (for unreplicated images):**

```python
# Pull-through fallback in Registry API

class RegistryPullHandler:
    def get_blob(self, repo: str, digest: str):
        local_region = get_current_region()  # e.g., "eu-west-1"
        
        # Try local S3 first
        try:
            return self.s3_clients[local_region].get_object(
                Bucket="registry-blobs",
                Key=f"sha256/{digest[7:9]}/{digest[7:]}"
            )
        except self.s3_clients[local_region].exceptions.NoSuchKey:
            pass
        
        # Fallback: pull from primary region
        log.warn(f"Blob {digest} not in {local_region}, pulling from primary")
        try:
            obj = self.s3_clients["us-east-1"].get_object(
                Bucket="registry-blobs",
                Key=f"sha256/{digest[7:9]}/{digest[7:]}"
            )
            data = obj["Body"].read()
            
            # Cache locally for future pulls (async)
            self._cache_locally(local_region, digest, data)
            
            return data
        except Exception:
            raise NotFoundException(f"Blob {digest} not found in any region")
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Replication lag > 5 min | Deploy in remote region must wait or pull cross-region | Pull-through fallback: remote region pulls from primary and caches |
| Primary region S3 outage | No new pushes; replicas still serve existing images | Promote a replica to primary (DNS failover). Pushes buffer in Kafka until primary recovers. |
| Kafka lag on replication events | Replication delayed | Monitor consumer lag. Scale replication workers. |
| Inconsistent state (manifest replicated but layers not yet) | Pull fails in remote region (manifest references missing layers) | Replicate layers before manifests (order guarantee in Kafka). Pull-through fallback for missing layers. |

**Interviewer Q&As:**

**Q1: How do you ensure a manifest is pullable in a remote region?**
A: We replicate in order: layers first, then manifest. The Kafka event for a push includes the manifest digest and all referenced layer digests. The replication controller processes layers first, then the manifest. A manifest is only marked as "replicated" after all its layers are confirmed present in the target region.

**Q2: What's the replication bandwidth cost?**
A: With deduplication, only new unique layers are replicated. ~18,600 new layers/day x 30 MB avg x 2 regions = ~1.1 TB/day cross-region. At AWS cross-region transfer pricing ($0.02/GB), that's ~$22/day = ~$660/month. This is the dominant cost of the registry.

**Q3: How do you handle a region failover?**
A: All regions have a read-write registry. Normally, pushes go to the primary (US-East). On primary outage, DNS failover routes pushes to EU-West (promoted to primary). After the original primary recovers, we do a reconciliation: replicate any blobs pushed to the temporary primary back to the original primary.

**Q4: Why not just use S3 Cross-Region Replication?**
A: S3 CRR has 15-minute SLA for replication, which is too slow for CI/CD pipelines (build in US-East, deploy to EU-West within 5 minutes). Active push-based replication gives us < 5-minute latency. S3 CRR is useful as a backup mechanism (belt and suspenders) but not as the primary replication path.

**Q5: How do you handle replication for pull-through cached images (from Docker Hub)?**
A: Pull-through cached images (e.g., `docker.io/library/nginx`) are replicated on first pull. When a user in US-East pulls nginx, it's cached in US-East's S3. The replication controller replicates it to other regions (since it's now in our S3). Subsequent pulls in EU-West serve from local cache.

**Q6: How do you monitor replication health?**
A: Key metrics: replication lag (time from push to replicated in all regions), replication failure rate, cross-region bandwidth usage. Alert if replication lag > 10 minutes (images may not be available for deployment). Dashboard shows per-region blob count (should be equal, within replication lag).

---

### Deep Dive 3: Vulnerability Scanning and Policy Enforcement

**Why it's hard:**
Every Docker image must be scanned for CVEs before it's promoted to production. With 9,300 images pushed per day, scanning must be fast, accurate, and integrated into the CI/CD pipeline. The scanner database must be updated frequently (new CVEs daily). Policy enforcement (block images with critical CVEs from production) must be robust.

**Implementation detail:**

```python
# Vulnerability Scanner Service

class VulnerabilityScanner:
    def __init__(self, trivy_binary: str, db, notification_service):
        self.trivy = trivy_binary
        self.db = db
        self.notifications = notification_service
    
    def scan_image(self, image_ref: str, manifest_id: int):
        """
        Scan a Docker image using Trivy. Called asynchronously after push.
        """
        self.db.update_scan_status(manifest_id, "scanning")
        
        try:
            # Run Trivy scan
            result = subprocess.run(
                [
                    self.trivy, "image",
                    "--severity", "CRITICAL,HIGH,MEDIUM,LOW",
                    "--format", "json",
                    "--output", f"/tmp/scan-{manifest_id}.json",
                    "--db-repository", "registry.internal/trivy-db",  # private mirror
                    "--skip-update",  # use pre-pulled DB
                    image_ref
                ],
                capture_output=True,
                timeout=300  # 5 min timeout
            )
            
            # Parse results
            with open(f"/tmp/scan-{manifest_id}.json") as f:
                scan_data = json.load(f)
            
            vuln_counts = self._count_vulnerabilities(scan_data)
            
            # Store results
            scan_status = self._determine_status(vuln_counts)
            self.db.insert_scan_result(
                manifest_id=manifest_id,
                scanner="trivy",
                scanner_version=self._get_trivy_version(),
                critical=vuln_counts["critical"],
                high=vuln_counts["high"],
                medium=vuln_counts["medium"],
                low=vuln_counts["low"],
                report_url=self._upload_report(f"/tmp/scan-{manifest_id}.json")
            )
            self.db.update_scan_status(manifest_id, scan_status)
            
            # Notify on critical findings
            if vuln_counts["critical"] > 0:
                self.notifications.send(
                    channel="security-alerts",
                    message=f"CRITICAL vulnerabilities found in {image_ref}: "
                            f"{vuln_counts['critical']} critical, {vuln_counts['high']} high"
                )
            
        except subprocess.TimeoutExpired:
            self.db.update_scan_status(manifest_id, "pending")  # retry later
            log.error(f"Scan timed out for {image_ref}")
        except Exception as e:
            self.db.update_scan_status(manifest_id, "pending")
            log.error(f"Scan failed for {image_ref}: {e}")
    
    def _determine_status(self, counts):
        if counts["critical"] > 0:
            return "critical"
        elif counts["high"] > 0:
            return "warning"
        else:
            return "clean"
```

**Policy enforcement (Kubernetes admission controller):**

```yaml
# Kyverno policy: block unscanned or critical images
apiVersion: kyverno.io/v1
kind: ClusterPolicy
metadata:
  name: require-scan-clean
spec:
  validationFailureAction: enforce
  rules:
    - name: check-image-scan
      match:
        resources:
          kinds:
            - Pod
          namespaces:
            - production
      validate:
        message: "Image must be scanned and have no critical vulnerabilities"
        deny:
          conditions:
            any:
              - key: "{{request.object.spec.containers[].image}}"
                operator: AnyNotIn
                value: "{{lookup('registry-api', '/api/v1/scan-status', 'clean,warning')}}"
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Trivy DB outdated | New CVEs not detected | Daily DB update via cron. Mirror Trivy DB to internal registry. Alert if DB > 24h old. |
| Scan queue backlog | Images deployed before scan completes | Admission controller rejects images with `scan_status=pending`. CI pipeline waits for scan before promotion. |
| False positive (CVE doesn't apply) | Blocks a valid deployment | Allowlist for known false positives (per-repo, requires security team approval). |
| Scanner OOM on large image | Scan fails | Retry on larger instance. Timeout prevents blocking other scans. |

**Interviewer Q&As:**

**Q1: How do you handle scanning 9,300 images per day?**
A: Scanning runs as a pool of Trivy workers (10 pods, each processing ~1,000 images/day). Each scan takes ~30 seconds for a 200 MB image. 10 workers x 2,880 scans/day (at 30s each) = 28,800 scans/day capacity. We have 3x headroom. Workers consume from a Kafka queue and scale horizontally.

**Q2: How do you handle CVEs in base images that you don't control?**
A: We maintain a curated set of base images (golden images) that are scanned and rebuilt weekly. Services must use these golden images (enforced by a Dockerfile linter in CI). When a critical CVE is found in a base image, we rebuild all golden images and trigger a rebuild of all dependent services (via CI pipeline).

**Q3: How do you handle the lag between a CVE being published and Trivy detecting it?**
A: Trivy DB is updated every 6 hours from our mirror. For zero-day CVEs, the security team can add custom signatures. We also run periodic re-scans of all images in production (weekly) to catch newly discovered CVEs in already-deployed images.

**Q4: How does policy enforcement work without blocking every deployment?**
A: Policy tiers: (1) Critical CVEs in production: blocked (hard fail). (2) High CVEs in production: warning (soft fail, requires security team approval for exception). (3) Medium/Low: informational (shown in dashboard, not blocking). (4) Development/staging: no blocking (scan results are informational only).

**Q5: How do you avoid scanning the same image twice?**
A: Scans are keyed by manifest digest. If a tag is re-pushed with the same digest (no change), the existing scan result is reused. If a tag points to a new digest, a new scan is triggered. Since digests are content-addressed, this dedup is exact.

**Q6: How do you handle scanning for non-Docker artifacts (Maven JARs)?**
A: Trivy supports scanning filesystems and SBOMs. For Maven JARs, we extract the SBOM (list of dependencies) and scan with `trivy sbom`. For Python, we scan `requirements.txt`. The scan results are stored in the same `scan_results` table with the appropriate `manifest_id` replaced by `maven_artifact_id`.

---

## 7. Scheduling & Resource Management

### Registry Resource Requirements

| Component | Resources | Instances | Scaling |
|-----------|-----------|-----------|---------|
| Registry API | 4 CPU, 8 Gi memory | 5 (per region) | HPA on CPU |
| Package API | 2 CPU, 4 Gi memory | 3 | HPA on CPU |
| Metadata DB (MySQL) | 8 CPU, 32 Gi memory | 1 primary + 2 replicas (per region) | Vertical scale |
| Redis cache | 2 CPU, 8 Gi memory | 3 (Sentinel) | Vertical scale |
| Blob store (S3) | N/A (managed) | N/A | Infinite |
| Trivy scanner | 4 CPU, 8 Gi memory | 10 workers | Scale with queue depth |
| Replication controller | 2 CPU, 4 Gi memory | 3 | Scale with blob event rate |
| Retention manager | 2 CPU, 4 Gi memory | 1 (cron) | Batch processing |

### Pull Optimization

- **Registry API S3 redirect:** For blob pulls, the Registry API returns an HTTP 302 redirect to a pre-signed S3 URL. The client downloads directly from S3, bypassing the Registry API pod. This offloads blob transfer bandwidth to S3 (which scales infinitely).
- **Node-level image cache:** Kubernetes nodes cache pulled layers on local disk. Second pull of the same image is instant (no registry hit). Cache eviction is by LRU when disk > 80%.
- **Image pre-pull DaemonSet:** For frequently deployed images, a DaemonSet pre-pulls the latest version to all nodes during off-peak hours.

---

## 8. Scaling Strategy

| Challenge | Solution |
|-----------|----------|
| 10,000 concurrent image pulls | S3 redirect: Registry API returns S3 pre-signed URL, clients download from S3 directly. S3 handles unlimited concurrent downloads. |
| 100,000 images in a repository | Tag listing uses pagination (cursor-based). MySQL index on (repository_id, tag_name). |
| 500 TB storage | S3 handles unlimited storage. Lifecycle policies move infrequently accessed blobs to S3 Infrequent Access ($0.0125/GB/month, 46% savings). |
| Metadata DB bottleneck | Read replicas for pull (manifest resolution is a read). Writes (push) are less frequent (9,300/day). Redis cache for hot tags. |
| Trivy scanning backlog | Scale Trivy workers horizontally. Use layer-level caching: if a layer was scanned in a previous image, reuse the CVE results. |

### Interviewer Q&As

**Q1: How do you handle a deployment that pulls the same image to 1,000 pods simultaneously?**
A: (1) S3 redirect means 1,000 clients download from S3 directly (no registry bottleneck). (2) Kubernetes node-level caching: if multiple pods on the same node pull the same image, it's downloaded once. With 1,000 pods on 100 nodes, it's ~100 S3 downloads (10 pods/node, first pull caches for others). (3) Registry API only serves manifest (small JSON); blob downloads go to S3.

**Q2: What happens when Docker Hub rate-limits you (pull rate limit)?**
A: Pull-through cache. All public image references in Dockerfiles use our registry as a mirror: `FROM registry.internal/dockerhub/library/nginx:latest`. First pull fetches from Docker Hub and caches. Subsequent pulls serve from our cache. We never exceed Docker Hub rate limits because only the pull-through cache talks to Docker Hub.

**Q3: How do you handle storage costs growing to 500 TB?**
A: (1) Retention policies delete old tags (keep last 20, keep releases). (2) S3 Infrequent Access for blobs not accessed in 90 days (46% savings). (3) Layer deduplication (60-80% savings). (4) Garbage collection removes unreferenced blobs weekly. Expected effective cost: ~$6,000/month for 500 TB (after tiering and dedup).

**Q4: How does the registry handle a spike in pushes (CI/CD burst)?**
A: Pushes are mostly S3 writes (blob upload), which S3 handles at any scale. The bottleneck is MySQL for manifest metadata writes, which at 9,300 writes/day is trivial. Redis absorbs tag cache updates. A burst of 100 pushes/minute is easily handled.

**Q5: How do you handle registry upgrades without downtime?**
A: Rolling update of the Registry API pods. Since the API is stateless (all state in MySQL/S3/Redis), old pods drain while new pods start. The Load Balancer health check ensures traffic only goes to healthy pods. Clients that hit a draining pod get a retry signal (HTTP 503 with Retry-After header).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Impact | Detection | Mitigation | Recovery Time |
|---|---------|--------|-----------|------------|---------------|
| 1 | S3 outage (primary region) | Can't push; can't pull (new images) | S3 error rate metric | Redirect pulls to replica region (DNS failover). Buffer pushes in Kafka. | Depends on S3 (SLA: 99.99%) |
| 2 | MySQL primary down | Can't push (metadata writes fail); pulls from cache work | MySQL health check | Automated failover to replica. Pulls use Redis cache for tag resolution. | < 30 s (failover) |
| 3 | Redis down | Tag cache misses -> MySQL direct queries | Redis Sentinel health | MySQL serves all reads (slightly higher latency). Redis Sentinel auto-failover. | < 30 s |
| 4 | Registry API pod crash | Reduced capacity | Pod health check; HPA | LB routes to healthy pods. HPA scales up replacements. | < 30 s |
| 5 | Replication controller down | Replica regions have stale images | Consumer lag metric | Pull-through fallback: remote regions pull from primary. Replication catches up on recovery. | < 1 min (restart) |
| 6 | Trivy scanner down | New images not scanned | Scan queue growing | Images still usable (scan_status=pending). Deploy to staging allowed (scan not required for non-prod). Prod deploy blocked until scan completes. | < 5 min (restart) |
| 7 | Storage corruption (bit rot) | Incorrect image served | S3 checksums detect corruption | S3 provides durability (11 nines). Corrupted blob is re-fetched from another region's replica. | Automatic (S3 self-healing) |
| 8 | Retention policy misconfigured (too aggressive) | Important images deleted | Deleted image count spike alert | Soft delete with 7-day recovery window. Blob GC grace period. Audit log shows what was deleted and why. | < 1 min (restore from soft delete) |
| 9 | DDoS on registry | Registry overwhelmed | Rate limit alerts | Rate limiting per-client-IP. S3 redirect offloads bandwidth. CDN (CloudFront) absorbs DDoS for pull traffic. | Real-time (rate limiting) |
| 10 | Certificate expiry | All clients fail TLS | cert-manager monitoring, 14-day alert | cert-manager auto-renewal. Manual renewal fallback. | 5 min (manual cert apply) |

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold | Source |
|--------|------|-----------------|--------|
| `registry_pull_duration_seconds` | Histogram | p95 > 10 s | Registry API |
| `registry_push_duration_seconds` | Histogram | p95 > 30 s | Registry API |
| `registry_pull_total` | Counter by (repo, status) | error_rate > 1% | Registry API |
| `registry_push_total` | Counter by (repo, status) | error_rate > 5% | Registry API |
| `blob_dedup_ratio` | Gauge | < 0.4 (poor dedup) | Registry API |
| `storage_total_bytes` | Gauge | > 400 TB (capacity planning) | S3 metrics |
| `replication_lag_seconds` | Histogram | p95 > 300 s (5 min) | Replication Controller |
| `replication_failed_count` | Counter | > 100/hour | Replication Controller |
| `scan_queue_depth` | Gauge | > 1,000 (backlog) | Trivy Scanner |
| `scan_duration_seconds` | Histogram | p95 > 300 s | Trivy Scanner |
| `scan_critical_count` | Counter by repo | > 0 (alert security) | Trivy Scanner |
| `retention_deleted_blobs` | Counter | spike > 10,000 (misconfiguration?) | Retention Manager |
| `pull_through_cache_hit_rate` | Gauge | < 70% (cache ineffective) | Pull-Through Cache |

### Dashboards
1. **Registry Health:** Push/pull success rate, latency, throughput, error rate.
2. **Storage:** Total storage, growth rate, dedup ratio, per-repo usage (top 20), retention effectiveness.
3. **Security:** Scan coverage (% of images scanned), critical/high vulnerability count, top vulnerable images.
4. **Replication:** Per-region lag, replication throughput, failure rate.
5. **Client Analytics:** Most-pulled images, busiest repos, pull patterns by time of day.

---

## 11. Security

| Control | Implementation |
|---------|---------------|
| Authentication | All push/pull requires authentication. Token-based (JWT) via Docker token auth protocol. Tokens issued by an OAuth2 server (integrated with corporate SSO). |
| RBAC | Per-repository access control. Teams can push to their repos only. All authenticated users can pull internal repos. Private repos require explicit access grant. |
| Image signing | All CI-built images signed with cosign. Kubernetes admission controller (Kyverno) enforces: only signed images can be deployed to production. |
| Supply chain | SLSA provenance attestation stored alongside images (as OCI referrers). Build metadata: who built, from which commit, on which builder. |
| Encryption at rest | S3 server-side encryption (SSE-S3 or SSE-KMS). MySQL TDE. |
| Encryption in transit | TLS 1.3 for all API connections. S3 VPC endpoints for in-region traffic. |
| Audit logging | All push, pull, and delete operations logged with user identity, source IP, image reference, timestamp. Stored in Elasticsearch (90-day retention). |
| Network isolation | Registry API accessible only from corporate network (VPN or VPC). No public internet access. Pull-through cache is the only component that reaches public internet. |
| Rate limiting | Per-user: 100 pulls/min, 50 pushes/min. Prevents abuse and protects S3 request budget. |
| Container scanning | Trivy scans every image on push. Critical CVEs block production deployment. Integration with security team's vulnerability management workflow. |

---

## 12. Incremental Rollout Strategy

### Rolling Out the Artifact Registry

**Phase 1: Docker registry only (Week 1-2)**
- Deploy Docker Distribution v2 API with S3 backend.
- Migrate Docker Hub mirror to pull-through cache.
- 5 services push to new registry.

**Phase 2: Scanning integration (Week 3-4)**
- Integrate Trivy scanning pipeline.
- Run in audit mode (scan and report, don't block).
- Tune scan policies based on results.

**Phase 3: Maven/PyPI (Week 5-6)**
- Deploy package API endpoints.
- Migrate Maven settings.xml to point to new registry.
- Configure Gradle/Maven proxy settings.

**Phase 4: Geo-replication (Week 7-8)**
- Set up S3 buckets in EU-West and AP-SE.
- Deploy replication controller.
- Verify pull latency in remote regions.

**Phase 5: Full production (Week 9-12)**
- All CI/CD pipelines push to new registry.
- RBAC enforced.
- Retention policies active.
- Old registry decommissioned after 30-day overlap.

### Rollout Q&As

**Q1: How do you migrate from an existing registry (e.g., Docker Hub, Nexus) without downtime?**
A: Dual-push during migration: CI pipeline pushes to both old and new registry. Deploy systems pull from new registry with fallback to old. After verifying all images are in the new registry (digest comparison), disable old registry access. 30-day overlap for safety.

**Q2: How do you handle services that reference images by tag (mutable) in the old registry?**
A: Set up a proxy that redirects old registry URLs to the new registry. For Docker images, configure Docker daemon's `registry-mirrors`. For Maven, configure repository mirror in `settings.xml`. Gradually update Dockerfiles and pom.xml files to reference the new registry directly.

**Q3: What if the new registry has a bug that corrupts an image?**
A: Content-addressable storage prevents corruption: the client verifies the SHA-256 digest after pull. If the digest doesn't match, the pull fails with a clear error. The client retries or falls back to the old registry. Additionally, S3 checksums provide server-side integrity verification.

**Q4: How do you handle the retention policy rollout (avoiding accidental deletion)?**
A: (1) Retention runs in dry-run mode for 2 weeks (shows what would be deleted, doesn't delete). (2) Soft delete with 7-day recovery window. (3) Alert if retention would delete more than 10% of a repository's tags. (4) Permanent tags (releases) are never deleted by retention policies.

**Q5: How do you validate that geo-replication is working correctly?**
A: (1) Push an image to primary. (2) Within 5 minutes, verify the same digest is pullable from each replica region. (3) Compare blob counts across regions (should converge within replication lag). (4) Automated validation: a cron job pushes a test image hourly and verifies cross-region availability.

---

## 13. Trade-offs & Decision Log

| Decision | Option Chosen | Alternative | Rationale |
|----------|---------------|-------------|-----------|
| Blob storage backend | S3 | MinIO, Azure Blob, GCS | S3 is our cloud provider; 11-nines durability; infinite scale; VPC endpoints for free in-region transfer. |
| Metadata storage | MySQL | PostgreSQL, DynamoDB | Low write volume; relational model fits metadata well; operational familiarity. |
| Pull optimization | S3 redirect (302) | Proxy through Registry API, CDN | S3 redirect offloads all blob bandwidth to S3 (unlimited). No CDN needed for internal traffic. |
| Dedup level | Layer-level (Docker standard) | Chunk-level (Casync), file-level | Layer-level gives 60-80% dedup with zero custom tooling. Chunk-level adds complexity for marginal improvement. |
| Replication | Active push via Kafka | S3 CRR, on-demand pull-through | Active push gives < 5 min latency (vs. S3 CRR's 15 min). Pull-through is our fallback. |
| Vulnerability scanner | Trivy | Snyk, Clair, Anchore | Trivy is open-source, fast, well-maintained, and supports all our formats (Docker, Maven, pip). Clair is Docker-only. Snyk is SaaS (data sovereignty concern). |
| Retention strategy | Soft delete + GC grace period | Hard delete immediately | Soft delete prevents accidental data loss. 7-day recovery window catches misconfigured policies. |
| Image signing | cosign (Sigstore) | Notary v2, GPG | cosign is simple, keyless mode integrates with OIDC, and is the emerging standard. Notary v2 is more complex. |

---

## 14. Agentic AI Integration

### AI-Powered Registry Intelligence

| Use Case | Implementation |
|----------|---------------|
| **Automated vulnerability remediation** | AI agent detects critical CVE in a base image, finds the patched version, generates a Dockerfile update, and creates a PR for all affected services. "CVE-2026-1234 (critical) in eclipse-temurin:21.0.1. Patched in 21.0.2. Created PRs for 342 services." |
| **Storage optimization recommendations** | Agent analyzes storage usage patterns and recommends: "Repository org/legacy-svc has 200 tags but only 3 have been pulled in the last 90 days. Recommend reducing retention to last 5 tags, saving 15 GB." |
| **Image layer optimization** | Agent analyzes Dockerfile layer structure and recommends optimizations: "Service order-svc rebuilds the dependency layer on every push because COPY pom.xml is after COPY src. Recommend reordering for better caching. Estimated build time reduction: 40%." |
| **Dependency update automation** | Agent monitors Maven Central and PyPI for new versions of dependencies used across the organization. When a dependency with a known CVE is updated, agent creates PRs to bump the version, runs CI, and auto-merges if tests pass. |
| **Anomaly detection** | Agent monitors push/pull patterns. "Unusual: 500 pulls of image org/internal-tool:v3 from IP range 10.0.99.* (unknown subnet). Potential unauthorized access." |
| **Capacity forecasting** | Agent predicts storage growth based on push rate trends and recommends when to increase S3 budget or adjust retention policies. |

---

## 15. Complete Interviewer Q&A Bank

**Q1: Explain the Docker image format and how a registry stores it.**
A: A Docker image consists of: (1) A manifest (JSON) listing the config blob and layer blobs by digest. (2) A config blob (JSON) containing image metadata (env vars, CMD, labels). (3) Layer blobs (compressed tarballs, each representing a filesystem diff). The registry stores each blob at its SHA-256 digest key in S3. The manifest maps tags to the config + layers. On pull, the client fetches the manifest, then downloads each layer by digest.

**Q2: What is content-addressable storage and why is it important for a registry?**
A: Content-addressable storage uses the SHA-256 hash of the content as the storage key. This provides: (1) Immutability: changing content changes the key, so data can't be silently corrupted. (2) Deduplication: identical content stored once (same hash = same key). (3) Integrity verification: client verifies content matches the hash after download. (4) Cache-friendly: content at a given hash never changes, so it can be cached indefinitely.

**Q3: How do you handle Docker Hub rate limiting for open-source base images?**
A: Pull-through cache. Our registry proxies Docker Hub: `docker pull registry.internal/dockerhub/library/nginx:latest`. First pull fetches from Docker Hub, caches locally, and serves. Subsequent pulls (from any developer or CI pipeline) serve from cache. We never exceed Docker Hub's 100 pulls/6 hours limit because only our pull-through cache contacts Docker Hub.

**Q4: How do you implement garbage collection for unreferenced blobs?**
A: Two-phase: (1) Mark: scan all manifests and compute the set of referenced blob digests. Any blob not in this set is unreferenced. (2) Sweep: delete unreferenced blobs from S3 and MySQL after a 7-day grace period. The grace period handles the case where a push is in progress (layers uploaded but manifest not yet written). GC runs weekly during off-peak hours. It's a stop-the-world operation for deletes (no pushes during GC) or uses a snapshot of references.

**Q5: How do you handle a Docker pull that takes 30 seconds instead of 5?**
A: Diagnose: which layers are slow? If it's a large unique layer (> 500 MB), that's expected. If it's a small layer, check: (1) S3 latency (CloudWatch). (2) Cross-region pull (should be local). (3) Registry API latency (Prometheus). (4) Node disk I/O (if extracting layers is slow). Common fix: ensure pulls use S3 redirect (blob served directly from S3), not proxied through Registry API.

**Q6: How do you enforce that only CI-built images are deployed to production?**
A: Image signing + admission controller. CI pipeline signs every image with cosign using a CI-only signing key. The Kubernetes admission controller (Kyverno) rejects any pod in the production namespace whose image is not signed by the CI key. Developers can't push manually-built images to production because they don't have the signing key.

**Q7: How do you handle retention for Maven SNAPSHOT artifacts?**
A: Maven SNAPSHOTs are mutable (same version can be overwritten). Retention policy: keep the last 5 SNAPSHOT builds, delete older ones. Release versions (non-SNAPSHOT) are kept forever. This is implemented in the retention manager with a query: `SELECT * FROM maven_artifacts WHERE is_snapshot = TRUE GROUP BY group_id, artifact_id, version HAVING count > 5 ORDER BY published_at DESC`.

**Q8: How do you handle a registry migration (e.g., from Nexus to Harbor)?**
A: (1) Set up new registry alongside old. (2) CI pipelines push to both (dual-push). (3) Bulk-import existing images from old registry using `skopeo copy --all` (preserves digests). (4) Update k8s manifests to reference new registry. (5) Verify: every image in old registry exists in new (digest comparison). (6) Decommission old after 30 days.

**Q9: How do lazy loading / on-demand layer fetch work?**
A: Standard Docker pull downloads all layers before starting the container. Lazy loading (Stargz, Nydus, OverlayBD) downloads only the file ranges needed at startup. The image layers are stored in a seekable format (eStargz). The container runtime mounts layers as FUSE filesystems and fetches file chunks on-demand from the registry. This reduces container start time from 30s (full pull) to 2s (lazy pull) for large images.

**Q10: How do you handle a registry that serves 500 TB of data?**
A: Storage is in S3 (unlimited, $0.023/GB). Traffic is served via S3 redirect (registry only serves manifests, ~2 KB). S3 handles unlimited concurrent downloads. Cost optimization: (1) S3 Intelligent-Tiering automatically moves cold blobs to cheaper storage. (2) Retention policies delete old tags. (3) Layer dedup reduces unique storage by 60-80%.

**Q11: How do you handle multi-tenancy (multiple teams sharing one registry)?**
A: Repository naming convention: `{team}/{service}:{tag}`. RBAC: each team has push access to their repos only; all authenticated users have pull access. Resource quotas: per-team storage limit (e.g., 5 TB) prevents one team from consuming all storage. Rate limiting: per-team pull/push rate limits.

**Q12: How does Helm chart storage differ from Docker image storage?**
A: Helm charts are OCI artifacts (since Helm 3.8+). They follow the same OCI Distribution spec as Docker images: pushed as a manifest + blobs (chart tarball). The registry stores them alongside Docker images with a different `mediaType`. The chart repo API (`GET /v2/{repo}/tags/list`) works the same way. For legacy Helm repos (index.yaml), we generate a static index from the OCI artifacts.

**Q13: How do you handle a vulnerability that affects 80% of your images?**
A: (1) Identify affected images via scan results (Elasticsearch query: CVE ID). (2) Determine the fix (base image update, dependency bump). (3) Rebuild all golden base images with the fix. (4) Trigger CI pipelines for all affected services (automated rebuild from same source commit with new base image). (5) Track remediation progress in a dashboard. This is where the AI vulnerability remediation agent is most valuable -- it can automate steps 2-4.

**Q14: How do you implement bandwidth optimization for large images?**
A: (1) S3 redirect: blob bandwidth offloaded to S3. (2) Registry-level compression: images stored compressed (gzip/zstd), served compressed. (3) Layer caching on Kubernetes nodes. (4) Image pre-pull DaemonSet for frequently deployed images. (5) For very large images (> 1 GB), use eStargz lazy loading: only download the layers needed at startup time.

**Q15: How does the registry integrate with the CI/CD pipeline?**
A: (1) CI pipeline builds the Docker image (BuildKit). (2) Pipeline pushes to registry (`docker push registry.internal/org/svc:sha-abc123`). (3) Registry triggers Trivy scan asynchronously. (4) Pipeline polls scan status (or receives webhook). (5) If scan is clean, pipeline promotes the artifact (re-tags as `staging`). (6) ArgoCD detects the new tag and deploys. The registry is the handoff point between "build" and "deploy."

**Q16: What's the disaster recovery plan for the registry?**
A: (1) All data is in S3 (11-nines durability, cross-region replication). (2) MySQL is Multi-AZ with daily backups (30-day retention). (3) Redis is backed by AOF persistence. (4) If the primary region is lost: promote a replica region to primary (DNS failover). The replica has all blobs (via replication) and a read-replica MySQL (promoted to primary). RTO: 15 minutes. RPO: < 5 minutes (replication lag).

---

## 16. References

- Docker Registry HTTP API V2 Specification: https://docs.docker.com/registry/spec/api/
- OCI Distribution Specification: https://github.com/opencontainers/distribution-spec
- OCI Image Specification: https://github.com/opencontainers/image-spec
- Harbor (Open Source Container Registry): https://goharbor.io/
- JFrog Artifactory: https://www.jfrog.com/confluence/display/JFROG/JFrog+Artifactory
- Sonatype Nexus: https://help.sonatype.com/repomanager3
- Trivy (Vulnerability Scanner): https://aquasecurity.github.io/trivy/
- cosign (Container Signing): https://docs.sigstore.dev/cosign/overview/
- Stargz / eStargz (Lazy Loading): https://github.com/containerd/stargz-snapshotter
- Nydus (On-Demand Image Loading): https://nydus.dev/
- skopeo (Image Copy Tool): https://github.com/containers/skopeo
- Kyverno (Kubernetes Policy Engine): https://kyverno.io/
