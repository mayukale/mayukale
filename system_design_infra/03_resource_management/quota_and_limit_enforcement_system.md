# System Design: Quota and Limit Enforcement System

> **Relevance to role:** A cloud infrastructure platform engineer must enforce resource boundaries across multi-tenant environments to prevent noisy neighbors, control costs, and ensure fair resource distribution. This system spans Kubernetes ResourceQuota and LimitRange, OpenStack quota management, API rate limiting, and hierarchical organizational quota enforcement — all critical for operating shared bare-metal and cloud infrastructure at scale.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | Define hierarchical quotas: organization → team → project → user, with inheritance and override. |
| FR-2 | Enforce compute quotas: CPU cores, RAM, GPU count, GPU memory per quota scope. |
| FR-3 | Enforce storage quotas: PersistentVolumeClaim count and total size, ephemeral storage. |
| FR-4 | Enforce object count quotas: pods, services, configmaps, secrets, PVCs per namespace. |
| FR-5 | Enforce API rate limits: requests per second per tenant, with burst allowance (token bucket). |
| FR-6 | Support soft limits (warn but allow) and hard limits (reject if exceeded). |
| FR-7 | Support burst quota: allow temporary overage with billing implications and automatic reclamation. |
| FR-8 | Apply default resource limits (LimitRange) to workloads that don't specify them. |
| FR-9 | Provide quota request workflow: request → manager approval → allocation. |
| FR-10 | Real-time usage tracking and quota utilization dashboards. |
| FR-11 | Enforce quotas at admission time (prevent over-allocation) not just at runtime. |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Quota check latency (admission webhook) | < 10 ms |
| NFR-2 | Usage aggregation freshness | < 30 s lag |
| NFR-3 | API rate limit accuracy | < 1% false positive rate |
| NFR-4 | System availability | 99.99% |
| NFR-5 | Throughput (quota checks/sec) | 20,000 |
| NFR-6 | Quota update propagation | < 5 s |
| NFR-7 | Audit trail completeness | 100% of quota changes |

### Constraints & Assumptions

- Multi-tenant environment: 500 organizations, 5,000 teams, 50,000 projects.
- Kubernetes as primary orchestration layer (ResourceQuota + LimitRange are native primitives).
- OpenStack Cinder/Nova quotas for VM/storage management.
- Bare-metal provisioning has its own quota enforcement path.
- Quotas must be checkable without hitting the database on every request (caching is essential).
- Some quotas are "hard" (strictly enforced) and others are "soft" (allow overage with notification).

### Out of Scope

- Billing and cost allocation (separate system consumes quota usage data).
- Chargeback/showback dashboards (downstream from this system).
- Application-level rate limiting (this system handles infrastructure API rate limiting only).
- Network bandwidth quotas (handled by SDN layer).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Organizations | 500 | Given |
| Teams | 5,000 | ~10 teams per org |
| Projects | 50,000 | ~10 projects per team |
| Users | 100,000 | ~2 users per project average |
| Kubernetes namespaces | 50,000 | 1:1 with projects |
| Quota check requests/sec (peak) | 20,000 | 10,000 pod scheduling/sec + 10,000 API calls/sec |
| Quota modifications/day | 5,000 | New project creation, limit changes, approvals |
| API rate limit checks/sec | 100,000 | Every API call across all tenants |
| Usage aggregation events/sec | 50,000 | Pod create/delete/update events |

### Latency Requirements

| Operation | p50 | p99 | p999 |
|-----------|-----|-----|------|
| Quota admission check | 2 ms | 8 ms | 20 ms |
| API rate limit check | 0.5 ms | 2 ms | 5 ms |
| Usage aggregation update | 100 ms | 500 ms | 2 s |
| Quota modification (admin) | 50 ms | 200 ms | 1 s |
| Quota request approval workflow | N/A (human) | N/A | N/A |
| Dashboard query | 200 ms | 1 s | 3 s |

### Storage Estimates

| Data | Size per record | Count | Total |
|------|-----------------|-------|-------|
| Quota definitions | 2 KB | 50,000 (projects) + 5,000 (teams) + 500 (orgs) | 111 MB |
| Current usage snapshots | 1 KB | 55,500 scopes | 55 MB |
| Quota change history (1 year) | 0.5 KB | 1,825,000 (5,000/day x 365) | 912 MB |
| Rate limit counters | 64 bytes | 100,000 (per-user buckets) | 6.4 MB |
| Quota request records | 2 KB | 100,000/year | 200 MB |
| **Total** | | | **~1.3 GB** |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Quota checks inbound | 20,000/sec x 0.5 KB request | 10 MB/s |
| Quota check responses | 20,000/sec x 0.2 KB | 4 MB/s |
| Rate limit checks | 100,000/sec x 0.1 KB | 10 MB/s |
| Usage event stream | 50,000/sec x 0.2 KB | 10 MB/s |
| **Total** | | **~34 MB/s** |

---

## 3. High Level Architecture

```
                         ┌──────────────────────────┐
                         │       API Gateway          │
                         │  ┌────────────────────┐   │
                         │  │  Rate Limit Filter  │   │
                         │  │  (Token Bucket per  │   │
                         │  │   tenant, in Redis) │   │
                         │  └────────────────────┘   │
                         └──────────┬─────────────────┘
                                    │
                   ┌────────────────┼──────────────────┐
                   │                │                   │
          ┌────────▼──────┐  ┌─────▼───────┐  ┌───────▼───────┐
          │ K8s API Server │  │ OpenStack   │  │ Bare Metal    │
          │ + Admission    │  │ API (Nova,  │  │ Provisioner   │
          │   Webhooks     │  │ Cinder)     │  │ API           │
          └────────┬───────┘  └─────┬───────┘  └───────┬───────┘
                   │                │                   │
                   └────────────────┼───────────────────┘
                                    │
                         ┌──────────▼──────────────┐
                         │   Quota Enforcement      │
                         │   Service (QES)          │
                         │                          │
                         │  ┌────────────────────┐  │
                         │  │ Admission Checker   │  │  ← synchronous, < 10ms
                         │  │ (cached quota +     │  │
                         │  │  usage lookup)      │  │
                         │  └────────────────────┘  │
                         │  ┌────────────────────┐  │
                         │  │ LimitRange Defaulter│  │  ← applies default limits
                         │  └────────────────────┘  │
                         │  ┌────────────────────┐  │
                         │  │ Burst Quota Manager │  │  ← tracks temporary overages
                         │  └────────────────────┘  │
                         └──────────┬───────────────┘
                                    │
                   ┌────────────────┼──────────────────┐
                   │                │                   │
          ┌────────▼───────┐ ┌─────▼───────┐ ┌────────▼───────┐
          │  Quota Store   │ │ Usage        │ │ Approval       │
          │  (MySQL 8.0)   │ │ Aggregator   │ │ Workflow       │
          │  Source of     │ │ (Prometheus  │ │ Engine         │
          │  truth for     │ │  + custom    │ │ (request →     │
          │  quota defs    │ │  aggregation)│ │  approve →     │
          └────────────────┘ └─────────────┘ │  allocate)     │
                                              └────────────────┘
                   ┌──────────────────────────────────────────┐
                   │            Real-time Cache               │
                   │  ┌──────────┐  ┌─────────────────────┐  │
                   │  │  Redis    │  │  Local In-process    │  │
                   │  │  Cluster  │  │  Cache (Caffeine)    │  │
                   │  │  (usage   │  │  (quota definitions  │  │
                   │  │   counts) │  │   + computed limits) │  │
                   │  └──────────┘  └─────────────────────┘  │
                   └──────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **API Gateway Rate Limiter** | First line of defense. Token bucket per tenant per API endpoint. Implemented in Redis (EVALSHA for atomic increment + check). Rejects requests exceeding rate limits before they reach backend services. |
| **K8s Admission Webhooks** | ValidatingWebhookConfiguration that calls the Quota Enforcement Service on every CREATE/UPDATE for pods, deployments, PVCs, services. Also MutatingWebhookConfiguration for LimitRange defaulting. |
| **Quota Enforcement Service (QES)** | Core service. Runs admission checks against cached quota definitions and usage. Handles LimitRange defaulting. Manages burst quota accounting. |
| **Quota Store (MySQL)** | Source of truth for quota definitions (hierarchical), usage snapshots, approval records, and audit history. |
| **Usage Aggregator** | Consumes pod/VM lifecycle events, aggregates resource usage per quota scope (project, team, org), and writes to Redis + MySQL. Uses Prometheus for metric collection and custom aggregation logic for quota accounting. |
| **Approval Workflow Engine** | Manages the quota request lifecycle: user requests increase → manager reviews → approval/denial → automatic allocation. Integrates with Slack/Teams for notifications. |
| **Redis Cluster** | Stores real-time usage counters (atomic increments on pod create, decrements on pod delete). Rate limit buckets (token bucket state). Quota definitions cache. |
| **Local Cache** | In-process cache in each QES instance for quota definitions and computed limits. Reduces Redis round-trips for the hottest data. |

### Data Flows

**Primary — Quota Admission Check (pod creation):**
1. User creates a pod (kubectl apply).
2. K8s API server calls ValidatingWebhook on QES.
3. QES checks local cache for quota definition of the target namespace.
4. QES reads current usage from Redis (atomic GET).
5. QES computes: `current_usage + pod_request <= quota_limit`.
6. If within quota: ALLOW. If soft limit exceeded: ALLOW + emit warning event. If hard limit exceeded: DENY with error message.
7. On ALLOW, QES atomically increments usage counter in Redis (INCRBY).
8. MySQL is updated asynchronously (usage snapshot every 30 seconds).

**Secondary — LimitRange Defaulting:**
1. User creates a pod without resource limits.
2. K8s API server calls MutatingWebhook on QES.
3. QES looks up LimitRange for the namespace.
4. QES injects default limits (e.g., CPU limit = 2 cores, RAM limit = 4 GiB) and default requests (e.g., CPU request = 0.5 cores, RAM request = 1 GiB).
5. Returns mutated pod spec.

**Tertiary — Quota Request Workflow:**
1. User submits quota increase request via API/UI.
2. Request stored in MySQL with status `PENDING`.
3. Notification sent to team manager via Slack.
4. Manager approves/denies via Slack button or web UI.
5. On approval: quota definition updated in MySQL, propagated to Redis and local caches within 5 seconds.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Hierarchical quota scopes
CREATE TABLE quota_scopes (
    scope_id         CHAR(36) PRIMARY KEY,
    scope_type       ENUM('organization','team','project','user') NOT NULL,
    scope_name       VARCHAR(255) NOT NULL,
    parent_scope_id  CHAR(36),
    -- Kubernetes namespace mapping (for project-level)
    k8s_namespace    VARCHAR(255),
    -- OpenStack project ID mapping
    openstack_project_id VARCHAR(64),
    -- Metadata
    owner_email      VARCHAR(255),
    cost_center      VARCHAR(64),
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    FOREIGN KEY (parent_scope_id) REFERENCES quota_scopes(scope_id),
    UNIQUE INDEX idx_scope_type_name (scope_type, scope_name),
    INDEX idx_scope_parent (parent_scope_id),
    INDEX idx_scope_namespace (k8s_namespace)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Quota definitions (what's allowed)
CREATE TABLE quota_definitions (
    quota_id         CHAR(36) PRIMARY KEY,
    scope_id         CHAR(36) NOT NULL,
    resource_type    ENUM('cpu_cores','ram_mb','gpu_count','gpu_mem_mb',
                          'disk_gb','pvc_count','pvc_total_gb',
                          'ephemeral_storage_gb','pod_count','service_count',
                          'configmap_count','secret_count',
                          'api_requests_per_sec','api_requests_per_min') NOT NULL,
    -- Limits
    hard_limit       BIGINT NOT NULL,              -- absolute maximum, requests rejected
    soft_limit       BIGINT,                       -- warning threshold, requests allowed but flagged
    burst_limit      BIGINT,                       -- temporary max (e.g., 120% of hard limit)
    burst_duration_sec INT DEFAULT 3600,           -- max burst duration (1 hour default)
    -- Defaults (LimitRange equivalent)
    default_request  BIGINT,                       -- default per-workload request
    default_limit    BIGINT,                       -- default per-workload limit
    max_per_workload BIGINT,                       -- max any single workload can request
    min_per_workload BIGINT,                       -- min any single workload must request
    -- Inheritance
    inherited        BOOLEAN NOT NULL DEFAULT FALSE, -- inherited from parent scope
    override_parent  BOOLEAN NOT NULL DEFAULT FALSE, -- overrides parent's limit
    -- Versioning
    version          BIGINT NOT NULL DEFAULT 1,
    effective_from   TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    effective_until   TIMESTAMP(3),                -- NULL = no expiry
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    FOREIGN KEY (scope_id) REFERENCES quota_scopes(scope_id),
    UNIQUE INDEX idx_quota_scope_resource (scope_id, resource_type),
    INDEX idx_quota_resource (resource_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Current usage tracking (real-time)
CREATE TABLE quota_usage (
    usage_id         CHAR(36) PRIMARY KEY,
    scope_id         CHAR(36) NOT NULL,
    resource_type    ENUM('cpu_cores','ram_mb','gpu_count','gpu_mem_mb',
                          'disk_gb','pvc_count','pvc_total_gb',
                          'ephemeral_storage_gb','pod_count','service_count',
                          'configmap_count','secret_count',
                          'api_requests_per_sec','api_requests_per_min') NOT NULL,
    current_usage    BIGINT NOT NULL DEFAULT 0,
    peak_usage       BIGINT NOT NULL DEFAULT 0,    -- historical peak
    peak_timestamp   TIMESTAMP(3),
    -- Burst tracking
    in_burst         BOOLEAN NOT NULL DEFAULT FALSE,
    burst_start      TIMESTAMP(3),
    burst_usage      BIGINT NOT NULL DEFAULT 0,    -- usage during current burst
    last_updated     TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    FOREIGN KEY (scope_id) REFERENCES quota_scopes(scope_id),
    UNIQUE INDEX idx_usage_scope_resource (scope_id, resource_type),
    INDEX idx_usage_burst (in_burst)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Quota change requests (approval workflow)
CREATE TABLE quota_requests (
    request_id       CHAR(36) PRIMARY KEY,
    scope_id         CHAR(36) NOT NULL,
    resource_type    ENUM('cpu_cores','ram_mb','gpu_count','gpu_mem_mb',
                          'disk_gb','pvc_count','pvc_total_gb',
                          'ephemeral_storage_gb','pod_count','service_count',
                          'configmap_count','secret_count',
                          'api_requests_per_sec','api_requests_per_min') NOT NULL,
    current_limit    BIGINT NOT NULL,
    requested_limit  BIGINT NOT NULL,
    justification    TEXT NOT NULL,
    -- Workflow state
    status           ENUM('pending','approved','denied','auto_approved','expired','cancelled') NOT NULL DEFAULT 'pending',
    requester_id     CHAR(36) NOT NULL,
    reviewer_id      CHAR(36),
    review_comment   TEXT,
    reviewed_at      TIMESTAMP(3),
    -- Auto-approval rules
    auto_approvable  BOOLEAN NOT NULL DEFAULT FALSE, -- true if within auto-approve thresholds
    expires_at       TIMESTAMP(3),                   -- request expires if not reviewed
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    FOREIGN KEY (scope_id) REFERENCES quota_scopes(scope_id),
    INDEX idx_requests_status (status),
    INDEX idx_requests_scope (scope_id),
    INDEX idx_requests_reviewer (reviewer_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Rate limit configuration
CREATE TABLE rate_limit_configs (
    config_id        CHAR(36) PRIMARY KEY,
    scope_id         CHAR(36) NOT NULL,
    endpoint_pattern VARCHAR(255) NOT NULL,         -- e.g., "POST /v1/pods", "* /v1/*"
    -- Token bucket parameters
    tokens_per_sec   INT NOT NULL,                  -- refill rate
    bucket_size      INT NOT NULL,                  -- max burst size
    -- Response when limited
    retry_after_sec  INT NOT NULL DEFAULT 60,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    FOREIGN KEY (scope_id) REFERENCES quota_scopes(scope_id),
    UNIQUE INDEX idx_ratelimit_scope_endpoint (scope_id, endpoint_pattern)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Audit log for all quota changes
CREATE TABLE quota_audit_log (
    audit_id         BIGINT AUTO_INCREMENT PRIMARY KEY,
    scope_id         CHAR(36) NOT NULL,
    resource_type    VARCHAR(64) NOT NULL,
    action           ENUM('quota_created','quota_updated','quota_deleted',
                          'usage_increased','usage_decreased','burst_started',
                          'burst_ended','soft_limit_exceeded','hard_limit_rejected',
                          'request_submitted','request_approved','request_denied') NOT NULL,
    actor_id         CHAR(36),                     -- who made the change
    old_value        BIGINT,
    new_value        BIGINT,
    details          JSON,
    created_at       TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    INDEX idx_audit_scope (scope_id, created_at),
    INDEX idx_audit_action (action, created_at),
    INDEX idx_audit_time (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Criteria | MySQL 8.0 | Redis | etcd | PostgreSQL |
|----------|-----------|-------|------|------------|
| **Use for** | Quota definitions, usage history, audit trail | Real-time usage counters, rate limit buckets | K8s ResourceQuota objects | Alternative to MySQL |
| **Consistency** | Strong (ACID) | Eventual (single-key atomic) | Strong (Raft) | Strong (ACID) |
| **Performance for counters** | Moderate (row lock per update) | Excellent (INCR, O(1)) | Not designed for high-frequency counters | Moderate |
| **Hierarchical queries** | CTE support (MySQL 8.0+) | Not suited | Not suited | Excellent CTE support |
| **JSON support** | Good | Native data structures | Key-value only | Excellent |

**Selected: MySQL 8.0 (source of truth) + Redis (real-time counters).**

**Justification:** MySQL handles the relational aspects well: hierarchical quota scopes, approval workflows, audit trails, and complex analytics queries. Redis handles the hot path: real-time counter increments (INCRBY, DECRBY) for usage tracking and token bucket state for rate limiting. The combination keeps admission checks under 10ms (Redis read) while maintaining full ACID guarantees for configuration changes (MySQL).

### Indexing Strategy

| Index | Table | Purpose |
|-------|-------|---------|
| `idx_quota_scope_resource` | quota_definitions | Look up quota for a specific scope + resource type (primary lookup) |
| `idx_usage_scope_resource` | quota_usage | Look up current usage for admission check |
| `idx_scope_parent` | quota_scopes | Traverse hierarchy for inherited quotas |
| `idx_scope_namespace` | quota_scopes | Map k8s namespace to quota scope |
| `idx_requests_reviewer` | quota_requests | Reviewer dashboard: pending requests for a specific reviewer |
| `idx_audit_scope` | quota_audit_log | Audit trail per scope, time-ordered |

---

## 5. API Design

### REST Endpoints

```
# Quota Management
POST   /v1/quotas                      # Create quota definition
GET    /v1/quotas?scope_id={id}        # List quotas for a scope
GET    /v1/quotas/{id}                 # Get specific quota
PUT    /v1/quotas/{id}                 # Update quota
DELETE /v1/quotas/{id}                 # Delete quota

# Usage
GET    /v1/usage?scope_id={id}         # Get current usage for a scope
GET    /v1/usage/history?scope_id={id}&from={ts}&to={ts}  # Historical usage

# Quota Requests
POST   /v1/quota-requests              # Submit a quota increase request
GET    /v1/quota-requests?status=pending&reviewer={id}  # List pending requests
POST   /v1/quota-requests/{id}/approve # Approve a request
POST   /v1/quota-requests/{id}/deny    # Deny a request

# Rate Limits
POST   /v1/rate-limits                 # Create rate limit config
GET    /v1/rate-limits?scope_id={id}   # List rate limits for a scope
PUT    /v1/rate-limits/{id}            # Update rate limit

# Admission (internal, called by webhooks)
POST   /v1/admit                       # Check if resource creation is within quota
POST   /v1/default                     # Apply LimitRange defaults

# Hierarchy
GET    /v1/scopes/{id}/tree            # Get full hierarchy under a scope
GET    /v1/scopes/{id}/effective-quotas # Get effective quotas (inherited + overrides)
```

#### Full Schema Examples

**POST /v1/admit** (admission check):
```json
// Request
{
    "idempotency_key": "uuid-123",
    "namespace": "ml-team-dev",
    "resource_type": "pod",
    "resources": {
        "cpu_cores": 4,
        "ram_mb": 8192,
        "gpu_count": 1
    },
    "workload_id": "pod-abc123",
    "auth_token": "Bearer eyJ..."
}

// Response (allowed)
{
    "allowed": true,
    "warnings": [],
    "quota_status": {
        "cpu_cores": {"used": 450, "hard_limit": 1000, "soft_limit": 800},
        "ram_mb": {"used": 1048576, "hard_limit": 2097152, "soft_limit": 1572864},
        "gpu_count": {"used": 15, "hard_limit": 32, "soft_limit": null}
    }
}

// Response (denied)
{
    "allowed": false,
    "reason": "quota exceeded: gpu_count usage (32) + request (1) exceeds hard limit (32)",
    "quota_status": {
        "gpu_count": {"used": 32, "hard_limit": 32, "soft_limit": null}
    },
    "suggestion": "Request a quota increase at POST /v1/quota-requests or release unused GPU allocations."
}
```

**Rate limiting headers (on every API response):**
```
X-RateLimit-Limit: 1000
X-RateLimit-Remaining: 742
X-RateLimit-Reset: 1680000060
Retry-After: 60  (only when rate limited, HTTP 429)
```

### CLI Design

```bash
# Quota management
quota-ctl quota create \
    --scope=project:ml-team-dev \
    --resource=gpu_count \
    --hard-limit=32 \
    --soft-limit=24 \
    --burst-limit=40 \
    --burst-duration=3600

quota-ctl quota list --scope=team:ml-team --include-children --output=table

quota-ctl quota update \
    --scope=project:ml-team-dev \
    --resource=cpu_cores \
    --hard-limit=2000

# Usage queries
quota-ctl usage show --scope=project:ml-team-dev --output=table
# Output:
# RESOURCE         USED     SOFT LIMIT  HARD LIMIT  UTILIZATION
# cpu_cores        450      800         1000        45.0%
# ram_mb           1048576  1572864     2097152     50.0%
# gpu_count        15       24          32          46.9%
# pod_count        127      -           500         25.4%

quota-ctl usage history --scope=project:ml-team-dev --resource=gpu_count --from=7d --output=chart

# Quota requests
quota-ctl request create \
    --scope=project:ml-team-dev \
    --resource=gpu_count \
    --new-limit=64 \
    --justification="Large model training requires 64 GPUs for 2 weeks"

quota-ctl request list --status=pending --reviewer=me
quota-ctl request approve req-uuid-123 --comment="Approved for Q2 training budget"

# LimitRange (defaults)
quota-ctl limits set \
    --scope=project:ml-team-dev \
    --resource=cpu_cores \
    --default-request=0.5 \
    --default-limit=2 \
    --max-per-workload=32 \
    --min-per-workload=0.1

# Rate limits
quota-ctl ratelimit set \
    --scope=team:ml-team \
    --endpoint="POST /v1/pods" \
    --rate=100/sec \
    --burst=200

# Hierarchy view
quota-ctl tree --scope=org:engineering --depth=3
# Output:
# org:engineering (CPU: 10000 cores, GPU: 200)
# ├── team:ml-team (CPU: 3000, GPU: 128)
# │   ├── project:ml-team-dev (CPU: 1000, GPU: 32)
# │   ├── project:ml-team-staging (CPU: 500, GPU: 16)
# │   └── project:ml-team-prod (CPU: 1500, GPU: 80)
# ├── team:backend (CPU: 4000, GPU: 0)
# │   ├── project:api-service (CPU: 2000, GPU: 0)
# │   └── project:batch-jobs (CPU: 2000, GPU: 0)
# └── team:data (CPU: 3000, GPU: 72)
```

---

## 6. Core Component Deep Dives

### 6.1 Hierarchical Quota Resolution Engine

**Why it's hard:**
Quotas at different hierarchy levels interact in complex ways. A project's quota cannot exceed its team's quota, and a team's quota cannot exceed the organization's quota. But the *sum* of all projects under a team can exceed the team quota (oversubscription). Resolving the "effective quota" for a given scope requires traversing the hierarchy, applying inheritance rules, and handling overrides. This must happen in < 10ms for admission checks.

**Approaches Compared:**

| Approach | Lookup Speed | Consistency | Flexibility | Complexity |
|----------|-------------|-------------|-------------|------------|
| Flat (no hierarchy) | O(1) | Simple | None | Very Low |
| Tree traversal per request | O(depth) per check | Always correct | Full | Medium |
| Materialized effective quotas | O(1) | Eventually consistent (needs recomputation) | Full | High |
| Cached tree with invalidation | O(1) amortized, O(depth) on miss | Consistent within TTL | Full | Medium-High |

**Selected: Cached tree with invalidation.**

**Justification:** We cache the full resolved quota tree in each QES instance (local Caffeine cache) and in Redis. When a quota definition changes, we invalidate the affected subtree (the scope and all its children). Cache misses trigger a tree traversal from MySQL. This gives O(1) lookups for 99%+ of requests while maintaining correctness.

**Implementation (pseudocode):**

```python
class QuotaResolver:
    def __init__(self):
        self.cache = LocalCache(max_size=100_000, ttl=30)  # 30-second TTL
        self.redis = RedisClient()
        self.db = MySQLClient()
    
    def get_effective_quota(self, scope_id: str, resource_type: str) -> EffectiveQuota:
        """Resolve the effective quota for a scope, considering hierarchy."""
        cache_key = f"quota:{scope_id}:{resource_type}"
        
        # L1: Local cache
        cached = self.cache.get(cache_key)
        if cached:
            return cached
        
        # L2: Redis cache
        cached = self.redis.get(cache_key)
        if cached:
            result = EffectiveQuota.deserialize(cached)
            self.cache.put(cache_key, result)
            return result
        
        # L3: Compute from database
        result = self._resolve_from_db(scope_id, resource_type)
        
        # Cache at both levels
        self.redis.setex(cache_key, 30, result.serialize())
        self.cache.put(cache_key, result)
        
        return result
    
    def _resolve_from_db(self, scope_id: str, resource_type: str) -> EffectiveQuota:
        """Walk up the hierarchy to resolve effective quota."""
        # Get the scope chain: project -> team -> org
        chain = self._get_ancestor_chain(scope_id)
        
        # Start from the top (org), apply each level's overrides
        effective_hard = float('inf')
        effective_soft = float('inf')
        effective_burst = float('inf')
        default_request = None
        default_limit = None
        max_per_workload = float('inf')
        min_per_workload = 0
        
        for scope in chain:  # ordered from org (top) to project (bottom)
            quota = self.db.query(
                "SELECT * FROM quota_definitions "
                "WHERE scope_id = %s AND resource_type = %s "
                "AND (effective_until IS NULL OR effective_until > NOW())",
                scope.scope_id, resource_type
            )
            if quota:
                # Each level can tighten (but not loosen) the limit
                if quota.override_parent:
                    # Override: use this level's values directly
                    effective_hard = quota.hard_limit
                    effective_soft = quota.soft_limit or effective_soft
                    effective_burst = quota.burst_limit or effective_burst
                else:
                    # Inherit: take the minimum of parent and this level
                    effective_hard = min(effective_hard, quota.hard_limit)
                    if quota.soft_limit:
                        effective_soft = min(effective_soft, quota.soft_limit)
                    if quota.burst_limit:
                        effective_burst = min(effective_burst, quota.burst_limit)
                
                # LimitRange defaults: most specific (lowest) level wins
                if quota.default_request is not None:
                    default_request = quota.default_request
                if quota.default_limit is not None:
                    default_limit = quota.default_limit
                if quota.max_per_workload is not None:
                    max_per_workload = min(max_per_workload, quota.max_per_workload)
                if quota.min_per_workload is not None:
                    min_per_workload = max(min_per_workload, quota.min_per_workload)
        
        return EffectiveQuota(
            hard_limit=effective_hard if effective_hard != float('inf') else None,
            soft_limit=effective_soft if effective_soft != float('inf') else None,
            burst_limit=effective_burst if effective_burst != float('inf') else None,
            default_request=default_request,
            default_limit=default_limit,
            max_per_workload=max_per_workload if max_per_workload != float('inf') else None,
            min_per_workload=min_per_workload if min_per_workload > 0 else None
        )
    
    def _get_ancestor_chain(self, scope_id: str) -> List[Scope]:
        """Get all ancestors from root to this scope."""
        chain = []
        current = scope_id
        while current:
            scope = self.db.query(
                "SELECT * FROM quota_scopes WHERE scope_id = %s", current)
            chain.append(scope)
            current = scope.parent_scope_id
        chain.reverse()  # root first
        return chain
    
    def invalidate_subtree(self, scope_id: str, resource_type: str):
        """Called when a quota definition changes. Invalidates the scope and all descendants."""
        # Get all descendant scope IDs using recursive CTE
        descendants = self.db.query("""
            WITH RECURSIVE subtree AS (
                SELECT scope_id FROM quota_scopes WHERE scope_id = %s
                UNION ALL
                SELECT qs.scope_id FROM quota_scopes qs
                JOIN subtree st ON qs.parent_scope_id = st.scope_id
            )
            SELECT scope_id FROM subtree
        """, scope_id)
        
        # Invalidate all caches
        keys = [f"quota:{d.scope_id}:{resource_type}" for d in descendants]
        self.redis.delete(*keys)
        for key in keys:
            self.cache.invalidate(key)
```

**Failure Modes:**
1. **Cache stampede after invalidation:** 50,000 projects invalidated at once → all hit MySQL simultaneously. Mitigation: probabilistic early expiration (jitter) and lock-based cache population (only one thread recomputes, others wait).
2. **Inconsistent hierarchy:** A team's total child quotas exceed the team quota. Mitigation: this is allowed by design (oversubscription). The effective quota is always `min(parent, self)` unless `override_parent=true`.
3. **Stale cache during approval:** Quota increased but cache hasn't updated yet → admission still rejects. Mitigation: quota modification directly invalidates cache before returning success. Also, the 30-second TTL ensures eventual correctness.

**Interviewer Q&As:**

**Q1: How do you handle the case where the sum of child quotas exceeds the parent quota?**
A: This is intentional oversubscription, just like CPU overcommit. A team with 1,000 CPU cores might give each of 5 projects 400 cores (total 2,000). In practice, not all projects use their full allocation simultaneously. The parent quota acts as a hard ceiling — the team can never actually use more than 1,000 cores total. Usage is tracked at each level; admission is checked at every level in the chain.

**Q2: How does the hierarchy depth affect admission check latency?**
A: With 4 levels (org → team → project → user), cache hits return in < 1ms. Cache misses require 4 MySQL queries (or 1 with a CTE). In practice, 95%+ are cache hits because quota definitions change infrequently. Worst case (cold cache): ~5ms.

**Q3: What happens if a parent quota is reduced below the sum of current child usage?**
A: Existing workloads are not immediately terminated (that would be too disruptive). The parent's usage counter continues to reflect actual usage. New workload admissions are rejected until usage drops below the new limit. An alert is sent to the admin showing the over-quota state.

**Q4: How do you handle cross-scope quota transfers?**
A: Team admins can transfer quota between projects using the quota update API. This is an atomic operation: decrease project A's quota and increase project B's quota within a single MySQL transaction. The team-level total remains unchanged.

**Q5: How do you prevent a user from circumventing quotas by creating multiple projects?**
A: The team-level quota is the ultimate constraint. Creating 10 projects under a team doesn't increase the team's total capacity. Also, project creation requires team admin approval, and there's a max projects-per-team configuration.

**Q6: How does this integrate with Kubernetes ResourceQuota?**
A: We sync our quota definitions to Kubernetes ResourceQuota objects. When our quota changes, a controller updates the corresponding ResourceQuota in the target namespace. The Kubernetes admission controller enforces quotas natively, and our system provides the source of truth and the hierarchical management layer on top.

---

### 6.2 Token Bucket Rate Limiter

**Why it's hard:**
Rate limiting at 100,000 requests/sec across a distributed system (multiple API gateway instances) requires atomic counter operations with sub-millisecond latency. The token bucket algorithm must handle burst correctly, prevent race conditions between gateway instances, and support per-tenant per-endpoint granularity. Distributed rate limiting is fundamentally harder than single-process rate limiting.

**Approaches Compared:**

| Approach | Accuracy | Latency | Distributed | Burst Support |
|----------|----------|---------|-------------|---------------|
| Fixed window counter | Low (boundary burst) | Very low | Easy (Redis INCR) | No |
| Sliding window log | High | High (store all timestamps) | Hard | Natural |
| Sliding window counter | Medium | Low | Medium | Approximate |
| Token bucket | High | Low | Medium | Explicit burst size |
| Leaky bucket | High | Low | Medium | No (smoothing) |
| Token bucket in Redis (Lua script) | High | Very low | Yes | Yes |

**Selected: Token bucket implemented in Redis via Lua script.**

**Justification:** Token bucket provides explicit control over both the sustained rate and burst size. Redis Lua scripts execute atomically (no race conditions between gateway instances). Sub-millisecond latency at 100K ops/sec. Used by AWS, Stripe, and most large-scale API platforms.

**Implementation:**

```lua
-- Redis Lua script for token bucket rate limiting
-- KEYS[1] = rate limit key (e.g., "rl:{tenant_id}:{endpoint}")
-- ARGV[1] = tokens_per_sec (refill rate)
-- ARGV[2] = bucket_size (max tokens)
-- ARGV[3] = current_time_ms (unix timestamp in milliseconds)
-- ARGV[4] = tokens_requested (usually 1)

local key = KEYS[1]
local rate = tonumber(ARGV[1])
local capacity = tonumber(ARGV[2])
local now = tonumber(ARGV[3])
local requested = tonumber(ARGV[4])

-- Get current state
local state = redis.call('HMGET', key, 'tokens', 'last_refill')
local tokens = tonumber(state[1])
local last_refill = tonumber(state[2])

-- Initialize if first request
if tokens == nil then
    tokens = capacity
    last_refill = now
end

-- Refill tokens based on elapsed time
local elapsed_sec = (now - last_refill) / 1000.0
local refill = elapsed_sec * rate
tokens = math.min(capacity, tokens + refill)
last_refill = now

-- Check if request can be allowed
local allowed = 0
local remaining = 0

if tokens >= requested then
    tokens = tokens - requested
    allowed = 1
    remaining = math.floor(tokens)
else
    remaining = math.floor(tokens)
    allowed = 0
end

-- Save state
redis.call('HMSET', key, 'tokens', tostring(tokens), 'last_refill', tostring(last_refill))
redis.call('EXPIRE', key, math.ceil(capacity / rate) + 60)  -- Auto-expire inactive buckets

-- Return: allowed (0/1), remaining tokens, retry_after_ms (0 if allowed)
local retry_after = 0
if allowed == 0 then
    retry_after = math.ceil((requested - tokens) / rate * 1000)
end

return {allowed, remaining, retry_after}
```

**Python wrapper:**

```python
class DistributedRateLimiter:
    def __init__(self, redis_client: Redis):
        self.redis = redis_client
        self.script_sha = self._load_script()
    
    def _load_script(self) -> str:
        """Load the Lua script into Redis and return the SHA."""
        return self.redis.script_load(TOKEN_BUCKET_LUA_SCRIPT)
    
    def check_rate_limit(self, tenant_id: str, endpoint: str, 
                         tokens_per_sec: int, bucket_size: int) -> RateLimitResult:
        key = f"rl:{tenant_id}:{endpoint}"
        now_ms = int(time.time() * 1000)
        
        result = self.redis.evalsha(
            self.script_sha, 1, key,
            tokens_per_sec, bucket_size, now_ms, 1)
        
        allowed, remaining, retry_after_ms = result
        
        return RateLimitResult(
            allowed=bool(allowed),
            remaining=remaining,
            retry_after_sec=retry_after_ms / 1000 if retry_after_ms > 0 else 0,
            limit=bucket_size,
        )
    
    def get_rate_limit_config(self, tenant_id: str, endpoint: str) -> Tuple[int, int]:
        """Look up rate limit config for a tenant + endpoint."""
        # Check cache first
        config = self.config_cache.get(f"{tenant_id}:{endpoint}")
        if config:
            return config.tokens_per_sec, config.bucket_size
        
        # Fallback: query MySQL
        # Match most specific endpoint pattern first
        config = self.db.query("""
            SELECT tokens_per_sec, bucket_size 
            FROM rate_limit_configs rlc
            JOIN quota_scopes qs ON rlc.scope_id = qs.scope_id
            WHERE qs.scope_id IN (
                SELECT scope_id FROM quota_scopes 
                WHERE scope_type = 'team' 
                AND scope_id = (SELECT parent_scope_id FROM quota_scopes 
                                WHERE scope_name = %s AND scope_type = 'project')
            )
            AND %s LIKE REPLACE(rlc.endpoint_pattern, '*', '%%')
            ORDER BY LENGTH(rlc.endpoint_pattern) DESC
            LIMIT 1
        """, tenant_id, endpoint)
        
        if config:
            return config.tokens_per_sec, config.bucket_size
        
        # Default rate limit
        return 100, 200  # 100 req/sec, burst of 200
```

**Failure Modes:**
1. **Redis unavailable:** Rate limiter can't check. Two options: (a) fail-open (allow all requests, risking overload), (b) fail-closed (reject all requests, guaranteed safety but blocks legitimate traffic). We choose fail-open with local in-memory rate limiting as fallback (approximate but prevents extreme abuse).
2. **Clock skew between API gateway instances:** Different `now_ms` values cause inconsistent token refill. Mitigation: use Redis server time (`redis.call('TIME')`) instead of client time.
3. **Hot key:** A single tenant's rate limit key receives 10K+ checks/sec. Mitigation: Redis handles this fine for single keys (100K ops/sec per shard). If needed, replicate the key across shards.
4. **Rate limit config change lag:** New config deployed but old config cached. Mitigation: config cache TTL = 60 seconds. For urgent changes, invalidate cache via pub/sub.

**Interviewer Q&As:**

**Q1: Why token bucket over sliding window?**
A: Token bucket explicitly models burst capacity. A tenant with a 100 req/sec limit and bucket size 200 can handle a burst of 200 requests, then sustains at 100/sec. Sliding window doesn't have this explicit burst concept. Token bucket is also simpler to implement atomically in Redis.

**Q2: How do you handle per-endpoint vs per-tenant rate limits?**
A: We support both: per-tenant global rate limits (e.g., 10,000 req/min total) and per-endpoint limits (e.g., 100 pod creates/min). Each creates a separate token bucket in Redis. A request must pass both checks.

**Q3: What about rate limiting at the cluster level (not per-tenant)?**
A: Global cluster-level rate limits protect backend services from aggregate overload. Implemented as a separate token bucket with key `rl:global:{endpoint}`. The total tokens_per_sec is the backend's maximum throughput.

**Q4: How do you communicate rate limit status to clients?**
A: Standard HTTP headers: `X-RateLimit-Limit` (bucket size), `X-RateLimit-Remaining` (current tokens), `X-RateLimit-Reset` (epoch when bucket refills). When limited: HTTP 429 with `Retry-After` header.

**Q5: How do you test rate limiting accuracy?**
A: Load tests with known patterns: (1) sustained traffic at exactly the limit — expect 0% rejection. (2) Sustained traffic at 2x the limit — expect ~50% rejection. (3) Burst followed by silence — expect burst allowed up to bucket size. We measure actual rejection rates against theoretical expectations.

**Q6: How does rate limiting interact with service mesh retry policies?**
A: Service mesh retries can amplify rate-limited requests (a 429 triggers a retry, which also gets 429'd). Mitigation: the `Retry-After` header tells the client when to retry. Well-behaved clients respect this. We also exempt internal service-to-service calls from tenant rate limits (they have separate, higher limits).

---

### 6.3 Burst Quota Manager

**Why it's hard:**
Strict quotas lead to poor user experience — a team at 99% GPU quota can't run one more experiment even if the cluster has spare capacity. Burst quotas allow temporary overages (e.g., 120% of hard limit for up to 1 hour). The challenge is: tracking burst state per scope, automatically reclaiming burst capacity after the window expires, preventing abuse (unlimited bursting), and integrating with billing (burst usage is billed at a premium).

**Approaches Compared:**

| Approach | User Experience | Cost Control | Complexity |
|----------|----------------|--------------|------------|
| Strict hard limits only | Poor (hard rejection) | Excellent | Low |
| Soft limits with alerts only | Good (never blocked) | Poor (no enforcement) | Low |
| Burst with time window | Good (flexibility when needed) | Good (time-bounded) | Medium |
| Dynamic quotas (auto-adjust to demand) | Excellent | Medium | High |
| Credit system (save unused quota for later) | Good (rewards efficient use) | Good | High |

**Selected: Burst with time window + billing integration.**

**Implementation (pseudocode):**

```python
class BurstQuotaManager:
    def __init__(self):
        self.redis = RedisClient()
        self.db = MySQLClient()
    
    def check_and_admit(self, scope_id: str, resource_type: str, 
                        requested_amount: int) -> AdmitResult:
        """Check if a resource request can be admitted, considering burst."""
        quota = self.quota_resolver.get_effective_quota(scope_id, resource_type)
        usage = self.redis.get_usage(scope_id, resource_type)
        
        new_usage = usage.current + requested_amount
        
        # Case 1: Within hard limit — always allow
        if new_usage <= quota.hard_limit:
            self._increment_usage(scope_id, resource_type, requested_amount)
            
            # Check soft limit for warning
            if quota.soft_limit and new_usage > quota.soft_limit:
                return AdmitResult(allowed=True, 
                    warning=f"Soft limit exceeded: {new_usage}/{quota.soft_limit}")
            return AdmitResult(allowed=True)
        
        # Case 2: Exceeds hard limit but within burst limit
        if quota.burst_limit and new_usage <= quota.burst_limit:
            burst_state = self._get_or_start_burst(scope_id, resource_type, quota)
            
            if burst_state is None:
                # Burst not allowed (already expired or disabled)
                return AdmitResult(allowed=False, 
                    reason=f"Hard limit exceeded: {new_usage}/{quota.hard_limit}. "
                           f"Burst quota exhausted or expired.")
            
            if burst_state.remaining_seconds <= 0:
                # Burst window expired
                return AdmitResult(allowed=False,
                    reason=f"Burst window expired. Usage must return below "
                           f"hard limit ({quota.hard_limit}) before burst can be used again.")
            
            self._increment_usage(scope_id, resource_type, requested_amount)
            self._record_burst_usage(scope_id, resource_type, requested_amount)
            
            return AdmitResult(allowed=True,
                warning=f"BURST: Using burst quota. {burst_state.remaining_seconds}s "
                        f"remaining. Burst usage billed at 2x rate.",
                burst_active=True,
                burst_remaining_sec=burst_state.remaining_seconds)
        
        # Case 3: Exceeds burst limit — hard deny
        return AdmitResult(allowed=False,
            reason=f"Exceeds burst limit: {new_usage}/{quota.burst_limit}. "
                   f"Request a quota increase.")
    
    def _get_or_start_burst(self, scope_id: str, resource_type: str, 
                             quota: EffectiveQuota) -> Optional[BurstState]:
        """Get or start a burst window."""
        burst_key = f"burst:{scope_id}:{resource_type}"
        
        state = self.redis.hgetall(burst_key)
        if state:
            start_time = float(state['start_time'])
            elapsed = time.time() - start_time
            remaining = quota.burst_duration_sec - elapsed
            if remaining <= 0:
                # Burst expired — check if usage has returned below hard limit
                usage = self.redis.get_usage(scope_id, resource_type)
                if usage.current <= quota.hard_limit:
                    # Reset burst (can be used again)
                    self.redis.delete(burst_key)
                    return self._start_new_burst(burst_key, quota)
                else:
                    # Usage still above hard limit — burst cannot be renewed
                    return None
            return BurstState(remaining_seconds=remaining)
        
        # Start new burst
        return self._start_new_burst(burst_key, quota)
    
    def _start_new_burst(self, burst_key: str, quota: EffectiveQuota) -> BurstState:
        """Start a new burst window."""
        self.redis.hmset(burst_key, {
            'start_time': str(time.time()),
            'total_burst_usage': '0'
        })
        self.redis.expire(burst_key, quota.burst_duration_sec + 3600)  # extra hour for cleanup
        
        # Log burst start for billing
        self._emit_burst_event('burst_started', burst_key)
        
        return BurstState(remaining_seconds=quota.burst_duration_sec)
    
    def periodic_burst_enforcement(self):
        """Run every 60 seconds to reclaim expired burst capacity."""
        active_bursts = self.redis.scan_match("burst:*")
        
        for burst_key in active_bursts:
            state = self.redis.hgetall(burst_key)
            scope_id, resource_type = self._parse_burst_key(burst_key)
            quota = self.quota_resolver.get_effective_quota(scope_id, resource_type)
            
            start_time = float(state['start_time'])
            elapsed = time.time() - start_time
            
            if elapsed > quota.burst_duration_sec:
                # Burst expired — warn tenant, start evicting if still over
                usage = self.redis.get_usage(scope_id, resource_type)
                if usage.current > quota.hard_limit:
                    overage = usage.current - quota.hard_limit
                    self._emit_burst_event('burst_expired_over_quota', burst_key,
                        details={'overage': overage})
                    # Trigger eviction of lowest-priority workloads to bring under limit
                    self._request_eviction(scope_id, resource_type, overage)
                
                self.redis.delete(burst_key)
                self._emit_burst_event('burst_ended', burst_key)
```

**Failure Modes:**
1. **Burst never reclaimed:** Redis key lost during failover; system forgets burst was active. Mitigation: MySQL records burst events. Periodic reconciliation (every 5 minutes) checks MySQL for active bursts and recreates Redis state if missing.
2. **Burst abuse:** Tenant keeps using burst, drops below hard limit briefly, then bursts again. Mitigation: cooldown period (e.g., 4 hours between burst windows). Max burst events per day configurable.
3. **Race condition during burst start:** Two requests simultaneously start a burst. Mitigation: Redis SETNX (set if not exists) for the burst key. Only the first request starts the burst.

**Interviewer Q&As:**

**Q1: How is burst quota priced differently from regular quota?**
A: Burst usage is billed at 2x the normal rate. This discourages habitual bursting while providing flexibility for genuine spikes. The billing system consumes burst events from the audit stream and calculates the premium.

**Q2: What happens when a burst expires and the tenant is still over quota?**
A: A graduated response: (1) Warning notification at burst expiry. (2) Grace period of 15 minutes. (3) After grace period, lowest-priority workloads are evicted until usage drops below the hard limit. (4) The eviction order respects PodDisruptionBudgets.

**Q3: Can burst quotas be disabled for specific tenants?**
A: Yes. Setting `burst_limit = NULL` in the quota definition disables burst for that scope. This is used for tenants with strict cost controls or compliance requirements.

**Q4: How do you prevent one project's burst from starving another project under the same team?**
A: The team-level quota is the ultimate constraint. If one project bursts, it still can't exceed the team's total allocation. If the team-level is also at capacity, burst is denied.

**Q5: How do you handle burst for GPU resources?**
A: GPU burst is more restrictive because GPUs are scarce. Default burst allowance for GPUs is 0% (no burst). When enabled, it's typically 10% overage for 30 minutes max, compared to 20% for 1 hour for CPU/RAM.

**Q6: How do you communicate burst status to users?**
A: (1) CLI/API responses include burst warnings. (2) Dashboard shows a yellow "BURST ACTIVE" banner with countdown. (3) Slack notification when burst starts and when 50% of burst window has elapsed. (4) Email alert when burst expires.

---

## 7. Scheduling & Resource Management

### Placement Algorithm

Quota enforcement is integrated into the scheduling pipeline as a **filter predicate**. Before a workload is placed on any node, the scheduler checks:

1. **Project-level quota:** Does the project have remaining quota for the requested resources?
2. **Team-level quota:** Does the team have remaining capacity (considering all projects)?
3. **Organization-level quota:** Is the org under its total allocation?
4. **Per-workload limits (LimitRange):** Does this workload's request fall within the [min, max] per-workload range?

If any check fails, the workload is rejected at admission time (before entering the scheduling queue).

### Conflict Detection

- **Optimistic locking on usage counters:** Redis INCRBY is atomic. If two admission checks run simultaneously for the same scope and only one slot remains, the first INCRBY succeeds and the second finds usage > limit.
- **Double-check in MySQL:** For critical quotas (GPU), the admission webhook performs a secondary check against MySQL (source of truth) after the Redis check.

### Queue & Priority

Quota checks happen *before* queueing. A request that exceeds quota never enters the scheduling queue — it's rejected immediately with a clear error message. This prevents quota-exceeding workloads from consuming scheduler resources.

### Preemption Policy

Quota enforcement does not trigger preemption. If a tenant is over quota, new requests are rejected — existing workloads are not evicted. Exception: burst expiration may trigger eviction of the tenant's own workloads (self-eviction, not cross-tenant).

### Starvation Prevention

- A tenant who is consistently under their guaranteed quota gets priority in scheduling (their requests are processed first).
- Burst quotas prevent starvation from transient spikes: a team can temporarily exceed their allocation rather than waiting in a queue.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling | Max Scale |
|-----------|---------|-----------|
| **QES instances** | Stateless, behind LB. Scale to match admission check throughput. | 20 instances for 20K checks/sec |
| **API Gateway rate limiter** | Embedded in each gateway instance. Redis is the shared state. | Scales with gateway fleet |
| **Usage aggregator** | Partition by scope_id hash. Each aggregator handles a subset of scopes. | 10 instances |
| **Approval workflow** | Low volume. Single active instance with standby. | 2 (HA) |

### Database Scaling

| Store | Strategy |
|-------|----------|
| **MySQL** | Single primary + 2 read replicas. Active data < 2 GB. Read replicas serve dashboard queries and audit log queries. |
| **Redis** | 6-shard cluster. Sharded by scope_id. Each shard handles ~17K ops/sec (rate limit checks + usage counters). |

### Caching

| Layer | Technology | Data | TTL | Hit Ratio |
|-------|-----------|------|-----|-----------|
| **L1 — QES in-process** | Caffeine | Resolved effective quotas, rate limit configs | 30s | 95% |
| **L2 — Redis** | Redis Cluster | Usage counters (real-time), rate limit buckets, burst state | N/A (real-time) | 98% |
| **L3 — MySQL read replica** | MySQL | Quota definitions, history | Source of truth | N/A |

**Interviewer Q&As:**

**Q1: What's the bottleneck at 50,000 namespaces?**
A: The local cache size in each QES instance. 50,000 namespaces x 10 resource types = 500,000 cached effective quotas = ~500 MB. This fits in memory. Redis handles 500,000 usage counters comfortably. MySQL handles the 50,000 quota definitions easily.

**Q2: How do you handle a Redis failure affecting rate limiting?**
A: Fail-open with degraded local rate limiting. Each QES instance maintains a local in-memory token bucket (approximate, not coordinated across instances). This provides rough rate limiting until Redis recovers. Accuracy degrades from 99%+ to ~80% during the outage.

**Q3: How do you handle schema evolution for quota types?**
A: New resource types are added to the ENUM in MySQL. Existing quotas are unaffected. The QES ignores unknown resource types gracefully. This allows us to add new quota types (e.g., `network_bandwidth_gbps`) without a full system deployment.

**Q4: What if the admission webhook is slow or down?**
A: Kubernetes has a `failurePolicy` setting for webhooks. We use `failurePolicy: Fail` for production namespaces (safety first — reject if we can't check quota) and `failurePolicy: Ignore` for system namespaces (availability first — kube-system pods must always be allowed).

**Q5: How do you load test the quota system?**
A: Synthetic test harness that generates admission check requests at 2x production rate. We verify: (1) all checks complete < 10ms p99, (2) quota enforcement is accurate (no false allows or false denials), (3) rate limiting accuracy within 1%, (4) burst accounting is correct.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Recovery | RTO |
|---------|--------|-----------|----------|-----|
| QES instance crash | Reduced throughput, other instances handle load | K8s health check | Auto-restart, LB removes | 5s |
| All QES instances down | All pod creations blocked (webhook fails) | Zero admits for > 5s | PagerDuty P1. Restart fleet. Temporary: disable webhook. | 30s |
| Redis unavailable | Usage counters stale, rate limiting approximate | Redis health, latency spike | Redis failover (Sentinel). QES falls back to MySQL for admission, local rate limiting. | 10-30s |
| MySQL primary failure | Quota definitions can't be updated. Reads from cache. | Replication lag, health check | Promote read replica (Orchestrator). | 30-60s |
| Usage counter drift | Over-quota workloads admitted or under-quota workloads rejected | Reconciliation detects mismatch | Periodic reconciliation (every 5 min) recomputes usage from actual pod counts. | Self-healing (5 min) |
| Stale cache | Quota change not reflected in admission checks | TTL expiration (30s max) | Cache invalidation on write. Worst case: 30s staleness. | 30s |

### Automated Recovery

1. **Usage reconciliation:** Every 5 minutes, the aggregator counts actual running pods/VMs per namespace and compares to Redis counters. Discrepancies are corrected.
2. **Orphaned burst cleanup:** Every 15 minutes, scan for burst windows that should have expired but weren't cleaned up (Redis key still exists past expiry).
3. **Webhook health check:** QES exposes `/healthz` endpoint. Kubernetes removes unhealthy webhook endpoints automatically.

### Retry Strategy

| Operation | Strategy | Retries | Backoff |
|-----------|----------|---------|---------|
| Admission check (Redis read) | Retry with MySQL fallback | 1 Redis, then MySQL | 0ms (immediate fallback) |
| Usage increment (Redis write) | Fire and forget with async MySQL reconciliation | 1 | N/A (reconciliation corrects) |
| Rate limit check | Retry or fail-open | 1 | 0ms (fail-open if retry fails) |
| Quota definition write | Retry on deadlock | 3 | 50ms, 100ms, 200ms |

### Circuit Breaker

| Dependency | Failure Threshold | Open Duration | Behavior |
|------------|-------------------|---------------|----------|
| Redis | 5 failures in 5s | 15s | Fall back to MySQL reads + local rate limiting |
| MySQL | 3 failures in 10s | 30s | Serve from cache only. Reject quota modifications. |
| K8s API | 5 failures in 10s | 30s | Stop updating K8s ResourceQuota objects. Cache continues to serve. |

### Consensus & Coordination

- No distributed consensus needed for the hot path. Redis atomic operations (INCRBY, EVALSHA) handle concurrent access.
- Quota definition updates use MySQL transactions (single-writer serialization).
- Usage reconciliation uses a distributed lock (Redis SETNX) to prevent multiple reconcilers from running simultaneously for the same scope.

---

## 10. Observability

### Key Metrics

| Metric | Type | Labels | Alert Threshold |
|--------|------|--------|-----------------|
| `quota_admission_latency_ms` | Histogram | scope_type, result | p99 > 10ms |
| `quota_admission_total` | Counter | scope_type, result(allow/deny/burst) | Deny rate > 10% |
| `quota_utilization_pct` | Gauge | scope_id, resource_type | > 90% for > 1 hour |
| `rate_limit_rejection_total` | Counter | tenant_id, endpoint | > 100/min |
| `rate_limit_latency_ms` | Histogram | endpoint | p99 > 5ms |
| `burst_active_count` | Gauge | resource_type | > 10 simultaneous bursts |
| `burst_expired_over_quota` | Counter | scope_id | > 0 (always investigate) |
| `usage_reconciliation_drift` | Gauge | scope_id, resource_type | > 5% drift |
| `quota_request_pending_count` | Gauge | | > 50 pending |
| `quota_request_approval_time_hours` | Histogram | | p50 > 24 hours |

### Distributed Tracing

Trace per admission check: `webhook_receive → cache_lookup → usage_read → quota_evaluate → respond`.
Each span annotated with: scope_id, resource_type, requested_amount, current_usage, quota_limit, result.

### Logging

| Level | When | Content |
|-------|------|---------|
| INFO | Admission allowed | scope_id, resource, requested, usage, limit |
| WARN | Soft limit exceeded | scope_id, resource, usage, soft_limit |
| WARN | Burst started | scope_id, resource, burst_window |
| ERROR | Hard limit rejection | scope_id, resource, requested, usage, limit |
| ERROR | Redis/MySQL failure | error_type, fallback_used |

### Alerting

| Alert | Condition | Severity |
|-------|-----------|----------|
| QuotaWebhookDown | All QES instances unhealthy | P1 |
| HighDenialRate | > 20% deny rate for any scope for 5 min | P2 |
| QuotaNearCapacity | Any scope > 95% utilization for 1 hour | P3 |
| BurstOverQuota | Burst expired with usage still above hard limit | P2 |
| RequestsPendingApproval | > 50 requests pending > 48 hours | P3 |
| UsageDrift | Reconciliation drift > 10% for any scope | P2 |

---

## 11. Security

### Auth & AuthZ

- **Quota management:** Only `quota-admin` role can create/modify/delete quotas. Team managers can manage quotas within their subtree.
- **Usage visibility:** Users see their own project's usage. Team admins see all projects under their team. Org admins see everything.
- **Rate limit config:** Only platform admins can modify rate limits.
- **Approval workflow:** Approver must have manager role for the target scope's parent.

### Secrets Management

- Database credentials via Vault with dynamic secrets (1-hour TTL).
- Redis ACL with per-service passwords.
- Webhook TLS certificates from internal CA.
- No credentials in environment variables or container images.

### Audit Logging

All quota mutations logged with full context:
- Who changed what, when, from what value to what value.
- Approval chain for quota requests.
- Burst events with duration and usage details.
- Rate limit rejections (for abuse detection).

Audit log: append-only, shipped to S3, 7-year retention for compliance.

---

## 12. Incremental Rollout Strategy

**Phase 1 — Rate Limiting (Week 1-2):**
Deploy API gateway rate limiting with generous limits (10x expected traffic). Monitor rejection rate. Tighten gradually.

**Phase 2 — Monitoring Mode (Week 3-4):**
Deploy QES in monitoring mode: it logs what it *would* deny but doesn't actually reject anything. This identifies which teams would be affected by quota enforcement.

**Phase 3 — Soft Limits (Week 5-6):**
Enable soft limits only. Allow everything but generate warnings for over-quota usage. Give teams time to adjust.

**Phase 4 — Hard Limits (Week 7-10):**
Enable hard limits per team, one at a time. Start with teams that have consistently been under their quotas (low risk). Monitor for disruption.

**Phase 5 — Burst Quotas (Week 11-12):**
Enable burst quotas for teams that have been hitting hard limits. This provides the flexibility they need while maintaining cost control.

**Rollout Q&As:**

**Q1: What if a team is currently using more resources than their new quota?**
A: We set the initial quota to 120% of their current peak usage. This gives them headroom to operate normally while capping growth. Over the next quarter, we work with teams to optimize and gradually tighten quotas.

**Q2: How do you handle emergency quota increases during an incident?**
A: Platform on-call has the ability to bypass the approval workflow and immediately increase quotas. This is logged as an "emergency override" in the audit trail and requires post-incident review.

**Q3: What if the admission webhook adds too much latency to pod creation?**
A: The QES is designed for < 10ms p99. If latency exceeds this, we have a kill switch to disable the webhook (Kubernetes `failurePolicy: Ignore`). We also benchmark before enabling: the webhook must pass a load test of 30,000 admission checks/sec with < 10ms p99.

**Q4: How do you migrate from Kubernetes-native ResourceQuota to this system?**
A: We run both in parallel initially. Our system syncs quota definitions to Kubernetes ResourceQuota objects. The Kubernetes admission controller provides native enforcement as a safety net. Once we validate our system is accurate, we can optionally disable the Kubernetes-native enforcement (or keep it as defense in depth).

**Q5: How do you handle quota enforcement for OpenStack and bare-metal alongside Kubernetes?**
A: All three platforms call the same QES for admission checks. The QES is platform-agnostic — it deals with abstract resource types (CPU, RAM, GPU). Each platform's admission path (K8s webhook, OpenStack Nova filter, bare-metal API middleware) calls the same QES endpoint.

---

## 13. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Risk |
|----------|-------------------|--------|-----------|------|
| Usage counter store | MySQL only vs Redis only vs MySQL + Redis | MySQL + Redis | Redis for speed, MySQL for durability. Reconciliation handles drift. | Two systems to maintain. Drift between them. |
| Rate limiting algorithm | Fixed window vs Sliding window vs Token bucket | Token bucket (Redis Lua) | Explicit burst control. Atomic implementation. Industry standard. | Redis dependency on hot path. |
| Hierarchy model | Flat quotas vs 2-level (org/project) vs N-level | 4-level (org/team/project/user) | Matches organizational structure. Each level adds meaningful control. | Complex resolution logic. Deep hierarchies slow down cold-cache lookups. |
| Burst strategy | No burst vs Time-windowed burst vs Credit system | Time-windowed burst | Simple, predictable, time-bounded. Credits add complexity. | Potential abuse if cooldown is too short. |
| Webhook failure policy | Fail-open vs Fail-closed | Fail-closed (production), Fail-open (system namespaces) | Safety first for production. Availability first for critical system components. | Fail-closed can block all deployments if QES is down. |
| Quota scope mapping | 1:1 namespace:quota vs Many:1 | 1:1 (project = namespace = quota scope) | Simple, clear ownership. | Many namespaces = many quota scopes to manage. |
| LimitRange enforcement | K8s native vs Custom webhook | Custom webhook (unified with quota check) | Single webhook call for both quota check and LimitRange defaulting. | Replaces native K8s functionality. |

---

## 14. Agentic AI Integration

### AI-Powered Quota Management

**1. Intelligent Quota Recommendation:**
An ML model analyzes historical usage patterns per scope and recommends optimal quotas. Features: average usage, peak usage, growth trend, seasonality, workload type. Output: recommended hard limit, soft limit, and burst limit. Example: "Project ml-experiments has used an average of 18 GPUs (peak 24) over the past 30 days with 15% monthly growth. Recommended: hard_limit=32, soft_limit=24, burst_limit=38."

**2. Anomaly Detection for Quota Abuse:**
An anomaly detection model identifies unusual usage patterns: sudden spikes in resource consumption, repeated burst usage (suggesting quota gaming), rate limit evasion (rotating API keys). When detected, an alert with full context is sent to the security team.

**3. Natural Language Quota Operations:**
- "Show me which teams are consistently over 80% of their GPU quota" → Agent queries usage data and presents a ranked list.
- "Why was my pod creation rejected?" → Agent traces the admission check, identifies which quota was exceeded, and suggests remediation (request increase, clean up unused resources, or use burst).
- "What would happen if we reduce team X's CPU quota by 20%?" → Agent simulates: checks current usage, projects future usage, identifies workloads that would be affected.

**4. Automated Quota Request Triage:**
An AI agent triages quota increase requests:
- Auto-approve requests within 20% of current usage (clearly justified by actual need).
- Flag requests > 200% increase for human review with analysis: "This team has never used more than 10 GPUs. They're requesting 64. Possible reasons: new ML project (check Jira), mistake (common), or team reorganization (check HR system)."
- Suggest alternative solutions: "Instead of increasing GPU quota, consider using preemptible GPUs for training jobs (60% cheaper, same result)."

**5. Cost-Aware Quota Optimization:**
An AI agent continuously monitors the relationship between quota allocations and actual usage across the organization. It generates weekly optimization reports: "Total allocated GPU quota across all teams: 500. Total used (peak): 280. 44% of allocated GPU capacity is unused. Top candidates for reduction: Team A (allocated 64, used max 12), Team B (allocated 32, used max 8)."

### Guard Rails
- AI quota recommendations require human approval before implementation.
- Auto-approval thresholds are conservative (< 20% increase) and require matching usage history.
- All AI decisions logged with reasoning chain.
- Kill switch: disable all AI features without affecting core quota enforcement.

---

## 15. Complete Interviewer Q&A Bank

**Q1: How does Kubernetes ResourceQuota work internally?**
A: ResourceQuota is enforced via an admission controller. When a pod is created, the ResourceQuota admission plugin checks the pod's resource requests against the namespace's ResourceQuota object. It atomically increments the usage counter in the ResourceQuota status field. The ResourceQuota controller periodically recomputes usage by listing all objects in the namespace (reconciliation). Limits are per-namespace, not hierarchical (which is why our system adds the hierarchy layer).

**Q2: What's the difference between ResourceQuota and LimitRange in Kubernetes?**
A: ResourceQuota limits the *total* resources a namespace can consume (e.g., total 100 CPU cores across all pods). LimitRange limits *individual* workloads (e.g., each pod can request max 8 CPU cores, default 1 core). They're complementary: LimitRange prevents any single pod from being too large; ResourceQuota prevents the namespace from consuming too much in aggregate.

**Q3: How do you handle quota enforcement for resources that aren't countable (like network bandwidth)?**
A: Network bandwidth quotas are enforced at the SDN layer (Cilium, Calico bandwidth plugin), not at admission time. Our system tracks and reports bandwidth allocation as a quota metric, but the actual enforcement happens in the network data plane via traffic shaping (Linux tc). We can deny pod creation if the requested bandwidth would exceed the namespace's bandwidth quota.

**Q4: What happens if Redis fails during an admission check?**
A: Circuit breaker triggers after 5 failures in 5 seconds. Fall back to MySQL for usage data (adds ~5ms latency). For rate limiting, fall back to local in-memory token buckets (approximate). Alert fires for on-call. Once Redis recovers, reconciliation corrects any drift.

**Q5: How do you handle the race condition where two pods are admitted simultaneously and push usage over the limit?**
A: Redis INCRBY is atomic. Both admission checks read the current value, increment atomically, and check the result. If both increment, one of them sees a post-increment value above the limit. That admission check can either: (a) decrement and deny (strict mode), or (b) allow it (eventual consistency mode, will be caught by reconciliation). We use strict mode for hard limits.

**Q6: How do you implement quota inheritance efficiently?**
A: We pre-compute "effective quotas" for each scope by walking up the hierarchy. The effective quota is cached with a 30-second TTL. When a quota at any level changes, we invalidate the entire subtree. The tree traversal uses MySQL recursive CTEs, which are efficient even for deep hierarchies.

**Q7: What's the typical accuracy of the token bucket rate limiter in a distributed setup?**
A: With Redis Lua scripts, accuracy is > 99% because all state is in a single Redis key per bucket. The only source of inaccuracy is Redis cluster replication lag during failover (a few seconds where the new master might have slightly stale bucket state). In practice, this results in < 0.1% over-admission during failover events.

**Q8: How would you handle quota for GPU MIG (Multi-Instance GPU) slices?**
A: We add new resource types: `gpu_mig_1g5gb`, `gpu_mig_2g10gb`, `gpu_mig_3g20gb`, etc. (1/7, 2/7, 3/7 of an A100). Each MIG slice type has its own quota. This is more granular than `gpu_count` and allows teams to use fractional GPUs. The admission check verifies both the MIG slice quota and the physical GPU quota.

**Q9: How do you prevent quota hoarding (allocating quota but not using it)?**
A: Usage-based reclamation: if a scope's usage is < 50% of its quota for > 30 days, we flag it for review. The AI agent generates a recommendation to reduce the quota. Burst quotas also help — a team with a smaller base quota can still handle occasional spikes without hoarding a large base allocation.

**Q10: How do you handle multi-cluster quota enforcement?**
A: Quotas are defined centrally in the quota store (MySQL). Each cluster's QES reads from the same source. Usage is aggregated across clusters: if a team uses 50 GPU on cluster A and 30 GPU on cluster B, their total usage is 80 GPU. Cross-cluster usage aggregation uses an eventual consistency model (updated every 30 seconds).

**Q11: What's the performance cost of adding an admission webhook to every pod creation?**
A: In our benchmarks, the QES webhook adds 2-5ms to pod creation latency (p99). For a cluster creating 10,000 pods/sec, this is an aggregate overhead of 20-50 CPU-seconds/sec on the QES fleet. With 20 QES instances, each handles 500 webhook calls/sec, well within capacity. The latency is dominated by a single Redis GET (0.5ms) + computation (1ms) + network round-trip (1-3ms).

**Q12: How do you handle quota for ephemeral resources (like spot/preemptible instances)?**
A: Preemptible instances have their own quota type: `preemptible_cpu_cores`, `preemptible_gpu_count`. This allows teams to have a large preemptible quota (for cost savings) and a smaller guaranteed quota. The two quotas are enforced independently. This encourages efficient use of preemptible capacity.

**Q13: How do you enforce API rate limits across multiple API gateways?**
A: All gateway instances share the same Redis cluster for token bucket state. The Lua script ensures atomicity regardless of which gateway instance processes the request. There's no need for inter-gateway coordination — Redis serializes all operations on a given bucket key.

**Q14: How do you handle quota during disaster recovery / failover?**
A: The QES runs in active-active across AZs. MySQL is replicated across AZs. Redis Cluster spans AZs. During AZ failover, the surviving AZ's QES instances handle all traffic. Quotas are unaffected because the state is replicated. Usage counters may have a brief (< 10 second) inconsistency during failover, corrected by reconciliation.

**Q15: How would you implement quota alerts that notify teams before they hit their limits?**
A: Three threshold notifications: (1) 70% utilization — informational email to team lead. (2) 85% utilization — Slack notification to team channel. (3) 95% utilization — PagerDuty alert to team on-call. Each notification includes: current usage, limit, projected time to exhaustion (based on 7-day trend), and a link to request a quota increase.

**Q16: How do you handle the cold start problem for new projects with no usage history?**
A: New projects inherit a default quota template based on their team's tier (small: 10 CPU, 32 GB RAM; medium: 100 CPU, 256 GB RAM; large: 1000 CPU, 2048 GB RAM, 16 GPU). The template is chosen during project creation based on the project's declared purpose. Quotas can be adjusted after 2 weeks of usage data is collected.

---

## 16. References

1. Kubernetes ResourceQuota: https://kubernetes.io/docs/concepts/policy/resource-quotas/
2. Kubernetes LimitRange: https://kubernetes.io/docs/concepts/policy/limit-range/
3. Kubernetes Admission Webhooks: https://kubernetes.io/docs/reference/access-authn-authz/extensible-admission-controllers/
4. OpenStack Quota Management: https://docs.openstack.org/nova/latest/user/quotas.html
5. Token Bucket Algorithm: https://en.wikipedia.org/wiki/Token_bucket
6. Redis Rate Limiting Patterns: https://redis.io/commands/incr (pattern section)
7. Stripe Rate Limiting: https://stripe.com/blog/rate-limiters
8. Google Cloud Resource Quotas: https://cloud.google.com/compute/quotas
9. AWS Service Quotas: https://docs.aws.amazon.com/servicequotas/
10. Pat Helland. "Standing on Distributed Shoulders of Giants." *ACM Queue, 2020*.
