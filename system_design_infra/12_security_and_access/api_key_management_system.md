# System Design: API Key Management System

> **Relevance to role:** A cloud infrastructure platform exposes APIs consumed by internal services, CI/CD pipelines, CLI tools, partner integrations, and automation scripts. API keys are the primary credential for programmatic access. This system handles key generation, scoped permissions, rate limiting per key, usage tracking, rotation, revocation, and leak detection. On a platform with bare-metal IaaS, Kubernetes, job schedulers, and message brokers, API keys control access to infrastructure operations -- creating VMs, scheduling jobs, managing clusters. A leaked key with broad permissions is a direct path to a catastrophic infrastructure compromise.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | Generate API keys using CSPRNG with type-prefix identification (e.g., `plat_live_`, `plat_test_`). |
| FR-2 | Store only the SHA-256 hash of the key; never store plaintext after initial generation. |
| FR-3 | Scoped permissions: each key is bound to a set of allowed actions, resources, and namespaces. |
| FR-4 | Key rotation: generate new key, configurable grace period (dual-key acceptance), revoke old key. |
| FR-5 | Rate limiting per key: configurable requests/second, requests/minute, requests/day. |
| FR-6 | Usage tracking: per-key request count, last used timestamp, error rate. |
| FR-7 | Key revocation with propagation across all API gateway nodes within 5 seconds. |
| FR-8 | Key expiration: optional TTL with auto-revocation on expiry. |
| FR-9 | Leak detection: register key patterns with GitHub secret scanning and internal scanners. |
| FR-10 | Automated leak response: detect leaked key, immediately revoke, notify owner. |
| FR-11 | Key metadata: owner, team, environment, description, creation date. |
| FR-12 | Self-service key management: owners can create, rotate, and revoke their own keys. |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Key validation latency | < 1 ms (cached), < 5 ms (uncached) |
| NFR-2 | Availability | 99.99% (key validation is on the API critical path) |
| NFR-3 | Throughput | 100,000 key validations/sec across all gateway nodes |
| NFR-4 | Revocation propagation | < 5 seconds globally |
| NFR-5 | Key generation latency | < 100 ms |
| NFR-6 | Usage data freshness | < 5 minutes for dashboards |
| NFR-7 | Leak detection response | < 5 minutes from detection to revocation |

### Constraints & Assumptions

- API gateway: Envoy (or Kong/custom gateway) with ext_authz filter.
- Redis cluster for key validation cache and rate limiting.
- MySQL 8.0 for key metadata and audit.
- Elasticsearch 8.x for usage analytics and audit search.
- Kafka for event streaming (revocation propagation, usage aggregation).
- Keys are used by: internal services (Java/Python), CI/CD pipelines, CLI tools, partner integrations.

### Out of Scope

- OAuth2/OIDC token management (different credential type).
- User session management (browser-based auth).
- mTLS client certificates (covered in certificate_lifecycle_management.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Active API keys | 50,000 | 2,000 services x 5 keys avg (prod, staging, CI, partner, backup) + 40,000 pipeline/automation keys |
| Key validations/sec (peak) | 100,000 | All API traffic through gateway |
| Key validations/day | ~4 billion | 100K/sec x 86,400 x 0.5 avg utilization |
| Key creations/day | 500 | New services, rotations, partner onboarding |
| Key revocations/day | 200 | Rotations, decommissions, leak responses |
| Rate limit checks/sec | 100,000 | 1:1 with validations |
| Usage data points/day | ~4 billion | One per validation |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Key validation (cache hit) | < 1 ms p99 |
| Key validation (cache miss) | < 5 ms p99 |
| Rate limit check | < 0.5 ms p99 (Redis) |
| Key creation | < 100 ms p99 |
| Key revocation propagation | < 5 s p99 |
| Usage dashboard query | < 2 s p99 |

### Storage Estimates

| Data | Size | Calculation |
|------|------|-------------|
| Key metadata (MySQL) | ~500 MB | 50,000 keys x 10 KB avg |
| Key validation cache (Redis) | ~500 MB | 50,000 keys x 10 KB (hash + permissions + rate limits) |
| Rate limit counters (Redis) | ~200 MB | 50,000 keys x 4 KB (sliding window counters) |
| Usage data (1 day) | ~200 GB | 4B events x 50 bytes avg (aggregated per minute) |
| Usage data (90-day hot) | ~18 TB | 200 GB x 90 days |
| Audit log (1 day) | ~10 GB | Key lifecycle events (create, rotate, revoke, leak) |

### Bandwidth Estimates

| Flow | Bandwidth |
|------|-----------|
| Key validation requests (gateway to Redis) | ~100 MB/sec (100K req/sec x 1 KB) |
| Revocation events (Redis pub/sub) | < 1 KB/sec (200 revocations/day) |
| Usage event stream (Kafka) | ~5 MB/sec (100K events/sec x 50 bytes, aggregated) |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                    API Consumers                                  │
│                                                                   │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────────┐  │
│  │ Services  │  │ CI/CD    │  │ CLI      │  │ Partner        │  │
│  │ (Java/Py) │  │ Pipelines│  │ Tools    │  │ Integrations   │  │
│  └─────┬─────┘  └────┬─────┘  └────┬─────┘  └───────┬────────┘  │
│        │              │              │                 │           │
│        │  API Key in Header: X-API-Key: plat_live_abc123...      │
│        └──────────────┴──────────────┴─────────────────┘          │
└───────────────────────────────┬───────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────┐
│                    API Gateway Layer (Envoy)                       │
│                                                                   │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  ext_authz Filter                                          │  │
│  │  1. Extract API key from header                            │  │
│  │  2. Hash key (SHA-256)                                     │  │
│  │  3. Lookup hash in Redis cache                             │  │
│  │     - Cache hit: get permissions, rate limits              │  │
│  │     - Cache miss: query Key Validation Service             │  │
│  │  4. Check rate limits (Redis sliding window)               │  │
│  │  5. Check permissions (action + resource match)            │  │
│  │  6. Allow/deny                                             │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                   │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  Rate Limiter (Redis-backed)                               │  │
│  │  - Sliding window: per-key, per-endpoint                   │  │
│  │  - Global rate limit: per-key total                        │  │
│  │  - Burst allowance: token bucket                           │  │
│  └────────────────────────────────────────────────────────────┘  │
└───────────────────────────────┬───────────────────────────────────┘
                                │
              ┌─────────────────┼─────────────────┐
              │                 │                   │
              ▼                 ▼                   ▼
┌─────────────────┐  ┌──────────────────┐  ┌───────────────────┐
│  Redis Cluster   │  │ Key Management   │  │  Usage Collector   │
│                  │  │ Service          │  │                    │
│  - Key cache     │  │                  │  │  - Async usage     │
│    (hash→meta)   │  │  - Key CRUD      │  │    events to Kafka │
│  - Rate limit    │  │  - Rotation      │  │  - Aggregation     │
│    counters      │  │  - Revocation    │  │    (per-key/min)   │
│  - Revocation    │  │  - Permission    │  │                    │
│    pub/sub       │  │    management    │  └────────┬──────────┘
│                  │  │                  │           │
└─────────┬────────┘  └────────┬─────────┘           │
          │                    │                      │
          │           ┌────────▼─────────┐           │
          │           │  MySQL 8.0       │           │
          │           │                  │           │
          │           │  - Key metadata  │           │
          │           │  - Permissions   │           │
          │           │  - Audit trail   │           │
          │           └──────────────────┘           │
          │                                           │
          │           ┌───────────────────────────────▼───────────┐
          │           │  Kafka                                     │
          │           │  - key.revocations (fanout to all gateways)│
          │           │  - key.usage.events (aggregated usage)     │
          │           │  - key.audit (lifecycle events)            │
          └───────────┤                                            │
                      └───────────────────┬───────────────────────┘
                                          │
                      ┌───────────────────▼───────────────────────┐
                      │  Elasticsearch 8.x                         │
                      │  - Usage analytics (per-key dashboards)    │
                      │  - Audit log search                        │
                      │  - Anomaly detection input                 │
                      └────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                    Leak Detection Layer                            │
│                                                                   │
│  ┌────────────────────┐  ┌─────────────────────────────────────┐│
│  │ GitHub Secret       │  │ Internal Scanner                    ││
│  │ Scanning            │  │                                     ││
│  │ (webhook on match)  │  │ - Scans logs, configs, repos        ││
│  │                     │  │ - Regex: plat_(live|test)_[A-Za-z]+ ││
│  └────────┬────────────┘  └──────────────┬──────────────────────┘│
│           │                               │                       │
│           └───────────────┬───────────────┘                       │
│                           ▼                                       │
│           ┌───────────────────────────────┐                      │
│           │  Leak Response Automation      │                      │
│           │  1. Identify leaked key        │                      │
│           │  2. Immediately revoke         │                      │
│           │  3. Notify owner (email+Slack) │                      │
│           │  4. Create incident ticket     │                      │
│           │  5. Generate replacement key   │                      │
│           └───────────────────────────────┘                      │
└──────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **API Gateway (Envoy)** | Extracts API key, performs validation, rate limiting, permission check. On the critical path. |
| **Redis Cluster** | Key validation cache (hash -> metadata), rate limit counters (sliding window), revocation pub/sub (real-time invalidation). |
| **Key Management Service** | CRUD for API keys. Handles generation, rotation, revocation. Writes to MySQL. Publishes events to Kafka. |
| **MySQL 8.0** | Source of truth for key metadata, permissions, audit trail. |
| **Usage Collector** | Async collection of per-key usage metrics. Publishes to Kafka for aggregation. |
| **Kafka** | Event streaming: revocation propagation, usage aggregation, audit events. |
| **Elasticsearch** | Usage analytics, audit search, anomaly detection input. |
| **Leak Detection** | GitHub secret scanning webhook + internal regex scanner. Triggers automated revocation on leak detection. |

### Data Flows

1. **Key validation (happy path):** Request with `X-API-Key: plat_live_abc123...` -> Envoy ext_authz -> SHA-256 hash of key -> Redis lookup (cache hit) -> permissions + rate limit check -> allow.

2. **Key validation (cache miss):** Redis miss -> Key Validation Service queries MySQL -> returns metadata -> populates Redis cache (TTL: 5 min) -> validate.

3. **Key revocation propagation:** Admin revokes key -> Key Management Service marks key revoked in MySQL -> publishes to Kafka `key.revocations` topic -> all gateway nodes consume event -> delete key from Redis cache -> publish Redis pub/sub for immediate local invalidation.

4. **Leak detection:** GitHub secret scanning detects key pattern in public repo -> webhook to Leak Response Service -> service looks up key by prefix + partial match -> revokes key -> notifies owner -> creates incident.

---

## 4. Data Model

### Core Entities & Schema (Full SQL)

```sql
-- ============================================================
-- API KEYS
-- ============================================================
CREATE TABLE api_keys (
    key_id           BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    key_prefix       VARCHAR(32) NOT NULL COMMENT 'e.g., plat_live_abc1 (first 12 chars for identification)',
    key_hash         VARBINARY(32) NOT NULL COMMENT 'SHA-256 hash of the full key',
    key_hint         VARCHAR(8) NOT NULL COMMENT 'Last 4 chars for display: ...xyz9',
    owner_user_id    VARCHAR(255) NOT NULL COMMENT 'Owner email or service account',
    owner_team       VARCHAR(255) NOT NULL,
    environment      ENUM('production', 'staging', 'development', 'ci') NOT NULL,
    key_type         ENUM('service', 'ci_cd', 'partner', 'personal', 'automation') NOT NULL,
    description      VARCHAR(512) NOT NULL,
    status           ENUM('active', 'rotated', 'revoked', 'expired', 'leaked') NOT NULL DEFAULT 'active',
    expires_at       TIMESTAMP NULL DEFAULT NULL COMMENT 'Optional expiration',
    last_used_at     TIMESTAMP NULL DEFAULT NULL,
    last_used_ip     VARCHAR(45) DEFAULT NULL,
    total_requests   BIGINT UNSIGNED NOT NULL DEFAULT 0,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    rotated_at       TIMESTAMP NULL DEFAULT NULL,
    revoked_at       TIMESTAMP NULL DEFAULT NULL,
    revoked_by       VARCHAR(255) DEFAULT NULL,
    revoke_reason    VARCHAR(255) DEFAULT NULL,
    replaced_by_id   BIGINT UNSIGNED DEFAULT NULL COMMENT 'Points to the replacement key after rotation',
    replaces_id      BIGINT UNSIGNED DEFAULT NULL COMMENT 'Points to the key this replaced',
    grace_period_end TIMESTAMP NULL DEFAULT NULL COMMENT 'Old key accepted until this time during rotation',
    metadata_json    JSON DEFAULT NULL COMMENT 'Custom metadata (project, repo, etc.)',
    UNIQUE KEY uk_key_hash (key_hash),
    UNIQUE KEY uk_key_prefix (key_prefix),
    INDEX idx_owner (owner_user_id),
    INDEX idx_team (owner_team),
    INDEX idx_status (status),
    INDEX idx_expires (expires_at),
    INDEX idx_last_used (last_used_at),
    INDEX idx_environment (environment, status),
    FOREIGN KEY (replaced_by_id) REFERENCES api_keys(key_id),
    FOREIGN KEY (replaces_id) REFERENCES api_keys(key_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- API KEY PERMISSIONS (scoped access)
-- ============================================================
CREATE TABLE api_key_permissions (
    permission_id    BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    key_id           BIGINT UNSIGNED NOT NULL,
    resource_type    VARCHAR(128) NOT NULL COMMENT 'e.g., pods, vms, jobs, namespaces, clusters',
    resource_pattern VARCHAR(255) NOT NULL DEFAULT '*' COMMENT 'glob pattern: myapp-*, specific name, or *',
    namespace_pattern VARCHAR(255) NOT NULL DEFAULT '*' COMMENT 'namespace or * for all',
    actions          JSON NOT NULL COMMENT '["get", "list", "create", "delete"]',
    conditions       JSON DEFAULT NULL COMMENT 'e.g., {"labels": {"env": "dev"}, "ip_cidr": ["10.0.0.0/8"]}',
    FOREIGN KEY (key_id) REFERENCES api_keys(key_id) ON DELETE CASCADE,
    INDEX idx_key_resource (key_id, resource_type),
    INDEX idx_resource (resource_type, namespace_pattern)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- RATE LIMIT POLICIES
-- ============================================================
CREATE TABLE rate_limit_policies (
    policy_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    key_id           BIGINT UNSIGNED NOT NULL,
    window_type      ENUM('second', 'minute', 'hour', 'day') NOT NULL,
    max_requests     INT UNSIGNED NOT NULL,
    burst_size       INT UNSIGNED DEFAULT NULL COMMENT 'Token bucket burst allowance',
    endpoint_pattern VARCHAR(255) DEFAULT '*' COMMENT 'Specific endpoint or * for global',
    FOREIGN KEY (key_id) REFERENCES api_keys(key_id) ON DELETE CASCADE,
    UNIQUE KEY uk_key_window_endpoint (key_id, window_type, endpoint_pattern),
    INDEX idx_key (key_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- API KEY USAGE (aggregated per minute)
-- ============================================================
CREATE TABLE api_key_usage (
    usage_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    key_id           BIGINT UNSIGNED NOT NULL,
    window_start     TIMESTAMP NOT NULL COMMENT 'Start of the 1-minute window',
    endpoint         VARCHAR(255) NOT NULL,
    request_count    INT UNSIGNED NOT NULL DEFAULT 0,
    error_count      INT UNSIGNED NOT NULL DEFAULT 0,
    avg_latency_ms   DECIMAL(8,2) DEFAULT NULL,
    p99_latency_ms   DECIMAL(8,2) DEFAULT NULL,
    bytes_transferred BIGINT UNSIGNED NOT NULL DEFAULT 0,
    http_2xx_count   INT UNSIGNED NOT NULL DEFAULT 0,
    http_4xx_count   INT UNSIGNED NOT NULL DEFAULT 0,
    http_5xx_count   INT UNSIGNED NOT NULL DEFAULT 0,
    UNIQUE KEY uk_key_window_endpoint (key_id, window_start, endpoint),
    INDEX idx_key_window (key_id, window_start),
    INDEX idx_window (window_start)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (UNIX_TIMESTAMP(window_start)) (
    -- Daily partitions managed by automation
);

-- ============================================================
-- API KEY AUDIT LOG
-- ============================================================
CREATE TABLE api_key_audit_log (
    audit_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    timestamp_utc    TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    event_type       ENUM('create', 'rotate', 'revoke', 'expire', 'permission_change',
                          'rate_limit_change', 'leak_detected', 'leak_revoked',
                          'grace_period_start', 'grace_period_end') NOT NULL,
    key_id           BIGINT UNSIGNED NOT NULL,
    key_prefix       VARCHAR(32) NOT NULL,
    performed_by     VARCHAR(255) NOT NULL,
    details          JSON DEFAULT NULL COMMENT 'Event-specific details',
    source_ip        VARCHAR(45) DEFAULT NULL,
    request_id       VARCHAR(128) DEFAULT NULL,
    INDEX idx_timestamp (timestamp_utc),
    INDEX idx_key (key_id, timestamp_utc),
    INDEX idx_event (event_type, timestamp_utc),
    INDEX idx_performer (performed_by, timestamp_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (UNIX_TIMESTAMP(timestamp_utc)) (
    -- Daily partitions
);

-- ============================================================
-- LEAK DETECTION EVENTS
-- ============================================================
CREATE TABLE leak_detection_events (
    event_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    detected_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    source           ENUM('github_scanning', 'internal_scanner', 'manual_report', 'honeypot') NOT NULL,
    key_id           BIGINT UNSIGNED DEFAULT NULL,
    key_prefix       VARCHAR(32) NOT NULL,
    leak_location    VARCHAR(1024) NOT NULL COMMENT 'URL of repo, log file, etc.',
    auto_revoked     BOOLEAN NOT NULL DEFAULT FALSE,
    revoked_at       TIMESTAMP DEFAULT NULL,
    owner_notified   BOOLEAN NOT NULL DEFAULT FALSE,
    notified_at      TIMESTAMP DEFAULT NULL,
    incident_id      VARCHAR(128) DEFAULT NULL COMMENT 'Link to incident ticket',
    resolved_at      TIMESTAMP DEFAULT NULL,
    FOREIGN KEY (key_id) REFERENCES api_keys(key_id),
    INDEX idx_detected (detected_at),
    INDEX idx_key (key_id),
    INDEX idx_source (source, detected_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Store | Use Case | Justification |
|-------|----------|---------------|
| **MySQL 8.0** | Key metadata, permissions, audit log, leak events | ACID for key lifecycle operations. Source of truth. |
| **Redis Cluster** | Key validation cache, rate limiting, revocation pub/sub | Sub-millisecond reads. Sliding window counters. Pub/sub for cross-node invalidation. |
| **Kafka** | Event streaming (revocation, usage, audit) | Durable ordered delivery. Decouples gateway from downstream processors. |
| **Elasticsearch 8.x** | Usage analytics, audit search | Full-text search, aggregations for dashboards. |

### Indexing Strategy

| Table | Key Index | Purpose |
|-------|-----------|---------|
| `api_keys` | `(key_hash)` UNIQUE | Primary validation lookup. |
| `api_keys` | `(key_prefix)` UNIQUE | Identification without full key. |
| `api_keys` | `(owner_user_id)` | "List my keys." |
| `api_keys` | `(status, expires_at)` | Find expiring keys. |
| `api_key_permissions` | `(key_id, resource_type)` | Permission check during validation. |
| `api_key_usage` | PARTITION by day + `(key_id, window_start)` | Usage queries per key. |

---

## 5. API Design

### REST Endpoints

```
# ── Key Management ───────────────────────────────────────────
POST   /api/v1/keys
  Body: {
    "name": "myapp-prod-key",
    "environment": "production",
    "key_type": "service",
    "description": "Production API key for myapp backend",
    "owner_team": "platform-core",
    "permissions": [
      {
        "resource_type": "pods",
        "namespace_pattern": "myapp-*",
        "actions": ["get", "list", "create", "delete"]
      },
      {
        "resource_type": "jobs",
        "namespace_pattern": "myapp-batch",
        "actions": ["get", "list", "create"]
      }
    ],
    "rate_limits": [
      {"window_type": "second", "max_requests": 100},
      {"window_type": "day", "max_requests": 1000000}
    ],
    "expires_in": "90d",
    "ip_allowlist": ["10.0.0.0/8"]
  }
  Response: {
    "key_id": 12345,
    "api_key": "plat_live_aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789abcdef",
    "key_prefix": "plat_live_aBcD",
    "key_hint": "...cdef",
    "expires_at": "2026-07-08T00:00:00Z",
    "message": "Store this key securely. It will not be shown again."
  }

GET    /api/v1/keys                          # List keys (filter: owner, team, env, status)
GET    /api/v1/keys/{key_id}                 # Get key metadata (never returns the key itself)
PUT    /api/v1/keys/{key_id}                 # Update metadata/description
DELETE /api/v1/keys/{key_id}                 # Revoke key

# ── Key Rotation ─────────────────────────────────────────────
POST   /api/v1/keys/{key_id}/rotate
  Body: {"grace_period": "24h"}
  Response: {
    "new_key_id": 12346,
    "api_key": "plat_live_xYzAbCdEfGhIjKlMnOpQrStUvWxYz0123456789",
    "old_key_valid_until": "2026-04-10T15:00:00Z",
    "message": "Old key will be accepted during grace period."
  }

POST   /api/v1/keys/{key_id}/revoke
  Body: {"reason": "Detected in public repository", "immediate": true}

# ── Permissions ──────────────────────────────────────────────
GET    /api/v1/keys/{key_id}/permissions     # List permissions for key
POST   /api/v1/keys/{key_id}/permissions     # Add permission
DELETE /api/v1/keys/{key_id}/permissions/{perm_id}  # Remove permission

# ── Rate Limits ──────────────────────────────────────────────
GET    /api/v1/keys/{key_id}/rate-limits     # Get rate limit policy
PUT    /api/v1/keys/{key_id}/rate-limits     # Update rate limits

# ── Usage ────────────────────────────────────────────────────
GET    /api/v1/keys/{key_id}/usage           # Get usage stats
  Query: ?from=2026-04-01&to=2026-04-09&granularity=hour
GET    /api/v1/keys/{key_id}/usage/top-endpoints  # Top endpoints by request count

# ── Validation (internal, called by gateway) ────────────────
POST   /api/v1/internal/validate
  Body: {"key_hash": "<sha256>", "action": "create", "resource_type": "pods", "namespace": "myapp"}
  Response: {
    "valid": true,
    "key_id": 12345,
    "permissions_match": true,
    "rate_limit_remaining": {"second": 95, "day": 999500},
    "owner_team": "platform-core"
  }

# ── Leak Detection ───────────────────────────────────────────
POST   /api/v1/leak-detection/github-webhook  # GitHub secret scanning webhook
POST   /api/v1/leak-detection/report          # Manual leak report
GET    /api/v1/leak-detection/events          # List leak detection events

# ── Health ───────────────────────────────────────────────────
GET    /api/v1/keys/health                    # System health (Redis, MySQL, Kafka)
GET    /api/v1/keys/metrics                   # Prometheus metrics
```

### CLI

```bash
# ── Key Management ──
platform-key create --name myapp-prod-key --env production --type service \
  --team platform-core \
  --permission "pods:myapp-*:get,list,create,delete" \
  --permission "jobs:myapp-batch:get,list,create" \
  --rate-limit "100/s,1000000/d" \
  --expires 90d \
  --ip-allowlist "10.0.0.0/8"

platform-key list --team platform-core --env production --status active
platform-key describe --key-id 12345
platform-key describe --prefix plat_live_aBcD

# ── Key Rotation ──
platform-key rotate --key-id 12345 --grace-period 24h
platform-key revoke --key-id 12345 --reason "Decommissioned service"
platform-key revoke --prefix plat_live_aBcD --reason "Leaked" --immediate

# ── Permissions ──
platform-key add-permission --key-id 12345 \
  --resource namespaces --namespace "myapp-*" --actions "get,list"
platform-key remove-permission --key-id 12345 --permission-id 678

# ── Usage ──
platform-key usage --key-id 12345 --last 7d --granularity hour
platform-key usage --team platform-core --last 30d

# ── Validation (debug) ──
platform-key validate --key "plat_live_aBcD..." \
  --action create --resource pods --namespace myapp

# ── Leak Detection ──
platform-key scan --repo https://github.com/corp/myapp
platform-key leak-events --last 30d

# ── Audit ──
platform-key audit --key-id 12345 --last 90d
platform-key audit --team platform-core --event-type revoke --last 30d
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Key Generation and Storage Security

**Why it's hard:**
API keys must be unpredictable (CSPRNG), efficiently validatable (fast hash lookup), identifiable without exposure (prefix system), and never stored in plaintext. The prefix must encode enough information for triage (environment, type) without being predictable. The hash must be resistant to rainbow table attacks (though API keys are long enough that this is less of a concern than passwords).

| Approach | Security | Identifiability | Validation Speed |
|----------|----------|----------------|-----------------|
| **UUID v4** | Good (122 bits entropy) | None (opaque) | Fast (hash lookup) |
| **Prefixed random (GitHub-style)** | Good (configurable entropy) | High (prefix = type) | Fast (hash lookup) |
| **HMAC-based (key = HMAC(secret, id))** | Good (if secret is secure) | By ID | Fast (recompute HMAC) |
| **JWT-based API keys** | Medium (self-contained) | High (claims visible) | No DB lookup (signature verify) |

**Selected: Prefixed random token with SHA-256 hash storage (GitHub-style)**

**Justification:** Prefixed tokens (like `ghp_xxxx` for GitHub, `sk_live_xxxx` for Stripe) are the industry best practice. The prefix identifies the key type for triage (leak detection, routing). The random body provides entropy. SHA-256 hash storage means a database breach doesn't expose keys.

**Implementation Detail:**

```
Key Format:
──────────
plat_live_aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789abcdef
├──┤ ├──┤ ├──────────────────────────────────────────┤
 │    │    └── Random body: 40 chars, base62 (A-Za-z0-9)
 │    │        Entropy: 40 * log2(62) ≈ 238 bits
 │    │
 │    └── Environment: live, test, ci, stg
 │
 └── Platform prefix: plat

Total key length: ~50 chars
Prefix for identification: first 12 chars (plat_live_aBcD)
Hint for display: last 4 chars (...cdef)

Key Generation (Python):
───────────────────────
import secrets
import hashlib
import string

ALPHABET = string.ascii_letters + string.digits  # base62

def generate_api_key(environment: str) -> tuple[str, bytes, str, str]:
    """Returns (full_key, key_hash, prefix, hint)"""
    prefix = f"plat_{environment}_"
    random_body = ''.join(secrets.choice(ALPHABET) for _ in range(40))
    full_key = prefix + random_body
    
    key_hash = hashlib.sha256(full_key.encode('utf-8')).digest()
    key_prefix = full_key[:12]
    key_hint = full_key[-4:]
    
    return full_key, key_hash, key_prefix, key_hint

Key Generation (Java):
─────────────────────
public record ApiKeyResult(String fullKey, byte[] keyHash, String prefix, String hint) {}

public ApiKeyResult generateApiKey(String environment) {
    SecureRandom csprng = new SecureRandom();
    String alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    StringBuilder body = new StringBuilder(40);
    for (int i = 0; i < 40; i++) {
        body.append(alphabet.charAt(csprng.nextInt(alphabet.length())));
    }
    String fullKey = "plat_" + environment + "_" + body;
    byte[] keyHash = MessageDigest.getInstance("SHA-256")
                                  .digest(fullKey.getBytes(StandardCharsets.UTF_8));
    return new ApiKeyResult(fullKey, keyHash, fullKey.substring(0, 12), fullKey.substring(fullKey.length() - 4));
}

Storage:
───────
- Full key: shown to user ONCE at creation, then discarded. Never stored.
- SHA-256 hash: stored in MySQL (key_hash column, VARBINARY(32)).
- Prefix (12 chars): stored for identification (leak detection, admin lookup).
- Hint (4 chars): stored for display in UI ("...cdef").

Validation Flow:
───────────────
1. Extract key from request header
2. SHA-256(key) → hash
3. Redis GET key_cache:{hex(hash)} → metadata (cache hit)
   OR MySQL SELECT * FROM api_keys WHERE key_hash = ? (cache miss)
4. Check status = 'active'
5. Check permissions match request
6. Check rate limits
7. Allow/deny
```

**Why not bcrypt/scrypt for the hash?**
Unlike passwords, API keys have 238 bits of entropy. Brute-force is infeasible even with SHA-256. bcrypt/scrypt would add 100ms+ per validation -- unacceptable on the critical path at 100K validations/sec.

**Failure Modes:**
- **CSPRNG failure:** If the system's entropy pool is depleted, `SecureRandom` blocks on Linux. Monitor `/proc/sys/kernel/random/entropy_avail`. Use `getrandom()` syscall which blocks until sufficient entropy.
- **Hash collision:** SHA-256 collision probability for 50,000 keys is astronomically low (~10^-68). Not a practical concern.
- **Key displayed but not stored (creation failure):** Use a transaction: generate key -> store hash in MySQL -> return key to user. If MySQL write fails, the key is never returned. Idempotency key prevents double-creation.

**Interviewer Q&As:**

**Q1: Why use a prefix on the API key instead of a completely random string?**
A: (1) Leak detection: GitHub secret scanning (and internal scanners) can match the regex `plat_(live|test|ci|stg)_[A-Za-z0-9]{40}` to detect leaked keys in code, logs, or public repos. Without a prefix, you'd need to hash every string and check the database. (2) Triage: when a key appears in a log, the prefix immediately tells you the environment and type. (3) Routing: the prefix can route the validation to the correct datacenter or key store.

**Q2: Why SHA-256 instead of bcrypt for hashing API keys?**
A: API keys have ~238 bits of entropy, making brute-force infeasible regardless of hash function. bcrypt's purpose is to slow down brute-force of low-entropy passwords. At 100K validations/sec, bcrypt (100ms/hash) would require 10,000 CPU cores just for hashing. SHA-256 is < 0.01ms and sufficient for high-entropy keys.

**Q3: What happens if the key generation endpoint is called concurrently?**
A: Each key generation uses CSPRNG independently -- no shared state. The UNIQUE constraint on `key_hash` in MySQL prevents duplicate keys (astronomically unlikely but guarded). Each request gets a unique key.

**Q4: How do you handle key display security (showing the key only once)?**
A: The full key is returned in the API response body (HTTPS) and displayed in the CLI output with a warning: "Store this key securely. It will not be shown again." The key is never logged (request/response logging excludes the `api_key` field). The audit log records the creation event with the key_prefix only.

**Q5: How do you handle key migration if you need to change the hash algorithm?**
A: Add a `hash_algorithm` column to `api_keys`. New keys use the new algorithm. Old keys remain with SHA-256. Validation code checks the algorithm column and uses the correct hash function. Over time, as old keys are rotated, all keys migrate to the new algorithm.

**Q6: What is the entropy calculation, and is 238 bits sufficient?**
A: base62 alphabet (62 chars), 40 random characters: entropy = 40 * log2(62) = 40 * 5.954 = 238.2 bits. For comparison, a 128-bit key would take 2^128 / (10^12 hashes/sec) = 10^16 years to brute-force. 238 bits is astronomically secure.

---

### Deep Dive 2: Revocation Propagation

**Why it's hard:**
When a key is revoked (especially due to a leak), it must be rejected by ALL API gateway nodes within seconds. The system has 50+ gateway nodes, each with a local Redis cache. Cache invalidation must be global, reliable, and fast. A 5-second delay means 500,000 requests could use a revoked key.

| Approach | Propagation Speed | Reliability | Complexity |
|----------|------------------|-------------|-----------|
| **Short cache TTL only** | Up to TTL (5 min) | High | Low |
| **Redis pub/sub** | < 1 second | Medium (messages lost if subscriber disconnects) | Low |
| **Kafka consumer per node** | < 2 seconds | High (durable) | Medium |
| **Redis pub/sub + Kafka (dual)** | < 1 second + guaranteed delivery | Highest | Medium |
| **Poll MySQL for revocations** | Up to poll interval (5-60s) | High | Low |

**Selected: Redis pub/sub (fast path) + Kafka consumer (reliable path)**

**Justification:** Redis pub/sub gives sub-second propagation for connected nodes. Kafka provides guaranteed delivery if a node misses the pub/sub message (was restarting, network blip). The dual approach ensures both speed and reliability.

**Implementation Detail:**

```
Revocation Propagation Flow:
────────────────────────────

1. Admin revokes key (or leak detector auto-revokes)
   │
   ▼
2. Key Management Service:
   - UPDATE api_keys SET status = 'revoked', revoked_at = NOW() WHERE key_id = 12345
   - Publish to Kafka topic: key.revocations
     {"key_id": 12345, "key_hash": "hex...", "reason": "leaked", "timestamp": "..."}
   - Publish to Redis pub/sub channel: key:revocations
     {"key_hash": "hex...", "action": "revoke"}
   │
   ▼
3. Gateway Nodes (50+ nodes):

   Fast path (Redis pub/sub):
   ┌─────────────────────────────────────────────┐
   │  Redis Pub/Sub Subscriber (per gateway)      │
   │  Channel: key:revocations                    │
   │                                              │
   │  On message:                                 │
   │    DEL key_cache:{key_hash}                  │
   │    Add to local blacklist (in-memory set)    │
   │                                              │
   │  Delivery: best-effort, < 1 second           │
   └─────────────────────────────────────────────┘

   Reliable path (Kafka consumer):
   ┌─────────────────────────────────────────────┐
   │  Kafka Consumer (per gateway, consumer group)│
   │  Topic: key.revocations                      │
   │                                              │
   │  On event:                                   │
   │    DEL key_cache:{key_hash}                  │
   │    Add to local blacklist                    │
   │    ACK offset                                │
   │                                              │
   │  Delivery: guaranteed, < 2 seconds           │
   └─────────────────────────────────────────────┘

   Periodic sync (defense-in-depth):
   ┌─────────────────────────────────────────────┐
   │  Every 60 seconds:                           │
   │    Query MySQL for revocations in last 5 min │
   │    Compare against local blacklist           │
   │    Add any missing revocations               │
   └─────────────────────────────────────────────┘

4. Validation check (updated):
   IF key_hash in local_blacklist → DENY (instant)
   ELSE Redis GET key_cache:{key_hash}
     - If found and status = 'revoked' → DENY
     - If found and status = 'active' → continue validation
     - If not found → query MySQL (may be revoked there)
```

**Grace period for key rotation:**
```
Key Rotation with Grace Period:
──────────────────────────────
T=0:  POST /api/v1/keys/{old_key_id}/rotate?grace_period=24h
      - New key generated (key_id: 12346)
      - Old key status: 'active', grace_period_end: T+24h
      - New key status: 'active'
      - BOTH keys accepted during grace period

T=0 to T+24h:  Grace period
      - Old key → validated, response includes header:
        X-API-Key-Deprecated: true
        X-API-Key-Expires: 2026-04-10T15:00:00Z
      - New key → validated normally

T+24h:  Grace period ends
      - Old key status → 'rotated'
      - Redis cache for old key → deleted
      - Only new key accepted
```

**Failure Modes:**
- **Redis pub/sub message lost:** Kafka consumer picks up the revocation within 2 seconds. Periodic MySQL sync catches it within 60 seconds.
- **Kafka consumer lag:** If lag exceeds 5 seconds, alert fires. Gateway continues checking Redis (which may still have stale cache). Periodic MySQL sync is the safety net.
- **Redis cluster down:** Validation falls back to MySQL (slower, ~5 ms). Rate limiting degrades (cannot enforce per-second limits without Redis). Revocation still propagates via Kafka.
- **Network partition isolating a gateway node:** The isolated node cannot receive pub/sub or Kafka. Its Redis cache continues serving with TTL-based expiry (5 min). After TTL, it queries MySQL directly. Worst case: 5-minute delay for revocation on partitioned nodes.

**Interviewer Q&As:**

**Q1: Why both Redis pub/sub AND Kafka? Isn't that redundant?**
A: Redis pub/sub is fire-and-forget -- if a subscriber is disconnected, it misses the message. Kafka provides guaranteed delivery with consumer offsets. For security-critical revocations (leaked key), we need both speed (pub/sub: < 1s) and guarantee (Kafka: no message loss). The overlap is intentional for defense-in-depth.

**Q2: What if the revocation needs to propagate across multiple regions?**
A: Kafka MirrorMaker replicates the `key.revocations` topic to all regions. Redis pub/sub is per-region (not cross-region). The Kafka path handles cross-region propagation with ~100-500 ms additional latency (inter-region network). For immediate cross-region revocation, we also add the revoked hash to a global Redis key (`global:revoked_keys`) replicated via Redis cross-datacenter replication.

**Q3: How do you handle the case where a key is revoked while a long-running request is in progress?**
A: The revocation takes effect for new requests only. An already-authenticated request continues to completion. This is acceptable because: (1) individual requests are short (< 1s). (2) For long-running operations (batch job submissions), the key was valid at authorization time. (3) For streaming connections, we enforce periodic re-validation (every 5 minutes).

**Q4: How do you prevent cache inconsistency during rotation grace period?**
A: Both old and new keys exist in the Redis cache with their respective status. The old key's cache entry includes `grace_period_end`. Validation logic checks: if `status = 'active' AND (grace_period_end IS NULL OR grace_period_end > now())` then accept. When grace period ends, a scheduled job updates MySQL and publishes a revocation for the old key.

**Q5: What is the maximum revocation delay in the worst case?**
A: Worst case (Redis pub/sub lost + Kafka consumer lagging + MySQL sync hasn't run): 60 seconds (next MySQL sync cycle). In practice: < 5 seconds for 99.9% of revocations (Kafka delivery). We measure and alert on revocation propagation time end-to-end.

**Q6: How do you verify that all nodes received the revocation?**
A: After publishing a revocation, the Key Management Service queries a sample of gateway nodes' `/internal/validate` endpoint with the revoked key hash. If any node still accepts it after 10 seconds, an alert fires. This is a continuous verification mechanism, not a blocking check.

---

### Deep Dive 3: Rate Limiting Per Key

**Why it's hard:**
Rate limiting must be enforced at 100K requests/sec across 50+ gateway nodes. Per-key limits must be globally consistent (a key with 100 req/sec limit should not get 100 req/sec * 50 nodes = 5,000 req/sec). The limiter must add < 0.5 ms latency per request.

| Approach | Global Consistency | Latency | Accuracy |
|----------|-------------------|---------|----------|
| **Local counter per node** | None (key gets N * node_count limit) | < 0.01 ms | Low |
| **Redis central counter** | Perfect | ~0.5 ms (network hop) | High |
| **Redis + local token bucket** | Good (eventual consistency) | < 0.1 ms (local check) | Medium |
| **Sliding window log (Redis)** | Perfect | ~0.5 ms | Highest |

**Selected: Redis sliding window counter with local token bucket cache**

**Justification:** Redis sliding window provides accurate global rate limiting. A local token bucket (refilled from Redis every 100ms) reduces Redis round trips by 10x. The local bucket accepts 90% of requests without hitting Redis; only refill and limit-exceeded checks go to Redis.

**Implementation Detail:**

```
Rate Limiting Architecture:
──────────────────────────

Per-gateway node:
┌───────────────────────────────────────────────────────┐
│  Local Token Bucket (in-memory, per key)               │
│                                                        │
│  key_hash → {tokens_remaining: 8, last_refill: T-50ms}│
│                                                        │
│  On request:                                           │
│    IF tokens_remaining > 0:                            │
│      tokens_remaining -= 1                             │
│      ALLOW (no Redis call)                             │
│    ELSE:                                               │
│      Call Redis to check global counter                │
│      IF global counter < limit:                        │
│        Refill local bucket                             │
│        ALLOW                                           │
│      ELSE:                                             │
│        DENY (429 Too Many Requests)                    │
│                                                        │
│  Every 100ms: batch-sync with Redis                    │
│    Report consumed tokens                              │
│    Receive new allocation                              │
└───────────────────────────────────────────────────────┘

Redis Sliding Window Counter:
────────────────────────────
Key: rate_limit:{key_hash}:{window}
Value: sorted set (timestamp → request_id)

Algorithm (per-second window):
  MULTI
    ZREMRANGEBYSCORE rate_limit:{hash}:sec 0 (now - 1000ms)   # Remove old entries
    ZADD rate_limit:{hash}:sec now request_id                  # Add current
    ZCARD rate_limit:{hash}:sec                                # Count in window
    EXPIRE rate_limit:{hash}:sec 2                             # TTL safety
  EXEC

  IF count > max_requests_per_second → DENY

Optimized: Fixed Window Counter (simpler, slight inaccuracy at window boundary):
  INCR rate_limit:{hash}:{current_second}
  EXPIRE rate_limit:{hash}:{current_second} 2
  IF count > limit → DENY

Rate Limit Response Headers:
───────────────────────────
HTTP/1.1 200 OK
X-RateLimit-Limit: 100
X-RateLimit-Remaining: 95
X-RateLimit-Reset: 1712678460
X-RateLimit-Policy: "100/s, 1000000/d"

HTTP/1.1 429 Too Many Requests
Retry-After: 1
X-RateLimit-Limit: 100
X-RateLimit-Remaining: 0
X-RateLimit-Reset: 1712678460
```

**Multiple rate limit windows:**
```
Per key, enforce multiple windows simultaneously:
  - 100 requests/second  (burst protection)
  - 5,000 requests/minute (sustained load protection)
  - 1,000,000 requests/day (quota management)

Each window has its own Redis counter.
Request is denied if ANY window is exceeded.
```

**Failure Modes:**
- **Redis down:** Fall back to local-only token bucket. Each node enforces `limit / num_nodes` (approximate). Less accurate but prevents total bypass. Alert immediately.
- **Clock skew between nodes:** Sliding window uses Redis server time (`TIME` command), not local time. All nodes use the same reference.
- **Hot key (one key gets disproportionate traffic):** Redis pipeline batching. If a single key exceeds 10K req/sec, shard its counter across multiple Redis keys.

**Interviewer Q&As:**

**Q1: How do you handle rate limiting for keys that are used across multiple services?**
A: The rate limit is per-key globally, not per-service. If key X has a 100/sec limit, all services using key X share that 100/sec. If services need separate limits, they should use separate keys. We also support per-endpoint rate limits: `"endpoint_pattern": "/api/v1/pods/*"` can have a different limit than the global key limit.

**Q2: What is the accuracy trade-off with the local token bucket?**
A: The local bucket can allow up to `bucket_size * num_nodes` requests in a burst before the global limit kicks in. For a 100/sec limit with bucket_size=10 and 50 nodes, a worst-case burst is 500 requests in a short window. This is acceptable because: (1) the window quickly closes when nodes sync with Redis, (2) for strict limits, we can disable the local bucket and always check Redis.

**Q3: How do you implement rate limit changes without restarting gateways?**
A: Rate limit policies are cached in Redis alongside the key metadata. When an admin changes a rate limit, the Key Management Service updates MySQL and publishes a cache invalidation event. The next validation request for that key re-fetches the updated policy from MySQL into Redis.

**Q4: How do you handle rate limiting for partner keys with different SLAs?**
A: Partner keys have their own `rate_limit_policies` rows. We support tiered rate limits: Bronze (100/sec), Silver (1,000/sec), Gold (10,000/sec). The tier is encoded in the key metadata and the corresponding rate limit policy is enforced. Partners can view their usage and remaining quota via a self-service dashboard.

**Q5: What happens when a rate-limited request is denied?**
A: HTTP 429 response with `Retry-After` header indicating when the client should retry. Response includes `X-RateLimit-*` headers showing current limit, remaining count, and reset time. For CI/CD pipelines, we recommend implementing exponential backoff on 429 responses.

**Q6: How do you prevent abuse where someone creates many keys to bypass rate limits?**
A: (1) Per-account key creation limit (max 50 active keys per team). (2) Global rate limit per source IP (in addition to per-key). (3) Per-team aggregate rate limit (all keys from a team share a team-wide quota). (4) Anomaly detection: alert on team creating unusually many keys.

---

### Deep Dive 4: Leak Detection and Automated Response

**Why it's hard:**
API keys can appear in public Git repos, log files, Slack messages, CI/CD configs, or Stack Overflow posts. Detection must be fast (minutes, not hours) and the response automated (human response is too slow for a leaked production key). False positives must be minimized (revoking a key that wasn't actually leaked causes an outage).

| Approach | Coverage | Speed | False Positive Rate |
|----------|----------|-------|-------------------|
| **GitHub Secret Scanning only** | Public repos on GitHub | < 5 min | Very Low (exact match) |
| **Internal regex scanner (logs, configs)** | Internal systems | Continuous | Medium (prefix match) |
| **Honeypot keys** | Active attacker detection | Real-time (on use) | Zero |
| **External scanning (Shodan, Pastebin)** | Public internet | Hours | High |

**Selected: All four approaches combined**

**Justification:** Defense in depth. GitHub secret scanning catches public repo leaks. Internal scanner catches leaks in logs, configs, internal repos. Honeypot keys detect active exploitation. External scanning catches leaks outside GitHub.

**Implementation Detail:**

```
GitHub Secret Scanning Integration:
────────────────────────────────────
1. Register custom pattern with GitHub:
   POST /repos/{org}/{repo}/secret-scanning/custom-patterns
   {
     "name": "Platform API Key",
     "pattern": "plat_(live|test|ci|stg)_[A-Za-z0-9]{38,42}",
     "before_secret": "",
     "after_secret": ""
   }

2. GitHub scans all commits, PRs, issues for matches
3. On match, GitHub sends webhook:
   POST https://platform.corp.com/api/v1/leak-detection/github-webhook
   {
     "type": "secret_scanning_alert",
     "action": "created",
     "alert": {
       "secret_type": "platform_api_key",
       "secret": "plat_live_aBcD...",  # full key or partial
       "repository": "corp/myapp",
       "html_url": "https://github.com/corp/myapp/blob/main/config.yaml#L42"
     }
   }

Automated Response Flow:
───────────────────────
┌────────────────────────────────────────────────────────────┐
│  Leak Detection Service                                     │
│                                                             │
│  1. Receive webhook/scanner alert                           │
│  2. Extract key (or prefix if partial)                      │
│  3. Lookup key in MySQL:                                    │
│     - Full key: SHA-256 hash → exact match                  │
│     - Prefix only: prefix match                             │
│  4. Verify it's a real active key (not test data)           │
│  5. Determine severity:                                     │
│     - Production key → CRITICAL → auto-revoke immediately   │
│     - Test key → HIGH → auto-revoke + notify                │
│     - Already revoked/expired → LOW → notify only           │
│  6. Auto-revoke:                                            │
│     - Update status = 'leaked' in MySQL                     │
│     - Publish revocation to Kafka + Redis pub/sub           │
│     - Create replacement key with same permissions          │
│  7. Notify:                                                 │
│     - Slack: @owner + #security-alerts channel              │
│     - Email: owner + team lead + security team              │
│     - PagerDuty: if production key                          │
│  8. Create incident ticket:                                 │
│     - Attach: leak location, key usage history,             │
│       timestamp, automated actions taken                    │
│  9. Log leak event in MySQL                                 │
└────────────────────────────────────────────────────────────┘

Internal Scanner:
────────────────
Runs continuously, scanning:
  - Internal Git repos (GitLab/GitHub Enterprise)
  - CI/CD configs (Jenkins, GitLab CI, GitHub Actions)
  - Application logs (via Elasticsearch query)
  - Kubernetes ConfigMaps and environment variables
  - Slack messages (via Slack API audit)
  - Confluence/Wiki pages

Regex: plat_(live|test|ci|stg)_[A-Za-z0-9]{38,42}

Honeypot Keys:
─────────────
- Generate "canary" API keys with no permissions
- Place them in tempting locations:
  - Comments in public repos: # API_KEY=plat_live_HONEYPOT123...
  - config.example files
  - Internal wikis
- Any request using a honeypot key triggers:
  - Immediate alert with source IP
  - Log the full request for forensics
  - No actual access granted (key has zero permissions)
```

**Failure Modes:**
- **GitHub webhook delayed:** GitHub's SLA is < 5 minutes. If webhook is delayed, the internal scanner provides backup coverage for GitHub Enterprise repos.
- **False positive (non-key string matches regex):** Lookup in MySQL. If the hash doesn't match any active key, it's a false positive. Log and ignore. The regex is specific enough (prefix + exact length) to minimize false positives.
- **Auto-revocation of wrong key:** The system always matches by hash, not by partial string. Risk is near-zero. For prefix-only matches, require human confirmation before revocation.
- **Leak detection service down:** Webhooks queue at the load balancer. Internal scanner has its own schedule. Honeypot keys continue to work (they alert on use, not on detection).

**Interviewer Q&As:**

**Q1: How does GitHub Secret Scanning work for custom patterns?**
A: GitHub allows organizations to register custom regex patterns. GitHub scans every push, PR, and issue body against registered patterns. On match, it creates a "secret scanning alert" and sends a webhook to a configured endpoint. GitHub can also flag the alert in the repository UI, allowing developers to see and remediate directly.

**Q2: What if the leaked key has already been used by an attacker?**
A: (1) The audit log shows all requests made with that key since the leak was likely introduced (check the Git commit timestamp vs. key usage). (2) Security team reviews the actions taken with the key to assess damage. (3) Any resources created/modified with the leaked key are flagged for review. (4) If the key had write access, a deeper investigation is triggered.

**Q3: How do you prevent developers from accidentally committing API keys?**
A: Prevention layers: (1) Pre-commit hooks (git-secrets, detect-secrets) that block commits containing key patterns. (2) CI pipeline check that scans PRs for secrets before merge. (3) Education: developer onboarding includes secrets hygiene training. (4) Vault Agent or environment variable injection so keys are never in code.

**Q4: How do honeypot keys help detect attacks?**
A: If an attacker breaches a system and finds an API key, they'll likely try to use it. A honeypot key has zero permissions but is monitored. Any request using it is an indicator of compromise. The request's source IP, headers, and payload are captured for forensics. This provides early warning before the attacker finds a real key.

**Q5: What is the end-to-end time from leak to revocation?**
A: GitHub secret scanning: detection < 5 minutes + webhook processing < 10 seconds + revocation propagation < 5 seconds = **total < 5.5 minutes**. Internal scanner: scan interval (continuous for high-priority sources) + processing + propagation = **< 10 minutes**. Honeypot: real-time (detected on use) + propagation = **< 10 seconds**.

**Q6: How do you handle the replacement key distribution after a leak revocation?**
A: The automated response generates a replacement key with identical permissions and notifies the owner with the new key via a secure channel (Vault KV, not email/Slack). The owner then updates their service configuration. For services using Vault Secrets Operator, the replacement key is automatically synced to the k8s Secret -- zero manual intervention.

---

## 7. Scaling Strategy

**Key validation scaling:**
- Redis cluster with 6+ nodes (3 primary, 3 replica). Each primary handles ~50K reads/sec.
- Gateway nodes are stateless; scale horizontally. 50+ nodes handle 100K validations/sec.
- Local token bucket cache absorbs 80%+ of rate limit checks.

**Key metadata scaling:**
- MySQL read replicas for key lookups (99.9% reads).
- Write path is low volume (~700 writes/day for creates + rotations + revocations).

**Usage data scaling:**
- Usage events aggregated per minute per key per endpoint before writing to Elasticsearch.
- Kafka acts as buffer between gateways and Elasticsearch.
- Elasticsearch scales with data nodes; ILM for hot/warm/cold.

**Interviewer Q&As:**

**Q1: What is the bottleneck in this system?**
A: Redis. At 100K validations/sec, each validation requires 1-2 Redis operations (cache lookup + rate limit check). Redis handles ~100K ops/sec per primary node. With 3 primaries and key-based sharding, we get ~300K ops/sec capacity. At peak, we use ~200K ops/sec (66% utilization). Adding more Redis primaries scales linearly.

**Q2: How do you handle a Redis cluster failure?**
A: (1) Fall back to MySQL for key validation (~5 ms latency increase). (2) Rate limiting degrades to per-node local limits. (3) Redis Sentinel triggers automatic failover for individual node failures. (4) Alert fires for manual investigation. (5) If full cluster is down, API latency increases but availability is maintained.

**Q3: How do you handle API key validation in a multi-region setup?**
A: Each region has its own Redis cluster. Key metadata is replicated from the primary MySQL to regional read replicas. Revocation events are cross-region via Kafka MirrorMaker. Key creation can happen in any region (writes go to primary MySQL).

**Q4: What happens during a mass key rotation (e.g., security policy requires all keys rotated)?**
A: Orchestrated rotation: (1) Generate new keys in batch (1,000/minute to avoid overwhelming MySQL). (2) Old keys enter grace period (24h). (3) Notify all owners via batch notification. (4) After grace period, old keys are revoked in batch. (5) Dashboard shows rotation progress per team.

**Q5: How do you optimize Redis memory for 50,000 keys?**
A: Each key's cache entry is ~10 KB (hash + permissions + rate limits + metadata). Total: ~500 MB. Redis cluster with 6 nodes (3 primary x 2 replicas) has ample capacity. We use Redis hash data structures (more memory-efficient than individual keys for sub-fields). TTL on cache entries (5 min) ensures stale entries are cleaned.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---------|--------|-----------|------------|-----|
| **Redis cluster down** | Validation latency increases (MySQL fallback); rate limiting degraded | Redis health check | MySQL fallback for validation. Local rate limits. | < 30s (sentinel failover) |
| **MySQL primary down** | No key creates/rotates/revokes; reads from replicas continue | MySQL heartbeat | Automated failover. Key creation queued. | < 30s |
| **Kafka down** | Revocation propagation delayed; usage data queued | Kafka health check | Redis pub/sub for revocation. Local usage buffer. | < 5 min |
| **Key Management Service down** | No key CRUD operations | Pod health check | Multiple replicas (3+). Existing keys continue working. | < 30s |
| **API Gateway node crash** | Traffic redistributed to other nodes | LB health check | LB removes unhealthy node. No client impact (retry). | Immediate |
| **GitHub webhook endpoint down** | Leak detection alerts delayed | Webhook delivery monitoring | GitHub retries for 24h. Internal scanner provides backup. | < 5 min |
| **Mass key revocation (false positive)** | Widespread authentication failures | Error rate spike | Emergency re-activation via admin CLI. Audit trail shows mass revocation. | < 5 min |
| **Redis pub/sub partition** | Some nodes don't receive revocations | Revocation verification job | Kafka consumer provides guaranteed delivery. MySQL sync as fallback. | < 60s |
| **Elasticsearch down** | Usage dashboards unavailable; no usage queries | ES health check | Kafka retains usage events (7-day retention). ES catches up on recovery. | < 1h |
| **CSPRNG entropy depletion** | Key generation blocks | `/proc/sys/kernel/random/entropy_avail` monitor | Use hardware RNG (rng-tools). Alert on low entropy. | < 5 min |

---

## 9. Security

### Authentication Chain

```
API Key Authentication Flow:
────────────────────────────
1. Client sends request with header:
   X-API-Key: plat_live_aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789abcdef

2. Envoy extracts key from header (HTTPS only -- reject HTTP)

3. SHA-256(key) → hash

4. Redis lookup: key_cache:{hex(hash)}
   Returns: key_id, owner, status, permissions, rate_limits, ip_allowlist

5. Validation checks (all must pass):
   a. status == 'active'
   b. expires_at is NULL or > now()
   c. grace_period_end is NULL or > now() (for rotated keys)
   d. source IP matches ip_allowlist (if configured)
   e. rate limit not exceeded
   f. permission matches (action + resource + namespace)

6. On success: forward request with headers:
   X-Authenticated-Key-Id: 12345
   X-Authenticated-Team: platform-core
   X-Authenticated-Environment: production
```

### Authorization Model

```
Permission Model:
────────────────
Each key has a set of permissions:
  {resource_type, resource_pattern, namespace_pattern, actions[], conditions}

Permission check algorithm:
  FOR each permission in key's permissions:
    IF resource_type matches request.resource_type
    AND resource_pattern matches request.resource_name (glob)
    AND namespace_pattern matches request.namespace (glob)
    AND request.action IN actions
    AND conditions_met(request, permission.conditions):
      RETURN ALLOW
  RETURN DENY

Condition types:
  - ip_cidr: ["10.0.0.0/8"] -- source IP must be in CIDR
  - time_window: {"start": "09:00", "end": "17:00", "tz": "US/Pacific"}
  - labels: {"env": "dev"} -- resource must have matching labels
```

### Audit Trail

- **Key lifecycle events:** create, rotate, revoke, expire, permission change, rate limit change.
- **Leak events:** detection, auto-revocation, notification, resolution.
- **Usage events:** per-key, per-minute aggregated (request count, error count, endpoints).
- **All audit events include:** timestamp, performer, key_id, key_prefix (not the full key), source IP.
- **Storage:** MySQL (lifecycle audit, 90 days), Elasticsearch (usage + audit search, 90 days), S3 (archive, 7 years).

### Threat Model

| Threat | Likelihood | Impact | Mitigation |
|--------|------------|--------|------------|
| **Key leaked in public repo** | High | Critical (if production) | GitHub secret scanning + auto-revocation. Prefix enables detection. |
| **Key leaked in internal logs** | High | High | Internal scanner. Logging framework strips `X-API-Key` headers. |
| **Key stolen from memory/network** | Medium | High | HTTPS only. Short-lived keys (90-day expiry). Rate limiting detects unusual usage. |
| **Brute force key guessing** | Very Low | Critical | 238-bit entropy makes brute force infeasible. Rate limit on auth failures per source IP. |
| **Key reuse after revocation** | Low | High | < 5 second revocation propagation. Redis + Kafka + MySQL triple check. |
| **Database breach (MySQL)** | Low | Medium (hashes only) | Only SHA-256 hashes stored. Cannot reconstruct keys from hashes. |
| **Insider creates overprivileged key** | Medium | High | Key creation requires approval for production keys. Permissions audited by AI agent. |
| **Rate limit bypass via distributed sources** | Medium | Medium | Per-key global rate limit (not per-IP). Keys have team-level aggregate limits. |

---

## 10. Incremental Rollout

**Phase 1 (Week 1-2): Core key management**
- Deploy Key Management Service, MySQL schema, Redis cluster.
- API key CRUD operations.
- Migrate existing API keys from config files to the system.

**Phase 2 (Week 3-4): Gateway integration**
- Envoy ext_authz filter for key validation.
- Redis caching for validation.
- Rate limiting (basic: per-key global).

**Phase 3 (Week 5-6): Usage tracking and rotation**
- Kafka usage pipeline.
- Elasticsearch dashboards.
- Key rotation with grace period.

**Phase 4 (Week 7-8): Leak detection**
- GitHub secret scanning integration.
- Internal scanner deployment.
- Automated revocation pipeline.

**Phase 5 (Week 9-10): Advanced features**
- Scoped permissions (resource + namespace).
- IP allowlisting.
- Honeypot keys.
- Self-service portal for teams.

**Interviewer Q&As:**

**Q1: How do you migrate existing API keys to the new system?**
A: (1) Inventory all existing keys (scan configs, Vault, k8s Secrets). (2) For each key, compute SHA-256 hash and register in the new system (key metadata, owner, permissions inferred from current usage). (3) Deploy gateway validation in "shadow mode" (log results but don't enforce). (4) Compare shadow results with existing auth. (5) Switch to enforcement once shadow mode shows 100% match.

**Q2: What is the rollback plan if the key validation system causes outages?**
A: The gateway ext_authz filter has a `failure_mode_allow` configuration. In emergency, set to `allow` -- all requests pass without key validation. This is the nuclear option (bypasses all auth) and should only be used for < 5 minutes while diagnosing the issue. A more targeted rollback: disable validation for specific endpoints via Envoy route configuration.

**Q3: How do you handle the transition from old auth to new auth?**
A: Dual-mode: both old auth mechanism and new API key are accepted during migration. The gateway checks both. If new key is provided, use it. If old auth is provided, use it (with a deprecation warning header). After 90 days, old auth is rejected.

**Q4: How do you onboard partner integrations?**
A: Self-service portal: partner fills out a form (company, use case, expected volume, required endpoints). Platform team reviews and approves. System generates a partner key with: scoped permissions, IP allowlist (partner's IP range), rate limits (per SLA tier), and 1-year expiry. Partner receives key via secure channel (Vault transit-encrypted email or secure portal).

**Q5: How do you handle key management for CI/CD pipelines?**
A: CI/CD keys have type `ci_cd`, scoped to `ci` environment, and are stored in the CI/CD platform's secret store (GitHub Actions secrets, GitLab CI variables). They have restricted permissions (deploy to staging, not production). Production deploys require a separate `service` key injected via Vault.

---

## 11. Trade-offs & Decision Log

| Decision | Trade-off | Rationale |
|----------|-----------|-----------|
| **Prefixed keys vs. opaque UUIDs** | Key structure reveals information (env type) vs. leak detection capability | The prefix is essential for automated leak detection (regex matching). The revealed information (environment type) is low-sensitivity. |
| **SHA-256 vs. bcrypt for hash storage** | Less computational cost vs. brute-force resistance | 238-bit entropy makes brute-force infeasible with any hash function. SHA-256 enables 100K validations/sec on the critical path. |
| **Redis pub/sub + Kafka vs. Kafka only** | Additional infrastructure vs. faster revocation | Sub-second revocation for leaked keys justifies the dual-path approach. Kafka alone would be 2-5 seconds. |
| **Local token bucket + Redis vs. Redis only** | Slight over-allowance vs. lower latency | 80% of requests served locally (< 0.1 ms). The occasional over-allowance during refill is acceptable. |
| **Auto-revocation on leak vs. human approval** | Risk of false positive outage vs. speed of response | Production key leaks are too urgent for human approval. False positive risk is mitigated by hash matching (not prefix-only). Test keys are also auto-revoked (low impact). |
| **Grace period on rotation vs. instant cutover** | Old key accepted during window vs. operational safety | 24-hour grace period gives teams time to update configurations. Without it, every rotation risks an outage. |
| **Per-key rate limiting vs. per-team** | Granularity vs. simplicity | Per-key allows fine-grained control. We also support per-team aggregate limits as a safety net. |
| **50-char key vs. shorter** | Usability (long to type) vs. security | API keys are copy-pasted, not typed. 238-bit entropy is worth the extra characters. |

---

## 12. Agentic AI Integration

### AI-Powered API Key Intelligence

**1. Usage Anomaly Detection**
- Train a model on per-key usage patterns: request rate, endpoints accessed, time of day, source IPs.
- Flag anomalies: "Key `plat_live_aBcD` normally makes 50 req/min to `/api/v1/pods`. In the last hour, it made 5,000 req/min to `/api/v1/secrets` from a new IP."
- Automated response: reduce rate limit, alert owner, optionally suspend key.

**2. Permission Right-Sizing**
- Analyze actual key usage vs. granted permissions.
- Recommend: "Key `plat_live_aBcD` has `pods:*:*` permission but only uses `pods:myapp-*:get,list`. Recommend scoping to read-only for `myapp-*` namespace."
- CLI: `platform-key ai recommend --key-id 12345`

**3. Key Lifecycle Management**
- Identify keys that haven't been used in 90+ days: recommend revocation.
- Identify keys approaching expiration: notify owners 30 days in advance.
- Identify keys with no rotation in 180+ days: recommend rotation.

**4. Automated Leak Forensics**
- On leak detection, AI agent:
  - Traces how the key ended up in the leak location (Git blame, commit history).
  - Identifies all requests made with the key since the leak timestamp.
  - Assesses the blast radius (what resources were accessed/modified).
  - Generates a forensic report for the incident ticket.

**5. Natural Language Key Management**
- "Create a read-only API key for the data team that can access pods in the analytics namespace."
- Agent translates to: `platform-key create --name data-team-read --permissions "pods:analytics-*:get,list" --team data-eng`

**6. Smart Rate Limit Tuning**
- Analyze historical usage to recommend optimal rate limits per key.
- "Key `plat_live_aBcD` hits its 100/sec limit 5 times per day during batch processing. Recommend increasing to 200/sec with a 500-request burst allowance."

---

## 13. Complete Interviewer Q&A Bank

**Q1: How do you design an API key that is both secure and identifiable for leak detection?**
A: Use a prefixed format: `plat_live_<40 random chars>`. The prefix (`plat_live_`) enables regex-based detection in GitHub secret scanning and internal scanners. The random body (40 base62 chars, ~238 bits entropy) ensures unpredictability. Store only the SHA-256 hash in the database. The prefix is also stored separately for identification without the full key.

**Q2: Why store the hash instead of encrypting the key?**
A: Encryption is reversible -- if the encryption key is compromised, all API keys are exposed. Hashing is one-way -- even with database access, the original keys cannot be recovered. Since we never need to retrieve the original key (users must store it themselves), hashing is strictly better for security.

**Q3: How do you implement API key rotation without downtime?**
A: Grace period rotation: (1) Generate new key. (2) Old key remains valid for a configurable grace period (24h). (3) Both keys are accepted simultaneously. (4) API responses include `X-API-Key-Deprecated: true` for the old key. (5) After grace period, old key is revoked. This gives clients time to switch without any downtime.

**Q4: How do you rate limit effectively across a distributed API gateway?**
A: Redis sliding window counter provides global consistency. Each gateway node increments the counter on Redis for each request. For performance, a local token bucket caches a batch of tokens (refilled from Redis every 100ms), serving 80% of requests without a Redis round trip. The trade-off is slight over-allowance during refill intervals.

**Q5: How would you handle a massive API key leak (e.g., database of keys exposed)?**
A: (1) The database only contains hashes, not plaintext keys. Attacker cannot use hashes directly. (2) As a precaution, force-rotate ALL keys: generate new keys, enter grace period, notify all owners. (3) Investigate the breach vector. (4) This is why we hash and never store plaintext -- the blast radius of a database breach is zero for the keys themselves.

**Q6: How do you prevent API key abuse in CI/CD pipelines?**
A: (1) CI/CD keys are scoped to `ci` environment with limited permissions. (2) Keys are stored in the CI/CD platform's secret store (not in code). (3) Short TTL: CI/CD keys expire in 90 days and must be rotated. (4) IP allowlist: CI/CD keys only work from the CI/CD runner's IP range. (5) Pre-commit hooks block key patterns in code.

**Q7: How do you handle API key management for a multi-tenant platform?**
A: (1) Each tenant can only see/manage their own keys (owner-based access control). (2) Keys are scoped to tenant namespaces (cannot access other tenants' resources). (3) Tenant-level aggregate rate limits (all keys from a tenant share a quota). (4) Tenant admins can create/revoke keys for their team via self-service.

**Q8: What is the difference between API keys and OAuth2 tokens?**
A: API keys are long-lived, static credentials for programmatic access. They identify the calling application/service. OAuth2 tokens are short-lived, represent a user's delegated authorization, and support scopes. For service-to-service calls (no user context), API keys are simpler. For user-delegated access, OAuth2 is more appropriate. We support both: API keys for automation, OAuth2 for user-facing APIs.

**Q9: How do you handle key validation latency if Redis and MySQL are both slow?**
A: (1) In-process LRU cache (10K entries, 60s TTL) as the first check. (2) Redis as the second check. (3) MySQL as the last resort. If all are slow, the gateway circuit breaker trips after 50ms (configurable). In emergency mode, the gateway can be configured to fail-open (dangerous) or fail-closed (safe but causes outage).

**Q10: How do you prevent timing attacks on API key validation?**
A: The SHA-256 hash is computed before the lookup. The comparison is constant-time (`MessageDigest.isEqual` in Java, `hmac.compare_digest` in Python). This prevents an attacker from determining how many characters of the key are correct based on response time. However, with 238-bit entropy, timing attacks are impractical anyway.

**Q11: How do you handle API key versioning if the key format changes?**
A: The prefix encodes the version implicitly. Current format: `plat_live_...`. If we change the format, new keys use a new prefix: `plat2_live_...`. The validation logic detects the prefix and applies the correct validation algorithm. Old keys continue to work until revoked/expired.

**Q12: How do you ensure the key is only transmitted over HTTPS?**
A: (1) API gateway rejects HTTP requests (redirect to HTTPS or 403). (2) HSTS headers on all responses. (3) For internal traffic, mTLS ensures encryption. (4) The API key header (`X-API-Key`) is never logged by the gateway (stripped from access logs).

**Q13: How do you implement per-endpoint rate limiting?**
A: Each key can have multiple rate limit policies with `endpoint_pattern` globs. Example: 100/sec global, 10/sec for `POST /api/v1/clusters/*`. The gateway matches the request path against all endpoint patterns and checks each applicable rate limit. The request is denied if any limit is exceeded.

**Q14: How do you handle the situation where a key is used in multiple environments?**
A: Keys are environment-scoped (`plat_live_...`, `plat_test_...`). A production key cannot access staging resources and vice versa. The gateway checks the key's environment against the target resource's environment. If a team needs access to both, they create separate keys per environment.

**Q15: What metrics do you expose for API key management?**
A: (1) `api_key_validations_total` (counter, labels: status=success/fail/rate_limited). (2) `api_key_validation_latency_seconds` (histogram). (3) `api_key_active_count` (gauge, labels: env, type). (4) `api_key_revocations_total` (counter, labels: reason). (5) `api_key_leaks_detected_total` (counter, labels: source). (6) `api_key_rate_limit_exceeded_total` (counter). (7) `api_key_rotation_age_days` (histogram, for tracking rotation hygiene).

**Q16: How do you handle API key management for serverless functions or ephemeral workloads?**
A: For ephemeral workloads (short-lived pods, serverless functions), use Vault dynamic secrets instead of static API keys. The workload authenticates to Vault via its platform identity (k8s SA, IAM role) and receives a short-lived API key (1h TTL) with scoped permissions. The key auto-expires, no rotation needed.

---

## 14. References

- [GitHub Secret Scanning](https://docs.github.com/en/code-security/secret-scanning)
- [Stripe API Key Design](https://stripe.com/docs/keys)
- [GitHub Token Format](https://github.blog/2021-04-05-behind-githubs-new-authentication-token-formats/)
- [Rate Limiting with Redis](https://redis.io/commands/incr#pattern-rate-limiter)
- [OWASP API Security Top 10](https://owasp.org/API-Security/)
- [Token Bucket Algorithm](https://en.wikipedia.org/wiki/Token_bucket)
- [Sliding Window Rate Limiting](https://blog.cloudflare.com/counting-things-a-lot-of-different-things/)
- [NIST SP 800-63B: Digital Identity Guidelines](https://pages.nist.gov/800-63-3/sp800-63b.html)
