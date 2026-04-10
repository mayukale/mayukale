# System Design: URL Shortener

---

## 1. Requirement Clarifications

### Functional Requirements
- Given a long URL, generate a unique short URL (e.g., `https://short.ly/aB3xZ9`)
- Redirecting a short URL to its original long URL (HTTP 301 or 302)
- Custom alias support: users may optionally specify their own short code
- Link expiration: URLs can be configured to expire after a set time or date
- Analytics: track click count, referrer, user-agent, geo-location per short URL
- User accounts: authenticated users can manage (view, edit, delete) their links
- Bulk creation: API clients can create multiple short URLs in a single request

### Non-Functional Requirements
- **High Availability:** the redirect path must be available 99.99% of the time (~52 min downtime/year); write path can tolerate 99.9%
- **Low Latency:** redirect p99 < 20 ms; creation p99 < 200 ms
- **Durability:** once a short URL is created, it must never silently disappear; explicit deletes are allowed
- **Consistency:** eventual consistency is acceptable for analytics; strong consistency required for uniqueness of short codes (no two long URLs mapped to the same code, no two codes pointing to different URLs without intent)
- **Read-heavy workload:** read:write ratio approximately 100:1
- **Scalability:** must handle up to 10x traffic spikes without manual intervention
- **Security:** prevent enumeration attacks, abuse (malware URLs), and SSRF via URL validation

### Out of Scope
- Full-featured URL management dashboard / web UI (assume API-first)
- Real-time streaming analytics pipeline (batch analytics is sufficient for MVP)
- Built-in A/B testing or link-in-bio pages
- QR code generation
- Password-protected links
- Multi-region active-active failover (single-region primary with DR replica is sufficient for scope)

---

## 2. Users & Scale

### User Types
- **Anonymous users:** can create short URLs (rate-limited); cannot manage them after creation
- **Authenticated users:** create, view, edit, delete, and see analytics for their links
- **API clients / developers:** programmatic bulk creation via API key auth
- **Link visitors (redirectees):** end users who click short links — the dominant traffic source

### Traffic Estimates

| Metric | Value | Reasoning |
|--------|-------|-----------|
| DAU (active redirects) | 100 million | Assumption: mid-size service comparable to early Bitly; 100M daily redirect events |
| Registered users | 50 million | Assumption: ~50% of redirect DAU have accounts |
| New URLs created/day | 1 million | Assumption: 1% of DAU create ~1 link/day each |
| Reads/sec (redirect QPS) | ~1,157 | 100M redirects/day ÷ 86,400 s/day ≈ 1,157 QPS |
| Writes/sec (creation QPS) | ~12 | 1M URLs/day ÷ 86,400 s/day ≈ 11.6 QPS |
| Peak multiplier | 5x | Viral links, marketing campaigns; peak redirect QPS ≈ 5,785 |
| Read:Write ratio | ~100:1 | 1,157 reads/s ÷ 11.6 writes/s |

**Assumptions stated explicitly:**
- 100M DAU is a working assumption; if the interviewer says 1B, multiply estimates by 10.
- "Day" is modeled as uniform; real traffic is bursty (business hours, time zones).
- Analytics writes are piggy-backed asynchronously; not counted in synchronous QPS.

### Latency Requirements
- **Redirect p99 < 20 ms** — This is the critical path. A short URL's entire value proposition is speed. Anything over 100 ms is noticeable to users and hurts the click-through experience. 20 ms p99 allows for cache lookup (~1 ms Redis) + network RTT (~5 ms within region) + HTTP overhead, with headroom.
- **Creation p99 < 200 ms** — Write path is not latency-sensitive for end users. 200 ms is acceptable for an interactive form submission or API call. This budget covers DB write + ID generation + optional async analytics enqueue.
- **Analytics read p99 < 500 ms** — Dashboard queries; not on the hot redirect path.

### Storage Estimates

| Data Type | Size/record | Records/day | Retention | Total |
|-----------|-------------|-------------|-----------|-------|
| URL mapping record | ~500 bytes | 1,000,000 | 5 years | ~915 GB |
| Click analytics event | ~200 bytes | 100,000,000 | 1 year | ~7.3 TB |
| User account record | ~256 bytes | ~5,000 new/day | 5 years | ~2.3 GB |

**URL mapping calculation:**
- 500 bytes × 1M records/day × 365 days × 5 years = 500 × 1,825,000,000 = ~912 GB ≈ ~1 TB with indexes

**Analytics calculation:**
- 200 bytes × 100M clicks/day × 365 days = ~7.3 TB/year
- Assumption: analytics older than 1 year is archived to cold storage (S3 Glacier)

**Working set for cache:**
- Hot URLs (80% of traffic hits 20% of links — Pareto): 20% of active URLs
- Active URLs ≈ 30-day window ≈ 30M records × 500 bytes = ~15 GB working set
- Redis cluster with 20 GB RAM covers the hot working set comfortably

### Bandwidth Estimates

| Direction | Calculation | Result |
|-----------|-------------|--------|
| Ingress (URL creation) | 11.6 writes/s × 500 bytes/record | ~5.8 KB/s |
| Egress (redirect responses) | 1,157 reads/s × 500 bytes (HTTP 301 response) | ~580 KB/s |
| Analytics ingress | 1,157 events/s × 200 bytes | ~231 KB/s |
| Peak egress (5x) | 5,785 reads/s × 500 bytes | ~2.9 MB/s |

Bandwidth is not a bottleneck at these numbers; even at 10x peak it is ~30 MB/s, well within typical datacenter capacity.

---

## 3. High-Level Architecture

```
                          ┌──────────────────────────────────────────────────────┐
                          │                    INTERNET                          │
                          └──────────────┬───────────────────────────────────────┘
                                         │
                          ┌──────────────▼───────────────┐
                          │         DNS (Route53)         │
                          │  short.ly → CDN/LB IP        │
                          └──────────────┬───────────────┘
                                         │
                    ┌────────────────────▼────────────────────┐
                    │              CDN (CloudFront)            │
                    │  - Caches 301 redirects for popular URLs │
                    │  - Static assets (analytics UI)          │
                    │  - TLS termination at edge               │
                    └───────────┬─────────────────────────────┘
                                │ Cache miss → origin
                    ┌───────────▼──────────────┐
                    │    Load Balancer (L7)     │
                    │  - Layer 7 (HTTP/HTTPS)   │
                    │  - Health checks          │
                    │  - Sticky sessions (none) │
                    └─────┬──────────┬──────────┘
                          │          │
              ┌───────────▼──┐  ┌────▼──────────────┐
              │  Redirect    │  │  Write / API       │
              │  Service     │  │  Service           │
              │  (stateless) │  │  (stateless)       │
              │  N instances │  │  N instances       │
              └──────┬───────┘  └────────┬───────────┘
                     │                   │
        ┌────────────▼──────┐   ┌────────▼──────────────────┐
        │  Redis Cluster    │   │  ID Generator Service      │
        │  (URL cache)      │   │  (Snowflake-style or       │
        │  ~20 GB hot set   │   │   pre-generated token pool)│
        └────────┬──────────┘   └────────┬──────────────────┘
                 │                       │
        ┌────────▼───────────────────────▼──────┐
        │        Primary Database (PostgreSQL)   │
        │        - URL mappings                 │
        │        - User accounts                │
        │        - Expiry metadata              │
        └────────────────────┬──────────────────┘
                             │  async CDC / replication
              ┌──────────────▼──────────────┐
              │  Analytics Pipeline          │
              │  Kafka → Flink → ClickHouse  │
              │  (click events, aggregates)  │
              └─────────────────────────────┘
```

**Component roles:**

- **DNS (Route53):** Maps `short.ly` to CDN/LB IPs. Low TTL (~60s) for fast failover. Weighted routing for canary deployments.
- **CDN (CloudFront):** For the redirect service, caches `HTTP 301` responses for popular short codes at edge nodes globally. This means viral links never hit the origin — the CDN absorbs traffic. Also serves the analytics dashboard's static assets.
- **Load Balancer (L7):** Distributes traffic across Redirect Service and Write/API Service instances. L7 allows path-based routing (`/api/*` → Write Service, everything else → Redirect Service). No sticky sessions needed because all services are stateless.
- **Redirect Service:** The hot path. Receives `GET /<code>`, looks up Redis, falls back to DB, returns HTTP 301/302. Must be extremely lightweight — no business logic beyond lookup + redirect.
- **Write / API Service:** Handles `POST /shorten`, `DELETE`, analytics queries, user auth. More logic-heavy but lower QPS. Rate limiting enforced here.
- **Redis Cluster:** Caches the `short_code → long_url` mapping for hot links. Sub-millisecond reads. Cluster mode with sharding across 3+ nodes for capacity and fault tolerance.
- **ID Generator Service:** Produces unique short codes. Decoupled so generation strategy can change independently. Options explored in Section 6.
- **PostgreSQL (Primary):** Source of truth for all URL mappings, users, expiry data. Chosen over NoSQL because URL records are structured, reads are key-value (by short_code), and we need ACID for uniqueness guarantees. Read replicas serve analytics queries.
- **Analytics Pipeline:** Kafka receives click events asynchronously from Redirect Service (fire-and-forget). Flink aggregates in micro-batches. ClickHouse (columnar OLAP) serves dashboard queries. Decoupled from redirect hot path — a Kafka lag does not affect redirect latency.

**Primary use-case data flow (redirect):**

1. User clicks `https://short.ly/aB3xZ9`
2. Browser → DNS resolves `short.ly` → CDN IP
3. CDN checks its cache for key `aB3xZ9`. If hit: returns `HTTP 301 Location: <long_url>` directly from edge. Done in ~5 ms globally.
4. CDN miss: request forwarded to Load Balancer → Redirect Service instance
5. Redirect Service checks Redis for key `aB3xZ9`. If hit: returns `HTTP 302 Location: <long_url>`. Done in ~10 ms.
6. Redis miss: Redirect Service queries PostgreSQL replica: `SELECT long_url, expires_at FROM urls WHERE short_code = 'aB3xZ9'`
7. If found and not expired: populate Redis cache (TTL = 24h), return `HTTP 302`
8. If not found: return `HTTP 404`
9. Asynchronously (non-blocking): Redirect Service publishes click event `{code, timestamp, ip, user_agent, referrer}` to Kafka topic `click-events`

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- USERS TABLE
-- ============================================================
CREATE TABLE users (
    id              BIGINT          PRIMARY KEY,          -- Snowflake ID
    email           VARCHAR(320)    NOT NULL UNIQUE,
    password_hash   CHAR(60)        NOT NULL,             -- bcrypt hash
    api_key         CHAR(32)        UNIQUE,               -- hex token for API access
    plan            VARCHAR(20)     NOT NULL DEFAULT 'free', -- free | pro | enterprise
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_users_api_key ON users(api_key);
CREATE INDEX idx_users_email   ON users(email);

-- ============================================================
-- URLS TABLE  (core mapping table)
-- ============================================================
CREATE TABLE urls (
    id              BIGINT          PRIMARY KEY,          -- Snowflake ID
    short_code      VARCHAR(12)     NOT NULL UNIQUE,      -- e.g. 'aB3xZ9'
    long_url        TEXT            NOT NULL,             -- original URL, up to ~2048 chars
    user_id         BIGINT          REFERENCES users(id) ON DELETE SET NULL,
    custom_alias    BOOLEAN         NOT NULL DEFAULT FALSE,
    expires_at      TIMESTAMPTZ,                          -- NULL = never expires
    is_active       BOOLEAN         NOT NULL DEFAULT TRUE,
    click_count     BIGINT          NOT NULL DEFAULT 0,   -- denormalized counter (updated async)
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- Primary lookup: short_code → long_url (the redirect hot path)
CREATE UNIQUE INDEX idx_urls_short_code ON urls(short_code);

-- User's link list (dashboard)
CREATE INDEX idx_urls_user_id_created ON urls(user_id, created_at DESC);

-- Expiry cleanup job
CREATE INDEX idx_urls_expires_at ON urls(expires_at) WHERE expires_at IS NOT NULL;

-- ============================================================
-- CLICK EVENTS TABLE  (write-optimized; bulk-inserted from Kafka consumer)
-- In practice this lives in ClickHouse, but modeled here for reference
-- ============================================================
CREATE TABLE click_events (
    id              BIGINT          PRIMARY KEY,
    short_code      VARCHAR(12)     NOT NULL,
    clicked_at      TIMESTAMPTZ     NOT NULL,
    ip_address      INET,
    country_code    CHAR(2),                              -- derived from IP via MaxMind GeoIP2
    city            VARCHAR(100),
    user_agent      TEXT,
    device_type     VARCHAR(20),                          -- mobile | desktop | tablet | bot
    referrer        TEXT,
    FOREIGN KEY (short_code) REFERENCES urls(short_code)
);

-- Time-series queries: "clicks per day for code X in the last 30 days"
CREATE INDEX idx_click_events_code_time ON click_events(short_code, clicked_at DESC);

-- ============================================================
-- RATE_LIMIT TABLE  (alternatively handled in Redis; shown here for completeness)
-- ============================================================
CREATE TABLE rate_limit_buckets (
    key             VARCHAR(128)    PRIMARY KEY,          -- e.g. 'anon:ip:1.2.3.4'
    tokens          SMALLINT        NOT NULL,
    last_refill     TIMESTAMPTZ     NOT NULL
);
-- In practice, this is kept entirely in Redis (INCR + EXPIRE), not PostgreSQL.
```

**Index strategy rationale:**
- `idx_urls_short_code` is the most performance-critical index — every redirect hits it. It is a B-tree unique index, giving O(log N) lookup on disk and O(1) in memory (PostgreSQL buffer pool).
- `click_events` in production moves to ClickHouse with a `MergeTree` engine ordered by `(short_code, clicked_at)` for fast range scans on time-series analytics.
- `idx_urls_expires_at` is a partial index (only non-NULL rows) — keeps it small for the background cleanup cron.

### Database Choice

- **Option A: PostgreSQL** — Pros: ACID transactions (critical for unique short_code constraint), mature ecosystem, rich index types (B-tree, hash, partial), JSONB for flexible metadata, excellent read replica support, point-in-time recovery. Cons: Vertical scaling limits (~50M rows before read replicas become mandatory), schema migrations require care at scale, not ideal for analytics (row-oriented).
- **Option B: DynamoDB** — Pros: Serverless, near-infinite horizontal scale, single-digit millisecond reads at any scale, managed. Cons: No ACID transactions across items (though DynamoDB Transactions exist, they are expensive and limited), no secondary indexes with strong consistency, limited query flexibility (no range queries on arbitrary columns), vendor lock-in, expensive at high read rates compared to self-managed Redis+PostgreSQL.
- **Option C: Cassandra** — Pros: Extremely high write throughput, linear horizontal scalability, multi-datacenter replication built-in. Cons: Eventual consistency by default (uniqueness constraint on short_code cannot be guaranteed without lightweight transactions, which are slow), no foreign keys, poor for analytics, operational complexity.

**Selected: PostgreSQL**

Reasoning specific to this system: the dominant operation is a point-lookup by `short_code` — a single-row read by primary key equivalent. PostgreSQL handles this at >100K QPS with connection pooling (PgBouncer) and read replicas. The URL table is expected to reach ~200M rows after 5 years (1M/day × 1825 days), which is well within PostgreSQL's tested scale range. The uniqueness guarantee on `short_code` is a hard requirement (two clicks on the same code must always go to the same URL); PostgreSQL's UNIQUE constraint enforces this atomically. Analytics is handled separately by ClickHouse, so PostgreSQL does not need to be the analytics store.

---

## 5. API Design

### Authentication
- **Anonymous:** No auth, rate-limited by IP (10 creates/hour via Redis token bucket)
- **Authenticated:** Bearer JWT in `Authorization` header; 15-minute expiry, refresh token in HttpOnly cookie
- **API clients:** `X-API-Key: <32-char-hex>` header; per-key rate limits defined by plan

### Endpoints

```
POST /api/v1/shorten
Description: Create a new short URL
Auth: Optional (anonymous or authenticated)
Rate limit: 10/hr anonymous, 1000/hr free, 10000/hr pro
Headers:
  Authorization: Bearer <jwt>  (optional)
  X-API-Key: <key>             (optional, alternative auth)
Request:
  {
    "long_url":     string (required, max 2048 chars, must be valid URL),
    "custom_alias": string (optional, 4-20 chars, alphanumeric + hyphens),
    "expires_at":   string (optional, ISO 8601 datetime),
    "tags":         string[] (optional, for user organization)
  }
Response 201 Created:
  {
    "short_url":   string,   // e.g. "https://short.ly/aB3xZ9"
    "short_code":  string,   // e.g. "aB3xZ9"
    "long_url":    string,
    "expires_at":  string | null,
    "created_at":  string
  }
Response 400 Bad Request:
  { "error": "invalid_url", "message": "The provided URL is not valid" }
Response 409 Conflict:
  { "error": "alias_taken", "message": "Custom alias 'my-link' is already in use" }
Response 429 Too Many Requests:
  { "error": "rate_limit_exceeded" }
  Headers: X-RateLimit-Limit: 10, X-RateLimit-Remaining: 0, X-RateLimit-Reset: 1712700000
```

```
GET /<short_code>
Description: Redirect to original long URL
Auth: None
Request: No body; short_code in URL path
Response 301 Moved Permanently (for permanent links, CDN-cacheable):
  Headers: Location: <long_url>
           Cache-Control: public, max-age=86400
Response 302 Found (for expiring links or analytics-tracked links):
  Headers: Location: <long_url>
           Cache-Control: no-store
Response 404 Not Found:
  { "error": "not_found", "message": "Short URL not found or expired" }
Response 410 Gone:
  { "error": "expired", "message": "This link has expired" }
```

```
GET /api/v1/urls/{short_code}
Description: Get metadata for a short URL (owner only)
Auth: Required (Bearer JWT); must be owner of the link
Response 200 OK:
  {
    "short_code":  string,
    "long_url":    string,
    "custom_alias": boolean,
    "expires_at":  string | null,
    "is_active":   boolean,
    "click_count": integer,
    "created_at":  string,
    "updated_at":  string
  }
Response 403 Forbidden: if caller is not the owner
Response 404 Not Found: if code doesn't exist
```

```
DELETE /api/v1/urls/{short_code}
Description: Deactivate (soft-delete) a short URL
Auth: Required (Bearer JWT); must be owner
Response 204 No Content: success
Response 403 Forbidden
Response 404 Not Found
```

```
PATCH /api/v1/urls/{short_code}
Description: Update long_url or expires_at for an existing short URL
Auth: Required (Bearer JWT); must be owner
Request:
  {
    "long_url":   string (optional),
    "expires_at": string | null (optional, null = remove expiry)
  }
Response 200 OK: updated URL object (same schema as GET)
Response 400 Bad Request: invalid URL
Response 403 Forbidden
Response 404 Not Found
```

```
GET /api/v1/urls/{short_code}/analytics
Description: Retrieve click analytics for a short URL
Auth: Required (Bearer JWT); must be owner
Query params:
  from:        ISO 8601 date (required)
  to:          ISO 8601 date (required, max range 90 days)
  granularity: "hour" | "day" | "week" (default: "day")
  page:        integer (default: 1)
  page_size:   integer (default: 50, max: 500)
Response 200 OK:
  {
    "short_code":    string,
    "total_clicks":  integer,
    "period": {
      "from": string,
      "to":   string
    },
    "timeseries": [
      { "bucket": string, "clicks": integer }
    ],
    "top_referrers": [
      { "referrer": string, "clicks": integer }
    ],
    "top_countries": [
      { "country_code": string, "clicks": integer }
    ],
    "devices": {
      "mobile": integer,
      "desktop": integer,
      "tablet": integer,
      "bot": integer
    },
    "pagination": {
      "page": integer,
      "page_size": integer,
      "total_pages": integer
    }
  }
```

```
GET /api/v1/urls
Description: List all URLs for authenticated user
Auth: Required (Bearer JWT)
Query params:
  page:      integer (default: 1)
  page_size: integer (default: 20, max: 100)
  sort:      "created_at" | "click_count" | "expires_at" (default: "created_at")
  order:     "asc" | "desc" (default: "desc")
Response 200 OK:
  {
    "urls": [ <url objects> ],
    "pagination": {
      "page": integer,
      "page_size": integer,
      "total": integer,
      "total_pages": integer
    }
  }
```

```
POST /api/v1/shorten/bulk
Description: Create up to 1000 short URLs in a single request
Auth: Required (API key, pro/enterprise plan only)
Request:
  {
    "urls": [
      { "long_url": string, "custom_alias": string (opt), "expires_at": string (opt) }
    ]
  }
Response 207 Multi-Status:
  {
    "results": [
      { "index": 0, "status": 201, "short_url": string },
      { "index": 1, "status": 409, "error": "alias_taken" }
    ]
  }
```

---

## 6. Deep Dive: Core Components

### Component: Short Code Generation

**Problem it solves:** Every URL creation requires a unique, short, URL-safe identifier. If two concurrent requests generate the same code and both try to insert it, one will fail (violating uniqueness), creating retries and degraded write latency. If codes are sequential or predictable, they are enumerable — an attacker can crawl all shortened URLs. If codes are too short, the keyspace is exhausted quickly.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| MD5/SHA hash of long URL | Hash the long URL, take first 6-8 chars of hex/base62 output | Deterministic (same URL always same code), no DB pre-check needed | Hash collisions for different URLs map to same prefix; same URL always gets same code (can't shorten same URL twice with different codes); 6-char base62 has 56B combos but collision probability rises with scale |
| Auto-increment DB ID → base62 | Store row, get auto-increment PK, encode to base62 | Simple, guaranteed unique, zero collisions | Sequential → enumerable; DB is a bottleneck (single sequence generator); requires insert-then-update or two DB calls |
| Pre-generated token pool | Generate N random codes offline, store in a "keys" table, pop from pool on demand | Zero collision risk, codes are random, fast at write time | Requires pool management, pool exhaustion handling, distributed pop is a contention point (needs row locking or Redis SPOP) |
| Snowflake-style ID → base62 | 64-bit time+worker+sequence ID, encode to base62 | Distributed, monotonically increasing (friendly for DB range scans), no central coordinator | Still partially enumerable if attacker knows epoch + worker ID; requires worker ID assignment |
| UUID v4 → base62 | Generate 128-bit random UUID, truncate to 8 chars of base62 | Highly random, no coordination | Truncation reintroduces collision risk; full UUID → 22 chars base62, too long for UX |
| Random base62 string | Generate 6-8 random alphanumeric chars, check DB for uniqueness, retry on collision | Simple, intuitive | Requires DB read on every write; at high scale, collision probability rises; read-modify-write must be atomic |

**Selected Approach & Reasoning:**

**Pre-generated token pool (Redis SPOP) with database fallback.**

Reasoning with numbers:
- Keyspace: base62 with 7 characters = 62^7 = ~3.5 trillion unique codes. At 1M new URLs/day, this lasts 3.5 trillion ÷ 1M = 9,589 years. We can comfortably use 7-char codes.
- Random codes from a pool are not guessable, preventing enumeration.
- Redis `SPOP` is an O(1) atomic pop from a set — no lock contention, no DB round-trip on the write hot path.
- A background job continuously refills the pool: pre-generates random 7-char base62 strings, batch-checks against the DB for prior existence, inserts new ones into the `available_codes` Redis set. Pool target size: 10M codes (~80 MB in Redis at ~8 bytes/code). Refill triggers when pool drops below 1M.
- Fallback: if Redis pool is empty (background job lagged), the Write Service falls back to generating a random code and doing a DB `INSERT ... ON CONFLICT DO NOTHING` with retry logic.

**Implementation Detail:**

```
Base62 alphabet: "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

Pool refill job (runs every 60s, or triggered when pool < 1M):
  BATCH_SIZE = 500,000
  codes = []
  while len(codes) < BATCH_SIZE:
      candidate = random_base62_string(length=7)
      codes.append(candidate)
  # Deduplicate within batch
  codes = list(set(codes))
  # Check DB for any already-used codes
  existing = db.query("SELECT short_code FROM urls WHERE short_code = ANY(%s)", codes)
  existing_set = {row.short_code for row in existing}
  new_codes = [c for c in codes if c not in existing_set]
  # Push to Redis set
  redis.sadd("available_codes", *new_codes)

Write path (URL creation):
  code = redis.spop("available_codes")
  if code is None:
      # Fallback: generate on the fly with DB uniqueness check
      code = generate_random_base62(7)
      # INSERT with ON CONFLICT retry handles race
  
  db.execute("""
      INSERT INTO urls (id, short_code, long_url, user_id, expires_at, created_at)
      VALUES (%s, %s, %s, %s, %s, NOW())
      ON CONFLICT (short_code) DO NOTHING
      RETURNING id
  """, [snowflake_id(), code, long_url, user_id, expires_at])
  
  if insert returned no rows:
      # Code was just taken (race condition in fallback path)
      retry with new code

Custom alias path:
  if custom_alias requested:
      validate charset and length
      check db: SELECT 1 FROM urls WHERE short_code = alias
      if exists: return HTTP 409 Conflict
      else: use alias as code (skip pool)
```

**Interviewer Deep-Dive Q&A:**

Q: What happens if the pre-generated pool runs out in Redis — say the refill job crashed and hasn't run in an hour?
A: The Write Service detects `SPOP` returning nil and immediately switches to the fallback path: generate a random 7-char base62 string in memory, then attempt `INSERT INTO urls ... ON CONFLICT (short_code) DO NOTHING`. If the INSERT succeeds (returns a row), proceed. If it conflicts (returns nothing), generate a new code and retry — up to 5 attempts. At our scale (12 writes/s), the probability of a collision in 7-char base62 space is 12 / 3.5 trillion ≈ negligible per attempt. The refill job also has an alerting threshold: PagerDuty fires if pool < 100K codes, giving on-call engineers time to investigate before the fallback is stressed.

Q: Why not just use MD5 of the long URL? It's deterministic and avoids any coordination.
A: Two problems. First, partial hash collision: MD5 produces 128 bits, but we only take 6-8 chars of base62 (~42 bits). Collision probability with 1B stored URLs approaches 50% (birthday paradox at 2^21 ≈ 2M URLs for 42-bit space). Second, idempotency may be a bug not a feature: if a user shortens `https://example.com` twice and expects two distinct short codes (e.g., for separate campaign tracking), MD5 gives the same code both times. Our system explicitly supports multiple short codes per long URL. Additionally, MD5 is cryptographically broken — while not used for security here, using it encourages bad habits.

Q: How do you handle the case where a custom alias contains offensive or reserved words?
A: Maintain a blocklist of reserved and offensive terms in a Redis set (`reserved_aliases`). On alias validation, do `SISMEMBER reserved_aliases <alias>` before the DB check. The blocklist covers: (a) system paths (`api`, `health`, `admin`, `static`), (b) profanity/slur list (maintained by Trust & Safety team), (c) brand impersonation terms. Users who hit the blocklist get HTTP 400 with error `reserved_alias`. The blocklist is loaded into Redis at startup and refreshed every 5 minutes; the source of truth is a config file in the code repository to ensure auditability.

Q: How do you prevent the short code from being enumerable even if it's randomly generated?
A: Random 7-char base62 provides 3.5 trillion possibilities. With 1M stored URLs, an attacker enumerating randomly would need to hit 1M/3.5T ≈ 0.000029% probability per attempt. To make brute-force economically infeasible: (a) rate-limit the redirect endpoint by IP — 60 redirects/minute per IP before returning 429; (b) serve 404 for non-existent codes identically fast (no timing oracle); (c) optionally add HMAC signing to custom aliases for enterprise tier. We do not use sequential IDs for short codes precisely to prevent enumeration.

Q: The refill job does a `SELECT short_code FROM urls WHERE short_code = ANY(...)` — doesn't this become slow at 200M rows?
A: The `WHERE short_code = ANY(array)` query uses the `idx_urls_short_code` B-tree index, which is O(k log N) for k candidates — for k=500K and N=200M, this is about 500K × 27 = ~13.5M index comparisons. At ~100ns per comparison on modern hardware, this is roughly 1.3s. This is acceptable for a background batch job running every 60s. However, at even larger scale, we can optimize: (a) use a Bloom filter in Redis to quickly reject codes that definitely exist before doing the DB query — reducing the candidate set dramatically; (b) partition the urls table by short_code prefix, so the ANY query hits fewer pages.

---

### Component: Redirect Hot Path

**Problem it solves:** The redirect endpoint is called ~1,157 times/second on average, with 5x spikes to ~5,785 QPS. Every millisecond of latency is user-visible. A DB query on every redirect would require a very large PostgreSQL read replica fleet. The hot path must serve the vast majority of redirects from cache without touching the DB.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Direct DB read on every redirect | No cache; query PostgreSQL replica on each request | Simplest; always fresh data | Requires ~5,785 DB reads/sec at peak; ~10 read replicas at $500/mo each; p99 latency ~20-50 ms from DB alone |
| Application-level Redis cache (cache-aside) | Redirect service checks Redis first, falls back to DB, populates cache | Sub-millisecond cache hits; DB only sees cache misses (~1-5% of traffic) | Cache invalidation complexity when URL is updated/deleted; cold start cache miss storm |
| CDN caching of 301 responses | CDN (CloudFront) caches the 301 HTTP response at edge by short_code URL path | Zero origin load for cached codes; global low latency | 301 is permanently cached by browsers — if URL changes, old clients are stuck until browser cache expires; must use 302 for mutable links |
| Local in-process LRU cache | Each Redirect Service instance holds a small LRU in memory | Zero network hop; fastest possible | Cache coherence across instances — a URL deleted on instance A is still cached on instance B; works only for immutable or eventually-consistent cases |

**Selected Approach & Reasoning:**

**Layered cache: CDN (for truly immutable 301s) + Redis (for all other redirects) + DB fallback.**

Rationale with numbers:
- CDN layer: Permanent links (no expiry, no custom update) can use HTTP 301 with `Cache-Control: public, max-age=86400`. Viral links get 100% CDN hit rate within minutes. At 5,785 peak QPS, even 70% CDN hit rate eliminates ~4,050 requests/sec from the origin — reducing origin to ~1,735 QPS.
- Redis layer: The remaining 1,735 QPS hits Redis. Redis Cluster with 3 shards handles 100K+ ops/sec each — our load is trivially served. Working set: ~15 GB of hot URLs fits in a 20 GB Redis cluster. Cache hit rate target: >95%. At 95% hit rate, only 87 QPS falls through to PostgreSQL — manageable with a single read replica.
- DB fallback: PostgreSQL read replica handles ~87 QPS cold misses. At ~5ms per query, this is 0.43 DB-seconds/second — well within one replica's capacity (~1000 QPS sustainable on r6g.2xlarge).

Cache key: `url:<short_code>` → `long_url|expires_at` (pipe-delimited string to avoid JSON overhead)
TTL: 24 hours for active URLs. Expired URLs: store a tombstone `EXPIRED` with TTL = 60s to prevent thundering herd on the DB for recently-expired codes.

**Implementation Detail:**

```
Redirect Service pseudocode (per request):

func handleRedirect(short_code string) HTTPResponse:
    // Check expiry-aware cache
    cached = redis.get("url:" + short_code)
    
    if cached == "NOT_FOUND":
        return HTTP 404   // negative cache hit
    
    if cached == "EXPIRED":
        return HTTP 410   // tombstone hit
    
    if cached != nil:
        parts = cached.split("|")
        long_url = parts[0]
        expires_at = parts[1]  // may be "null"
        if expires_at != "null" && time.now() > parse(expires_at):
            redis.set("url:" + short_code, "EXPIRED", ex=60)
            return HTTP 410
        // Async: fire click event to Kafka (non-blocking goroutine)
        go publishClickEvent(short_code, request)
        return HTTP 302, Location: long_url
    
    // Cache miss: query DB replica
    row = db_replica.queryRow(
        "SELECT long_url, expires_at, is_active FROM urls WHERE short_code = $1",
        short_code
    )
    
    if row == nil:
        redis.set("url:" + short_code, "NOT_FOUND", ex=300)  // 5min negative cache
        return HTTP 404
    
    if !row.is_active:
        redis.set("url:" + short_code, "NOT_FOUND", ex=300)
        return HTTP 410
    
    value = row.long_url + "|" + (row.expires_at ?? "null")
    redis.set("url:" + short_code, value, ex=86400)
    
    go publishClickEvent(short_code, request)
    return HTTP 302, Location: row.long_url

Cache invalidation (called by Write Service on update/delete):
    redis.del("url:" + short_code)
    // Also purge CDN: call CloudFront Invalidation API for path /<short_code>
```

**Interviewer Deep-Dive Q&A:**

Q: You're using HTTP 302 (temporary redirect) instead of 301 (permanent). How does this affect analytics, and when would you use 301?
A: HTTP 301 tells browsers "this redirect is permanent — cache it forever." Once a browser caches a 301, it never sends a request to `short.ly` again for that code, which means we never receive the click event — analytics break. HTTP 302 tells browsers "redirect for now, check again next time," so every click hits our server and we capture the event. The tradeoff: 302 generates one extra round-trip per click vs 301 where the browser bypasses us. We use 301 only for "static" links created by anonymous users with no expiry and where the creator explicitly opts out of analytics (advertised as a "fast link" tier). For all authenticated users and analytics-enabled links, we use 302. The CDN can still cache 302 responses (we set `Cache-Control: public, max-age=3600`) — this reduces origin load while still routing clicks through our infrastructure for counting.

Q: What is a thundering herd problem in this context, and how do you prevent it?
A: When a hot link's cache entry expires (TTL = 24h), hundreds of concurrent requests see a cache miss simultaneously and all rush to query the database — potentially overwhelming it. Prevention strategies we use: (a) **Mutex/single-flight**: Use a distributed lock (`SET url:lock:<code> 1 NX PX 500`) so only one request queries the DB while others wait; after the lock holder populates the cache, waiting requests serve from cache. (b) **Probabilistic early expiration (XFetch algorithm)**: Before the TTL expires, randomly extend it by a small amount based on current request rate — popular URLs effectively never expire because they're constantly refreshed. (c) **Staggered TTLs**: Add random jitter (±10% of TTL) so all cache entries don't expire simultaneously. (d) **Tombstone caching for deleted/expired URLs**: Store `NOT_FOUND` in cache with 5-minute TTL so DB is only queried once every 5 minutes for non-existent codes.

Q: How do you handle URL updates — if a user changes `aB3xZ9` from pointing to example.com to example2.com, how quickly does it propagate?
A: On PATCH request, the Write Service: (1) updates PostgreSQL (immediately consistent), (2) calls `redis.del("url:aB3xZ9")` (cache invalidated), (3) calls CloudFront Invalidation API for path `/aB3xZ9` (CDN purge, typically propagates in <5s globally). After the CDN purge, the next request to any edge fetches fresh from origin, gets the new URL, and CDN caches it. There is a window of <5s where some edge nodes may serve the old URL from CDN cache — acceptable for this use case. Browser-cached 301s (if we ever served them) are the hardest to invalidate; this is why we default to 302.

Q: The Redis cache stores `long_url|expires_at` as a string. What if long_url contains a pipe character?
A: Good catch. Two options: (a) Use a different delimiter that cannot appear in a URL — URLs cannot contain unencoded pipe characters (RFC 3986 restricts the path to unreserved/pct-encoded chars, and `|` must be percent-encoded as `%7C`), so a raw `|` in the stored long_url is safe as a delimiter as long as we validate and normalize incoming URLs before storage (we do: malformed URLs are rejected at creation time). (b) Alternatively, store as a Redis Hash: `HSET url:<code> long_url <val> expires_at <val>` — slightly more overhead but structurally cleaner. We use option (a) for performance (single string vs hash, ~30% less memory per key) and validate at write time.

Q: How do you handle the redirect for a link that expired 1 second ago — the cache still has it valid?
A: The cached value includes `expires_at` as a timestamp. On every cache hit, the Redirect Service checks `time.now() > expires_at` in application code — this costs one timestamp comparison (~100ns) per request. If expired: (a) return HTTP 410 Gone, (b) immediately write a tombstone to Redis (`SET url:<code> "EXPIRED" EX 60`) to prevent future cache hits from re-checking, (c) the tombstone TTL is 60s (short) because once the 410 is widely known, we can let it flush. This in-process expiry check means we never serve an expired redirect even if the Redis TTL hasn't fired yet.

---

### Component: Analytics Pipeline

**Problem it solves:** We receive ~1,157 click events/second. If we write analytics synchronously to PostgreSQL in the redirect hot path, each redirect takes an additional ~5ms DB write, increasing p99 from ~10ms to ~15ms+, and at peak (5,785 QPS) we'd need 30K DB writes/second — unsustainable for a transactional OLTP database.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Synchronous DB write on redirect | Write click_event row inline with redirect logic | Simple, immediately consistent | Adds DB write latency to redirect hot path; OLTP DB cannot sustain 100K+ writes/day efficiently for analytics queries |
| Fire-and-forget to Kafka | Redirect service publishes event to Kafka topic asynchronously | Decoupled; zero impact on redirect latency; Kafka buffers bursts | Eventual consistency (analytics lag by seconds to minutes); operational complexity of Kafka cluster |
| Redis counter increment | INCR on `clicks:<code>` key; background job flushes to DB | Extremely fast (sub-ms); simple | Loses granular event data (no per-click referrer, country, device); not queryable for time-series; data loss if Redis evicts |
| Direct writes to ClickHouse | Batch insert click events directly to ClickHouse from service | Good for analytics queries; columnar storage | ClickHouse is not designed for single-row inserts; needs batching; adds ClickHouse as a direct dependency of the hot path |

**Selected Approach & Reasoning:**

**Redis counter increment (for real-time click_count) + Kafka → Flink → ClickHouse (for detailed analytics).**

Two separate flows:
1. **Real-time click count**: Redirect Service does `REDIS INCR clicks:<short_code>`. Background job (every 30s) reads increments and batch-updates `urls.click_count` in PostgreSQL: `UPDATE urls SET click_count = click_count + $delta WHERE short_code = $code`. This gives users a fast approximate click count on their dashboard with ~30s lag, using no OLTP writes on the redirect path.

2. **Detailed analytics**: Redirect Service publishes a Kafka message (fire-and-forget, async, non-blocking). Kafka consumer (Flink job) enriches events (IP → country via MaxMind lookup), aggregates in 1-minute tumbling windows, and batch-inserts to ClickHouse. ClickHouse serves dashboard queries with MergeTree engine optimized for time-series range scans.

**Implementation Detail:**

```
// In Redirect Service (Go, non-blocking):
go func() {
    event := ClickEvent{
        ShortCode:  short_code,
        ClickedAt:  time.Now().UTC(),
        IP:         request.RemoteAddr,
        UserAgent:  request.Header.Get("User-Agent"),
        Referrer:   request.Header.Get("Referer"),
    }
    msg, _ := json.Marshal(event)
    producer.Produce(&kafka.Message{
        TopicPartition: kafka.TopicPartition{Topic: "click-events", Partition: kafka.PartitionAny},
        Value: msg,
    }, deliveryChan)  // deliveryChan is monitored by separate goroutine for errors
    
    // Also increment Redis counter (sync but sub-ms, acceptable)
    redis.Incr("clicks:" + short_code)
}()

// Flink job (simplified):
stream.keyBy(event -> event.shortCode)
      .window(TumblingEventTimeWindows.of(Time.minutes(1)))
      .aggregate(new ClickCountAggregate())
      .addSink(new ClickHouseSink(batchSize=10000, flushInterval=30s))
```

Kafka topic: `click-events`, 24 partitions (allows 24 parallel consumers), retention 7 days (replay window for recovery), replication factor 3.

**Interviewer Deep-Dive Q&A:**

Q: The Redirect Service publishes to Kafka in a goroutine — what if Kafka is down? Do you lose click events?
A: Yes, there is a small window of data loss if Kafka is completely unavailable. Mitigation options: (a) **Local buffer**: if the Kafka produce call fails after 3 retries with exponential backoff (total ~500ms), write the event to a local in-memory ring buffer (max 100K events); a background thread drains the buffer to Kafka when it recovers. This is best-effort; if the service instance crashes with a full buffer, we lose those events. (b) **Write to local disk (WAL)**: guaranteed durability but adds latency and disk I/O to the redirect path — not acceptable. (c) **Accept the loss**: analytics data is approximate by nature; losing <0.01% of clicks during a Kafka outage is tolerable. We choose option (a) with monitoring: if buffer fill rate > 50%, alert on-call. The SLA for analytics is "best-effort within 5 minutes" — not 100% accuracy.

---

## 7. Scaling

### Horizontal Scaling

- **Redirect Service**: Fully stateless — no session state, no local DB connection state (connections pooled via PgBouncer sidecar). Scale horizontally by adding instances behind the load balancer. Auto-scaling group triggers at CPU > 60% or p99 latency > 15ms. Each instance: 2 vCPU, 4 GB RAM. At 5,785 peak QPS with ~1 ms average response time, a single instance handles ~1000 QPS (accounting for Go's goroutine overhead and network I/O). Need ~6 instances at peak, run 8 for headroom.
- **Write/API Service**: Also stateless. Much lower QPS (12 writes/sec). 2 instances with auto-scaling provides plenty of headroom. Scale up before peak marketing campaigns.
- **ID Generator / Pool Refill**: Single background process is sufficient (12 URLs/sec). Run 2 instances with distributed locking (Redis `SET lock:refill NX EX 120`) to prevent double-refill.

### Database Scaling

**Sharding:**

- **Options:**
  - *Range-based on short_code*: Shard A = codes starting a-m, Shard B = n-z. Simple but creates hotspots: if most new codes start with letters from a certain range, one shard gets all writes.
  - *Hash-based on short_code*: `shard_id = crc32(short_code) % num_shards`. Distributes writes evenly. No range queries across shards needed (all URL lookups are point lookups by short_code). This is the correct choice.
  - *Directory-based (lookup service)*: A separate service maps `short_code → shard`. Adds a hop; overly complex for this access pattern.

- **Selected: Hash-based sharding on `short_code`**. At 200M URLs over 5 years, PostgreSQL handles this without sharding (single primary + read replicas). We shard when table reaches 500M rows or write QPS > 1000/sec. Start with 4 shards (expandable to 16 with consistent hashing to minimize re-sharding). Each shard runs a primary + 2 read replicas.

**Replication:**

- **Primary-replica (async)**: One primary handles all writes (12 QPS currently — trivial). 2 read replicas per shard handle read traffic. Async replication lag ~10-100ms in same AZ, ~500ms cross-AZ. Acceptable because the cache sits in front — cache is the truth for reads, and the replica is only hit on cache miss.
- **Why not multi-primary?**: Multi-primary (Galera, CockroachDB) introduces write conflict resolution overhead and higher latency on every write. At our write rate (12/sec), a single primary is not a bottleneck. Multi-primary adds complexity for no benefit at this scale.
- **Replication lag handling**: Read replicas serve analytics queries and cache-miss fallbacks. For analytics (eventual consistency is acceptable), lag is fine. For the redirect path: if a newly created URL is in the DB primary but the replica hasn't caught up, a redirect attempt in the next 100ms will get a cache miss → replica miss → 404. Mitigation: after creating a URL, write directly to Redis cache (not waiting for replica). This ensures the redirect works immediately even before replica catches up.
- **Read replica routing**: PgBouncer at each Redirect Service sidecar routes all read queries to replicas, write queries to primary. The Write Service explicitly targets the primary for all its queries.

**Caching:**

- **What to cache**: `short_code → long_url + expires_at` mapping. This is the 100% dominant read pattern. Also cache negative results (`NOT_FOUND`) with 5-minute TTL to prevent DB hammering for invalid codes.
- **Strategy: Cache-aside (lazy loading)**. The application checks Redis; on miss, reads from DB and populates cache. This is correct for our access pattern: we don't need to pre-warm all 200M URLs (most are never accessed again after creation). Cache only hot entries, loaded on demand.
  - *Write-through* (write to cache and DB simultaneously on creation) would be appropriate here too — we do this as an optimization: on URL creation, the Write Service immediately writes to Redis before returning the response. This ensures the first redirect (which may happen milliseconds after creation) hits the cache.
  - *Write-behind (write-back)*: Cache accepts write, asynchronously writes to DB. NOT used here because URL creation requires DB durability — if the cache node dies before flushing, the URL is lost.
- **Eviction: LRU (Least Recently Used)**. Redis `maxmemory-policy: allkeys-lru`. Our ~15 GB working set fits in 20 GB Redis cluster with LRU keeping hot links in cache. LFU (Least Frequently Used) is an alternative — better for Zipf-distributed access (most URLs are rarely accessed). We use LRU because: (a) simpler to reason about TTL interactions, (b) Redis LRU is O(1) via approximation (samples N random keys), (c) the difference in hit rate at our scale is <2%.
- **Invalidation**: On `PATCH` (URL update) or `DELETE`: Write Service calls `redis.del("url:<code>")` synchronously before returning the HTTP response. This is "cache invalidation on write." We do NOT use cache invalidation via TTL alone for mutations — a 24-hour stale redirect is unacceptable for user-initiated updates.
- **Tool: Redis vs Memcached**: **Redis**. Specific reasons: (a) Redis Cluster provides native horizontal scaling across shards — Memcached requires client-side sharding with no rebalancing on node failure; (b) Redis supports atomic operations (`INCR` for click counting, `SPOP` for code pool) that Memcached doesn't; (c) Redis persistence (RDB snapshots + AOF) allows recovery of the code pool and counters after crash — Memcached is pure ephemeral; (d) Redis Sentinel/Cluster provides automatic failover — Memcached has no built-in HA; (e) our dataset (~15 GB) fits in RAM, so Redis's RAM requirement is not a concern.

**CDN:**

- **What is served from CDN**: HTTP 301/302 redirect responses for short codes (the dominant traffic type). Static assets for the analytics dashboard UI (JS, CSS). API responses are NOT cached at CDN (they are authenticated and user-specific).
- **Pull CDN (CloudFront pull)**. CDN fetches from origin on cache miss and caches the response. Correct choice because: (a) we cannot predict which URLs will go viral — pull CDN automatically caches whatever gets traffic; (b) push CDN requires us to proactively push all 1M new URLs/day to every edge node — infeasible operationally; (c) CloudFront's origin shield feature coalesces all edge-node misses into a single request to origin, preventing thundering herd from multiple PoPs simultaneously.
- **Cache invalidation**: On URL update/delete, call `cloudfront.createInvalidation(paths=["/<short_code>"])`. CloudFront invalidations propagate globally in <5 seconds. Invalidation API limits: 1000 paths/month free, then $0.005/path. At our scale (12 URL updates/day ≈ 360/month), well within budget.

**Interviewer Deep-Dive Q&A (Scaling):**

Q: If traffic grows 100x to 100,000 redirect QPS, what breaks first and how do you fix it?
A: At 100K QPS, the bottleneck chain is: (1) **Redis**: our current 3-shard cluster handles ~300K ops/sec total — not the bottleneck. (2) **Redirect Service instances**: at 1000 QPS/instance, we need ~100 instances. Stateless scaling — just add more. ASG handles this. (3) **CDN**: CloudFront scales to millions of QPS — not a concern. (4) **DB on cache misses**: if hit rate stays at 95%, cache miss QPS = 5K, which is 10 read replicas. If hit rate drops (cold cache after deployment), we could hit 100K DB reads/sec — need 100+ replicas. Mitigation: ensure cache warmup on deployment (pre-populate top N hot URLs). (5) **PostgreSQL write path for click counters**: 100K INCR/sec to Redis is fine; the batch-flush to PostgreSQL happens every 30s on ~5K unique codes — fine. First real fix: scale out Redirect Service instances (stateless, easy). Second: ensure CDN hit rate is maximized by tuning Cache-Control headers.

Q: How would you handle a hot key problem — a single short URL receiving 50,000 redirects per second (viral tweet)?
A: A single Redis key under 50K QPS can actually handle it — Redis single-threaded operations handle ~100K ops/sec per instance, so one key on one Redis node is fine. The bigger concern is: (a) **CDN**: this is where we solve it. A viral URL should be cached at CDN edge — 50K QPS from a single popular short URL means CloudFront is serving it from ~50 edge locations, each handling ~1K QPS locally. (b) **Redirect Service**: 50K QPS is shared across instances — no hot instance problem because LB distributes by connection. (c) **Kafka click events**: 50K messages/second to a single partition causes consumer lag. Solution: use the `short_code` as the Kafka partition key hash, but cap partition assignment — partition by `hash(short_code) % 24`, so no single partition gets more than the natural hash distribution. For extreme virality, consider sampling (record only 1% of clicks during spike, multiply by 100 in analytics).

Q: How does database connection pooling work at scale?
A: Each Redirect Service instance maintains a PgBouncer sidecar in transaction pooling mode. PgBouncer configuration: `pool_size = 10` (10 DB connections per sidecar), `server_pool_size = 20`. With 8 Redirect Service instances × 10 connections = 80 total DB connections — far below PostgreSQL's max_connections (typically 200 on a db.r6g.4xlarge). Without PgBouncer, each Go goroutine that makes a DB call could hold a connection open for the duration of the request. At 5,785 QPS with ~5ms DB call time, we'd need 5785 × 0.005 = ~29 concurrent DB connections — manageable. But Go's `database/sql` already pools connections. PgBouncer's value here is protecting the DB from connection storms during deployments when many instances restart simultaneously.

Q: Your Redis cluster has 3 shards. What happens when one shard goes down?
A: Redis Cluster with 3 primary shards and 3 replica shards (one replica per primary). On primary shard failure: Redis Cluster's built-in failover promotes the replica to primary in ~1-2 seconds (configurable via `cluster-node-timeout`). During those 1-2 seconds, requests to the failed shard get CLUSTERDOWN errors — the Redirect Service catches these and falls back directly to the PostgreSQL replica, accepting higher latency for that window. After failover, the cluster resumes normal operation. We alert if a shard failover occurs (CloudWatch alarm on Redis `ReplGlobalDataTransferred` metric). To prevent correlated failures: place each Redis shard primary and its replica in different availability zones.

Q: How do you do a zero-downtime database schema migration at this scale?
A: Use the expand-contract pattern: (1) **Expand**: add the new column as nullable with no default (`ALTER TABLE urls ADD COLUMN new_col TYPE NULL`) — in PostgreSQL, this is an instant metadata-only change for nullable columns (no table rewrite). (2) **Backfill**: background job updates existing rows in batches of 1000 with `WHERE new_col IS NULL LIMIT 1000` and `pg_sleep(0.1)` between batches to avoid lock contention. (3) **Deploy new code** that reads both old and new column, writes to both. (4) **Contract**: once backfill is complete and old column is unused, `ALTER TABLE urls DROP COLUMN old_col` (another fast metadata op). Never run `ALTER TABLE` with a table rewrite (`ADD COLUMN NOT NULL DEFAULT`) on a live table with millions of rows — it takes an exclusive lock for minutes.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Mitigation |
|---------|--------|------------|
| DB primary goes down | All writes (URL creation) fail; reads continue from replica + cache | Automatic failover via RDS Multi-AZ or Patroni in ~30s; Write Service retries with exponential backoff; read path unaffected if cache populated |
| Cache (Redis) cluster down | All redirects fall through to DB; DB sees 1,157 QPS (vs normal ~87 QPS) | DB read replicas scaled to handle 2x cache-miss QPS; circuit breaker on Redis client trips after 5 consecutive failures, falls back directly to DB; alert fires in <1 minute |
| Cache miss storm (cold start) | After deployment, empty Redis cache causes DB overload | Pre-warm cache: before deploying, run a script that fetches top 1M hot URLs (by click_count in last 7 days) and populates Redis; use origin shield on CloudFront to coalesce CDN misses |
| Network partition (split brain) | DB replica cannot reach primary; Redis replica cannot reach primary | PostgreSQL: replica enters read-only mode (refuses writes), primary continues serving. Redis Cluster: uses majority-quorum (requires 3+ primary nodes) — minority partition refuses writes but allows reads |
| Redirect Service crash | Subset of traffic fails until LB health check removes instance | LB health check every 5s, unhealthy threshold = 2 → instance removed in ~10s. Auto-scaling replaces instance. Other instances absorb traffic (each has headroom). |
| Kafka consumer lag | Analytics events accumulate; dashboard data lags | Kafka retains events for 7 days. Flink consumer auto-scales (Kafka consumer group rebalance). Alert if lag > 1M events or > 5 minutes. Redirect path is unaffected — Kafka is decoupled. |
| ID pool exhaustion (Redis SPOP returns nil) | Write fallback path activates | Fallback generates random code + DB uniqueness check. Alert fires if pool < 100K. Refill job has its own monitoring and auto-restart via Kubernetes liveness probe. |
| URL creation service down | Users cannot create new short URLs | Read/redirect path is independent. Write Service has 2 instances minimum; LB detects failure and routes to healthy instance. Stateless restart < 10s. |

### Failover Strategy

- **Active-Passive for PostgreSQL**: One primary (active), one hot standby in a different AZ (passive). Standby streams WAL continuously. On primary failure, Patroni (or RDS Multi-AZ) promotes standby to primary. Write Service reconnects via DNS CNAME that Patroni updates on failover. **RTO: ~30-60 seconds** (failover detection + promotion + DNS TTL). **RPO: ~0** (synchronous replication to standby means no data loss; async replicas may lag).
- **Active-Active for Redirect Service**: All Redirect Service instances are active simultaneously behind the LB. This is true active-active — if one instance dies, the others immediately absorb traffic. No failover delay for the read path.
- **Active-Active for Redis**: Redis Cluster distributes data across shards; each shard has primary + replica. Reads can be served from replicas if primary fails (with `READONLY` flag). Redis Sentinel for smaller setups; Redis Cluster for our scale.
- **RTO target: <60 seconds** for write path, **<10 seconds** for read path.
- **RPO target: 0 data loss** for URL mappings (synchronous replica). Analytics events: acceptable to lose events during Kafka outage windows (best-effort).

### Retries & Idempotency

- **Retry strategy for DB writes**: Exponential backoff with jitter. Base delay = 100ms, multiplier = 2, max delay = 10s, max attempts = 5, jitter = ±25% of computed delay. Example delays: 100ms, 214ms, 456ms, 991ms, 10s. Jitter prevents synchronized retry storms from multiple service instances.
- **Idempotency for URL creation**: URL creation with a custom alias is naturally idempotent — `POST /shorten` with `custom_alias=my-link` checks if the alias exists and returns 409 if so. For programmatic clients making retried requests (e.g., due to network timeout after server received the request), we support an optional `Idempotency-Key` header. The Write Service stores `(idempotency_key, response_body)` in Redis with 24-hour TTL. On retry with same key, returns the stored response without re-processing. Key format: UUID v4 provided by client.
- **Idempotency for analytics**: Kafka messages carry a `event_id` (UUID). Flink uses exactly-once semantics with Kafka transactions and ClickHouse upserts keyed on `event_id` — prevents double-counting on consumer restarts.

### Circuit Breaker

- **Redis circuit breaker**: Applied in Redirect Service's Redis client. Configuration: window = 10 seconds, failure threshold = 50% of calls failing, minimum requests in window = 20, open→half-open after 30 seconds. When open: all Redis calls immediately return "cache miss" and fall through to DB. This prevents the Redirect Service from accumulating goroutine queue waiting on an unresponsive Redis instead of degrading gracefully.
- **DB circuit breaker**: Applied in Redirect Service's DB client. Same thresholds. When DB circuit is open: return HTTP 503 (prefer this over returning wrong data). Alert fires immediately.
- **Kafka circuit breaker**: Applied to the async click event producer. When open: events go to local in-memory buffer (max 10K events). Non-blocking — redirect path is never affected.
- **Implementation**: Use `sony/gobreaker` library in Go, or `resilience4j` in Java. Standard half-open probe logic: after `OpenTimeout`, send one trial request; if it succeeds, close the breaker; if it fails, reset the open timer.

---

## 9. Monitoring & Observability

| Metric | Tool | Alert Threshold | Why |
|--------|------|-----------------|-----|
| Redirect p99 latency | CloudWatch / Datadog APM | > 50 ms for 5 min | Core SLA; p99 target is 20ms, 50ms = 2.5x slack |
| Redirect error rate (5xx) | Datadog | > 0.1% over 5 min | Indicates service or dependency failure |
| Cache hit rate (Redis) | Redis INFO stats → Datadog | < 90% over 15 min | Drop in hit rate = unusual miss storm or cache failure |
| Redis memory usage | CloudWatch ElastiCache | > 85% of maxmemory | Approaching eviction; need to scale up or tune TTL |
| DB replication lag | PostgreSQL `pg_stat_replication` | > 500ms | Long lag = stale data risk on redirect fallback |
| DB connection pool saturation | PgBouncer metrics | > 90% pool usage | Risk of connection exhaustion and query queuing |
| URL creation QPS | Datadog custom metric | > 3× baseline | Potential abuse/spam campaign; trigger rate limit tightening |
| Kafka consumer lag | Burrow / Datadog | > 1M events or > 5 min lag | Analytics pipeline degraded |
| Code pool size (Redis SCARD) | Custom job → Datadog | < 100,000 codes | Pool near exhaustion; escalate to page |
| Redirect 404 rate | Datadog | > 5% of requests | Possible scraping/enumeration attack |
| CDN cache hit rate | CloudFront metrics | < 70% for top URLs | Misconfiguration or cache invalidation storm |
| Certificate expiry | AWS Certificate Manager | < 30 days until expiry | TLS failure if cert expires |

### Distributed Tracing

Use **OpenTelemetry** with **Jaeger** (or Datadog APM). Every HTTP request gets a `trace_id` (UUID v4) injected at the Load Balancer level. Each downstream call (Redis, PostgreSQL, Kafka) creates a child span. Trace context is propagated via HTTP headers (`traceparent` per W3C Trace Context spec) and Kafka message headers.

Sampling strategy: 100% of requests sampled for error traces (any 4xx/5xx), 1% of successful requests sampled (tail-based sampling). This gives full visibility into failures without overwhelming the trace storage backend.

Key spans to instrument:
- `redirect.redis_lookup` — duration and outcome (hit/miss/error)
- `redirect.db_fallback` — duration, query fingerprint
- `redirect.kafka_publish` — duration (async; sampled separately)
- `write.id_generation` — pool pop vs fallback
- `write.db_insert` — duration, conflict detection

### Logging

Structured JSON logging to stdout, collected by Fluent Bit, shipped to Elasticsearch (ELK stack) or Datadog Logs.

Log levels:
- **INFO**: request received, redirect served (with `short_code`, `cache_hit: true/false`, `latency_ms`)
- **WARN**: cache miss (expected but notable), Redis fallback activated, retry attempt #N
- **ERROR**: DB unreachable, Redis unreachable, Kafka publish failed after all retries

Log fields on every redirect:
```json
{
  "timestamp": "2026-04-09T12:34:56.789Z",
  "level": "info",
  "trace_id": "abc123...",
  "service": "redirect-service",
  "instance_id": "i-0a1b2c3d",
  "short_code": "aB3xZ9",
  "cache_hit": true,
  "latency_ms": 4.2,
  "status_code": 302,
  "client_ip": "1.2.3.4",  // hashed/anonymized for GDPR
  "user_agent_class": "browser"  // not full UA string in logs
}
```

Do NOT log full `long_url` in access logs (may contain sensitive query parameters). Log only `short_code`.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A | Option B | Chosen | Reason |
|----------|----------|----------|--------|--------|
| HTTP redirect type | 301 (permanent, browser-cached) | 302 (temporary, not cached) | 302 for analytics links; 301 opt-in only | 301 breaks analytics permanently for that client; 302 allows per-click event capture |
| Short code generation | Hash of long URL (MD5 truncated) | Pre-generated random pool | Pre-generated pool | Hash has partial collision risk at scale; pool guarantees uniqueness with no DB roundtrip on write hot path |
| Primary database | PostgreSQL (relational) | DynamoDB (NoSQL) | PostgreSQL | ACID uniqueness constraint, rich query flexibility, proven at 200M row scale, no vendor lock-in |
| Cache strategy | Write-through (always cache on write) | Cache-aside (lazy load on read) | Both (cache on creation + lazy load on read) | Write on creation ensures first redirect hits cache; lazy load handles cases where cache is evicted |
| Analytics pipeline | Synchronous DB write | Async Kafka → ClickHouse | Async Kafka | Synchronous write adds ~5ms to redirect p99 and limits throughput; Kafka decouples entirely |
| Redirect service scaling | Vertical (larger instances) | Horizontal (more stateless instances) | Horizontal | Stateless service scales linearly; vertical scaling has limits and causes single point of risk |
| Code length | 6 chars (62^6 = 56B) | 7 chars (62^7 = 3.5T) | 7 chars | 6 chars exhausted in 153K years at 1M/day, but 7 chars provides 9,589 years — better keyspace vs minimal UX difference (1 char) |
| CDN redirect caching | Cache all redirects | Cache only permanent (301) links | Cache 302s with short TTL (1hr) for CDN, 301s for opt-in permanent | Balance between CDN load reduction and URL mutability support |
| Click counting | Real-time per-redirect DB increment | Async Redis INCR + batch flush | Async Redis + batch flush | Real-time DB write adds 5ms+ to redirect path; Redis INCR is sub-ms and non-blocking |

---

## 11. Follow-up Interview Questions (minimum 15 Q&As)

**Q1: How would you handle a sudden 10x traffic spike (e.g., a celebrity tweets your short link)?**

A: The system is designed for this. Step-by-step response: (1) CDN absorbs the initial wave — CloudFront auto-scales and the viral URL gets cached at all ~400+ PoPs within minutes of the first requests from each region. (2) Redis easily handles 10x (11,570 QPS) — our cluster is rated for 300K+ ops/sec. (3) Redirect Service auto-scales via ASG — CloudWatch alarm on CPU > 60% adds 2 instances every 5 minutes. Pre-emptively: we can configure scheduled scaling for anticipated events (e.g., a planned marketing push). (4) DB: cache hit rate should be near 100% for a single viral URL — DB sees essentially no additional load. If the URL was just created (cache cold): the CDN Origin Shield feature coalesces all PoP misses into 1 request to origin, which populates the cache once. After that, zero DB load. (5) Kafka: 10x events = 11,570 messages/sec to the `click-events` topic. 24 partitions at ~500 events/sec/partition — within throughput limits. Consumer lag may increase temporarily; Flink scales out consumer parallelism.

**Q2: How do you prevent abuse — someone using your service to shorten malware or phishing URLs?**

A: Multi-layer approach: (1) **URL validation at creation time**: Validate URL syntax (RFC 3986 compliance), resolve DNS (ensure domain exists), check against Google Safe Browsing API and VirusTotal API for known malware/phishing domains. Reject or flag URLs that fail these checks. (2) **Rate limiting**: Anonymous users limited to 10 URLs/hour per IP. Unusual burst from a single IP (> 100 URLs/minute) triggers temporary IP ban and human review. (3) **Domain blocklist**: Maintain a blocklist of known malicious domains (PhishTank database, abuse.ch feeds). Check on creation; block in near-real-time by adding to a Redis set that the Write Service checks. (4) **Redirect-time check**: Before serving the redirect (or asynchronously), check the long URL against a lightweight local blocklist (Bloom filter of ~10M known bad domains, ~100MB RAM). If flagged, return 451 Unavailable For Legal Reasons with an abuse notice. (5) **User reporting**: Allow visitors to report malicious links via `GET /<code>?report=1`. Flagged links are queued for manual review and temporarily disabled.

**Q3: How would you implement geographic routing — users in Europe should hit EU servers, users in US should hit US servers?**

A: (1) **DNS-based geo-routing**: Route53 Geolocation routing policy — `short.ly` resolves to EU load balancer IP for European clients, US load balancer IP for US clients. Failover: if EU LB is unhealthy, Route53 falls back to global/US endpoint. (2) **Regional cache clusters**: Deploy a Redis cluster in each region. Each regional Redirect Service queries its local Redis first, then its local PostgreSQL read replica. (3) **Data replication**: The PostgreSQL primary lives in US-East. EU region has read replicas with ~100ms replication lag (transatlantic). URL creation always goes to the primary (US-East) via the Write Service. Redirect lookups in EU may see replication lag for very newly created URLs — mitigated by write-through cache: Write Service populates the EU Redis on URL creation (via cross-region pub/sub on Kafka). (4) **GDPR compliance**: EU traffic must not leave EU region. Click event data from EU users is stored in the EU ClickHouse cluster and never replicated to US. User account data for EU-registered users lives in EU PostgreSQL replica with no EU→US sync for PII.

**Q4: How would you implement link analytics in real-time (< 1 second latency) for a "live dashboard" feature?**

A: Real-time analytics (< 1s) requires a different architecture than our batch pipeline: (1) Replace Kafka → Flink → ClickHouse with Kafka → Redis Sorted Sets + Redis Streams. On each click event: Flink (with 1-second tumbling windows) updates a Redis Sorted Set for `clicks:by-minute:<short_code>` with `ZADD` and a Counter for `clicks:total:<short_code>`. Dashboard WebSocket connection subscribes to a Redis pub/sub channel `analytics:<short_code>`. (2) The Flink job publishes per-second aggregates to the pub/sub channel. Dashboard receives updates via WebSocket push. (3) For historical data (> 1 minute ago), query ClickHouse as before. For last 60 seconds, query Redis Sorted Set. (4) Trade-off: Redis Sorted Set for time-series is memory-intensive — one key per unique (short_code, minute) combination. Retention in Redis: last 1 hour only. Older data in ClickHouse. (5) At 100M clicks/day = 1,157/sec, updating Redis for every click event (even batched per second) is ~1,157 ZADD ops/sec — trivial for Redis.

**Q5: A user reports their short URL is returning a 404 even though they just created it. How do you debug this?**

A: Systematic debugging process: (1) **Check if the URL is in the database**: `SELECT * FROM urls WHERE short_code = 'xyz'` on the primary — if not found, the creation API call may have failed silently on the client side (network timeout where the server succeeded but the client never received the 201). (2) **Check the cache**: `redis-cli GET url:xyz` on all cluster shards (since it's hash-partitioned). If the DB has it but Redis doesn't, a cache miss should fall through to DB — check the Redirect Service logs for `short_code=xyz` to see if it's even receiving the request. (3) **Check CDN cache**: CloudFront may have cached a 404 response from before the URL was created (if the user tested the URL before creation). Fix: `cloudfront.createInvalidation(["/<code>"])`. CDN negative caching: check if we're setting `Cache-Control: no-store` on 404 responses — we should be (do not cache 404s). (4) **Check replication lag**: If the Redirect Service read replica is lagging and the Write Service didn't populate the cache: `SELECT now() - replay_lsn::text::interval FROM pg_stat_replication` on the primary. (5) **Trace ID correlation**: Ask the user for the `X-Request-Id` header from their request; find the trace in Jaeger and see exactly where the 404 was generated.

**Q6: How do you handle URL expiration efficiently — you have 200M URLs, some with expiry dates. How do you clean them up without scanning the whole table?**

A: (1) **Partial index for expiry**: `CREATE INDEX idx_urls_expires_at ON urls(expires_at) WHERE expires_at IS NOT NULL AND is_active = TRUE`. This index contains only expiring, active URLs — likely < 5% of all URLs. Cleanup job: `UPDATE urls SET is_active = FALSE WHERE expires_at < NOW() AND is_active = TRUE LIMIT 1000` using the partial index. Run every minute; the `LIMIT 1000` prevents long-running locks. (2) **Redis TTL**: When storing in cache, set TTL = `min(URL_TTL_remaining, 24h)`. This means the cache entry expires around the same time as the URL itself — no need to explicitly invalidate expired entries from cache; they expire naturally. (3) **On-access expiry check**: In the Redirect Service, even if the cache hasn't expired yet (due to TTL rounding), we check `expires_at > now()` in application code and return 410 immediately if expired. (4) **Tombstone**: After the background cleanup job marks URLs as inactive, it also publishes their codes to a Redis list; a worker pops codes and `DEL`s their cache entries — ensuring redirects start returning 410 promptly. (5) Scale: 1M URLs/day × 30-day average TTL = 30M expiring URLs in flight at any time. Partial index has 30M rows — fine for index scans.

**Q7: How would you design the custom alias feature to prevent namespace squatting (someone registering all valuable aliases like 'apple', 'google', 'amazon')?**

A: (1) **Blocklist of reserved aliases**: Maintain a list of Fortune 500 brand names, common words, and trademarked terms (curated by Trust & Safety). All registered names go into a Redis set `reserved_aliases`. Custom alias validation checks this set before allowing registration. (2) **Pro/enterprise-only feature**: Meaningful custom aliases (< 8 chars, common words) are restricted to paying users. Free tier can use custom aliases but only with a minimum length of 10 chars. (3) **Squatting detection**: Flag accounts that register > 50 custom aliases that match brand names. Route to manual review queue. (4) **DMCA/trademark claims**: Process: trademark holder submits claim via support ticket → Trust & Safety verifies → alias deactivated and new code assigned. Alias goes into `reserved_aliases`. (5) **TTL on aliases**: Custom aliases automatically expire if the URL is deleted and the user is on the free tier — aliases are not permanently "owned." Pro users can keep aliases permanently. (6) **Unicode/homograph attacks**: Normalize all alias inputs to ASCII before validation (reject non-ASCII); this prevents `goog1e` (with digit 1 not letter l) attacks. Apply NFC normalization for any inputs that slip through.

**Q8: Your redirect service is stateless, but how do you handle rate limiting across multiple service instances consistently?**

A: Stateless services cannot count requests locally across instances. Solution: **centralized rate limiting via Redis**. Algorithm: Token Bucket implemented in Redis using a Lua script for atomicity.

```lua
-- KEYS[1] = rate_limit_key (e.g. "rl:ip:1.2.3.4")
-- ARGV[1] = max_tokens, ARGV[2] = refill_rate (tokens/sec), ARGV[3] = current_timestamp
local key = KEYS[1]
local max_tokens = tonumber(ARGV[1])
local refill_rate = tonumber(ARGV[2])
local now = tonumber(ARGV[3])
local data = redis.call('HMGET', key, 'tokens', 'last_refill')
local tokens = tonumber(data[1]) or max_tokens
local last_refill = tonumber(data[2]) or now
local elapsed = now - last_refill
local new_tokens = math.min(max_tokens, tokens + elapsed * refill_rate)
if new_tokens < 1 then return 0 end
redis.call('HMSET', key, 'tokens', new_tokens - 1, 'last_refill', now)
redis.call('EXPIRE', key, 3600)
return 1
```

This Lua script runs atomically in Redis (single-threaded execution). All Redirect Service instances point to the same Redis cluster. The rate limit key is typically the client IP (for anonymous) or user_id (for authenticated). At our QPS, this adds ~1ms to each request (one Redis round-trip) — acceptable given our 20ms p99 budget.

**Q9: How would you support the ability to see "all links pointing to a specific destination domain" — e.g., give me all short URLs that redirect to example.com?**

A: This is a "reverse index" problem. The URLs table has `long_url` as a TEXT field — full-text indexing a URL for domain extraction is expensive. Options: (1) **Store domain separately**: Add a `destination_domain VARCHAR(253)` column extracted at write time (`new URL(long_url).hostname`). Index: `CREATE INDEX idx_urls_domain_user ON urls(destination_domain, user_id)`. Query: `SELECT * FROM urls WHERE destination_domain = 'example.com' AND user_id = $id`. This is the simplest approach and works well for per-user queries. (2) **For admin/cross-user queries** (abuse detection): Use Elasticsearch — index all `urls` records with `long_url` and `destination_domain` fields. ES supports wildcard and prefix queries efficiently. Feed via CDC (Debezium) from PostgreSQL. (3) **PostgreSQL functional index**: `CREATE INDEX idx_urls_domain ON urls(regexp_replace(long_url, '^https?://([^/]+).*', '\1'))` — this is fragile (regex on index) and not recommended for production. Option 1 (stored domain column) is the right approach.

**Q10: What security measures do you take to prevent SSRF (Server-Side Request Forgery) attacks via the URL shortener?**

A: When the Write Service validates/previews the long URL, if it makes an HTTP request to the URL (e.g., to fetch the page title for link preview), it's vulnerable to SSRF: `POST /shorten {"long_url": "http://169.254.169.254/latest/meta-data/"}`. Mitigations: (1) **No outbound HTTP requests to user-provided URLs during creation**: Do not fetch the URL to validate it. Use syntactic/DNS validation only. If you do fetch (for preview), run in a sandboxed microservice with network egress policy that blocks RFC 1918 ranges (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16), link-local (169.254.0.0/16), and loopback (127.0.0.0/8). (2) **DNS rebinding protection**: After resolving the domain to an IP, check if the IP is in a private range — reject if so. Re-resolve before every HTTP request (prevent TTL-0 rebinding attacks). (3) **URL scheme allow-list**: Only allow `http://` and `https://` schemes. Reject `file://`, `ftp://`, `gopher://`, `javascript://`, `data:`, etc. (4) **Redirect chain depth limit**: When following redirects (if any), limit to 3 hops to prevent redirect loops.

**Q11: How would you implement a "link rotation" feature — one short code randomly distributes traffic across multiple destination URLs (useful for A/B testing)?**

A: Extend the data model: (1) Add a `url_group` table: `(group_id, short_code, is_group=true)`. Add a `url_group_destinations` table: `(group_id, long_url, weight INT, is_active BOOL)`. (2) On redirect, detect `is_group=TRUE` for the short code. Load all destinations for the group (cached in Redis as a list: `LRANGE url_group:<code> 0 -1`). Select destination using weighted random selection. (3) Cache: store the group's destinations in Redis as a JSON array or Redis List with total weight. TTL = 5 minutes (shorter than normal to allow A/B experiment updates to propagate). (4) Analytics: tag each click_event with `destination_url` to enable per-variant click tracking. (5) Consistency: weighted selection uses the same algorithm on all instances (seeded by request time, not instance — so it's pseudo-random, not coordinated). True uniform distribution is achieved over large N; at 1,157 QPS across a 2-variant test, both variants converge to their weights within seconds.

**Q12: How do you handle the "short URL creation is idempotent from the client's perspective" — a client sends the same POST request twice due to a network timeout?**

A: This is the classic at-most-once vs at-least-once write problem. Solution: **Idempotency keys**. The client includes an `Idempotency-Key: <uuid>` header. The Write Service: (1) Before processing, check Redis: `GET idempotency:<uuid>`. If found: return the stored response immediately (status code + body). (2) If not found: process the request (generate code, write to DB). (3) After successful DB write: store `SET idempotency:<uuid> <response_json> EX 86400` (24h TTL). (4) Return the response. If the client retries due to timeout (after step 2 but before step 3 response was received): the second request finds the key in Redis and returns the same short URL as the first request — correct behavior. Edge case: if the service crashes between DB write and Redis write, the second request will re-process and generate a new short code. This is an acceptable edge case (extremely rare; results in a duplicate short URL for the same long URL, which is valid per our design).

**Q13: How would you design the "bulk URL creation" endpoint for an enterprise customer uploading a CSV of 1 million URLs?**

A: Synchronous processing of 1M URLs in a single HTTP request is infeasible (would time out). Asynchronous job pattern: (1) `POST /api/v1/bulk-jobs` with multipart CSV upload. Server validates CSV format (first 100 rows), stores file to S3, creates a `bulk_job` record in DB with status=`pending`, returns HTTP 202 Accepted with `{"job_id": "j-abc123", "status_url": "/api/v1/bulk-jobs/j-abc123"}`. (2) A Bulk Job Worker (separate service) polls or is triggered by a job queue (SQS/Kafka). It reads the CSV from S3 in streaming fashion (line by line), calls the URL creation logic in batches of 1000, writes results to an output S3 file. (3) Client polls `GET /api/v1/bulk-jobs/j-abc123` → `{"status": "processing", "progress": 45000, "total": 1000000}`. Or use webhooks for push notification on completion. (4) Result download: `GET /api/v1/bulk-jobs/j-abc123/result` → pre-signed S3 URL to output CSV. (5) Rate: Bulk Worker processes ~1000 URLs/second (limited by DB write throughput). 1M URLs ≈ 17 minutes. At 12 writes/sec on the main Write Service, Bulk Worker has a separate DB connection pool to avoid starving interactive API users.

**Q14: How do you guarantee that a short code is never reused after its URL is deleted?**

A: This is a subtle correctness requirement. A deleted short code could be re-issued to a new URL — if a user has bookmarked the old short link, they'd now be redirected to a completely different destination (phishing risk, user confusion). Two approaches: (1) **Soft delete only, never reuse**: Mark deleted URLs with `is_active=FALSE`. The short code row remains in the `urls` table permanently (or for 5 years). The pre-generated pool job checks the `urls` table before adding codes to the pool — so deleted codes are never re-issued. DB storage cost: 500 bytes × (deletions at 10% of 1M/day) = 50K deletions/day × 500 bytes = 25 MB/day = ~9 GB/year — acceptable. (2) **Quarantine period**: After deletion, add the code to a Redis set `quarantine_codes` with TTL = 30 days. Pool generation job checks this set (via `SISMEMBER`) before adding to the pool. After 30 days, the code is eligible for reuse. This allows a 30-day cache/CDN/browser-bookmark window to expire. **We choose option 2** with a 90-day quarantine for enterprise users (their links may be in printed materials). After 90 days, the deleted code returns to the available pool.

**Q15: What changes would you make to support 10 billion URLs over 10 years (vs. 200M in our current design)?**

A: At 10B URLs: (1) **Database sharding becomes mandatory**: 10B × 500 bytes = 5 TB of URL data. Single PostgreSQL instance won't do it. Shard by `hash(short_code) % 256` (256 logical shards, mapped to 16 physical PostgreSQL clusters of 16 shards each). Use a shard routing service (like Vitess) that's transparent to the application. (2) **Key length**: 10B URLs / 3.5T possible 7-char codes = 0.29% utilization — still fine for 7 chars. No need to extend. (3) **Redis working set grows**: At 10B URLs with 20% hot = 2B hot URLs × 500 bytes = 1 TB. Cannot fit in RAM. Solution: use Redis as an L1 cache (only truly hot URLs — top 100M by recency) and add a Memcached or compressed in-process cache tier. Or use RocksDB-backed Redis (RedisRocksDB / Speedb) for cost-effective SSD-backed caching at TB scale. (4) **Short code generation**: At 10B URLs, the pool refill job's `SELECT ... WHERE short_code = ANY(...)` query over 10B rows is slow. Switch to Snowflake IDs exclusively (monotonically increasing, guaranteed unique without DB check) encoded to base62. Accept minor enumerability risk (mitigated by rate limiting). (5) **CDN becomes even more critical**: At 10B URLs but still ~100B redirects/day (10:1 ratio), CDN hit rates must be > 99% to keep origin sustainable. Invest in CDN prefetching for anticipated viral content.

---

## 12. References & Further Reading

- **"Building a URL Shortener" — Alex Xu, System Design Interview Volume 1** (Chapter covering URL shortener design, capacity estimation patterns)
- **"Designing Data-Intensive Applications" — Martin Kleppmann** (Chapter 5: Replication; Chapter 6: Partitioning — foundational for DB sharding and replication decisions)
- **Bitly Engineering Blog — "The Architecture of Bitly"** (bitly.com engineering blog — describes their evolution from single-server to distributed architecture; search for their O'Reilly Velocity talks)
- **"Snowflake: A Network-Attached Storage System for Unique ID Generation" — Twitter Engineering Blog** (Original Snowflake ID design; describes the timestamp + worker + sequence encoding used for distributed unique ID generation)
- **Redis Cluster Specification — redis.io/docs/reference/cluster-spec** (Official specification for Redis Cluster sharding and failover algorithm)
- **PostgreSQL Documentation: Partial Indexes — postgresql.org/docs/current/indexes-partial.html** (Covers partial index creation and use cases, directly relevant to the expiry index design)
- **"Cache Stampede Prevention with Probabilistic Early Expiration" (XFetch algorithm) — Andrea Vattani et al., 2015** (Describes the probabilistic early expiration algorithm for preventing thundering herd on cache expiry)
- **Google Safe Browsing API — developers.google.com/safe-browsing** (Official documentation for the URL safety checking API referenced in the abuse prevention section)
- **"An Analysis of Hash-Based Short URL Systems" — various academic surveys** (Survey papers available on ACM Digital Library covering hash collision probability calculations for URL shorteners)
- **CloudFront Developer Guide: Origin Shield — docs.aws.amazon.com/AmazonCloudFront** (Documents the Origin Shield feature that coalesces cache misses from multiple edge locations into a single request to origin)
- **"Kafka: The Definitive Guide" — Gwen Shapira, Neha Narkhede, Todd Palino** (O'Reilly; covers partition design, consumer group scaling, and exactly-once semantics relevant to the analytics pipeline)
- **"The Twelve-Factor App" — 12factor.net** (Foundational principles behind stateless service design that underpins the horizontal scaling strategy)
