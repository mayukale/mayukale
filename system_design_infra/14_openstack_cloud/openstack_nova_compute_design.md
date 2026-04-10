# System Design: OpenStack Nova Compute — VM Scheduling and Lifecycle

> **Relevance to role:** Nova is the core compute service in OpenStack. As a cloud infrastructure platform engineer, you will own the entire VM lifecycle — from API request through scheduling, placement, hypervisor provisioning, and eventual teardown. Deep understanding of nova-scheduler filters/weights, the Placement API resource model, nova-conductor's role in protecting database integrity, and hypervisor driver abstractions is essential for operating a production cloud at scale.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Users can create, start, stop, reboot, resize, rebuild, and delete virtual machine instances via REST API |
| FR-2 | The scheduler places VMs on compute hosts based on resource availability, affinity/anti-affinity rules, and availability zones |
| FR-3 | The Placement API tracks inventories of VCPU, MEMORY_MB, DISK_GB, and custom resource classes per compute host (resource provider) |
| FR-4 | Users can attach/detach volumes (Cinder), network interfaces (Neutron), and floating IPs to running instances |
| FR-5 | VM lifecycle states are tracked and exposed: BUILD, ACTIVE, SHUTOFF, PAUSED, SUSPENDED, RESCUED, ERROR, DELETED |
| FR-6 | Live migration and cold migration between compute hosts are supported |
| FR-7 | Server groups with affinity and anti-affinity policies constrain placement |
| FR-8 | Flavors define resource templates (vcpus, ram, disk, extra_specs for NUMA, CPU pinning, huge pages, GPU passthrough) |
| FR-9 | Instance snapshots create Glance images from running or stopped VMs |
| FR-10 | Quota enforcement per project limits VCPU, RAM, instances, and other resources |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | API latency for instance create (acceptance) | < 500 ms |
| NFR-2 | Time from API call to VM booting (end-to-end provisioning) | < 60 s for standard flavors |
| NFR-3 | Scheduler throughput | 500+ placement decisions / second |
| NFR-4 | Control plane availability | 99.99% |
| NFR-5 | No single point of failure in any Nova service | Active-active or active-passive HA |
| NFR-6 | Zero data loss on controller failure | Synchronous DB replication |
| NFR-7 | Support for 10,000+ compute hosts per cell | Cells v2 architecture |

### Constraints & Assumptions
- OpenStack release: 2024.2 (Dalmatian) or later
- Hypervisor: libvirt/KVM as primary driver; VMware vCenter driver for brownfield environments
- Database: MySQL 8.0 with Galera Cluster for synchronous multi-master replication
- Message bus: RabbitMQ 3.12+ with quorum queues
- Shared storage: Ceph RBD for ephemeral disks (enables live migration without block migration)
- Keystone v3 for authentication and authorization
- Cells v2 is deployed (mandatory since Queens release)

### Out of Scope
- Bare-metal provisioning (Ironic)
- Container orchestration (Zun/Magnum)
- Serverless / FaaS workloads
- Billing and metering (Cloudkitty)
- Detailed Cinder or Neutron internals (covered in other files)

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Estimate | Calculation |
|--------|----------|-------------|
| Total compute hosts | 10,000 | 200 racks x 50 servers/rack |
| VMs per host (avg) | 40 | Assuming 2:1 CPU overcommit, 128 vCPUs physical = 256 vCPUs, avg VM = 4 vCPU |
| Total VMs | 400,000 | 10,000 hosts x 40 VMs |
| VM creates per day | 20,000 | ~5% churn rate on 400K VMs |
| VM creates per second (peak) | 50 | 3x average during burst (20,000/86,400 x 3) |
| API requests per second (total) | 2,000 | List/show operations dominate; 40:1 read:write |
| Scheduler decisions per second (peak) | 150 | 50 creates + resize/migrate operations |
| Projects (tenants) | 5,000 | |
| Users | 20,000 | |

### Latency Requirements

| Operation | Target | Notes |
|-----------|--------|-------|
| POST /servers (API acceptance) | < 500 ms | Returns 202 Accepted with instance UUID |
| Scheduling decision | < 200 ms | Filter + weigh candidates |
| Placement API allocation claim | < 50 ms | Single DB transaction |
| Full boot (API to SSH-ready) | < 60 s | Image download cached; DHCP + cloud-init |
| Live migration (4 GB RAM VM) | < 10 s | Pre-copy with < 100 ms downtime |
| GET /servers (list, 100 items) | < 200 ms | Paginated |
| GET /servers/{id} | < 50 ms | Single row lookup |

### Storage Estimates

| Data | Size | Calculation |
|------|------|-------------|
| Instance records in DB | ~400 MB | 400K instances x ~1 KB per row |
| instance_extra table | ~2 GB | NUMA topology, PCI devices JSON per instance |
| block_device_mapping | ~200 MB | 400K instances x 2 mappings avg x 250 bytes |
| Migrations table | ~500 MB | Historical migration records |
| Total Nova DB | ~10 GB | Including indexes, cell0 + cell DBs |
| RabbitMQ message rate | 5,000 msg/s | Nova RPC calls across all services |
| Ephemeral disk storage (Ceph) | 8 PB | 400K VMs x 20 GB avg ephemeral disk |

### Bandwidth Estimates

| Flow | Bandwidth | Calculation |
|------|-----------|-------------|
| API traffic | 200 Mbps | 2,000 req/s x 100 KB avg response (list operations) |
| RPC (RabbitMQ) | 500 Mbps | 5,000 msg/s x 10 KB avg message |
| Live migration (concurrent) | 80 Gbps | 8 concurrent migrations x 10 Gbps each |
| Image download (Glance to compute) | 40 Gbps | 50 creates/s x peak, image cached after first download |
| Ceph replication | 100 Gbps | 3x replication of ephemeral writes |

---

## 3. High Level Architecture

```
                              ┌─────────────────────────────────────┐
                              │          External Clients           │
                              │  (Horizon, CLI, Terraform, APIs)    │
                              └──────────────┬──────────────────────┘
                                             │ HTTPS (port 8774)
                                             ▼
                              ┌──────────────────────────────┐
                              │        HAProxy / VIP          │
                              │   (load balances Nova API)    │
                              └──────────────┬───────────────┘
                                             │
                     ┌───────────────────────┼───────────────────────┐
                     ▼                       ▼                       ▼
              ┌─────────────┐        ┌─────────────┐        ┌─────────────┐
              │  nova-api   │        │  nova-api   │        │  nova-api   │
              │ (instance 1)│        │ (instance 2)│        │ (instance 3)│
              └──────┬──────┘        └──────┬──────┘        └──────┬──────┘
                     │                      │                      │
        ┌────────────┴──────────────────────┴──────────────────────┘
        │
        ├──────────────────────┐
        │                      │
        ▼                      ▼
 ┌──────────────┐    ┌──────────────────┐
 │   Keystone   │    │  Placement API   │
 │  (auth/authz)│    │  (resource       │
 │              │    │   tracking)      │
 └──────────────┘    └────────┬─────────┘
                              │
                              ▼
                     ┌─────────────────┐        ┌──────────────────────┐
                     │  placement DB   │        │    API DB (cell0)    │
                     │  (MySQL/Galera) │        │    (MySQL/Galera)    │
                     └─────────────────┘        └──────────────────────┘

        ┌─────────────────────────────────────────────────────────────┐
        │                     RabbitMQ Cluster                        │
        │          (AMQP 0-9-1, topic exchanges)                     │
        └───────┬──────────────┬──────────────────┬───────────────────┘
                │              │                  │
                ▼              ▼                  ▼
      ┌──────────────┐ ┌──────────────┐  ┌───────────────────┐
      │nova-scheduler│ │nova-conductor│  │  nova-conductor   │
      │  (instance 1)│ │ (instance 1) │  │  (instance 2)     │
      │              │ │              │  │                   │
      └──────────────┘ └──────┬───────┘  └─────┬─────────────┘
                              │                │
                              ▼                ▼
                     ┌─────────────────────────────┐
                     │      Cell DB (cell1)        │
                     │      (MySQL/Galera)         │
                     └─────────────────────────────┘

        ┌─────────────────────────────────────────────────────────────┐
        │                    Compute Hosts                            │
        │                                                             │
        │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
        │  │nova-compute  │  │nova-compute  │  │nova-compute  │ ...  │
        │  │  + libvirt   │  │  + libvirt   │  │  + libvirt   │      │
        │  │  + QEMU/KVM  │  │  + QEMU/KVM  │  │  + QEMU/KVM  │      │
        │  └──────────────┘  └──────────────┘  └──────────────┘      │
        │                                                             │
        └─────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **nova-api** | Stateless REST API front-end. Validates requests, checks Keystone tokens, enforces quotas, writes instance records to cell0 DB, sends RPC cast to nova-conductor for build requests. Runs N instances behind HAProxy. |
| **nova-scheduler** | Consumes `select_destinations` RPC calls. Queries Placement API for candidate hosts, applies scheduler filters and weights, returns list of selected hosts. Stateless; multiple instances for HA. |
| **nova-conductor** | Mediates between nova-compute and the database. Nova-compute never accesses the DB directly (security boundary). Handles complex operations: build, resize confirm, live migration orchestration. Multiple instances for HA. |
| **nova-compute** | Runs on every hypervisor host. Manages VM lifecycle via the hypervisor driver (libvirt). Reports resource inventory to Placement. Communicates only via RabbitMQ RPC. |
| **Placement API** | Tracks resource provider inventories, allocations, and traits. Used by the scheduler to find candidate hosts. Separate service since Stein release. |
| **Keystone** | Identity service: authenticates users, issues tokens (Fernet), manages projects/domains/roles, enforces RBAC policies. |
| **RabbitMQ** | AMQP message broker for all Nova RPC. Topic exchanges route messages: `nova` exchange with routing keys like `scheduler`, `conductor`, `compute.<hostname>`. |
| **MySQL/Galera** | Relational database backend. Separate DBs: `nova_api` (global), `nova_cell0` (failed instances), `nova_cell1..N` (per-cell instance data), `placement`. |

### Data Flows — VM Create

```
1. Client → nova-api: POST /v2.1/servers
2. nova-api → Keystone: validate token, check RBAC
3. nova-api → nova_api DB: create instance record (vm_state=BUILDING, task_state=SCHEDULING)
4. nova-api → RabbitMQ → nova-conductor: build_instances RPC cast
5. nova-conductor → Placement API: GET /allocation_candidates (filtered by flavor resources)
6. nova-conductor → RabbitMQ → nova-scheduler: select_destinations RPC call
7. nova-scheduler → Placement API: GET /allocation_candidates
8. nova-scheduler: applies filters (AvailabilityZoneFilter, RamFilter, etc.), applies weights
9. nova-scheduler → Placement API: PUT /allocations/{consumer_id} (claim resources)
10. nova-scheduler → nova-conductor: returns selected host
11. nova-conductor → cell DB: update instance record with host assignment
12. nova-conductor → RabbitMQ → nova-compute.<host>: build_and_run_instance RPC cast
13. nova-compute → Glance: download image (or use cached copy)
14. nova-compute → Neutron: allocate port, get VIF details
15. nova-compute → Cinder: attach any pre-created volumes
16. nova-compute → libvirt: define domain XML, start VM
17. nova-compute → cell DB (via conductor): update vm_state=ACTIVE
18. nova-compute → Neutron: bind port to host
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- nova_api database (global, shared across cells)

CREATE TABLE flavors (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    name        VARCHAR(255) NOT NULL UNIQUE,
    vcpus       INT NOT NULL,
    memory_mb   INT NOT NULL,
    root_gb     INT NOT NULL DEFAULT 0,
    ephemeral_gb INT NOT NULL DEFAULT 0,
    swap        INT NOT NULL DEFAULT 0,
    rxtx_factor FLOAT DEFAULT 1.0,
    is_public   BOOLEAN DEFAULT TRUE,
    disabled    BOOLEAN DEFAULT FALSE,
    created_at  DATETIME NOT NULL,
    updated_at  DATETIME
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE flavor_extra_specs (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    flavor_id   INT NOT NULL,
    spec_key    VARCHAR(255) NOT NULL,
    spec_value  VARCHAR(255) NOT NULL,
    FOREIGN KEY (flavor_id) REFERENCES flavors(id) ON DELETE CASCADE,
    UNIQUE KEY (flavor_id, spec_key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
-- Example extra_specs:
--   hw:cpu_policy=dedicated
--   hw:numa_nodes=2
--   hw:mem_page_size=1048576
--   pci_passthrough:alias=gpu-a100:1
--   resources:CUSTOM_GPU_A100=1

CREATE TABLE request_specs (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    instance_uuid   VARCHAR(36) NOT NULL UNIQUE,
    spec            MEDIUMTEXT NOT NULL,  -- JSON blob: flavor, image, scheduler hints
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE instance_mappings (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    instance_uuid   VARCHAR(36) NOT NULL UNIQUE,
    cell_id         INT,
    project_id      VARCHAR(255) NOT NULL,
    user_id         VARCHAR(255),
    queued_for_delete BOOLEAN DEFAULT FALSE,
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    FOREIGN KEY (cell_id) REFERENCES cell_mappings(id),
    INDEX idx_project_id (project_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE cell_mappings (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    uuid            VARCHAR(36) NOT NULL UNIQUE,
    name            VARCHAR(255),
    transport_url   TEXT NOT NULL,         -- RabbitMQ connection for this cell
    database_connection TEXT NOT NULL,     -- MySQL connection for this cell
    disabled        BOOLEAN DEFAULT FALSE,
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE host_mappings (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    cell_id         INT NOT NULL,
    host            VARCHAR(255) NOT NULL UNIQUE,
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    FOREIGN KEY (cell_id) REFERENCES cell_mappings(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Cell database (per-cell, e.g., nova_cell1)

CREATE TABLE instances (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    uuid            VARCHAR(36) NOT NULL UNIQUE,
    user_id         VARCHAR(255),
    project_id      VARCHAR(255),
    image_ref       VARCHAR(36),
    flavor_id       INT,
    display_name    VARCHAR(255),
    display_description TEXT,
    host            VARCHAR(255),           -- compute host
    node            VARCHAR(255),           -- hypervisor hostname
    availability_zone VARCHAR(255),
    vm_state        VARCHAR(255) NOT NULL DEFAULT 'building',
    task_state      VARCHAR(255),
    power_state     INT DEFAULT 0,          -- 0=NOSTATE, 1=RUNNING, 3=PAUSED, 4=SHUTDOWN
    launched_at     DATETIME,
    terminated_at   DATETIME,
    deleted         INT DEFAULT 0,
    deleted_at      DATETIME,
    key_name        VARCHAR(255),
    config_drive    BOOLEAN DEFAULT FALSE,
    locked          BOOLEAN DEFAULT FALSE,
    locked_by       VARCHAR(255),           -- 'owner' or 'admin'
    root_device_name VARCHAR(255) DEFAULT '/dev/vda',
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    INDEX idx_project_id (project_id),
    INDEX idx_host (host),
    INDEX idx_vm_state (vm_state),
    INDEX idx_deleted_created (deleted, created_at),
    INDEX idx_uuid (uuid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE instance_extra (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    instance_uuid   VARCHAR(36) NOT NULL UNIQUE,
    numa_topology   MEDIUMTEXT,             -- JSON: NUMA node assignments
    pci_requests    MEDIUMTEXT,             -- JSON: PCI passthrough requests
    flavor          MEDIUMTEXT,             -- JSON: full flavor at creation time
    vcpu_model      MEDIUMTEXT,             -- JSON: CPU model/flags
    migration_context MEDIUMTEXT,           -- JSON: in-flight migration details
    trusted_certs   MEDIUMTEXT,
    resources       MEDIUMTEXT,             -- JSON: additional resources
    FOREIGN KEY (instance_uuid) REFERENCES instances(uuid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE block_device_mapping (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    instance_uuid       VARCHAR(36) NOT NULL,
    device_name         VARCHAR(255),           -- /dev/vda, /dev/vdb
    source_type         VARCHAR(255),           -- 'image', 'volume', 'snapshot', 'blank'
    destination_type    VARCHAR(255),           -- 'local', 'volume'
    volume_id           VARCHAR(36),
    volume_size         INT,
    snapshot_id         VARCHAR(36),
    image_id            VARCHAR(36),
    boot_index          INT,
    delete_on_termination BOOLEAN DEFAULT FALSE,
    connection_info     MEDIUMTEXT,             -- JSON: iSCSI/RBD target info
    created_at          DATETIME NOT NULL,
    updated_at          DATETIME,
    deleted             INT DEFAULT 0,
    INDEX idx_instance_uuid (instance_uuid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE virtual_interfaces (
    id              INT AUTO_INCREMENT PRIMARY KEY,
    instance_uuid   VARCHAR(36) NOT NULL,
    network_id      VARCHAR(36),
    address         VARCHAR(255),           -- MAC address
    uuid            VARCHAR(36) NOT NULL UNIQUE,
    tag             VARCHAR(255),
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    deleted         INT DEFAULT 0,
    INDEX idx_instance_uuid (instance_uuid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE migrations (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    instance_uuid       VARCHAR(36) NOT NULL,
    source_compute      VARCHAR(255),
    source_node         VARCHAR(255),
    dest_compute        VARCHAR(255),
    dest_node           VARCHAR(255),
    migration_type      ENUM('migration', 'resize', 'live-migration', 'evacuation'),
    status              VARCHAR(255),       -- 'pre-migrating', 'migrating', 'post-migrating', 'finished', 'error', 'reverted'
    old_instance_type_id INT,
    new_instance_type_id INT,
    created_at          DATETIME NOT NULL,
    updated_at          DATETIME,
    INDEX idx_instance_uuid (instance_uuid),
    INDEX idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE compute_nodes (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    uuid                VARCHAR(36) NOT NULL UNIQUE,
    host                VARCHAR(255) NOT NULL,
    hypervisor_hostname VARCHAR(255) NOT NULL,
    hypervisor_type     VARCHAR(255) NOT NULL,      -- 'QEMU', 'VMware vCenter Server'
    hypervisor_version  INT,
    vcpus               INT NOT NULL,
    vcpus_used          INT NOT NULL DEFAULT 0,
    memory_mb           INT NOT NULL,
    memory_mb_used      INT NOT NULL DEFAULT 0,
    local_gb            INT NOT NULL,
    local_gb_used       INT NOT NULL DEFAULT 0,
    running_vms         INT NOT NULL DEFAULT 0,
    current_workload    INT NOT NULL DEFAULT 0,
    disk_available_least INT,
    host_ip             VARCHAR(255),
    forced_down         BOOLEAN DEFAULT FALSE,
    mapped              INT DEFAULT 0,
    created_at          DATETIME NOT NULL,
    updated_at          DATETIME,
    deleted             INT DEFAULT 0,
    INDEX idx_host (host)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Placement database

CREATE TABLE resource_providers (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    uuid        VARCHAR(36) NOT NULL UNIQUE,
    name        VARCHAR(200) NOT NULL,
    generation  INT NOT NULL DEFAULT 0,         -- optimistic concurrency
    parent_provider_uuid VARCHAR(36),           -- for nested providers (NUMA, GPU)
    root_provider_uuid   VARCHAR(36) NOT NULL,
    INDEX idx_parent (parent_provider_uuid),
    INDEX idx_root (root_provider_uuid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE inventories (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    resource_provider_id INT NOT NULL,
    resource_class_id   INT NOT NULL,           -- VCPU=0, MEMORY_MB=1, DISK_GB=2, CUSTOM_*
    total               INT NOT NULL,
    reserved            INT NOT NULL DEFAULT 0,
    min_unit            INT NOT NULL DEFAULT 1,
    max_unit            INT NOT NULL,
    step_size           INT NOT NULL DEFAULT 1,
    allocation_ratio    FLOAT NOT NULL DEFAULT 1.0,  -- overcommit ratio
    UNIQUE KEY (resource_provider_id, resource_class_id),
    FOREIGN KEY (resource_provider_id) REFERENCES resource_providers(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE allocations (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    resource_provider_id INT NOT NULL,
    consumer_id         VARCHAR(36) NOT NULL,   -- instance UUID
    resource_class_id   INT NOT NULL,
    used                INT NOT NULL,
    created_at          DATETIME NOT NULL,
    updated_at          DATETIME,
    INDEX idx_consumer (consumer_id),
    INDEX idx_resource_provider (resource_provider_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE traits (
    id          INT AUTO_INCREMENT PRIMARY KEY,
    name        VARCHAR(255) NOT NULL UNIQUE     -- COMPUTE_STATUS_DISABLED, HW_CPU_X86_AVX2, CUSTOM_*
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE resource_provider_traits (
    resource_provider_id INT NOT NULL,
    trait_id             INT NOT NULL,
    PRIMARY KEY (resource_provider_id, trait_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Database | Justification |
|----------|--------------|
| **MySQL 8.0 + Galera Cluster** | Standard for OpenStack. Galera provides synchronous multi-master replication for HA. All Nova services use SQLAlchemy ORM with oslo.db. Galera's certification-based replication ensures zero data loss on node failure. |
| **Separate databases per cell** | Cells v2 requires separate DB per cell. This isolates failure domains — a cell DB issue affects only that cell's instances. The `nova_api` DB is global and small. |
| **Placement has its own DB** | Placement was extracted from Nova. Its own DB allows independent scaling and prevents contention with Nova cell DBs. |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| instances | (project_id) | List instances by project |
| instances | (host) | List instances on a host (used by nova-compute startup, migration) |
| instances | (vm_state) | Filter by state |
| instances | (deleted, created_at) | Soft-delete aware queries, archive old records |
| instance_mappings | (project_id) | Cross-cell instance lookup |
| instance_mappings | (instance_uuid) | Primary lookup |
| allocations | (consumer_id) | Find all allocations for an instance |
| allocations | (resource_provider_id) | Find all allocations on a host |
| compute_nodes | (host) | Map hostname to compute node record |
| migrations | (instance_uuid, status) | Track active migrations per instance |
| resource_providers | (parent_provider_uuid) | Traverse nested provider tree |

---

## 5. API Design

### REST Endpoints (OpenStack Compute API v2.1)

```
# Instance lifecycle
POST   /v2.1/servers                                    # Create instance
GET    /v2.1/servers                                    # List instances (paginated)
GET    /v2.1/servers/detail                             # List instances with full details
GET    /v2.1/servers/{server_id}                        # Show instance details
PUT    /v2.1/servers/{server_id}                        # Update instance (name, description)
DELETE /v2.1/servers/{server_id}                        # Delete instance

# Instance actions
POST   /v2.1/servers/{server_id}/action
  Body: {"os-start": null}                              # Start
  Body: {"os-stop": null}                               # Stop
  Body: {"reboot": {"type": "SOFT|HARD"}}               # Reboot
  Body: {"pause": null}                                 # Pause (save RAM state)
  Body: {"unpause": null}                               # Unpause
  Body: {"suspend": null}                               # Suspend (save to disk)
  Body: {"resume": null}                                # Resume from suspend
  Body: {"resize": {"flavorRef": "..."}}                # Resize (cold migration)
  Body: {"confirmResize": null}                         # Confirm resize
  Body: {"revertResize": null}                          # Revert resize
  Body: {"os-migrateLive": {"host": "...",              # Live migration
         "block_migration": "auto",
         "force": false}}
  Body: {"createImage": {"name": "...",                 # Snapshot
         "metadata": {...}}}
  Body: {"rescue": {"adminPass": "...",                 # Rescue mode
         "rescue_image_ref": "..."}}
  Body: {"lock": null}                                  # Lock instance
  Body: {"unlock": null}                                # Unlock instance

# Flavors
GET    /v2.1/flavors                                    # List flavors
GET    /v2.1/flavors/{flavor_id}                        # Show flavor details
POST   /v2.1/flavors                                    # Create flavor (admin)
DELETE /v2.1/flavors/{flavor_id}                        # Delete flavor (admin)
GET    /v2.1/flavors/{flavor_id}/os-extra_specs         # List extra specs
PUT    /v2.1/flavors/{flavor_id}/os-extra_specs/{key}   # Set extra spec

# Server groups
POST   /v2.1/os-server-groups                           # Create server group
GET    /v2.1/os-server-groups                           # List server groups
GET    /v2.1/os-server-groups/{group_id}                # Show server group
DELETE /v2.1/os-server-groups/{group_id}                # Delete server group

# Hypervisors (admin)
GET    /v2.1/os-hypervisors                             # List compute nodes
GET    /v2.1/os-hypervisors/{hypervisor_id}             # Show compute node details
GET    /v2.1/os-hypervisors/{hypervisor_id}/servers     # List instances on host

# Migrations (admin)
GET    /v2.1/servers/{server_id}/migrations             # List migrations for instance
GET    /v2.1/os-migrations                              # List all migrations

# Placement API (separate service, port 8778)
GET    /placement/resource_providers                     # List resource providers
GET    /placement/resource_providers/{uuid}/inventories  # Show inventories
GET    /placement/resource_providers/{uuid}/allocations  # Show allocations
GET    /placement/allocation_candidates?resources=VCPU:4,MEMORY_MB:8192,DISK_GB:80
                                                         # Get candidate hosts
PUT    /placement/allocations/{consumer_id}              # Claim allocations
GET    /placement/resource_providers/{uuid}/traits       # Show traits
```

### CLI (openstack CLI commands)

```bash
# Instance lifecycle
openstack server create --flavor m1.large --image ubuntu-22.04 \
  --network private --key-name mykey --availability-zone az1 myvm

openstack server list --project myproject --status ACTIVE
openstack server show <server-id>
openstack server delete <server-id>

# Actions
openstack server start <server-id>
openstack server stop <server-id>
openstack server reboot --hard <server-id>
openstack server pause <server-id>
openstack server suspend <server-id>
openstack server resize <server-id> --flavor m1.xlarge
openstack server resize confirm <server-id>
openstack server migrate --live <dest-host> <server-id>

# Flavors
openstack flavor create --vcpus 4 --ram 8192 --disk 80 m1.large
openstack flavor set m1.large --property hw:cpu_policy=dedicated
openstack flavor set m1.large --property hw:numa_nodes=2
openstack flavor set m1.large --property resources:CUSTOM_GPU_A100=1

# Server groups
openstack server group create --policy anti-affinity my-group
openstack server create --hint group=<group-uuid> --flavor m1.large ...

# Placement (via osc-placement plugin)
openstack resource provider list
openstack resource provider show <uuid>
openstack resource provider inventory list <uuid>
openstack allocation candidate list --resource VCPU=4,MEMORY_MB=8192

# Hypervisors (admin)
openstack hypervisor list
openstack hypervisor show <hypervisor-id>
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Nova Scheduler — Filter and Weight Pipeline

**Why it's hard:**
The scheduler must make optimal placement decisions across 10,000+ hosts in under 200ms, balancing resource utilization, affinity constraints, availability zone requirements, and custom placement rules — all while handling concurrent requests without double-booking resources.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Filter-Weight (current Nova scheduler)** | Proven at scale, extensible filter chain, well-understood | Sequential filter evaluation can be slow with many hosts; doesn't natively handle bin-packing optimization |
| **Placement pre-filter + scheduler** | Placement API pre-filters candidates using SQL (very fast), scheduler only evaluates shortlist | Requires maintaining Placement inventory accuracy; two-step process adds complexity |
| **Centralized solver (LP/ILP)** | Globally optimal placement decisions; handles complex constraints | Too slow for real-time scheduling (NP-hard); overkill for most use cases |
| **Distributed scheduling (Omega-style)** | Each scheduler independently claims resources; no bottleneck | Conflict resolution needed; wastes resources on failed claims; harder to reason about |

**Selected approach:** Placement pre-filter + Filter-Weight scheduler (the current Nova architecture since Stein)

**Justification:** The Placement API handles the heavy lifting of resource matching via SQL queries. The scheduler receives a short-list of candidates (typically 100 from thousands) and applies fine-grained filters and weights. This two-phase approach keeps scheduling under 200ms even at 10,000 hosts.

**Implementation detail:**

```
Phase 1: Placement Pre-Filter
─────────────────────────────
1. Build resource request from flavor:
   resources: VCPU=4, MEMORY_MB=8192, DISK_GB=80
   required traits: HW_CPU_X86_AVX2, COMPUTE_STATUS_ENABLED
   forbidden traits: COMPUTE_STATUS_DISABLED

2. GET /placement/allocation_candidates?
     resources=VCPU:4,MEMORY_MB:8192,DISK_GB:80
     &required=HW_CPU_X86_AVX2,!COMPUTE_STATUS_DISABLED
     &limit=1000

3. Placement returns list of (resource_provider, allocation_request) tuples
   Each tuple includes the exact allocation to claim if selected.

Phase 2: Filter Pipeline
────────────────────────
Filters run in order; each removes unsuitable hosts:

  AvailabilityZoneFilter  → only hosts in requested AZ
  ComputeFilter           → only hosts with enough resources (redundant after Placement, kept for safety)
  RamFilter               → check RAM overcommit (ram_allocation_ratio, e.g. 1.5)
  DiskFilter              → check disk overcommit
  ImagePropertiesFilter   → match image hw requirements (architecture, hypervisor type)
  ServerGroupAntiAffinityFilter → exclude hosts already running members of same anti-affinity group
  ServerGroupAffinityFilter     → only hosts already running members of same affinity group
  AggregateInstanceExtraSpecsFilter → match host aggregates by flavor extra_specs
  NUMATopologyFilter      → validate NUMA topology fit
  PciPassthroughFilter    → check PCI device availability (GPU, NIC SR-IOV)
  ComputeCapabilitiesFilter → match host capabilities to flavor extra_specs
  IoOpsFilter             → limit concurrent I/O operations per host

Phase 3: Weight Pipeline
────────────────────────
Remaining hosts scored by weighers:

  RAMWeigher              → prefer hosts with most free RAM (spread) or least (stack)
  DiskWeigher             → prefer hosts with most free disk
  MetricsWeigher          → custom metrics from compute hosts
  ServerGroupSoftAffinityWeigher → prefer hosts with group members
  CrossCellWeigher        → prefer same cell (if cross-cell resize)
  BuildFailureWeigher     → penalize hosts with recent build failures

Final score = Σ (weigher_score × weigher_multiplier) / max_score
Top host selected. Placement allocation claimed atomically.
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| No valid host found | Instance goes to ERROR state | RetryFilter allows retries on alternate hosts; Placement re-queried |
| Placement allocation race (two schedulers claim same capacity) | One claim fails (generation conflict) | Retry with new candidate; Placement uses optimistic locking (generation field) |
| Scheduler crashes mid-decision | Orphaned request_spec | nova-conductor detects timeout, retries or marks ERROR |
| Stale Placement data | Overcommit beyond desired ratio | nova-compute periodically updates inventory (every 60s); healing audit |
| Filter bug removes all candidates | No valid host | Monitor filter pass/fail rates; canary new filter configurations |

**Interviewer Q&As:**

**Q1: How does Nova prevent two schedulers from placing VMs on the same host and overcommitting it?**
A: The Placement API uses optimistic concurrency control via a `generation` field on resource providers. When the scheduler claims allocations, it includes the generation it read. If another scheduler already modified the provider (incrementing the generation), the claim fails with a 409 Conflict. The scheduler then retries with fresh allocation candidates. This is similar to a compare-and-swap operation.

**Q2: What happens if a compute host goes down and its resource inventory becomes stale in Placement?**
A: Nova-compute periodically reports its inventory to Placement (default: every 60 seconds via the `update_available_resource` periodic task). If the compute host goes down, the inventory becomes stale. Operators can mark the host as forced_down via the API, which sets the `COMPUTE_STATUS_DISABLED` trait, causing Placement to exclude it from candidates. Additionally, the `ComputeFilter` checks the service status. For long-term down hosts, operators run `nova-manage cell_v2 delete_host` to clean up.

**Q3: How does the scheduler handle requests for custom resources like GPUs?**
A: GPUs are modeled as custom resource classes in Placement (e.g., `CUSTOM_GPU_NVIDIA_A100`). Flavors specify them via extra_specs: `resources:CUSTOM_GPU_NVIDIA_A100=1`. Placement uses nested resource providers: the GPU is a child provider of the compute host. Allocation candidates include both the parent (VCPU, MEMORY_MB) and child (CUSTOM_GPU_NVIDIA_A100). The `PciPassthroughFilter` in the scheduler additionally validates PCI device availability on the host.

**Q4: Can you change scheduler behavior without restarting services?**
A: The scheduler reads filter and weigher configuration from nova.conf. Changing `enabled_filters` or `weight_classes` requires a service restart. However, host aggregates and their metadata can be modified at runtime — the `AggregateInstanceExtraSpecsFilter` reads these dynamically. Placement traits can also be modified at runtime. For more dynamic control, custom scheduler drivers can be implemented.

**Q5: How does server group anti-affinity work at scale?**
A: The `ServerGroupAntiAffinityFilter` queries the server group membership and checks which hosts already have group members. At scale (e.g., 500 members), this can become expensive. Optimization: the scheduler caches server group membership per scheduling cycle. The filter checks `instances.host` for each group member. With Cells v2, this requires cross-cell queries via instance_mappings, which can add latency. Best practice: limit server group size (default `max_server_per_host=1` for anti-affinity).

**Q6: What is the `limit` parameter in Placement allocation_candidates, and how does it affect scheduling?**
A: The `limit` parameter (default: 1000 in most deployments) controls how many candidates Placement returns. A lower limit means faster Placement queries but fewer choices for the filter pipeline. A higher limit means more comprehensive filtering but slower Placement response. The optimal setting depends on cluster heterogeneity — if hosts are homogeneous, a small limit suffices. If hosts have diverse capabilities, a larger limit ensures rare configurations are found.

---

### Deep Dive 2: Nova-Conductor — Database Proxy and Orchestration

**Why it's hard:**
Nova-compute runs on every hypervisor host — potentially thousands of machines in untrusted or semi-trusted zones. Allowing direct database access from nova-compute would be a massive security risk (a compromised compute host could read/modify any instance data). Nova-conductor solves this by acting as a trusted intermediary, but it must handle high throughput without becoming a bottleneck.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Direct DB access from compute** | Simple, low latency | Security risk; compromised compute host can read all instance data; DB connection scaling issues (10K compute hosts = 10K DB connections) |
| **Nova-conductor as DB proxy (current)** | Security boundary; compute hosts never need DB credentials; connection pooling; conductor can batch/optimize queries | Additional hop adds latency; conductor is on the critical path |
| **Event-sourcing / message-based state** | Fully decoupled; replay-able state changes | Complex; eventual consistency issues; not how OpenStack was designed |
| **Per-cell conductor with read replicas** | Reduces cross-cell conductor load | Complex topology; read replicas still need conductor for writes |

**Selected approach:** Nova-conductor as DB proxy + orchestrator, deployed per-cell

**Justification:** This is the production-proven OpenStack architecture. Conductor handles both DB proxying and complex orchestration tasks (build, resize, live migration). Per-cell deployment ensures conductor scales with the cell, and multiple conductor instances provide HA via RabbitMQ consumer groups.

**Implementation detail:**

Conductor serves two roles:

1. **DB proxy**: nova-compute sends RPC calls to conductor for all database reads and writes. Conductor uses SQLAlchemy to execute queries against the cell database. Example: `instance_update(instance_uuid, {'vm_state': 'active'})` arrives via RPC, conductor executes the SQL UPDATE.

2. **Orchestrator**: Complex multi-step operations are coordinated by conductor:
   - **Build**: Conductor receives build request from nova-api, calls scheduler, maps instance to cell, sends build_and_run_instance to nova-compute
   - **Resize**: Conductor coordinates cold migration: allocates on destination, sends prep to dest compute, migrates data, sends finish to dest, waits for confirm/revert
   - **Live migration**: Conductor checks compatibility, calls source/dest compute hosts in sequence, handles rollback on failure

```
Conductor RPC methods (key subset):
───────────────────────────────────
build_instances()           → full build orchestration
rebuild_instance()          → rebuild with new image
migrate_server()            → cold migration / resize
live_migrate_instance()     → live migration orchestration
confirm_resize()            → finalize resize on destination
revert_resize()             → rollback resize to source
instance_update()           → DB proxy: update instance fields
instance_get_by_uuid()      → DB proxy: read instance
migration_update()          → DB proxy: update migration record
compute_node_update()       → DB proxy: update compute node stats
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| All conductor instances crash | No new VMs can be created; running VMs unaffected | Multiple conductor instances per cell; monitor conductor process |
| Conductor → DB connection lost | All state updates fail | Galera cluster with multiple endpoints; connection retry with exponential backoff |
| Conductor → RabbitMQ disconnected | RPC calls fail | RabbitMQ cluster with quorum queues; conductor reconnects automatically |
| Slow conductor processing | Build latency increases | Scale conductor instances; monitor RPC message queue depth |
| Conductor and compute version mismatch | RPC incompatibility | Rolling upgrade: conductor upgraded before compute (N+1 compatible RPC) |
| Long-running operation in conductor crashes | Orphaned instance in BUILD state | Periodic task in conductor detects stuck instances and retries or marks ERROR |

**Interviewer Q&As:**

**Q1: Why can't nova-compute access the database directly? What's the actual threat model?**
A: Compute hosts run in the data plane — they execute tenant workloads, which may be malicious. A VM escape (e.g., QEMU vulnerability) could compromise the host. If nova-compute had DB credentials, the attacker could read all instance data across all tenants, modify scheduling decisions, or corrupt the database. By routing through conductor, the compute host only has RabbitMQ credentials scoped to its cell's exchanges, and conductor enforces authorization on each operation.

**Q2: How do you scale conductor for a cell with 5,000 compute hosts?**
A: Run multiple conductor instances (typically 5-10 per cell). RabbitMQ distributes RPC calls round-robin across conductor consumers. Each conductor instance maintains a pool of DB connections (typically 20-50). Monitor the `conductor` RPC queue depth — if it's growing, add instances. For read-heavy workloads, consider adding a MySQL read replica behind conductor.

**Q3: What happens if conductor crashes during a live migration?**
A: The migration is left in an intermediate state. A periodic task (`_heal_instance_mappings` and `_check_live_migration_abort`) detects stuck migrations. If the migration was in `pre-migrating` state, it can be safely cleaned up. If it was in `migrating` state (memory copy in progress), the source compute's libvirt will eventually time out and the VM remains on the source. Operators can manually abort the migration via `openstack server migration abort`.

**Q4: How does conductor handle rolling upgrades?**
A: Nova uses RPC versioning via oslo.versionedobjects. Each RPC method has a version (e.g., `4.0`, `4.1`). Conductor pins its RPC version to the minimum version that all compute nodes support (set via `upgrade_levels.compute` in nova.conf). During a rolling upgrade: first upgrade conductor (it understands both old and new RPC versions), then upgrade compute nodes one by one, then unpin the RPC version to enable new features. This is the standard OpenStack N-1 compatibility guarantee.

**Q5: Can conductor be a bottleneck for instance status reporting?**
A: Yes, nova-compute reports resource usage every 60 seconds. With 5,000 compute hosts per cell, that's ~83 reports/second, each triggering a DB write via conductor. Mitigation: 1) Increase reporting interval for large cells (e.g., 120s). 2) Use Placement API for inventory reporting (Placement is a separate service with its own DB, bypassing conductor). 3) Batch updates in conductor. 4) Use MySQL connection pooling in conductor.

**Q6: What is the difference between nova-conductor and nova-super-conductor?**
A: In Cells v2 architecture, the "super conductor" runs in the API cell (with access to the nova_api database) and handles cross-cell operations: mapping instances to cells, cross-cell resize, and global queries. Per-cell conductors handle operations within their cell (build, local resize, live migration). The super conductor communicates with per-cell conductors via cell-specific RabbitMQ transports. This separation prevents a per-cell conductor from accessing other cells' data.

---

### Deep Dive 3: Placement API — Resource Tracking

**Why it's hard:**
The Placement API must maintain a globally consistent view of resource availability across 10,000+ compute hosts, handling concurrent allocation claims from multiple schedulers without double-booking, while supporting complex resource models (nested providers for NUMA/GPU, custom resource classes, traits) and responding in under 50ms.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **In-scheduler resource tracking (pre-Placement)** | Fast (in-memory) | Stale data across multiple schedulers; no support for complex resources; scheduler restarts lose state |
| **Separate Placement service with SQL backend (current)** | Consistent; supports complex resource models; independent scaling; SQL queries replace in-memory filtering | Additional service to deploy; DB becomes bottleneck at extreme scale |
| **Distributed key-value store (etcd)** | Fast reads; strong consistency | Harder to model complex queries (nested providers, traits); limited query expressiveness |
| **In-memory cache + async DB** | Very fast allocation checks | Stale cache leads to overcommit; complex invalidation logic |

**Selected approach:** Separate Placement service with MySQL backend

**Justification:** Placement's SQL-based approach enables complex queries (e.g., "find hosts with 4 VCPU, 8GB RAM, 1 GPU, AVX2 support, not disabled, in AZ-1") in a single query. The generation-based optimistic locking prevents double-booking without distributed locks.

**Implementation detail:**

```
Resource Provider Hierarchy (nested providers):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  compute-host-001 (root provider)
  ├── VCPU: total=128, reserved=4, allocation_ratio=2.0  → usable=252
  ├── MEMORY_MB: total=524288, reserved=8192, allocation_ratio=1.0  → usable=516096
  ├── DISK_GB: total=2000, reserved=100, allocation_ratio=1.0  → usable=1900
  ├── Traits: HW_CPU_X86_AVX2, HW_CPU_X86_SSE42, COMPUTE_STATUS_ENABLED
  │
  ├── numa-node-0 (child provider)
  │   ├── VCPU: total=64, allocation_ratio=1.0   (for pinned workloads)
  │   ├── MEMORY_MB: total=262144
  │   └── Traits: HW_NUMA_ROOT
  │
  ├── numa-node-1 (child provider)
  │   ├── VCPU: total=64, allocation_ratio=1.0
  │   ├── MEMORY_MB: total=262144
  │   └── Traits: HW_NUMA_ROOT
  │
  ├── gpu-0 (child provider)
  │   ├── CUSTOM_GPU_NVIDIA_A100: total=1
  │   └── Traits: CUSTOM_GPU_NVIDIA_A100_80GB
  │
  └── gpu-1 (child provider)
      ├── CUSTOM_GPU_NVIDIA_A100: total=1
      └── Traits: CUSTOM_GPU_NVIDIA_A100_80GB

Allocation Candidate Query Flow:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

GET /placement/allocation_candidates?
  resources=VCPU:4,MEMORY_MB:8192,DISK_GB:80
  &required=HW_CPU_X86_AVX2
  &resources_GPU:CUSTOM_GPU_NVIDIA_A100=1    ← request from numbered group
  &required_GPU=CUSTOM_GPU_NVIDIA_A100_80GB

Placement SQL (simplified):
1. Find root providers that have VCPU, MEMORY_MB, DISK_GB with sufficient capacity
   AND have trait HW_CPU_X86_AVX2
2. For each matching root, find child providers with CUSTOM_GPU_NVIDIA_A100
   AND trait CUSTOM_GPU_NVIDIA_A100_80GB
3. Compute available = (total - reserved) * allocation_ratio - allocated
4. Return (root_provider, allocation_request) pairs

Allocation Claim (atomic):
━━━━━━━━━━━━━━━━━━━━━━━━━

PUT /placement/allocations/{instance-uuid}
{
  "allocations": {
    "compute-host-001-uuid": {
      "resources": {"VCPU": 4, "MEMORY_MB": 8192, "DISK_GB": 80}
    },
    "gpu-0-uuid": {
      "resources": {"CUSTOM_GPU_NVIDIA_A100": 1}
    }
  },
  "consumer_generation": null,   ← null for new allocation
  "project_id": "...",
  "user_id": "..."
}

Placement checks:
1. For each resource_provider, verify generation matches (optimistic lock)
2. Verify sufficient capacity: used + request ≤ (total - reserved) * allocation_ratio
3. If all checks pass: INSERT allocations, increment provider generations
4. If any check fails: return 409 Conflict, scheduler retries
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Placement DB down | No new VMs can be scheduled | Galera cluster; multiple Placement API instances |
| Stale inventory (compute host reports delayed) | Overcommit or underutilization | 60s periodic reporting; `_heal_allocations` periodic task reconciles |
| Orphaned allocations (instance deleted but allocation remains) | Phantom resource usage | `nova-manage placement heal_allocations` CLI; periodic reconciliation |
| Generation conflict storms under high concurrency | Scheduling throughput drops | Increase `limit` in allocation_candidates to spread load; use random jitter in retries |
| Nested provider misconfiguration | GPUs/NUMA not discoverable | Validate provider tree on nova-compute startup; automated topology detection |

**Interviewer Q&As:**

**Q1: How does Placement handle overcommit ratios?**
A: Each inventory record has an `allocation_ratio` field. For VCPU, it might be 2.0 (200% overcommit). The formula for available capacity is: `(total - reserved) * allocation_ratio - allocated`. For example, a host with 128 physical VCPUs, 4 reserved, and 2.0 overcommit has `(128 - 4) * 2.0 = 248` allocatable VCPUs. For pinned workloads (hw:cpu_policy=dedicated), the allocation_ratio is set to 1.0 on the NUMA child provider, preventing overcommit.

**Q2: What are traits, and how do they differ from resource classes?**
A: Resource classes represent quantitative resources (VCPU, MEMORY_MB, CUSTOM_GPU_A100) that are consumed via allocations. Traits represent qualitative capabilities (HW_CPU_X86_AVX2, STORAGE_DISK_SSD, CUSTOM_RACK_POWER_REDUNDANT) that are boolean properties of a resource provider. A scheduling request can require or forbid traits but doesn't consume them. This distinction enables expressing "I need a host with AVX2 support" without modeling AVX2 as a consumable resource.

**Q3: How does Placement prevent the "double booking" problem with multiple schedulers?**
A: Optimistic concurrency via the `generation` field. Each resource provider has a generation counter. When a scheduler reads allocation candidates, it receives the current generation. When it claims allocations (PUT /allocations), Placement checks that the generation hasn't changed. If another scheduler already claimed resources (incrementing the generation), the claim returns 409 Conflict. The scheduler discards that candidate and retries with another. This is lock-free and scales well.

**Q4: How do you model SR-IOV network interfaces in Placement?**
A: SR-IOV VFs (Virtual Functions) are modeled as a custom resource class (e.g., `CUSTOM_NET_VF` or `NET_SRIOV_VF`) on a child resource provider representing the physical NIC. Each NIC provider has an inventory of VFs (e.g., total=64). When a port is created with `vnic_type=direct`, Neutron adds a Placement request group requiring `NET_SRIOV_VF:1` with a trait like `CUSTOM_PHYSNET_TENANT`. The scheduler ensures the host has an available VF on the correct physical network.

**Q5: What happens when a compute host is decommissioned?**
A: The operator disables the compute service (`openstack compute service set --disable <host>`), which sets the `COMPUTE_STATUS_DISABLED` trait. Live-migrate all instances off the host. Then run `openstack compute service delete <host-service-id>` to remove the service record, and `nova-manage placement delete_resource_provider <uuid>` to remove the provider from Placement. Any orphaned allocations are cleaned up by the deletion cascading.

**Q6: How does Placement scale for 10,000+ resource providers?**
A: Placement's SQL queries are optimized with appropriate indexes on resource_providers, inventories, and allocations. The `allocation_candidates` query uses SQL JOINs with aggregate filtering — it's a single round-trip to the DB. At 10,000 providers with 50 resource classes each, the inventories table has ~500K rows, which MySQL handles easily. Multiple Placement API instances behind HAProxy distribute load. The main bottleneck is write contention on popular providers, mitigated by the `limit` parameter spreading candidates.

---

### Deep Dive 4: VM Lifecycle State Machine

**Why it's hard:**
A VM moves through many states, and transitions can be triggered by user actions, internal events (build failures, host failures), and administrative operations (migration, evacuation). The state machine must be deterministic, handle concurrent operations safely, and provide clear error recovery paths. Task state and VM state are decoupled, adding complexity.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Dual-state model (vm_state + task_state)** | Separates "what the VM is" from "what operation is in progress"; allows concurrent operations tracking | Complex; many invalid state combinations; hard to reason about |
| **Single unified state** | Simple to understand | Can't distinguish "VM is active" from "VM is active and being live-migrated" |
| **Event-sourced state machine** | Full audit trail; replayable | Overkill for OpenStack; eventual consistency issues |

**Selected approach:** Dual-state model (current Nova architecture)

**Implementation detail:**

```
VM States (vm_state):
═══════════════════
  BUILDING    → VM is being created
  ACTIVE      → VM is running
  PAUSED      → VM execution paused (RAM preserved)
  SUSPENDED   → VM state saved to disk
  STOPPED     → VM gracefully shut down (SHUTOFF in API)
  RESCUED     → VM booted from rescue image
  RESIZED     → VM resized, awaiting confirm/revert
  SHELVED     → VM shelved (image saved, resources may be freed)
  SHELVED_OFFLOADED → VM shelved and resources freed
  SOFT_DELETED → VM soft-deleted (recoverable)
  ERROR       → VM in error state
  DELETED     → VM deleted (terminal)

Task States (task_state):
═════════════════════════
  SCHEDULING          → Waiting for scheduler
  BLOCK_DEVICE_MAPPING → Setting up block devices
  NETWORKING          → Allocating network resources
  SPAWNING            → Hypervisor creating VM
  POWERING_ON         → Starting VM
  POWERING_OFF        → Stopping VM
  REBOOTING           → Rebooting (soft or hard)
  MIGRATING           → Live migration in progress
  RESIZE_PREP         → Preparing for resize
  RESIZE_MIGRATING    → Copying data for resize
  RESIZE_FINISH       → Completing resize on destination
  RESIZE_REVERTING    → Reverting resize
  REBUILDING          → Rebuilding VM from image
  SUSPENDING          → Suspending VM
  RESUMING            → Resuming VM
  PAUSING             → Pausing VM
  UNPAUSING           → Unpausing VM
  RESCUING            → Entering rescue mode
  SHELVING            → Shelving VM
  UNSHELVING          → Unshelving VM
  DELETING            → Deleting VM
  null                → No task in progress

State Transition Diagram (key paths):
══════════════════════════════════════

  [API: create] ──→ BUILDING/SCHEDULING
                    ──→ BUILDING/NETWORKING
                    ──→ BUILDING/BLOCK_DEVICE_MAPPING
                    ──→ BUILDING/SPAWNING
                    ──→ ACTIVE/null         ← SUCCESS
                    ──→ ERROR/null          ← FAILURE at any step

  [API: stop]   ──→ ACTIVE/POWERING_OFF ──→ STOPPED/null
  [API: start]  ──→ STOPPED/POWERING_ON ──→ ACTIVE/null
  [API: reboot] ──→ ACTIVE/REBOOTING ──→ ACTIVE/null
  [API: pause]  ──→ ACTIVE/PAUSING ──→ PAUSED/null
  [API: suspend]──→ ACTIVE/SUSPENDING ──→ SUSPENDED/null

  [API: resize] ──→ ACTIVE/RESIZE_PREP
                ──→ ACTIVE/RESIZE_MIGRATING
                ──→ ACTIVE/RESIZE_FINISH
                ──→ RESIZED/null         ← awaiting confirm
  [API: confirm]──→ ACTIVE/null
  [API: revert] ──→ RESIZED/RESIZE_REVERTING ──→ ACTIVE/null

  [API: live-migrate] ──→ ACTIVE/MIGRATING ──→ ACTIVE/null
  [API: delete] ──→ */DELETING ──→ DELETED/null (from any state)
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| VM stuck in BUILD state | User can't use VM; resources consumed | Periodic task detects build timeout (default: 1800s); moves to ERROR |
| VM in ERROR after build | Resources partially allocated | User can delete; or admin rebuilds. Placement allocations cleaned up. |
| Task state stuck (e.g., MIGRATING forever) | Can't perform other operations on VM | Admin resets task state: `nova reset-state --active <vm-id>` |
| Concurrent operations (stop while migrating) | Undefined behavior | Nova locks instance during operations using task_state as a mutex; concurrent requests get 409 Conflict |

**Interviewer Q&As:**

**Q1: Why does Nova have both vm_state and task_state instead of a single state?**
A: vm_state represents the durable state of the VM (what it "is"), while task_state represents what transient operation is in progress. This separation allows expressing "the VM is ACTIVE and being live-migrated" as vm_state=ACTIVE + task_state=MIGRATING. Without this separation, you'd need states like ACTIVE_MIGRATING, ACTIVE_REBOOTING, ACTIVE_RESIZING, etc., creating a combinatorial explosion. It also enables `task_state=null` to clearly indicate "idle, ready for new operations."

**Q2: How does Nova prevent concurrent operations on the same instance?**
A: Nova uses task_state as an implicit lock. Before starting an operation, the API checks that task_state is null (no operation in progress). If another operation is underway, the API returns HTTP 409 Conflict. This is enforced in the API layer with `expected_task_state` checks during DB updates (optimistic locking). Additionally, some operations grab a per-instance lock at the compute level using oslo.concurrency's file-based or DB-based locks.

**Q3: What happens if a compute host crashes while a VM is in BUILD/SPAWNING state?**
A: The VM remains in BUILD/SPAWNING state. The nova-conductor periodic task `_check_instance_build_time_limit` detects that the build has exceeded the timeout. The instance is moved to ERROR state. The Placement allocations remain (resources reserved). The user can delete the instance (which cleans up allocations) or an admin can rebuild it. If the compute host comes back, nova-compute's init process reconciles: it checks for instances that should exist on the host but don't, and updates their state.

**Q4: How does shelving work and why is it useful?**
A: Shelving stops a VM and optionally offloads its resources. `shelve` creates a snapshot image and stops the VM — resources remain on the host (vm_state=SHELVED). `shelve_offload` removes the VM from the host entirely, freeing compute resources (vm_state=SHELVED_OFFLOADED). When unshelved, the VM goes through the scheduler again to find a host. Shelving is useful for infrequently used VMs — you keep the configuration and data but free the resources. It's like "parking" a VM.

**Q5: How are power_state and vm_state related?**
A: `vm_state` is the desired/expected state (managed by Nova's state machine). `power_state` is the actual hypervisor-reported state (RUNNING=1, PAUSED=3, SHUTDOWN=4, CRASHED=6). Nova periodically syncs power_state from the hypervisor (every 60s via `_sync_power_states`). If there's a mismatch (e.g., vm_state=ACTIVE but power_state=SHUTDOWN because the guest crashed), Nova can either force-reconcile (restart the VM) or update vm_state to match, depending on configuration.

**Q6: Can you transition from ERROR to ACTIVE?**
A: Not directly via the standard state machine. An admin can reset the state using `nova reset-state --active <instance-id>`, which forcibly sets vm_state=ACTIVE and task_state=null. This is a manual recovery tool. Alternatively, the user can rebuild the instance (`POST /servers/{id}/action {"rebuild": ...}`), which transitions from ERROR through REBUILDING/SPAWNING back to ACTIVE. The rebuild recreates the VM on the same host with a fresh image.

---

## 7. Scheduling & Resource Management

### Nova Scheduler Architecture

```
┌────────────────────────────────────────────────────────┐
│                    Nova Scheduler                       │
│                                                         │
│  ┌─────────────────────┐   ┌────────────────────────┐  │
│  │  Placement Querier  │──→│  Placement API          │  │
│  │  (pre-filter)       │   │  (returns candidates)   │  │
│  └─────────┬───────────┘   └────────────────────────┘  │
│            │                                            │
│            ▼                                            │
│  ┌─────────────────────┐                                │
│  │  Filter Pipeline    │                                │
│  │  ┌───────────────┐  │                                │
│  │  │AZ Filter      │  │  Enabled filters (in order):  │
│  │  ├───────────────┤  │  1. AvailabilityZoneFilter    │
│  │  │Compute Filter │  │  2. ComputeFilter             │
│  │  ├───────────────┤  │  3. RamFilter                 │
│  │  │Image Props    │  │  4. DiskFilter                │
│  │  ├───────────────┤  │  5. ImagePropertiesFilter     │
│  │  │ServerGroup    │  │  6. ServerGroupAntiAffFilter  │
│  │  ├───────────────┤  │  7. NUMATopologyFilter        │
│  │  │NUMA Topology  │  │  8. PciPassthroughFilter      │
│  │  ├───────────────┤  │  9. AggregateInstanceExtra    │
│  │  │PCI Passthru   │  │  10. IoOpsFilter              │
│  │  └───────────────┘  │                                │
│  └─────────┬───────────┘                                │
│            │                                            │
│            ▼                                            │
│  ┌─────────────────────┐                                │
│  │  Weight Pipeline    │                                │
│  │  RAMWeigher (-1.0)  │  Negative = spread (default)  │
│  │  DiskWeigher        │  Positive = stack              │
│  │  MetricsWeigher     │                                │
│  │  ServerGroupWeigher │                                │
│  │  BuildFailWeigher   │                                │
│  └─────────┬───────────┘                                │
│            │                                            │
│            ▼                                            │
│  ┌─────────────────────┐                                │
│  │ Select Top-N Hosts  │  (N = scheduler_host_subset)  │
│  │ Random from top-N   │  Default: N=1                  │
│  └─────────────────────┘                                │
└────────────────────────────────────────────────────────┘
```

### Resource Management Concepts

| Concept | Description |
|---------|------------|
| **Resource Provider** | Entity that provides resources. Typically a compute node, but can be a child (NUMA node, GPU, NIC). |
| **Resource Class** | Type of resource: VCPU, MEMORY_MB, DISK_GB, VGPU, SRIOV_NET_VF, or CUSTOM_* |
| **Inventory** | How much of a resource class a provider has: total, reserved, allocation_ratio, min/max/step_unit |
| **Allocation** | How much of a resource class is consumed by a consumer (instance). Sum of allocations ≤ capacity. |
| **Trait** | Qualitative property of a provider: HW_CPU_X86_AVX2, STORAGE_DISK_SSD, CUSTOM_* |
| **Aggregate** | Group of resource providers (like a rack or AZ). Used for scoping allocation candidates. |
| **Host Aggregate** | Nova concept: group of hosts with metadata. Used by AggregateInstanceExtraSpecsFilter. |
| **Availability Zone** | User-visible grouping of hosts. Implemented as host aggregates with `availability_zone` metadata. |
| **Cells v2** | Deployment unit: each cell has its own DB and message queue. API cell is global. Scales to 10K hosts/cell. |

### Overcommit Ratios

| Resource | Default Ratio | Production Recommendation | Notes |
|----------|---------------|--------------------------|-------|
| VCPU | 16.0 | 2.0 - 4.0 | For general workloads; 1.0 for latency-sensitive |
| MEMORY_MB | 1.5 | 1.0 - 1.2 | Memory overcommit via KSM; risky for OOM |
| DISK_GB | 1.0 | 1.0 | Thin provisioning handled by storage layer (Ceph) |

### Quota Enforcement

```
Quota types and defaults (per project):
  instances:    10
  cores:        20
  ram:          51200 (MB)
  key_pairs:    100
  server_groups: 10
  server_group_members: 10

Quota enforcement flow:
1. nova-api receives create request
2. Check quota: SELECT SUM(vcpus), SUM(memory_mb), COUNT(*) 
   FROM instances WHERE project_id=? AND deleted=0
3. Compare against project quota limits
4. If over quota, return HTTP 403 Forbidden
5. Reserve quota (increment in-use counters)
6. On build failure, release reserved quota

Unified limits (Keystone, newer approach):
- Quotas defined in Keystone, enforced by each service
- Enables consistent quota management across services
- oslo.limit library handles enforcement
```

---

## 8. Scaling Strategy

### Cells v2 Architecture

```
┌──────────────────────────────────────────────────────┐
│                      API Cell                         │
│  ┌──────────┐  ┌───────────┐  ┌──────────────────┐  │
│  │ nova-api │  │ Placement │  │ nova_api DB      │  │
│  │ (x3)    │  │ API (x3)  │  │ (Galera)         │  │
│  └──────────┘  └───────────┘  └──────────────────┘  │
│  ┌───────────────────────┐    ┌──────────────────┐  │
│  │ Super Conductor (x2) │    │ nova_cell0 DB    │  │
│  └───────────────────────┘    │ (failed builds)  │  │
│                                └──────────────────┘  │
└──────────────────────────────────────────────────────┘
        │                    │                    │
        ▼                    ▼                    ▼
┌──────────────┐   ┌──────────────┐   ┌──────────────┐
│   Cell 1     │   │   Cell 2     │   │   Cell 3     │
│ ┌──────────┐ │   │ ┌──────────┐ │   │ ┌──────────┐ │
│ │RabbitMQ  │ │   │ │RabbitMQ  │ │   │ │RabbitMQ  │ │
│ │Cluster   │ │   │ │Cluster   │ │   │ │Cluster   │ │
│ └──────────┘ │   │ └──────────┘ │   │ └──────────┘ │
│ ┌──────────┐ │   │ ┌──────────┐ │   │ ┌──────────┐ │
│ │MySQL     │ │   │ │MySQL     │ │   │ │MySQL     │ │
│ │Galera    │ │   │ │Galera    │ │   │ │Galera    │ │
│ └──────────┘ │   │ └──────────┘ │   │ └──────────┘ │
│ ┌──────────┐ │   │ ┌──────────┐ │   │ ┌──────────┐ │
│ │Conductor │ │   │ │Conductor │ │   │ │Conductor │ │
│ │Scheduler │ │   │ │Scheduler │ │   │ │Scheduler │ │
│ └──────────┘ │   │ └──────────┘ │   │ └──────────┘ │
│ 3,000 hosts  │   │ 3,000 hosts  │   │ 4,000 hosts  │
└──────────────┘   └──────────────┘   └──────────────┘
```

### Scaling Dimensions

| Dimension | Strategy | Limit |
|-----------|----------|-------|
| API throughput | Add nova-api instances behind HAProxy | Stateless; scale horizontally |
| Scheduling throughput | Multiple scheduler instances; Placement pre-filtering reduces work | ~500 decisions/sec with 3 schedulers |
| DB connections | Connection pooling in SQLAlchemy (pool_size=20, max_overflow=50); Galera cluster | ~3,000 connections per Galera cluster |
| Compute hosts | Cells v2 partitioning; 3,000-10,000 hosts per cell | RabbitMQ cluster per cell limits fan-out |
| RabbitMQ throughput | Dedicated cluster per cell; quorum queues for durability | ~50,000 msg/sec per cluster |
| Placement queries | Multiple Placement instances; MySQL read replicas for GET queries | Bottleneck at ~10,000 concurrent writes/sec |

### Interviewer Q&As

**Q1: At what scale does a single-cell deployment break, and why?**
A: A single cell starts showing stress at around 5,000-10,000 compute hosts. The limiting factors are: (1) RabbitMQ message volume — 10,000 compute hosts generating periodic reports creates ~170 messages/sec just for resource updates, plus all RPC traffic. (2) MySQL connection count — each conductor and compute service maintains DB connections. (3) Scheduler decision time increases as more hosts pass through filters. The Cells v2 architecture addresses this by partitioning into cells, each with its own RabbitMQ and MySQL.

**Q2: How do you handle cross-cell operations like listing all instances across cells?**
A: Nova-api uses `instance_mappings` in the api_cell database to know which cell each instance lives in. For `GET /servers` (list), nova-api fans out queries to all cell databases in parallel (via the super conductor or direct "scatter-gather"). Results are merged and sorted in the API layer. This is inherently slower than a single-DB query. Optimization: `instance_mappings` contains project_id, enabling filtering before fan-out. For very large deployments, consider caching strategies or limiting cross-cell list operations.

**Q3: How do you add a new cell to a running deployment?**
A: 1) Deploy the cell's infrastructure: MySQL cluster, RabbitMQ cluster. 2) Register the cell: `nova-manage cell_v2 create_cell --name cell4 --transport-url rabbit://... --database_connection mysql://...`. 3) Deploy nova-conductor and nova-scheduler in the new cell. 4) Add compute hosts to the new cell — they register automatically when nova-compute starts. 5) Discover hosts: `nova-manage cell_v2 discover_hosts`. The scheduler will automatically start placing VMs in the new cell based on available resources.

**Q4: How does the scheduler decide which cell to place a VM in?**
A: In the current architecture, the Placement API returns allocation candidates from all cells (resource providers are global in Placement). The scheduler selects the best host regardless of cell. The super conductor then maps the selected host to its cell using `host_mappings` and routes the build request to the appropriate cell's conductor. In practice, cell selection is implicit — it follows the host selection. If you want to bias toward specific cells, use availability zones or host aggregates.

**Q5: What is the upgrade path when you need to add capacity beyond 10K hosts per cell?**
A: You don't grow a cell beyond its comfort zone. Instead, add new cells. Each cell is independently upgradeable and maintainable. For existing cells that have grown too large, you can "split" a cell by: (1) deploying new cell infrastructure, (2) disabling scheduling to hosts that will move, (3) live-migrating all instances off those hosts, (4) re-registering the hosts in the new cell. This is a manual process and requires maintenance windows for the migrated hosts.

**Q6: How do you benchmark and capacity-plan for the control plane?**
A: Use Rally (OpenStack benchmarking tool) to simulate API load: concurrent create/delete/list operations. Key metrics: (1) API response latency P50/P99, (2) scheduler decisions/sec, (3) RabbitMQ queue depth, (4) MySQL slow query log and connection utilization, (5) conductor RPC processing time. Size the control plane for 3x expected peak. Rule of thumb: 3 API nodes, 3 schedulers, 5 conductors per 5,000 hosts. Test with realistic workloads — list operations are often the bottleneck.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure Scenario | Impact | Detection | Recovery | RTO |
|---|-----------------|--------|-----------|----------|-----|
| 1 | Single nova-api instance crashes | Reduced API capacity; HAProxy routes to healthy instances | HAProxy health check (HTTP 200 on `/`) | HAProxy removes failed backend; replace instance | < 10s |
| 2 | All nova-api instances crash | Complete API outage; running VMs unaffected | External monitoring; HAProxy returns 503 | Restart nova-api processes; Pacemaker can manage | < 60s |
| 3 | Nova-scheduler crashes | No new VMs scheduled; builds queue in RabbitMQ | RabbitMQ queue depth alert; service health check | Restart scheduler; queued builds processed automatically | < 30s |
| 4 | Nova-conductor crashes (per-cell) | No builds or state updates in that cell; running VMs unaffected | RabbitMQ queue depth; periodic health check | Restart conductor; queued operations processed | < 30s |
| 5 | Single compute host crashes | All VMs on host are down | nova-compute missed heartbeat; fencing via IPMI | Evacuate VMs to other hosts (`nova host-evacuate`); requires shared storage or boot-from-volume | 2-10 min |
| 6 | RabbitMQ cluster partition (split-brain) | Message delivery failures; builds fail | RabbitMQ monitoring (partition detection) | Restart minority partition; quorum queues prevent split-brain | 1-5 min |
| 7 | MySQL Galera node failure | One less DB node; cluster continues with quorum | Galera monitoring (wsrep_cluster_size) | Rejoin failed node; SST (full sync) if too far behind | < 30s for failover |
| 8 | MySQL Galera cluster quorum loss | DB writes fail; API returns 500 | Galera monitoring; API error rate spike | Restore quorum: bootstrap from most advanced node | 5-15 min |
| 9 | Placement API down | No new scheduling decisions; running VMs unaffected | API health check; scheduler error rate | Restart Placement; multiple instances for HA | < 30s |
| 10 | Ceph cluster degraded | VM disk I/O slowed; new VM creation may fail | Ceph health monitoring (HEALTH_WARN/ERR) | Ceph self-heals; replace failed OSDs; may need to add capacity | Minutes to hours |
| 11 | Keystone down | No new auth tokens; cached tokens still work until expiry | Keystone health check; API 401 rate | Restart Keystone; Fernet tokens don't require DB for validation | < 60s |
| 12 | Network partition between control plane and compute | Compute hosts can't receive new commands; running VMs continue | nova-compute heartbeat timeout | Resolve network issue; VMs continue running autonomously | Varies |

### OpenStack-Specific HA Patterns

| Pattern | Implementation | Details |
|---------|---------------|---------|
| **Active-Active API services** | nova-api, Placement, Keystone behind HAProxy | Stateless services; HAProxy round-robin with health checks; VIP via keepalived |
| **Active-Active workers** | nova-scheduler, nova-conductor | Multiple instances consume from RabbitMQ queues; RabbitMQ distributes messages |
| **Galera Cluster** | 3-node synchronous multi-master MySQL | All nodes accept reads/writes; certification-based replication; auto-eviction of slow nodes |
| **RabbitMQ Quorum Queues** | 3-node RabbitMQ cluster with quorum queues | Raft-based consensus for queue durability; replaces classic mirrored queues |
| **Fernet Tokens** | Keystone Fernet token provider | Tokens validated locally using symmetric keys; no DB lookup needed; key rotation via cron |
| **Compute HA (Masakari)** | Automatic VM evacuation on host failure | Monitors compute hosts via IPMI/iLO; triggers evacuation via Nova API; requires shared storage |
| **Pacemaker/Corosync** | HA for HAProxy VIP, Galera arbitrator | Manages VIP failover; prevents split-brain with STONITH fencing |
| **Anti-affinity for control plane** | Spread controllers across failure domains | Controllers in different racks/power zones; use server group anti-affinity for control plane VMs |

### Instance Evacuation Procedure

```
Host failure detected (heartbeat timeout > 60s):
1. Fencing: IPMI power-off failed host (prevent split-brain)
2. nova-manage: force-down compute service
   nova service-force-down <host> nova-compute
3. For each instance on failed host:
   a. If boot-from-volume: evacuate (recreate VM on new host, attach same volume)
      nova evacuate <instance-id> [--target-host <host>]
   b. If ephemeral disk on shared storage (Ceph): evacuate (same as above)
   c. If ephemeral disk on local storage: data is LOST; can only rebuild from image
4. Verify all instances running on new hosts
5. Clean up failed host records

Automated with Masakari:
- masakari-monitors: detects host failure via pacemaker-remote
- masakari-engine: orchestrates evacuation
- Configurable: which instances to evacuate, priority, target hosts
```

---

## 10. Observability

### Key Metrics

| Category | Metric | Source | Alert Threshold |
|----------|--------|--------|-----------------|
| **API** | nova_api_request_duration_seconds | StatsD / Prometheus | P99 > 2s |
| **API** | nova_api_requests_total (by method, status) | StatsD / Prometheus | Error rate > 1% |
| **API** | nova_api_active_connections | HAProxy | > 80% max connections |
| **Scheduler** | nova_scheduler_select_destinations_duration | StatsD | P99 > 500ms |
| **Scheduler** | nova_scheduler_no_valid_host_total | StatsD | Increasing trend |
| **Conductor** | nova_conductor_rpc_queue_depth | RabbitMQ management API | > 100 messages |
| **Conductor** | nova_conductor_build_duration_seconds | StatsD | P99 > 120s |
| **Compute** | nova_compute_running_vms | nova-compute periodic report | Unexpected drops |
| **Compute** | nova_compute_vcpus_used / vcpus_total | Placement API | > 90% utilization |
| **Compute** | nova_compute_memory_used_mb / memory_total_mb | Placement API | > 85% utilization |
| **Placement** | placement_allocation_candidates_duration | StatsD | P99 > 100ms |
| **Placement** | placement_allocation_claim_conflicts | StatsD | > 10/min (contention) |
| **RabbitMQ** | rabbitmq_queue_messages_ready | RabbitMQ exporter | > 1000 in any queue |
| **RabbitMQ** | rabbitmq_fd_used / fd_total | RabbitMQ exporter | > 80% |
| **MySQL** | mysql_threads_connected | MySQL exporter | > 80% max_connections |
| **MySQL** | mysql_slow_queries | MySQL exporter | Increasing trend |
| **MySQL** | galera_wsrep_cluster_size | Galera exporter | < expected node count |
| **Instance** | instances_in_error_state | DB query | Increasing trend |
| **Instance** | instances_in_build_over_5min | DB query | > 0 (stuck builds) |
| **Migration** | active_live_migrations | DB query | > max_concurrent_live_migrations |

### OpenStack Ceilometer / Telemetry

```
Ceilometer Pipeline:
  nova-compute → ceilometer-agent-compute → gnocchi (time-series DB)

Metrics collected by Ceilometer:
  compute.instance.booting.time    → time to boot (seconds)
  cpu                              → cumulative CPU time (ns)
  cpu_util                         → CPU utilization (%)
  memory.usage                     → memory used by instance (MB)
  memory.resident                  → resident memory (MB)
  disk.read.bytes                  → disk read bytes
  disk.write.bytes                 → disk write bytes
  disk.read.requests               → disk read IOPS
  disk.write.requests              → disk write IOPS
  network.incoming.bytes           → network RX bytes
  network.outgoing.bytes           → network TX bytes
  network.incoming.packets         → network RX packets
  network.outgoing.packets         → network TX packets

Gnocchi storage:
  Time-series database optimized for metrics
  Aggregation policies: mean, max, min over 1min, 5min, 1hr, 1day
  Retention: raw data 7 days, aggregated 1 year

Prometheus integration (modern approach):
  openstack-exporter → scrapes OpenStack APIs → Prometheus
  Benefits: unified monitoring stack, Grafana dashboards, Alertmanager
  Recommended over Ceilometer for operational metrics
```

### Logging Strategy

```
oslo.log configuration:
  log_dir = /var/log/nova/
  log_file = nova-{service}.log
  debug = False (True in staging)
  use_syslog = True (centralized via rsyslog → ELK)

Key log entries to alert on:
  "NoValidHost" in nova-scheduler.log       → capacity or filter issue
  "MessagingTimeout" in any log             → RabbitMQ connectivity issue
  "DBConnectionError" in conductor log      → MySQL connectivity issue
  "BuildAbortException" in nova-compute.log → build failure
  "LiveMigrationError" in nova-compute.log  → migration failure
  "AMQP server on ... is unreachable"       → RabbitMQ node down

Structured logging (oslo.log + JSON formatter):
  Enable for ELK/Splunk ingestion
  Fields: timestamp, severity, request_id, instance_uuid, project_id, user_id
  request_id enables end-to-end tracing across services
```

---

## 11. Security

### Keystone: Auth, Tokens, Projects, Domains

```
Keystone Architecture:
━━━━━━━━━━━━━━━━━━━━━

  Domains → Projects → Users → Roles → Policies

  Domain: top-level organizational unit (e.g., "acme-corp")
  Project: resource container (instances, networks, volumes belong to a project)
  User: identity (can be in multiple projects with different roles)
  Role: named permission set (admin, member, reader)
  Policy: JSON/YAML rules mapping API operations to required roles

Authentication Flow:
  1. User → Keystone: POST /v3/auth/tokens
     Body: {"auth": {"identity": {"methods": ["password"],
            "password": {"user": {"name": "...", "password": "...",
            "domain": {"name": "..."}}}}}}
  2. Keystone validates credentials against identity backend (SQL, LDAP, AD)
  3. Keystone issues Fernet token (contains user_id, project_id, roles, expiry)
  4. User → nova-api: POST /v2.1/servers
     Header: X-Auth-Token: <fernet-token>
  5. nova-api → Keystone: validate token (or validate locally with Fernet keys)
  6. nova-api checks oslo.policy rules: create requires 'member' role

Fernet Tokens:
  - Symmetric encryption using rotating keys
  - No database storage needed (unlike UUID tokens)
  - Token contains: user_id, project_id, roles, issued_at, expires_at
  - Key rotation: primary key (signs new tokens), secondary keys (validate old tokens),
    staged key (next primary)
  - Rotation interval: typically every 24 hours
  - Max token age: typically 1 hour (short-lived)

RBAC Policy (oslo.policy):
  /etc/nova/policy.yaml (overrides defaults in code)
  
  Examples:
  "os_compute_api:servers:create": "role:member"
  "os_compute_api:servers:delete": "role:member"
  "os_compute_api:servers:create:forced_host": "role:admin"
  "os_compute_api:os-migrate-server:migrate_live": "role:admin"
  "os_compute_api:os-hypervisors:list": "role:admin"
  
  Scoped tokens (newer):
  - System-scoped: global operations (manage compute services)
  - Domain-scoped: domain-level operations
  - Project-scoped: project-level operations (most VM operations)
```

### Security Hardening

| Area | Practice |
|------|----------|
| **Network** | Control plane on separate VLAN from data plane; TLS for all API endpoints; internal services use TLS mutual auth |
| **Secrets** | Barbican for secret storage; DB passwords in vault; RabbitMQ credentials per-service |
| **Compute isolation** | SELinux/AppArmor on compute hosts; sVirt for VM isolation; seccomp profiles for QEMU |
| **API** | Rate limiting via oslo.middleware or HAProxy; request size limits; API microversioning prevents version attacks |
| **Database** | MySQL TLS; per-service DB users with minimal privileges; nova-compute has NO DB credentials |
| **RabbitMQ** | TLS; per-service vhost and credentials; topic-based ACLs |
| **Audit** | Keystone CADF audit events; oslo.middleware logs all API requests with user/project context |
| **Image security** | Glance image signing with Barbican; image property protection (admin-only properties) |
| **Metadata service** | Proxy via nova-metadata-api on compute hosts; prevents SSRF attacks from instances to control plane |

---

## 12. Incremental Rollout

### Rollout Strategy for Nova Changes

```
Phase 1: Dev/Test Environment
  - Full functional testing with Tempest test suite
  - Performance benchmarking with Rally
  - Duration: 1-2 weeks

Phase 2: Staging Cell (mirrors production)
  - Deploy to a dedicated staging cell with real hardware
  - Run synthetic workloads simulating production patterns
  - Duration: 1 week

Phase 3: Canary Cell (1 production cell, smallest)
  - Deploy to 1 production cell (~3,000 hosts)
  - Monitor error rates, latency, resource utilization
  - Duration: 1 week with bake time

Phase 4: Rolling Deployment (remaining cells)
  - Deploy cell-by-cell over 1-2 weeks
  - Each cell gets 24hr bake time before next
  - Rollback plan: revert cell to previous version

Phase 5: Cleanup
  - Remove old version's compatibility code
  - Unpin RPC versions
  - Run DB migrations (if any)
```

### Rollout Q&As

**Q1: How do you perform a zero-downtime upgrade of Nova across a large deployment?**
A: OpenStack supports rolling upgrades with N-1 compatibility. Steps: (1) Upgrade nova-conductor first — it understands both old and new RPC versions. (2) Upgrade nova-scheduler. (3) Upgrade nova-api. (4) Upgrade nova-compute hosts one by one (this is the slowest step). Throughout, pin `upgrade_levels.compute=auto` so conductor sends RPC in a version the compute node understands. (5) After all compute nodes are upgraded, unpin the RPC version and run any online DB migrations (`nova-manage db online_data_migrations`).

**Q2: What if a DB schema migration is needed?**
A: Nova separates schema changes into "expand" and "contract" phases. Expand migrations add new columns/tables (backward compatible). Contract migrations remove old columns (breaking). Process: (1) Run expand migrations before upgrading services. (2) Upgrade all services. (3) Run online data migrations to backfill new columns. (4) Run contract migrations only after all services are on the new version. This ensures no service reads/writes columns that don't exist.

**Q3: How do you test that a new scheduler filter doesn't break existing workloads?**
A: (1) Deploy the filter in "audit mode" — it runs but its results are logged, not enforced. Compare filter results with expected behavior. (2) Use Rally to run scheduling benchmarks: create 10,000 VMs with various flavors and check placement quality. (3) Shadow traffic: run a second scheduler instance in parallel with the new filter, compare decisions without acting on them. (4) Canary with a single cell and non-production workloads first.

**Q4: How do you handle rollback if a new nova-compute version has a bug?**
A: Nova's RPC version compatibility allows downgrade. Steps: (1) Stop nova-compute on affected hosts. (2) Reinstall previous version. (3) Start nova-compute. (4) Conductor automatically adjusts RPC version (if auto-pin enabled). Running VMs are unaffected by nova-compute restarts — libvirt manages VM lifecycle independently. The critical constraint: don't run contract DB migrations until all hosts are confirmed stable on the new version.

**Q5: How do you roll out a change to the Placement API resource model (e.g., adding a new custom resource class)?**
A: Custom resource classes are created via API and stored in the DB — no code deployment needed. Steps: (1) Create the resource class: `openstack resource class create CUSTOM_GPU_NVIDIA_H100`. (2) Update compute host configuration to report the new resource (nova.conf or virt driver plugin). (3) Restart nova-compute on hosts with the new resource — they'll report inventory to Placement. (4) Create flavors that request the resource. (5) Users can now launch VMs with the flavor. Rollback: remove the flavor, delete the resource class.

**Q6: How do you handle a situation where a canary cell shows higher error rates after upgrade?**
A: (1) Immediately stop the rollout to other cells. (2) Check error types: if it's a specific API call or instance operation, investigate logs (structured logging with request_id helps). (3) If the error is in nova-compute, check if it's a specific hypervisor configuration. (4) Compare metrics: API latency, scheduler decisions, RabbitMQ queue depth. (5) If the root cause isn't quickly identified, roll back the canary cell. (6) Fix in dev, re-test, and start the canary again. Never proceed with a known-bad rollout.

---

## 13. Trade-offs & Decision Log

| # | Decision | Options Considered | Selected | Rationale |
|---|----------|--------------------|----------|-----------|
| 1 | Hypervisor driver | libvirt/KVM, VMware, Hyper-V, Xen | libvirt/KVM | Open source; best community support; highest performance; all major features (live migration, NUMA, GPU passthrough) |
| 2 | Shared storage for ephemeral | Local disk, NFS, Ceph RBD, GlusterFS | Ceph RBD | Enables live migration without block copy; distributed and self-healing; proven at scale; thin provisioning |
| 3 | Message broker | RabbitMQ, ZeroMQ, Kafka, Apache Qpid | RabbitMQ | Standard for OpenStack; quorum queues provide durability; well-tested; cluster mode for HA |
| 4 | DB replication | MySQL async replication, Galera, MySQL Group Replication | Galera | Synchronous multi-master; zero data loss on node failure; proven in OpenStack deployments |
| 5 | Token format | UUID, Fernet, JWS | Fernet | No DB storage; fast validation; supports key rotation; smaller than UUID tokens |
| 6 | Overcommit strategy | No overcommit, aggressive overcommit, per-resource ratios | Per-resource ratios | VCPU 2:1 allows density for bursty workloads; MEMORY 1:1 prevents OOM; configurable per aggregate |
| 7 | Cell sizing | Large cells (10K hosts), small cells (1K hosts) | 3,000-5,000 hosts/cell | Balances operational complexity vs blast radius; keeps RabbitMQ and MySQL comfortable |
| 8 | Scheduler spread vs stack | Spread (distribute VMs), Stack (fill hosts) | Spread (default) | Better fault isolation (host failure affects fewer VMs per project); easier live migration (more free capacity) |
| 9 | Instance HA | No HA (user responsibility), Masakari (automatic), heat-based | Masakari for critical workloads | Automatic evacuation on host failure; configurable per-project; requires shared storage |
| 10 | Telemetry | Ceilometer/Gnocchi, Prometheus/Grafana, both | Prometheus + Ceilometer | Prometheus for operational metrics (fast, unified); Ceilometer for billing/metering (integrated with OpenStack) |

---

## 14. Agentic AI Integration

### AI-Enhanced Nova Operations

| Use Case | AI Approach | Implementation |
|----------|------------|----------------|
| **Predictive scheduling** | ML model predicts VM resource usage patterns; scheduler uses predictions for placement | Train on Ceilometer metrics (CPU, memory, network). Model outputs expected resource utilization profile. Scheduler weigher uses predicted utilization instead of requested resources. Reduces stranded capacity by 20-30%. |
| **Anomaly detection on compute hosts** | Unsupervised learning detects abnormal resource patterns | Monitor per-host metrics (CPU steal, memory ballooning, I/O latency). Detect degraded hardware before failure. Auto-disable host in scheduler and trigger preemptive migration. |
| **Capacity forecasting** | Time-series forecasting (Prophet/LSTM) predicts capacity needs | Feed historical instance creation rates, resource utilization trends. Forecast when cells will exhaust capacity. Trigger automated procurement or cell provisioning. |
| **Intelligent live migration** | RL agent optimizes migration timing and target selection | Agent observes workload patterns (batch jobs quieter at night). Selects optimal migration window to minimize application impact. Learns from migration outcomes (downtime, failure rate). |
| **Natural language operations** | LLM-powered chatops for common Nova operations | "Migrate all VMs off host-042 for maintenance" → translates to API calls. Validates safety (checks server groups, anti-affinity before migration). Explains current cluster state in natural language. |
| **Auto-remediation** | AI agent diagnoses and resolves common issues | Detects: instance stuck in BUILD → checks conductor logs, RabbitMQ connectivity, host capacity. Takes action: retry build, select alternate host, or escalate. Learns from operator actions. |

### Integration Architecture

```
┌────────────────────────────────────────────┐
│            AI Operations Layer              │
│  ┌─────────────┐  ┌──────────────────────┐ │
│  │ Prediction  │  │ Anomaly Detection   │ │
│  │ Service     │  │ Service             │ │
│  └──────┬──────┘  └──────────┬──────────┘ │
│         │                    │             │
│  ┌──────▼────────────────────▼──────────┐ │
│  │        Feature Store (Redis)         │ │
│  │  host_metrics, instance_profiles,    │ │
│  │  migration_history, failure_patterns │ │
│  └──────────────────┬──────────────────┘ │
│                     │                     │
└─────────────────────┼─────────────────────┘
                      │
          ┌───────────▼───────────┐
          │   Nova Custom Weigher │ ← Scheduler plugin
          │   (ai_weigher.py)     │
          └───────────────────────┘
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through exactly what happens when a user runs `openstack server create`.**
A: The CLI sends POST to nova-api. nova-api: validates Keystone token, checks RBAC policy, validates request (flavor exists, image exists, network exists), checks quotas, creates instance record in nova_api DB (vm_state=BUILDING), creates request_spec, sends `build_instances` RPC cast to nova-conductor. nova-conductor sends `select_destinations` RPC call to nova-scheduler. Scheduler queries Placement API for allocation candidates, applies filters (AZ, RAM, NUMA, PCI, etc.), applies weights (RAM, disk, metrics), selects host, claims allocations in Placement. Returns host to conductor. Conductor maps instance to cell, writes to cell DB, sends `build_and_run_instance` to nova-compute on selected host. nova-compute downloads image from Glance (if not cached), creates port in Neutron, creates block device mappings, generates libvirt XML domain definition, calls `virsh define` + `virsh start`, updates instance state to ACTIVE.

**Q2: How does Nova handle a situation where the selected compute host fails during VM build?**
A: When nova-compute encounters a build failure, it sends an RPC back to nova-conductor with the error. Conductor checks the retry count (configurable via `max_build_retries`, default 0). If retries remain, conductor re-calls the scheduler for a new host (excluding the failed host via `RetryFilter`). If retries exhausted, the instance moves to ERROR state. The Placement allocation on the failed host is cleaned up, and new allocations are claimed on the retry host. If nova-compute crashes entirely (no error RPC), the conductor's periodic task detects the build timeout and marks the instance ERROR.

**Q3: Explain the difference between live migration, cold migration, resize, and evacuation.**
A: **Live migration**: moves a running VM to another host with near-zero downtime. Memory is copied iteratively (pre-copy) or on-demand (post-copy). **Cold migration**: stops the VM, copies disk data to destination, starts on new host. Used when live migration isn't possible. **Resize**: changes the flavor (CPU/RAM/disk). Implemented as cold migration to a new host with new resource allocations. User must confirm or revert. **Evacuation**: recreates a VM on a new host after the original host fails. Requires shared storage or boot-from-volume. Instance data on local disk is lost if not shared.

**Q4: How does Nova support GPU passthrough?**
A: (1) Configure PCI passthrough in nova.conf on compute host: `pci.alias = {"name":"gpu-a100", "vendor_id":"10de", "product_id":"20b5"}`. (2) nova-compute detects PCI devices and reports them to Placement as a nested resource provider with custom resource class `CUSTOM_GPU_NVIDIA_A100`. (3) Create flavor with extra_spec: `resources:CUSTOM_GPU_NVIDIA_A100=1`. (4) Scheduler filters: `PciPassthroughFilter` validates device availability. Placement pre-filters by resource class. (5) On build: libvirt generates XML with `<hostdev>` for PCI passthrough or `<mdev>` for vGPU.

**Q5: What is the purpose of Cells v2, and how does it differ from Cells v1?**
A: Cells v2 (mandatory since Queens) partitions the deployment into cells, each with its own database and message queue. This isolates failure domains and scales beyond single-DB limits. Unlike Cells v1 (deprecated), Cells v2: (1) uses a global Placement API (not per-cell scheduling), (2) has a global nova_api DB with instance_mappings (scatter-gather for cross-cell queries), (3) requires cell0 for instances that fail to schedule, (4) supports cross-cell resize (since Yoga). Every deployment has at least one cell (cell1) plus cell0.

**Q6: How does the quota system work, and what are its limitations?**
A: Quotas are stored in the nova_api database per project. When an API request arrives, Nova checks current usage (COUNT of instances, SUM of vcpus/ram for active instances in the project) against the quota limit. If over, HTTP 403 is returned. Limitations: (1) Race condition: two concurrent creates might both pass the check (usage is read at check time, not locked). Mitigation: use usages table with row-level locking. (2) Cross-cell inaccuracy: usage must be aggregated from all cells (scatter-gather), which can be slow. (3) Quota doesn't account for resources consumed by system overhead (e.g., hypervisor overhead per VM).

**Q7: How would you debug a "NoValidHost" error?**
A: (1) Check the scheduler logs — they log which filter rejected which hosts and why. (2) Verify Placement has inventory for the requested resources: `openstack allocation candidate list --resource VCPU=<n>,MEMORY_MB=<m>`. (3) Check if the AZ is correct and has hosts. (4) Check host aggregates if using AggregateInstanceExtraSpecsFilter. (5) Check compute services are up: `openstack compute service list`. (6) Check if hosts have the `COMPUTE_STATUS_DISABLED` trait. (7) Check PCI devices if GPU is requested. (8) Check NUMA topology fit if using pinned instances. Common causes: genuine capacity exhaustion, misconfigured AZ, stale Placement inventory, disabled hosts.

**Q8: How does Nova handle NUMA-aware scheduling?**
A: Flavors with `hw:numa_nodes=N` trigger NUMA-aware placement. nova-compute reports NUMA topology as nested resource providers in Placement (one child per NUMA node with VCPU and MEMORY_MB inventory). The `NUMATopologyFilter` validates that the requested NUMA layout can fit on the host: it checks that each NUMA node has enough free VCPUs and memory, respects CPU pinning constraints, and validates huge page availability. Libvirt generates XML with `<numatune>` and `<cputune>` elements that bind VM vCPUs to host pCPUs on specific NUMA nodes, and memory is allocated from the corresponding NUMA node.

**Q9: What is the metadata service, and why is it important?**
A: The metadata service (nova-metadata-api, port 169.254.169.254) provides instance-specific data to VMs: hostname, SSH public keys, user-data (cloud-init scripts), network configuration. Cloud-init inside the VM queries this on boot. It's served via a well-known link-local IP. In production, Neutron metadata agent proxies requests to nova-metadata-api, adding the instance ID based on the source IP/port. Security concern: instances must not be able to access another instance's metadata, which is why the proxy adds authentication based on network identity.

**Q10: How would you implement a custom scheduler filter?**
A: Create a Python class inheriting from `nova.scheduler.filters.BaseHostFilter`. Implement `host_passes(self, host_state, spec_obj)` that returns True if the host is suitable. The `host_state` object contains host resources, aggregates, and metadata. The `spec_obj` contains the instance request (flavor, image, scheduler hints). Register the filter in nova.conf's `enabled_filters` list. Example: a filter that excludes hosts in a "draining" aggregate: check if any aggregate metadata has `draining=true`. Deploy via `pip install` on scheduler nodes and restart.

**Q11: Explain how server group anti-affinity is enforced.**
A: When a user creates a server group with `anti-affinity` policy and boots an instance with `--hint group=<uuid>`, the scheduler receives the group hint in the request_spec. The `ServerGroupAntiAffinityFilter` queries all instances in the group and their current hosts. It rejects any candidate host that already runs a group member. This ensures each member runs on a distinct host. Limitation: if the group has more members than available hosts, scheduling fails. Performance consideration: for large groups, the filter must query many instances — caching helps.

**Q12: How does Nova interact with Neutron during VM creation?**
A: During build, nova-compute calls Neutron to (1) create a port on the requested network (if not pre-created), (2) get the port's VIF details (MAC address, IP, network type, segmentation ID). Nova passes VIF details to the hypervisor driver, which configures the VM's network interface (e.g., OVS port). After the VM starts, nova-compute calls Neutron to bind the port to the host — Neutron then programs the OVS flows (or other backend) to connect the port to the network. If Neutron is slow or down during build, the instance goes to ERROR state.

**Q13: What is the nova-manage command, and when would you use it?**
A: `nova-manage` is an administrative CLI for operations that bypass the API: (1) `db sync` — run DB migrations during upgrades. (2) `cell_v2 discover_hosts` — register new compute hosts. (3) `cell_v2 map_instances` — map instances to cells during migration. (4) `placement heal_allocations` — fix orphaned or incorrect Placement allocations. (5) `db online_data_migrations` — run online data backfills. (6) `db archive_deleted_rows` — move soft-deleted rows to shadow tables. (7) `cell_v2 list_cells` — view cell topology. It's used for Day 2 operations, troubleshooting, and upgrades.

**Q14: How would you handle a scenario where a compute host's clock is skewed?**
A: Clock skew can cause: (1) Fernet token validation failures (token appears expired). (2) Instance state reporting out of order. (3) Ceph issues (auth tokens invalid). Mitigation: (1) NTP (chrony) on all hosts, synced to internal NTP servers. (2) Set `auth_token` middleware to allow clock skew tolerance. (3) nova-compute's resource reporting uses monotonic clocks for intervals. (4) Galera cluster requires clocks within a few seconds. Detection: monitor clock drift with Prometheus `node_timex_offset_seconds`. Alert on drift > 1s.

**Q15: How do you archive old instance data to keep the database performant?**
A: Nova uses soft deletion (deleted column + deleted_at timestamp). Over time, the instances table accumulates hundreds of thousands of soft-deleted rows, slowing queries. Solution: (1) Run `nova-manage db archive_deleted_rows --max_rows 10000` periodically (cron job). This moves soft-deleted rows from instances, instance_extra, block_device_mapping, etc. to shadow tables (shadow_instances, etc.). (2) Shadow tables can be periodically purged or dumped to cold storage. (3) Alternative: set up a separate reporting database and replicate there before purging. Target: keep active tables under 1M rows.

**Q16: What is the difference between nova-compute's resource tracker and the Placement API?**
A: The resource tracker (RT) runs inside nova-compute and maintains a local view of the host's resources. It queries the hypervisor (libvirt) for actual resource usage and reports to Placement via the `update_available_resource` periodic task (every 60s). Placement maintains the global view used by the scheduler. The RT is the "source of truth" for what the host actually has — Placement is the "booking system" for what's been allocated. They can diverge: RT detects local changes (VM crashed, consuming less CPU), while Placement tracks allocations (reserved but not yet consumed). The `_heal_allocations` audit reconciles them.

---

## 16. References

| # | Resource | URL |
|---|----------|-----|
| 1 | Nova Developer Documentation | https://docs.openstack.org/nova/latest/ |
| 2 | Nova Architecture Overview | https://docs.openstack.org/nova/latest/user/architecture.html |
| 3 | Placement API Reference | https://docs.openstack.org/placement/latest/ |
| 4 | Nova Cells v2 Layout | https://docs.openstack.org/nova/latest/user/cellsv2-layout.html |
| 5 | Nova Scheduler Filters | https://docs.openstack.org/nova/latest/admin/scheduling.html |
| 6 | Nova Live Migration | https://docs.openstack.org/nova/latest/admin/live-migration-usage.html |
| 7 | OpenStack Operations Guide | https://docs.openstack.org/operations-guide/ |
| 8 | Galera Cluster for MySQL | https://galeracluster.com/library/documentation/ |
| 9 | RabbitMQ Quorum Queues | https://www.rabbitmq.com/docs/quorum-queues |
| 10 | Masakari (Instance HA) | https://docs.openstack.org/masakari/latest/ |
| 11 | Oslo.Versionedobjects | https://docs.openstack.org/oslo.versionedobjects/latest/ |
| 12 | Ceph RBD with Nova | https://docs.ceph.com/en/latest/rbd/rbd-openstack/ |
