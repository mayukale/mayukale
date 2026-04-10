# System Design: RBAC Authorization System

> **Relevance to role:** Every bare-metal IaaS and Kubernetes platform must answer "who can do what to which resource." A misconfigured authorization layer is the fastest path to a production incident or data breach. This design covers enterprise-grade RBAC for a cloud infrastructure platform -- spanning Kubernetes RBAC, OpenStack Keystone policy, job-scheduler permissions, and API-gateway enforcement -- at the scale of tens of thousands of service accounts and millions of authorization decisions per second.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | Define **Roles** as named collections of **Permissions** (verb + resource + optional conditions). |
| FR-2 | Bind Roles to **Subjects** (users, groups, service accounts). |
| FR-3 | Support **hierarchical role inheritance** (e.g., `platform-admin` inherits all permissions of `namespace-admin`). |
| FR-4 | Support **scoped bindings**: cluster-wide, namespace/project-scoped, resource-instance-scoped. |
| FR-5 | Kubernetes-native integration: Role, ClusterRole, RoleBinding, ClusterRoleBinding. |
| FR-6 | Policy-as-code evaluation via OPA/Gatekeeper (Rego). |
| FR-7 | Relationship-based access control (ReBAC, Zanzibar-like) for object-level permissions (e.g., "user X is owner of project Y"). |
| FR-8 | Audit log of **every** authorization decision (allow + deny). |
| FR-9 | Deny-by-default: no implicit permissions. |
| FR-10 | Emergency break-glass override with mandatory audit trail. |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Policy evaluation latency | p99 < 1 ms for cached decisions, p99 < 5 ms cold |
| NFR-2 | Availability | 99.999% (authorization is on the critical path) |
| NFR-3 | Throughput | 500,000 authorization decisions/sec cluster-wide |
| NFR-4 | Consistency | Strongly consistent within 2 seconds of policy change |
| NFR-5 | Audit completeness | Zero lost authorization decisions in the audit log |
| NFR-6 | Scalability | Support 50,000+ distinct roles, 200,000+ bindings |

### Constraints & Assumptions

- The platform runs Kubernetes 1.28+ on bare metal with 500+ nodes.
- Users authenticate via OIDC (corporate IdP) or service-account tokens.
- OpenStack Keystone is used for IaaS project/tenant identity.
- MySQL 8.0 is the OLTP store; Elasticsearch 8.x for audit search.
- Java (Spring Boot) and Python (FastAPI) services consume the authorization API.

### Out of Scope

- Authentication (handled by IdP / OIDC / Keystone -- consumed as input).
- Network-level access control (covered in zero_trust_network_design.md).
- Secrets management (covered in secrets_management_system.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Human users | 10,000 | Engineering org |
| Service accounts | 40,000 | ~80 per namespace x 500 namespaces |
| Roles | 5,000 | Custom + built-in |
| RoleBindings | 200,000 | Avg 40 bindings/namespace x 5,000 namespaces (k8s + platform) |
| Authorization decisions/sec (peak) | 500,000 | 500 nodes x 1,000 decisions/sec/node |
| Authorization decisions/day | ~20 billion | 500k/sec x 86,400 sec x 0.5 avg utilization |
| Audit events/day | ~20 billion | 1:1 with decisions |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Cached policy evaluation | < 1 ms p99 |
| Cold policy evaluation (cache miss) | < 5 ms p99 |
| Role/binding CRUD | < 50 ms p99 |
| Audit log query (last 24h) | < 2 s p99 |
| Policy propagation (change to enforcement) | < 2 s p99 |

### Storage Estimates

| Data | Size | Calculation |
|------|------|-------------|
| Roles + permissions | ~50 MB | 5,000 roles x ~10 KB avg (JSON rules) |
| Bindings | ~200 MB | 200,000 bindings x ~1 KB |
| Audit log (1 day) | ~4 TB | 20B events x 200 bytes avg |
| Audit log (90-day hot) | ~360 TB | 4 TB x 90 days |
| Policy cache (per node) | ~100 MB | Full policy set replicated |

### Bandwidth Estimates

| Flow | Bandwidth |
|------|-----------|
| Policy sync (push, per node) | ~1 KB/sec steady state, burst to 1 MB on full sync |
| Audit event stream | ~100 MB/sec (500k events/sec x 200 bytes) |
| Audit query responses | ~10 MB/sec avg |

---

## 3. High-Level Architecture

```
                          ┌──────────────────────────────────────────────────┐
                          │              Identity Provider (OIDC)            │
                          │         (Okta / Azure AD / Keycloak)            │
                          └────────────────────┬─────────────────────────────┘
                                               │ JWT with claims
                                               ▼
┌────────────┐   authz   ┌──────────────────────────────────────────────────┐
│  API       │──request──▶│           Authorization Gateway                 │
│  Gateway   │◀──allow/──│  ┌─────────────┐  ┌─────────────────────────┐   │
│  (Envoy)   │   deny    │  │ Policy      │  │ Decision Cache          │   │
│            │           │  │ Engine      │  │ (in-process LRU +       │   │
└────────────┘           │  │ (OPA)       │  │  Redis cluster)         │   │
                         │  └──────┬──────┘  └─────────────────────────┘   │
                         │         │ load policies                          │
                         │         ▼                                        │
                         │  ┌─────────────────────────────────────────┐    │
                         │  │         Policy Store                     │    │
                         │  │  (etcd / MySQL + change-data-capture)    │    │
                         │  └──────┬──────────────────────┬───────────┘    │
                         └─────────┼──────────────────────┼────────────────┘
                                   │                      │
                    ┌──────────────▼───┐    ┌─────────────▼──────────────┐
                    │  RBAC Service     │    │   ReBAC Service            │
                    │  (Role/Binding    │    │   (Zanzibar-like           │
                    │   CRUD, hierarchy │    │    relationship tuples)    │
                    │   resolution)     │    │                            │
                    └──────────┬───────┘    └─────────────┬──────────────┘
                               │                          │
                    ┌──────────▼──────────────────────────▼──────────────┐
                    │                   MySQL 8.0                         │
                    │  (roles, permissions, bindings, relationships)      │
                    └────────────────────────┬───────────────────────────┘
                                             │ CDC (Debezium)
                                             ▼
                    ┌────────────────────────────────────────────────────┐
                    │              Kafka (Audit + CDC topics)            │
                    └──────────┬─────────────────────────┬──────────────┘
                               │                         │
                    ┌──────────▼──────────┐   ┌──────────▼──────────────┐
                    │  Elasticsearch 8.x  │   │  Cold Storage (S3/HDFS) │
                    │  (Audit hot: 90d)   │   │  (Audit archive: 7yr)   │
                    └─────────────────────┘   └─────────────────────────┘

                    ┌────────────────────────────────────────────────────┐
                    │        Kubernetes API Server (webhook)             │
                    │  ┌──────────────────────────────────────────────┐  │
                    │  │  Admission Webhook ──▶ OPA/Gatekeeper        │  │
                    │  │  RBAC (native) ──▶ Role/ClusterRole          │  │
                    │  └──────────────────────────────────────────────┘  │
                    └────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Identity Provider** | Issues JWTs with user/group claims; source of truth for identity. |
| **API Gateway (Envoy)** | Intercepts every request; calls Authorization Gateway for allow/deny. |
| **Authorization Gateway** | Hosts OPA engine + decision cache; evaluates policies. |
| **Policy Engine (OPA)** | Evaluates Rego policies against request context. |
| **Decision Cache** | In-process LRU (100K entries) + Redis cluster for cross-node consistency. |
| **Policy Store** | etcd (for Kubernetes-native policies) or MySQL with CDC for platform policies. |
| **RBAC Service** | CRUD for roles, permissions, bindings; resolves role hierarchy. |
| **ReBAC Service** | Manages Zanzibar-like relationship tuples for object-level access. |
| **MySQL 8.0** | Stores all authorization data (roles, bindings, relationships). |
| **Kafka** | Streams audit events and CDC events for policy propagation. |
| **Elasticsearch** | Indexes audit logs for search (hot 90 days). |
| **OPA/Gatekeeper** | Kubernetes admission controller for policy enforcement on k8s resources. |

### Data Flows

1. **Request flow:** Client -> API Gateway -> Authorization Gateway (OPA evaluates policy against JWT claims + request context) -> allow/deny -> API Gateway forwards or rejects.
2. **Policy update flow:** Admin updates role/binding via RBAC Service -> MySQL -> CDC (Debezium) -> Kafka -> Authorization Gateway consumes change -> OPA policy bundle updated -> cache invalidated.
3. **Audit flow:** Every authorization decision -> Kafka audit topic -> Elasticsearch (hot) + S3 (cold).
4. **Kubernetes flow:** kubectl request -> API Server -> RBAC check (native) + Admission Webhook -> OPA/Gatekeeper -> allow/deny.

---

## 4. Data Model

### Core Entities & Schema (Full SQL)

```sql
-- ============================================================
-- ROLES
-- ============================================================
CREATE TABLE roles (
    role_id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    role_name        VARCHAR(255) NOT NULL,
    scope_type       ENUM('cluster', 'namespace', 'project', 'resource') NOT NULL,
    scope_value      VARCHAR(512) DEFAULT NULL COMMENT 'e.g., namespace name; NULL for cluster scope',
    description      TEXT,
    is_system        BOOLEAN NOT NULL DEFAULT FALSE,
    parent_role_id   BIGINT UNSIGNED DEFAULT NULL COMMENT 'for role hierarchy / inheritance',
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    created_by       VARCHAR(255) NOT NULL,
    UNIQUE KEY uk_role_scope (role_name, scope_type, scope_value),
    FOREIGN KEY (parent_role_id) REFERENCES roles(role_id) ON DELETE SET NULL,
    INDEX idx_parent (parent_role_id),
    INDEX idx_scope (scope_type, scope_value)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- PERMISSIONS
-- ============================================================
CREATE TABLE permissions (
    permission_id    BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    role_id          BIGINT UNSIGNED NOT NULL,
    api_group        VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'k8s API group or platform domain',
    resource         VARCHAR(255) NOT NULL COMMENT 'e.g., pods, nodes, jobs, volumes',
    verb             ENUM('get','list','watch','create','update','patch','delete','bind','escalate','*') NOT NULL,
    resource_names   JSON DEFAULT NULL COMMENT '["specific-pod-name"] or null for all',
    conditions       JSON DEFAULT NULL COMMENT 'ABAC-style conditions: {"labels":{"env":"prod"}}',
    effect           ENUM('allow', 'deny') NOT NULL DEFAULT 'allow',
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (role_id) REFERENCES roles(role_id) ON DELETE CASCADE,
    INDEX idx_role_resource (role_id, resource, verb),
    INDEX idx_resource_verb (resource, verb)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- SUBJECTS
-- ============================================================
CREATE TABLE subjects (
    subject_id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    subject_type     ENUM('user', 'group', 'service_account') NOT NULL,
    subject_name     VARCHAR(512) NOT NULL COMMENT 'e.g., user email, group DN, SA name',
    namespace        VARCHAR(255) DEFAULT NULL COMMENT 'for service accounts',
    external_id      VARCHAR(512) DEFAULT NULL COMMENT 'IdP unique identifier',
    is_active        BOOLEAN NOT NULL DEFAULT TRUE,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_subject (subject_type, subject_name, namespace),
    INDEX idx_external (external_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- ROLE BINDINGS
-- ============================================================
CREATE TABLE role_bindings (
    binding_id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    role_id          BIGINT UNSIGNED NOT NULL,
    subject_id       BIGINT UNSIGNED NOT NULL,
    scope_type       ENUM('cluster', 'namespace', 'project', 'resource') NOT NULL,
    scope_value      VARCHAR(512) DEFAULT NULL,
    expires_at       TIMESTAMP NULL DEFAULT NULL COMMENT 'for time-bound access',
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by       VARCHAR(255) NOT NULL,
    FOREIGN KEY (role_id) REFERENCES roles(role_id) ON DELETE CASCADE,
    FOREIGN KEY (subject_id) REFERENCES subjects(subject_id) ON DELETE CASCADE,
    UNIQUE KEY uk_binding (role_id, subject_id, scope_type, scope_value),
    INDEX idx_subject_scope (subject_id, scope_type, scope_value),
    INDEX idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- RELATIONSHIP TUPLES (Zanzibar-style ReBAC)
-- ============================================================
CREATE TABLE relationship_tuples (
    tuple_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    object_type      VARCHAR(255) NOT NULL COMMENT 'e.g., project, namespace, vm',
    object_id        VARCHAR(512) NOT NULL,
    relation         VARCHAR(128) NOT NULL COMMENT 'e.g., owner, editor, viewer, parent',
    subject_type     VARCHAR(255) NOT NULL COMMENT 'user, group, or object_type#relation',
    subject_id       VARCHAR(512) NOT NULL,
    subject_relation VARCHAR(128) DEFAULT NULL COMMENT 'for indirect: project:123#editor',
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    created_by       VARCHAR(255) NOT NULL,
    UNIQUE KEY uk_tuple (object_type, object_id, relation, subject_type, subject_id, subject_relation),
    INDEX idx_object (object_type, object_id),
    INDEX idx_subject (subject_type, subject_id),
    INDEX idx_relation (relation)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- AUTHORIZATION AUDIT LOG
-- ============================================================
CREATE TABLE authorization_audit_log (
    audit_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    timestamp_utc    TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    subject_type     VARCHAR(64) NOT NULL,
    subject_name     VARCHAR(512) NOT NULL,
    action           VARCHAR(64) NOT NULL,
    resource_type    VARCHAR(255) NOT NULL,
    resource_name    VARCHAR(512) DEFAULT NULL,
    scope_type       VARCHAR(64) NOT NULL,
    scope_value      VARCHAR(512) DEFAULT NULL,
    decision         ENUM('allow', 'deny') NOT NULL,
    matched_role     VARCHAR(255) DEFAULT NULL,
    matched_binding  BIGINT UNSIGNED DEFAULT NULL,
    evaluation_ms    DECIMAL(8,3) NOT NULL,
    request_id       VARCHAR(128) NOT NULL,
    source_ip        VARCHAR(45) DEFAULT NULL,
    user_agent       VARCHAR(512) DEFAULT NULL,
    INDEX idx_timestamp (timestamp_utc),
    INDEX idx_subject (subject_name, timestamp_utc),
    INDEX idx_decision (decision, timestamp_utc),
    INDEX idx_request (request_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (UNIX_TIMESTAMP(timestamp_utc)) (
    -- Partitions created daily by automation
);

-- ============================================================
-- OPA POLICY BUNDLES
-- ============================================================
CREATE TABLE opa_policy_bundles (
    bundle_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    bundle_name      VARCHAR(255) NOT NULL UNIQUE,
    rego_source      LONGTEXT NOT NULL,
    data_json        LONGTEXT DEFAULT NULL,
    version          INT UNSIGNED NOT NULL DEFAULT 1,
    is_active        BOOLEAN NOT NULL DEFAULT TRUE,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    updated_by       VARCHAR(255) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Store | Use Case | Justification |
|-------|----------|---------------|
| **MySQL 8.0** | Roles, bindings, relationships, policies | ACID transactions for permission changes; strong consistency required. |
| **etcd** | Kubernetes-native RBAC (Role, ClusterRole, RoleBinding, ClusterRoleBinding) | Already the k8s backing store; native watch for changes. |
| **Redis Cluster** | Decision cache | Sub-millisecond reads; TTL-based invalidation. |
| **Elasticsearch 8.x** | Audit log search (hot 90 days) | Full-text search on audit fields; aggregation for dashboards. |
| **S3 / HDFS** | Audit log archive (7 years) | Compliance retention at low cost. |
| **Kafka** | Event streaming (CDC + audit) | Durable, ordered delivery for policy propagation and audit ingestion. |

### Indexing Strategy

| Table | Key Index | Purpose |
|-------|-----------|---------|
| `roles` | `(role_name, scope_type, scope_value)` UNIQUE | Fast role lookup; prevents duplicates. |
| `permissions` | `(role_id, resource, verb)` | Permission check: "does role X allow verb Y on resource Z?" |
| `role_bindings` | `(subject_id, scope_type, scope_value)` | "What roles does subject X have in scope Y?" |
| `relationship_tuples` | `(object_type, object_id, relation)` | ReBAC check: "who has relation R on object O?" |
| `authorization_audit_log` | PARTITION by day + `(subject_name, timestamp_utc)` | Fast audit queries by subject within a time range. |

---

## 5. API Design

### REST Endpoints

```
# ── Role Management ──────────────────────────────────────────
POST   /api/v1/roles                         # Create role
GET    /api/v1/roles                         # List roles (filter: scope_type, scope_value)
GET    /api/v1/roles/{role_id}               # Get role detail
PUT    /api/v1/roles/{role_id}               # Update role
DELETE /api/v1/roles/{role_id}               # Delete role (fails if bindings exist)
GET    /api/v1/roles/{role_id}/permissions   # List permissions in role
POST   /api/v1/roles/{role_id}/permissions   # Add permission to role
DELETE /api/v1/roles/{role_id}/permissions/{perm_id}  # Remove permission

# ── Role Bindings ────────────────────────────────────────────
POST   /api/v1/bindings                      # Create binding
GET    /api/v1/bindings                      # List bindings (filter: subject, role, scope)
DELETE /api/v1/bindings/{binding_id}         # Revoke binding
POST   /api/v1/bindings/{binding_id}/renew   # Extend expiry

# ── Authorization Check ─────────────────────────────────────
POST   /api/v1/authorize                     # Check authorization
  Request body:
  {
    "subject": {"type": "service_account", "name": "job-scheduler", "namespace": "batch"},
    "action": "create",
    "resource": {"type": "pods", "api_group": "", "name": "", "namespace": "batch"},
    "context": {"labels": {"team": "data-eng"}}
  }
  Response:
  {
    "allowed": true,
    "reason": "matched role 'batch-operator' via binding 12345",
    "evaluation_ms": 0.42
  }

POST   /api/v1/authorize/batch               # Batch authorization (multiple checks)

# ── ReBAC ────────────────────────────────────────────────────
POST   /api/v1/relationships                 # Create relationship tuple
DELETE /api/v1/relationships                 # Delete relationship tuple
POST   /api/v1/relationships/check           # Check: does subject have relation on object?
POST   /api/v1/relationships/expand          # Expand: who has relation R on object O?
POST   /api/v1/relationships/lookup          # Lookup: what objects does subject S have relation R on?

# ── Audit ────────────────────────────────────────────────────
GET    /api/v1/audit/decisions               # Query audit log (filter: subject, resource, decision, time)
GET    /api/v1/audit/decisions/{request_id}  # Get specific decision by request ID

# ── OPA Policy Management ───────────────────────────────────
POST   /api/v1/policies                      # Upload Rego policy bundle
GET    /api/v1/policies                      # List active policies
GET    /api/v1/policies/{bundle_id}          # Get policy source
PUT    /api/v1/policies/{bundle_id}          # Update policy
POST   /api/v1/policies/{bundle_id}/test     # Dry-run policy against test inputs

# ── Break-Glass ─────────────────────────────────────────────
POST   /api/v1/break-glass                   # Emergency elevated access (requires MFA + approval)
```

### CLI

```bash
# ── Role Management ──
platform-authz role create --name batch-operator --scope namespace:batch \
  --permissions '[{"resource":"pods","verb":"*"},{"resource":"jobs","verb":"*"}]'

platform-authz role list --scope-type namespace --scope-value batch
platform-authz role describe batch-operator
platform-authz role add-permission batch-operator --resource configmaps --verb get
platform-authz role delete batch-operator

# ── Bindings ──
platform-authz bind --role batch-operator --subject-type service_account \
  --subject job-scheduler --namespace batch --expires 24h

platform-authz bindings list --subject job-scheduler
platform-authz bindings revoke --binding-id 12345

# ── Authorization Check ──
platform-authz check --subject-type user --subject alice@corp.com \
  --action delete --resource pods --namespace production

# ── ReBAC ──
platform-authz relationship create --object project:infra-core \
  --relation editor --subject user:alice@corp.com

platform-authz relationship check --object namespace:prod-infra \
  --relation viewer --subject user:bob@corp.com

# ── Audit ──
platform-authz audit query --subject alice@corp.com --decision deny --last 24h
platform-authz audit export --start 2026-01-01 --end 2026-01-31 --format csv

# ── OPA ──
platform-authz policy upload --name no-privileged-pods --file policy.rego
platform-authz policy test --name no-privileged-pods --input test-pod.json

# ── Break-Glass ──
platform-authz break-glass --reason "P0 incident INC-4521" --duration 2h --mfa-token 123456

# ── Kubernetes native (kubectl) ──
kubectl create role batch-reader --verb=get,list --resource=pods -n batch
kubectl create rolebinding batch-reader-binding --role=batch-reader \
  --serviceaccount=batch:job-scheduler -n batch
kubectl auth can-i create pods -n batch --as=system:serviceaccount:batch:job-scheduler
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Policy Evaluation Engine (OPA Integration)

**Why it's hard:**
Policy evaluation sits on the critical path of every API request. It must be sub-millisecond for cached decisions, support complex rule compositions (RBAC + ABAC + ReBAC), and propagate policy changes within seconds -- all while handling 500K decisions/sec.

| Approach | Latency | Complexity | Flexibility | Consistency |
|----------|---------|------------|-------------|-------------|
| **Embedded OPA (sidecar/library)** | < 1 ms (in-process) | Medium | High (Rego) | Eventually consistent (bundle sync) |
| **Central OPA service** | 1-5 ms (network hop) | Low | High | Strongly consistent |
| **Custom policy engine** | < 0.5 ms | Very High | Low (custom DSL) | Depends on impl |
| **Casbin library** | < 1 ms | Low | Medium (model DSL) | In-process only |

**Selected: Embedded OPA with bundle sync + local cache**

**Justification:** OPA as a Go library embedded in the authorization sidecar gives in-process evaluation speed (sub-millisecond) with Rego's full expressiveness. Policy bundles are synced via Kafka CDC, giving near-real-time propagation without the latency of a central service.

**Implementation Detail:**

```
Authorization Sidecar (per node):
┌─────────────────────────────────────────────┐
│  Envoy ext_authz filter                      │
│       │                                      │
│       ▼                                      │
│  ┌─────────────────────────────────────┐    │
│  │  OPA Engine (embedded, Go library)   │    │
│  │  ┌───────────────────────────────┐  │    │
│  │  │ Policy Bundle (Rego rules)     │  │    │
│  │  │ - rbac.rego                    │  │    │
│  │  │ - abac.rego                    │  │    │
│  │  │ - rebac.rego                   │  │    │
│  │  └───────────────────────────────┘  │    │
│  │  ┌───────────────────────────────┐  │    │
│  │  │ Data (roles, bindings, tuples) │  │    │
│  │  └───────────────────────────────┘  │    │
│  └─────────────────────────────────────┘    │
│       │                                      │
│       ▼                                      │
│  ┌─────────────────────────────────────┐    │
│  │  Decision Cache (LRU, 100K entries)  │    │
│  │  Key: hash(subject+action+resource)  │    │
│  │  TTL: 60 seconds                     │    │
│  └─────────────────────────────────────┘    │
│       │                                      │
│       ▼                                      │
│  Kafka Consumer (policy CDC events)          │
│  - Incremental bundle updates                │
│  - Cache invalidation on policy change       │
└─────────────────────────────────────────────┘
```

**Rego policy example (RBAC):**
```rego
package platform.authz

import future.keywords.in

default allow := false

# RBAC: check if subject has a role binding that permits the action
allow {
    some binding in data.bindings
    binding.subject_name == input.subject.name
    binding.subject_type == input.subject.type
    scope_matches(binding, input.resource)
    some perm in data.roles[binding.role_id].permissions
    perm.resource == input.resource.type
    perm.verb in [input.action, "*"]
}

# Role hierarchy: inherit permissions from parent roles
allow {
    some binding in data.bindings
    binding.subject_name == input.subject.name
    ancestor_role := get_ancestor_roles(binding.role_id)
    some perm in data.roles[ancestor_role].permissions
    perm.resource == input.resource.type
    perm.verb in [input.action, "*"]
}

# Deny rules override allow
deny {
    some binding in data.bindings
    binding.subject_name == input.subject.name
    some perm in data.roles[binding.role_id].permissions
    perm.effect == "deny"
    perm.resource == input.resource.type
    perm.verb in [input.action, "*"]
}

# Final decision: allow only if not denied
authz_decision := "allow" {
    allow
    not deny
}

authz_decision := "deny" {
    not allow
}

authz_decision := "deny" {
    deny
}
```

**Failure Modes:**
- **OPA crash:** Envoy falls back to deny-all (fail-closed). Health check triggers restart.
- **Stale policy data:** Kafka consumer lag monitor alerts if policy propagation exceeds 5 seconds.
- **Cache poisoning:** Decision cache uses short TTL (60s) and is invalidated on any policy CDC event.
- **Rego policy error:** Policy uploads go through `opa test` before activation; rollback on failure.

**Interviewer Q&As:**

**Q1: How do you handle policy evaluation when the OPA sidecar is overloaded?**
A: We implement circuit-breaking in Envoy. If the ext_authz call exceeds 5 ms p99 for 10 consecutive seconds, Envoy trips the circuit and fails closed (deny all). This protects the data path. We also auto-scale sidecars -- on bare metal, we run multiple OPA instances per node behind a local load balancer and scale based on CPU utilization.

**Q2: How do you ensure Rego policies don't have bugs that lock everyone out?**
A: Three-layer protection: (1) All policy changes go through `opa test` with a mandatory test suite before merge. (2) Canary deployment: new policies are deployed to 1% of nodes first, and we monitor deny-rate spikes. (3) Break-glass: a separate path bypasses OPA entirely (direct k8s auth) with mandatory audit.

**Q3: What happens if Kafka is down and policy changes can't propagate?**
A: OPA sidecars continue operating with their last known policy bundle. A watchdog timer detects staleness > 5 minutes and emits a critical alert. We also maintain a secondary sync path: OPA can pull bundles from an S3-backed bundle server as a fallback.

**Q4: How do you debug an unexpected deny?**
A: The audit log captures the full evaluation trace (matched rules, intermediate decisions). We also expose `POST /api/v1/authorize` in dry-run mode with `?explain=full` that returns the OPA decision trace showing which Rego rules fired.

**Q5: How do you handle the bootstrapping problem -- how does OPA get its initial policy bundle?**
A: On startup, the sidecar pulls the full bundle from an S3-hosted bundle server (signed and versioned). It will not serve any traffic until the bundle is loaded (readiness probe fails). Kafka CDC is used for incremental updates after initial load.

**Q6: What is the cache key design for the decision cache?**
A: Key = SHA-256(subject_type + subject_name + action + resource_type + resource_name + scope). We avoid including mutable context (like labels) in the cache key because that would make the cache useless. For ABAC conditions, we skip caching and always evaluate live.

---

### Deep Dive 2: Kubernetes RBAC and Service Account Token Management

**Why it's hard:**
Kubernetes has its own built-in RBAC system (Role, ClusterRole, RoleBinding, ClusterRoleBinding) that operates independently from the platform's authorization system. We must unify them without creating conflicting decisions. Additionally, service account tokens have evolved significantly (projected volumes, audience-bound tokens via TokenRequest API), and misconfigured tokens are a top attack vector.

| Approach | Consistency | Operational Overhead | Flexibility |
|----------|-------------|---------------------|-------------|
| **Kubernetes RBAC only** | Built-in, consistent | Low | Limited (no ABAC, no ReBAC) |
| **Replace k8s RBAC with webhook** | Fully custom | High (must handle all k8s authz) | Maximum |
| **Layer on top: k8s RBAC + admission webhook (OPA/Gatekeeper)** | Dual layer | Medium | High |
| **Sync platform RBAC to k8s RBAC objects** | Eventually consistent | Medium | High |

**Selected: Sync platform RBAC to k8s RBAC objects + OPA/Gatekeeper admission webhook**

**Justification:** We keep Kubernetes native RBAC for performance (it's compiled into the API server) and supplement it with OPA/Gatekeeper for policies that k8s RBAC cannot express (label-based restrictions, time-based access, cross-resource constraints). Platform RBAC changes are synced to k8s Role/RoleBinding objects by a controller.

**Implementation Detail:**

```
Platform RBAC Service                  Kubernetes Cluster
┌──────────────────┐                  ┌──────────────────────────────┐
│  role: batch-op  │   sync           │  Role: batch-op              │
│  permissions:    │──controller──▶   │    rules:                    │
│    pods: *       │                  │    - apiGroups: [""]          │
│    jobs: *       │                  │      resources: ["pods"]      │
│                  │                  │      verbs: ["*"]             │
│  binding:        │   sync           │  RoleBinding: batch-op-bind  │
│    SA: job-sched │──controller──▶   │    roleRef: batch-op         │
│    ns: batch     │                  │    subjects:                  │
└──────────────────┘                  │    - kind: ServiceAccount     │
                                      │      name: job-scheduler      │
                                      │      namespace: batch         │
                                      └──────────────────────────────┘

                                      OPA/Gatekeeper:
                                      ┌──────────────────────────────┐
                                      │  ConstraintTemplate:          │
                                      │    K8sRequiredLabels          │
                                      │  Constraint:                  │
                                      │    must have "team" label     │
                                      │    on all pods in "prod-*"    │
                                      │    namespaces                 │
                                      └──────────────────────────────┘
```

**Service Account Token Best Practices:**

```yaml
# Projected volume token (Kubernetes 1.20+, audience-bound)
apiVersion: v1
kind: Pod
spec:
  serviceAccountName: job-scheduler
  containers:
  - name: worker
    volumeMounts:
    - name: token
      mountPath: /var/run/secrets/tokens
      readOnly: true
  volumes:
  - name: token
    projected:
      sources:
      - serviceAccountToken:
          path: token
          expirationSeconds: 3600    # 1 hour TTL
          audience: "https://platform.internal"  # audience-bound
```

```bash
# TokenRequest API (programmatic, short-lived)
kubectl create token job-scheduler \
  --namespace batch \
  --audience "https://vault.internal" \
  --duration 600s   # 10 minutes
```

**Failure Modes:**
- **Sync controller lag:** k8s RBAC diverges from platform RBAC. Periodic full reconciliation every 5 minutes catches drift. Alert on divergence.
- **Stale SA token:** Legacy automount tokens never expire. We enforce `automountServiceAccountToken: false` via Gatekeeper and require projected volumes.
- **Token theft:** Short TTL (1 hour) + audience binding limits blast radius. Token must match the expected audience in the resource server.
- **Gatekeeper webhook timeout:** k8s API server has a 10-second webhook timeout. If Gatekeeper is slow, the webhook is configured with `failurePolicy: Fail` (deny on timeout) for production namespaces and `Ignore` for system namespaces.

**Interviewer Q&As:**

**Q1: Why not replace Kubernetes RBAC entirely with a webhook authorizer?**
A: Performance. The k8s API server makes hundreds of authorization checks per second for internal controllers (kubelet, scheduler, controller-manager). A webhook adds network latency to every check. Native RBAC is evaluated in-process. We only use webhooks for admission (validating/mutating) where we need policy expressiveness beyond RBAC.

**Q2: How do you prevent privilege escalation through role creation?**
A: Kubernetes has a built-in escalation prevention: a user cannot create a RoleBinding that grants permissions they don't already have, unless they have the `bind` verb on that role. We enforce the same in the platform RBAC service. Additionally, Gatekeeper constraints block creation of ClusterRoleBindings to `cluster-admin` except by a specific set of admin subjects.

**Q3: How do you handle the transition from legacy SA tokens to projected volumes?**
A: (1) Gatekeeper constraint blocks new pods with `automountServiceAccountToken: true`. (2) We instrument the TokenReview API to log all legacy token usage. (3) A migration dashboard shows per-namespace progress. (4) After 30-day warning period, we revoke legacy tokens by rotating the SA signing key.

**Q4: What is audience binding and why does it matter?**
A: A projected SA token includes an `aud` (audience) claim. A token issued with `audience: "https://vault.internal"` will be rejected by the Kubernetes API server (which expects its own audience). This prevents a token stolen from one service from being replayed against another service. It is critical defense-in-depth for workload identity.

**Q5: How do you handle multi-cluster RBAC?**
A: We use a federation controller that syncs platform RBAC to each cluster's k8s RBAC. Each cluster has a local sync agent that watches the central MySQL (via Kafka CDC) and reconciles Role/RoleBinding objects. Cluster-specific overrides are supported via scope annotations.

**Q6: How do you audit what a service account can actually do (effective permissions)?**
A: `kubectl auth can-i --list --as=system:serviceaccount:batch:job-scheduler -n batch` gives the native k8s view. Our platform CLI `platform-authz effective-permissions --subject-type service_account --subject job-scheduler --namespace batch` merges k8s RBAC + platform RBAC + ReBAC into a unified view.

---

### Deep Dive 3: Zanzibar-Style Relationship-Based Access Control (ReBAC)

**Why it's hard:**
RBAC answers "does this user have this role?" but cannot answer "does this user own this specific project?" or "does this user belong to a team that manages this namespace?" ReBAC (as pioneered by Google Zanzibar) models authorization as a graph of relationships. The challenge is evaluating transitive relationships (e.g., "user is member of team, team owns project, project contains namespace") at sub-millisecond latency.

| Approach | Transitive Relations | Latency | Operational Cost |
|----------|---------------------|---------|-----------------|
| **Direct SQL joins** | Possible but slow at depth | 5-50 ms | Low |
| **Materialized graph (pre-computed)** | Fast reads | < 1 ms | High (write amplification) |
| **On-demand graph traversal with cache** | Flexible | 1-5 ms first, < 1 ms cached | Medium |
| **Dedicated Zanzibar service (SpiceDB/OpenFGA)** | Native support | < 2 ms | Medium |

**Selected: Dedicated SpiceDB + platform integration**

**Justification:** SpiceDB (open-source Zanzibar implementation) provides native graph traversal with built-in caching, check/expand/lookup APIs, and a schema language. It avoids the complexity of building a graph engine from scratch while offering production-grade performance.

**Implementation Detail:**

```
Schema (SpiceDB DSL):
────────────────────
definition user {}

definition team {
    relation member: user
    relation admin: user
    permission manage = admin
    permission view = member + admin
}

definition project {
    relation owner_team: team
    relation editor: user | team#member
    relation viewer: user | team#member
    permission edit = editor + owner_team->manage
    permission view = viewer + editor + owner_team->view
}

definition namespace {
    relation parent_project: project
    relation admin: user | team#admin
    permission manage = admin + parent_project->edit
    permission view = parent_project->view
}

Relationship Tuples:
────────────────────
team:infra-core#member@user:alice@corp.com
team:infra-core#admin@user:bob@corp.com
project:cloud-platform#owner_team@team:infra-core
namespace:prod-infra#parent_project@project:cloud-platform

Check: can alice view namespace:prod-infra?
  → namespace:prod-infra#parent_project → project:cloud-platform
  → project:cloud-platform#owner_team → team:infra-core
  → team:infra-core#member → user:alice@corp.com
  → alice has view permission via team membership ✓
```

**Failure Modes:**
- **Graph cycle:** SpiceDB's schema validation rejects cycles at schema definition time.
- **Deep transitive chain:** Max depth limit (default 25 hops). Beyond that, flatten the relationship.
- **SpiceDB unavailability:** Fall back to RBAC-only (which is more permissive by design). Alert immediately.
- **Stale cache after relationship change:** SpiceDB uses Zookies (opaque consistency tokens) to ensure reads-after-writes are consistent.

**Interviewer Q&As:**

**Q1: Why not just use RBAC with fine-grained roles instead of ReBAC?**
A: Role explosion. If you have 10,000 projects and need per-project ownership, you'd need 10,000 roles and 10,000 bindings per user. ReBAC models this as one relationship tuple per user-project pair and evaluates transitively. It scales linearly with relationships, not quadratically with roles.

**Q2: How does ReBAC interact with RBAC in the authorization decision?**
A: The OPA policy engine combines both: `allow { rbac_allow } OR allow { rebac_allow }`. RBAC handles broad platform roles (admin, operator, viewer). ReBAC handles object-level permissions (owner of project X, editor of namespace Y). Deny rules from either system override.

**Q3: How do you handle consistency when a team membership changes?**
A: SpiceDB supports "Zookie-based" consistency. After writing a relationship change, the write returns a Zookie token. Subsequent checks include this Zookie to ensure they see the updated state. For cache invalidation across nodes, we use SpiceDB's built-in watch API to stream changes and invalidate local caches.

**Q4: What happens if the relationship graph becomes very large (millions of tuples)?**
A: SpiceDB shards relationship tuples by object_type and object_id using consistent hashing. It pre-computes common transitive paths using background workers. At Google scale, Zanzibar handles trillions of tuples. Our projected 200K-500K tuples are well within comfortable range.

**Q5: How do you migrate existing RBAC-only permissions to include ReBAC?**
A: Additive migration. Phase 1: Deploy SpiceDB alongside existing RBAC. Phase 2: Model high-value relationships (project ownership, team membership). Phase 3: The OPA policy begins checking ReBAC for specific resource types. Phase 4: Gradually move more resource types to ReBAC. At no point do we remove RBAC -- it remains the baseline.

**Q6: How do you test authorization policies before deploying?**
A: SpiceDB supports `CheckPermission` with a `consistency: minimize_latency` option for testing. We also use `zed validate` to check schema correctness and `zed testserver` for integration tests. OPA policies are tested with `opa test` against a fixture set of 500+ test cases covering edge cases.

---

### Deep Dive 4: Audit Trail for Authorization Decisions

**Why it's hard:**
Every authorization decision must be logged -- 500K decisions/sec producing ~100 MB/sec of audit data. Losing audit events is a compliance violation. The audit system must not add latency to the authorization critical path.

| Approach | Reliability | Latency Impact | Cost |
|----------|-------------|----------------|------|
| **Synchronous DB write** | High | +5-10 ms per decision | Very High (DB bottleneck) |
| **Async Kafka publish** | High (with acks=all) | < 0.1 ms (fire-and-forget locally) | Medium |
| **Local buffer + batch ship** | Medium (lose buffer on crash) | Zero | Low |
| **Sidecar log file + Fluentd** | Medium | Zero | Low |

**Selected: Async Kafka publish with local buffer fallback**

**Justification:** Kafka with `acks=1` (leader ack) gives < 0.1 ms publish latency with high durability. A local ring buffer holds events if Kafka is unreachable, and flushes when connectivity resumes. This ensures zero latency impact on the authorization path while maintaining audit completeness.

**Implementation Detail:**
- OPA sidecar publishes decision to a local ring buffer (in-memory, 100K events).
- A background thread batch-publishes to Kafka topic `authz.audit.decisions` every 100 ms or when buffer reaches 1,000 events.
- Kafka consumer writes to Elasticsearch (hot, 90-day retention) and S3 (cold, 7-year retention).
- If Kafka is down, ring buffer persists to a local WAL file (append-only, 1 GB max). On recovery, WAL is replayed.

**Failure Modes:**
- **Kafka down:** Local WAL absorbs up to 1 GB (~5 million events). If WAL fills, oldest events are dropped with a CRITICAL alert.
- **Elasticsearch ingestion lag:** Kafka topic has 7-day retention. If ES catches up within 7 days, no data loss.
- **Disk failure on WAL:** WAL is on a separate disk from the OS. If both fail, the node is already unhealthy and would be drained.

**Interviewer Q&As:**

**Q1: How do you guarantee no audit events are lost?**
A: Defense in depth: (1) Kafka `acks=1` with 3x replication. (2) Local WAL as fallback. (3) Reconciliation job compares decision count from OPA metrics against Kafka consumer offset and Elasticsearch document count. Discrepancies trigger alerts.

**Q2: How do you handle audit log tampering?**
A: (1) Kafka topics are append-only (no deletes except by retention policy). (2) Audit events include an HMAC signature using a key from Vault. (3) S3 archive uses object lock (WORM) for compliance. (4) Elasticsearch indices are read-only after the day closes.

**Q3: How do you query audit logs efficiently at this scale?**
A: Elasticsearch time-based indices (`authz-audit-2026.04.09`) with ILM policy. Common queries (by subject, by resource, by decision) have dedicated indices via Elasticsearch index templates. For complex analytics, data is also available in the data warehouse (Hive/Presto) via S3.

**Q4: What is the audit event schema?**
A: `{timestamp, request_id, subject_type, subject_name, action, resource_type, resource_name, scope, decision, matched_role, matched_binding, evaluation_ms, source_ip, user_agent, trace_id}`. The trace_id links to the distributed trace for the originating request.

**Q5: How do you handle PII in audit logs (e.g., user emails)?**
A: User emails are pseudonymized in the Elasticsearch index using a one-way hash. The mapping from hash to email is stored in a separate, access-controlled table. Only authorized auditors can de-pseudonymize. S3 archive retains the original for legal hold.

**Q6: What is the retention strategy?**
A: Hot (Elasticsearch): 90 days, optimized for search. Warm (S3 Standard): 1 year, for ad-hoc queries via Athena. Cold (S3 Glacier): 7 years, for compliance. Automatic ILM transitions.

---

## 7. Scaling Strategy

**Horizontal scaling of the authorization path:**
- OPA sidecars run on every node (DaemonSet). Each sidecar handles only local traffic. No central bottleneck.
- Decision cache is per-node (in-process LRU). No cross-node cache coordination needed for reads.
- Policy bundles are pushed via Kafka to all sidecars independently.

**Scaling the RBAC/ReBAC service:**
- MySQL read replicas for authorization queries (reads are 99.9% of traffic).
- SpiceDB horizontally scales with consistent hashing of relationship tuples.
- Write path (role/binding changes) is low volume (~100 writes/sec) and goes to MySQL primary.

**Scaling audit ingestion:**
- Kafka topic partitioned by subject_name hash (64 partitions). Parallelizes Elasticsearch ingestion.
- Elasticsearch scales by adding data nodes. Time-based indices allow independent scaling of hot vs. warm storage.

**Interviewer Q&As:**

**Q1: What is the bottleneck in this system?**
A: The audit ingestion pipeline. At 500K events/sec, Elasticsearch must index ~100 MB/sec. We mitigate by batching (1,000 events per bulk request), using time-based indices (one per day), and scaling ES data nodes horizontally. The authorization decision path itself is not a bottleneck because it's fully distributed (per-node sidecars).

**Q2: How do you handle a new cluster joining the fleet?**
A: The sync controller detects the new cluster via a cluster registry. It performs a full sync of all platform RBAC -> k8s RBAC objects. OPA sidecars on new nodes pull the full policy bundle on startup. The process is automated and takes < 5 minutes.

**Q3: How do you handle role/binding growth to millions of entries?**
A: MySQL partitions the `role_bindings` table by `scope_type`. OPA data bundles are sharded by namespace -- each sidecar only loads policies relevant to its node's namespaces. This keeps the per-sidecar data size under 100 MB even at 1M+ bindings.

**Q4: What if policy evaluation latency degrades?**
A: (1) Monitor p99 latency per sidecar via Prometheus. (2) If p99 exceeds 2 ms, alert fires. (3) Common causes: oversized policy bundle (optimize Rego, remove unused rules), cache miss rate spike (increase cache size), or CPU contention (check node utilization). (4) As a last resort, shed load by failing open for non-critical namespaces (configurable per namespace risk tier).

**Q5: How do you handle multi-region authorization?**
A: Each region has a full replica of the authorization system (MySQL read replica, local Kafka cluster, local OPA sidecars). Policy changes are written to the primary region and replicated via MySQL semi-synchronous replication + Kafka MirrorMaker. Latency for policy propagation across regions is < 5 seconds.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---------|--------|-----------|------------|-----|
| **OPA sidecar crash** | Authorization fails for local pods | Envoy health check (2s interval) | Envoy returns deny (fail-closed). Kubelet restarts sidecar. | < 10s |
| **Kafka cluster down** | Audit events queue locally; policy updates stall | Kafka consumer lag monitor; local WAL growth alert | Local WAL absorbs audit events. OPA sidecars use last-known policy. | < 5 min (Kafka recovery) |
| **MySQL primary down** | No role/binding writes; reads continue from replicas | MySQL heartbeat + ProxySQL health check | Automated failover to synchronous replica (orchestrator/MHA). | < 30s |
| **Redis cache down** | Cross-node cache misses; OPA evaluates from bundle data | Redis sentinel + Prometheus | In-process LRU cache still works. Cold evaluation adds ~1 ms. | < 15s |
| **SpiceDB down** | ReBAC checks fail; RBAC still works | gRPC health check | OPA policy falls back to RBAC-only for affected resources. Alert. | < 30s |
| **Elasticsearch down** | Audit search unavailable; ingestion queues in Kafka | ES cluster health API | Kafka retains 7 days. Once ES recovers, consumer catches up. | < 1h |
| **Policy bundle corruption** | OPA rejects corrupted bundle; continues with last good | OPA bundle validation (checksum) | Rollback to previous bundle version. Alert. | Immediate |
| **Kafka consumer lag > 5 min** | Policy propagation delayed; stale decisions | Consumer lag metric | Scale consumer group. If persistent, switch to S3 bundle pull. | < 10 min |
| **Full network partition** | Node isolated; no policy updates | Node heartbeat | Sidecar operates with cached policy. Deny new access after staleness threshold (30 min). | Depends on partition |
| **Compromised service account** | Unauthorized access with stolen token | Anomaly detection on audit log | Immediate token revocation via TokenRequest API. Rotate SA. | < 5 min (manual) |

---

## 9. Security

### Authentication Chain

```
User Request Flow:
  Browser/CLI → OIDC Login → IdP (Okta/Azure AD) → JWT issued
  JWT → API Gateway (Envoy) → JWT verification (JWKS endpoint)
  Verified claims: sub, email, groups, aud, exp

Service-to-Service Flow:
  Pod → Projected SA Token (audience-bound, 1h TTL)
  → Target service validates token via TokenReview API
  → Or: mTLS with SPIFFE SVID (see certificate_lifecycle_management.md)

OpenStack Flow:
  User → Keystone Auth (username/password or application credential)
  → Keystone token (Fernet) → Passed to OpenStack services
  → Keystone policy.json evaluated per-service
```

### Authorization Model

```
Three-layer authorization:

Layer 1: RBAC (coarse-grained)
  - Platform roles: admin, operator, developer, viewer
  - Scoped: cluster, namespace, project
  - Enforced by: OPA sidecar

Layer 2: ABAC (attribute-based conditions)
  - Conditions on roles: "only allow if resource has label env=dev"
  - Time-based: "allow only during business hours"
  - Enforced by: OPA Rego conditions

Layer 3: ReBAC (fine-grained, object-level)
  - Relationship tuples: user X is owner of project Y
  - Transitive: team membership → project access → namespace access
  - Enforced by: SpiceDB via OPA external data

Decision logic:
  IF any deny rule matches → DENY
  ELSE IF (rbac_allow OR rebac_allow) AND abac_conditions_met → ALLOW
  ELSE → DENY (default)
```

### Audit Trail

- **What is logged:** Every authorization decision (allow and deny), including subject, action, resource, scope, matched rule, evaluation time, request ID, source IP.
- **Where:** Kafka -> Elasticsearch (hot 90 days) -> S3 (cold 7 years).
- **Integrity:** HMAC signature on each event. S3 object lock (WORM) for tamper evidence.
- **Access to audit logs:** Separate RBAC role (`audit-reader`); no one with `audit-reader` can also modify authorization policies (separation of duties).
- **Alerting:** Real-time alerts on: deny rate spike (> 2x baseline), break-glass usage, privilege escalation attempts, access from unusual IPs.

### Threat Model

| Threat | Likelihood | Impact | Mitigation |
|--------|------------|--------|------------|
| **Privilege escalation via role creation** | Medium | Critical | k8s escalation prevention + platform-side check: cannot grant permissions you don't have. |
| **Stolen service account token** | High | High | Short TTL (1h), audience binding, anomaly detection on usage patterns. |
| **Policy bypass via direct etcd access** | Low | Critical | etcd encrypted at rest, mTLS between API server and etcd, no direct etcd access for users. |
| **Insider threat: admin modifies own permissions** | Medium | Critical | Separation of duties: permission changes require approval from a different admin. All changes audited. |
| **Denial of service on authorization path** | Medium | Critical | Per-node sidecars (no central bottleneck), rate limiting per subject, circuit breaker. |
| **Audit log tampering** | Low | High | Append-only Kafka, HMAC signatures, S3 WORM. |
| **Stale deny after permission grant** | Medium | Low | Short cache TTL (60s), explicit cache invalidation on permission change via CDC. |
| **OPA policy injection** | Low | Critical | Policy changes require code review + CI/CD pipeline. No runtime policy modification. |

---

## 10. Incremental Rollout

**Phase 1 (Week 1-2): Foundation**
- Deploy MySQL schema, RBAC service, basic CRUD.
- Migrate existing permission data from ad-hoc configs.
- OPA sidecar deployed in audit-only mode (log decisions but don't enforce).

**Phase 2 (Week 3-4): Enforcement on non-production**
- Enable OPA enforcement on dev/staging namespaces.
- Deploy Gatekeeper constraints for pod security.
- Monitor deny rates, tune policies.

**Phase 3 (Week 5-6): Production rollout**
- Enable OPA enforcement on production, one namespace at a time.
- Break-glass procedure tested and documented.
- Audit pipeline (Kafka -> ES) fully operational.

**Phase 4 (Week 7-8): ReBAC integration**
- Deploy SpiceDB.
- Model project/team ownership relationships.
- OPA policies begin checking ReBAC for project-scoped resources.

**Phase 5 (Week 9-10): Advanced features**
- Time-based access (expires_at on bindings).
- ABAC conditions in Rego.
- Multi-cluster sync.

**Interviewer Q&As:**

**Q1: How do you handle the transition without breaking existing access?**
A: Audit-only mode in Phase 1. OPA logs what it would deny but doesn't actually deny. We analyze the logs to identify missing roles/bindings. Only after the deny rate in audit-only mode drops to zero do we switch to enforcement.

**Q2: What if a critical service breaks when you enable enforcement?**
A: (1) Break-glass: `platform-authz break-glass --reason "INC-1234" --duration 2h` grants temporary cluster-admin. (2) Rollback: disable OPA enforcement for that namespace by setting `envoy.ext_authz.enabled: false` in the namespace's Envoy config. Takes effect in < 30 seconds.

**Q3: How do you test authorization changes before deploying?**
A: (1) Unit tests: `opa test` with fixture data in CI. (2) Integration tests: spin up a test k8s cluster with known roles/bindings, run `kubectl auth can-i` checks. (3) Staging: full copy of production policies in staging environment. (4) Canary: new policies deployed to 1% of nodes first.

**Q4: How do you communicate authorization changes to users?**
A: (1) Self-service UI shows each user their effective permissions. (2) `platform-authz whoami` CLI command. (3) Pre-rollout email to namespace owners listing any permissions that will change. (4) Slack bot that notifies users when they receive deny decisions with instructions to request access.

**Q5: How do you handle emergency rollback of a bad policy?**
A: OPA bundles are versioned. `platform-authz policy rollback --bundle-id X --to-version N` pushes the previous version via Kafka. Propagation takes < 5 seconds. Additionally, each node's sidecar has the previous bundle version cached locally, so a `SIGHUP` to the sidecar triggers an immediate rollback to the local cache.

---

## 11. Trade-offs & Decision Log

| Decision | Trade-off | Rationale |
|----------|-----------|-----------|
| **Embedded OPA (sidecar) vs. central OPA service** | Duplicated policy data on every node vs. lower latency | Sub-millisecond latency on the authz critical path outweighs the ~100 MB/node memory cost. |
| **Fail-closed (deny on OPA failure) vs. fail-open** | Availability risk vs. security risk | For a security system, fail-closed is the correct default. Break-glass provides escape hatch. |
| **Kafka for audit vs. direct ES write** | Additional infrastructure vs. durability | Kafka decouples the authorization path from ES availability. If ES is slow, authz is unaffected. |
| **SpiceDB for ReBAC vs. custom graph engine** | External dependency vs. development time | SpiceDB is battle-tested (Zanzibar model) and saves 6+ months of engineering. |
| **MySQL for policy store vs. etcd for everything** | Two stores to manage vs. k8s coupling | MySQL provides richer queries for platform RBAC (joins, aggregations) that etcd cannot. |
| **Short cache TTL (60s) vs. longer TTL** | Higher cache miss rate vs. faster policy propagation | 60s is acceptable because cold evaluation is still < 5 ms. Faster propagation reduces the window of stale decisions. |
| **Sync platform RBAC to k8s RBAC vs. webhook-only** | Eventual consistency vs. native performance | k8s native RBAC is faster and doesn't add latency to every API server call. Sync lag < 2s is acceptable. |
| **HMAC on audit events vs. plain events** | CPU overhead vs. tamper evidence | HMAC adds < 0.01 ms per event. Tamper evidence is a compliance requirement. |

---

## 12. Agentic AI Integration

### AI-Powered Authorization Intelligence

**1. Anomaly Detection on Authorization Patterns**
- Train an anomaly detection model on the audit log: per-subject access patterns (time of day, resource types, action frequency).
- Flag unusual patterns: a service account that normally does read-only suddenly issuing deletes; a user accessing resources outside their normal project scope.
- Integration: Elasticsearch ML jobs + custom Python model. Alerts fed to security team via PagerDuty.

**2. Automated Least-Privilege Recommendation**
- An agent periodically analyzes role bindings vs. actual usage (from audit logs).
- Recommends removing unused permissions: "Service account X has `pods/*` but only used `pods/get` and `pods/list` in the last 90 days. Recommend scoping to read-only."
- CLI: `platform-authz ai recommend --subject job-scheduler --namespace batch`
- Requires human approval before applying.

**3. Natural Language Policy Authoring**
- Engineer describes a policy in English: "Allow the data-eng team to create and delete Spark jobs in the batch namespace, but only during business hours US-Pacific."
- LLM agent generates the Rego policy + role definition + binding.
- Agent runs `opa test` against generated policy with standard test fixtures.
- Output requires human review and approval before deployment.

**4. Incident Response Automation**
- On detection of compromised credentials (e.g., token used from unusual IP):
  - Agent automatically revokes the service account token.
  - Agent creates a temporary replacement token with reduced permissions.
  - Agent opens an incident ticket with full audit trail attached.
  - Agent notifies the on-call engineer with a summary.

**5. Policy Drift Detection**
- Agent compares intended state (Git-committed policies) against actual state (OPA bundles deployed, k8s RBAC objects).
- Reports drift: "ClusterRole `node-reader` in cluster us-east-1 has 3 extra permissions not in Git."
- Can auto-remediate by re-syncing from Git (with approval gate).

**6. Access Request Triage**
- Users request access via a Slack bot or portal.
- AI agent evaluates: (1) Does the user's team typically have this access? (2) Is the request consistent with their role? (3) Are there similar approved requests?
- Auto-approves low-risk requests (e.g., read-only access to non-production). Escalates high-risk requests (e.g., write access to production) to a human approver.

---

## 13. Complete Interviewer Q&A Bank

**Q1: Compare RBAC vs. ABAC vs. ReBAC. When would you use each?**
A: RBAC assigns permissions via roles -- simple, auditable, but suffers from role explosion at scale. ABAC evaluates attributes (user department, resource labels, time of day) -- flexible but harder to audit ("why was this allowed?"). ReBAC models relationships between subjects and objects in a graph -- excellent for object-level permissions but requires a graph engine. In practice, we layer all three: RBAC for coarse roles, ABAC for conditions, ReBAC for fine-grained object ownership.

**Q2: How does Google Zanzibar work, and how would you implement it?**
A: Zanzibar stores authorization data as relationship tuples: `(object#relation@subject)`. For example, `doc:readme#viewer@user:alice`. Permission checks traverse the relationship graph: to check if alice can view a folder, it checks if alice is a viewer of the folder, or a viewer of any parent folder (transitive). We implement this via SpiceDB, which provides the same data model, check/expand/lookup APIs, and a schema language for defining relationships and permissions.

**Q3: How do you prevent a denial-of-service on the authorization system?**
A: (1) Per-node OPA sidecars -- no central bottleneck. (2) In-process LRU cache absorbs 80%+ of requests. (3) Rate limiting per subject (100 decisions/sec per SA by default). (4) Circuit breaker in Envoy: if OPA p99 exceeds 10 ms for 30 seconds, trip the circuit and fail-closed. (5) OPA bundles have a size limit; overly complex policies are rejected at upload time.

**Q4: How do you handle authorization for batch job schedulers?**
A: The job scheduler service account has a role that permits creating pods, volumes, and configmaps in specific namespaces. Jobs themselves run as separate service accounts with scoped permissions for only the resources they need. The scheduler cannot escalate -- it cannot create bindings or modify roles. Each job's service account uses a projected token with a 1-hour TTL and audience bound to the target API.

**Q5: What is the principle of least privilege, and how do you enforce it technically?**
A: Least privilege: every subject should have only the permissions necessary for its function, nothing more. Technical enforcement: (1) Deny-by-default (no implicit permissions). (2) Automated audits comparing granted vs. used permissions. (3) Time-bound bindings (expires_at). (4) Namespace-scoped roles preferred over cluster roles. (5) AI-powered recommendations to prune unused permissions.

**Q6: How do you handle emergency access (break-glass)?**
A: Break-glass creates a temporary, heavily audited elevation. Process: (1) User requests via CLI with reason and MFA token. (2) System grants temporary cluster-admin (or requested role) for a fixed duration (max 4 hours). (3) Every action during break-glass is logged with a special `break_glass=true` flag. (4) Automatic page to security team. (5) Post-incident review of all break-glass actions.

**Q7: How do you handle cross-namespace or cross-project authorization?**
A: ReBAC handles this naturally. A user who is an editor on a project has access to all namespaces within that project via transitive relationships. For RBAC, cross-namespace access requires explicit ClusterRole or multiple namespace-scoped RoleBindings. We prefer ReBAC for cross-cutting access patterns because it avoids binding proliferation.

**Q8: What are the security implications of Kubernetes RoleBinding vs. ClusterRoleBinding?**
A: A RoleBinding is namespace-scoped -- it can only grant access within its namespace, even if it references a ClusterRole (the ClusterRole's permissions are limited to that namespace). A ClusterRoleBinding grants access cluster-wide. The risk with ClusterRoleBinding is that a single misconfiguration grants excessive access everywhere. We restrict ClusterRoleBinding creation to a small set of admin subjects and require approval.

**Q9: How do you integrate OpenStack Keystone with the platform authorization system?**
A: Keystone issues tokens with project-scoped roles. We sync Keystone projects and roles to the platform RBAC system via a Keystone event listener (CADF notifications on Kafka). When a user accesses bare-metal IaaS resources, the API gateway extracts the Keystone token, validates it against Keystone, and maps the Keystone role to a platform role for OPA evaluation.

**Q10: How do you handle RBAC for multi-tenant environments?**
A: (1) Tenant isolation via namespaces (k8s) or projects (OpenStack). (2) Roles are scoped to tenant boundaries -- a tenant admin cannot create resources outside their namespace. (3) Network policies enforce network-level isolation. (4) ReBAC ensures that tenant A's users cannot traverse relationships to tenant B's resources. (5) Resource quotas per tenant prevent noisy-neighbor issues.

**Q11: What is OPA Gatekeeper, and how does it differ from OPA as a sidecar?**
A: Gatekeeper is an OPA-based admission controller for Kubernetes. It evaluates policies at resource creation/update time (validating admission webhook), not at request authorization time. Example: Gatekeeper can enforce "all pods must have resource limits" or "no pods in namespace prod can use hostNetwork." The sidecar OPA handles authorization (who can do what); Gatekeeper handles policy compliance (what is allowed to exist in the cluster).

**Q12: How do you handle secret rotation for the authorization system's own credentials?**
A: (1) MySQL credentials: dynamic secrets from Vault with 24h TTL. (2) Kafka credentials: SASL/SCRAM with Vault-managed rotation. (3) SpiceDB pre-shared key: rotated via Vault with zero-downtime (dual-key acceptance during rotation window). (4) OPA bundle signing key: rotated quarterly; old key accepted for 7 days after rotation.

**Q13: How would you implement attribute-based conditions in Rego?**
A: Rego natively supports attribute evaluation. Example: `allow { input.resource.labels.env == "dev"; input.time.hour >= 9; input.time.hour < 17 }`. Attributes come from three sources: (1) JWT claims (user attributes), (2) Resource metadata (labels, annotations -- fetched from k8s API or cached), (3) Context (time, source IP). The OPA data bundle includes a mapping of resource attributes, updated via CDC.

**Q14: How do you monitor the health of the authorization system?**
A: Metrics: (1) Decision latency p50/p99 per sidecar. (2) Cache hit rate. (3) Deny rate (overall and per-subject). (4) Policy bundle age (time since last update). (5) Kafka consumer lag. (6) SpiceDB latency. Dashboards: Grafana with per-namespace authorization overview. Alerts: latency > 2ms p99, deny rate spike > 2x, policy age > 5 min, Kafka lag > 1 min.

**Q15: How do you handle the "confused deputy" problem in microservices?**
A: The confused deputy problem occurs when a service uses its own elevated credentials to act on behalf of a user, bypassing the user's authorization. Mitigation: (1) Token forwarding: services pass the original user's JWT downstream (not their own SA token). (2) OPA evaluates the original user's permissions, not the service's. (3) For service-initiated actions (not user-triggered), the service uses its own SA token with scoped permissions. (4) Audit log captures both the acting service and the original user for traceability.

**Q16: How would you implement time-limited access (e.g., "grant access for 4 hours")?**
A: The `role_bindings` table has an `expires_at` column. A background reaper job runs every minute to delete expired bindings and publish a cache invalidation event to Kafka. The OPA policy also checks `binding.expires_at > current_time` during evaluation, providing double-enforcement. For Kubernetes, we use a controller that deletes expired RoleBindings.

**Q17: How do you handle authorization during a cluster upgrade or maintenance?**
A: (1) OPA sidecars are updated via rolling DaemonSet update (one node at a time). (2) During sidecar restart (~10 seconds), Envoy queues requests (configurable timeout). (3) Kubernetes native RBAC is unaffected by sidecar restarts. (4) Policy bundle is backward-compatible (new fields are optional). (5) Canary: upgrade sidecars on 1 node first, verify authorization metrics, then proceed.

---

## 14. References

- [Kubernetes RBAC Documentation](https://kubernetes.io/docs/reference/access-authn-authz/rbac/)
- [Open Policy Agent (OPA)](https://www.openpolicyagent.org/docs/latest/)
- [OPA Gatekeeper](https://open-policy-agent.github.io/gatekeeper/website/)
- [Google Zanzibar Paper](https://research.google/pubs/pub48190/) -- "Zanzibar: Google's Consistent, Global Authorization System"
- [SpiceDB](https://authzed.com/spicedb) -- Open-source Zanzibar implementation
- [Kubernetes TokenRequest API](https://kubernetes.io/docs/reference/kubernetes-api/authentication-resources/token-request-v1/)
- [Kubernetes Projected Volumes](https://kubernetes.io/docs/concepts/storage/projected-volumes/)
- [Rego Policy Language](https://www.openpolicyagent.org/docs/latest/policy-language/)
- [NIST RBAC Model](https://csrc.nist.gov/projects/role-based-access-control)
- [OpenStack Keystone Policy](https://docs.openstack.org/keystone/latest/admin/service-api-protection.html)
