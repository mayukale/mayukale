# System Design: Secrets Management System

> **Relevance to role:** Every service on a cloud infrastructure platform needs credentials -- database passwords, API keys, TLS certificates, cloud provider tokens. A secrets management system (modeled on HashiCorp Vault) is the central trust anchor. For bare-metal IaaS, Kubernetes workloads, job schedulers, and MySQL/Elasticsearch clusters, this system controls who can access what credential, issues dynamic short-lived secrets, and provides encryption as a service. A misconfigured or unavailable secrets system can cause a full platform outage or a catastrophic breach.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | Store and retrieve static secrets (KV store with versioning). |
| FR-2 | Generate **dynamic secrets** (database credentials, cloud IAM tokens) with TTL and auto-revocation. |
| FR-3 | Multiple **auth methods**: Kubernetes SA JWT, AWS IAM, LDAP, AppRole, OIDC. |
| FR-4 | **Secret engines**: KV v2, PKI (certificate issuance), Database (MySQL, PostgreSQL), AWS, GCP, Transit (encryption as a service). |
| FR-5 | **Secret leasing**: every secret has a lease_id, TTL, renewable flag. |
| FR-6 | **Policy-based access control**: path-based ACL policies (HCL syntax). |
| FR-7 | **Audit logging**: every secret access, creation, deletion, authentication event. |
| FR-8 | **Seal/Unseal mechanism**: secrets encrypted at rest; unsealing requires threshold of key shares (Shamir's Secret Sharing). |
| FR-9 | **Auto-unseal** via cloud KMS (AWS KMS, GCP Cloud KMS) or Transit engine. |
| FR-10 | **Vault Agent**: sidecar for auto-auth, secret caching, template rendering. |
| FR-11 | **Vault Secrets Operator** (Kubernetes): sync secrets to k8s Secret objects via CRDs. |
| FR-12 | **Transit engine**: encrypt/decrypt data without exposing the encryption key. |
| FR-13 | **Secret rotation**: automated rotation of static secrets on a schedule. |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Secret read latency | p99 < 10 ms (cached), p99 < 50 ms (uncached) |
| NFR-2 | Availability | 99.99% (secrets are critical path) |
| NFR-3 | Throughput | 10,000 secret reads/sec, 1,000 writes/sec |
| NFR-4 | Durability | Zero secret loss (Raft consensus, backups) |
| NFR-5 | Audit completeness | Every operation logged, no gaps |
| NFR-6 | Seal/unseal time | < 30 seconds (auto-unseal) |
| NFR-7 | Dynamic secret issuance | < 500 ms (database credential generation) |

### Constraints & Assumptions

- Production Vault cluster: 5 nodes (Raft integrated storage), 3-node quorum.
- Bare-metal servers -- no cloud KMS available directly; auto-unseal uses a separate "seal Vault" or HSM.
- All Kubernetes workloads access secrets via Vault Agent sidecar or Vault Secrets Operator.
- MySQL 8.0 credentials are managed as dynamic secrets.
- Java services use Spring Cloud Vault; Python services use hvac library.

### Out of Scope

- Certificate lifecycle management (covered in certificate_lifecycle_management.md).
- API key management for external consumers (covered in api_key_management_system.md).
- Network-level security (covered in zero_trust_network_design.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Services (secret consumers) | 2,000 | Microservices across the platform |
| Kubernetes pods (peak) | 50,000 | Running concurrently across 500 nodes |
| Secret reads/sec (peak) | 10,000 | 50,000 pods x 0.2 reads/sec avg (token renewal, secret fetch) |
| Secret writes/sec | 200 | Secret updates, dynamic credential issuance |
| Dynamic credential issuances/sec | 100 | MySQL + cloud provider credentials |
| Active leases | 200,000 | 50,000 pods x 4 leases avg (token + DB cred + TLS + app secret) |
| Vault Agent instances | 50,000 | One per pod (sidecar) |
| Auth requests/sec | 2,000 | Pod startup + token renewal (30-min TTL) |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| KV secret read (cached by Vault Agent) | < 1 ms (local cache) |
| KV secret read (Vault server) | < 10 ms p99 |
| Dynamic secret issuance (MySQL) | < 500 ms p99 |
| Transit encrypt/decrypt | < 5 ms p99 |
| Token authentication (k8s auth method) | < 50 ms p99 |
| Lease renewal | < 10 ms p99 |

### Storage Estimates

| Data | Size | Calculation |
|------|------|-------------|
| KV secrets (all versions) | ~5 GB | 100,000 secret paths x 10 versions x 5 KB avg |
| Active leases | ~200 MB | 200,000 leases x 1 KB |
| Audit log (1 day) | ~50 GB | 10,000 ops/sec x 86,400 sec x 0.5 avg util x 1 KB |
| Raft storage total | ~10 GB | Secrets + leases + policies + auth data |
| Vault Agent cache (per pod) | ~10 MB | Cached secrets + tokens |

### Bandwidth Estimates

| Flow | Bandwidth |
|------|-----------|
| Secret reads (Vault Agent to Vault) | ~10 MB/sec (10,000 reads/sec x 1 KB) |
| Raft replication | ~5 MB/sec (writes replicated to 4 followers) |
| Audit log stream | ~10 MB/sec to syslog/Kafka |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                        Client Layer                                   │
│                                                                       │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────────┐   │
│  │  Java Service │  │ Python Svc   │  │  Job Scheduler           │   │
│  │  (Spring      │  │ (hvac lib)   │  │  (AppRole auth)          │   │
│  │  Cloud Vault) │  │              │  │                          │   │
│  └──────┬───────┘  └──────┬───────┘  └────────────┬─────────────┘   │
│         │                  │                        │                  │
│  ┌──────▼──────────────────▼────────────────────────▼──────────────┐ │
│  │              Vault Agent Sidecar (per pod)                       │ │
│  │  ┌────────────────────────────────────────────────────────┐     │ │
│  │  │  Auto-Auth (k8s SA JWT → Vault token)                  │     │ │
│  │  │  Secret Cache (in-memory, TTL-aware)                   │     │ │
│  │  │  Template Rendering (consul-template syntax)           │     │ │
│  │  │  → Renders secrets to files, env vars, or k8s secrets  │     │ │
│  │  └────────────────────────────────────────────────────────┘     │ │
│  └──────────────────────────────┬──────────────────────────────────┘ │
└─────────────────────────────────┼────────────────────────────────────┘
                                  │ HTTPS (mTLS)
                                  ▼
┌──────────────────────────────────────────────────────────────────────┐
│                      Vault Server Cluster                             │
│                                                                       │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │                      Load Balancer                              │  │
│  │               (Active node routing)                             │  │
│  └──────────┬─────────────────┬─────────────────┬─────────────────┘  │
│             │                 │                   │                    │
│  ┌──────────▼──┐   ┌─────────▼───┐   ┌──────────▼──┐               │
│  │  Vault Node  │   │  Vault Node  │   │  Vault Node  │ ... (5)     │
│  │  (Active)    │   │  (Standby)   │   │  (Standby)   │              │
│  │             │   │              │   │              │               │
│  │  ┌────────┐ │   │  ┌────────┐  │   │  ┌────────┐  │              │
│  │  │ Secret │ │   │  │ Secret │  │   │  │ Secret │  │              │
│  │  │Engines │ │   │  │Engines │  │   │  │Engines │  │              │
│  │  ├────────┤ │   │  ├────────┤  │   │  ├────────┤  │              │
│  │  │ Auth   │ │   │  │ Auth   │  │   │  │ Auth   │  │              │
│  │  │Methods │ │   │  │Methods │  │   │  │Methods │  │              │
│  │  ├────────┤ │   │  ├────────┤  │   │  ├────────┤  │              │
│  │  │ Policy │ │   │  │ Policy │  │   │  │ Policy │  │              │
│  │  │Engine  │ │   │  │Engine  │  │   │  │Engine  │  │              │
│  │  ├────────┤ │   │  ├────────┤  │   │  ├────────┤  │              │
│  │  │ Raft   │◀┼──▶│  │ Raft   │◀─┼──▶│  │ Raft   │  │              │
│  │  │Storage │ │   │  │Storage │  │   │  │Storage │  │              │
│  │  └────────┘ │   │  └────────┘  │   │  └────────┘  │              │
│  └─────────────┘   └─────────────┘   └─────────────┘               │
│                                                                       │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │  Auto-Unseal: HSM / Transit Seal (separate Vault) / AWS KMS   │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
          │                    │                     │
          ▼                    ▼                     ▼
┌──────────────┐   ┌──────────────────┐   ┌──────────────────────┐
│  MySQL 8.0   │   │  AWS/GCP APIs    │   │  Audit Backend       │
│  (dynamic    │   │  (dynamic IAM    │   │  (syslog → Kafka     │
│   creds)     │   │   credentials)   │   │   → Elasticsearch)   │
└──────────────┘   └──────────────────┘   └──────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│           Vault Secrets Operator (Kubernetes)                         │
│                                                                       │
│  ┌─────────────────┐  ┌──────────────────┐  ┌────────────────────┐  │
│  │VaultStaticSecret│  │VaultDynamicSecret│  │VaultPKISecret      │  │
│  │  CRD            │  │  CRD             │  │  CRD               │  │
│  │  → k8s Secret   │  │  → k8s Secret    │  │  → k8s Secret      │  │
│  │  (synced)       │  │  (rotated)       │  │  (cert + key)      │  │
│  └─────────────────┘  └──────────────────┘  └────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Vault Server Cluster** | Central secret store; 5-node Raft cluster with 1 active + 4 standbys. Handles all secret operations. |
| **Raft Integrated Storage** | Vault's built-in consensus storage (since Vault 1.4). Replaces Consul. Provides HA + durability. |
| **Secret Engines** | Pluggable backends: KV v2 (static), Database (dynamic MySQL/PG creds), PKI (certificates), Transit (encryption), AWS/GCP (cloud IAM). |
| **Auth Methods** | Pluggable authentication: Kubernetes (SA JWT), AppRole (machine auth), LDAP, OIDC, AWS IAM. |
| **Policy Engine** | HCL-based ACL policies mapping auth identity to secret paths. |
| **Auto-Unseal** | Automated unsealing via HSM (PKCS#11), AWS KMS, or Transit seal from a separate Vault. |
| **Vault Agent** | Sidecar: auto-auth, secret caching, template rendering. Eliminates direct Vault API calls from application code. |
| **Vault Secrets Operator** | Kubernetes operator: syncs Vault secrets to native k8s Secret objects via CRDs. |
| **Audit Backend** | Every Vault operation logged to syslog -> Kafka -> Elasticsearch. |

### Data Flows

1. **Pod startup (k8s auth):** Pod starts -> Vault Agent reads SA token from projected volume -> Agent calls Vault `/auth/kubernetes/login` with JWT -> Vault verifies JWT with k8s TokenReview API -> Vault returns a Vault token with policies attached -> Agent caches token and fetches secrets.

2. **Dynamic MySQL credential:** Application (via Agent) requests `database/creds/myapp-role` -> Vault Database engine connects to MySQL, creates user with specific grants, sets TTL -> Returns username/password to application -> Lease tracked by Vault -> On TTL expiry, Vault revokes the MySQL user.

3. **Secret rotation:** Vault Agent template watches for secret changes. When Vault rotates a KV secret (new version), Agent re-renders the template file. Application detects file change (inotify) or Agent sends SIGHUP.

4. **Transit encryption:** Application sends plaintext to `transit/encrypt/my-key` -> Vault encrypts with AES-256-GCM using the named key -> Returns ciphertext (vault:v1:base64...) -> Application stores ciphertext in MySQL. Decryption is the reverse.

---

## 4. Data Model

### Core Entities & Schema (Full SQL)

Note: Vault uses its own internal storage (Raft/Consul), not a relational DB. The schema below represents the **logical data model** as it would be stored in Vault's encrypted backend, plus the **platform management layer** (MySQL) for tracking and auditing.

```sql
-- ============================================================
-- VAULT LOGICAL DATA MODEL (stored in Raft, shown as SQL for clarity)
-- ============================================================

-- KV Secret (KV v2 engine)
-- Vault path: secret/data/{path}
-- Vault stores this as encrypted entries in Raft
CREATE TABLE kv_secrets (
    path             VARCHAR(512) PRIMARY KEY COMMENT 'e.g., secret/data/myapp/db-config',
    version          INT UNSIGNED NOT NULL DEFAULT 1,
    data_encrypted   BLOB NOT NULL COMMENT 'AES-256-GCM encrypted JSON',
    metadata         JSON NOT NULL COMMENT '{"created_time", "deletion_time", "destroyed", "custom_metadata"}',
    cas_required     BOOLEAN NOT NULL DEFAULT FALSE COMMENT 'Check-And-Set: prevent concurrent writes',
    max_versions     INT UNSIGNED NOT NULL DEFAULT 10,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Leases
CREATE TABLE leases (
    lease_id         VARCHAR(512) PRIMARY KEY COMMENT 'e.g., database/creds/myapp-role/abcd1234',
    secret_engine    VARCHAR(128) NOT NULL COMMENT 'database, aws, pki, etc.',
    path             VARCHAR(512) NOT NULL,
    token_id         VARCHAR(128) NOT NULL COMMENT 'Vault token that created this lease',
    issue_time       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expire_time      TIMESTAMP NOT NULL,
    last_renewal     TIMESTAMP DEFAULT NULL,
    ttl_seconds      INT UNSIGNED NOT NULL,
    max_ttl_seconds  INT UNSIGNED NOT NULL,
    renewable        BOOLEAN NOT NULL DEFAULT TRUE,
    revoked          BOOLEAN NOT NULL DEFAULT FALSE,
    revoke_time      TIMESTAMP DEFAULT NULL,
    INDEX idx_expire (expire_time),
    INDEX idx_token (token_id),
    INDEX idx_engine_path (secret_engine, path)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Auth Tokens
CREATE TABLE auth_tokens (
    token_accessor   VARCHAR(128) PRIMARY KEY COMMENT 'Non-sensitive token identifier',
    token_id_hash    VARBINARY(64) NOT NULL COMMENT 'SHA-256 of actual token (never stored plaintext)',
    auth_method      VARCHAR(128) NOT NULL COMMENT 'kubernetes, approle, ldap, oidc',
    entity_id        VARCHAR(128) DEFAULT NULL COMMENT 'Vault identity entity',
    policies         JSON NOT NULL COMMENT '["default", "myapp-read"]',
    metadata         JSON DEFAULT NULL COMMENT 'Auth metadata: k8s namespace, SA name, etc.',
    creation_time    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expire_time      TIMESTAMP NOT NULL,
    num_uses         INT UNSIGNED DEFAULT 0 COMMENT '0 = unlimited',
    ttl_seconds      INT UNSIGNED NOT NULL,
    max_ttl_seconds  INT UNSIGNED NOT NULL,
    renewable        BOOLEAN NOT NULL DEFAULT TRUE,
    orphan           BOOLEAN NOT NULL DEFAULT FALSE,
    INDEX idx_expire (expire_time),
    INDEX idx_entity (entity_id),
    INDEX idx_auth_method (auth_method)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Policies (HCL stored as text)
CREATE TABLE policies (
    policy_name      VARCHAR(255) PRIMARY KEY,
    policy_hcl       LONGTEXT NOT NULL,
    version          INT UNSIGNED NOT NULL DEFAULT 1,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    updated_by       VARCHAR(255) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- PLATFORM MANAGEMENT LAYER (actual MySQL tables)
-- ============================================================

-- Secret Ownership Registry (who owns which secret path)
CREATE TABLE secret_ownership (
    ownership_id     BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    vault_path       VARCHAR(512) NOT NULL,
    owner_team       VARCHAR(255) NOT NULL,
    owner_email      VARCHAR(255) NOT NULL,
    environment      ENUM('dev', 'staging', 'prod') NOT NULL,
    rotation_policy  ENUM('manual', 'daily', 'weekly', 'monthly', 'dynamic') NOT NULL DEFAULT 'manual',
    last_rotated     TIMESTAMP DEFAULT NULL,
    next_rotation    TIMESTAMP DEFAULT NULL,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY uk_path_env (vault_path, environment),
    INDEX idx_team (owner_team),
    INDEX idx_next_rotation (next_rotation)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Secret Access Audit (for platform-level tracking)
CREATE TABLE secret_access_audit (
    audit_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    timestamp_utc    TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    operation        ENUM('read', 'create', 'update', 'delete', 'list', 'login', 'renew', 'revoke') NOT NULL,
    vault_path       VARCHAR(512) NOT NULL,
    auth_method      VARCHAR(128) NOT NULL,
    entity_name      VARCHAR(255) DEFAULT NULL,
    source_namespace VARCHAR(255) DEFAULT NULL COMMENT 'k8s namespace',
    source_pod       VARCHAR(255) DEFAULT NULL,
    source_ip        VARCHAR(45) DEFAULT NULL,
    success          BOOLEAN NOT NULL,
    error_message    TEXT DEFAULT NULL,
    request_id       VARCHAR(128) NOT NULL,
    INDEX idx_timestamp (timestamp_utc),
    INDEX idx_path (vault_path, timestamp_utc),
    INDEX idx_entity (entity_name, timestamp_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (UNIX_TIMESTAMP(timestamp_utc)) (
    -- Daily partitions managed by automation
);

-- Dynamic Credential Tracking
CREATE TABLE dynamic_credential_tracking (
    tracking_id      BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    lease_id         VARCHAR(512) NOT NULL,
    secret_engine    VARCHAR(128) NOT NULL,
    backend_system   VARCHAR(255) NOT NULL COMMENT 'e.g., mysql-prod-cluster',
    generated_user   VARCHAR(255) NOT NULL COMMENT 'e.g., v-k8s-myapp-abcd1234',
    requesting_entity VARCHAR(255) NOT NULL,
    requesting_namespace VARCHAR(255) DEFAULT NULL,
    issued_at        TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at       TIMESTAMP NOT NULL,
    revoked_at       TIMESTAMP DEFAULT NULL,
    revocation_reason VARCHAR(255) DEFAULT NULL,
    INDEX idx_lease (lease_id),
    INDEX idx_backend (backend_system),
    INDEX idx_entity (requesting_entity),
    INDEX idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Store | Use Case | Justification |
|-------|----------|---------------|
| **Raft Integrated Storage** | Vault's internal data (secrets, leases, tokens, policies) | Built into Vault 1.4+; no external dependency. Provides HA consensus. |
| **MySQL 8.0** | Platform management layer (ownership, tracking, extended audit) | Rich queries for operational dashboards, compliance reports. |
| **Elasticsearch 8.x** | Audit log search | Full-text search across audit events; aggregation dashboards. |
| **Kafka** | Audit event streaming | Decouples Vault audit backend from downstream consumers. |

### Indexing Strategy

| Table | Key Index | Purpose |
|-------|-----------|---------|
| `secret_ownership` | `(vault_path, environment)` UNIQUE | Fast lookup of secret owner. |
| `secret_access_audit` | PARTITION by day + `(vault_path, timestamp_utc)` | Audit queries by path. |
| `dynamic_credential_tracking` | `(backend_system, expires_at)` | Identify expiring credentials per backend. |
| `leases` (logical) | `(expire_time)` | Lease expiry processing. |

---

## 5. API Design

### REST Endpoints (Vault HTTP API)

```
# ── Authentication ───────────────────────────────────────────
POST   /v1/auth/kubernetes/login
  Body: {"role": "myapp", "jwt": "<SA token>"}
  Response: {"auth": {"client_token": "hvs.xxx", "lease_duration": 1800, "policies": ["myapp-read"]}}

POST   /v1/auth/approle/login
  Body: {"role_id": "xxx", "secret_id": "yyy"}

POST   /v1/auth/ldap/login/{username}
  Body: {"password": "xxx"}

# ── KV v2 Secrets ────────────────────────────────────────────
GET    /v1/secret/data/{path}              # Read secret (current version)
GET    /v1/secret/data/{path}?version=3    # Read specific version
POST   /v1/secret/data/{path}              # Create/update secret
  Body: {"data": {"username": "admin", "password": "s3cret"}, "options": {"cas": 1}}
DELETE /v1/secret/data/{path}              # Soft delete (latest version)
POST   /v1/secret/delete/{path}            # Soft delete specific versions
  Body: {"versions": [1, 2]}
POST   /v1/secret/undelete/{path}          # Undelete versions
POST   /v1/secret/destroy/{path}           # Permanently destroy versions
GET    /v1/secret/metadata/{path}          # Read metadata (all versions info)
DELETE /v1/secret/metadata/{path}          # Delete all versions + metadata
LIST   /v1/secret/metadata/{path}          # List secret keys under path

# ── Dynamic Secrets (Database) ───────────────────────────────
GET    /v1/database/creds/{role}           # Generate dynamic MySQL credential
  Response: {
    "lease_id": "database/creds/myapp-role/abcd1234",
    "lease_duration": 3600,
    "renewable": true,
    "data": {"username": "v-k8s-myapp-abcd1234", "password": "A1B2c3D4..."}
  }

POST   /v1/database/config/{name}          # Configure database connection
POST   /v1/database/roles/{name}           # Configure dynamic role (SQL template)
POST   /v1/database/rotate-root/{name}     # Rotate root credentials

# ── Transit (Encryption as a Service) ───────────────────────
POST   /v1/transit/encrypt/{key_name}
  Body: {"plaintext": "base64-encoded-data"}
  Response: {"ciphertext": "vault:v1:XYZ..."}

POST   /v1/transit/decrypt/{key_name}
  Body: {"ciphertext": "vault:v1:XYZ..."}
  Response: {"plaintext": "base64-encoded-data"}

POST   /v1/transit/keys/{key_name}         # Create encryption key
POST   /v1/transit/keys/{key_name}/rotate  # Rotate encryption key
GET    /v1/transit/keys/{key_name}         # Read key metadata

# ── Lease Management ────────────────────────────────────────
PUT    /v1/sys/leases/renew
  Body: {"lease_id": "database/creds/myapp-role/abcd1234", "increment": 3600}

PUT    /v1/sys/leases/revoke
  Body: {"lease_id": "database/creds/myapp-role/abcd1234"}

PUT    /v1/sys/leases/revoke-prefix/{prefix}  # Revoke all leases under prefix

# ── System Operations ───────────────────────────────────────
PUT    /v1/sys/seal                        # Seal Vault
PUT    /v1/sys/unseal                      # Unseal (provide key share)
GET    /v1/sys/seal-status                 # Check seal status
GET    /v1/sys/health                      # Health check
GET    /v1/sys/leader                      # Current leader info
POST   /v1/sys/policies/acl/{name}        # Create/update policy
GET    /v1/sys/policies/acl/{name}        # Read policy
LIST   /v1/sys/policies/acl               # List policies
```

### CLI

```bash
# ── Authentication ──
vault login -method=kubernetes role=myapp
vault login -method=ldap username=alice
vault login -method=approle role_id=$ROLE_ID secret_id=$SECRET_ID

# ── KV v2 Secrets ──
vault kv put secret/myapp/db-config username=admin password=s3cret
vault kv get secret/myapp/db-config
vault kv get -version=3 secret/myapp/db-config
vault kv metadata get secret/myapp/db-config
vault kv delete secret/myapp/db-config
vault kv destroy -versions=1,2 secret/myapp/db-config
vault kv list secret/myapp/

# ── Dynamic Secrets (Database) ──
vault read database/creds/myapp-role
# Output:
# Key             Value
# lease_id        database/creds/myapp-role/abcd1234
# lease_duration  1h
# username        v-k8s-myapp-abcd1234
# password        A1B2c3D4...

# Configure database engine
vault write database/config/mysql-prod \
    plugin_name=mysql-database-plugin \
    connection_url="{{username}}:{{password}}@tcp(mysql-prod:3306)/" \
    allowed_roles="myapp-role" \
    username="vault-admin" \
    password="rootpassword"

vault write database/roles/myapp-role \
    db_name=mysql-prod \
    creation_statements="CREATE USER '{{name}}'@'%' IDENTIFIED BY '{{password}}'; \
        GRANT SELECT, INSERT ON mydb.* TO '{{name}}'@'%';" \
    default_ttl="1h" \
    max_ttl="24h"

# ── Transit ──
vault write transit/keys/myapp-key type=aes256-gcm96
vault write transit/encrypt/myapp-key plaintext=$(echo "hello" | base64)
vault write transit/decrypt/myapp-key ciphertext="vault:v1:XYZ..."

# ── Lease Management ──
vault lease renew database/creds/myapp-role/abcd1234
vault lease revoke database/creds/myapp-role/abcd1234
vault lease revoke -prefix database/creds/myapp-role

# ── Seal/Unseal ──
vault operator seal
vault operator unseal $KEY_SHARE_1
vault operator unseal $KEY_SHARE_2
vault operator unseal $KEY_SHARE_3
vault status

# ── Policy Management ──
vault policy write myapp-read - <<EOF
path "secret/data/myapp/*" {
  capabilities = ["read", "list"]
}
path "database/creds/myapp-role" {
  capabilities = ["read"]
}
path "transit/encrypt/myapp-key" {
  capabilities = ["update"]
}
path "transit/decrypt/myapp-key" {
  capabilities = ["update"]
}
EOF

vault policy list
vault policy read myapp-read

# ── Vault Agent Config ──
# /etc/vault-agent/config.hcl
auto_auth {
  method "kubernetes" {
    mount_path = "auth/kubernetes"
    config = {
      role = "myapp"
    }
  }
  sink "file" {
    config = {
      path = "/home/vault/.vault-token"
    }
  }
}

cache {
  use_auto_auth_token = true
}

template {
  source      = "/etc/vault-agent/templates/db-creds.ctmpl"
  destination = "/app/config/db-creds.json"
  command     = "pkill -SIGHUP myapp"  # Signal app to reload
}

# ── Vault Secrets Operator (Kubernetes CRDs) ──
# VaultStaticSecret
cat <<EOF | kubectl apply -f -
apiVersion: secrets.hashicorp.com/v1beta1
kind: VaultStaticSecret
metadata:
  name: myapp-db-config
  namespace: myapp
spec:
  vaultAuthRef: myapp-auth
  mount: secret
  path: myapp/db-config
  type: kv-v2
  refreshAfter: 60s
  destination:
    name: myapp-db-secret    # k8s Secret name
    create: true
EOF

# VaultDynamicSecret
cat <<EOF | kubectl apply -f -
apiVersion: secrets.hashicorp.com/v1beta1
kind: VaultDynamicSecret
metadata:
  name: myapp-db-creds
  namespace: myapp
spec:
  vaultAuthRef: myapp-auth
  mount: database
  path: creds/myapp-role
  renewalPercent: 67
  destination:
    name: myapp-db-dynamic-secret
    create: true
EOF
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Seal/Unseal Mechanism and Shamir's Secret Sharing

**Why it's hard:**
All secrets in Vault are encrypted at rest with a master key. The master key itself is encrypted by the unseal key. The unseal key must never exist in a single place, yet Vault must be able to reconstruct it to start serving requests. This is a fundamental tension between security and availability.

| Approach | Security | Availability | Operational Cost |
|----------|----------|--------------|-----------------|
| **Shamir's Secret Sharing (manual unseal)** | Highest (requires N of M key holders) | Low (manual intervention on restart) | High (operators must be available) |
| **Auto-unseal via cloud KMS** | High (KMS-protected) | High (automatic on restart) | Low (depends on KMS availability) |
| **Auto-unseal via HSM (PKCS#11)** | Highest (hardware protection) | High (requires HSM reachability) | Medium (HSM hardware cost) |
| **Auto-unseal via Transit seal** | High (another Vault protects the key) | High (requires seal Vault) | Medium (manage two Vaults) |

**Selected: Auto-unseal via HSM for production; Transit seal for non-production**

**Justification:** On bare-metal infrastructure without cloud KMS, HSM provides the highest security for the master key. For dev/staging environments, a separate "seal Vault" using Transit engine provides auto-unseal without HSM hardware cost.

**Implementation Detail:**

```
Vault Initialization (Shamir's Secret Sharing):
──────────────────────────────────────────────
vault operator init -key-shares=5 -key-threshold=3

This generates:
  1. Master Key (256-bit AES key)
  2. Encryption Key (derived from master key; encrypts all data)
  3. 5 Unseal Key Shares (Shamir split of master key)
  4. Root Token (initial admin token; should be revoked after setup)

Seal/Unseal Flow:
─────────────────
┌─────────────────────────────────────────────────┐
│  Vault Storage (Raft)                            │
│  ┌───────────────────────────────────────────┐  │
│  │  Encrypted Data                            │  │
│  │  (AES-256-GCM with encryption key)         │  │
│  └───────────────────────┬───────────────────┘  │
│                          │                       │
│  ┌───────────────────────▼───────────────────┐  │
│  │  Encryption Key                            │  │
│  │  (encrypted by master key)                 │  │
│  └───────────────────────┬───────────────────┘  │
│                          │                       │
│  ┌───────────────────────▼───────────────────┐  │
│  │  Master Key                                │  │
│  │  (encrypted by unseal key)                 │  │
│  │                                            │  │
│  │  Manual: reconstructed from Shamir shares  │  │
│  │  Auto:   decrypted by HSM / KMS / Transit  │  │
│  └───────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘

Auto-Unseal with HSM:
────────────────────
Vault start → detects seal type "pkcs11" →
  contacts HSM via PKCS#11 →
  HSM decrypts master key →
  Vault loads encryption key →
  Vault begins serving requests

Auto-Unseal with Transit Seal:
──────────────────────────────
┌─────────────┐                      ┌─────────────────┐
│ Main Vault  │  decrypt master key  │  Seal Vault     │
│ (sealed)    │─────────────────────▶│  (always-on,    │
│             │                      │   transit engine)│
│             │◀─────────────────────│                  │
│ (unsealed!) │   plaintext master   │                  │
└─────────────┘                      └─────────────────┘
```

**Key rotation:**
```bash
# Rekey (change unseal key shares, e.g., when a key holder leaves)
vault operator rekey -init -key-shares=5 -key-threshold=3
vault operator rekey -nonce=$NONCE $KEY_SHARE_1
vault operator rekey -nonce=$NONCE $KEY_SHARE_2
vault operator rekey -nonce=$NONCE $KEY_SHARE_3
# New shares are generated; old shares are invalid
```

**Failure Modes:**
- **HSM unreachable on startup:** Vault remains sealed. Alert fires. On bare metal, the HSM is typically a local PCIe card (not network-dependent), minimizing this risk.
- **Seal Vault down (Transit seal):** Main Vault cannot unseal until seal Vault recovers. Mitigation: seal Vault runs on 3 separate nodes with its own HA.
- **Key share loss:** If fewer than threshold shares are available, Vault cannot unseal. Recovery: restore from backup + rekey. Prevention: each share stored in a separate physical safe / secure location.
- **Root token compromise:** Root token should be revoked after initial setup. If needed, generate a new one with `vault operator generate-root`.

**Interviewer Q&As:**

**Q1: Explain Shamir's Secret Sharing in simple terms.**
A: You have a secret (the master key). You split it into N shares such that any K of those N shares can reconstruct the original secret, but K-1 shares give you zero information about the secret. Mathematically, it works by creating a random polynomial of degree K-1 where the secret is the y-intercept. Each share is a point on that polynomial. With K points you can reconstruct the polynomial (Lagrange interpolation), with fewer you cannot.

**Q2: Why does Vault use two layers of encryption (master key + encryption key)?**
A: Separation of concerns. The encryption key directly encrypts all data. The master key encrypts the encryption key. This means when you rekey (change unseal shares), you only re-encrypt the master key, not all the data. If the encryption key were split directly, rekeying would require re-encrypting the entire storage backend.

**Q3: What happens if all Vault nodes restart simultaneously (e.g., power failure)?**
A: With manual unseal, all nodes come up sealed. Operators must provide K of N key shares to unseal at least one node. That node becomes active and other nodes auto-join the Raft cluster. With auto-unseal, all nodes automatically unseal (assuming HSM/KMS is reachable) and elect a leader via Raft. Recovery time is < 30 seconds for auto-unseal, minutes for manual.

**Q4: How do you handle auto-unseal when the HSM is also on the same bare-metal server?**
A: We use network HSMs (or PCIe HSM cards in each Vault node) in an HA configuration. Each Vault node has its own HSM access. If a node's HSM fails, only that node is affected -- the other 4 nodes continue serving. We can also use a separate "seal Vault" cluster as a software alternative.

**Q5: How do you rotate the master key without downtime?**
A: Vault's `operator rekey` operation runs online. It doesn't rotate the master key itself -- it generates a new set of unseal key shares from the existing master key (or creates a new master key if `target=recovery` is used). The data encryption key remains unchanged. For auto-unseal, the KMS/HSM key can be rotated independently using the cloud/HSM provider's key rotation feature, and Vault will re-wrap the master key automatically.

**Q6: What is the difference between seal keys and recovery keys in auto-unseal mode?**
A: In auto-unseal mode, the HSM/KMS directly protects the master key -- no Shamir shares are needed for unseal. However, Vault still generates "recovery keys" (also Shamir-split) for operations that require a quorum of humans: `operator generate-root`, `operator rekey`, `operator migrate`. Recovery keys cannot unseal the Vault -- they are strictly for administrative operations.

---

### Deep Dive 2: Dynamic Secrets Engine (Database Credentials)

**Why it's hard:**
Dynamic secrets solve the "shared password" problem -- instead of 50 services sharing one MySQL password, each service gets a unique, short-lived credential. The challenge is generating credentials fast enough (< 500 ms), managing thousands of concurrent leases, revoking credentials reliably on expiry, and handling MySQL connection limits.

| Approach | Credential Uniqueness | TTL Management | Revocation Reliability |
|----------|----------------------|----------------|----------------------|
| **Static shared credentials** | None (all share one) | Manual | None (must rotate everywhere) |
| **Vault dynamic secrets** | Per-request unique | Automatic (lease TTL) | Automatic (Vault revokes on expiry) |
| **Sidecar credential proxy** | Per-pod unique | Custom TTL logic | Custom revocation |
| **Database-native temporary users** | Per-request unique | Database-managed | Database-managed |

**Selected: Vault Database Engine (dynamic secrets)**

**Justification:** Vault's database engine provides a battle-tested, standardized approach: unique per-request credentials, automatic TTL-based revocation, and supports MySQL, PostgreSQL, MongoDB, and others. Centralized lease management gives visibility into all active credentials.

**Implementation Detail:**

```
Dynamic Credential Lifecycle:
────────────────────────────
                         ┌──────────────────────────┐
                         │   Vault (Active Node)      │
                         │                            │
 1. App requests cred    │  ┌──────────────────────┐ │
────────────────────────▶│  │ Database Engine       │ │
                         │  │                      │ │
                         │  │ 2. Generate username  │ │    3. CREATE USER + GRANT
                         │  │    v-k8s-myapp-xxxx  │─┼──────────────────────┐
                         │  │                      │ │                      │
                         │  │ 4. Create lease      │ │                      ▼
                         │  │    TTL: 1 hour       │ │              ┌──────────────┐
 5. Return creds ◀───────│  │    renewable: true   │ │              │  MySQL 8.0   │
    {user, password,     │  │                      │ │              │  (target DB) │
     lease_id, ttl}      │  └──────────────────────┘ │              │              │
                         │                            │              │  User:       │
                         │  Lease Expiry Manager:     │              │  v-k8s-myapp │
                         │  ┌──────────────────────┐ │              │  -xxxx       │
                         │  │ Every 30s, check for │ │              │              │
                         │  │ expired leases       │ │  6. On TTL   │              │
                         │  │                      │─┼──expiry────▶│  DROP USER   │
                         │  │ Revoke by executing  │ │              │  v-k8s-myapp │
                         │  │ revocation SQL       │ │              │  -xxxx       │
                         │  └──────────────────────┘ │              └──────────────┘
                         └──────────────────────────┘

Role Configuration:
──────────────────
vault write database/roles/myapp-role \
    db_name=mysql-prod \
    creation_statements="
        CREATE USER '{{name}}'@'%' IDENTIFIED BY '{{password}}';
        GRANT SELECT, INSERT, UPDATE ON mydb.mytable TO '{{name}}'@'%';
    " \
    revocation_statements="
        DROP USER IF EXISTS '{{name}}'@'%';
    " \
    default_ttl="1h" \
    max_ttl="24h"

Lease Renewal:
─────────────
Application renews lease before TTL expires.
Vault Agent handles this automatically:
  - Renews at 2/3 of TTL (e.g., at 40 minutes for a 1-hour TTL)
  - If renewal fails, fetches new credentials and signals the app

Credential naming convention:
  v-{auth_method}-{role}-{random}
  e.g., v-k8s-myapp-AbCd1234
  Max length: 32 chars (MySQL username limit)
```

**MySQL connection management:**
```
MySQL max_connections = 5000 (typical for production)
Active dynamic users at peak = 2000 (50,000 pods, but only ~4% use MySQL)
Buffer = 3000 connections for direct admin, replication, monitoring
```

**Failure Modes:**
- **Vault cannot connect to MySQL:** Credential generation fails. Application gets error, retries with exponential backoff. Vault health check monitors MySQL reachability.
- **Lease revocation fails (MySQL down during revocation):** Vault marks lease as "revocation pending" and retries. A sweep job runs every 5 minutes to retry failed revocations. Stale users are also cleaned by a daily MySQL audit job.
- **Too many dynamic users exceed MySQL max_connections:** Rate limiting on the database engine (max 100 credentials/minute). Alert when active lease count exceeds 80% of max_connections.
- **Vault leader failover during credential issuance:** Raft ensures the lease is committed before returning to the client. If the leader fails mid-transaction, the client gets an error and retries. The partially-created MySQL user is cleaned up by the revocation sweep.

**Interviewer Q&As:**

**Q1: Why dynamic secrets instead of just rotating a static password periodically?**
A: Static rotation has a window of vulnerability: all services using the old password must be updated simultaneously. Dynamic secrets eliminate shared credentials entirely -- each service instance gets a unique user. If one credential leaks, only that instance is compromised, and it auto-expires. Blast radius is dramatically reduced.

**Q2: How do you handle credential rotation for the Vault-to-MySQL admin connection?**
A: `vault write database/rotate-root/mysql-prod` rotates the root credentials that Vault uses to connect to MySQL. After rotation, only Vault knows the new password -- not even the admin who set it up. This is critical because if the Vault admin password is compromised, the attacker only has the old (now invalid) password.

**Q3: What if a pod is killed abruptly and the lease is not revoked?**
A: Vault's lease expiry manager runs independently. When the TTL expires, Vault revokes the credential regardless of whether the pod is alive. This is a key advantage of TTL-based leases -- no dependency on the client for cleanup. Vault Agent also attempts a best-effort lease revocation on SIGTERM.

**Q4: How do you handle database schema migrations with dynamic credentials?**
A: Migration tools (Flyway, Liquibase) use a separate Vault role with elevated permissions (CREATE TABLE, ALTER TABLE). This role has a strict max_ttl (15 minutes) and is bound to the CI/CD pipeline's service account. After migration, the credential expires. Regular application roles have only DML permissions.

**Q5: How do you monitor dynamic credential sprawl?**
A: (1) `vault list sys/leases/lookup/database/creds/` shows all active leases. (2) Prometheus metrics: `vault_expire_num_leases` (total active leases), `vault_runtime_alloc_bytes` (memory pressure from lease tracking). (3) Custom dashboard showing leases per role, average TTL, renewal rate. (4) Alert if total leases exceed threshold (e.g., 10,000).

**Q6: Can you use dynamic secrets for Elasticsearch?**
A: Vault's database engine supports Elasticsearch via the `elasticsearch-database-plugin`. It creates temporary users with specific roles. However, Elasticsearch's security model is less granular than MySQL's, so we typically use API key-based access for Elasticsearch (managed separately, see api_key_management_system.md) and reserve Vault dynamic secrets for MySQL/PostgreSQL.

---

### Deep Dive 3: Vault Agent and Kubernetes Integration

**Why it's hard:**
50,000 pods need secrets at startup, and they need credential renewal throughout their lifetime. Each pod must authenticate to Vault using its Kubernetes service account. The challenge is minimizing Vault server load (not 50,000 concurrent connections), handling pod startup latency, and delivering secrets to applications that don't natively support Vault.

| Approach | Vault Load | App Complexity | Secret Freshness |
|----------|-----------|----------------|-----------------|
| **Direct Vault API from app code** | High (every pod connects) | High (Vault SDK in every app) | Real-time |
| **Vault Agent sidecar** | Medium (Agent caches + batches) | Low (app reads files/env) | Near real-time (template refresh) |
| **Vault Secrets Operator (VSO)** | Low (one controller) | Lowest (native k8s Secrets) | Configurable (refreshAfter) |
| **Init container + volume** | Low (one-time fetch) | Low | Stale (no refresh) |

**Selected: Vault Agent sidecar (primary) + VSO (for teams that prefer k8s-native)**

**Justification:** Vault Agent provides the best balance: caching reduces Vault server load by 10x, template rendering delivers secrets in any format (JSON, env file, properties), and auto-auth handles Kubernetes authentication transparently. VSO is offered as an alternative for teams that want pure Kubernetes-native Secret objects.

**Implementation Detail:**

```
Vault Agent Sidecar (injected via mutating webhook):
────────────────────────────────────────────────────
┌─────────────────────────────────────────────────────────┐
│  Pod                                                     │
│  ┌────────────────────┐  ┌─────────────────────────────┐│
│  │  App Container      │  │  Vault Agent Sidecar        ││
│  │                     │  │                             ││
│  │  Reads secrets from │  │  1. Auto-Auth:              ││
│  │  /vault/secrets/    │◀─│     Read SA token from      ││
│  │  db-creds.json      │  │     projected volume        ││
│  │                     │  │     POST /auth/kubernetes/   ││
│  │  Or: reads env vars │  │     login                   ││
│  │  from k8s Secret    │  │     → Vault token           ││
│  │                     │  │                             ││
│  │                     │  │  2. Template Rendering:     ││
│  │                     │  │     Fetch secrets from      ││
│  │                     │  │     Vault using token       ││
│  │                     │  │     Render to files in      ││
│  │                     │  │     shared volume           ││
│  │                     │  │                             ││
│  │                     │  │  3. Cache:                  ││
│  │                     │  │     In-memory cache of      ││
│  │                     │  │     secrets + Vault token   ││
│  │                     │  │     Serves stale-while-     ││
│  │                     │  │     revalidate on Vault     ││
│  │                     │  │     unreachability          ││
│  │                     │  │                             ││
│  │                     │  │  4. Lease Renewal:          ││
│  │                     │  │     Auto-renews token       ││
│  │                     │  │     + dynamic secrets       ││
│  │                     │  │     at 2/3 TTL              ││
│  └─────────┬──────────┘  └──────────────┬──────────────┘│
│            │ shared emptyDir volume      │               │
│            └────────────────────────────┘                │
└─────────────────────────────────────────────────────────┘

Agent Template Example (db-creds.ctmpl):
────────────────────────────────────────
{{ with secret "database/creds/myapp-role" }}
{
  "host": "mysql-prod.svc.cluster.local",
  "port": 3306,
  "username": "{{ .Data.username }}",
  "password": "{{ .Data.password }}",
  "database": "mydb"
}
{{ end }}

Kubernetes Auth Method Flow:
───────────────────────────
Vault Agent → reads /var/run/secrets/tokens/vault-token (projected SA token)
           → POST /v1/auth/kubernetes/login {role: "myapp", jwt: "<token>"}
           → Vault calls k8s TokenReview API to validate the JWT
           → Vault checks: SA name, namespace, audience match the role config
           → Returns Vault token with policies: ["default", "myapp-read"]

Vault Kubernetes Auth Role Config:
──────────────────────────────────
vault write auth/kubernetes/role/myapp \
    bound_service_account_names=myapp-sa \
    bound_service_account_namespaces=myapp \
    audience="vault" \
    token_policies="myapp-read" \
    token_ttl=30m \
    token_max_ttl=4h
```

**Vault Agent Injector (mutating webhook):**
```yaml
# Pod annotation to trigger Vault Agent injection
metadata:
  annotations:
    vault.hashicorp.com/agent-inject: "true"
    vault.hashicorp.com/role: "myapp"
    vault.hashicorp.com/agent-inject-secret-db-creds: "database/creds/myapp-role"
    vault.hashicorp.com/agent-inject-template-db-creds: |
      {{ with secret "database/creds/myapp-role" }}
      MYSQL_USER={{ .Data.username }}
      MYSQL_PASS={{ .Data.password }}
      {{ end }}
```

**Failure Modes:**
- **Vault unreachable during pod startup:** Agent retries with exponential backoff (1s, 2s, 4s, 8s, max 30s). Pod readiness probe fails until secrets are available. Configurable: allow startup with cached secrets from previous run (if persistent volume is used).
- **Token expires and renewal fails:** Agent fetches a new token via k8s auth. If k8s auth also fails (API server issue), Agent serves cached secrets until its cache TTL expires, then the pod is restarted.
- **Vault Agent sidecar OOMKilled:** Default memory limit: 128 MB. If cache grows too large (many secrets), increase limit. Agent caches only secrets that templates reference, not all Vault data.
- **Vault Secrets Operator controller down:** Existing k8s Secrets remain (they're just k8s objects). No new secrets are synced until the controller recovers. Stale secrets may cause issues if they have short TTLs. Mitigation: run VSO with 2 replicas (leader election).

**Interviewer Q&As:**

**Q1: Why use Vault Agent sidecar instead of having the app call Vault directly?**
A: (1) No Vault SDK needed in the application -- reduces coupling and supports any language. (2) Caching: Agent caches secrets and tokens, reducing Vault server load by 10x. (3) Auto-renewal: Agent handles lease renewal and token renewal transparently. (4) Template rendering: secrets delivered in the exact format the app expects. (5) Consistent auth: the k8s auth flow is handled once by the Agent, not duplicated in every app.

**Q2: How do you handle the "init container" problem where the app needs secrets before it starts?**
A: Vault Agent supports an init mode: it runs as an init container first (fetches initial secrets), then continues as a sidecar (for renewal and updates). Kubernetes does not start the main container until all init containers succeed. This ensures secrets are available at app startup.

**Q3: What is the advantage of Vault Secrets Operator over Vault Agent?**
A: VSO syncs Vault secrets to native Kubernetes Secret objects. This works for applications that already read from k8s Secrets (environment variables, volume mounts) and cannot be modified. The downside is that k8s Secrets are stored in etcd (base64-encoded, not encrypted by default -- though etcd encryption at rest should be enabled). VSO is a convenience layer; Agent is more secure (secrets only in memory, never in etcd).

**Q4: How do you limit the blast radius if a Vault Agent's token is compromised?**
A: (1) Token policies are scoped to only the secrets that specific pod needs (`myapp-read` policy allows only `secret/data/myapp/*`). (2) Token TTL is 30 minutes (auto-renewed). (3) Token is bound to the k8s service account -- if the SA is deleted, the token is revoked. (4) Token has a max_ttl of 4 hours -- even if continuously renewed, it expires.

**Q5: How do you handle Vault Agent for batch jobs that run for seconds?**
A: For short-lived jobs, we use the init-container-only mode (no sidecar). The init container fetches secrets, writes them to a shared volume, and exits. The job container reads the secrets and runs. No renewal is needed because the job completes before the secret TTL. For longer batch jobs, the full sidecar mode is used.

**Q6: How many concurrent Vault connections does this architecture produce?**
A: With 50,000 pods, each running a Vault Agent, worst case is 50,000 connections. However: (1) Agent uses connection pooling (default 5 connections per agent). (2) Agent caches secrets for their TTL, so most operations don't hit Vault. (3) Token renewal is every 30 minutes, not continuous. Steady-state Vault connections: ~500 (connection pool reuse). Peak (pod storm): ~5,000. Vault handles this easily with 5 nodes.

---

### Deep Dive 4: Transit Engine (Encryption as a Service)

**Why it's hard:**
Applications need to encrypt sensitive data (PII, payment info, secrets) before storing in MySQL. They should not manage encryption keys directly. The transit engine must provide < 5 ms encrypt/decrypt latency, support key rotation without re-encrypting all data, and never expose the key material.

| Approach | Key Management | Latency | Key Exposure Risk |
|----------|---------------|---------|-------------------|
| **Application-managed keys** | Distributed (each app) | < 1 ms (local) | High (keys in app config/memory) |
| **Cloud KMS** | Centralized (cloud) | 5-20 ms | Low (cloud-managed) |
| **Vault Transit** | Centralized (Vault) | 2-5 ms | None (keys never leave Vault) |
| **Database TDE** | Database-managed | Transparent | Medium (key in DB config) |

**Selected: Vault Transit Engine**

**Justification:** Keys never leave Vault (not even to the application). Supports automatic key rotation with key versioning -- old ciphertexts can still be decrypted while new encryptions use the latest key version. Centralized audit of all encrypt/decrypt operations.

**Implementation Detail:**

```
Transit Encrypt/Decrypt Flow:
────────────────────────────
┌────────────┐     POST /transit/encrypt/payments-key     ┌────────────┐
│ Java       │     {"plaintext": "base64(card_number)"}   │ Vault      │
│ Service    │────────────────────────────────────────────▶│ Transit    │
│            │                                             │ Engine     │
│            │◀────────────────────────────────────────────│            │
│            │     {"ciphertext": "vault:v2:AbCdEf..."}   │            │
│            │                                             │  Key Ring: │
│  Store in  │                                             │  v1: AES   │
│  MySQL:    │                                             │  v2: AES   │
│  vault:v2: │                                             │  (current) │
│  AbCdEf... │                                             │            │
└────────────┘                                             └────────────┘

Key Rotation:
────────────
POST /transit/keys/payments-key/rotate
  → v3 key version created
  → New encryptions use v3
  → v1 and v2 ciphertexts still decryptable
  → "Rewrap" operation: convert old ciphertext to latest key version
    POST /transit/rewrap/payments-key
    {"ciphertext": "vault:v1:OldData..."}
    → {"ciphertext": "vault:v3:NewData..."}

Key Configuration:
─────────────────
vault write transit/keys/payments-key \
    type=aes256-gcm96 \
    auto_rotate_period=90d \
    min_decryption_version=1 \
    min_encryption_version=3 \
    deletion_allowed=false

Convergent Encryption (for searchable encrypted fields):
──────────────────────────────────────────────────────────
vault write transit/keys/email-lookup \
    type=aes256-gcm96 \
    convergent_encryption=true \
    derived=true

# Same plaintext + context → same ciphertext (deterministic)
# Enables encrypted field lookup in MySQL without decryption
vault write transit/encrypt/email-lookup \
    plaintext=$(echo "alice@corp.com" | base64) \
    context=$(echo "user-email" | base64)
```

**Failure Modes:**
- **Vault down during encrypt/decrypt:** Application circuit-breaker trips. For non-critical paths, fail gracefully (skip encryption logging). For critical paths (payment processing), queue and retry.
- **Key accidentally deleted:** `deletion_allowed=false` prevents this. If overridden and deleted, all ciphertext becomes permanently unreadable. Vault backup is the only recovery path.
- **Key rotation during batch operation:** Atomic from the client's perspective. In-flight encryptions may use v(N) or v(N+1). Both are valid. Rewrap operation can normalize later.

**Interviewer Q&As:**

**Q1: How does Transit handle key rotation without re-encrypting all existing data?**
A: The ciphertext includes the key version: `vault:v2:AbCdEf...`. When Vault decrypts, it reads the version from the prefix and uses the corresponding key. New encryptions use the latest version. Old ciphertexts remain valid. The optional "rewrap" operation re-encrypts with the latest key without exposing plaintext.

**Q2: What is the performance impact of Transit encrypt/decrypt?**
A: Vault benchmarks show ~14,000 encrypt operations/sec on a single node for AES-256-GCM. With 5 nodes (1 active + performance standby reads), we get ~50,000 ops/sec. Latency is 2-5 ms per operation. For bulk operations, Vault supports batch encrypt/decrypt (up to 128 items per request).

**Q3: How does convergent encryption work, and when would you use it?**
A: Convergent encryption produces the same ciphertext for the same plaintext + context. This allows you to create an encrypted index in MySQL: `WHERE encrypted_email = vault_encrypt("alice@corp.com")`. Use case: searching by email in a PII-protected table without decrypting all rows. Caveat: it reveals when two rows have the same value (frequency analysis risk), so use only for lookup, not for all encrypted fields.

**Q4: Can you use Transit instead of TLS for data in transit?**
A: No. Transit is for data-at-rest encryption (encrypt before storing). TLS protects data in transit (network encryption). They serve complementary purposes. Transit encrypts individual fields; TLS encrypts the entire connection.

**Q5: How do you audit who encrypted/decrypted what?**
A: Vault audit log captures every Transit operation: who (token/entity), what (key name, operation type), when (timestamp). It does NOT log the plaintext or ciphertext (configurable via `hmac_accessor`). The request ID links to the application's distributed trace for end-to-end visibility.

**Q6: How do you handle Transit key expiration for compliance (e.g., PCI requires key rotation every 12 months)?**
A: `auto_rotate_period=365d` (or stricter: 90d). Set `min_encryption_version` to the version created within the compliance window. Set `min_decryption_version` to allow decryption of data up to the retention period (e.g., 7 years = keep old versions). Audit reports show key rotation history.

---

## 7. Scaling Strategy

**Vault Server Scaling:**
- **Vertical:** Vault is CPU-bound (crypto operations) and memory-bound (lease tracking). Recommended: 8 vCPU, 32 GB RAM per node.
- **Horizontal (read scaling):** Vault 1.11+ supports Performance Standby Nodes that serve read requests (consistent reads). Scale from 5 to 7+ nodes for read throughput.
- **Performance Replication:** For multi-datacenter, use Vault Enterprise performance replication. Each datacenter has a local cluster that serves reads; writes go to the primary.

**Vault Agent Scaling:**
- Agent runs as a sidecar per pod. No central bottleneck.
- Agent cache reduces Vault server load by 10x.
- For very high-traffic services, increase Agent memory limit and cache TTL.

**Lease Management Scaling:**
- At 200,000 active leases, Vault uses ~200 MB of memory for lease tracking.
- Lease expiry is processed in batches (256 leases per tick by default).
- If lease count grows beyond 500,000, tune `max_lease_count_on_revocation` and increase expiry worker count.

**Interviewer Q&As:**

**Q1: What happens when Vault becomes a single point of failure?**
A: (1) Vault HA with Raft: 5 nodes, tolerates 2 failures. (2) Performance standbys for read scaling. (3) Vault Agent caching: if Vault is briefly unavailable, cached secrets continue working. (4) For multi-datacenter, performance replication ensures each DC has a local cluster. (5) Disaster recovery replication provides a warm standby cluster.

**Q2: How do you handle a Vault version upgrade across 5 nodes?**
A: Rolling upgrade: (1) Upgrade standby nodes one at a time. (2) Verify each node rejoins the cluster. (3) Step down the active node (`vault operator step-down`) -- a standby takes over. (4) Upgrade the former active. (5) Vault is backward-compatible within a minor version; data migration is automatic on first startup of the new version.

**Q3: How do you handle thousands of policies without performance degradation?**
A: Vault evaluates policies at token creation time, not at every request. When a token is created, its attached policies are resolved and the effective permissions are cached with the token. The per-request check is a simple path match against the cached policy set, which is O(n) in the number of policy paths (typically < 100 per token). 10,000 total policies is fine because each token only carries 5-10.

**Q4: What is the storage scaling limit for Raft integrated storage?**
A: Raft stores all data on disk (encrypted). Practical limit is 10-50 GB. Beyond that, performance degrades (snapshot time increases). With 100,000 secrets x 10 KB = 1 GB + leases + metadata, we're well within limits. If secrets grow beyond 50 GB, consider sharding into multiple Vault clusters by domain (one for DB creds, one for PKI, etc.).

**Q5: How do you load test Vault?**
A: Vault provides `vault-benchmark` tool. We simulate: (1) 10,000 concurrent Agent auth requests. (2) 5,000 KV reads/sec. (3) 1,000 dynamic credential issuances. (4) 500 Transit encrypt/decrypt ops/sec. Acceptance criteria: p99 < 50 ms for reads, < 500 ms for dynamic creds. Load test runs monthly in a production-mirrored environment.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---------|--------|-----------|------------|-----|
| **Vault active node crash** | Writes fail for 5-10 seconds until leader election | Raft heartbeat timeout (10s) | Automatic leader election among standbys. Clients retry. | < 15s |
| **Vault minority partition (2 of 5 nodes down)** | No impact (3-node quorum maintained) | Node health monitoring | Automatic recovery when nodes rejoin. | Immediate |
| **Vault majority partition (3 of 5 nodes down)** | Vault goes read-only (no quorum for writes) | Raft quorum loss alert | Agents serve cached secrets. Manual intervention to restore quorum. | Minutes to hours |
| **Raft storage corruption** | Data loss risk | Raft integrity check on startup | Restore from hourly backup snapshot. Rejoin corrupted node with fresh Raft. | < 30 min |
| **HSM failure (auto-unseal)** | Node cannot unseal on restart | HSM health check | Other nodes continue. Replace HSM. | < 1h |
| **MySQL target down (dynamic secrets)** | New credential issuance fails | DB connection health check | Applications use existing (non-expired) credentials. Alert. | Depends on MySQL recovery |
| **Audit backend (Kafka) down** | Vault blocks all operations (audit is blocking by default) | Kafka health check | Secondary audit backend (local file). Or configure non-blocking audit (compliance trade-off). | < 5 min |
| **Vault Agent crash in pod** | Pod loses secret renewal; secrets become stale | Agent health check (readiness probe) | Kubelet restarts sidecar. App continues with cached secrets. | < 30s |
| **Mass pod restart (node failure, deployment)** | Spike in auth requests to Vault | Auth request rate monitoring | Vault rate limiter. Agent spread over time via pod disruption budget. | Self-healing |
| **Vault token stolen** | Unauthorized secret access | Anomaly detection on audit log | Token TTL limits blast radius. Revoke token immediately. | < 5 min |
| **Raft snapshot backup failure** | Backup gap | Backup monitoring | Alert. Fix backup job. Raft data remains on disk. | Immediate (fix backup) |

---

## 9. Security

### Authentication Chain

```
Authentication Methods (layered):
─────────────────────────────────
1. Kubernetes Auth:
   Pod SA JWT → Vault → k8s TokenReview API → validated
   Bound to: SA name, namespace, audience
   Token TTL: 30 min, max_ttl: 4h

2. AppRole Auth (for non-k8s workloads):
   role_id (known to app) + secret_id (fetched at deploy time, single-use)
   → Vault → validated
   Secret_id: single-use, TTL 60s, CIDR-bound

3. LDAP Auth (for human operators):
   username + password → Vault → LDAP server → validated
   MFA required for production access (via Vault Sentinel/MFA)

4. OIDC Auth (for CLI/UI):
   Browser redirect → IdP → JWT → Vault → validated
   Used for vault UI and CLI login for humans
```

### Authorization Model

```
Vault Policy Language (HCL):
───────────────────────────
# myapp-read policy
path "secret/data/myapp/*" {
  capabilities = ["read", "list"]
}

path "database/creds/myapp-role" {
  capabilities = ["read"]
}

path "transit/encrypt/myapp-key" {
  capabilities = ["update"]
}

# Deny production secrets to non-prod service accounts
path "secret/data/prod/*" {
  capabilities = ["deny"]
}

Policy Hierarchy:
────────────────
- root policy: full access (only for initial setup, then revoked)
- admin policy: manage all secrets, engines, auth methods
- team-admin policy: manage secrets under team prefix
- app-read policy: read specific secrets for one application
- default policy: minimal (token self-lookup, renewal)

Sentinel Policies (Vault Enterprise):
─────────────────────────────────────
# Require MFA for production secret writes
sentinel {
  enforcement_level = "hard-mandatory"
  rule {
    request.path matches "secret/data/prod/*" and
    request.operation in ["create", "update", "delete"] and
    not identity.entity.metadata.mfa_verified
  }
}
```

### Audit Trail

- **Vault audit device:** Every request/response logged (blocking: Vault will not serve if audit backend is down, ensuring completeness).
- **Log format:** JSON with request (path, operation, token accessor, source IP) and response (status, wrapping info).
- **Sensitive data:** Plaintext values are HMAC'd in audit logs (not logged in clear). Token values are hashed.
- **Backends:** Primary: syslog -> Kafka -> Elasticsearch. Secondary: local file (fallback).
- **Retention:** Elasticsearch: 90 days. S3: 7 years.
- **Alerting:** Real-time alerts on: root token usage, secret deletion, policy changes, failed authentications > threshold.

### Threat Model

| Threat | Likelihood | Impact | Mitigation |
|--------|------------|--------|------------|
| **Unseal key compromise** | Low | Critical (full secret access) | Shamir split across 5 holders. HSM auto-unseal eliminates key shares in production. |
| **Root token usage** | Low (should be revoked) | Critical | Root token revoked after init. Alert on any root token generation. |
| **Vault server compromise** | Low | Critical | (1) Sealed data is encrypted at rest. (2) Memory-only decrypted data (not persisted to disk). (3) mTLS for all Vault communication. |
| **Service account token theft** | Medium | High | Short TTL (30 min), audience binding, scoped policies. |
| **Insider threat (admin reads secrets)** | Medium | High | Audit every access. Response wrapping for sensitive secrets. Separation of duties. |
| **Vault Agent memory dump** | Low | Medium | Secrets in Agent memory are not swapped to disk (mlock). Agent runs with minimal privileges. |
| **Denial of service on Vault** | Medium | High | Rate limiting per token. Multiple Vault nodes. Agent caching absorbs spikes. |
| **Supply chain attack (malicious plugin)** | Low | Critical | Only official Vault plugins. Plugin hash verification. No custom plugins in production. |

---

## 10. Incremental Rollout

**Phase 1 (Week 1-2): Vault cluster deployment**
- Deploy 5-node Vault cluster with Raft storage on dedicated bare-metal nodes.
- Configure auto-unseal (HSM or Transit seal).
- Set up audit backend (Kafka -> ES).
- Create initial admin policies.

**Phase 2 (Week 3-4): Kubernetes auth + static secrets**
- Configure Kubernetes auth method.
- Migrate existing k8s Secrets to Vault KV v2.
- Deploy Vault Agent injector (mutating webhook).
- Pilot with 5 non-critical services in dev namespace.

**Phase 3 (Week 5-6): Dynamic secrets**
- Configure Database engine for MySQL.
- Create dynamic roles for 10 services.
- Monitor lease lifecycle, credential generation latency.
- Validate revocation reliability.

**Phase 4 (Week 7-8): Production rollout**
- Expand to all namespaces, one team at a time.
- Deploy Vault Secrets Operator for teams that prefer k8s-native Secrets.
- Transit engine for PII encryption.

**Phase 5 (Week 9-10): Advanced features**
- Secret rotation automation.
- Performance replication for multi-datacenter.
- Sentinel policies for compliance enforcement.

**Interviewer Q&As:**

**Q1: How do you migrate existing plaintext secrets in k8s to Vault?**
A: (1) Inventory all k8s Secrets across namespaces. (2) Write each to Vault KV v2 under a structured path (`secret/data/{namespace}/{app}/{secret-name}`). (3) Update pod manifests to use Vault Agent annotations. (4) Verify the app works with Vault-sourced secrets. (5) Delete the k8s Secret. We use a migration tool that automates steps 1-3 and generates the annotation patches.

**Q2: What is the rollback plan if Vault causes pod startup failures?**
A: (1) Disable the Vault Agent injector webhook (pods start without the sidecar). (2) Re-create k8s Secrets from Vault backup (we maintain a k8s Secret mirror for 30 days after migration). (3) Rollback is instant because removing the webhook annotation lets pods start without Vault.

**Q3: How do you handle the "chicken and egg" problem -- Vault needs secrets (TLS certs, DB creds) to start?**
A: Vault's own TLS certificates are provisioned during initial deployment (manual or via Ansible). The Raft cluster uses built-in TLS. The auto-unseal credential (HSM PIN or KMS key) is the only "bootstrapping" secret, stored in the HSM itself or in a sealed config file on the Vault node. After Vault starts, it manages its own certificate renewal via the PKI engine.

**Q4: How do you test dynamic secret generation in a staging environment?**
A: A separate Vault cluster for staging, configured with a separate MySQL staging instance. Dynamic roles have the same SQL templates as production but connect to staging DB. We run automated tests: generate 1,000 credentials in 5 minutes, verify MySQL user creation, wait for TTL expiry, verify MySQL user deletion.

**Q5: How do you handle the case where a team resists migrating from k8s Secrets to Vault?**
A: We offer Vault Secrets Operator (VSO) as a bridge: Vault syncs secrets to native k8s Secrets, so the application code doesn't change at all. The team just adds a VaultStaticSecret CRD pointing to the Vault path, and VSO creates the k8s Secret automatically. This provides Vault's benefits (audit, rotation, dynamic secrets) without application changes.

---

## 11. Trade-offs & Decision Log

| Decision | Trade-off | Rationale |
|----------|-----------|-----------|
| **Raft integrated storage vs. Consul backend** | Simpler operations vs. Consul's service mesh features | Raft is built into Vault since 1.4, eliminates Consul dependency. Vault is the only consumer of this storage. Fewer moving parts. |
| **Auto-unseal (HSM) vs. manual Shamir** | Availability vs. human-in-the-loop security | On bare metal with automated deployments, manual unseal is a major ops burden. HSM provides equivalent security with automatic availability. |
| **Vault Agent sidecar vs. CSI driver** | Per-pod overhead (sidecar) vs. simpler mount | Sidecar supports auto-renewal and template rendering, which CSI driver does not. The ~30 MB memory per sidecar is acceptable for the functionality gained. |
| **Blocking audit vs. non-blocking** | Availability (Vault halts if audit fails) vs. audit completeness | Blocking is the safer default. If Kafka is down, Vault halts -- this is a feature, not a bug, for compliance. We mitigate with dual audit backends. |
| **Dynamic secrets vs. static rotation** | Complexity vs. security | Dynamic secrets add Vault dependency on the critical path. But they eliminate shared passwords, reduce blast radius, and provide automatic cleanup. The security benefit outweighs the complexity. |
| **KV v2 (versioned) vs. KV v1** | Storage overhead vs. rollback capability | Versioning uses ~10x storage (10 versions default) but enables instant rollback of bad secret updates. Critical for production incident response. |
| **Transit engine vs. application-level encryption** | Centralized key management vs. latency | Transit adds 2-5 ms per encrypt/decrypt. For most applications this is acceptable. The benefit is centralized key management, rotation, and audit. For ultra-low-latency paths, consider client-side encryption with keys fetched from Vault at startup. |
| **Single Vault cluster vs. per-team clusters** | Operational simplicity vs. blast radius | Single cluster for consistency and auditability. Namespace isolation within Vault (Vault Enterprise) for multi-tenancy. Per-team clusters only if regulatory isolation is required. |

---

## 12. Agentic AI Integration

### AI-Powered Secrets Management Intelligence

**1. Secret Sprawl Detection**
- An agent scans all Vault KV paths and compares against active service deployments.
- Identifies orphaned secrets: secrets with no corresponding deployment or last-accessed > 90 days.
- CLI: `vault-ai detect-sprawl --namespace myapp --last-accessed-threshold 90d`
- Generates cleanup recommendations with impact assessment.

**2. Dynamic Credential Anomaly Detection**
- Train a model on normal credential issuance patterns: time of day, source namespace, credential type, frequency.
- Flag anomalies: "Service account X in namespace Y just requested 50 MySQL credentials in 1 minute (normal: 2/min)."
- Automated response: rate-limit the entity, alert security team, optionally revoke suspicious leases.

**3. Policy Generation from Usage Patterns**
- Analyze audit logs to determine what secrets each service actually accesses.
- Generate least-privilege Vault policies: "Service myapp-worker only reads `secret/data/myapp/worker-config` and `database/creds/worker-role`. Current policy allows `secret/data/myapp/*` -- recommend scoping."
- Output: HCL policy file for human review.

**4. Secret Rotation Risk Assessment**
- Before rotating a secret, the agent identifies all consumers (from audit logs), all dependent services (from k8s pod specs), and potential impact.
- "Rotating `secret/data/shared/redis-password` will affect 12 services across 3 namespaces. 8 use Vault Agent (auto-reload). 4 require manual restart."
- Generates a rotation runbook with a step-by-step plan.

**5. Incident Response: Credential Compromise**
- On alert of compromised credential:
  - Agent immediately revokes the lease.
  - Generates a new credential for affected services.
  - Restarts affected pods (rolling restart via k8s).
  - Opens incident ticket with timeline.
  - Generates forensic report from audit logs.

**6. Natural Language Vault Queries**
- "Show me all services in the payment namespace that have access to production database credentials."
- Agent translates to: query Vault policies + auth role bindings + audit logs. Returns structured report.
- Supports follow-up: "Revoke access for service X" -- agent generates the policy change for approval.

---

## 13. Complete Interviewer Q&A Bank

**Q1: Explain how Vault's seal/unseal mechanism protects secrets at rest.**
A: All secrets in Vault's storage backend are encrypted with a data encryption key (DEK). The DEK is encrypted with the master key. The master key is encrypted by the unseal key. In manual mode, the unseal key is split into N shares using Shamir's Secret Sharing; K shares are needed to reconstruct it. In auto-unseal mode, a KMS/HSM encrypts the master key directly. When Vault is sealed, the master key and DEK exist only in encrypted form -- all data is encrypted at rest. Unsealing decrypts the master key in memory, which then decrypts the DEK, allowing Vault to serve requests. The master key never touches disk in plaintext.

**Q2: What are the trade-offs between Vault's Raft storage and Consul storage backend?**
A: Raft (integrated storage, Vault 1.4+): simpler to operate (no separate Consul cluster), data encrypted within Vault, backup via `vault operator raft snapshot`. Consul: more mature HA, supports Vault + other services, but Consul must be separately operated, data in Consul is encrypted by Vault (not by Consul), and it adds an operational dependency. For most deployments, Raft is now recommended.

**Q3: How does Vault handle high availability with Raft?**
A: 5 nodes form a Raft cluster. One is elected leader (active), 4 are standbys. The leader handles all writes and replicates to standbys. On leader failure, a new leader is elected (< 10 seconds). Standbys forward requests to the leader. With Performance Standby (Vault 1.11+), standbys can serve read requests directly, scaling read throughput.

**Q4: Explain the difference between Vault's auth methods and secret engines.**
A: Auth methods verify identity (Kubernetes JWT, LDAP credentials, AWS IAM) and return a Vault token with attached policies. Secret engines manage secret lifecycle (KV stores key-value pairs, Database generates dynamic credentials, Transit encrypts data, PKI issues certificates). Auth is "who are you?"; secret engines are "what can you access?"

**Q5: How would you handle a scenario where Vault's audit backend is down?**
A: By default, Vault blocks all operations if no audit backend is available (this is a security feature, not a bug). Mitigation: (1) Configure two audit backends (syslog + local file). (2) If both fail, Vault halts. (3) For extreme availability requirements, configure one backend as `non-blocking` (accepts the compliance risk of potential audit gaps). (4) Monitor audit backend health and alert before both fail.

**Q6: How do dynamic secrets reduce the blast radius of a credential leak?**
A: (1) Each pod gets a unique credential (not shared). A leak compromises one pod, not all pods. (2) Short TTL (1 hour) means the credential auto-expires. The attacker has limited time to exploit it. (3) Vault can revoke the specific lease immediately upon detection. (4) Unique credential names (`v-k8s-myapp-xxxx`) make forensics easy -- you can identify exactly which pod was compromised from the MySQL audit log.

**Q7: What is response wrapping, and when would you use it?**
A: Response wrapping: instead of returning the secret directly, Vault wraps it in a single-use token with a short TTL. The client unwraps the token to get the secret. If the wrapping token is intercepted and used, the original client's unwrap fails, alerting you to the interception. Use case: passing secrets between systems (e.g., CI/CD deploying a secret_id to a new service instance).

**Q8: How would you implement zero-downtime secret rotation?**
A: (1) Write the new secret version to Vault KV v2 (old version still available). (2) Vault Agent template detects the change and re-renders the config file. (3) Agent sends SIGHUP to the application process (or the application watches the file via inotify). (4) Application reloads with the new secret. (5) For database credentials: dynamic secrets handle this automatically (new credentials on renewal). For static database passwords: Vault's `database/rotate-root` rotates the shared password, and all apps using dynamic secrets are unaffected.

**Q9: How does Vault's Kubernetes auth method work internally?**
A: (1) Client (Vault Agent) reads the projected SA token from `/var/run/secrets/tokens/vault-token`. (2) Client sends POST to `/auth/kubernetes/login` with the JWT and role name. (3) Vault extracts the JWT and calls the Kubernetes TokenReview API to validate it. (4) Vault checks that the SA name, namespace, and audience match the configured role. (5) If valid, Vault returns a Vault token with the policies defined in the role configuration. The projected SA token is audience-bound (to "vault") and short-lived (configured via `expirationSeconds`).

**Q10: How do you back up and restore Vault?**
A: (1) Backup: `vault operator raft snapshot save backup.snap`. The snapshot contains all Vault data (encrypted). Schedule hourly. Store in S3 with versioning. (2) Restore: `vault operator raft snapshot restore backup.snap` on a fresh Vault cluster. The cluster initializes with the backup data and requires unsealing (same unseal keys). (3) Backup testing: monthly restore to a test cluster to verify integrity.

**Q11: What is Vault's "cubbyhole" engine?**
A: Cubbyhole is a per-token private namespace. Secrets stored in cubbyhole are accessible only to the token that created them. When the token expires, the cubbyhole secrets are destroyed. Use case: temporary staging area for secrets during deployment or for response wrapping. It's the safest place to store short-lived sensitive data because it's completely isolated.

**Q12: How would you use Vault for database credential management across multiple MySQL clusters?**
A: (1) Configure a separate database engine mount per MySQL cluster: `database/mysql-prod-us`, `database/mysql-prod-eu`. (2) Each mount has its own connection config (host, admin credentials). (3) Define roles per application per cluster: `database/mysql-prod-us/creds/myapp-role`. (4) Vault policies scope each service to its cluster. (5) Root credential rotation is per-cluster. (6) Monitor active leases per cluster to stay within MySQL `max_connections`.

**Q13: How do you handle Vault in a disaster recovery scenario?**
A: (1) DR Replication (Vault Enterprise): secondary cluster receives a continuous stream of encrypted data from primary. On primary failure, promote secondary (`vault operator raft promote`). RPO: seconds (async replication lag). (2) Manual DR: restore from Raft snapshot backup. RPO: up to 1 hour (backup interval). (3) In both cases, new cluster needs the same unseal mechanism (HSM/KMS keys must be available at DR site).

**Q14: What are the risks of using Vault's Transit engine for all encryption needs?**
A: (1) Latency: 2-5 ms per operation may be too slow for high-throughput data pipelines. Consider fetching the data key at startup and encrypting locally (envelope encryption). (2) Availability dependency: if Vault is down, encryption/decryption fails. Mitigate with caching and retry. (3) Cost at scale: at 50,000 encrypt ops/sec, you need a dedicated Transit Vault cluster. (4) Not suitable for streaming encryption (file-level, disk-level) -- use OS-level encryption for that.

**Q15: How would you implement secret zero (the bootstrap secret problem)?**
A: "Secret zero" is the initial credential that lets a service authenticate to Vault. Solutions: (1) Kubernetes auth: the secret zero is the projected SA token, which is provided by the kubelet (trusted platform). (2) AppRole: the role_id is baked into the deployment config, the secret_id is injected at deploy time via a trusted CI/CD pipeline (e.g., the CI/CD runner fetches the secret_id from Vault using its own credentials). (3) AWS IAM auth: the secret zero is the IAM role attached to the EC2 instance. In all cases, the secret zero is provided by a trusted platform layer, not by the application.

**Q16: How do you handle multi-tenancy in Vault?**
A: (1) Vault Namespaces (Enterprise): each team/tenant gets a namespace (`secret/teamA/`, `secret/teamB/`) with isolated policies, auth methods, and audit. (2) Open-source: use path-based isolation with strict policies. Each team's secrets live under a path prefix, and policies restrict access to that prefix. (3) In both cases, a platform admin manages the Vault cluster, and team admins manage their own namespace/path.

**Q17: What monitoring and alerting should you set up for a production Vault cluster?**
A: Key metrics: (1) `vault.core.handle_request` (latency). (2) `vault.expire.num_leases` (active lease count). (3) `vault.runtime.alloc_bytes` (memory usage). (4) `vault.raft.leader.lastContact` (Raft health). (5) `vault.audit.log_request` (audit backend health). (6) `vault.token.creation` (auth load). Alerts: (a) Leader last contact > 5s (Raft instability). (b) Active lease count > 80% of max. (c) Audit backend errors. (d) Seal status = sealed (critical). (e) Storage utilization > 80%.

---

## 14. References

- [HashiCorp Vault Documentation](https://developer.hashicorp.com/vault/docs)
- [Vault Architecture](https://developer.hashicorp.com/vault/docs/internals/architecture)
- [Vault Seal/Unseal](https://developer.hashicorp.com/vault/docs/concepts/seal)
- [Shamir's Secret Sharing](https://en.wikipedia.org/wiki/Shamir%27s_secret_sharing)
- [Vault Integrated Storage (Raft)](https://developer.hashicorp.com/vault/docs/configuration/storage/raft)
- [Vault Database Secrets Engine](https://developer.hashicorp.com/vault/docs/secrets/databases)
- [Vault Transit Engine](https://developer.hashicorp.com/vault/docs/secrets/transit)
- [Vault Kubernetes Auth Method](https://developer.hashicorp.com/vault/docs/auth/kubernetes)
- [Vault Agent](https://developer.hashicorp.com/vault/docs/agent-and-proxy/agent)
- [Vault Secrets Operator](https://developer.hashicorp.com/vault/docs/platform/k8s/vso)
- [Kubernetes Projected Volumes](https://kubernetes.io/docs/concepts/storage/projected-volumes/)
- [Kubernetes TokenRequest API](https://kubernetes.io/docs/reference/kubernetes-api/authentication-resources/token-request-v1/)
