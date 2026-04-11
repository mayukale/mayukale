# Problem-Specific Design — URL Shortener (01_url_shortener)

## URL Shortener

### Unique Functional Requirements
- Redirect short URLs to long URLs via HTTP 301 or 302 response
- Custom alias support (user-chosen short codes)
- Per-link click analytics: click count, referrer, user-agent, geo-location (country, city)
- Bulk creation API for multiple short URLs in a single request
- URL expiration with configurable TTL

### Unique Components / Services
- **Bloom filter**: Checked at redirect time against 10M known malicious domains (SSRF and phishing prevention)
- **Safe Browsing API**: URL validation at creation time (Google Safe Browsing or equivalent)
- **MaxMind GeoIP2**: IP-to-country/city enrichment applied by Flink on click events
- **XFetch algorithm**: Probabilistic cache early expiration to prevent thundering herd on popular URLs
- **Distributed mutex (Redis lock)**: Single-flight pattern for cache-miss coalescing under load

### Unique Data Models
```sql
-- urls table
CREATE TABLE urls (
    id              BIGINT PRIMARY KEY,        -- Snowflake ID
    short_code      VARCHAR(12) NOT NULL UNIQUE,
    long_url        TEXT NOT NULL,
    user_id         BIGINT REFERENCES users(id),
    custom_alias    BOOLEAN DEFAULT FALSE,
    expires_at      TIMESTAMPTZ,
    is_active       BOOLEAN DEFAULT TRUE,
    click_count     BIGINT DEFAULT 0,          -- denormalized, updated async
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    updated_at      TIMESTAMPTZ DEFAULT NOW()
);

-- click_events table (ClickHouse, MergeTree ordered by (short_code, clicked_at))
CREATE TABLE click_events (
    id              BIGINT,
    short_code      VARCHAR(12),
    clicked_at      TIMESTAMPTZ,
    ip_address      INET,
    country_code    CHAR(2),
    city            VARCHAR(100),
    user_agent      TEXT,
    device_type     VARCHAR(20),   -- mobile|desktop|tablet|bot
    referrer        TEXT
);
```
- Redis cache key: `url:<short_code>` → `long_url|expires_at`
- Redis rate limit key: `rl:ip:<ip>` or `rl:user:<id>` with token bucket fields

### Unique Scalability / Design Decisions
- **301 vs 302 redirect choice**: 301 (permanent) is CDN-cacheable indefinitely and opt-in only; 302 (temporary) used by default to capture per-click analytics; this is a core product trade-off
- **Staggered TTL jitter**: Random ±10% jitter added to Redis TTLs to prevent cache expiration storms
- **XFetch probabilistic early expiration**: Recomputes cache entry slightly before TTL expiry to avoid thundering herd on popular short codes
- **Soft deletes with 90-day quarantine**: Deleted short codes are quarantined before being eligible for reuse, preventing user confusion
- **451 Unavailable For Legal Reasons**: Used for malware/phishing blocked URLs (distinct from 404 Not Found)
- **Kafka partition key = short_code**: Ensures all click events for one URL land on the same partition for ordered processing

### Key Differentiator
The URL shortener's core hot path is a sub-20ms HTTP redirect — every design decision (layered CDN + Redis caching, 301/302 choice, pre-generated code pool, XFetch) is optimized to make that redirect as fast as possible while preserving click analytics without adding latency.

---

## Pastebin

### Unique Functional Requirements
- Store and retrieve arbitrary text/code content (up to 1 MB per paste)
- Optional syntax highlighting hint (language tag; rendering is client-side)
- Configurable visibility: public (listed in feed), unlisted (URL-only), private (account-only)
- Optional password protection for paste access
- Paste forking (copy an existing paste as a new one — the "edit" mechanism since content is immutable)
- Raw content endpoint `/raw/<key>` returning plain text without HTML wrapper
- Public feed of recent/trending public pastes
- GDPR compliance: EU user data stays in EU region; reported illegal content removable within hours

### Unique Components / Services
- **S3 (object storage)**: Stores all paste content (not in PostgreSQL); prefix-sharded (`pastes/0/`, `pastes/1/`, ...) to handle > 5,500 RPS per prefix; lifecycle rules for TTL-based auto-expiry
- **Content Scanner**: Background job + at-creation synchronous scan using `detect-secrets` (Yelp) and regex patterns to detect AWS keys, PEM headers, SSNs, PII; auto-sets visibility to unlisted if detected
- **Patroni**: PostgreSQL HA manager for automatic primary election (explicitly named)
- **Redis Pub/Sub**: Used for per-user clipboard sync channel (`paste:user:<user_id>:clipboard`) and distributed locking for key pool refill

### Unique Data Models
```sql
-- pastes table
CREATE TABLE pastes (
    id              BIGINT PRIMARY KEY,        -- Snowflake ID
    paste_key       VARCHAR(8) NOT NULL UNIQUE,-- base62
    user_id         BIGINT REFERENCES users(id),
    title           VARCHAR(200),
    language        VARCHAR(50),               -- syntax hint
    size_bytes      INT NOT NULL,              -- max 1048576 (1 MB)
    sha256_checksum CHAR(64) NOT NULL,         -- integrity + deduplication
    visibility      VARCHAR(10) NOT NULL,      -- public|unlisted|private
    password_hash   CHAR(60),                  -- bcrypt, optional
    expires_at      TIMESTAMPTZ,
    is_active       BOOLEAN DEFAULT TRUE,
    view_count      BIGINT DEFAULT 0,          -- async, approximate
    fork_of         BIGINT REFERENCES pastes(id),
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    updated_at      TIMESTAMPTZ DEFAULT NOW()
);

-- paste_views (ClickHouse, time-series)
CREATE TABLE paste_views (
    id              BIGINT,
    paste_key       VARCHAR(8),
    viewed_at       TIMESTAMPTZ,
    ip_hash         CHAR(64),                  -- SHA-256 of IP (GDPR)
    country_code    CHAR(2),
    referrer_domain VARCHAR(255),
    user_agent_class VARCHAR(20)               -- browser|cli|bot|api
);
```
- S3 object key: `pastes/<shard>/<paste_key>`; S3 metadata: language, paste_key; lifecycle rule: auto-delete after N days via `ExpiryDate` tag
- Redis: `paste:content:<key>` (string, content < 64 KB only); `available_paste_keys` (set, SPOP-based pool); `paste:delete:<key>` (delete token for anonymous pastes)
- Content deduplication: multiple `pastes` rows can reference same S3 object via identical `sha256_checksum`

### Unique Scalability / Design Decisions
- **Content in S3, metadata in PostgreSQL**: Avoids PostgreSQL TOAST bloat at TB scale; S3 is 5x cheaper; native CDN origin; lifecycle rules handle expiry automatically
- **Content immutability + forking**: Immutable content enables aggressive CDN caching (long-lived cache entries); "fork" provides edit semantics without requiring cache invalidation
- **S3 prefix sharding**: `crc32(paste_key) % 16` prefixes used to stay under S3's 5,500 RPS per prefix limit
- **SHA-256 content deduplication**: Multiple paste records sharing identical content point to one S3 object
- **Hybrid expiry**: S3 lifecycle rules (content) + PostgreSQL hourly cron soft-delete (metadata) + Redis TTL (cache) + lazy expiry on read (return 410 Gone, write tombstone)
- **Server-side bcrypt for password protection**: Brute-force guessing rate-limited to 5 attempts/minute per paste key
- **Public feed via Redis Sorted Set**: ZADD on creation, ZREVRANGE for feed queries — O(log N) insert vs DB index scan at scale
- **Delete tokens for anonymous pastes**: Anonymous creators receive a delete token (stored in Redis with TTL = paste lifetime) since they have no account for auth
- **S3 PUT retry with exponential backoff**: Up to 5 retries (100ms, 200ms, 400ms, 800ms, 1600ms ± 20% jitter) for transient S3 errors

### Key Differentiator
Pastebin's defining architectural choice is separating immutable text content (S3) from mutable metadata (PostgreSQL), which enables near-zero-cost CDN caching for all content and eliminates the need for cache invalidation — the "fork to edit" model enforces immutability as a product constraint to preserve this property.
