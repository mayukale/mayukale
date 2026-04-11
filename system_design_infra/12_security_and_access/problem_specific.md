# Problem-Specific Notes — Security & Access (12_security_and_access)

## api_key_management

### Unique Purpose
Generate, validate, rotate, and revoke API keys for cloud platform services. 50K active keys, 500 services, 100K validations/sec. Never store plaintext keys; detect leaked keys automatically.

### Unique Architecture
- **Key generation**: CSPRNG prefix + 40-char base62 body = 238 bits entropy; `plat_{env}_` prefix enables regex-based leak detection
- **Storage**: only SHA-256 hash stored; plaintext never persists after initial issuance
- **Validation path**: Envoy ext_authz → hash lookup in Redis → permissions + rate limits → forward or 401/429
- **Leak detection**: GitHub secret scanning webhook + internal scanner; regex matches `plat_(live|test)_[A-Za-z0-9]{40}`; auto-revocation on detection

### Key Schema
```sql
CREATE TABLE api_keys (
  key_id       BIGINT PRIMARY KEY AUTO_INCREMENT,
  key_hash     VARBINARY(32) UNIQUE NOT NULL,  -- SHA-256(full_key)
  key_prefix   VARCHAR(12) NOT NULL,           -- first 12 chars: "plat_live_aB"
  key_hint     VARCHAR(4)  NOT NULL,           -- last 4 chars for display
  owner_team   VARCHAR(128) NOT NULL,
  environment  ENUM('live','test') NOT NULL,
  status       ENUM('active','rotated','revoked') NOT NULL DEFAULT 'active',
  created_at   DATETIME(6) NOT NULL,
  updated_at   DATETIME(6) NOT NULL
);
CREATE TABLE key_permissions (
  permission_id  BIGINT PRIMARY KEY,
  key_id         BIGINT NOT NULL REFERENCES api_keys(key_id),
  resource_type  VARCHAR(64) NOT NULL,
  namespace_pattern VARCHAR(256) NOT NULL,  -- supports wildcards
  actions        JSON NOT NULL              -- ["read","write","delete"]
);
CREATE TABLE key_rate_limits (
  limit_id     BIGINT PRIMARY KEY,
  key_id       BIGINT NOT NULL,
  window_type  ENUM('second','minute','day') NOT NULL,
  max_requests INT NOT NULL
);
CREATE TABLE key_rotation_history (
  rotation_id  BIGINT PRIMARY KEY,
  key_id       BIGINT NOT NULL,        -- old key
  new_key_id   BIGINT NOT NULL,        -- new key
  grace_period INT NOT NULL,           -- seconds
  grace_until  DATETIME(6) NOT NULL,
  rotated_at   DATETIME(6) NOT NULL
);
CREATE TABLE leak_detection_events (
  event_id             BIGINT PRIMARY KEY,
  key_prefix           VARCHAR(12) NOT NULL,
  source               ENUM('github','internal') NOT NULL,
  detected_at          DATETIME(6) NOT NULL,
  revocation_triggered_at DATETIME(6)
);
```

### Unique Algorithm: Key Generation
```python
ALPHABET = string.ascii_letters + string.digits  # base62

def generate_api_key(environment: str) -> tuple[str, bytes]:
    prefix = f"plat_{environment}_"          # "plat_live_" or "plat_test_"
    body = ''.join(secrets.choice(ALPHABET) for _ in range(40))
    full_key = prefix + body                  # ~50 chars total, 238 bits entropy
    key_hash = hashlib.sha256(full_key.encode()).digest()  # 32 bytes
    key_prefix = full_key[:12]               # for identification without exposure
    key_hint = full_key[-4:]                 # last 4 for display
    return full_key, key_hash               # full_key shown once; never stored
```

### Unique Algorithm: Sliding Window Rate Limiting
```lua
-- Redis Lua (atomic): sliding window per key per window_type
local key = KEYS[1]          -- "rl:{key_hash}:{window_type}"
local now = tonumber(ARGV[1])
local window = tonumber(ARGV[2])  -- seconds
local limit = tonumber(ARGV[3])
redis.call('ZREMRANGEBYSCORE', key, 0, now - window * 1000)
local count = redis.call('ZCARD', key)
if count >= limit then return 0 end
redis.call('ZADD', key, now, now .. math.random())
redis.call('EXPIRE', key, window + 1)
return 1
```

### Unique NFRs
- Validation: 100K/sec; < 1 ms cached (Redis), < 5 ms uncached (MySQL)
- Revocation propagation: < 5 s to all gateway nodes via Redis pub/sub + Kafka
- Key entropy: 238 bits (CSPRNG, base62 × 40 chars)
- Rotation grace period: configurable; old key works until `grace_until`

---

## certificate_lifecycle_management

### Unique Purpose
PKI hierarchy management for bare-metal Kubernetes: 500 nodes, 50K pods, all service-to-service mTLS. Offline root CA → online intermediate CAs → 24h leaf certs issued by SPIRE/Vault PKI at 5,000 SVIDs/sec.

### Unique Architecture
- **PKI hierarchy**: Root CA (20-year, air-gapped, ECDSA P-384) → 3 Intermediate CAs (5-year, Vault-hosted, separate per domain: infra/workload/ingress) → Leaf certs (24h TTL)
- **Workload certs**: SPIRE Server issues X.509 SVIDs with `spiffe://cluster.local/ns/{ns}/sa/{sa}` URI SAN; SPIRE Agent DaemonSet delivers via Unix domain socket
- **Public certs**: cert-manager + ACME (Let's Encrypt); Certificate CRD → Secret (auto-renewed at 2/3 TTL)
- **Revocation**: CRL published to CDN; OCSP responder for real-time checks

### Key Schema
```sql
CREATE TABLE certificates (
  serial_number    VARCHAR(40) UNIQUE NOT NULL,
  subject_cn       VARCHAR(255) NOT NULL,
  san_dns          JSON,                  -- ["api.example.com", "*.example.com"]
  san_ip           JSON,                  -- ["10.0.0.1"]
  san_spiffe_id    VARCHAR(512),          -- spiffe://cluster.local/...
  ttl_seconds      INT NOT NULL,
  key_type         ENUM('RSA2048','RSA4096','ECDSA_P256','ECDSA_P384') NOT NULL,
  issuer_ca_id     BIGINT NOT NULL,
  issued_at        DATETIME(6) NOT NULL,
  expires_at       DATETIME(6) NOT NULL,
  status           ENUM('active','revoked','expired') NOT NULL,
  revoked_at       DATETIME(6),
  revocation_reason TINYINT,
  fingerprint_sha256 CHAR(64) NOT NULL
);
CREATE TABLE certificate_authorities (
  ca_id          BIGINT PRIMARY KEY,
  name           VARCHAR(128) UNIQUE NOT NULL,
  cert_pem       TEXT NOT NULL,
  key_type       ENUM('RSA4096','ECDSA_P384') NOT NULL,
  validity_years INT NOT NULL,
  parent_ca_id   BIGINT,          -- NULL for root
  is_online      BOOLEAN NOT NULL,
  status         ENUM('active','revoked') NOT NULL
);
-- SPIRE registration entries (mirrored from SPIRE Server)
CREATE TABLE svid_registrations (
  entry_id    VARCHAR(64) UNIQUE NOT NULL,
  spiffe_id   VARCHAR(512) UNIQUE NOT NULL,  -- spiffe://cluster.local/ns/foo/sa/bar
  parent_id   VARCHAR(512) NOT NULL,
  selectors   JSON NOT NULL,   -- [{"type":"k8s","value":"ns:foo"},{"type":"k8s","value":"sa:bar"}]
  ttl_seconds INT NOT NULL DEFAULT 86400
);
```

### SVID Rotation Algorithm
```
Cert TTL = 24 hours (86400 s)
Renewal trigger: at 67% of TTL → 16 hours (57600 s)
SPIRE Agent renews proactively:
  1. Agent calls Workload API refresh at 16h mark
  2. SPIRE Server generates new X.509 SVID (< 100 ms)
  3. New SVID delivered via Unix domain socket to workload
  4. Old SVID kept valid until TTL expires (for in-flight connections)
  5. Transparent rotation — pod never restarts, no service disruption

For key compromise:
  1. Revoke old intermediate CA → invalidates all its leaf certs
  2. Issue new intermediate CA from root
  3. Re-issue all affected SVIDs (< 1 hour for 50K pods)
```

### Unique NFRs
- SVID issuance throughput: 5,000/sec (Vault PKI engine)
- SVID TTL: 24h; renewal at 16h (67% TTL)
- mTLS handshake overhead: < 10 ms (first), < 1 ms (resumed)
- Root CA validity: 20 years; air-gapped; RSA 4096 or ECDSA P-384
- Availability: 99.99% for SPIRE/cert-manager; root CA never online

---

## rbac_authorization_system

### Unique Purpose
Enterprise RBAC spanning Kubernetes, OpenStack, job-scheduler, API-gateway. 50K roles, 200K bindings, 500K authorization decisions/sec, < 1 ms cached latency. Supports role inheritance and ReBAC (Zanzibar-like object-level relationships).

### Unique Architecture
- **OPA embedded**: Go library; Rego policy evaluation in-process at Authorization Gateway; no network hop for evaluation
- **Two-tier cache**: in-process LRU (100K entries, 60 s TTL) → Redis cluster → MySQL on miss
- **ReBAC**: Zanzibar-like relationship tuples for object-level access (`user X is owner of project Y`)
- **Kubernetes sync**: Role/ClusterRole/RoleBinding objects mirrored to MySQL; bidirectional sync controller

### Key Schema
```sql
CREATE TABLE roles (
  role_id     BIGINT PRIMARY KEY AUTO_INCREMENT,
  role_name   VARCHAR(128) NOT NULL,
  scope_type  ENUM('cluster','namespace','project','resource') NOT NULL,
  scope_value VARCHAR(256),
  parent_role_id BIGINT,                 -- for role inheritance
  is_system   BOOLEAN NOT NULL DEFAULT FALSE,
  created_by  VARCHAR(128) NOT NULL,
  created_at  DATETIME(6) NOT NULL,
  UNIQUE (role_name, scope_type, scope_value)
);
CREATE TABLE permissions (
  permission_id BIGINT PRIMARY KEY,
  role_id    BIGINT NOT NULL REFERENCES roles(role_id),
  resource   VARCHAR(128) NOT NULL,     -- "pods", "secrets", "jobs"
  verb       VARCHAR(64)  NOT NULL,     -- "get", "list", "create", "*"
  effect     ENUM('allow','deny') NOT NULL DEFAULT 'allow',
  conditions JSON                        -- optional attribute conditions
);
CREATE TABLE role_bindings (
  binding_id   BIGINT PRIMARY KEY,
  role_id      BIGINT NOT NULL,
  subject_type ENUM('user','group','service_account') NOT NULL,
  subject_name VARCHAR(256) NOT NULL,
  scope_type   ENUM('cluster','namespace','project') NOT NULL,
  scope_value  VARCHAR(256),
  expires_at   DATETIME(6),
  created_by   VARCHAR(128) NOT NULL,
  created_at   DATETIME(6) NOT NULL
);
CREATE TABLE relationships (  -- ReBAC (Zanzibar-like)
  object   VARCHAR(256) NOT NULL,   -- "project:my-project"
  relation VARCHAR(64)  NOT NULL,   -- "owner", "editor", "viewer"
  subject  VARCHAR(256) NOT NULL,   -- "user:alice" or "group:eng-team#member"
  created_at DATETIME(6) NOT NULL,
  PRIMARY KEY (object, relation, subject)
);
CREATE TABLE authorization_audit (
  event_id   BIGINT PRIMARY KEY,
  subject    VARCHAR(256) NOT NULL,
  action     VARCHAR(64)  NOT NULL,
  resource   VARCHAR(256) NOT NULL,
  decision   ENUM('allow','deny') NOT NULL,
  timestamp  DATETIME(6) NOT NULL,
  eval_trace JSON           -- which rule matched
) PARTITION BY RANGE (UNIX_TIMESTAMP(timestamp));
```

### Unique Algorithm: OPA Rego Evaluation
```rego
package rbac.authz
import future.keywords.in

default allow := false

# Direct role binding match
allow if {
    binding := data.bindings[_]
    binding.subject == input.subject
    scope_matches(binding, input.resource)
    perm := data.roles[binding.role_id].permissions[_]
    perm.resource in [input.resource.type, "*"]
    perm.verb in [input.action, "*"]
    perm.effect == "allow"
}

# Inherited permission via parent role
allow if {
    binding := data.bindings[_]
    binding.subject == input.subject
    ancestor := ancestor_roles[binding.role_id][_]  # recursive parent traversal
    perm := data.roles[ancestor].permissions[_]
    perm.resource in [input.resource.type, "*"]
    perm.verb in [input.action, "*"]
}

# Explicit deny overrides allow
deny if {
    binding := data.bindings[_]
    binding.subject == input.subject
    perm := data.roles[binding.role_id].permissions[_]
    perm.effect == "deny"
    perm.resource in [input.resource.type, "*"]
    perm.verb in [input.action, "*"]
}

authz_decision := "deny"  if deny
authz_decision := "allow" if { allow; not deny }
authz_decision := "deny"  if { not allow; not deny }  # default deny
```

### Unique NFRs
- Decision throughput: 500K/sec
- Latency: < 1 ms (LRU cache hit), < 5 ms (cold OPA eval), < 100 ms (policy bundle reload)
- Policy propagation: < 2 s from MySQL change to all OPA instances (Kafka CDC)
- Scale: 50K roles, 200K bindings, 100+ namespaces
- Kubernetes admission webhook: < 10 ms P99 (OPA/Gatekeeper)

---

## secrets_management_system

### Unique Purpose
HashiCorp Vault-compatible secrets management for bare-metal IaaS + Kubernetes: 100K secret paths, 200K active leases, 10K reads/sec, dynamic credentials with auto-revocation, HSM-backed auto-unseal.

### Unique Architecture
- **Vault cluster**: 5-node Raft integrated storage; 1 active + 4 standby; failover < 30 s
- **Secret engines (pluggable)**: KV v2 (static + versioned), Database (dynamic creds), PKI, Transit (AES-256-GCM), AWS/GCP (IAM tokens)
- **Auth methods (pluggable)**: Kubernetes SA JWT (via TokenReview API), AppRole, LDAP, OIDC
- **Vault Agent sidecar**: auto-auth, template rendering (writes secrets to pod filesystem), in-memory TTL-aware cache
- **Vault Secrets Operator**: Kubernetes CRD `VaultStaticSecret` / `VaultDynamicSecret` → syncs to k8s Secret objects

### Key Data Structures
```
# Vault Raft Storage (internal, not SQL)
sys/mounts:
  kv-v2/           → KV v2 engine
  database/        → Database engine (MySQL, PostgreSQL)
  pki/             → PKI engine
  transit/         → Encryption as a service

# KV v2 path structure
kv-v2/data/infra/db-password:
  data: {password: "...", username: "app"}
  metadata: {versions: {1: {...}, 2: {...}}, current_version: 2}

# Database role (dynamic creds)
database/roles/myapp-role:
  db_name: "mydb-mysql"
  creation_statements: ["CREATE USER '{{name}}'@'%' IDENTIFIED BY '{{password}}' ...",
                        "GRANT SELECT,INSERT ON mydb.* TO '{{name}}'@'%'"]
  default_ttl: "1h"
  max_ttl: "24h"

# Lease table (in-memory + Raft-persisted)
leases/{lease_id}:
  path: "database/creds/myapp-role"
  entity_id: "k8s-serviceaccount-xyz"
  ttl: 3600
  renewable: true
  created_at: <unix>
  expires_at: <unix>

# Vault policy (HCL)
path "kv-v2/data/infra/*" {
  capabilities = ["read", "list"]
}
path "database/creds/myapp-role" {
  capabilities = ["read"]
}
```

### Unique Algorithm: Seal/Unseal (Shamir's Secret Sharing)
```
Master Key (256-bit AES) → encrypts all Raft storage
Unseal key → split into N shares (default: N=5, threshold=3)

# Auto-unseal (production):
  HSM (PKCS#11) holds unseal key → Vault contacts HSM on startup
  → No human needed for restart
  
# Manual unseal (break-glass):
  3 of 5 human key holders each enter their share
  → Vault XOR-reconstructs master key → unseals storage

# Transit unseal (non-prod):
  Separate Vault cluster holds transit key
  → Primary Vault encrypts its master key with transit key
  → On unseal: call transit/decrypt on secondary Vault
```

### Unique Algorithm: Dynamic MySQL Credential Issuance
```
vault read database/creds/myapp-role:
1. Vault connects to MySQL as vault-admin user
2. Generates random username: v-k8s-myapp-<8-hex>
3. Generates 32-char random password (CSPRNG)
4. Executes: CREATE USER '{name}'@'%' IDENTIFIED BY '{password}'
5. Executes: GRANT SELECT,INSERT ON mydb.* TO '{name}'@'%'
6. Returns: {username, password, lease_id, ttl=3600, renewable=true}
7. On lease expiry (or explicit revoke):
   Executes: DROP USER '{name}'@'%'
   → Credential auto-revoked; no manual cleanup
```

### Unique NFRs
- Secret reads: 10K/sec (Vault server); 100K/sec (Vault Agent cache)
- Dynamic credential issuance: < 50 ms (database engine)
- Lease TTL: 1h default (database); 24h (PKI SVIDs); renewable up to max_ttl
- Availability: 99.99% (5-node Raft; 3-node quorum; < 30 s leader failover)
- Audit: 100% coverage; every read/write/auth logged via syslog → Kafka

---

## zero_trust_network_design

### Unique Purpose
BeyondCorp-style zero trust for bare-metal Kubernetes: 500 nodes, 50K pods, no VPN. Identity-based micro-segmentation via eBPF + mTLS + context-aware access proxy for human access (kubectl, SSH, database).

### Unique Architecture
- **Cilium (eBPF)**: label-derived numeric identity (not IP-based); CiliumNetworkPolicy at L3/L4/L7; Hubble for flow observability
- **SPIFFE/SPIRE**: workload identity (X.509 SVIDs) for all pod-to-pod mTLS
- **Istio/Envoy**: per-pod mTLS sidecar; AuthorizationPolicy for L7 (method, path, SPIFFE identity)
- **Context-Aware Access Proxy**: human access via Teleport/Boundary (SSH short-lived certs), kube-gate (kubectl), ProxySQL + identity-aware (DB)
- **Device trust registry**: continuous device posture scoring; score < 80 → session terminated

### Key Schema
```sql
CREATE TABLE device_registry (
  device_id        VARCHAR(64) UNIQUE NOT NULL,
  owner_email      VARCHAR(256) NOT NULL,
  device_uuid      VARCHAR(36) NOT NULL,
  os_type          ENUM('linux','macos','windows') NOT NULL,
  os_version       VARCHAR(64) NOT NULL,
  disk_encrypted   BOOLEAN NOT NULL,
  device_cert      TEXT,           -- X.509 device identity
  enrolled_at      DATETIME(6) NOT NULL,
  last_verified_at DATETIME(6) NOT NULL,
  posture_status   ENUM('compliant','non_compliant','unknown') NOT NULL
);
CREATE TABLE access_proxy_sessions (
  session_id       VARCHAR(64) PRIMARY KEY,
  user_email       VARCHAR(256) NOT NULL,
  target_resource  VARCHAR(512) NOT NULL,
  proxy_type       ENUM('kubectl','ssh','database') NOT NULL,
  access_level     VARCHAR(64) NOT NULL,
  started_at       DATETIME(6) NOT NULL,
  expires_at       DATETIME(6) NOT NULL,
  terminated_at    DATETIME(6),
  recorded_session_url VARCHAR(1024)  -- for SSH session recording
);
CREATE TABLE network_flow_audit (
  flow_id          BIGINT PRIMARY KEY,
  source_identity  VARCHAR(256) NOT NULL,  -- Cilium numeric ID + label set
  dest_identity    VARCHAR(256) NOT NULL,
  src_port         SMALLINT,
  dst_port         SMALLINT NOT NULL,
  protocol         TINYINT NOT NULL,
  verdict          ENUM('ALLOWED','DENIED') NOT NULL,
  policy_name      VARCHAR(256),
  timestamp_utc    DATETIME(6) NOT NULL
) PARTITION BY RANGE (UNIX_TIMESTAMP(timestamp_utc));
```

### Unique Algorithm: Cilium Identity Model
```
# Labels → numeric identity (not IP-based)
Pod A {app=frontend, env=prod, ns=web} → Cilium Identity 12345
Pod B {app=api-server, env=prod, ns=api} → Cilium Identity 12346

# eBPF map (kernel-level, O(1) lookup)
{src: 12345, dst: 12346, port: 8080, proto: TCP} → ALLOW
{src: 12345, dst: *,     port: *,    proto: *}   → DENY (default)

# Policy survives pod restarts: IP changes, identity (labels) stays same
# Namespace policy example:
apiVersion: cilium.io/v2
kind: CiliumNetworkPolicy
metadata: {name: frontend-egress}
spec:
  endpointSelector: {matchLabels: {app: frontend}}
  egress:
  - toEndpoints:
    - matchLabels: {app: api-server}
    toPorts:
    - ports: [{port: "8080", protocol: TCP}]
```

### Unique Algorithm: Device Trust Scoring
```python
def compute_device_trust_score(device: Device) -> int:
    score = 0
    if device.os_patched_within_days(30):  score += 20
    if device.disk_encrypted:              score += 20
    if device.no_unapproved_software:      score += 20
    if device.antimalware_running:         score += 20
    if device.mfa_enabled:                 score += 20
    return score  # 0–100

# Access proxy: check score before granting session
if compute_device_trust_score(device) < 80:
    raise AccessDenied("device posture non-compliant")

# Continuous evaluation: re-check every 60s during active session
# If score drops below 80 mid-session → terminate immediately
```

### Unique NFRs
- eBPF enforcement: < 0.1 ms per packet; 10M+ policy evals/sec at kernel level
- mTLS handshake: < 10 ms first connection; < 1 ms resumed
- Policy propagation: < 5 s from CiliumNetworkPolicy change to kernel enforcement on all nodes
- Network flow audit: 100% coverage via Hubble; partitioned by hour
- Access proxy sessions: TTL-bound; session recording for SSH; device trust continuous
- Availability: 99.99% data plane (eBPF never loses packets on control plane downtime); 99.9% control plane
