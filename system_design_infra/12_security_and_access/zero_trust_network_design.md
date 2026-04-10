# System Design: Zero Trust Network Design

> **Relevance to role:** Traditional perimeter security ("castle and moat") fails in a cloud infrastructure platform where workloads run on bare-metal nodes, Kubernetes pods, and VMs across multiple network segments. Compromising one host should not grant access to the entire network. Zero trust architecture eliminates implicit trust based on network location -- every request must carry a verifiable identity, every access decision is logged, and micro-segmentation limits blast radius. For this role, zero trust is the architectural foundation connecting RBAC, secrets management, certificate management, and API key management into a coherent security posture.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | **Identity-based access**: every request (human, service, workload) must carry a verifiable cryptographic identity. |
| FR-2 | **Micro-segmentation**: enforce network policies at pod, VM, and node levels (Kubernetes NetworkPolicy, eBPF-based enforcement). |
| FR-3 | **mTLS everywhere**: all service-to-service communication encrypted and mutually authenticated. |
| FR-4 | **Context-aware access proxy**: BeyondCorp-style proxy for human access to infrastructure (replaces VPN). |
| FR-5 | **Continuous verification**: re-authenticate on sensitive operations; session re-evaluation on context change. |
| FR-6 | **Device trust**: endpoint verification before granting access to production systems. |
| FR-7 | **Service mesh authorization**: fine-grained AuthorizationPolicy for service-to-service calls. |
| FR-8 | **Audit everything**: every network connection, every access decision, every policy evaluation logged. |
| FR-9 | **Dynamic policy**: policies update in real-time based on threat intelligence, user behavior, device posture. |
| FR-10 | **Zero trust for infrastructure management**: kubectl, SSH, database access all go through identity-verified proxies. |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | mTLS handshake latency overhead | < 2 ms (established connection), < 10 ms (new connection) |
| NFR-2 | Network policy enforcement latency | < 0.1 ms per packet (eBPF dataplane) |
| NFR-3 | Availability | 99.99% for the data plane; 99.9% for control plane |
| NFR-4 | Policy propagation | < 5 seconds from change to enforcement |
| NFR-5 | Audit coverage | 100% of access decisions logged |
| NFR-6 | Throughput | Support 1M+ concurrent mTLS connections |
| NFR-7 | Context-aware proxy latency | < 50 ms added to human access paths |

### Constraints & Assumptions

- Bare-metal Kubernetes 1.28+ with 500+ nodes.
- Service mesh: Istio (with Envoy sidecars) or Cilium service mesh (eBPF-based).
- CNI: Cilium (eBPF-based networking and security enforcement).
- Identity: SPIFFE/SPIRE for workload identity (detailed in certificate_lifecycle_management.md).
- Secrets: Vault (detailed in secrets_management_system.md).
- Authorization: OPA/Gatekeeper + SpiceDB (detailed in rbac_authorization_system.md).
- Human access: BeyondCorp-style access proxy for kubectl, SSH, database, and admin consoles.
- No VPN: all access goes through identity-aware proxies.

### Out of Scope

- DDoS protection (handled by edge infrastructure).
- WAF (Web Application Firewall) for application-layer attacks.
- Physical security of data centers.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Bare-metal nodes | 500 | Production fleet |
| Kubernetes pods (peak) | 50,000 | Running concurrently |
| Concurrent mTLS connections | 1,000,000+ | Each pod: ~20 connections to other pods |
| Network policies (Cilium) | 10,000+ | Namespace-level + service-level policies |
| Service mesh AuthorizationPolicies | 5,000+ | Per-service ingress/egress rules |
| Human users accessing infrastructure | 500 concurrent | Engineers + SREs + on-call |
| NetworkPolicy evaluations/sec | 10,000,000+ | Per-packet enforcement via eBPF |
| Access proxy requests/sec | 5,000 | kubectl, SSH, DB, admin console |
| Audit events/sec | 500,000+ | Network + application + access decisions |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| eBPF NetworkPolicy enforcement | < 0.1 ms per packet |
| mTLS handshake (Envoy, first connection) | < 10 ms |
| mTLS overhead (established connection) | < 0.5 ms per request |
| Access proxy (human requests) | < 50 ms added latency |
| Policy propagation (change to enforcement) | < 5 seconds |
| SPIFFE SVID issuance | < 100 ms |
| OPA policy evaluation | < 1 ms |

### Storage Estimates

| Data | Size | Calculation |
|------|------|-------------|
| Network policies (Cilium) | ~100 MB | 10,000 policies x 10 KB |
| AuthorizationPolicies (Istio/Envoy) | ~50 MB | 5,000 policies x 10 KB |
| eBPF maps (per node) | ~50 MB | Policy maps, connection tracking |
| Audit log (1 day) | ~10 TB | 500K events/sec x 86,400 x 0.5 util x 500 bytes |
| Device trust registry | ~50 MB | 10,000 devices x 5 KB |

### Bandwidth Estimates

| Flow | Bandwidth |
|------|-----------|
| mTLS overhead (TLS record headers) | ~5% of total traffic |
| Policy sync (Cilium agent) | ~1 MB/sec per node (burst) |
| Audit event stream | ~250 MB/sec (500K events/sec x 500 bytes) |
| SPIRE trust bundle distribution | < 1 MB/sec |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                    ZERO TRUST ARCHITECTURE LAYERS                      │
│                                                                       │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │  LAYER 1: IDENTITY                                             │  │
│  │                                                                │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌────────────────────┐  │  │
│  │  │  SPIFFE/SPIRE │  │  OIDC IdP    │  │  Device Trust      │  │  │
│  │  │  (Workload    │  │  (Human      │  │  (Endpoint          │  │  │
│  │  │   Identity)   │  │   Identity)  │  │   Verification)    │  │  │
│  │  │              │  │              │  │                    │  │  │
│  │  │  X.509 SVIDs │  │  JWT tokens  │  │  Device posture   │  │  │
│  │  │  JWT SVIDs   │  │  MFA claims  │  │  certificates     │  │  │
│  │  └──────────────┘  └──────────────┘  └────────────────────┘  │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                       │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │  LAYER 2: NETWORK ENFORCEMENT                                  │  │
│  │                                                                │  │
│  │  ┌──────────────────────┐  ┌───────────────────────────────┐  │  │
│  │  │  Cilium (eBPF)        │  │  Service Mesh (Istio/Envoy)  │  │  │
│  │  │                       │  │                               │  │  │
│  │  │  - NetworkPolicy      │  │  - mTLS (automatic)          │  │  │
│  │  │  - CiliumNetworkPolicy│  │  - AuthorizationPolicy       │  │  │
│  │  │  - DNS-aware policies │  │  - RequestAuthentication     │  │  │
│  │  │  - L3/L4/L7 filtering│  │  - PeerAuthentication        │  │  │
│  │  │  - Identity-based     │  │  - Traffic routing           │  │  │
│  │  │    (not IP-based)     │  │                               │  │  │
│  │  └──────────────────────┘  └───────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                       │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │  LAYER 3: APPLICATION ENFORCEMENT                              │  │
│  │                                                                │  │
│  │  ┌──────────────────────┐  ┌───────────────────────────────┐  │  │
│  │  │  OPA/Gatekeeper       │  │  JWT Validation               │  │  │
│  │  │  (Policy evaluation)  │  │  (Per-request authn/authz)   │  │  │
│  │  │                       │  │                               │  │  │
│  │  │  - RBAC policies      │  │  - Verify JWT signature      │  │  │
│  │  │  - ABAC conditions    │  │  - Check claims (sub, aud)   │  │  │
│  │  │  - ReBAC relationships│  │  - Enforce audience binding  │  │  │
│  │  └──────────────────────┘  └───────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                       │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │  LAYER 4: DATA PROTECTION                                     │  │
│  │                                                                │  │
│  │  ┌──────────────────────┐  ┌───────────────────────────────┐  │  │
│  │  │  Encryption at Rest   │  │  Encryption in Transit        │  │  │
│  │  │  (LUKS, Vault Transit)│  │  (mTLS, TLS 1.3)             │  │  │
│  │  └──────────────────────┘  └───────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                       │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │  LAYER 5: OBSERVABILITY                                        │  │
│  │                                                                │  │
│  │  ┌──────────────────────┐  ┌───────────────────────────────┐  │  │
│  │  │  Hubble (Cilium)      │  │  Audit Log Pipeline           │  │  │
│  │  │  (Network flow logs)  │  │  (Kafka → ES → S3)           │  │  │
│  │  └──────────────────────┘  └───────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│                    HUMAN ACCESS (BeyondCorp Model)                     │
│                                                                       │
│  Engineer's Laptop                                                    │
│  ┌────────────────────────┐                                          │
│  │  Device Agent           │  Checks: OS patched, disk encrypted,    │
│  │  (Endpoint Verification)│  approved software, no malware          │
│  └────────────┬────────────┘                                          │
│               │                                                       │
│  ┌────────────▼────────────┐                                          │
│  │  OIDC Authentication    │  Okta/Azure AD + MFA (hardware key)     │
│  └────────────┬────────────┘                                          │
│               │                                                       │
│  ┌────────────▼──────────────────────────────────────────────────┐   │
│  │  Context-Aware Access Proxy                                    │   │
│  │  (Identity + Device Trust + Context → Access Decision)        │   │
│  │                                                                │   │
│  │  ┌────────────┐  ┌────────────┐  ┌──────────────────────┐   │   │
│  │  │ kubectl     │  │ SSH Proxy  │  │ Database Proxy       │   │   │
│  │  │ Proxy       │  │ (Teleport/ │  │ (ProxySQL +          │   │   │
│  │  │ (kube-gate) │  │  Boundary) │  │  identity-aware)     │   │   │
│  │  └─────┬──────┘  └─────┬──────┘  └──────────┬───────────┘   │   │
│  │        │                │                     │               │   │
│  └────────┼────────────────┼─────────────────────┼───────────────┘   │
│           │                │                     │                    │
│           ▼                ▼                     ▼                    │
│     k8s API Server    Bare-metal Nodes      MySQL Clusters           │
└──────────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **SPIFFE/SPIRE** | Workload identity for all pods and services. Issues X.509/JWT SVIDs. |
| **OIDC IdP** | Human identity (Okta, Azure AD). Issues JWTs with user/group/MFA claims. |
| **Device Trust Agent** | Verifies endpoint posture: OS patched, disk encrypted, no malware, approved device. |
| **Cilium (eBPF)** | CNI with eBPF-based network policy enforcement. Identity-aware (Cilium identity, not IP). DNS-aware. L3/L4/L7 filtering. |
| **Service Mesh (Istio/Envoy)** | Automatic mTLS, AuthorizationPolicy, RequestAuthentication. Application-layer enforcement. |
| **OPA/Gatekeeper** | Policy-as-code evaluation for authorization decisions. |
| **Context-Aware Access Proxy** | BeyondCorp-style proxy for human access. Evaluates identity + device + context for every request. |
| **kubectl Proxy** | Proxies kubectl requests through identity verification. Replaces direct kube-apiserver access. |
| **SSH Proxy (Teleport/Boundary)** | Proxies SSH connections with identity, MFA, session recording. No direct SSH to nodes. |
| **Database Proxy** | Proxies MySQL connections with identity-based access control. |
| **Hubble** | Cilium's observability layer. Network flow logs with identity labels. |
| **Audit Pipeline** | Kafka -> Elasticsearch -> S3 for all access decisions and network flows. |

### Data Flows

1. **Service-to-service (mTLS):** Pod A (Envoy sidecar) -> mTLS handshake with Pod B (Envoy sidecar) -> both present SPIFFE SVIDs -> identity verified -> Envoy checks AuthorizationPolicy -> allow/deny -> traffic flows encrypted.

2. **Human kubectl access:** Engineer -> OIDC login (MFA) -> Device trust check -> kubectl proxy -> proxy verifies JWT + device cert + evaluates context (time, location, risk score) -> forwards to kube-apiserver -> kube-apiserver RBAC check -> response.

3. **Human SSH access:** Engineer -> Teleport/Boundary proxy -> OIDC + MFA + device trust -> session certificate issued (short-lived, 8h) -> SSH to target node -> session recorded -> audit logged.

4. **Network policy enforcement:** Pod A sends packet to Pod B -> Cilium eBPF program in kernel -> checks CiliumNetworkPolicy (identity-based, not IP-based) -> allow/deny -> if denied, packet dropped at kernel level.

---

## 4. Data Model

### Core Entities & Schema (Full SQL)

```sql
-- ============================================================
-- WORKLOAD IDENTITIES (complementing SPIRE's internal store)
-- ============================================================
CREATE TABLE workload_identities (
    identity_id      BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    spiffe_id        VARCHAR(512) NOT NULL UNIQUE,
    trust_domain     VARCHAR(255) NOT NULL,
    workload_type    ENUM('pod', 'vm', 'bare_metal', 'job', 'function') NOT NULL,
    kubernetes_ns    VARCHAR(255) DEFAULT NULL,
    kubernetes_sa    VARCHAR(255) DEFAULT NULL,
    node_name        VARCHAR(255) DEFAULT NULL,
    labels           JSON DEFAULT NULL,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_seen        TIMESTAMP DEFAULT NULL,
    is_active        BOOLEAN NOT NULL DEFAULT TRUE,
    INDEX idx_spiffe (spiffe_id),
    INDEX idx_k8s (kubernetes_ns, kubernetes_sa),
    INDEX idx_node (node_name),
    INDEX idx_active (is_active, last_seen)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- DEVICE TRUST REGISTRY
-- ============================================================
CREATE TABLE device_registry (
    device_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    device_uuid      VARCHAR(128) NOT NULL UNIQUE COMMENT 'Hardware UUID or TPM-bound ID',
    owner_email      VARCHAR(255) NOT NULL,
    device_type      ENUM('laptop', 'desktop', 'mobile', 'server') NOT NULL,
    os_type          ENUM('macos', 'windows', 'linux', 'ios', 'android') NOT NULL,
    os_version       VARCHAR(64) DEFAULT NULL,
    disk_encrypted   BOOLEAN NOT NULL DEFAULT FALSE,
    firewall_enabled BOOLEAN NOT NULL DEFAULT FALSE,
    last_os_update   TIMESTAMP DEFAULT NULL,
    device_cert_hash VARCHAR(64) DEFAULT NULL COMMENT 'SHA-256 of device certificate',
    trust_score      INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0-100, computed from posture',
    enrollment_date  TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    last_verified    TIMESTAMP DEFAULT NULL,
    is_compliant     BOOLEAN NOT NULL DEFAULT FALSE,
    compliance_issues JSON DEFAULT NULL COMMENT '["os_outdated", "firewall_disabled"]',
    is_active        BOOLEAN NOT NULL DEFAULT TRUE,
    INDEX idx_owner (owner_email),
    INDEX idx_compliant (is_compliant),
    INDEX idx_trust_score (trust_score),
    INDEX idx_last_verified (last_verified)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- NETWORK POLICIES (tracking/inventory, actual enforcement in Cilium/k8s)
-- ============================================================
CREATE TABLE network_policies (
    policy_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    policy_name      VARCHAR(255) NOT NULL,
    policy_type      ENUM('k8s_network_policy', 'cilium_network_policy',
                          'cilium_clusterwide_policy', 'istio_authorization_policy',
                          'istio_peer_authentication') NOT NULL,
    namespace        VARCHAR(255) DEFAULT NULL COMMENT 'NULL for cluster-wide',
    policy_yaml      LONGTEXT NOT NULL,
    status           ENUM('active', 'pending', 'disabled', 'testing') NOT NULL DEFAULT 'active',
    created_by       VARCHAR(255) NOT NULL,
    approved_by      VARCHAR(255) DEFAULT NULL,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    applied_at       TIMESTAMP DEFAULT NULL,
    INDEX idx_type_ns (policy_type, namespace),
    INDEX idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- ACCESS PROXY SESSIONS (BeyondCorp)
-- ============================================================
CREATE TABLE access_proxy_sessions (
    session_id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    session_token    VARCHAR(128) NOT NULL UNIQUE,
    user_email       VARCHAR(255) NOT NULL,
    device_id        BIGINT UNSIGNED DEFAULT NULL,
    proxy_type       ENUM('kubectl', 'ssh', 'database', 'admin_console', 'web_app') NOT NULL,
    target_resource  VARCHAR(512) NOT NULL COMMENT 'e.g., cluster:prod, node:worker-001, db:mysql-prod',
    access_level     ENUM('read', 'write', 'admin') NOT NULL,
    mfa_verified     BOOLEAN NOT NULL DEFAULT FALSE,
    mfa_method       ENUM('hardware_key', 'totp', 'push', 'none') DEFAULT NULL,
    device_trust_score INT UNSIGNED DEFAULT NULL,
    context_json     JSON DEFAULT NULL COMMENT '{"ip": "1.2.3.4", "geo": "US-NYC", "time": "09:00"}',
    risk_score       INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0-100, computed from context',
    decision         ENUM('allow', 'deny', 'step_up_auth', 'limited_access') NOT NULL,
    deny_reason      VARCHAR(512) DEFAULT NULL,
    started_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at       TIMESTAMP NOT NULL,
    ended_at         TIMESTAMP DEFAULT NULL,
    bytes_transferred BIGINT UNSIGNED DEFAULT 0,
    commands_executed INT UNSIGNED DEFAULT 0,
    FOREIGN KEY (device_id) REFERENCES device_registry(device_id),
    INDEX idx_user (user_email, started_at),
    INDEX idx_device (device_id),
    INDEX idx_target (target_resource, started_at),
    INDEX idx_decision (decision, started_at),
    INDEX idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- NETWORK FLOW AUDIT LOG
-- ============================================================
CREATE TABLE network_flow_audit (
    flow_id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    timestamp_utc    TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    source_identity  VARCHAR(512) NOT NULL COMMENT 'SPIFFE ID or Cilium identity',
    source_namespace VARCHAR(255) DEFAULT NULL,
    source_pod       VARCHAR(255) DEFAULT NULL,
    source_ip        VARCHAR(45) NOT NULL,
    source_port      INT UNSIGNED NOT NULL,
    dest_identity    VARCHAR(512) NOT NULL,
    dest_namespace   VARCHAR(255) DEFAULT NULL,
    dest_pod         VARCHAR(255) DEFAULT NULL,
    dest_ip          VARCHAR(45) NOT NULL,
    dest_port        INT UNSIGNED NOT NULL,
    protocol         ENUM('TCP', 'UDP', 'ICMP', 'HTTP', 'gRPC', 'DNS') NOT NULL,
    http_method      VARCHAR(10) DEFAULT NULL,
    http_path        VARCHAR(512) DEFAULT NULL,
    http_status      INT UNSIGNED DEFAULT NULL,
    verdict          ENUM('forwarded', 'dropped', 'rejected', 'error') NOT NULL,
    policy_matched   VARCHAR(255) DEFAULT NULL COMMENT 'Policy that allowed/denied',
    encrypted        BOOLEAN NOT NULL DEFAULT FALSE COMMENT 'mTLS in use',
    bytes_total      BIGINT UNSIGNED DEFAULT 0,
    latency_ms       DECIMAL(8,2) DEFAULT NULL,
    INDEX idx_timestamp (timestamp_utc),
    INDEX idx_source (source_identity, timestamp_utc),
    INDEX idx_dest (dest_identity, timestamp_utc),
    INDEX idx_verdict (verdict, timestamp_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (UNIX_TIMESTAMP(timestamp_utc)) (
    -- Hourly partitions (high volume)
);

-- ============================================================
-- ZERO TRUST POLICY DECISIONS
-- ============================================================
CREATE TABLE zt_policy_decisions (
    decision_id      BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    timestamp_utc    TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    layer            ENUM('network', 'transport', 'application', 'data') NOT NULL,
    enforcement_point ENUM('cilium', 'envoy', 'opa', 'access_proxy', 'application') NOT NULL,
    source_identity  VARCHAR(512) NOT NULL,
    dest_identity    VARCHAR(512) NOT NULL,
    action_requested VARCHAR(128) NOT NULL,
    resource_type    VARCHAR(128) NOT NULL,
    decision         ENUM('allow', 'deny', 'rate_limited', 'step_up') NOT NULL,
    matched_policy   VARCHAR(255) DEFAULT NULL,
    evaluation_ms    DECIMAL(8,3) DEFAULT NULL,
    context_json     JSON DEFAULT NULL,
    request_id       VARCHAR(128) DEFAULT NULL,
    INDEX idx_timestamp (timestamp_utc),
    INDEX idx_source_dest (source_identity, dest_identity, timestamp_utc),
    INDEX idx_decision (decision, timestamp_utc),
    INDEX idx_layer (layer, enforcement_point, timestamp_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (UNIX_TIMESTAMP(timestamp_utc)) (
    -- Hourly partitions
);
```

### Database Selection

| Store | Use Case | Justification |
|-------|----------|---------------|
| **Cilium (eBPF maps)** | Runtime network policy enforcement | In-kernel, per-packet performance. |
| **etcd (Kubernetes)** | NetworkPolicy, CiliumNetworkPolicy, AuthorizationPolicy CRDs | Kubernetes-native storage. |
| **SPIRE (SQL/SQLite)** | SVID registrations, trust bundles | SPIRE's native datastore. |
| **MySQL 8.0** | Policy inventory, device registry, session tracking | Relational queries for management and compliance. |
| **Elasticsearch 8.x** | Network flow logs, audit search | Full-text search on flow data. Aggregation for dashboards. |
| **Redis** | Session cache, real-time risk scoring | Low-latency session validation. |

### Indexing Strategy

| Table | Key Index | Purpose |
|-------|-----------|---------|
| `workload_identities` | `(spiffe_id)` UNIQUE | Lookup workload by SPIFFE ID. |
| `device_registry` | `(owner_email)` | List devices per user. |
| `access_proxy_sessions` | `(user_email, started_at)` | Audit sessions per user. |
| `network_flow_audit` | PARTITION by hour + `(source_identity, timestamp_utc)` | Flow analysis per source. |
| `zt_policy_decisions` | PARTITION by hour + `(decision, timestamp_utc)` | Deny analysis. |

---

## 5. API Design

### REST Endpoints

```
# ── Device Trust ─────────────────────────────────────────────
POST   /api/v1/devices/enroll
  Body: {
    "device_uuid": "ABC-123-DEF",
    "os_type": "macos",
    "os_version": "14.4",
    "disk_encrypted": true,
    "device_cert": "-----BEGIN CERTIFICATE-----..."
  }

GET    /api/v1/devices                        # List enrolled devices
GET    /api/v1/devices/{device_id}            # Get device details
PUT    /api/v1/devices/{device_id}/posture    # Update device posture (from agent)
POST   /api/v1/devices/{device_id}/verify     # Trigger posture verification
DELETE /api/v1/devices/{device_id}            # Unenroll device

# ── Access Proxy ─────────────────────────────────────────────
POST   /api/v1/access/request
  Body: {
    "user_email": "alice@corp.com",
    "target_resource": "cluster:prod",
    "proxy_type": "kubectl",
    "access_level": "read",
    "justification": "Debugging pod restart issue",
    "duration": "4h"
  }
  Response: {
    "session_id": 789,
    "decision": "allow",
    "proxy_endpoint": "kubectl-proxy.corp.com:443",
    "kubeconfig": "...",
    "expires_at": "2026-04-09T19:00:00Z",
    "conditions": ["read-only", "audit-logged"]
  }

GET    /api/v1/access/sessions                # List active sessions
GET    /api/v1/access/sessions/{session_id}   # Get session details
POST   /api/v1/access/sessions/{session_id}/terminate  # Force-end session
GET    /api/v1/access/sessions/{session_id}/recording  # Get session recording

# ── Network Policy Management ────────────────────────────────
POST   /api/v1/network-policies               # Create/apply network policy
GET    /api/v1/network-policies               # List policies (filter: type, namespace)
GET    /api/v1/network-policies/{policy_id}   # Get policy details
PUT    /api/v1/network-policies/{policy_id}   # Update policy
DELETE /api/v1/network-policies/{policy_id}   # Delete policy
POST   /api/v1/network-policies/{policy_id}/test  # Test policy against simulated traffic

# ── Network Flow Analysis ────────────────────────────────────
GET    /api/v1/flows                          # Query network flows (filter: source, dest, verdict)
GET    /api/v1/flows/topology                 # Service dependency graph
GET    /api/v1/flows/anomalies               # Detected anomalies

# ── Zero Trust Health ────────────────────────────────────────
GET    /api/v1/zt/health                      # Overall zero trust health
GET    /api/v1/zt/coverage                    # mTLS coverage, policy coverage
GET    /api/v1/zt/compliance                  # Zero trust compliance report
GET    /api/v1/zt/metrics                     # Prometheus metrics
```

### CLI

```bash
# ── Device Management ──
platform-zt device enroll --uuid ABC-123-DEF --os macos
platform-zt device list --owner alice@corp.com
platform-zt device verify --uuid ABC-123-DEF
platform-zt device posture --uuid ABC-123-DEF

# ── Access Proxy ──
platform-zt access request --target cluster:prod --type kubectl \
  --level read --duration 4h --reason "Debugging INC-1234"

platform-zt access sessions --user alice@corp.com --active
platform-zt access terminate --session-id 789

# ── Network Policy ──
platform-zt policy create --name deny-default-ns \
  --type cilium --namespace default \
  --file deny-all.yaml

platform-zt policy list --namespace production
platform-zt policy test --name allow-frontend-to-api \
  --source frontend --dest api-server --port 8080

# ── Network Flow Analysis ──
platform-zt flows --source ns:frontend --dest ns:backend --last 1h
platform-zt flows --verdict dropped --last 24h
platform-zt flows topology --namespace production

# ── Zero Trust Health ──
platform-zt health
platform-zt coverage --detail
  # Output:
  # mTLS Coverage: 98.5% (750/762 connections encrypted)
  # NetworkPolicy Coverage: 95% (475/500 namespaces have deny-default)
  # Device Trust Coverage: 100% (all active users have compliant devices)

# ── SSH via Proxy ──
platform-zt ssh worker-node-042 --reason "Kernel debug"
# → Authenticated via OIDC + MFA
# → Device trust verified
# → Short-lived SSH certificate issued (8h)
# → Session recorded

# ── Database via Proxy ──
platform-zt db connect mysql-prod --database mydb --level read \
  --reason "Query optimization"
# → Dynamic credential issued via Vault
# → Connection proxied through identity-aware DB proxy
# → All queries logged
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Micro-Segmentation with Cilium eBPF

**Why it's hard:**
Traditional network policies (iptables) operate on IP addresses. In Kubernetes, IP addresses are ephemeral -- a pod's IP changes on every restart. Micro-segmentation must work on workload identity, not IP. It must enforce at kernel level for performance (millions of packets/sec) without adding latency. eBPF programs in the Linux kernel provide this, but they must be correct (a bug = kernel crash) and manageable at scale.

| Approach | Performance | Identity-Aware | L7 Support | Operational Complexity |
|----------|-------------|----------------|-----------|----------------------|
| **Kubernetes NetworkPolicy (iptables)** | Medium | No (IP-based) | No (L3/L4 only) | Low |
| **Calico (iptables/eBPF)** | High (eBPF mode) | Yes (with labels) | Limited | Medium |
| **Cilium (eBPF-native)** | Highest | Yes (Cilium identity) | Yes (HTTP, gRPC, DNS) | Medium |
| **Istio AuthorizationPolicy (Envoy L7)** | Medium (user-space proxy) | Yes (SPIFFE) | Full L7 | High |

**Selected: Cilium eBPF (L3/L4/L7 network enforcement) + Istio AuthorizationPolicy (application-layer)**

**Justification:** Cilium provides kernel-level enforcement with identity awareness (Cilium allocates numeric identities to endpoints based on labels, not IPs). It handles L3/L4 filtering with zero user-space overhead and can also do L7 filtering (HTTP, gRPC, DNS). Istio adds application-layer authorization (JWT validation, rich L7 policies). Together, they provide defense-in-depth at every layer.

**Implementation Detail:**

```
Cilium Identity Model:
─────────────────────
Cilium assigns a numeric identity to each endpoint (pod) based on its labels.
Two pods with the same labels share the same Cilium identity.
Network policies reference identities, not IPs.

Example:
  Pod A (labels: app=frontend, env=prod) → Cilium Identity 12345
  Pod B (labels: app=api-server, env=prod) → Cilium Identity 12346
  
  eBPF map:
    identity:12345 → identity:12346 port:8080 → ALLOW
    identity:12345 → identity:* port:* → DENY

CiliumNetworkPolicy (L3/L4):
───────────────────────────
apiVersion: cilium.io/v2
kind: CiliumNetworkPolicy
metadata:
  name: api-server-ingress
  namespace: production
spec:
  endpointSelector:
    matchLabels:
      app: api-server
  ingress:
  - fromEndpoints:
    - matchLabels:
        app: frontend
    toPorts:
    - ports:
      - port: "8080"
        protocol: TCP
  - fromEndpoints:
    - matchLabels:
        app: monitoring
    toPorts:
    - ports:
      - port: "9090"   # Prometheus metrics
        protocol: TCP

CiliumNetworkPolicy (L7 - HTTP):
──────────────────────────────
apiVersion: cilium.io/v2
kind: CiliumNetworkPolicy
metadata:
  name: api-server-l7
  namespace: production
spec:
  endpointSelector:
    matchLabels:
      app: api-server
  ingress:
  - fromEndpoints:
    - matchLabels:
        app: frontend
    toPorts:
    - ports:
      - port: "8080"
        protocol: TCP
      rules:
        http:
        - method: GET
          path: "/api/v1/products.*"
        - method: POST
          path: "/api/v1/orders"

CiliumClusterwideNetworkPolicy (default deny):
────────────────────────────────────────────
apiVersion: cilium.io/v2
kind: CiliumClusterwideNetworkPolicy
metadata:
  name: default-deny-all
spec:
  endpointSelector: {}  # Applies to all pods
  ingress:
  - fromEndpoints:
    - matchLabels:
        "reserved:host": ""   # Allow from node (kubelet health checks)
  egress:
  - toEndpoints:
    - matchLabels:
        "k8s:io.kubernetes.pod.namespace": kube-system   # Allow DNS
    toPorts:
    - ports:
      - port: "53"
        protocol: UDP

DNS-Aware Policy:
────────────────
apiVersion: cilium.io/v2
kind: CiliumNetworkPolicy
metadata:
  name: allow-external-api
  namespace: production
spec:
  endpointSelector:
    matchLabels:
      app: payment-service
  egress:
  - toFQDNs:
    - matchName: "api.stripe.com"
    toPorts:
    - ports:
      - port: "443"
        protocol: TCP

eBPF Enforcement Path:
─────────────────────
Packet arrives at pod's veth interface
  → eBPF program (tc ingress hook)
  → Extract source/dest Cilium identity from packet metadata
  → Lookup in policy map (eBPF hash map)
  → ALLOW: forward packet to pod
  → DENY: drop packet, increment counter
  
  Total overhead: < 0.1 ms per packet
  Zero user-space context switches
```

**Hubble (Cilium Observability):**
```bash
# Observe network flows in real-time
hubble observe --namespace production --verdict DROPPED
hubble observe --from-pod frontend-abc --to-pod api-server-xyz

# Flow logs example:
# Apr  9 15:00:01 production/frontend-abc → production/api-server-xyz
#   TCP 8080 FORWARDED (policy: api-server-ingress)
# Apr  9 15:00:02 production/frontend-abc → production/database-xyz
#   TCP 3306 DROPPED (policy: default-deny-all)
```

**Failure Modes:**
- **Cilium agent crash on a node:** All eBPF programs continue running (they're in the kernel, not user-space). No policy enforcement gap. Cilium agent restarts and resumes management. However, new policy updates won't be applied until the agent is back.
- **eBPF map overflow:** Each node supports ~64K Cilium identities. At 500 nodes with 100 pods each, we use ~50K identities (well within limits). Alert on identity count approaching 60K.
- **Policy misconfiguration blocks critical traffic:** Start with default-deny + explicit allows. Use `hubble observe --verdict DROPPED` to debug. Cilium supports `policy-audit` mode (log but don't enforce).
- **L7 enforcement performance impact:** L7 policies route traffic through a Cilium proxy (Envoy). This adds ~1 ms latency. Use L7 only for specific services that need it; L3/L4 for everything else.

**Interviewer Q&As:**

**Q1: Why identity-based policies instead of IP-based?**
A: In Kubernetes, pod IPs are ephemeral. A NetworkPolicy that allows `from: 10.0.1.5` breaks when the pod restarts with a new IP. Cilium assigns identities based on labels (`app=frontend`), which are stable. When a pod restarts, it gets the same identity (same labels). The eBPF maps don't need to be updated. This is fundamental to zero trust -- you trust the identity, not the address.

**Q2: How does Cilium handle network policies at scale (10,000+ policies)?**
A: Cilium compiles policies into eBPF maps (hash maps). Each map entry is an identity-pair → verdict. The lookup is O(1) per packet. Even with 10,000 policies affecting 50,000 identities, the eBPF map size is manageable (~10 MB per node). Cilium's agent computes the effective policy on each node (only policies relevant to local pods) and programs the eBPF maps.

**Q3: What is the difference between CiliumNetworkPolicy and Kubernetes NetworkPolicy?**
A: Kubernetes NetworkPolicy: L3/L4 only (IP + port), no L7, limited label selectors, no DNS-aware rules, no default deny for egress (only ingress). CiliumNetworkPolicy: all of the above plus L7 (HTTP, gRPC, Kafka, DNS), DNS-aware egress rules (`toFQDNs`), identity-aware, and cluster-wide policies. CiliumNetworkPolicy is a superset.

**Q4: How do you enforce micro-segmentation for bare-metal VMs (not in Kubernetes)?**
A: Cilium can run outside Kubernetes as a standalone agent on bare-metal hosts. It manages eBPF programs on the host's network interfaces. VM-to-VM traffic goes through the same eBPF enforcement as pod-to-pod. VM identities can be assigned via labels in Cilium's external workload mode or via SPIRE SVIDs.

**Q5: How do you debug network connectivity issues caused by policies?**
A: (1) `hubble observe --verdict DROPPED --from-pod xxx --to-pod yyy` shows dropped packets and the policy that matched. (2) `cilium policy get --endpoint <id>` shows the effective policy for a specific endpoint. (3) `cilium connectivity test` runs a suite of connectivity checks. (4) `cilium monitor` shows real-time BPF events. (5) Policy audit mode logs what would be dropped without actually dropping.

**Q6: How do you prevent policy conflicts when multiple teams manage their namespace policies?**
A: (1) Cluster-wide default-deny is managed by the platform team (CiliumClusterwideNetworkPolicy). (2) Namespace-scoped policies are managed by service owners (CiliumNetworkPolicy). (3) The effective policy is the intersection: traffic must be allowed by both cluster-wide and namespace policies. (4) Gatekeeper constraints prevent teams from creating overly permissive policies (e.g., cannot allow from all namespaces).

---

### Deep Dive 2: BeyondCorp Context-Aware Access Proxy

**Why it's hard:**
Replacing VPN with identity-aware proxies requires evaluating multiple trust signals for every human access request: identity (who), device (what), context (where/when), and behavior (how). The access decision must be dynamic -- a request from a compliant device at the office during business hours may be allowed, while the same request from an unpatched laptop at 3 AM from an unusual country should require additional verification.

| Approach | Deployment Complexity | User Experience | Context Awareness |
|----------|----------------------|----------------|-------------------|
| **Traditional VPN** | Low | Poor (full tunnel, latency) | None (all-or-nothing) |
| **SSH bastion host** | Low | Poor (hop through bastion) | Minimal (IP-based) |
| **BeyondCorp proxy (custom)** | High | Good (transparent, per-resource) | Full |
| **Commercial (Cloudflare Access, Google BeyondCorp)** | Medium | Good | Full |
| **Open-source (Teleport, Boundary)** | Medium | Good | Medium-High |

**Selected: Open-source stack: Teleport (SSH/k8s) + custom OAuth2 Proxy (web apps) + ProxySQL (database)**

**Justification:** Teleport provides SSH proxy with session recording, kubectl proxy, and database proxy -- all with OIDC integration and device trust. For web applications, we use a custom OAuth2 proxy with device-trust integration. This avoids vendor lock-in and gives us full control over the access decision logic.

**Implementation Detail:**

```
Access Decision Engine:
──────────────────────
┌────────────────────────────────────────────────────────────┐
│  Context-Aware Access Decision                              │
│                                                             │
│  Inputs:                                                    │
│  ┌──────────────┐ ┌──────────────┐ ┌────────────────────┐ │
│  │ Identity      │ │ Device       │ │ Context            │ │
│  │ - JWT (OIDC)  │ │ - Device UUID│ │ - Source IP        │ │
│  │ - User email  │ │ - OS version │ │ - Geo location     │ │
│  │ - Groups      │ │ - Disk enc.  │ │ - Time of day      │ │
│  │ - MFA status  │ │ - Patched?   │ │ - Prev. sessions   │ │
│  │ - Role        │ │ - Trust score│ │ - Risk indicators  │ │
│  └──────────────┘ └──────────────┘ └────────────────────┘ │
│                                                             │
│  Decision Logic (OPA Rego):                                 │
│  ┌─────────────────────────────────────────────────────┐   │
│  │ 1. Identity verified? (valid JWT, not revoked)       │   │
│  │ 2. MFA completed? (hardware key for production)      │   │
│  │ 3. Device enrolled and compliant?                    │   │
│  │    - OS patched within 30 days                       │   │
│  │    - Disk encryption enabled                         │   │
│  │    - Firewall enabled                                │   │
│  │ 4. Context acceptable?                               │   │
│  │    - Known IP range or familiar geo                  │   │
│  │    - Not flagged by anomaly detection                │   │
│  │ 5. Resource sensitivity vs. trust level?             │   │
│  │    - Production admin: require all above + hardware  │   │
│  │      MFA + trust_score > 90                          │   │
│  │    - Staging read: require identity + MFA            │   │
│  │    - Dev: require identity only                      │   │
│  └─────────────────────────────────────────────────────┘   │
│                                                             │
│  Outputs:                                                   │
│  ┌──────────┐ ┌─────────────┐ ┌────────────────────────┐  │
│  │ ALLOW    │ │ DENY        │ │ STEP-UP AUTH           │  │
│  │ + session│ │ + reason    │ │ (require hardware key  │  │
│  │ token    │ │ + guidance  │ │  or additional context)│  │
│  └──────────┘ └─────────────┘ └────────────────────────┘  │
└────────────────────────────────────────────────────────────┘

Trust Level Calculation:
───────────────────────
trust_level = f(identity_trust, device_trust, context_trust)

identity_trust:
  +30  authenticated (valid JWT)
  +20  MFA hardware key
  +10  MFA TOTP
  +10  member of approved group

device_trust (0-100 from device agent):
  +30  disk encrypted
  +20  OS patched (< 30 days)
  +15  firewall enabled
  +15  endpoint protection active
  +10  enrolled device
  +10  device cert valid

context_trust:
  +30  known IP range (corporate network)
  +20  familiar geo (same country as usual)
  +10  business hours
  -20  new geo (first time from this country)
  -30  flagged IP (threat intelligence)
  -40  anomalous behavior (unusual time/resource)

Access Matrix:
─────────────
Resource Sensitivity │ Required Trust Level
────────────────────┼─────────────────────
Production admin     │ > 90  (all signals green)
Production read      │ > 70  (MFA + compliant device)
Staging any          │ > 50  (MFA)
Dev/test any         │ > 30  (authenticated)

Teleport Configuration:
──────────────────────
# teleport.yaml
teleport:
  auth_service:
    authentication:
      type: oidc
      second_factor: hardware_key  # WebAuthn
    device_trust:
      mode: required              # Device must be enrolled
      auto_enroll: true
  proxy_service:
    web_listen_addr: 0.0.0.0:443
    kube_listen_addr: 0.0.0.0:3026
    mysql_listen_addr: 0.0.0.0:3036
    ssh_public_addr: teleport.corp.com:3023

# Teleport role for SRE
kind: role
version: v7
metadata:
  name: sre-prod
spec:
  allow:
    node_labels:
      env: production
    kubernetes_groups: ["sre-team"]
    kubernetes_labels:
      env: production
    db_labels:
      env: production
    db_names: ["*"]
    db_users: ["readonly"]
    require_session_mfa: hardware_key_or_passwordless
    device_trust_mode: required
  options:
    max_session_ttl: 8h
    record_session:
      default: best_effort
      ssh: strict
```

**Failure Modes:**
- **Access proxy down:** Multiple replicas behind load balancer. If all are down, no human access to infrastructure. Emergency: direct kube-apiserver access via break-glass (separate path, heavily audited).
- **IdP down:** Cannot authenticate. Teleport supports local user fallback for emergency (break-glass accounts, hardware key only).
- **Device agent not reporting:** Device trust score decays over time. If not verified in 24 hours, trust score drops below threshold, access denied. User must connect from a compliant device.
- **False positive (compliant device flagged as non-compliant):** Admin can manually override device trust for a specific session (with audit trail). User can also re-run posture check.

**Interviewer Q&As:**

**Q1: Why replace VPN with zero trust access proxies?**
A: VPN provides network-level access -- once you're "in," you can reach everything on the internal network. Zero trust proxies provide per-resource access -- each access request is individually evaluated. VPN is all-or-nothing; zero trust is granular. VPN also adds latency (full tunnel), doesn't verify device posture, and creates a single point of entry that attackers target.

**Q2: What is the BeyondCorp model?**
A: BeyondCorp (developed by Google) is a security model that shifts access controls from the network perimeter to individual users and devices. Core principles: (1) Access to services is not determined by network location. (2) Access is granted based on the user, the device, and its state. (3) All access is authenticated, authorized, and encrypted. (4) Access is dynamic -- continuously re-evaluated.

**Q3: How do you handle SSH access to bare-metal nodes without VPN?**
A: Teleport SSH proxy. Engineers connect to Teleport (OIDC + MFA + device trust). Teleport issues a short-lived SSH certificate (8h TTL, bound to the user's identity). The SSH connection goes through Teleport's proxy, which records the session. No SSH keys on nodes -- nodes trust the Teleport CA and accept any certificate it signs for authorized users.

**Q4: How do you handle database access without VPN?**
A: Teleport database proxy (or custom ProxySQL with identity-aware auth). Engineer requests access via CLI. Proxy authenticates via OIDC + MFA. Vault issues a short-lived MySQL credential with scoped permissions (read-only for most users). Connection is proxied. All queries are logged. Credential expires after the session.

**Q5: How do you handle emergency access when the proxy is down?**
A: Break-glass path: (1) A dedicated kube-apiserver endpoint is exposed on a separate network (management VLAN). (2) Access requires a break-glass certificate stored in a hardware HSM (not the same as regular access). (3) Two-person rule: requires two authorized operators to activate. (4) Every action is logged and immediately reviewed by security team.

**Q6: How do you prevent lateral movement if an attacker compromises a developer's laptop?**
A: (1) Device trust: if the device is compromised (malware detected), the device agent reports non-compliant posture -> trust score drops -> access denied. (2) MFA: even with a compromised device, the attacker needs the hardware key (separate device). (3) Short-lived credentials: the session expires in 8h. (4) Micro-segmentation: even if the attacker gets a session, they can only access resources the developer is authorized for. (5) Anomaly detection: unusual patterns (resource access, timing) trigger alerts.

---

### Deep Dive 3: Service Mesh mTLS and AuthorizationPolicy

**Why it's hard:**
mTLS between 50,000 pods means 50,000 TLS termination points, each needing certificates, trust bundles, and authorization policies. The service mesh must transparently handle mTLS without application changes, support both strict and permissive modes (for migration), and evaluate fine-grained authorization policies (method, path, headers, source identity) without adding significant latency.

| Approach | Transparency | Performance | Authorization Granularity |
|----------|-------------|-------------|--------------------------|
| **Application-level TLS** | None (app must implement) | Varies | Application decides |
| **Istio (Envoy sidecar)** | Full (transparent proxy) | ~1 ms overhead | Full L7 (method, path, headers, JWT) |
| **Cilium service mesh (eBPF)** | Full (kernel-level) | < 0.5 ms overhead | L3/L4 + basic L7 |
| **Linkerd (rust-based proxy)** | Full | ~0.5 ms overhead | L4 + basic L7 |

**Selected: Istio (Envoy) for L7 authorization + Cilium for L3/L4 (complementary)**

**Justification:** Istio's AuthorizationPolicy provides the richest L7 authorization (method, path, JWT claims, source principal). Cilium handles L3/L4 at kernel level (faster, lower overhead). Together, they enforce at every layer without redundant user-space proxying for L3/L4 traffic.

**Implementation Detail:**

```
mTLS Configuration (Istio):
──────────────────────────
# PeerAuthentication: enforce mTLS for all traffic in the mesh
apiVersion: security.istio.io/v1beta1
kind: PeerAuthentication
metadata:
  name: default
  namespace: istio-system   # Mesh-wide
spec:
  mtls:
    mode: STRICT    # Reject plaintext connections

# Per-namespace exception (for legacy services during migration)
apiVersion: security.istio.io/v1beta1
kind: PeerAuthentication
metadata:
  name: legacy-permissive
  namespace: legacy-services
spec:
  mtls:
    mode: PERMISSIVE   # Accept both mTLS and plaintext

AuthorizationPolicy Examples:
────────────────────────────
# Allow frontend to call api-server on specific paths
apiVersion: security.istio.io/v1beta1
kind: AuthorizationPolicy
metadata:
  name: api-server-authz
  namespace: production
spec:
  selector:
    matchLabels:
      app: api-server
  action: ALLOW
  rules:
  - from:
    - source:
        principals: ["cluster.local/ns/production/sa/frontend"]
    to:
    - operation:
        methods: ["GET"]
        paths: ["/api/v1/products/*", "/api/v1/categories/*"]
  - from:
    - source:
        principals: ["cluster.local/ns/production/sa/frontend"]
    to:
    - operation:
        methods: ["POST"]
        paths: ["/api/v1/orders"]

# Deny all traffic to payment service except from order service
apiVersion: security.istio.io/v1beta1
kind: AuthorizationPolicy
metadata:
  name: payment-service-deny-all
  namespace: production
spec:
  selector:
    matchLabels:
      app: payment-service
  action: DENY
  rules:
  - from:
    - source:
        notPrincipals:
        - "cluster.local/ns/production/sa/order-service"
        - "cluster.local/ns/production/sa/refund-service"

# JWT validation for external API access
apiVersion: security.istio.io/v1beta1
kind: RequestAuthentication
metadata:
  name: external-jwt
  namespace: production
spec:
  selector:
    matchLabels:
      app: api-gateway
  jwtRules:
  - issuer: "https://auth.corp.com"
    jwksUri: "https://auth.corp.com/.well-known/jwks.json"
    audiences: ["https://api.corp.com"]
    forwardOriginalToken: true

mTLS Handshake Flow (Envoy sidecar-to-sidecar):
──────────────────────────────────────────────
Client Pod                                Server Pod
┌─────────────┐                          ┌─────────────┐
│  App         │                          │  App         │
│  Container   │                          │  Container   │
│  (plaintext) │                          │  (plaintext) │
│      │       │                          │      ▲       │
│      ▼       │                          │      │       │
│  ┌─────────┐ │   mTLS (TLS 1.3)       │  ┌─────────┐ │
│  │ Envoy   │─┼─────────────────────────┼─▶│ Envoy   │ │
│  │ Sidecar │ │  ClientHello            │  │ Sidecar │ │
│  │         │◀┼─ ServerHello + Cert     ─┼──│         │ │
│  │ Presents│ │  ClientCert             │  │ Verifies│ │
│  │ Client  │─┼─ Finished              ─┼──│ Client  │ │
│  │ SVID    │ │  [encrypted traffic]    │  │ SVID    │ │
│  └─────────┘ │                          │  └─────────┘ │
│  SPIFFE ID:  │                          │  SPIFFE ID:  │
│  spiffe://   │                          │  spiffe://   │
│  .../sa/     │                          │  .../sa/     │
│  frontend    │                          │  api-server  │
└─────────────┘                          └─────────────┘

Envoy checks AuthorizationPolicy:
  source.principal = "cluster.local/ns/production/sa/frontend"
  destination.path = "/api/v1/products/123"
  destination.method = "GET"
  → Matches rule 1 of api-server-authz → ALLOW
```

**Failure Modes:**
- **Envoy sidecar crash:** Pod loses mTLS + authorization. Kubelet restarts sidecar. During restart (~5 seconds), traffic to/from the pod fails (strict mTLS requires both sides). Mitigation: Envoy's graceful shutdown drains connections.
- **SPIFFE SVID expired, sidecar not rotated:** mTLS handshake fails (expired cert). SPIRE Agent pushes new SVID via SDS. If SPIRE is down, Envoy uses cached SVID. If cached SVID expires, connections fail. Alert on SPIRE health.
- **AuthorizationPolicy misconfiguration denies legitimate traffic:** `istioctl analyze` checks for policy conflicts before apply. Start with AUDIT mode (log but don't enforce), then switch to DENY after verification.
- **Istio control plane (istiod) down:** Existing policies continue enforced by Envoy sidecars (data plane). New policy changes won't propagate. New pods won't get sidecar injected (if injection webhook is down). Run 3+ istiod replicas.

**Interviewer Q&As:**

**Q1: What is the performance overhead of Envoy sidecar mTLS?**
A: Initial TLS 1.3 handshake: ~5 ms (amortized over many requests per connection). Per-request overhead (encryption/decryption): ~0.3 ms for a typical 1 KB request. Connection reuse (HTTP/2) minimizes handshake frequency. For latency-critical paths, consider Cilium's eBPF-based mTLS (no user-space proxy, < 0.1 ms overhead) for L4, with Envoy only for L7.

**Q2: How do you handle services that cannot have an Envoy sidecar?**
A: (1) For legacy services, use PERMISSIVE mode: accept both mTLS and plaintext. (2) For performance-sensitive services (e.g., Redis, Kafka), use Cilium's WireGuard encryption (L3 encryption, no sidecar needed). (3) For external services, use an egress gateway that terminates mTLS from the mesh and connects to the external service.

**Q3: How does Istio AuthorizationPolicy work internally?**
A: istiod compiles AuthorizationPolicy into Envoy RBAC filter configuration, which is pushed to Envoy sidecars via xDS (Envoy's discovery service). Each Envoy sidecar evaluates the RBAC filter on every request. The evaluation is fast (microseconds) because it's compiled into a decision tree. The source identity comes from the mTLS handshake (peer certificate's SPIFFE ID).

**Q4: What is the difference between PeerAuthentication and RequestAuthentication in Istio?**
A: PeerAuthentication configures mTLS at the transport layer (how connections are established). It defines whether mTLS is STRICT (required), PERMISSIVE (optional), or DISABLE. RequestAuthentication configures JWT validation at the application layer (how requests are authenticated). A request can be both mTLS-authenticated (peer identity) and JWT-authenticated (user identity).

**Q5: How do you handle mTLS for headless services (StatefulSets like MySQL, Elasticsearch)?**
A: Headless services have stable DNS names (`mysql-0.mysql.namespace.svc`). Envoy sidecar handles mTLS for each pod. The certificate's SAN includes the pod's DNS name. For direct pod-to-pod communication (e.g., MySQL replication), mTLS is transparent -- Envoy handles it without MySQL configuration changes (as long as MySQL connects to the sidecar's local port).

**Q6: How do you migrate from no-mTLS to full mTLS?**
A: Phased: (1) Deploy Istio with PERMISSIVE mode globally. All traffic works (both plaintext and mTLS). (2) Dashboard shows mTLS adoption rate per namespace. (3) Gradually switch namespaces to STRICT mode, starting with low-risk ones. (4) Monitor for connection failures (`istioctl proxy-config` for debugging). (5) After all namespaces are STRICT, disable PERMISSIVE globally.

---

## 7. Scaling Strategy

**Cilium scaling:**
- eBPF programs run per-node. No central bottleneck.
- Policy compilation is done by the Cilium agent per-node (only local pods' policies).
- For 500 nodes, the Cilium operator (central) handles identity allocation and policy distribution. Scale: 3 replicas.

**Istio scaling:**
- istiod (control plane): 3+ replicas. Handles xDS pushes to all Envoy sidecars. For 50,000 pods, istiod needs ~4 GB memory and 4 CPUs per replica.
- Envoy sidecars (data plane): per-pod, no central bottleneck. Each sidecar handles its own mTLS and authorization.

**Access proxy scaling:**
- Teleport: 3+ proxy replicas behind load balancer. Each proxy handles ~500 concurrent sessions.
- Session recordings stored in S3 (scalable storage).

**Audit pipeline scaling:**
- 500K events/sec: Kafka with 128 partitions. Elasticsearch with 10+ data nodes.

**Interviewer Q&As:**

**Q1: What is the bottleneck in a zero trust architecture at this scale?**
A: The Istio control plane (istiod). With 50,000 pods, every policy change triggers an xDS push to all Envoy sidecars. For large policy changes, this can spike istiod CPU and network. Mitigation: (1) Incremental xDS (Envoy gets only the delta). (2) Rate-limit policy pushes. (3) Scale istiod horizontally.

**Q2: How do you handle the thundering herd when 10,000 pods restart simultaneously?**
A: (1) Each pod needs a SVID and Envoy configuration. SPIRE Agent caches SVIDs per node -- pods with the same identity don't generate new SVIDs. (2) istiod uses lazy xDS: sidecars pull config on startup rather than getting it pushed. (3) Pod disruption budgets limit concurrent restarts.

**Q3: How do you handle network policy at scale without performance degradation?**
A: Cilium's eBPF maps are O(1) lookup. Adding more policies doesn't increase per-packet latency -- it only increases eBPF map size. At 10,000 policies, the eBPF maps are ~10 MB per node. The Cilium agent's policy compilation is the bottleneck: for very large policy sets, it may take 1-2 seconds to recompile. This is a control-plane concern, not a data-plane concern.

**Q4: How do you handle multi-cluster zero trust?**
A: (1) SPIFFE federation: each cluster has its own trust domain, federated trust bundles. (2) Istio multi-cluster: east-west gateway for cross-cluster mTLS. (3) Cilium ClusterMesh: connect Cilium across clusters for cross-cluster NetworkPolicy. (4) Access proxy: single Teleport cluster spans all regions.

**Q5: How do you measure zero trust coverage?**
A: Dashboard metrics: (1) mTLS coverage: % of connections that are mTLS (target: 100% in production). (2) NetworkPolicy coverage: % of namespaces with default-deny policy. (3) AuthorizationPolicy coverage: % of services with explicit ingress rules. (4) Device trust coverage: % of active users with compliant devices. (5) Audit coverage: % of access decisions logged.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---------|--------|-----------|------------|-----|
| **Cilium agent crash on node** | eBPF programs continue (in-kernel). No new policy updates for that node. | Cilium agent health check | DaemonSet restarts agent. Existing policies unaffected. | < 30s |
| **istiod down (all replicas)** | No new policy pushes. Existing sidecars continue with last config. New pods get no sidecar. | istiod health check | 3+ replicas with anti-affinity. New pods wait for istiod recovery. | < 1 min |
| **Envoy sidecar crash** | Pod loses mTLS + authz for ~5 seconds | Sidecar health check | Kubelet restarts sidecar. Pod readiness fails until sidecar ready. | < 10s |
| **SPIRE Server down** | No new SVIDs. Cached SVIDs continue. | SPIRE health check | 3 replicas. Agent cache. | < 1 min |
| **Access proxy down** | No human access to infra | LB health check | Multiple replicas. Break-glass path. | < 30s |
| **IdP down (Okta/Azure AD)** | No human authentication | IdP status page | Local break-glass accounts (hardware key only). | Depends on IdP |
| **Hubble relay down** | No network flow observability | Hubble health check | Flows buffered in Cilium agents. No data-plane impact. | < 5 min |
| **Kafka audit pipeline down** | Audit events queue locally | Kafka health | Local buffer on each node. Kafka 7-day retention on recovery. | < 5 min |
| **Full network partition** | Isolated nodes operate with cached policies and SVIDs | Heartbeat | Cached state continues. Deny new access after staleness threshold. | Depends on partition |
| **eBPF program bug (kernel panic)** | Node crash | Kernel crash dump | Cilium releases are tested on the exact kernel version. Canary node rollout. | Node reboot (~5 min) |

---

## 9. Security

### Authentication Chain

```
Workload Authentication:
  Pod → SPIRE Agent (node-local UDS) → SVID (X.509 or JWT)
  SVID → mTLS handshake (peer authentication)
  OR: SVID JWT → HTTP header → RequestAuthentication (Istio)

Human Authentication:
  Engineer → OIDC IdP (Okta/Azure AD) → JWT (with MFA claim)
  + Device Agent → Device Trust Service → Device certificate
  → Access Proxy evaluates: JWT + device cert + context
  → Short-lived session certificate (Teleport) or session token

Infrastructure Authentication:
  Node → SPIRE Agent (node attestation via TPM or k8s PSAT)
  → Node SVID
  etcd/kube-apiserver → Static TLS certificates (managed by Ansible/kubeadm)
```

### Authorization Model

```
Multi-Layer Authorization:
─────────────────────────
Layer 1: Network (Cilium eBPF)
  - L3/L4: source identity → dest identity + port
  - L7: HTTP method + path (optional)
  - Default: deny all, explicit allow

Layer 2: Transport (Istio PeerAuthentication)
  - mTLS STRICT: only authenticated peers can connect
  - Identity: SPIFFE principal from certificate

Layer 3: Application (Istio AuthorizationPolicy)
  - L7: source principal + method + path + headers
  - JWT claims (for human-initiated requests)

Layer 4: Business Logic (OPA / Application)
  - RBAC/ReBAC: does this identity have permission for this action?
  - Tenant isolation: does this identity belong to this tenant?

Every layer must independently authorize.
Traffic must pass ALL layers to reach the application.
```

### Audit Trail

- **Network flows:** Hubble captures every connection (source, dest, port, verdict, policy matched). Exported to Kafka -> Elasticsearch.
- **mTLS events:** Every mTLS handshake logged (peer identities, cipher suite, cert serial).
- **AuthorizationPolicy decisions:** Every allow/deny logged with rule that matched.
- **Access proxy sessions:** Full session metadata (user, device, target, duration, commands executed).
- **SSH sessions:** Full session recording (keystroke-level) stored in S3.
- **Database queries:** All queries proxied through identity-aware proxy, logged.

### Threat Model

| Threat | Likelihood | Impact | Mitigation |
|--------|------------|--------|------------|
| **Lateral movement after pod compromise** | High | High | Micro-segmentation (Cilium): compromised pod can only reach explicitly allowed destinations. mTLS: cannot impersonate another identity. |
| **Stolen SPIFFE SVID** | Medium | High | Short TTL (24h). SVID bound to node (cannot be used from another node without the private key, which is in memory only). |
| **Compromised developer laptop** | High | High | Device trust: malware detection → posture fails → access denied. MFA: hardware key required (separate device). Short-lived sessions (8h). |
| **Insider threat (authorized user)** | Medium | Critical | Least privilege (per-resource access). Full audit trail. Anomaly detection. Session recording. |
| **mTLS bypass (connecting directly to pod, skipping Envoy)** | Low | High | Cilium NetworkPolicy requires mTLS at L3 level. Pod security context: only Envoy listens on external interfaces. |
| **Policy misconfiguration (overly permissive)** | Medium | High | Gatekeeper constraints on policy objects. CI/CD policy review. Default deny baseline. |
| **DDoS on access proxy** | Medium | Medium | Rate limiting. Geo-blocking. Cloud DDoS protection in front of proxy. |
| **Control plane compromise (istiod)** | Low | Critical | istiod runs with minimal RBAC. mTLS between istiod and sidecars. Audit all istiod actions. |

---

## 10. Incremental Rollout

**Phase 1 (Week 1-3): Network foundation**
- Deploy Cilium as CNI.
- Default deny (CiliumClusterwideNetworkPolicy) in audit mode.
- Hubble for flow visibility.
- Identify all legitimate traffic patterns from Hubble data.

**Phase 2 (Week 4-6): mTLS deployment**
- Deploy Istio in PERMISSIVE mode.
- Deploy SPIRE Server and Agents.
- Monitor mTLS adoption rate per namespace.
- Target: 80% mTLS coverage.

**Phase 3 (Week 7-9): Policy enforcement**
- Switch Cilium to enforcement mode (based on Phase 1 audit data).
- Deploy AuthorizationPolicies for top 20 services.
- Switch mTLS to STRICT for pilot namespaces.

**Phase 4 (Week 10-12): Human access proxying**
- Deploy Teleport for SSH and kubectl access.
- Migrate engineers from VPN to proxy (one team at a time).
- Deploy device trust agents on all laptops.

**Phase 5 (Week 13-16): Full zero trust**
- mTLS STRICT for all namespaces.
- NetworkPolicy coverage: 100%.
- VPN decommissioned.
- Database proxy for all MySQL access.
- Full audit pipeline operational.

**Interviewer Q&As:**

**Q1: How do you avoid breaking existing services when deploying default deny?**
A: (1) Audit mode: Cilium policy-audit logs what would be denied without actually denying. (2) Use Hubble flow data to auto-generate allow policies for observed traffic. (3) Roll out enforcement one namespace at a time. (4) Keep an "escape hatch": a namespace label that disables enforcement (`policy-mode: audit`).

**Q2: How do you handle services that cannot have Envoy sidecars during migration?**
A: PERMISSIVE mode: these services receive both plaintext and mTLS traffic. They are not sidecar-injected. They appear as "plaintext" in Hubble. We track them on a migration dashboard and work with service owners to add sidecar injection. For genuinely incompatible services (e.g., UDP-heavy), use Cilium WireGuard encryption (L3) instead.

**Q3: How do you convince engineers to give up VPN access?**
A: (1) Show that the proxy is faster (per-resource access, no full tunnel overhead). (2) The proxy works from anywhere (no corporate network needed). (3) Better UX: `tsh ssh node-001` instead of `vpn connect && ssh bastion && ssh node-001`. (4) Mandatory: after Phase 5, VPN is decommissioned. (5) Break-glass for emergency.

**Q4: How do you handle the cold-start problem for Hubble flow data?**
A: During initial Cilium deployment (Phase 1), run in audit mode for 2 weeks. Hubble collects all traffic patterns. We use this data to auto-generate CiliumNetworkPolicies using a tool like Cilium's `policy-verdict` and custom scripts that convert flow data to policy YAML. Engineers review and approve.

**Q5: What is the VPN decommissioning process?**
A: (1) All access through Teleport/proxy for 30 days (parallel with VPN). (2) Monitor: any access still going through VPN. (3) Notify remaining VPN users individually. (4) Disable VPN for non-emergency use. (5) Decommission VPN infrastructure. (6) Keep one VPN endpoint for disaster recovery (sealed, break-glass only).

---

## 11. Trade-offs & Decision Log

| Decision | Trade-off | Rationale |
|----------|-----------|-----------|
| **Cilium (eBPF) vs. Calico (iptables)** | Learning curve vs. performance | eBPF provides kernel-level policy enforcement without iptables overhead. Identity-based policies are foundational for zero trust. |
| **Istio + Cilium vs. Cilium-only service mesh** | Two systems vs. L7 richness | Istio's AuthorizationPolicy is more expressive than Cilium's L7 (JWT validation, rich header matching). We use Cilium for L3/L4, Istio for L7. |
| **Teleport vs. Boundary vs. custom proxy** | Feature set vs. vendor lock-in | Teleport provides SSH + k8s + database proxy in one tool with OIDC + device trust. Saves building 3 separate proxies. |
| **STRICT mTLS everywhere vs. PERMISSIVE for some** | Security vs. compatibility | STRICT is the goal. PERMISSIVE during migration. Permanently PERMISSIVE only for explicitly documented legacy services (with a migration timeline). |
| **Device trust as required vs. optional** | User friction vs. security | Required for production access. Optional for dev/staging (to not block developer productivity). |
| **eBPF L7 filtering vs. Envoy-only L7** | Kernel-level performance vs. proxy flexibility | eBPF L7 is faster but less feature-rich. Use eBPF for simple L7 (HTTP method/path), Envoy for complex L7 (JWT, headers, gRPC method). |
| **Audit everything vs. sample** | Storage cost vs. compliance | Audit everything. The storage cost (10 TB/day) is justified by compliance requirements and forensic needs. Use tiered storage (hot/warm/cold). |
| **Default deny vs. default allow** | Risk of breaking things vs. security | Default deny is fundamental to zero trust. Phased rollout (audit mode first) mitigates risk. |

---

## 12. Agentic AI Integration

### AI-Powered Zero Trust Intelligence

**1. Automated Network Policy Generation**
- Agent observes network flows (Hubble data) for 7 days.
- Generates minimum-privilege CiliumNetworkPolicies that allow observed traffic and deny everything else.
- CLI: `platform-zt ai generate-policies --namespace production --observation-days 7`
- Human review required before applying.

**2. Anomaly Detection on Network Flows**
- Train model on normal traffic patterns: source-dest pairs, request rates, time-of-day profiles.
- Flag anomalies: "Pod `worker-abc` in namespace `batch` is making connections to `payment-service` in namespace `production` -- this has never happened before."
- Automated response: alert security team, optionally add a temporary deny rule pending investigation.

**3. Risk-Based Access Decisions**
- AI model computes real-time risk scores for access proxy requests.
- Inputs: user behavior history, device posture trend, geo anomaly, time anomaly, current threat intelligence.
- Output: risk score (0-100) that feeds into the access decision engine.
- Example: "Alice is requesting production kubectl access from a new city at 2 AM. Risk score: 85. Require step-up authentication (hardware key + video call with on-call)."

**4. Zero Trust Coverage Gap Analysis**
- Agent scans the entire platform and identifies gaps:
  - Namespaces without default-deny policy.
  - Services without AuthorizationPolicy.
  - Connections that are plaintext (not mTLS).
  - Users accessing production without MFA.
  - Devices not enrolled in device trust.
- Generates a prioritized remediation plan.

**5. Policy Conflict Detection**
- Before applying a new policy, agent analyzes the interaction with all existing policies.
- Detects: contradictions (policy A allows, policy B denies same traffic), shadows (policy A makes policy B irrelevant), and gaps (no policy covers this traffic pattern).
- Example: "New CiliumNetworkPolicy 'allow-frontend-to-api' conflicts with existing 'deny-all-to-api' policy. Resolution: deny-all takes precedence (Cilium evaluates deny first)."

**6. Incident Response Automation**
- On detection of a compromised workload:
  1. Agent isolates the workload by applying a deny-all CiliumNetworkPolicy.
  2. Agent captures network flow history for the workload (last 24h from Hubble).
  3. Agent identifies all workloads the compromised workload communicated with (blast radius).
  4. Agent generates a forensic report.
  5. Agent notifies security team with actionable summary.

---

## 13. Complete Interviewer Q&A Bank

**Q1: Explain zero trust architecture in simple terms.**
A: Traditional security is like a castle: strong walls (firewall), but once you're inside, you can go anywhere. Zero trust says: there is no "inside." Every request, from every user and service, must prove its identity, be authorized, and be encrypted -- regardless of where it comes from. A server in the same rack is treated the same as a server across the internet.

**Q2: What are the core principles of zero trust?**
A: (1) Never trust, always verify: no implicit trust based on network location. (2) Least privilege: grant minimum necessary access. (3) Assume breach: design as if the attacker is already inside. (4) Verify explicitly: authenticate and authorize every request with all available signals (identity, device, context). (5) Inspect and log: audit every access decision.

**Q3: How does micro-segmentation differ from traditional firewalls?**
A: Traditional firewalls create a few large zones (DMZ, internal, external) with rules between zones. Micro-segmentation creates a policy around each individual workload (pod, VM). Every workload has its own allow list. This limits blast radius: compromising one workload doesn't grant access to adjacent workloads, even on the same subnet.

**Q4: How do you implement zero trust for Kubernetes specifically?**
A: (1) Pod identity: SPIFFE/SPIRE SVIDs for every pod. (2) mTLS: Istio/service mesh for encrypted, authenticated communication. (3) NetworkPolicy: Cilium CiliumNetworkPolicy for L3/L4/L7 micro-segmentation. (4) AuthorizationPolicy: Istio for L7 authorization. (5) RBAC: Kubernetes RBAC for API access. (6) Admission control: OPA/Gatekeeper for policy enforcement. (7) No default service account permissions.

**Q5: What is the difference between network segmentation and identity-based segmentation?**
A: Network segmentation uses IP addresses and subnets. Identity-based segmentation uses workload identities (labels, SPIFFE IDs). In Kubernetes, IPs change constantly. A network-segmented policy `allow from 10.0.1.0/24` breaks when pods move. An identity-based policy `allow from app=frontend` works regardless of IP. Identity-based is essential for dynamic environments.

**Q6: How do you handle zero trust for east-west traffic (internal service-to-service)?**
A: (1) mTLS: every connection encrypted and authenticated. (2) AuthorizationPolicy: every service has explicit allow rules for who can call it and on which paths. (3) NetworkPolicy: L3/L4 enforcement as a second layer. (4) Audit: every connection logged with identities. This is the hardest part of zero trust -- most attacks exploit east-west traffic that was implicitly trusted.

**Q7: How do you handle zero trust for north-south traffic (external access)?**
A: (1) API Gateway (Envoy): TLS termination, JWT validation, rate limiting. (2) Access proxy (Teleport): human access with OIDC + MFA + device trust. (3) Ingress controller: TLS with cert-manager certificates. (4) No direct access to internal services from outside -- all traffic goes through an identity-aware gateway.

**Q8: What is continuous verification, and how do you implement it?**
A: Continuous verification means re-evaluating access decisions throughout a session, not just at login time. Implementation: (1) Short-lived tokens/certificates (1-8 hour TTL). (2) Session re-evaluation: if device posture changes (e.g., firewall disabled), the session is terminated. (3) Step-up authentication: sensitive operations (production writes) require re-MFA even within an active session. (4) Anomaly detection: unusual behavior triggers re-authentication.

**Q9: How do you handle the performance impact of zero trust (encryption, policy evaluation)?**
A: (1) eBPF policy evaluation: < 0.1 ms per packet (kernel-level, no context switches). (2) mTLS: TLS 1.3 with connection reuse (HTTP/2) minimizes handshake overhead. Amortized per-request: < 0.5 ms. (3) AuthorizationPolicy: Envoy evaluates in microseconds (compiled decision tree). (4) Total overhead: < 2 ms per request for most services. This is acceptable for the security benefit.

**Q10: How do you handle zero trust for legacy services that cannot support mTLS?**
A: (1) Sidecar proxy: Envoy sidecar handles mTLS transparently. The legacy app speaks plaintext to its local sidecar. (2) If sidecar cannot be injected: Cilium WireGuard encryption (L3 encryption, no app changes). (3) If neither: create an isolated network segment for the legacy service with strict ingress/egress rules. Document the risk and plan migration.

**Q11: What is the role of eBPF in zero trust networking?**
A: eBPF (extended Berkeley Packet Filter) allows running custom programs in the Linux kernel. For zero trust: (1) Packet filtering: enforce NetworkPolicy at kernel level (faster than iptables). (2) Identity assignment: Cilium uses eBPF to tag packets with workload identity. (3) Observability: Hubble uses eBPF to capture flows without instrumentation. (4) Encryption: WireGuard integration via eBPF. eBPF is the foundation for high-performance, identity-aware networking.

**Q12: How do you handle DNS in a zero trust environment?**
A: DNS is a trust problem: pods query DNS to resolve service names, and DNS responses can be spoofed. Mitigation: (1) Cilium DNS-aware policies: only allow connections to resolved DNS names (e.g., `allow egress to api.stripe.com` -- Cilium monitors DNS responses and only allows connections to the resolved IPs). (2) CoreDNS with DNSSEC or DNS over TLS. (3) Cilium agent-level DNS proxy captures all DNS queries for auditing and policy enforcement.

**Q13: How do you implement zero trust for database access?**
A: (1) No direct database access: all connections go through an identity-aware proxy (Teleport DB proxy or ProxySQL with Vault integration). (2) Dynamic credentials from Vault (per-user, per-session, short TTL). (3) mTLS between proxy and database. (4) All queries logged via proxy. (5) NetworkPolicy: only the proxy can reach the database; no pod-to-database direct connections.

**Q14: What is the relationship between zero trust and defense in depth?**
A: Defense in depth is the strategy of having multiple independent security layers. Zero trust is a specific implementation of defense in depth for access control: identity verification at every layer (network, transport, application, data), no single layer whose failure grants full access. A compromised network layer (attacker on the same VLAN) doesn't help if transport (mTLS) and application (authorization) layers are enforced independently.

**Q15: How do you measure the effectiveness of zero trust?**
A: (1) Blast radius of simulated breach: "If pod X is compromised, what can the attacker reach?" (should be minimal with micro-segmentation). (2) mTLS coverage: % of traffic encrypted. (3) Policy coverage: % of workloads with explicit allow-only policies. (4) Time to detect unauthorized access: audit log analysis. (5) Red team exercises: can a red team move laterally after gaining initial access?

**Q16: How do you handle zero trust across hybrid environments (bare metal + Kubernetes + VMs)?**
A: (1) SPIFFE/SPIRE for universal workload identity (works across k8s, VMs, bare metal). (2) Cilium for eBPF-based enforcement on all Linux hosts (not just k8s). (3) WireGuard mesh for VM-to-pod and bare-metal-to-pod encryption. (4) Single access proxy (Teleport) for all human access regardless of target type. (5) Unified audit pipeline collecting from all enforcement points.

**Q17: What is the cost of zero trust? Is it worth it?**
A: Costs: (1) Infrastructure: sidecars use ~30 MB memory per pod (50K pods = 1.5 TB cluster-wide). (2) Latency: ~1-2 ms per request. (3) Operational complexity: managing policies, certificates, identity providers. (4) Engineering time: 6-12 months for full deployment. Benefits: (1) Dramatically reduced blast radius. (2) Compliance with modern frameworks (NIST 800-207, SOC2). (3) Enables secure remote work without VPN. (4) Visibility into all traffic patterns. For an infrastructure platform, the security benefit far outweighs the cost.

---

## 14. References

- [NIST SP 800-207: Zero Trust Architecture](https://csrc.nist.gov/publications/detail/sp/800-207/final)
- [Google BeyondCorp](https://cloud.google.com/beyondcorp)
- [Cilium Documentation](https://docs.cilium.io/)
- [Cilium eBPF Networking](https://cilium.io/use-cases/transparent-encryption/)
- [Istio Security](https://istio.io/latest/docs/concepts/security/)
- [Istio Authorization Policy](https://istio.io/latest/docs/reference/config/security/authorization-policy/)
- [SPIFFE/SPIRE](https://spiffe.io/)
- [Teleport](https://goteleport.com/docs/)
- [HashiCorp Boundary](https://www.boundaryproject.io/)
- [Hubble (Cilium Observability)](https://docs.cilium.io/en/stable/gettingstarted/hubble/)
- [Kubernetes Network Policy](https://kubernetes.io/docs/concepts/services-networking/network-policies/)
- [eBPF.io](https://ebpf.io/)
- [Envoy External Authorization](https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/security/ext_authz_filter)
