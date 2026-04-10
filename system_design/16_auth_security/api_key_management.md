# System Design: API Key Management Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Key Generation** — generate API keys with cryptographically sufficient entropy (minimum 256 bits); expose keys to the owner once (at creation time); never store or retrieve the raw key thereafter.
2. **Key Hashing for Storage** — store only a one-way hash of the raw key; incoming requests present the raw key which is hashed for lookup; stored hash does not enable reconstruction of the raw key.
3. **Scoped Permissions** — each API key is granted a specific set of scopes/permissions (e.g., `read:data`, `write:records`); keys enforce least-privilege.
4. **Key Rotation** — support generating a replacement key for an existing key with a configurable overlap window (e.g., both old and new key valid for 24 hours to allow safe deployment); after overlap, old key is automatically revoked.
5. **Key Revocation** — immediate revocation of any key by the owner or an admin; revoked key fails validation within milliseconds.
6. **Usage Tracking** — record per-key usage: request count, last used timestamp, bytes transferred, error rate; aggregated daily and monthly.
7. **Key Leakage Detection** — scan public repositories (GitHub, npm, PyPI), paste sites, and dark web dumps for exposed API keys; auto-suspend leaked keys and notify owners.
8. **Expiry** — optional expiry date on keys (e.g., short-lived keys for CI/CD pipelines); expired keys fail validation.
9. **Environment Segmentation** — keys are scoped to environments (production, staging, development); a production key cannot be used in staging endpoints.
10. **Key Metadata** — descriptive labels (e.g., "Mobile App v2 - iOS Production"), owner user/team, creation/expiry dates, last rotation date.
11. **Audit Log** — every key creation, rotation, revocation, suspension, and validation (success/failure) is logged with timestamp, key ID (never raw key), IP, and user agent.
12. **Admin Controls** — admins can revoke any key, view usage aggregates (but never the raw key), export audit logs, and configure global key policies (max keys per user, forced rotation interval).

### Non-Functional Requirements

1. **Validation Latency** — p99 key validation < 5 ms (Redis hot path); this is on the hot path of every API request.
2. **Availability** — 99.99% uptime; key validation is a dependency for all API traffic.
3. **Throughput** — 500,000 key validation requests/second at peak.
4. **Security** — keys are irreversible once issued (no recovery by platform); brute-force infeasible due to key entropy; storage breach does not expose raw keys.
5. **Scalability** — validation layer is stateless; key metadata store sharded by key prefix.
6. **Consistency** — key revocation must be globally effective within 500 ms.

### Out of Scope

- OAuth2 token issuance (see `oauth2_provider.md`).
- Per-endpoint rate limiting beyond key-level quotas (handled by API gateway).
- API key billing and monetization (assumed external billing service).
- Key analytics dashboards (consume the usage events produced here).

---

## 2. Users & Scale

### User Types

| Type | Description | Primary Operations |
|---|---|---|
| Developer / API Consumer | Creates keys for their application | Create, rotate, revoke, view usage |
| Enterprise Customer Admin | Manages keys for their organization | Create for team members, revoke, audit |
| Machine (CI/CD) | Uses short-lived keys for automated pipelines | Validate (heavy), rotate via API |
| API Gateway / Middleware | Validates keys on every inbound request | Validate (dominant workload) |
| Admin (Platform Ops) | Platform-wide key management, compliance | Revoke, audit, configure policies |
| Leakage Detection System | Automated scanner for exposed keys | Read key prefixes, suspend |

### Traffic Estimates

**Assumptions:**
- 10,000 active API consumer accounts.
- Average 10 active keys per account = 100,000 active API keys.
- API traffic: 50 million API requests/day requiring key validation.
- Each API request = 1 key validation call (gateway validates before proxying to upstream).
- Key management operations (create/rotate/revoke): 10,000/day (low; administrative operations).
- Usage tracking writes: 50M events/day aggregated into per-key counters.

| Operation | Daily Volume | Peak Multiplier | Peak RPS |
|---|---|---|---|
| Key Validation (hot path) | 50,000,000 | 10× | 50M × 10 / 86,400 = **5,787 RPS** |
| Key Creation | 5,000 | 5× | < 1 RPS |
| Key Rotation | 2,000 | 2× | < 1 RPS |
| Key Revocation | 1,000 | 5× | < 1 RPS |
| Usage Counter Increment | 50,000,000 | 10× | **5,787 RPS** (Redis INCR, same as validation) |
| Usage Query (dashboard) | 500,000 | 5× | **29 RPS** |
| Audit Log Write | 51,000,000 | 10× | **5,903 RPS** (async via Kafka) |

**Design note:** 5,787 RPS for key validation is the design constraint. At p99 < 5 ms, Redis handles this easily (single Redis node handles 100K+ simple operations/second). The challenge is low-latency revocation propagation.

### Latency Requirements

| Operation | Target p50 | Target p99 | Notes |
|---|---|---|---|
| Key validation | 0.5 ms | 5 ms | Redis GET; on critical path of every API call |
| Key creation | 50 ms | 200 ms | Hash computation + DB write |
| Key revocation (propagation) | 50 ms | 500 ms | Delete from Redis + all regional caches |
| Usage counter increment | 0.1 ms | 1 ms | Redis INCR, non-blocking |
| Usage query (aggregated) | 5 ms | 50 ms | Pre-aggregated counters |

### Storage Estimates

| Data | Record Size | Volume | Storage |
|---|---|---|---|
| Key metadata record | 1 KB | 100,000 active keys | 100 MB (trivial) |
| Key hash for validation | 128 bytes | 100,000 | 12.8 MB (fits in single Redis node) |
| Usage counters (current month) | 64 bytes × 3 counters/key | 100,000 keys | 19.2 MB (trivial in Redis) |
| Usage aggregates (daily, 2 years) | 128 bytes/key/day | 100K keys × 730 days = 73M rows | 73M × 128B = **9.3 GB** |
| Audit log (hot, 90 days) | 512 bytes/event | 51M/day × 90 days = 4.6B | 4.6B × 512B = **2.3 TB** |
| Audit log (cold, 7 years) | 256 bytes compressed | ~130B events | **~33 TB** |

### Bandwidth Estimates

| Flow | Payload | Daily Requests | Daily Bandwidth |
|---|---|---|---|
| Key validation request (inbound to gateway) | 2 KB (request headers) | 50M | 100 GB |
| Validation response (pass/fail + metadata) | 256 bytes | 50M | 12.8 GB |
| Key creation response (with raw key — one time) | 1 KB | 5,000 | 5 MB |
| Usage query response | 2 KB | 500,000 | 1 GB |
| **Total/day** | | | **~114 GB (~130 Mbps avg; ~1.3 Gbps peak)** |

---

## 3. High-Level Architecture

```
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │                    API Consumers (Developers / Applications)                 │
  └──────────────────────┬──────────────────────────────────────────────────────┘
                         │ HTTPS
              ┌──────────┴──────────────────┐
              │ API Key Management Portal   │
              │ (Developer Dashboard UI)    │ ◄── Create / Rotate / Revoke / View Usage
              └──────────┬──────────────────┘
                         │ Internal API
  ┌──────────────────────▼──────────────────────────────────────────────────────┐
  │                   API Key Management Service                                 │
  │                   (Stateless pods, N replicas)                               │
  │                                                                              │
  │   ┌─────────────────┐   ┌──────────────────┐   ┌────────────────────────┐  │
  │   │ Key Lifecycle   │   │ Validation       │   │ Usage Tracking         │  │
  │   │ Handler         │   │ Handler          │   │ Handler                │  │
  │   │ - Create        │   │ (Hot path)       │   │ - INCR counters        │  │
  │   │ - Rotate        │   │ - Hash lookup    │   │ - Query aggregates     │  │
  │   │ - Revoke        │   │ - Scope check    │   │ - Export reports       │  │
  │   │ - Expire        │   │ - Expiry check   │   └────────────────────────┘  │
  │   └─────────────────┘   └──────────────────┘                               │
  │                                                                              │
  │   ┌─────────────────────────────────────────────────────────────────────┐   │
  │   │ Leakage Detection Worker (background)                               │   │
  │   │ - Poll GitHub/npm/paste scanning service                            │   │
  │   │ - Receive webhook from scan partner                                 │   │
  │   │ - Auto-suspend + notify on match                                    │   │
  │   └─────────────────────────────────────────────────────────────────────┘   │
  └───────────────────────────┬──────────────────────────────────────────────────┘
                              │
    ┌─────────────────────────┼──────────────────────────────────────┐
    │                         │                                       │
  ┌─▼──────────────────┐  ┌──▼─────────────────────────┐  ┌────────▼──────────┐
  │ Key Metadata Store │  │ Key Validation Store        │  │ Usage Store        │
  │ (PostgreSQL)       │  │ (Redis Cluster)             │  │ (Redis INCR +      │
  │                    │  │                             │  │  PostgreSQL daily  │
  │ - key_id           │  │ key: "apikey:<hash>"        │  │  aggregates)       │
  │ - key_hash         │  │ value: {key_id, scope,      │  │                    │
  │ - owner, scopes    │  │         status, expiry,     │  └────────────────────┘
  │ - status, expiry   │  │         environment}        │
  │ - metadata labels  │  │                             │  ┌────────────────────┐
  └────────────────────┘  └─────────────────────────────┘  │ Audit Log          │
                                                            │ (Kafka → S3 /      │
                                                            │  ClickHouse)       │
  ┌──────────────────────────────────────────────────────┐  └────────────────────┘
  │                   API Gateway Layer                   │
  │   (Separate deployment; API Key Management Service    │
  │    provides validation library / sidecar)             │
  │                                                       │
  │   On every inbound API request:                       │
  │   1. Extract key from X-API-Key header                │
  │   2. Hash the key: SHA-256(prefix + raw_key)          │
  │   3. Redis GET "apikey:<hash>" → metadata             │
  │   4. Check: active? not expired? correct environment? │
  │   5. Check scope against requested endpoint           │
  │   6. If valid: proxy request + INCR usage counter     │
  │   7. If invalid: 401 Unauthorized                     │
  └──────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────┐
  │         Leakage Detection Integration                 │
  │                                                       │
  │   External scanner ──► Webhook POST /internal/       │
  │   (GitHub Code Search,   suspect_key_found           │
  │    GitGuardian, Trufflehog)  {pattern_match,         │
  │                              source_url}              │
  │                                  │                    │
  │   Key Management Service ◄───────┘                   │
  │   - Extract key prefix from match                     │
  │   - Look up key by prefix in DB                       │
  │   - Suspend key + notify owner                        │
  └──────────────────────────────────────────────────────┘
```

**Component Roles:**

- **API Key Management Service**: All lifecycle operations (create, rotate, revoke, suspend). Stateless pods. Writes to PostgreSQL (durable) and Redis (validation cache).
- **Key Metadata Store (PostgreSQL)**: Canonical durable store for all key metadata. The `key_hash` column enables lookup by incoming key. Never stores raw keys.
- **Key Validation Store (Redis Cluster)**: Write-through cache populated at key creation. Serves the 5,787 RPS hot path at sub-millisecond latency. Entries are invalidated immediately on revocation.
- **Usage Store (Redis + PostgreSQL)**: Redis INCR for real-time per-key counters. A background aggregation job reads Redis counters every 5 minutes, writes to PostgreSQL daily_usage table, and resets the Redis counter (or uses sliding window).
- **API Gateway**: Validates keys on every inbound request using the Redis Validation Store. Does not call the Key Management Service on the hot path — reads Redis directly. Produces usage increment events.
- **Audit Log (Kafka → ClickHouse/S3)**: Async, non-blocking. Key Management Service emits events to Kafka; consumers persist to ClickHouse (30-day hot, query-optimized) and S3 Parquet (7-year cold).
- **Leakage Detection Worker**: Background process; integrates with external scanning services via webhooks. Auto-suspends keys and triggers owner notifications.

**Primary Use-Case Data Flow (Key Validation):**

```
1.  Client sends API request:
    GET /api/v1/data
    X-API-Key: sk_live_<EXAMPLE_KEY>...

2.  API Gateway receives request.

3.  Extract key from header. Identify key format version and environment prefix 
    (e.g., "sk_live_" = production, "sk_test_" = staging).

4.  Compute hash: hash = SHA-256("sk_live_" + raw_key_suffix)
    Why include prefix in hash: prevents key prefix stripping attacks.

5.  Redis GET "apikey:{hash}" → serialized JSON or nil.

6.  If nil:
    a. Redis cache miss: lookup PostgreSQL by hash.
    b. If found in PG and active: warm Redis cache, proceed.
    c. If not found or revoked: return 401.

7.  Deserialize key metadata:
    {key_id, scopes, status, expires_at, environment, owner_id}

8.  Validations:
    a. status == "active" (not revoked, not suspended)
    b. expires_at > NOW() (or expires_at IS NULL for non-expiring keys)
    c. environment == request.environment (e.g., request to /api/v1/... matches "live")
    d. required_scope for endpoint is in key.scopes

9.  If all pass: attach {key_id, owner_id, scopes} to request context.
    Proxy request to upstream service.

10. Async (non-blocking): INCR Redis counter "usage:{key_id}:requests:{day}"
    Publish audit event to Kafka.

11. Return upstream response to client.
```

---

## 4. Data Model

### Entities & Schema

```sql
-- =============================================
-- API Keys: core metadata (never stores raw key)
-- =============================================
CREATE TABLE api_keys (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    -- Identifier: key_id is what appears in the developer dashboard and logs
    -- It is NOT the same as the raw key, which is never stored
    
    -- Hash of the raw key (SHA-256, hex-encoded, 64 chars)
    -- Used to look up key on incoming requests
    key_hash            CHAR(64)        NOT NULL UNIQUE,
    
    -- Visible prefix of the raw key (e.g., "sk_live_AbCd1234")
    -- Allows developer to identify which key in the dashboard
    -- Exposing first 12 chars of the raw key is safe if keys have >= 256-bit entropy
    -- (attacker knowing prefix of a 43-char random suffix has no advantage)
    key_prefix          VARCHAR(20)     NOT NULL,
    
    -- Owner
    owner_user_id       UUID            NOT NULL,
    owner_team_id       UUID,                                 -- optional team ownership
    
    -- Key metadata
    label               VARCHAR(255)    NOT NULL,             -- "Mobile App v2 - iOS Production"
    environment         VARCHAR(20)     NOT NULL              -- 'production','staging','development'
                            CHECK (environment IN ('production','staging','development')),
    
    -- Permissions
    scopes              TEXT[]          NOT NULL,             -- ['read:data', 'write:records']
    
    -- Status lifecycle: active → revoked (terminal) or suspended (recoverable)
    status              VARCHAR(20)     NOT NULL DEFAULT 'active'
                            CHECK (status IN ('active','revoked','suspended','expired')),
    
    -- Expiry (optional)
    expires_at          TIMESTAMPTZ,
    
    -- Rotation tracking
    rotated_from        UUID            REFERENCES api_keys(id),  -- previous key in rotation chain
    rotation_overlap_ends_at  TIMESTAMPTZ,                        -- when old key in rotation pair expires
    
    -- Audit fields
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    created_by_ip       INET,
    last_used_at        TIMESTAMPTZ,
    last_rotated_at     TIMESTAMPTZ,
    revoked_at          TIMESTAMPTZ,
    revoked_by_user_id  UUID,
    revoked_reason      VARCHAR(100),
    suspended_at        TIMESTAMPTZ,
    suspended_reason    VARCHAR(100),
    
    -- Leakage detection
    suspected_leak_url  TEXT,                                     -- URL where key was found
    suspected_leak_at   TIMESTAMPTZ
);

CREATE INDEX idx_ak_key_hash ON api_keys (key_hash);
CREATE INDEX idx_ak_owner ON api_keys (owner_user_id, status);
CREATE INDEX idx_ak_prefix ON api_keys (key_prefix);              -- for leakage detection lookup
CREATE INDEX idx_ak_expires ON api_keys (expires_at) WHERE expires_at IS NOT NULL;

-- =============================================
-- Usage counters: daily aggregates
-- =============================================
CREATE TABLE api_key_usage_daily (
    key_id              UUID            NOT NULL REFERENCES api_keys(id),
    usage_date          DATE            NOT NULL,
    request_count       BIGINT          NOT NULL DEFAULT 0,
    error_count         BIGINT          NOT NULL DEFAULT 0,
    bytes_transferred   BIGINT          NOT NULL DEFAULT 0,
    PRIMARY KEY (key_id, usage_date)
);

CREATE INDEX idx_usage_key_date ON api_key_usage_daily (key_id, usage_date DESC);
CREATE INDEX idx_usage_date ON api_key_usage_daily (usage_date);  -- for platform-wide aggregates

-- =============================================
-- Key rotation events
-- =============================================
CREATE TABLE key_rotation_events (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    old_key_id          UUID            NOT NULL REFERENCES api_keys(id),
    new_key_id          UUID            NOT NULL REFERENCES api_keys(id),
    initiated_by        UUID            NOT NULL,             -- user_id
    initiated_at        TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    overlap_window_hours INTEGER        NOT NULL DEFAULT 24,  -- both keys valid for this duration
    completed_at        TIMESTAMPTZ                           -- when old key auto-expired
);

-- =============================================
-- Audit log
-- =============================================
CREATE TABLE api_key_audit_events (
    id                  UUID            DEFAULT gen_random_uuid(),
    key_id              UUID,                                 -- NOT the raw key
    event_type          VARCHAR(60)     NOT NULL,
    -- event types: key_created, key_rotated, key_revoked, key_suspended, key_resumed,
    --              validation_success, validation_failure, usage_threshold_exceeded,
    --              leak_detected, rotation_overlap_expired
    event_timestamp     TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    actor_user_id       UUID,                                 -- who performed the action
    actor_ip            INET,
    actor_user_agent    TEXT,
    metadata            JSONB,
    PRIMARY KEY (event_timestamp, id)
) PARTITION BY RANGE (event_timestamp);

CREATE INDEX idx_akae_key_id ON api_key_audit_events (key_id, event_timestamp DESC);
CREATE INDEX idx_akae_owner ON api_key_audit_events 
    USING GIN (metadata) WHERE metadata ? 'owner_user_id';

-- =============================================
-- Key policy configuration (per tenant / global)
-- =============================================
CREATE TABLE api_key_policies (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    tenant_id           UUID,                                 -- NULL = global policy
    max_keys_per_user   INTEGER         NOT NULL DEFAULT 20,
    max_keys_per_team   INTEGER         NOT NULL DEFAULT 100,
    forced_rotation_days INTEGER,                             -- NULL = no forced rotation
    allowed_environments TEXT[]         NOT NULL DEFAULT ARRAY['production','staging','development'],
    max_expiry_days     INTEGER,                             -- NULL = no maximum
    require_expiry      BOOLEAN         NOT NULL DEFAULT FALSE,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);
```

### Database Choice

| Database | Fit | Reasoning |
|---|---|---|
| **PostgreSQL** | **Selected (metadata + audit + usage)** | ACID for key lifecycle transactions; partitioned audit tables; mature JSONB for flexible metadata; excellent indexing |
| **Redis** | **Selected (validation hot path)** | Sub-millisecond O(1) GET by hash; native TTL for expiring keys; INCR for usage counters; handles 500K RPS with clustering |
| DynamoDB | Alternative | Auto-scaling; eventual consistency on LSIs is concern for immediate revocation; viable with careful design |
| Cassandra | Alternative (usage) | High write throughput for 50M events/day; overkill at current scale; revisit at 100× scale |
| ClickHouse | **Selected (usage analytics)** | Column-oriented; handles time-series aggregation queries over billions of events; not suitable for transactional key management |

**Key design rationale:**
- PostgreSQL serves low-frequency (< 1 RPS) lifecycle operations with full ACID guarantees.
- Redis serves the 5,787 RPS validation hot path. The entire active key set (100K keys × 256 bytes = 25.6 MB) fits in a single Redis instance's memory — Redis Cluster is used for fault tolerance, not memory capacity.
- Write-through pattern: every lifecycle change in PostgreSQL triggers a synchronous Redis update (or delete for revocations). Revocation deletes the Redis key immediately — within the same transaction's post-commit hook.

---

## 5. API Design

Base URL: `https://api.platform.example.com/v1/apikeys`

Authentication for management endpoints: Bearer token (user's OAuth2 access token or session JWT). Validation endpoint: internal service mesh (no external auth; called by API gateway only).

---

### POST /v1/apikeys

**Auth:** Bearer token (user or service account)  
**Rate Limit:** 10 keys/user/hour, 100 keys/user/day

```
Request:
  Content-Type: application/json
  Authorization: Bearer eyJ...
  {
    "label": "Mobile App v2 - iOS Production",
    "environment": "production",
    "scopes": ["read:data", "write:records"],
    "expires_at": "2027-04-09T00:00:00Z",    // optional
    "team_id": "uuid"                          // optional
  }

Response 201 Created:
  {
    "key_id": "uuid",
    "key": "sk_live_EXAMPLE_KEY_REDACTED_FOR_DOCS",
    // ^^^^^^^^ RAW KEY: shown ONCE, never retrievable again
    "key_prefix": "sk_live_4JuP8",            // shown in dashboard for identification
    "label": "Mobile App v2 - iOS Production",
    "environment": "production",
    "scopes": ["read:data", "write:records"],
    "expires_at": "2027-04-09T00:00:00Z",
    "created_at": "2026-04-09T10:00:00Z"
  }

Response 400:
  { "error": "invalid_scope", "details": ["write:admin is not allowed"] }

Response 403:
  { "error": "key_limit_exceeded", "current": 20, "max": 20 }
```

---

### GET /v1/apikeys

**Auth:** Bearer token  
**Pagination:** cursor-based (`?cursor=<opaque>&limit=20`)

```
Response 200 OK:
  {
    "keys": [
      {
        "key_id": "uuid",
        "key_prefix": "sk_live_4JuP8",
        "label": "Mobile App v2 - iOS Production",
        "environment": "production",
        "scopes": ["read:data", "write:records"],
        "status": "active",
        "expires_at": "2027-04-09T00:00:00Z",
        "created_at": "2026-04-09T10:00:00Z",
        "last_used_at": "2026-04-09T10:45:00Z"
        // NOTE: "key" field never returned after creation
      }
    ],
    "next_cursor": null
  }
```

---

### DELETE /v1/apikeys/{key_id}

**Auth:** Bearer token (must own the key or be admin)  
**Rate Limit:** 50 revocations/user/hour

```
Request:
  {
    "reason": "Key compromised - rotating after developer offboarding"  // optional
  }

Response 204 No Content   // immediately revoked; Redis entry deleted
Response 404 Not Found    // key not found or not owned by caller
Response 403 Forbidden    // insufficient permissions
```

---

### POST /v1/apikeys/{key_id}/rotate

**Auth:** Bearer token (must own the key)  
**Rate Limit:** 20 rotations/user/hour

```
Request:
  {
    "overlap_hours": 24,    // old key valid for 24 more hours (default; range: 0-168)
    "label": "Mobile App v2 - iOS Production (rotated)"  // optional new label
  }

Response 201 Created:
  {
    "new_key_id": "uuid",
    "new_key": "sk_live_EXAMPLE_NEW_KEY_REDACTED_FOR_DOCS",
    // ^^^^^^^ RAW KEY: shown ONCE
    "new_key_prefix": "sk_live_Xz9Qw",
    "old_key_id": "uuid",
    "old_key_expires_at": "2026-04-10T10:00:00Z",   // old key active until this time
    "overlap_hours": 24
  }
```

---

### GET /v1/apikeys/{key_id}/usage

**Auth:** Bearer token (must own the key)  
**Pagination:** date range (`?from=2026-01-01&to=2026-04-09&granularity=day`)

```
Response 200 OK:
  {
    "key_id": "uuid",
    "key_prefix": "sk_live_4JuP8",
    "period": { "from": "2026-01-01", "to": "2026-04-09" },
    "summary": {
      "total_requests": 4820000,
      "total_errors": 12400,
      "error_rate": 0.0026,
      "total_bytes_transferred": 9480000000
    },
    "daily": [
      {
        "date": "2026-04-09",
        "request_count": 45000,
        "error_count": 112,
        "bytes_transferred": 89400000
      }
    ]
  }
```

---

### POST /v1/apikeys/{key_id}/suspend

**Auth:** Bearer token (owner) or Admin  
**Note:** Suspension is recoverable (unlike revocation). Used when a key is suspected leaked but under investigation.

```
Request:
  { "reason": "Suspected leak detected in GitHub commit abc123" }

Response 200 OK:
  { "key_id": "uuid", "status": "suspended", "suspended_at": "2026-04-09T11:00:00Z" }
```

---

### POST /v1/apikeys/{key_id}/resume

**Auth:** Admin only (owner cannot self-resume a suspended key without admin review)

```
Response 200 OK:
  { "key_id": "uuid", "status": "active" }
```

---

### POST /internal/v1/validate (Internal — API Gateway Only)

**Auth:** Internal service mesh mTLS; not exposed externally  
**Rate Limit:** No limit (internal)

```
Request:
  { 
    "key_hash": "sha256-hex-of-raw-key",   // gateway pre-hashes key
    "required_scope": "read:data",
    "environment": "production"
  }

Response 200 OK (valid):
  {
    "valid": true,
    "key_id": "uuid",
    "owner_id": "uuid",
    "scopes": ["read:data", "write:records"],
    "expires_at": "2027-04-09T00:00:00Z"
  }

Response 200 OK (invalid):
  {
    "valid": false,
    "reason": "revoked"    // "revoked" | "expired" | "suspended" | "invalid_scope" | "not_found"
  }
  // Always 200; non-200 means the validation service itself failed
```

---

## 6. Deep Dive: Core Components

### 6.1 Key Generation and Entropy Requirements

**Problem it solves:**  
API keys are long-lived bearer credentials. An attacker who can guess or brute-force a valid key gains full access to the associated account's permissions. The key generation algorithm must produce keys with sufficient entropy to make enumeration infeasible, even with a botnet performing billions of validation requests per day.

**Approaches Compared:**

| Method | Entropy | Length | Guessable? | Notes |
|---|---|---|---|---|
| UUID v4 | 122 bits | 36 chars | No | Sufficient but not URL-safe; no version/env info |
| CSPRNG base64 (16 bytes) | 128 bits | 22 chars | Barely (short) | Too short; 128-bit margin is thin at scale |
| **CSPRNG base64url (32 bytes)** | **256 bits** | **43 chars** | **No** | **Selected: 2^256 search space** |
| CSPRNG base62 (32 bytes) | ~190 bits | 43 chars | No | Slightly less entropy; avoids +/ chars |
| HMAC-based deterministic | Depends on key | Variable | Depends | For deterministic re-generation; not required here |

**Selected: 32-byte CSPRNG encoded as base64url (URL-safe base64 without padding)**

Reasoning: 32 bytes = 256 bits of entropy. Even at 1 billion guesses per second (which would require an absurd amount of network traffic and would be detected), the expected time to find a valid key is 2^256 / 10^9 seconds ≈ 1.16 × 10^68 years — infeasible by astronomical margins.

**Key Format:**

```
Format: <environment_prefix>_<random_suffix>

Examples:
  sk_live_EXAMPLE_KEY_REDACTED_FOR_DOCSuVwXyZ01234  (production)
  sk_test_EXAMPLE_TEST_KEY_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX  (staging/test)
  sk_dev_EXAMPLE_DEV_KEY_XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX  (development)

Structure:
  sk_        → service key type identifier (helps with regex-based scanning)
  live_      → environment: live | test | dev
  [43 chars] → CSPRNG(32 bytes) → base64url (no padding, URL-safe)

Total raw key length: 3 + 5 + 43 = 51 characters (environment dependent)
Entropy: 256 bits (in the 43-char random suffix)
```

**Why include a prefix:**
1. The prefix (e.g., `sk_live_`) allows automatic detection by secret scanning tools (GitHub, GitLab, IDEs, pre-commit hooks). Platforms register their key pattern regex with GitHub's secret scanning program.
2. The prefix reveals the environment (safe to expose — this is not sensitive), allowing gateways to quickly reject test keys on production endpoints without even hashing.
3. The key type prefix allows future key types (e.g., `wk_` for webhook keys, `pk_` for publishable keys) without collision.

**Implementation:**

```python
import secrets
import base64
import hashlib

ENVIRONMENTS = {
    'production': 'live',
    'staging': 'test', 
    'development': 'dev'
}

def generate_api_key(environment: str) -> tuple[str, str, str]:
    """
    Returns (raw_key, key_hash, key_prefix)
    
    raw_key: shown to user once, never stored
    key_hash: stored in database and Redis
    key_prefix: shown in dashboard for identification
    """
    env_prefix = ENVIRONMENTS[environment]
    
    # Generate cryptographically secure random bytes
    # secrets module uses os.urandom() which reads from /dev/urandom (Linux) 
    # or BCryptGenRandom (Windows) - both are CSPRNG
    random_bytes = secrets.token_bytes(32)
    
    # Encode as URL-safe base64 (no padding)
    # base64url uses A-Z, a-z, 0-9, -, _ (URL-safe; no +, /, or = chars)
    random_suffix = base64.urlsafe_b64encode(random_bytes).rstrip(b'=').decode('ascii')
    # Result: exactly 43 chars (ceil(32 * 8 / 6) = 43 base64 chars, with trailing byte alignment)
    
    raw_key = f"sk_{env_prefix}_{random_suffix}"
    
    # Hash: SHA-256 is appropriate (key has 256 bits entropy; offline cracking infeasible)
    # Include the full key (with prefix) to prevent prefix-stripping attacks
    key_hash = hashlib.sha256(raw_key.encode('utf-8')).hexdigest()
    
    # Prefix: first 8 chars of random suffix (sufficient for user identification)
    # Revealing 8 chars (48 bits) of a 256-bit key provides no security advantage to attacker
    key_prefix = f"sk_{env_prefix}_{random_suffix[:8]}"
    
    return raw_key, key_hash, key_prefix

def validate_key_format(raw_key: str) -> bool:
    """Sanity check before hashing (prevent excessive computation on obviously invalid inputs)"""
    import re
    pattern = r'^sk_(live|test|dev)_[A-Za-z0-9\-_]{43}$'
    return bool(re.match(pattern, raw_key))
```

**Interviewer Q&As:**

Q: Why SHA-256 for API key hashing instead of bcrypt or Argon2id?  
A: API keys contain 256 bits of CSPRNG entropy — they are not susceptible to dictionary attacks. Bcrypt and Argon2id are designed to be slow to compute specifically to resist offline cracking of low-entropy secrets like passwords. Applying Argon2id to API keys would add ~100 ms to every API request (validation calls happen at 5,787 RPS), creating a severe performance bottleneck for zero security benefit. SHA-256 is secure here because the 256-bit random key space makes brute-force enumeration computationally infeasible regardless of hash speed. SHA-256 validation takes ~microseconds.

Q: Why is it safe to display the first 8 characters of the random suffix in the dashboard as a "key prefix"?  
A: The random suffix has 256 bits (43 base64url chars). Exposing the first 8 characters reveals 48 bits. The attacker still needs to guess the remaining 208 bits (35 chars), which is 2^208 ≈ 4 × 10^62 possibilities — computationally infeasible. The exposed prefix serves purely as a user-facing identification aid (e.g., "I need to revoke the key starting with `sk_live_4JuP8wX9`") without compromising security. This pattern is used by Stripe, AWS, and GitHub for the same reason.

Q: How do you handle the case where an attacker tries to enumerate valid key prefixes by monitoring validation latency?  
A: The validation path hashes the full raw key and performs a Redis GET by hash. The response time is essentially constant (~1ms) regardless of whether the key is valid. The Redis GET returns either a value or nil — both in ~0.5ms. There is no "prefix match" step that would create timing differences. Even if an attacker tries to infer information from varying response times, the noise in network RTT (10-50ms) completely drowns out any sub-millisecond timing signal.

Q: What are the entropy requirements for API keys in regulatory/compliance contexts?  
A: NIST SP 800-63B and PCI DSS 4.0 do not directly specify API key entropy, but they set authentication secret requirements of at least 112 bits (64-bit random nonces are considered minimum for session tokens). For API keys used as long-lived credentials: OWASP recommends 128 bits minimum; most security-conscious platforms (AWS, GitHub, Stripe) use 256 bits. Our choice of 256 bits exceeds all current regulatory guidance. FIPS 140-2/3 compliance: the CSPRNG must be a FIPS-approved random number generator; `os.urandom()` on RHEL/CentOS with FIPS mode enabled uses the FIPS-validated DRBG.

Q: How would you design a "publishable key" (client-side, lower privilege) vs. a "secret key" (server-side, higher privilege)?  
A: Distinguish by key type prefix (`pk_live_` vs `sk_live_`). Publishable keys: limited to read-only, non-sensitive scopes (e.g., `read:public_data`); designed to be embedded in frontend JavaScript; rate-limited more aggressively; do not grant access to account management or financial operations. Secret keys: full configured scope; must never appear in client-side code or version control; used only in server environments. The validation logic checks key type against the endpoint's security classification: an endpoint marked `secret_key_required` rejects `pk_` keys regardless of scope. This is the pattern used by Stripe and Braintree.

---

### 6.2 Key Revocation and Cache Invalidation

**Problem it solves:**  
API keys are the primary means of authentication for API consumers. When a key is compromised, offboarded, or expired, revocation must take effect globally within milliseconds — not after a cache TTL expires. A key that was revoked but still valid in a Redis cache creates an attack window where a stolen key remains usable.

**Approaches Compared:**

| Approach | Revocation Latency | Complexity | Impact on Validation RPS |
|---|---|---|---|
| Short TTL (expire and re-check DB) | Bounded by TTL (up to 60s) | Low | None |
| No cache (DB on every validation) | Immediate | Low | Catastrophic (5K RPS → PostgreSQL) |
| Cache with pub/sub invalidation | < 500ms (pub/sub delivery) | Medium | None (delete from Redis) |
| **Cache + immediate DELETE (selected)** | **< 50ms (same DC)** | **Low-Medium** | **None** |
| Versioned cache (generation counter) | < 1ms after version change | Medium | +1 Redis GET for version check |

**Selected: Write-through Redis cache with immediate DEL on revocation**

Reasoning: On revocation, the Key Management Service calls `Redis.DEL("apikey:{key_hash}")` synchronously as part of the revocation transaction (within the same DB transaction's post-commit hook). This makes the key invalid in Redis within the same millisecond as the revocation is committed. The next validation attempt by the attacker finds nil in Redis, falls back to PostgreSQL, and gets `status=revoked`. No TTL-based delay.

**Multi-region revocation:**

```
Problem: Key Management Service (region A) revokes key.
         Redis replica in region B has stale entry.
         Attacker using stolen key hits an API gateway in region B.

Solution: Pub/sub revocation propagation

Architecture:
  Key Management Service:
    1. Transaction: UPDATE api_keys SET status='revoked' WHERE id=? (PostgreSQL)
    2. Post-commit: Redis.DEL("apikey:{key_hash}") [local region]
    3. Publish to global message bus:
       Topic: "apikey.revocations"
       Message: { key_hash: "<hash>", revoked_at: "<timestamp>", key_id: "<id>" }

  Regional Redis Invalidation Workers (one per region):
    Subscribe to "apikey.revocations" topic
    On message:
      Redis.DEL("apikey:{key_hash}")  [local region's Redis]
      // Propagation time = message bus cross-region latency (~100-200ms)

  Validation logic on cache miss after revocation:
    1. Redis.GET("apikey:{hash}") → nil
    2. PostgreSQL lookup → status='revoked'
    3. Return invalid: do NOT re-populate Redis cache (prevent zombie caching)
    4. Return 401

Global revocation SLA: < 500ms (sum of: PostgreSQL commit + pub/sub publish + 
    message bus cross-region delivery + Redis DEL at target region)
```

**Implementation Detail:**

```python
def revoke_api_key(key_id: str, revoked_by: str, reason: str) -> None:
    """
    Revocation must be:
    1. Durable: PostgreSQL UPDATE committed before returning
    2. Immediate: Redis DEL called in post-commit hook
    3. Global: pub/sub message broadcast to all regions
    """
    with db.transaction(isolation='READ COMMITTED') as txn:
        key = txn.execute(
            "UPDATE api_keys SET status='revoked', revoked_at=NOW(), "
            "revoked_by_user_id=?, revoked_reason=? "
            "WHERE id=? AND status='active' RETURNING key_hash, id",
            [revoked_by, reason, key_id]
        ).fetchone()
        
        if key is None:
            raise KeyNotFoundError(f"Key {key_id} not found or not active")
        
        # Post-commit hook: executes after txn.commit()
        txn.on_commit(lambda: _propagate_revocation(key.key_hash, key.id))
    
    # Emit audit event (async)
    emit_audit_event('key_revoked', key_id=key_id, actor=revoked_by, reason=reason)

def _propagate_revocation(key_hash: str, key_id: str) -> None:
    """Called after PostgreSQL commit confirms durability"""
    
    # 1. Immediate local Redis invalidation
    redis_client.delete(f"apikey:{key_hash}")
    
    # 2. Global pub/sub (Kafka or Redis Streams) for cross-region propagation
    publish_to_message_bus(
        topic="apikey.revocations",
        message={
            "key_hash": key_hash,
            "key_id": key_id,
            "revoked_at": time.time()
        }
    )
    # Non-blocking: function returns; message delivered asynchronously

def warm_redis_cache(key: ApiKey) -> None:
    """Write-through: called after key creation and on cache miss (active key)"""
    cache_entry = {
        "key_id": key.id,
        "owner_id": key.owner_user_id,
        "scopes": key.scopes,
        "status": key.status,
        "environment": key.environment,
        "expires_at": key.expires_at.timestamp() if key.expires_at else None
    }
    
    if key.expires_at:
        remaining_ttl = int((key.expires_at - datetime.now()).total_seconds())
        if remaining_ttl <= 0:
            return  # already expired; don't cache
        redis_client.setex(f"apikey:{key.key_hash}", remaining_ttl, json.dumps(cache_entry))
    else:
        # Non-expiring key: cache with a long TTL; refreshed on each validation hit
        # We use 1h TTL + refresh-on-access to ensure stale entries are cleaned up
        # (e.g., if a key is suspended, the 1h window is the max staleness)
        # For revocation, we DELETE explicitly — the TTL is a fallback, not the primary mechanism
        redis_client.setex(f"apikey:{key.key_hash}", 3600, json.dumps(cache_entry))
```

**Interviewer Q&As:**

Q: What is the attack window if Redis is down during a revocation?  
A: If the local Redis DEL call fails (Redis down), the cache entry remains until it naturally expires (1-hour TTL). During this window, a revoked key appears valid to the validation path. Mitigations: (1) Circuit breaker: if Redis DEL fails, write the revocation to a "pending Redis invalidations" queue (PostgreSQL table or reliable message queue). A background worker retries the DEL when Redis recovers. (2) The Redis TTL (1 hour) bounds the maximum attack window. (3) For high-security revocations (key leaked/compromised), implement a "revocation bypass" check: on every validation cache hit, compare the cached `status` against a bloom filter of recently revoked key hashes stored in a small in-memory set on each pod. The pod's in-memory revocation set is populated via the pub/sub channel and limits the attack window to pub/sub delivery latency (~100ms) independent of Redis state.

Q: How do you handle key rotation atomically so there's never a window where neither key is valid?  
A: Key rotation creates the new key first (it is immediately active), then starts the overlap window for the old key. The sequence: (1) Generate new key and insert into DB + Redis (new key immediately usable). (2) Update old key with `rotation_overlap_ends_at = NOW() + overlap_hours`. (3) During the overlap window: both old and new keys are valid. (4) A background job (`overlap_expiry_worker`) runs every minute, finds keys where `rotation_overlap_ends_at < NOW()`, and marks them as `expired` + DELetes from Redis. There is never a gap where both keys are invalid. The user deploys the new key during the overlap window at their own pace.

Q: How does key expiry work with the Redis cache?  
A: At key creation/cache-warming time, the Redis entry is set with `SETEX` using `TTL = expires_at - now()`. Redis will automatically evict the entry at expiry. The validation path checks `expires_at` from the cached metadata as a belt-and-suspenders check (in case of clock skew between Redis TTL and server time). No background sweep job needed for expiry correctness. A background job for cleanup of PostgreSQL records still runs nightly (`UPDATE api_keys SET status='expired' WHERE expires_at < NOW() AND status='active'`) for data consistency.

Q: What should the validation service return for a suspended key vs. a revoked key?  
A: Both return `valid: false` to the API gateway, which returns `401 Unauthorized` to the caller. The difference is internal: suspended = potentially recoverable (under investigation), revoked = terminal. The validation response includes the `reason` field for internal logging and monitoring: `"reason": "suspended"` generates a different alert than `"reason": "revoked"`. The API consumer sees the same `401` response either way — there is no information leakage about whether the key is revoked vs. suspended vs. expired.

Q: How do you ensure the revocation is visible to the developer's dashboard immediately?  
A: The developer dashboard reads from PostgreSQL (not Redis). Since the revocation is committed to PostgreSQL synchronously (step 1 of the revocation function), the dashboard shows the key as revoked immediately after the operation completes. The Redis propagation is for the API validation hot path. Dashboard reads go to a PostgreSQL read replica with < 1s replication lag — acceptable for a management UI.

---

### 6.3 API Key Leakage Detection

**Problem it solves:**  
Developers routinely accidentally commit API keys to version control, paste them in GitHub issues, include them in public forum posts, or leave them in Docker images pushed to public registries. Once a key appears in a public location, it can be scraped by automated bots within minutes. The platform needs to detect this quickly, suspend the key, and notify the developer — ideally before damage is done.

**Detection Approaches Compared:**

| Approach | Detection Speed | Coverage | Complexity |
|---|---|---|---|
| Reactive only (wait for abuse reports) | Hours to days | Low | Trivial | 
| Periodic regex scan of GitHub (manual) | Hours | Low | Low |
| GitHub Secret Scanning Partner Program | Minutes to hours | GitHub only | Low (program enrollment) |
| GitGuardian / Trufflehog (external service) | Real-time | Very broad | Medium (integration) |
| **Partner program + external scanner + canary tokens (selected)** | **Minutes** | **Very broad** | **Medium** |

**Selected: Multi-vector leakage detection**

1. **GitHub Secret Scanning Partner Program**: Register the key format regex pattern with GitHub. GitHub scans all public commits and alerts the platform via webhook when a match is found. This is free, covers all of GitHub, and has low latency (minutes from commit to alert). Pattern: `sk_(live|test|dev)_[A-Za-z0-9\-_]{43}`.

2. **External Scanner Integration (GitGuardian, Trufflehog)**: Commercial/OSS tools that scan GitHub, GitLab, npm, PyPI, Docker Hub, Pastebin, and more. Integrate via webhook.

3. **Canary Tokens (Honeypot Keys)**: Generate fake API keys that are syntactically valid and stored as "canary" entries in Redis. These keys have no permissions and will never appear in legitimate traffic. If a canary key appears in a validation request, it signals the key database or Redis has been exfiltrated (attacker found keys and is trying them). Immediately alert security team.

**Implementation Detail:**

```
GitHub Secret Scanning Webhook (inbound):
  POST /internal/webhooks/secret-scanning
  {
    "type": "secret_scanning_alert",
    "token_type": "platform_api_key",
    "token": "sk_live_4JuP8wX9...",    // the exposed key
    "url": "https://github.com/user/repo/blob/abc123/config.py#L42",
    "source": "github"
  }

Handler:
  function HandleLeakageAlert(alert):
    raw_key = alert.token
    
    // 1. Validate key format (reject obviously wrong formats — scanner false positives)
    if not validate_key_format(raw_key): 
      log("false positive from scanner")
      return
    
    // 2. Hash the key to look up in DB
    key_hash = SHA256(raw_key)
    
    // 3. Look up key
    key = DB.SELECT FROM api_keys WHERE key_hash = ? AND status = 'active'
    if key is None:
      log("leaked key not found or already revoked")
      return
    
    // 4. Suspend key immediately (not revoke — owner may want to review)
    DB.UPDATE api_keys SET 
      status='suspended',
      suspended_at=NOW(),
      suspended_reason='leak_detected',
      suspected_leak_url=alert.url,
      suspected_leak_at=NOW()
    WHERE id = key.id
    
    // 5. Delete from Redis (immediate validation block)
    Redis.DEL(f"apikey:{key_hash}")
    
    // 6. Publish global revocation event
    publish_to_message_bus("apikey.revocations", {key_hash, key.id})
    
    // 7. Notify owner
    send_notification(
      user_id=key.owner_user_id,
      type='key_leak_detected',
      message=f"Your API key '{key.key_prefix}...' was found in a public location: {alert.url}. "
               "It has been suspended. Please rotate this key immediately.",
      urgency='high'
    )
    
    // 8. Audit
    emit_audit_event('leak_detected', key_id=key.id, source_url=alert.url)

Canary token check (in validation path):
  function ValidateKey(raw_key):
    key_hash = SHA256(raw_key)
    cached = Redis.GET(f"apikey:{key_hash}")
    
    if cached and JSON.parse(cached).status == 'canary':
      // HONEYPOT HIT: attacker has our key database
      emit_critical_security_alert('canary_key_validated', {
        key_hash: key_hash,
        request_ip: request.ip,
        timestamp: now()
      })
      return invalid("not_found")   // do not reveal it's a canary
    
    // ... normal validation ...
```

**Interviewer Q&As:**

Q: What is the trade-off between suspending vs. immediately revoking a leaked key?  
A: Suspension (recoverable): the key is blocked immediately, but the owner can investigate whether the leak is real, whether the key was actually used maliciously, and coordinate with their team before the key is permanently invalidated. Immediate revocation: maximum security but may cause production outages if the developer's application was still using the key during the investigation window. The correct default: suspend immediately (< 1 second effect) and let the owner decide to revoke or rotate. The only exception is confirmed high-severity usage (malicious activity detected on the key) — in that case, revoke without waiting for owner action. Always give the developer a parallel path to issue a new key immediately so they can restore service while the compromised key is being analyzed.

Q: How do you handle false positives from the leakage detection scanner?  
A: False positives (key pattern match that is not an actual platform key) are handled by: (1) Re-validating the matched string against the full key format regex before taking action. (2) Checking that the matched key exists in the database before suspending — a random string matching the pattern is not a real key. (3) Implementing a "grace window" of 5 minutes between detection and suspension for low-confidence matches, during which a human-in-the-loop review can override. High-confidence matches (exact format match, key exists in DB, already had prior legitimate usage from a different IP) should be suspended immediately without waiting. Track false positive rate; if > 1%, adjust the scanner's sensitivity or pattern matching.

Q: How do you handle a key that was leaked but the attacker already used it before suspension?  
A: Forensic response: (1) Retrieve audit log for the key: all validation events, IP addresses, timestamps, resources accessed. (2) If the attacker made API calls: identify the resources accessed/modified (using request logs from the API gateway, correlated by `key_id`). (3) Notify the owner of the scope of damage. (4) For write operations: initiate data incident response (rollback if feasible, notify affected downstream users). (5) Block attacker IPs at the API gateway. (6) Analyze whether the leaked key indicates a broader compromise (e.g., the developer's entire credential set may be exposed). This is an incident response workflow that the Key Management Service supports by providing complete audit trails.

Q: What prevents an attacker from repeatedly creating API keys, leaking them, and monitoring which ones are suspended to probe the detection system?  
A: Rate limits on key creation (10/hour per user). Created keys are not visible to the attacker beyond what they already know (they created the key). The fact that a leaked key gets suspended is visible to the owner but not the leaker (unless the owner is the attacker). The suspension response to the leaker is a `401` — indistinguishable from any other invalid key response. The timing of suspension reveals nothing operationally useful to the attacker (they know the key works until it doesn't). The only information leakage is "this platform has leakage detection," which is public knowledge and not sensitive.

Q: How would you implement canary tokens (honeypot keys) and what does triggering one tell you?  
A: Canary keys are pre-generated API keys with the correct format and stored in the key DB and Redis with `status='canary'`. They are never issued to any customer. They never appear in any legitimate traffic. Canary keys are seeded in various locations: (1) At the end of a sorted list of keys in the DB (an attacker who dumps the DB in order may try the first few and the last few keys). (2) As decoys in infrastructure configs in a canary repo monitored for access. (3) In encrypted backups (if the backup decryption key is stolen, keys will be tried). If any canary key is validated by the validation service, it means: (a) The key database or Redis snapshot was exfiltrated, (b) An attacker is probing the validation endpoint, (c) A backup was decrypted without authorization. Trigger immediate P0 security incident response.

---

## 7. Scaling

### Horizontal Scaling

**Key Management Service (lifecycle operations):** Low-RPS administrative API (<< 1 RPS on write paths). A single instance plus standby suffices; add replicas for geographic proximity to developer dashboards.

**Validation Service (API gateway integration):** The validation logic is essentially a Redis GET + JSON parse + scope check. This runs in-process at the API gateway with a Redis connection pool. At 500K RPS, assuming 10,000 API gateway pods each handling 50 requests/second, each pod makes ~50 Redis GETs/second. Redis Cluster handles this effortlessly.

**Redis Cluster sizing:**
- 100K active keys × 256 bytes per entry = 25.6 MB — entire keyspace fits in 1 GB Redis instance.
- 500K validations/second = 500K Redis GET operations/second.
- Single Redis node: ~100K simple commands/second.
- 500K / 100K = 5 Redis primary nodes minimum.
- Redis Cluster with 6 primary + 6 replica nodes provides: ~600K commands/second capacity with fault tolerance.

**Usage counter scaling:**
- 500K INCR operations/second to Redis. Optimize: batch INCR using Redis pipelines (send 100 INCRs in one TCP roundtrip). Each API gateway pod batches 100ms worth of increments and flushes via pipeline.
- Alternative: use Kafka to aggregate usage events and batch-write to PostgreSQL every 5 minutes (reduces Redis write pressure; loses real-time counter accuracy).

### DB Sharding

**Key metadata (PostgreSQL):** 100K keys × 1 KB = 100 MB — trivially fits on a single PostgreSQL instance. No sharding needed at this scale. Shard by `key_hash` prefix if scale reaches 100M+ keys.

**Usage aggregates (PostgreSQL):** 9.3 GB growing at ~13 MB/day. Partition by month. Archive partitions older than 2 years to cold storage.

### Replication

| Store | Strategy | RPO | RTO |
|---|---|---|---|
| Key Metadata (PostgreSQL) | Synchronous to 1 standby (Patroni) | 0 | < 30s |
| Redis Cluster | 1 replica per shard, AOF enabled | < 1s | < 10s |
| Audit Log (Kafka) | Replication factor 3, min.insync.replicas=2 | 0 | N/A |

### Caching

Covered in Component 6.2. Summary: write-through Redis cache at key creation; immediate DEL on revocation; 1-hour TTL as fallback; regional pub/sub for cross-DC invalidation.

### Interviewer Q&As on Scaling

Q: At what scale does SHA-256 key hashing become a CPU bottleneck?  
A: SHA-256 computes at ~1 GB/second on a modern CPU core (hardware-accelerated via AES-NI / SHA extensions). An API key is ~51 bytes. SHA-256 of 51 bytes: 51 / 1,000,000,000 bytes/second = 0.051 µs per hash. At 500K validations/second: 500K × 0.051 µs = 25.5 ms of CPU time per second across all requests. Spread across 10K gateway pods: each pod spends 0.0025 ms/second on SHA-256. Completely negligible. SHA-256 will not be a CPU bottleneck until multiple billions of validations per second on a single machine.

Q: How do you handle burst traffic during a DDoS that uses stolen API keys?  
A: An attacker using a valid (stolen) API key is hard to distinguish from legitimate traffic at the key-validation layer. Defense: (1) Per-key rate limits: even a valid key has a configured maximum RPS (e.g., 10K req/s). The API gateway enforces this via Redis counters. If a key exceeds its limit, the key is temporarily throttled (429) rather than suspended (to avoid false-positive suspension of legitimate spiky workloads). (2) Anomaly detection: if a key's request rate is 100× its 7-day average, trigger an alert and step up to human review or automated suspension. (3) IP-level rate limiting at the load balancer for volumetric DDoS, independent of key validation.

Q: How would you implement per-key rate limits efficiently at 500K RPS?  
A: Sliding window rate limiter in Redis using a sorted set: `ZADD ratelimit:{key_id} {timestamp} {request_id}` + `ZREMRANGEBYSCORE ... 0 {window_start}` + `ZCARD`. Or use a fixed window with `INCR ratelimit:{key_id}:{minute} + EXPIRE`. The fixed window approach is O(1) per request: one INCR and one check against the limit. For 500K keys, Redis handles this at sub-millisecond per operation. Optimize by combining the key validation GET and rate limit INCR into a single Redis Lua script (atomic, one roundtrip):

```lua
-- Lua script executed atomically in Redis
local key_data = redis.call('GET', KEYS[1])           -- validation lookup
if key_data == nil then return {nil, nil} end
local count = redis.call('INCR', KEYS[2])             -- rate limit counter
redis.call('EXPIRE', KEYS[2], ARGV[1])                -- TTL for window
return {key_data, count}
```

Q: How do you ensure usage tracking is not a bottleneck on the validation hot path?  
A: Usage tracking is performed asynchronously, completely decoupled from the validation response. Two approaches: (1) Fire-and-forget in-process: after responding to the API caller, the gateway pod enqueues a INCR operation to a local in-memory queue. A background goroutine/thread drains the queue and pipelines INCR commands to Redis. The API caller never waits. (2) Publish to Kafka: the validation success event is published to a Kafka topic; a consumer aggregates and writes counts. Option 1 is simpler and has lower latency for the usage dashboard. Option 2 is more resilient (Kafka provides durability if the gateway pod crashes before flushing). For critical billing usage data, use Kafka (guaranteed delivery); for informational dashboards, in-process queue is acceptable.

Q: What is the key lookup complexity and how does it compare to JWT validation?  
A: API key validation: hash(key) → Redis GET O(1) → JSON parse → scope check → O(n) where n = number of scopes in key (typically < 10). Total: ~1-2 ms. JWT validation: base64 decode header + payload → SHA-256 of header.payload → RSA-2048 verify (signature check) → parse payload → check exp, iss, aud → O(1) claims check. Total: RSA verify is the expensive step at ~0.5 ms on modern hardware with cryptographic acceleration. Both are O(1) and within 1-2ms. JWT validation has no external dependency (JWKS cached in-process). API key validation requires Redis (network call, ~0.5 ms RTT). For lowest latency: JWT is slightly faster due to no Redis network hop. For most use cases, both are well within acceptable bounds.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| Redis validation store down | All API key validation fails → 503 | Health check + error rate spike | Redis Cluster auto-failover (< 10s); fallback: serve from PostgreSQL (slower, 5-10ms) with circuit breaker |
| PostgreSQL down | Key creation/rotation/revocation fails; cache misses cannot be resolved | Health check | Patroni auto-failover (< 30s); Redis cache continues serving active keys during PG failover |
| Key Management Service down | Lifecycle operations unavailable; validation unaffected | Health check | Validation runs in Redis (no dependency on Key Management Service pod) |
| Revocation pub/sub delivery failure | Regional caches not invalidated promptly | Message delivery monitoring | Retry with exponential backoff; 1-hour TTL bounds maximum stale window |
| Clock skew between nodes | Key expiry inconsistency | NTP monitoring | NTP sync; validate expiry against server time from same pod |
| Leakage scanner webhook down | Leaked keys not detected promptly | Dead man's switch on scanner heartbeat | Alert on-call; manual check GitHub alerts console |
| Usage counter Redis failure | Usage data gap | Counter aggregation anomaly detection | Re-construct from API gateway access logs via batch job |

### Idempotency

- **Key Creation**: Not idempotent (generates new CSPRNG key each call). Deduplication: developer dashboard prevents double-click re-submission (client-side) + idempotency key header support (`Idempotency-Key: <uuid>`) cached for 24 hours.
- **Key Revocation**: Idempotent: revoking an already-revoked key returns success (no error).
- **Key Rotation**: Idempotent with `Idempotency-Key` header: same rotation request returns the already-created new key (cached by idempotency key for 24 hours).
- **Leakage suspension**: Idempotent: suspending an already-suspended key is a no-op.

### Circuit Breaker

Wrap Redis connection and PostgreSQL connection. If Redis is unavailable: fall back to PostgreSQL for validation with appropriate latency degradation. If PostgreSQL is unavailable: continue serving from Redis cache; block lifecycle operations with clear error message. If both are down: return 503 Service Unavailable for all key validation (fail closed — security critical path must not fail open).

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert |
|---|---|---|
| `apikey_validation_total{result="valid|revoked|expired|not_found|suspended"}` | Counter | `not_found` rate > 10% → possible scraping |
| `apikey_validation_duration_seconds` | Histogram (p99) | p99 > 10ms → warn |
| `apikey_redis_hit_ratio` | Gauge | < 0.99 → alert |
| `apikey_revocations_total{reason}` | Counter | `leak_detected` reason > 0 → alert |
| `apikey_canary_validations_total` | Counter | Any > 0 → P0 security alert |
| `apikey_creation_total` | Counter | Spike > 100/min → alert (enumeration/abuse) |
| `apikey_rotation_total` | Counter | Spike → alert |
| `apikey_leakage_detections_total{source}` | Counter | Any > 0 → alert |
| `apikey_revocation_propagation_latency_seconds` | Histogram | p99 > 1s → alert |
| `apikey_usage_counter_lag_seconds` | Gauge | > 5 min → warn (counters delayed) |

### Distributed Tracing

- Validation path: `extract_key` → `hash_key` → `redis_get` → `scope_check`. Total span tagged with `cache_hit: true/false` and `valid: true/false`.
- Lifecycle operations: `create_key` → `write_postgres` → `write_redis` → `emit_event`.
- Leakage detection: `receive_webhook` → `lookup_key` → `suspend_key` → `notify_owner`.

### Logging

- Never log raw API keys. Log only `key_prefix` (first 12 chars) and `key_id` (UUID).
- Log `key_hash[0:16]` for correlation with validation events.
- Validation failure logs include: key_prefix, failure reason, requesting IP, endpoint attempted.
- Leakage detection logs include: key_prefix, source URL (sanitized), detection timestamp, action taken.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Alternative | Rationale |
|---|---|---|---|
| Key hashing algorithm | SHA-256 | Argon2id | Keys have 256-bit entropy; bcrypt/Argon2 adds latency for no security benefit |
| Key entropy | 256 bits (32-byte CSPRNG) | 128 bits | Future-proof; exceeds all current guidance; minimal cost |
| Key format | Prefixed (`sk_live_...`) | Raw random string | Prefix enables secret scanning partnership, environment routing, type detection |
| Revocation propagation | Redis DEL + pub/sub | TTL-based expiry | Immediate effect locally (< 1ms); cross-region < 500ms; TTL alone is too slow |
| Leakage detection | Multi-vector (scanner partner + webhook + canary) | Reactive only | Proactive reduces window from days to minutes; canary catches DB exfiltration |
| Suspension vs revocation | Suspend on detection, revoke on confirmation | Immediate revocation | Preserves developer ability to investigate without production outage |
| Usage tracking | Redis INCR + async PostgreSQL aggregation | Synchronous DB write | Redis INCR is non-blocking; sync DB write adds latency to hot path |
| Key prefix display | First 8 chars of random suffix | Show nothing | Sufficient for identification; does not aid brute-force (208 bits remain unknown) |
| Internal validation endpoint | Redis GET via API gateway (no service call) | Call Key Management Service | Eliminates service-to-service call on hot path; Redis is directly queryable |

---

## 11. Follow-up Interview Questions

**Q1: How would you implement hierarchical API keys where a parent key can spawn child keys with narrower scopes?**  
A: Parent key has full scope set. Child key creation: POST /v1/apikeys with `parent_key_id` and a subset of the parent key's scopes. Validation: child key's `parent_key_id` is stored; if parent key is revoked, all child keys are also revoked (cascade). Implement cascade via DB trigger or background job: on parent revocation, update all children to revoked status and DEL their Redis entries. Use case: a CI/CD pipeline key (child) has only `read:artifacts` scope derived from a developer's broader key (parent). Revoking the developer key automatically kills the pipeline key.

**Q2: How do you design API key scopes to be backward-compatible when adding new scopes?**  
A: Additive changes only: never remove a scope that existing keys may have. Deprecate scopes by marking them in the schema as deprecated (`is_deprecated=true`) and showing a warning in the dashboard when a key uses a deprecated scope. Migration: implement a compatibility mapping where the deprecated scope `read:data` is aliased to the new scope `read:records`. At resource server validation, expand deprecated scopes to their replacements. New sensitive scopes (e.g., `admin:billing`) require explicit re-grant — existing keys never gain new scopes without user action.

**Q3: What is the difference between an API key and a Personal Access Token (PAT)?**  
A: Both are long-lived bearer credentials. Differences: (1) Scope: API keys are often tied to a service account or application; PATs are tied to a specific human user and inherit that user's permissions (or a subset). (2) Revocation impact: revoking a PAT affects only operations by that human; revoking an API key affects a service. (3) Audit attribution: PAT usage is attributed to the human user in audit logs; API key usage is attributed to the key/application. (4) GitHub/GitLab use "personal access tokens" for human developers making API calls on their own behalf; "API keys" are for application-to-application authentication. The technical implementation is identical; the naming is a UX/semantic distinction.

**Q4: How do you implement IP allowlisting for an API key?**  
A: Add an `allowed_ips` field (CIDR ranges array) to the key record. Store in the Redis cache entry. At validation time: check if `request.ip` falls within any CIDR in `allowed_ips`. If `allowed_ips` is empty, no IP restriction. IP allowlisting provides defense-in-depth: even a stolen key is useless from an attacker's IP. Trade-off: breaks when the developer's egress IP changes (CI/CD environment, cloud provider scaling). Solution: accept both specific IPs and CIDR ranges; provide a clear "your IP is X, add it to the allowlist" message in the dashboard.

**Q5: How would you implement API key versioning to support multiple key formats as the service evolves?**  
A: Embed a version identifier in the key prefix: `sk_v2_live_...` vs `sk_v1_live_...`. The validation path extracts the version from the prefix, routes to the version-specific validation logic (hashing algorithm, format validation, scope interpretation). Maintain backward compatibility: continue supporting old format keys for existing customers; route new key creation to the latest format. Version migration: when deprecating an old key format, notify customers and set a sunset date. The migration is gradual: old keys remain valid until individually rotated to the new format.

**Q6: What are the security considerations for storing the raw key in a password manager or vault?**  
A: The platform never stores the raw key after creation. Developers must store it in: (1) A secrets manager (AWS Secrets Manager, HashiCorp Vault, 1Password Teams) — preferred. (2) Environment variables in their deployment environment (not in code). (3) A CI/CD secrets store (GitHub Actions Secrets, GitLab CI Variables). What not to do: hardcode in source code, commit to version control, log in application logs, send over unencrypted channels. The platform can enforce this by educating developers and implementing the secret scanning integration — keys found in source code trigger immediate suspension and an educational notification.

**Q7: How do you handle key migration when acquiring a company or merging API platforms?**  
A: Key migration procedure: (1) Map acquired company's key format to the platform's format (possibly using a translation layer that accepts old-format keys and routes them to the platform's validation service). (2) Create new platform keys for all acquired application key records (mapping `old_key_id → new_key_id`). (3) Operate both systems in parallel for a migration window (90 days). (4) Notify acquired customers to rotate their keys to the new format. (5) After sunset date, deprecate the translation layer. Security consideration: during migration, the acquired company's key database may have weaker security properties (e.g., plaintext storage, weaker entropy). Prioritize migrating all keys with weak entropy to new high-entropy keys.

**Q8: How do you implement per-endpoint scope enforcement at the API gateway?**  
A: Maintain a scope map: `endpoint_scope_map = {"/api/v1/data:GET": "read:data", "/api/v1/records:POST": "write:records"}`. At the API gateway: (1) Look up required scope for the endpoint+method from the map. (2) After key validation, check `required_scope in key.scopes`. (3) If not authorized: return `403 Forbidden` with `WWW-Authenticate: ApiKey error="insufficient_scope", scope="write:records"`. The scope map can be stored in a configuration file loaded at gateway startup, or pulled from a configuration service with periodic refresh. Use a trie data structure for efficient prefix-based scope matching if you have many endpoints.

**Q9: What is the security impact of including the key hash in API error responses?**  
A: Never include the key hash in API error responses. The hash should only be used internally for database lookup. An attacker who sees the hash of their own key gains no useful information (SHA-256 is irreversible). However, including the hash in error responses creates a potential oracle: if an attacker can trigger validation of arbitrary keys, the presence/absence of a hash match could leak information about the key space. Additionally, hashes in responses could inadvertently end up in client logs, monitoring systems, or error tracking services — unnecessarily widening the attack surface. Error responses should only include: generic error code ("INVALID_API_KEY"), HTTP 401 status, and optionally the key prefix the user provided (to help legitimate users debug which key they're sending).

**Q10: How do you design the key management service for a multi-tenant SaaS platform where each tenant has their own key namespace?**  
A: Add `tenant_id` as a top-level partition key in all tables. Key format: `sk_live_<tenant_short_id>_<random_suffix>`. Redis key: `apikey:{tenant_id}:{key_hash}`. Validation includes tenant verification: extract `tenant_short_id` from the key prefix, confirm the request is for the correct tenant's API endpoint (prevent cross-tenant key usage). Each tenant has an isolated key namespace — even if two tenants have identical random suffixes (astronomically unlikely with 256-bit entropy, but theoretically possible without tenant namespace separation), the tenant_id prevents collision. Admin operations: platform admins can view keys across tenants; tenant admins can only view their own tenant's keys (enforced at the API layer with RLS in PostgreSQL).

**Q11: How do you handle key theft from a compromised developer workstation?**  
A: If a developer's machine is compromised, all keys stored on that machine are potentially stolen. Response: (1) Key rotation: developer rotates all keys with `overlap_hours=0` (immediate revocation of old keys, no overlap window) to minimize the damage window. (2) Audit review: examine all recent usage of compromised keys for unauthorized access patterns. (3) Notification to affected downstream users if data was accessed. (4) Device quarantine: the compromised machine's IP is added to a block list while investigation proceeds. Prevention: encourage use of secrets managers that provide short-lived credentials (e.g., AWS IAM roles instead of long-lived keys for cloud services); implement workstation-bound keys (IP allowlisting to known developer IP ranges); periodic forced rotation policies.

**Q12: What is the maximum practical number of API keys per user before performance degrades?**  
A: Validation performance: each validation is a single Redis GET — the number of keys per user is irrelevant to validation latency. Dashboard query performance: listing a user's 20 keys is a PostgreSQL SELECT with user_id index — fast up to thousands of keys. The practical limit is operational (what does the user actually need?). Our policy maximum is 20 keys/user (configurable per tenant). At enterprise scale, team-based key management (100 keys/team) is appropriate. Technical limit: the only database-side concern is the `api_key_usage_daily` table, which grows at `keys_per_user × 365 rows/year`. At 100 keys × 730 days = 73,000 rows per user — PostgreSQL handles this trivially with an index on `(key_id, usage_date)`.

**Q13: How do you implement a "test mode" with test keys that hit a sandbox environment?**  
A: Test keys (`sk_test_` prefix) are validated by the same infrastructure but route requests to a sandbox API backend. Implementation: (1) Key metadata includes `environment=staging`. (2) API gateway routing: if key environment is `staging`, proxy to sandbox endpoint instead of production. (3) Sandbox responses are clearly labeled (e.g., `X-Sandbox: true` response header). (4) Test keys have separate usage quotas and rate limits. (5) Test keys are never usable against production endpoints (enforced in scope + environment check). (6) Sandbox data is isolated from production data (separate database schemas or tenants). Stripe's test/live key separation is the canonical example of this pattern.

**Q14: How would you rate-limit based on API key tiers (free vs. paid vs. enterprise)?**  
A: Store `tier` in the key metadata (and in the Redis cache entry). Rate limits per tier: free=100 RPM, paid=10K RPM, enterprise=100K RPM. Rate limit enforcement in the API gateway: after key validation, apply tier-specific rate limit counter. Upgrade path: when a user upgrades their account, update the key's tier in PostgreSQL and Redis (cache entry update). No new key required. Alert at 80% of tier limit to prompt upgrade. Overage policy: soft limit (allow burst up to 110% with HTTP 429 after threshold) vs. hard limit (strict 429 at limit) — configurable per tier. Store the rate limit parameters in the key's Redis cache entry so the gateway doesn't need a separate tier lookup call.

**Q15: How do you design an API key system for a public API that must be free of vendor lock-in?**  
A: Follow open standards where possible: (1) JWT-based API keys: issue JWTs with platform-specific claims as "API tokens." The JWT format is a standard; the payload schema is yours. Clients can decode the header/payload without proprietary tools. (2) Standardized scopes: use OAuth2 scope conventions (resource:action). (3) Introspection endpoint compatible with RFC 7662 so clients and resource servers can integrate with standard OAuth2 tooling. (4) Well-documented key format regex for secret scanning integrations. (5) Standard HTTP authentication: `Authorization: Bearer` or `X-API-Key` header. (6) Key export: if a customer leaves the platform, they can revoke all their keys (we never had the raw keys anyway, so there's nothing to "export"). The customer re-creates keys in the new platform. Design for portability by not embedding platform-specific semantics in the keys themselves.

---

## 12. References & Further Reading

- **NIST SP 800-63B** — Digital Identity Guidelines: Authentication. https://pages.nist.gov/800-63-3/sp800-63b.html
- **OWASP API Security Top 10** — API2:2023 Broken Authentication. https://owasp.org/API-Security/editions/2023/en/0xa2-broken-authentication/
- **OWASP Cryptographic Storage Cheat Sheet**. https://cheatsheetseries.owasp.org/cheatsheets/Cryptographic_Storage_Cheat_Sheet.html
- **GitHub Secret Scanning Partner Program**. https://docs.github.com/en/code-security/secret-scanning/secret-scanning-partner-program
- **GitGuardian — Secrets Detection**. https://docs.gitguardian.com/
- **Trufflehog — Open Source Secret Scanner**. https://github.com/trufflesecurity/trufflehog
- **Stripe API Keys Documentation** (canonical example of prefix-based key design). https://stripe.com/docs/keys
- **AWS Security Token Service** (reference for short-lived credential issuance patterns). https://docs.aws.amazon.com/STS/latest/APIReference/welcome.html
- **PKCS#11 Standard** — Cryptographic Token Interface Standard (relevant to HSM-backed key generation). https://www.oasis-open.org/committees/pkcs11/
- **RFC 7519** — JSON Web Tokens (relevant if JWT-based API keys are considered). https://www.rfc-editor.org/rfc/rfc7519
- **FIPS 140-3** — Security Requirements for Cryptographic Modules (relevant for regulated industries requiring FIPS-validated CSPRNG). https://csrc.nist.gov/publications/detail/fips/140/3/final
- **Python secrets module** — CSPRNG-backed secrets generation. https://docs.python.org/3/library/secrets.html
- **HaveIBeenPwned API** — Breached credential checking reference for comparison. https://haveibeenpwned.com/API/v3
- **Canary Tokens** — Honeypot token generation reference. https://canarytokens.org/
- **Redis GETDEL command** — Atomic get-and-delete for single-use tokens. https://redis.io/docs/latest/commands/getdel/
