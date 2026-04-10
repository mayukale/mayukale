# System Design: Multi-Region OpenStack Cloud Platform

> **Relevance to role:** Operating a production cloud at enterprise scale requires multi-region deployments for disaster recovery, data sovereignty, and latency reduction. As a cloud infrastructure platform engineer, you must understand how OpenStack regions share identity (Keystone federation), how to design cross-region networking, how to handle data consistency for metadata and images, and how to orchestrate failover — all while keeping control plane latency manageable across geographically distributed data centers.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Users authenticate once (single Keystone) and access resources in any region |
| FR-2 | Each region operates independently: its own Nova, Neutron, Cinder, Glance |
| FR-3 | A global API catalog lists services per region; users select region for resource creation |
| FR-4 | Images (Glance) can be shared across regions (image replication) |
| FR-5 | Cross-region networking: VMs in different regions can communicate over private networks |
| FR-6 | Disaster recovery: workloads can fail over from one region to another |
| FR-7 | Global quotas enforced across regions (prevent a user from consuming all resources in every region) |
| FR-8 | Centralized monitoring and logging across all regions |
| FR-9 | Region-aware orchestration (Heat / Terraform) can deploy multi-region stacks |
| FR-10 | DNS resolves to the nearest region for latency-sensitive workloads |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Intra-region API latency | < 100 ms |
| NFR-2 | Cross-region API latency (Keystone auth) | < 300 ms (depends on physical distance) |
| NFR-3 | Cross-region data plane latency | < 50 ms (US regions); < 150 ms (intercontinental) |
| NFR-4 | Region independence (one region down, others continue) | 100% independent operation |
| NFR-5 | RPO for cross-region DR | < 1 hour for metadata; < 15 min for critical data |
| NFR-6 | RTO for cross-region failover | < 30 minutes |
| NFR-7 | Control plane availability per region | 99.99% |
| NFR-8 | Global control plane availability | 99.95% (accounts for cross-region dependencies) |

### Constraints & Assumptions
- 3-5 regions (e.g., US-East, US-West, EU-West, APAC-Southeast, APAC-Northeast)
- Each region has 2-3 availability zones (separate power/cooling domains within a region)
- Cross-region connectivity via dedicated dark fiber or MPLS circuits (not public internet)
- RTT between US regions: ~30ms; US to EU: ~80ms; US to APAC: ~150ms
- Single Keystone deployment (with federation for partner clouds) or Keystone per region with federation
- OpenStack release: consistent across regions (±1 release for rolling upgrades)

### Out of Scope
- Multi-cloud (AWS/GCP/Azure integration)
- Edge computing / far-edge regions
- Compliance-specific implementations (GDPR data residency enforcement at application level)
- Inter-provider BGP peering details

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Estimate | Calculation |
|--------|----------|-------------|
| Regions | 5 | US-East, US-West, EU-West, APAC-SE, APAC-NE |
| Availability zones per region | 3 | Separate power domains |
| Compute hosts per region | 10,000 | 200 racks x 50 servers |
| Total compute hosts | 50,000 | 5 regions x 10,000 |
| VMs per region | 400,000 | 10K hosts x 40 VMs |
| Total VMs | 2,000,000 | 5 regions x 400K |
| Projects | 10,000 | Some projects span multiple regions |
| API calls per region per second | 2,000 | Nova + Neutron + Cinder + Glance |
| Cross-region API calls per second | 200 | Keystone token validation, image replication, DR sync |
| Keystone auth requests/second (global) | 5,000 | 1,000/region x 5 regions |
| Cross-region data transfer | 10 Gbps per inter-region link | DR replication, image sync, cross-region networking |

### Latency Requirements

| Operation | Target | Notes |
|-----------|--------|-------|
| Keystone token issuance (local) | < 50 ms | Fernet tokens, local validation |
| Keystone token issuance (federated) | < 300 ms | Cross-region SAML/OIDC exchange |
| Nova create VM (local region) | < 60 s | Same as single-region |
| Cross-region image copy | Minutes | 10 GB image / 10 Gbps link = ~8s; plus overhead |
| Cross-region VM failover | < 30 min | Image copy + boot + network reconfiguration |
| Cross-region L3 VPN latency | < RTT + 5ms | VPN encap/decap overhead minimal |
| Global catalog lookup | < 100 ms | Cached locally per region |

### Storage Estimates

| Data | Size Per Region | Total | Calculation |
|------|-----------------|-------|-------------|
| Keystone DB | 500 MB | 500 MB (shared) | 10K projects, 50K users, tokens ephemeral |
| Nova DB (per cell) | 10 GB | 150 GB | 5 regions x 3 cells x 10 GB |
| Neutron DB | 2 GB | 10 GB | 5 regions x 2 GB |
| Glance image storage | 50 TB | 250 TB | 5 regions x 50 TB; replicated images counted once per region |
| Cinder volume storage | 2 PB | 10 PB | 5 regions x 2 PB |
| Cross-region replication (daily delta) | 500 GB | 2.5 TB | Incremental replication |

### Bandwidth Estimates

| Flow | Bandwidth | Calculation |
|------|-----------|-------------|
| Inter-region links (each pair) | 100 Gbps | Provisioned capacity per link |
| Cross-region DR replication | 10 Gbps | 500 GB/day in 8 business hours peak |
| Cross-region networking (tenant VPN) | 20 Gbps | Aggregate across all tenant VPNs |
| Image replication between regions | 5 Gbps | Background sync of popular images |
| Control plane cross-region | 1 Gbps | Keystone, catalog sync, monitoring |
| Intra-region backbone | 10 Tbps | 10K hosts x 1 Gbps avg per host |

---

## 3. High Level Architecture

```
                          ┌──────────────────────────────────┐
                          │         Global Services           │
                          │                                    │
                          │  ┌────────────┐  ┌─────────────┐ │
                          │  │  Keystone   │  │ Global DNS  │ │
                          │  │ (Identity)  │  │ (GSLB)     │ │
                          │  │ [HA Cluster]│  │             │ │
                          │  └─────┬──────┘  └──────┬──────┘ │
                          │        │                 │        │
                          │  ┌─────▼─────────────────▼─────┐ │
                          │  │     Service Catalog          │ │
                          │  │  (endpoints per region)      │ │
                          │  └──────────────────────────────┘ │
                          │  ┌──────────────────────────────┐ │
                          │  │   Global Monitoring           │ │
                          │  │  (Prometheus Federation /    │ │
                          │  │   Thanos)                    │ │
                          │  └──────────────────────────────┘ │
                          └──────────────┬───────────────────┘
                                         │
            ┌────────────────────────────┼────────────────────────────┐
            │                            │                            │
            ▼                            ▼                            ▼
┌───────────────────────┐  ┌───────────────────────┐  ┌───────────────────────┐
│      US-EAST          │  │      EU-WEST          │  │     APAC-SE           │
│   Region              │  │   Region              │  │   Region              │
│                       │  │                       │  │                       │
│ ┌───────────────────┐ │  │ ┌───────────────────┐ │  │ ┌───────────────────┐ │
│ │  Keystone Proxy   │ │  │ │  Keystone Proxy   │ │  │ │  Keystone Proxy   │ │
│ │  (local cache)    │ │  │ │  (local cache)    │ │  │ │  (local cache)    │ │
│ └───────────────────┘ │  │ └───────────────────┘ │  │ └───────────────────┘ │
│                       │  │                       │  │                       │
│ ┌─────┐ ┌─────────┐  │  │ ┌─────┐ ┌─────────┐  │  │ ┌─────┐ ┌─────────┐  │
│ │Nova │ │Neutron  │  │  │ │Nova │ │Neutron  │  │  │ │Nova │ │Neutron  │  │
│ │     │ │         │  │  │ │     │ │         │  │  │ │     │ │         │  │
│ ├─────┤ ├─────────┤  │  │ ├─────┤ ├─────────┤  │  │ ├─────┤ ├─────────┤  │
│ │Cinde│ │Glance   │  │  │ │Cinde│ │Glance   │  │  │ │Cinde│ │Glance   │  │
│ │r    │ │         │  │  │ │r    │ │         │  │  │ │r    │ │         │  │
│ ├─────┤ ├─────────┤  │  │ ├─────┤ ├─────────┤  │  │ ├─────┤ ├─────────┤  │
│ │Heat │ │Barbican │  │  │ │Heat │ │Barbican │  │  │ │Heat │ │Barbican │  │
│ └─────┘ └─────────┘  │  │ └─────┘ └─────────┘  │  │ └─────┘ └─────────┘  │
│                       │  │                       │  │                       │
│ ┌───────────────────┐ │  │ ┌───────────────────┐ │  │ ┌───────────────────┐ │
│ │   AZ-1  AZ-2 AZ-3│ │  │ │   AZ-1  AZ-2 AZ-3│ │  │ │   AZ-1  AZ-2 AZ-3│ │
│ │   [compute hosts] │ │  │ │   [compute hosts] │ │  │ │   [compute hosts] │ │
│ └───────────────────┘ │  │ └───────────────────┘ │  │ └───────────────────┘ │
│                       │  │                       │  │                       │
│ ┌───────────────────┐ │  │ ┌───────────────────┐ │  │ ┌───────────────────┐ │
│ │  Ceph Cluster     │ │  │ │  Ceph Cluster     │ │  │ │  Ceph Cluster     │ │
│ │  (local)          │ │  │ │  (local)          │ │  │ │  (local)          │ │
│ └───────────────────┘ │  │ └───────────────────┘ │  │ └───────────────────┘ │
└───────────┬───────────┘  └───────────┬───────────┘  └───────────┬───────────┘
            │                          │                          │
            └──────────────────────────┼──────────────────────────┘
                                       │
                          ┌────────────▼──────────────┐
                          │   DCI (Data Center         │
                          │   Interconnect)            │
                          │   Dark Fiber / MPLS        │
                          │   BGP Peering              │
                          │   100 Gbps per link pair   │
                          └───────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Global Keystone** | Single identity service for all regions. Issues Fernet tokens valid everywhere. Manages projects, users, roles globally. Deployed as HA cluster in one region with caching proxies in others. |
| **Keystone Proxy (per region)** | Local cache of Keystone tokens and catalog. Validates Fernet tokens locally using synced key repository. Reduces cross-region latency for auth. |
| **Service Catalog** | Part of Keystone. Lists endpoints for every service in every region. Users select region from catalog. Example: `compute` → `us-east: https://nova.us-east.cloud.example.com/v2.1` |
| **Per-Region Services** | Nova, Neutron, Cinder, Glance, Heat, Barbican operate independently per region. Each has its own DB, message queue, and compute hosts. Region failure doesn't affect others. |
| **Glance (per region with sync)** | Each region stores images locally. Popular base images replicated across regions. Glance image import/copy tasks handle replication. |
| **Ceph (per region)** | Block storage and ephemeral storage. Not replicated cross-region (too much data, too much latency). DR uses snapshots and RBD mirror for critical volumes. |
| **DCI (Data Center Interconnect)** | Physical network connecting regions. Dark fiber or MPLS circuits. Carries cross-region API traffic, DR replication, and tenant VPN traffic. |
| **Global DNS (GSLB)** | GeoDNS resolves API endpoints to nearest region. Handles region failover by updating DNS records. |
| **Global Monitoring** | Prometheus federation or Thanos aggregates metrics from all regions. Centralized Grafana dashboards. |

### Data Flows

```
Cross-Region Authentication:
  1. User in EU-West sends API request to EU-West nova-api
  2. nova-api extracts Fernet token from request
  3. Fernet token validated LOCALLY using synced key repository (no Keystone call!)
  4. If token validation fails (key not synced), fallback to Keystone proxy
  5. Keystone proxy checks local cache, then forwards to global Keystone if miss
  6. Global Keystone in US-East validates and returns token info (300ms RTT)

Cross-Region Image Replication:
  1. Admin uploads image to US-East Glance
  2. Glance image import task initiated: glance image-import --import-method copy-image
  3. Image metadata replicated to EU-West Glance DB
  4. Image data transferred over DCI (10 Gbps link)
  5. EU-West Glance stores image in local Ceph cluster
  6. Image available in EU-West for VM creation

Cross-Region DR Failover:
  1. US-East region experiences total failure
  2. Monitoring detects failure; alert to operations team
  3. DR automation triggered:
     a. Cinder RBD mirror promotes replicated volumes in EU-West
     b. Boot-from-volume VMs recreated in EU-West using promoted volumes
     c. Floating IPs re-created in EU-West (new public IPs)
     d. DNS updated: application endpoints point to EU-West
  4. Application resumes in EU-West (new IPs, possibly stale data)
  5. RTO: 15-30 minutes depending on automation level
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Global Keystone Database (shared across regions)

CREATE TABLE keystone_regions (
    id              VARCHAR(255) PRIMARY KEY,     -- 'us-east', 'eu-west', 'apac-se'
    description     TEXT,
    parent_region_id VARCHAR(255),
    extra           TEXT                           -- JSON metadata
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE keystone_services (
    id              VARCHAR(64) PRIMARY KEY,
    type            VARCHAR(255) NOT NULL,         -- 'compute', 'network', 'image', etc.
    enabled         BOOLEAN DEFAULT TRUE,
    extra           TEXT
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE keystone_endpoints (
    id              VARCHAR(64) PRIMARY KEY,
    service_id      VARCHAR(64) NOT NULL,
    region_id       VARCHAR(255),
    interface       ENUM('public', 'internal', 'admin') NOT NULL,
    url             TEXT NOT NULL,
    enabled         BOOLEAN DEFAULT TRUE,
    FOREIGN KEY (service_id) REFERENCES keystone_services(id),
    FOREIGN KEY (region_id) REFERENCES keystone_regions(id),
    UNIQUE KEY (service_id, region_id, interface)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Example endpoint entries:
-- (compute, us-east, public) → https://nova.us-east.cloud.example.com/v2.1
-- (compute, eu-west, public) → https://nova.eu-west.cloud.example.com/v2.1
-- (network, us-east, public) → https://neutron.us-east.cloud.example.com/v2.0
-- (image, us-east, public)   → https://glance.us-east.cloud.example.com/v2

-- Cross-Region Image Replication Tracking

CREATE TABLE image_replication (
    id              VARCHAR(36) PRIMARY KEY,
    image_id        VARCHAR(36) NOT NULL,          -- Glance image UUID
    source_region   VARCHAR(255) NOT NULL,
    dest_region     VARCHAR(255) NOT NULL,
    status          ENUM('pending', 'copying', 'complete', 'error') NOT NULL,
    bytes_copied    BIGINT DEFAULT 0,
    total_bytes     BIGINT NOT NULL,
    started_at      DATETIME,
    completed_at    DATETIME,
    error_message   TEXT,
    created_at      DATETIME NOT NULL,
    INDEX idx_image (image_id),
    INDEX idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Cross-Region DR Configuration

CREATE TABLE dr_protection_groups (
    id                  VARCHAR(36) PRIMARY KEY,
    name                VARCHAR(255) NOT NULL,
    source_region       VARCHAR(255) NOT NULL,
    target_region       VARCHAR(255) NOT NULL,
    protection_type     ENUM('volume_replication', 'image_snapshot', 'full_stack') NOT NULL,
    rpo_minutes         INT NOT NULL,              -- target RPO
    rto_minutes         INT NOT NULL,              -- target RTO
    status              ENUM('active', 'failed_over', 'disabled') DEFAULT 'active',
    last_sync_at        DATETIME,
    created_at          DATETIME NOT NULL,
    updated_at          DATETIME
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE dr_protected_resources (
    id                  VARCHAR(36) PRIMARY KEY,
    protection_group_id VARCHAR(36) NOT NULL,
    resource_type       ENUM('instance', 'volume', 'network', 'floating_ip') NOT NULL,
    source_resource_id  VARCHAR(36) NOT NULL,       -- resource UUID in source region
    target_resource_id  VARCHAR(36),                 -- resource UUID in target region (after failover)
    sync_status         ENUM('synced', 'syncing', 'stale', 'failed') NOT NULL,
    last_synced_at      DATETIME,
    FOREIGN KEY (protection_group_id) REFERENCES dr_protection_groups(id),
    INDEX idx_group (protection_group_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Global Quota Database (optional, for cross-region quota enforcement)

CREATE TABLE global_quotas (
    id              VARCHAR(36) PRIMARY KEY,
    project_id      VARCHAR(255) NOT NULL,
    resource        VARCHAR(255) NOT NULL,          -- 'instances', 'cores', 'ram'
    global_limit    INT NOT NULL,                   -- total across all regions
    UNIQUE KEY (project_id, resource),
    INDEX idx_project (project_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE global_quota_usages (
    id              VARCHAR(36) PRIMARY KEY,
    project_id      VARCHAR(255) NOT NULL,
    resource        VARCHAR(255) NOT NULL,
    region          VARCHAR(255) NOT NULL,
    in_use          INT NOT NULL DEFAULT 0,
    reserved        INT NOT NULL DEFAULT 0,
    updated_at      DATETIME NOT NULL,
    UNIQUE KEY (project_id, resource, region),
    INDEX idx_project_resource (project_id, resource)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Database | Justification |
|----------|--------------|
| **Keystone DB: MySQL Galera (single region, HA)** | Keystone DB is small (~500MB). Deploying in one region with Galera for HA. Other regions validate Fernet tokens locally (no DB needed). |
| **Per-region service DBs: MySQL Galera per region** | Each region's Nova, Neutron, Cinder, Glance have their own Galera clusters. Independent failure domains. |
| **DR/Replication tracking: Dedicated MySQL in operations region** | Small dataset; tracks cross-region replication status. Could also be in the global Keystone DB. |
| **Global Quotas: Keystone or dedicated service** | Cross-region quotas need a central store. Can be in Keystone's DB or a separate microservice. |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| keystone_endpoints | (service_id, region_id, interface) | Lookup endpoint by service + region |
| image_replication | (image_id) | Track all replications of an image |
| dr_protected_resources | (protection_group_id) | List all resources in a DR group |
| global_quota_usages | (project_id, resource) | Aggregate usage across regions |

---

## 5. API Design

### REST Endpoints

```
# Global Keystone (identity)
POST   /v3/auth/tokens                                 # Authenticate (works from any region)
GET    /v3/auth/catalog                                 # Service catalog (all regions)
GET    /v3/regions                                      # List regions
GET    /v3/regions/{region_id}                          # Show region
GET    /v3/endpoints?region_id=us-east                  # List endpoints in a region

# Per-Region Services (standard OpenStack APIs, scoped by region)
# User selects region via: OS_REGION_NAME environment variable or --os-region-name CLI flag
POST   https://nova.us-east.cloud.example.com/v2.1/servers        # Create VM in US-East
POST   https://nova.eu-west.cloud.example.com/v2.1/servers        # Create VM in EU-West

# Cross-Region Image Replication (custom extension or Glance tasks)
POST   /v2/images/{image_id}/import
  Body: {"method": {"name": "copy-image"},
         "stores": ["us-east-store", "eu-west-store"]}             # Copy image to multiple regions

# DR Operations (custom API, not standard OpenStack)
POST   /v1/protection-groups                            # Create DR protection group
GET    /v1/protection-groups                            # List groups
POST   /v1/protection-groups/{id}/failover              # Trigger failover
POST   /v1/protection-groups/{id}/failback              # Trigger failback
GET    /v1/protection-groups/{id}/status                # Check sync status

# Global Quotas (custom API)
GET    /v1/quotas/{project_id}                          # Get global quota
PUT    /v1/quotas/{project_id}                          # Set global quota
GET    /v1/quotas/{project_id}/usage                    # Get usage across regions
```

### CLI (openstack CLI commands)

```bash
# Region selection
export OS_REGION_NAME=us-east
openstack server list                                    # Lists VMs in US-East

export OS_REGION_NAME=eu-west
openstack server list                                    # Lists VMs in EU-West

# Explicit region flag
openstack --os-region-name us-east server list
openstack --os-region-name eu-west server list

# List regions
openstack region list

# List endpoints across regions
openstack endpoint list --service compute

# Cross-region image copy
openstack image set --property os_glance_importing_to_stores=eu-west-store <image-id>
openstack image import --store eu-west-store --import-method copy-image <image-id>

# DR operations (custom CLI or Terraform)
terraform apply -target=module.dr_failover

# Check multi-region status
for region in us-east us-west eu-west apac-se; do
  echo "=== $region ==="
  openstack --os-region-name $region hypervisor stats show
  openstack --os-region-name $region server list --all-projects --status ACTIVE -f value -c ID | wc -l
done
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Keystone Federation and Cross-Region Identity

**Why it's hard:**
All regions need to share identity (users, projects, roles) but the identity service itself must not become a cross-region dependency that takes down all regions if one region's network is partitioned. Fernet tokens must be validated locally (no cross-region calls for every API request), but key rotation must be synchronized across regions. Federation with external IdPs adds additional complexity.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Single global Keystone** | Simplest; single source of truth for users/projects/roles | Cross-region latency for auth; SPOF if Keystone region fails |
| **Keystone per region with federation (SAML/OIDC)** | Each region is independent; survives regional failures | Complex federation setup; user/project sync challenges; token format differences |
| **Single Keystone with local Fernet validation** | Auth validated locally (no cross-region call); Keystone needed only for token issuance | Must sync Fernet keys across regions; initial auth requires cross-region call |
| **Keystone with local caching proxy** | Reduces cross-region calls; cached catalog and token data | Cache staleness; invalidation complexity |

**Selected approach:** Single global Keystone with local Fernet key sync and caching proxies

**Justification:** Fernet tokens are self-contained cryptographic tokens. Any service with the Fernet key repository can validate them locally, eliminating cross-region calls for the common case (API requests with existing tokens). Token issuance (login) requires reaching Keystone, but that's infrequent. Keystone runs in a HA cluster in one "home" region with caching proxies in all regions.

**Implementation detail:**

```
Fernet Key Distribution Architecture:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Keystone Home Region (US-East):
  /etc/keystone/fernet-keys/
    0  ← staged key (will become primary on next rotation)
    1  ← secondary key (validates old tokens)
    2  ← primary key (signs new tokens)

Key Sync Process (every 30 minutes):
  1. Keystone in US-East rotates keys: 0→1, 1→2, new 0 created
  2. Sync agent pushes keys to all regions via secure channel (SCP/Vault)
  3. Each region's Keystone proxy receives updated keys
  4. Keystone middleware in each region's services uses local keys for validation

Token Validation Flow (no cross-region call):
  1. User sends request to eu-west nova-api with Fernet token
  2. keystonemiddleware in nova-api decrypts Fernet token using LOCAL keys
  3. Extracts: user_id, project_id, roles, expiry
  4. Validates: not expired, project exists (cached), roles valid
  5. Proceeds with request — zero cross-region latency!

Token Issuance Flow (requires cross-region call, infrequent):
  1. User in EU-West sends POST /v3/auth/tokens to local Keystone proxy
  2. Proxy forwards to global Keystone in US-East
  3. Keystone authenticates (LDAP/SQL), issues Fernet token
  4. Token returned to user (RTT: ~80ms US-East to EU-West)
  5. User uses token for subsequent requests (validated locally)

Service Catalog Caching:
  - Each region's Keystone proxy caches the service catalog (all regions' endpoints)
  - TTL: 5 minutes (short enough for endpoint updates to propagate)
  - Catalog is small: ~1 KB per region x 10 services x 5 regions = ~50 KB
  - CLI/SDK caches catalog for the session lifetime

Federation with External IdP (optional):
  - Keystone supports SAML 2.0 and OpenID Connect (OIDC)
  - External IdP (Okta, Azure AD) authenticates users
  - Keystone maps external attributes to OpenStack projects/roles
  - Mapping rules: {"remote": [{"type": "group"}], 
                     "local": [{"group": {"name": "engineers"}}]}
  - Enables SSO across OpenStack and other enterprise systems
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Global Keystone down | No new token issuance; existing tokens still validated locally until expiry | Keystone HA cluster (Galera + HAProxy); extended token expiry (4-8 hours) for resilience |
| Fernet key sync failure | Remote regions can't validate tokens issued with new key | Multiple key versions (keep N-3 keys); alert on sync failure; manual key push |
| Keystone proxy in a region crashes | Auth calls go directly to global Keystone (higher latency) | Multiple proxy instances per region; HAProxy failover |
| Network partition isolating a region | No new tokens; existing tokens valid until expiry; no catalog updates | Extended token lifetime; local service catalog cache; graceful degradation |
| Fernet key compromise | All tokens can be forged | Immediate key rotation; revoke all tokens; re-authenticate all users |

**Interviewer Q&As:**

**Q1: How do you handle token expiry during a regional network partition?**
A: Fernet tokens are validated locally using cryptographic keys — no network call to Keystone is needed. During a partition, existing tokens remain valid until their expiry time. We set token expiry to 4-8 hours (vs default 1 hour) to give more runway during partitions. If the partition lasts longer, users can't get new tokens but existing sessions continue. Services cache the token validation result (keystonemiddleware cache), reducing the frequency of re-validation. When the partition heals, users re-authenticate normally.

**Q2: How do you prevent a compromised region from affecting other regions?**
A: Each region's services only have Fernet validation keys (not Keystone admin credentials). A compromised region can't create new users, modify roles, or issue tokens — it can only validate existing ones. The Fernet keys are the most sensitive asset — they must be protected with encryption at rest and TLS in transit. If a region is compromised, rotate Fernet keys immediately (invalidating all tokens), revoke the compromised region's service credentials, and audit all activity. The other regions continue operating with the new keys after sync.

**Q3: How do you manage project quotas across regions?**
A: Two approaches: (1) **Per-region quotas** (simple): each region has independent quotas. A project might have 100 VMs in US-East and 50 in EU-West, with no cross-region enforcement. Easy to implement but users can consume more total than intended. (2) **Global quotas** (complex): a central quota service tracks usage across all regions. Each region reports usage periodically. Before creating a resource, the service checks global usage. Challenges: latency (cross-region check per create), availability (central service is a dependency), consistency (usage reporting lag). Recommendation: start with per-region quotas; add global quotas for high-value resources (GPUs, large flavors).

**Q4: How does Keystone federation work with an external SAML identity provider?**
A: (1) Configure Keystone as a SAML Service Provider (SP) with Apache mod_shib. (2) Exchange metadata with the Identity Provider (IdP) — e.g., corporate Active Directory via ADFS. (3) Configure mapping rules in Keystone: map SAML attributes (groups, department) to OpenStack roles and projects. (4) User authenticates: browser redirects to IdP → IdP authenticates → returns SAML assertion → Keystone validates assertion → maps to local identity → issues Fernet token. (5) Subsequent API calls use the Fernet token normally. This enables SSO and eliminates the need to manage user passwords in Keystone.

**Q5: What is the blast radius if the Keystone home region fails completely?**
A: All existing tokens continue to work in all regions (Fernet local validation). Users who need new tokens (expired token, new login) can't authenticate — this is the main impact. Mitigation: (1) Keystone HA cluster spans 3 AZs within the home region — survives AZ failure. (2) If the entire region fails, Keystone can be promoted in another region using DB backup (RPO: last backup). (3) Extended token lifetimes (8 hours) give 8 hours of continued operation. (4) Consider active-active Keystone in 2 regions with Galera cross-region replication (adds complexity but reduces blast radius).

**Q6: How do you handle service endpoint updates when a region is added or removed?**
A: (1) Adding a region: register endpoints in Keystone for the new region: `openstack endpoint create --region apac-ne compute public https://nova.apac-ne.cloud.example.com/v2.1`. Repeat for each service. The catalog caches refresh automatically (5-min TTL). CLI users see the new region after re-authentication. (2) Removing a region: delete endpoints in Keystone. Delete the region. Ensure no resources reference the deleted region. Catalog caches will stop listing the region after TTL expiry. Terraform/Heat templates referencing the region must be updated.

---

### Deep Dive 2: Cross-Region Networking

**Why it's hard:**
Tenants need their VMs in different regions to communicate privately (same L2 or L3 network). This requires tunneling across the DCI (Data Center Interconnect), handling different VXLAN VNI spaces per region, routing between regions, and maintaining security (encrypted tunnels). The control plane must coordinate network configuration across independently managed Neutron deployments.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **VPN as a Service (VPNaaS per region)** | IPsec tunnels between tenant routers in each region; encrypted; standard | Per-tenant setup; limited bandwidth; complex management |
| **BGP EVPN DCI** | Extends VXLAN across regions using BGP EVPN control plane; transparent L2/L3 | Requires BGP routers at each DC; operator-managed; not per-tenant self-service |
| **Neutron interconnection (Tricircle/Networking-L2GW)** | OpenStack-native; extends Neutron networks across regions | Experimental; limited community support; complex |
| **Overlay-in-overlay (VXLAN over IPsec/WireGuard)** | VXLAN tunnels over encrypted DCI; per-tenant isolation | Double encapsulation overhead; MTU challenges |
| **Application-layer VPN (WireGuard/OpenVPN per tenant)** | Tenant-managed; flexible | Not transparent; requires config in VMs |

**Selected approach:** BGP EVPN DCI for infrastructure-level connectivity + VPNaaS for tenant self-service

**Justification:** BGP EVPN provides operator-managed cross-region L3 connectivity for infrastructure needs (shared services, management). VPNaaS (IPsec) allows tenants to create their own cross-region tunnels on demand. This two-layer approach separates infrastructure and tenant concerns.

**Implementation detail:**

```
BGP EVPN DCI Architecture:
━━━━━━━━━━━━━━━━━━━━━━━━━

US-East                                    EU-West
┌─────────────────────┐                    ┌─────────────────────┐
│  Spine Switches      │                    │  Spine Switches      │
│  (BGP Route          │◄──── eBGP ────────►│  (BGP Route          │
│   Reflectors)        │   over DCI          │   Reflectors)        │
└──────────┬──────────┘                    └──────────┬──────────┘
           │ iBGP EVPN                                │ iBGP EVPN
           ▼                                          ▼
┌─────────────────────┐                    ┌─────────────────────┐
│  Leaf Switches       │                    │  Leaf Switches       │
│  (VXLAN VTEPs)       │                    │  (VXLAN VTEPs)       │
└──────────┬──────────┘                    └──────────┬──────────┘
           │                                          │
     ┌─────┴─────┐                              ┌─────┴─────┐
     │Compute    │                              │Compute    │
     │Hosts      │                              │Hosts      │
     └───────────┘                              └───────────┘

Cross-Region L3 Routing:
  - Each region has its own VNI space (can overlap)
  - DCI routers perform VNI translation at region boundaries
  - Or: use globally unique VNIs (partition VNI space per region)
  - BGP EVPN Type-5 routes advertise IP prefixes across regions
  - Type-2 routes (MAC/IP) for L2 extension (use sparingly — blast radius)

Tenant VPNaaS Flow:
  1. Tenant creates VPN service in US-East:
     openstack vpn service create --router us-east-router vpn-us-east
  
  2. Tenant creates VPN service in EU-West:
     openstack vpn service create --router eu-west-router vpn-eu-west
  
  3. Create IPsec site connections:
     # In US-East:
     openstack vpn ipsec site connection create \
       --vpnservice vpn-us-east \
       --ikepolicy ike-policy \
       --ipsecpolicy ipsec-policy \
       --peer-address <eu-west-router-external-ip> \
       --peer-cidr 10.0.2.0/24 \            # EU-West subnet
       --psk "shared-secret" \
       conn-to-eu-west
     
     # In EU-West (mirror configuration):
     openstack vpn ipsec site connection create \
       --vpnservice vpn-eu-west \
       --ikepolicy ike-policy \
       --ipsecpolicy ipsec-policy \
       --peer-address <us-east-router-external-ip> \
       --peer-cidr 10.0.1.0/24 \            # US-East subnet
       --psk "shared-secret" \
       conn-to-us-east

  4. IPsec tunnel established between router namespaces
  5. VM in US-East (10.0.1.5) can now reach VM in EU-West (10.0.2.10)
  6. Traffic encrypted with ESP (AES-256-GCM)
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| DCI link failure | All cross-region traffic fails | Redundant DCI links (2+ diverse paths); BGP reconverges to alternate path |
| BGP session failure between regions | Route withdrawal; cross-region subnets unreachable | BFD for fast detection (~1s); multiple BGP sessions; route damping |
| IPsec tunnel failure (VPNaaS) | Tenant cross-region traffic fails | IKE DPD (Dead Peer Detection); automatic re-keying; multiple tunnel endpoints |
| VNI collision across regions | Traffic mismatch; data leak between tenants | Globally unique VNI allocation or VNI translation at DCI boundary |
| DCI congestion | High latency, packet loss | QoS prioritization on DCI; separate queues for control plane vs data plane |

**Interviewer Q&As:**

**Q1: How do you ensure tenant network isolation across regions?**
A: Each tenant's cross-region traffic is isolated by: (1) VPNaaS: IPsec tunnel per tenant with unique security associations (SA). Traffic encrypted and authenticated — can't be spoofed. (2) BGP EVPN: VNI-based isolation extends to the DCI. Each tenant network has a unique VNI (or VRF for L3). DCI routers enforce VRF isolation. (3) No direct L2 extension across regions by default (reduces blast radius). L3 routing only, with explicit firewall rules at region boundaries.

**Q2: What is the bandwidth overhead of IPsec encryption on cross-region traffic?**
A: IPsec ESP with AES-256-GCM adds ~50-70 bytes per packet (ESP header, IV, ICV, padding). For small packets (e.g., 64-byte), overhead is significant (~80%). For typical VM traffic (1400-byte packets), overhead is ~4%. CPU overhead: with AES-NI hardware acceleration (standard on modern CPUs), a single core handles ~10 Gbps of IPsec. For higher throughput, use NIC IPsec offload (Intel QAT, NVIDIA ConnectX). Alternatively, the DCI link itself can be encrypted at Layer 1 (MACsec) or Layer 2, eliminating per-tunnel CPU overhead.

**Q3: How do you handle DNS resolution across regions?**
A: Three-level DNS: (1) **Global DNS (GSLB)**: resolves public-facing services to the nearest healthy region. Uses GeoDNS (Akamai, Cloudflare, or self-hosted PowerDNS + lua-records). (2) **Per-region internal DNS**: Designate (OpenStack DNS) provides DNS for tenant networks within a region. VMs resolve internal names. (3) **Cross-region DNS forwarding**: tenant-specific DNS zone (e.g., myproject.internal) with records in each region. DNS forwarder routes queries to the appropriate region. Alternatively, a global internal DNS zone with records for all regions.

**Q4: How do you select which region to place a workload in?**
A: Selection criteria: (1) **Latency**: place close to users (GeoDNS-measured). (2) **Data sovereignty**: legal requirements dictate region (e.g., EU data in EU-West). (3) **Capacity**: region with most available resources. (4) **Cost**: regions may have different hardware costs (power, cooling, land). (5) **Redundancy**: spread across regions for HA. Implementation: application-level (user chooses via `--os-region-name`), or orchestration-level (Heat/Terraform templates specify region per resource). No automatic cross-region scheduling in OpenStack — region is always explicit.

**Q5: How do you handle a split-brain scenario where both regions think they're primary?**
A: Split-brain prevention: (1) DCI link monitoring via BFD at both ends — if DCI is down but both regions are up, neither attempts to take over the other's workload. (2) DR failover requires explicit human approval (not automatic) to prevent false positives. (3) If automated, use a third "witness" site (cloud-based or a lightweight monitoring site) to confirm region failure. (4) Fencing: the failed region's resources are not accessed until confirmed down (like STONITH for hosts). (5) After partition heals, reconciliation: compare timestamps, resolve conflicts, merge state.

**Q6: How do you estimate the DCI bandwidth needed?**
A: Calculation: (1) DR replication: volume snapshots + DB backups. If protecting 100 TB of volumes with 1% daily change rate, that's 1 TB/day = ~100 Mbps sustained. (2) Image replication: 10 new/updated images per day x 10 GB = 100 GB/day = ~10 Mbps. (3) Cross-region tenant VPN: estimate 10% of total tenant traffic is cross-region. If total is 10 Tbps, cross-region is 1 Tbps — but this is usually much lower; realistic estimate: 10-50 Gbps per DCI link. (4) Control plane: < 1 Gbps (Keystone, monitoring). Total: provision 100 Gbps per DCI link pair with 50% headroom = 200 Gbps provisioned.

---

### Deep Dive 3: Cross-Region Disaster Recovery

**Why it's hard:**
DR requires replicating data (volumes, images, databases) across regions with bounded RPO, orchestrating failover that recreates infrastructure in the DR region within bounded RTO, handling network re-addressing (floating IPs are region-specific), and ensuring application consistency during failover. OpenStack doesn't have a built-in DR orchestrator — it must be built on top.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Volume replication (Ceph RBD mirror)** | Block-level replication; RPO < 1 min with journaling; standard Ceph feature | Only covers block storage; compute/network must be recreated; Ceph-specific |
| **VM snapshot and copy** | Full VM image replicated; easy to understand | RPO = snapshot interval (hours); large data transfer; slow RTO |
| **Application-level replication** | Best RPO (near-zero); application-aware | Requires app changes; not generic for all workloads |
| **Heat/Terraform stack redeployment** | Infrastructure as Code; idempotent; recreates full stack | Doesn't preserve data (only infra); needs data replication separately |
| **Kolla-ansible / TripleO stack replication** | Full OpenStack stack replicated | Overkill for tenant DR; more for control plane DR |

**Selected approach:** Ceph RBD mirror for volume replication + Terraform for infrastructure recreation + custom DR orchestrator

**Justification:** Ceph RBD mirror provides near-real-time block replication (RPO < 1 min). Terraform stack defines the infrastructure (networks, routers, security groups, instances). The custom DR orchestrator coordinates: promote mirrored volumes, apply Terraform, update DNS. This covers both data and infrastructure recovery.

**Implementation detail:**

```
DR Architecture:
━━━━━━━━━━━━━━━

Primary (US-East)                          DR (EU-West)
┌──────────────────────┐                   ┌──────────────────────┐
│ VM: app-server-1     │                   │                      │
│  ├── /dev/vda (boot) │                   │ (standby — no VMs    │
│  │   Ceph RBD vol-01 │──── rbd mirror ──►│ until failover)      │
│  └── /dev/vdb (data) │                   │                      │
│      Ceph RBD vol-02 │──── rbd mirror ──►│ Ceph stores mirrored │
│                      │                   │ images of vol-01,    │
│ Ceph Cluster (primary)│                   │ vol-02               │
│ Pool: volumes         │                   │                      │
│ rbd-mirror daemon    │◄─── journaling ──►│ rbd-mirror daemon    │
│ (pushes changes)     │                   │ (receives changes)   │
└──────────────────────┘                   └──────────────────────┘

RBD Mirror Configuration:
  # On primary (US-East):
  ceph osd pool set volumes rbd_mirroring_mode image
  rbd mirror image enable volumes/vol-01 journaling
  rbd mirror image enable volumes/vol-02 journaling
  
  # On DR (EU-West):
  ceph osd pool set volumes rbd_mirroring_mode image
  rbd-mirror -d  # daemon that receives and replays journal entries

  # Replication lag monitoring:
  rbd mirror image status volumes/vol-01
  # Output shows: state: replaying, last_sync: 2s ago

Failover Procedure (automated via DR orchestrator):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Step 1: Detect Primary Failure (2 min)
  - Monitoring detects US-East control plane unreachable
  - Cross-check from multiple monitoring locations (avoid false positive)
  - Alert to operations team; auto-failover if configured

Step 2: Promote Mirrored Volumes (5 min)
  # On EU-West Ceph:
  rbd mirror image promote volumes/vol-01 --force
  rbd mirror image promote volumes/vol-02 --force
  # --force because primary is unreachable (can't demote cleanly)
  # Last few seconds of writes may be lost (RPO = journal replication lag)

Step 3: Recreate Infrastructure (10 min)
  # Terraform apply in EU-West:
  terraform apply -var="region=eu-west" -var="dr_mode=true"
  
  # Terraform creates:
  - Networks and subnets (same CIDRs as primary)
  - Routers and floating IPs (NEW external IPs — different from primary)
  - Security groups
  - Instances (boot from promoted volumes)
  - Load balancers

Step 4: Attach Promoted Volumes (5 min)
  # For each protected instance:
  openstack --os-region-name eu-west server create \
    --flavor m1.large \
    --boot-from-volume vol-01 \
    --network private-net \
    --security-group app-sg \
    app-server-1-dr

Step 5: Update DNS (2 min)
  # GSLB health check detects US-East down
  # Automatic DNS failover to EU-West
  # Or manual: update DNS records for app.example.com → EU-West floating IPs

Step 6: Verify and Notify (5 min)
  # Run health checks on DR instances
  # Notify operations team of successful failover
  # Update monitoring to track DR region as primary

Total RTO: ~25 minutes (automated)

Failback Procedure (after primary recovery):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. Repair US-East infrastructure
2. Re-establish RBD mirror from EU-West (now primary) to US-East
3. Wait for full sync (may take hours for large volumes)
4. Planned failover (clean):
   a. Stop application in EU-West
   b. Demote volumes in EU-West
   c. Promote volumes in US-East
   d. Recreate infrastructure in US-East
   e. Update DNS back to US-East
5. Re-establish normal direction mirror (US-East → EU-West)
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| RBD mirror replication lag > RPO | Data loss during failover exceeds target | Monitor replication lag; alert on RPO breach; tune journal settings |
| Promoted volume has corrupt data | DR instance boots with inconsistent filesystem | Application-level recovery (fsck, DB recovery); test DR regularly |
| DR region lacks capacity for full failover | Not all VMs can be recreated | Reserve capacity in DR region; or prioritize critical workloads |
| DNS failover too slow | Users still directed to failed region | Low TTL on DNS records (30-60s); use health-check-based GSLB |
| Failover triggered by false positive | Unnecessary disruption; split-brain risk | Require multi-point confirmation; manual approval for failover |
| Network addresses change after failover | Applications with hardcoded IPs break | Use DNS names, not IPs; or use same private subnet CIDRs in both regions |

**Interviewer Q&As:**

**Q1: How do you achieve RPO < 1 minute for cross-region DR?**
A: Ceph RBD mirror with journaling mode provides near-synchronous replication. Every write to the primary volume is journaled and replayed on the DR site. The replication lag depends on DCI bandwidth and I/O rate. For typical workloads (100 MB/s write rate per volume), with a 10 Gbps DCI link, lag is < 1 second. For burst writes, lag may temporarily increase. Monitor with `rbd mirror image status` — if lag exceeds RPO, alert. Alternative: synchronous replication (Ceph stretch clusters) for RPO=0, but requires low-latency DCI (< 10ms RTT) and halves write performance.

**Q2: Why not use synchronous replication for zero RPO?**
A: Synchronous replication (every write acknowledged by both regions before returning to the VM) provides RPO=0 but: (1) Write latency increases by RTT (US-East to EU-West: +80ms per write). For database workloads doing 10,000 writes/sec, this is unacceptable. (2) DCI link failure halts all writes (availability trade-off). (3) Ceph stretch clusters require < 10ms RTT between sites. Synchronous replication is viable for metro-distance DR (same city, < 5ms RTT) but not for geo-distance. For geo-DR, asynchronous replication with RPO < 1 min is the practical choice.

**Q3: How do you test DR without impacting production?**
A: (1) **Non-disruptive DR test**: clone the mirrored volumes in the DR region (Ceph snapshot → clone). Boot test VMs from clones. Verify application functionality. Delete test VMs. Primary is unaffected. (2) **Game day**: schedule a full failover test quarterly. Announce maintenance window. Execute full failover procedure. Measure actual RTO. Fail back. Document issues. (3) **Chaos engineering**: randomly inject failures (kill RBD mirror daemon, simulate DCI failure) and verify alerting and recovery procedures. (4) **Automated DR testing**: nightly job creates a clone, boots a VM, runs a health check, reports pass/fail.

**Q4: How do you handle applications that use floating IPs if the IPs change after failover?**
A: Floating IPs are region-specific (from the region's external subnet) — they can't be preserved across regions. Solutions: (1) **DNS**: applications use DNS names, not IPs. GSLB updates DNS to point to DR region's IPs. TTL of 30-60s ensures fast convergence. (2) **Anycast**: if using BGP anycast, the same IP can be advertised from multiple regions — DR region starts advertising the IP. (3) **Elastic IP service** (custom): maintain a pool of IPs that can be "moved" between regions via BGP advertisement. (4) For internal services: use same private subnet CIDRs in both regions — VMs get the same private IPs, only external IPs change.

**Q5: What is the cost of maintaining a DR region?**
A: Costs include: (1) **Compute**: if DR region has hot standby VMs, cost is 100% of primary. If cold standby (VMs created on failover), compute cost is near zero during normal operation. Recommended: cold standby for most workloads, warm standby for critical services. (2) **Storage**: Ceph in DR region must match primary capacity for replicated volumes. ~100% of primary storage cost for protected data. (3) **DCI**: dark fiber/MPLS circuit cost (fixed cost regardless of usage). (4) **Control plane**: full OpenStack deployment in DR region (always running). (5) **Reserved capacity**: must ensure DR region has enough compute capacity to absorb failover load. Total: typically 30-50% of primary region cost for cold standby DR.

**Q6: How do you prioritize which workloads get DR protection?**
A: Tiered approach: (1) **Tier 1** (RPO < 1 min, RTO < 15 min): business-critical databases and services. RBD mirror with journaling. Hot standby in DR. 10% of workloads, 70% of DR cost. (2) **Tier 2** (RPO < 1 hour, RTO < 1 hour): important services. RBD mirror with snapshot-based replication (hourly). Cold standby. 30% of workloads. (3) **Tier 3** (RPO < 24 hours, RTO < 4 hours): non-critical services. Daily volume snapshots copied to DR. Rebuilt from Terraform on failover. 60% of workloads. Classification based on business impact analysis (BIA) with application owners.

---

## 7. Scheduling & Resource Management

### Cross-Region Resource Management

```
Region Capacity Dashboard:
━━━━━━━━━━━━━━━━━━━━━━━━━

Region       | VCPUs (used/total) | RAM GB (used/total) | VMs     | Util %
-------------|--------------------|--------------------|---------|--------
US-East      | 800K / 1.28M       | 1.2 PB / 1.6 PB    | 380K    | 62%
US-West      | 600K / 1.28M       | 0.9 PB / 1.6 PB    | 290K    | 47%
EU-West      | 700K / 1.28M       | 1.1 PB / 1.6 PB    | 350K    | 55%
APAC-SE      | 400K / 640K        | 0.6 PB / 0.8 PB    | 200K    | 62%
APAC-NE      | 350K / 640K        | 0.5 PB / 0.8 PB    | 180K    | 55%
Total        | 2.85M / 5.12M      | 4.3 PB / 6.4 PB    | 1.4M    | 56%

Capacity Planning Rules:
  - Maintain 30% headroom per region for burst and DR failover
  - APAC regions receive failover from each other (APAC-SE ↔ APAC-NE)
  - US regions receive failover from each other
  - EU-West fails over to US-East (GDPR consideration: data stays in EU for normal ops)
  
Region Selection Logic (for orchestration):
  1. Check data sovereignty requirements → restrict to allowed regions
  2. Check latency requirements → prefer nearest region to users
  3. Check capacity → prefer regions with > 40% free capacity
  4. Check cost → prefer cheaper regions (off-peak, lower power cost)
  5. Check DR requirements → ensure DR region has capacity
```

### Availability Zones vs Regions

```
Hierarchy:
  Region (us-east)
    ├── AZ-1 (us-east-1a) ← separate building/power
    │   ├── Cell 1 (3,000 hosts)
    │   └── Cell 2 (3,000 hosts)
    ├── AZ-2 (us-east-1b) ← separate building/power
    │   └── Cell 3 (3,000 hosts)
    └── AZ-3 (us-east-1c) ← separate building/power
        └── Cell 4 (1,000 hosts)

Key Differences:
  Region:
    - Geographically separate data center
    - Own control plane (Nova, Neutron, Cinder)
    - High latency between regions (30-150ms)
    - No shared L2 network between regions
    - Different external IP ranges
    
  Availability Zone:
    - Same data center campus / metro area
    - Shared control plane (same Nova, Neutron)
    - Low latency between AZs (< 2ms)
    - Shared L2/L3 network (same Neutron)
    - Same external IP ranges
    - Separate power, cooling, network uplinks
```

---

## 8. Scaling Strategy

### Multi-Region Scaling Dimensions

| Dimension | Strategy | Notes |
|-----------|----------|-------|
| Adding a new region | Deploy full OpenStack stack; register in Keystone; establish DCI links | 2-4 weeks for infrastructure; 1 week for OpenStack deployment |
| Scaling within a region | Add cells, compute hosts, Ceph OSDs | Standard single-region scaling |
| Cross-region traffic growth | Upgrade DCI links (100G → 400G); add more link pairs | Physical infrastructure lead time: 3-6 months |
| Global Keystone scaling | Add caching proxies; increase token lifetime; optimize DB | Keystone DB is small; bottleneck is auth endpoint throughput |
| Image replication scaling | Parallel copy tasks; CDN for popular images; incremental delta sync | Bottleneck: DCI bandwidth |
| DR replication scaling | Dedicate DCI bandwidth for DR; prioritize Tier 1; batch Tier 3 | Must not saturate DCI links |

### Interviewer Q&As

**Q1: How do you add a new region to an existing multi-region deployment?**
A: (1) Physical: provision data center, deploy servers, networking, storage. (2) OpenStack: deploy all services (Nova, Neutron, Cinder, Glance, etc.) using deployment automation (Kolla-ansible, TripleO, or custom). (3) Keystone: register region and endpoints: `openstack region create apac-ne; openstack endpoint create --region apac-ne compute public ...`. (4) DCI: establish network links to existing regions; configure BGP peering. (5) Fernet keys: sync to new region. (6) Monitoring: add new region to Prometheus federation, Grafana dashboards. (7) DR: configure replication relationships. (8) Testing: run Tempest tests in new region; test cross-region connectivity. Total timeline: 4-8 weeks from hardware readiness to production.

**Q2: How do you handle version skew between regions during a rolling upgrade?**
A: Regions are upgraded one at a time. During the transition, some regions run version N and others N+1. Impact: (1) Keystone API: v3 is stable; no issues. (2) Service APIs: microversioning handles this — clients negotiate the max version supported by the target region. (3) Image format: ensure image formats are compatible across versions. (4) Terraform/Heat: templates must be compatible with both versions. Strategy: upgrade the least critical region first (e.g., APAC-NE), run for 1 week with monitoring, then proceed. Total upgrade across 5 regions: 5-8 weeks.

**Q3: How do you handle a scenario where one region is consistently over capacity?**
A: (1) Short-term: redirect new workloads to other regions (quota adjustment, user communication). (2) Medium-term: add compute capacity within the region (new cells, new racks). (3) Long-term: build a new region in the same geographic area (e.g., US-East-2). (4) Optimization: identify idle VMs (low CPU/memory utilization) and shelve/delete them. Use right-sizing recommendations to reduce waste. (5) Burst to another region: for latency-insensitive workloads, provide tooling to migrate to a region with capacity.

**Q4: How do you ensure consistent deployment across all regions?**
A: (1) Infrastructure as Code: all OpenStack configurations, flavors, images, networks defined in Git (Ansible, Terraform). (2) CI/CD pipeline: changes tested in staging, then deployed to all regions via automation. (3) Configuration management: Kolla-ansible or Puppet/Chef ensures consistent nova.conf, neutron.conf across regions. (4) Compliance scanning: automated checks that all regions have same security groups, same image versions, same service configuration. (5) Drift detection: periodic comparison of actual vs desired state; alert on drift.

**Q5: What is the latency impact of running Keystone in only one region?**
A: Token issuance (login) requires a round-trip to the Keystone home region. For a user in APAC-SE logging into Keystone in US-East, that's ~200ms RTT. This happens once per session (or every few hours when token expires). All subsequent API calls validate the Fernet token locally (< 1ms). The service catalog is cached by the SDK/CLI (no repeated fetches). Impact: initial login is slightly slower for remote regions, but 99%+ of API operations are unaffected. If 200ms login latency is unacceptable, deploy active-active Keystone in 2 regions with Galera cross-region replication.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Impact | Detection | Recovery | RTO |
|---|---------|--------|-----------|----------|-----|
| 1 | Single region total failure | All VMs in that region down | External monitoring; GSLB health check | DR failover to paired region | 15-30 min |
| 2 | DCI link failure between two regions | Cross-region traffic between those regions fails | BGP session down; BFD timeout | Alternate DCI path (if available); or traffic rerouted via third region | < 5 min (BGP reconverge) |
| 3 | Keystone home region network partition | Remote regions can't issue new tokens; existing tokens valid | Token issuance failures in remote regions | Extended token lifetime buys time; promote standby Keystone if > 8 hours | N/A (graceful degradation) |
| 4 | RBD mirror replication lag exceeds RPO | Potential data loss if failover occurs now | Monitor mirror status; alert on RPO breach | Investigate cause (DCI congestion, source I/O spike); increase DCI bandwidth | N/A (no outage) |
| 5 | DNS GSLB failure | Users can't resolve API endpoints | DNS monitoring (external checks) | Secondary DNS provider; static DNS entries in client /etc/hosts as last resort | < 5 min (DNS TTL) |
| 6 | Cross-region image replication failure | DR region missing latest images; VM boot may fail | Image replication monitoring | Retry replication; ensure base images always replicated; fall back to older image version | Minutes |
| 7 | Single AZ failure within a region | VMs in that AZ down; other AZs unaffected | Compute host heartbeats; AZ health check | Evacuate VMs to other AZs (if capacity); anti-affinity ensures distributed workloads | 5-15 min |
| 8 | Galera cluster failure in a region | All API operations in that region fail | Galera monitoring; API error rate | Bootstrap cluster; restore from backup if needed | 5-30 min |

### Multi-Region HA Patterns

| Pattern | Implementation |
|---------|---------------|
| **Region independence** | Each region operates fully independently. No cross-region dependency for data plane. Control plane dependency only on Keystone (mitigated by Fernet local validation). |
| **AZ spreading** | Critical workloads distributed across 3 AZs using server group anti-affinity. AZ failure affects < 33% of any workload. |
| **Paired regions for DR** | US-East ↔ US-West; EU-West ↔ US-East; APAC-SE ↔ APAC-NE. Each pair replicates to each other. |
| **GSLB with health checks** | DNS resolves to healthy regions. TTL 30s. Health check every 10s from multiple locations. |
| **Graduated alerting** | AZ failure: page on-call. Region degraded: page senior on-call. Region down: page incident commander. |
| **Capacity reservation** | Each region reserves 30% capacity for receiving DR failover from paired region. |

---

## 10. Observability

### Key Metrics

| Category | Metric | Source | Alert Threshold |
|----------|--------|--------|-----------------|
| **Cross-region** | dci_link_latency_ms | ICMP / BFD | > 2x normal RTT |
| **Cross-region** | dci_link_bandwidth_utilization | Switch stats | > 70% |
| **Cross-region** | rbd_mirror_replication_lag_seconds | Ceph | > 60s (RPO breach) |
| **Cross-region** | image_replication_pending_count | Custom | > 10 images |
| **Keystone** | keystone_token_issuance_latency_ms | StatsD | P99 > 1s |
| **Keystone** | keystone_fernet_key_age_hours | Custom | > 48h (sync failed) |
| **Per-region** | region_api_error_rate | Prometheus | > 1% |
| **Per-region** | region_compute_utilization | Placement API | > 70% |
| **Per-region** | region_vm_count | Nova API | Unexpected drops > 5% |
| **DR** | dr_failover_readiness | Custom health check | NOT READY |
| **DR** | dr_last_successful_test_age_days | Custom | > 90 days |
| **DNS** | gslb_health_check_status | DNS provider | Any region UNHEALTHY |

### OpenStack Ceilometer / Telemetry

```
Global Monitoring Architecture:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Each Region:
    Prometheus (local) ──── scrapes ──── local exporters
        │                                (openstack, node, ceph, ovn)
        │
        ▼
    Thanos Sidecar ──── uploads to ──── Object Store (S3/Swift)
    
  Global:
    Thanos Query ──── reads from ──── all regions' object stores
        │
        ▼
    Grafana (global dashboards)
        ├── Region comparison dashboard
        ├── Cross-region replication dashboard
        ├── Capacity planning dashboard
        └── DR readiness dashboard

  Alerting:
    Alertmanager (global) ──── receives alerts from ──── all regions' Prometheus
        ├── PagerDuty (Tier 1 alerts)
        ├── Slack (Tier 2 alerts)
        └── Email (Tier 3 alerts)

  Logging:
    Each Region: rsyslog → Kafka → Elasticsearch (regional cluster)
    Global: Elasticsearch cross-cluster search for multi-region log queries
```

---

## 11. Security

### Keystone: Auth, Tokens, Projects, Domains

```
Multi-Region Security Model:
━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. Single Identity, Region-Scoped Resources:
   - Users and projects are GLOBAL (defined in Keystone)
   - Resources (VMs, networks, volumes) are REGIONAL
   - A user in project "web-app" can create VMs in US-East AND EU-West
   - Quotas are PER-REGION (or global with custom quota service)

2. RBAC:
   - "admin" role: manage all regions (system-scoped)
   - "member" role: create resources in any region (project-scoped)
   - Custom roles: "region-admin:us-east" for region-specific admin
   - Application credentials: scoped to specific projects and regions

3. Cross-Region API Security:
   - All inter-region API calls use TLS (mTLS for service-to-service)
   - DCI links encrypted (MACsec or IPsec)
   - Fernet key distribution over encrypted channel (Vault transit)

4. Data Sovereignty:
   - Keystone domains can map to geographic regions
   - Policy can restrict resource creation to specific regions:
     Custom middleware: check if project's "allowed_regions" includes target region
   - Volume/image data physically stays in the region where created
   - Cross-region replication only to approved DR regions
```

---

## 12. Incremental Rollout

### Rollout Q&As

**Q1: How do you roll out a new region with zero impact on existing regions?**
A: The new region is deployed independently — it doesn't share any infrastructure with existing regions except Keystone. Steps: (1) Deploy all OpenStack services in the new region. (2) Test internally (Tempest, Rally). (3) Register endpoints in Keystone — this makes the region visible in the catalog. (4) Restrict access initially: set quotas to zero for all projects in the new region. (5) Onboard pilot users: increase quota for specific projects. (6) Monitor for 2 weeks. (7) Open to all users. Existing regions are completely unaffected throughout.

**Q2: How do you migrate a workload from one region to another?**
A: No automated cross-region live migration exists in OpenStack. Process: (1) Create a snapshot/image of the VM. (2) Copy image to destination region (Glance image import). (3) Copy volume to destination region (Cinder backup → restore, or Ceph export/import). (4) Create VM in destination region from copied image, attach copied volumes. (5) Update DNS/load balancer to point to new VM. (6) Verify and decommission old VM. For stateful applications, this requires a maintenance window. For stateless (behind LB), use blue-green: bring up in new region, add to LB, remove old from LB.

**Q3: How do you handle a situation where different regions need different OpenStack configurations?**
A: Use configuration hierarchy: (1) Global config: common across all regions (Keystone, security policies, image standards). (2) Region-specific config: nova.conf overrides per region (e.g., different hypervisor types, different CPU overcommit ratios due to hardware differences). (3) AZ-specific config: different availability zones may have different hardware generations. Implementation: Ansible group_vars hierarchy: `all/` → `region-us-east/` → `az-us-east-1a/`. Changes to global config deployed to all regions; region-specific changes deployed only to that region.

**Q4: How do you validate cross-region networking after a DCI upgrade?**
A: (1) Pre-upgrade: record baseline latency, bandwidth, packet loss between all region pairs. (2) During upgrade: monitor BGP sessions — should reconverge within seconds. (3) Post-upgrade: (a) Ping test between monitoring VMs in each region pair. (b) Bandwidth test (iperf3) to verify link capacity. (c) RBD mirror lag check — verify replication caught up. (d) VPNaaS tunnel status check — all tunnels should be UP. (e) BGP route table verification — all prefixes present. (4) Run for 24 hours with monitoring before declaring success.

**Q5: How do you deprecate and decommission a region?**
A: (1) Announce deprecation with 6-month timeline. (2) Stop creating new workloads: set all project quotas to zero in the region. (3) Help users migrate: provide tooling and documentation for cross-region migration. (4) Verify no active resources remain: `openstack server list --all-projects --status ACTIVE` should be empty. (5) Stop DR replication to/from the region. (6) Delete region endpoints from Keystone. (7) Delete region from Keystone. (8) Decommission hardware. Critical: ensure no DR dependencies — any region that was failing over to the deprecated region must be repointed.

---

## 13. Trade-offs & Decision Log

| # | Decision | Options Considered | Selected | Rationale |
|---|----------|--------------------|----------|-----------|
| 1 | Keystone topology | Single global, per-region federated, active-active | Single global + Fernet local validation | Simplest; Fernet eliminates most cross-region auth calls; acceptable login latency |
| 2 | DR approach | Synchronous replication, async (RBD mirror), snapshot-based | Async RBD mirror + Terraform | RPO < 1 min without latency penalty; Terraform recreates infrastructure |
| 3 | Cross-region networking | VPNaaS, BGP EVPN, overlay-in-overlay | BGP EVPN + VPNaaS | EVPN for infrastructure; VPNaaS for tenant self-service |
| 4 | DCI encryption | IPsec, MACsec, no encryption | MACsec (Layer 1) | Line-rate encryption; no CPU overhead; no MTU impact |
| 5 | Global monitoring | Centralized (single Prometheus), federated (Thanos) | Thanos federation | Each region owns its data; global queries without central bottleneck |
| 6 | Region count | 2 (active-passive), 3+ (active-active) | 5 regions | Geographic coverage; regulatory compliance; latency reduction |
| 7 | DR model | Hot standby, warm standby, cold standby | Tiered (hot for Tier 1, cold for Tier 3) | Cost-effective; matches SLA requirements per workload tier |
| 8 | Cross-region quotas | Per-region only, global | Per-region (initially) | Simpler; global quotas can be added later; most users don't span regions |
| 9 | Image distribution | Pull-on-demand, push replication, CDN | Push replication for base images; pull for custom | Base images pre-staged; custom images copied on first request |
| 10 | Failover automation | Fully manual, semi-auto (human approval), fully auto | Semi-automatic (auto-detect, human-approve) | Prevents false positive failovers; fast enough for 30-min RTO |

---

## 14. Agentic AI Integration

### AI-Enhanced Multi-Region Operations

| Use Case | AI Approach | Implementation |
|----------|------------|----------------|
| **Predictive capacity planning** | Time-series forecasting per region | Predict region-level compute/storage exhaustion 30-90 days ahead. Input: historical usage trends, seasonal patterns, growth rate. Output: procurement recommendations per region. |
| **Intelligent workload placement** | Multi-objective optimization | Given latency, cost, capacity, and compliance constraints, recommend optimal region for new workloads. ML model trained on historical placement decisions and outcomes. |
| **DR readiness scoring** | Composite health scoring | AI agent continuously evaluates: replication lag, target region capacity, DCI health, last DR test result. Outputs a 0-100 readiness score per protection group. Alerts when score drops. |
| **Anomaly detection across regions** | Cross-region correlation | Detect when one region deviates from the pattern of others (e.g., higher API error rate, lower VM boot success rate). May indicate region-specific issue. Reduces MTTD. |
| **Natural language runbook execution** | LLM-powered operations | "Fail over protection-group-web to eu-west" → agent validates readiness, shows impact assessment, requests confirmation, executes failover steps. Provides real-time status in natural language. |
| **Cost optimization across regions** | RL-based scheduling | Agent learns cost patterns (time-of-day electricity pricing, spot capacity availability) and recommends region/time for batch workloads to minimize cost. |

---

## 15. Complete Interviewer Q&A Bank

**Q1: How does OpenStack define regions and availability zones?**
A: A **region** is a separate OpenStack deployment with its own Nova, Neutron, Cinder, and Glance services. Regions share Keystone for identity and are listed in the service catalog. A user explicitly selects a region for each operation. An **availability zone (AZ)** is a grouping of compute hosts within a region. AZs share the same Nova control plane but have separate failure domains (power, cooling, network uplinks). AZs are implemented as Nova host aggregates with `availability_zone` metadata. Users select AZ when creating a VM (`--availability-zone az1`).

**Q2: What is the CAP theorem trade-off in a multi-region OpenStack deployment?**
A: We favor Availability and Partition tolerance (AP), accepting eventual consistency. Each region operates independently — if a network partition isolates a region, it continues serving requests (Availability). Keystone data (users, projects) may be stale in the partitioned region, but Fernet tokens work locally. Resources created in different regions are independent (no strong consistency needed across regions). The only "CP" component is the Keystone DB — if we need strong consistency for user updates, we sacrifice availability during partition. In practice, user updates are rare, so this is acceptable.

**Q3: How do you handle the "noisy neighbor" problem across regions?**
A: Noisy neighbors are primarily an intra-host problem (one VM consuming resources affects co-located VMs). Cross-region: a user consuming excessive resources in one region doesn't affect other regions (independent infrastructure). Intra-region mitigation: (1) VCPU pinning for sensitive workloads. (2) NUMA-aware scheduling. (3) QoS policies on network and disk I/O. (4) Per-project quotas limit resource consumption. (5) Monitoring: detect hosts with high CPU steal time or I/O contention, and live-migrate noisy VMs.

**Q4: How would you implement a global load balancer spanning multiple regions?**
A: Octavia (OpenStack LBaaS) is per-region. For global load balancing: (1) **DNS-based GSLB**: GeoDNS resolves the application domain to the nearest region's Octavia VIP. Health checks determine which regions are healthy. Simple but limited to DNS TTL for failover speed. (2) **Anycast**: advertise the same VIP from multiple regions via BGP. Traffic naturally routes to the nearest region. Fast failover (BGP convergence). Requires BGP at the edge of each region. (3) **External GSLB** (F5, Cloudflare, AWS Route 53): managed service handles global health checking and routing. Recommended: DNS-based GSLB for simplicity, anycast for latency-sensitive applications.

**Q5: How do you ensure data sovereignty in a multi-region cloud?**
A: (1) **Region-level controls**: projects can be tagged with allowed regions (custom metadata). Custom middleware rejects API calls to unauthorized regions. (2) **Keystone domains**: map organizational units to domains; domain policies restrict actions to specific regions. (3) **Image and volume residency**: images/volumes physically stored only in the region where created. Cross-region replication only to approved DR regions. (4) **Network isolation**: no cross-region L2 extension by default; tenant VPN requires explicit setup. (5) **Audit trail**: Keystone CADF events log which user accessed which region. (6) **Contractual**: SLA/ToS specifies data location. Technical controls enforce it.

**Q6: How do you handle clock synchronization across regions?**
A: Each region has internal NTP servers (stratum 2) synced to public stratum 1 sources (GPS, atomic clock). All hosts within a region sync to the regional NTP servers. Cross-region: clocks may differ by a few milliseconds (NTP accuracy). Impact: (1) Fernet tokens use UTC with second granularity — millisecond differences don't matter. (2) Log timestamps: use NTP-synced UTC across all regions for correlated analysis. (3) Ceph: requires clocks within 0.05s (NTP easily achieves this). (4) Galera: requires < 1s clock difference. Monitoring: alert on NTP drift > 100ms.

**Q7: How would you design a multi-region object storage system?**
A: OpenStack Swift supports multi-region deployment natively via "global clusters." Each region has a Swift cluster. Objects are replicated to N regions based on a storage policy (e.g., "3 replicas: 2 in US-East, 1 in EU-West"). The ring determines which regions hold each object. Reads can be served from the nearest region. Writes are eventually consistent across regions. For images (Glance): store in Swift with a multi-region storage policy. This provides automatic image replication without custom tooling.

**Q8: How do you perform capacity planning for DCI links?**
A: Monitor and forecast: (1) Measure current DCI utilization per link and per traffic type (DR replication, tenant VPN, control plane). (2) Identify peak patterns (backup windows, business hours). (3) Forecast growth: historical trend + planned new workloads. (4) Plan for burst: DR failover may suddenly need 10x normal replication bandwidth. (5) Rule of thumb: upgrade DCI when average utilization reaches 50% (to handle burst to 100%). (6) Lead time: physical link upgrades take 3-6 months, so plan ahead. Typical progression: 10G → 100G → 400G per link.

**Q9: How do you handle a scenario where one region is running a different OpenStack release?**
A: OpenStack APIs are backward compatible via microversioning. A client sends `OpenStack-API-Version: compute 2.90` and the server responds with the highest compatible version. If the older region supports up to 2.87, the client automatically downgrades. Terraform/Heat templates should use the lowest common version. Key concern: Keystone DB schema must be compatible across versions — since we use a single Keystone, it must be at the latest version, and older services use backward-compatible API versions. Best practice: keep all regions within one release (N vs N-1) and upgrade within 3 months.

**Q10: What monitoring alerts would you set up for a multi-region deployment?**
A: Critical alerts: (1) Region API endpoint unreachable (GSLB health check fails). (2) DCI link down (BGP session lost). (3) RBD mirror replication lag > RPO threshold. (4) Keystone Fernet key age > 48 hours (sync failure). (5) Region compute utilization > 80%. (6) Cross-region latency > 2x baseline. Warning alerts: (7) Image replication backlog > 10 images. (8) DR readiness score < 80. (9) Galera node out of cluster in any region. (10) Certificate expiring within 30 days. Info alerts: (11) Capacity forecast: region exhaustion within 60 days. (12) New region registered. (13) Successful DR test completed.

**Q11: How do you ensure configuration consistency across 5 regions?**
A: GitOps approach: (1) All OpenStack configuration (nova.conf, neutron.conf, etc.) stored in Git. (2) CI pipeline validates configuration changes. (3) CD pipeline (Ansible) deploys to all regions. (4) Drift detection: hourly job compares running config vs Git; alerts on drift. (5) Flavors, images, security group templates defined centrally and synced. (6) Region-specific overrides in separate config files (e.g., region-specific NTP servers, DNS servers). (7) Post-deployment validation: automated tests verify each region has expected services, correct configuration, and passes functional tests.

**Q12: How do you handle a partial region failure (e.g., one AZ down but others up)?**
A: (1) VMs in the failed AZ: evacuate to other AZs if shared storage or boot-from-volume. Otherwise, VMs are lost (must be rebuilt). (2) Control plane: typically spans all AZs with anti-affinity — survives single AZ failure. (3) Neutron: network nodes in other AZs serve DHCP/L3 for affected VMs. (4) No need for cross-region failover — this is intra-region HA. (5) Communication: status page updated; affected users notified. (6) Recovery: repair failed AZ; rebalance workloads.

**Q13: How would you implement a "follow the sun" deployment pattern?**
A: Batch processing workloads can run in whichever region has the cheapest electricity (off-peak hours). Implementation: (1) Define time-of-day cost model per region. (2) Orchestrator (Heat or custom) schedules batch jobs in the cheapest region for the current time. (3) Data must be accessible: use object storage with cross-region replication, or pre-stage data. (4) Results collected in a central location or the user's home region. (5) AI optimization: RL agent learns optimal scheduling considering cost, latency, and data transfer time. Benefits: 20-30% cost reduction for flexible workloads.

**Q14: What is the impact of adding a 6th region to an existing 5-region deployment?**
A: (1) Keystone: register new region and endpoints (trivial). (2) DCI: establish links to at least 2 existing regions (physical work). (3) Fernet key sync: add new region to distribution. (4) DR pairing: assign DR partner region. (5) Monitoring: add to Thanos federation, Grafana dashboards. (6) Image replication: sync base images to new region. (7) DNS: add new region to GSLB pool. Impact on existing regions: minimal — no configuration changes in existing regions (except DR pairing if reassigned). The new region is additive.

**Q15: How do you handle compliance auditing across multiple regions?**
A: (1) Centralized audit log: Keystone CADF events from all regions flow to a central SIEM (Splunk, ELK). (2) Each region's service logs include request_id for end-to-end tracing. (3) Compliance dashboards: show security group changes, admin actions, user access patterns per region. (4) Automated compliance checks: verify encryption (TLS everywhere, encrypted storage), access controls (no default passwords), network isolation (no unauthorized cross-tenant traffic). (5) Periodic third-party audits with access to centralized logs. (6) Region-specific compliance: EU regions have additional GDPR controls; report on data access logs.

---

## 16. References

| # | Resource | URL |
|---|----------|-----|
| 1 | OpenStack Regions and Endpoints | https://docs.openstack.org/keystone/latest/admin/manage-regions.html |
| 2 | Keystone Federation | https://docs.openstack.org/keystone/latest/admin/federation/federated_identity.html |
| 3 | Keystone Fernet Tokens | https://docs.openstack.org/keystone/latest/admin/fernet-token-faq.html |
| 4 | Ceph RBD Mirroring | https://docs.ceph.com/en/latest/rbd/rbd-mirroring/ |
| 5 | Glance Multi-Store | https://docs.openstack.org/glance/latest/admin/multistores.html |
| 6 | OpenStack HA Guide | https://docs.openstack.org/ha-guide/index.html |
| 7 | BGP EVPN Data Center Interconnect | https://datatracker.ietf.org/doc/html/rfc7432 |
| 8 | Thanos (Prometheus at Scale) | https://thanos.io/tip/thanos/design.md/ |
| 9 | OpenStack Large Scale Deployment | https://docs.openstack.org/arch-design/use-cases/use-case-large-scale.html |
| 10 | Kolla-Ansible Deployment | https://docs.openstack.org/kolla-ansible/latest/ |
