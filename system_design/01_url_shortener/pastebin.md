# System Design: Pastebin

---

## 1. Requirement Clarifications

### Functional Requirements
- Users can create a "paste" — a block of plain text or code, stored with a unique shareable URL
- Pastes can be retrieved by their unique URL/key at any time within their lifetime
- Pastes support optional syntax highlighting hints (language tag stored with paste; rendering is client-side)
- Pastes can be set to expire after a configurable duration (10 minutes, 1 hour, 1 day, 1 week, 1 month, never)
- Optional password protection: pastes can require a password to view
- Optional visibility: public (listed in recent/trending) vs unlisted (accessible only via direct URL)
- Users can optionally create an account to manage (view list of, delete, update) their pastes
- Pastes can be forked: create a copy of an existing paste as a new paste
- Raw paste content accessible via a `/raw/<key>` endpoint (plain text, no HTML wrapper)
- Paste size limit: maximum 1 MB per paste

### Non-Functional Requirements
- **Availability:** read path must be highly available at 99.99% (~52 min/year downtime); write path at 99.9%
- **Durability:** paste content must never be silently lost; once stored, retrievable until expiry
- **Latency:** paste retrieval p99 < 50 ms; paste creation p99 < 300 ms
- **Scalability:** system must handle 10x traffic spikes without manual intervention
- **Consistency:** strong consistency for paste creation (unique key); eventual consistency acceptable for "recent pastes" feeds
- **Read-heavy workload:** read:write ratio approximately 10:1 (lower than URL shortener because paste content is large and less likely to be viral)
- **Security:** content scanning for credentials/PII leak detection (optional feature), rate limiting, spam prevention
- **Compliance:** GDPR — EU user data stays in EU; content reported as illegal must be removable within hours

### Out of Scope
- Real-time collaborative editing (Google Docs-style)
- Version history / git-style diffing between paste revisions
- Embedded paste rendering widget (JavaScript embed code)
- Full-text search across all paste contents (search is out of scope; may be added later)
- Paid tiers or billing
- Native mobile apps (API-first, web UI assumed)

---

## 2. Users & Scale

### User Types
- **Anonymous users:** create pastes without an account; cannot manage them after creation (only delete via a delete token provided at creation time)
- **Authenticated users:** create, view, list, edit, delete pastes on their account
- **Paste viewers:** the dominant traffic type; people who receive a paste URL and read its content
- **API integrators:** developers posting pastes programmatically (e.g., from CLI tools, IDEs, CI systems)
- **Bots/scrapers:** must be rate-limited at the edge

### Traffic Estimates

| Metric | Value | Reasoning |
|--------|-------|-----------|
| DAU | 10 million | Assumption: mid-tier service comparable to early Pastebin.com; 10M unique daily readers |
| New pastes created/day | 1 million | Assumption: 10% of DAU create 1 paste/day |
| Paste reads/day | 10 million | Assumption: 10:1 read:write ratio; average paste viewed 10 times |
| Reads/sec (QPS) | ~116 | 10M reads/day ÷ 86,400 s/day ≈ 115.7 QPS |
| Writes/sec (QPS) | ~12 | 1M pastes/day ÷ 86,400 s/day ≈ 11.6 QPS |
| Peak multiplier | 5x | Viral paste or HN/Reddit link; peak read QPS ≈ 578 |
| Read:Write ratio | ~10:1 | Lower than URL shortener — paste content is heavier, less viral |

**Assumptions stated explicitly:**
- 10M DAU is a conservative estimate for a mid-tier Pastebin. Scale 10x if the interviewer specifies Pastebin.com's actual traffic (~18M/month as of public estimates).
- "1 MB max paste size" is enforced at the API layer; average paste is assumed to be ~10 KB (code snippets, config files, logs).
- 10% of pastes are never read after creation (anonymous throwaway pastes).

### Latency Requirements
- **Read p99 < 50 ms** — Users reading a paste expect near-instant rendering. Paste content can be up to 1 MB, but average is ~10 KB. 50 ms p99 allows for cache hit (~2 ms Redis) + object storage fetch (~20 ms S3) + response serialization + network RTT. This is more lenient than URL shortener (20 ms) because paste retrieval involves reading a larger payload.
- **Write p99 < 300 ms** — Creating a paste involves: key generation + metadata DB write + object storage write. S3 PUT latency is ~100-200 ms at p99. 300 ms total budget is reasonable. Users accept a brief wait when submitting.
- **Raw content p99 < 100 ms** — The `/raw/<key>` endpoint is used by CLI tools and automation; 100 ms is acceptable.

### Storage Estimates

| Data Type | Size/record | Records/day | Retention | Total |
|-----------|-------------|-------------|-----------|-------|
| Paste metadata (DB) | ~512 bytes | 1,000,000 | 5 years | ~936 GB |
| Paste content (object storage) | ~10 KB average | 1,000,000 | Per TTL (avg 30 days) | ~300 GB active |
| Paste content long-term | ~10 KB | 1,000,000 × 365 days × "never expire" fraction (~10%) | 5 years | ~18 TB |
| User account | ~256 bytes | ~5,000/day | 5 years | ~2.3 GB |

**Metadata calculation:**
- 512 bytes × 1M pastes/day × 1825 days = ~936 GB with indexes over 5 years

**Content calculation:**
- Average paste: 10 KB. 90% of pastes have TTL < 30 days → ~270M active pastes × 10 KB = ~2.7 TB max active content on object storage.
- 10% never expire → 10% × 1M/day × 1825 days × 10 KB = ~1.8 TB/year → ~9 TB over 5 years.
- Total object storage (S3) over 5 years: ~11 TB. At $0.023/GB-month for S3 Standard, ~$252/month at 11 TB.

**Working set for cache:**
- Hot pastes (80% of reads hit 20% of pastes — Pareto principle): 20% of 30M active pastes = 6M pastes. At 10 KB each: ~60 GB. This is large for Redis RAM. Strategy: cache only metadata + small pastes (< 64 KB); for large pastes, rely on CDN or S3 with Transfer Acceleration. Redis cluster: 32 GB RAM covers metadata for all active pastes + full content of pastes < 2 KB.

### Bandwidth Estimates

| Direction | Calculation | Result |
|-----------|-------------|--------|
| Ingress (paste creation) | 11.6 writes/s × 10 KB avg | ~116 KB/s |
| Egress (paste reads) | 116 reads/s × 10 KB avg | ~1.16 MB/s |
| Peak egress (5x) | 578 reads/s × 10 KB avg | ~5.78 MB/s |
| Raw content egress | 20% of reads via /raw, same avg | ~0.23 MB/s |

Bandwidth is modest. Even at 10x peak (5.78 MB/s × 10 = ~58 MB/s), this is trivially handled by a single CDN distribution. S3 is rated for unlimited throughput (with request rate optimization for high-QPS prefixes).

---

## 3. High-Level Architecture

```
                      ┌───────────────────────────────────────────────────────┐
                      │                      INTERNET                         │
                      └──────────────────┬────────────────────────────────────┘
                                         │
                      ┌──────────────────▼──────────────────┐
                      │           DNS (Route53)              │
                      │   pastebin.io → CDN edge IP          │
                      └──────────────────┬────────────────────┘
                                         │
                   ┌─────────────────────▼──────────────────────┐
                   │               CDN (CloudFront)              │
                   │  - Caches GET /paste/<key> responses        │
                   │  - Caches GET /raw/<key> responses          │
                   │  - Serves static assets (JS/CSS/fonts)      │
                   │  - TLS termination, DDoS protection (WAF)  │
                   └──────────────┬─────────────────────────────┘
                                  │ Cache miss → origin
                   ┌──────────────▼──────────────┐
                   │       Load Balancer (L7)     │
                   │   Path-based routing:        │
                   │   /api/* → API Service       │
                   │   /paste/* → Read Service    │
                   │   /raw/* → Read Service      │
                   └─────┬──────────────┬─────────┘
                         │              │
             ┌───────────▼──┐    ┌──────▼───────────────┐
             │  Read        │    │  Write / API          │
             │  Service     │    │  Service              │
             │  (stateless) │    │  (stateless)          │
             │  N instances │    │  N instances          │
             └───────┬──────┘    └────────┬──────────────┘
                     │                    │
        ┌────────────▼──────┐   ┌─────────▼─────────────────┐
        │  Redis Cluster    │   │  Key Generator Service     │
        │  - Paste metadata │   │  (pre-generated key pool)  │
        │  - Content cache  │   │  Same design as URL        │
        │    (small pastes) │   │  shortener; 8-char base62  │
        │  - Rate limit     │   └─────────┬──────────────────┘
        │  - Delete tokens  │             │
        └──────────┬────────┘   ┌─────────▼──────────────────────┐
                   │            │     Object Storage (S3)         │
        ┌──────────▼────────┐   │  - Stores raw paste content     │
        │  PostgreSQL       │   │  - Prefix-sharded for >5K RPS   │
        │  (Primary + 2     │   │  - Lifecycle rules for TTL      │
        │   Read Replicas)  │   │    (S3 Object Expiry)           │
        │  - Paste metadata │   └─────────────────────────────────┘
        │  - User accounts  │
        │  - Paste index    │
        └───────────────────┘
```

**Component roles:**

- **DNS (Route53):** Resolves `pastebin.io` to CloudFront distribution. Health-check-based failover to secondary region if primary is unhealthy.
- **CDN (CloudFront):** The most impactful optimization for Pastebin. Paste content is immutable after creation (no in-place editing — only create new paste). Immutability means CDN can cache paste reads indefinitely (until TTL expiry). A paste that goes viral on Hacker News is served entirely from CDN within minutes, with zero origin load per subsequent request. WAF rules at CDN block known bot signatures and rate-limit IPs at edge.
- **Load Balancer (L7):** Path-based routing. `/api/*` (write, management) → Write/API Service. `/paste/*` and `/raw/*` (reads) → Read Service. SSL termination handled by CDN; LB listens on HTTP internally.
- **Read Service:** Handles `GET /paste/<key>` and `GET /raw/<key>`. Checks Redis for metadata (including content for small pastes). Falls back to PostgreSQL for metadata, then S3 for content. Extremely lightweight — just lookup + response assembly.
- **Write/API Service:** Handles `POST /api/v1/pastes`, `DELETE`, `PATCH`, user auth, paste listing. Contains all business logic (validation, key generation coordination, rate limiting enforcement).
- **Key Generator Service:** Maintains a pool of pre-generated 8-char base62 keys in Redis (`SPOP available_paste_keys`). Decoupled from Write Service for the same reasons as in URL shortener. Background refill job runs every 60 seconds.
- **Redis Cluster:** Multi-purpose: (a) paste metadata cache + small content cache, (b) rate limit counters, (c) delete tokens for anonymous pastes, (d) key pool for generation. 32 GB cluster across 3 shards.
- **PostgreSQL:** Source of truth for paste metadata (key, language, TTL, user_id, visibility, password_hash, created_at, expires_at). Does NOT store paste content — that's in S3. Read replicas serve list/search queries.
- **S3 (Object Storage):** Stores raw paste content as objects keyed by paste key (`pastes/<key>`). S3 is ideal here: (a) arbitrarily large objects up to 5 TB (our max is 1 MB per paste), (b) 11 nines durability, (c) native lifecycle rules for automatic expiry (saves building a cleanup job), (d) scales to millions of requests/second with prefix sharding, (e) cheap at $0.023/GB-month, (f) CDN (CloudFront) can be configured as an S3 origin directly — reads of popular pastes never touch our services.

**Primary use-case data flow (paste read):**

1. User opens `https://pastebin.io/paste/aB3xZ9rQ`
2. Browser → DNS → CDN. CDN checks its cache for the object at path `/paste/aB3xZ9rQ`. Cache hit: returns full HTML page (or JSON for API clients) from CDN edge. Done in ~10 ms.
3. CDN miss: request forwarded to Load Balancer → Read Service instance.
4. Read Service checks Redis for key `paste:meta:aB3xZ9rQ`. If hit: has metadata (language, expiry, visibility). For small pastes (< 64 KB), content is also in Redis (`paste:content:aB3xZ9rQ`). Assembles response and returns. Done in ~15 ms.
5. Partial Redis miss (metadata only): Read Service fetches content from S3 (`GET s3://pastes-bucket/pastes/aB3xZ9rQ`). S3 GET latency: ~20-50 ms. Populates Redis content cache if paste < 64 KB. Returns response.
6. Full Redis miss: query PostgreSQL replica for metadata, then S3 for content. Populate both Redis caches. Return response.
7. Password-protected paste: if `password_hash` is set on the metadata, Read Service returns a 401 with `requires_password: true`. Client submits password in subsequent request; Read Service computes `bcrypt(password)` and compares. If match, serves content.
8. Expiry check: if `expires_at < now()`, return HTTP 410 Gone, write tombstone to Redis (`paste:meta:<key>` = `EXPIRED`, TTL = 60s).

**Primary use-case data flow (paste creation):**

1. User submits `POST /api/v1/pastes` with content body.
2. Write Service validates: size ≤ 1 MB, language tag is valid, expiry is valid option, not blocked by rate limiter.
3. Key Generator: `SPOP available_paste_keys` from Redis → get unique key.
4. Write Service computes `sha256(content)` as ETag for cache validation.
5. Write Service uploads content to S3: `PUT s3://pastes-bucket/pastes/<key>` with `Content-Type: text/plain`, `x-amz-expiration` lifecycle tag if expiry is set.
6. Write Service inserts metadata to PostgreSQL: key, language, size, expiry, user_id, visibility, password_hash (if set), sha256_checksum.
7. Write Service writes to Redis cache: `SET paste:meta:<key> <json> EX <ttl>` and (if content ≤ 64 KB) `SET paste:content:<key> <content> EX <ttl>`.
8. For anonymous pastes: generate a `delete_token` (cryptographically random 32-char hex), store `SET paste:delete:<key> <delete_token> EX <paste_ttl>`, return to user in response.
9. Return HTTP 201 with paste URL and delete token.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- USERS TABLE
-- ============================================================
CREATE TABLE users (
    id              BIGINT          PRIMARY KEY,          -- Snowflake ID
    username        VARCHAR(50)     NOT NULL UNIQUE,
    email           VARCHAR(320)    NOT NULL UNIQUE,
    password_hash   CHAR(60)        NOT NULL,             -- bcrypt, cost factor 12
    api_key         CHAR(32)        UNIQUE,               -- hex token for API access
    default_language VARCHAR(50)    DEFAULT 'text',       -- user's preferred syntax highlight
    default_expiry  VARCHAR(20)     DEFAULT '1month',     -- user's preferred default TTL
    default_visibility VARCHAR(20)  DEFAULT 'public',
    paste_count     INT             NOT NULL DEFAULT 0,   -- denormalized counter
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_users_email    ON users(email);
CREATE INDEX idx_users_api_key  ON users(api_key) WHERE api_key IS NOT NULL;
CREATE INDEX idx_users_username ON users(username);

-- ============================================================
-- PASTES TABLE  (metadata only — content stored in S3)
-- ============================================================
CREATE TABLE pastes (
    id              BIGINT          PRIMARY KEY,          -- internal Snowflake ID
    paste_key       CHAR(8)         NOT NULL UNIQUE,      -- public key, e.g. 'aB3xZ9rQ'
    user_id         BIGINT          REFERENCES users(id) ON DELETE SET NULL,
    title           VARCHAR(200),                         -- optional user-provided title
    language        VARCHAR(50)     NOT NULL DEFAULT 'text', -- syntax hint: 'python','sql','text'
    size_bytes      INT             NOT NULL,             -- content size in bytes (max 1MB = 1048576)
    sha256_checksum CHAR(64)        NOT NULL,             -- SHA-256 of content for integrity + dedup
    visibility      VARCHAR(20)     NOT NULL DEFAULT 'public', -- public | unlisted | private
    password_hash   CHAR(60),                             -- bcrypt hash if password-protected
    expires_at      TIMESTAMPTZ,                          -- NULL = never expires
    is_active       BOOLEAN         NOT NULL DEFAULT TRUE,
    view_count      BIGINT          NOT NULL DEFAULT 0,   -- denormalized, updated async
    fork_of         CHAR(8)         REFERENCES pastes(paste_key), -- NULL if original
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- Hot path: paste key lookup (every read goes through this)
CREATE UNIQUE INDEX idx_pastes_key ON pastes(paste_key);

-- User's paste list (dashboard)
CREATE INDEX idx_pastes_user_created ON pastes(user_id, created_at DESC) WHERE user_id IS NOT NULL;

-- Public paste feed (recent public pastes)
CREATE INDEX idx_pastes_public_feed ON pastes(created_at DESC) WHERE visibility = 'public' AND is_active = TRUE;

-- Expiry cleanup: partial index on non-null expiry
CREATE INDEX idx_pastes_expires_at ON pastes(expires_at) WHERE expires_at IS NOT NULL AND is_active = TRUE;

-- Deduplication: same content hash should yield same stored object in S3
CREATE INDEX idx_pastes_sha256 ON pastes(sha256_checksum);

-- ============================================================
-- PASTE_VIEWS TABLE  (write-optimized; in practice use ClickHouse)
-- For reference — actual analytics store is columnar
-- ============================================================
CREATE TABLE paste_views (
    id              BIGINT          PRIMARY KEY,
    paste_key       CHAR(8)         NOT NULL,
    viewed_at       TIMESTAMPTZ     NOT NULL,
    ip_hash         CHAR(64),                             -- SHA-256 of IP for dedup, not raw IP (GDPR)
    country_code    CHAR(2),
    referrer_domain VARCHAR(200),
    user_agent_class VARCHAR(20)    -- browser | cli | bot | api
);

CREATE INDEX idx_paste_views_key_time ON paste_views(paste_key, viewed_at DESC);

-- ============================================================
-- S3 OBJECT NAMING CONVENTION (not a DB table, documented here)
-- Bucket: pastebin-content-prod
-- Object key: pastes/<paste_key>
-- Example: pastes/aB3xZ9rQ
-- Content-Type: text/plain; charset=utf-8
-- Metadata: x-amz-meta-language: python, x-amz-meta-paste-key: aB3xZ9rQ
-- S3 Lifecycle rule: objects with tag expires_in_days=<N> auto-delete after N days
-- ============================================================

-- ============================================================
-- ANONYMOUS_DELETE_TOKENS  (stored in Redis, schema documented here)
-- Redis key: paste:delete:<paste_key>
-- Value: <delete_token>  (32-char cryptographically random hex)
-- TTL: same as paste expiry (or 365 days if never-expire)
-- ============================================================
```

**Design decision — content in S3, not PostgreSQL:**
Storing 10 KB average content in PostgreSQL as a `TEXT` or `BYTEA` column would work up to ~100M pastes (~1 TB of content inline). Beyond that, PostgreSQL table bloat and TOAST (The Oversized-Attribute Storage Technique) fragmentation degrade read performance. S3 stores content as a flat object — retrieval is O(1) by key, scales to petabytes, costs 10x less than RDS storage, and allows direct CDN origin bypass for popular content. The tradeoff is an extra network hop (PostgreSQL → S3) on cache miss. Given our 50 ms read latency budget, this is acceptable.

**Deduplication via sha256:**
When a user submits content with the same SHA-256 as an existing paste, we can store only one copy in S3 (hard link semantics — multiple metadata rows point to the same S3 object key by hash). Index on `sha256_checksum` supports this check. For 1M pastes/day with some duplication (e.g., common boilerplate code), this saves 5-15% of S3 storage. Implementation: `SELECT paste_key FROM pastes WHERE sha256_checksum = $hash AND is_active = TRUE LIMIT 1`. If found, new paste metadata row points to existing S3 object. Note: object lifecycle must account for multiple metadata rows; only delete S3 object when all referring paste rows are inactive.

### Database Choice

- **Option A: PostgreSQL** — Pros: ACID uniqueness constraint on `paste_key`, rich index types (partial indexes for public feed and expiry), foreign key integrity, mature ecosystem, excellent for metadata-only storage (small rows), point-in-time recovery. Cons: Not suited for large BLOB content (hence content in S3 separately); needs connection pooling at scale; schema migrations require care.
- **Option B: DynamoDB** — Pros: Serverless auto-scaling, single-digit ms reads, no connection management. Cons: No ACID uniqueness guarantee for `paste_key` without DynamoDB Transactions (which cost 2x read/write units); no partial indexes (can't build the public feed efficiently without a GSI that costs extra); limited query flexibility; expensive at high read rates.
- **Option C: MongoDB** — Pros: Flexible schema (easy to add fields without migration), good for document-like paste metadata. Cons: Eventual consistency by default; no ACID multi-document transactions in older versions; read preference routing is less mature than PostgreSQL's primary-replica; operational complexity comparable to PostgreSQL but with fewer operational tools available.
- **Option D: Cassandra** — Pros: High write throughput, linear horizontal scaling, built-in multi-datacenter replication. Cons: Eventual consistency makes `paste_key` uniqueness unreliable; no secondary indexes with strong consistency; poor for the public feed (requires wide partition scans or denormalized tables); best suited for time-series analytics, not metadata lookup.

**Selected: PostgreSQL**

Reasoning specific to Pastebin: The metadata table has a dominant access pattern of point-lookup by `paste_key` — a single-row SELECT by unique indexed column. PostgreSQL handles this at >100K QPS with read replicas and PgBouncer. After 5 years, the pastes table reaches ~365M rows × 512 bytes = ~186 GB of table data — well within a single PostgreSQL cluster's tested range. The public feed query (`SELECT ... WHERE visibility='public' ORDER BY created_at DESC LIMIT 50`) is served by the partial index `idx_pastes_public_feed` and is paginated — no full table scans. The `sha256_checksum` deduplication lookup is also a simple index scan. DynamoDB or Cassandra would complicate uniqueness guarantees without proportional benefit at this scale.

---

## 5. API Design

### Authentication
- **Anonymous:** No auth required for creating or reading pastes. Rate-limited by IP.
- **Authenticated:** Bearer JWT (`Authorization: Bearer <token>`). JWT payload contains `user_id`, `plan`, `exp` (15-minute lifetime). Refresh token in HttpOnly cookie.
- **API clients:** `X-API-Key: <32-char-hex>` header. Rate limits determined by plan.
- **Delete tokens for anonymous pastes:** `X-Delete-Token: <32-char-hex>` — provided in creation response, required for anonymous paste deletion.

### Endpoints

```
POST /api/v1/pastes
Description: Create a new paste
Auth: Optional (anonymous or authenticated)
Rate limit: 10/hr anonymous per IP; 100/hr free user; 10000/hr pro API key
Headers:
  Content-Type: application/json
  Authorization: Bearer <jwt> (optional)
  X-API-Key: <key> (optional, alternative auth)
  Idempotency-Key: <uuid-v4> (optional, for safe retries)
Request:
  {
    "content":    string (required, max 1 MB, UTF-8),
    "title":      string (optional, max 200 chars),
    "language":   string (optional, default "text" — e.g., "python", "sql", "bash"),
    "expiry":     string (optional, enum: "10min" | "1hour" | "1day" | "1week" | "1month" | "never", default "1month"),
    "visibility": string (optional, enum: "public" | "unlisted", default "public"),
    "password":   string (optional, plaintext — stored as bcrypt hash)
  }
Response 201 Created:
  {
    "paste_key":    string,    // e.g. "aB3xZ9rQ"
    "url":          string,    // e.g. "https://pastebin.io/paste/aB3xZ9rQ"
    "raw_url":      string,    // e.g. "https://pastebin.io/raw/aB3xZ9rQ"
    "title":        string | null,
    "language":     string,
    "expiry":       string,
    "expires_at":   string | null,  // ISO 8601
    "visibility":   string,
    "size_bytes":   integer,
    "created_at":   string,
    "delete_token": string | null   // only for anonymous pastes; store this to delete later
  }
Response 400 Bad Request:
  { "error": "content_too_large", "message": "Paste content exceeds 1 MB limit" }
  { "error": "invalid_language", "message": "Unsupported language tag" }
Response 429 Too Many Requests:
  Headers: X-RateLimit-Limit: 10, X-RateLimit-Remaining: 0, X-RateLimit-Reset: <epoch>
```

```
GET /paste/{paste_key}
Description: Retrieve paste with HTML rendering wrapper (web UI)
GET /raw/{paste_key}
Description: Retrieve raw paste content (plain text, no HTML)
Auth: None for public/unlisted. For password-protected pastes, provide password.
Query params:
  password: string (optional, for password-protected pastes)
Response 200 OK (for /raw):
  Content-Type: text/plain; charset=utf-8
  Body: <raw paste content>
  Headers:
    ETag: "<sha256_checksum>"
    Cache-Control: public, max-age=86400  (for public, non-expiring pastes)
    Cache-Control: public, max-age=<seconds_until_expiry>  (for expiring pastes)
    X-Paste-Language: python
    X-Paste-Created: 2026-04-09T12:00:00Z
    X-Paste-Expires: 2026-05-09T12:00:00Z  (if expiring)
Response 401 Unauthorized:
  { "error": "password_required", "requires_password": true }
Response 404 Not Found:
  { "error": "not_found" }
Response 410 Gone:
  { "error": "expired", "message": "This paste has expired" }
```

```
GET /api/v1/pastes/{paste_key}
Description: Get paste metadata + content as JSON (API clients)
Auth: None for public; optional for unlisted (URL is the auth); required for private
Response 200 OK:
  {
    "paste_key":    string,
    "title":        string | null,
    "content":      string,
    "language":     string,
    "size_bytes":   integer,
    "visibility":   string,
    "expires_at":   string | null,
    "view_count":   integer,
    "fork_of":      string | null,
    "created_at":   string,
    "user": {
      "username": string | null   // null for anonymous pastes
    }
  }
```

```
DELETE /api/v1/pastes/{paste_key}
Description: Delete (deactivate) a paste
Auth: Authenticated (must be owner) OR anonymous with delete_token
Headers:
  Authorization: Bearer <jwt>          (for authenticated owner)
  X-Delete-Token: <32-char-hex>       (for anonymous paste owner)
Response 204 No Content: success
Response 401 Unauthorized: missing auth
Response 403 Forbidden: wrong delete token or not the owner
Response 404 Not Found
```

```
PATCH /api/v1/pastes/{paste_key}
Description: Update paste metadata (NOT content — content is immutable after creation)
Auth: Required (Bearer JWT); must be owner
Note: Content is intentionally immutable. To "edit" a paste, fork it.
Request:
  {
    "title":      string (optional),
    "language":   string (optional),
    "visibility": string (optional, enum: "public" | "unlisted"),
    "expires_at": string | null (optional, extend or remove expiry)
  }
Response 200 OK: updated paste metadata object
Response 400 Bad Request: cannot change visibility to "private" (not a supported option)
Response 403 Forbidden
Response 404 Not Found
```

```
POST /api/v1/pastes/{paste_key}/fork
Description: Create a new paste as a copy of an existing one (allows modification of content)
Auth: Optional
Request:
  {
    "content":    string (optional, override content; if omitted, use original content),
    "title":      string (optional),
    "language":   string (optional),
    "expiry":     string (optional),
    "visibility": string (optional)
  }
Response 201 Created: new paste object with fork_of=<original_paste_key>
```

```
GET /api/v1/pastes
Description: List pastes for authenticated user
Auth: Required (Bearer JWT)
Query params:
  page:      integer (default: 1)
  page_size: integer (default: 20, max: 100)
  language:  string (optional filter)
  sort:      "created_at" | "view_count" (default: "created_at")
  order:     "asc" | "desc" (default: "desc")
Response 200 OK:
  {
    "pastes": [ <paste metadata objects, no content field> ],
    "pagination": {
      "page": integer,
      "page_size": integer,
      "total": integer,
      "total_pages": integer
    }
  }
```

```
GET /api/v1/feed
Description: Public feed of recent pastes (visibility=public)
Auth: None
Query params:
  page:      integer (default: 1)
  page_size: integer (default: 50, max: 100)
  language:  string (optional filter, e.g. "python")
Response 200 OK:
  {
    "pastes": [ <paste metadata objects, no content field> ],
    "pagination": { ... }
  }
Cache-Control: public, max-age=60  (feed is cached at CDN for 60 seconds)
```

---

## 6. Deep Dive: Core Components

### Component: Paste Key Generation

**Problem it solves:** Every paste creation requires a unique 8-character identifier that is: (a) not guessable (to prevent enumeration of unlisted pastes), (b) short enough to be human-shareable, (c) generated without a centralized bottleneck at 12 writes/sec (low rate, but solution should be scalable to 10x).

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Random base62 string with DB uniqueness check | Generate random 8-char string, SELECT to check uniqueness, retry on collision | Simple; guaranteed unique via DB constraint | Requires DB read on every write; collision probability: at 365M records, P(collision in 8-char base62) = 365M / 62^8 ≈ 0.00016% per attempt — very low but possible |
| UUID v4 truncated to base62 | Take first 8 chars of UUID's base62 encoding | Highly random | Truncation reduces effective bits; UUID v4 has 122 random bits; 8 chars of base62 = ~48 bits; less random but sufficient |
| Sequential ID encoded to base62 | Auto-increment PK → base62 | Zero collisions, predictable | Sequential = enumerable; reveals paste count to competitors; security risk for unlisted pastes |
| Pre-generated pool (Redis SPOP) | Offline generate keys, store in Redis set, pop atomically | Zero collision risk, zero DB read on hot path, random (not guessable) | Pool management overhead; pool exhaustion fallback needed |
| Snowflake ID → base62 | 64-bit time+worker+seq → base62 | Distributed, unique, no coordination at generation time | Partially enumerable (time-ordered); 64-bit base62 = 11 chars — too long for 8-char target; need truncation, reintroducing collision risk |
| HMAC-based keyed hash | HMAC-SHA256(server_secret, user_id + timestamp + nonce) → base62 truncation | Cryptographically strong; hard to enumerate | Still requires truncation, so collisions possible; key reveals generation time if secret leaks |

**Selected Approach & Reasoning:**

**Pre-generated random pool (Redis SPOP) — same architecture as URL shortener key generation.**

Numbers:
- Keyspace: 8-char base62 = 62^8 = ~218 trillion unique keys. At 1M pastes/day, this lasts 218 trillion ÷ 1M = 218 million years. Keyspace is effectively infinite.
- Pool target: 5M keys in Redis set (~40 MB at ~8 bytes/key).
- Refill triggers when pool drops below 500K keys; background job generates 2M new keys, batch-checks against `pastes` DB table for prior use, inserts clean ones into pool.
- Key property: purely random base62 → no temporal or sequential information leaked. Unlisted pastes cannot be enumerated because P(guessing any valid key) = active_pastes / 218T ≈ 365M / 218T ≈ 0.00017% per attempt. At 10 guesses/second, expected time to find one valid key: 218T / 365M / 10 ≈ 597 years.

**Implementation Detail:**

```
base62_alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

func generate_key(length=8) -> string:
    return ''.join(secrets.choice(base62_alphabet) for _ in range(length))

// Pool refill (background job, runs every 60s, triggered when pool < 500K):
func refill_pool():
    BATCH = 2_000_000
    candidates = {generate_key() for _ in range(BATCH)}  // set for dedup
    // Check DB for already-used keys (using partial index on active pastes)
    existing = db.query(
        "SELECT paste_key FROM pastes WHERE paste_key = ANY(%s)",
        list(candidates)
    )
    existing_set = {row.paste_key for row in existing}
    new_keys = candidates - existing_set
    redis.sadd("available_paste_keys", *new_keys)
    log.info(f"Refilled pool with {len(new_keys)} keys, pool size: {redis.scard('available_paste_keys')}")

// Write path:
func create_paste(content, ...):
    key = redis.spop("available_paste_keys")
    if key is None:
        // Fallback: random key + DB upsert with conflict detection
        key = generate_key()
        // INSERT ... ON CONFLICT (paste_key) DO NOTHING
    
    checksum = sha256(content.encode('utf-8')).hexdigest()
    
    // Check for content deduplication
    existing = db.query(
        "SELECT paste_key FROM pastes WHERE sha256_checksum = %s AND is_active = TRUE LIMIT 1",
        checksum
    )
    if existing and user wants dedup:
        s3_key = f"pastes/{existing.paste_key}"  // reuse existing S3 object
    else:
        s3_key = f"pastes/{key}"
        s3.put_object(Bucket='pastebin-content-prod', Key=s3_key, Body=content.encode())
    
    db.execute("""
        INSERT INTO pastes (id, paste_key, user_id, language, size_bytes, sha256_checksum,
                           visibility, password_hash, expires_at, created_at)
        VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, NOW())
    """, [snowflake_id(), key, user_id, language, len(content), checksum,
          visibility, bcrypt(password) if password else None, expires_at])
    
    return key
```

**Interviewer Deep-Dive Q&A:**

Q: You said unlisted pastes are secured by the obscurity of the key. Is that sufficient security for sensitive content?
A: Obscurity alone is not sufficient for truly sensitive data — it's defense-in-depth layer 1. Additional measures: (a) **Password protection**: for sensitive pastes, users must set a password. The stored bcrypt hash is checked at read time; brute-force is rate-limited to 5 attempts per paste key per minute. (b) **URL secrecy**: unlisted paste URLs should only be shared via channels the user controls; they should not be stored in CDN access logs in plaintext — we hash the paste key in our access logs. (c) **Expiry enforcement**: sensitive pastes should use short TTL (10 minutes or 1 hour). (d) **No Google indexing**: unlisted pastes carry `X-Robots-Tag: noindex` response header and we submit a `disallow /paste/*` directive in `robots.txt`. (e) **Rate limiting on reads**: 100 reads/minute per IP per paste key triggers CAPTCHA or temporary block — prevents brute-force enumeration of content for a known key.

Q: The pool refill job runs a `SELECT paste_key FROM pastes WHERE paste_key = ANY(array)` with 2M candidates. How long does this take at 365M rows?
A: The `idx_pastes_key` unique index supports this ANY-array lookup with index scans. For k=2M candidates and N=365M rows, the database performs 2M individual index lookups in a single query. PostgreSQL can typically process ~500K index lookups/second for this type of query. Estimate: 2M / 500K = ~4 seconds. This is acceptable for a background job that runs every 60 seconds. Optimization if needed: (a) Run the refill job less frequently but with larger batches (trade memory for frequency). (b) Use a Bloom filter: load the `paste_key` column into a Redis Bloom filter (`BF.ADD`). Before DB check, filter out candidates with `BF.EXISTS` — candidates that "definitely don't exist" skip the DB query entirely. With false positive rate 0.1%, 99.9% of collisions still detected; the remaining 0.1% cause a DB insert conflict (handled by ON CONFLICT). Bloom filter for 365M items at 0.1% FPR requires ~850 MB of Redis memory — feasible.

Q: What if two concurrent paste creation requests both pop the same key from Redis? (Race condition)
A: `SPOP` on a Redis set is atomic and linearizable — two concurrent SPOP calls will always return different elements. This is guaranteed by Redis's single-threaded command execution model. Concurrent calls: process A calls SPOP → gets "aB3xZ9rQ"; process B calls SPOP → gets "qR7mNk2P". No collision possible via the SPOP path. Race condition can only occur in the fallback path (where SPOP returned nil and we generate a random key). In the fallback, the DB `INSERT ... ON CONFLICT (paste_key) DO NOTHING` handles the race: only one insert succeeds; the other must retry with a new key.

Q: How do you handle paste content deduplication across storage when the same content is submitted by different users?
A: Using the SHA-256 hash as a content-addressable key in S3. When content hash already exists in S3 (checked via `SELECT paste_key FROM pastes WHERE sha256_checksum = $hash`): store a new paste metadata row but set the S3 object key to point to the existing object (`s3://bucket/pastes/<original_key>`). Multiple metadata rows reference one S3 object. Complications: (a) S3 lifecycle/expiry — if the original paste expires, its S3 object gets deleted, breaking the references. Solution: use S3 object key = sha256 hash itself (`s3://bucket/content/<sha256>`), not the paste key. Lifecycle rule deletes the content object only when all referencing paste metadata rows are inactive. (b) Privacy: user A's "private" content stored in S3 is the same object as user B's "public" content after deduplication. This is an acceptable technical implementation detail as long as we never expose user A's content via user B's paste (access control is enforced at the metadata layer, not the S3 layer — S3 bucket is private, all access goes through our service). (c) Benefit: on average 5-15% storage savings from common boilerplate pastes.

Q: What if someone submits a 1 MB paste with random content designed to exhaust your key pool? (Adversarial paste creation)
A: Pool exhaustion attack: adversary submits 10M paste creations (at 10/hr rate limit per IP = 1M IPs needed — infeasible for a single attacker). The key pool has 5M keys and is refilled continuously. At 12 writes/sec normal load, the pool drains in 5M / 12 ≈ 5.7 days without refill — but the refill job runs every 60s and generates 2M keys per run. Net pool change per hour: +2M (refill every 60s × 60 = 120M — wait, correction: refill runs every 60s and generates 2M keys, so 2M keys/min = 120M/hour added minus 12 keys/sec × 3600 = 43,200 keys/hour consumed. Pool grows, not shrinks, under normal load. Under adversarial load (rate-limited at 10/hr/IP): even with 100K distinct IPs, that's 1M pastes/hour = 277 writes/sec. Pool refill rate (120M/hr) >> adversarial consumption (1M/hr). Not a concern. DDoS protection at CDN/WAF layer handles the connection layer.

---

### Component: Content Storage & Retrieval (S3 + Cache Layering)

**Problem it solves:** Paste content ranges from a few bytes to 1 MB per paste. Storing it inline in PostgreSQL creates table bloat and degrades query performance. We need content to be: (a) durable (11 nines), (b) fast to read (< 50 ms p99), (c) cheap to store (~$0.023/GB-month), (d) automatically expirable (lifecycle rules), (e) CDN-cacheable.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| PostgreSQL TEXT column | Store content inline in pastes table | Single DB call for metadata + content | Table bloat; TOAST fragmentation; sequential scans become expensive; DB becomes a bottleneck for large reads |
| Redis (content cache only) | Cache content in Redis, source of truth elsewhere | Sub-ms reads for hot content | RAM-limited (~10 KB × 1M active = 10 GB just for recent pastes — feasible but only for small content); Redis is not durable enough as the only store |
| S3 (source of truth) + Redis (cache for small pastes) + CDN | S3 for all content; Redis caches pastes < 64 KB; CDN caches public pastes | Infinite scale, cheap, native CDN integration, lifecycle rules | Extra network hop to S3 on cache miss; S3 GET latency ~20-50 ms adds to p99 |
| Dedicated blob store (MinIO, Ceph) | Self-hosted object storage | Avoid AWS vendor lock-in; full control | Operational burden; does not scale as easily as S3; same interface as S3 |
| Cassandra wide-column | Store content as a BLOB column in Cassandra | High write throughput; linear scaling | Not cost-effective for large values; read amplification for large objects; complex operational model |

**Selected Approach & Reasoning:**

**S3 as source of truth + Redis cache for small pastes (< 64 KB) + CloudFront CDN.**

Why S3 over PostgreSQL for content:
- At 1M pastes/day × 10 KB average = 10 GB/day. After 1 year = 3.65 TB of content. PostgreSQL tables this large require dedicated SSD storage and significantly impact query performance for all operations on the table.
- S3 GET latency: 20-50 ms p99 within the same region. Our 50 ms read p99 budget accommodates this with no Redis cache hit.
- S3 lifecycle rules: configure `Expiration` rules on objects tagged with `expires_in_days=<N>`. S3 automatically deletes objects — no cleanup job needed for content (metadata cleanup in PostgreSQL still needed for `is_active` flag).
- CloudFront can be configured to use S3 as the origin directly, meaning popular public pastes are served from CDN with no service layer involved.
- Cost: 11 TB of content in S3 Standard = 11,000 GB × $0.023/GB/month = $253/month. Equivalent RDS storage: $0.115/GB/month = $1,265/month. S3 is 5x cheaper.

S3 prefix sharding:
- S3 has a default limit of 5,500 GET requests/second per prefix. At 578 peak QPS, a single prefix handles this. If scale grows 10x (5,780 QPS), distribute across 4 prefixes: `pastes/0/`, `pastes/1/`, `pastes/2/`, `pastes/3/` keyed by `crc32(paste_key) % 4`. AWS documentation recommends this for > 5,500 RPS per prefix. In practice, S3 auto-scales per-prefix limits; official recommendation since 2018 is that S3 auto-scales, but having multiple prefixes is still safer for very high QPS.

Cache layering:
- **Layer 1 - CDN (CloudFront)**: Public pastes cached at edge nodes globally. Cache key: full URL path `/raw/<paste_key>`. `Cache-Control: public, max-age=<seconds_until_expiry>`. CDN hit → response in ~5-15 ms from nearest PoP.
- **Layer 2 - Redis content cache**: For small pastes (< 64 KB), store content in Redis: `SET paste:content:<key> <content> EX <ttl>`. 32 GB Redis cluster holds ~500K pastes at 64 KB each (500K × 64 KB = 32 GB). In practice, most pastes are < 5 KB, so Redis holds several million hot pastes.
- **Layer 3 - S3**: Cache miss falls through to S3 `GetObject` call. After fetching, populate Redis (if ≤ 64 KB) and let CDN cache the response for the next request.

**Implementation Detail:**

```
Read Service pseudocode:

func handlePasteRead(paste_key string, password string) HTTPResponse:
    // Layer 1: CDN handles it before we even see the request (transparent)
    
    // Layer 2: Redis metadata check
    meta_json = redis.get("paste:meta:" + paste_key)
    
    if meta_json == "NOT_FOUND":
        return HTTP 404
    
    if meta_json == "EXPIRED":
        return HTTP 410
    
    if meta_json != nil:
        meta = parse_json(meta_json)
    else:
        // Redis miss: query PostgreSQL replica
        meta = db_replica.queryRow(
            "SELECT paste_key, language, size_bytes, sha256_checksum, visibility, " +
            "password_hash, expires_at, is_active, view_count, user_id " +
            "FROM pastes WHERE paste_key = $1",
            paste_key
        )
        if meta == nil:
            redis.set("paste:meta:" + paste_key, "NOT_FOUND", ex=300)
            return HTTP 404
        if !meta.is_active:
            redis.set("paste:meta:" + paste_key, "NOT_FOUND", ex=300)
            return HTTP 410
        // Cache metadata (content excluded — stored separately)
        redis.set("paste:meta:" + paste_key, json_encode(meta), ex=min(86400, seconds_until(meta.expires_at)))
    
    // Check expiry
    if meta.expires_at != nil && time.now() > meta.expires_at:
        redis.set("paste:meta:" + paste_key, "EXPIRED", ex=60)
        return HTTP 410
    
    // Password check
    if meta.password_hash != nil:
        if password == "":
            return HTTP 401, {"requires_password": true}
        if !bcrypt.verify(password, meta.password_hash):
            return HTTP 403, {"error": "wrong_password"}
    
    // Fetch content
    content = redis.get("paste:content:" + paste_key)
    if content == nil:
        // S3 fetch
        s3_resp = s3.get_object(Bucket="pastebin-content-prod", Key="pastes/" + paste_key)
        content = s3_resp.Body.read()
        if len(content) <= 65536:  // 64 KB threshold
            redis.set("paste:content:" + paste_key, content, ex=min(86400, seconds_until(meta.expires_at)))
    
    // Async view count increment
    go redis.incr("paste:views:" + paste_key)
    
    return HTTP 200, content, headers={
        "Content-Type": "text/plain; charset=utf-8",
        "ETag": '"' + meta.sha256_checksum + '"',
        "X-Paste-Language": meta.language,
        "Cache-Control": cache_control_for(meta),
    }

func cache_control_for(meta):
    if meta.password_hash != nil:
        return "private, no-store"  // Never cache password-protected
    if meta.visibility == "unlisted":
        return "private, max-age=3600"  // Cache in browser only
    if meta.expires_at == nil:
        return "public, max-age=86400"  // Cache up to 1 day (CDN caches longer via s-maxage)
    secs = max(0, seconds_until(meta.expires_at))
    return f"public, max-age={min(secs, 86400)}, s-maxage={min(secs, 2592000)}"
```

**Interviewer Deep-Dive Q&A:**

Q: How do you ensure paste content is not corrupted in S3? What if S3 returns an incomplete or corrupted object?
A: (a) **SHA-256 integrity check**: At creation time, we compute `sha256(content)` and store it as `sha256_checksum` in PostgreSQL. At read time (after fetching from S3), compute `sha256(fetched_content)` and compare against stored checksum. If mismatch: log an error with the paste_key, return HTTP 500 to user, alert on-call. (b) **S3 built-in checksum**: S3 supports MD5 ETag and optional SHA-256 checksums at the SDK level (`ChecksumSHA256`). Our S3 PUT includes `ChecksumSHA256=<base64-sha256>`. S3 verifies the checksum before acknowledging the write — so object storage itself is verified. (c) **S3 durability**: 11 nines durability means data loss from S3 itself is a ~once-in-a-millennium event for 1M objects. Corruption (bit flip) is handled by S3's internal redundancy (stores data across multiple AZs with Reed-Solomon coding). We don't need a secondary backup for S3 content unless compliance requires it (for regulated industries, replicate to a second S3 bucket in a different region with cross-region replication).

Q: A paste is deleted by the user but the CDN has cached it for 24 hours. How do you ensure the deleted content is not served?
A: Three-step invalidation: (1) PostgreSQL: `UPDATE pastes SET is_active = FALSE WHERE paste_key = $key` and `DELETE` in S3 via `s3.delete_object(Key=...)`. (2) Redis: `DEL paste:meta:<key>` and `DEL paste:content:<key>`. (3) CDN: `cloudfront.createInvalidation(Paths=["/" + key + "*"])` — the wildcard invalidates both `/paste/<key>` and `/raw/<key>`. CloudFront invalidation propagates globally in < 5 seconds. Until the CDN invalidation completes (worst case 5 seconds), some edge nodes may serve stale cached content. This is acceptable — it's a very short window. For legal/DMCA takedowns where speed is critical, we also set `Cache-Control: no-store` on sensitive content at creation time, preventing CDN caching entirely.

Q: How does the S3 object lifecycle interact with PostgreSQL `is_active` flag? They can get out of sync.
A: Potential inconsistencies: (a) S3 deletes object (lifecycle rule fires) before PostgreSQL `is_active` is set to FALSE. Result: metadata says paste exists, but S3 returns 404. Read Service catches S3 NoSuchKey error, returns HTTP 410 Gone to user, and marks the PostgreSQL row as `is_active = FALSE` (compensating write). (b) PostgreSQL marks `is_active = FALSE` (user deleted) but S3 lifecycle hasn't fired yet. Result: metadata says gone, S3 still has content. Read Service checks metadata first — sees `is_active = FALSE` → returns 404/410 without touching S3. S3 cleanup happens naturally via lifecycle rule later. (c) For deduplication (multiple paste rows → same S3 object): S3 lifecycle rule must NOT fire until all referencing paste rows are inactive. Solution: use a separate S3 key for deduplicated content (`s3://bucket/content/<sha256>`), and a separate cleanup job that periodically runs `SELECT sha256_checksum FROM pastes WHERE sha256_checksum = $hash AND is_active = TRUE` — only delete the S3 object when this query returns zero rows. The background cleanup job runs daily.

---

### Component: Paste Expiry & Cleanup

**Problem it solves:** With 1M pastes/day and ~90% having TTLs ranging from 10 minutes to 1 month, millions of paste objects need to be cleaned up. Without cleanup, storage grows unboundedly, and stale paste keys remain "used" preventing reuse. Two things must expire: (a) database metadata rows, (b) S3 content objects.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| S3 Object Expiry lifecycle rules | S3 automatically deletes objects after N days | Zero-code, S3-managed, reliable | Only works for content, not metadata in PostgreSQL; granularity is days (not hours/minutes) for S3 lifecycle |
| PostgreSQL background job (cron) | Periodic `UPDATE pastes SET is_active=FALSE WHERE expires_at < NOW()` | Works for metadata; simple | Must be scheduled carefully; long-running queries can cause lock contention |
| Redis TTL (cache-based expiry) | Set TTL = paste lifetime on Redis cache key; expired keys auto-evict | Zero-code for cache; consistent TTL management | Only evicts from cache, not from S3 or DB; cannot be sole cleanup mechanism |
| Event-driven expiry (delayed job queue) | On paste creation, schedule a job for `expires_at` time | Precise expiry to the second; no polling | Requires a reliable job scheduler (e.g., Sidekiq, BullMQ) with durable job storage; complex to manage job state across millions of future jobs |
| Lazy expiry (check on read) | Don't clean up until someone reads the paste; return 410 | Zero background load | Stale data persists in DB/S3 indefinitely unless read; wastes storage |

**Selected Approach & Reasoning:**

**Hybrid: S3 lifecycle rules (for content) + PostgreSQL cron batch job (for metadata) + Redis TTL (for cache) + lazy expiry on read.**

1. **S3 lifecycle**: At paste creation, tag S3 object with `ExpiryDate` metadata. S3 lifecycle rule: `Expiration: DateLessThanEquals ${ExpiryDate}`. S3 deletes content automatically. For "never expire" pastes, no expiry tag → no lifecycle rule → stays forever.

2. **Redis TTL**: `SET paste:meta:<key> <json> EX <seconds_until_expiry>` and `SET paste:content:<key> <content> EX <seconds_until_expiry>`. Redis automatically evicts cache entries at expiry time. No separate cache cleanup needed.

3. **PostgreSQL cron job**: Runs every 5 minutes. Uses the partial index `idx_pastes_expires_at`:
   ```sql
   UPDATE pastes SET is_active = FALSE
   WHERE expires_at < NOW() AND is_active = TRUE
   LIMIT 5000;
   ```
   `LIMIT 5000` prevents long-running queries. At 1M pastes/day with 90% expiring, ~900K pastes expire/day = ~625 pastes/minute. Each cron run updates ~3,125 rows (5 minutes × 625). Well within the LIMIT. After the UPDATE, the cron job also deletes Redis keys for the expired pastes (batch `DEL` pipeline).

4. **Lazy expiry on read**: As a safety net, the Read Service checks `expires_at < now()` in application code on every access. If expired: return HTTP 410, write tombstone to Redis, schedule async cleanup. This handles the window between cron runs.

Precision issue for short TTLs (10 minutes): S3 lifecycle granularity is 1 day — cannot delete after 10 minutes. Solution: for pastes with TTL < 24 hours, do NOT use S3 lifecycle tags. Instead, rely on: (a) the PostgreSQL cron to mark metadata inactive (runs every 5 min), (b) lazy expiry on read for the precise window. The S3 object for a "10 minute" paste persists in S3 until the daily cleanup job explicitly deletes it. Daily cleanup: `SELECT paste_key FROM pastes WHERE expires_at < NOW() - INTERVAL '1 day' AND is_active = FALSE` → batch S3 DeleteObjects call.

**Interviewer Deep-Dive Q&A:**

Q: What happens to the key pool when pastes expire — are short codes returned to the pool for reuse?
A: By default, expired paste keys are NOT returned to the pool. The `pastes` table retains `is_active=FALSE` rows permanently (or for 5 years before archival). The key generation pool refill job queries `SELECT paste_key FROM pastes WHERE paste_key = ANY(...)` which finds both active and inactive rows — inactive expired paste keys are still "used" and won't be re-added to the pool. This is intentional: the same risks as URL shortener key reuse apply — a user may have cached the paste URL in their notes. After a quarantine period of 90 days post-expiry, keys become eligible for reuse. A separate job runs weekly: `SELECT paste_key FROM pastes WHERE expires_at < NOW() - INTERVAL '90 days' AND is_active = FALSE` → adds these keys back to a `released_keys` Redis set, which the pool refill job prioritizes before generating new random keys. At our scale, key reuse is never necessary (218 trillion keyspace vs 365M max keys used = 0.00017% exhaustion in 5 years), so this is purely a theoretical design choice; in practice, we simply never reuse.

---

## 7. Scaling

### Horizontal Scaling

- **Read Service:** Fully stateless. Auto-scaling group (ASG) triggers on CPU > 60% or p99 latency > 40ms. At 578 peak QPS (5x) with ~30ms average response time (S3 fetch time dominates), each instance handles ~100 concurrent requests (limited by S3 round-trip latency, not CPU). 4 GB RAM per instance: 1 GB for process overhead, 3 GB for in-flight connections. At 578 QPS × 30ms = ~17 concurrent requests per instance, 8 instances handle peak load with 6x headroom.
- **Write/API Service:** Stateless. 12 writes/sec baseline; peak 60 writes/sec. 2 instances are sufficient; 4 for HA. Scale up for anticipated marketing campaigns.
- **Key Generator / Pool Refill:** Single background process (12 pastes/sec is trivially served by the pool). 2 instances with distributed locking via Redis (`SET lock:key-refill NX EX 120`).

### Database Scaling

**Sharding:**

- **Hash-based sharding on `paste_key`**: All paste lookups are point-reads by `paste_key`. Hash sharding distributes load evenly. Formula: `shard_id = crc32(paste_key) % num_shards`. Start with 4 shards; expand to 16 with consistent hashing. At 365M pastes in 5 years (365M × 512 bytes ≈ 186 GB table data), a single PostgreSQL cluster handles this without sharding. Shard when writes exceed 1000/sec or table exceeds 1B rows. Our scale (12 writes/sec) means sharding is a future concern, not current.
- **Why not range-based**: `paste_key` is random base62 — range sharding on random keys distributes evenly anyway (equivalent to hash for uniform random values), but hash is explicit about this intent.
- **User data**: Users table (50M rows at most) stays on a single PostgreSQL cluster, no sharding needed.

**Replication:**

- **Primary-replica (async)**: One primary handles all writes. 2 read replicas per shard for read traffic. Replication lag: ~10-100ms within same AZ. For the Pastebin use case, replication lag has less impact than in URL shortener because: (a) paste reads are not as latency-critical (50ms budget vs 20ms), (b) users typically don't read their own paste milliseconds after creation (they copy the URL from the creation response, which came from the primary's write result). Still, Write Service populates Redis cache on creation to handle any lag window.
- **Why not multi-primary**: Write rate (12/sec) is trivially handled by a single primary. Multi-primary adds write conflict resolution complexity with no benefit.
- **Replication lag mitigation**: After paste creation, Write Service sets Redis cache from the newly written data (not from the replica). This ensures the read path works even before replica catches up.

**Caching:**

- **What to cache**: (a) Paste metadata (paste_key → metadata JSON), 512 bytes each — cache all active pastes, ~186 MB for 365M rows at 512B. (b) Paste content for pastes < 64 KB — covers ~80% of pastes by count. (c) Rate limit counters. (d) Negative cache (NOT_FOUND) with 5-minute TTL. (e) Delete tokens for anonymous pastes (TTL = paste lifetime).
- **Strategy: Cache-aside (lazy loading) + Write-through on creation**. Lazy loading for the general case (most pastes are read infrequently). Write-through on creation (Write Service explicitly populates both metadata and content cache on first write) for the "just created" scenario.
- **Eviction: LRU** (`allkeys-lru`). Paste access patterns follow a power law — most pastes are read once or twice, a few are read millions of times. LRU correctly keeps recently-accessed pastes in cache. We also apply explicit TTLs matching paste expiry so expired pastes don't occupy cache slots.
- **Invalidation**: On paste deletion or metadata update (`PATCH`): Write Service calls `redis.delete("paste:meta:<key>")` and `redis.delete("paste:content:<key>")`. CDN invalidation via CloudFront API. Invalidation on metadata update is only needed for fields that affect the read response (visibility, password, language) — not for `view_count` (which we don't read from cache on every request).
- **Tool: Redis** — same reasoning as URL shortener: atomic operations (`INCR` for view counts), clustering, persistence, Lua scripting for atomic rate limiting. Memcached lacks Redis Cluster's automatic failover and has no persistence (delete tokens would be lost on restart, leaving anonymous users unable to delete their pastes).

**CDN:**

- **What's served from CDN**: The dominant read type is `/raw/<paste_key>` and `/paste/<paste_key>` for public pastes. These are immutable content (paste content cannot change after creation) — ideal CDN candidates. `Cache-Control: public, s-maxage=2592000` (30 days) for long-lived pastes. CDN also serves the web UI static assets.
- **Pull CDN**: CloudFront origin is our Read Service (not S3 directly) for `/paste/` and `/raw/` paths. This allows us to inject `X-Paste-Language`, handle password protection, and record view counts before caching. Alternatively, for `/raw/<paste_key>` of public pastes, configure S3 as the CDN origin directly — removes our service from the hot path entirely for the most common read type.
- **Cache invalidation on delete**: `cloudfront.createInvalidation(Paths=["/paste/<key>", "/raw/<key>"])`. Propagates in < 5 seconds.
- **Cache key optimization**: For password-protected pastes: `Cache-Control: private, no-store` — never cache at CDN or shared caches. For unlisted: `Cache-Control: private, max-age=3600` — cache in browser only.

**Interviewer Deep-Dive Q&A (Scaling):**

Q: How would you scale to 100x the current load (1.16M reads/sec vs current 116/sec)?
A: At 1.16M reads/sec: (1) **CDN becomes the primary server**: At CDN hit rate > 99% for public pastes, origin sees only ~11,600 QPS. CloudFront scales to millions of QPS — no change needed. Ensure public paste `Cache-Control` headers are set for maximum CDN effectiveness. (2) **Redis**: 1.16M QPS with 99% CDN hit → ~11,600 Redis reads/sec. Our cluster handles 300K ops/sec — fine. (3) **Read Service**: Scale to ~100 instances. Stateless — trivial. (4) **S3**: At 1% CDN miss on 1.16M = 11,600 S3 GETs/sec. With prefix sharding (e.g., `pastes/0/ ... pastes/15/` = 16 prefixes), each prefix sees ~725 QPS — well within S3's auto-scaled limit. (5) **PostgreSQL**: Cache miss rate on metadata ~5% = 580 QPS to DB. 5 read replicas handle this. Key change at 100x: add more CDN prefetch configuration and tune `s-maxage` to keep popular pastes in CDN longer.

Q: The analytics view count (`view_count` in PostgreSQL) is updated by a background batch job from Redis INCR. What's the consistency model and is it acceptable?
A: The `view_count` in PostgreSQL is an **approximate, eventually consistent** counter. The flow: (1) On each read, `redis.incr("paste:views:<key>")`. (2) Background job (every 30 seconds): reads all `paste:views:*` keys, batch-updates PostgreSQL `UPDATE pastes SET view_count = view_count + $delta WHERE paste_key = $key`. (3) Redis increments the delta since last flush. The consistency model: up to 30 seconds of staleness in PostgreSQL's `view_count`. For the user dashboard, this is acceptable — users don't expect real-time view counts; "updated within a minute" is fine. Risk: if a Redis node fails between flush cycles, ~30 seconds of view count increments are lost. This is acceptable for an approximate counter. For exact counts (if required), use Kafka + Flink + ClickHouse for exact-once analytics (same as URL shortener's analytics pipeline). The `view_count` in PostgreSQL serves as a fast approximate figure for the dashboard; the accurate figure comes from ClickHouse.

Q: How do you handle the public feed (`GET /api/v1/feed`) at scale — it requires scanning recent public pastes?
A: The public feed is served by: `SELECT paste_key, title, language, created_at, view_count FROM pastes WHERE visibility = 'public' AND is_active = TRUE ORDER BY created_at DESC LIMIT 50 OFFSET $offset`. The partial index `idx_pastes_public_feed ON pastes(created_at DESC) WHERE visibility='public' AND is_active=TRUE` makes this a pure index scan (no table heap access needed for the covered columns). At 365M rows with ~40% public = 146M rows, this index is ~1.5 GB. At high scale: (a) **Cache the feed**: The public feed changes with every new paste creation. Cache feed pages in Redis with a 60-second TTL. At 12 new pastes/second, the feed becomes stale instantly anyway — 60-second cache loses at most 720 entries per page refresh, acceptable for a public feed. (b) **Pre-compute feed**: Maintain a Redis Sorted Set `public:feed` keyed by `paste_key`, scored by `created_at` epoch. On paste creation, `ZADD public:feed <timestamp> <paste_key>`. Feed query: `ZREVRANGE public:feed 0 49`. On paste deletion: `ZREM public:feed <paste_key>`. Trim to 100K entries via `ZREMRANGEBYRANK public:feed 0 -100001` after each ZADD. This completely removes the DB query from the feed path.

Q: PostgreSQL is the source of truth for paste metadata, but Redis stores delete tokens for anonymous pastes. What if Redis loses data?
A: Redis persistence is configured with both RDB snapshots (every 60 seconds) and AOF (append-only file, `appendfsync everysec`). On Redis restart, AOF replay recovers all commands, losing at most 1 second of writes. The delete token's TTL is reset to the paste's remaining lifetime on recovery (Redis re-applies the TTL from the AOF). In the rare case of Redis data loss (AOF not synced, server crashes hard): the delete token is lost. Anonymous users who had stored the token can no longer delete their paste. Mitigation: (a) For anonymously created pastes with no user account, the system is designed with "fire and forget" semantics — the delete token is provided once and not recoverable (by design, same as a password). Users should save it. (b) If the paste contains PII and the user contacts support, manual deletion via admin API. (c) TTL ensures the paste expires naturally anyway (max 1 month for anonymous pastes). For the highest durability requirement, delete tokens could be stored in PostgreSQL instead of Redis: `INSERT INTO paste_delete_tokens (paste_key, token_hash, expires_at) VALUES (...)` — this adds a DB query on delete but improves durability. Trade-off is acceptable to implement for regulated environments.

Q: How would you add full-text search over paste content at scale?
A: Full-text search over 11 TB of paste content is a significant addition. Not in scope for MVP, but design considerations: (1) **Elasticsearch**: CDC stream from PostgreSQL (via Debezium) pushes new paste metadata to Elasticsearch. Content is indexed as a `text` field with standard analyzer (language-aware tokenization). At 1M pastes/day, Elasticsearch ingestion rate: ~12 documents/second — trivial. Storage: 11 TB of content would require ~22 TB of Elasticsearch storage (inverted index ~2x raw size) — expensive. Mitigate by indexing only the first 10 KB of each paste (covers most use cases) and filtering by `visibility=public`. (2) **PostgreSQL full-text search**: `tsvector` column on content — feasible for < 50M rows at < 1 GB total content; breaks down at 11 TB scale. (3) **ClickHouse full-text**: ClickHouse has substring search capabilities (`LIKE`, `match()`) on columnar string data; better than Elasticsearch for exact substring queries but weaker for fuzzy/ranked search. (4) **Privacy concern**: Full-text indexing of paste content indexes potentially sensitive user-submitted data. Only public pastes should be indexed. Unlisted pastes must be excluded from any search index.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Mitigation |
|---------|--------|------------|
| PostgreSQL primary goes down | All paste creation fails; reads continue from replica + Redis cache | RDS Multi-AZ automatic failover in ~30-60s; Write Service retries with exponential backoff; read path unaffected due to cache + replica |
| S3 unavailable (region outage) | Cache misses cannot serve content; only Redis-cached pastes served | Read Service returns cached content (cache hit rate ~80%); for cache misses, return HTTP 503 with Retry-After header; alert fires immediately; S3 outages are extremely rare (<1/year historically) |
| Redis cluster shard failure | Cache misses increase; fallback to DB + S3 | Redis Cluster auto-promotes replica in ~1-2s; Read Service circuit breaker detects failure; falls through to DB + S3; p99 latency degrades from 20ms to ~70ms but service remains up |
| CDN misconfiguration caches 404 | Users get stale 404 for valid pastes | S3 object and DB: never cache 404 responses (`Cache-Control: no-store` on 404). CDN must be configured not to cache 4xx. Monitoring: alert on CDN 4xx rate spike. |
| Key pool exhaustion | New pastes use fallback random generation | Alert at pool < 500K keys; fallback has < 0.01% collision probability per attempt; service continues; refill job auto-restarts via Kubernetes liveness probe |
| Write Service crash during paste creation | Paste content in S3 but metadata not in DB (orphaned objects) | Idempotency key handling: on retry, check if metadata exists before re-writing to S3. Orphaned objects: daily S3 inventory job cross-references against DB and deletes orphans older than 24h. |
| Kafka consumer lag (analytics) | View count freshness degrades | Kafka retains 7 days; consumer catches up. View count in Redis still increments correctly; only PostgreSQL flush is delayed. User-visible impact: view count on dashboard lags by more than 30s. |
| Abuse: massive paste creation (spam) | Storage cost, index bloat, rate limiting exhaustion | WAF at CDN blocks IP ranges; rate limiting at Write Service; content hash deduplication reduces storage impact; spam pastes auto-expire (max 1 month TTL for anonymous) |

### Failover Strategy

- **PostgreSQL**: Active-passive with hot standby in a different AZ. Patroni manages automatic promotion. RTO: ~60 seconds (detection + promotion + DNS update). RPO: 0 (synchronous replication to standby). After failover, Write Service reconnects via the Patroni-managed DNS CNAME (`postgres-primary.internal`) which automatically points to the new primary.
- **Redis Cluster**: Active-active across 3 primary shards + 3 replica shards (one replica per primary). Failover per shard: ~1-2 seconds (Redis Cluster election). During failover window, requests to failed shard fall back to DB + S3. RTO for Redis: ~2 seconds per shard, transparent to end users via graceful degradation.
- **S3**: S3 is inherently multi-AZ within a region (stores data across ≥ 3 AZs). Single-AZ failures are transparent. Region-level S3 outage: replicate critical content to a second region using S3 Cross-Region Replication. Primary region serves normally; secondary region is read-only standby for DR.
- **Read Service (application)**: Active-active. All instances serve traffic simultaneously. Instance failure: LB health check removes instance in ~10 seconds. Remaining instances handle load (each has >80% headroom normally). Auto-scaling replaces failed instance within 3-5 minutes.
- **RTO target: < 60 seconds** for write path, **< 10 seconds** for read path.
- **RPO target: 0** for paste metadata; **best-effort (< 30 seconds)** for view count analytics.

### Retries & Idempotency

- **S3 write retry**: S3 PUTs are idempotent (same key + same content = same object). Retry up to 5 times with exponential backoff (100ms, 200ms, 400ms, 800ms, 1600ms + ±20% jitter) on S3 `RequestTimeout`, `ServiceUnavailable`, or `SlowDown` errors. Do NOT retry on `AccessDenied` or `NoSuchBucket` (non-transient errors).
- **PostgreSQL insert retry**: The `INSERT INTO pastes ... ON CONFLICT (paste_key) DO NOTHING` makes creation idempotent for the same key. On transient DB errors (connection lost, deadlock), retry up to 3 times with 100ms exponential backoff.
- **Idempotency keys**: Client provides `Idempotency-Key: <uuid-v4>` header. Write Service stores `(idempotency_key → response_json)` in Redis with 24h TTL. On retry: return stored response without re-processing. This prevents double-creating pastes when client times out. Implementation: before processing, `SET idempotency:<key> "PROCESSING" NX EX 30` (lock for 30s); after success, `SET idempotency:<key> <response_json> EX 86400`.
- **Analytics at-least-once**: Kafka producer uses `acks=all` and `retries=MAX_INT` for exactly-once semantics in the producer. Flink consumer uses Kafka transactions + ClickHouse upsert on `event_id` for exactly-once in the consumer.

### Circuit Breaker

- **Redis circuit breaker** (in Read/Write Service):
  - Failure threshold: 50% of calls fail within a 10s sliding window (minimum 20 calls)
  - Open → Half-open: after 30 seconds
  - Behavior when open: fall through to PostgreSQL + S3 directly
  - Implementation: `sony/gobreaker` or `resilience4j`
- **S3 circuit breaker** (in Read Service):
  - Failure threshold: 30% of calls fail within 10s window
  - Behavior when open: return HTTP 503 with `Retry-After: 30` header; do NOT serve stale content (paste content could be expired or deleted — serving stale is a privacy risk)
  - Exception: Redis-cached content is served normally even when S3 circuit is open
- **PostgreSQL circuit breaker** (in both services):
  - Failure threshold: 5 consecutive failures OR 50% failure rate in 10s
  - Behavior when open: return HTTP 503 immediately — do not queue requests

---

## 9. Monitoring & Observability

| Metric | Tool | Alert Threshold | Why |
|--------|------|-----------------|-----|
| Read p99 latency | Datadog APM / CloudWatch | > 100 ms for 5 min | Core SLA; p99 target 50ms, 100ms = 2x slack |
| Write p99 latency | Datadog APM | > 500 ms for 5 min | Paste creation SLA |
| S3 GET latency p99 | CloudWatch S3 metrics | > 200 ms for 5 min | S3 latency spike indicates region degradation |
| S3 error rate | CloudWatch | > 0.5% 5xx for 2 min | S3 availability degradation |
| CDN cache hit rate | CloudFront metrics | < 80% for 15 min | Low hit rate = origin overload |
| Redis cache hit rate | Redis INFO keyspace_hits/misses | < 85% for 15 min | More cache misses = more S3 and DB calls |
| Redis memory usage | CloudWatch ElastiCache | > 80% maxmemory | Approaching eviction; scale up or reduce TTL |
| DB replication lag | pg_stat_replication | > 1000 ms | Risk of serving stale metadata |
| Paste creation QPS | Datadog custom metric | > 5× baseline | Potential spam campaign; tighten rate limits |
| 410 Gone rate | Datadog | > 10% of reads | Possible misconfiguration of expiry or cleanup over-aggression |
| Key pool size | Custom metric (SCARD) | < 500,000 | Near exhaustion of pre-generated keys |
| Anonymous delete token Redis key count | Custom metric | Drop > 20%/hour unexpectedly | Possible Redis key eviction under memory pressure |
| Content integrity failures (sha256 mismatch) | Application logs → Datadog | Any occurrence | Data corruption in S3 |
| CDN 404 cache events | CloudFront access logs | Any | CDN should not cache 404s; indicates config error |

### Distributed Tracing

**OpenTelemetry SDK** in all services. Trace context propagated via `traceparent` header (W3C spec) through Load Balancer → Read/Write Service → Redis → PostgreSQL → S3. S3 calls instrumented with the AWS SDK's OpenTelemetry integration.

Sampling strategy:
- 100% sampling for all error responses (4xx server errors, 5xx)
- 100% sampling for S3 fetch (cache miss path) — most important for latency analysis
- 1% sampling for Redis cache hit path (very high volume, low information density)
- 10% sampling for DB queries

Key spans:
- `read.redis_meta_lookup` — hit/miss/error, duration
- `read.redis_content_lookup` — hit/miss (with size), duration
- `read.s3_fetch` — duration, object size, error code
- `read.db_fallback` — query fingerprint, duration
- `write.s3_upload` — duration, content size
- `write.db_insert` — duration
- `write.cache_populate` — duration

Parent trace exported to **Jaeger** (self-hosted) or Datadog APM. Trace IDs included in HTTP response headers (`X-Trace-ID`) for customer support debugging.

### Logging

Structured JSON logging, shipped via Fluent Bit to Elasticsearch/Datadog Logs.

Log fields per request:
```json
{
  "timestamp": "2026-04-09T12:34:56.789Z",
  "level": "info",
  "trace_id": "abc123...",
  "service": "read-service",
  "instance_id": "i-0a1b2c3d4e",
  "paste_key": "aB3xZ9rQ",
  "operation": "read_paste",
  "cache_hit": false,
  "cache_layer": "s3_fallback",
  "content_size_bytes": 4096,
  "password_protected": false,
  "latency_ms": 47.3,
  "status_code": 200,
  "client_ip_hash": "sha256:a1b2c3...",  // not raw IP, GDPR compliant
  "country_code": "US",
  "user_agent_class": "browser"
}
```

Log retention: INFO logs → 30 days. WARN/ERROR logs → 90 days. Security-relevant logs (auth failures, rate limit hits) → 1 year.

Do NOT log: raw paste content, passwords (even wrong ones), full IP addresses (use SHA-256 hash), full user-agent strings (use classified category).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A | Option B | Chosen | Reason |
|----------|----------|----------|--------|--------|
| Content storage | PostgreSQL TEXT column | S3 object storage | S3 | 5x cheaper; scales to TB without DB impact; native CDN origin; lifecycle rules for auto-expiry; PostgreSQL TEXT/TOAST degrades at TB scale |
| Paste key length | 6 chars (56B keyspace) | 8 chars (218T keyspace) | 8 chars | 6 chars: 56B / 1M/day = 153K years fine, but 8 chars is standard for Pastebin UX and provides margin for deduplication key reuse without ever recycling |
| Content mutability | Mutable (in-place edit) | Immutable (fork to change) | Immutable | Immutability enables aggressive CDN caching (Content-Addressable, no invalidation needed for edits); simplifies consistency model; "fork" provides edit semantics without mutability |
| Delete mechanism for anonymous users | No delete (fire-and-forget) | Delete token at creation | Delete token | Users frequently need to delete accidentally-shared pastes; fire-and-forget creates user trust issues and privacy risks; delete token is a well-understood pattern (like a tracking number) |
| Public feed architecture | DB query with index | Redis Sorted Set | Redis Sorted Set (at scale) | DB index is sufficient initially; Redis Sorted Set is O(log N) ZADD + O(k) ZREVRANGE with no DB query; pre-compute at scale to reduce DB read replicas needed |
| View count accuracy | Exact synchronous DB counter | Approximate async Redis+batch | Approximate async | Synchronous DB increment adds ~5ms to every read; at 116 QPS this is 116 DB writes/sec on the read path — wasteful; approximate count (30s lag) is acceptable for UX |
| CDN cache behavior | Cache based on response headers only | Explicit CDN invalidation API on change | Both: headers + invalidation | Headers ensure correctness (TTL bounds); invalidation provides immediate propagation on explicit delete/update; defense-in-depth |
| Password protection storage | Client-side hash (HMAC in URL) | Server-side bcrypt hash in DB | Server-side bcrypt | Client-side hash leaks the password mechanism; server-side allows password changes (if we add PATCH /password later) and proper rate limiting on bcrypt comparison attempts |
| Analytics store | PostgreSQL view_count column only | ClickHouse time-series | Both: approximate counter in PostgreSQL + ClickHouse for time-series | PostgreSQL column gives fast dashboard snapshot; ClickHouse enables time-series queries (clicks per day), top referrers, country breakdown — not possible from a single counter |
| Key reuse after expiry | Never reuse (waste 0.00017% of keyspace) | Reuse after 90-day quarantine | Never reuse (effectively) | Keyspace is so large that reuse is unnecessary; complexity of quarantine tracking is not worth the marginal storage saving |

---

## 11. Follow-up Interview Questions (minimum 15 Q&As)

**Q1: How would you handle a sudden 10x traffic spike from a viral paste (e.g., a leaked document)?**

A: The response is multi-layered. (1) **CDN absorbs the bulk**: if the paste is public, CloudFront caches it at all PoPs within minutes. At 10x = 1,160 QPS to origin → 100% CDN hit rate for a hot paste means origin sees ~0 QPS for that paste. CloudFront scales automatically to millions of QPS. (2) **Auto-scaling**: if other pastes are also seeing traffic (not just one viral paste), ASG adds Read Service instances when CPU > 60%. Scales from 8 to 80 instances in ~10 minutes. (3) **Redis**: 1,160 QPS is trivial for our 32 GB cluster rated at 300K+ ops/sec. (4) **S3**: a single paste at 1,160 reads/sec is fine; S3 GET request rate limit is 5,500/sec per prefix (auto-scales further). (5) **If the paste is unlisted (not cached by CDN)**: service must handle 1,160 QPS directly. Redis caches the paste content (if < 64 KB) — this handles the load. Pre-warm: after detecting a spike (cache read count exceeds threshold), explicitly set a longer TTL on the Redis content key. (6) **DDoS concern**: if the traffic is malicious (attacker), WAF at CloudFront rate-limits IPs to 100 reads/minute. Legitimate viral traffic from diverse IPs is allowed through. (7) **Cost**: 1,160 reads/sec × 10 KB = 11.6 MB/s egress from CDN. At CloudFront pricing ~$0.085/GB, this is 1 GB/day = ~$2.55/day — trivial.

**Q2: How do you handle the scenario where a user submits a paste with sensitive data (API keys, passwords) — is there any PII detection?**

A: This is a product/compliance question with technical implementation. Options: (1) **Passive scanning**: run a background job that scans paste content for known secret patterns (regex for AWS access keys, private key PEM headers, credit card patterns, SSNs). Use open-source libraries like `detect-secrets` (Yelp) or `trufflehog`. Flag matching pastes for review or automatic notification. (2) **At-creation scanning**: in the Write Service, synchronously scan content before storing. At our scale (12 pastes/sec, ~10 KB each), regex scanning ~120 KB/sec is trivial. If a secret is detected: (a) warn the user ("This paste may contain sensitive credentials. Consider using private visibility."), (b) auto-set visibility to unlisted, (c) log the event for Trust & Safety. (3) **GitGuardian-style incident response**: if a paste is reported as containing credentials, auto-notify the affected service (GitHub has a similar program where they invalidate leaked OAuth tokens). Requires integrations with major cloud providers. (4) **GDPR PII detection**: detect names + emails + SSNs via NLP/regex in EU-submitted pastes; flag for compliance review. Privacy consideration: scanning content implies the system reads all user content. This must be disclosed in the Terms of Service and Privacy Policy.

**Q3: How would you implement pagination for the public feed without the N+1 problem or deep offset penalties?**

A: Standard `LIMIT N OFFSET M` pagination has O(M) cost — PostgreSQL scans and discards M rows to find the offset. At page 1000 (50 items/page = offset 50,000), this means scanning 50,000 index entries. Better alternatives: (1) **Keyset (cursor) pagination**: Instead of `OFFSET`, use `WHERE created_at < $cursor AND paste_key < $last_key ORDER BY created_at DESC, paste_key DESC LIMIT 50`. The cursor is the `created_at` + `paste_key` of the last item on the previous page, URL-encoded as a base64 opaque string. This is O(log N) because it starts scanning from the cursor position in the index — no skipping. (2) **Redis Sorted Set approach**: `ZREVRANGEBYSCORE public:feed $max_score $min_score LIMIT 0 50`. Client passes the last score (timestamp) as the cursor. O(log N + k). This is the preferred approach at scale since it removes DB from the feed entirely. Tradeoff: cursor-based pagination cannot jump to an arbitrary page (no "page 5 of 20") — only "next page". For a public feed this is acceptable (infinite scroll UX); for admin dashboards with page numbers, use `OFFSET` on an indexed column.

**Q4: A user reports that their paste URL is returning 410 Gone even though they set it to "never expire". How do you debug this?**

A: Systematic investigation: (1) **Database check**: `SELECT paste_key, expires_at, is_active FROM pastes WHERE paste_key = 'xyz'` on the primary. If `is_active = FALSE` and `expires_at` is NULL: the paste was explicitly soft-deleted (by the user, or incorrectly by a cleanup job). (2) **Cleanup job bug**: check if the cleanup cron recently ran with an incorrect predicate. Query: `SELECT count(*) FROM pastes WHERE is_active = FALSE AND expires_at IS NULL AND updated_at > NOW() - INTERVAL '1 hour'` — if unexpectedly high, the cleanup job has a bug (OFF by one on IS NULL check). Roll back the cleanup job; restore affected rows from a database backup. (3) **Redis tombstone**: `redis-cli GET paste:meta:xyz` — if it returns `"EXPIRED"` but DB says paste is active, there's a Redis/DB inconsistency. Fix: `redis-cli DEL paste:meta:xyz` and the next read will re-fetch from DB. (4) **Trace the request**: correlate `X-Trace-ID` from user's browser request with Jaeger — see which code path returned 410. (5) **S3 object**: `aws s3 ls s3://pastebin-content-prod/pastes/xyz` — if S3 object doesn't exist but DB says active: lifecycle rule fired incorrectly. Restore from S3 versioning (if enabled) or database backup.

**Q5: How do you prevent someone from storing 1 million pastes of 1 MB each, consuming 1 TB of your S3 storage for free?**

A: Multi-layer defense: (1) **Rate limiting at creation**: anonymous users: 10 pastes/hour per IP. Authenticated free users: 100 pastes/hour. Pro users: 10,000 pastes/hour. These limits make 1M pastes take at minimum 100,000 hours = 11+ years for a free user. (2) **Total storage quota per user**: authenticated users have a storage quota (e.g., 100 MB total for free, 10 GB for pro). Write Service checks `SUM(size_bytes) FROM pastes WHERE user_id = $id AND is_active = TRUE` against the quota. Cache this aggregate in Redis and update on each paste creation/deletion. (3) **Anonymous pastes have forced TTL**: anonymous users cannot create "never expire" pastes; maximum TTL for anonymous is 1 month. This ensures storage is self-cleaning. (4) **Content deduplication**: if the attacker submits the same 1 MB content repeatedly, deduplication (sha256 check) means only 1 S3 object is stored regardless of how many metadata rows exist. (5) **Billing alarm**: AWS cost monitoring alerts if S3 storage exceeds expected growth rate. Emergency: enable more aggressive rate limiting at WAF.

**Q6: How do you implement paste forking — specifically, how do you ensure the fork doesn't just reference the parent's S3 object (which might be deleted)?**

A: Two models for fork storage: (1) **Reference parent content** (copy-on-write semantics): the fork's `pastes` table row stores `fork_of = <parent_key>` and has no separate S3 object — it reads content from the parent's S3 object. Problem: if parent is deleted, fork loses content. Also, deduplication via sha256 is cleaner. (2) **Copy content on fork** (deep copy): Write Service copies the S3 object to a new key (`s3.copy_object(Source=parent_key, Dest=fork_key)`). S3 CopyObject is a server-side operation — O(1) time regardless of file size, no data transferred to/from our service. New paste row has its own S3 object, independent lifecycle. This is the correct approach. Implementation: at fork creation, call `s3.copy_object()` + insert new metadata row with `fork_of = parent_key`. Shallow change: if the user modifies content in the fork, the Write Service uploads new content to the fork's S3 key (overwrite). Since content is immutable after initial creation, "fork with modification" = upload modified content to the new paste's S3 key. Deduplication: if fork content == parent content (no change), sha256 will match and we can skip the S3 copy (single object serves both).

**Q7: How would you implement syntax highlighting? Should it be server-side or client-side?**

A: **Client-side rendering** (preferred). Paste content is served as `Content-Type: text/plain` — raw text. The web browser renders it in a `<pre>` tag and a JavaScript library (Prism.js or Highlight.js) applies syntax highlighting based on the `language` metadata returned in the HTTP response header (`X-Paste-Language: python`). Reasons to prefer client-side: (a) Server-side highlighting requires running a syntax highlighter (Pygments, Rouge) on every read — adds ~10-50 ms per paste read, eliminating our ability to serve raw content from CDN (CDN would cache the highlighted HTML, but different clients want different themes). (b) Client-side allows users to choose their color theme (dark/light mode). (c) Server-side highlighted HTML cannot be cached at CDN for anonymous users with different theme preferences. (d) The raw text format is universal — API clients, CLI tools (`curl /raw/<key>`), and web UI all get the same base content; only the web UI applies highlighting. Exception: server-side highlighting for PDF export or print functionality (where JS is unavailable) — pre-generate highlighted HTML on demand and cache in S3 (`pastes/highlighted/<key>.html`).

**Q8: How do you handle a request to serve a paste that's exactly 1 MB — what are the performance implications?**

A: A 1 MB paste read has specific performance implications: (1) **S3 latency**: S3 GET for a 1 MB object takes ~50-150 ms at p50 (network transfer time for 1 MB at ~100 MB/s sustained throughput over HTTPS: ~10 ms transfer, but TTFB + handshake adds ~50-100 ms). (2) **Redis**: We don't cache pastes > 64 KB in Redis content cache (to prevent a few large pastes from evicting thousands of small ones). Metadata (512 bytes) is still cached in Redis. (3) **CDN**: CloudFront has a default object size limit of 20 GB — 1 MB is fine. CDN caches the full 1 MB at each PoP on first miss; subsequent requests serve from CDN memory. (4) **Response assembly**: Redirect Service reads 1 MB from S3, writes to HTTP response buffer. At 116 QPS with 5% being 1 MB pastes = ~6 large reads/sec. Total bandwidth: 6 × 1 MB = 6 MB/s — manageable. Instance memory: each Read Service instance needs at least 2× the largest paste in flight (1 MB × concurrent requests × 2 = 1 MB × 100 concurrent × 2 = 200 MB buffer). With 4 GB instance RAM, this is fine. (5) **Client-side**: Browser streams the response; syntax highlighting via Prism.js on a 1 MB file can take 500ms-2s of JavaScript execution on a slow device. Consider lazy loading syntax highlighting for large pastes (load first 10 KB immediately, rest incrementally).

**Q9: How do you implement the "download as file" feature — e.g., download a paste as `my-config.yaml`?**

A: This is a response header change. When a download is requested (via UI button or query parameter `?download=1`): Read Service responds with: `Content-Disposition: attachment; filename="<title_or_paste_key>.<language_extension>"`. For example, a Python paste: `Content-Disposition: attachment; filename="aB3xZ9rQ.py"`. The `Content-Type` header changes from `text/plain` to `application/octet-stream` to force browser download rather than in-browser rendering. The URL `/raw/<paste_key>?download=1` is distinct from `/raw/<paste_key>` in CDN cache keys (CloudFront uses query strings in cache key if configured). Cache the download version separately if needed. Implementation: language tag → file extension mapping table (Python → `.py`, SQL → `.sql`, Bash → `.sh`, JSON → `.json`, etc.) stored in application config. For unknown language → default to `.txt`.

**Q10: What's your strategy for handling multi-region deployments — users in Asia have 200ms latency to a US-based service?**

A: Multi-region deployment strategy: (1) **CDN solves the read path globally**: CloudFront has PoPs in Asia (Tokyo, Singapore, Mumbai, Sydney). Once a paste is cached at the nearest PoP, Asia reads get ~10-30 ms CDN latency regardless of where the origin is. For the vast majority of reads (public popular pastes), multi-region deployment of origin is not needed. (2) **Write path (paste creation)**: Paste creation involves a write to PostgreSQL (primary in US-East). Asia users experience ~200ms RTT to the primary. Options: (a) Accept the latency — creation is a one-time action, 200ms is barely perceptible and within our 300ms p99 budget. (b) Deploy regional Write Service instances with a regional PostgreSQL primary and cross-region replication to the global primary. Conflict resolution: last-write-wins on paste_key (extremely rare collision given random keys across regions). (c) Use a globally distributed database (CockroachDB, Spanner) with regional primary affinity. CockroachDB: writes commit in ~6 ms intra-region, cross-region commits use Raft consensus adding ~100-200 ms RTT. Complex but correct. (3) **GDPR compliance**: EU region must have a dedicated PostgreSQL cluster in EU with no data sync to US for EU user data. This requires geo-routing (Route53 Geolocation) to ensure EU users hit the EU origin, not the US origin.

**Q11: How would you design A/B testing for a new redirect format (e.g., testing whether a confirmation page before redirect reduces abuse)?**

A: A/B testing at the infrastructure level: (1) **Feature flag at CDN**: CloudFront Functions (lightweight JS at edge) can read a user's bucket assignment cookie and modify the request before forwarding to origin — routing bucket A to `/redirect-v1/<key>` and bucket B to `/redirect-v2/<key>`. (2) **Assignment service**: users (by IP hash for anonymous, by user_id for authenticated) are assigned to buckets. Assignment stored in Redis (`SET ab:bucket:<user_hash> <bucket> EX 604800` — 7-day TTL for stable assignment). (3) **Measurement**: click_events in Kafka tagged with `experiment_variant: A|B`. Flink computes conversion rates per variant. ClickHouse aggregates for statistical significance testing. (4) **Guardrails**: monitor redirect p99 latency per variant (ensure variant B doesn't add latency), bounce rate, error rates. Automatic rollback if variant B shows >10% degradation. (5) **For Pastebin specifically**: a confirmation page before redirect adds ~500ms round-trip. Statistical test: measure 7-day abuse reports per 1000 redirects per variant. Hypothesis: confirmation page reduces malware/phishing clickthroughs. Minimum detectable effect: 10% reduction at 95% confidence. Sample size calculator determines how many impressions needed before a decision.

**Q12: How do you handle UTF-8 vs binary content? What if someone tries to paste a binary file (like a compiled binary)?**

A: (1) **Content-Type enforcement**: We only accept `Content-Type: text/*` or JSON/YAML/XML at the API layer. Binary content is rejected with HTTP 415 Unsupported Media Type. (2) **UTF-8 validation**: Before storing, validate that the content is valid UTF-8 using the language's standard library (Go: `utf8.Valid([]byte(content))`, Python: `content.encode('utf-8')` — catches encoding errors). Invalid UTF-8 → HTTP 400 Bad Request with error `invalid_encoding`. (3) **Why reject binary**: (a) Syntax highlighting is meaningless for binary. (b) Browser rendering of binary content is unreliable and security-risky (could trigger browser vulnerabilities). (c) Storage cost — binary files are often much larger than text; deduplication via sha256 is less effective for unique binaries. (4) **Use cases that may need binary-like content**: Base64-encoded data is valid UTF-8 and is accepted. Users who want to share binary data should use a file hosting service (S3 presigned URL, etc.), not a text pastebin. (5) **Null byte detection**: even with valid UTF-8, reject content containing null bytes (`\x00`) — these can cause issues in logging pipelines and some text renderers, and are often indicators of binary content or injection attempts.

**Q13: How would you implement paste templates — pre-filled starter content for common use cases (e.g., "New Python script", "SQL schema template")?**

A: This is a product feature with minimal infrastructure changes: (1) **Static templates**: store templates in a configuration file (YAML/JSON) in the application repository, served as a static JSON endpoint `GET /api/v1/templates`. Template object: `{id, name, language, description, content}`. Load at startup, cache in Redis. No DB table needed. (2) **Template selection in UI**: when user selects a template, pre-populate the paste creation form with the template content. Template content is not stored — it's just a UI prefill. The resulting paste is a normal paste with no special link to the template. (3) **Community templates**: if users can submit templates: store in a `templates` PostgreSQL table (`id, name, language, content, author_user_id, upvotes, is_approved`). Moderation queue for approval. Only approved templates surfaced in the list. (4) **Template usage stats**: track which templates are used via a `template_id` field in the `pastes` table (nullable). Aggregate in ClickHouse for "most popular templates" feature. (5) **CDN**: `GET /api/v1/templates` response is public and static (changes infrequently) — cache at CDN with `Cache-Control: public, max-age=3600`. Invalidate on template addition/update.

**Q14: How would you design a "clipboard sync" feature — the same paste content accessible across a user's devices in real-time?**

A: Real-time sync across devices requires a push mechanism, not polling: (1) **Architecture**: User creates paste → server stores → publishes event to a per-user channel in Redis Pub/Sub (`paste:user:<user_id>:clipboard`). Each of the user's other devices maintains a WebSocket connection to a Sync Service, subscribed to their user channel. On event received, device fetches the new paste content. (2) **WebSocket service**: Stateful (each WebSocket connection has a user_id). Cannot be fully stateless like the Read Service. Sticky sessions at LB (session persistence by user_id hash) OR use Redis Pub/Sub as the bus (all Sync Service instances subscribe to the same Redis channel, broadcast to their connected clients). (3) **Scale**: at 10M DAU with average 1.5 devices per user = 15M WebSocket connections. Each WebSocket uses ~32 KB of memory per open connection. 15M × 32 KB = 480 GB RAM — not feasible on a few large instances. Use a specialized WebSocket service (Phoenix Channels, Ably, Pusher, or AWS API Gateway WebSocket) that's optimized for long-lived connections with low memory per connection. Phoenix Channels: ~2 KB/connection → 15M × 2 KB = 30 GB. (4) **Persistence**: if a device is offline when the sync event fires, it misses the pub/sub event. Solution: on device reconnect, fetch the user's recent paste list via `GET /api/v1/pastes?since=<last_sync_timestamp>` (a polling fallback for the offline gap).

**Q15: What cost optimizations would you make if the company needed to cut infrastructure costs by 50%?**

A: Cost breakdown and optimization levers: (1) **S3 storage ($252/month for 11 TB)**: Use S3 Intelligent-Tiering — objects not accessed for 30 days auto-move to S3-IA ($0.0125/GB vs $0.023/GB Standard). Pastes accessed rarely (80% of pastes after 7 days) could save ~40% on storage. Estimated saving: ~$100/month. (2) **CDN egress**: CloudFront charges ~$0.085/GB egress. At 5.78 MB/s peak, ~500 GB/day × $0.085 = ~$42.50/day at peak. Optimize: increase CDN cache TTL (`s-maxage=2592000` for long-lived pastes), reducing origin egress from CDN. Also, CloudFront to S3 egress is free (same AWS network) — configure CloudFront to use S3 origin directly for `/raw/*` to eliminate our service's egress charges. (3) **EC2 instances**: Redirect Service runs on on-demand. Switch to Spot Instances for stateless services — up to 70% cost reduction. Use Reserved Instances for the 2 always-on minimum instances. Estimated saving: 40% on EC2 costs. (4) **RDS**: right-size read replicas. At 87 QPS cache-miss reads, 2 read replicas are sufficient. Run primary on a smaller instance (db.r6g.large instead of db.r6g.2xlarge). Use Aurora instead of RDS PostgreSQL — Aurora auto-scales storage and can scale down to $0.10/ACU-hour in Aurora Serverless v2 during off-peak. (5) **Redis**: use ElastiCache Reserved Instance pricing (1-year reserved, ~30% discount). Right-size: 32 GB cluster may be oversized — monitor actual memory usage and right-size to 20 GB if working set fits. (6) **Remove analytics ClickHouse cluster**: simplify to Redis counters + PostgreSQL aggregates only if detailed analytics are not a product requirement. ClickHouse cluster is likely $500-2000/month — removing it saves significantly. Add back when analytics becomes a paid feature.

---

## 12. References & Further Reading

- **"Designing Data-Intensive Applications" — Martin Kleppmann** (O'Reilly, 2017) — Chapter 10 on batch processing (analytics pipeline), Chapter 5 on replication (primary-replica, replication lag), Chapter 3 on storage engines (B-tree indexes for PostgreSQL lookup patterns)
- **"System Design Interview, Volume 1" — Alex Xu** — Chapter on Pastebin-like system and URL shortener; foundational capacity estimation framework used in this document
- **Pastebin.com Architecture** — Grokking the System Design interview course (Educative.io) — documents the reference architecture for a Pastebin-like service
- **Amazon S3 Developer Guide: Best Practices Design Patterns** — docs.aws.amazon.com/AmazonS3/latest/userguide/optimizing-performance.html — documents request rate optimization via prefix sharding, S3 lifecycle rules, and CopyObject performance
- **Amazon CloudFront Developer Guide: Cache-Control Headers** — docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide — covers s-maxage, Cache-Control behavior, and invalidation API limits
- **Redis Cluster Specification** — redis.io/docs/reference/cluster-spec — documents hash slot assignment, shard failover algorithm, and SPOP atomicity guarantees
- **PostgreSQL Documentation: Partial Indexes** — postgresql.org/docs/current/indexes-partial.html — foundation for the `idx_pastes_public_feed` and `idx_pastes_expires_at` partial index designs
- **PostgreSQL Documentation: TOAST** — postgresql.org/docs/current/storage-toast.html — explains TOAST (The Oversized-Attribute Storage Technique) and why large TEXT columns in PostgreSQL degrade read performance — justifies using S3 for content storage
- **"Probabilistic Early Expiration (XFetch)" — Vattani, Chierichetti, Panagiotou, 2015** — academic paper on the thundering herd prevention algorithm for cache expiry; available via ACM Digital Library
- **detect-secrets — Yelp Engineering** — github.com/Yelp/detect-secrets — open-source Python library for scanning content for secrets patterns; referenced in the PII/secret detection section
- **"The Tail at Scale" — Dean & Barroso, Google (2013)** — Communications of the ACM, 56(2) — explains hedged requests and tied requests for reducing tail latency; applicable to the S3 fetch optimization
- **W3C Trace Context Specification** — w3.org/TR/trace-context — defines the `traceparent` header format used in distributed tracing across Read/Write Service → Redis → PostgreSQL spans
- **"Building Microservices, 2nd edition" — Sam Newman** (O'Reilly, 2021) — Chapter on circuit breaker pattern; foundational for the Redis and S3 circuit breaker implementation
- **"Database Internals" — Alex Petrov** (O'Reilly, 2019) — Chapter on B-tree index implementations in PostgreSQL; relevant to understanding index scan performance at 365M rows
- **Keyset Pagination: "We need tool support for keyset pagination" — Markus Winand** — use-the-index-luke.com/no-offset — explains why OFFSET pagination is O(N) and keyset pagination is O(log N) for the public feed implementation
