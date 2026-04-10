# System Design: Configuration Management Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Centralized Configuration Store**: Store application configuration (non-secret key-value pairs, structured JSON/YAML documents) in a central service, versioned and auditable.
2. **Config Versioning**: Every write to a config key creates a new immutable version. Prior versions are retrievable by version number or timestamp.
3. **Environment-Specific Overrides**: A config key can have a base value with environment-specific overrides (dev, staging, production). Reads resolve the effective value for a given environment.
4. **Hot Reload**: Applications can subscribe to config changes and receive updates without restarting. The SDK notifies the application callback within 5 seconds of a change.
5. **Secret Management Integration**: Sensitive values (passwords, API keys) are stored in a vault (HashiCorp Vault or AWS Secrets Manager), not in the config service itself. Config entries can reference vault paths using a `vault://` URI scheme; the SDK transparently resolves and injects vault values.
6. **Config Validation**: Schemas (JSON Schema) can be attached to config namespaces. Writes that fail schema validation are rejected.
7. **Rollback**: One-click rollback to any previous version of a config key or namespace.
8. **Namespace/Hierarchy**: Configs are organized in a hierarchical namespace (e.g., `payments/gateway/timeout_ms`). Applications can subscribe to an entire namespace and receive all keys under it.
9. **Access Control**: Fine-grained RBAC: read-only, write, admin per namespace. Service accounts for applications.
10. **Audit Trail**: All config reads (for sensitive keys) and all writes are logged immutably.
11. **Config Diff**: Ability to diff any two versions of a config namespace for change review.
12. **Templating**: Support for config values that reference other config values (e.g., `"endpoint": "https://{{db.host}}:{{db.port}}/mydb"`).

### Non-Functional Requirements

- **Availability**: 99.99% uptime for config reads. The config service is in the hot path of application startup; an outage must not prevent applications from starting.
- **Read Latency**: Config reads from SDK local cache < 0.1ms. Initial config load (network fetch on app start) < 500ms (p99).
- **Write Latency**: Config write (API call) < 500ms (p99).
- **Propagation Latency**: Config change propagates to all subscribed SDKs within 5 seconds (p99).
- **Consistency**: Strong consistency on write (a write is not acknowledged until durably committed). Reads may be slightly stale (SDK cache) but converge within 5 seconds.
- **Scale**: 10,000 services across 500 organizations, each with up to 100,000 config keys. 1 billion config reads per day (all served from SDK cache, not from the service).
- **Security**: Secrets never stored in plaintext in the config service database. Vault references resolved at read time with application-scoped credentials.

### Out of Scope

- Secrets storage implementation (delegated to HashiCorp Vault / AWS Secrets Manager).
- Infrastructure provisioning configuration (Terraform state — that's IaC, not application config).
- Feature flags (see feature_flag_service.md — different lifecycle and evaluation semantics).
- Service discovery (Consul, Kubernetes Service DNS — separate concern).
- Database schema migrations.

---

## 2. Users & Scale

### User Types

| User Type | Description | Primary Actions |
|---|---|---|
| Developer | Configures application settings | Create namespaces, set key-value pairs, test changes in dev |
| Platform/DevOps Engineer | Manages production configs | Approve production changes, manage service account access |
| SRE | Responds to incidents | Emergency config overrides (circuit breaker values, timeouts), rollbacks |
| Application (SDK) | Reads config at startup and runtime | Initialize with namespace, cache all keys, receive hot-reload notifications |
| Automated Pipeline | Promotes config changes across environments | Read staging config, diff against prod, promote on approval |
| Security Auditor | Reviews access and change history | Query audit trail, verify no secrets stored in plaintext |

### Traffic Estimates

Assumptions:
- 10,000 services, average 10 instances each = 100,000 application instances.
- Each instance reads ~1,000 config keys at startup. Startup frequency: 10 restarts/instance/day (rolling deploys, autoscaling).
- Each instance receives ~5 config changes/day via hot reload.
- Config write frequency: 1,000 writes/day across all orgs (config changes are infrequent).

| Metric | Calculation | Result |
|---|---|---|
| Config reads/day (all in SDK cache) | 100,000 instances × 1,000 keys × 10 reads/key/day | 1 billion/day (in-process, no network) |
| Initial config loads/day (network) | 100,000 instances × 10 restarts/day | 1M loads/day = ~11.6 loads/sec |
| Config loads peak (deploy burst) | 11.6 × 20 (all services deploy simultaneously) | ~232 loads/sec peak |
| Hot reload events/day | 100,000 instances × 5 changes/day | 500,000 events/day = ~5.8 events/sec |
| Config writes/sec | 1,000 writes/day / 86,400 | ~0.01 writes/sec (very low) |
| SSE connections | 100,000 application instances | 100,000 persistent connections |
| Schema validation calls | Same as write rate | ~0.01/sec |
| Vault secret resolution | At SDK init per secret reference | ~11.6 × (avg secrets per app = 20) = ~232 vault calls/sec at peak |

### Latency Requirements

| Operation | Target (p50) | Target (p99) | Notes |
|---|---|---|---|
| Config read (SDK cache) | < 0.01 ms | < 0.1 ms | In-process hash map lookup |
| Config load on startup | 50 ms | 500 ms | Network fetch of namespace snapshot |
| Config write (API) | 100 ms | 500 ms | Postgres write + Kafka publish |
| Change propagation to SDK | 1 s | 5 s | Kafka → SSE → SDK |
| Rollback operation | 200 ms | 1 s | Version lookup + write |
| Diff computation | 50 ms | 500 ms | Two-version JSON diff |
| Vault secret resolution | 5 ms | 50 ms | Vault API call (cached in SDK) |

### Storage Estimates

| Data Type | Size per Unit | Volume | Retention | Total |
|---|---|---|---|---|
| Config key-value records | ~1 KB/key | 100K keys/org × 500 orgs = 50M keys | Indefinite (versioned) | 50M × 1 KB = 50 GB |
| Config versions (history) | ~1 KB/version | 50M keys × 10 avg versions | Indefinite | 500M × 1 KB = 500 GB |
| Config snapshots (cache) | ~100 MB/namespace snapshot | 10,000 namespaces × 10 envs | Latest only | 100 TB (too large for Redis; use S3) |
| Audit events | ~500 B/event | 1,000 writes/day = 365K/year | 7 years | 365K × 7 × 500 B = ~1.28 GB (small) |
| Schemas (JSON Schema docs) | ~10 KB/schema | 10,000 namespaces | Indefinite | 100 MB |

Note: Config snapshot size is dominated by the number of keys. A realistic namespace is ~1,000 keys × 1 KB = 1 MB. At 10,000 namespaces × 10 envs = 100,000 snapshots × 1 MB = 100 GB in Redis — manageable.

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| SDK initial load (startup) | 232 loads/sec × 1 MB avg namespace | ~232 MB/s peak |
| Hot reload deltas | 5.8 events/sec × 100 KB delta × 100,000 SDK instances / 86,400 | ~674 MB/s (delta pushes to all instances) |
| Config write API | 0.01 writes/sec × 10 KB | Negligible |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│                             MANAGEMENT PLANE                                         │
│                                                                                      │
│  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐                 │
│  │  Web UI          │   │  Management API  │   │  Schema          │                 │
│  │  (Config editor, │──▶│  (REST, RBAC)    │   │  Validator       │                 │
│  │   diff viewer,   │   │  /v1/configs     │──▶│  (JSON Schema)   │                 │
│  │   audit viewer)  │   │  /v1/namespaces  │   │                  │                 │
│  └──────────────────┘   └────────┬─────────┘   └──────────────────┘                 │
│                                  │                                                   │
│         ┌────────────────────────┼────────────────────────┐                         │
│         │                        │                        │                         │
│         ▼                        ▼                        ▼                         │
│  ┌──────────────┐  ┌─────────────────────────┐  ┌──────────────────┐               │
│  │  Auth Service│  │  Config Store           │  │  Vault Connector │               │
│  │  (RBAC,      │  │  (PostgreSQL)           │  │  (resolve        │               │
│  │   JWT, svc   │  │  namespaces, keys,      │  │   vault:// refs) │               │
│  │   accounts)  │  │  versions, audit        │  │                  │               │
│  └──────────────┘  └──────────┬──────────────┘  └──────────────────┘               │
│                               │ on write: publish                                    │
│                               ▼                                                      │
│                    ┌─────────────────────────┐                                       │
│                    │  Change Event Bus       │                                       │
│                    │  (Kafka)                │                                       │
│                    │  Topic: config-changes  │                                       │
│                    └───────────┬─────────────┘                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                  │
              ▼                  ▼                  ▼
┌────────────────────────────────────────────────────────────────────────────────────┐
│                              DELIVERY PLANE                                         │
│                                                                                     │
│  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐                │
│  │  Snapshot        │   │  SSE / Streaming │   │  Polling         │                │
│  │  Builder         │   │  Server          │   │  Server          │                │
│  │  (builds env     │──▶│  (push deltas to │   │  (ETag-based     │                │
│  │   snapshots,     │   │   connected SDKs)│   │   GET /configs)  │                │
│  │   stores in S3/  │   │                  │   │                  │                │
│  │   Redis)         │   └──────────────────┘   └──────────────────┘                │
│  └──────────────────┘                                                               │
│                                                                                     │
│  ┌─────────────────────────────────────────────────────────────────┐                │
│  │                     Config Read API                             │                │
│  │  GET /v1/sdk/namespace/{ns}?env=production                     │                │
│  │  (returns full namespace snapshot for SDK initialization)       │                │
│  └─────────────────────────────────────────────────────────────────┘                │
└────────────────────────────────────────────────────────────────────────────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                  │
              ▼                  ▼                  ▼
    ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
    │  Server-Side SDK │  │  Sidecar Process │  │  Kubernetes      │
    │  (in-process     │  │  (mounted config │  │  ConfigMap       │
    │   cache, hot     │  │   files, inotify │  │  Sync Operator   │
    │   reload hooks)  │  │   watch)         │  │  (sync config    │
    │                  │  │                  │  │   to K8s         │
    │                  │  │                  │  │   ConfigMaps)    │
    └──────────────────┘  └──────────────────┘  └──────────────────┘
              │
              ▼
    ┌──────────────────────────────────────┐
    │  Vault Connector (in SDK)            │
    │  - Resolves vault:// references      │
    │  - Caches resolved secrets           │
    │  - Rotates via Vault lease renewal   │
    └──────────────────────────────────────┘
```

**Component Roles:**

- **Management API**: CRUD for namespaces, config keys, schemas. Validates all writes against attached JSON schemas. Enforces RBAC. Writes to PostgreSQL and publishes `ConfigChangedEvent` to Kafka.
- **Schema Validator**: Validates incoming config values against a JSON Schema document attached to the namespace. Validates before the write is committed. Returns detailed error messages for schema violations.
- **Config Store (PostgreSQL)**: Source of truth. Stores all config keys, versions, environment overrides, schemas, and audit events. Immutable versioned history.
- **Vault Connector**: Manages the interface between the config service and vault backends. When a config value contains a `vault://` reference, the connector resolves it using a service-scoped vault token. Also handles vault secret rotation notifications.
- **Change Event Bus (Kafka)**: Durable delivery of config change events to all delivery-plane consumers. Partitioned by `org_id` × `namespace`.
- **Snapshot Builder**: Consumes change events, rebuilds environment snapshots, stores in Redis (for small namespaces) or S3 (for large namespaces). The snapshot is the authoritative serialized config for an org × namespace × environment.
- **SSE Streaming Server**: Pushes config deltas to connected SDK instances. Maintains a map of connections organized by org × namespace × environment.
- **Polling Server**: Serves ETag-based polling requests. Returns `304` if no changes, `200` with new snapshot if changed.
- **Config Read API**: Serves the full namespace snapshot at SDK initialization. CDN-cacheable for non-sensitive configs (without vault references).
- **Server-Side SDK**: Downloads namespace snapshot at startup, resolves vault references, caches all values in a thread-safe in-memory map, registers hot-reload callbacks, maintains SSE connection.
- **Sidecar Process**: For applications that prefer file-based config (legacy apps), the sidecar mounts config as files on a shared volume and uses `inotify` to notify the app of changes.
- **Kubernetes ConfigMap Sync Operator**: For Kubernetes-native applications, syncs config service values to Kubernetes ConfigMaps and Secrets on change.

**Primary Use-Case Data Flow (application starts and loads config):**

1. Application starts; SDK initializes with service account JWT and namespace `payments/gateway`.
2. SDK calls `GET /v1/sdk/namespace/payments/gateway?env=production`. Config Read API returns snapshot (MessagePack-encoded, 1 MB, ETag: `v137`).
3. SDK deserializes snapshot into in-memory hash map. Identifies all values with `vault://` prefixes.
4. SDK calls Vault Connector with application's Vault token to resolve vault references. Vault returns secrets; SDK stores in-memory (never in logs).
5. SDK registers application's hot-reload callback function.
6. SDK opens SSE connection to `/v1/sdk/stream/payments/gateway?env=production`.
7. Application calls `config.get("payments.gateway.timeout_ms")` — returns `5000` from in-memory map in < 0.1ms.
8. An SRE changes `payments.gateway.timeout_ms` from `5000` to `3000` in the Management UI.
9. Management API validates the change (schema: `type: integer, minimum: 100, maximum: 30000`), writes new version to Postgres, publishes `ConfigChangedEvent` to Kafka.
10. Snapshot Builder rebuilds the snapshot, updates Redis.
11. SSE Server pushes delta `{"key": "payments.gateway.timeout_ms", "value": 3000, "version": 138}` to all connected SDKs.
12. SDK invokes the registered hot-reload callback with the changed key-value pair. Application code updates the HTTP client timeout without restarting.

---

## 4. Data Model

### Entities & Schema

```sql
-- Organizations
CREATE TABLE organizations (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    slug            VARCHAR(64) UNIQUE NOT NULL,
    name            VARCHAR(255) NOT NULL,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Environments
CREATE TABLE environments (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    name            VARCHAR(64) NOT NULL,    -- 'development', 'staging', 'production'
    slug            VARCHAR(64) NOT NULL,
    is_production   BOOLEAN NOT NULL DEFAULT false,
    UNIQUE(org_id, slug)
);

-- Namespaces (hierarchical config groupings)
CREATE TABLE namespaces (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    path            VARCHAR(1024) NOT NULL,  -- e.g., 'payments/gateway'
    description     TEXT,
    schema_id       UUID REFERENCES config_schemas(id),  -- optional validation schema
    created_by      UUID REFERENCES users(id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(org_id, path)
);

-- Schemas for validation
CREATE TABLE config_schemas (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL,
    namespace_id    UUID REFERENCES namespaces(id) ON DELETE SET NULL,
    name            VARCHAR(255) NOT NULL,
    json_schema     JSONB NOT NULL,          -- JSON Schema document
    version         INT NOT NULL DEFAULT 1,
    created_by      UUID REFERENCES users(id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Config keys (the actual configuration entries)
CREATE TABLE config_keys (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    namespace_id    UUID NOT NULL REFERENCES namespaces(id) ON DELETE CASCADE,
    key             VARCHAR(1024) NOT NULL,   -- e.g., 'timeout_ms'
    full_path       VARCHAR(2048) NOT NULL,   -- e.g., 'payments/gateway/timeout_ms'
    description     TEXT,
    value_type      VARCHAR(32) NOT NULL,     -- 'string','integer','float','boolean','json','vault_ref'
    is_sensitive    BOOLEAN NOT NULL DEFAULT false, -- if true: mask in UI, encrypt in DB
    tags            TEXT[] NOT NULL DEFAULT '{}',
    created_by      UUID REFERENCES users(id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(namespace_id, key)
);

-- Config values per environment (current live value)
CREATE TABLE config_values (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    config_key_id   UUID NOT NULL REFERENCES config_keys(id) ON DELETE CASCADE,
    environment_id  UUID NOT NULL REFERENCES environments(id) ON DELETE CASCADE,
    value           TEXT,                    -- serialized value; vault refs stored as vault:// URI
    encrypted_value BYTEA,                   -- AES-256-GCM encrypted for sensitive keys
    version         BIGINT NOT NULL DEFAULT 1,
    updated_by      UUID REFERENCES users(id),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(config_key_id, environment_id)
);

-- Immutable version history for config values
CREATE TABLE config_versions (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    config_key_id   UUID NOT NULL REFERENCES config_keys(id) ON DELETE CASCADE,
    environment_id  UUID NOT NULL REFERENCES environments(id) ON DELETE CASCADE,
    version         BIGINT NOT NULL,
    value           TEXT,
    encrypted_value BYTEA,
    changed_by      UUID REFERENCES users(id),
    change_reason   TEXT,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    INDEX idx_config_versions_key_env (config_key_id, environment_id, version DESC)
);

-- Per-namespace snapshots (metadata; actual snapshot data in S3/Redis)
CREATE TABLE namespace_snapshots (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    namespace_id    UUID NOT NULL REFERENCES namespaces(id),
    environment_id  UUID NOT NULL REFERENCES environments(id),
    version         BIGINT NOT NULL,         -- max config_value.version within namespace
    etag            VARCHAR(64) NOT NULL,    -- SHA-256 of snapshot content
    storage_key     VARCHAR(1024),           -- S3 key if snapshot exceeds Redis limit
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(namespace_id, environment_id)
);

-- RBAC: Namespace access policies
CREATE TABLE access_policies (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL,
    namespace_path  VARCHAR(1024) NOT NULL,  -- can be prefix (e.g., 'payments/*')
    principal_type  VARCHAR(32) NOT NULL,    -- 'user','group','service_account'
    principal_id    UUID NOT NULL,
    permission      VARCHAR(32) NOT NULL,    -- 'read','write','admin'
    created_by      UUID REFERENCES users(id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Service accounts (for application SDKs)
CREATE TABLE service_accounts (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id),
    name            VARCHAR(255) NOT NULL,
    description     TEXT,
    api_key_hash    VARCHAR(128) NOT NULL,   -- bcrypt hash of the API key
    allowed_namespaces TEXT[] NOT NULL DEFAULT '{}', -- namespaces this SA can read
    created_by      UUID REFERENCES users(id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Audit events (append-only)
CREATE TABLE audit_events (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL,
    actor_id        UUID,
    actor_type      VARCHAR(32) NOT NULL,    -- 'user','service_account','system'
    event_type      VARCHAR(128) NOT NULL,   -- e.g., 'config.value.updated','config.rolled_back'
    namespace_path  VARCHAR(1024),
    config_key      VARCHAR(1024),
    environment     VARCHAR(64),
    before_value    TEXT,                    -- masked if is_sensitive
    after_value     TEXT,                    -- masked if is_sensitive
    change_reason   TEXT,
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    INDEX idx_audit_org_time      (org_id, occurred_at DESC),
    INDEX idx_audit_namespace     (org_id, namespace_path, occurred_at DESC)
);

-- Users
CREATE TABLE users (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id),
    email           VARCHAR(255) NOT NULL,
    display_name    VARCHAR(255),
    role            VARCHAR(32) NOT NULL DEFAULT 'member',
    UNIQUE(org_id, email)
);
```

### Database Choice

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **PostgreSQL** | ACID transactions for atomic version + value update; JSONB for schema documents; strong consistency; mature ecosystem | Requires horizontal sharding at scale | **Selected** |
| etcd | Designed for distributed config; watch API native; strong consistency via Raft | Limited storage (1-8 GB by default); no SQL querying; complex cluster management; not suitable for 50M keys | Not suitable at this key count |
| Consul KV | Service discovery integration; watch API; ACLs | Storage limits; no versioning native; weaker querying | Not suitable as primary store |
| ZooKeeper | Proven at large scale; ephemeral/persistent nodes | Complex API; no SQL; no versioning; Java-only native client | Not suitable |
| DynamoDB | Scalable; streams for change events | No complex queries; no versioning native; no JSONB | Not selected |

**Selected: PostgreSQL** with the following rationale:
- Config version history requires a proper append-only audit table with flexible querying (by namespace, by actor, by time range). Only a relational DB with JSONB support provides this without multiple separate services.
- The `SELECT FOR UPDATE` mechanism ensures that concurrent writes to the same config key don't result in lost updates (each write atomically increments the version and inserts a version record).
- etcd (the Kubernetes configuration store) is often considered for this use case but caps out at ~8 GB storage and is not designed for 50M application config keys.

**Supporting stores:**
- **Redis**: Namespace snapshots (< 10 MB each). O(1) lookup by `{namespace_id}:{env_id}` key.
- **S3**: Large namespace snapshots (> 10 MB), encrypted at rest.
- **Kafka**: Config change event bus.
- **HashiCorp Vault / AWS Secrets Manager**: Secret storage backend (not part of the config service but integrated via vault references).

---

## 5. API Design

```
Base URL: https://api.configservice.example.com/v1
Auth: Management APIs — Bearer JWT (OAuth 2.0). SDK APIs — API key (X-Service-Key header).
Rate limits: Management API — 200 req/min per user. SDK read — 60 req/min per service instance.
```

### Namespace Management

```
# List namespaces
GET /orgs/{org_id}/namespaces?prefix=payments&limit=50&cursor=...
Response: 200
{ "namespaces": [ { "id", "path", "description", "key_count", "schema_attached" } ] }

# Create namespace
POST /orgs/{org_id}/namespaces
Body: {
  "path": "payments/gateway",
  "description": "Payment gateway configuration",
  "schema": { ... }  // optional JSON Schema document
}
Response: 201
{ "namespace_id": "...", "path": "payments/gateway" }

# Get namespace config (all keys × all environments)
GET /orgs/{org_id}/namespaces/{namespace_path}?include_history=false
Response: 200
{
  "namespace": { "id", "path", "schema": {...} },
  "keys": [
    {
      "key": "timeout_ms",
      "full_path": "payments/gateway/timeout_ms",
      "value_type": "integer",
      "is_sensitive": false,
      "environments": {
        "production": { "value": 5000, "version": 42, "updated_at": "..." },
        "staging":    { "value": 3000, "version": 41, "updated_at": "..." },
        "development":{ "value": 1000, "version": 38, "updated_at": "..." }
      }
    }
  ]
}

# Diff two versions of a namespace
GET /orgs/{org_id}/namespaces/{namespace_path}/diff?env=production&from_version=41&to_version=42
Response: 200
{
  "added":   [ { "key", "new_value" } ],
  "removed": [ { "key", "old_value" } ],
  "changed": [ { "key", "old_value", "new_value" } ]
}
```

### Config Key CRUD

```
# Set a config value
PUT /orgs/{org_id}/namespaces/{namespace_path}/keys/{key}
Body: {
  "value": 3000,
  "environment": "production",
  "change_reason": "Reducing timeout for performance improvement"
}
Response: 200
{ "version": 43, "updated_at": "..." }

# Get a config value (with history)
GET /orgs/{org_id}/namespaces/{namespace_path}/keys/{key}?env=production&history=true&limit=10
Response: 200
{
  "current": { "value": 3000, "version": 43, "updated_at": "...", "updated_by": {...} },
  "history": [
    { "version": 43, "value": 3000, "changed_by": {...}, "change_reason": "...", "created_at": "..." },
    { "version": 42, "value": 5000, "changed_by": {...}, "change_reason": "...", "created_at": "..." }
  ]
}

# Rollback a key to a previous version
POST /orgs/{org_id}/namespaces/{namespace_path}/keys/{key}/rollback
Body: {
  "environment": "production",
  "target_version": 42,
  "change_reason": "Rolling back timeout increase — caused latency spikes"
}
Response: 200
{ "new_version": 44, "value": 5000 }

# Batch update multiple keys in a namespace (atomic)
POST /orgs/{org_id}/namespaces/{namespace_path}/batch
Body: {
  "environment": "production",
  "changes": [
    { "key": "timeout_ms", "value": 3000 },
    { "key": "retry_count", "value": 3 },
    { "key": "circuit_breaker_threshold", "value": 0.5 }
  ],
  "change_reason": "Performance tuning batch"
}
Response: 200
{ "versions": { "timeout_ms": 43, "retry_count": 22, "circuit_breaker_threshold": 8 } }
```

### Promote Between Environments

```
# Promote staging config to production (for review before applying)
GET /orgs/{org_id}/namespaces/{namespace_path}/promote/preview?from=staging&to=production
Response: 200
{
  "diff": {
    "changed": [ { "key": "timeout_ms", "staging_value": 3000, "production_value": 5000 } ]
  },
  "approval_required": true
}

# Apply the promotion
POST /orgs/{org_id}/namespaces/{namespace_path}/promote
Body: {
  "from_environment": "staging",
  "to_environment": "production",
  "keys": ["timeout_ms"],   // optional: promote only specific keys; default = all
  "change_reason": "Promoting staged timeout change to production"
}
Response: 200
{ "promoted_keys": ["timeout_ms"], "new_versions": { "timeout_ms": 43 } }
```

### SDK APIs

```
# SDK: Download full namespace snapshot (on startup)
GET /sdk/namespace/{namespace_path}?env=production
Headers: X-Service-Key: svc-xxxx
Response: 200
Content-Type: application/x-msgpack
X-Config-Etag: "sha256:abc123"
X-Config-Version: "43"
Body: <MessagePack: all keys with resolved values (vault refs replaced with placeholder types)>

# SDK: Poll for changes (ETag-based)
GET /sdk/namespace/{namespace_path}?env=production
Headers:
  X-Service-Key: svc-xxxx
  If-None-Match: "sha256:abc123"
Response: 304 Not Modified  OR  200 with new snapshot

# SDK: Stream updates (SSE)
GET /sdk/stream/{namespace_path}?env=production
Headers: X-Service-Key: svc-xxxx
Response: 200 text/event-stream
event: config_updated
data: {"key": "payments/gateway/timeout_ms", "value": 3000, "version": 43}

event: snapshot_version
data: {"etag": "sha256:def456", "version": "43"}
```

### Audit Log

```
# Query audit log
GET /orgs/{org_id}/audit?namespace=payments/gateway&env=production&
    actor_id={user_id}&from=2026-04-01T00:00:00Z&to=2026-04-09T23:59:59Z&limit=50&cursor=...
Response: 200
{
  "events": [
    {
      "event_type": "config.value.updated",
      "actor": { "id", "email", "type" },
      "namespace_path": "payments/gateway",
      "config_key": "timeout_ms",
      "environment": "production",
      "before_value": "5000",
      "after_value": "3000",
      "change_reason": "...",
      "occurred_at": "..."
    }
  ]
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Hot Reload (Sub-5-Second Config Propagation)

**Problem it solves**: When an SRE changes a config value (e.g., increases circuit breaker threshold during an incident), all 100,000 running application instances must pick up the change within 5 seconds without restarting.

**Approaches Comparison:**

| Approach | Mechanism | Latency | Complexity | Works During App Restart? |
|---|---|---|---|---|
| **File-based polling (inotify)** | Sidecar writes config to file; app polls file with inotify | < 1s (inotify is event-driven) | Medium (sidecar required) | No (file re-read on restart) |
| **In-process polling** | SDK polls `/sdk/namespace` endpoint every N seconds | N seconds maximum | Low | Yes (initial load) |
| **In-process SSE** | SDK holds SSE connection; receives push on change | < 1s | Medium | Yes |
| **Kubernetes ConfigMap** | Config synced to K8s ConfigMap; app reads mounted volume | 1-60s (kubelet sync interval) | High (requires K8s operator) | Yes |
| **Message queue consumer** | App subscribes to Kafka/SQS topic | < 1s | High (app must be Kafka consumer) | Yes |

**Selected: In-process SSE for server applications + Kubernetes ConfigMap sync for Kubernetes-native apps + inotify sidecar for legacy apps**

Rationale: SSE provides near-instant push without requiring applications to implement Kafka consumer logic. The SDK abstracts the SSE complexity. For Kubernetes workloads where application teams prefer declarative config, the ConfigMap sync operator is the better fit (it requires zero SDK code changes). For legacy apps that read config from files, the sidecar approach doesn't require code changes at all.

**Implementation — SDK hot reload mechanism:**

```go
type ConfigSDK struct {
    namespace   string
    environment string
    serviceKey  string
    cache       *sync.Map          // map[string]ConfigValue
    etag        string
    callbacks   []HotReloadCallback
    mu          sync.RWMutex
    stopCh      chan struct{}
}

type HotReloadCallback func(key string, oldValue, newValue ConfigValue)

func (sdk *ConfigSDK) Initialize() error {
    // Step 1: Load full snapshot
    snapshot, etag, err := sdk.fetchSnapshot()
    if err != nil {
        // On failure: try local disk cache (written by previous run)
        snapshot, err = sdk.loadLocalCache()
        if err != nil {
            return fmt.Errorf("failed to load config and no local cache: %w", err)
        }
    }
    sdk.applySnapshot(snapshot)
    sdk.etag = etag

    // Step 2: Resolve vault references
    if err := sdk.resolveVaultRefs(); err != nil {
        return fmt.Errorf("vault ref resolution failed: %w", err)
    }

    // Step 3: Persist snapshot to local disk cache for startup resilience
    sdk.writeLocalCache(snapshot)

    // Step 4: Start background SSE listener
    go sdk.streamUpdates()

    return nil
}

func (sdk *ConfigSDK) streamUpdates() {
    backoff := 1 * time.Second
    for {
        select {
        case <-sdk.stopCh:
            return
        default:
        }

        err := sdk.consumeSSE()
        if err != nil {
            log.Warnf("SSE connection lost: %v, retrying in %v", err, backoff)
            time.Sleep(backoff)
            backoff = min(backoff*2, 60*time.Second)

            // On reconnect: do a full snapshot sync to catch any missed deltas
            if snapshot, etag, err := sdk.fetchSnapshot(); err == nil {
                sdk.applySnapshot(snapshot)
                sdk.etag = etag
            }
        } else {
            backoff = 1 * time.Second // Reset backoff on successful connection
        }
    }
}

func (sdk *ConfigSDK) consumeSSE() error {
    req, _ := http.NewRequest("GET",
        fmt.Sprintf("%s/sdk/stream/%s?env=%s", baseURL, sdk.namespace, sdk.environment), nil)
    req.Header.Set("X-Service-Key", sdk.serviceKey)
    req.Header.Set("Accept", "text/event-stream")
    req.Header.Set("Cache-Control", "no-cache")

    resp, err := http.DefaultClient.Do(req)
    if err != nil {
        return err
    }
    defer resp.Body.Close()

    scanner := bufio.NewScanner(resp.Body)
    var eventType, data string

    for scanner.Scan() {
        line := scanner.Text()
        if strings.HasPrefix(line, "event: ") {
            eventType = strings.TrimPrefix(line, "event: ")
        } else if strings.HasPrefix(line, "data: ") {
            data = strings.TrimPrefix(line, "data: ")
        } else if line == "" && data != "" {
            sdk.handleSSEEvent(eventType, data)
            eventType, data = "", ""
        }
    }
    return scanner.Err()
}

func (sdk *ConfigSDK) handleSSEEvent(eventType, data string) {
    switch eventType {
    case "config_updated":
        var update ConfigUpdateEvent
        if err := json.Unmarshal([]byte(data), &update); err != nil {
            log.Errorf("Failed to parse config update: %v", err)
            return
        }
        sdk.applyUpdate(update)
    case "snapshot_version":
        var ver SnapshotVersionEvent
        json.Unmarshal([]byte(data), &ver)
        if ver.Etag != sdk.etag {
            // Version mismatch: full re-sync
            if snapshot, etag, err := sdk.fetchSnapshot(); err == nil {
                sdk.applySnapshot(snapshot)
                sdk.etag = etag
            }
        }
    }
}

func (sdk *ConfigSDK) applyUpdate(update ConfigUpdateEvent) {
    sdk.mu.Lock()
    defer sdk.mu.Unlock()

    oldValue, _ := sdk.cache.Load(update.Key)
    newValue := ConfigValue{Raw: update.Value, Version: update.Version}

    // Resolve vault ref if the new value is a vault reference
    if strings.HasPrefix(update.Value.(string), "vault://") {
        resolved, err := sdk.vaultConnector.Resolve(update.Value.(string))
        if err != nil {
            log.Errorf("Failed to resolve vault ref for %s: %v", update.Key, err)
            return  // Keep old value; don't update to unresolved ref
        }
        newValue.Resolved = resolved
    }

    sdk.cache.Store(update.Key, newValue)

    // Invoke hot-reload callbacks
    for _, cb := range sdk.callbacks {
        go cb(update.Key, oldValue.(ConfigValue), newValue)
    }
}

func (sdk *ConfigSDK) Get(key string) (interface{}, bool) {
    val, ok := sdk.cache.Load(key)
    if !ok {
        return nil, false
    }
    cv := val.(ConfigValue)
    if cv.Resolved != nil {
        return cv.Resolved, true
    }
    return cv.Raw, true
}
```

**Interviewer Q&A:**

Q: What happens if the config service is down during application startup?
A: The SDK has a tiered fallback: (1) Try the config service API. (2) On failure, try the local disk cache (`/var/cache/configsdk/{namespace}.msgpack`) written by the previous successful load. (3) On cache miss (first-ever start), try the Kubernetes ConfigMap mounted at a well-known path. (4) If all fail, the SDK raises an error — the application either refuses to start (fail-fast for critical configs) or starts with hardcoded defaults (acceptable for optional configs). The SDK allows applications to declare which namespaces are required vs. optional.

Q: How do you prevent a bad config value from crashing all instances simultaneously?
A: The hot-reload callback is invoked per-instance as the SSE event is received. Applications implement the callback as a validation-then-apply pattern: validate the new value against application-level constraints before accepting it. If validation fails, the application logs a warning, retains the old value, and emits a metric `config.hot_reload.validation_failed`. An alert fires when this metric > 0. A recommended pattern: `config.getWithFallback("timeout_ms", 5000)` — returns 5000 if the key is missing or invalid. Additionally, staged rollout of config changes can be implemented by sending the SSE delta to a random 5% of connected SDKs first, monitoring error rates, then broadcasting to all.

Q: How does the SDK handle vault reference rotation when a Vault secret is rotated?
A: HashiCorp Vault issues secrets with a `lease_id` and `lease_duration`. The SDK's Vault Connector tracks all active leases. Before a lease expires, the connector calls `vault token renew` or `vault lease renew` to extend it (if the app's Vault policy allows). If a secret is rotated (the underlying secret value changes, which happens on a rotation policy), Vault fires a `vault-secret-rotated` event. The config service subscribes to Vault events (via Vault's event notification system or periodic polling), detects rotation, and publishes a `ConfigChangedEvent` for all config keys that reference the rotated vault path. SDKs re-resolve those vault references via the normal hot-reload path.

Q: How do you handle a config change that must be applied atomically across multiple keys (e.g., changing both a host and port simultaneously to point to a new server)?
A: The batch update API (`POST /namespaces/{ns}/batch`) applies all changes in a single PostgreSQL transaction. The Snapshot Builder rebuilds the snapshot only once after all keys are updated. The SSE Server sends a single `config_batch_updated` event with all changed keys. The SDK applies all changes under a write lock before invoking callbacks, ensuring the application never sees a half-updated state.

Q: Should the hot-reload callback be synchronous or asynchronous?
A: Asynchronous (called in a separate goroutine/thread), with a timeout enforced by the SDK. If the callback takes longer than 5 seconds, the SDK logs an error and continues. Synchronous callbacks would block the SSE event loop, preventing processing of subsequent updates. The recommended pattern for application code: the callback updates an `atomic.Value` (Go) or `AtomicReference` (Java) that the request handlers read on each request. This is a lock-free pattern that avoids contention between the hot-reload callback and the request handling threads.

---

### 6.2 Config Versioning and Rollback

**Problem it solves**: When a config change causes an incident, operators must be able to identify which change caused it and revert to a known-good state within 60 seconds.

**Approaches Comparison:**

| Approach | Version Storage | Rollback Mechanism | Query Capability |
|---|---|---|---|
| **Git-based versioning** | Store configs as files in a Git repo | Git revert/checkout | Full diff, blame, log |
| **DB append-only versions** | Each change inserts a new version row | Re-apply old version row as new write | SQL queries; schema enforcement |
| **Snapshot-based versioning** | Snapshot entire namespace at each change | Re-apply old snapshot | Efficient full-namespace rollback |
| **Event sourcing** | Store change events, not current state | Replay events up to a point in time | Powerful; complex to implement |

**Selected: DB append-only version rows (per-key) + namespace-level snapshot versioning**

Rationale: Per-key versioning allows surgical rollback of a single key without affecting others. Namespace snapshot versioning allows full-namespace rollback in one operation. Both are stored in Postgres; the version history table is append-only (no updates/deletes by the application).

**Implementation — version + rollback pseudocode:**

```python
def set_config_value(
    org_id: str, namespace_path: str, key: str, environment: str,
    new_value: Any, actor_id: str, change_reason: str
) -> int:
    """
    Atomically update a config value, record a version, and write an audit event.
    Returns the new version number.
    """
    # Validate against schema if one is attached
    schema = get_namespace_schema(org_id, namespace_path)
    if schema:
        errors = validate_against_schema(schema, key, new_value)
        if errors:
            raise ValidationError(errors)

    with db.transaction():
        # Lock the current value row for this key × environment
        current = db.query_one(
            "SELECT * FROM config_values WHERE config_key_id = %s AND environment_id = %s FOR UPDATE",
            get_config_key_id(org_id, namespace_path, key),
            get_environment_id(org_id, environment)
        )

        old_value = current.value if current else None
        new_version = (current.version + 1) if current else 1

        # Serialize value; encrypt if sensitive
        config_key_meta = get_config_key_meta(org_id, namespace_path, key)
        if config_key_meta.is_sensitive:
            stored_value = None
            encrypted_value = encrypt_aes256gcm(serialize(new_value))
        else:
            stored_value = serialize(new_value)
            encrypted_value = None

        # Upsert config_values (current value)
        db.execute("""
            INSERT INTO config_values
                (config_key_id, environment_id, value, encrypted_value, version, updated_by, updated_at)
            VALUES (%s, %s, %s, %s, %s, %s, NOW())
            ON CONFLICT (config_key_id, environment_id) DO UPDATE SET
                value = EXCLUDED.value,
                encrypted_value = EXCLUDED.encrypted_value,
                version = EXCLUDED.version,
                updated_by = EXCLUDED.updated_by,
                updated_at = EXCLUDED.updated_at
        """, current.config_key_id, current.environment_id,
             stored_value, encrypted_value, new_version, actor_id)

        # Append to version history (immutable)
        db.execute("""
            INSERT INTO config_versions
                (config_key_id, environment_id, version, value, encrypted_value,
                 changed_by, change_reason, created_at)
            VALUES (%s, %s, %s, %s, %s, %s, %s, NOW())
        """, current.config_key_id, current.environment_id, new_version,
             stored_value, encrypted_value, actor_id, change_reason)

        # Audit event
        db.execute("""
            INSERT INTO audit_events
                (org_id, actor_id, actor_type, event_type, namespace_path,
                 config_key, environment, before_value, after_value, change_reason, occurred_at)
            VALUES (%s, %s, 'user', 'config.value.updated', %s, %s, %s, %s, %s, %s, NOW())
        """, org_id, actor_id, namespace_path, key, environment,
             mask_if_sensitive(config_key_meta, old_value),
             mask_if_sensitive(config_key_meta, stored_value),
             change_reason)

    # After transaction commits: publish change event
    publish_config_change_event(
        org_id=org_id, namespace_path=namespace_path,
        key=key, environment=environment, new_version=new_version,
        new_value=stored_value  # vault refs go as-is; SDK resolves
    )

    return new_version

def rollback_config_key(
    org_id: str, namespace_path: str, key: str, environment: str,
    target_version: int, actor_id: str, change_reason: str
) -> int:
    # Fetch the target version's value
    target = db.query_one("""
        SELECT value, encrypted_value FROM config_versions
        WHERE config_key_id = (SELECT id FROM config_keys
                                WHERE namespace_id = (SELECT id FROM namespaces
                                                       WHERE org_id=%s AND path=%s)
                                AND key=%s)
          AND environment_id = (SELECT id FROM environments WHERE org_id=%s AND slug=%s)
          AND version = %s
    """, org_id, namespace_path, key, org_id, environment, target_version)

    if not target:
        raise ValueError(f"Version {target_version} not found")

    # Decrypt if needed
    rollback_value = decrypt_if_encrypted(target) if target.encrypted_value else deserialize(target.value)

    # Apply as a new write (creates a new version number, preserving full history)
    return set_config_value(
        org_id, namespace_path, key, environment,
        rollback_value, actor_id,
        change_reason or f"Rollback to version {target_version}"
    )
```

**Interviewer Q&A:**

Q: Rollback creates a new version record rather than reverting to the old one. Why?
A: The version history is an append-only audit trail. Deleting or rewriting history would violate the auditability requirement. By creating a new version with the rolled-back value, we preserve the full change history: "at version N, the value was X; at version N+1 (due to a mistake), it became Y; at version N+2 (rollback), it returned to X." This is how Postgres itself handles MVCC, and how Git handles reverts.

Q: How do you implement namespace-level rollback efficiently (rolling back 1,000 keys at once)?
A: The batch rollback fetches all `config_versions` records for the target namespace × environment at the target version timestamp (`created_at <= target_timestamp`), taking the maximum version number for each key at or before that timestamp. Then it applies all updates in a single transaction using the batch update API. For a 1,000-key namespace, this is a single SQL query for version lookup and a single transaction with 1,000 upserts — typically under 200ms.

---

### 6.3 Secret Management Integration (vault:// References)

**Problem it solves**: Developers want to store a database password in the config service alongside non-sensitive configs, without the config service storing the plaintext secret.

**Implementation — vault reference resolution:**

```python
VAULT_REF_PATTERN = re.compile(r'^vault://(?P<path>[^?]+)(\?field=(?P<field>\w+))?$')

class VaultConnector:
    def __init__(self, vault_addr: str, role_id: str, secret_id: str):
        self.vault_addr = vault_addr
        self._client = hvac.Client(url=vault_addr)
        # Authenticate via AppRole
        resp = self._client.auth.approle.login(role_id=role_id, secret_id=secret_id)
        self._token = resp['auth']['client_token']
        self._lease_duration = resp['auth']['lease_duration']
        self._lease_start = time.time()
        self._cache: Dict[str, CachedSecret] = {}
        self._lock = threading.Lock()

    def resolve(self, vault_ref: str) -> str:
        """Resolve a vault:// reference to its actual value."""
        match = VAULT_REF_PATTERN.match(vault_ref)
        if not match:
            raise ValueError(f"Invalid vault reference: {vault_ref}")

        path = match.group('path')
        field = match.group('field') or 'value'
        cache_key = f"{path}#{field}"

        with self._lock:
            cached = self._cache.get(cache_key)
            if cached and cached.expires_at > time.time():
                return cached.value

            # Fetch from Vault
            secret = self._client.secrets.kv.v2.read_secret_version(path=path)
            value = secret['data']['data'][field]
            lease_duration = secret['data']['metadata'].get('lease_duration', 3600)

            # Cache with 80% of lease duration to ensure proactive refresh
            self._cache[cache_key] = CachedSecret(
                value=value,
                expires_at=time.time() + (lease_duration * 0.8)
            )
            return value

    def renew_token(self):
        """Called by background thread before token expiry."""
        resp = self._client.auth.token.renew_self()
        self._token = resp['auth']['client_token']
        self._lease_duration = resp['auth']['lease_duration']
        self._lease_start = time.time()
```

When the SDK builds the snapshot for an application, vault references in config values are NOT resolved by the config service — they're passed through as `vault://...` strings. The SDK resolves them in-process using the application's own Vault credentials (AppRole). This means the config service never sees the plaintext secret. Two applications can have different Vault permissions for the same `vault://` reference — one may have access, the other won't.

**Interviewer Q&A:**

Q: What if a vault reference becomes invalid (the vault path is deleted or the token is revoked)?
A: The SDK logs an error and retains the last successfully resolved value for the duration of the `vault_error_retention_seconds` window (default: 300 seconds). After this window, if Vault is still unreachable, the SDK invokes the error callback and the application may choose to shut down or continue with stale secrets. An alert fires immediately on vault resolution failure. This graceful degradation prevents an instantaneous outage if Vault has a brief network blip.

---

## 7. Scaling

### Horizontal Scaling

- **Management API**: Stateless; scale behind ALB. Write rate is < 1 req/sec — 2 instances for HA is sufficient.
- **Config Read API / SDK API**: Stateless; scale based on startup burst. At 232 loads/sec peak × 1 MB = 232 MB/s, served from Redis cache. With 50ms average serve time, need ~12 API server instances.
- **SSE Server**: 100,000 persistent connections. Each async I/O server instance handles 10,000 connections = 10 instances. Sticky session routing by `{namespace_id}:{env_id}`.
- **Snapshot Builder**: Kafka consumer; scale by adding consumer instances. At < 1 change/sec, single instance is sufficient with a hot standby.

### DB Sharding

- Shard by `org_id`. Config key count per org: up to 100,000 × 10 environments = 1M version records/org. With 500 orgs, single Postgres cluster at 500M rows. At ~500 bytes/row average = 250 GB — fits in a large Postgres instance without sharding. Apply sharding when approaching 2 TB.
- Read replicas: 2 per shard for Dashboard reads (version history, audit log queries).

### Caching

| Cache Layer | Data | TTL | Invalidation |
|---|---|---|---|
| Redis (L1) | Namespace snapshots (per org × namespace × env) | Indefinite, invalidated on write | Config change event triggers rebuild and overwrite |
| S3 (L2) | Large snapshots > 10 MB | Indefinite | Overwritten on change |
| CDN (L3) | Snapshots for public/anonymous configs (no vault refs) | 60 s | Cache-Control header on API response |
| SDK in-process | All config values for subscribed namespace | Until SSE update received | SSE event triggers applyUpdate() |
| SDK local disk | Last successful snapshot (startup fallback) | Until SDK overwrites | Written on every successful snapshot load |

**Interviewer Q&As:**

Q: A developer accidentally pushed a config change that took down 10,000 services. How do you prevent blast radius at this scale?
A: Several safeguards: (1) Staged config rollout: the SSE server can be configured to send an update to a random 1% of connected SDKs first. Monitor error rates for 60 seconds before broadcasting to all. (2) Config change approval workflow: production config changes require a second approver. (3) Schema validation rejects obviously invalid values (e.g., a string where an integer is expected). (4) SDK validation before applying: the hot-reload callback validates the new value against application-defined constraints before updating the in-memory cache. (5) Kill switch: a separate "disable hot reload" config key can prevent any further changes from propagating (useful during an active incident).

Q: How do you handle namespace snapshot cache invalidation with strong consistency guarantees?
A: The Snapshot Builder writes the new snapshot to Redis atomically using a Lua script that sets the new value and increments an ETag counter. SDK polling requests use If-None-Match; if the ETag hasn't changed since the last request, the server returns 304 immediately without re-reading from Redis. The ETag is a SHA-256 of the snapshot content, computed by the Snapshot Builder. There is no cache invalidation race: the snapshot is always built from the current Postgres state (not from a diff), so a concurrent write that happens between two snapshot builds results in both being correctly serialized.

Q: How do you optimize read performance for the startup burst (232 SDK loads/sec)?
A: The Config Read API serves snapshots directly from Redis. Redis GET for a 1 MB value takes < 1ms. At 232 req/sec and 10ms avg response time (network + Redis read + serialization), need ~3 server instances. For even higher burst, CDN caching: for namespaces without vault references, the snapshot can be cached at Cloudflare with a 30-second TTL. A deployment of 10,000 instances all starting simultaneously would be served by the CDN edge, not the origin.

Q: How do you handle the SSE server being a sticky-session bottleneck when an org has 50,000 application instances?
A: With 50,000 connections per org × namespace, and each SSE instance handling 10,000 connections, 5 instances are needed for that org's namespace. Sticky session routing by `hash(org_id + namespace_id) % N_instances` ensures all instances for an org × namespace land on the same set of SSE servers. To handle this, the sticky session hash maps to a range of servers (a "virtual node" approach similar to consistent hashing), so a single org's 50,000 connections are spread across 5 dedicated SSE instances while other orgs use other instances.

Q: How do you prevent the config service from becoming a SPOF for application startup?
A: Defense in depth: (1) SDK local disk cache: if the config service is down, the SDK loads the last-written snapshot from disk. (2) Kubernetes ConfigMap sync: even if the config service is down, the ConfigMap was last synced and is available to the pod. (3) CDN caching: non-sensitive configs are cached at the CDN edge with a 60-second TTL. (4) Config service itself is deployed across 3 AZs with auto-failover. (5) Applications can declare `startup_mode: "best_effort"` to start with defaults if the config service is unreachable, logging a warning rather than refusing to start.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Detection | Recovery |
|---|---|---|---|
| Management API | Crash | ALB health check | Other instance serves; no writes during failover (< 5s) |
| Postgres primary | Crash | Patroni | Failover to replica; < 30s; pending writes retry with backoff |
| Redis cluster | Node failure | Redis Sentinel | Replica promotion; SDK serves from in-process cache during gap |
| Kafka broker | Crash | Controller rebalance | Change events may be delayed; Snapshot Builder resumes from offset on restart |
| SSE Server | Crash | Kubernetes liveness probe | SDKs reconnect with backoff; trigger full snapshot re-sync on reconnect |
| Vault | Unavailable | Vault Connector health check | SDK serves cached resolved secrets for `vault_error_retention_seconds`; alert fires |
| S3 (snapshot storage) | Throttling | Snapshot Builder upload error | Retry with exponential backoff; Redis copy (for small snapshots) remains valid |
| CDN | Cache poisoning / incorrect eviction | Monitor cache hit rate | Set short TTL (60s); poisoned responses expire quickly |

### Idempotency

- Config write is idempotent for identical values: writing the same value as the current value produces a new version record but with no actual change to the snapshot. The Snapshot Builder detects this and skips the snapshot rebuild (ETag doesn't change because snapshot content didn't change).
- Snapshot Builder is idempotent: re-processing the same Kafka change event produces the same Redis snapshot content. The ETag computation prevents unnecessary SSE pushes.

### Circuit Breaker

- The Vault Connector wraps Vault API calls in a circuit breaker. After 3 consecutive vault resolution failures within 60 seconds, it enters Open state, serving only cached secrets. It attempts to reconnect every 30 seconds (Half-Open). This prevents thundering herd on Vault during a brief vault outage.
- The SDK wraps the config service API in a circuit breaker. After 5 failed snapshot fetches within 120 seconds, the SDK enters "offline mode," serving exclusively from disk cache, and retries the API every 60 seconds.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Purpose |
|---|---|---|---|
| `config.write.latency_p99` | Histogram | > 500ms | Management API performance |
| `config.propagation.latency_p99` | Histogram | > 5s | Hot reload SLA |
| `sdk.startup.load_latency_p99` | Histogram | > 500ms | SDK startup performance |
| `sdk.connections.active` | Gauge | — | SSE server capacity |
| `sdk.hot_reload.validation_failed_rate` | Counter rate | > 0% | Bad config values propagated |
| `sdk.offline_mode.instance_count` | Gauge | > 1% of fleet | SDK-to-service connectivity |
| `vault.resolution.error_rate` | Counter rate | > 0% | Vault health |
| `vault.secret.cache_hit_rate` | Gauge | < 90% | Vault resolution performance |
| `config.schema_validation.rejection_rate` | Counter rate | > 5% | Config write quality |
| `config.rollback.frequency` | Counter | > 3/hour | High incident rate in config changes |
| `postgres.replication.lag_seconds` | Gauge | > 10s | Read replica staleness |
| `redis.snapshot.stale_age_seconds` | Gauge | > 30s | Snapshot not rebuilt after expected change |

### Distributed Tracing

Every config write generates a trace that spans: Management API write → Postgres transaction → Kafka publish → Snapshot Builder rebuild → Redis write → SSE push to first SDK. The propagation latency metric (from Postgres write to SSE push timestamp) is computed from the trace.

### Logging

- All config writes (Management API): structured log with `org_id`, `namespace_path`, `key`, `environment`, `actor_id`, `old_value` (masked if sensitive), `new_value` (masked if sensitive), `duration_ms`.
- SDK connectivity events: `INFO` on connect/disconnect; `WARN` on reconnect with backoff; `ERROR` on entering offline mode.
- Vault resolution failures: `ERROR` with vault path (not the secret value) and error message.
- Schema validation failures: `WARN` with the failed validation rule details; `ERROR` if schema itself is invalid.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|---|---|---|---|
| Config storage | PostgreSQL | etcd / Consul KV | 50M keys exceeds etcd storage limits; PostgreSQL provides SQL querying for version history and audit |
| Secret handling | vault:// references resolved in SDK | Store encrypted secrets in config service | Config service never holds plaintext secrets; each application's Vault permissions determine access independently |
| Version history | Append-only `config_versions` table | Git repo | SQL queries for specific version ranges; transactional audit writes; no Git clone overhead |
| Hot reload | SSE (server-push) | Polling | Sub-second propagation vs. N-second polling lag; polling acceptable only for mobile/edge cases |
| SDK startup resilience | Local disk cache | Fail-fast on service unavailable | Applications must start during a config service outage; disk cache ensures no hard dependency on availability |
| Snapshot format | MessagePack | JSON | 60% smaller; faster SDK deserialization reduces startup time |
| Vault integration | AppRole authentication per application | Shared vault token | Each application's Vault token is scoped to only the secrets it needs; a compromised token doesn't expose all secrets |
| Batch update atomicity | Single DB transaction | Sequential individual writes | Prevents half-applied config states; applications see all-or-nothing updates |
| Schema validation | At write time (synchronous) | At read time (in SDK) | Fail early; prevent invalid configs from entering the store at all |

---

## 11. Follow-up Interview Questions

**Q1: How do you handle config that has different types in different environments (e.g., integer in production, string in dev)?**
A: The schema is enforced per-namespace, not per-environment. If the schema declares `timeout_ms: integer`, both dev and production values must be integers. If you need type-per-environment flexibility, the key should have `value_type: json` with environment-specific schemas. However, this is usually a configuration management anti-pattern — configs should have consistent types across environments for type-safe SDK access.

**Q2: How would you design a config promotion workflow that requires diff approval before applying?**
A: The promote operation has a two-phase API: `GET /promote/preview` (returns the diff without applying) and `POST /promote` (applies after approval). For orgs with approval policies, the `POST /promote` stores the proposed change as a `pending_promotion` record. An approver reviews the diff and calls `POST /approve/{promotion_id}`. Only then does the system apply the changes. Automatic approval is supported for changes that reduce values (e.g., timeout reduction is auto-approved; timeout increase requires manual review).

**Q3: How do you support GitOps-style config management where configs are defined in a Git repository?**
A: Implement a bidirectional sync. A Git push to the config repository (e.g., `configs/production/payments/gateway.yaml`) triggers a CI pipeline that calls the config service API to update keys. The config service's change events can also write back to Git (via a bot commit) for the other direction. For a strictly GitOps model, the config service is read-only to applications and all writes go through Git PR → CI pipeline → config service API. This provides Git's full review history and rollback (git revert) in addition to the config service's own versioning.

**Q4: How do you detect and alert on config drift (staging and production diverging significantly)?**
A: A daily drift analysis job computes the diff between all staging and production namespace snapshots for all orgs. Keys that exist in one environment but not the other, or have values that differ by more than a configured threshold, are flagged. Results are surfaced in the Management UI as a "Config Drift" report and can trigger Slack/email alerts. For critical namespaces (e.g., `security/*`), drift alerts can be made blocking (pipeline fails if staging and production are out of sync on specific keys).

**Q5: How would you implement config templating that resolves cross-key references?**
A: Define a template syntax: `"endpoint": "https://{{db.host}}:{{db.port}}/mydb"`. At snapshot build time, the Snapshot Builder resolves templates by looking up referenced keys within the same namespace. If `db.host = "db1.example.com"` and `db.port = 5432`, the resolved value is `"https://db1.example.com:5432/mydb"`. The snapshot stores both the raw template string and the resolved value. The SDK returns the resolved value by default; the raw template is accessible via `config.getRaw("endpoint")`. Circular references are detected during schema validation (same algorithm as flag prerequisite cycle detection).

**Q6: How do you handle extremely large config values (e.g., an RSA public key or a TLS certificate chain)?**
A: Config values are stored as text in PostgreSQL. A single config value is capped at 1 MB (enforced at the API layer). RSA keys (~2 KB) and TLS cert chains (~10 KB) are well within limits. For truly large blobs (e.g., a 10 MB configuration file), store the file in S3 and store only the S3 URL (or S3 presigned URL reference) as the config value. The SDK downloads the referenced file on demand and caches it locally.

**Q7: How would you support multi-region deployments where each region should read from a local config service instance?**
A: Deploy a config service instance in each region. Primary writes go to the primary region's PostgreSQL. Cross-region replication (Postgres streaming replication or logical replication to read replicas in secondary regions) propagates changes within 1-5 seconds. SDKs in each region connect to the local config service instance for reads. Writes always go to the primary region. For disaster recovery (primary region failure), promote the secondary region's Postgres to primary and update the write endpoint DNS record (failover RTO: ~60 seconds).

**Q8: How do you handle sensitive config that should never appear in logs, even in error messages?**
A: At the schema level, mark keys as `is_sensitive: true`. The audit log stores `"[REDACTED]"` for the before/after values of sensitive keys. The SDK marks resolved vault values with an `IsSensitive` flag; the value's `String()` method returns `"[REDACTED]"` to prevent accidental logging. Application code should use `config.getSecretValue()` instead of `config.get()` for sensitive keys — the SDK enforces that sensitive values are never accessible as plain strings, only through a `SecretValue` wrapper that controls how the value is used (e.g., write to HTTP headers but not to logs).

**Q9: How do you version the config schema itself (what happens when you need to rename a config key)?**
A: Key renames are backward-incompatible changes. The recommended pattern: (1) Create the new key alongside the old key, set both to the same value. (2) Update application code to read the new key (with a fallback to the old key for backward compat). (3) Deploy updated application code. (4) Archive the old key. The config service supports a `deprecated_alias` field on config keys: `config.get("db_timeout_ms")` transparently falls back to `config.get("database.timeout_ms")` if the new key is not found, with a deprecation warning in the SDK logs.

**Q10: How would you implement per-tenant configuration for a multi-tenant SaaS (each customer has different config values)?**
A: Add a `tenant_id` dimension to the config lookup. The namespace hierarchy: `{service}/{component}/{tenant_id}/{key}`. The SDK API key is scoped to a specific tenant. SDK reads return tenant-specific values where present, falling back to the service-level default if the tenant doesn't have an override. For 10,000 tenants × 100 config keys = 1M per-tenant config values per service — manageable in PostgreSQL. The snapshot includes only the effective values (base + tenant overrides merged), not all raw overrides, to keep SDK memory footprint small.

**Q11: How would you support automated config testing (CI/CD pipeline validates configs before deploying)?**
A: Add a `validate` endpoint: `POST /namespaces/{ns}/validate` accepts a JSON object of proposed config values and checks them against the schema, returning pass/fail with detailed errors. The CI/CD pipeline runs this in a pre-deploy step. Additionally, implement config property-based tests: define invariants (e.g., `timeout_ms > 100`, `max_connections >= min_connections`) as part of the schema's `assertions` field. These are enforced both at write time and in the CI validation step.

**Q12: How do you manage config access control at a granular level (per-key permissions, not just per-namespace)?**
A: The `access_policies` table supports wildcard namespace paths. For per-key granularity, use the full key path as the namespace path: `payments/gateway/stripe_webhook_secret`. Grant read access only to the payments service's service account for this specific key. For most use cases, namespace-level permissions are sufficient. Per-key permissions are an advanced feature for particularly sensitive individual values (production database passwords, signing keys).

**Q13: How do you handle config changes during a network partition (some SDK instances can't reach the config service)?**
A: During a partition, unreachable SDKs serve from their in-memory cache (stale). When the partition heals, the SSE connection reconnects, downloads the full snapshot, and applies all changes that occurred during the partition. The SDK's local disk cache is also updated at this point. Monotonic version numbers ensure the SDK can detect if it missed changes (current in-memory version N, new snapshot has version N+5 — indicating 4 changes were missed). All missed changes are applied atomically from the fresh snapshot rather than trying to replay individual deltas.

**Q14: What's the right TTL for the local disk cache?**
A: The local disk cache has no TTL — it is valid indefinitely as a startup fallback. Its staleness is bounded by the last time the application successfully connected to the config service. The SDK records the `snapshot_timestamp` in the cache metadata. On startup with a cache hit, the SDK logs: `"Starting with cached config from {timestamp}. Connecting to config service for updates."` For compliance-sensitive configs, operators can set a `max_cache_age_hours` parameter: if the cache is older than this threshold, the SDK refuses to start from cache and requires a fresh fetch.

**Q15: How would you design config rollback for a batch of related changes across multiple namespaces?**
A: Introduce "change sets" — a named group of config changes applied atomically. The change set is created as a transaction record that references all the individual config value versions it modified. Rollback of a change set atomically rolls back all affected keys to their pre-changeset versions in a single database transaction. This supports the common pattern of a feature launch changing configs across multiple services simultaneously (e.g., enabling a payment provider requires changes in `payments/gateway`, `notifications/email`, and `billing/invoicing` namespaces).

---

## 12. References & Further Reading

- **etcd Documentation — Configuration Store for Kubernetes**: https://etcd.io/docs/ — Reference design for distributed configuration with Raft consensus; useful for understanding strong consistency trade-offs.
- **HashiCorp Consul KV Store**: https://developer.hashicorp.com/consul/docs/dynamic-app-config/kv — Distributed config with health checking integration.
- **HashiCorp Vault — AppRole Authentication**: https://developer.hashicorp.com/vault/docs/auth/approle — Basis for service-to-vault authentication in the connector design.
- **HashiCorp Vault — KV Secrets Engine**: https://developer.hashicorp.com/vault/docs/secrets/kv/kv-v2
- **Spring Cloud Config (reference implementation)**: https://spring.io/projects/spring-cloud-config — JVM ecosystem centralized config; comparison point for the hierarchical override model.
- **Netflix Archaius (dynamic configuration management)**: https://github.com/Netflix/archaius — Battle-tested Java config management library with hot reload; influenced the SDK callback design.
- **JSON Schema Specification**: https://json-schema.org/specification — Basis for the config validation schema format.
- **Server-Sent Events (SSE) — MDN Web Docs**: https://developer.mozilla.org/en-US/docs/Web/API/Server-sent_events/Using_server-sent_events
- **AWS Systems Manager Parameter Store**: https://docs.aws.amazon.com/systems-manager/latest/userguide/systems-manager-parameter-store.html — Production reference for hierarchical config with versioning and environment-specific overrides.
- **Kubernetes ConfigMap**: https://kubernetes.io/docs/concepts/configuration/configmap/ — Kubernetes-native application config mechanism integrated with the sync operator.
- **PostgreSQL MVCC and SELECT FOR UPDATE**: https://www.postgresql.org/docs/current/explicit-locking.html — Mechanism preventing lost updates in concurrent config writes.
- **NIST SP 800-57 — Key Management Guidelines**: https://csrc.nist.gov/publications/detail/sp/800-57-part-1/rev-5/final — Guidance on encryption key rotation relevant to encrypted sensitive config values.
- **MessagePack specification**: https://msgpack.org/index.html
- **inotify Linux man page**: https://man7.org/linux/man-pages/man7/inotify.7.html — Basis for file-based hot reload in the sidecar component.
