# System Design: Certificate Lifecycle Management

> **Relevance to role:** On a bare-metal IaaS platform running Kubernetes, every service-to-service call, every ingress endpoint, and every workload identity assertion depends on X.509 certificates. A certificate lifecycle management system handles issuance, renewal, rotation, and revocation of certificates across hundreds of nodes and tens of thousands of pods. Expired certificates cause hard outages; compromised keys require instant revocation. This system is the foundation for mTLS, workload identity (SPIFFE/SPIRE), and zero-trust networking.

---

## 1. Requirement Clarifications

### Functional Requirements

| ID | Requirement |
|----|-------------|
| FR-1 | Manage a **PKI hierarchy**: offline root CA, online intermediate CAs, leaf certificates. |
| FR-2 | Automated certificate issuance via **cert-manager** (Kubernetes) with Certificate, Issuer, ClusterIssuer CRDs. |
| FR-3 | **ACME protocol** support for public-facing certificates (Let's Encrypt). |
| FR-4 | **Vault PKI engine** for internal certificate issuance (short-lived, high-volume). |
| FR-5 | **mTLS** enforcement for all service-to-service communication. |
| FR-6 | **SPIFFE/SPIRE** for workload identity: issue SPIFFE Verifiable Identity Documents (SVIDs). |
| FR-7 | **Zero-downtime certificate rotation**: no service interruption during cert renewal. |
| FR-8 | **Certificate monitoring**: alert on certificates expiring within 30 days. |
| FR-9 | **Revocation**: CRL (Certificate Revocation List) and OCSP (Online Certificate Status Protocol). |
| FR-10 | **Key compromise response**: emergency re-issuance of all certificates signed by compromised CA. |

### Non-Functional Requirements

| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Certificate issuance latency | < 500 ms (Vault PKI), < 30 s (ACME/Let's Encrypt) |
| NFR-2 | Certificate renewal success rate | 99.99% (failed renewals = outage) |
| NFR-3 | Availability of issuance path | 99.99% |
| NFR-4 | SVID issuance throughput | 5,000 SVIDs/sec (for pod churn) |
| NFR-5 | Monitoring coverage | 100% of certificates tracked |
| NFR-6 | CRL/OCSP response latency | < 50 ms |
| NFR-7 | Key compromise response time | < 1 hour to re-issue all affected certificates |

### Constraints & Assumptions

- Bare-metal Kubernetes 1.28+ with 500+ nodes and 50,000+ pods.
- Service mesh (Istio/Linkerd) for mTLS, or direct SPIRE integration.
- Public-facing endpoints use Let's Encrypt (ACME).
- Internal PKI uses Vault PKI engine with short-lived certificates (24h TTL).
- cert-manager v1.13+ deployed in every cluster.
- Java services use JKS/PKCS12; Python services use PEM files.

### Out of Scope

- SSH certificate management (separate system).
- Client-side TLS for end-user browsers (CDN handles this).
- Code signing certificates.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Kubernetes nodes | 500 | Bare-metal fleet |
| Pods (peak) | 50,000 | Running concurrently |
| Internal certificates (mTLS leaf certs) | 50,000 | One per pod (SVID or cert-manager issued) |
| Ingress certificates (public) | 500 | Public-facing services |
| Node certificates (kubelet, etcd) | 2,000 | 500 nodes x 4 certs (kubelet, kube-proxy, etcd client, etcd peer) |
| Certificate issuances/day | 100,000 | 50,000 pods x 2 avg restarts/day |
| Certificate renewals/day | 50,000 | 24h TTL, 50K active certs renewed daily |
| SVID issuances/sec (peak) | 5,000 | Pod churn during deployments |
| ACME renewals/day | ~8 | 500 certs / 60-day lifetime / renew at 30 days |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Vault PKI certificate issuance | < 500 ms p99 |
| SPIRE SVID issuance | < 100 ms p99 |
| cert-manager Certificate ready | < 30 s (ACME), < 5 s (Vault) |
| OCSP response | < 50 ms p99 |
| Certificate rotation (zero downtime) | < 60 s total |

### Storage Estimates

| Data | Size | Calculation |
|------|------|-------------|
| Active certificates (metadata) | ~500 MB | 50,000 certs x 10 KB metadata |
| CRL | ~5 MB | 10,000 revoked serial numbers x 500 bytes |
| Certificate audit log (1 day) | ~5 GB | 150,000 events x 35 KB |
| CA private keys (HSM-protected) | < 1 KB | RSA 4096 or ECDSA P-384 |
| Certificate chain bundles | ~50 MB | 500 unique chains x 100 KB |

### Bandwidth Estimates

| Flow | Bandwidth |
|------|-----------|
| Certificate issuance (Vault PKI) | ~5 MB/sec peak (5,000 issuances/sec x 1 KB cert) |
| OCSP responses | ~500 KB/sec (10,000 checks/sec x 50 bytes) |
| CRL distribution | ~5 MB per update, distributed via CDN |
| SPIRE SVID delivery | ~5 MB/sec peak |

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                        PKI Hierarchy                              │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │  Root CA (OFFLINE)                                        │    │
│  │  - RSA 4096 / ECDSA P-384                                │    │
│  │  - 20-year validity                                       │    │
│  │  - Stored in HSM (air-gapped)                            │    │
│  │  - Signs intermediate CAs only                            │    │
│  └──────────────────────┬───────────────────────────────────┘    │
│                         │ signs                                    │
│  ┌──────────────────────▼───────────────────────────────────┐    │
│  │  Intermediate CA(s) (ONLINE)                              │    │
│  │  - 5-year validity                                        │    │
│  │  - Hosted in Vault PKI engine                            │    │
│  │  - Signs leaf certificates                                │    │
│  │  - Separate CAs for: infra, workload, ingress            │    │
│  └──────────────┬────────────────────┬──────────────────────┘    │
│                 │                    │                             │
│      ┌──────────▼──────┐  ┌─────────▼─────────┐                 │
│      │  Leaf Certs     │  │  Leaf Certs        │                 │
│      │  (internal mTLS)│  │  (ingress TLS)     │                 │
│      │  24h TTL        │  │  90-day (ACME)     │                 │
│      └─────────────────┘  └────────────────────┘                 │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                    Certificate Issuance Layer                     │
│                                                                   │
│  ┌─────────────────┐  ┌────────────────┐  ┌───────────────────┐ │
│  │  Vault PKI      │  │  SPIRE Server  │  │  cert-manager     │ │
│  │  Engine         │  │                │  │  Controller       │ │
│  │                 │  │  Issues SVIDs  │  │                   │ │
│  │  Signs CSRs     │  │  (X.509 +     │  │  Manages cert     │ │
│  │  Issues certs   │  │   JWT SVIDs)  │  │  lifecycle via    │ │
│  │  Short TTL      │  │               │  │  CRDs             │ │
│  │  (24h default)  │  │  SPIFFE trust │  │                   │ │
│  │                 │  │  domain       │  │  Issuers:         │ │
│  │                 │  │               │  │  - Vault           │ │
│  │                 │  │               │  │  - ACME (LE)       │ │
│  │                 │  │               │  │  - SelfSigned      │ │
│  └────────┬────────┘  └───────┬───────┘  └────────┬──────────┘ │
└───────────┼───────────────────┼────────────────────┼─────────────┘
            │                   │                    │
            ▼                   ▼                    ▼
┌──────────────────────────────────────────────────────────────────┐
│                    Kubernetes Cluster                              │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  cert-manager CRDs:                                       │   │
│  │  Certificate ──▶ Issuer/ClusterIssuer ──▶ k8s Secret     │   │
│  │                    (tls.crt + tls.key)                    │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  SPIRE Agent (DaemonSet, per node):                       │   │
│  │  - Attests workload identity (k8s, unix PID)              │   │
│  │  - Delivers SVIDs to pods via Workload API (UDS)          │   │
│  │  - Rotates SVIDs automatically                            │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Service Mesh (Istio/Linkerd):                            │   │
│  │  - Envoy sidecar terminates/originates mTLS               │   │
│  │  - Uses SPIRE SVIDs or cert-manager certificates          │   │
│  │  - AuthorizationPolicy for fine-grained access            │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Monitoring:                                               │   │
│  │  - cert-manager Prometheus metrics                        │   │
│  │  - Certificate expiry exporter                            │   │
│  │  - Grafana dashboards + PagerDuty alerts                  │   │
│  └──────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                    Revocation Layer                                │
│                                                                   │
│  ┌────────────────────┐  ┌────────────────────────────────────┐ │
│  │  CRL Distribution  │  │  OCSP Responder                    │ │
│  │  Point (CDP)       │  │  (Vault built-in or standalone)    │ │
│  │  Updated on revoke │  │  Real-time revocation check        │ │
│  │  Cached at CDN     │  │  Stapled to TLS handshake          │ │
│  └────────────────────┘  └────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Root CA** | Offline, air-gapped. Signs only intermediate CAs. Key stored in HSM. Validity: 20 years. |
| **Intermediate CA(s)** | Online, hosted in Vault PKI engine. Signs leaf certificates. Separate CAs for different trust domains (infra, workload, ingress). Validity: 5 years. |
| **Vault PKI Engine** | High-volume certificate issuance. Signs CSRs, issues certs with short TTLs (24h). Supports automated renewal. |
| **SPIRE Server** | Issues SPIFFE Verifiable Identity Documents (SVIDs) for workload identity. Manages trust bundles across trust domains. |
| **SPIRE Agent** | DaemonSet on each node. Attests workloads, delivers SVIDs via SPIFFE Workload API (Unix domain socket). |
| **cert-manager** | Kubernetes controller. Manages Certificate CRDs -> Issuer -> k8s Secret. Supports Vault, ACME, self-signed issuers. |
| **Service Mesh** | Envoy sidecar handles mTLS termination/origination. Consumes certificates from SPIRE or cert-manager. |
| **CRL/OCSP** | Revocation infrastructure. CRL: periodic list of revoked serials. OCSP: real-time revocation check. |
| **Certificate Monitoring** | Prometheus exporters + Grafana dashboards. Alerts on expiry, issuance failures, chain issues. |

### Data Flows

1. **Internal cert issuance (Vault PKI via cert-manager):** Pod created -> cert-manager watches Certificate CRD -> generates CSR -> sends to Vault PKI `/sign` endpoint -> Vault signs with intermediate CA -> cert-manager stores cert + key in k8s Secret -> pod mounts Secret.

2. **SPIRE SVID issuance:** Pod starts -> SPIRE Agent on same node attests the workload (k8s pod attestation) -> Agent requests SVID from SPIRE Server -> Server issues X.509 SVID with SPIFFE ID (`spiffe://cluster.local/ns/myapp/sa/myapp-sa`) -> Agent delivers SVID to pod via Workload API.

3. **ACME (Let's Encrypt):** cert-manager detects Certificate CRD with ACME issuer -> creates Order -> solves challenge (HTTP-01 or DNS-01) -> Let's Encrypt validates -> issues 90-day cert -> cert-manager stores in k8s Secret -> renews at 60 days (30 days before expiry).

4. **mTLS handshake:** Client pod (Envoy sidecar) initiates TLS to server pod -> both present leaf certs -> each validates the other's cert chain up to the trusted root -> mTLS established.

5. **Revocation:** Admin revokes certificate via Vault CLI -> serial added to CRL -> CRL pushed to CDN -> OCSP responder updated -> clients checking CRL/OCSP reject the revoked certificate.

---

## 4. Data Model

### Core Entities & Schema (Full SQL)

```sql
-- ============================================================
-- CERTIFICATE AUTHORITIES
-- ============================================================
CREATE TABLE certificate_authorities (
    ca_id            BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    ca_name          VARCHAR(255) NOT NULL UNIQUE,
    ca_type          ENUM('root', 'intermediate') NOT NULL,
    parent_ca_id     BIGINT UNSIGNED DEFAULT NULL,
    subject_dn       VARCHAR(1024) NOT NULL COMMENT 'e.g., CN=Platform Root CA, O=Corp',
    key_algorithm    ENUM('rsa-2048', 'rsa-4096', 'ecdsa-p256', 'ecdsa-p384') NOT NULL,
    serial_number    VARCHAR(128) NOT NULL UNIQUE,
    not_before       TIMESTAMP NOT NULL,
    not_after        TIMESTAMP NOT NULL,
    crl_url          VARCHAR(512) DEFAULT NULL,
    ocsp_url         VARCHAR(512) DEFAULT NULL,
    vault_mount_path VARCHAR(255) DEFAULT NULL COMMENT 'e.g., pki/infra-intermediate',
    key_storage      ENUM('hsm', 'vault', 'software') NOT NULL,
    is_active        BOOLEAN NOT NULL DEFAULT TRUE,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (parent_ca_id) REFERENCES certificate_authorities(ca_id),
    INDEX idx_type_active (ca_type, is_active),
    INDEX idx_not_after (not_after)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- ISSUED CERTIFICATES (inventory/tracking)
-- ============================================================
CREATE TABLE issued_certificates (
    cert_id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    serial_number    VARCHAR(128) NOT NULL UNIQUE,
    issuing_ca_id    BIGINT UNSIGNED NOT NULL,
    subject_cn       VARCHAR(255) NOT NULL,
    subject_dn       VARCHAR(1024) NOT NULL,
    san_dns          JSON DEFAULT NULL COMMENT '["api.corp.com", "*.corp.com"]',
    san_ip           JSON DEFAULT NULL COMMENT '["10.0.1.1"]',
    san_uri          JSON DEFAULT NULL COMMENT '["spiffe://cluster.local/ns/myapp/sa/myapp-sa"]',
    key_algorithm    ENUM('rsa-2048', 'rsa-4096', 'ecdsa-p256', 'ecdsa-p384') NOT NULL,
    not_before       TIMESTAMP NOT NULL,
    not_after        TIMESTAMP NOT NULL,
    cert_type        ENUM('server', 'client', 'mtls', 'svid', 'ingress') NOT NULL,
    issuance_source  ENUM('vault_pki', 'spire', 'acme', 'manual') NOT NULL,
    kubernetes_ns    VARCHAR(255) DEFAULT NULL,
    kubernetes_sa    VARCHAR(255) DEFAULT NULL,
    kubernetes_secret VARCHAR(255) DEFAULT NULL COMMENT 'k8s Secret name storing the cert',
    spiffe_id        VARCHAR(512) DEFAULT NULL,
    status           ENUM('active', 'expired', 'revoked', 'pending_renewal') NOT NULL DEFAULT 'active',
    revoked_at       TIMESTAMP DEFAULT NULL,
    revocation_reason ENUM('unspecified', 'keyCompromise', 'caCompromise', 'affiliationChanged',
                          'superseded', 'cessationOfOperation') DEFAULT NULL,
    fingerprint_sha256 VARCHAR(64) NOT NULL COMMENT 'SHA-256 of DER-encoded cert',
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (issuing_ca_id) REFERENCES certificate_authorities(ca_id),
    INDEX idx_ca_status (issuing_ca_id, status),
    INDEX idx_subject (subject_cn),
    INDEX idx_not_after (not_after),
    INDEX idx_spiffe (spiffe_id),
    INDEX idx_k8s (kubernetes_ns, kubernetes_sa),
    INDEX idx_fingerprint (fingerprint_sha256),
    INDEX idx_status_expiry (status, not_after)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- CERTIFICATE REVOCATION LIST
-- ============================================================
CREATE TABLE certificate_revocations (
    revocation_id    BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    serial_number    VARCHAR(128) NOT NULL,
    issuing_ca_id    BIGINT UNSIGNED NOT NULL,
    revoked_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    reason           ENUM('unspecified', 'keyCompromise', 'caCompromise', 'affiliationChanged',
                          'superseded', 'cessationOfOperation') NOT NULL DEFAULT 'unspecified',
    revoked_by       VARCHAR(255) NOT NULL,
    crl_published    BOOLEAN NOT NULL DEFAULT FALSE,
    ocsp_updated     BOOLEAN NOT NULL DEFAULT FALSE,
    UNIQUE KEY uk_serial_ca (serial_number, issuing_ca_id),
    FOREIGN KEY (issuing_ca_id) REFERENCES certificate_authorities(ca_id),
    INDEX idx_ca_published (issuing_ca_id, crl_published)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- CERTIFICATE MONITORING
-- ============================================================
CREATE TABLE certificate_monitors (
    monitor_id       BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    cert_id          BIGINT UNSIGNED DEFAULT NULL,
    endpoint         VARCHAR(512) DEFAULT NULL COMMENT 'e.g., api.corp.com:443',
    check_type       ENUM('expiry', 'chain', 'ocsp', 'crl', 'mtls') NOT NULL,
    last_checked     TIMESTAMP DEFAULT NULL,
    last_status      ENUM('ok', 'warning', 'critical', 'error') NOT NULL DEFAULT 'ok',
    days_to_expiry   INT DEFAULT NULL,
    alert_threshold_days INT NOT NULL DEFAULT 30,
    notified_at      TIMESTAMP DEFAULT NULL,
    FOREIGN KEY (cert_id) REFERENCES issued_certificates(cert_id),
    INDEX idx_status (last_status),
    INDEX idx_days (days_to_expiry)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- SPIFFE TRUST DOMAINS
-- ============================================================
CREATE TABLE spiffe_trust_domains (
    domain_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    trust_domain     VARCHAR(255) NOT NULL UNIQUE COMMENT 'e.g., cluster.local',
    spire_server_url VARCHAR(512) NOT NULL,
    trust_bundle     LONGTEXT NOT NULL COMMENT 'PEM-encoded CA bundle',
    bundle_updated   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    is_active        BOOLEAN NOT NULL DEFAULT TRUE,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- SPIFFE REGISTRATION ENTRIES
-- ============================================================
CREATE TABLE spiffe_registrations (
    entry_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    spiffe_id        VARCHAR(512) NOT NULL COMMENT 'spiffe://cluster.local/ns/myapp/sa/myapp-sa',
    parent_id        VARCHAR(512) NOT NULL COMMENT 'spiffe://cluster.local/agent/node-xxxx',
    selectors        JSON NOT NULL COMMENT '[{"type":"k8s","value":"ns:myapp"},{"type":"k8s","value":"sa:myapp-sa"}]',
    trust_domain_id  BIGINT UNSIGNED NOT NULL,
    ttl_seconds      INT UNSIGNED NOT NULL DEFAULT 86400,
    dns_names        JSON DEFAULT NULL,
    is_active        BOOLEAN NOT NULL DEFAULT TRUE,
    created_at       TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (trust_domain_id) REFERENCES spiffe_trust_domains(domain_id),
    UNIQUE KEY uk_spiffe_parent (spiffe_id, parent_id),
    INDEX idx_trust_domain (trust_domain_id),
    INDEX idx_spiffe_id (spiffe_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ============================================================
-- CERTIFICATE LIFECYCLE AUDIT
-- ============================================================
CREATE TABLE certificate_audit_log (
    audit_id         BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    timestamp_utc    TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    event_type       ENUM('issue', 'renew', 'revoke', 'expire', 'rotation_start', 'rotation_complete',
                          'ca_sign', 'crl_publish', 'ocsp_update', 'key_compromise') NOT NULL,
    cert_serial      VARCHAR(128) DEFAULT NULL,
    ca_name          VARCHAR(255) DEFAULT NULL,
    subject_cn       VARCHAR(255) DEFAULT NULL,
    spiffe_id        VARCHAR(512) DEFAULT NULL,
    performed_by     VARCHAR(255) NOT NULL,
    details          JSON DEFAULT NULL,
    request_id       VARCHAR(128) DEFAULT NULL,
    INDEX idx_timestamp (timestamp_utc),
    INDEX idx_serial (cert_serial),
    INDEX idx_event (event_type, timestamp_utc),
    INDEX idx_ca (ca_name, timestamp_utc)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (UNIX_TIMESTAMP(timestamp_utc)) (
    -- Daily partitions
);
```

### Database Selection

| Store | Use Case | Justification |
|-------|----------|---------------|
| **HSM** | Root CA private key, intermediate CA private keys | Hardware protection; key never exported. |
| **Vault PKI Engine** | Online intermediate CA + certificate issuance | Integrated with Vault auth/policy; high-volume signing. |
| **SPIRE Server (SQLite/PostgreSQL)** | SVID registration entries, trust bundles | SPIRE's native datastore for workload registrations. |
| **MySQL 8.0** | Certificate inventory, monitoring, audit | Relational queries for compliance reporting. |
| **etcd (Kubernetes)** | cert-manager Certificate CRDs, k8s Secrets (TLS) | Kubernetes-native storage for cert-manager objects. |
| **Elasticsearch 8.x** | Certificate audit log search | Full-text search, aggregation dashboards. |

### Indexing Strategy

| Table | Key Index | Purpose |
|-------|-----------|---------|
| `issued_certificates` | `(status, not_after)` | Find certificates expiring soon. |
| `issued_certificates` | `(kubernetes_ns, kubernetes_sa)` | Find certs for a specific workload. |
| `issued_certificates` | `(fingerprint_sha256)` | Lookup cert by fingerprint (debugging). |
| `certificate_revocations` | `(issuing_ca_id, crl_published)` | Find revocations not yet in CRL. |
| `certificate_audit_log` | PARTITION by day + `(event_type, timestamp_utc)` | Audit queries. |

---

## 5. API Design

### REST Endpoints

```
# ── Certificate Issuance ─────────────────────────────────────
POST   /api/v1/certificates/issue
  Body: {
    "common_name": "myapp.myapp-ns.svc.cluster.local",
    "san_dns": ["myapp.myapp-ns.svc", "myapp"],
    "san_ip": ["10.96.1.100"],
    "ttl": "24h",
    "key_type": "ecdsa-p256",
    "issuer": "vault-infra-intermediate"
  }
  Response: {
    "serial_number": "3a:1b:2c:...",
    "certificate": "-----BEGIN CERTIFICATE-----\n...",
    "private_key": "-----BEGIN EC PRIVATE KEY-----\n...",
    "ca_chain": ["-----BEGIN CERTIFICATE-----\n..."],
    "not_after": "2026-04-10T15:00:00Z",
    "fingerprint_sha256": "ab:cd:ef:..."
  }

POST   /api/v1/certificates/sign-csr
  Body: {
    "csr": "-----BEGIN CERTIFICATE REQUEST-----\n...",
    "ttl": "24h",
    "issuer": "vault-infra-intermediate"
  }

# ── Certificate Inventory ────────────────────────────────────
GET    /api/v1/certificates                   # List all (filter: status, expiry, namespace)
GET    /api/v1/certificates/{serial}          # Get certificate details
GET    /api/v1/certificates/expiring          # Certificates expiring within N days
  Query: ?days=30&namespace=prod

# ── Certificate Revocation ───────────────────────────────────
POST   /api/v1/certificates/{serial}/revoke
  Body: {"reason": "keyCompromise", "performed_by": "admin@corp.com"}

GET    /api/v1/crl/{ca_name}                 # Get CRL for a CA (PEM/DER)
GET    /api/v1/ocsp/{ca_name}                # OCSP responder endpoint

# ── CA Management ────────────────────────────────────────────
GET    /api/v1/cas                            # List certificate authorities
GET    /api/v1/cas/{ca_id}                    # Get CA details
POST   /api/v1/cas/{ca_id}/rotate            # Rotate intermediate CA key
GET    /api/v1/cas/{ca_id}/chain              # Get full CA chain (PEM)

# ── SPIFFE ───────────────────────────────────────────────────
POST   /api/v1/spiffe/registrations           # Create SPIFFE registration entry
GET    /api/v1/spiffe/registrations           # List registrations
DELETE /api/v1/spiffe/registrations/{entry_id}
GET    /api/v1/spiffe/trust-bundle            # Get trust bundle for federation
POST   /api/v1/spiffe/trust-bundle/federate   # Federate with external trust domain

# ── Monitoring ───────────────────────────────────────────────
GET    /api/v1/certificates/health             # Overall PKI health dashboard
GET    /api/v1/certificates/metrics            # Prometheus-compatible metrics

# ── Emergency Operations ─────────────────────────────────────
POST   /api/v1/emergency/reissue-all
  Body: {"ca_id": 3, "reason": "keyCompromise"}
  # Triggers mass re-issuance of all certs signed by compromised CA
```

### CLI

```bash
# ── Certificate Issuance ──
platform-cert issue --cn myapp.myapp-ns.svc.cluster.local \
  --san-dns myapp.myapp-ns.svc,myapp \
  --ttl 24h --key-type ecdsa-p256 --issuer vault-infra-intermediate \
  --output /app/tls/

platform-cert sign-csr --csr /tmp/myapp.csr --ttl 24h --issuer vault-infra-intermediate

# ── Inventory ──
platform-cert list --status active --expiring-within 30d
platform-cert list --namespace prod --issuer vault-infra-intermediate
platform-cert describe --serial 3a:1b:2c:...
platform-cert verify --cert /app/tls/tls.crt --ca-chain /app/tls/ca.crt

# ── Revocation ──
platform-cert revoke --serial 3a:1b:2c:... --reason keyCompromise
platform-cert crl download --ca vault-infra-intermediate --output /tmp/crl.pem

# ── Monitoring ──
platform-cert check-expiry --threshold 30d
platform-cert health --verbose

# ── SPIFFE ──
platform-cert spiffe register \
  --spiffe-id spiffe://cluster.local/ns/myapp/sa/myapp-sa \
  --parent-id spiffe://cluster.local/agent/node-001 \
  --selector k8s:ns:myapp --selector k8s:sa:myapp-sa \
  --ttl 24h

platform-cert spiffe list-registrations --namespace myapp

# ── cert-manager (kubectl) ──
kubectl get certificates -n myapp
kubectl describe certificate myapp-tls -n myapp
kubectl get certificaterequests -n myapp

# cert-manager Certificate CRD:
cat <<EOF | kubectl apply -f -
apiVersion: cert-manager.io/v1
kind: Certificate
metadata:
  name: myapp-tls
  namespace: myapp
spec:
  secretName: myapp-tls-secret
  duration: 24h
  renewBefore: 8h
  issuerRef:
    name: vault-infra-issuer
    kind: ClusterIssuer
  commonName: myapp.myapp.svc.cluster.local
  dnsNames:
  - myapp.myapp.svc.cluster.local
  - myapp.myapp.svc
  - myapp
  privateKey:
    algorithm: ECDSA
    size: 256
EOF

# cert-manager ClusterIssuer (Vault):
cat <<EOF | kubectl apply -f -
apiVersion: cert-manager.io/v1
kind: ClusterIssuer
metadata:
  name: vault-infra-issuer
spec:
  vault:
    path: pki/infra-intermediate/sign/server-role
    server: https://vault.vault-system.svc:8200
    caBundle: <base64-ca-bundle>
    auth:
      kubernetes:
        role: cert-manager
        mountPath: /v1/auth/kubernetes
        serviceAccountRef:
          name: cert-manager
EOF

# ── Emergency ──
platform-cert emergency reissue-all --ca vault-infra-intermediate \
  --reason keyCompromise --confirm
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: PKI Hierarchy and Vault PKI Engine

**Why it's hard:**
The PKI hierarchy is the foundation of trust. A compromised root CA means every certificate in the platform is untrustworthy. An offline root CA is secure but operationally complex (how do you sign a new intermediate?). The Vault PKI engine must handle 5,000+ signings/sec while keeping the CA key secure. Short-lived certificates (24h) improve security but increase issuance volume dramatically.

| Approach | Security | Operational Ease | Issuance Speed |
|----------|----------|-----------------|----------------|
| **Single CA (root = issuer)** | Low (root compromise = total loss) | Simple | Fast |
| **2-tier (root + 1 intermediate)** | Good (root offline) | Medium | Fast |
| **3-tier (root + policy CA + issuing CA)** | Best (defense in depth) | Complex | Slightly slower |
| **Cross-signed intermediates** | Good (multiple trust anchors) | Complex | Fast |

**Selected: 2-tier PKI with domain-specific intermediates**

**Justification:** 2-tier provides the right balance. Root is offline (HSM-protected, used once a year to sign intermediates). Multiple intermediates partition trust: `infra-intermediate` (for node certs, etcd), `workload-intermediate` (for pod mTLS/SVIDs), `ingress-intermediate` (for public-facing TLS). Compromising one intermediate doesn't affect the others.

**Implementation Detail:**

```
PKI Hierarchy Implementation:
────────────────────────────

Step 1: Root CA (HSM, air-gapped machine)
  openssl req -new -x509 -days 7300 -key hsm:slot0 \
    -out root-ca.pem \
    -subj "/CN=Platform Root CA/O=Corp/C=US" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"

Step 2: Intermediate CAs in Vault
  # Enable PKI engine for infra
  vault secrets enable -path=pki/infra-intermediate pki
  vault secrets tune -max-lease-ttl=43800h pki/infra-intermediate

  # Generate CSR for intermediate
  vault write pki/infra-intermediate/intermediate/generate/internal \
    common_name="Platform Infra Intermediate CA" \
    key_type=ec \
    key_bits=384 \
    ou="Infrastructure" \
    organization="Corp"

  # Sign CSR with root CA (on air-gapped machine)
  openssl x509 -req -in intermediate.csr \
    -CA root-ca.pem -CAkey hsm:slot0 \
    -days 1825 \  # 5 years
    -extfile intermediate-ext.cnf

  # Import signed intermediate back to Vault
  vault write pki/infra-intermediate/intermediate/set-signed \
    certificate=@signed-intermediate.pem

Step 3: Configure Vault PKI roles
  vault write pki/infra-intermediate/roles/server-role \
    allowed_domains="svc.cluster.local,node.internal" \
    allow_subdomains=true \
    max_ttl=24h \
    key_type=ec \
    key_bits=256 \
    require_cn=false \
    allowed_uri_sans="spiffe://cluster.local/*" \
    enforce_hostnames=true \
    server_flag=true \
    client_flag=true

Step 4: Issue leaf certificate
  vault write pki/infra-intermediate/issue/server-role \
    common_name="myapp.myapp.svc.cluster.local" \
    alt_names="myapp.myapp.svc,myapp" \
    ttl=24h

Certificate Chain:
  Leaf (24h) → Infra Intermediate (5 yr) → Root (20 yr)
  
  Leaf cert includes:
  - Subject: CN=myapp.myapp.svc.cluster.local
  - SAN: DNS:myapp.myapp.svc.cluster.local, DNS:myapp.myapp.svc
  - Key Usage: Digital Signature, Key Encipherment
  - Extended Key Usage: TLS Web Server, TLS Web Client
  - Authority Info Access: OCSP URL
  - CRL Distribution Point: CRL URL
```

**CRL and OCSP Configuration:**
```bash
# CRL endpoint on Vault
vault write pki/infra-intermediate/config/urls \
    issuing_certificates="https://vault.internal:8200/v1/pki/infra-intermediate/ca" \
    crl_distribution_points="https://vault.internal:8200/v1/pki/infra-intermediate/crl" \
    ocsp_servers="https://vault.internal:8200/v1/pki/infra-intermediate/ocsp"

# Vault auto-builds CRL; also supports OCSP responder (Vault 1.12+)
```

**Failure Modes:**
- **Vault PKI engine unavailable:** cert-manager retries with exponential backoff. Existing certificates continue working (24h TTL gives buffer). Alert if issuance failure rate > 1%.
- **HSM failure (root CA):** Root CA is used once per year to sign intermediates. HSM failure has zero immediate impact on operations. Maintain a backup HSM with the same key (m-of-n key backup).
- **Intermediate CA key compromise:** Revoke the intermediate. Issue new intermediate from root CA. Re-issue all leaf certificates signed by the compromised intermediate. CRL/OCSP updated immediately.
- **Clock skew:** Certificate validation depends on accurate clocks. NTP must be enforced on all nodes. cert-manager includes `notBefore` checks.

**Interviewer Q&As:**

**Q1: Why short-lived certificates (24h) instead of 1-year certificates?**
A: Short-lived certificates reduce the window of exposure if a certificate is compromised -- the attacker has at most 24 hours before the cert expires naturally. They also reduce dependency on CRL/OCSP (if revocation infrastructure fails, damage is time-limited). The trade-off is higher issuance volume, which Vault PKI handles easily (5,000+/sec).

**Q2: How do you handle the root CA key ceremony?**
A: Formal ceremony with multiple witnesses, documented procedure. Root CA key generated on an air-gapped machine with HSM. Key ceremony happens once (initial setup) and once every 5 years (intermediate renewal). The ceremony is recorded and the procedure is version-controlled. At least 3 of 5 key custodians must be present.

**Q3: Why separate intermediate CAs for different domains?**
A: Blast radius reduction. If the workload intermediate is compromised, it only affects pod mTLS certs -- not node certs or ingress certs. Different intermediates can also have different policies (e.g., workload intermediate allows SPIFFE URIs, ingress intermediate allows public domains).

**Q4: How do you handle certificate issuance for nodes that are not in Kubernetes?**
A: Bare-metal nodes (outside k8s) use Vault PKI directly. The node's provisioning tool (Ansible/PXE) obtains a Vault token via AppRole auth, requests a certificate from Vault PKI, and installs it. Renewal is handled by a cron job or Vault Agent running natively on the node.

**Q5: What happens when the intermediate CA certificate itself is about to expire?**
A: We monitor intermediate CA expiry with a 180-day warning. Renewal: (1) Generate new CSR from Vault. (2) Root CA signs the new intermediate (key ceremony). (3) Import the new signed intermediate into Vault. (4) Vault automatically starts using the new intermediate for new issuances. (5) Old intermediate remains valid for existing certificates. Cross-signing ensures no trust gap.

**Q6: How do you validate the full chain in mTLS?**
A: Each client/server has the CA bundle (root + intermediates) in its trust store. During TLS handshake, the presented certificate's chain is validated up to the trusted root. cert-manager includes the CA chain in the k8s Secret (`ca.crt`). For SPIRE, the trust bundle is fetched from the SPIRE Server and rotated automatically.

---

### Deep Dive 2: SPIFFE/SPIRE for Workload Identity

**Why it's hard:**
In a dynamic platform with 50,000 pods, IP addresses change constantly. You cannot trust network location. Every workload needs a cryptographic identity. SPIFFE (Secure Production Identity Framework for Everyone) provides a standard (SPIFFE ID as URI), and SPIRE is the reference implementation. The challenge is attesting workloads at scale (5,000 SVIDs/sec), rotating SVIDs without disruption, and federating trust across clusters.

| Approach | Identity Granularity | Attestation Strength | Standard Compliance |
|----------|---------------------|---------------------|-------------------|
| **Kubernetes SA tokens** | Per-service-account | Medium (k8s-native) | Not universal |
| **SPIFFE/SPIRE** | Per-workload (pod) | High (node + workload attestation) | SPIFFE standard |
| **Istio identity (Citadel)** | Per-service-account | Medium (k8s SA-based) | Istio-specific |
| **Custom mTLS with Vault** | Per-certificate | Varies | Custom |

**Selected: SPIFFE/SPIRE with Kubernetes workload attestation**

**Justification:** SPIRE provides platform-agnostic workload identity that works across Kubernetes and bare-metal VMs. The SPIFFE standard ensures interoperability with other systems (Envoy, Istio, gRPC). Node attestation + workload attestation provides strong identity guarantees.

**Implementation Detail:**

```
SPIRE Architecture:
──────────────────
┌─────────────────────────────────────────────────────────┐
│  SPIRE Server (3 replicas, leader election)              │
│                                                          │
│  ┌─────────────────────────────────────────────────┐    │
│  │  Registration Entries:                            │    │
│  │  spiffe://cluster.local/ns/myapp/sa/myapp-sa     │    │
│  │    parent: spiffe://cluster.local/agent/node-001 │    │
│  │    selectors: k8s:ns:myapp, k8s:sa:myapp-sa     │    │
│  │    ttl: 24h                                       │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─────────────────────────────────────────────────┐    │
│  │  CA (signing SVIDs):                              │    │
│  │  - Uses Vault PKI as upstream CA                  │    │
│  │  - Or built-in CA with disk/memory key            │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─────────────────────────────────────────────────┐    │
│  │  Trust Bundle:                                    │    │
│  │  - Root CA certificates for this trust domain     │    │
│  │  - Federated trust bundles from other domains     │    │
│  └─────────────────────────────────────────────────┘    │
└───────────────────────┬─────────────────────────────────┘
                        │ mTLS (agent-to-server)
                        ▼
┌─────────────────────────────────────────────────────────┐
│  SPIRE Agent (DaemonSet, one per node)                   │
│                                                          │
│  ┌─────────────────────────────────────────────────┐    │
│  │  Node Attestation:                                │    │
│  │  - k8s PSAT (Projected Service Account Token)    │    │
│  │  - Or: join token, TPM, AWS IID                  │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─────────────────────────────────────────────────┐    │
│  │  Workload Attestation:                            │    │
│  │  - k8s: verify pod namespace, service account    │    │
│  │    via kubelet API                                │    │
│  │  - unix: verify PID, UID, GID                    │    │
│  └─────────────────────────────────────────────────┘    │
│                                                          │
│  ┌─────────────────────────────────────────────────┐    │
│  │  Workload API (Unix Domain Socket):               │    │
│  │  /run/spire/agent/sockets/agent.sock             │    │
│  │  - FetchX509SVID → returns X.509 cert + key      │    │
│  │  - FetchJWTSVID → returns JWT token               │    │
│  │  - FetchX509Bundles → returns trust bundles       │    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
                        │ UDS
                        ▼
┌─────────────────────────────────────────────────────────┐
│  Pod / Workload                                          │
│  ┌───────────────────┐  ┌─────────────────────────────┐ │
│  │  App Container    │  │  Envoy Sidecar              │ │
│  │                   │  │  (or SPIRE helper)          │ │
│  │                   │  │                             │ │
│  │                   │  │  SDS (Secret Discovery      │ │
│  │                   │  │  Service) integration:      │ │
│  │                   │  │  Envoy fetches SVID via SDS │ │
│  │                   │  │  from SPIRE Agent           │ │
│  └───────────────────┘  └─────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘

SVID (SPIFFE Verifiable Identity Document):
──────────────────────────────────────────
X.509 SVID:
  Subject: O=SPIRE
  URI SAN: spiffe://cluster.local/ns/myapp/sa/myapp-sa
  Key: ECDSA P-256
  Validity: 24 hours
  Issuer: Platform Workload Intermediate CA

JWT SVID:
  {
    "sub": "spiffe://cluster.local/ns/myapp/sa/myapp-sa",
    "aud": ["https://api.internal"],
    "exp": 1712678400,
    "iss": "https://spire.cluster.local"
  }
```

**SPIRE Registration (automated via controller):**
```yaml
# Kubernetes SPIFFE Controller (auto-registers pods)
apiVersion: spire.spiffe.io/v1alpha1
kind: ClusterSPIFFEID
metadata:
  name: myapp-identity
spec:
  spiffeIDTemplate: "spiffe://{{ .TrustDomain }}/ns/{{ .PodMeta.Namespace }}/sa/{{ .PodSpec.ServiceAccountName }}"
  podSelector:
    matchLabels:
      app: myapp
  namespaceSelector:
    matchLabels:
      spiffe-enabled: "true"
  ttl: "24h"
```

**Failure Modes:**
- **SPIRE Server down:** SPIRE Agent caches SVIDs locally. Existing SVIDs remain valid until TTL expiry. New workloads cannot get SVIDs until server recovers. Run 3 SPIRE Server replicas for HA.
- **SPIRE Agent crash on a node:** Pods on that node lose SVID renewal. Kubelet restarts the agent (DaemonSet). Existing SVIDs in Envoy's SDS cache remain valid until TTL.
- **SVID TTL expiry during SPIRE outage:** If outage exceeds SVID TTL (24h), mTLS breaks for affected pods. Mitigation: increase SVID TTL to 48h (trade-off: longer exposure window) or ensure SPIRE HA is robust.
- **Trust bundle rotation:** SPIRE automatically distributes trust bundle updates via the Workload API. During rotation, both old and new bundles are trusted (overlap window).

**Interviewer Q&As:**

**Q1: What is the difference between SPIFFE and SPIRE?**
A: SPIFFE is the specification (standards for identity naming `spiffe://`, SVID format, Workload API). SPIRE is the reference implementation of SPIFFE. Other implementations exist (Istio's Citadel partially implements SPIFFE). SPIFFE defines what workload identity should look like; SPIRE implements how to issue, attest, and deliver those identities.

**Q2: How does SPIRE attest that a workload is who it claims to be?**
A: Two-layer attestation. (1) Node attestation: SPIRE Agent proves its node identity to SPIRE Server (e.g., via k8s Projected SA Token -- the kubelet issues a token for the SPIRE Agent's service account, which SPIRE Server validates). (2) Workload attestation: SPIRE Agent verifies the pod's identity by querying the kubelet API to confirm the pod's namespace, service account, labels match the registration entry selectors.

**Q3: How does SPIFFE/SPIRE differ from Kubernetes service account tokens?**
A: K8s SA tokens are JWTs bound to a service account. They work within Kubernetes but are not understood outside of it. SPIFFE IDs are URIs (`spiffe://...`) that are platform-agnostic. SPIRE can issue SVIDs to Kubernetes pods, VMs, bare-metal processes -- all in the same trust domain. SPIRE also provides X.509 SVIDs (for mTLS), not just JWTs.

**Q4: How do you federate SPIFFE trust across multiple clusters?**
A: SPIRE supports trust domain federation. Each cluster has its own trust domain (e.g., `spiffe://us-east.prod`, `spiffe://eu-west.prod`). SPIRE Servers exchange trust bundles via the Bundle Endpoint. A workload in us-east can verify an SVID from eu-west by checking against eu-west's trust bundle. This enables cross-cluster mTLS without a shared CA.

**Q5: How does Envoy consume SVIDs from SPIRE?**
A: Envoy's SDS (Secret Discovery Service) protocol. SPIRE Agent exposes an SDS server. Envoy sidecar connects to the SPIRE Agent's Unix Domain Socket and requests certificates via SDS. When the SVID is rotated, SPIRE Agent pushes the new certificate to Envoy via SDS -- zero-downtime rotation. No filesystem writes, no pod restart.

**Q6: What are the selectors in SPIRE, and how do they work?**
A: Selectors are the criteria that SPIRE Agent uses to match a workload to a registration entry. For Kubernetes: `k8s:ns:myapp` (namespace), `k8s:sa:myapp-sa` (service account), `k8s:pod-label:app:myapp` (pod label). Multiple selectors are ANDed. When a pod connects to the Workload API, the Agent looks up the pod's attributes via kubelet and matches against registered selectors to determine which SPIFFE ID to issue.

---

### Deep Dive 3: cert-manager and Automated Certificate Lifecycle

**Why it's hard:**
cert-manager must orchestrate certificate lifecycle across thousands of workloads, supporting multiple issuers (Vault, ACME, self-signed), handling failures gracefully (ACME rate limits, Vault downtime), and ensuring renewal happens before expiry. A single missed renewal means a production outage.

| Approach | Automation Level | Kubernetes Integration | Issuer Support |
|----------|-----------------|----------------------|----------------|
| **Manual certificate management** | None | None | Any |
| **cert-manager** | Full (CRD-driven) | Native (Certificate -> k8s Secret) | Vault, ACME, self-signed, CA, Venafi |
| **cert-manager + CSI driver** | Full | CSI volume mount | Same |
| **Custom controller** | Full | Custom | Custom |

**Selected: cert-manager with Vault Issuer (internal) + ACME Issuer (public)**

**Justification:** cert-manager is the de facto standard for Kubernetes certificate management. It watches Certificate CRDs, requests certs from configured Issuers, stores them as k8s Secrets, and auto-renews before expiry. Vault Issuer for internal PKI gives us short-lived certs with centralized policy. ACME Issuer for public certs integrates with Let's Encrypt.

**Implementation Detail:**

```
cert-manager Certificate Lifecycle:
──────────────────────────────────
1. User creates Certificate CRD
   ┌─────────────────────────────────────────┐
   │  Certificate: myapp-tls                  │
   │  secretName: myapp-tls-secret            │
   │  duration: 24h                           │
   │  renewBefore: 8h                         │
   │  issuerRef: vault-infra-issuer           │
   │  dnsNames: [myapp.myapp.svc, myapp]     │
   └──────────────────┬──────────────────────┘
                      │
2. cert-manager creates CertificateRequest
   ┌──────────────────▼──────────────────────┐
   │  CertificateRequest: myapp-tls-xxxxx     │
   │  issuerRef: vault-infra-issuer           │
   │  csr: <generated CSR>                    │
   └──────────────────┬──────────────────────┘
                      │
3. Issuer processes the request
   ┌──────────────────▼──────────────────────┐
   │  ClusterIssuer: vault-infra-issuer       │
   │  → Sends CSR to Vault PKI /sign endpoint│
   │  → Vault signs with intermediate CA      │
   │  → Returns signed certificate + chain    │
   └──────────────────┬──────────────────────┘
                      │
4. cert-manager creates k8s Secret
   ┌──────────────────▼──────────────────────┐
   │  Secret: myapp-tls-secret                │
   │  type: kubernetes.io/tls                  │
   │  data:                                    │
   │    tls.crt: <certificate + chain PEM>    │
   │    tls.key: <private key PEM>            │
   │    ca.crt: <CA bundle PEM>               │
   └──────────────────────────────────────────┘

5. Renewal (automatic):
   - cert-manager checks: is (not_after - renewBefore) reached?
   - For 24h cert with renewBefore=8h: renew at T+16h
   - Creates new CertificateRequest → new cert → updates Secret
   - Pods using the Secret get the new cert via kubelet volume refresh
   
   Timeline for 24h cert, renewBefore=8h:
   ├───────────────────────────────────────────────┤
   T=0h              T=16h (renew)        T=24h (expires)
   Issued             New cert ready        Old cert expires

ACME (Let's Encrypt) Lifecycle:
──────────────────────────────
1. cert-manager creates Order
2. Order creates Challenge (HTTP-01 or DNS-01)
   HTTP-01: cert-manager creates a temporary pod + service to serve the token
   DNS-01: cert-manager creates a TXT record via DNS provider API (Route53, CloudFlare)
3. Let's Encrypt validates the challenge
4. cert-manager downloads the signed certificate
5. Certificate: 90-day validity
6. renewBefore: 30d (renew at day 60)

cert-manager Rate Limit Awareness:
─────────────────────────────────
Let's Encrypt rate limits:
  - 50 certificates per registered domain per week
  - 5 duplicate certificates per week
  - 300 new orders per account per 3 hours

cert-manager handles this by:
  - Reusing existing orders when possible
  - Exponential backoff on rate limit errors
  - Alerting when approaching rate limits
```

**Failure Modes:**
- **Vault issuer unreachable:** cert-manager retries with backoff. Certificate stays valid until not_after. If renewBefore window passes without renewal, alert fires.
- **ACME challenge failure:** DNS propagation delay, HTTP challenge pod not reachable. cert-manager retries. If repeated failure, alert fires 7 days before expiry (failsafe).
- **k8s Secret not mounting in pod:** kubelet refreshes mounted secrets every ~60s by default (`kubeletSyncFrequency`). If the Secret doesn't exist yet, the pod mount waits.
- **cert-manager controller down:** Existing certificates continue working. No renewals happen. Run 2 replicas with leader election. Alert on controller pod unhealthy.

**Interviewer Q&As:**

**Q1: How does cert-manager's renewBefore work, and what is a good default?**
A: `renewBefore` specifies how long before expiry cert-manager should start the renewal process. For a 24h cert, `renewBefore: 8h` means renewal starts at T+16h, giving 8 hours to complete renewal (plenty of time). For a 90-day Let's Encrypt cert, `renewBefore: 30d` means renewal at day 60, per Let's Encrypt's recommendation. The general rule: renewBefore should be at least 3x the expected issuance time.

**Q2: How do you handle cert-manager in an air-gapped environment (no Let's Encrypt)?**
A: Use only Vault PKI Issuer or CA Issuer (self-managed CA). No ACME issuer. For public-facing certs, either use a commercial CA with API integration or manually provision and let cert-manager manage rotation. cert-manager supports importing existing certs.

**Q3: What happens if the private key is compromised for a cert-manager-managed certificate?**
A: (1) Delete the k8s Secret. cert-manager detects the Secret is missing and generates a new private key + CSR. (2) The old certificate is revoked via the platform API. (3) For mass compromise (CA key), use `platform-cert emergency reissue-all`. (4) cert-manager's `privateKey.rotationPolicy: Always` can be set to generate a new key on every renewal.

**Q4: How does cert-manager handle certificate for ingress controllers?**
A: cert-manager can read annotations on Ingress resources: `cert-manager.io/cluster-issuer: letsencrypt-prod`. When it detects a new Ingress with a TLS section, it automatically creates a Certificate CRD, obtains the cert, and stores it in the Secret referenced by the Ingress. The ingress controller (nginx/envoy) picks up the new cert automatically.

**Q5: How do you migrate from self-managed certificates to cert-manager?**
A: (1) Import existing cert and key into a k8s Secret. (2) Create a Certificate CRD pointing to the same Secret name. (3) cert-manager adopts the existing Secret and begins managing its lifecycle. (4) On next renewal, cert-manager generates a new cert from the configured Issuer. The transition is seamless.

**Q6: What is the cert-manager CSI driver, and when would you use it?**
A: cert-manager CSI driver (csi-driver) mounts certificates directly into pods as CSI volumes instead of k8s Secrets. Advantages: private key never stored in etcd (stays in tmpfs), per-pod unique certificates (not shared via Secret). Use when: high-security workloads where etcd exposure of private keys is unacceptable, or when each pod needs a unique identity certificate.

---

### Deep Dive 4: Zero-Downtime Certificate Rotation

**Why it's hard:**
When a certificate is renewed, both the TLS server and all its clients must transition smoothly. The server must present the new cert without dropping connections. Clients must trust both the old and new cert during the transition. For mTLS, both sides must update simultaneously. A failed rotation = an outage.

| Approach | Downtime Risk | Complexity | Scope |
|----------|--------------|-----------|-------|
| **Pod restart with new cert** | Brief (during restart) | Low | Per-pod |
| **Hot reload (file watch)** | Zero | Medium | Per-service |
| **Envoy SDS (Secret Discovery)** | Zero | Low (Envoy handles it) | Service mesh |
| **Dual cert (old + new)** | Zero | High | Manual |

**Selected: Envoy SDS for service mesh + file-watch for non-mesh services**

**Justification:** In the service mesh, Envoy's SDS protocol provides seamless certificate rotation without any application awareness. For services outside the mesh, cert-manager updates the k8s Secret, and the application watches the mounted file for changes (or kubelet refreshes the volume).

**Implementation Detail:**

```
Rotation Flow (Envoy SDS):
──────────────────────────
1. SPIRE Agent issues new SVID (8h before expiry)
2. SPIRE Agent pushes new SVID to Envoy via SDS
3. Envoy performs hot-swap: new cert for new connections
4. Existing connections continue with old cert (connection lifetime)
5. Old cert expires naturally
6. Total zero-downtime: no pod restart, no connection drop

Timeline:
├─────────────────────────────────────────────────┤
T=0h        T=16h             T=20h      T=24h
Cert issued  New cert via SDS  Old conn   Old cert
             New conn uses     drains     expires
             new cert

Rotation Flow (File-watch, non-mesh):
─────────────────────────────────────
1. cert-manager renews cert → updates k8s Secret
2. kubelet detects Secret change → updates mounted volume (~60s)
3. Application detects file change:
   - Java: WatchService on /tls/tls.crt, reload SSLContext
   - Python: inotify watcher, reload ssl.SSLContext
   - Nginx: inotify + reload, or periodic signal (SIGHUP)
   - Go: fsnotify, rebuild tls.Config with new cert
4. New connections use new cert
5. Existing connections drain naturally (or RST after grace period)

Java Hot Reload Example:
───────────────────────
@Component
public class TlsCertificateReloader {
    private volatile SSLContext sslContext;
    
    @PostConstruct
    public void init() throws Exception {
        reloadCertificate();
        WatchService watcher = FileSystems.getDefault().newWatchService();
        Path certDir = Paths.get("/tls");
        certDir.register(watcher, StandardWatchEventKinds.ENTRY_MODIFY);
        
        scheduler.scheduleAtFixedRate(() -> {
            WatchKey key = watcher.poll();
            if (key != null) {
                reloadCertificate();
                key.reset();
            }
        }, 0, 5, TimeUnit.SECONDS);
    }
    
    private void reloadCertificate() {
        // Load new cert + key → create new SSLContext
        // Atomically swap sslContext reference
    }
}
```

**Failure Modes:**
- **Application does not detect file change:** kubelet atomic symlink swap may not trigger inotify in all cases. Mitigation: periodic check (every 30s) in addition to inotify.
- **New cert is invalid:** cert-manager validates the cert before writing to Secret. Application should validate the cert chain before loading. If validation fails, keep the old cert and alert.
- **Client does not trust new cert (CA rotation):** During CA rotation, the trust bundle must include both old and new CA certs. SPIRE handles this with trust bundle updates via Workload API.
- **Connection storm after rotation:** If all pods rotate simultaneously, many TLS handshakes occur. Stagger renewal times by adding jitter to renewBefore.

**Interviewer Q&As:**

**Q1: How do you handle certificate rotation for etcd and the Kubernetes API server?**
A: etcd and kube-apiserver use static certificates (not managed by cert-manager, which runs inside the cluster). A separate automation (Ansible playbook or kubeadm) rotates these certs. kubeadm supports `kubeadm certs renew all`. For zero-downtime, rotate one etcd member at a time (restart after cert update), and the API server supports dynamic cert reloading (since k8s 1.22).

**Q2: What is the difference between certificate rotation and certificate renewal?**
A: Renewal: getting a new certificate before the old one expires (same key or new key). Rotation: the act of replacing the old certificate with the new one in all locations (servers, clients, trust stores). Renewal without rotation = the new cert sits unused and the old cert expires (outage).

**Q3: How do you ensure all clients trust the new certificate?**
A: As long as the new certificate is signed by the same CA (or a CA in the client's trust bundle), no client-side changes are needed. If the CA itself is rotating, you must distribute the new CA cert to all clients' trust bundles BEFORE issuing leaf certs with the new CA. This is called "trust bundle rotation" and must happen before "CA rotation."

**Q4: How do you handle Java applications with JKS keystores?**
A: cert-manager stores certs as PEM in k8s Secrets. For Java apps, we either: (1) Use a sidecar that converts PEM to JKS/PKCS12 on file change. (2) Use Spring Boot's `server.ssl.certificate` and `server.ssl.certificate-private-key` (PEM support since Spring Boot 2.7). (3) Use a Vault Agent template that renders a PKCS12 file directly.

**Q5: How do you stagger certificate renewals to avoid thundering herd?**
A: cert-manager adds jitter to the renewal time. For a certificate with `renewBefore: 8h`, the actual renewal time is `not_after - 8h - random(0, 2h)`. This spreads renewals over a 2-hour window instead of all happening at the same instant. Vault PKI also has built-in rate limiting.

**Q6: What happens if the kubelet takes longer than 60 seconds to refresh the mounted Secret?**
A: The kubelet's `syncFrequency` is configurable (default 1m). For time-critical rotation, set it to 10s. Alternatively, use cert-manager CSI driver (immediately available) or SPIRE Workload API (push-based, instant). For very sensitive applications, consider an in-application Vault client that fetches directly.

---

## 7. Scaling Strategy

**cert-manager scaling:**
- cert-manager controller is a single-leader controller. Scale CPU/memory for the leader.
- For 50,000+ certificates, tune `--max-concurrent-challenges=20` and `--dns01-recursive-nameservers` for ACME.
- cert-manager handles 50K certificate objects well. Beyond 100K, consider sharding by namespace.

**SPIRE scaling:**
- SPIRE Server: 3 replicas with leader election. Scale CPU for signing operations.
- SPIRE Agent: one per node (DaemonSet). Memory scales with number of registered workloads per node.
- For 50K+ SVIDs, use SPIRE's built-in cache and stagger SVID rotation.

**Vault PKI scaling:**
- Vault's PKI engine is CPU-bound (crypto signing). 5 Vault nodes handle 5,000+ signings/sec.
- For higher throughput, use ECDSA (faster than RSA signing).
- Performance standby nodes can serve PKI reads (CA chain, CRL).

**Interviewer Q&As:**

**Q1: What is the bottleneck for certificate issuance at 5,000/sec?**
A: CPU for cryptographic signing. RSA-4096 signing is ~500/sec per core. ECDSA P-256 signing is ~10,000/sec per core. Using ECDSA for leaf certs (intermediate CA can be RSA for compatibility), a single Vault node handles 5,000+/sec. The Raft replication overhead is minimal for read-heavy PKI workloads.

**Q2: How do you handle certificate issuance spikes during deployments?**
A: During a large rolling deployment (10,000 pods restarting), all pods need new certs simultaneously. Pre-mitigation: use pod disruption budgets to limit concurrent restarts. SPIRE Agent caches SVIDs for pods that haven't changed identity. cert-manager batches CertificateRequests. Vault PKI has configurable rate limits.

**Q3: How do you monitor certificate lifecycle at scale?**
A: (1) cert-manager Prometheus metrics: `certmanager_certificate_expiration_timestamp_seconds`, `certmanager_certificate_ready_condition`. (2) Custom exporter that scans all k8s Secrets of type `kubernetes.io/tls` and reports days-to-expiry. (3) Grafana dashboard showing expiry distribution, issuance rate, failure rate. (4) PagerDuty alert: any cert expiring within 7 days without a pending renewal.

**Q4: How do you handle CRL distribution at scale?**
A: With 50K short-lived certs, CRLs grow quickly. Mitigation: (1) Short-lived certs reduce the need for revocation (they expire naturally). (2) Use OCSP stapling instead of CRL for real-time checks. (3) CRL partitioned by CA (each intermediate has its own CRL). (4) CRL served from CDN with 1-hour cache. (5) For internal mTLS with 24h certs, disable CRL checking entirely and rely on TTL.

**Q5: How do you handle certificate management for non-Kubernetes workloads?**
A: Bare-metal services use Vault PKI directly (via Vault Agent or CLI). VMs use SPIRE Agent (non-k8s node attestation: join token or TPM). Legacy services use Ansible-managed certificates with Vault as the CA.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---------|--------|-----------|------------|-----|
| **Vault PKI engine down** | No new cert issuance | Vault health check + cert-manager metrics | Existing certs valid until TTL. cert-manager retries. Alert. | < 5 min (Vault HA) |
| **SPIRE Server down (all replicas)** | No new SVID issuance | SPIRE health check | SPIRE Agent serves cached SVIDs. Existing SVIDs valid until TTL. | < 5 min (restart) |
| **SPIRE Agent crash on node** | Pods on node lose SVID renewal | DaemonSet health | Kubelet restarts Agent. Envoy SDS cache continues. | < 30s |
| **cert-manager controller down** | No cert renewals | Pod health check | Run 2+ replicas (leader election). Existing certs valid. | < 30s |
| **ACME provider outage (Let's Encrypt)** | No public cert renewal | cert-manager challenge failure metric | 30-day renewal buffer. Alert. Switch to backup CA if needed. | Hours (external dependency) |
| **Root CA HSM failure** | Cannot sign new intermediates | HSM health monitoring | Backup HSM. Existing intermediates valid for years. | Hours-days (HSM replacement) |
| **Intermediate CA key compromise** | All certs signed by it are untrustworthy | Security team detection | Emergency re-issuance of all affected certs. CRL published. | < 1 hour (target) |
| **Clock skew (> 1 min)** | Cert validation failures (not yet valid / already expired) | NTP monitoring | Enforce NTP (chrony) on all nodes. Alert on skew > 10s. | < 5 min |
| **Certificate trust bundle not distributed** | Clients reject valid certs (unknown CA) | mTLS failure rate spike | SPIRE auto-distributes bundles. For cert-manager, CA bundle in configmap/secret, mounted to pods. | < 5 min |
| **Mass pod restart (node failure)** | Spike in cert issuance | Issuance rate metric | Rate limiting + SPIRE Agent cache. Stagger restarts via PDB. | Self-healing |
| **etcd corruption (cert-manager data)** | Certificate CRDs lost; renewals stop | etcd health + cert-manager reconciliation | Restore from etcd backup. cert-manager re-creates Certificate CRDs from existing Secrets. | < 30 min |

---

## 9. Security

### Authentication Chain

```
Certificate Issuance Authentication:
─────────────────────────────────────
1. cert-manager → Vault PKI:
   cert-manager authenticates to Vault via Kubernetes auth method
   (dedicated service account + Vault role with PKI sign capability)

2. SPIRE Agent → SPIRE Server:
   Agent authenticates via node attestation
   (Projected SA Token validated by SPIRE Server via TokenReview API)

3. Pod → SPIRE Agent (Workload API):
   Agent verifies pod identity via kubelet API
   (namespace, service account, labels, pod UID)
   No user-presented credentials needed

4. ACME (Let's Encrypt):
   Domain ownership verified via challenge (HTTP-01/DNS-01)
   Account key registered with Let's Encrypt
```

### Authorization Model

```
Who can issue certificates:
───────────────────────────
- cert-manager service account: can sign CSRs via Vault PKI role
  Vault policy: path "pki/infra-intermediate/sign/server-role" { capabilities = ["update"] }
  
- SPIRE Server: can sign SVIDs (has CA key or delegates to Vault PKI)
  Vault policy: path "pki/workload-intermediate/sign/spire-role" { capabilities = ["update"] }
  
- Human admins: can issue certs via CLI for debugging (separate Vault role, audited)
  Vault policy: path "pki/infra-intermediate/issue/admin-role" { capabilities = ["update"] }
  Additional constraint: max_ttl=1h, not renewable

Who can revoke certificates:
────────────────────────────
- Platform security team: full revocation capability
- Service owners: can revoke certs in their namespace
- Automated systems: can revoke on key compromise detection

Certificate scope enforcement:
─────────────────────────────
- Vault PKI role constrains: allowed_domains, max_ttl, key_type, SANs
- Cannot issue certs for domains outside allowed list
- Cannot issue certs with TTL exceeding max_ttl
- Cannot issue certs with wildcard unless explicitly allowed
```

### Audit Trail

- **Every certificate issuance logged:** serial, subject, SANs, issuer, requestor, timestamp.
- **Every revocation logged:** serial, reason, requestor.
- **SPIRE audit log:** every SVID issuance, attestation result.
- **cert-manager events:** Kubernetes events on Certificate CRD (visible via `kubectl describe certificate`).
- **Vault audit log:** every PKI sign/issue operation.

### Threat Model

| Threat | Likelihood | Impact | Mitigation |
|--------|------------|--------|------------|
| **Root CA key compromise** | Very Low (HSM + air-gap) | Catastrophic | HSM with tamper detection. m-of-n key backup. Incident response plan for full PKI re-bootstrap. |
| **Intermediate CA key compromise** | Low | Critical | Vault protects key (never exported). Immediate revocation + re-issuance plan tested quarterly. |
| **Leaf cert private key theft** | Medium | Medium | Short TTL (24h). Key generated per-pod (not shared). cert-manager CSI driver keeps key in tmpfs. |
| **Fake certificate issuance** | Low | High | Vault PKI roles constrain allowed domains/SANs. Certificate Transparency logs (for public certs). |
| **SPIRE impersonation** | Low | High | Node attestation (cannot fake kubelet identity). Workload attestation (cannot fake pod identity without node access). |
| **Man-in-the-middle (no mTLS)** | Medium (without mTLS) | High | Enforce mTLS everywhere via service mesh. NetworkPolicy blocks non-mesh traffic. |
| **CRL/OCSP unavailability** | Medium | Medium | Short-lived certs reduce dependency on revocation. OCSP stapling. Soft-fail vs. hard-fail configurable per service. |
| **Certificate pinning bypass** | Low | Medium | Do not pin leaf certs (they rotate frequently). Pin CA cert instead. HPKP deprecated; use CAA DNS records. |

---

## 10. Incremental Rollout

**Phase 1 (Week 1-2): PKI foundation**
- Set up root CA (HSM ceremony).
- Create intermediate CAs in Vault PKI.
- Deploy cert-manager with Vault ClusterIssuer.
- Issue certs for 5 pilot services.

**Phase 2 (Week 3-4): SPIRE deployment**
- Deploy SPIRE Server (3 replicas) and SPIRE Agent (DaemonSet).
- Register pilot workloads.
- Validate SVID issuance and rotation.

**Phase 3 (Week 5-6): mTLS rollout**
- Enable mTLS for service mesh (permissive mode first -- allow plaintext + mTLS).
- Monitor mTLS adoption rate per namespace.
- Strict mTLS enforcement on pilot namespaces.

**Phase 4 (Week 7-8): Full rollout**
- cert-manager for all namespaces.
- SPIRE registration for all workloads.
- Strict mTLS on all production namespaces.
- ACME issuer for public-facing ingress.

**Phase 5 (Week 9-10): Monitoring and compliance**
- Certificate expiry dashboards.
- Automated compliance reporting (all certs < 24h TTL, all CAs in inventory).
- Emergency re-issuance drill.

**Interviewer Q&As:**

**Q1: How do you migrate services from long-lived certificates to short-lived ones?**
A: (1) Issue a short-lived cert alongside the existing long-lived cert. (2) Update the application to use the new cert (cert-manager managed). (3) Verify renewal works (monitor through at least 2 rotation cycles). (4) Revoke the old long-lived cert. (5) Repeat per service.

**Q2: How do you handle services that cannot reload certificates without a restart?**
A: (1) Use Envoy sidecar to terminate TLS -- Envoy handles rotation via SDS, application is unaware. (2) For non-mesh services, use a TLS proxy (envoy/haproxy) in front of the application. (3) As a last resort, accept a brief restart on rotation (use rolling restart with PDB).

**Q3: What is the rollback plan if mTLS breaks a critical service?**
A: Service mesh permissive mode: `kubectl annotate namespace myns istio-injection=enabled` with `PeerAuthentication` in PERMISSIVE mode. This allows both plaintext and mTLS. If mTLS breaks, traffic falls back to plaintext automatically. Once issues are resolved, switch to STRICT.

**Q4: How do you handle third-party services that don't support short-lived certificates?**
A: Issue them longer-lived certificates (90-day, managed by cert-manager) with a different Vault PKI role. These get standard rotation at 60 days. Document the risk and plan migration to short-lived certs.

**Q5: How do you test the emergency re-issuance procedure?**
A: Quarterly drill: (1) Revoke a test intermediate CA. (2) Trigger emergency re-issuance for all certs signed by that CA in the staging environment. (3) Measure time to full re-issuance. Target: < 1 hour. (4) Document and improve the procedure.

---

## 11. Trade-offs & Decision Log

| Decision | Trade-off | Rationale |
|----------|-----------|-----------|
| **24h TTL for internal certs vs. 90-day** | Higher issuance volume vs. lower exposure window | Short-lived certs are a core zero-trust principle. Vault PKI handles the volume. Reduced dependency on CRL/OCSP. |
| **SPIRE vs. Istio Citadel for workload identity** | Extra component vs. platform-agnostic identity | SPIRE works across k8s and bare metal. Istio Citadel only works within Istio. We have bare-metal services that need identity too. |
| **2-tier PKI vs. 3-tier** | Less defense-in-depth vs. operational simplicity | 3-tier adds a policy CA between root and issuing CA. For our scale, 2-tier with separate intermediates per domain is sufficient. |
| **Vault PKI vs. CFSSL / Smallstep** | Vault dependency vs. integrated secret + cert management | We already run Vault for secrets. Using Vault PKI avoids another CA system and integrates auth/policy. |
| **File-watch rotation vs. pod restart** | Application complexity vs. zero downtime | For service mesh services, Envoy SDS handles it. For others, file-watch is a one-time implementation. Zero downtime justifies the effort. |
| **OCSP stapling vs. CRL-only** | Additional infrastructure vs. real-time revocation | OCSP stapling is more efficient (server staples response) and doesn't expose client access patterns. CRL as fallback. |
| **cert-manager vs. custom controller** | External dependency vs. maintenance burden | cert-manager is the community standard, widely adopted, well-maintained. Building custom would take months and produce an inferior result. |

---

## 12. Agentic AI Integration

### AI-Powered Certificate Intelligence

**1. Certificate Expiry Prediction and Anomaly Detection**
- Train a model on certificate renewal patterns. Predict which certificates are at risk of failing renewal (based on issuer health, past failures, rate limit proximity).
- Alert: "Certificate for api.corp.com has failed renewal 3 times in the last week. ACME rate limit approaching. Manual intervention needed."

**2. Automated Key Compromise Response**
- On detection of a leaked private key (e.g., GitHub secret scanning, Vault audit anomaly):
  - Agent immediately revokes the certificate.
  - Agent triggers cert-manager to re-issue with a new key.
  - Agent verifies the new cert is deployed and old cert is revoked.
  - Agent opens incident ticket with full timeline.

**3. PKI Health Assessment**
- Agent periodically scans the entire certificate inventory and generates a health report:
  - Certificates with TTL > 90 days (should be short-lived).
  - Certificates using RSA-2048 (recommend upgrade to ECDSA P-256).
  - CAs approaching expiry.
  - Trust bundles that are stale.
  - mTLS coverage gaps (namespaces without mTLS enforcement).

**4. Natural Language Certificate Queries**
- "Show me all certificates in the payment namespace that expire in the next 7 days."
- "Which services are still using self-signed certificates?"
- "What is the certificate chain for api.prod.corp.com?"

**5. Certificate Rotation Orchestration**
- Agent orchestrates CA rotation:
  1. Generate new intermediate CA.
  2. Distribute new CA cert to all trust bundles.
  3. Wait for trust bundle propagation (verify via health checks).
  4. Switch issuance to new intermediate.
  5. Monitor for trust failures.
  6. Revoke old intermediate after all old certs expire.

**6. Compliance Reporting**
- Agent generates compliance reports: "100% of production certificates have TTL < 24h. 100% use ECDSA P-256. 0 certificates use SHA-1. Last CA rotation: 2026-01-15."
- Maps against standards: PCI-DSS, SOC2, FedRAMP.

---

## 13. Complete Interviewer Q&A Bank

**Q1: Explain the PKI hierarchy and why you need an offline root CA.**
A: The hierarchy is Root CA -> Intermediate CA -> Leaf certificates. The root CA is the ultimate trust anchor. If its key is compromised, the entire PKI is compromised. By keeping it offline (air-gapped HSM), we eliminate the attack surface. The root is only used to sign intermediates (once every few years). Intermediates are online (in Vault) for high-volume leaf issuance. If an intermediate is compromised, only certs signed by that intermediate are affected -- the root can sign a new intermediate and the PKI continues.

**Q2: What is the ACME protocol, and how does cert-manager use it?**
A: ACME (Automated Certificate Management Environment) is the protocol used by Let's Encrypt. The client proves domain ownership via a challenge (HTTP-01: serve a token on the domain's web server; DNS-01: create a TXT record). cert-manager automates this: it creates the challenge, waits for validation, downloads the cert, and stores it as a k8s Secret. It also handles renewal before expiry.

**Q3: What is SPIFFE, and why is it important for workload identity?**
A: SPIFFE is a standard for identifying workloads in distributed systems. Each workload gets a SPIFFE ID (URI: `spiffe://trust-domain/path`). This identity is cryptographically attested (via X.509 or JWT SVID). Unlike IP-based identity (which changes with pod rescheduling), SPIFFE IDs are stable and verifiable. This enables zero-trust: you trust the identity, not the network location.

**Q4: How does mTLS work, and why is it important?**
A: In standard TLS, only the server presents a certificate. In mTLS (mutual TLS), both client and server present certificates and verify each other. This ensures both parties are authenticated. Important for service-to-service communication: the server knows which client is calling (via the client cert's SPIFFE ID or CN), and the client knows the server is legitimate. This prevents eavesdropping, tampering, and impersonation.

**Q5: What happens if a certificate is revoked but the CRL/OCSP is unavailable?**
A: This is the "soft-fail vs. hard-fail" debate. Soft-fail: if CRL/OCSP is unreachable, accept the certificate (availability over security). Hard-fail: reject the certificate (security over availability). For internal mTLS with 24h certs, we use soft-fail (the cert expires soon anyway). For public-facing endpoints, we use OCSP stapling (server includes OCSP response in TLS handshake, so client doesn't need to reach OCSP separately).

**Q6: How do you handle certificate management for MySQL and Elasticsearch clusters?**
A: MySQL: cert-manager issues server certs for each MySQL node. Clients connect via mTLS with Vault-issued client certs. Elasticsearch: same approach, with node-to-node mTLS certs and separate client certs. Both managed by cert-manager with Vault PKI issuer. Rotation is zero-downtime (MySQL and ES both support hot cert reload).

**Q7: What is certificate transparency, and should you use it for internal certs?**
A: Certificate Transparency (CT) is a public log of all issued certificates. Browsers require CT for publicly trusted certs (to detect misissued certs). For internal certs, CT is not required and would expose internal hostnames. We only submit public-facing certs to CT logs (Let's Encrypt does this automatically). Internal certs stay private.

**Q8: How do you handle certificates for wildcard domains?**
A: Wildcard certs (`*.corp.com`) are convenient (one cert for all subdomains) but risky (compromising the cert exposes all subdomains). We prefer per-service certs for internal use (short-lived, minimal blast radius). For ingress, we allow wildcards for convenience but issue them via cert-manager with ACME DNS-01 challenge (HTTP-01 doesn't support wildcards).

**Q9: What is OCSP stapling, and how does it improve performance?**
A: Normally, a TLS client checks OCSP by contacting the CA's OCSP responder -- adding latency and exposing the client's browsing patterns. With OCSP stapling, the TLS server periodically fetches the OCSP response and "staples" it to the TLS handshake. The client gets the revocation status without making a separate request. Faster and more private.

**Q10: How do you handle CA rotation (replacing an intermediate CA)?**
A: (1) Generate new intermediate CA in Vault PKI. (2) Add the new CA cert to all trust bundles (alongside the old one). (3) Wait for trust bundle distribution (verify). (4) Switch cert-manager issuer to new CA. (5) New certs are signed by new CA. (6) Old certs continue to be trusted (both CAs in trust bundle). (7) After all old certs expire (24h), remove old CA from trust bundle. (8) Revoke old intermediate.

**Q11: What is the difference between X.509 SVIDs and JWT SVIDs in SPIRE?**
A: X.509 SVIDs are X.509 certificates with the SPIFFE ID in the URI SAN. Used for mTLS (long-lived connections, transparent to application). JWT SVIDs are JWTs with the SPIFFE ID as the `sub` claim. Used for API authentication (stateless, single-use, audience-bound). Use X.509 for transport security (mTLS), JWT for application-layer authorization.

**Q12: How do you monitor certificate health across 50,000 pods?**
A: (1) cert-manager exports Prometheus metrics for all managed certificates. (2) SPIRE exports metrics for SVID issuance/rotation. (3) A custom exporter scans all k8s Secrets of type `kubernetes.io/tls` and reports `days_to_expiry`. (4) External probes connect to endpoints and check presented cert expiry. (5) Grafana dashboard shows the distribution of certificate expiry times. (6) PagerDuty alerts if any cert is within 7 days of expiry without pending renewal.

**Q13: How do you handle the bootstrap problem for SPIRE?**
A: SPIRE Server needs to be trusted before it can issue SVIDs. Bootstrap: (1) SPIRE Server's own TLS cert is provisioned during initial deployment (Vault PKI or manual). (2) SPIRE Agent bootstraps via node attestation (k8s PSAT: Agent presents its SA token to Server, Server validates via TokenReview). (3) The SPIRE trust bundle is distributed as a k8s ConfigMap (seeded at install time).

**Q14: What are the implications of using ECDSA vs. RSA for leaf certificates?**
A: ECDSA P-256: faster signing (10x), faster verification (5x), smaller key (32 bytes vs. 256 bytes for RSA-2048), smaller cert. RSA-2048: wider compatibility (legacy systems), slower. For internal mTLS at 5,000 certs/sec, ECDSA is the clear choice. For public-facing certs, ECDSA is broadly supported. Only use RSA if you have legacy Java 7 clients or hardware load balancers that don't support ECDSA.

**Q15: How do you ensure that revoked certificates are not accepted after revocation?**
A: (1) Short-lived certs (24h): even without revocation checking, damage is time-limited. (2) CRL: published by Vault, distributed via CDN, cached by clients. Updated on every revocation. (3) OCSP: real-time check. Vault built-in OCSP responder. (4) OCSP stapling: server-side, no client-side round trip. (5) For service mesh: Envoy can be configured to check OCSP or CRL. (6) For emergency: push a config update to reject specific fingerprints at the Envoy level (fastest path).

**Q16: How do you handle multi-cluster certificate management?**
A: (1) Shared root CA across clusters. (2) Per-cluster intermediate CAs in Vault (separate PKI mounts). (3) SPIRE federation: each cluster's trust domain federated with others. (4) cert-manager in each cluster with local ClusterIssuer pointing to local Vault. (5) Cross-cluster mTLS: both clusters' CAs in each other's trust bundles.

---

## 14. References

- [cert-manager Documentation](https://cert-manager.io/docs/)
- [SPIFFE Specification](https://spiffe.io/docs/latest/spiffe-about/overview/)
- [SPIRE Documentation](https://spiffe.io/docs/latest/spire-about/)
- [Vault PKI Engine](https://developer.hashicorp.com/vault/docs/secrets/pki)
- [Let's Encrypt / ACME Protocol](https://letsencrypt.org/docs/client-options/)
- [RFC 6960 - OCSP](https://datatracker.ietf.org/doc/html/rfc6960)
- [RFC 5280 - X.509 PKI](https://datatracker.ietf.org/doc/html/rfc5280)
- [Envoy SDS (Secret Discovery Service)](https://www.envoyproxy.io/docs/envoy/latest/configuration/security/secret)
- [Kubernetes TLS Certificate Management](https://kubernetes.io/docs/tasks/tls/managing-tls-in-a-cluster/)
- [SPIFFE/SPIRE Production Deployment Guide](https://spiffe.io/docs/latest/deploying/)
