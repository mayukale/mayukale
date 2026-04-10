# System Design: Feature Flag Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Flag Management**: Create, update, archive, and delete feature flags with types: boolean, string, number, and JSON variants.
2. **Targeting Rules**: Evaluate flags against per-request context (user ID, org ID, email, country, plan tier, custom attributes). Support rule operators: equals, not equals, contains, starts with, ends with, regex match, numeric comparison (>, <, >=, <=, between), set membership (in, not in).
3. **Percentage Rollout**: Serve a flag variant to a deterministic X% of users using consistent hash-based bucketing, so the same user always gets the same variant.
4. **Segmentation**: Define reusable user segments (named groups of targeting rules) and reference them in multiple flags.
5. **Kill Switch**: Allow instant disabling of any flag globally, overriding all targeting rules, with propagation to all SDKs within 1 second.
6. **Configuration Propagation**: Push flag configuration updates to all SDK instances within 5 seconds of a change.
7. **Audit Trail**: Record every flag change (create, update, delete, enable, disable) with actor, timestamp, before/after values, and change reason.
8. **Experimentation Integration**: Associate flags with A/B test experiments; record flag evaluation events for metric computation in the A/B testing platform.
9. **SDK Support**: Provide server-side SDKs (Node.js, Python, Go, Java, Ruby) and client-side SDKs (JavaScript/browser, React Native, iOS, Android).
10. **Environment Support**: Each flag exists independently per environment (development, staging, production) with separate configurations and targeting rules.
11. **Prerequisite Flags**: Allow a flag to be gated on the result of another flag (flag B only evaluates if flag A returns true for this user).

### Non-Functional Requirements

- **SDK Evaluation Latency**: Flag evaluation must complete in under 1ms for server-side SDKs (in-process, no network call). For client-side SDKs, initial flag load must complete in under 100ms (p99).
- **Availability**: 99.99% uptime. Flag evaluation must continue even if the flag management service is completely down (SDK caches must cover outages).
- **Propagation Latency**: Flag changes must propagate to all connected SDK instances within 5 seconds (p99). Kill switch must propagate within 1 second.
- **Throughput**: Support 1 trillion flag evaluations per day across all customers (a large e-commerce platform may evaluate 50+ flags per page render, millions of page views/day).
- **Consistency**: For a given user, the same flag must return the same variant throughout a single user session (sticky sessions), even if the flag is being rolled out.
- **Multi-tenancy**: Fully isolated flag configurations per customer organization; no cross-org data leakage.
- **Flag Count**: Support up to 10,000 active flags per environment per organization.

### Out of Scope

- A/B test statistical analysis (delegated to the A/B Testing Platform).
- Full experiment metric computation (this service emits events; computation is elsewhere).
- General application configuration management (see config_management.md).
- User identity and authentication management.
- UI analytics and session recording.

---

## 2. Users & Scale

### User Types

| User Type | Description | Primary Actions |
|---|---|---|
| Product Manager | Defines flags, manages rollouts | Create flags, adjust rollout percentages, archive flags |
| Developer | Implements flag in code, debugs evaluation | Create flags, test flag values, view evaluation logs |
| Release Engineer | Controls release exposure | Toggle flags, set kill switches, approve rollout increases |
| SDK (Server-side) | Calls flag evaluation in-process | Initialize SDK with all flags, evaluate flags per request |
| SDK (Client-side) | Fetches flags on app load | Fetch personalized flag values for a specific user |
| Data Scientist | Associates flags with experiments | Connect flags to A/B tests, view evaluation statistics |

### Traffic Estimates

Assumptions:
- 500 customer organizations.
- Average 10M daily active users (DAU) per large org, 50 large orgs = 500M DAU total at scale.
- Realistically at launch: 50 orgs × 1M DAU = 50M DAU.
- Each user triggers ~200 flag evaluations/day (server-side, in-process — no network I/O).
- 1,000 flag configuration changes/day across all orgs (low compared to evaluations).
- 50,000 SDK instances connected (server fleet + client SDKs).

| Metric | Calculation | Result |
|---|---|---|
| Flag evaluations/day | 50M DAU × 200 evaluations | 10 billion/day |
| Flag evaluations/sec (avg) | 10B / 86,400 | ~115,740/sec |
| Flag evaluations/sec (peak 5x) | 115,740 × 5 | ~578,700/sec |
| Config change events/day | 1,000 changes × 50 orgs | 50,000/day |
| Config change events/sec | 50,000 / 86,400 | ~0.6/sec (negligible) |
| SDK streaming connections | 50,000 SDK instances | 50,000 persistent connections |
| Client-side SDK flag loads | 50M DAU × 2 loads/day (app open) | 100M/day = ~1,157/sec avg |
| Client-side SDK loads peak | ~1,157 × 10 (morning spike) | ~11,570/sec |

Note: All 10 billion evaluations/day are computed **in-process** by the SDK using a locally cached copy of flag rules. They never reach the flag management servers. Only flag configuration syncs (50,000 events/day) and client-side flag loads (100M/day) generate network traffic.

### Latency Requirements

| Operation | Target (p50) | Target (p99) | Notes |
|---|---|---|---|
| Server-side flag evaluation | < 0.1 ms | < 1 ms | In-process; no network hop |
| Client-side initial flag load | 20 ms | 100 ms | Network fetch of personalized flags |
| Flag change propagation to SDK | 1 s | 5 s | Via SSE or polling |
| Kill switch propagation | < 500 ms | 1 s | Priority propagation channel |
| Flag CRUD API response | 50 ms | 200 ms | Management UI responsiveness |
| Audit log query | 100 ms | 500 ms | Paginated historical read |

### Storage Estimates

| Data Type | Size per Unit | Volume | Retention | Total |
|---|---|---|---|---|
| Flag definitions | ~5 KB/flag (JSON with rules) | 10,000 flags × 500 orgs × 10 envs = 50M records | Indefinite (versioned) | 50M × 5 KB = 250 GB |
| Segment definitions | ~2 KB/segment | ~1,000 segs × 500 orgs × 10 envs = 5M records | Indefinite | 5M × 2 KB = 10 GB |
| Audit events | ~1 KB/event | 50,000 events/day | 7 years | 50,000 × 365 × 7 × 1 KB = ~127 GB |
| Flag evaluation events (for A/B) | ~200 B/event | 10B/day × 5% sampled = 500M/day | 90 days | 500M × 200 B × 90 = ~9 TB |
| SDK config cache (per-env snapshot) | ~5 KB/flag × 10,000 flags = 50 MB/env | 500 orgs × 10 envs = 5,000 snapshots | Latest only | 5,000 × 50 MB = 250 GB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| SDK initial config download | 50 MB/snapshot × 50,000 new SDK instances/day / 86,400 | ~29 MB/s avg |
| SDK config update push (SSE delta) | ~5 KB delta × 0.6 changes/sec × 50,000 connections | ~150 MB/s at peak change burst |
| Client-side flag load | 11,570 req/s × ~2 KB response | ~22 MB/s peak |
| Flag evaluation events to A/B platform | 500M events/day × 200 B / 86,400 | ~1.16 GB/s (Kafka ingestion) |

---

## 3. High-Level Architecture

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                              FLAG MANAGEMENT PLANE                                     │
│                                                                                        │
│  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐                   │
│  │  Management UI   │   │  Management API  │   │  Auth Service    │                   │
│  │  (React SPA)     │──▶│  (REST, JWT-gated│──▶│  (OAuth 2.0,     │                   │
│  │                  │   │   CRUD + rules)  │   │   RBAC)          │                   │
│  └──────────────────┘   └────────┬─────────┘   └──────────────────┘                   │
│                                  │                                                     │
│                                  ▼                                                     │
│  ┌──────────────────────────────────────────────────────────────────────┐              │
│  │                      Flag Store (PostgreSQL)                         │              │
│  │  flags | flag_rules | segments | segment_rules | environments |      │              │
│  │  flag_versions | audit_events | evaluation_stats                     │              │
│  └──────────────────────────────┬───────────────────────────────────────┘              │
│                                 │ on write: publish change event                       │
│                                 ▼                                                      │
│  ┌──────────────────────────────────────────────────────────────────────┐              │
│  │                   Change Event Bus (Kafka)                           │              │
│  │  Topic: flag-changes (partitioned by org_id)                        │              │
│  └──────────────────────────────┬───────────────────────────────────────┘              │
└────────────────────────────────────────────────────────────────────────────────────────┘
                                  │
                    ┌─────────────┼──────────────┐
                    │             │              │
                    ▼             ▼              ▼
┌───────────────────────────────────────────────────────────────────────────────────────┐
│                             FLAG DELIVERY PLANE                                        │
│                                                                                        │
│  ┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐                   │
│  │  Config Snapshot │   │  SSE / Streaming │   │  Polling         │                   │
│  │  Service         │   │  Server          │   │  Endpoint        │                   │
│  │  (builds full    │   │  (persistent     │   │  (/v1/flags/poll │                   │
│  │   env snapshots) │   │   connections)   │   │   ?etag=...)      │                   │
│  └──────────┬───────┘   └──────────────────┘   └──────────────────┘                   │
│             │                     │                      │                             │
│             ▼                     │                      │                             │
│  ┌──────────────────┐             └──────────────────────┘                            │
│  │  Config Cache    │                      │                                           │
│  │  (Redis per-env  │◀─────────────────────┘                                          │
│  │   snapshot)      │   invalidated on change                                         │
│  └──────────────────┘                                                                  │
│                                                                                        │
│  ┌──────────────────────────────────────────────────────────────────────┐              │
│  │                   Client-Side Flag API                               │              │
│  │  POST /v1/sdk/evaluate  (user context → personalized flag values)   │              │
│  │  GET  /v1/sdk/flags?env=prod (full env snapshot for bootstrapping)  │              │
│  └──────────────────────────────────────────────────────────────────────┘              │
└───────────────────────────────────────────────────────────────────────────────────────┘
                                  │
                    ┌─────────────┼──────────────┐
                    │             │              │
                    ▼             ▼              ▼
          ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
          │ Server-Side  │  │ Client-Side  │  │ Client-Side  │
          │ SDK          │  │ JS/Browser   │  │ Mobile       │
          │ (Node/Python │  │ SDK          │  │ SDK (iOS/    │
          │  /Go/Java)   │  │              │  │  Android)    │
          │              │  │              │  │              │
          │ In-process   │  │ Fetches on   │  │ Fetches on   │
          │ evaluation   │  │ page load,   │  │ app open,    │
          │ (no network) │  │ SSE updates  │  │ polling      │
          └──────────────┘  └──────────────┘  └──────────────┘
                    │
                    ▼
          ┌──────────────────────────────────────┐
          │   Event Recorder (Kafka Producer)    │
          │   flag_evaluation_events topic        │
          │   (sampled, for A/B integration)      │
          └──────────────────────────────────────┘
```

**Component Roles:**

- **Management API**: CRUD for flags, rules, segments, environments. Validates rule logic (no circular prerequisites), persists changes to Postgres, and publishes `FlagChangedEvent` to Kafka on every mutation.
- **Flag Store (PostgreSQL)**: Source of truth for all flag definitions, versioning, and audit history. Write patterns are low-frequency; read patterns are dominated by the Config Snapshot Service.
- **Change Event Bus (Kafka)**: Decouples Management API from the delivery plane. Allows multiple consumers (Config Snapshot Service, SSE Server, audit processor) to react to changes independently.
- **Config Snapshot Service**: Consumes `FlagChangedEvent` from Kafka, rebuilds the full environment snapshot (all flags for org × environment), serializes to a compact binary format (MessagePack), and writes to Redis.
- **Config Cache (Redis)**: Per-org per-environment snapshot stored as a Redis key (e.g., `snapshot:{org_id}:{env_id}`). SDKs download this snapshot at startup and receive incremental updates. The key is versioned via an ETag (SHA-256 of snapshot content).
- **SSE Streaming Server**: Maintains persistent Server-Sent Events connections with server-side SDK instances. When a flag changes, it pushes a delta payload (only the changed flags, not the full snapshot) to all connected SDKs for the affected org × environment.
- **Polling Endpoint**: For SDKs that cannot maintain persistent connections (mobile, embedded). SDKs send `If-None-Match: <etag>` header; the endpoint returns `304 Not Modified` if unchanged, or the new snapshot with a new ETag if changed.
- **Client-Side Flag API**: Accepts a user context (user ID, attributes) and returns personalized flag values for that user, evaluated server-side. Client-side SDKs call this on app initialization to avoid shipping evaluation logic (and business rules) to the client.
- **Server-Side SDK**: Downloads the full flag ruleset for the org × environment at startup. Evaluates flags in-process (sub-millisecond) using the locally cached ruleset. Reconnects and re-downloads on disconnect. Ships with the evaluation engine embedded.
- **Event Recorder**: The SDK (or a sidecar process) emits sampled flag evaluation events to Kafka for consumption by the A/B Testing Platform.

**Primary Use-Case Data Flow (server-side SDK evaluates a flag):**

1. Server-side SDK initializes: calls `GET /v1/sdk/flags?env=production` with SDK API key. Returns the full snapshot (50 MB compressed). SDK stores in memory.
2. SDK opens SSE connection to `/v1/sdk/stream?env=production`. Holds this connection open.
3. Application code calls `flags.evaluate("checkout-redesign", user_context)` — entirely in-process, no network call. Returns `{ "variant": "treatment", "reason": "targeting_rule_3" }` in < 0.1ms.
4. SDK (optionally, sampled at 1%) enqueues an evaluation event to its local buffer. A background goroutine/thread flushes the buffer to Kafka every 5 seconds.
5. A flag manager updates "checkout-redesign" targeting rules via the Management UI.
6. Management API writes the change to Postgres, publishes `FlagChangedEvent{org_id, env_id, flag_id, new_definition}` to Kafka.
7. Config Snapshot Service rebuilds the env snapshot, updates Redis.
8. SSE Server pushes a delta `data: {"updated_flags": ["checkout-redesign"], "snapshot_version": "v42"}` to all connected server-side SDKs in that org × environment.
9. SDK receives the delta, fetches updated snapshot from Config Cache (or the delta itself contains the full flag diff), updates in-memory ruleset atomically (using a read-write lock).
10. Subsequent evaluations use the new rules. Old evaluations in-flight complete with the old rules (fine — lock is per-SDK, not per-evaluation).

---

## 4. Data Model

### Entities & Schema

```sql
-- Organizations
CREATE TABLE organizations (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    slug        VARCHAR(64) UNIQUE NOT NULL,
    name        VARCHAR(255) NOT NULL,
    plan_tier   VARCHAR(32) NOT NULL DEFAULT 'starter',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Environments (dev, staging, production, etc.)
CREATE TABLE environments (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    name        VARCHAR(64) NOT NULL,          -- 'production', 'staging', 'dev'
    slug        VARCHAR(64) NOT NULL,
    sdk_key     VARCHAR(128) UNIQUE NOT NULL,  -- secret key for SDK authentication
    is_production BOOLEAN NOT NULL DEFAULT false,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(org_id, slug)
);

-- Feature flags (metadata only; rules are in flag_rules)
CREATE TABLE flags (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    key             VARCHAR(255) NOT NULL,      -- e.g., 'checkout-redesign'
    name            VARCHAR(512) NOT NULL,
    description     TEXT,
    flag_type       VARCHAR(32) NOT NULL,       -- 'boolean','string','number','json'
    tags            TEXT[] NOT NULL DEFAULT '{}',
    archived        BOOLEAN NOT NULL DEFAULT false,
    created_by      UUID NOT NULL REFERENCES users(id),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(org_id, key)
);

-- Per-environment flag configuration (the "live" config for a flag in one env)
CREATE TABLE flag_environments (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    flag_id         UUID NOT NULL REFERENCES flags(id) ON DELETE CASCADE,
    environment_id  UUID NOT NULL REFERENCES environments(id) ON DELETE CASCADE,
    enabled         BOOLEAN NOT NULL DEFAULT false,  -- global kill switch
    default_variant VARCHAR(255) NOT NULL DEFAULT 'false', -- served when no rule matches
    version         BIGINT NOT NULL DEFAULT 1,       -- incremented on each change
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(flag_id, environment_id)
);

-- Variants (possible values a flag can return)
CREATE TABLE variants (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    flag_id     UUID NOT NULL REFERENCES flags(id) ON DELETE CASCADE,
    key         VARCHAR(255) NOT NULL,          -- e.g., 'control', 'treatment', 'true'
    value       JSONB NOT NULL,                 -- the actual value served
    description TEXT,
    weight      NUMERIC(5,2),                   -- optional weight for experiment allocation
    UNIQUE(flag_id, key)
);

-- Targeting rules for a flag in an environment (ordered; first match wins)
CREATE TABLE flag_rules (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    flag_env_id     UUID NOT NULL REFERENCES flag_environments(id) ON DELETE CASCADE,
    priority        INT NOT NULL,               -- lower = evaluated first
    rule_type       VARCHAR(32) NOT NULL,       -- 'targeting','percentage_rollout','prerequisite'
    description     TEXT,
    serve_variant   VARCHAR(255),               -- which variant to serve on match
    -- For percentage_rollout rules
    rollout_percent NUMERIC(5,2),              -- 0.00 to 100.00
    rollout_seed    VARCHAR(128),              -- salt for hash bucketing
    -- For prerequisite rules
    prerequisite_flag_id UUID REFERENCES flags(id),
    prerequisite_variant VARCHAR(255),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Individual conditions within a targeting rule (AND semantics within a rule)
CREATE TABLE rule_conditions (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    rule_id     UUID NOT NULL REFERENCES flag_rules(id) ON DELETE CASCADE,
    attribute   VARCHAR(255) NOT NULL,          -- e.g., 'user.email', 'org.plan', 'country'
    operator    VARCHAR(64) NOT NULL,           -- 'equals','not_equals','contains','in','not_in',
                                                --  'starts_with','ends_with','regex','gt','lt',
                                                --  'gte','lte','between','semver_gte'
    value       JSONB NOT NULL,                 -- string, number, array, or range object
    negate      BOOLEAN NOT NULL DEFAULT false
);

-- Reusable segments
CREATE TABLE segments (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID NOT NULL REFERENCES organizations(id) ON DELETE CASCADE,
    name        VARCHAR(255) NOT NULL,
    description TEXT,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    UNIQUE(org_id, name)
);

CREATE TABLE segment_conditions (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    segment_id  UUID NOT NULL REFERENCES segments(id) ON DELETE CASCADE,
    attribute   VARCHAR(255) NOT NULL,
    operator    VARCHAR(64) NOT NULL,
    value       JSONB NOT NULL,
    negate      BOOLEAN NOT NULL DEFAULT false
);

-- Allows rules to reference segments
CREATE TABLE rule_segments (
    rule_id     UUID NOT NULL REFERENCES flag_rules(id) ON DELETE CASCADE,
    segment_id  UUID NOT NULL REFERENCES segments(id) ON DELETE CASCADE,
    negate      BOOLEAN NOT NULL DEFAULT false,
    PRIMARY KEY (rule_id, segment_id)
);

-- Immutable audit history (append-only)
CREATE TABLE audit_events (
    id              UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id          UUID NOT NULL,
    actor_id        UUID REFERENCES users(id),
    actor_type      VARCHAR(32) NOT NULL,       -- 'user','api_key','system'
    event_type      VARCHAR(128) NOT NULL,      -- e.g., 'flag.enabled','flag.rule.updated'
    entity_type     VARCHAR(64) NOT NULL,
    entity_id       UUID NOT NULL,
    before_state    JSONB,
    after_state     JSONB,
    change_reason   TEXT,
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    INDEX idx_audit_org_time   (org_id, occurred_at DESC),
    INDEX idx_audit_entity     (entity_type, entity_id, occurred_at DESC)
);

-- Evaluation statistics (aggregated counters, not raw events)
CREATE TABLE evaluation_stats (
    flag_id         UUID NOT NULL REFERENCES flags(id),
    environment_id  UUID NOT NULL REFERENCES environments(id),
    date            DATE NOT NULL,
    variant_key     VARCHAR(255) NOT NULL,
    evaluation_count BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (flag_id, environment_id, date, variant_key)
);

-- Users
CREATE TABLE users (
    id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    org_id      UUID NOT NULL REFERENCES organizations(id),
    email       VARCHAR(255) NOT NULL,
    display_name VARCHAR(255),
    role        VARCHAR(32) NOT NULL DEFAULT 'member', -- 'owner','admin','member','viewer'
    UNIQUE(org_id, email)
);
```

### Database Choice

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| **PostgreSQL** | ACID for flag version management; JSONB for flexible rule/value storage; strong consistency for audit trail | Needs sharding at large org counts | **Selected for flag store** |
| Redis | Sub-millisecond reads; ideal for config snapshots | Not a primary store — no complex querying, no audit trail | **Selected as cache layer only** |
| DynamoDB | Predictable latency; serverless | No JSONB-equivalent querying; poor for audit log queries | Not selected for primary store |
| Cassandra | High write throughput | Poor for complex queries; eventual consistency | Not suitable for flag rules (need strong consistency) |

**Selected: PostgreSQL** (flag store) + **Redis** (config snapshot cache)

Rationale: Flag configuration changes are infrequent (< 1/sec). The critical path (evaluation) is entirely in-process from the SDK's cached copy; PostgreSQL is never on the hot path for evaluations. PostgreSQL provides the ACID transactions needed to atomically version flag changes and write audit records. Redis stores the compiled, serialized snapshot for fast SDK sync.

---

## 5. API Design

```
Base URL: https://api.featureflags.example.com/v1
Auth: Management APIs — Bearer JWT. SDK APIs — SDK API key (X-SDK-Key header).
Rate limits: Management API — 100 req/min per user. SDK sync — 10 req/min per SDK instance.
```

### Flag Management

```
# List flags in an environment
GET /orgs/{org_id}/flags?env={env_slug}&archived=false&tag=payments&limit=50&cursor=...
Response: 200
{
  "flags": [
    {
      "id", "key", "name", "flag_type", "tags",
      "env_config": { "enabled", "default_variant", "version" },
      "variants": [ { "key", "value", "weight" } ],
      "rule_count": 3
    }
  ],
  "next_cursor": "..."
}

# Get full flag detail (including rules)
GET /orgs/{org_id}/flags/{flag_key}?env={env_slug}
Response: 200
{
  "flag": { "id", "key", "name", "flag_type", "created_by", "created_at" },
  "environments": {
    "production": {
      "enabled": true, "default_variant": "false", "version": 42,
      "rules": [
        {
          "id", "priority": 1, "rule_type": "targeting",
          "description": "Enable for beta users",
          "conditions": [ { "attribute": "user.email", "operator": "ends_with", "value": "@beta.example.com" } ],
          "serve_variant": "true"
        },
        {
          "id", "priority": 2, "rule_type": "percentage_rollout",
          "rollout_percent": 10.0, "rollout_seed": "checkout-redesign-prod-2026",
          "serve_variant": "treatment"
        }
      ]
    }
  }
}

# Create a flag
POST /orgs/{org_id}/flags
Body: {
  "key": "checkout-redesign",
  "name": "Checkout Redesign",
  "flag_type": "boolean",
  "tags": ["checkout", "experiment"],
  "environments": {
    "production": { "enabled": false, "default_variant": "false" },
    "staging":    { "enabled": true,  "default_variant": "false" }
  },
  "variants": [
    { "key": "true",  "value": true  },
    { "key": "false", "value": false }
  ]
}
Response: 201
{ "flag_id": "...", "key": "checkout-redesign" }

# Update flag rules for an environment
PUT /orgs/{org_id}/flags/{flag_key}/environments/{env_slug}/rules
Body: {
  "rules": [
    {
      "priority": 1,
      "rule_type": "targeting",
      "description": "Internal users",
      "conditions": [ { "attribute": "user.email", "operator": "ends_with", "value": "@example.com" } ],
      "serve_variant": "treatment"
    },
    {
      "priority": 2,
      "rule_type": "percentage_rollout",
      "rollout_percent": 5.0,
      "rollout_seed": "checkout-v2-2026-04",
      "serve_variant": "treatment"
    }
  ],
  "change_reason": "Starting 5% canary rollout"
}
Response: 200
{ "version": 43, "updated_at": "..." }

# Kill switch: enable or disable a flag globally
PATCH /orgs/{org_id}/flags/{flag_key}/environments/{env_slug}
Body: { "enabled": false, "change_reason": "Disabling due to P0 incident" }
Response: 200
{ "version": 44, "enabled": false }

# Get flag audit history
GET /orgs/{org_id}/flags/{flag_key}/history?env={env_slug}&limit=25&cursor=...
Response: 200
{
  "events": [
    {
      "event_type": "flag.rule.updated",
      "actor": { "id", "email" },
      "before_state": { ... },
      "after_state": { ... },
      "change_reason": "...",
      "occurred_at": "..."
    }
  ]
}
```

### SDK Delivery APIs

```
# Server-side SDK: download full environment snapshot (called at SDK init)
GET /sdk/flags
Headers: X-SDK-Key: sdk-prod-xxxx
Response: 200
Content-Type: application/x-msgpack
X-Snapshot-Version: sha256:abc123...
X-Snapshot-Etag: "v42"
Body: <MessagePack-encoded snapshot of all flags for this org × env>

# Server-side SDK: polling for updates (ETag-based)
GET /sdk/flags
Headers:
  X-SDK-Key: sdk-prod-xxxx
  If-None-Match: "v42"
Response: 304 Not Modified  (if no changes)
       OR 200 with new snapshot + new ETag

# Server-side SDK: streaming updates (SSE)
GET /sdk/stream
Headers: X-SDK-Key: sdk-prod-xxxx
Response: 200 text/event-stream
event: flag_updated
data: {"flag_key":"checkout-redesign","version":43,"definition":{...}}

event: snapshot_version
data: {"version":"v43","etag":"sha256:def456"}

# Client-side SDK: get personalized flag values for a user
POST /sdk/evaluate
Headers: X-SDK-Key: sdk-browser-yyyy
Body: {
  "user": {
    "key": "user_abc123",
    "attributes": {
      "email": "jane@example.com",
      "plan": "enterprise",
      "country": "US",
      "beta_user": true
    }
  },
  "flag_keys": ["checkout-redesign", "new-nav", "dark-mode"]  // empty = all flags
}
Response: 200
{
  "evaluations": {
    "checkout-redesign": { "variant": "treatment", "value": true, "reason": "targeting_rule" },
    "new-nav":           { "variant": "control",   "value": false,"reason": "default" },
    "dark-mode":         { "variant": "on",        "value": true, "reason": "percentage_rollout" }
  },
  "snapshot_version": "v43"
}

# SDK: record evaluation events (batch, for A/B integration)
POST /sdk/events
Headers: X-SDK-Key: sdk-prod-xxxx
Body: {
  "events": [
    {
      "type": "flag_evaluated",
      "flag_key": "checkout-redesign",
      "variant": "treatment",
      "user_key": "user_abc123",
      "timestamp": "2026-04-09T10:00:00Z",
      "metadata": { "experiment_id": "exp_001" }
    }
  ]
}
Response: 202 Accepted
```

---

## 6. Deep Dive: Core Components

### 6.1 Flag Evaluation Engine (In-Process SDK)

**Problem it solves**: Evaluate a flag for a given user context against potentially hundreds of targeting rules in under 1ms, consistently and deterministically (same user always gets same variant for percentage rollouts).

**Approaches Comparison:**

| Approach | Mechanism | Latency | Consistency | Blast Radius if Server Down |
|---|---|---|---|---|
| **Remote evaluation (API call per eval)** | Every `evaluate()` call hits the flag service API | 5-50ms network RTT | Server controls consistency | Flags unavailable |
| **In-process cached rules** | SDK downloads all rules; evaluates locally | < 0.1ms (in-memory) | Strong (same rules until update) | Evaluations continue from cache |
| **Edge evaluation (CDN workers)** | Rules evaluated in CDN edge workers | < 5ms (nearby PoP) | Good | PoP-level cache serves stale rules |
| **Hybrid: server eval for client, in-process for server** | Client apps call evaluate API; server apps use local cache | < 1ms server, < 100ms client | Strong | Server apps unaffected; client apps see cached responses |

**Selected: In-process for server-side SDKs; server-side evaluation endpoint for client-side SDKs (hybrid)**

Rationale: Server-side applications (Node.js, Python, Go) process millions of requests/sec and cannot afford a network call per flag evaluation. They download the entire ruleset and evaluate locally. Client-side applications (browsers, mobile) should not receive business logic (rule details) that could reveal unreleased features; the server evaluates on their behalf and returns only the variant values.

**Implementation — Evaluation engine pseudocode:**

```python
class FlagEvaluationEngine:
    def __init__(self):
        self._ruleset: Dict[str, FlagConfig] = {}
        self._lock = threading.RWLock()

    def evaluate(
        self,
        flag_key: str,
        user_context: UserContext,
        default_value: Any = None
    ) -> EvaluationResult:
        with self._lock.read():
            flag = self._ruleset.get(flag_key)
            if flag is None:
                return EvaluationResult(value=default_value, reason="flag_not_found")
            if not flag.enabled:
                return EvaluationResult(
                    value=self._resolve_variant(flag, flag.default_variant),
                    reason="flag_disabled"
                )
            return self._evaluate_rules(flag, user_context)

    def _evaluate_rules(self, flag: FlagConfig, ctx: UserContext) -> EvaluationResult:
        # Rules are pre-sorted by priority (ascending) at snapshot load time
        for rule in flag.rules:
            if rule.rule_type == "prerequisite":
                # Evaluate the prerequisite flag first
                prereq_result = self.evaluate(rule.prerequisite_flag_key, ctx)
                if prereq_result.value != rule.prerequisite_variant:
                    continue  # prerequisite not met; skip this rule

            if self._matches_rule(rule, ctx):
                if rule.rule_type == "percentage_rollout":
                    variant_key = self._bucket_user(
                        user_key=ctx.key,
                        flag_key=flag.key,
                        seed=rule.rollout_seed,
                        rollout_pct=rule.rollout_percent
                    )
                    if variant_key is None:
                        continue  # user not in rollout; try next rule
                    return EvaluationResult(
                        value=self._resolve_variant(flag, variant_key),
                        reason="percentage_rollout"
                    )
                else:
                    return EvaluationResult(
                        value=self._resolve_variant(flag, rule.serve_variant),
                        reason="targeting_rule"
                    )

        # No rule matched — serve default variant
        return EvaluationResult(
            value=self._resolve_variant(flag, flag.default_variant),
            reason="default"
        )

    def _matches_rule(self, rule: Rule, ctx: UserContext) -> bool:
        # All conditions must match (AND semantics)
        for cond in rule.conditions:
            attr_value = ctx.get_attribute(cond.attribute)
            if not self._evaluate_condition(cond, attr_value):
                return False
        # All referenced segments must also match
        for seg_ref in rule.segment_refs:
            segment_match = self._matches_segment(seg_ref.segment, ctx)
            if seg_ref.negate:
                segment_match = not segment_match
            if not segment_match:
                return False
        return True

    def _bucket_user(
        self,
        user_key: str,
        flag_key: str,
        seed: str,
        rollout_pct: float
    ) -> Optional[str]:
        """
        Deterministic bucket assignment using MurmurHash3.
        Returns variant_key if user is in rollout, None otherwise.
        Bucket range: [0, 10000). User is in rollout if bucket < rollout_pct * 100.
        """
        hash_input = f"{seed}.{flag_key}.{user_key}"
        bucket = mmh3.hash(hash_input, signed=False) % 10_000
        threshold = int(rollout_pct * 100)   # e.g., 10.0% -> 1000
        if bucket < threshold:
            return "treatment"
        return None

    def _evaluate_condition(self, cond: Condition, value: Any) -> bool:
        op = cond.operator
        target = cond.value
        result = False
        if op == "equals":        result = value == target
        elif op == "not_equals":  result = value != target
        elif op == "in":          result = value in target
        elif op == "not_in":      result = value not in target
        elif op == "contains":    result = isinstance(value, str) and target in value
        elif op == "starts_with": result = isinstance(value, str) and value.startswith(target)
        elif op == "ends_with":   result = isinstance(value, str) and value.endswith(target)
        elif op == "regex":       result = bool(re.match(target, str(value)))
        elif op == "gt":          result = float(value) > float(target)
        elif op == "lt":          result = float(value) < float(target)
        elif op == "gte":         result = float(value) >= float(target)
        elif op == "lte":         result = float(value) <= float(target)
        elif op == "between":     result = float(target[0]) <= float(value) <= float(target[1])
        elif op == "semver_gte":  result = semver.compare(str(value), str(target)) >= 0
        return (not result) if cond.negate else result
```

**Interviewer Q&A:**

Q: What hash function should you use for percentage bucketing, and why not MD5/SHA-1?
A: MurmurHash3 (non-cryptographic). It produces a uniform distribution, is extremely fast (< 10ns for short strings), and is available in all major languages. MD5 and SHA-1 are cryptographic hash functions: much slower (> 100ns) and produce non-uniform low-order bits in some implementations. We only need statistical uniformity, not collision resistance. The bucket function takes `(seed + flag_key + user_key)` to ensure: (a) different flags produce independent buckets for the same user, and (b) the seed can be rotated to re-randomize allocations without changing the flag key.

Q: How do you ensure a user doesn't flip between variants when the rollout percentage increases from 5% to 10%?
A: The bucket is deterministic and fixed: `bucket = hash(seed.flag_key.user_key) % 10000`. At 5% rollout, users with `bucket < 500` see the treatment. At 10%, users with `bucket < 1000` see treatment. All users in the 0-499 range were already seeing treatment; users in 500-999 are newly added. No user who was previously in treatment flips out. This is the "consistent hashing rollout" property and only holds as long as the seed doesn't change.

Q: How do you prevent circular prerequisite flag dependencies (flag A requires flag B requires flag A)?
A: The Management API validates prerequisites on write using a DFS cycle detection algorithm over the prerequisite graph. If adding a prerequisite creates a cycle, the write is rejected with a 422 error. The graph is stored in-memory in the Management API (loaded from DB at startup, updated on each change). The evaluation engine also has a circuit breaker: it tracks `currently_evaluating` as a set in thread-local storage; if it encounters a flag already in that set, it returns the default value and logs a warning (defensive coding against a DB bug that bypassed the cycle check).

Q: How would you support multi-variate flags with weighted random allocation?
A: The `variants` table has a `weight` column. For a multi-variate flag with variants `control (50%)`, `treatment_a (30%)`, `treatment_b (20%)`, the bucketing becomes: `bucket = hash(seed.flag_key.user_key) % 10000`. Cumulative weight boundaries: `control: [0, 5000)`, `treatment_a: [5000, 8000)`, `treatment_b: [8000, 10000)`. The evaluation engine checks which range `bucket` falls in. Weights are validated to sum to 100% on write.

Q: The SDK ruleset is 50 MB. How do you minimize memory and startup time?
A: (1) Ship the snapshot as MessagePack (binary, ~60% smaller than JSON). (2) Apply delta compression on updates — only changed flags are sent over SSE, not the full snapshot. (3) At SDK startup, load the snapshot lazily: process the most-called flags first (by evaluation count, reported via telemetry), so the SDK can begin serving requests before the full snapshot is parsed. (4) Use a sorted hash map internally for O(1) flag lookup by key.

---

### 6.2 Configuration Propagation (Sub-5-Second Update Delivery)

**Problem it solves**: When a flag is changed, all SDK instances (potentially 50,000) must receive the update within 5 seconds. A kill switch must propagate within 1 second.

**Approaches Comparison:**

| Approach | Mechanism | Propagation Latency | Connection Overhead | Works for Mobile? |
|---|---|---|---|---|
| **Short polling** | SDK polls every N seconds | Up to N seconds | Low (disconnected between polls) | Yes |
| **Long polling** | SDK holds HTTP connection until update | Near-instant | Medium (1 connection/SDK) | Limited |
| **Server-Sent Events (SSE)** | SDK holds persistent HTTP/1.1 connection; server pushes | Near-instant | Medium (1 connection/SDK) | Limited (battery) |
| **WebSocket** | Full-duplex persistent connection | Near-instant | Medium | Limited |
| **Push notifications (FCM/APNs)** | OS-level push to mobile | 1-5s (OS-dependent) | Handled by OS | Yes (mobile only) |

**Selected: SSE for server-side SDKs + 30-second polling for mobile/client SDKs**

Rationale: SSE is simpler than WebSocket (unidirectional from server to client suffices), rides HTTP/1.1 or HTTP/2, and is natively handled by web browsers. For server-side SDKs (50,000 fleet nodes), SSE connections are permanent background connections. For mobile SDKs, SSE drains battery; 30-second polling is acceptable (kill switch propagation at 30s worst-case for mobile — acceptable trade-off for battery life, with an emergency in-app push notification fallback for critical kill switches).

**Kill switch priority lane**: Kill switch updates (`flag.enabled = false`) are published to a separate high-priority Kafka topic `flag-kill-switches`. A dedicated SSE broadcaster consumes only this topic and pushes to all connected SDKs immediately, bypassing the normal Config Snapshot Service rebuild pipeline. This provides the 1-second SLA.

**Implementation — SSE broadcaster pseudocode:**

```python
class SSEBroadcaster:
    """
    One instance per org × environment.
    Manages all SSE connections for that scope.
    """
    def __init__(self, org_id: str, env_id: str):
        self.org_id = org_id
        self.env_id = env_id
        self._connections: Dict[str, asyncio.Queue] = {}  # conn_id → message queue
        self._lock = asyncio.Lock()

    async def add_connection(self, conn_id: str, queue: asyncio.Queue):
        async with self._lock:
            self._connections[conn_id] = queue
        # Send current snapshot version immediately on connect
        snapshot_etag = await redis.get(f"snapshot_etag:{self.org_id}:{self.env_id}")
        await queue.put(SSEEvent(
            event="snapshot_version",
            data=json.dumps({"etag": snapshot_etag})
        ))

    async def remove_connection(self, conn_id: str):
        async with self._lock:
            self._connections.pop(conn_id, None)

    async def broadcast(self, event: FlagChangedEvent, priority: bool = False):
        """Broadcast a flag change to all connections for this org × env."""
        sse_payload = SSEEvent(
            event="flag_updated",
            data=json.dumps({
                "flag_key": event.flag_key,
                "version":  event.version,
                "definition": event.definition,  # full flag definition for delta update
                "is_kill_switch": event.is_kill_switch
            })
        )
        async with self._lock:
            connections_snapshot = list(self._connections.items())

        # Broadcast concurrently; don't let a slow connection block others
        tasks = [
            asyncio.create_task(self._send_to_connection(conn_id, queue, sse_payload))
            for conn_id, queue in connections_snapshot
        ]
        await asyncio.gather(*tasks, return_exceptions=True)

    async def _send_to_connection(
        self, conn_id: str, queue: asyncio.Queue, event: SSEEvent
    ):
        try:
            await asyncio.wait_for(queue.put(event), timeout=2.0)
        except asyncio.TimeoutError:
            # Connection is slow/backed up — disconnect and let SDK reconnect
            await self.remove_connection(conn_id)

# Kafka consumer for flag changes
async def consume_flag_changes():
    async for msg in kafka_consumer("flag-changes"):
        event = FlagChangedEvent.parse(msg.value)
        broadcaster = get_broadcaster(event.org_id, event.env_id)
        priority = event.is_kill_switch
        await broadcaster.broadcast(event, priority=priority)

# SSE endpoint handler
async def sse_handler(request: Request):
    sdk_key = request.headers.get("X-SDK-Key")
    env = authenticate_sdk_key(sdk_key)
    conn_id = str(uuid4())
    queue = asyncio.Queue(maxsize=100)

    broadcaster = get_broadcaster(env.org_id, env.id)
    await broadcaster.add_connection(conn_id, queue)

    async def event_generator():
        try:
            while True:
                # Send keepalive every 30s to prevent proxy timeouts
                try:
                    event = await asyncio.wait_for(queue.get(), timeout=30)
                    yield f"event: {event.event}\ndata: {event.data}\n\n"
                except asyncio.TimeoutError:
                    yield ": keepalive\n\n"
        finally:
            await broadcaster.remove_connection(conn_id)

    return StreamingResponse(event_generator(), media_type="text/event-stream")
```

**Interviewer Q&A:**

Q: 50,000 persistent SSE connections to a single service — how do you handle this at scale?
A: Each SSE server instance can handle ~10,000 concurrent connections with async I/O (Python asyncio, Node.js event loop, Go goroutines). Deploy 10 SSE server instances behind a load balancer. Use sticky sessions (by SDK key hash) so all connections from the same org × env land on the same instance. This means each broadcaster has a complete view of its org's connections without cross-instance coordination. When an instance fails, SDKs reconnect (they retry SSE connection with exponential backoff); they immediately receive the current snapshot version and re-sync.

Q: How does the SDK handle receiving a delta update that it can't apply (e.g., it's on an old version and the delta doesn't include all changes)?
A: The SDK tracks the `snapshot_version` it last successfully processed. Each SSE delta includes the base version it was computed from. If the SDK's current version doesn't match the delta's base version (meaning it missed an intermediate update), the SDK falls back to downloading the full snapshot via `GET /sdk/flags`. This catch-up mechanism ensures eventual consistency even if SSE events are missed.

Q: What happens to the SSE broadcaster if it crashes mid-broadcast?
A: The broadcaster is stateless between broadcasts. Kafka provides durability — the change event is durably stored in Kafka and was not ACK'd by the broadcaster (since the broadcaster crashed). Kafka retains the message; when the broadcaster restarts (Kubernetes pod restart), it resumes from the last committed offset and re-processes the event. In the meantime, connected SDK instances hold their previous ruleset (stale by a few seconds maximum) and continue evaluating — no outage for end users.

Q: How do you scale the SSE infrastructure if you grow to 500,000 SDK connections?
A: Increase SSE server instances from 10 to 100 (50,000 connections each × 100 instances = 5M connections supported). The sticky session routing (by `hash(org_id) % N_instances`) scales linearly. At 500,000 connections, 50 instances handle 10,000 each. The Config Snapshot Service and Kafka remain shared infrastructure — only the SSE delivery layer scales out.

---

### 6.3 Audit Trail and Change Management

**Problem it solves**: Every flag change must be recorded immutably with full before/after state for compliance (SOC 2, GDPR incident response) and root cause analysis ("who changed this flag and when did it cause the incident?").

**Implementation — Transactional audit logging:**

```python
def update_flag_rules(
    flag_key: str, env_slug: str, org_id: str,
    new_rules: List[RuleSpec], actor_id: str, change_reason: str
) -> FlagVersion:
    with db.transaction():
        # 1. Load current state (for before_state)
        flag_env = db.query_one("""
            SELECT fe.*, json_agg(fr.*) as rules
            FROM flag_environments fe
            LEFT JOIN flag_rules fr ON fr.flag_env_id = fe.id
            WHERE fe.flag_id = (SELECT id FROM flags WHERE org_id=%s AND key=%s)
              AND fe.environment_id = (SELECT id FROM environments WHERE org_id=%s AND slug=%s)
            GROUP BY fe.id
        """, org_id, flag_key, org_id, env_slug)

        before_state = flag_env.to_dict()

        # 2. Delete existing rules and insert new ones
        db.execute("DELETE FROM flag_rules WHERE flag_env_id = %s", flag_env.id)
        for rule in new_rules:
            rule_id = db.execute_returning("""
                INSERT INTO flag_rules (flag_env_id, priority, rule_type, serve_variant,
                    rollout_percent, rollout_seed, description)
                VALUES (%s, %s, %s, %s, %s, %s, %s) RETURNING id
            """, flag_env.id, rule.priority, rule.rule_type, rule.serve_variant,
                 rule.rollout_percent, rule.rollout_seed, rule.description)
            for cond in rule.conditions:
                db.execute("""
                    INSERT INTO rule_conditions (rule_id, attribute, operator, value, negate)
                    VALUES (%s, %s, %s, %s, %s)
                """, rule_id, cond.attribute, cond.operator, json.dumps(cond.value), cond.negate)

        # 3. Increment version
        new_version = db.execute_returning("""
            UPDATE flag_environments
            SET version = version + 1, updated_at = NOW()
            WHERE id = %s RETURNING version
        """, flag_env.id)

        after_state = {**before_state, "rules": [r.to_dict() for r in new_rules], "version": new_version}

        # 4. Write audit event IN THE SAME TRANSACTION
        db.execute("""
            INSERT INTO audit_events
                (org_id, actor_id, actor_type, event_type, entity_type, entity_id,
                 before_state, after_state, change_reason, occurred_at)
            VALUES (%s, %s, 'user', 'flag.rules.updated', 'flag_environment', %s,
                    %s, %s, %s, NOW())
        """, org_id, actor_id, flag_env.id,
             json.dumps(before_state), json.dumps(after_state), change_reason)

    # 5. Publish change event to Kafka AFTER transaction commits
    kafka_producer.publish("flag-changes", FlagChangedEvent(
        org_id=org_id,
        env_id=flag_env.environment_id,
        flag_key=flag_key,
        version=new_version,
        definition=after_state
    ))

    return FlagVersion(version=new_version)
```

The audit event is written in the same database transaction as the flag change. If either the flag update or the audit write fails, both are rolled back — ensuring the audit log is always consistent with the actual flag state.

**Interviewer Q&A:**

Q: What if someone with database access deletes audit records directly?
A: Two controls: (1) Application-level: the application account has `INSERT`, `SELECT` on `audit_events` but no `UPDATE` or `DELETE`. (2) Infrastructure-level: for regulated industries, replicate audit events to an immutable store (AWS S3 with Object Lock in COMPLIANCE mode, or an append-only write-once Kafka topic with unlimited retention) that even database admins cannot modify. This provides a second source of truth outside the mutable RDBMS.

Q: How do you support flag changes with approval workflows (change must be reviewed before applying)?
A: Add a `flag_change_requests` table. A change is drafted (persisted with status `pending_approval`), reviewed by required approvers, and only applied (the actual `flag_rules` update) upon final approval. The approval workflow supports auto-approval for low-risk changes (e.g., a percentage rollout decrease) and requires manual approval for high-risk changes (e.g., enabling a flag in production for the first time, or changing a flag associated with a revenue-critical experiment).

---

## 7. Scaling

### Horizontal Scaling

- **Management API**: Stateless; scale behind ALB. Typical load: < 100 req/sec. 3 instances is sufficient for high availability.
- **Config Snapshot Service**: Single writer to Redis per org × env snapshot. Partitioned by `org_id` across Kafka consumer group. At 50 changes/sec burst, 3 instances handle recomputation.
- **SSE Server**: Sticky-session load balanced by `org_id`. Each instance handles 10,000 connections with async I/O. 10 instances for 100,000 connections. Scale out proportionally.
- **Client-Side Evaluate API**: Stateless; evaluates rules from Redis cache. Scale based on RPS: 11,570 req/sec peak at 50ms average server time = need ~580 CPU threads = ~30 server instances (20 vCPU each).
- **SDK Event Ingestion (A/B events)**: Kafka producer endpoint; stateless. Events flow to Kafka and are consumed by the A/B platform.

### DB Sharding

- Shard by `org_id`. Flag data per org is small (< 1 GB typically), so sharding is driven by write throughput isolation (one noisy org's updates shouldn't affect another's) not by data size.
- Use Citus (Postgres extension) with `org_id` as distribution column. Reference tables: `environments`, `users`.

### Replication

- **Postgres**: 1 primary + 2 read replicas per shard. Management API reads from primary (strong consistency). Audit log queries use read replicas.
- **Redis**: Redis Cluster with 6 nodes (3 primaries, 3 replicas). Sharded by `{org_id}:{env_id}` key. Snapshot size per slot: 50 MB; total cache size: 50 orgs × 10 envs × 50 MB = 25 GB — fits comfortably in memory.

### Caching

| Data | Cache | TTL | Note |
|---|---|---|---|
| Per-env flag snapshot | Redis | Indefinite (invalidated on change) | ~50 MB per entry; 5,000 entries = 250 GB total |
| SDK key → org/env mapping | In-process LRU | 5 min | Auth check on every SDK request |
| Segment definitions | Embedded in snapshot | Same as snapshot | Segments de-normalized into snapshot at build time |
| Evaluation stats (read by UI) | Redis counter | 1 day aggregate | Incremented by event processor; read by dashboard |

**Interviewer Q&As:**

Q: If the Redis cluster is unavailable, what happens to flag evaluations?
A: Server-side SDKs hold the full ruleset in process memory. They are entirely unaffected by Redis unavailability — evaluations continue from the in-memory cache. Client-side evaluate API requests would fail (since they read rules from Redis). Mitigation: client-side SDK caches the last received evaluation results in browser localStorage / mobile persistent storage and serves them on API failure (stale-on-error). SDKs expose a `flag_evaluation_source: "cache"` reason code so callers can detect this.

Q: How do you handle a flag change that must be atomic across multiple environments (e.g., enable in staging and production simultaneously)?
A: The Management API supports a `multi_environment_update` endpoint that wraps changes to multiple `flag_environments` rows in a single database transaction. All environments are updated atomically or none are. Each updated environment gets its own Kafka change event, so SSE propagation happens independently per environment (but near-simultaneously, within milliseconds of the transaction commit).

Q: What is the memory overhead of the SDK holding 10,000 flags in-process?
A: 10,000 flags × 5 KB average = 50 MB raw JSON. Deserialized into Go structs or Python dicts, actual memory usage is ~80-120 MB due to object overhead and hash map buckets. For a Java application with JVM overhead, expect ~200-250 MB. This is acceptable on any modern server instance. If memory is a concern (e.g., function-as-a-service), the SDK supports lazy loading: only the flags declared in the application code are fetched at init, reducing the snapshot to only needed flags.

Q: How do you handle 500,000 client-side evaluate API calls per second?
A: The evaluate endpoint is computationally O(rules per flag) per flag evaluated, and uses the in-memory ruleset from Redis. At 50ms server time per request and horizontal scaling, 500,000 req/sec requires ~25,000 CPU cores. Alternative: implement edge evaluation using Cloudflare Workers or AWS Lambda@Edge. The flag ruleset snapshot (50 MB per env) is pushed to CDN edge nodes. Evaluation runs at the edge node geographically nearest to the user. This reduces origin server load to near-zero for evaluations and brings latency to < 5ms globally.

Q: How would you design flag evaluation to be thread-safe in a multi-threaded application server?
A: The `_ruleset` dictionary is protected by a read-write lock (RWLock). All evaluations acquire a read lock (non-exclusive; concurrent evaluations proceed in parallel). The SDK's background updater (SSE consumer) acquires the write lock only when applying a flag change. Write lock is held for < 1ms (the time to swap the pointer to the new ruleset, using copy-on-write semantics: build a new dictionary with updated flags, then atomically replace the reference). This design means no evaluation is ever blocked for more than 1ms due to an update.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Detection | Recovery |
|---|---|---|---|
| Management API | Instance crash | ALB health check | Other instances serve traffic; no data loss |
| Postgres primary | Crash | Patroni detects (5s TTL) | Failover to replica; < 30s outage; management writes retry |
| Redis cluster | Node failure | Redis Sentinel | Sentinel promotes replica; < 15s; SDK holds in-memory cache |
| Kafka broker | Crash | Kafka controller | Partition rebalance; change events may be delayed but not lost |
| SSE Server | Process crash | Kubernetes liveness probe | SDK reconnects with exponential backoff (1s, 2s, 4s...); downloads full snapshot on reconnect |
| SDK in-process cache | App restart | N/A | SDK re-downloads snapshot on init (typically < 500ms) |
| Full flag service outage | All services unavailable | Client monitoring | SDK serves last cached values; client-side SDK serves localStorage cache; no crash |

### Idempotency

- Flag version is a monotonically increasing integer. SDKs track the version they last processed. If they receive a delta with version N+2 (gap), they request a full snapshot rather than applying incomplete deltas.
- Kafka change events carry the flag version. The Config Snapshot Service is idempotent: it rebuilds the snapshot from PostgreSQL (the source of truth) and stores in Redis, overwriting any previous value. Processing the same change event twice results in the same Redis snapshot.

### Circuit Breaker

- SDK's connection to the SSE server is wrapped in a retry circuit breaker. After 5 failed connection attempts in 60 seconds, the SDK enters "disconnected mode": it evaluates from its cached ruleset and silently re-attempts connection every 60 seconds. It emits a `sdk.disconnected` metric for monitoring.
- The evaluate API wraps Redis reads in a circuit breaker. If Redis is unavailable for > 10 seconds, the evaluate API switches to reading flag definitions directly from PostgreSQL (slower, but correct).

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Purpose |
|---|---|---|---|
| `flag.evaluation.latency_p99` | Histogram | > 1ms | SDK evaluation engine performance |
| `flag.update.propagation_latency` | Histogram | > 5s | Time from DB write to SDK receiving SSE event |
| `flag.kill_switch.propagation_latency` | Histogram | > 1s | Kill switch SLA |
| `sdk.connections.active` | Gauge | — | Capacity planning for SSE servers |
| `sdk.disconnected.count` | Gauge | > 1% of fleet | SDK connectivity health |
| `flag.evaluation.unknown_flag_rate` | Counter rate | > 0.1% | Flags used in code but not created in service |
| `flag.change_events.kafka_lag` | Gauge | > 100 | Config propagation pipeline health |
| `evaluate_api.error_rate_5xx` | Counter rate | > 0.1% | Client-side flag service health |
| `redis.snapshot.hit_rate` | Gauge | < 99% | Snapshot cache effectiveness |
| `audit.write.failure_rate` | Counter rate | > 0% | Compliance risk: any audit write failure is critical |

### Distributed Tracing

- Every flag change operation carries a `trace_id`. The trace spans: Management API write → Postgres transaction → Kafka publish → Config Snapshot Service rebuild → Redis write → SSE push to SDK.
- The propagation latency metric is derived from the difference between the Kafka message timestamp and the SSE server's push timestamp (both recorded in the trace).

### Logging

- Flag evaluation events are NOT logged per-evaluation (too high volume). Only aggregate counters (by flag × variant × day) are stored in `evaluation_stats`.
- Management API operations are logged with full request context: actor, flag_key, environment, changed fields.
- SDK connectivity events (connect, disconnect, reconnect, full-sync) are logged at INFO level in the SSE server for debugging propagation issues.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|---|---|---|---|
| Evaluation location (server-side) | In-process SDK | Remote API call per evaluation | Network latency (5-50ms) is unacceptable for high-frequency evaluations; in-process is < 0.1ms |
| Evaluation location (client-side) | Server-side evaluate API | Ship rules to browser | Shipping rules to browser exposes unreleased feature logic; server eval hides rule details |
| Update propagation | SSE (server-sent events) | WebSocket | SSE is unidirectional (sufficient), simpler, works over standard HTTP, and is natively supported by browsers |
| Mobile update propagation | 30-second polling | SSE | SSE maintains persistent TCP connection; drains mobile battery; polling is acceptable for mobile and uses less battery |
| Snapshot format | MessagePack (binary) | JSON | 60% smaller than JSON; faster to deserialize; SDK startup time reduced |
| Hash function for bucketing | MurmurHash3 | MD5/SHA-1/random | Non-cryptographic, fast, uniform distribution; same user always maps to same bucket |
| Audit log consistency | Write in same DB transaction as change | Async audit (fire-and-forget) | Async audit can be lost if the service crashes after the flag change but before the audit write |
| Segment evaluation | De-normalized into snapshot | Evaluated by querying DB at SDK init | SDK must be fully self-contained; DB queries during evaluation would violate the < 1ms requirement |

---

## 11. Follow-up Interview Questions

**Q1: How would you implement scheduled flag changes (e.g., enable a flag at a specific UTC time)?**
A: Add a `scheduled_changes` table with `flag_id`, `environment_id`, `change_spec` (JSONB of the update to apply), and `execute_at` (timestamp). A scheduler service polls for `execute_at < NOW()` records every 10 seconds, applies them via the same flag update code path (triggering audit log and propagation), and marks them `executed`. For precision, use a separate high-priority queue (e.g., a Redis sorted set keyed by `execute_at` epoch) that the scheduler can consume more efficiently.

**Q2: How do you prevent a bad flag configuration from taking down an entire service?**
A: Three safeguards: (1) SDK default values: `flags.evaluate("my-flag", user_ctx, default=False)` — if the flag is missing or evaluation throws an exception, the default is returned. (2) Try-catch wrapping in the evaluation engine: any exception in condition evaluation falls through to the default variant. (3) Validation in the Management API: rule conditions are syntactically and semantically validated before being saved (e.g., regex patterns are compiled to verify validity, numeric comparisons verify the attribute is expected to be numeric). The SDK is designed to never throw an exception that propagates to the application.

**Q3: How do you handle GDPR-compliant deletion — if a user requests to be forgotten, their targeting rule history may contain their user ID?**
A: The `rule_conditions` table stores attribute names and values — it might contain lists of user IDs for targeted rollouts. User IDs in explicit targeting rules must be replaced with pseudonymous identifiers (e.g., a one-way hash of the user ID) rather than raw user IDs. The audit log stores `user_key` (SDK's user.key field) which is also pseudonymous — the SDK never sends PII as the user key. If PII attributes (email, name) are used in rule conditions, they must be anonymized in the audit records. A GDPR deletion process scans audit records and redacts PII fields.

**Q4: How would you implement flag dependence on real-time data (e.g., "enable this flag if the current server error rate is below 0.1%")?**
A: This is a "contextual flag" or "dynamic targeting rule." The approach: define a special attribute prefix `$metric.` that triggers real-time metric lookup. The evaluation engine, when it encounters a condition like `$metric.error_rate < 0.001`, calls a metrics sidecar (a local HTTP call to a metrics agent process, < 1ms response time from a local cache). The metrics agent is pre-loaded with 30-second rolling aggregates from Prometheus. This is a power feature and requires careful performance characterization — not suitable for conditions on every flag.

**Q5: How do you test a flag change before rolling it out to any real users?**
A: Test environments (dev/staging) with dedicated SDK keys. Additionally, the Management API supports a `preview_evaluate` endpoint: pass a user context and flag rules (not yet saved), and get back what the evaluation result would be. This allows product managers to verify rule logic in the UI before saving. For load testing, the SDK supports a `mock_rules` initialization mode where the SDK uses in-memory rules provided programmatically, bypassing the flag service entirely.

**Q6: How would you support a "flag dependency graph" feature where one flag's evaluation is shown in the context of all its prerequisites?**
A: Build a `flag_prerequisite_graph` view by traversing the `flag_rules` table where `rule_type = 'prerequisite'`. Store the adjacency list in Redis as a directed graph. Expose a `GET /flags/{flag_key}/dependencies` endpoint that returns the full dependency tree (depth-first, with cycle detection). The UI renders this as a DAG visualization.

**Q7: What's the impact of evaluating flags for anonymous (unauthenticated) users?**
A: Anonymous users don't have a stable `user.key`. Options: (1) Generate a random `anonymous_id` and store it in a cookie/localStorage — consistent within a session. (2) Use the session ID as the user key. (3) Use the device ID for mobile. The key property required is that percentage rollouts are consistent: `hash(seed.flag_key.user_key)` produces the same bucket as long as the user_key is stable across page loads. For fully anonymous users (no cookie, no session), flag evaluations use only attribute-based targeting (country, browser, OS) — no percentage rollout.

**Q8: How would you support centralized flag governance (a platform team approves all new flags before creation)?**
A: Add a `flag_lifecycle` state machine: `proposed → review → approved → active → archived`. New flags start in `proposed`. A platform team reviews and approves. Only `approved` flags can be enabled in production. This is enforced in the Management API: `PATCH /flags/{id}/environments/production {enabled: true}` fails if the flag is not `approved`. Auto-approval can be configured for non-production environments.

**Q9: How do you handle the flag service going down entirely for 30 minutes?**
A: Server-side SDKs continue evaluating from their in-memory cache — no user impact, no degradation. Client-side SDKs served their last evaluate API response are cached locally; re-requests during outage serve the cached version (stale). The risk: flag changes made during the outage are not propagated. When the service recovers, SDKs reconnect and receive the latest snapshot. The outage window is at most 30 minutes of staleness for connected SDKs — acceptable for feature flags (as opposed to security-critical configurations).

**Q10: How would you implement SDK analytics — understanding which flags are most evaluated and by what percentage of users?**
A: The SDK samples evaluation events (e.g., 1% of evaluations) and sends them to the `/sdk/events` endpoint in batches. The event processor aggregates by `(flag_key, variant, date)` and increments the `evaluation_stats` table. The dashboard reads from `evaluation_stats` for flag-level analytics. For user-level analytics (how many unique users saw each variant), use HyperLogLog in Redis — increment per-flag-per-variant HLL on each evaluation event, then query HLL count for unique user estimates.

**Q11: How would you design flag evaluation for IoT devices with intermittent connectivity?**
A: IoT SDKs bootstrap by downloading a compact flag snapshot (MessagePack-encoded, maximum 1 MB for critical flags only). The snapshot is stored in device flash memory. On network availability, the device polls for updates using the ETag-based polling endpoint. Evaluation runs on-device from the stored snapshot. The device includes `last_sync_timestamp` in telemetry so the platform can detect devices running very stale configurations (> 7 days) and alert operators.

**Q12: How do you ensure the flag service itself doesn't become a single point of failure for your entire microservices platform?**
A: The SDK's in-process evaluation is the primary defense: the flag service can be completely down and server-side evaluations continue. For client-side paths, a content delivery network (CDN) caches evaluate API responses for anonymous attributes (country, device type) with a 60-second TTL. Per-user evaluations are client-cached in localStorage. The management API has no impact on the data plane (evaluation) — taking it down for maintenance doesn't affect live traffic.

**Q13: What is "bootstrapping" for client-side SDKs and how do you implement it?**
A: On first page load, the client-side JS SDK makes a request to the evaluate API, which introduces a 20-100ms delay before flags are available. During this window, the app either shows a loading state or uses default values. To eliminate this, server-side rendering (SSR) can embed the evaluate response into the HTML page as a `<script>` tag: `window.__featureflags = {...}`. The SDK reads this bootstrap data synchronously on init, making flags available before any network request. The SDK still opens an SSE connection in the background for live updates.

**Q14: How do you manage flag tech debt — flags that are never cleaned up?**
A: Add flag lifecycle management: (1) A `stale_after` date on each flag (set at creation, defaults to 90 days). (2) A daily job scans for flags where `stale_after < NOW()` and sends a Slack notification to the flag's creator and team. (3) The Management UI shows a "flag graveyard" view of stale flags. (4) Flag usage telemetry: if a flag has zero evaluations in the last 30 days, it's automatically marked `candidate_for_removal`. (5) Code reference scanning: a CI check searches the codebase for flag key references; flags not referenced in code for > 30 days are highlighted for removal.

**Q15: How would you implement gradual rollout with automatic rollback if a metric degrades?**
A: Integrate with the A/B Testing Platform. When creating a percentage rollout rule, optionally attach a `guardrail_metric` (e.g., `checkout_error_rate < 2%`). The A/B Platform monitors the metric for users in the treatment bucket. If the guardrail is breached, the A/B Platform calls the Management API to set `rollout_percent = 0` for the flag rule (automatic rollback). The flag service records this as a system-initiated change in the audit log with reason `"Automatic rollback: guardrail metric checkout_error_rate exceeded threshold 2%"`.

---

## 12. References & Further Reading

- **LaunchDarkly Engineering Blog — Flag Evaluation Architecture**: https://launchdarkly.com/blog/flag-evaluation-using-local-evaluation-and-streaming/
- **Fowler, M. — Feature Toggles (Feature Flags)**: https://martinfowler.com/articles/feature-toggles.html — Canonical taxonomy of flag types (release, experiment, ops, permission toggles).
- **Bucketing and Hash Functions in A/B Testing**: https://engineering.linkedin.com/ab-testing/xlnt-platform-driving-ab-testing-linkedin
- **MurmurHash3 specification**: https://github.com/aappleby/smhasher — Non-cryptographic hash function used for consistent bucketing.
- **Server-Sent Events (SSE) W3C Specification**: https://html.spec.whatwg.org/multipage/server-sent-events.html
- **Redis Cluster Specification**: https://redis.io/docs/reference/cluster-spec/
- **OpenFeature — Open Standard for Feature Flags**: https://openfeature.dev/ — Vendor-neutral SDK API specification.
- **Unleash Feature Toggle System (open source reference implementation)**: https://github.com/Unleash/unleash
- **Flipper (Ruby feature flags, reference implementation)**: https://github.com/flippercloud/flipper
- **MessagePack specification**: https://msgpack.org/index.html — Binary serialization format used for compact snapshot encoding.
- **Hodges, J. (HyperLogLog for cardinality estimation)**: Flajolet, P. et al. (2007). HyperLogLog: the analysis of a near-optimal cardinality estimation algorithm. *DMTCS Proceedings*, AH, 137–156.
- **AWS Application Load Balancer — Sticky Sessions**: https://docs.aws.amazon.com/elasticloadbalancing/latest/application/sticky-sessions.html
- **SOC 2 Type II Compliance for SaaS**: https://www.aicpa.org/resources/landing/system-and-organization-controls-soc-suite-of-services
