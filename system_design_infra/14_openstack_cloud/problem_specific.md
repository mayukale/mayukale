# Problem-Specific Notes — OpenStack Cloud (14_openstack_cloud)

## cloud_control_plane_design

### Unique Purpose
Unified REST API surface for 10 OpenStack services on 10,000 compute hosts. 5,000 API req/sec, 10,000 concurrent connections, 10,000 msg/sec RabbitMQ, 5,000 TPS database. 99.99% availability.

### Unique Architecture
- **Multi-service API surface**: Keystone, Nova, Neutron, Cinder, Glance, Placement, Heat, Barbican, Octavia, Designate — all fronted by single HAProxy VIP
- **Controller node layout**: 3–5 controller nodes; each runs all API services (active-active) + 1 RabbitMQ node + 1 Galera node
- **Keepalived VRRP**: active controller holds VIP; passive monitors via VRRP; failover < 3 s

### RabbitMQ Exchange Layout
```
Exchange: nova (topic)
  Routing keys: conductor, scheduler, compute.host-001 … compute.host-10000

Exchange: neutron (topic)
  Routing keys: q-plugin, dhcp_agent.host-net-001, l3_agent.host-net-001

Exchange: cinder (topic)
  Routing keys: cinder-scheduler, cinder-volume.host-storage-001
```

### Database Sizing
| Database | Size | Contents |
|----------|------|----------|
| keystone | 500 MB | users, projects, roles, assignments |
| nova_api | 1 GB | flavors, request_specs, cell_mappings |
| nova_cell0/1/2 | 30 GB total | instances, migrations, compute_nodes |
| neutron | 2 GB | networks, ports, subnets, security_groups |
| cinder | 1 GB | volumes, snapshots, backups |
| glance | 200 MB | image metadata |
| heat | 500 MB | stacks, resources, events |
| barbican | 200 MB | secrets, containers, orders |
| placement | 500 MB | resource_providers, inventories, allocations |
| **Total** | **~40 GB** | |

### RabbitMQ Sizing (10,000 hosts)
```
Each compute host creates ~3 queues
10,000 hosts × 3 queues = 30,000 queues per cell
Memory per queue: ~30 KB
Total: 30,000 × 30 KB = 900 MB per cell
Message rate: ~3,000 msg/s per cell (resource updates + builds + migrations)
RabbitMQ cluster: 3 nodes × 64 GB RAM = sufficient
Quorum queue: leader + 2 followers; write ACK from majority (2 of 3); failover < 10 s
```

### Galera Configuration
```ini
[mysqld]
wsrep_cluster_name = openstack-galera
wsrep_cluster_address = gcomm://controller-1,controller-2,controller-3
```

### Unique NFRs
- 5,000 API req/sec global; 10,000 concurrent connections
- Keystone auth: 1,000 req/sec; Nova API: 2,000 req/sec; Neutron: 500 req/sec
- Memcached: < 1 ms get/set; 4 GB per controller
- Single node failure recovery: < 30 s
- Bandwidth: API traffic 500 Mbps; RabbitMQ replication 200 Mbps; Galera replication 100 Mbps

---

## multi_region_cloud_platform

### Unique Purpose
5 regions × 10,000 hosts = 50,000 total hosts; 2,000,000 total VMs. Single federated identity (Keystone), shared image replication, global quotas, centralized monitoring (Thanos federation), multi-region Heat orchestration.

### Unique Architecture
- **Single global Keystone** (US-East): token issuance requires cross-region call (~80 ms); validation is local via Fernet
- **Per-region Keystone proxy**: caches tokens + catalog locally (5-min TTL); catalog ~50 KB (10 services × 5 regions)
- **Fernet key distribution**: keys rotated every 30 min; 3 stages; synced to all regions before rotation
- **GeoDNS (GSLB)**: resolves API endpoints to nearest region; DR failover via DNS TTL
- **DCI (100 Gbps dark fiber/MPLS)**: carries image replication, DR replication, tenant VPN, control plane cross-region traffic

### Regional Architecture
```
5 regions: US-East, US-West, EU-West, APAC-SE, APAC-NE
3 AZs per region (separate power domains)
10,000 compute hosts per region
400,000 VMs per region
2,000,000 total VMs (5 regions)
```

### Fernet Key Repository (per region)
```
/etc/keystone/fernet-keys/
  0  ← staged key (will become primary on next rotation)
  1  ← secondary key (validates tokens signed by old primary)
  2  ← primary key (signs new tokens)
Rotation interval: 30 minutes
```

### DCI Bandwidth Budget
```
Cross-region DR replication:     ~100 Mbps sustained (1 TB/day)
Image replication:               ~10 Mbps (100 GB/day)
Tenant VPN (10% of total):       10–50 Gbps per link
Control plane cross-region:      < 1 Gbps
Total provisioned: 100 Gbps per link pair with 50% headroom = 200 Gbps
```

### Cross-Region Image Copy
```
Time = image_size / bandwidth + overhead
Example: 10 GB image / 10 Gbps link = ~8 s + overhead
Glance storage: 50 TB per region; 250 TB total (5 regions)
Cinder storage: 2 PB per region; 10 PB total
```

### Storage Scale
- Glance images: 50 TB / region (250 TB total)
- Cinder volumes: 2 PB / region (10 PB total)
- Intra-region backbone: 10 Tbps; North-south: 2 Tbps; VXLAN east-west: 10 Tbps aggregate

### Unique NFRs
- Intra-region control plane latency: < 100 ms
- Cross-region API latency: < 300 ms
- Token issuance (cross-region): ~80 ms RTT; subsequent validation: < 5 ms local
- Image replication: < 5 min (for 10 GB image on 10 Gbps link)
- Catalog cache TTL: 5 min; size: ~50 KB per region

---

## openstack_neutron_network_design

### Unique Purpose
Networking for 10,000 hosts, 400,000 VMs, 500,000 ports, 50,000 floating IPs, 25,000 security groups × 10 rules = 250,000 rules. OVN-based distributed control; < 3 s rule propagation to all hosts.

### Unique Architecture
- **ML2 plugin**: type drivers (VXLAN VNI allocation, VLAN, GRE, Flat) + mechanism drivers (OVN, OVS, SR-IOV)
- **OVN**: OVN Northbound DB (logical topology, Raft cluster) → OVN Southbound DB (compiled flows, Raft cluster) → `ovn-controller` on each host (replaces per-host L3/DHCP/metadata agents)
- **OVS bridges**: `br-int` (integration), `br-tun` (VXLAN tunnels), `br-ex` (external/NAT)
- **DVR**: east-west traffic routed locally on each compute host; no central router bottleneck

### Key Schema
```sql
CREATE TABLE networks (
  id          VARCHAR(36) PRIMARY KEY,
  project_id  VARCHAR(36) NOT NULL,
  name        VARCHAR(255),
  status      ENUM('ACTIVE','DOWN') NOT NULL,
  mtu         INT NOT NULL DEFAULT 1500,
  created_at  DATETIME NOT NULL,
  INDEX idx_project (project_id)
) ENGINE=InnoDB;

CREATE TABLE ports (
  id               VARCHAR(36) PRIMARY KEY,
  network_id       VARCHAR(36) NOT NULL,
  device_id        VARCHAR(36),             -- instance UUID
  device_owner     VARCHAR(255),            -- "compute:nova", "network:dhcp", "network:router_interface"
  mac_address      VARCHAR(17) NOT NULL,
  binding_host_id  VARCHAR(255),            -- compute host
  binding_vif_type VARCHAR(255),            -- 'ovs', 'hw_veb' (SR-IOV)
  binding_vnic_type VARCHAR(255),           -- 'normal', 'direct' (SR-IOV)
  status           ENUM('DOWN','ACTIVE') NOT NULL,
  project_id       VARCHAR(36),
  FOREIGN KEY (network_id) REFERENCES networks(id),
  INDEX idx_network (network_id),
  INDEX idx_device (device_id),
  INDEX idx_binding_host (binding_host_id),
  INDEX idx_project (project_id)
) ENGINE=InnoDB;

CREATE TABLE ip_allocations (
  id         INT AUTO_INCREMENT PRIMARY KEY,
  port_id    VARCHAR(36) NOT NULL,
  subnet_id  VARCHAR(36) NOT NULL,
  ip_address VARCHAR(64) NOT NULL,   -- IPv4 or IPv6
  created_at DATETIME NOT NULL,
  UNIQUE KEY (port_id, subnet_id, ip_address),
  FOREIGN KEY (port_id) REFERENCES ports(id) ON DELETE CASCADE,
  INDEX idx_subnet_ip (subnet_id, ip_address)  -- IP uniqueness + DHCP lease lookup
) ENGINE=InnoDB;

CREATE TABLE security_group_rules (
  id                VARCHAR(36) PRIMARY KEY,
  security_group_id VARCHAR(36) NOT NULL,
  direction         ENUM('ingress','egress') NOT NULL,
  protocol          VARCHAR(40),             -- 'tcp','udp','icmp',NULL (any)
  port_range_min    INT,
  port_range_max    INT,
  remote_ip_prefix  VARCHAR(255),            -- CIDR: '10.0.0.0/24'
  remote_group_id   VARCHAR(36),             -- reference another SG
  created_at        DATETIME NOT NULL,
  INDEX idx_security_group (security_group_id)
) ENGINE=InnoDB;
```

### OVS Flow Scale
```
Flows per host (40 VMs × 1,250 flows/VM):
  Security group flows:   ~250 per VM (ingress + egress per rule)
  Forwarding flows:       ~100 per VM (L2/L3 forwarding)
  Total per host:         ~50,000 flows
OVN compiles logical flows to OVS datapath flows;
ovn-controller pushes delta updates on any change
```

### Latency Targets
| Operation | Target |
|-----------|--------|
| Port create (API) | < 500 ms |
| Port binding (OVS flow programming) | < 3 s |
| Security group rule propagation (all 10K hosts) | < 3 s |
| East-west (same rack, VXLAN) | < 0.5 ms |
| East-west (cross-rack) | < 1 ms |
| North-south (router NAT) | < 2 ms |

### Unique NFRs
- 500,000 ports; 25,000 SGs × 10 rules = 250,000 rules
- 50,000 floating IPs (1:1 NAT); 12.5% of VMs
- Network provisioning: < 5 s end-to-end (port → DHCP-ready)
- East-west throughput: > 90% wire speed (VXLAN overhead ~50 bytes)

---

## openstack_nova_compute_design

### Unique Purpose
VM lifecycle on 10,000 hosts, 400,000 VMs. 50–150 VM creates/sec peak. < 200 ms scheduling decision. < 60 s VM boot end-to-end. Placement API pre-filtering scales to 10,000+ hosts.

### Unique Architecture
- **nova-conductor**: mediates nova-api ↔ database; orchestrates build/resize/migration; multiple instances per cell for HA
- **nova-scheduler**: stateless; filter + weight pipeline on Placement API candidates; < 200 ms
- **nova-compute**: per-hypervisor daemon; manages VM lifecycle via libvirt/QEMU; heartbeats to Placement API
- **Cells v2**: `nova_cell0` (failed builds), `nova_cell1`, `nova_cell2`; separate DB + conductor per cell; `nova_api.cell_mappings` routes requests

### Key Schema
```sql
CREATE TABLE instances (
  uuid         VARCHAR(36) PRIMARY KEY,
  project_id   VARCHAR(36) NOT NULL,
  host         VARCHAR(255),
  vm_state     VARCHAR(255),   -- BUILD,ACTIVE,SHUTOFF,PAUSED,SUSPENDED,RESCUED,ERROR,DELETED
  task_state   VARCHAR(255),   -- SCHEDULING,SPAWNING,MIGRATING,...
  power_state  INT,
  created_at   DATETIME NOT NULL,
  updated_at   DATETIME,
  deleted      INT NOT NULL DEFAULT 0,
  INDEX idx_project (project_id),
  INDEX idx_host (host),
  INDEX idx_vm_state (vm_state),
  INDEX idx_deleted_created (deleted, created_at)
) ENGINE=InnoDB;

CREATE TABLE compute_nodes (
  id                BIGINT AUTO_INCREMENT PRIMARY KEY,
  host              VARCHAR(255) NOT NULL,
  vcpus             INT, memory_mb INT, local_gb INT,
  vcpus_used        INT, memory_mb_used INT, local_gb_used INT,
  running_instances INT,
  forced_down       BOOLEAN DEFAULT FALSE,
  created_at        DATETIME NOT NULL,
  updated_at        DATETIME,
  deleted           INT NOT NULL DEFAULT 0,
  INDEX idx_host (host)
) ENGINE=InnoDB;

-- Placement database
CREATE TABLE resource_providers (
  id                   INT AUTO_INCREMENT PRIMARY KEY,
  uuid                 VARCHAR(36) NOT NULL UNIQUE,
  name                 VARCHAR(200) NOT NULL,
  generation           INT NOT NULL DEFAULT 0,   -- optimistic locking
  parent_provider_uuid VARCHAR(36),              -- for NUMA nodes, GPU sub-providers
  root_provider_uuid   VARCHAR(36) NOT NULL,
  INDEX idx_parent (parent_provider_uuid)
) ENGINE=InnoDB;

CREATE TABLE inventories (
  id                   INT AUTO_INCREMENT PRIMARY KEY,
  resource_provider_id INT NOT NULL,
  resource_class_id    INT NOT NULL,   -- VCPU=0, MEMORY_MB=1, DISK_GB=2, CUSTOM_*
  total                INT NOT NULL,
  reserved             INT NOT NULL DEFAULT 0,
  allocation_ratio     FLOAT NOT NULL DEFAULT 1.0,  -- overcommit
  UNIQUE KEY (resource_provider_id, resource_class_id),
  FOREIGN KEY (resource_provider_id) REFERENCES resource_providers(id)
) ENGINE=InnoDB;

CREATE TABLE allocations (
  id                   INT AUTO_INCREMENT PRIMARY KEY,
  resource_provider_id INT NOT NULL,
  consumer_id          VARCHAR(36) NOT NULL,  -- instance UUID
  resource_class_id    INT NOT NULL,
  used                 INT NOT NULL,
  created_at           DATETIME NOT NULL,
  INDEX idx_consumer (consumer_id),
  INDEX idx_resource_provider (resource_provider_id)
) ENGINE=InnoDB;
```

### VM Create Flow
```
1. POST /servers → nova-api: 202 Accepted (< 500 ms)
2. nova-api: INSERT instances (vm_state=BUILDING, task_state=SCHEDULING)
3. RPC Cast → nova-conductor: build_instances
4. nova-conductor: GET /placement/allocation_candidates (SQL pre-filter, < 50 ms)
5. RPC Call → nova-scheduler: select_destinations (< 200 ms, applies filters)
6. nova-scheduler: PUT /placement/allocations/{consumer_id} (claim)
7. RPC → nova-compute: build_and_run_instance
8. nova-compute: download image (cached) + libvirt/QEMU boot
Total time: < 60 s from POST to SSH-accessible
```

### Scheduler Filters
```
AvailabilityZoneFilter        (hard: must match AZ)
RamFilter                     (hard: with ram_allocation_ratio)
DiskFilter                    (hard: with disk_allocation_ratio)
ImagePropertiesFilter         (hard: min_disk, min_ram, hw:cpu_policy)
ServerGroupAntiAffinityFilter (soft/hard: affinity group rules)
NUMATopologyFilter            (hard: NUMA cell + hugepages matching)
```

### Overcommit Defaults
```
cpu_allocation_ratio  = 2.0   (256 pCPUs → 512 vCPUs available)
ram_allocation_ratio  = 1.5   (128 GB → 192 GB available)
disk_allocation_ratio = 1.0   (no overcommit)
GPU hosts: all ratios = 1.0
```

### Unique NFRs
- VM creates: 50/sec average, 150/sec peak (3×)
- Scheduler throughput: 500+ decisions/sec
- Placement API pre-filter: < 50 ms; scheduler filters: < 200 ms total
- Nova DB: ~10 GB total; `instance_extra` (NUMA, PCI) 2 GB

---

## vm_live_migration_system

### Unique Purpose
Near-zero downtime VM migration on 2,000 hosts, 80,000 VMs, up to 256 GB RAM / 256 vCPU. Pre-copy for standard VMs (< 100 ms network cutover), post-copy for large memory VMs, XBZRLE compression. CPU compatibility checking, NUMA topology preservation. 2,000 migrations/day, 150 concurrent per 50-host wave.

### Unique Architecture
- **Pre-copy (default)**: iterative dirty page transfer; converges when `dirty_rate < bandwidth × threshold`; final pause + transfer + network cutover < 100 ms
- **Post-copy**: immediate pause + state transfer → resume on destination, demand-page from source via userfaultfd; avoids non-convergence for active DBs
- **Auto-converge**: throttle vCPUs by `auto_converge_initial_pct = 20%` increments to reduce dirty rate
- **XBZRLE**: XOR-based compression of dirty pages; typical 3:1 ratio; `cache_size = xbzrle_cache_pct × VM_RAM`

### Key Schema
```sql
CREATE TABLE migrations (
  uuid              VARCHAR(36) PRIMARY KEY,
  instance_uuid     VARCHAR(36) NOT NULL,
  source_compute    VARCHAR(255) NOT NULL,
  dest_compute      VARCHAR(255) NOT NULL,
  migration_type    ENUM('live-migration','cold-migration','resize','evacuation'),
  status            ENUM('accepted','running','paused','succeeded','failed','cancelled'),
  memory_total      BIGINT,    -- bytes
  memory_processed  BIGINT,
  disk_total        BIGINT,
  disk_processed    BIGINT,
  created_at        DATETIME NOT NULL,
  updated_at        DATETIME,
  deleted           INT NOT NULL DEFAULT 0,
  INDEX idx_instance (instance_uuid),
  INDEX idx_source (source_compute)
) ENGINE=InnoDB;

CREATE TABLE migration_policies (
  id                          INT AUTO_INCREMENT PRIMARY KEY,
  project_id                  VARCHAR(36),
  max_concurrent_outbound     INT NOT NULL DEFAULT 3,
  max_concurrent_inbound      INT NOT NULL DEFAULT 3,
  bandwidth_limit_mbps        INT NOT NULL DEFAULT 8000,
  timeout_seconds             INT NOT NULL DEFAULT 3600,
  max_retries                 INT NOT NULL DEFAULT 3,
  auto_converge_enabled       TINYINT(1) NOT NULL DEFAULT 1,
  auto_converge_initial_pct   INT NOT NULL DEFAULT 20,
  auto_converge_increment_pct INT NOT NULL DEFAULT 10,
  xbzrle_enabled              TINYINT(1) NOT NULL DEFAULT 1,
  xbzrle_cache_pct            INT NOT NULL DEFAULT 10,  -- % of VM RAM
  postcopy_enabled            TINYINT(1) NOT NULL DEFAULT 0,
  postcopy_after_precopy_rounds INT NOT NULL DEFAULT 5,
  max_downtime_ms             INT NOT NULL DEFAULT 500,
  UNIQUE INDEX idx_policy_project (project_id)
) ENGINE=InnoDB;
```

### Migration Strategy Decision
```
Given: migration_bandwidth = 1 GB/s (8 Gbps)

Pre-copy CONVERGES when:
  dirty_rate < bandwidth × convergence_threshold
  e.g., 200 MB/s < 1,000 MB/s × 0.3 → 200 < 300 → CONVERGES
  → 8 GB VM: ~10 s total; final pause ~13 ms

Pre-copy DOES NOT CONVERGE when:
  dirty_rate (2 GB/s) > bandwidth (1 GB/s)
  Options:
    Auto-converge: throttle vCPUs 20%→30%→40%→50%
      dirty rate: 2.0→1.6→1.28→1.02→0.82 GB/s (converges after 4 steps)
      downside: 20-50% CPU throttling during migration
    Post-copy: pause now, demand-page from source via userfaultfd
      page fault latency: ~0.5 ms per 4 KB page (25 Gbps RDMA)
      risk: VM lost if source dies before all pages fetched
    XBZRLE: effective bandwidth = 1 GB/s × 3 (compression) = 3 GB/s > 2 GB/s
      cache_size = 256 MB (10% × 256 GB VM RAM × 0.1)
```

### Migration Examples
```
Standard VM (8 GB RAM, shared Ceph, 200 MB/s dirty rate):
  Iteration 1: 8 GB      → 8.0 s (1.6 GB dirtied)
  Iteration 2: 1.6 GB    → 1.6 s (320 MB dirtied)
  Iteration 3: 320 MB    → 0.32 s (64 MB)
  Iteration 4: 64 MB     → 0.064 s (12.8 MB)
  Iteration 5 (pause):   → 0.013 s
  Total: ~10 s; pause: ~13 ms + 5 ms network cutover = ~18 ms total

Block migration (100 GB local disk + 8 GB RAM):
  Disk at 500 MB/s: 200 s (dominant); Memory: 8 s (parallel)
  Total: ~210 s (vs ~10 s with shared Ceph = 21× faster with Ceph)

Rolling upgrade (2,000 hosts, 40 VMs/host, 5% waves = 50 hosts/wave):
  Concurrent migrations: 50 × 3 = 150 parallel
  VMs per wave: 50 × 40 = 2,000
  Time per wave: 2,000 / 150 × 10 s = ~133 s ≈ 2.2 min
  40 waves × (2.2 min + 5 min maintenance) = ~288 min ≈ 5 hours
  Network bandwidth: 150 × 8 Gbps = 1.2 Tbps (4.7% of 25.6 Tbps spine)
```

### NUMA Topology Preservation
```json
// migration_numa_topology.numa_topology (source host)
{
  "cells": [
    {"id": 0, "cpus": [0,1,2,3,4,5,6,7], "mem_kb": 65536000,
     "pagesize_kb": 2048, "pinned_cpus": [0,1,2,3]},
    {"id": 1, "cpus": [8,9,10,11,12,13,14,15], "mem_kb": 65536000,
     "pinned_cpus": []}
  ]
}
// Instance pinning must be reproduced identically on destination
// nova-conductor verifies topology compatibility before migration proceeds
```

### Unique NFRs
- Network cutover: < 100 ms; total pause: < 500 ms
- Standard 8 GB VM migration: ~10 s total; ~18 ms downtime
- Concurrent migrations per host: 3 outbound + 3 inbound
- Migration timeout: 3,600 s (configurable per policy)
- Bandwidth limit: 8,000 Mbps per migration (configurable)
- Rolling upgrade (2,000 hosts): ~5 hours total
