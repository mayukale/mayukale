# System Design: IaaS Platform Control Plane

> **Relevance to role:** A cloud infrastructure platform engineer must design and operate the full IaaS control plane that abstracts physical resources (compute, network, storage) into programmable APIs, supporting both bare-metal and virtual machine workloads across multi-region data centers with OpenStack integration.

---

## 1. Requirement Clarifications

### Functional Requirements
- **Compute lifecycle management**: Provision, start, stop, reboot, terminate bare-metal servers and virtual machines via unified REST API.
- **Resource hierarchy**: Region -> Availability Zone -> Rack -> Server, with capacity tracking at each level.
- **Network management (SDN)**: Create/delete virtual networks, subnets, VLANs, floating IPs, security groups, load balancers. Integration with physical network fabric via SDN controller.
- **Storage management**: Attach/detach block volumes, object storage, NFS/shared filesystems. Integrate with Ceph or equivalent distributed storage.
- **Identity and access management**: Multi-tenant RBAC, project/namespace isolation, service accounts, API key management.
- **Image management**: Upload, version, distribute OS images and snapshots across regions.
- **Billing and metering**: Track resource usage (CPU-hours, GPU-hours, storage-GB-months, network-GB-egress), emit events for billing pipeline.
- **OpenStack integration layer**: Expose Nova (compute), Neutron (network), Cinder (storage), Keystone (identity), Glance (image) compatible APIs for teams already using OpenStack tooling.
- **Quota management**: Per-tenant quotas on instances, vCPUs, RAM, storage, floating IPs.
- **Metadata service**: Instance metadata endpoint (169.254.169.254) for cloud-init, user data, SSH keys.
- **Resource tagging**: Arbitrary key-value tags on all resources for cost allocation and organization.

### Non-Functional Requirements
- **Availability target**: Control plane: 99.99% (52.6 min/year). Data plane: 99.999% (5.3 min/year) -- running instances must not be affected by control plane outages.
- **Consistency model**: Strong consistency for resource state (cannot double-allocate a server). Eventual consistency acceptable for metering/billing (< 30s lag) and dashboard views.
- **Latency target**: API p50 < 100ms, p99 < 500ms. VM boot < 60s. Bare-metal provisioning < 15 min.
- **Durability**: Resource state: RPO = 0 (synchronous replication). Block storage: 99.999999999% (11 nines, triple-replicated Ceph).
- **Scalability**: 100,000 physical servers, 500,000 virtual instances, 10,000 tenants, 5,000 API calls/sec.

### Constraints & Assumptions
- Multiple data center regions (5 regions, 3 AZs each).
- OpenStack is an existing investment -- we wrap/extend it, not replace it.
- Physical network uses leaf-spine topology with EVPN-VXLAN overlay.
- Storage backend is Ceph (block via RBD, object via RadosGW, filesystem via CephFS).
- Java/Spring Boot for API services; Python for OpenStack integration and provisioning agents.
- Kubernetes runs the control plane services (but the managed workloads are bare-metal/VM, not containers).

### Out of Scope
- Container/Kubernetes-as-a-Service (managed K8s offering)
- PaaS-level services (managed databases, serverless functions)
- Physical data center construction and power/cooling
- End-user application architecture

---

## 2. Scale & Capacity Estimates

### Users & Traffic
| Metric | Value | Calculation |
|--------|-------|-------------|
| Physical servers | 100,000 | 5 regions x 3 AZs x ~6,700 servers/AZ |
| Virtual instances | 500,000 | ~5 VMs per physical server on average |
| Tenants | 10,000 | Internal teams + external customers |
| API calls/sec (QPS read) | 4,000 | Dashboard polls, instance status, network queries |
| API calls/sec (QPS write) | 1,000 | Provision, state changes, network mutations |
| Peak multiplier | 3x | 15,000 total QPS during peak |

### Latency Requirements
| Operation | Target | Justification |
|-----------|--------|---------------|
| Create VM (API response) | p99 < 500ms | Synchronous acceptance; async provisioning |
| VM boot (total) | p99 < 60s | Hypervisor launch + OS boot |
| Bare-metal provision | p99 < 15 min | IPMI + PXE + OS install |
| Network create (API) | p99 < 200ms | SDN controller config |
| Volume attach | p99 < 10s | Ceph RBD map + iSCSI/NVMeoF attach |
| API GET (instance details) | p99 < 100ms | Cache-backed reads |

### Storage Estimates
| Data type | Size/record | Volume/day | Retention | Total |
|-----------|-------------|------------|-----------|-------|
| Instance records | ~3 KB | 50,000 creates/day | Forever (soft-delete) | ~55 GB/year |
| Network records | ~1 KB | 10,000/day | Forever | ~3.6 GB/year |
| Volume records | ~1 KB | 20,000/day | Forever | ~7.3 GB/year |
| Metering events | ~200 B | 50M/day (every instance reports hourly) | 1 year | ~3.6 TB/year |
| Audit logs | ~500 B | 5M/day | 3 years | ~2.7 TB/year |
| OS images | ~2 GB each | ~5 new/day | 6 months | ~1.8 TB |

### Bandwidth Estimates
| Direction | Calculation | Result |
|-----------|-------------|--------|
| API inbound | 15,000 req/sec x 1 KB = 15 MB/s | ~120 Mbps |
| API outbound | 15,000 req/sec x 3 KB = 45 MB/s | ~360 Mbps |
| Image distribution | 100 provisions/hr x 2 GB / 3600 = 56 MB/s | Served from per-AZ cache |
| Metering pipeline | 50M events/day x 200 B / 86400 = 116 KB/s | ~1 Mbps |
| Ceph replication (inter-AZ) | Varies; 3x replication within AZ | AZ-local |

---

## 3. High Level Architecture

```
                          +-------------------+
                          |  Users / CLI /    |
                          |  Terraform / SDK  |
                          +--------+----------+
                                   |
                                   v
                          +--------+----------+
                          |   API Gateway     |
                          | (Rate limit, Auth,|
                          |  Routing, TLS)    |
                          +--------+----------+
                                   |
              +--------------------+--------------------+
              |                    |                    |
              v                    v                    v
     +--------+------+   +--------+------+    +--------+------+
     | Compute       |   | Network       |    | Storage       |
     | Service       |   | Service       |    | Service       |
     | (Nova-compat) |   | (Neutron-     |    | (Cinder-      |
     | Java/Spring   |   |  compat)      |    |  compat)      |
     +--------+------+   | Java/Spring   |    | Java/Spring   |
              |           +--------+------+    +--------+------+
              |                    |                    |
     +--------v------+   +--------v------+    +--------v------+
     | OpenStack     |   | SDN           |    | Ceph          |
     | Nova Agent    |   | Controller    |    | Cluster       |
     | (Python)      |   | (OVN/OVS/    |    | (RBD, RadosGW)|
     |               |   |  Contrail)    |    |               |
     +--------+------+   +--------+------+    +--------+------+
              |                    |                    |
              v                    v                    v
     +--------+------+   +--------+------+    +--------+------+
     | Hypervisors / |   | Physical      |    | Storage       |
     | BMC (IPMI)    |   | Switches /    |    | Nodes (OSD)   |
     | bare metal    |   | ToR / Spine   |    |               |
     +--------------+    +--------------+     +--------------+

     +---------------------------------------------------------------+
     |                   Shared Services                              |
     | +----------+ +----------+ +--------+ +--------+ +----------+  |
     | | Identity | | Image    | | Meter- | | Quota  | | Scheduler|  |
     | | (Keystone| | (Glance  | | ing    | | Service| | / Place- |  |
     | |  compat) | |  compat) | | Service| |        | | ment     |  |
     | +----------+ +----------+ +--------+ +--------+ +----------+  |
     +---------------------------------------------------------------+

     +---------------------------------------------------------------+
     |                  Data Stores                                   |
     | +--------+  +--------+  +--------+  +---------+  +--------+  |
     | | MySQL  |  | Redis  |  | Kafka  |  | Elastic |  | etcd   |  |
     | | (state)|  | (cache)|  | (events|  | search  |  | (coord)|  |
     | |        |  |        |  |  meter)|  | (logs)  |  |        |  |
     | +--------+  +--------+  +--------+  +---------+  +--------+  |
     +---------------------------------------------------------------+
```

### Component Roles

**API Gateway:** Unified entry point for all IaaS operations. Routes requests to the appropriate service based on URL path (`/compute/*` -> Compute Service, `/network/*` -> Network Service). Handles authentication (JWT validation via Keystone-compatible token), rate limiting, request logging.

**Compute Service (Java/Spring Boot):** Manages the lifecycle of bare-metal instances and VMs. For VMs: communicates with OpenStack Nova via its API or directly with libvirt/KVM hypervisors. For bare-metal: communicates with the Bare-Metal Reservation Platform (see bare_metal_reservation_platform.md) and OpenStack Ironic for provisioning. Stateless; state stored in MySQL.

**Network Service (Java/Spring Boot):** Manages virtual networks, subnets, security groups, floating IPs, load balancers. Translates high-level network intent into SDN controller commands. Compatible with OpenStack Neutron API. SDN backend: OVN (Open Virtual Network) for overlay networks, or Contrail/Tungsten Fabric for larger deployments.

**Storage Service (Java/Spring Boot):** Manages block volumes (Ceph RBD), object storage (Ceph RadosGW), shared filesystems (CephFS). Compatible with OpenStack Cinder API. Handles volume snapshots, backups, encryption.

**Identity Service (Keystone-compatible):** Authentication (OIDC/SAML/API keys), token issuance, project/tenant management, role assignments. Wraps or extends OpenStack Keystone.

**Image Service (Glance-compatible):** Stores and distributes OS images and instance snapshots. Images stored in Ceph RadosGW (object storage). Per-AZ image cache for fast provisioning.

**Scheduler/Placement Service:** Decides which physical host to place a VM on (or which bare-metal machine to assign). Considers: resource availability (CPU, RAM, GPU), affinity/anti-affinity rules, AZ constraints, overcommit ratios.

**Metering Service:** Collects usage events from all services via Kafka. Aggregates into hourly/daily/monthly usage records. Feeds billing pipeline.

**Quota Service:** Enforces per-tenant resource limits. Checked synchronously on every create/resize operation.

### Primary Data Flow: Create a VM Instance

1. User calls `POST /compute/v2/servers` with flavor, image, network, SSH key.
2. API Gateway authenticates, rate-limits, routes to Compute Service.
3. Compute Service validates request, checks quota (Quota Service), generates instance UUID.
4. Scheduler/Placement Service selects a hypervisor host based on flavor requirements and placement constraints.
5. Compute Service persists instance record in MySQL (status: `BUILDING`), returns 202 Accepted with instance ID.
6. Compute Service sends a provision command to the selected hypervisor host via RPC (RabbitMQ or gRPC).
7. Hypervisor agent (Nova compute agent, Python):
   a. Downloads the OS image from the per-AZ Glance cache.
   b. Creates a Ceph RBD volume (root disk).
   c. Requests network port creation from Network Service -> SDN controller configures OVS flow rules.
   d. Launches the VM via libvirt/KVM.
   e. Reports status back to Compute Service.
8. Compute Service updates instance status to `ACTIVE` in MySQL.
9. Metering event `instance.create` published to Kafka.
10. User polls `GET /compute/v2/servers/{id}` or receives a webhook notification when status = `ACTIVE`.

### Secondary Data Flow: Attach a Block Volume

1. User calls `POST /storage/v2/volumes/{volume_id}/attachments` with instance_id.
2. Storage Service validates the volume and instance are in the same AZ.
3. Storage Service creates a Ceph RBD mapping and initiates an iSCSI/NVMeoF target on the storage node.
4. Compute Service configures the hypervisor to attach the block device to the VM (or BMC for bare-metal: configures the machine's iSCSI initiator).
5. Attachment record persisted in MySQL. Metering event emitted.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Instance: VM or bare-metal server
CREATE TABLE instances (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    instance_uuid       CHAR(36) NOT NULL,
    tenant_id           BIGINT UNSIGNED NOT NULL,
    project_id          BIGINT UNSIGNED NOT NULL,
    name                VARCHAR(255) NOT NULL,
    instance_type       ENUM('vm','bare_metal') NOT NULL,
    status              ENUM('BUILDING','ACTIVE','SHUTOFF','PAUSED','SUSPENDED',
                             'REBOOT','RESIZE','ERROR','DELETED') NOT NULL DEFAULT 'BUILDING',
    flavor_id           BIGINT UNSIGNED NOT NULL,
    image_id            BIGINT UNSIGNED NULL,          -- Source image
    host_id             BIGINT UNSIGNED NULL,           -- Physical host / hypervisor
    reservation_id      BIGINT UNSIGNED NULL,           -- For bare-metal, links to reservation
    availability_zone   VARCHAR(32) NOT NULL,
    region              VARCHAR(32) NOT NULL,
    
    -- Computed resource usage (denormalized from flavor)
    vcpus               SMALLINT UNSIGNED NOT NULL,
    ram_mb              INT UNSIGNED NOT NULL,
    disk_gb             INT UNSIGNED NOT NULL,
    gpu_count           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    
    -- Network
    fixed_ip            VARCHAR(45) NULL,
    floating_ip         VARCHAR(45) NULL,
    
    -- Metadata
    user_data           TEXT NULL,                      -- Base64-encoded cloud-init
    key_pair_name       VARCHAR(255) NULL,
    tags                JSON NULL,
    
    launched_at         DATETIME NULL,
    terminated_at       DATETIME NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    deleted_at          DATETIME NULL,                  -- Soft delete
    
    UNIQUE KEY uk_uuid (instance_uuid),
    INDEX idx_tenant_status (tenant_id, status),
    INDEX idx_host (host_id),
    INDEX idx_az_status (availability_zone, status),
    INDEX idx_project (project_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Flavor: instance sizing template
CREATE TABLE flavors (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    name                VARCHAR(64) NOT NULL,           -- e.g., 'gpu.h100.8xlarge'
    vcpus               SMALLINT UNSIGNED NOT NULL,
    ram_mb              INT UNSIGNED NOT NULL,
    disk_gb             INT UNSIGNED NOT NULL,
    gpu_type            VARCHAR(32) NULL,
    gpu_count           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    is_public           BOOLEAN NOT NULL DEFAULT TRUE,
    is_bare_metal       BOOLEAN NOT NULL DEFAULT FALSE,
    extra_specs         JSON NULL,                      -- e.g., {"hw:numa_nodes": 2}
    
    UNIQUE KEY uk_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Host: physical server / hypervisor
CREATE TABLE hosts (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    host_uuid           CHAR(36) NOT NULL,
    hostname            VARCHAR(255) NOT NULL,
    host_type           ENUM('hypervisor','bare_metal','storage','network') NOT NULL,
    status              ENUM('enabled','disabled','maintenance') NOT NULL DEFAULT 'enabled',
    state               ENUM('up','down','unknown') NOT NULL DEFAULT 'unknown',
    region              VARCHAR(32) NOT NULL,
    availability_zone   VARCHAR(32) NOT NULL,
    rack_id             VARCHAR(32) NOT NULL,
    
    -- Total capacity
    total_vcpus         SMALLINT UNSIGNED NOT NULL,
    total_ram_mb        INT UNSIGNED NOT NULL,
    total_disk_gb       INT UNSIGNED NOT NULL,
    total_gpus          TINYINT UNSIGNED NOT NULL DEFAULT 0,
    
    -- Used capacity (updated on instance placement/removal)
    used_vcpus          SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    used_ram_mb         INT UNSIGNED NOT NULL DEFAULT 0,
    used_disk_gb        INT UNSIGNED NOT NULL DEFAULT 0,
    used_gpus           TINYINT UNSIGNED NOT NULL DEFAULT 0,
    
    -- Overcommit ratios
    cpu_overcommit      DECIMAL(3,1) NOT NULL DEFAULT 1.0,  -- 1.0 for GPU, 4.0 for general
    ram_overcommit      DECIMAL(3,1) NOT NULL DEFAULT 1.0,
    
    hypervisor_type     VARCHAR(32) NULL,               -- 'kvm','xen',NULL (bare-metal)
    hypervisor_version  VARCHAR(32) NULL,
    
    last_heartbeat      DATETIME NULL,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    UNIQUE KEY uk_uuid (host_uuid),
    UNIQUE KEY uk_hostname (hostname),
    INDEX idx_az_status (availability_zone, status, state),
    INDEX idx_capacity (host_type, availability_zone, used_vcpus, used_ram_mb)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Virtual network
CREATE TABLE networks (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    network_uuid        CHAR(36) NOT NULL,
    tenant_id           BIGINT UNSIGNED NOT NULL,
    name                VARCHAR(255) NOT NULL,
    network_type        ENUM('vxlan','vlan','flat') NOT NULL DEFAULT 'vxlan',
    segmentation_id     INT UNSIGNED NULL,              -- VXLAN VNI or VLAN ID
    cidr                VARCHAR(50) NOT NULL,            -- e.g., '10.0.0.0/24'
    gateway_ip          VARCHAR(45) NULL,
    dns_nameservers     JSON NULL,
    is_external         BOOLEAN NOT NULL DEFAULT FALSE,
    status              ENUM('ACTIVE','DOWN','BUILD','ERROR') NOT NULL DEFAULT 'BUILD',
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    UNIQUE KEY uk_uuid (network_uuid),
    INDEX idx_tenant (tenant_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Block volume
CREATE TABLE volumes (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    volume_uuid         CHAR(36) NOT NULL,
    tenant_id           BIGINT UNSIGNED NOT NULL,
    name                VARCHAR(255) NULL,
    size_gb             INT UNSIGNED NOT NULL,
    volume_type         VARCHAR(64) NOT NULL DEFAULT 'ssd', -- 'ssd','hdd','nvme'
    status              ENUM('creating','available','in-use','deleting',
                             'error','backing-up','restoring') NOT NULL DEFAULT 'creating',
    attached_instance   BIGINT UNSIGNED NULL,
    availability_zone   VARCHAR(32) NOT NULL,
    snapshot_id         BIGINT UNSIGNED NULL,           -- Created from snapshot
    encrypted           BOOLEAN NOT NULL DEFAULT FALSE,
    ceph_pool           VARCHAR(64) NOT NULL DEFAULT 'volumes',
    ceph_image          VARCHAR(128) NULL,              -- RBD image name
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    
    UNIQUE KEY uk_uuid (volume_uuid),
    INDEX idx_tenant_status (tenant_id, status),
    INDEX idx_instance (attached_instance)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Quota tracking
CREATE TABLE quotas (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    tenant_id           BIGINT UNSIGNED NOT NULL,
    resource_type       ENUM('instances','vcpus','ram_mb','gpu','volumes',
                             'volume_gb','floating_ips','networks','snapshots') NOT NULL,
    quota_limit         INT NOT NULL DEFAULT -1,         -- -1 = unlimited
    in_use              INT UNSIGNED NOT NULL DEFAULT 0,
    reserved            INT UNSIGNED NOT NULL DEFAULT 0, -- Optimistic reservation
    
    UNIQUE KEY uk_tenant_resource (tenant_id, resource_type)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Metering events (high-volume; consider partitioning by date)
CREATE TABLE metering_events (
    id                  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    event_uuid          CHAR(36) NOT NULL,
    tenant_id           BIGINT UNSIGNED NOT NULL,
    resource_type       VARCHAR(32) NOT NULL,           -- 'instance','volume','floating_ip'
    resource_id         CHAR(36) NOT NULL,
    event_type          VARCHAR(64) NOT NULL,           -- 'instance.create','instance.delete'
    payload             JSON NOT NULL,                  -- Resource metadata at event time
    timestamp           DATETIME NOT NULL,
    
    INDEX idx_tenant_time (tenant_id, timestamp),
    INDEX idx_resource (resource_id, timestamp)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (TO_DAYS(timestamp)) (
    -- Monthly partitions for efficient pruning
    PARTITION p202604 VALUES LESS THAN (TO_DAYS('2026-05-01')),
    PARTITION p202605 VALUES LESS THAN (TO_DAYS('2026-06-01')),
    PARTITION p_future VALUES LESS THAN MAXVALUE
);
```

### Database Selection

**Selected: MySQL 8.0 (InnoDB)** for resource state (instances, networks, volumes, quotas). Same reasoning as the bare-metal reservation platform: ACID transactions for resource allocation, `SELECT FOR UPDATE` for quota enforcement, semi-synchronous replication for RPO=0.

**Kafka** for metering events and async notifications (high volume, append-only, consumer-group pattern).

**Elasticsearch** for log search, audit log search, and instance metadata search (full-text search on tags, names).

**Redis** for caching host capacity, instance status, and quota counters.

**etcd** for leader election and distributed coordination (scheduler leader, metering aggregator leader).

### Indexing Strategy

- **`idx_az_status`** on instances/hosts: Powers the scheduler's "find hosts with capacity in this AZ" query.
- **`idx_tenant_status`** on instances: Per-tenant dashboard ("show my running instances").
- **`idx_capacity`** on hosts: Scheduler's primary query. Composite index on (host_type, AZ, used_vcpus, used_ram_mb) allows efficient filtering for hosts with available capacity.
- **`idx_tenant_time`** on metering_events: Billing queries ("tenant X's usage in April"). Partitioned by month for fast pruning.

---

## 5. API Design

```
# === Compute (Nova-compatible) ===

POST   /compute/v2/servers
  Description: Create an instance (VM or bare-metal)
  Request: { "server": { "name": "...", "flavorRef": "...", "imageRef": "...",
             "networks": [...], "key_name": "...", "user_data": "...",
             "availability_zone": "us-east-1a", "metadata": {...} } }
  Response: 202 Accepted { "server": { "id": "...", "status": "BUILDING", ... } }

GET    /compute/v2/servers/{id}
GET    /compute/v2/servers                      (list, with ?status=ACTIVE&limit=100)
DELETE /compute/v2/servers/{id}                  (terminate)
POST   /compute/v2/servers/{id}/action
  Actions: { "reboot": {"type": "SOFT|HARD"} }
           { "resize": {"flavorRef": "..."} }
           { "os-stop": null }
           { "os-start": null }
           { "createImage": {"name": "my-snapshot"} }

# === Network (Neutron-compatible) ===

POST   /network/v2/networks                     (create virtual network)
GET    /network/v2/networks/{id}
POST   /network/v2/subnets                      (create subnet)
POST   /network/v2/ports                        (create port / attach to instance)
POST   /network/v2/floatingips                  (allocate floating IP)
PUT    /network/v2/floatingips/{id}             (associate with port)
POST   /network/v2/security-groups              (create security group)
POST   /network/v2/security-group-rules         (add rules)

# === Storage (Cinder-compatible) ===

POST   /storage/v3/volumes                      (create volume)
GET    /storage/v3/volumes/{id}
DELETE /storage/v3/volumes/{id}
POST   /storage/v3/volumes/{id}/action
  Actions: { "os-attach": {"instance_uuid": "...", "mountpoint": "/dev/vdb"} }
           { "os-detach": null }
           { "os-extend": {"new_size": 200} }
POST   /storage/v3/snapshots                    (create snapshot)

# === Identity (Keystone-compatible) ===

POST   /identity/v3/auth/tokens                 (authenticate, get token)
GET    /identity/v3/projects                    (list projects)
GET    /identity/v3/roles                       (list roles)
POST   /identity/v3/role_assignments            (assign role to user on project)

# === Image (Glance-compatible) ===

POST   /image/v2/images                         (create image metadata)
PUT    /image/v2/images/{id}/file               (upload image data)
GET    /image/v2/images/{id}
GET    /image/v2/images                         (list images)

# === Platform-specific extensions (non-OpenStack) ===

GET    /platform/v1/capacity                    (capacity overview by region/AZ/type)
GET    /platform/v1/tenants/{id}/usage          (usage summary for billing)
POST   /platform/v1/tenants/{id}/quotas         (set quotas)
GET    /platform/v1/health                      (system health dashboard data)
```

### CLI Design

```bash
# Create a VM instance
iaas-cli server create \
  --name "training-worker-01" \
  --flavor gpu.h100.8xlarge \
  --image ubuntu-22.04-cuda \
  --network my-training-net \
  --az us-east-1a \
  --key-name my-ssh-key \
  --user-data cloud-init.yaml \
  --tag team=ml --tag project=llm

# List instances
iaas-cli server list --status ACTIVE --output table

# Create a network
iaas-cli network create --name my-net --cidr 10.0.0.0/24

# Create and attach a volume
iaas-cli volume create --name data-vol --size 500 --type nvme --az us-east-1a
iaas-cli volume attach data-vol --server training-worker-01 --device /dev/vdb

# Terraform provider usage
# provider "iaas" { endpoint = "https://api.iaas.internal" }
# resource "iaas_server" "worker" { ... }
```

---

## 6. Core Component Deep Dives

### Component: Scheduler / Placement Service

**Why it's hard:** Placing 500,000 instances across 100,000 hosts while respecting resource constraints (CPU, RAM, GPU, disk), affinity/anti-affinity rules, AZ requirements, overcommit ratios, and host maintenance windows -- all while handling 1,000 placement decisions/sec with low latency.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Random** | Pick a random eligible host | Simple, distributed | Poor utilization, no constraint satisfaction | Test environments |
| **Least-loaded** | Pick host with most free capacity | Good balance | O(n) scan per request, no affinity support | Small clusters |
| **Filter-then-weigh** (Nova scheduler) | Filter hosts by hard constraints, then rank by weighted scoring | Flexible, extensible, proven in OpenStack | Single-threaded scheduling can be a bottleneck | **Medium-large OpenStack deployments** |
| **Bin packing (Google Borg)** | Optimize utilization via constraint programming | Best utilization | High latency for placement decision, complex | Very large clusters with batch workloads |
| **Cell-based (Nova cells v2)** | Partition hosts into cells, each with independent DB/scheduler | Scales to massive fleets, blast radius isolation | Cross-cell operations are complex | **Largest deployments (>10K hosts)** |

**Selected Approach:** Filter-then-weigh scheduler within a cell-based architecture (Nova cells v2 pattern). Each cell manages ~5,000 hosts. A global "super-scheduler" routes requests to the appropriate cell.

**Implementation:**

```java
public class PlacementScheduler {
    
    private final List<HostFilter> filters;
    private final List<HostWeigher> weighers;
    
    public PlacementScheduler() {
        this.filters = List.of(
            new AvailabilityZoneFilter(),    // Hard constraint: must match requested AZ
            new ResourceAvailabilityFilter(), // Hard constraint: enough CPU/RAM/GPU
            new HostStateFilter(),            // Hard constraint: host must be enabled + up
            new AffinityFilter(),             // Soft/hard: server group affinity rules
            new AntiAffinityFilter(),         // Soft/hard: anti-affinity rules
            new AggregateFilter(),            // Flavor-specific host aggregates
            new MaintenanceWindowFilter()     // Exclude hosts with imminent maintenance
        );
        
        this.weighers = List.of(
            new RamWeigher(1.0),             // Prefer hosts with more free RAM
            new CpuWeigher(0.5),             // Secondary: more free CPU
            new GpuWeigher(2.0),             // High weight: GPUs are scarce
            new RackSpreadWeigher(0.3),      // Spread instances across racks
            new HostLoadWeigher(0.8)         // Prefer less-loaded hosts
        );
    }
    
    public Host schedule(InstanceRequest request) {
        // Step 1: Get all hosts in the cell
        List<Host> candidates = hostRepository.findByCell(request.getCellId());
        
        // Step 2: Apply filters (hard constraints)
        for (HostFilter filter : filters) {
            candidates = filter.filter(candidates, request);
            if (candidates.isEmpty()) {
                throw new NoValidHostException(
                    "Filter " + filter.getName() + " eliminated all hosts");
            }
        }
        
        // Step 3: Apply weighers (soft preferences)
        Map<Host, Double> scores = new HashMap<>();
        for (Host host : candidates) {
            double score = 0.0;
            for (HostWeigher weigher : weighers) {
                score += weigher.weigh(host, request) * weigher.getMultiplier();
            }
            scores.put(host, score);
        }
        
        // Step 4: Pick top N candidates, select randomly among them (spread)
        List<Host> topN = scores.entrySet().stream()
            .sorted(Map.Entry.<Host, Double>comparingByValue().reversed())
            .limit(3)
            .map(Map.Entry::getKey)
            .collect(toList());
        
        return topN.get(ThreadLocalRandom.current().nextInt(topN.size()));
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Scheduler crashes | No new instances can be created | Multiple scheduler replicas (active-active with cell partitioning). If one cell's scheduler dies, K8s restarts it in 15s. Other cells unaffected. |
| Stale host capacity data | Over-placement (host overloaded) or under-placement (capacity wasted) | Host capacity updated in MySQL on every instance create/delete. Redis cache of capacity with 10s TTL. Placement verified at hypervisor level (reject if actually full). |
| No valid hosts after filtering | Instance creation fails | Return clear error: "No hosts available in us-east-1a with 8 GPUs. Available in us-east-1b: 12 hosts." Allow user to retry with relaxed constraints. |

**Interviewer Deep-Dive Q&A:**

Q: How does cell-based scheduling work with cross-cell operations?
A: Each cell is an independent failure domain with its own MySQL database, message broker, and scheduler. The global API routes requests to the appropriate cell based on the requested AZ (each AZ maps to one cell). Cross-cell operations (e.g., listing all instances for a tenant across cells) are handled by the global API, which scatter-gathers from all cells and merges results. This is a read-only fan-out and is acceptable at < 500ms. Write operations are always cell-local. If a cell is down, only that cell's AZ is affected; other AZs continue operating.

Q: How do you handle the "stale capacity" problem between scheduler decision and actual placement?
A: Two-phase approach: (1) The scheduler "claims" resources optimistically (increments `used_vcpus`, `used_ram_mb` in the hosts table within a transaction). (2) The hypervisor agent verifies the claim and actually creates the VM. If the hypervisor rejects (truly out of capacity due to race condition), the claim is rolled back and the scheduler retries on a different host. This claim-then-verify pattern is exactly how Nova's scheduler works. The claim reduces the retry rate to < 0.1% under normal load.

Q: What overcommit ratios do you use and why?
A: CPU: 4:1 for general-purpose instances (most workloads do not sustain 100% CPU). GPU: 1:1 (no overcommit -- GPU workloads need dedicated access). RAM: 1.5:1 for general-purpose (with kernel same-page merging, KSM, and ballooning). For bare-metal: no overcommit (dedicated hardware). These ratios are configurable per host aggregate, so GPU hosts and general-purpose hosts have different policies.

Q: How does anti-affinity enforcement work for high-availability deployments?
A: Users create a "server group" with policy `anti-affinity` and specify a scope (rack, AZ, host). When creating instances in that group, the AntiAffinityFilter excludes hosts (or racks, or AZs) that already have instances from the same group. Example: a 3-replica database cluster with rack-level anti-affinity ensures each replica is on a different rack. If anti-affinity cannot be satisfied (not enough racks), the request fails with a clear error rather than silently violating the constraint (hard anti-affinity). Soft anti-affinity is a weigher, not a filter -- it prefers spreading but allows co-location if necessary.

Q: How does the scheduler handle spot/preemptible instances differently from on-demand?
A: Spot instances have a lower priority tier in the scheduler. When capacity is tight, the scheduler can mark spot instances as preemptible. If an on-demand request arrives and no capacity is available, the preemption engine (see bare_metal_reservation_platform.md) identifies spot instances to terminate. Spot instances get a 2-minute warning (via metadata service notification) before termination. The scheduler tracks spot vs. on-demand capacity separately to ensure on-demand SLAs are met.

Q: How do you prevent scheduler hot-spots where all new VMs land on the same host?
A: The `topN` random selection in the scheduler spreads load among the top 3 candidates. Additionally, the `HostLoadWeigher` penalizes hosts that have had recent placements (exponential decay over the last 5 minutes). This prevents the "thundering herd to one host" problem during burst creation. The `RackSpreadWeigher` further distributes instances across racks for failure domain isolation.

---

### Component: OpenStack Integration Layer

**Why it's hard:** OpenStack APIs have complex, evolving specifications with microversioned behavior. We must maintain compatibility for existing tooling (Terraform OpenStack provider, openstackclient, Heat templates) while adding platform-specific extensions. The integration must handle OpenStack's eventual consistency model and asynchronous operations correctly.

**All Approaches Considered:**

| Approach | Description | Pros | Cons | Best for |
|----------|-------------|------|------|----------|
| **Run OpenStack natively** | Deploy full OpenStack (DevStack/Kolla/TripleO) | Full compatibility | Operational complexity, upgrade difficulty, monolithic | Orgs fully committed to OpenStack |
| **API facade (shim layer)** | Implement OpenStack-compatible REST API that translates to our internal services | Full control, selective compatibility, simpler ops | Must implement and maintain API compatibility | **Our use case: leverage OpenStack ecosystem without full deployment** |
| **Hybrid: OpenStack for VMs, custom for bare-metal** | OpenStack manages VM lifecycle, custom service for bare-metal | Leverages OpenStack's VM maturity | Two control planes to maintain, coordination complexity | Orgs with large existing VM fleet + new bare-metal needs |

**Selected Approach:** API facade. Our Java services implement OpenStack-compatible REST API endpoints (Nova, Neutron, Cinder, Keystone, Glance) backed by our own data model and provisioning logic. We test compatibility against the OpenStack Tempest test suite.

**Implementation:**

```java
// Nova-compatible server create endpoint
@RestController
@RequestMapping("/compute/v2/{tenantId}/servers")
public class NovaCompatController {
    
    @PostMapping
    public ResponseEntity<Map<String, Object>> createServer(
            @PathVariable String tenantId,
            @RequestBody NovaCreateServerRequest request,
            @RequestHeader("X-OpenStack-Nova-API-Version") String microversion) {
        
        // Map Nova request to internal model
        InstanceRequest internal = NovaRequestMapper.toInternal(request, microversion);
        internal.setTenantId(tenantId);
        
        // Handle microversion differences
        if (MicroversionUtil.isAtLeast(microversion, "2.67")) {
            // Microversion 2.67 added block_device_mapping_v2 with volume_type
            internal.setBlockDeviceMappings(
                NovaRequestMapper.mapBlockDevicesV2(request));
        }
        
        // Create instance via internal service
        Instance instance = computeService.createInstance(internal);
        
        // Map internal response to Nova format
        Map<String, Object> novaResponse = NovaResponseMapper.toNova(instance, microversion);
        
        return ResponseEntity.status(HttpStatus.ACCEPTED)
            .header("X-OpenStack-Nova-API-Version", microversion)
            .body(Map.of("server", novaResponse));
    }
}

// OpenStack Keystone token validation filter
@Component
public class KeystoneTokenFilter extends OncePerRequestFilter {
    
    @Override
    protected void doFilterInternal(HttpServletRequest request,
                                      HttpServletResponse response,
                                      FilterChain chain) throws ... {
        String token = request.getHeader("X-Auth-Token");
        if (token == null) {
            response.setStatus(401);
            return;
        }
        
        // Validate against our identity service (Keystone-compatible)
        TokenInfo tokenInfo = identityService.validateToken(token);
        if (tokenInfo == null || tokenInfo.isExpired()) {
            response.setStatus(401);
            return;
        }
        
        // Set security context
        SecurityContextHolder.getContext().setAuthentication(
            new KeystoneAuthentication(tokenInfo));
        
        chain.doFilter(request, response);
    }
}
```

**Interviewer Deep-Dive Q&A:**

Q: How do you handle OpenStack API microversions?
A: Microversions are backward-compatible API extensions. The client sends `X-OpenStack-Nova-API-Version: 2.67` to request a specific behavior. Our facade maintains a registry of microversion changes and conditionally includes/excludes fields in request/response mapping. We support microversions from 2.1 (minimum) to 2.90 (latest we have implemented). Unknown microversions above our maximum return 406 Not Acceptable. We run the OpenStack Tempest test suite nightly against our facade to verify compatibility.

Q: What if OpenStack releases a new microversion that changes behavior we have not implemented?
A: We cap our supported microversion at the latest we have tested. Clients requesting a higher version get a 406 response with a header indicating our maximum supported version. We track upstream OpenStack releases and implement new microversions on a quarterly cadence. Critical security-related microversions are fast-tracked.

Q: How do you handle the Terraform OpenStack provider?
A: The Terraform OpenStack provider uses the standard OpenStack APIs (Nova, Neutron, Cinder). Since our facade is API-compatible, Terraform works without modification. We maintain a Terraform module library with pre-tested configurations for common patterns (GPU instance + network + volume). We also provide a native Terraform provider (`terraform-provider-iaas`) for platform-specific features not in OpenStack's API.

Q: How do you handle OpenStack's "polling for status" pattern versus webhooks?
A: OpenStack's native pattern is polling (`GET /servers/{id}` until status = `ACTIVE`). We support this for compatibility. Additionally, we offer a webhook/callback extension (`X-Callback-URL` header on create requests) that POSTs to the caller when the operation completes. For internal services, we publish events to Kafka. The CLI supports `--wait` flag that polls under the hood with exponential backoff.

Q: What is the hardest part of maintaining OpenStack compatibility?
A: Error response format. OpenStack services have inconsistent error formats across projects (Nova returns `{"error": {"message": "...", "code": 400}}`, Neutron returns `{"NeutronError": {"message": "...", "type": "...", "detail": "..."}}`). Getting these exactly right for every error case is tedious and critical for client library compatibility. We maintain a comprehensive error mapping layer and validate against Tempest tests.

Q: How do you handle OpenStack service discovery (Keystone catalog)?
A: Keystone's service catalog (`GET /v3/auth/catalog`) returns endpoints for all services. Our identity service returns a catalog pointing to our API Gateway's service-specific paths. For example, the Nova entry points to `https://api.iaas.internal/compute/v2`, Neutron to `https://api.iaas.internal/network/v2`. This is transparent to clients using the standard OpenStack SDK.

---

## 7. Scheduling & Resource Management

### Hierarchical Resource Tracking

```
Region (us-east-1)
  ├── AZ (us-east-1a) ─── Cell A
  │     ├── Rack R001
  │     │     ├── Server S001 [CPU: 128/64 used, RAM: 512GB/256GB used, GPU: 8/4 used]
  │     │     ├── Server S002 [CPU: 128/96 used, RAM: 512GB/480GB used, GPU: 8/8 used]
  │     │     └── ...
  │     ├── Rack R002
  │     └── ...
  │     Aggregate capacity: CPU: 50,000 / 35,000 used (70%)
  ├── AZ (us-east-1b) ─── Cell B
  └── AZ (us-east-1c) ─── Cell C
```

Resource tracking is hierarchical: each host reports its own capacity. AZ/region aggregates are computed in real-time by the capacity service (reading from Redis cache, refreshed every 10 seconds from MySQL).

### Quota Enforcement

Two-phase quota reservation (to handle concurrent requests):

```sql
-- Phase 1: Reserve quota (before scheduling)
UPDATE quotas 
SET reserved = reserved + ? 
WHERE tenant_id = ? AND resource_type = 'vcpus' 
  AND (in_use + reserved + ?) <= quota_limit;
-- Check affected rows: 0 means quota exceeded

-- Phase 2: Commit quota (after successful placement)
UPDATE quotas 
SET in_use = in_use + ?, reserved = reserved - ?
WHERE tenant_id = ? AND resource_type = 'vcpus';

-- Phase 2 (failure): Release reservation
UPDATE quotas 
SET reserved = reserved - ?
WHERE tenant_id = ? AND resource_type = 'vcpus';
```

This prevents two concurrent requests from both seeing "within quota" and both succeeding, exceeding the actual limit.

### Conflict Detection for Bare-Metal Reservations

Delegated to the Bare-Metal Reservation Platform (see bare_metal_reservation_platform.md). The IaaS Compute Service calls the Reservation Platform's API when the instance type is `bare_metal`.

---

## 8. Scaling Strategy

### Horizontal Scaling

- **Cell architecture**: Each cell (one per AZ) scales independently. A cell manages ~5,000 hosts. If an AZ grows beyond 5,000 hosts, we split it into two cells.
- **Service replicas per cell**: Compute Service: 4-8 replicas. Network Service: 2-4 replicas. Storage Service: 2-4 replicas.
- **Global services**: API Gateway, Identity Service, and the super-scheduler are global, deployed across all AZs, and scaled to handle aggregate traffic.

### Database Scaling

- **Per-cell MySQL**: Each cell has its own MySQL primary-replica pair. This provides natural sharding by AZ.
- **Global MySQL**: Identity, quotas, and billing data are in a global MySQL instance (replicated across regions).
- **Connection pooling**: HikariCP, 30 connections per service replica.

### Caching

| Layer | What to cache | Strategy | Tool | Eviction | TTL | Invalidation |
|-------|---------------|----------|------|----------|-----|--------------|
| Host capacity | Used/free resources per host | Write-through | Redis | N/A | 10s TTL | Updated on every instance create/delete |
| Instance status | Current state per instance | Cache-aside | Redis | LRU | 30s | Kafka event on state change |
| Flavor list | Immutable flavor definitions | Cache-aside | Local (Caffeine) | N/A | 5 min | Admin changes rare; TTL sufficient |
| Image metadata | Image name, size, checksum | Cache-aside | Redis | LRU | 10 min | Invalidated on image upload/delete |
| Token validation | Keystone token -> project/role mapping | Cache-aside | Local (Caffeine) | LRU | Token expiry time | Token revocation list checked |
| Quota counters | Per-tenant resource usage | Write-through | Redis | N/A | N/A | Atomic updates on resource create/delete |

### Kubernetes-Specific

All control plane services run on a management Kubernetes cluster (separate from tenant workloads):

- **Pod disruption budgets**: `minAvailable: 50%` for all services. Ensures rolling updates do not take down more than half the replicas.
- **Resource quotas per namespace**: Each cell's services run in a dedicated namespace with resource quotas to prevent one cell from consuming all management cluster resources.
- **Priority classes**: Control plane services get `system-cluster-critical` priority, ensuring they are not evicted by lower-priority workloads on the management cluster.

**Interviewer Deep-Dive Q&A:**

Q: How do you handle a full region failure?
A: Each region is fully independent. If us-east-1 goes down entirely: (1) Global DNS (Route 53 health checks) stops routing to us-east-1 endpoints. (2) Users are redirected to their secondary region. (3) Resource state for us-east-1 is unavailable until the region recovers (we do not cross-region replicate instance state -- it would add complexity without benefit since the instances themselves are in the failed region). (4) Global services (identity, billing) are multi-region active-active, so authentication continues working.

Q: How do you handle the "noisy neighbor" problem at the hypervisor level?
A: (1) **CPU pinning** for performance-sensitive instances (GPU, real-time): vCPUs are pinned to specific physical cores, eliminating CPU contention. (2) **NUMA-aware placement**: Large instances are placed respecting NUMA topology to avoid cross-socket memory access. (3) **IO throttling**: Ceph RBD QoS limits IOPS/bandwidth per volume. (4) **Network QoS**: OVS traffic shaping per tenant. (5) **No overcommit for GPU hosts**: Eliminates GPU contention entirely.

Q: How do you upgrade the hypervisor software without disrupting running VMs?
A: Live migration. (1) Select a target host with available capacity. (2) Live-migrate VMs off the source host (QEMU live migration: memory pages copied iteratively, then a brief pause for final sync -- typically < 100ms downtime). (3) Once the source host is empty, upgrade its hypervisor software. (4) Re-enable the host. (5) Repeat for next host (rolling upgrade). For bare-metal instances, there is no live migration -- we schedule maintenance during the next reservation gap.

Q: How do you handle storage backend (Ceph) scaling?
A: Ceph scales by adding OSDs (Object Storage Daemons). Each OSD is a disk. Adding new storage nodes triggers automatic rebalancing (CRUSH algorithm redistributes data). We limit rebalancing rate (`osd_max_backfill`) to avoid impacting live IO. Storage capacity is monitored at 70% utilization threshold; new nodes are provisioned at 75%. We maintain separate Ceph pools for different performance tiers: `nvme-pool` (NVMe SSDs, high IOPS), `ssd-pool` (SAS SSDs, balanced), `hdd-pool` (HDDs, capacity).

Q: How does metering scale with 500K instances reporting hourly?
A: 500K instances x 24 reports/day = 12M metering events/day. Each event is ~200 bytes. Total: ~2.4 GB/day. Kafka ingests this easily (a 3-broker cluster handles 100K messages/sec). The metering aggregation service consumes events and writes hourly summaries to MySQL (partitioned by month). Raw events are retained in Kafka for 7 days for reprocessing, then archived to object storage. Billing queries read from the aggregated summaries table, not raw events.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios & Mitigations

| Failure | Blast Radius | Detection | Mitigation | RTO | RPO |
|---------|-------------|-----------|------------|-----|-----|
| Cell MySQL primary fails | One AZ cannot create/modify instances | MySQL orchestrator | Auto-failover to semi-sync replica | 30s | 0 |
| Hypervisor crash | VMs on that host go down | Libvirt/heartbeat miss | Auto-evacuate VMs to other hosts (if HA enabled); page on-call | 2-5 min (evacuation) | 0 (Ceph-backed root disk survives) |
| Ceph OSD failure | Degraded IO for affected placement groups | Ceph health checks | Ceph auto-replicates to restore redundancy (3x replication) | 0 (transparent) | 0 |
| SDN controller down | Cannot create/modify networks | Health check | SDN controller is 3-node HA cluster; leader re-election | 10s | 0 |
| Kafka broker failure | Event delivery delayed | Kafka broker health | 3-broker cluster with replication factor 3; partition leader re-election | 10s | 0 |
| API Gateway failure | API unreachable | Load balancer health check | Multiple gateway instances; LB removes failed instance | 5s | N/A |
| Full AZ power outage | All instances in AZ down | External monitoring | DR: workloads re-created in other AZs (if user has anti-affinity + Ceph cross-AZ replication). Most users manually recover. | Hours | Depends on replication config |

### Automated Recovery

**VM auto-evacuation (HA instances):**
When a hypervisor fails (missed 3 consecutive heartbeats over 45 seconds), the compute service:
1. Marks all instances on that host as `ERROR`.
2. For instances with `ha_enabled: true`: attempts to boot them on a different host using the same Ceph-backed root disk.
3. Sends STONITH (Shoot The Other Node In The Head) command via IPMI to force-fence the failed hypervisor, preventing split-brain.
4. Notifies affected tenants.

**Metering catch-up:**
If the metering pipeline falls behind (Kafka consumer lag > 10,000), a catch-up consumer spins up with higher throughput (larger batch size, more parallelism). Missing metering data is reconstructed from instance state snapshots in MySQL.

### Retry Strategy
Same as bare-metal platform: exponential backoff with jitter, circuit breakers per external dependency.

---

## 10. Observability

### Key Metrics

| Metric | Type | Tool | Alert Threshold | Business Impact |
|--------|------|------|-----------------|-----------------|
| `instance.create.latency` | Histogram | Prometheus | p99 > 2 min (VM), > 20 min (BM) | User experience |
| `instance.create.success_rate` | Counter | Prometheus | < 95% over 5 min | Users cannot create instances |
| `host.utilization.cpu` | Gauge (per host) | Prometheus | > 90% sustained | Over-committed host |
| `host.utilization.ram` | Gauge (per host) | Prometheus | > 85% sustained | Memory pressure |
| `ceph.health_status` | Gauge | Prometheus | != HEALTH_OK | Storage degradation |
| `ceph.osd.utilization` | Gauge (per OSD) | Prometheus | > 80% | Need to add storage |
| `cell.capacity.remaining` | Gauge (per cell, per resource) | Prometheus | < 10% | Capacity planning trigger |
| `api.error_rate` | Counter (per service) | Prometheus | > 1% 5xx | Service degradation |
| `quota.utilization` | Gauge (per tenant) | Prometheus | > 90% | Tenant about to hit limit |

### Distributed Tracing
OpenTelemetry traces span: API Gateway -> Service -> Scheduler -> Hypervisor Agent -> Ceph. Sampling: 100% for errors, 5% for success (high volume).

### Logging
Structured JSON, aggregated via Filebeat -> Kafka -> Elasticsearch -> Kibana. Each log entry includes: tenant_id, instance_uuid, host_id, trace_id.

---

## 11. Security

### Multi-Tenant Isolation

| Layer | Isolation Mechanism |
|-------|-------------------|
| Compute | Hypervisor (KVM) enforces CPU/memory isolation; bare-metal is physically isolated |
| Network | VXLAN VNI per tenant; security groups (OVS firewall rules); no default cross-tenant traffic |
| Storage | Ceph RADOS namespaces per tenant; volume encryption with per-tenant keys |
| API | JWT tenant_id claim enforced; all queries scoped by tenant_id; no cross-tenant resource access |
| Management | BMC network isolated from tenant network; only provisioning workers can reach BMC |

### Authentication & Authorization
- Keystone-compatible token auth for API access.
- mTLS between all internal services (Istio).
- Service accounts for automated systems (provisioning workers, metering pipeline).
- RBAC with roles: `member` (create/manage own resources), `admin` (manage all resources in project), `cloud_admin` (global operations).

### Secrets Management
- HashiCorp Vault for all secrets.
- Ceph client keyrings rotated monthly.
- Database credentials: Vault dynamic secrets (1-hour TTL).
- Instance user-data encrypted at rest (AES-256) if it contains sensitive data (user opt-in).

### Network Security
- All external API traffic: TLS 1.3.
- Internal service mesh: mTLS via Istio.
- BMC/IPMI network: air-gapped from tenant data plane (separate physical switches or strict VLAN isolation).
- Security groups default-deny; users must explicitly open ports.

---

## 12. Incremental Rollout Strategy

### Per-Cell Rolling Upgrade

1. Select the smallest cell (fewest instances) as the canary.
2. Deploy new version to the canary cell's services.
3. Monitor for 24 hours: error rate, latency, instance creation success rate.
4. If metrics are green, deploy to the next cell. Repeat until all cells are upgraded.
5. Global services (API Gateway, Identity) upgraded last, since they affect all cells.

### Database Migrations

- All schema changes are backward-compatible (add column with default, never remove/rename).
- Applied via gh-ost for online DDL.
- New code deployed first (can handle old and new schema), then migration applied, then old-schema code path removed in the next release.

### OpenStack API Version Rollout

- New microversions are additive (never break old behavior).
- Feature flagged per tenant for testing.
- Old microversion behavior preserved indefinitely (backward compatibility guarantee).

**Rollout Q&A:**

Q: How do you upgrade the SDN controller without disrupting network connectivity?
A: The SDN controller (OVN) is a 3-node cluster. We upgrade one node at a time (rolling restart). OVN uses Raft consensus; losing one node temporarily does not affect operations. During the upgrade, no new network configurations are applied (queued for ~30 seconds). Existing network flows are handled by the local OVS on each compute host (data plane is independent of control plane). We verify OVN cluster health after each node upgrade before proceeding to the next.

Q: How do you handle a hypervisor kernel upgrade?
A: Live-migrate all VMs off the host -> upgrade kernel -> reboot -> run health checks -> re-enable host. For hosts with >50 VMs, we pre-schedule migrations over a 30-minute window (5 concurrent migrations to avoid network saturation). This is automated by the maintenance scheduler. For bare-metal hosts with active reservations, we wait for the reservation to end (or negotiate an early termination with the tenant for urgent security patches).

Q: How do you roll out a new Ceph version?
A: Ceph supports rolling upgrades (monitors first, then OSDs, then clients). (1) Upgrade Ceph monitors one at a time (3-node Raft cluster). (2) Upgrade OSDs one at a time (Ceph rebalances around the temporarily-down OSD). We limit to 5% of OSDs upgrading simultaneously to cap the performance impact. (3) Upgrade client libraries on compute hosts (requires service restart, coordinated with live migration). Total upgrade window for 1000 OSD nodes: ~1 week.

Q: How do you add a new AZ (availability zone) to an existing region?
A: (1) Provision physical infrastructure (racks, servers, network, Ceph storage). (2) Create a new cell for the AZ (new MySQL database, new scheduler, new message broker). (3) Register the cell with the global super-scheduler and API Gateway. (4) Register the AZ in the identity/catalog service. (5) Enable the AZ for a subset of tenants (feature flag) for testing. (6) Gradually open to all tenants. The cell architecture makes this clean: the new AZ is completely independent and cannot affect existing AZs.

Q: How do you test infrastructure changes before production?
A: Three environments: (1) **Dev**: Single-rack, ~20 servers, full stack. For daily development and integration testing. (2) **Staging**: One AZ, ~500 servers, production-like config. For pre-release validation, load testing, chaos testing. (3) **Production canary cell**: The smallest production cell, receiving a fraction of real traffic. Every change passes through dev -> staging -> canary -> full rollout.

---

## 13. Trade-offs & Decision Log

| Decision | Option A | Option B | Option C | Chosen | Specific Reason |
|----------|----------|----------|----------|--------|-----------------|
| Architecture | Monolithic control plane | Microservices per resource type | Cell-based with microservices | **Cell-based** | Combines blast-radius isolation (per-AZ cells) with service modularity. Avoids monolith coupling and cross-AZ dependencies. |
| OpenStack integration | Run full OpenStack | API facade (shim) | Hybrid | **API facade** | Full control over internals while maintaining ecosystem compatibility. Running full OpenStack has excessive operational overhead for our customization needs. |
| VM placement | Random | Least-loaded | Filter-then-weigh | **Filter-then-weigh** | Proven in OpenStack at scale. Extensible with custom filters/weighers. Handles complex constraints (affinity, topology, maintenance). |
| Network overlay | VLAN | VXLAN (OVN) | Contrail | **VXLAN/OVN** | VLAN limited to 4094 IDs; VXLAN supports 16M VNIs. OVN is open-source, well-integrated with OpenStack, and performant. |
| Storage backend | Local disk | Ceph | NetApp / Pure | **Ceph** | Open-source, proven at scale, unified block/object/file. Live migration requires shared storage (local disk cannot support it). |
| Block storage attach protocol | iSCSI | NVMeoF | Ceph RBD direct | **Ceph RBD direct** | Lowest latency (kernel RBD module, no network storage protocol overhead). iSCSI adds unnecessary protocol overhead when both compute and storage are on the same Ceph cluster. |
| Metering | Poll-based (cron scrapes) | Event-driven (Kafka) | Agent-based (collectd) | **Event-driven** | Precise event timing (create/delete), scalable (Kafka handles millions of events), no polling overhead. Agent-based is good for metric collection but not for billing-grade metering. |
| Global DB vs. per-cell DB | One global MySQL | Per-cell MySQL | Global CockroachDB | **Per-cell MySQL** | Avoids cross-AZ write latency. Each cell's data is independent. Global queries (billing, dashboards) are read-only scatter-gather, acceptable at higher latency. CockroachDB adds operational complexity without sufficient benefit at our scale. |

---

## 14. Agentic AI Integration

### Where AI Adds Value

1. **Capacity planning agent**: Analyzes historical usage trends, tenant growth, seasonal patterns to forecast capacity needs per AZ/machine type 4-8 weeks ahead. Generates procurement recommendations with confidence intervals.

2. **Incident diagnosis agent**: When a host fails, the agent correlates logs (Elasticsearch), metrics (Prometheus), and hardware telemetry to identify root cause. "Host S1234 kernel panic at 14:23 UTC. Correlated with DIMM ECC error rate spike starting at 14:15. Root cause: failing DIMM in slot B2. Recommendation: RMA DIMM, live-migrate remaining VMs."

3. **Cost optimization agent**: Identifies underutilized instances ("Instance i-abc has averaged 3% CPU for 30 days. Recommend downsizing from 32-core to 8-core flavor, saving $X/month") and notifies tenants.

4. **Placement optimization agent**: Periodically analyzes current placement and suggests live migrations to improve utilization balance, reduce cross-rack traffic, or prepare for maintenance windows.

### Guard Rails
- Read-only queries: unrestricted.
- Live migration recommendations: require human approval for production instances.
- Capacity procurement recommendations: require manager approval (financial impact).
- Instance termination (cost optimization): never automated -- recommendation only.

---

## 15. Complete Interviewer Q&A Bank

**Q: How does your IaaS platform handle a "thundering herd" of 10,000 simultaneous instance creation requests (e.g., autoscaling event)?**
A: (1) API Gateway rate-limits per tenant (e.g., 100 creates/sec). (2) Requests are accepted asynchronously (202 Accepted) and queued in Kafka per cell. (3) Each cell's compute service processes requests at its sustainable rate (~50 creates/sec for VM, ~10/sec for bare-metal). (4) The scheduler spreads load across hosts. (5) If a single tenant's request exceeds quota, immediate rejection. (6) For legitimate massive scaling (e.g., 1000 instances for ML training), we offer a "bulk create" API that optimizes scheduling (batch placement, parallelized provisioning).

**Q: How do you prevent resource leaks (orphaned VMs, unattached volumes)?**
A: (1) **Reconciliation job** runs hourly: compares DB state against actual hypervisor state. If a VM exists in DB as `ACTIVE` but the hypervisor reports it as nonexistent, it is marked `ERROR` and on-call is alerted. (2) **Orphan volume scanner**: Identifies volumes in `available` state for > 30 days with no recent attachment. Notifies tenant. After 90 days, archived to cold storage. (3) **Instance TTL**: Optional per-tenant policy that auto-terminates instances after a configurable duration (e.g., dev instances auto-terminate after 7 days). (4) **Cost reports**: Monthly reports per tenant showing all resources and costs, making it easy to spot forgotten resources.

**Q: How does the metadata service (169.254.169.254) work, and how do you secure it?**
A: The metadata service runs on each compute host (deployed as a systemd service or pod). When a VM makes an HTTP request to 169.254.169.254, the hypervisor's network stack (iptables/OVS) intercepts it and routes to the local metadata service. The metadata service identifies the requesting VM by its source IP (unique on the host) and returns instance-specific data: hostname, SSH public keys, user data, network config. Security: (1) Only the instance's own metadata is served (source IP verification). (2) Sensitive user data is optionally encrypted. (3) No cross-instance metadata access is possible due to network isolation.

**Q: How do you handle live migration of a VM with a GPU?**
A: You generally cannot live-migrate GPU-passthrough VMs because the GPU state is not transferable between hosts. Options: (1) **Cold migration**: Stop the VM, move the root disk (already on Ceph, so just update the placement), start on a new host with a different GPU. Downtime: 1-5 minutes. (2) **Application-level migration**: The user checkpoints their workload, creates a new instance, resumes from checkpoint. We provide tooling to automate this. (3) **vGPU (NVIDIA GRID/MIG)**: If using virtual GPUs instead of passthrough, NVIDIA supports vGPU live migration (since vGPU 14.0), but with caveats (same GPU model required on target). For bare-metal GPU instances: no migration at all -- we wait for the reservation to end.

**Q: How does billing work when a VM is stopped vs. running?**
A: Running VMs (`ACTIVE`): billed for CPU + RAM + storage + network. Stopped VMs (`SHUTOFF`): billed for storage only (root disk still exists on Ceph). This incentivizes users to terminate instances they are not using, rather than just stopping them. GPU instances: billed at full rate even when stopped (because the GPU is exclusively reserved and cannot be used by others while the VM exists). We emit `instance.start` and `instance.stop` metering events, and the billing pipeline calculates charges based on the time in each state.

**Q: How do you test the OpenStack compatibility layer?**
A: We run the OpenStack Tempest test suite (the official integration test suite for OpenStack clouds) against our facade. Tempest covers ~2000 API test cases across compute, network, storage, and identity. We run this in CI on every PR and nightly against staging. We also maintain a compatibility matrix showing which OpenStack features we support, partially support, or do not support. The Terraform OpenStack provider's acceptance tests are also part of our CI.

**Q: How do you handle clock skew across 100,000 servers?**
A: All servers run NTP (chrony) synchronized to internal NTP servers. Target accuracy: < 10ms. For critical operations (metering timestamps, token expiry, reservation time windows), we use server-side timestamps (the service generates the timestamp, not the client). For operations where ordering matters (audit logs), we use hybrid logical clocks (HLC) that combine physical time with a logical counter to ensure total ordering even under clock skew. MySQL's `NOW()` function uses the database server's clock, which is NTP-synchronized.

**Q: How do you handle the "split-brain" scenario where a hypervisor host appears down from the control plane but is still running VMs?**
A: This is the classic fencing problem. Before evacuating VMs to another host, we MUST ensure the original host is truly dead (otherwise, the same VM runs on two hosts simultaneously, causing data corruption). STONITH (Shoot The Other Node In The Head): (1) Send IPMI power-off command to the suspected-down host. (2) Wait for BMC to confirm power off. (3) Only then, evacuate VMs. If IPMI is also unreachable (e.g., management network partition), we do NOT auto-evacuate -- we alert on-call for manual intervention. False positive auto-evacuation is worse than delayed recovery.

**Q: What is the most expensive mistake an operator can make on this platform?**
A: Accidentally terminating a large number of production instances (e.g., deleting all instances in an AZ). Mitigations: (1) **Termination protection**: Instances can be flagged as `termination_protected`, requiring an explicit two-step process to delete. (2) **Soft delete**: `DELETE` sets `deleted_at` but retains the record and root disk for 24 hours. `PURGE` (admin-only) permanently deletes. (3) **Breakglass for bulk operations**: Any API call affecting > 10 instances requires MFA re-authentication and a 5-minute delay (cancelable). (4) **Audit alerts**: Bulk deletions (> 5 instances in 1 minute by same user) trigger a real-time Slack alert to the security channel.

**Q: How do you handle IPv4 exhaustion for tenant networks?**
A: (1) **Private address space**: Tenants use RFC 1918 addresses (10.0.0.0/8) for their virtual networks. Overlapping is fine because each tenant's network is isolated by VXLAN. (2) **Floating IPs (public)**: We maintain a pool of public IPv4 addresses. When the pool runs low, we add more from our allocation or use CGNAT for non-production workloads. (3) **IPv6**: All new deployments support dual-stack. We encourage IPv6-only for internal service-to-service traffic. (4) **NAT gateway**: Tenants can share a small number of public IPs via a managed NAT gateway for outbound traffic.

**Q: How does the storage service handle volume encryption?**
A: (1) **Encryption at rest**: Ceph RBD supports LUKS encryption per volume. The encryption key is stored in Barbican (OpenStack key manager) or HashiCorp Vault. (2) **Key hierarchy**: Per-volume Data Encryption Key (DEK), encrypted by a per-tenant Key Encryption Key (KEK), stored in Vault. (3) **Performance**: LUKS encryption on modern CPUs with AES-NI has < 5% throughput overhead. (4) **Key rotation**: Re-encrypting a volume with a new DEK is an offline operation (requires volume detach). KEK rotation does not require data re-encryption (just re-wrap the DEK).

**Q: How do you size the management Kubernetes cluster that runs the control plane?**
A: The management cluster runs: 5 regions x 3 AZs = 15 cells, each with ~5 services x 4 replicas = ~300 pods, plus global services (~50 pods), plus observability stack (~30 pods). Total: ~380 pods. At 2 CPU + 4 GB RAM per pod average, we need: ~760 CPU cores, ~1.5 TB RAM. We run ~40 management nodes (16 cores, 64 GB RAM each), leaving 50% headroom for spikes. The management cluster is spread across 3 AZs with topology spread constraints to survive an AZ failure.

**Q: How do you perform capacity planning for this platform?**
A: (1) **Utilization tracking**: Current utilization by resource type (CPU, RAM, GPU, storage) per AZ, updated in real-time dashboards. (2) **Growth modeling**: Linear regression on 90-day utilization trend, extrapolated 6 months. (3) **Lead time accounting**: GPU servers take 8-12 weeks to procure. Storage nodes take 4-6 weeks. We trigger procurement when projected utilization hits 75% of capacity, factoring in lead time. (4) **Burst buffer**: Each AZ maintains 10-15% spare capacity for burst workloads. (5) **AI forecasting agent**: Supplements trend analysis with knowledge of upcoming projects (from team roadmaps ingested from Jira) to predict step-function demand increases.

**Q: How do you ensure data durability for block volumes?**
A: Ceph RBD with replication factor 3 (each data chunk stored on 3 different OSDs, on 3 different hosts, ideally in 3 different racks). Durability: 99.999999999% (11 nines). Additional protections: (1) Scrubbing: Ceph continuously verifies data integrity (checksums). Corrupted replicas are auto-repaired from healthy copies. (2) Snapshots: Automated daily snapshots with 7-day retention (user-configurable). (3) Cross-AZ backup: Users can optionally enable cross-AZ volume backup (async replication to another Ceph cluster in a different AZ). RPO: 15 minutes.

---

## 16. References

- "Nova Scheduler Architecture" -- OpenStack documentation. Filter-scheduler, weigher plugins, cell architecture.
- "OVN Architecture" -- Open Virtual Network documentation. Logical switches, routers, distributed gateway.
- "Ceph: A Scalable, High-Performance Distributed File System" -- Weil et al., OSDI 2006. CRUSH algorithm, RADOS object store.
- "OpenStack Ironic (Bare Metal Provisioning)" -- OpenStack documentation. PXE boot, IPMI integration, cleaning.
- "Borg, Omega, and Kubernetes" -- Google, ACM Queue 2016. Cell-based architecture, scheduler design.
- "Large-scale cluster management at Google with Borg" -- Verma et al., EuroSys 2015. Quota management, preemption, cell architecture.
- "Designing Data-Intensive Applications" by Martin Kleppmann -- Chapters 5-9. Replication, partitioning, transactions, consistency.
- "Vitess: Database Clustering System for Horizontal Scaling of MySQL" -- PlanetScale / YouTube Engineering. Connection pooling, resharding.
- "The OpenStack Tempest Integration Test Suite" -- OpenStack QA documentation. API compatibility testing.
- "SPIFFE/SPIRE" -- Secure Production Identity Framework for Everyone. Service identity for mTLS.
- "Building Secure and Reliable Systems" -- Google SRE. Chapter on STONITH, fencing, split-brain prevention.
