# System Design: Web Portal for IaaS

> **Relevance to role:** A cloud infrastructure platform engineer must design and build the web portal that serves as the primary interface for IaaS management. This involves full-stack concerns: React frontend, Spring Boot (Java) backend, BFF pattern, real-time updates (WebSocket/SSE), multi-tenancy, SSO/RBAC, and API design. The portal is how thousands of engineers interact with the infrastructure platform daily.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|-------------|--------|
| FR-1 | Dashboard | Resource utilization charts, cost breakdown, recent activity |
| FR-2 | Resource Management | CRUD for VMs, bare-metal, K8s clusters, storage |
| FR-3 | Real-Time Status | Live provisioning status updates via WebSocket/SSE |
| FR-4 | Cost Visibility | Per-project cost breakdown, budget tracking, trend analysis |
| FR-5 | Multi-Tenancy | Tenant (project) isolation at API and data layer |
| FR-6 | SSO Authentication | SAML 2.0 / OIDC integration with corporate IdP (Okta) |
| FR-7 | Fine-Grained RBAC | Per-resource permissions (viewer, developer, approver, admin) |
| FR-8 | Template Catalog | Browse, customize, and deploy infrastructure templates |
| FR-9 | Approval Workflows | Submit, review, approve/reject provisioning requests |
| FR-10 | Audit Log Viewer | Search and filter infrastructure audit events |
| FR-11 | Quota Management | View and manage project resource quotas |
| FR-12 | Responsive Design | Mobile-friendly for on-call engineers |
| FR-13 | Embedded Monitoring | Grafana dashboards or custom charts for resource metrics |
| FR-14 | Notification Center | In-app notifications + preferences for email/Slack |
| FR-15 | Search | Full-text search across resources, templates, audit logs |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Availability | 99.95% (portal); 99.99% (API) |
| NFR-2 | Page Load Time | Initial: < 3s; Subsequent (SPA navigation): < 500ms |
| NFR-3 | API Latency | P50: 100ms; P99: 500ms |
| NFR-4 | Concurrent Users | 1,000 simultaneous portal users |
| NFR-5 | WebSocket Connections | 1,000 concurrent connections |
| NFR-6 | Mobile Support | Responsive breakpoints: 320px - 2560px |
| NFR-7 | Accessibility | WCAG 2.1 AA compliance |
| NFR-8 | Browser Support | Chrome, Firefox, Safari, Edge (last 2 versions) |

### Constraints & Assumptions
- React 18+ (TypeScript) for frontend.
- Spring Boot 3.x (Java 21) for backend.
- BFF (Backend-for-Frontend) pattern: portal-specific API layer.
- MySQL 8.0 as primary database.
- Elasticsearch for search and audit logs.
- Redis for session caching and real-time pub/sub.
- RabbitMQ for async event processing.
- CDN (CloudFront) for static assets.
- Corporate IdP: Okta (OIDC + SAML).

### Out of Scope
- Infrastructure provisioning logic (handled by backend services; portal is the UI layer).
- CI/CD pipeline management.
- Network configuration UI.
- IaC editor (covered in infrastructure_as_code_platform.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Total registered users | 10,000 engineers | 10,000 |
| Daily active users (DAU) | 30% of total | 3,000 |
| Peak concurrent users | 10% of DAU | 300 |
| Max concurrent users (peak event) | 3x normal peak | 1,000 |
| Page views per user per day | 20 pages | 20 |
| Total page views/day | 3,000 x 20 | 60,000 |
| API calls per page view | 5 avg (list, detail, quota, user, notifications) | 5 |
| Total API calls/day | 60,000 x 5 | 300,000 |
| API calls/sec (average) | 300,000 / 86,400 | ~3.5 RPS |
| API calls/sec (peak) | 3.5 x 5 (peak factor) | ~17 RPS |
| WebSocket connections (peak) | 300 (active users with live dashboards) | 300 |
| WebSocket messages/sec | 300 connections x 1 msg/5s avg | ~60 msg/s |

### Latency Requirements
| Operation | P50 | P99 |
|-----------|-----|-----|
| Initial page load (cold, uncached) | 1.5s | 3s |
| SPA page navigation (cached) | 200ms | 500ms |
| API call (list/get) | 50ms | 300ms |
| API call (create/update) | 100ms | 500ms |
| WebSocket event delivery | 50ms | 200ms |
| Dashboard chart render | 500ms | 2s |
| Full-text search | 200ms | 1s |

### Storage Estimates

| Data | Calculation | Size |
|------|-------------|------|
| Frontend bundle (JS + CSS) | React app, compressed | ~500KB gzipped |
| Static assets (images, fonts) | Icons, logos | ~2MB |
| API response cache (Redis) | 1,000 users x 20KB session | ~20MB |
| WebSocket state (Redis) | 300 connections x 1KB | ~300KB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Frontend initial load | 500KB x 300 peak users (CDN-served) | ~150MB burst (CDN handles) |
| API traffic (peak) | 17 RPS x 5KB avg | ~85 KB/s |
| WebSocket traffic | 60 msg/s x 200B avg | ~12 KB/s |
| Dashboard charts data | 300 users x 50KB chart data (cached) | ~15MB burst (cached) |

---

## 3. High Level Architecture

```
+-------------------+        +--------------------+
|    CDN             |        |    Corporate IdP   |
|  (CloudFront)      |        |    (Okta)          |
|  Static JS/CSS/img |        |    OIDC + SAML     |
+--------+----------+        +---------+----------+
         |                              |
         |        +---------------------+
         |        |
+--------v--------v-----------+
|     Load Balancer (HAProxy) |
|     TLS Termination         |
|     /static -> CDN          |
|     /api    -> BFF          |
|     /ws     -> WebSocket    |
+--------+----------+---------+
         |          |
    +----v----+ +---v---------+
    |  React  | |  WebSocket  |
    |  SPA    | |  Gateway    |
    | (served | |  (Spring    |
    |  by CDN)| |  WebSocket) |
    +---------+ +------+------+
                       |
              +--------v--------+
              |   BFF Layer     |
              |  (Spring Boot)  |
              |                 |
              | - Auth Filter   |
              | - Rate Limiter  |
              | - Response      |
              |   Aggregation   |
              | - Error Mapping |
              +--------+--------+
                       |
       +-------+-------+-------+-------+
       |       |       |       |       |
  +----v--+ +--v---+ +-v----+ +v----+ +v--------+
  |Resource| |Quota | |Audit | |Cost | |Template |
  |Service | |Svc   | |Svc   | |Svc  | |Svc     |
  |(Java)  | |(Java)| |(Java)| |(Java)| |(Java)  |
  +---+----+ +--+---+ +--+---+ +--+--+ +---+----+
      |         |         |        |        |
  +---v---------v---------v--------v--------v---+
  |              MySQL 8.0 Primary               |
  |              + Read Replicas                 |
  +---+------------------------------------------+
      |
  +---v-----------+     +-------------------+
  | Elasticsearch  |     | Redis Cluster     |
  | (Audit + Search)|     | (Session + Cache  |
  +----------------+     |  + Pub/Sub)       |
                         +-------------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **CDN (CloudFront)** | Serves React SPA bundle (JS, CSS, images, fonts). Edge caching for global performance. |
| **Load Balancer (HAProxy)** | TLS termination, L7 routing (/api -> BFF, /ws -> WebSocket), health checks, sticky sessions for WebSocket |
| **React SPA** | Single-page application; dashboard, resource management, template catalog, approval workflows, audit viewer |
| **WebSocket Gateway** | Spring WebSocket server; pushes real-time provisioning status, notifications, dashboard updates to connected clients |
| **BFF Layer** | Backend-for-Frontend; portal-specific API aggregation, auth token validation, response shaping (hides backend complexity from frontend), rate limiting |
| **Resource Service** | CRUD for VMs, bare-metal, K8s clusters, storage; delegates to provisioners |
| **Quota Service** | Quota enforcement and reporting |
| **Audit Service** | Writes and queries audit events (Elasticsearch) |
| **Cost Service** | Cost estimation, tracking, and reporting |
| **Template Service** | Template catalog CRUD |
| **MySQL 8.0** | Source of truth for resources, users, projects, quotas |
| **Elasticsearch** | Audit log storage, full-text search across resources and templates |
| **Redis** | Session cache, real-time pub/sub (WebSocket fan-out), API response cache, rate limiting counters |

### Data Flows

**Page Load Flow:**
1. User navigates to `https://portal.infra.company.com`.
2. CDN serves `index.html` + JS bundle (React SPA).
3. React app initializes; checks for existing session (JWT in httpOnly cookie).
4. If no session: redirect to Okta OIDC `/authorize` endpoint.
5. On successful auth: Okta redirects back with authorization code.
6. BFF exchanges code for tokens (server-side), creates session, returns session cookie.
7. React app loads dashboard; fetches data from BFF APIs.

**Real-Time Update Flow:**
1. User is viewing resource detail page for a provisioning VM.
2. React app opens WebSocket connection to `/ws/resources/{uid}`.
3. WebSocket Gateway subscribes to Redis pub/sub channel `resource:{uid}`.
4. Backend provisioning completes; Resource Service publishes status update to RabbitMQ.
5. Event consumer writes to MySQL and publishes to Redis pub/sub channel.
6. WebSocket Gateway receives message, pushes to connected client.
7. React app updates UI in real-time (status: "provisioning" -> "active").

---

## 4. Data Model

### Core Entities & Schema

The portal shares the same MySQL schema as the self-service portal (see `developer_self_service_portal.md`). Additional portal-specific tables:

```sql
-- User sessions (server-side session storage, backed by Redis)
-- Redis key: session:{session_id}
-- Redis value (JSON):
-- {
--   "user_id": 1,
--   "email": "alice@company.com",
--   "roles": {"ml-team": "developer", "platform": "admin"},
--   "access_token": "eyJhbG...",
--   "refresh_token": "dGhpcyB...",
--   "token_expires_at": "2026-04-09T20:00:00Z",
--   "created_at": "2026-04-09T10:00:00Z",
--   "last_activity": "2026-04-09T10:30:00Z"
-- }
-- TTL: 8 hours (session max lifetime)

-- User preferences (portal settings)
CREATE TABLE user_preferences (
    user_id         BIGINT PRIMARY KEY REFERENCES users(id),
    theme           ENUM('light','dark','system') NOT NULL DEFAULT 'system',
    default_project BIGINT REFERENCES projects(id),
    dashboard_layout JSON,        -- widget positions and sizes
    notification_prefs JSON,      -- { "email": true, "slack": true, "portal": true }
    timezone        VARCHAR(50) DEFAULT 'UTC',
    locale          VARCHAR(10) DEFAULT 'en-US',
    updated_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- In-app notifications
CREATE TABLE portal_notifications (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id         BIGINT NOT NULL REFERENCES users(id),
    type            VARCHAR(50) NOT NULL,          -- 'provision_complete', 'approval_needed', etc.
    title           VARCHAR(255) NOT NULL,
    body            TEXT,
    resource_uid    VARCHAR(64),
    link            VARCHAR(500),                  -- deep link to relevant page
    is_read         BOOLEAN NOT NULL DEFAULT FALSE,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_pn_user_unread (user_id, is_read, created_at),
    INDEX idx_pn_user_recent (user_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Dashboard widgets (customizable dashboard)
CREATE TABLE dashboard_widgets (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id         BIGINT NOT NULL REFERENCES users(id),
    widget_type     VARCHAR(50) NOT NULL,          -- 'resource_count', 'cost_chart', 'recent_activity', etc.
    position_x      INT NOT NULL,
    position_y      INT NOT NULL,
    width           INT NOT NULL DEFAULT 4,        -- grid units
    height          INT NOT NULL DEFAULT 3,
    config_json     JSON,                          -- widget-specific config (filters, time range, etc.)
    INDEX idx_dw_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Cost tracking (daily snapshots)
CREATE TABLE daily_cost_snapshots (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    project_id      BIGINT NOT NULL REFERENCES projects(id),
    date            DATE NOT NULL,
    resource_type   VARCHAR(50) NOT NULL,
    resource_count  INT NOT NULL,
    total_cost_cents BIGINT NOT NULL,
    breakdown_json  JSON,                          -- per-resource costs
    UNIQUE KEY uk_cost_snapshot (project_id, date, resource_type),
    INDEX idx_cost_project_date (project_id, date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Saved searches / filters
CREATE TABLE saved_filters (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id         BIGINT NOT NULL REFERENCES users(id),
    name            VARCHAR(255) NOT NULL,
    entity_type     VARCHAR(50) NOT NULL,          -- 'resources', 'audit', 'templates'
    filter_json     JSON NOT NULL,                 -- saved filter parameters
    is_default      BOOLEAN NOT NULL DEFAULT FALSE,
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_sf_user (user_id, entity_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Store | Use Case | Justification |
|-------|----------|---------------|
| MySQL 8.0 | Resources, users, projects, quotas, notifications, preferences | Transactional consistency; relational queries for dashboard aggregation |
| Elasticsearch 8.x | Audit logs, full-text search, resource search | Full-text search with facets; time-series audit queries; 7-year retention |
| Redis 6.x | Sessions, API cache, WebSocket pub/sub, rate limiting | Sub-ms session lookups; pub/sub for real-time updates; sliding window rate limiting |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| portal_notifications | (user_id, is_read, created_at) | Unread notifications badge count |
| portal_notifications | (user_id, created_at) | Notification feed |
| daily_cost_snapshots | (project_id, date) | Cost dashboard queries |
| saved_filters | (user_id, entity_type) | Load user's saved filters |
| Elasticsearch | resource_uid + resource_type + status | Resource search facets |
| Elasticsearch | timestamp + project_name + actor_email | Audit log queries |

---

## 5. API Design

### REST Endpoints (BFF Layer)

**Base URL:** `https://portal.infra.company.com/api/v1`

**Authentication:** Session cookie (HttpOnly, Secure, SameSite=Strict). BFF validates session against Redis. JWT access token stored server-side in session (not exposed to browser).

**Rate Limiting:** 200 requests/min per user (read), 30 requests/min per user (write). Enforced via Redis sliding window.

**Standard Response Envelope:**
```json
{
  "data": { ... },
  "meta": {
    "request_id": "req-abc123",
    "timestamp": "2026-04-09T10:00:00Z",
    "pagination": { "page": 1, "size": 50, "total": 156 }
  },
  "errors": []
}
```

#### Authentication

```
GET /auth/login
  Redirects to Okta OIDC /authorize with:
    response_type=code
    client_id=<portal_client_id>
    redirect_uri=https://portal.infra.company.com/api/v1/auth/callback
    scope=openid profile email groups
    state=<CSRF token>
    nonce=<nonce>

GET /auth/callback?code=<auth_code>&state=<csrf>
  BFF exchanges code for tokens:
    POST https://company.okta.com/oauth2/default/v1/token
    Body: grant_type=authorization_code, code=<auth_code>, redirect_uri=<...>
  BFF validates id_token (signature, issuer, audience, nonce)
  BFF creates server-side session in Redis
  Sets HttpOnly cookie: Set-Cookie: SESSION=<session_id>; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=28800
  Redirects to: https://portal.infra.company.com/dashboard

GET /auth/me
  Response 200:
  {
    "data": {
      "user_id": 1,
      "email": "alice@company.com",
      "display_name": "Alice Smith",
      "department": "ML Platform",
      "roles": {
        "ml-team": "developer",
        "platform-team": "admin"
      },
      "preferences": {
        "theme": "dark",
        "default_project": "ml-team",
        "timezone": "America/New_York"
      }
    }
  }

POST /auth/logout
  BFF invalidates session in Redis
  Clears session cookie
  Redirects to Okta /logout

POST /auth/refresh
  BFF uses stored refresh_token to get new access_token from Okta
  Updates session in Redis
  Response 200: { "success": true }
```

#### Dashboard

```
GET /dashboard/summary?project=ml-team
  Response 200:
  {
    "data": {
      "resources": {
        "total": 47,
        "by_status": { "active": 42, "provisioning": 2, "expiring": 3 },
        "by_type": { "vm": 30, "bare_metal": 5, "k8s_cluster": 2, "storage": 10 }
      },
      "costs": {
        "current_month": 12500.00,
        "budget": 20000.00,
        "utilization_pct": 62.5,
        "trend": "increasing",
        "daily_avg": 416.67
      },
      "quotas": {
        "most_utilized": [
          { "type": "gpu", "used_pct": 75, "used": 15, "limit": 20 },
          { "type": "vcpu", "used_pct": 60, "used": 192, "limit": 320 }
        ]
      },
      "recent_activity": [
        {
          "event": "resource.created",
          "resource_uid": "vm-abc123",
          "resource_name": "training-vm-01",
          "actor": "alice@company.com",
          "timestamp": "2026-04-09T09:45:00Z"
        }
      ],
      "pending_approvals": 3,
      "expiring_resources": 2
    }
  }

GET /dashboard/cost-chart?project=ml-team&period=30d&granularity=daily
  Response 200:
  {
    "data": {
      "labels": ["2026-03-10", "2026-03-11", ...],
      "datasets": [
        { "label": "VM", "data": [150.00, 155.00, ...] },
        { "label": "Bare Metal", "data": [200.00, 200.00, ...] },
        { "label": "K8s", "data": [50.00, 55.00, ...] },
        { "label": "Storage", "data": [10.00, 10.00, ...] }
      ],
      "total": 12500.00,
      "budget": 20000.00
    }
  }

GET /dashboard/utilization?project=ml-team
  Response 200:
  {
    "data": {
      "cpu": { "allocated": 192, "used_avg": 120, "capacity": 320 },
      "memory_gb": { "allocated": 768, "used_avg": 500, "capacity": 1024 },
      "gpu": { "allocated": 15, "used_avg": 12, "capacity": 20 },
      "storage_tb": { "allocated": 5.0, "used_avg": 3.2, "capacity": 10.0 }
    }
  }
```

#### Resources

```
GET /resources?project=ml-team&type=vm&status=active&search=training&page=1&size=50&sort=created_at:desc
  Response 200:
  {
    "data": {
      "items": [
        {
          "resource_uid": "vm-abc123",
          "name": "training-vm-01",
          "resource_type": "vm",
          "status": "active",
          "spec_summary": "16 vCPU, 64GB RAM, 4x H100",
          "ip_addresses": ["10.0.5.42"],
          "cost_per_hour": 12.50,
          "created_by": { "email": "alice@company.com", "name": "Alice Smith" },
          "project": { "id": 10, "name": "ml-team" },
          "provisioned_at": "2026-04-08T10:30:00Z",
          "expires_at": "2026-04-15T00:00:00Z",
          "health": "healthy"
        }
      ]
    },
    "meta": {
      "pagination": { "page": 1, "size": 50, "total": 23 },
      "facets": {
        "status": { "active": 20, "expiring": 2, "provisioning": 1 },
        "type": { "vm": 15, "bare_metal": 5, "k8s_cluster": 3 }
      }
    }
  }

GET /resources/{uid}
  Response 200:
  {
    "data": {
      "resource_uid": "vm-abc123",
      "name": "training-vm-01",
      "resource_type": "vm",
      "status": "active",
      "spec": {
        "vcpu": 16,
        "memory_gb": 64,
        "disk_gb": 500,
        "gpu": { "type": "nvidia-h100", "count": 4 },
        "image": "ubuntu-22.04-cuda-12.2",
        "ssh_keys": ["ssh-ed25519 AAAA..."]
      },
      "ip_addresses": ["10.0.5.42"],
      "cost_per_hour": 12.50,
      "total_cost_to_date": 450.00,
      "created_by": { "id": 1, "email": "alice@company.com", "name": "Alice Smith" },
      "project": { "id": 10, "name": "ml-team" },
      "template": { "id": 42, "name": "ML Training Rig (GPU)" },
      "provisioned_at": "2026-04-08T10:30:00Z",
      "expires_at": "2026-04-15T00:00:00Z",
      "health": {
        "status": "healthy",
        "last_check": "2026-04-09T10:00:00Z",
        "checks": [
          { "name": "connectivity", "status": "pass" },
          { "name": "disk_usage", "status": "pass", "detail": "45% used" },
          { "name": "gpu_health", "status": "pass", "detail": "4/4 GPUs healthy" }
        ]
      },
      "timeline": [
        { "event": "requested", "timestamp": "2026-04-08T10:00:00Z", "actor": "alice@company.com" },
        { "event": "approved", "timestamp": "2026-04-08T10:15:00Z", "actor": "manager@company.com" },
        { "event": "provisioning_started", "timestamp": "2026-04-08T10:15:05Z" },
        { "event": "active", "timestamp": "2026-04-08T10:30:00Z" }
      ]
    }
  }

POST /resources
  (Same as developer_self_service_portal.md)

DELETE /resources/{uid}
  Response 202:
  {
    "data": {
      "resource_uid": "vm-abc123",
      "status": "deprovisioning",
      "message": "Resource termination initiated. You will be notified when complete."
    }
  }
```

#### Notifications (In-App)

```
GET /notifications?unread_only=true&page=1&size=20
  Response 200:
  {
    "data": {
      "items": [
        {
          "id": 456,
          "type": "provision_complete",
          "title": "VM training-vm-01 is ready",
          "body": "Your VM has been provisioned. IP: 10.0.5.42",
          "resource_uid": "vm-abc123",
          "link": "/resources/vm-abc123",
          "is_read": false,
          "created_at": "2026-04-09T10:30:00Z"
        },
        {
          "id": 455,
          "type": "approval_needed",
          "title": "Approval requested: GPU reservation by Bob",
          "body": "Bob requests 8x H100 servers for project ml-team ($320/hr)",
          "link": "/approvals/789",
          "is_read": false,
          "created_at": "2026-04-09T10:00:00Z"
        }
      ],
      "unread_count": 5
    }
  }

POST /notifications/{id}/read
  Response 200: { "success": true }

POST /notifications/read-all
  Response 200: { "success": true, "marked_read": 5 }
```

#### Search

```
GET /search?q=training+gpu&type=resources,templates&page=1&size=20
  Response 200:
  {
    "data": {
      "items": [
        {
          "entity_type": "resource",
          "resource_uid": "vm-abc123",
          "name": "training-vm-01",
          "resource_type": "vm",
          "status": "active",
          "project": "ml-team",
          "highlight": "ML <em>training</em> VM with <em>GPU</em> acceleration",
          "score": 0.95
        },
        {
          "entity_type": "template",
          "template_id": 42,
          "name": "ML Training Rig (GPU)",
          "category": "vm",
          "highlight": "Pre-configured <em>training</em> environment with <em>GPU</em>s",
          "score": 0.88
        }
      ],
      "total": 7,
      "facets": {
        "entity_type": { "resource": 5, "template": 2 }
      }
    }
  }
```

### WebSocket API

```
Connection: wss://portal.infra.company.com/ws
Authentication: Session cookie sent during WebSocket handshake

# Subscribe to resource status updates
Client -> Server:
{
  "type": "subscribe",
  "channel": "resource:vm-abc123"
}

# Server pushes status update
Server -> Client:
{
  "type": "resource_status",
  "channel": "resource:vm-abc123",
  "data": {
    "resource_uid": "vm-abc123",
    "status": "active",
    "message": "Provisioning complete",
    "ip_addresses": ["10.0.5.42"],
    "timestamp": "2026-04-09T10:30:00Z"
  }
}

# Subscribe to project activity feed
Client -> Server:
{
  "type": "subscribe",
  "channel": "project:ml-team:activity"
}

# Server pushes activity
Server -> Client:
{
  "type": "activity",
  "channel": "project:ml-team:activity",
  "data": {
    "event": "resource.created",
    "actor": "bob@company.com",
    "resource_uid": "vm-def456",
    "resource_name": "eval-vm-02",
    "timestamp": "2026-04-09T10:35:00Z"
  }
}

# Subscribe to notifications
Client -> Server:
{
  "type": "subscribe",
  "channel": "user:notifications"
}

# Server pushes notification
Server -> Client:
{
  "type": "notification",
  "channel": "user:notifications",
  "data": {
    "id": 457,
    "type": "resource_expiring",
    "title": "VM training-vm-01 expires in 24 hours",
    "link": "/resources/vm-abc123"
  }
}

# Heartbeat
Client -> Server (every 30s):
{ "type": "ping" }

Server -> Client:
{ "type": "pong" }
```

### CLI Design

The web portal's API is also consumed by `infra-cli`. See `cli_client_for_infra_platform.md` for complete CLI command reference.

---

## 6. Core Component Deep Dives

### 6.1 BFF (Backend-for-Frontend) Pattern

**Why it's hard:** The portal frontend needs data shaped differently from backend microservices. A list page might need data from 3 services (resources + quotas + cost). Without a BFF, the frontend makes multiple round-trips, increasing latency and complexity. The BFF must aggregate, transform, and cache without becoming a monolithic bottleneck.

| Approach | Pros | Cons |
|----------|------|------|
| **Direct frontend-to-microservices** | No BFF layer to maintain | N+1 API calls; auth complexity in frontend; tight coupling |
| **API Gateway (generic)** | Single entry point; rate limiting | No business logic; can't aggregate |
| **GraphQL** | Flexible queries; single round-trip | Complex server-side; caching harder; N+1 resolver problem |
| **BFF (dedicated backend per frontend)** | Frontend-optimized APIs; aggregation; server-side auth | Extra service to maintain; potential for BFF bloat |

**Selected approach: BFF (Backend-for-Frontend) with Spring Boot.**

**Justification:** The portal has unique needs: dashboard aggregation (data from 5 services), server-side session management (token security), and response shaping (frontend-friendly formats with facets, highlights). A BFF provides all this without GraphQL's complexity. We already use Spring Boot, so the BFF is consistent with our stack.

**Implementation:**

```java
@RestController
@RequestMapping("/api/v1/dashboard")
public class DashboardBFFController {

    private final ResourceServiceClient resourceClient;
    private final QuotaServiceClient quotaClient;
    private final CostServiceClient costClient;
    private final AuditServiceClient auditClient;

    /**
     * Dashboard summary: aggregates data from 4 services in parallel.
     */
    @GetMapping("/summary")
    public DashboardSummary getSummary(@RequestParam String project,
                                       @AuthUser UserContext user) {
        // Verify user has access to this project
        authz.checkProjectAccess(user, project, Role.VIEWER);

        // Parallel calls to backend services
        CompletableFuture<ResourceSummary> resourcesFuture =
            CompletableFuture.supplyAsync(() -> resourceClient.getSummary(project));
        CompletableFuture<CostSummary> costsFuture =
            CompletableFuture.supplyAsync(() -> costClient.getMonthlySummary(project));
        CompletableFuture<List<QuotaUsage>> quotasFuture =
            CompletableFuture.supplyAsync(() -> quotaClient.getTopUtilized(project, 5));
        CompletableFuture<List<AuditEvent>> activityFuture =
            CompletableFuture.supplyAsync(() -> auditClient.getRecent(project, 10));

        // Wait for all with timeout
        CompletableFuture.allOf(resourcesFuture, costsFuture, quotasFuture, activityFuture)
            .orTimeout(5, TimeUnit.SECONDS)
            .join();

        // Aggregate
        return DashboardSummary.builder()
            .resources(resourcesFuture.get())
            .costs(costsFuture.get())
            .quotas(quotasFuture.get())
            .recentActivity(activityFuture.get())
            .pendingApprovals(resourceClient.countPendingApprovals(user, project))
            .build();
    }
}

/**
 * BFF-level response caching.
 * Caches aggregated responses in Redis with short TTL.
 */
@Component
public class BFFCacheInterceptor implements HandlerInterceptor {

    @Override
    public boolean preHandle(HttpServletRequest request, HttpServletResponse response,
                            Object handler) {
        if (!"GET".equals(request.getMethod())) return true;

        String cacheKey = buildCacheKey(request);
        String cached = redis.get(cacheKey);
        if (cached != null) {
            response.setContentType("application/json");
            response.getWriter().write(cached);
            response.setHeader("X-Cache", "HIT");
            return false; // Short-circuit
        }
        return true;
    }

    private String buildCacheKey(HttpServletRequest request) {
        // Cache key: user_id + path + query params (project scoped)
        String userId = SecurityContextHolder.getContext().getAuthentication().getName();
        return String.format("bff:%s:%s:%s", userId, request.getRequestURI(), request.getQueryString());
    }
}
```

**BFF responsibilities:**
1. **Authentication:** Validate session cookie, extract user context.
2. **Authorization:** Check project membership and role before forwarding.
3. **Aggregation:** Combine data from multiple backend services into single response.
4. **Transformation:** Shape responses for frontend needs (e.g., add facets, highlights, summaries).
5. **Caching:** Short-lived cache (30s-60s) for dashboard data.
6. **Error handling:** Translate backend errors into user-friendly messages.
7. **Rate limiting:** Per-user rate limits via Redis sliding window.

**Failure modes:**
- **Backend service down:** BFF returns partial data with a `degraded: true` flag. Dashboard shows available widgets; unavailable widgets show "data unavailable" placeholder.
- **BFF overloaded:** Auto-scale BFF pods (HPA on CPU/memory). Circuit breaker prevents cascade to backend services.
- **Cache stampede:** When cache expires, many concurrent requests hit backend. Mitigation: probabilistic early expiration (jitter on TTL) + single-flight pattern (only one request refreshes cache, others wait).

**Interviewer Q&As:**

**Q1: Why a BFF instead of GraphQL?**
A: (1) Our frontend is a single SPA, not multiple clients (mobile, TV, etc.) that GraphQL serves well. (2) The BFF can implement server-side caching trivially (cache the aggregated response). GraphQL caching is harder (each query is unique). (3) The BFF keeps auth tokens server-side (session cookie) rather than exposing JWTs to the browser. (4) The BFF can implement rate limiting per user more easily.

**Q2: How do you prevent the BFF from becoming a monolithic bottleneck?**
A: (1) BFF is stateless: horizontal scaling via pod replicas. (2) All backend calls are async (CompletableFuture) with timeouts. (3) Circuit breakers prevent waiting on slow backends. (4) BFF caches frequently-accessed data (30s TTL). (5) We monitor BFF latency per endpoint; any endpoint exceeding P99 > 500ms is investigated.

**Q3: How do you handle partial failures in aggregated responses?**
A: Each backend call is independent (parallel CompletableFuture). If the cost service is down but resources and quotas are up, the dashboard returns resources and quotas with `costs: null` and `degraded_services: ["cost"]`. The frontend renders available data and shows a banner: "Some data is temporarily unavailable."

**Q4: How do you keep the BFF in sync with backend API changes?**
A: (1) Backend services publish OpenAPI specs. (2) BFF auto-generates client code from OpenAPI specs (feign clients). (3) Contract tests verify BFF expectations match backend responses. (4) Backward-compatible API changes only (additive fields).

**Q5: How does the BFF handle file uploads (e.g., SSH key upload)?**
A: The BFF proxies file uploads to the backend service. For large files (e.g., OS images), the BFF returns a pre-signed S3 upload URL; the frontend uploads directly to S3, then notifies the BFF.

**Q6: How do you test the BFF layer?**
A: (1) Unit tests with WireMock (mock backend services). (2) Integration tests with embedded backend stubs. (3) Contract tests (Pact) ensuring BFF ↔ backend API compatibility. (4) E2E tests with Playwright against the full portal stack.

---

### 6.2 Real-Time Updates (WebSocket)

**Why it's hard:** Provisioning operations take 30 seconds to 15 minutes. Users expect live status updates without refreshing. The system must handle 1,000 concurrent WebSocket connections, fan out events to the right subscribers, handle disconnections and reconnections, and authenticate WebSocket connections.

| Approach | Pros | Cons |
|----------|------|------|
| **Polling (short)** | Simplest; works everywhere | High server load; latency = poll interval; wasteful |
| **Long polling** | Lower latency than short polling | Complex connection management; still one-directional |
| **Server-Sent Events (SSE)** | Simple; built-in reconnection; HTTP-based | Unidirectional; limited concurrent connections per domain |
| **WebSocket** | Bidirectional; low latency; efficient | More complex; requires sticky sessions or external pub/sub |

**Selected approach: WebSocket with Redis pub/sub for fan-out.**

**Justification:** WebSocket provides the lowest latency and supports bidirectional communication (client can subscribe/unsubscribe to specific channels). Redis pub/sub enables horizontal scaling of WebSocket servers (any server can deliver any event). The 1,000 connection target is well within WebSocket capacity.

**Implementation:**

```java
@Configuration
@EnableWebSocketMessageBroker
public class WebSocketConfig implements WebSocketMessageBrokerConfigurer {

    @Override
    public void configureMessageBroker(MessageBrokerRegistry config) {
        // Use Redis as message broker for multi-instance pub/sub
        config.enableStompBrokerRelay("/topic")
              .setRelayHost(redisHost)
              .setRelayPort(redisPort);
        config.setApplicationDestinationPrefixes("/app");
    }

    @Override
    public void registerStompEndpoints(StompEndpointRegistry registry) {
        registry.addEndpoint("/ws")
                .setAllowedOrigins("https://portal.infra.company.com")
                .withSockJS();  // Fallback for older browsers
    }
}

@Component
public class WebSocketAuthInterceptor implements ChannelInterceptor {

    @Override
    public Message<?> preSend(Message<?> message, MessageChannel channel) {
        StompHeaderAccessor accessor = StompHeaderAccessor.wrap(message);
        if (StompCommand.CONNECT.equals(accessor.getCommand())) {
            // Validate session cookie from handshake
            String sessionId = extractSessionId(accessor);
            UserSession session = sessionManager.validate(sessionId);
            if (session == null) {
                throw new AuthenticationException("Invalid session");
            }
            accessor.setUser(new WebSocketUser(session));
        }
        return message;
    }
}

@Controller
public class ResourceStatusWebSocketController {

    /**
     * Client subscribes to resource status updates.
     * Channel: /topic/resource/{uid}
     */
    @SubscribeMapping("/resource/{uid}")
    public void subscribeToResource(@DestinationVariable String uid,
                                     SimpMessageHeaderAccessor accessor) {
        UserContext user = (UserContext) accessor.getUser();
        // Verify user has access to this resource's project
        authz.checkResourceAccess(user, uid, Role.VIEWER);
    }
}

/**
 * Event consumer: listens to RabbitMQ events and pushes to WebSocket clients via Redis pub/sub.
 */
@Component
public class ResourceEventConsumer {

    private final SimpMessagingTemplate messagingTemplate;

    @RabbitListener(queues = "resource.status.updates")
    public void handleResourceUpdate(ResourceStatusEvent event) {
        // Push to all WebSocket subscribers of this resource
        messagingTemplate.convertAndSend(
            "/topic/resource/" + event.getResourceUid(),
            new ResourceStatusMessage(
                event.getResourceUid(),
                event.getStatus(),
                event.getMessage(),
                event.getIpAddresses(),
                Instant.now()
            )
        );

        // Also push to project activity feed
        messagingTemplate.convertAndSend(
            "/topic/project/" + event.getProjectName() + "/activity",
            new ActivityMessage(event)
        );
    }

    @RabbitListener(queues = "user.notifications")
    public void handleNotification(NotificationEvent event) {
        // Push to specific user's notification channel
        messagingTemplate.convertAndSendToUser(
            event.getUserEmail(),
            "/queue/notifications",
            new NotificationMessage(event)
        );
    }
}
```

**Frontend WebSocket client:**

```typescript
// React hook for WebSocket resource subscription
function useResourceStatus(resourceUid: string) {
    const [status, setStatus] = useState<ResourceStatus | null>(null);
    const stompClient = useStompClient();

    useEffect(() => {
        if (!stompClient) return;

        const subscription = stompClient.subscribe(
            `/topic/resource/${resourceUid}`,
            (message) => {
                const update = JSON.parse(message.body);
                setStatus(update);
            }
        );

        return () => subscription.unsubscribe();
    }, [stompClient, resourceUid]);

    return status;
}

// Connection management with automatic reconnection
function useStompClient() {
    const [client, setClient] = useState<Client | null>(null);

    useEffect(() => {
        const stompClient = new Client({
            brokerURL: `wss://${window.location.host}/ws`,
            reconnectDelay: 5000,      // 5 second reconnect delay
            heartbeatIncoming: 10000,  // Server ping every 10s
            heartbeatOutgoing: 10000,  // Client ping every 10s
            onConnect: () => {
                console.log('WebSocket connected');
            },
            onDisconnect: () => {
                console.log('WebSocket disconnected, reconnecting...');
            },
            onStompError: (frame) => {
                console.error('STOMP error:', frame.headers.message);
            }
        });

        stompClient.activate();
        setClient(stompClient);

        return () => stompClient.deactivate();
    }, []);

    return client;
}
```

**Failure modes:**
- **WebSocket server crash:** Client auto-reconnects (SockJS fallback). Subscription state is lost on the server; client re-subscribes on reconnect.
- **Redis pub/sub down:** WebSocket events fail to fan out. Mitigation: client falls back to polling (5-second interval) when no WebSocket updates received for 30 seconds.
- **Stale connections:** Server-side heartbeat (every 10s). If client doesn't respond to 3 consecutive pings, connection is closed. Client will auto-reconnect.
- **Memory leak from abandoned subscriptions:** Server tracks subscriptions per connection. When connection closes, all subscriptions are cleaned up. TTL on Redis subscription metadata (1 hour).

**Interviewer Q&As:**

**Q1: Why WebSocket instead of SSE for this use case?**
A: (1) Bidirectional: the client needs to send subscribe/unsubscribe messages. SSE is server-to-client only. (2) Multiple channels: a user might subscribe to resource status + project activity + notifications simultaneously. With SSE, each would need a separate HTTP connection (browser limit: 6 per domain). WebSocket multiplexes over a single connection. (3) That said, SSE would work for simpler push-only scenarios.

**Q2: How do you scale WebSocket servers horizontally?**
A: Redis pub/sub decouples event producers from WebSocket servers. Any WebSocket server can deliver any event because they all subscribe to the same Redis channels. The load balancer uses sticky sessions (IP hash or cookie-based) for WebSocket connections. Adding more WebSocket pods handles more concurrent connections.

**Q3: What's the maximum number of WebSocket connections per server?**
A: A single Spring WebSocket server on a 4-core pod with 4GB RAM can handle ~10,000 concurrent connections (WebSocket connections are lightweight: ~10KB each). Our 1,000 concurrent connections target needs only 1-2 pods. We set the limit conservatively at 2,000 per pod.

**Q4: How do you handle authentication for WebSocket connections?**
A: The initial WebSocket handshake includes the session cookie (same-origin, SameSite=Strict). The server validates the session against Redis during the STOMP CONNECT phase. If the session is invalid or expired, the connection is rejected with a 401 frame. If the session expires during an active connection, the next heartbeat triggers a session refresh.

**Q5: How do you prevent a user from subscribing to resources they shouldn't see?**
A: The `@SubscribeMapping` handler checks authorization: it verifies the user is a member of the resource's project before allowing the subscription. If unauthorized, the subscription is rejected with an error frame.

**Q6: What happens when the backend publishes an event but no one is subscribed?**
A: The event is published to Redis pub/sub regardless. If no WebSocket server has subscribers for that channel, the event is silently dropped. This is acceptable because events are informational (the actual state is in the database, not in events). Events are not durably queued for WebSocket delivery.

---

### 6.3 Frontend Architecture & Performance

**Why it's hard:** The portal is a complex SPA with many views (dashboard, resource list, detail pages, template catalog, approval workflows, audit logs). It must load fast (< 3s initial), feel responsive (< 500ms navigation), work on mobile, and handle real-time updates without unnecessary re-renders.

| Approach | Pros | Cons |
|----------|------|------|
| **Server-side rendering (SSR) - Next.js** | Fast initial load; SEO | Heavier server; complexity; SSR/hydration bugs |
| **Static site generation (SSG)** | Fastest initial load | Not suitable for dynamic data |
| **SPA with code splitting** | Simple deployment; fast navigation; CDN-served | Slower initial load (large bundle) |
| **SPA + service worker (PWA)** | Offline capability; fast repeat visits | Complex caching; update issues |

**Selected approach: SPA (React 18 + TypeScript) with aggressive code splitting + CDN.**

**Justification:** The portal is an internal tool (no SEO needed). SPA provides the best navigation experience (instant page transitions). Code splitting reduces initial bundle size. CDN ensures fast delivery globally.

**Implementation:**

```typescript
// App.tsx - Lazy-loaded routes
const Dashboard = lazy(() => import('./pages/Dashboard'));
const ResourceList = lazy(() => import('./pages/ResourceList'));
const ResourceDetail = lazy(() => import('./pages/ResourceDetail'));
const TemplateCatalog = lazy(() => import('./pages/TemplateCatalog'));
const ApprovalQueue = lazy(() => import('./pages/ApprovalQueue'));
const AuditLog = lazy(() => import('./pages/AuditLog'));
const CostDashboard = lazy(() => import('./pages/CostDashboard'));
const QuotaManager = lazy(() => import('./pages/QuotaManager'));
const Settings = lazy(() => import('./pages/Settings'));

function App() {
    return (
        <AuthProvider>
            <ThemeProvider>
                <Layout>
                    <Suspense fallback={<PageSkeleton />}>
                        <Routes>
                            <Route path="/" element={<Dashboard />} />
                            <Route path="/resources" element={<ResourceList />} />
                            <Route path="/resources/:uid" element={<ResourceDetail />} />
                            <Route path="/templates" element={<TemplateCatalog />} />
                            <Route path="/approvals" element={<ApprovalQueue />} />
                            <Route path="/audit" element={<AuditLog />} />
                            <Route path="/costs" element={<CostDashboard />} />
                            <Route path="/quotas" element={<QuotaManager />} />
                            <Route path="/settings" element={<Settings />} />
                        </Routes>
                    </Suspense>
                </Layout>
            </ThemeProvider>
        </AuthProvider>
    );
}

// Data fetching with React Query (TanStack Query)
function ResourceListPage() {
    const [filters, setFilters] = useState<ResourceFilters>(defaultFilters);
    const { data, isLoading, error } = useQuery({
        queryKey: ['resources', filters],
        queryFn: () => api.getResources(filters),
        staleTime: 30_000,      // Consider fresh for 30s
        refetchInterval: 60_000, // Background refresh every 60s
    });

    // Real-time updates overlay on cached data
    const wsUpdate = useProjectActivity(filters.project);
    useEffect(() => {
        if (wsUpdate?.event === 'resource.status_changed') {
            // Invalidate and refetch
            queryClient.invalidateQueries({ queryKey: ['resources'] });
        }
    }, [wsUpdate]);

    if (isLoading) return <ResourceListSkeleton />;
    if (error) return <ErrorBanner error={error} />;

    return (
        <div>
            <ResourceFiltersBar filters={filters} onChange={setFilters} facets={data.meta.facets} />
            <ResourceTable resources={data.data.items} />
            <Pagination meta={data.meta.pagination} onChange={(page) => setFilters({...filters, page})} />
        </div>
    );
}
```

**Performance optimizations:**

1. **Code splitting:** Each page is a separate chunk (~50KB each). Initial load only downloads the landing page.
2. **Prefetching:** On hover over navigation link, prefetch that page's chunk.
3. **React Query caching:** API responses cached with configurable staleTime. Background refetch for freshness.
4. **Virtual scrolling:** For large lists (1,000+ items), use `react-window` to render only visible rows.
5. **Skeleton loading:** Show content skeletons while data loads (perceived performance).
6. **Image optimization:** SVG icons (no raster images); lazy-load below-fold images.
7. **CDN caching:** Static assets cached with content-hash filenames (immutable caching, long TTL).
8. **Compression:** gzip/Brotli on all responses (BFF and CDN).

**Bundle size targets:**

| Chunk | Target | Contents |
|-------|--------|----------|
| Main (framework) | < 100KB gzipped | React, React Router, React Query, state management |
| Vendor (shared libs) | < 150KB gzipped | UI component library (MUI/Ant Design), charting (Chart.js), date handling |
| Dashboard page | < 50KB gzipped | Dashboard components, chart configs |
| Resource list page | < 30KB gzipped | Table, filters, pagination |
| Resource detail page | < 40KB gzipped | Detail view, timeline, health checks |
| Total initial (dashboard) | < 300KB gzipped | Main + vendor + dashboard |

**Responsive Design:**

```css
/* Mobile-first breakpoints */
:root {
    --breakpoint-sm: 640px;   /* Phone landscape */
    --breakpoint-md: 768px;   /* Tablet */
    --breakpoint-lg: 1024px;  /* Desktop */
    --breakpoint-xl: 1280px;  /* Wide desktop */
}

/* Dashboard grid: 1 column on mobile, 2 on tablet, 4 on desktop */
.dashboard-grid {
    display: grid;
    grid-template-columns: 1fr;
    gap: 16px;
}

@media (min-width: 768px) {
    .dashboard-grid {
        grid-template-columns: repeat(2, 1fr);
    }
}

@media (min-width: 1024px) {
    .dashboard-grid {
        grid-template-columns: repeat(4, 1fr);
    }
}
```

**Failure modes:**
- **CDN down:** Fallback to serving static assets from BFF origin server. Slower but functional.
- **API error on page load:** Show error boundary with retry button. Cached data (React Query) shown if available.
- **WebSocket disconnection:** Auto-reconnect with backoff. Show "connection lost" banner. Fall back to polling.
- **Large dataset rendering:** Virtual scrolling prevents browser freeze. Pagination on server side.

**Interviewer Q&As:**

**Q1: Why React instead of Vue or Angular for this portal?**
A: (1) Team familiarity: our frontend engineers know React. (2) Ecosystem: largest library/component ecosystem. (3) TypeScript support: first-class. (4) Performance: React 18 concurrent features (Suspense, startTransition) help with large list rendering. (5) Vue would also be excellent; Angular is heavier and opinionated in ways we don't need.

**Q2: How do you handle state management in the frontend?**
A: Three layers: (1) Server state: React Query (TanStack Query) manages all API data. Provides caching, background refresh, optimistic updates. (2) UI state: React useState/useReducer for form state, filter selections, modal visibility. (3) Global state: React Context for auth, theme, and user preferences. We avoid Redux because React Query handles the most complex state (server data).

**Q3: How do you handle authentication on the frontend?**
A: The frontend never sees JWT tokens. Auth is cookie-based: (1) Login redirects to Okta. (2) BFF handles token exchange server-side. (3) BFF sets HttpOnly session cookie. (4) Frontend checks `/auth/me` on startup; if 401, redirects to login. (5) Refresh is handled by the BFF (transparent to frontend). This prevents XSS token theft.

**Q4: How do you handle real-time chart updates on the dashboard?**
A: (1) Initial data loaded via API (React Query, 30s stale time). (2) Chart.js or Recharts renders the data. (3) WebSocket delivers incremental updates. (4) On update, we append the new data point and slide the chart window. (5) Heavy updates (full chart refresh) throttled to max once per 5 seconds to prevent DOM thrashing.

**Q5: How do you test the frontend?**
A: (1) Unit tests: React Testing Library for component logic. (2) Integration tests: MSW (Mock Service Worker) for API mocking. (3) E2E tests: Playwright for full user flows (login -> create resource -> verify status). (4) Visual regression: Storybook + Chromatic for component visual tests. (5) Accessibility: axe-core in CI.

**Q6: How do you handle CSP (Content Security Policy) for the portal?**
A: Strict CSP headers set by the BFF: `default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline' (for CSS-in-JS); connect-src 'self' wss://portal.infra.company.com; img-src 'self' data:; frame-src https://grafana.infra.company.com (for embedded dashboards)`.

---

### 6.4 Session Management & JWT Token Handling

**Why it's hard:** The portal must maintain user sessions across page reloads, handle token refresh transparently, and support concurrent sessions (multiple browser tabs). Tokens must be kept secure from XSS attacks while maintaining a smooth user experience (no surprise logouts).

| Approach | Pros | Cons |
|----------|------|------|
| **JWT in localStorage** | Simple; survives page refresh | XSS vulnerability (JS can read token) |
| **JWT in httpOnly cookie** | XSS-safe; automatic inclusion | CSRF risk (mitigated by SameSite); opaque to frontend |
| **Server-side session + cookie** | Most secure (token never in browser); revocable | Session storage needed (Redis); extra hop |
| **BFF token relay** | Best of both: frontend uses opaque session cookie, BFF holds real tokens | Slightly more complex |

**Selected approach: Server-side session with HttpOnly cookie. BFF manages JWT lifecycle.**

**Implementation:**

```java
@Component
public class SessionManager {
    private final RedisTemplate<String, String> redis;
    private final ObjectMapper objectMapper;
    private static final Duration SESSION_TTL = Duration.ofHours(8);
    private static final Duration TOKEN_REFRESH_BUFFER = Duration.ofMinutes(5);

    /**
     * Create a new session after successful OIDC authentication.
     */
    public String createSession(OIDCTokenResponse tokenResponse, UserInfo userInfo) {
        String sessionId = generateSecureSessionId(); // 256-bit random

        UserSession session = UserSession.builder()
            .userId(userInfo.getId())
            .email(userInfo.getEmail())
            .displayName(userInfo.getDisplayName())
            .roles(userInfo.getGroupsAsRoles())
            .accessToken(tokenResponse.getAccessToken())
            .refreshToken(tokenResponse.getRefreshToken())
            .tokenExpiresAt(Instant.now().plus(tokenResponse.getExpiresIn(), ChronoUnit.SECONDS))
            .createdAt(Instant.now())
            .lastActivity(Instant.now())
            .build();

        redis.opsForValue().set(
            "session:" + sessionId,
            objectMapper.writeValueAsString(session),
            SESSION_TTL
        );

        return sessionId;
    }

    /**
     * Validate and optionally refresh a session.
     */
    public UserSession validateSession(String sessionId) {
        String json = redis.opsForValue().get("session:" + sessionId);
        if (json == null) return null;

        UserSession session = objectMapper.readValue(json, UserSession.class);

        // Check if access token needs refresh
        if (session.getTokenExpiresAt().minus(TOKEN_REFRESH_BUFFER).isBefore(Instant.now())) {
            session = refreshSessionToken(sessionId, session);
        }

        // Update last activity
        session.setLastActivity(Instant.now());
        redis.opsForValue().set(
            "session:" + sessionId,
            objectMapper.writeValueAsString(session),
            SESSION_TTL
        );

        return session;
    }

    /**
     * Refresh the access token using the refresh token.
     */
    private UserSession refreshSessionToken(String sessionId, UserSession session) {
        try {
            OIDCTokenResponse newTokens = oktaClient.refreshToken(session.getRefreshToken());
            session.setAccessToken(newTokens.getAccessToken());
            session.setRefreshToken(newTokens.getRefreshToken()); // Rotate refresh token
            session.setTokenExpiresAt(
                Instant.now().plus(newTokens.getExpiresIn(), ChronoUnit.SECONDS));
            return session;
        } catch (OAuthException e) {
            // Refresh token invalid/expired: invalidate session
            redis.delete("session:" + sessionId);
            throw new SessionExpiredException("Session expired. Please log in again.");
        }
    }

    /**
     * Invalidate a session (logout).
     */
    public void invalidateSession(String sessionId) {
        redis.delete("session:" + sessionId);
    }

    private String generateSecureSessionId() {
        byte[] bytes = new byte[32]; // 256 bits
        SecureRandom.getInstanceStrong().nextBytes(bytes);
        return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes);
    }
}
```

**Cookie configuration:**

```java
@Configuration
public class CookieConfig {

    @Bean
    public CookieSerializer cookieSerializer() {
        DefaultCookieSerializer serializer = new DefaultCookieSerializer();
        serializer.setCookieName("SESSION");
        serializer.setUseHttpOnlyCookie(true);   // Not accessible via JavaScript
        serializer.setUseSecureCookie(true);      // HTTPS only
        serializer.setSameSite("Strict");         // CSRF protection
        serializer.setCookiePath("/");
        serializer.setCookieMaxAge((int) Duration.ofHours(8).toSeconds());
        serializer.setDomainNamePattern("^.+?\\.(\\w+\\.[a-z]+)$"); // portal.infra.company.com
        return serializer;
    }
}
```

**Refresh token rotation:**

```
Timeline:
T=0:     Login -> Access Token (1h TTL) + Refresh Token (30d TTL) stored in Redis session
T=55m:   API call -> Session manager detects token expiring in 5m -> auto-refresh
         -> New Access Token (1h TTL) + New Refresh Token (30d TTL) stored in Redis
         -> Old refresh token invalidated (rotation)
T=1h55m: API call -> Auto-refresh again
...
T=8h:    Session TTL expires -> Redis key deleted -> Next API call gets 401 -> Frontend redirects to login
```

**Failure modes:**
- **Redis down:** No session validation possible. All users get 401. Mitigation: Redis Sentinel for HA. Circuit breaker returns "service unavailable" instead of 401 (prevents mass logout).
- **Refresh token stolen:** Refresh token rotation detects stolen tokens. If the legitimate user's refresh succeeds, the stolen token is invalidated. If the attacker refreshes first, the legitimate user's next refresh fails (alerting to compromise).
- **Concurrent tab token refresh:** Two tabs call BFF simultaneously, both trigger refresh. Session mutex in Redis (SETNX) ensures only one refresh at a time. Second tab waits and uses the refreshed token.
- **Session fixation:** Session ID is regenerated after authentication (never reuse pre-auth session ID).

**Interviewer Q&As:**

**Q1: Why server-side sessions instead of JWT-only?**
A: (1) Revocation: server-side sessions can be instantly invalidated (admin deactivates user -> session deleted from Redis). JWTs are valid until expiry unless you maintain a blacklist (which is essentially a server-side session). (2) Token security: access/refresh tokens never leave the server. (3) Session size: session cookie is 43 bytes (session ID). JWT cookie would be 1-2KB. (4) The trade-off is Redis dependency, but we need Redis for caching anyway.

**Q2: How do you handle CSRF with SameSite=Strict cookies?**
A: SameSite=Strict is the primary CSRF defense (cookie not sent on cross-origin requests). Additionally, for state-changing operations (POST/PUT/DELETE), the BFF checks `Origin` and `Referer` headers. We also implement the Synchronizer Token Pattern (CSRF token in meta tag, sent as X-CSRF-Token header) as defense-in-depth.

**Q3: What happens when the session expires while the user is actively working?**
A: The access token is refreshed transparently by the BFF (5-minute buffer before expiry). The session has an 8-hour hard TTL (Redis key TTL). 10 minutes before session expiry, the BFF returns a `X-Session-Expiring: 600` header. The frontend shows a "Session expiring in 10 minutes. Save your work." banner with a "Extend session" button that calls `/auth/refresh`.

**Q4: How do you handle multiple active sessions for the same user?**
A: Each session is independent (separate Redis key). A user can have sessions in multiple browsers/devices. Admin can view all active sessions for a user and revoke any/all. There's a configurable max sessions limit (default: 5). When exceeded, the oldest session is invalidated.

**Q5: How do you prevent session cookie theft via network interception?**
A: (1) Secure flag: cookie only sent over HTTPS. (2) HSTS header: browser never connects via HTTP. (3) Certificate pinning (optional): browser rejects MITM certificates. (4) Session bound to IP range: if client IP changes drastically (different country), session is invalidated.

**Q6: How do you handle graceful logout vs forced logout (admin-initiated)?**
A: Graceful: user clicks logout -> BFF invalidates session in Redis -> BFF calls Okta `/logout` endpoint -> clears cookie. Forced: admin invalidates session in Redis -> user's next API call gets 401 -> frontend redirects to login. The forced path doesn't call Okta logout (user's Okta session may still be active, which is fine -- they'll just re-authenticate).

---

## 7. Scheduling & Resource Management

### Dashboard Data Scheduling

The portal needs fresh dashboard data without overwhelming backend services:

1. **Cost snapshots:** A daily cron job (2 AM) calculates per-project costs from resource billing data and stores snapshots in `daily_cost_snapshots` table. Dashboard reads from this pre-computed table.

2. **Utilization data:** Every 5 minutes, a background job queries Prometheus/VictoriaMetrics for resource utilization metrics and caches the results in Redis. Dashboard reads from Redis cache.

3. **Notification digest:** A scheduled job runs every 5 minutes to batch individual events into digest notifications (e.g., "3 VMs provisioned in the last hour" instead of 3 separate notifications).

### Real-Time Event Scheduling

```java
@Component
public class EventFanOutScheduler {
    /**
     * Processes events from RabbitMQ and distributes to:
     * 1. WebSocket (real-time push)
     * 2. Portal notifications (in-app)
     * 3. Elasticsearch (audit)
     */
    @RabbitListener(queues = "portal.events", concurrency = "3-10")
    public void processEvent(InfraEvent event) {
        // Fan out in parallel
        CompletableFuture.allOf(
            CompletableFuture.runAsync(() -> webSocketPublisher.publish(event)),
            CompletableFuture.runAsync(() -> notificationService.createIfNeeded(event)),
            CompletableFuture.runAsync(() -> auditService.index(event))
        ).join();
    }
}
```

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach | Baseline | Max |
|-----------|-----------------|----------|-----|
| CDN (CloudFront) | Auto-scales | N/A | Unlimited |
| Load Balancer (HAProxy) | Active-passive pair | 2 | 2 |
| BFF (Spring Boot) | HPA on CPU (target 60%) | 3 pods | 10 pods |
| WebSocket Gateway | HPA on connection count | 2 pods | 5 pods |
| Event Consumer | HPA on queue depth | 2 pods | 8 pods |
| MySQL | Primary + 2 replicas | 3 | 3 (scale up, not out) |
| Elasticsearch | 3-node cluster | 3 | 6 data nodes |
| Redis | 3-node Sentinel | 3 | 3 (scale up) |

### Performance Optimization Path

If portal grows to 10,000 concurrent users (10x current):

1. **BFF:** Scale to 30 pods. Aggressive response caching (60s TTL for dashboard).
2. **WebSocket:** 5 pods, each handling 2,000 connections. Redis pub/sub handles fan-out.
3. **MySQL:** Read replicas for all GET endpoints. Consider materialized views for dashboard queries.
4. **CDN:** Already scales. Add a second CDN for geographic redundancy.
5. **Frontend:** No change needed (SPA served from CDN).

### Interviewer Q&As

**Q1: What's the first bottleneck as concurrent users grow from 300 to 3,000?**
A: BFF API latency. The BFF makes parallel calls to backend services for aggregation. At 3,000 users, backend services see 10x more traffic. Mitigation: (1) BFF caching with Redis (30s TTL). (2) Batch API endpoints on backend services (e.g., get 50 resources in one call). (3) Scale BFF pods to 10+. (4) Read replicas for database-heavy queries.

**Q2: How do you handle CDN cache invalidation when the frontend is updated?**
A: Frontend assets are content-hashed (e.g., `main.a1b2c3.js`). New deployment creates new files with new hashes. The `index.html` (not cached, or short TTL) references new file hashes. Old files remain in CDN until TTL expires (no explicit invalidation needed). This is the standard approach for SPA deployments.

**Q3: How do you scale WebSocket connections beyond 10,000?**
A: (1) Each WebSocket pod handles 2,000 connections. 5 pods = 10,000. (2) For more: add more pods. (3) If Redis pub/sub becomes a bottleneck (unlikely at this scale), switch to a dedicated message broker like NATS or use Redis Cluster. (4) Consider SSE for read-only subscriptions (lighter weight per connection).

**Q4: How do you handle thundering herd on the dashboard?**
A: 9 AM Monday: all 3,000 DAU open the portal simultaneously. (1) BFF caches dashboard data with 30s TTL. First request computes; subsequent 29.9s of requests hit cache. (2) Single-flight pattern: only one request per cache key fetches from backend; others wait for the result. (3) Pre-warm cache: cron job refreshes top projects' dashboard data before 9 AM.

**Q5: How would you add a mobile app?**
A: (1) The BFF already serves a clean REST API. A mobile app (React Native or native) can use the same BFF endpoints. (2) Mobile might need a separate BFF instance (different response shapes, less data per call). (3) Authentication: use OAuth2 Authorization Code + PKCE (not device flow; mobile has a browser). (4) Push notifications: add Firebase/APNs integration to the notification service.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---------|--------|-----------|------------|-----|
| CDN down | Portal can't load | Synthetic monitoring | Multi-CDN (CloudFront + Fastly); origin fallback | 1 min |
| BFF pods crash | API errors | K8s liveness probe | HPA ensures min 3 pods; K8s restarts crashed pods | 10s (individual pod) |
| MySQL primary down | No writes | Connection health check | Automated failover (MHA/Orchestrator); BFF uses read replica for GET | 30s |
| MySQL replica down | Slower reads | Replication monitoring | Multiple replicas; BFF routes to healthy replicas | 10s |
| Redis down | No sessions (all users logged out); no cache | Sentinel monitoring | Redis Sentinel auto-failover; BFF falls through to MySQL for critical reads | 10s |
| Elasticsearch down | No search; no audit log viewing | Health check | Audit writes buffered in RabbitMQ; search degraded banner shown | 5 min |
| RabbitMQ down | No real-time updates; no event fan-out | Queue monitoring | Mirrored queues; polling fallback on frontend | 1 min |
| WebSocket server down | Live updates stop | Connection monitoring | Auto-reconnect; polling fallback | 5s (client reconnects) |
| Okta down | No new logins | Okta status monitoring | Existing sessions valid until expiry (8h); emergency local auth for admins | N/A |
| BFF circuit breaker open | Dashboard shows partial data | Metrics monitoring | Graceful degradation; show cached data with "degraded" banner | Automatic |

### Chaos Testing

- **Weekly:** Kill random BFF pod during business hours. Verify: no user-visible errors (K8s restarts pod < 10s).
- **Monthly:** Simulate Redis failover. Verify: sessions survive Sentinel promotion.
- **Quarterly:** Simulate MySQL failover. Verify: portal recovers within 30s.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold | Description |
|--------|------|-----------------|-------------|
| `portal.page_load.p99` | Histogram | > 3s | Initial page load time (Real User Monitoring) |
| `portal.spa_navigation.p99` | Histogram | > 500ms | SPA page navigation time |
| `portal.api.latency.p99` | Histogram | > 500ms | BFF API response time |
| `portal.api.error_rate` | Rate | > 1% | 5xx error rate |
| `portal.ws.connections` | Gauge | > 2000 | Active WebSocket connections |
| `portal.ws.message_rate` | Rate | < 10/s sustained | WebSocket message throughput |
| `portal.session.active` | Gauge | Informational | Active user sessions |
| `portal.session.create_rate` | Rate | > 100/min | Login rate (detect SSO issues) |
| `portal.cache.hit_rate` | Rate | < 80% | BFF cache effectiveness |
| `portal.cdn.cache_hit_rate` | Rate | < 90% | CDN cache effectiveness |
| `portal.bundle_size` | Gauge | > 500KB gzipped | Frontend bundle size (build-time) |
| `portal.lcp` | Histogram | > 2.5s | Largest Contentful Paint (Core Web Vital) |
| `portal.fid` | Histogram | > 100ms | First Input Delay (Core Web Vital) |
| `portal.cls` | Histogram | > 0.1 | Cumulative Layout Shift (Core Web Vital) |

### Frontend Monitoring

```typescript
// Real User Monitoring (RUM)
import { init as initRUM } from '@datadog/browser-rum';

initRUM({
    applicationId: 'infra-portal',
    clientToken: '...',
    site: 'datadoghq.com',
    service: 'infra-portal-frontend',
    sampleRate: 100,
    trackInteractions: true,
    trackResources: true,
    trackLongTasks: true,
});

// Custom performance marks
performance.mark('dashboard-data-loaded');
performance.measure('dashboard-load-time', 'navigationStart', 'dashboard-data-loaded');
```

### Distributed Tracing

- Request ID generated at load balancer, propagated through all services.
- Frontend includes `X-Request-Id` header in all API calls.
- BFF propagates trace context to backend services (OpenTelemetry).
- Full trace visible in Jaeger: browser -> CDN -> LB -> BFF -> backend services -> MySQL/ES.

---

## 11. Security

### Auth & AuthZ

**Authentication Flow (OIDC with Okta):**

```
Browser                  BFF                       Okta
  |                       |                         |
  |-- GET /dashboard ---->|                         |
  |                       |-- No session cookie ---->|
  |<-- 302 /auth/login --|                         |
  |                       |                         |
  |-- GET /auth/login --->|                         |
  |<-- 302 Okta/authorize-|                         |
  |                       |                         |
  |-- GET Okta/authorize -------------------------------->|
  |                       |                         |-- User authenticates (MFA)
  |<-- 302 /auth/callback?code=XYZ ------------------|
  |                       |                         |
  |-- GET /auth/callback-->|                         |
  |                       |-- POST Okta/token ------>|
  |                       |<-- tokens --------------|
  |                       |-- Validate id_token      |
  |                       |-- Create Redis session   |
  |<-- Set-Cookie: SESSION |                         |
  |<-- 302 /dashboard ----|                         |
  |                       |                         |
  |-- GET /dashboard ---->|                         |
  |   (with SESSION cookie)|                         |
  |<-- 200 + HTML --------|                         |
```

### SSO Integration

```java
@Configuration
@EnableWebSecurity
public class SecurityConfig {

    @Bean
    public SecurityFilterChain filterChain(HttpSecurity http) throws Exception {
        http
            .authorizeHttpRequests(authz -> authz
                .requestMatchers("/api/v1/auth/**").permitAll()
                .requestMatchers("/health", "/ready").permitAll()
                .requestMatchers("/api/v1/**").authenticated()
            )
            .oauth2Login(oauth2 -> oauth2
                .authorizationEndpoint(authz -> authz
                    .baseUri("/api/v1/auth/login")
                )
                .redirectionEndpoint(redir -> redir
                    .baseUri("/api/v1/auth/callback")
                )
                .tokenEndpoint(token -> token
                    .accessTokenResponseClient(oktaTokenResponseClient())
                )
                .userInfoEndpoint(userInfo -> userInfo
                    .userService(oktaUserService())
                )
                .successHandler(oidcSuccessHandler())
            )
            .csrf(csrf -> csrf
                .csrfTokenRepository(CookieCsrfTokenRepository.withHttpOnlyFalse())
                .csrfTokenRequestHandler(new CsrfTokenRequestAttributeHandler())
            )
            .sessionManagement(session -> session
                .sessionCreationPolicy(SessionCreationPolicy.IF_REQUIRED)
                .maximumSessions(5)
            )
            .headers(headers -> headers
                .contentSecurityPolicy(csp -> csp
                    .policyDirectives("default-src 'self'; script-src 'self'; " +
                        "style-src 'self' 'unsafe-inline'; " +
                        "connect-src 'self' wss://portal.infra.company.com; " +
                        "img-src 'self' data:; " +
                        "frame-src https://grafana.infra.company.com")
                )
                .frameOptions(fo -> fo.deny())
                .httpStrictTransportSecurity(hsts -> hsts
                    .includeSubDomains(true)
                    .maxAgeInSeconds(31536000)
                )
            );

        return http.build();
    }
}
```

### RBAC Enforcement

**Multi-level authorization:**

1. **Route-level (BFF):** Spring Security checks authentication for all `/api/v1/**` routes.
2. **Project-level (BFF):** Custom filter checks that the user is a member of the requested project.
3. **Role-level (BFF):** `@PreAuthorize` annotations check specific roles per endpoint.
4. **Resource-level (backend):** Backend services verify the user can access the specific resource.

```java
@Component
public class ProjectAuthorizationFilter extends OncePerRequestFilter {

    @Override
    protected void doFilterInternal(HttpServletRequest request, HttpServletResponse response,
                                     FilterChain filterChain) throws ServletException, IOException {
        String projectParam = request.getParameter("project");
        if (projectParam == null) {
            projectParam = extractProjectFromPath(request.getRequestURI());
        }

        if (projectParam != null) {
            UserSession session = getSession(request);
            if (!session.getRoles().containsKey(projectParam)) {
                response.sendError(403, "Not a member of project: " + projectParam);
                return;
            }
        }

        filterChain.doFilter(request, response);
    }
}
```

### Row-Level Security (Multi-Tenancy)

All database queries include a project_id filter derived from the authenticated user's session:

```java
@Repository
public class ResourceRepository {

    // All queries are project-scoped
    @Query("SELECT r FROM Resource r WHERE r.projectId = :projectId AND r.status = :status")
    List<Resource> findByProjectAndStatus(@Param("projectId") Long projectId,
                                          @Param("status") ResourceStatus status);

    // NEVER expose a method that queries all projects (except for admin endpoints)
}

// Hibernate filter for automatic tenant isolation
@FilterDef(name = "tenantFilter", parameters = @ParamDef(name = "projectId", type = Long.class))
@Filter(name = "tenantFilter", condition = "project_id = :projectId")
@Entity
public class Resource { ... }
```

---

## 12. Incremental Rollout

### Phase 1: Authentication & Shell (Weeks 1-3)
- React app scaffold with routing, lazy loading.
- BFF with Okta OIDC integration.
- Session management (Redis).
- Login/logout flow.
- Navigation shell with sidebar.
- User profile page.

### Phase 2: Resource Management (Weeks 4-7)
- Resource list page with filtering, search, pagination.
- Resource detail page with status and timeline.
- Create resource flow (template selection -> customization -> submit).
- Delete resource with confirmation.
- RBAC enforcement per project.

### Phase 3: Dashboard & Real-Time (Weeks 8-11)
- Dashboard with resource summary widgets.
- Cost charts (daily, weekly, monthly).
- Quota utilization bars.
- WebSocket integration for live status updates.
- Notification center (in-app).

### Phase 4: Advanced Features (Weeks 12-16)
- Template catalog with browsable cards.
- Approval workflow UI (pending list, approve/reject).
- Audit log viewer with search and filters.
- Embedded Grafana dashboards.
- Mobile-responsive optimization.
- Accessibility audit and fixes.

### Rollout Q&As

**Q1: How do you deploy frontend updates without downtime?**
A: (1) Build new bundle with content-hashed filenames. (2) Upload to S3/CDN. (3) Update `index.html` to reference new bundle. (4) Old bundles remain available (browsers with cached `index.html` still load old bundle until refresh). (5) No server restart needed. (6) Optionally show "New version available, click to reload" banner when we detect a version mismatch.

**Q2: How do you A/B test new portal features?**
A: LaunchDarkly feature flags. The BFF includes a feature flag evaluation in the `/auth/me` response: `{ "features": { "new_dashboard": true, "v2_templates": false } }`. React app conditionally renders features based on flags. This allows gradual rollout to specific teams or users.

**Q3: How do you handle browser caching issues (users seeing stale portal)?**
A: Content-hashed filenames ensure cache busting for JS/CSS. `index.html` has `Cache-Control: no-cache` (always revalidated with CDN). Service worker (if used) has version-based update logic. Worst case: user hard-refreshes (Ctrl+F5).

**Q4: How do you handle the portal when the backend is being upgraded?**
A: BFF circuit breaker detects backend unavailability. Portal shows "Under maintenance" banner for affected features. Unaffected features continue working. The BFF returns cached dashboard data (stale but available). Status page shows which services are under maintenance.

**Q5: How do you measure portal adoption and user satisfaction?**
A: (1) RUM metrics: page load time, interaction time, error rate. (2) Feature usage analytics: which pages are visited, which templates are popular, which features are unused. (3) NPS survey: quarterly in-app survey. (4) Support ticket volume: declining tickets = good portal UX. (5) Self-service ratio: % of provisioning done via portal vs manual tickets.

---

## 13. Trade-offs & Decision Log

| Decision | Chosen | Alternative | Why |
|----------|--------|-------------|-----|
| Frontend framework | React 18 (TypeScript) | Vue 3, Angular, Svelte | Team expertise; largest ecosystem; excellent TypeScript support |
| UI component library | Ant Design (antd) | Material UI, Chakra UI | Enterprise-focused; rich component set (tables, forms, charts); used by many internal tools |
| State management | React Query + Context | Redux, Zustand, MobX | React Query handles 90% of state (server data); Context for remaining (auth, theme); Redux overkill |
| Backend framework | Spring Boot 3.x | Express.js, FastAPI, Gin | Organizational standard; Spring Security for OIDC/SAML; Spring WebSocket |
| Session storage | Redis (server-side) | JWT in cookie, JWT in localStorage | Token never in browser (XSS-safe); instant revocation; session metadata (last activity) |
| Real-time | WebSocket (STOMP) | SSE, Long Polling | Bidirectional; multiplexed subscriptions; lower latency |
| API pattern | BFF | GraphQL, Direct-to-service | Optimized for single frontend; server-side aggregation; auth token isolation |
| Search | Elasticsearch | MySQL FULLTEXT, Meilisearch | Already running ES for audit; supports faceted search, highlighting |
| Charts | Chart.js via react-chartjs-2 | Recharts, ECharts, D3 | Lightweight; sufficient for cost/utilization charts; good React integration |
| Embedded monitoring | Grafana iframes | Custom charts, Kibana | Grafana already deployed for ops; rich dashboard ecosystem; SSO integration |

---

## 14. Agentic AI Integration

### AI-Powered Dashboard Insights

The dashboard includes an "AI Insights" widget that surfaces actionable observations:

```
AI Insights for project ml-team:

1. Cost Anomaly: Your daily spend increased 35% yesterday ($580 vs $430 avg).
   Cause: 2 new GPU VMs provisioned by bob@company.com.
   Action: [Review] [Dismiss]

2. Unused Resources: 3 VMs have had <5% CPU utilization for 7 days.
   Estimated savings: $360/month if terminated.
   Action: [View VMs] [Schedule Cleanup]

3. Quota Alert: GPU utilization at 85%. At current growth rate, you'll hit
   the hard limit in ~2 weeks.
   Action: [Request Quota Increase]
```

### Conversational Infrastructure Assistant

Embedded chat widget in the portal:

```
User: "Show me all resources created by my team this week"

AI: Here are the resources created by ml-team members this week:

| Resource | Type | Created By | Created | Cost/hr |
|----------|------|------------|---------|---------|
| training-vm-03 | VM | alice@ | Mon | $12.50 |
| eval-cluster | K8s | bob@ | Tue | $8.00 |
| data-store-02 | Storage | carol@ | Wed | $0.50 |

Total new cost: $21.00/hr ($504/day)

Would you like to see more details on any of these?
```

### AI-Assisted Template Selection

```
User: "I need infrastructure for a real-time recommendation service"

AI: Based on your requirements, I recommend:

1. **Serving Layer**: K8s cluster (5 nodes, CPU-optimized)
   - Handles real-time inference
   - Auto-scaling enabled
   Template: "ML Serving Stack" | $5.00/hr

2. **Feature Store**: 2x VMs with high-memory (128GB RAM each)
   - Redis-backed feature cache
   Template: "Feature Store" | $3.00/hr

3. **Model Storage**: Object storage (100GB)
   - S3-compatible, versioned
   Template: "ML Model Registry" | $0.10/hr

Total estimated cost: $8.10/hr ($194/day)

[Use These Templates] [Customize] [Ask More Questions]
```

### Architecture

```
+------------------+     +-------------------+
| React Portal     |     | AI Widget (React) |
| (Chat component) |---->| (streaming UI)    |
+--------+---------+     +--------+----------+
         |                         |
         v                         v
+--------+-------------------------+---------+
|          BFF (Spring Boot)                  |
|  POST /api/v1/ai/chat                      |
|  - Stream response (SSE)                   |
|  - Tool execution (server-side)            |
+--------+-----------------------------------+
         |
+--------v---------+     +------------------+
| AI Gateway       |---->| LLM (Claude)     |
| (tool use)       |     | with tool calling|
+------------------+     +------------------+
```

**Safety:**
- AI chat is read-only by default. Provisioning actions suggested by AI require user confirmation.
- AI responses include source attribution (which API calls were made to generate the response).
- All AI interactions are logged to the audit trail.
- Rate limited: 20 AI queries per user per hour.
- PII scrubbing: user emails shown but tokens/secrets never included in AI context.

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through what happens from when a user opens the portal to when the dashboard is fully rendered.**
A: (1) Browser requests `portal.infra.company.com`. (2) CDN serves `index.html` (~1KB, not cached). (3) Browser parses HTML, requests JS bundle from CDN (content-hashed, cached long-term). (4) React app mounts, checks for SESSION cookie via `GET /auth/me`. (5) If no session: redirect to Okta login. If valid session: BFF returns user context. (6) React Router renders Dashboard page (lazy-loaded, ~50KB chunk). (7) Dashboard component mounts, fires 4 parallel API calls via React Query: summary, cost-chart, utilization, notifications. (8) BFF receives requests, validates session, makes parallel calls to backend services (with 30s Redis cache). (9) Responses arrive (~200ms P50). React renders widgets progressively (skeleton -> data). (10) WebSocket connection established for live updates. (11) Dashboard fully interactive (~1.5s total from navigation).

**Q2: How do you handle multi-tenancy in the portal?**
A: Four layers: (1) Authentication: user identity from Okta OIDC. (2) Project membership: BFF filter checks user belongs to requested project (from session roles). (3) API scoping: all queries include `project_id` filter. (4) Database: Hibernate `@Filter` annotation adds `WHERE project_id = :projectId` to all queries. A user can be a member of multiple projects (with different roles); they switch via project selector in the UI.

**Q3: How do you embed Grafana dashboards in the portal?**
A: (1) Grafana configured with anonymous access for embedded mode (or auth proxy). (2) Portal embeds Grafana dashboards via `<iframe>` with CSP allowing `frame-src https://grafana.infra.company.com`. (3) Dashboard URL includes project and resource filters as query params. (4) Grafana org/team mapped to portal project for data isolation. (5) Alternative: use Grafana API to fetch panel JSON and render with Chart.js (more work but no iframe).

**Q4: How do you handle the case where a user has many resources (1,000+)?**
A: (1) Server-side pagination (50 items per page by default). (2) Server-side filtering (status, type, search query). (3) Virtual scrolling (react-window) for long lists. (4) Faceted search: show counts per filter value so users can narrow down quickly. (5) Saved filters: users save frequently-used filter combinations.

**Q5: How do you handle form validation for the resource creation wizard?**
A: (1) Client-side validation: React Hook Form with Zod schema validation. Provides instant feedback as user types. (2) Server-side validation: BFF validates against JSON Schema (same schema used by template system). (3) Quota pre-check: before submission, async call to check quota availability. (4) Cost estimation: as user adjusts parameters, real-time cost calculation via debounced API call.

**Q6: How do you handle internationalization (i18n) if the portal needs to support multiple languages?**
A: (1) react-i18next for string externalization. (2) Translation files per locale (JSON, lazy-loaded). (3) Date/time formatting via Intl API (respects user's locale setting). (4) Number formatting (currency, units) via Intl.NumberFormat. (5) RTL support if needed (CSS logical properties). (6) Currently English-only; i18n infrastructure is in place for future locales.

**Q7: How do you handle the approval workflow UI?**
A: (1) Approver sees a "Pending Approvals" badge in the sidebar (count from dashboard summary). (2) Approval queue page lists all pending requests for the user's projects (where they have approver role). (3) Each request shows: requester, resource details, estimated cost, justification. (4) Approver clicks Approve/Reject, adds optional comment. (5) WebSocket updates the requester's resource detail page in real-time. (6) Email/Slack notification sent to requester.

**Q8: How do you prevent XSS in the portal?**
A: (1) React auto-escapes JSX expressions (no `dangerouslySetInnerHTML` except for search highlights, sanitized with DOMPurify). (2) CSP header blocks inline scripts. (3) HttpOnly cookies prevent token theft via XSS. (4) Input validation on all API endpoints. (5) Output encoding for any user-generated content rendered in the UI.

**Q9: How do you handle the scenario where the portal needs to display data from multiple backend services that have different latencies?**
A: (1) BFF makes parallel calls with individual timeouts. (2) If a slow service times out (5s), BFF returns partial data with `degraded: true`. (3) Frontend renders available data immediately; shows loading spinner for pending sections. (4) React Suspense with granular boundaries: each dashboard widget is a separate Suspense boundary. (5) staleWhileRevalidate: show cached data while fresh data loads.

**Q10: How do you handle deep linking (sharing a URL to a specific resource)?**
A: React Router handles client-side routing. URL structure: `/resources/vm-abc123`. When a user opens this URL directly: (1) CDN serves `index.html`. (2) React app loads, Router matches `/resources/:uid`. (3) ResourceDetail component loads (lazy), fetches data via React Query. (4) If user isn't authenticated: redirect to login, then back to the deep link (stored in OIDC `state` parameter).

**Q11: How do you handle error pages (404, 500)?**
A: (1) React ErrorBoundary wraps the app: catches render errors, shows friendly error page with "Report Issue" button. (2) 404: React Router catch-all route renders a "Page Not Found" component. (3) API 404: resource detail page shows "Resource not found or you don't have access." (4) API 500: error banner with request ID for debugging: "Something went wrong. Reference: req-abc123".

**Q12: How do you implement the cost dashboard with drill-down?**
A: (1) Top level: monthly cost per project (bar chart). (2) Click project: daily cost breakdown by resource type (stacked area chart). (3) Click resource type: individual resource costs (table with sort by cost). (4) Click resource: resource detail page with cost timeline. Data source: `daily_cost_snapshots` table for historical, real-time calculation for current day. All aggregations done in SQL (GROUP BY with rollup).

**Q13: How do you handle accessibility for screen readers?**
A: (1) Semantic HTML: proper heading hierarchy, landmark regions, form labels. (2) ARIA attributes: `aria-label`, `aria-describedby`, `aria-live` for dynamic updates. (3) Keyboard navigation: all interactive elements focusable; skip-to-content link. (4) Color contrast: WCAG AA minimum (4.5:1 for text). (5) Automated testing: axe-core in CI catches common violations. (6) Manual testing: quarterly screen reader testing (VoiceOver, NVDA).

**Q14: How do you handle the portal when the user's connection is slow or intermittent?**
A: (1) React Query retries failed requests (3 attempts with exponential backoff). (2) Stale data shown while revalidating (staleWhileRevalidate pattern). (3) Offline indicator banner: "You appear to be offline. Showing cached data." (4) Service worker (optional): caches API responses for critical pages. (5) Reduced data mode: detect slow connections via Network Information API; skip heavy assets (charts, embedded dashboards).

**Q15: How would you implement a "dark mode" for the portal?**
A: (1) CSS variables for all colors: `--bg-primary`, `--text-primary`, `--border-color`, etc. (2) Theme toggled via user preference (stored in `user_preferences` table). (3) System preference detection: `prefers-color-scheme: dark` media query for default. (4) Theme switch component: Light / Dark / System toggle in settings. (5) CSS: `.theme-dark { --bg-primary: #1a1a2e; --text-primary: #e0e0e0; }`. (6) Ant Design supports theme customization via ConfigProvider.

**Q16: How do you handle the portal's performance monitoring in production?**
A: (1) Real User Monitoring (RUM): Datadog or custom Performance Observer API tracking LCP, FID, CLS. (2) Synthetic monitoring: Playwright scripts run every 5 minutes from multiple locations, measuring page load and critical user flows. (3) Backend APM: Spring Boot Actuator + Micrometer + Prometheus for BFF metrics. (4) Error tracking: Sentry for frontend JavaScript errors with stack traces and user context. (5) Alerting: PagerDuty for P99 latency > 3s or error rate > 1%.

---

## 16. References

1. **React 18** - Concurrent features, Suspense: https://react.dev/
2. **Spring Boot 3** - Web framework: https://spring.io/projects/spring-boot
3. **Spring Security OAuth2** - OIDC/SAML integration: https://docs.spring.io/spring-security/reference/servlet/oauth2/
4. **Spring WebSocket** - STOMP over WebSocket: https://docs.spring.io/spring-framework/reference/web/websocket.html
5. **TanStack Query (React Query)** - Server state management: https://tanstack.com/query
6. **Ant Design** - React UI component library: https://ant.design/
7. **Chart.js** - Canvas-based charting: https://www.chartjs.org/
8. **SockJS** - WebSocket fallback: https://github.com/sockjs/sockjs-client
9. **Content Security Policy** - MDN reference: https://developer.mozilla.org/en-US/docs/Web/HTTP/CSP
10. **Web Vitals** - Core Web Vitals metrics: https://web.dev/vitals/
11. **BFF Pattern** - Sam Newman's original article: https://samnewman.io/patterns/architectural/bff/
12. **OWASP Session Management** - Security best practices: https://owasp.org/www-project-web-security-testing-guide/latest/4-Web_Application_Security_Testing/06-Session_Management_Testing/
