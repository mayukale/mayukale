# System Design: Developer Self-Service Portal

> **Relevance to role:** A cloud infrastructure platform engineer must build the abstraction layer that lets thousands of developers provision VMs, bare-metal servers, Kubernetes clusters, and storage without filing tickets. This portal sits at the intersection of job scheduling, resource management, quota enforcement, and multi-tenancy -- all core to this role.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|-------------|--------|
| FR-1 | VM Provisioning | Users can request VMs with specific CPU/RAM/disk/GPU configs |
| FR-2 | Bare-Metal Reservation | Reserve physical servers with time windows (start/end) |
| FR-3 | Kubernetes Cluster Creation | One-click K8s clusters with version, node count, node type |
| FR-4 | Storage Allocation | Block (Ceph RBD), Object (S3-compatible), File (NFS) |
| FR-5 | Template Catalog | Curated infra templates (e.g., "ML Training Rig", "Web App Stack") |
| FR-6 | Multi-Step Workflows | Request -> Approval -> Provisioning -> Notification pipeline |
| FR-7 | Cost Estimation | Show estimated cost before provisioning commits |
| FR-8 | Quota Enforcement | Hard/soft limits per project, team, and user |
| FR-9 | Lifecycle Management | Auto-expire resources after project end date; renewal flow |
| FR-10 | SSO Integration | SAML 2.0 / OIDC with corporate IdP (Okta, Azure AD) |
| FR-11 | Audit Trail | Immutable log of who provisioned what, when, at what cost |
| FR-12 | Notifications | Email + Slack + webhook on provision start/complete/fail/expire |
| FR-13 | RBAC | Role-based access: viewer, developer, approver, admin |
| FR-14 | Project Management | Projects group resources; each project has budget + quota |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Availability | 99.95% (portal), 99.99% (provisioning API) |
| NFR-2 | Latency | Portal page load < 2s; API response < 500ms |
| NFR-3 | Provisioning Time | VM < 3 min; bare-metal < 15 min; K8s cluster < 10 min |
| NFR-4 | Scale | 10,000 active developers, 50,000 active resources |
| NFR-5 | Audit Retention | 7 years for compliance |
| NFR-6 | Concurrent Users | 1,000 simultaneous portal users |

### Constraints & Assumptions
- Organization has 10,000+ engineers across 500+ teams.
- Existing infrastructure: OpenStack for VMs, custom bare-metal provisioning (IPMI/Redfish), Kubernetes (upstream).
- MySQL 8.0 as primary RDBMS; Elasticsearch for search and audit logs.
- Java (Spring Boot) for backend services; React for frontend.
- Message broker: RabbitMQ for async provisioning workflows.
- Corporate IdP is Okta (OIDC/SAML 2.0).

### Out of Scope
- Network provisioning (VLANs, firewalls) -- handled by separate network team.
- DNS management -- separate service.
- Application deployment (CI/CD) -- separate platform.
- Cost billing/chargeback -- we provide estimates; finance system handles invoicing.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Total developers | Given | 10,000 |
| Daily active users | 30% of total | 3,000 |
| Peak concurrent users | 10% of DAU | 300 |
| Provisioning requests/day | 3,000 DAU x 2 requests avg | 6,000 |
| Provisioning requests/sec (avg) | 6,000 / 86,400 | ~0.07 RPS |
| Provisioning requests/sec (peak) | 10x average | ~0.7 RPS |
| Portal page views/day | 3,000 x 20 pages | 60,000 |
| API calls/day (portal + CLI) | 60,000 + 40,000 CLI | 100,000 |
| API calls/sec (peak) | 100,000 / 86,400 x 5 (peak factor) | ~6 RPS |
| Approval workflow events/day | 6,000 x 40% need approval | 2,400 |
| Notifications/day | 6,000 x 3 events each | 18,000 |

### Latency Requirements
| Operation | P50 | P99 |
|-----------|-----|-----|
| Portal page load | 800ms | 2s |
| API response (list/get) | 100ms | 500ms |
| API response (create/provision) | 200ms (accepted) | 1s |
| Template rendering | 300ms | 1s |
| Cost estimation calculation | 500ms | 2s |

### Storage Estimates

| Data Type | Calculation | Size |
|-----------|-------------|------|
| Resource records | 50,000 active x 2KB avg | 100 MB |
| Audit logs/year | 100,000 events/day x 1KB x 365 | ~36 GB/year |
| Templates | 500 templates x 50KB | 25 MB |
| User profiles + RBAC | 10,000 users x 5KB | 50 MB |
| Workflow state | 6,000/day x 10KB x 30 days | ~1.8 GB |
| MySQL total (active) | Sum of above | ~2 GB |
| Elasticsearch (audit, 7yr) | 36 GB x 7 | ~250 GB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Portal static assets | 300 users x 2MB (cached) | Negligible (CDN) |
| API traffic (peak) | 6 RPS x 5KB avg response | ~30 KB/s |
| WebSocket updates | 300 connections x 100B/s | ~30 KB/s |
| Notification payloads | 18,000/day x 2KB | ~0.4 KB/s |

---

## 3. High Level Architecture

```
                                    +-------------------+
                                    |   CDN (CloudFront)|
                                    |   Static Assets   |
                                    +--------+----------+
                                             |
                                    +--------v----------+
    +------------+                  |   Load Balancer    |
    |  Corporate |                  |   (HAProxy/NLB)    |
    |  IdP (Okta)|<----OIDC/SAML-->+--------+----------+
    +------------+                           |
                               +-------------+-------------+
                               |                           |
                      +--------v--------+         +--------v--------+
                      |  React Frontend |         |  BFF API Gateway|
                      |  (SPA)          |         |  (Spring Boot)  |
                      +-----------------+         +--------+--------+
                                                           |
                          +----------+----------+----------+----------+
                          |          |          |          |          |
                   +------v--+ +----v----+ +---v-----+ +-v-------+ +v----------+
                   | Resource | | Workflow| | Template| | Quota   | | Audit     |
                   | Service  | | Engine  | | Service | | Service | | Service   |
                   | (Java)   | | (Java)  | | (Java)  | | (Java)  | | (Java)    |
                   +------+---+ +----+----+ +---------+ +----+----+ +-----+-----+
                          |          |                        |            |
                          |    +-----v------+                 |            |
                          |    | RabbitMQ   |                 |      +-----v------+
                          |    | (Workflow  |                 |      |Elasticsearch|
                          |    |  Events)   |                 |      | (Audit Logs)|
                          |    +-----+------+                 |      +------------+
                          |          |                        |
                   +------v----------v------------------------v------+
                   |              MySQL 8.0 (Primary + Replica)      |
                   +------------------+------------------------------+
                                      |
              +-----------------------+-----------------------+
              |                       |                       |
     +--------v--------+    +--------v--------+    +---------v-------+
     | OpenStack API   |    | Bare-Metal      |    | Kubernetes API  |
     | (VM Provisioning|    | Provisioner     |    | (Cluster Create)|
     | Nova/Neutron)   |    | (IPMI/Redfish)  |    | (ClusterAPI)    |
     +-----------------+    +-----------------+    +-----------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **CDN** | Serves React SPA static assets (JS bundles, images, fonts) |
| **Load Balancer** | TLS termination, L7 routing, health checks |
| **React Frontend** | Single-page app; template catalog UI, provisioning wizards, dashboards |
| **BFF API Gateway** | Backend-for-Frontend; aggregates backend services, handles auth token validation, rate limiting |
| **Resource Service** | CRUD for VMs, bare-metal, K8s clusters, storage; delegates to provisioners |
| **Workflow Engine** | Multi-step approval workflows; state machine (request -> pending_approval -> approved -> provisioning -> active -> expiring -> terminated) |
| **Template Service** | Template catalog CRUD; template rendering with parameter substitution |
| **Quota Service** | Enforces resource limits per project/team/user; reservation-based quota (optimistic check + pessimistic lock on commit) |
| **Audit Service** | Writes immutable audit events to Elasticsearch; queryable audit trail |
| **RabbitMQ** | Async event bus for provisioning workflows, notifications, audit ingestion |
| **MySQL** | Source of truth for resources, users, projects, quotas, workflows |
| **Elasticsearch** | Audit log storage (7yr retention), full-text search on resources/templates |
| **OpenStack API** | VM lifecycle (create, resize, snapshot, delete) via Nova |
| **Bare-Metal Provisioner** | Physical server management via IPMI/Redfish + PXE boot |
| **Kubernetes API** | Cluster lifecycle via ClusterAPI or custom controller |

### Data Flows

**Provisioning Request Flow:**
1. User selects template or fills custom form in React UI.
2. Frontend calls BFF: `POST /api/v1/resources` with resource spec.
3. BFF validates auth (JWT from Okta), calls Quota Service to check limits.
4. If quota OK, Resource Service creates resource record (status: `pending_approval`).
5. Workflow Engine evaluates approval policy (auto-approve if cost < $100, else require manager).
6. On approval, Workflow Engine publishes `resource.approved` event to RabbitMQ.
7. Resource Service consumer picks up event, calls appropriate provisioner (OpenStack/bare-metal/K8s).
8. Provisioner runs async; status updates published to RabbitMQ.
9. BFF pushes status updates to frontend via WebSocket.
10. On completion, Notification Service sends email + Slack; Audit Service logs event.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Users (synced from corporate IdP)
CREATE TABLE users (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    external_id     VARCHAR(255) NOT NULL UNIQUE,  -- IdP subject ID
    email           VARCHAR(255) NOT NULL UNIQUE,
    display_name    VARCHAR(255) NOT NULL,
    department      VARCHAR(255),
    is_active       BOOLEAN NOT NULL DEFAULT TRUE,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_users_email (email),
    INDEX idx_users_external_id (external_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Projects (resource grouping + billing)
CREATE TABLE projects (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    name            VARCHAR(255) NOT NULL UNIQUE,
    display_name    VARCHAR(255) NOT NULL,
    description     TEXT,
    owner_id        BIGINT NOT NULL REFERENCES users(id),
    budget_cents    BIGINT DEFAULT 0,           -- monthly budget in cents
    status          ENUM('active','suspended','archived') NOT NULL DEFAULT 'active',
    expires_at      TIMESTAMP NULL,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_projects_owner (owner_id),
    INDEX idx_projects_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Project membership + roles
CREATE TABLE project_members (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    project_id      BIGINT NOT NULL REFERENCES projects(id),
    user_id         BIGINT NOT NULL REFERENCES users(id),
    role            ENUM('viewer','developer','approver','admin') NOT NULL DEFAULT 'developer',
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_project_user (project_id, user_id),
    INDEX idx_pm_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Quotas per project
CREATE TABLE quotas (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    project_id      BIGINT NOT NULL REFERENCES projects(id),
    resource_type   ENUM('vcpu','memory_gb','disk_gb','gpu','vm_count','bare_metal_count','k8s_cluster_count','storage_tb') NOT NULL,
    hard_limit      BIGINT NOT NULL,
    soft_limit      BIGINT NOT NULL,             -- warning threshold
    used            BIGINT NOT NULL DEFAULT 0,
    reserved        BIGINT NOT NULL DEFAULT 0,   -- pending provisioning
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_quota (project_id, resource_type),
    INDEX idx_quotas_project (project_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Infrastructure templates
CREATE TABLE templates (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    name            VARCHAR(255) NOT NULL UNIQUE,
    display_name    VARCHAR(255) NOT NULL,
    category        ENUM('vm','bare_metal','kubernetes','storage','composite') NOT NULL,
    description     TEXT,
    spec_json       JSON NOT NULL,               -- template parameters + defaults
    cost_formula    JSON NOT NULL,               -- pricing model for estimation
    version         INT NOT NULL DEFAULT 1,
    is_published    BOOLEAN NOT NULL DEFAULT FALSE,
    created_by      BIGINT NOT NULL REFERENCES users(id),
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_templates_category (category, is_published)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Resources (provisioned infrastructure)
CREATE TABLE resources (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    resource_uid    VARCHAR(64) NOT NULL UNIQUE,  -- e.g., "vm-a1b2c3d4"
    project_id      BIGINT NOT NULL REFERENCES projects(id),
    created_by      BIGINT NOT NULL REFERENCES users(id),
    template_id     BIGINT REFERENCES templates(id),
    resource_type   ENUM('vm','bare_metal','k8s_cluster','block_storage','object_storage','file_storage') NOT NULL,
    name            VARCHAR(255) NOT NULL,
    spec_json       JSON NOT NULL,               -- resolved resource specification
    status          ENUM('pending_approval','approved','provisioning','active','failed',
                         'expiring','deprovisioning','terminated') NOT NULL DEFAULT 'pending_approval',
    provider_id     VARCHAR(255),                -- OpenStack UUID, server serial, etc.
    ip_addresses    JSON,
    cost_per_hour   DECIMAL(10,4) DEFAULT 0,
    expires_at      TIMESTAMP NULL,
    provisioned_at  TIMESTAMP NULL,
    terminated_at   TIMESTAMP NULL,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_resources_project (project_id, status),
    INDEX idx_resources_type_status (resource_type, status),
    INDEX idx_resources_uid (resource_uid),
    INDEX idx_resources_created_by (created_by),
    INDEX idx_resources_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Approval workflows
CREATE TABLE workflows (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    resource_id     BIGINT NOT NULL REFERENCES resources(id),
    workflow_type   ENUM('auto_approve','single_approval','multi_approval') NOT NULL,
    status          ENUM('pending','approved','rejected','cancelled','timed_out') NOT NULL DEFAULT 'pending',
    policy_json     JSON NOT NULL,               -- approval policy that was evaluated
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    resolved_at     TIMESTAMP NULL,
    INDEX idx_workflows_resource (resource_id),
    INDEX idx_workflows_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Approval decisions
CREATE TABLE approval_decisions (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    workflow_id     BIGINT NOT NULL REFERENCES workflows(id),
    approver_id     BIGINT NOT NULL REFERENCES users(id),
    decision        ENUM('approved','rejected') NOT NULL,
    comment         TEXT,
    decided_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_ad_workflow (workflow_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Notifications
CREATE TABLE notifications (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id         BIGINT NOT NULL REFERENCES users(id),
    resource_id     BIGINT REFERENCES resources(id),
    channel         ENUM('email','slack','webhook','portal') NOT NULL,
    event_type      VARCHAR(100) NOT NULL,
    payload_json    JSON NOT NULL,
    status          ENUM('pending','sent','failed') NOT NULL DEFAULT 'pending',
    sent_at         TIMESTAMP NULL,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_notif_user (user_id, created_at),
    INDEX idx_notif_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Elasticsearch Audit Index

```json
{
  "mappings": {
    "properties": {
      "event_id":       { "type": "keyword" },
      "timestamp":      { "type": "date" },
      "actor_id":       { "type": "keyword" },
      "actor_email":    { "type": "keyword" },
      "action":         { "type": "keyword" },
      "resource_uid":   { "type": "keyword" },
      "resource_type":  { "type": "keyword" },
      "project_id":     { "type": "keyword" },
      "project_name":   { "type": "keyword" },
      "details":        { "type": "object", "enabled": true },
      "cost_cents":     { "type": "long" },
      "ip_address":     { "type": "ip" },
      "user_agent":     { "type": "text" }
    }
  },
  "settings": {
    "number_of_shards": 3,
    "number_of_replicas": 1,
    "index.lifecycle.name": "audit-policy",
    "index.lifecycle.rollover_alias": "audit-logs"
  }
}
```

### Database Selection

| Store | Use Case | Justification |
|-------|----------|---------------|
| MySQL 8.0 | Resources, users, projects, quotas, workflows | Strong consistency for quota enforcement; ACID for approval workflows; well-understood operational model |
| Elasticsearch 8.x | Audit logs, full-text search | Append-only audit events; time-series queries; 7-year retention with ILM; full-text search on resource names/descriptions |
| Redis 6.x | Session cache, rate limiting, quota cache | Sub-ms reads for frequently-checked quotas; distributed rate limiting with sliding window |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| resources | (project_id, status) | List resources by project filtered by status |
| resources | (resource_type, status) | Capacity planning queries across resource types |
| resources | (expires_at) | Cron job to find expiring resources |
| resources | (created_by) | "My resources" view |
| quotas | (project_id, resource_type) UNIQUE | Quota lookup during provisioning |
| workflows | (status) | Dashboard for pending approvals |
| notifications | (user_id, created_at) | User notification feed |
| Elasticsearch audit | timestamp + project_name + action | Compliance queries: "all actions by project X in date range" |

---

## 5. API Design

### REST Endpoints

**Base URL:** `https://portal-api.infra.company.com/api/v1`

**Authentication:** All endpoints require `Authorization: Bearer <JWT>` obtained via OIDC flow with corporate Okta. JWT contains: `sub`, `email`, `groups`, `exp`.

**Rate Limiting:** 100 requests/min per user (read), 20 requests/min per user (write). Rate limit headers: `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset`.

#### Templates

```
GET /templates
  Query: ?category=vm&published=true&page=1&size=20
  Response 200:
  {
    "items": [
      {
        "id": 42,
        "name": "ml-training-gpu",
        "display_name": "ML Training Rig (GPU)",
        "category": "vm",
        "description": "Pre-configured VM with CUDA drivers...",
        "spec": {
          "vcpu": 16,
          "memory_gb": 64,
          "disk_gb": 500,
          "gpu": { "type": "nvidia-h100", "count": 4 },
          "image": "ubuntu-22.04-cuda-12.2"
        },
        "cost_per_hour": 12.50,
        "version": 3
      }
    ],
    "total": 45,
    "page": 1,
    "size": 20
  }

GET /templates/{id}
  Response 200: { single template object }

POST /templates
  Auth: admin role required
  Body:
  {
    "name": "web-app-stack",
    "display_name": "Web Application Stack",
    "category": "composite",
    "spec_json": { ... },
    "cost_formula": { "base_hourly": 0.50, "per_vcpu": 0.02, "per_gb_ram": 0.01 }
  }
  Response 201: { created template }
```

#### Resources

```
POST /resources
  Body:
  {
    "project_id": 10,
    "template_id": 42,           // optional, can use raw spec
    "name": "alice-training-01",
    "resource_type": "vm",
    "spec": {
      "vcpu": 16,
      "memory_gb": 64,
      "disk_gb": 500,
      "gpu": { "type": "nvidia-h100", "count": 4 },
      "image": "ubuntu-22.04-cuda-12.2",
      "ssh_keys": ["ssh-ed25519 AAAA..."]
    },
    "expires_at": "2026-04-16T00:00:00Z"
  }
  Response 202 (Accepted):
  {
    "resource_uid": "vm-a1b2c3d4",
    "status": "pending_approval",
    "estimated_cost_per_hour": 12.50,
    "estimated_total_cost": 2100.00,
    "workflow_id": 789,
    "message": "Resource request submitted. Approval required (estimated cost > $100)."
  }

GET /resources
  Query: ?project_id=10&status=active&type=vm&page=1&size=50
  Response 200:
  {
    "items": [ { resource objects } ],
    "total": 23,
    "page": 1,
    "size": 50
  }

GET /resources/{uid}
  Response 200:
  {
    "resource_uid": "vm-a1b2c3d4",
    "name": "alice-training-01",
    "resource_type": "vm",
    "status": "active",
    "spec": { ... },
    "ip_addresses": ["10.0.5.42"],
    "cost_per_hour": 12.50,
    "total_cost_to_date": 450.00,
    "created_by": { "id": 1, "email": "alice@company.com" },
    "project": { "id": 10, "name": "ml-team" },
    "provisioned_at": "2026-04-09T10:30:00Z",
    "expires_at": "2026-04-16T00:00:00Z"
  }

DELETE /resources/{uid}
  Response 202:
  {
    "resource_uid": "vm-a1b2c3d4",
    "status": "deprovisioning",
    "message": "Resource termination initiated."
  }

POST /resources/{uid}/extend
  Body: { "new_expires_at": "2026-04-23T00:00:00Z" }
  Response 200: { updated resource }
```

#### Cost Estimation

```
POST /cost/estimate
  Body:
  {
    "resource_type": "vm",
    "spec": { "vcpu": 16, "memory_gb": 64, "gpu": { "type": "nvidia-h100", "count": 4 } },
    "duration_hours": 168
  }
  Response 200:
  {
    "cost_per_hour": 12.50,
    "total_estimated_cost": 2100.00,
    "breakdown": {
      "compute": 4.00,
      "memory": 0.64,
      "gpu": 7.86,
      "storage": 0.00
    },
    "currency": "USD"
  }
```

#### Quotas

```
GET /projects/{id}/quotas
  Response 200:
  {
    "project_id": 10,
    "project_name": "ml-team",
    "quotas": [
      { "resource_type": "vcpu", "used": 128, "reserved": 16, "soft_limit": 200, "hard_limit": 256 },
      { "resource_type": "gpu", "used": 8, "reserved": 4, "soft_limit": 16, "hard_limit": 20 },
      { "resource_type": "memory_gb", "used": 512, "reserved": 64, "soft_limit": 800, "hard_limit": 1024 }
    ]
  }

PUT /projects/{id}/quotas
  Auth: admin role required
  Body:
  {
    "quotas": [
      { "resource_type": "gpu", "soft_limit": 24, "hard_limit": 32 }
    ]
  }
  Response 200: { updated quotas }
```

#### Workflows / Approvals

```
GET /workflows/pending
  Query: ?approver=me
  Response 200:
  {
    "items": [
      {
        "workflow_id": 789,
        "resource": { "uid": "vm-a1b2c3d4", "name": "alice-training-01", "type": "vm" },
        "requester": { "email": "alice@company.com" },
        "estimated_cost": 2100.00,
        "created_at": "2026-04-09T10:00:00Z"
      }
    ]
  }

POST /workflows/{id}/approve
  Body: { "comment": "Approved for Q2 ML project" }
  Response 200: { workflow with status "approved" }

POST /workflows/{id}/reject
  Body: { "comment": "Budget exceeded. Please use smaller instance." }
  Response 200: { workflow with status "rejected" }
```

#### Audit

```
GET /audit
  Query: ?project_id=10&action=resource.created&from=2026-04-01&to=2026-04-09&page=1&size=100
  Response 200:
  {
    "items": [
      {
        "event_id": "evt-abc123",
        "timestamp": "2026-04-09T10:30:00Z",
        "actor_email": "alice@company.com",
        "action": "resource.created",
        "resource_uid": "vm-a1b2c3d4",
        "resource_type": "vm",
        "project_name": "ml-team",
        "cost_cents": 210000,
        "details": { "spec": { ... } }
      }
    ],
    "total": 156,
    "page": 1
  }
```

### CLI Design

The portal is also accessible via `infra-cli` (covered in detail in `cli_client_for_infra_platform.md`). Relevant commands:

```bash
# Template browsing
infra-cli templates list --category vm
infra-cli templates show ml-training-gpu

# Resource provisioning
infra-cli resources create --template ml-training-gpu --project ml-team --name alice-train-01
infra-cli resources list --project ml-team --status active
infra-cli resources delete vm-a1b2c3d4

# Quota checking
infra-cli quota show --project ml-team

# Approval management
infra-cli workflows pending
infra-cli workflows approve 789 --comment "Approved"
```

---

## 6. Core Component Deep Dives

### 6.1 Quota Enforcement Engine

**Why it's hard:** Concurrent provisioning requests from multiple users in the same project can cause over-provisioning if quota checks and decrements are not atomic. Naive "check then provision" has a classic TOCTOU race condition.

| Approach | Pros | Cons |
|----------|------|------|
| **Optimistic locking (SELECT FOR UPDATE)** | Strong consistency, simple | Contention under high concurrency |
| **Reservation-based (two-phase)** | Decouples check from commit; handles async provisioning | More complex; must handle reservation expiry |
| **Redis atomic counters** | Very fast, no DB contention | Inconsistent with MySQL on crash; requires reconciliation |
| **Saga pattern** | Distributed transactions across services | Complex compensation logic |

**Selected approach: Reservation-based with SELECT FOR UPDATE on commit.**

**Implementation:**
1. When a provisioning request arrives, the Quota Service starts a MySQL transaction.
2. `SELECT ... FOR UPDATE` on the quota row for `(project_id, resource_type)`.
3. Check: `used + reserved + requested <= hard_limit`. If not, reject.
4. If OK, increment `reserved` by the requested amount. Commit.
5. When provisioning succeeds, another transaction: decrement `reserved`, increment `used`.
6. When provisioning fails, decrement `reserved` (release reservation).
7. A background job runs every 5 minutes to expire stale reservations (reserved > 30 min without status change).

```java
@Transactional
public QuotaCheckResult reserveQuota(long projectId, ResourceType type, long amount) {
    Quota quota = quotaRepo.findByProjectAndTypeForUpdate(projectId, type); // SELECT FOR UPDATE
    long available = quota.getHardLimit() - quota.getUsed() - quota.getReserved();
    if (amount > available) {
        return QuotaCheckResult.denied(available, amount);
    }
    boolean softLimitExceeded = (quota.getUsed() + quota.getReserved() + amount) > quota.getSoftLimit();
    quota.setReserved(quota.getReserved() + amount);
    quotaRepo.save(quota);
    return QuotaCheckResult.granted(softLimitExceeded);
}
```

**Failure modes:**
- **DB deadlock:** Two concurrent requests lock quota rows in different order. Mitigation: always lock rows in canonical order (project_id, then resource_type alphabetically).
- **Reservation leak:** Provisioning process crashes after reservation but before commit/release. Mitigation: background reaper job with 30-min TTL on reservations.
- **Quota drift:** Actual usage drifts from tracked `used` value due to manual resource changes. Mitigation: periodic reconciliation job queries actual resources and adjusts `used`.

**Interviewer Q&As:**

**Q1: How do you handle quota across multiple resource types for a single provisioning request (e.g., a VM needs CPU, memory, GPU, and disk quota)?**
A: We acquire all quota reservations in a single transaction, locking rows in deterministic order (alphabetical by resource_type within the same project_id). If any single resource type is over limit, the entire request is rejected and no reservations are made. This ensures atomicity across resource types.

**Q2: What if a team legitimately needs to temporarily exceed their quota for a deadline?**
A: We support "quota overrides" -- an admin can temporarily increase the hard_limit with an expiry timestamp. The override is stored in a `quota_overrides` table and the effective limit is `max(hard_limit, override_limit)` during the override period. All overrides are audited.

**Q3: How do you prevent a single user from monopolizing a project's entire quota?**
A: We support optional per-user sub-quotas within a project. The `project_members` table can have per-user limits. The check becomes: `min(project_remaining, user_remaining)`.

**Q4: What happens when you delete a resource -- how does quota get reclaimed?**
A: When a resource transitions to `terminated`, the Resource Service publishes a `resource.terminated` event. The Quota Service consumer decrements `used` in a transaction. If the event is lost (broker down), the reconciliation job catches it within 5 minutes.

**Q5: How do you handle quota for resources with variable sizing (e.g., auto-scaling K8s clusters)?**
A: For auto-scaling resources, we reserve the maximum configured size at creation time. If the user sets `min_nodes=3, max_nodes=10`, we reserve quota for 10 nodes. This is conservative but prevents surprise over-quota. Users can see "reserved vs actual" in the portal.

**Q6: How does the cost estimation interact with quota?**
A: Cost estimation is a separate read-only calculation that doesn't modify quota. It runs before the user confirms the request. Only when the user clicks "Provision" do we attempt quota reservation. This prevents stale reservations from users just browsing costs.

---

### 6.2 Multi-Step Workflow Engine

**Why it's hard:** Provisioning workflows span minutes to hours (bare-metal can take 15+ minutes). The engine must handle approval chains, timeouts, retries, and partial failures while maintaining exactly-once semantics.

| Approach | Pros | Cons |
|----------|------|------|
| **Homegrown state machine in DB** | Simple, full control | Must implement persistence, retries, timers manually |
| **Temporal.io / Cadence** | Battle-tested, durable execution, built-in retries/timers | Operational overhead of running Temporal cluster |
| **Spring State Machine + RabbitMQ** | Integrates with existing stack; event-driven | Less mature than Temporal; state consistency tricky |
| **Camunda / Zeebe (BPMN)** | Visual workflow designer; audit-friendly | Heavy; BPMN overkill for infra workflows |

**Selected approach: Temporal.io for workflow orchestration.**

**Justification:** Provisioning workflows are long-running (minutes to hours), require durable state, and need built-in retry/timeout/compensation semantics. Temporal provides all of this without us reinventing it. The team already runs Kubernetes, so deploying Temporal is straightforward.

**Implementation:**

```
Workflow: ProvisionResourceWorkflow
  Input: ResourceRequest (spec, project, user, template)

  Step 1: ValidateRequest (Activity, 5s timeout)
    - Schema validation
    - Permission check

  Step 2: ReserveQuota (Activity, 10s timeout, 3 retries)
    - Call Quota Service to reserve

  Step 3: EvaluateApprovalPolicy (Activity, 5s timeout)
    - If cost < $100 and resource_type is VM: auto-approve
    - If cost >= $100 or bare-metal: require manager approval
    - If GPU count >= 8: require infra-team approval

  Step 4: WaitForApproval (Signal, 72h timeout)
    - Workflow pauses, waits for external signal (approval/rejection)
    - If timeout: auto-reject, release quota reservation
    - If rejected: release quota reservation, notify user

  Step 5: Provision (Activity, 30min timeout, 3 retries)
    - Call appropriate provisioner (OpenStack/bare-metal/K8s)
    - Poll for completion

  Step 6: ConfigureResource (Activity, 10min timeout)
    - Set up DNS, monitoring agent, SSH keys
    - Register in CMDB

  Step 7: FinalizeQuota (Activity, 10s timeout)
    - Convert reservation to used

  Step 8: Notify (Activity, 30s timeout)
    - Send email + Slack to requester
    - Update portal via WebSocket

  Compensation (on failure at any step):
    - Release quota reservation
    - Deprovision any partially created resources
    - Notify user of failure with reason
```

**Failure modes:**
- **Temporal cluster down:** Workflows are durably persisted in Temporal's DB. On recovery, workflows resume from last checkpoint. No provisioning is lost.
- **Provisioner API timeout:** Retry with exponential backoff (3 attempts, 1min/5min/15min). After exhaustion, compensate and notify.
- **Approval timeout:** 72h signal timeout triggers auto-rejection. Quota reservation released. User notified.
- **Duplicate execution:** Temporal guarantees exactly-once execution per workflow ID. We use `resource_uid` as workflow ID.

**Interviewer Q&As:**

**Q1: Why Temporal over a simple database-backed state machine?**
A: A database-backed state machine requires us to build: durable timers, retry policies, compensation logic, signal handling, and distributed task dispatch. Temporal provides all of this out of the box. The operational cost of running Temporal (3-node cluster on K8s) is far less than the engineering cost of building and debugging a custom workflow engine for long-running processes.

**Q2: How do you handle the case where approval is needed from two different people (multi-approval)?**
A: The workflow's Step 4 becomes a parallel wait: it sends signals to both required approvers and waits for all signals. Temporal's `Workflow.await()` with a predicate makes this natural. If any approver rejects, the workflow short-circuits to compensation.

**Q3: What if the provisioning succeeds but the notification fails?**
A: Notification is a non-critical activity. We retry 3 times, and if it still fails, we log the failure and mark the resource as active anyway. A background job scans for resources where `status = active AND notification_sent = false` and retries.

**Q4: How do you test workflows?**
A: Temporal provides a test framework that lets you mock activities and fast-forward timers. We unit-test each activity independently (e.g., test quota reservation in isolation) and integration-test the full workflow with a test Temporal cluster.

**Q5: Can users cancel a pending workflow?**
A: Yes. We send a cancellation signal to the Temporal workflow. The workflow handles cancellation by running compensation (release quota, clean up partial resources) and transitioning the resource to `cancelled`.

**Q6: How do you handle version upgrades of workflow definitions?**
A: Temporal supports workflow versioning via `Workflow.getVersion()`. When we change workflow logic, we use version branching so in-flight workflows continue with old logic and new workflows use new logic. We never break running workflows.

---

### 6.3 Template Catalog & Cost Estimation

**Why it's hard:** Templates must be flexible enough for diverse infrastructure patterns (single VM to multi-tier stacks) while being simple enough for developers to use. Cost estimation must be accurate across different resource types with different pricing models (per-hour compute, per-GB storage, per-GPU-hour).

| Approach | Pros | Cons |
|----------|------|------|
| **Static JSON templates** | Simple, predictable | No parameterization; template explosion |
| **Jinja2/Handlebars templating** | Flexible parameterization | Security risk (template injection); harder to validate |
| **Schema-driven forms (JSON Schema)** | Auto-generated UI; validatable | Limited expressiveness for complex templates |
| **Helm-like chart system** | Proven in K8s ecosystem; composable | Overkill for non-K8s resources |

**Selected approach: JSON Schema-driven templates with parameter overrides.**

**Implementation:**

A template defines:
1. **Parameters** (with types, defaults, constraints) -- expressed as JSON Schema.
2. **Resource spec** -- references parameters via `${param_name}` substitution.
3. **Cost formula** -- mathematical model using parameters.

```json
{
  "name": "ml-training-gpu",
  "category": "vm",
  "parameters": {
    "type": "object",
    "properties": {
      "gpu_count": { "type": "integer", "minimum": 1, "maximum": 8, "default": 4 },
      "gpu_type": { "type": "string", "enum": ["nvidia-a100", "nvidia-h100"], "default": "nvidia-h100" },
      "vcpu":     { "type": "integer", "minimum": 4, "maximum": 64, "default": 16 },
      "memory_gb": { "type": "integer", "minimum": 16, "maximum": 256, "default": 64 },
      "disk_gb":  { "type": "integer", "minimum": 100, "maximum": 2000, "default": 500 }
    },
    "required": ["gpu_count", "gpu_type"]
  },
  "spec_template": {
    "vcpu": "${vcpu}",
    "memory_gb": "${memory_gb}",
    "disk_gb": "${disk_gb}",
    "gpu": { "type": "${gpu_type}", "count": "${gpu_count}" },
    "image": "ubuntu-22.04-cuda-12.2"
  },
  "cost_formula": {
    "hourly": "(gpu_count * gpu_rate[gpu_type]) + (vcpu * 0.02) + (memory_gb * 0.01) + (disk_gb * 0.0001)",
    "gpu_rates": {
      "nvidia-a100": 1.50,
      "nvidia-h100": 2.00
    }
  }
}
```

**Cost Estimation Engine:**
```java
public CostEstimate calculateCost(Template template, Map<String, Object> params, int durationHours) {
    CostFormula formula = template.getCostFormula();
    ExpressionEngine engine = new SpELExpressionEngine();

    double hourlyRate = engine.evaluate(formula.getHourly(), params);
    double totalCost = hourlyRate * durationHours;

    Map<String, Double> breakdown = new LinkedHashMap<>();
    breakdown.put("gpu",     (int)params.get("gpu_count") * formula.getGpuRate((String)params.get("gpu_type")));
    breakdown.put("compute", (int)params.get("vcpu") * 0.02);
    breakdown.put("memory",  (int)params.get("memory_gb") * 0.01);
    breakdown.put("storage", (int)params.get("disk_gb") * 0.0001);

    return new CostEstimate(hourlyRate, totalCost, breakdown, "USD");
}
```

**Failure modes:**
- **Template injection:** User passes malicious parameter values. Mitigation: JSON Schema validation strictly enforces types, ranges, and enums before any substitution occurs.
- **Cost formula drift:** Actual pricing changes but templates still use old rates. Mitigation: cost rates are stored in a separate `pricing` table, not in templates. Templates reference rate keys, not hardcoded values.
- **Template version incompatibility:** Existing resources reference a template version that's been updated. Mitigation: templates are versioned; resources store the resolved spec, not just a template reference.

**Interviewer Q&As:**

**Q1: How do you handle composite templates (e.g., "Web App Stack" = 3 VMs + 1 load balancer + 1 database)?**
A: Composite templates define multiple resource specs, each referencing the same parameter set. The workflow engine provisions them in dependency order (database first, then app VMs, then load balancer). Cost estimation sums all components.

**Q2: How do you keep template pricing accurate?**
A: Pricing rates live in a dedicated `pricing` table, updated by the finance team via admin API. Templates reference rate keys (e.g., `gpu_rate.h100`), not hardcoded values. A nightly job validates that all template cost formulas resolve correctly with current rates.

**Q3: Can developers create their own templates?**
A: Yes, but with guardrails. Any user can create a "draft" template visible only to their project. Publishing to the org-wide catalog requires admin approval. This prevents template sprawl while enabling experimentation.

**Q4: How do you handle template deprecation?**
A: Templates have a `deprecated_at` field. Deprecated templates are hidden from the catalog but existing resources referencing them continue to work. A migration path is provided: the deprecated template points to its recommended replacement.

**Q5: What if the cost estimation is wildly wrong -- does the user get charged the estimate or actual?**
A: Users are charged actual usage. The estimate is advisory and shown with a disclaimer. We track estimation accuracy (estimated vs actual) as a metric and alert if error rate exceeds 10%.

**Q6: How do you prevent someone from creating a template that provisions a 1000-GPU cluster?**
A: Templates have a `max_cost_per_hour` field enforced by the system. Any template with estimated cost exceeding this limit requires infra-admin approval to publish. Additionally, quota enforcement prevents actual provisioning beyond limits regardless of template content.

---

## 7. Scheduling & Resource Management

### Resource Scheduling for Bare-Metal

Bare-metal servers are finite and non-fungible (specific hardware configs). Scheduling requires:

1. **Inventory Management:** A real-time view of all physical servers, their hardware specs, current allocation status, and maintenance windows.

2. **Reservation System:**
   - Users request: type (e.g., `gpu-h100-8x`), count, start time, duration.
   - Scheduler checks availability using an interval tree (overlapping reservation detection).
   - Implements bin-packing: prefer partially-utilized racks to minimize power/cooling costs.

3. **Preemption Policy:**
   - Priority levels: `critical` (production), `high` (staging), `normal` (dev), `low` (batch).
   - Lower-priority reservations can be preempted with 30-min warning for higher-priority.
   - Preempted users get notification + auto-rescheduled to next available slot.

4. **Maintenance Windows:**
   - Hardware maintenance is scheduled via `maintenance_windows` table.
   - Scheduler excludes servers with upcoming maintenance from new reservations.
   - Existing reservations overlapping maintenance get migrated (VMs) or rescheduled (bare-metal).

```sql
CREATE TABLE server_inventory (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    hostname        VARCHAR(255) NOT NULL UNIQUE,
    server_type     VARCHAR(100) NOT NULL,  -- e.g., "gpu-h100-8x", "cpu-epyc-128c"
    rack_id         VARCHAR(50) NOT NULL,
    datacenter      VARCHAR(50) NOT NULL,
    status          ENUM('available','reserved','provisioning','in_use','maintenance','decommissioned') NOT NULL,
    specs_json      JSON NOT NULL,
    ipmi_ip         VARCHAR(45),
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_si_type_status (server_type, status),
    INDEX idx_si_rack (rack_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE reservations (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    server_id       BIGINT NOT NULL REFERENCES server_inventory(id),
    resource_id     BIGINT NOT NULL REFERENCES resources(id),
    project_id      BIGINT NOT NULL REFERENCES projects(id),
    priority        ENUM('critical','high','normal','low') NOT NULL DEFAULT 'normal',
    starts_at       TIMESTAMP NOT NULL,
    ends_at         TIMESTAMP NOT NULL,
    status          ENUM('confirmed','active','completed','cancelled','preempted') NOT NULL,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_res_server_time (server_id, starts_at, ends_at),
    INDEX idx_res_project (project_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Scheduling Algorithm

```
function findAvailableServers(type, count, startTime, endTime):
    candidates = SELECT * FROM server_inventory
                 WHERE server_type = type AND status IN ('available', 'reserved')

    for each candidate:
        conflicts = SELECT COUNT(*) FROM reservations
                    WHERE server_id = candidate.id
                    AND starts_at < endTime AND ends_at > startTime
                    AND status IN ('confirmed', 'active')
        if conflicts == 0:
            available.add(candidate)

    if available.size >= count:
        return selectByRackAffinity(available, count)  // bin-pack into fewest racks
    else:
        return INSUFFICIENT_CAPACITY
```

### VM Scheduling (OpenStack Integration)

For VMs, we delegate to OpenStack Nova's scheduler but enforce our quota and approval layers on top:
1. Portal validates quota and runs approval workflow.
2. On approval, calls Nova API: `POST /servers` with flavor, image, network, and availability zone.
3. Nova's FilterScheduler handles host selection (RAM/CPU/disk filters + weighers).
4. Portal polls Nova for server status until `ACTIVE`.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach |
|-----------|-----------------|
| React Frontend | CDN-served; scales infinitely |
| BFF API Gateway | Stateless; horizontal pods behind LB; 4 pods baseline, HPA on CPU |
| Resource Service | Stateless; 3 pods baseline, HPA on request rate |
| Workflow Engine (Temporal workers) | Horizontal workers; scale by task queue depth |
| Quota Service | Stateless; 2 pods baseline (low traffic, high consistency needs) |
| Audit Service | Stateless; 3 pods; batched writes to ES |
| MySQL | Primary + 2 read replicas; reads go to replicas for list/search |
| Elasticsearch | 3-node cluster; add data nodes for storage |
| RabbitMQ | 3-node mirror cluster; add nodes for throughput |
| Redis | 3-node sentinel cluster; used for caching only |

### Database Scaling

At our scale (6 RPS peak writes, ~50K resource rows), MySQL easily handles the load on a single primary. If we grow 100x:
- **Sharding by project_id:** Resources, quotas, workflows are project-scoped. Shard key = `project_id % N`.
- **Read replicas:** List/search queries route to replicas. Write-after-read consistency handled by reading from primary within 1s of write.
- **Archival:** Resources in `terminated` status older than 90 days move to `resources_archive` table (or cold storage).

### Interviewer Q&As

**Q1: What's the first bottleneck you'd hit as you scale from 10K to 100K developers?**
A: MySQL write throughput on the quotas table. Every provisioning request takes a row-level lock on quotas. At 100K devs with 10x the provisioning rate (~70 RPS peak writes), lock contention becomes significant. Solution: partition quotas by project_id into 16 shards. Most requests within a project are sequential (same team), so intra-shard contention remains low.

**Q2: How would you handle a flash crowd (e.g., all 10K engineers provisioning at once for a hackathon)?**
A: (1) Queue provisioning requests in RabbitMQ with rate limiting -- accept all requests immediately (202 Accepted) but process at a sustainable rate. (2) Pre-provision a pool of "warm" VMs for the hackathon template. (3) Apply per-user rate limits (5 provisions/hour). (4) Auto-scale Temporal workers to handle the burst.

**Q3: How do you scale the notification system for 100K users?**
A: Notifications are asynchronous (RabbitMQ). We batch Slack notifications (one API call per channel, not per user). Email uses SES with batched sends. The notification service is the easiest to scale because it's fully asynchronous and idempotent.

**Q4: What about scaling the audit log?**
A: Elasticsearch scales horizontally. We use time-based indices (one per month) with ILM policies: hot (SSD, 0-3 months), warm (HDD, 3-12 months), cold (S3 snapshot, 1-7 years). At 36 GB/year, even 100x growth (3.6 TB/year) is manageable.

**Q5: How would you handle multi-region deployment of the portal?**
A: The portal is a control plane, not data plane, so single-region active-active is not required initially. For DR: MySQL async replication to secondary region, Elasticsearch cross-cluster replication, and a standby deployment that can be promoted. For true multi-region: shard by region (each region has its own portal instance managing local infrastructure, with a global catalog service for templates).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---------|--------|-----------|------------|-----|
| MySQL primary down | No writes; stale reads from replica | Heartbeat check every 5s | Automated failover to replica (MHA/Orchestrator); Temporal workflows pause and resume | 30s |
| Elasticsearch down | No audit writes; no search | Health check endpoint | Audit events buffered in RabbitMQ (TTL 24h); retry on recovery | 5 min |
| RabbitMQ down | Provisioning workflows stall | Queue depth monitoring | Mirrored queues (3 nodes); if cluster down, Temporal still has durable state | 1 min (node), 5 min (cluster) |
| Temporal cluster down | No new workflows; existing paused | Heartbeat monitoring | 3-node cluster; Temporal DB backed by MySQL with replication | 2 min |
| OpenStack API down | VM provisioning fails | API health checks | Retry with backoff; user sees "provisioning delayed" status | N/A (depends on OpenStack) |
| Bare-metal provisioner down | Bare-metal provisioning fails | Health check | Queue requests; retry on recovery; alert on-call | 15 min |
| Redis down | Cache miss; slightly slower quota checks | Sentinel monitoring | Fall through to MySQL; no data loss; sentinel auto-failover | 10s |
| Portal frontend CDN down | Users can't load portal | Synthetic monitoring | Multi-CDN failover (CloudFront -> Fastly); CLI still works via API | 1 min |
| Corporate IdP (Okta) down | No new logins | Okta status page monitoring | Existing JWTs valid until expiry (1h); extend JWT TTL during outage; emergency local auth for admins | N/A (depends on Okta) |
| Quota DB corruption | Over-provisioning or false denials | Reconciliation job alerts on drift | Reconciliation job corrects `used` values from actual resources; manual override for urgent cases | 5 min (auto-reconcile) |

### Circuit Breakers

Each service-to-service call uses Resilience4j circuit breakers:
- **Failure threshold:** 50% failures in 10-call sliding window.
- **Open state duration:** 30 seconds.
- **Fallback behavior:** Return cached data (for reads) or queue for retry (for writes).

### Idempotency

All provisioning operations are idempotent:
- Resource creation uses `resource_uid` as idempotency key.
- Workflow execution uses `resource_uid` as Temporal workflow ID (at-most-once guarantee).
- Quota operations use versioned updates (optimistic concurrency control as fallback).

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold | Description |
|--------|------|-----------------|-------------|
| `portal.request.latency.p99` | Histogram | > 2s | API response time |
| `portal.request.error_rate` | Rate | > 1% | 5xx error rate |
| `provisioning.duration.p99` | Histogram | VM > 5min, BM > 30min | End-to-end provisioning time |
| `provisioning.failure_rate` | Rate | > 5% | Provisioning failures |
| `quota.utilization_pct` | Gauge | > 90% (per project) | Quota usage as percentage of hard limit |
| `quota.reservation_leak` | Counter | > 0 sustained 30min | Reservations not resolved within TTL |
| `workflow.pending_approvals` | Gauge | > 50 | Pending approval queue depth |
| `workflow.approval_latency.p50` | Histogram | > 24h | Time from request to approval decision |
| `mysql.replication_lag` | Gauge | > 5s | Replica lag |
| `mysql.connections_active` | Gauge | > 80% max | Connection pool utilization |
| `rabbitmq.queue_depth` | Gauge | > 1000 | Message backlog |
| `elasticsearch.indexing_rate` | Rate | < 50% baseline | Audit ingestion rate |
| `resource.active_count` | Gauge | Informational | Total active resources by type |
| `resource.expiring_24h` | Gauge | Informational | Resources expiring in next 24h |
| `cost.daily_spend` | Gauge | > 120% budget | Daily spend per project |
| `notification.delivery_failure_rate` | Rate | > 5% | Failed notification deliveries |

### Dashboards

1. **Operations Dashboard:** Provisioning rate, failure rate, queue depth, latency percentiles.
2. **Capacity Dashboard:** Resource utilization by type, quota usage, available capacity.
3. **Cost Dashboard:** Daily/weekly/monthly spend by project, top consumers, cost trends.
4. **Audit Dashboard:** Recent actions, top actors, suspicious patterns (e.g., mass deletions).

### Distributed Tracing

- OpenTelemetry instrumentation on all services.
- Trace ID propagated through HTTP headers, RabbitMQ message properties, Temporal workflow context.
- Full trace from "user clicks Provision" to "resource Active" visible in Jaeger.

---

## 11. Security

### Auth & AuthZ

**Authentication flow:**
1. User navigates to portal.
2. React app redirects to Okta OIDC `/authorize` endpoint.
3. User authenticates (MFA enforced by Okta).
4. Okta redirects back with authorization code.
5. BFF exchanges code for tokens (`id_token`, `access_token`, `refresh_token`).
6. BFF validates `id_token` (signature, issuer, audience, expiry).
7. BFF issues session cookie (HttpOnly, Secure, SameSite=Strict) mapped to server-side session.
8. Subsequent API calls use session cookie; BFF validates session, extracts user context.

### SSO Integration

```yaml
# Spring Security OIDC Configuration
spring:
  security:
    oauth2:
      client:
        registration:
          okta:
            client-id: ${OKTA_CLIENT_ID}
            client-secret: ${OKTA_CLIENT_SECRET}
            scope: openid, profile, email, groups
            redirect-uri: "{baseUrl}/login/oauth2/code/{registrationId}"
        provider:
          okta:
            issuer-uri: https://company.okta.com/oauth2/default
            user-name-attribute: email
```

**SAML 2.0 (for legacy systems):**
- Spring Security SAML2 Service Provider.
- Metadata exchange with Okta.
- Attribute mapping: `NameID` -> email, `groups` -> roles.

### RBAC Enforcement

| Role | Permissions |
|------|------------|
| `viewer` | Read resources, templates, quotas for their projects |
| `developer` | All viewer + create/delete own resources, submit provisioning requests |
| `approver` | All developer + approve/reject workflows for their projects |
| `project_admin` | All approver + manage project members, modify project quotas (within org limits) |
| `infra_admin` | All permissions; manage templates, global quotas, user roles |

**Enforcement points:**
1. **API Gateway:** JWT validation, role extraction from Okta groups claim.
2. **Service layer:** `@PreAuthorize` annotations in Spring Boot.
3. **Data layer:** All queries include `project_id` filter; users can only access resources in projects they belong to.

```java
@PreAuthorize("hasRole('DEVELOPER') and @projectAuthz.isMember(#projectId, authentication)")
@PostMapping("/resources")
public ResponseEntity<Resource> createResource(@RequestBody ResourceRequest req) { ... }
```

### Additional Security Measures

- **Input validation:** JSON Schema validation on all request bodies.
- **SQL injection:** Parameterized queries via JPA/Hibernate.
- **XSS:** React auto-escapes; CSP headers; DOMPurify for any rendered HTML.
- **CSRF:** SameSite cookies + CSRF tokens for state-changing operations.
- **Secrets management:** Vault for service credentials; no secrets in config files.
- **Network isolation:** Services communicate over mTLS within K8s; ingress via TLS-only.

---

## 12. Incremental Rollout

### Phase 1: Foundation (Weeks 1-4)
- User service with Okta SSO integration.
- Project and RBAC management.
- Template catalog (read-only, admin-seeded).
- VM provisioning via OpenStack (auto-approve only, no workflow).
- Basic portal UI: list templates, create VM, view my resources.

### Phase 2: Workflows & Quotas (Weeks 5-8)
- Temporal-based workflow engine.
- Approval workflows (single-approver).
- Quota service with reservation-based enforcement.
- Cost estimation engine.
- Notification service (email only).

### Phase 3: Bare-Metal & K8s (Weeks 9-12)
- Bare-metal reservation system.
- K8s cluster provisioning via ClusterAPI.
- Storage provisioning (Ceph RBD, S3).
- Multi-approver workflows.
- Slack notifications.

### Phase 4: Polish & Scale (Weeks 13-16)
- Audit service with Elasticsearch.
- Dashboard with utilization charts.
- Lifecycle management (auto-expire, renewal).
- CLI integration.
- Template self-service (create + publish flow).

### Rollout Q&As

**Q1: How do you migrate existing manually-provisioned resources into the portal?**
A: We build an "import" feature. Infra-admins run a reconciliation script that queries OpenStack/bare-metal inventory, creates resource records in the portal DB, and assigns them to projects. Users confirm ownership in the portal. This is similar to Terraform's `import` command.

**Q2: How do you handle the chicken-and-egg problem where users need the portal to provision, but the portal itself needs infrastructure?**
A: The portal infrastructure is provisioned via IaC (Terraform/Ansible) managed by the infra team. The portal is "level 0" infrastructure that bootstraps everything else. We don't self-host through the portal -- that would be circular dependency.

**Q3: How do you get developer adoption?**
A: (1) Make the portal faster than filing a ticket (target: VM in 3 min vs 3 days via ticket). (2) Integrate with existing developer workflows (CLI, CI/CD). (3) Start with one friendly team as beta users; iterate on feedback. (4) Gradually deprecate the ticket-based process.

**Q4: What's your feature flag strategy?**
A: We use LaunchDarkly (or similar) for feature flags. Each new provisioning type (bare-metal, K8s, storage) is behind a flag, enabled per-team during rollout. This allows gradual exposure and quick rollback if issues arise.

**Q5: How do you handle rollback if a new portal version breaks provisioning?**
A: Blue-green deployment. The previous version remains deployed and can receive traffic within 30 seconds by updating the load balancer. Temporal workflows are version-safe (old workflows continue on old worker code). Database migrations are always forward-compatible (no destructive schema changes).

**Q6: How do you ensure zero downtime during database migrations?**
A: We follow the expand-contract pattern: (1) Add new columns/tables (expand). (2) Deploy code that writes to both old and new schema. (3) Backfill data. (4) Deploy code that reads from new schema. (5) Drop old columns (contract, weeks later). All migrations are tested against a production-clone database.

---

## 13. Trade-offs & Decision Log

| Decision | Chosen | Alternative | Why |
|----------|--------|-------------|-----|
| Workflow engine | Temporal.io | Homegrown state machine | Long-running workflows need durable execution; Temporal battle-tested at Uber/Netflix scale |
| Primary database | MySQL 8.0 | PostgreSQL | Organizational standard; team expertise; adequate for our scale |
| Audit store | Elasticsearch | MySQL + partitioning | 7-year retention + full-text search + time-series queries; ES excels at all three |
| Message broker | RabbitMQ | Kafka | We need task queues with acknowledgment, not event streaming; RabbitMQ's routing + dead-letter queues are better fit |
| Frontend | React SPA | Server-side rendering (Next.js) | Portal is an internal tool; SPA provides better UX for interactive workflows; SEO irrelevant |
| Auth | OIDC with Okta | SAML-only | OIDC is more modern, better developer experience, native JWT support |
| Quota enforcement | Reservation-based with SELECT FOR UPDATE | Redis atomic counters | Consistency with MySQL as source of truth; acceptable contention at our scale |
| Template system | JSON Schema-driven | Helm charts | Need to support VMs, bare-metal, and storage, not just K8s; JSON Schema is universal |
| Cost estimation | Formula-based (SpEL expressions) | Lookup table | Formulas handle parameterized templates; lookup tables would need one entry per parameter combination |
| Notification | Multi-channel (email + Slack + webhook) | Email only | Engineers live in Slack; webhooks enable integration with PagerDuty, custom tools |

---

## 14. Agentic AI Integration

### Natural Language Infrastructure Requests

**Feature:** Users can describe infrastructure needs in natural language, and an AI agent translates to a concrete provisioning request.

```
User: "I need a GPU machine for training a large language model. It should have at least 4 H100s,
       enough RAM for a 70B parameter model, and fast local storage. I'll need it for about a week."

AI Agent Response:
  "Based on your requirements, I recommend:
   - Template: ML Training Rig (GPU)
   - GPU: 4x NVIDIA H100 (80GB each = 320GB GPU memory, sufficient for 70B params with model parallelism)
   - CPU: 32 vCPUs (for data preprocessing)
   - RAM: 256 GB (for data loading buffers)
   - Storage: 2TB NVMe SSD (fast local storage for checkpoints)
   - Duration: 7 days
   - Estimated cost: $3,360 ($20/hr x 168 hours)

   This will require manager approval (cost > $100). Shall I submit the request?"
```

**Implementation:**
- LLM (GPT-4 or Claude) with function calling / tool use.
- Tools available to the agent: `search_templates`, `estimate_cost`, `check_quota`, `submit_request`.
- Guardrails: Agent cannot bypass approval workflows or quota limits.
- All AI-generated requests go through the same workflow engine.

### AI-Assisted Cost Optimization

```
User: "How can I reduce my team's infrastructure costs?"

AI Agent:
  "I analyzed your team's resource usage for the past 30 days:

   1. 3 GPU VMs (gpu-h100-4x) have been idle >80% of the time. Consider:
      - Switching to on-demand scheduling (provision only during training runs)
      - Estimated savings: $2,400/month

   2. Your K8s cluster 'ml-staging' has 10 nodes but average utilization is 25%.
      Consider scaling to 4 nodes with cluster autoscaler.
      - Estimated savings: $800/month

   3. 2 bare-metal reservations expired 5 days ago but are still active (missed auto-expire).
      - Immediate savings if released: $1,200/month

   Total potential savings: $4,400/month (35% reduction)"
```

### AI-Powered Anomaly Detection

- Monitor provisioning patterns; flag unusual activity.
- Example: "User X provisioned 50 VMs in the last hour (normal: 2/day). Possible runaway automation."
- Automatically pause provisioning and alert the project admin.

### Conversational Troubleshooting

```
User: "My VM vm-a1b2c3d4 is unreachable"

AI Agent:
  "Let me investigate vm-a1b2c3d4:
   1. Status in portal: active (provisioned 2 days ago)
   2. OpenStack status: ACTIVE
   3. Last health check: FAILED (2 minutes ago, timeout)
   4. Network: IP 10.0.5.42, VLAN 100
   5. Recent events: None unusual

   Likely causes:
   - OS-level issue (kernel panic, disk full)
   - Network connectivity issue

   Recommended actions:
   1. I can trigger an IPMI console reset (soft reboot). Shall I proceed?
   2. Or I can open an emergency ticket with the network team.
   3. I can also provision a replacement VM from the same template while we investigate."
```

### Architecture for AI Integration

```
+------------------+     +-------------------+     +------------------+
| Portal / CLI     |---->| AI Gateway        |---->| LLM Provider     |
| (user input)     |     | (Spring Boot)     |     | (Claude/GPT-4)   |
+------------------+     +--------+----------+     +------------------+
                                  |
                         +--------v----------+
                         | Tool Executor     |
                         | - search_templates|
                         | - estimate_cost   |
                         | - check_quota     |
                         | - query_resources |
                         | - get_metrics     |
                         +-------------------+
```

**Safety guardrails:**
- AI can propose but never directly execute destructive actions (delete, deprovision).
- All AI-proposed provisioning goes through the same approval workflow.
- AI responses are logged to the audit trail.
- Rate limiting: max 10 AI queries per user per hour.
- Prompt injection defense: user input is sandboxed; tool calls are validated against allowed schemas.

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through what happens when a developer clicks "Provision" for a GPU VM.**
A: (1) React frontend sends POST to BFF `/api/v1/resources`. (2) BFF validates JWT, extracts user/project context. (3) BFF calls Quota Service to check GPU, CPU, memory, disk quotas. (4) If quota OK, Resource Service creates resource record (status: pending_approval). (5) Workflow Engine evaluates approval policy (GPU VM > $100/day, so manager approval required). (6) Workflow pauses, waiting for approval signal. (7) Manager gets Slack notification, approves in portal. (8) Workflow resumes, calls OpenStack Nova to create server. (9) Polls Nova until server is ACTIVE (typically 2-3 min). (10) Updates resource record with IP, status=active. (11) Converts quota reservation to used. (12) Sends completion notification to developer. (13) Writes audit event to Elasticsearch.

**Q2: How do you prevent a runaway automation script from provisioning 1,000 VMs?**
A: Multiple layers: (1) Per-user rate limit (20 write requests/min). (2) Project quota hard limits. (3) Approval workflow triggers for bulk requests (>5 resources in 1 hour). (4) AI anomaly detection flags unusual patterns. (5) Circuit breaker on provisioner calls. Any single layer is sufficient; together they provide defense in depth.

**Q3: Your MySQL primary fails. What happens to in-flight provisioning requests?**
A: Temporal workflows are persisted in Temporal's own database (separate MySQL instance or PostgreSQL). So in-flight workflows are safe. The portal's MySQL failing means: no new requests can be created, quota checks fail (fail-closed, so no over-provisioning), and list/read operations fall back to read replica. Automated failover (MHA/Orchestrator) promotes a replica to primary within 30 seconds. Temporal workers retry failed activities automatically once the portal DB is back.

**Q4: How do you handle the case where OpenStack says a VM is created, but the portal DB write fails?**
A: The Temporal activity wrapping the OpenStack call is idempotent. If the DB write fails, Temporal retries the entire activity. On retry, we check if a VM with our `resource_uid` tag already exists in OpenStack (idempotency key). If yes, we skip creation and proceed to the DB write. This ensures exactly-once semantics.

**Q5: Why not use Kubernetes CRDs for everything instead of a custom portal?**
A: CRDs are great for K8s-native resources but our scope includes bare-metal servers, OpenStack VMs, and storage systems that aren't K8s-managed. Additionally, CRDs don't provide approval workflows, cost estimation, a user-friendly UI, or quota enforcement. The portal is the unified control plane across all infrastructure types.

**Q6: How do you ensure audit logs are tamper-proof?**
A: (1) Audit events are append-only in Elasticsearch (no update/delete API exposed). (2) Each event includes a SHA-256 hash of (previous_event_hash + current_event_data), creating a hash chain. (3) Periodic snapshots of ES indices are stored in immutable S3 storage (Object Lock). (4) Audit write access is limited to the Audit Service service account only.

**Q7: How do you handle multi-tenancy -- can Project A see Project B's resources?**
A: Strict tenant isolation: (1) Every API query includes a `project_id` filter derived from the user's JWT groups claim, not from user input. (2) Spring Security filters ensure users can only query projects they're members of. (3) MySQL views/row-level security for admin queries. (4) Elasticsearch queries include a `project_id` filter in all audit searches.

**Q8: What's your strategy for template versioning when you need to change a template that's in use?**
A: Templates are immutable once published (append-only versioning). A "change" creates a new version. Existing resources store the fully resolved spec (not a template reference), so they're unaffected. The template catalog shows the latest version; previous versions are accessible via API for audit purposes. We never modify a template in-place.

**Q9: How would you add support for a new infrastructure type (e.g., serverless functions)?**
A: The system is designed for extensibility: (1) Add a new `resource_type` enum value. (2) Create a new provisioner plugin (implements the `InfraProvisioner` interface). (3) Create templates for the new type. (4) Add quota resource types. (5) Register the provisioner in the Resource Service's provider registry. No changes to the workflow engine, quota service, or portal UI framework needed. The UI dynamically renders forms based on template JSON Schema.

**Q10: How do you handle the cost estimation for resources with variable pricing (e.g., spot instances)?**
A: We show three estimates: (1) On-demand price (guaranteed). (2) Spot/preemptible price (current, with disclaimer "may vary"). (3) Reserved price (if committed for a term). The cost formula includes a `pricing_model` parameter. For spot pricing, we query the current spot price from the provider API and cache it for 5 minutes.

**Q11: What happens when a resource's expiry date arrives?**
A: A cron job runs every 5 minutes to check `expires_at`. At 72h before expiry: warning notification. At 24h: final warning. At expiry: if no renewal, workflow transitions resource to `expiring` status (grace period of 24h). After grace period: `deprovisioning` -> actual teardown -> `terminated`. Users can renew at any point before termination.

**Q12: How do you handle the case where the cost estimation engine is wrong and a resource costs much more than estimated?**
A: We track `estimated_cost_per_hour` vs `actual_cost_per_hour` for every resource. A monitoring job flags resources where `actual > 1.2 * estimated` (20% threshold). When detected: (1) Alert the infra-finance team. (2) Notify the user. (3) Update the cost formula to prevent future discrepancies. We do NOT auto-terminate resources due to cost overruns -- that would be destructive.

**Q13: How do you test the portal end-to-end?**
A: (1) Unit tests: each service with mocked dependencies. (2) Integration tests: services + MySQL + Elasticsearch + RabbitMQ in Docker Compose. (3) Temporal workflow tests: mocked activities with time skipping. (4) E2E tests: Playwright against staging portal with a test OpenStack environment. (5) Load tests: k6 scripts simulating 1,000 concurrent users. (6) Chaos testing: inject failures (kill MySQL, introduce latency) and verify graceful degradation.

**Q14: How do you handle the scenario where a team's budget is exhausted mid-month?**
A: When `daily_spend * remaining_days > remaining_budget`, we: (1) Notify project admin and owner. (2) Block new provisioning requests for that project (soft enforcement). (3) Do NOT terminate existing resources (that would be destructive). (4) Admin can increase budget or the team can terminate unused resources. Configurable policy: some projects opt for hard enforcement (block new requests), others for soft (warn only).

**Q15: Why Spring Boot + Java instead of Go or Python for the backend?**
A: (1) Organizational standard -- the infra team has deep Java expertise. (2) Spring ecosystem provides: Spring Security (OIDC/SAML), Spring Data JPA (MySQL), Spring AMQP (RabbitMQ), Temporal Java SDK, Resilience4j. (3) JVM performance is excellent for this workload (not CPU-bound, mostly I/O). (4) Strong typing catches many bugs at compile time. (5) Go would also be a great choice for the provisioner components (lightweight, fast startup), and we use it for the CLI.

**Q16: How would you implement a "dry run" mode for provisioning?**
A: `POST /resources` accepts an optional `?dry_run=true` parameter. In dry-run mode: (1) Validate the request (schema, permissions). (2) Check quota (without reserving). (3) Calculate cost estimate. (4) Return the result without creating any records or triggering workflows. This is useful for CI/CD pipelines that want to validate infrastructure requests before committing.

---

## 16. References

1. **Temporal.io** - Durable execution framework: https://temporal.io/
2. **OpenStack Nova API** - VM lifecycle management: https://docs.openstack.org/api-ref/compute/
3. **Kubernetes ClusterAPI** - Declarative cluster lifecycle: https://cluster-api.sigs.k8s.io/
4. **Spring Security OAuth2** - OIDC/SAML integration: https://docs.spring.io/spring-security/reference/servlet/oauth2/
5. **Elasticsearch ILM** - Index lifecycle management for audit retention: https://www.elastic.co/guide/en/elasticsearch/reference/current/index-lifecycle-management.html
6. **RabbitMQ Reliability Guide** - Message durability and acknowledgment: https://www.rabbitmq.com/docs/reliability
7. **JSON Schema** - Template parameter validation: https://json-schema.org/
8. **Resilience4j** - Circuit breaker, rate limiter, retry: https://resilience4j.readme.io/
9. **NIST SP 800-92** - Guide to Computer Security Log Management (audit trail compliance)
10. **Google Borg** - Cluster management inspiration: https://research.google/pubs/pub43438/
