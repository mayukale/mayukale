# System Design: API Rate Limiter

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Enforce rate limits** per configurable dimensions: user ID, API key, IP address, endpoint, and tenant (for multi-tenant SaaS).
2. **Multiple limit tiers**: free (100 req/min), pro (1,000 req/min), enterprise (10,000 req/min); limits are configurable without redeployment.
3. **Return standard rate limit headers** on every response: `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`, `Retry-After`.
4. **Return HTTP 429 Too Many Requests** with a JSON error body when a client exceeds its limit.
5. **Support multiple time windows**: per-second burst limit AND per-minute sustained limit applied simultaneously.
6. **Rules engine**: admins can define rules (e.g., exempt certain IP ranges, apply stricter limits to unauthenticated callers) via a management API.
7. **Distributed enforcement**: limits must be consistent across all API gateway nodes — a client cannot bypass limits by routing requests to different nodes.
8. **Soft limits and hard limits**: a "warning" threshold (e.g., 80%) can trigger an alert header before a hard block.

### Non-Functional Requirements

1. **Latency overhead**: rate limiting decision must add < 5 ms p99 to each request (the limiter sits in the critical path).
2. **Availability**: 99.99% uptime — the limiter must fail open (allow traffic) when the backing store is unavailable, with an alert.
3. **Throughput**: sustain 500,000 requests/second across the fleet.
4. **Accuracy**: < 1% over-admission at burst boundaries (some over-admission is acceptable; under-admission is not — we prefer to admit slightly more than to block legitimate traffic).
5. **Consistency**: eventual consistency with a propagation window < 100 ms is acceptable across distributed nodes (strong consistency would require synchronous cross-node coordination, which violates the latency budget).
6. **Scalability**: horizontally scalable — adding API gateway nodes should not require reconfiguring the rate limiter.
7. **Auditability**: every 429 response must be logged with client identity, rule matched, and timestamp.
8. **Configuration hot-reload**: limit rules must take effect within 30 seconds of an admin update without a rolling restart.

### Out of Scope

- **DDoS mitigation at the network layer** (L3/L4 — handled by cloud WAF/CDN like Cloudflare or AWS Shield).
- **Authentication/authorization** — we assume a valid identity (user ID or API key) is already attached to the request by the auth middleware upstream.
- **Request queuing / delayed execution** — this design blocks or rejects; a separate throttling service handles queuing (see `throttling_service.md`).
- **Cost-based rate limiting** (e.g., limiting by compute cost of LLM tokens) — a specialized concern.
- **Geographic rate limiting** (blocking by country) — handled at CDN layer.

---

## 2. Users & Scale

### User Types

| Actor | Description | Typical Behavior |
|---|---|---|
| **Free-tier API consumers** | Developers, hobbyists | Bursty, low volume; ~100 req/min limit |
| **Pro-tier API consumers** | Startups, small businesses | Moderate, semi-predictable; ~1,000 req/min |
| **Enterprise API consumers** | Large companies with SLAs | High volume, consistent; ~10,000 req/min |
| **Unauthenticated callers** | Pre-auth traffic, scrapers | Lowest limits; rate-limited by IP |
| **Internal services** | Microservices calling each other | Exempt or high-limit; still tracked |
| **Admin operators** | Platform engineers | Low-volume management API calls |

### Traffic Estimates

**Assumptions:**
- Platform has 10 million registered API consumers (clients).
- Active daily clients: 20% = 2,000,000.
- Tier distribution: 80% free, 18% pro, 2% enterprise.
- Average request rate per active client:
  - Free: 5 req/min average (peak usage at 40% of their 100 req/min limit)
  - Pro: 200 req/min average (20% of 1,000 req/min)
  - Enterprise: 2,000 req/min average (20% of 10,000 req/min)

| Metric | Calculation | Result |
|---|---|---|
| Free-tier clients (active) | 2,000,000 × 0.80 | 1,600,000 |
| Pro-tier clients (active) | 2,000,000 × 0.18 | 360,000 |
| Enterprise clients (active) | 2,000,000 × 0.02 | 40,000 |
| Avg RPS from free tier | 1,600,000 × 5 req/min ÷ 60 | ~133,333 RPS |
| Avg RPS from pro tier | 360,000 × 200 req/min ÷ 60 | ~1,200,000 RPS |
| Avg RPS from enterprise | 40,000 × 2,000 req/min ÷ 60 | ~1,333,333 RPS |
| **Total average RPS** | 133,333 + 1,200,000 + 1,333,333 | **~2,666,666 RPS** |
| Peak RPS (3× average) | 2,666,666 × 3 | **~8,000,000 RPS** |
| Rate limit checks per request | 2 (per-minute + per-second) | — |
| **Total rate-limit ops/sec (peak)** | 8,000,000 × 2 | **~16,000,000 ops/sec** |

> **Implication**: At 16M ops/sec, Redis Cluster with pipelining is mandatory. A single Redis instance maxes out at ~1M simple ops/sec. We need a sharded Redis cluster.

### Latency Requirements

| Operation | Target p50 | Target p99 | Notes |
|---|---|---|---|
| Rate limit check (cache hit) | < 1 ms | < 3 ms | Redis GET + EVAL |
| Rate limit check (cache miss) | < 5 ms | < 10 ms | Fallback to local counter |
| Rules fetch (hot cache) | < 0.5 ms | < 2 ms | In-process LRU cache |
| 429 response generation | < 1 ms | < 2 ms | No DB call needed |
| Admin rules update | < 50 ms | < 200 ms | Async propagation acceptable |

### Storage Estimates

**Per counter entry size:**
- Key: `rl:{client_id}:{window}` = ~50 bytes (UTF-8)
- Value: integer counter = 8 bytes
- Redis overhead per key: ~100 bytes (dict entry, TTL, encoding)
- Total per entry: ~160 bytes

| Dimension | Calculation | Result |
|---|---|---|
| Unique rate-limit keys (active windows) | 2,000,000 active clients × 2 windows (min + sec) | 4,000,000 keys |
| Storage for counters | 4,000,000 × 160 bytes | ~640 MB |
| Rules store (JSON blobs per tier + overrides) | 10,000 rules × 2 KB each | ~20 MB |
| Audit log (429 events only) | 8,000,000 RPS × 0.1% rejection × 200 bytes × 86,400 sec | ~138 GB/day |
| Audit log (compressed, 10:1) | 138 GB ÷ 10 | ~14 GB/day |

> Counter storage easily fits in a Redis Cluster with 8 GB RAM. The audit log is written to object storage (S3) via a Kafka pipeline.

### Bandwidth Estimates

| Traffic Type | Calculation | Result |
|---|---|---|
| Inbound request headers read by limiter | 8,000,000 RPS × 500 bytes avg header | ~4 GB/s |
| Redis counter ops (read + write) | 16,000,000 ops × 50 bytes avg payload | ~800 MB/s |
| Response headers added (X-RateLimit-*) | 8,000,000 RPS × 150 bytes (3 headers) | ~1.2 GB/s |
| Kafka audit events | 8,000 rejected/sec × 500 bytes | ~4 MB/s |

---

## 3. High-Level Architecture

```
                          ┌─────────────────────────────────────────────────────┐
                          │                    CLIENT LAYER                     │
                          │   Mobile Apps │ Browser JS │ Server-side API calls  │
                          └────────────────────────┬────────────────────────────┘
                                                   │ HTTPS
                                                   ▼
                          ┌─────────────────────────────────────────────────────┐
                          │               CDN / WAF (Cloudflare)                │
                          │  - L3/L4 DDoS protection (out of scope)             │
                          │  - TLS termination                                  │
                          └────────────────────────┬────────────────────────────┘
                                                   │ HTTP/2
                                                   ▼
              ┌────────────────────────────────────────────────────────────────────┐
              │                   API GATEWAY CLUSTER (N nodes)                    │
              │  ┌─────────────────────────────────────────────────────────────┐  │
              │  │                   REQUEST PIPELINE                          │  │
              │  │                                                             │  │
              │  │  [Auth Middleware] → [Rate Limit Middleware] → [Proxy]      │  │
              │  │         │                     │                             │  │
              │  │   Extract identity      ┌─────┴──────┐                     │  │
              │  │   (API key/JWT)         │            │                     │  │
              │  │                    Rules Cache   Counter Check             │  │
              │  │                   (in-process)   (Redis Cluster)           │  │
              │  └─────────────────────────────────────────────────────────┘  │
              └────────────────┬──────────────────────────┬─────────────────────┘
                               │ Allow (< 5ms added)       │ Reject (HTTP 429)
                               ▼                           ▼
              ┌────────────────────────┐    ┌──────────────────────────────────┐
              │   UPSTREAM SERVICES    │    │        REJECT RESPONSE           │
              │  (microservices,       │    │  429 + X-RateLimit-* headers     │
              │   databases, etc.)     │    │  + Retry-After header            │
              └────────────────────────┘    └──────────────────────────────────┘
                                                           │
                                                           ▼ async
                                            ┌─────────────────────────────┐
                                            │       KAFKA (audit topic)   │
                                            └──────────────┬──────────────┘
                                                           │
                                                           ▼
                                            ┌─────────────────────────────┐
                                            │   AUDIT SINK (S3 + Athena)  │
                                            └─────────────────────────────┘

      ┌──────────────────────────────────────────────────────────────────────────┐
      │                         CONTROL PLANE                                   │
      │                                                                          │
      │  ┌──────────────────┐    ┌─────────────────────┐    ┌─────────────────┐ │
      │  │  Rules Admin API │───▶│  Rules Store (MySQL) │───▶│  Rules Cache    │ │
      │  │  (CRUD for tiers │    │  (source of truth    │    │  (Redis, 30s    │ │
      │  │   and overrides) │    │   for all rules)     │    │   TTL)          │ │
      │  └──────────────────┘    └─────────────────────┘    └────────┬────────┘ │
      │                                                               │ subscribe│
      │                                                               ▼          │
      │                                            ┌────────────────────────────┐│
      │                                            │  Gateway nodes poll/sub    ││
      │                                            │  rules every 30s (in-proc  ││
      │                                            │  LRU refreshed from Redis) ││
      │                                            └────────────────────────────┘│
      └──────────────────────────────────────────────────────────────────────────┘

      ┌──────────────────────────────────────────────────────────────────────────┐
      │                    REDIS CLUSTER (Counter Store)                         │
      │                                                                          │
      │   Shard 0        Shard 1        Shard 2   ...    Shard N                │
      │  (clients A-D)  (clients E-H)  (clients I-L)    (clients W-Z)           │
      │                                                                          │
      │  Each shard: 1 primary + 2 replicas (read replicas for observability)   │
      └──────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

| Component | Role |
|---|---|
| **CDN/WAF** | Absorbs network-layer floods; provides anycast routing. Not part of this design. |
| **API Gateway Cluster** | Entry point for all API traffic; hosts the rate-limit middleware in-process. Stateless — can be scaled horizontally. |
| **Rate Limit Middleware** | Extracts client identity, fetches applicable rules from in-process cache, performs atomic counter increment in Redis, sets response headers, returns 429 if limit exceeded. |
| **In-Process Rules Cache** | LRU cache per gateway node holding all active rules. Refreshed from Redis rules cache every 30 seconds. Eliminates a Redis round-trip for rule lookups. |
| **Redis Cluster (Counter Store)** | Authoritative store for all sliding window counters. Sharded by client key hash. Each counter has a TTL equal to the window size. |
| **Rules Store (MySQL)** | Source of truth for all rate limit rules and tier configurations. Written by the admin API. Durably persisted. |
| **Rules Cache (Redis)** | A Redis key-value store of serialized rule objects. Populated by the admin API on write; read by gateway nodes. Acts as a fan-out mechanism to all gateway nodes. |
| **Kafka (Audit Topic)** | Decouples 429 event logging from the critical request path. Gateway nodes produce events; audit sink consumes. |
| **Audit Sink (S3 + Athena)** | Long-term storage for 429 audit events. Queryable with Athena for abuse analysis and billing disputes. |
| **Rules Admin API** | Internal CRUD API for managing rules. Writes to MySQL and invalidates Redis rules cache. Requires admin auth. |

**Primary Use-Case Data Flow (Allowed Request):**
1. Client sends `GET /v1/data` with `Authorization: Bearer <api_key>`.
2. Gateway auth middleware validates the key and attaches `user_id=u123`, `tier=pro` to request context.
3. Rate limit middleware reads `tier=pro` from context; looks up rule in in-process cache: `{limit: 1000, window: 60s}`.
4. Middleware constructs Redis key: `rl:u123:60:1735689600` (window bucket = floor(now/60)).
5. Middleware runs Lua script on Redis shard for key hash of `u123`: `INCR key; EXPIRE key 60` atomically.
6. Redis returns new counter value `142`. Limit is 1000. Request is allowed.
7. Middleware sets headers: `X-RateLimit-Limit: 1000`, `X-RateLimit-Remaining: 858`, `X-RateLimit-Reset: 1735689660`.
8. Request is forwarded to upstream service. Response flows back through gateway with the rate limit headers appended.

---

## 4. Data Model

### Entities & Schema

**1. `rate_limit_rules` (MySQL — rules store)**

```sql
CREATE TABLE rate_limit_rules (
    id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    rule_name     VARCHAR(128)    NOT NULL,          -- human-readable name, e.g. "pro_tier_per_minute"
    dimension     ENUM('api_key','user_id','ip','tenant','endpoint','global') NOT NULL,
    tier          VARCHAR(64),                        -- NULL = applies to all tiers; "free", "pro", "enterprise"
    endpoint_glob VARCHAR(256),                       -- NULL = all endpoints; e.g. "/v1/search*"
    limit_count   INT UNSIGNED    NOT NULL,           -- number of requests allowed
    window_seconds SMALLINT UNSIGNED NOT NULL,        -- window size in seconds: 1, 60, 3600
    burst_limit   INT UNSIGNED,                       -- optional: instantaneous burst (per-second)
    algorithm     ENUM('sliding_window_counter','token_bucket') NOT NULL DEFAULT 'sliding_window_counter',
    priority      TINYINT UNSIGNED NOT NULL DEFAULT 100, -- lower = higher priority; most-specific rule wins
    is_active     BOOLEAN         NOT NULL DEFAULT TRUE,
    created_at    TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at    TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    created_by    VARCHAR(128)    NOT NULL,
    INDEX idx_dimension_tier (dimension, tier),
    INDEX idx_active (is_active),
    INDEX idx_priority (priority)
);
```

**2. `client_overrides` (MySQL — per-client exceptions)**

```sql
CREATE TABLE client_overrides (
    id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    client_type   ENUM('api_key','user_id','ip','tenant') NOT NULL,
    client_value  VARCHAR(256)    NOT NULL,           -- the actual key/IP/user_id
    rule_id       BIGINT UNSIGNED NOT NULL,           -- FK to rate_limit_rules
    override_limit INT UNSIGNED,                      -- NULL = use rule default
    override_window_seconds SMALLINT UNSIGNED,
    is_exempt     BOOLEAN         NOT NULL DEFAULT FALSE, -- if TRUE, skip rate limiting entirely
    expires_at    TIMESTAMP,                          -- NULL = no expiry
    reason        TEXT,                               -- justification for override
    created_at    TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by    VARCHAR(128)    NOT NULL,
    FOREIGN KEY (rule_id) REFERENCES rate_limit_rules(id),
    INDEX idx_client (client_type, client_value),
    INDEX idx_expires (expires_at)
);
```

**3. Redis Counter Keys (counter store)**

```
Key format:   rl:{dimension}:{client_value}:{window_seconds}:{window_bucket}
Example:      rl:uid:u123:60:28928160
              rl:ip:203.0.113.5:60:28928160
              rl:apikey:ak_xyz:3600:481136

Value:        Integer (INCR counter)
TTL:          window_seconds × 2  (2× window to handle clock skew at window boundaries)
```

- `window_bucket` = `floor(unix_timestamp / window_seconds)` — fixed bucket ID.
- Using 2× TTL ensures the key expires well after the window closes without needing explicit cleanup.
- For sliding window counter (the primary algorithm), TWO keys are maintained: current window + previous window.

**4. Redis Rules Cache Keys (rules cache)**

```
Key format:   rules:all                           -- serialized JSON array of all active rules
              rules:tier:{tier_name}              -- serialized JSON array of rules for a tier
              rules:override:{client_type}:{val}  -- serialized JSON for a specific client override

Value:        JSON blob
TTL:          60 seconds (gateway nodes poll at 30s; 60s TTL is a safety net)
```

**5. Audit Log Schema (Kafka → S3/Parquet)**

```json
{
  "event_id":      "uuid-v4",
  "timestamp":     "2025-01-01T00:00:00.000Z",
  "client_type":   "api_key",
  "client_value":  "ak_xyz_hashed",
  "user_id":       "u123",
  "tenant_id":     "t456",
  "ip_address":    "203.0.113.5",
  "endpoint":      "/v1/search",
  "http_method":   "GET",
  "rule_matched":  "pro_tier_per_minute",
  "limit":         1000,
  "window_seconds": 60,
  "counter_value": 1001,
  "gateway_node":  "gw-us-east-1a-007",
  "request_id":    "req-uuid"
}
```

### Database Choice

**For Counter Store:**

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **Redis Cluster** | Sub-millisecond latency; atomic Lua scripts; built-in TTL; hash slots enable horizontal sharding | Volatile (data lost on failure without AOF); requires careful cluster management | **Selected** |
| **Memcached** | Slightly lower latency than Redis; simpler | No Lua scripting; no TTL-based auto-expiry per key in all versions; no replication | Rejected — lack of atomic scripts is fatal for accuracy |
| **Cassandra** | Highly durable; great for write-heavy; tunable consistency | p99 latency 5-15 ms — exceeds our 3 ms p99 budget; no atomic increment-and-expire | Rejected — too slow |
| **DynamoDB** | Fully managed; atomic conditional writes | ~5-10 ms p99 minimum; cost at 16M ops/sec prohibitive | Rejected — latency and cost |
| **In-process (local)** | Zero network latency | Not distributed — each gateway node has independent counters; clients bypass by round-robining nodes | Rejected — violates distributed requirement |

**Justification for Redis Cluster:** Redis's `EVAL` (Lua scripting) provides atomicity for the increment-check-expire sequence without requiring a distributed lock. Hash slots enable consistent hashing so a given client key always hits the same shard. The `PERSIST`/`EXPIRE` commands natively implement TTL-based window expiry. At 160 bytes per counter and ~4M active counters, total memory is ~640 MB — trivially fits in a standard Redis node.

**For Rules Store:**

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **MySQL (InnoDB)** | ACID transactions; strong consistency; mature admin tooling; row-level locking for concurrent rule updates | Not as fast as NoSQL for high-read; requires connection pooling | **Selected** |
| **PostgreSQL** | Similar to MySQL; better JSON support | Equivalent for this use case; MySQL is the conventional choice here | Viable alternative |
| **MongoDB** | Flexible schema; easy to store rule JSON | Eventual consistency by default; overkill for structured rule data | Rejected |

**Justification:** Rules are written infrequently (admin operations) but must be strongly consistent — a new rule must be durably committed before it's visible to gateway nodes. MySQL InnoDB provides ACID guarantees. The total rules dataset is tiny (< 20 MB), so read performance is not a concern.

---

## 5. API Design

All management endpoints require `Authorization: Bearer <admin_token>` and are served on an internal network (not exposed to the public internet).

All rate-limited API endpoints (the downstream APIs being protected) are not described here — they are arbitrary. What we describe below is the rate limiter's own management API.

---

**GET /v1/ratelimit/rules**

Fetch all active rate limit rules.

```
GET /v1/ratelimit/rules?tier=pro&dimension=api_key&page=1&page_size=50
Authorization: Bearer <admin_token>

Response 200 OK:
{
  "rules": [
    {
      "id": 42,
      "rule_name": "pro_tier_per_minute",
      "dimension": "api_key",
      "tier": "pro",
      "endpoint_glob": null,
      "limit_count": 1000,
      "window_seconds": 60,
      "burst_limit": 50,
      "algorithm": "sliding_window_counter",
      "priority": 50,
      "is_active": true,
      "created_at": "2025-01-01T00:00:00Z",
      "created_by": "admin@example.com"
    }
  ],
  "pagination": {
    "page": 1,
    "page_size": 50,
    "total": 1
  }
}
```

---

**POST /v1/ratelimit/rules**

Create a new rule.

```
POST /v1/ratelimit/rules
Authorization: Bearer <admin_token>
Content-Type: application/json

{
  "rule_name": "free_tier_per_minute",
  "dimension": "user_id",
  "tier": "free",
  "limit_count": 100,
  "window_seconds": 60,
  "burst_limit": 20,
  "algorithm": "sliding_window_counter",
  "priority": 100
}

Response 201 Created:
{
  "id": 101,
  "rule_name": "free_tier_per_minute",
  ...
}

Response 409 Conflict:
{
  "error": "RULE_CONFLICT",
  "message": "A rule for dimension=user_id, tier=free, window=60 already exists (id=42). Update that rule or delete it first."
}
```

---

**PUT /v1/ratelimit/rules/{rule_id}**

Update an existing rule. Change takes effect within 30 seconds (next cache refresh cycle).

```
PUT /v1/ratelimit/rules/42
Authorization: Bearer <admin_token>
Content-Type: application/json

{
  "limit_count": 1500,
  "burst_limit": 75
}

Response 200 OK:
{
  "id": 42,
  "limit_count": 1500,
  ...
  "updated_at": "2025-06-01T12:00:00Z"
}

Rate Limits on this endpoint: 60 requests/minute per admin token.
```

---

**DELETE /v1/ratelimit/rules/{rule_id}**

Soft-delete a rule (sets `is_active = false`). Hard delete requires a separate audit-approved process.

```
DELETE /v1/ratelimit/rules/42
Authorization: Bearer <admin_token>

Response 204 No Content

Rate Limits: 60 requests/minute per admin token.
```

---

**POST /v1/ratelimit/overrides**

Create a per-client override (exempt a specific API key, or grant higher limits).

```
POST /v1/ratelimit/overrides
Authorization: Bearer <admin_token>
Content-Type: application/json

{
  "client_type": "api_key",
  "client_value": "ak_enterprise_xyz",
  "rule_id": 42,
  "override_limit": 5000,
  "is_exempt": false,
  "expires_at": "2025-12-31T23:59:59Z",
  "reason": "Enterprise contract SLA upgrade Q4 2025"
}

Response 201 Created:
{ "id": 201, ... }
```

---

**GET /v1/ratelimit/status/{client_type}/{client_value}**

Inspect the current counter state for a specific client. Useful for debugging.

```
GET /v1/ratelimit/status/api_key/ak_xyz
Authorization: Bearer <admin_token>

Response 200 OK:
{
  "client_type": "api_key",
  "client_value": "ak_xyz",
  "windows": [
    {
      "window_seconds": 60,
      "limit": 1000,
      "current_count": 142,
      "remaining": 858,
      "reset_at": "2025-01-01T00:01:00Z"
    },
    {
      "window_seconds": 1,
      "limit": 50,
      "current_count": 3,
      "remaining": 47,
      "reset_at": "2025-01-01T00:00:01Z"
    }
  ],
  "is_exempt": false,
  "active_override": null
}
```

---

**Standard Rate Limit Response Headers (on every proxied response):**

```
X-RateLimit-Limit: 1000
X-RateLimit-Remaining: 858
X-RateLimit-Reset: 1735689660
X-RateLimit-Policy: "1000;w=60;comment=pro_tier"
Retry-After: 42                  (only present on 429 responses; seconds until reset)
```

**Standard 429 Response Body:**

```json
{
  "error": "RATE_LIMIT_EXCEEDED",
  "message": "You have exceeded the rate limit of 1000 requests per 60 seconds.",
  "limit": 1000,
  "window_seconds": 60,
  "retry_after_seconds": 42,
  "rule": "pro_tier_per_minute",
  "documentation_url": "https://docs.example.com/rate-limits"
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Rate Limiting Algorithm Selection

**Problem it solves:** Given a request from a known client, determine in < 3 ms p99 whether the client is within their allowed limit for the current time window, atomically increment their counter, and return accurate headers — without allowing significant bursts beyond the configured limit or introducing starvation.

---

#### Algorithm Comparison

**Algorithm 1: Fixed Window Counter**

```
How it works:
  window_key = floor(now / window_seconds)
  count = INCR("rl:{client}:{window_key}")
  if count == 1: EXPIRE("rl:{client}:{window_key}", window_seconds)
  if count > limit: REJECT
  else: ALLOW
```

- **Memory:** O(1) per client per window. One integer per active window. ~160 bytes.
- **Accuracy:** Poor. A client can send `limit` requests in the last second of window N and `limit` requests in the first second of window N+1 — a 2× burst at boundaries.
- **Bursts allowed:** Yes — double-burst at window boundaries is possible.
- **Implementation complexity:** Very low. Two Redis commands.
- **Atomicity issue:** The INCR and EXPIRE are two separate commands — if the process crashes between them, the key has no TTL (memory leak). Mitigated by wrapping in a Lua script.
- **When to use:** Simple internal service limits where occasional double-bursts are acceptable; lowest implementation cost.

```
Boundary burst example:
  Window size: 60s, limit: 100
  t=59s: client sends 100 requests → all allowed (count=100 in window 1)
  t=61s: client sends 100 requests → all allowed (count=100 in window 2)
  ⇒ 200 requests in 2 seconds: 2× the intended limit
```

---

**Algorithm 2: Sliding Window Log**

```
How it works:
  key = "rl_log:{client}"
  now_ms = current_time_ms()
  window_start = now_ms - (window_seconds * 1000)

  MULTI
    ZREMRANGEBYSCORE(key, 0, window_start)    -- remove expired entries
    ZADD(key, now_ms, request_uuid)           -- log this request
    ZCARD(key)                                -- count entries in window
    EXPIRE(key, window_seconds)
  EXEC

  count = result[2]
  if count > limit: REJECT (and ZREM the entry just added)
  else: ALLOW
```

- **Memory:** O(requests_in_window) per client. Each entry in the sorted set is ~50 bytes (score + member UUID). A client at limit (1,000 req/min) = 1,000 × 50 bytes = **50 KB per client**. With 2M active clients = **100 GB** — completely impractical at scale.
- **Accuracy:** Perfect. Every request is timestamped; the window slides precisely.
- **Bursts allowed:** No. Exact per-window enforcement.
- **Implementation complexity:** Medium. Requires sorted set and multi-command transaction.
- **When to use:** Only suitable for very low-volume clients (e.g., < 100 req/min) or where perfect accuracy is a hard business requirement and the client population is small (< 10,000).

---

**Algorithm 3: Sliding Window Counter**

```
How it works:
  Approximation: blend the current and previous window counts
  weighted by how far into the current window we are.

  now = current_unix_timestamp()
  window_size = 60 (seconds)
  current_bucket = floor(now / window_size)
  prev_bucket = current_bucket - 1

  current_key = "rl:{client}:60:{current_bucket}"
  prev_key    = "rl:{client}:60:{prev_bucket}"

  -- Lua script (atomic):
  prev_count    = GET(prev_key) or 0
  current_count = GET(current_key) or 0
  elapsed_in_window = now - (current_bucket * window_size)
  weight = 1.0 - (elapsed_in_window / window_size)  -- 0.0 to 1.0

  estimated_count = (prev_count * weight) + current_count + 1  -- +1 for this request

  if estimated_count > limit:
    RETURN REJECT, estimated_count
  else:
    INCR(current_key)
    EXPIRE(current_key, window_size * 2)
    RETURN ALLOW, estimated_count
```

- **Memory:** O(1) per client — exactly 2 keys per window size. ~320 bytes per client per window dimension.
- **Accuracy:** Very good (< 1% error in practice). The weighted blend is an approximation that assumes uniform request distribution within the previous window — a reasonable assumption validated by Cloudflare in production.
- **Bursts allowed:** Minimal. The blending significantly smooths out boundary bursts. In the worst case (all `prev_count` requests in the last second of the prior window), the weight approaches 1.0 — the full previous count is carried over, preventing double-bursting.
- **Implementation complexity:** Medium. Requires a Lua script for atomicity; straightforward logic.
- **When to use:** **The recommended default for most production systems.** Excellent accuracy/memory trade-off. Used by Cloudflare, Stripe, Kong, and Redis Labs' own rate limiting documentation.

```
Accuracy analysis:
  window_size = 60s, limit = 100
  At t=45s into current window (elapsed=45s, weight=0.25):
  prev_count = 100 (all used), current_count = 0
  estimated = 100 * 0.25 + 0 + 1 = 26 → ALLOW

  This correctly allows requests because 75% of the previous window
  has "expired" from consideration. A fixed window counter would
  have allowed up to 200 requests at the boundary.
```

---

**Algorithm 4: Token Bucket**

```
How it works:
  Tokens refill at a constant rate (rate = limit/window_seconds tokens/sec).
  A request consumes 1 token. If tokens > 0: allow; else: reject.
  Burst capacity = bucket_size (can exceed sustained rate temporarily).

  key = "tb:{client}"
  now = current_time()

  -- Lua script:
  data = GET(key)  -- returns {tokens, last_refill_time}
  if data == nil:
    tokens = capacity
    last_refill = now

  elapsed = now - last_refill
  refill_amount = elapsed * (limit / window_seconds)
  tokens = min(capacity, tokens + refill_amount)
  last_refill = now

  if tokens >= 1:
    tokens = tokens - 1
    SET(key, {tokens, last_refill}, EX=window_seconds*2)
    RETURN ALLOW, tokens
  else:
    SET(key, {tokens, last_refill}, EX=window_seconds*2)
    RETURN REJECT, 0
```

- **Memory:** O(1) per client. One key storing two floats + timestamp = ~80 bytes. Very efficient.
- **Accuracy:** Excellent for sustained rate control. Allows intentional bursts up to `capacity` tokens.
- **Bursts allowed:** Yes — **by design**. `bucket_size > limit/window` means controlled bursts are permitted. This is a feature, not a bug.
- **Implementation complexity:** Medium-high. Floating-point arithmetic in Lua; requires careful handling of time precision and Redis serialization.
- **When to use:** When you want to **allow controlled bursts** while enforcing a sustained rate. Ideal for: API endpoints where clients occasionally need to send a burst of requests (e.g., batch operations), gaming APIs, streaming APIs. Not ideal when you need strict per-window count enforcement.

```
Example:
  limit = 100/min, capacity = 200 (2× burst allowed)
  refill_rate = 100/60 ≈ 1.67 tokens/sec

  Client is idle for 2 minutes → bucket fills to max capacity = 200
  Client sends 200 requests in 1 second → all allowed (burst)
  Client then waits until tokens refill before sending more
```

---

**Algorithm 5: Leaky Bucket**

```
How it works:
  Requests enter a queue (the "bucket"). A background process
  drains the queue at a constant rate (rate = limit/window_seconds req/sec).
  If the queue is full, new requests are rejected.
  All admitted requests are processed at the constant drain rate.

  key = "lb:{client}"
  queue_key = "lb_queue:{client}"
  max_queue_size = limit

  -- On each incoming request:
  queue_length = LLEN(queue_key)
  if queue_length >= max_queue_size:
    RETURN REJECT
  else:
    RPUSH(queue_key, request_id)
    RETURN QUEUED  -- response is deferred until the request is drained
```

- **Memory:** O(queue_depth) per client. At max queue depth (limit requests), each entry ~50 bytes. For limit=1000: 50 KB per client. With 2M clients = 100 GB. In practice, most queues are near-empty; but worst-case is identical to sliding window log.
- **Accuracy:** Perfect rate smoothing. Output rate is exactly `drain_rate`. No bursts in output.
- **Bursts allowed:** No — that's the point. Output is always smooth.
- **Implementation complexity:** Very high. Requires a background drain process, queue per client, coordination between the queue producer (rate limiter) and consumer (request processor). Latency becomes non-deterministic.
- **When to use:** Suited for **output rate smoothing** — e.g., a notification service that must send at most N emails/second to avoid overwhelming an SMTP server. NOT suitable for an API rate limiter that must return immediate accept/reject decisions, because queuing introduces variable latency.

---

#### Algorithm Comparison Summary Table

| Algorithm | Memory/Client | Burst Allowed | Boundary Accuracy | Complexity | Best Use Case |
|---|---|---|---|---|---|
| Fixed Window Counter | O(1) ~160B | Yes (2× burst) | Poor | Very Low | Simple internal limits |
| Sliding Window Log | O(requests) ~50KB at limit | No | Perfect | Medium | Low-volume, exact enforcement |
| **Sliding Window Counter** | **O(1) ~320B** | **Minimal** | **Very Good (<1% error)** | **Medium** | **Default: high-scale APIs** |
| Token Bucket | O(1) ~80B | Yes (configurable) | Excellent for sustained | Medium-High | Burst-tolerant APIs |
| Leaky Bucket | O(queue) | No (smoothed output) | Perfect (output rate) | Very High | Output rate smoothing |

#### Selected Algorithm: Sliding Window Counter (Primary) + Token Bucket (Burst Control)

**Reasoning:**
- Sliding window counter is the industry standard for API rate limiting because it provides near-perfect accuracy with O(1) memory. Cloudflare's blog post "Counting Things at Scale" validates this approach at massive scale.
- We layer a **per-second token bucket** (burst limiter) on top of the per-minute sliding window counter. This catches instantaneous floods (e.g., 1,000 requests in 1 second from a 1,000/min client) without adding significant complexity.
- The two-key sliding window approach (current + previous bucket) maps naturally to Redis's key expiry model.

#### Implementation Detail: Lua Script for Sliding Window Counter + Burst Check

```lua
-- Redis Lua Script: sliding_window_rate_limit.lua
-- Keys: [1]=current_minute_key, [2]=prev_minute_key, [3]=burst_key
-- Args: [1]=limit_per_minute, [2]=burst_limit_per_second, [3]=current_timestamp, [4]=window_seconds

local current_key = KEYS[1]   -- e.g. "rl:uid:u123:60:28928160"
local prev_key    = KEYS[2]   -- e.g. "rl:uid:u123:60:28928159"
local burst_key   = KEYS[3]   -- e.g. "rl:uid:u123:1:1735689600"

local limit        = tonumber(ARGV[1])
local burst_limit  = tonumber(ARGV[2])
local now          = tonumber(ARGV[3])
local window       = tonumber(ARGV[4])

-- ---- Burst check (per-second token bucket simplified as fixed window) ----
local burst_count = tonumber(redis.call('GET', burst_key) or '0')
if burst_count >= burst_limit then
    return {-1, 0, 0}  -- {-1 = burst rejected, current_count, remaining}
end

-- ---- Sliding window counter check ----
local prev_count    = tonumber(redis.call('GET', prev_key) or '0')
local current_count = tonumber(redis.call('GET', current_key) or '0')

local elapsed_in_window = now % window  -- seconds elapsed in current window
local weight = 1.0 - (elapsed_in_window / window)
local estimated_count = math.floor(prev_count * weight) + current_count

if estimated_count >= limit then
    return {-2, estimated_count, 0}  -- {-2 = window rejected, estimated_count, 0 remaining}
end

-- ---- Both checks passed: increment ----
redis.call('INCR', current_key)
redis.call('EXPIRE', current_key, window * 2)

redis.call('INCR', burst_key)
redis.call('EXPIRE', burst_key, 2)  -- 2-second TTL for per-second burst key

local remaining = limit - estimated_count - 1
return {1, estimated_count + 1, remaining}  -- {1 = allowed, new_count, remaining}
```

**Calling the script from Go (gateway middleware):**

```go
// RateLimitCheck performs atomic sliding window + burst check
func (r *RedisLimiter) RateLimitCheck(ctx context.Context, clientID string, rule Rule) (LimitResult, error) {
    now := time.Now().Unix()
    windowBucket := now / rule.WindowSeconds
    prevBucket   := windowBucket - 1

    currentKey := fmt.Sprintf("rl:uid:%s:%d:%d", clientID, rule.WindowSeconds, windowBucket)
    prevKey    := fmt.Sprintf("rl:uid:%s:%d:%d", clientID, rule.WindowSeconds, prevBucket)
    burstKey   := fmt.Sprintf("rl:uid:%s:1:%d", clientID, now)

    result, err := r.lua.Run(ctx, r.client,
        []string{currentKey, prevKey, burstKey},
        rule.Limit, rule.BurstLimit, now, rule.WindowSeconds,
    ).Slice()

    if err != nil {
        // Fail open: Redis unavailable — allow request, log error
        metrics.Counter("rate_limiter.redis_error").Inc(1)
        return LimitResult{Allowed: true, Remaining: -1}, nil
    }

    code      := int(result[0].(int64))
    count     := int(result[1].(int64))
    remaining := int(result[2].(int64))
    resetAt   := (windowBucket + 1) * rule.WindowSeconds

    switch code {
    case 1:
        return LimitResult{Allowed: true, Count: count, Remaining: remaining, ResetAt: resetAt}, nil
    case -1:
        return LimitResult{Allowed: false, Reason: "burst_limit", Count: count, ResetAt: now + 1}, nil
    case -2:
        return LimitResult{Allowed: false, Reason: "window_limit", Count: count, ResetAt: resetAt}, nil
    default:
        return LimitResult{Allowed: true}, nil  // unknown code: fail open
    }
}
```

#### Interviewer Q&A

**Q1: Why use a Lua script instead of Redis MULTI/EXEC transactions?**
A: MULTI/EXEC in Redis provides optimistic locking with WATCH — if another client modifies a watched key, the transaction aborts and you must retry. Under high contention (many concurrent requests from the same client hitting the same Redis shard), this causes retry storms. Lua scripts execute atomically on the Redis server: no network round-trips between operations, no contention, no retries. The entire script is a single atomic operation from Redis's perspective. This is critical for accuracy — without atomicity, two concurrent requests could both read the same counter value and both decide to allow themselves, causing over-admission.

**Q2: What happens if Redis is down? You said "fail open" — isn't that dangerous?**
A: It's a deliberate trade-off. The alternative is "fail closed" — reject all traffic when Redis is unavailable. At 8M RPS, a Redis outage that causes fail-closed would be an outage of the entire API platform. This is far worse than temporarily over-admitting some traffic. We mitigate the risk of Redis failure with: (1) Redis Sentinel or Cluster with automatic failover reducing MTTR to < 30 seconds; (2) an alert fires immediately when the rate limiter falls back to fail-open; (3) gateway nodes maintain an in-process local counter as a secondary limit — it's not distributed but caps individual node throughput. The combination ensures a Redis failure causes a brief degradation (not a complete bypass) while keeping the API available.

**Q3: How accurate is the sliding window counter approximation really? Can you prove the < 1% error claim?**
A: The error is bounded by the assumption that requests in the previous window were uniformly distributed. Let the actual distribution of requests in the previous window be any function f(t). The true sliding window count is the integral of f(t) over [now-window, now]. Our approximation uses prev_count * weight, which equals prev_count * (1 - elapsed/window). If requests were concentrated at the END of the previous window (worst case), we overcount slightly. If concentrated at the beginning, we undercount slightly. Cloudflare measured the error at 0.003% at their scale (billions of requests). Formally, the maximum error is bounded by the "burstiness" of the request distribution divided by window size. For real-world API traffic (which follows Poisson or power-law distributions, not adversarial step functions), < 1% error is consistently observed.

**Q4: How do you handle the case where a client makes requests from multiple IP addresses simultaneously? E.g., a distributed client with 100 servers?**
A: This is why we rate-limit primarily by `api_key` and `user_id`, not by IP. A single API key represents a single client regardless of how many source IPs they use. IP-based limiting is a secondary check for unauthenticated traffic (pre-auth abuse prevention). For authenticated clients, the `api_key` or `user_id` is the primary dimension. For genuinely distributed clients (enterprise with many servers), their enterprise tier limit (10,000 req/min) is set high enough to accommodate distributed usage — and they can request higher limits via the override mechanism.

**Q5: The sliding window counter uses two keys. What if the previous window's key has already expired (TTL elapsed)? Won't you get a null pointer?**
A: Yes — that's handled explicitly with `or '0'` in the Lua script: `tonumber(redis.call('GET', prev_key) or '0')`. If the key doesn't exist, GET returns a Redis nil, which Lua converts to `false`, and the `or '0'` coerces it to `'0'`, which `tonumber()` converts to `0`. A missing previous key means the previous window had zero traffic (it expired because the counter reached 0 and was reclaimed, or it was never set). This is correct behavior: `estimated_count = 0 * weight + current_count`. The TTL of `window_seconds * 2` ensures the key is available for at least one full window after it's set, which is sufficient for the previous window calculation.

---

### 6.2 Distributed Rate Limiting with Redis

**Problem it solves:** A single gateway node can maintain in-process counters with zero latency, but in a fleet of N gateway nodes, a client could route requests to all N nodes and get N× the allowed rate. We need a shared, consistent counter that all nodes read and write atomically.

#### Approaches Comparison

| Approach | Consistency | Latency Added | Complexity | Failure Mode |
|---|---|---|---|---|
| **Central Redis Cluster** | Eventually consistent (< 1ms replication lag) | 1-3 ms network RTT | Medium | Fail-open on Redis unavailability |
| Gossip/CRDT counters | Eventually consistent (seconds) | < 1 ms (local read) | Very high | Over-admission proportional to sync lag |
| Sticky routing (client → node) | Fully consistent (local) | 0 ms | Low | Node failure invalidates client affinity |
| Two-phase distributed lock | Strongly consistent | 5-20 ms (acquire + release) | High | Lock contention causes queue |
| Approximate counting (HyperLogLog) | Approximate | < 1 ms | Low | Structural over-admission (~2% error) |

**Selected: Central Redis Cluster**

**Reasoning:** The network RTT to Redis (same datacenter, < 1 ms typical, < 3 ms p99) fits within our 5 ms total overhead budget. Redis Cluster with hash slots ensures a given client's key always routes to the same shard (based on CRC16 of the key). Lua atomicity handles concurrent requests without locks. This is the architecture used by Kong API Gateway, AWS API Gateway, Nginx rate limiting module, and Stripe's rate limiting infrastructure.

**Optimizations for throughput:**

1. **Redis Pipelining for batch operations:** When a single request touches multiple rate limit dimensions (e.g., per-user + per-endpoint), pipeline all Lua script calls in a single network round-trip.

2. **Local shadow counter (async correction):** Each gateway node maintains an in-process counter that's incremented on every request. Every 100 ms, the node syncs its local delta to Redis and corrects its local view. Between syncs, the local counter is used for fast allow/reject decisions. This reduces Redis calls by up to 90% but allows up to 100 ms of over-admission. For most use cases, this trade-off is acceptable.

3. **Connection pooling:** Each gateway node maintains a pool of 50 persistent TCP connections to each Redis shard. Connection establishment overhead is amortized.

4. **Hash tags for multi-key atomicity:** If a Lua script must access multiple keys (e.g., current + previous window), all keys must be on the same Redis shard. We force this by using hash tags: `rl:{u123}:60:28928160` and `rl:{u123}:60:28928159` — the hash tag `{u123}` ensures both keys hash to the same slot.

**Implementation detail — Hash tags:**

```
Without hash tags:
  "rl:uid:u123:60:28928160" hashes to slot 7823 (shard 2)
  "rl:uid:u123:60:28928159" hashes to slot 4491 (shard 0)
  ⇒ MULTI/EXEC across shards is IMPOSSIBLE in Redis Cluster

With hash tags:
  "rl:{u123}:60:28928160" hashes slot of "{u123}" = slot 5474 (shard 1)
  "rl:{u123}:60:28928159" hashes slot of "{u123}" = slot 5474 (shard 1)
  ⇒ Both keys on the same shard; Lua script works correctly
```

#### Interviewer Q&A

**Q1: How do you size the Redis Cluster? How many shards do you need?**
A: At 16M ops/sec and ~50 bytes per operation: 16M × 50 = 800 MB/s throughput. A single Redis instance with a fast CPU (e.g., c5.2xlarge on AWS: 8 vCPUs, 16 GB RAM) handles ~500K-800K ops/sec for Lua scripts (heavier than plain GET/SET). We need 16M / 600K ≈ 27 shards. Round up to 32 shards for headroom. With 3 nodes per shard (1 primary + 2 replicas) = 96 Redis nodes. In practice, use Redis Cluster with 32 primaries and 64 replicas. Memory: 640 MB total data / 32 shards = 20 MB per shard — trivial. The sizing is CPU-bound, not memory-bound.

**Q2: What about Redis Cluster resharding? How do you rebalance without dropping counters?**
A: Redis Cluster's CLUSTER REBALANCE command migrates hash slots online — it moves keys in the background while both source and target shards handle requests for migrating slots using the MIGRATING/IMPORTING state machine. During migration, the cluster routes requests correctly: if a key has moved, the source shard returns a MOVED redirect; the client follows to the new shard. Our Redis client library (go-redis, jedis) handles MOVED redirects automatically. Counter accuracy during migration degrades slightly (< 1 second of dual-write inconsistency during slot migration), which is acceptable.

**Q3: Could you use Redis Streams or Pub/Sub to propagate rate limit state instead of direct key access?**
A: Not for the hot path. Pub/Sub introduces asynchrony — a subscriber may not receive the event before the next request arrives, causing over-admission. Streams have similar ordering/delivery timing issues under load. Direct key access with Lua scripts is synchronous and consistent. Pub/Sub is appropriate for the rules propagation path (rules updates are rare and can tolerate 100 ms propagation delay), but not for per-request counter checks.

**Q4: What if two requests arrive simultaneously at the exact same nanosecond on different gateway nodes for the same client?**
A: Redis is single-threaded for command execution. Even if two nodes submit their Lua scripts at the exact same wall-clock time, Redis serializes them. One executes first, increments the counter to N; the second executes next, reads counter N, increments to N+1. There is no race condition. The only "concurrent" execution that could occur is at the network level (two TCP packets arriving at the NIC simultaneously), but the kernel's TCP stack and Redis's event loop serialize them. This is one of the fundamental guarantees of Redis's architecture that makes it suitable for distributed rate limiting.

**Q5: How do you handle Redis key expiration under extreme write pressure? Could expired keys pile up and cause memory pressure?**
A: Redis's key expiration is handled in two ways: (1) lazy expiration — a key's TTL is checked on access; if expired, it's deleted and nil is returned; (2) active expiration — Redis samples 20 random keys with TTLs every 100 ms and deletes expired ones, repeating if > 25% of sampled keys were expired. Under our workload, keys have 120-second TTLs and are actively written every second. The active expiration scan handles our churn rate (4M keys with 2-minute TTLs = ~33K expirations/sec). Memory pressure is bounded by the 640 MB total counter size, which is well within shard capacity. We also set `maxmemory-policy allkeys-lru` as a safety net: if memory is unexpectedly full, LRU eviction removes the least-recently-used keys (which are likely from inactive clients anyway).

---

### 6.3 Rate Limit Bypass Prevention

**Problem it solves:** Sophisticated clients may attempt to circumvent rate limits by: rotating API keys, using multiple accounts, spoofing IP addresses, exploiting clock skew, or finding edge cases in the algorithm implementation.

#### Bypass Attack Vectors and Mitigations

| Attack Vector | Description | Mitigation |
|---|---|---|
| **API key rotation** | Client has multiple keys; rotates them to multiply effective limit | Enforce per-user-ID limits in addition to per-key limits. Since user_id links all keys to one account, aggregate limits apply. |
| **IP spoofing** | Forge source IP to bypass IP-based limits | Use `X-Forwarded-For` header from trusted CDN/LB only. Verify the last hop is a known CDN IP via an allowlist. |
| **Account farming** | Create many free-tier accounts | Require phone/email verification; rate-limit account creation by IP; apply per-IP limits regardless of tier. |
| **Clock skew exploitation** | Send requests near window boundary, knowing the server's clock | The sliding window counter algorithm is less susceptible than fixed window. Additionally, use NTP-synchronized clocks on all Redis nodes and gateway nodes. |
| **Redis race condition** | Exploit the window boundary to insert requests before the counter updates | Lua script atomicity eliminates this; there is no window of vulnerability. |
| **Retry storm amplification** | Trigger 429s, then flood retries — if retries don't count, bypass the limit | All requests count against the limit, regardless of whether they were retries. Retries are tracked client-side with exponential backoff. |
| **Header injection** | Client sets `X-RateLimit-Remaining: 999` hoping gateway reads it | Rate limit headers are set by the gateway on the RESPONSE, not read from the REQUEST. Client-provided headers are stripped or ignored. |
| **Distributed request origin** | Route requests through a botnet of IPs | Requires IP-based device fingerprinting, behavioral analysis, and anomaly detection — beyond rate limiting, into fraud detection scope. |

**Implementation — Trusted IP extraction:**

```go
// ExtractClientIP extracts the real client IP from a trusted proxy chain.
// ONLY trust X-Forwarded-For from known CDN CIDR ranges.
func ExtractClientIP(r *http.Request, trustedCIDRs []*net.IPNet) string {
    remoteIP := net.ParseIP(strings.Split(r.RemoteAddr, ":")[0])

    // Only trust X-Forwarded-For if the direct connection is from a trusted proxy
    for _, cidr := range trustedCIDRs {
        if cidr.Contains(remoteIP) {
            // Take the leftmost IP in X-Forwarded-For (the original client)
            xff := r.Header.Get("X-Forwarded-For")
            if xff != "" {
                // The leftmost IP is the client; rightmost IPs are proxies
                parts := strings.Split(xff, ",")
                clientIP := strings.TrimSpace(parts[0])
                if ip := net.ParseIP(clientIP); ip != nil {
                    return ip.String()
                }
            }
        }
    }
    // Not from a trusted proxy: use the direct connection IP
    return remoteIP.String()
}
```

---

## 7. Scaling

### Horizontal Scaling

**Gateway nodes:** Stateless — add nodes behind the load balancer. The rate limiter state is entirely in Redis, not in the gateway nodes. A new node is immediately fully functional. Use auto-scaling groups (AWS ASG or Kubernetes HPA) triggered at CPU > 70%.

**Redis Cluster:** Scale by adding shards. Redis Cluster supports online resharding without downtime. Adding 16 more shard primaries to an existing 32-shard cluster can be done in minutes with `redis-cli --cluster rebalance`.

**Rules store (MySQL):** Low write volume (admin operations only). Add read replicas for the rules cache population service. Use a connection pooler (ProxySQL or PgBouncer equivalent) for MySQL to handle connection spikes.

### DB Sharding

Redis is already sharded via hash slots. The sharding key is the `{client_id}` hash tag, which distributes clients uniformly across shards.

For the MySQL rules store, sharding is unnecessary — the total dataset is < 20 MB and read traffic is low.

### Replication

Redis: 1 primary + 2 replicas per shard. Replicas are asynchronous (replication lag < 1 ms in same AZ). Replicas are used for read traffic in the observability/admin path only — never for write path (all counter increments go to primaries to avoid split-brain counting).

MySQL: 1 primary + 2 read replicas. Admin writes go to primary; rules cache population reads from replicas.

### Caching

Three-tier cache hierarchy:

1. **In-process LRU (gateway node):** Holds all active rules (< 20 MB — the entire rules set fits in memory). TTL: 30 seconds. Cache hit rate: ~100% (rules change rarely). This eliminates Redis round-trips for rule lookups entirely.

2. **Redis (rules cache):** Acts as a shared fan-out cache between MySQL and gateway nodes. TTL: 60 seconds. Updated by admin API on every rule change. Population: async job every 30 seconds.

3. **MySQL (source of truth):** Only accessed by the rules population job, not by the hot path.

### Scaling Q&A

**Q1: Your peak estimate is 8M RPS. What if traffic spikes 10× to 80M RPS (viral event)?**
A: At 80M RPS, Redis ops go to 160M/sec. That requires ~270 Redis shards (at 600K Lua ops/sec per shard). Pre-warming to 270 shards is cost-prohibitive. Instead, the scaling strategy is: (1) Enable the local shadow counter mode — gateway nodes batch updates to Redis every 100 ms, reducing Redis ops by 100× to 1.6M/sec (easily handled by 3 shards). This introduces up to 100 ms of over-admission. (2) Enable adaptive sampling: only synchronize to Redis on 10% of requests; multiply the local delta by 10 on sync. This further reduces Redis load. (3) Activate a dedicated "emergency rate limit" mode with coarser-grained limits (per-hour instead of per-minute) using simpler counters. The system degrades gracefully rather than hard-failing.

**Q2: How do you handle multi-region deployments? Should each region have its own Redis?**
A: Yes — each region should have an independent Redis cluster. Cross-region Redis synchronization (e.g., via Redis Active-Active/CRDT) introduces 50-150 ms cross-region latency into the rate limit check, which blows our 5 ms p99 budget. The trade-off is that a client can get N× their limit by routing requests to N regions. We mitigate this by: (1) setting per-region limits to `total_limit / num_regions` (e.g., a 1000 req/min limit becomes 500 req/min per region if deploying in 2 regions); (2) using anycast routing so a client's requests consistently route to their nearest region. Only truly adversarial clients sending requests to multiple regions simultaneously get around this — and they're detectable via the audit log.

**Q3: How would you handle a "thundering herd" of requests when a rate limit window resets?**
A: The sliding window counter algorithm naturally mitigates thundering herds because window resets are soft. With a fixed window counter, when the window flips, all clients who were at their limit suddenly have 0 counts — triggering a simultaneous rush. With sliding window counter, the effective limit during the transition is `prev_count * weight + current_count`, so clients who saturated the previous window face a gradually increasing budget rather than an instant reset. This is an intrinsic algorithmic advantage. Additionally, the `X-RateLimit-Reset` header is the exact window boundary timestamp — clients who implement backoff with jitter (as recommended in our documentation) spread their retries across the reset window.

**Q4: Can you cache rate limit decisions (not just rules) to reduce Redis load?**
A: Not safely for allow decisions. If you cache "client X is allowed" for even 10 ms, during that 10 ms all concurrent requests from client X bypass Redis and all get "allowed" — potentially admitting `RPS_per_client × 10ms` extra requests. For example, a client at 10,000 RPS with 10 ms decision caching could admit 100 extra requests per cache period. You CAN cache reject decisions: if a client receives a 429, you can cache that locally for `retry_after_seconds` with certainty — there's no false positive risk. This is a common optimization in middleware implementations.

**Q5: How do you ensure the Redis Cluster doesn't become a single point of failure despite having replicas?**
A: Redis Cluster handles primary failure via automatic failover: Redis Sentinel (or the built-in Cluster failover mechanism) promotes a replica to primary within 30 seconds (typical MTTR < 10 seconds). During the failover window, requests to the affected shard fail. Our fail-open policy kicks in for those shard-specific keys. Since a 32-shard cluster has 1/32 of all keys on each shard, only ~3% of clients are affected during any single shard failover. Multi-region deployment provides an additional layer: if an entire Redis cluster in us-east-1 fails, traffic shifts to the us-west-2 cluster via DNS failover (60-second TTL).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario | Impact | Detection | Mitigation |
|---|---|---|---|
| Single Redis shard failure | ~3% of clients get fail-open (no rate limiting) for 10-30s | Redis health checks; shard error rate metric | Automatic replica promotion (Redis Cluster failover) |
| Full Redis cluster failure | All clients get fail-open; no rate limiting | Redis connection error spike; PagerDuty alert | Local in-process fallback counter (per-node soft limits); auto-scaling of Redis cluster |
| Gateway node crash | In-flight requests lost; rate limit counts slightly under-reported | Load balancer health check (10s interval) | Stateless design: load balancer removes unhealthy node; other nodes continue unaffected |
| Rules cache poisoning | Wrong limits applied (over or under) | Monitor for anomalous 429 rate or traffic spike | Rules cache has 60s TTL; admin can trigger manual cache invalidation; MySQL is authoritative ground truth |
| MySQL rules store failure | Cannot create/update rules; existing rules served from Redis cache | MySQL connection errors; ops alert | Cached rules served for up to 60 minutes before expiry; failover MySQL replica |
| Clock skew between nodes | Sliding window calculation slightly off; possible over/under admission | Monitor divergence between node times via NTP metrics | Use NTP with tight sync (< 1 ms skew); window boundary is 60 seconds — 1 ms skew is < 0.002% error |
| Kafka audit pipeline failure | 429 events not logged; audit gap | Kafka consumer lag metric; broker health | 429s still enforced (Kafka is async/non-blocking); retroactive audit from access logs is possible |
| Hash slot migration failure | Requests to migrating slots receive CLUSTERDOWN error | Redis CLUSTERDOWN error count metric | Redis client auto-retries MOVED/ASK errors; migration retried from last checkpoint |
| Lua script timeout | Script times out if Redis is overloaded; returns error | Redis slowlog; script timeout metric | Fail-open: return ALLOW on Lua timeout; alert fires; investigate Redis CPU |

### Failover Design

**Redis Cluster Failover:**
- Each shard: 1 primary + 2 replicas in different availability zones.
- Primary failure detected within 5 seconds (cluster-node-timeout = 5000 ms).
- Replica promoted within 10 seconds (typical).
- During failover window: fail-open policy applies for affected shard's keys.

**Gateway Node Failover:**
- Health check: load balancer checks `GET /healthz` every 5 seconds.
- Unhealthy node removed from rotation within 10 seconds.
- Since gateway nodes are stateless, failover is instantaneous for new requests.

### Retries and Idempotency

Rate limit operations are idempotent by nature — an INCR operation increments the counter, and there is no way to "un-increment" a failed request. If a gateway node fails after incrementing the Redis counter but before returning the response to the client, the counter is slightly over-counted. This is acceptable: the client will retry (from their SDK), consuming one more counter slot. The counter being slightly over-accurate is safe (conservative admission), not harmful.

For Redis operation retries:
- Transient Redis errors (connection reset, timeout): retry once with 1 ms backoff. If second attempt fails: fail-open.
- MOVED errors: auto-redirected by Redis client library to the correct shard (transparent).
- CLUSTERDOWN: fail-open immediately (no retry — the cluster is reconfiguring).

### Circuit Breaker

A circuit breaker wraps all Redis calls from the gateway middleware:

```
States:
  CLOSED (normal): All Redis calls executed. If error rate > 5% in 10s window → OPEN.
  OPEN (tripped): All Redis calls skipped; fail-open applied. After 30s → HALF-OPEN.
  HALF-OPEN: 1% of Redis calls executed as probes. If successful → CLOSED. If failing → OPEN.

Metrics monitored by circuit breaker:
  - Redis call error rate (connection refused, timeout)
  - Redis call p99 latency (if > 10ms → potential circuit open)
  - Redis call success rate
```

This ensures that a degraded Redis cluster (high latency, intermittent errors) triggers a gradual fail-open rather than causing cascading timeouts in the gateway threads.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Labels | Threshold / Alert |
|---|---|---|---|
| `ratelimit.requests.total` | Counter | `tier`, `dimension`, `decision={allowed,rejected_window,rejected_burst}` | — |
| `ratelimit.redis.latency_ms` | Histogram | `operation={check,get_rule}`, `shard_id` | Alert if p99 > 5 ms |
| `ratelimit.redis.errors.total` | Counter | `error_type={timeout,connection,clusterdown}` | Alert if > 10/min |
| `ratelimit.failopen.total` | Counter | `reason={redis_down,lua_timeout,circuit_open}` | Alert immediately on any fail-open |
| `ratelimit.circuit_breaker.state` | Gauge | `state={0=closed,1=open,2=halfopen}` | Alert if state = 1 (OPEN) |
| `ratelimit.rules_cache.hit_rate` | Gauge | `cache_level={inprocess,redis}` | Alert if in-process hit rate < 95% |
| `ratelimit.rejected.by_tier` | Counter | `tier`, `rule_name` | — |
| `ratelimit.rejected.by_client` | Counter | `client_id` (sampled top-N) | Alert if single client > 10K rejected/min |
| `ratelimit.counter.estimated_count` | Histogram | `tier` | — |
| `ratelimit.window.reset_latency_ms` | Histogram | — | Alert if p99 > 100 ms |
| `redis.cluster.used_memory_bytes` | Gauge | `shard_id` | Alert if > 80% of max memory |
| `redis.cluster.connected_slaves` | Gauge | `shard_id` | Alert if < 2 |

### Distributed Tracing

Every request through the rate limit middleware generates a trace span:

```
Trace: POST /v1/search (total: 12ms)
  ├── [Auth middleware] extract_identity: 0.5ms
  ├── [Rate limit middleware] check_rate_limit: 2.1ms
  │     ├── [rules_cache] get_rule: 0.05ms (in-process hit)
  │     ├── [redis] lua_script_eval: 1.8ms
  │     │     ├── shard_id: 7
  │     │     ├── key: rl:{u123}:60:28928160
  │     │     ├── result: ALLOW, count=143, remaining=857
  │     └── [headers] set_ratelimit_headers: 0.05ms
  └── [upstream proxy] forward_request: 9.4ms
```

Trace context propagated via W3C `traceparent` header. Stored in Jaeger (sampling rate: 1% of allowed requests, 100% of rejected requests for audit).

### Logging

**Structured log format (JSON):**

```json
{
  "ts": "2025-01-01T00:00:00.001Z",
  "level": "info",
  "msg": "rate_limit_check",
  "request_id": "req-uuid",
  "client_id": "u123",
  "tier": "pro",
  "rule": "pro_tier_per_minute",
  "decision": "allowed",
  "count": 143,
  "limit": 1000,
  "remaining": 857,
  "redis_latency_ms": 1.8,
  "gateway_node": "gw-us-east-1a-007"
}
```

**429 events** are always logged at `warn` level and also emitted to Kafka for the audit pipeline.

**Redis errors** are logged at `error` level with full context (error message, shard ID, key).

Log aggregation: Fluentd → Elasticsearch → Kibana dashboards. Retention: 7 days hot, 90 days warm, 1 year cold (S3).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A (Chosen) | Option B (Rejected) | Reason |
|---|---|---|---|
| Algorithm | Sliding window counter + burst check | Sliding window log (exact) | Log requires O(N) memory (50 KB/client); counter is O(1) with < 1% error |
| Counter store | Redis Cluster | Cassandra / DynamoDB | Redis Lua scripts provide atomicity; p99 < 3 ms; Cassandra too slow for hot path |
| Consistency model | Eventual (< 100 ms) | Strong (synchronous cross-node) | Strong consistency requires synchronous replication which adds 10-50 ms latency — violates SLA |
| Failure mode | Fail-open | Fail-closed | Fail-closed on Redis outage = total API outage; fail-open briefly allows over-admission but keeps API available |
| Rules propagation | Pull-based (30s poll) | Push-based (Pub/Sub) | Push requires all nodes subscribe reliably; pull is simpler and resilient to subscriber failures |
| Algorithm for burst | Fixed window per second | Token bucket | Fixed window per second is simpler (same algorithm used for minute window); token bucket is more flexible but complex |
| Audit log pipeline | Kafka async | Synchronous DB write | Synchronous write would add 5-10 ms to every 429 response; Kafka is non-blocking |
| Rate limit key namespace | `rl:{client_id}:...` | UUID-based keys | Human-readable keys simplify debugging; no collision risk with proper namespacing |
| Multi-region | Independent per-region Redis | Cross-region active-active Redis | Cross-region Redis adds 50-150 ms; independent clusters add acceptable over-admission at region level |
| Window size | 60-second primary + 1-second burst | Only 60-second window | 1-second burst window catches flood attacks that stay under 60s aggregate limit |

---

## 11. Follow-up Interview Questions

**Q1: How would you implement tiered rate limits where different API endpoints have different limits for the same user tier?**
A: Add an `endpoint_glob` column to the rules table (already in the schema). When resolving which rule applies, match both tier AND endpoint. Use a priority-ordered rule lookup: most-specific endpoint glob wins over wildcard. For example: rule 1 (`tier=pro, endpoint=/v1/search*`, limit=100/min, priority=10) beats rule 2 (`tier=pro, endpoint=*`, limit=1000/min, priority=100). In the Lua script, the resolved limit is passed as an argument — the rule resolution logic lives in application code, not in Redis.

**Q2: How do you handle rate limiting for WebSocket connections or streaming APIs where one "request" is a long-lived connection?**
A: Rate limiting strategies differ for streaming: (1) Connection rate limiting: apply rate limits to the handshake request (same as HTTP); limits how many new streams a client can open per minute. (2) Message rate limiting: apply limits to each message sent within a WebSocket connection. The gateway maintains a per-connection message counter alongside the connection state. (3) Bandwidth rate limiting: enforce maximum bytes/second per connection. For streaming, the token bucket algorithm (with refill rate = bytes/sec limit and bucket size = burst bytes) is more natural than window-based counters.

**Q3: A large enterprise customer complains that they're being rate limited even though they're within their stated monthly allocation. What's happening and how do you diagnose it?**
A: This is the "sustained vs. burst" confusion. Monthly allocation (e.g., 30M requests/month) doesn't mean they can send all 30M in one minute. Our per-minute limits enforce a throughput ceiling. Diagnosis steps: (1) Check the audit log in S3/Athena for `client_id = their_api_key` and filter for 429 events. Aggregate by minute — are they consistently hitting the per-minute limit? (2) Check the `GET /v1/ratelimit/status` endpoint for their current counter state. (3) Review their tier's rule in the rules table. Resolution: (a) If they have a legitimate need for higher instantaneous throughput, create an override with a higher limit for their API key. (b) If their traffic pattern is bursty, configure a larger burst_limit with a token bucket secondary check. (c) Educate them about implementing request queuing on their client side.

**Q4: How would you add cost-based rate limiting where different API endpoints consume different "credits" (e.g., a search costs 1 credit, an ML inference costs 10 credits)?**
A: Modify the rate limit model from "count of requests" to "sum of costs." Changes needed: (1) The rate limit rule defines a `credit_budget` instead of `request_count`. (2) Each endpoint definition includes a `credit_cost` (stored in the rules table or a new `endpoint_costs` table). (3) The Lua script changes from `INCR` to `INCRBY(cost)`: atomically add the credit cost and check against the budget. (4) The `X-RateLimit-Remaining` header reports remaining credits, not remaining requests. (5) The sliding window counter estimated count becomes estimated credit consumption. This is essentially how OpenAI's API rate limiting works (token-based limits). The implementation complexity is minimal — the core algorithm doesn't change.

**Q5: Your rate limiter adds 3ms p99 to every request. A customer is building a real-time trading system and needs < 1ms added latency. How would you accommodate this?**
A: Three approaches: (1) **Local-first with async sync**: Maintain in-process counters; sync to Redis asynchronously every 50 ms. Latency drops to ~0.1 ms (in-process lookup). Trade-off: up to 50 ms of over-admission possible. For a trading API, you'd set the per-client limit conservatively (e.g., 50% of max) and rely on the async correction. (2) **eBPF-based rate limiting at the kernel level**: Use XDP (eXpress Data Path) in the Linux kernel to perform rate limiting before the packet even reaches user space. eBPF programs can implement token bucket counters in kernel memory with sub-microsecond latency. Netflix, Facebook, and Cloudflare use this for ultra-low-latency rate limiting. (3) **Hardware offload**: Modern SmartNICs (e.g., NVIDIA BlueField) can perform rate limiting in NIC firmware. Exploring whether the customer's latency requirement is actually for the rate limit check or for the full round-trip to the API — the 3ms rate limit overhead may not be the bottleneck.

**Q6: How would you implement a "fair share" system where during overload, all clients get proportionally reduced limits rather than some getting full access and others getting nothing?**
A: This is admission control, not just rate limiting. Implement: (1) A global "system load" signal updated every second (e.g., CPU utilization, queue depth). (2) A load-shedding multiplier: at 90% load, multiply all limits by 0.8; at 100% load, by 0.5. (3) Publish this multiplier to all gateway nodes via Redis (the same rules cache mechanism). (4) Gateway nodes apply `effective_limit = base_limit * load_multiplier` in the Lua script. This ensures all clients proportionally get fewer requests during overload rather than first-come-first-served exhaustion.

**Q7: How do you prevent a client from learning the exact algorithm by probing the rate limiter with carefully timed requests?**
A: Full obfuscation is impossible — the `X-RateLimit-Remaining` header reveals the counter state. However: (1) Round `X-RateLimit-Remaining` to the nearest 10 (or use logarithmic bucketing) to reduce information leakage. (2) Add random jitter (± 5%) to the `Retry-After` value. (3) For the per-second burst check, don't expose a separate burst header — just return 429. (4) Vary the window reset time slightly (jitter) to prevent timing attacks on window boundaries. In practice, security-through-obscurity provides minimal protection — the best defense against API abuse is behavioral anomaly detection, not algorithm obfuscation.

**Q8: The sliding window counter can be off by up to X%. What's the worst-case scenario for your business, and is it acceptable?**
A: Worst case: a client sends exactly `limit` requests in the last 1 second of the previous window. `prev_count = limit`, `weight ≈ 1.0`, so `estimated_count ≈ limit`. The algorithm blocks them at the window boundary instead of allowing them. This is UNDER-admission (too conservative), not over-admission. The reverse case: client sends requests uniformly, then `weight = 0` at the end of the window and the full previous count isn't carried over — over-admission of up to `limit * (1 - weight)` extra requests. At worst, if `weight ≈ 0` (current window just started), the previous window count is ignored and a client gets double their limit. In practice, Cloudflare reports this worst case allows ~3× the limit during a 1-second window transition. For our business (API platform), this is acceptable: it means some Pro clients briefly get 1,000-3,000 requests in a narrow window instead of exactly 1,000. The risk is minor overload on upstream services, not security or billing fraud, since the limit resets correctly in the next window.

**Q9: How do you rate-limit server-sent events (SSE) or chunked transfer encoding where a single HTTP request can produce indefinite output?**
A: SSE/chunked responses are single HTTP requests that stream output. Rate limiting options: (1) **Connection-level limiting**: count SSE connection establishments (1 connection = 1 request); limit max concurrent SSE connections per client. (2) **Event-level limiting**: each SSE event (each data: line) is counted separately; the gateway inspects the response stream and increments a counter per event. This is more accurate but requires deep packet inspection. (3) **Data transfer rate limiting**: limit bytes/second per client (token bucket on outbound bytes). The gateway accumulates bytes sent and throttles the response stream if the rate exceeds the limit. In practice, option 1 + 3 is most common: limit concurrent connections and data transfer rate.

**Q10: What's your strategy for load testing the rate limiter itself?**
A: Load testing strategy: (1) **Unit tests**: verify Lua script correctness with known inputs/outputs. Test boundary conditions: exactly at limit, one over, after window reset. (2) **Integration tests**: spin up a local Redis instance; run 100 concurrent goroutines all hammering the same rate-limit key; verify exactly `limit` are allowed. (3) **Load tests (locust or k6)**: simulate 100K distinct client IDs at 8M total RPS using a distributed load testing cluster. Measure: (a) p99 latency of the Lua script, (b) number of ALLOW vs REJECT decisions (should match expected limits), (c) Redis memory usage, (d) Redis CPU utilization. (4) **Chaos tests**: kill a Redis shard mid-test; verify fail-open behavior and that the circuit breaker trips correctly. Kill a gateway node; verify load shifts seamlessly. (5) **Boundary tests**: send requests at exactly the window boundary (t = N × 60 + 0.001 seconds); verify the sliding window calculation correctly blends the previous and current counts.

**Q11: How would you handle a situation where your rate limiter causes a false positive (legitimate traffic rejected) for a major customer?**
A: Incident response playbook: (1) **Immediate mitigation (< 5 min)**: apply an emergency override via `POST /v1/ratelimit/overrides` to temporarily exempt or increase the limit for the affected `api_key`. This takes effect within 30 seconds. (2) **Diagnosis (< 30 min)**: query the audit log (S3 + Athena) for the client's rejected requests. Correlate with their traffic patterns. Identify the rule that triggered the false positive. Was it a legitimate spike? A misconfigured rule? Clock skew? (3) **Root cause analysis**: review rule configuration, Redis counter values, gateway node logs. (4) **Permanent fix**: adjust the rule or increase the client's tier limit. Test the new configuration. (5) **Post-mortem**: document timeline, impact (requests rejected), root cause, fix, and prevention measures.

**Q12: What changes would you make to support rate limiting in a gRPC system?**
A: gRPC changes the transport layer but not the rate limiting logic: (1) **Interceptor instead of HTTP middleware**: implement a gRPC server interceptor (unary and stream) that performs the rate limit check before the handler is called. In Go: `grpc.UnaryServerInterceptor` and `grpc.StreamServerInterceptor`. (2) **Client identity extraction**: extract API key from gRPC metadata (`ctx.Value("authorization")`), not HTTP headers. (3) **gRPC status codes**: return `codes.ResourceExhausted` (status code 8) instead of HTTP 429. Attach rate limit metadata to the status details (using `google.rpc.Status` with `QuotaFailure` details proto). (4) **Streaming-specific**: for server-streaming RPCs, rate limit the stream establishment (1 call = 1 request) and optionally each message. For bidirectional streaming, rate limit each client message. (5) **gRPC-specific headers**: gRPC doesn't have an equivalent to `X-RateLimit-*` headers in the traditional sense; use gRPC trailing metadata to communicate rate limit info.

**Q13: How do you handle clock drift between the Redis nodes in your cluster?**
A: Clock drift in Redis Cluster affects the `now` timestamp we pass to the Lua script: (1) **Root cause**: Redis Lua scripts receive the `ARGV` timestamp from the calling client (gateway node), not from the Redis server's clock. So the timestamp source is the gateway node's clock, not Redis's clock. Gateway nodes are synced via NTP. (2) **Mitigation**: ensure all gateway nodes have NTP sync with < 1 ms drift. Use a time server close to the gateway nodes (AWS Time Sync Service in the same AZ). (3) **Impact if drift occurs**: up to `drift_ms / window_size` fractional error in the weight calculation. For 10 ms drift on a 60-second window: 10ms / 60,000ms = 0.017% error — negligible. (4) **Redis server clock**: Redis does have an internal clock accessible via `redis.call('TIME')` in Lua scripts. If you distrust gateway clocks, use `redis.call('TIME')` to get the Redis server's time directly. This ensures all nodes use the same time source.

**Q14: Your rate limiter currently stores counters in Redis with TTL = window_seconds * 2. Why not TTL = window_seconds exactly?**
A: There are two reasons for 2× TTL: (1) **Previous window access**: the sliding window counter algorithm reads BOTH the current and previous window keys. The previous window key has been running for up to `window_seconds` already. If its TTL is exactly `window_seconds`, it could expire while we're still reading it (race condition). By setting TTL = `window_seconds * 2`, the previous window key is guaranteed to exist until `2 * window_seconds` after it was created — which is `window_seconds` after the window it represents ended. Plenty of time for any request that falls in the gap. (2) **Clock skew buffer**: if a gateway node's clock is slightly behind Redis's effective key creation time, a TTL of exactly `window_seconds` could cause the key to expire 1-2 seconds early from that node's perspective. The 2× buffer absorbs up to `window_seconds` of clock skew, which is a very conservative bound.

**Q15: How would you design the rate limiter differently if you were using a serverless architecture (AWS Lambda)?**
A: Serverless changes two fundamental assumptions: (1) **No persistent in-process state**: Lambda functions are stateless; in-process LRU cache for rules must be replaced. Use DynamoDB DAX (microsecond reads) or Lambda's `/tmp` ephemeral storage with a short warm-cache TTL. (2) **Cold starts**: a Lambda cold start takes 100-500 ms — far exceeding the 5 ms rate limit budget. Mitigation: (a) Provision Concurrency (keep Lambda functions warm), (b) use Lambda@Edge for rate limiting at CloudFront edge locations (reduces cold start impact by running in a warm JS runtime), (c) or use a dedicated sidecar approach (rate limiter runs as a separate long-lived process, not Lambda). (3) **Connections**: Lambda functions can't maintain persistent Redis connections efficiently. Use connection pooling via a proxy like ElastiCache Redis with `cluster_mode=enabled` and shorter connection TTLs. (4) **Recommendation**: for serverless architectures, consider DynamoDB atomic conditional writes (`UpdateItem` with `ConditionExpression`) instead of Redis — DynamoDB is fully managed and connection-pooling-free. The additional ~5-10 ms latency is the cost of going serverless.

---

## 12. References & Further Reading

1. **Cloudflare Blog — "How we built rate limiting capable of scaling to millions of domains"** (2021): https://blog.cloudflare.com/counting-things-a-lot-of-different-things/ — Details the sliding window counter approximation, error analysis, and Redis implementation at Cloudflare's scale.

2. **Stripe Engineering Blog — "Scaling your API with rate limiters"** (2016): https://stripe.com/blog/rate-limiters — Covers token bucket, leaky bucket, and fixed window counters in the context of Stripe's API. Discusses multiple limiter types running in parallel.

3. **Redis Official Documentation — Rate Limiting with Redis**: https://redis.io/docs/manual/patterns/rate-limiting/ — Canonical Redis patterns for rate limiting including INCR with TTL and Lua script atomicity.

4. **Alex Xu, "System Design Interview Volume 1", Chapter 4: Design a Rate Limiter** (2020) — ByteByteGo: Covers the five algorithms, distributed rate limiting architecture, and race condition analysis.

5. **Kong API Gateway Rate Limiting Plugin Documentation**: https://docs.konghq.com/hub/kong-inc/rate-limiting/ — Real-world implementation notes including Redis Cluster, PostgreSQL, and local policy store options.

6. **AWS API Gateway Throttling Documentation**: https://docs.aws.amazon.com/apigateway/latest/developerguide/api-gateway-request-throttling.html — Describes token bucket algorithm used by AWS API Gateway, burst limits, and steady-state limits.

7. **IETF RFC 6585 — "Additional HTTP Status Codes"** (2012): https://datatracker.ietf.org/doc/html/rfc6585 — Formally defines HTTP 429 Too Many Requests status code and the `Retry-After` header usage.

8. **IETF Draft — "RateLimit Header Fields for HTTP"**: https://datatracker.ietf.org/doc/html/draft-ietf-httpapi-ratelimit-headers — Draft standard for `RateLimit-Limit`, `RateLimit-Remaining`, `RateLimit-Reset` header naming and semantics. Used to inform our `X-RateLimit-*` implementation.

9. **Martin Kleppmann, "Designing Data-Intensive Applications", Chapter 8: The Trouble with Distributed Systems** (2017, O'Reilly) — Critical background on clock drift, network partitions, and distributed consistency models relevant to understanding why strong consistency is difficult in distributed rate limiters.

10. **Redis Labs — "Redis Cluster Specification"**: https://redis.io/docs/reference/cluster-spec/ — Hash slot assignment, MOVED/ASK redirects, cluster failover protocol — the foundation for understanding Redis Cluster sharding behavior.
