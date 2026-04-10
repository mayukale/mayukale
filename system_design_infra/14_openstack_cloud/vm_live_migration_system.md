# System Design: VM Live Migration System (OpenStack Nova)

> **Relevance to role:** VM live migration is a foundational capability of the OpenStack Nova backend used in this role's IaaS platform. Understanding how memory is iteratively transferred between hypervisors, how network continuity is maintained, and how to bound migration downtime to < 100ms are critical topics for a platform engineer managing thousands of VMs on bare-metal hypervisors.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Clarifying Question | Assumed Answer |
|---|-------------|---------------------|----------------|
| F1 | Live migration of running VMs between compute hosts | Should we support both shared-storage and block migration? | Yes, both. Shared storage (Ceph RBD, NFS) is primary; block migration for local disk |
| F2 | Near-zero downtime migration | What is the acceptable downtime SLA? | < 100ms for network cutover; < 500ms total VM pause |
| F3 | Pre-copy iterative memory migration | How many dirty-page iterations before convergence? | Auto-converge after 5 rounds or when dirty rate < bandwidth |
| F4 | Post-copy migration for large-memory VMs | When should post-copy be used vs pre-copy? | When VM dirty rate consistently exceeds migration bandwidth |
| F5 | Network cutover with zero packet loss | How are OVS/OVN flows updated? | Source sends GARP, OVN Southbound DB updates port binding |
| F6 | CPU compatibility checking | What if source and destination have different CPU models? | Use cpu_model=host-model or custom baseline; reject if incompatible |
| F7 | NUMA-aware migration | Must NUMA topology be preserved on destination? | Yes, identical NUMA pinning or fail migration |
| F8 | Migration progress monitoring | Real-time progress or periodic polling? | Both: libvirt events + periodic nova-compute polling |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NF1 | Migration downtime | < 100ms network, < 500ms total pause |
| NF2 | Concurrent migrations per host | Max 3 outbound + 3 inbound simultaneously |
| NF3 | Migration bandwidth limit | Configurable, default 8 Gbps per migration |
| NF4 | Migration timeout | Auto-cancel after 3600s (configurable) |
| NF5 | Success rate | > 99.5% for migrations that pass pre-checks |
| NF6 | Large VM support | VMs up to 1 TB RAM, 256 vCPUs |
| NF7 | Storage migration throughput | > 500 MB/s for block migration |

---

## 2. Scale & Capacity Estimates

### Cluster Parameters
```
Compute hosts:          2,000 nodes
VMs per host:           40 average (80,000 total VMs)
Average VM RAM:         8 GB
Large VMs (>64 GB):     5% of fleet = 4,000 VMs
Migration events/day:   2,000 (maintenance, rebalancing, evacuations)
Peak migrations/hour:   500 (during rolling upgrades)
```

### Migration Bandwidth Calculation
```
Single VM migration (8 GB RAM, shared storage):
  Initial memory transfer:    8 GB / (8 Gbps / 8) = 8 GB / 1 GB/s = 8 seconds
  Dirty page rate (typical):  200 MB/s for active workload
  
  Iteration 1: Transfer 8 GB          -> 8.0s (meanwhile 1.6 GB dirtied)
  Iteration 2: Transfer 1.6 GB        -> 1.6s (meanwhile 320 MB dirtied)
  Iteration 3: Transfer 320 MB        -> 0.32s (meanwhile 64 MB dirtied)
  Iteration 4: Transfer 64 MB         -> 0.064s (meanwhile 12.8 MB dirtied)
  Iteration 5: Pause VM, transfer remaining 12.8 MB -> 0.013s
  
  Total migration time:  ~10.0 seconds
  VM pause (downtime):   ~13 ms + network cutover ~5 ms = ~18 ms total
  
  Convergence condition: dirty_rate < migration_bandwidth * convergence_threshold
  200 MB/s < 1000 MB/s * 0.3 -> 200 < 300 -> CONVERGES
```

### Large VM Migration (256 GB RAM)
```
Active database VM (256 GB RAM, 2 GB/s dirty rate):
  Migration bandwidth: 8 Gbps = 1 GB/s
  
  dirty_rate (2 GB/s) > bandwidth (1 GB/s) -> PRE-COPY WILL NOT CONVERGE
  
  Options:
  1. Auto-converge: Throttle vCPUs by 20% increments
     - Reduce dirty rate: 2.0 -> 1.6 -> 1.28 -> 1.02 -> 0.82 GB/s
     - After 4 throttle steps (80% throttle): converges
     - Downside: significant performance impact during migration
     
  2. Post-copy migration:
     - Pause VM immediately, transfer CPU state + dirty bitmap -> ~100ms
     - Resume on destination, demand-page from source via userfaultfd
     - Page fault latency: ~0.5ms per 4 KB page over 25 Gbps RDMA
     - Working set (20% of 256 GB = 51.2 GB): ~51 seconds to fault in
     - Risk: if source dies, VM is lost (pages split across hosts)
     
  3. XBZRLE compression:
     - Compress dirty pages with XOR-based run-length encoding
     - Typical compression ratio: 3:1 for similar pages
     - Effective bandwidth: 1 GB/s * 3 = 3 GB/s > 2 GB/s dirty rate
     - Cache size required: 256 MB (1/1000 of RAM)
```

### Block Migration Bandwidth
```
VM with 100 GB local disk + 8 GB RAM:
  Disk transfer at 500 MB/s:  100 GB / 500 MB/s = 200 seconds
  Memory transfer (parallel):  8 GB / 1 GB/s = 8 seconds
  Total: ~210 seconds (disk-dominated)
  
  With Ceph RBD (shared storage): disk transfer = 0 (only memory)
  Total: ~10 seconds (100x faster)
```

### Cluster-Wide Migration Capacity
```
During rolling upgrade (all 2000 hosts evacuated in rolling waves):
  Hosts per wave:           50
  VMs per wave:             50 * 40 = 2,000 VMs
  Concurrent migrations:    50 hosts * 3 outbound = 150 parallel
  Time per VM:              ~10 seconds average
  Time per wave:            2,000 / 150 * 10 = ~133 seconds ~ 2.2 minutes
  Total waves:              2,000 / 50 = 40 waves
  Total upgrade time:       40 * (2.2 + 5 min maintenance) = ~288 minutes ~ 5 hours
  
  Network bandwidth per wave: 150 * 8 Gbps = 1.2 Tbps migration traffic
  Spine switch capacity:      typically 25.6 Tbps -> 4.7% utilization (acceptable)
```

---

## 3. High Level Architecture

### Migration System Architecture
```
+------------------------------------------------------------------+
|                        Nova API Service                            |
|  POST /servers/{id}/action {"os-migrateLive": {...}}              |
+------------------------+------------------------------------------+
                         | RPC: live_migrate_instance
                         v
+------------------------------------------------------------------+
|                     Nova Conductor                                 |
|  +---------------------------------------------------------------+|
|  |              LiveMigrationTask                                 ||
|  |  1. Check instance state (ACTIVE/PAUSED)                      ||
|  |  2. Check source host status                                  ||
|  |  3. Select destination (scheduler or specified)               ||
|  |  4. Pre-migration checks:                                     ||
|  |     - CPU compatibility (compare traits)                      ||
|  |     - NUMA topology match                                     ||
|  |     - Disk availability (block migration)                     ||
|  |     - Network connectivity                                    ||
|  |  5. Claim resources on destination (Placement API)            ||
|  |  6. Send RPC to source compute                                ||
|  +---------------------------------------------------------------+|
+------------------------+------------------------------------------+
                         | RPC: live_migration
                         v
+---------------------------------------+  +-----------------------------+
|        Source Nova-Compute            |  | Destination Nova-Compute    |
|  +-----------------------------+      |  | +-------------------------+ |
|  |   LiveMigrationManager      |      |  | | pre_live_migration()    | |
|  |                             |      |  | | - Prepare destination   | |
|  |  1. pre_live_migration() --+--RPC--+->| | - Create libvirt XML    | |
|  |     on destination          |      |  | | - Setup network ports   | |
|  |  2. Start libvirt migration |      |  | | - Mount shared storage  | |
|  |  3. Monitor progress        |      |  | +-------------------------+ |
|  |  4. Network cutover         |      |  |                             |
|  |  5. post_live_migration() --+-RPC--+->| post_live_migration_at_     |
|  |     cleanup source          |      |  | destination()               |
|  +----------+------------------+      |  | - Activate network ports    |
|             |                         |  | - Update port bindings      |
|             v                         |  | - Confirm resource claims   |
|  +-----------------------------+      |  +-----------------------------+
|  |     Libvirt / QEMU          |      |
|  |  +------------------------+ |      |         Libvirt / QEMU
|  |  | virDomainMigrateToURI  | |      |  +------------------------+
|  |  | - Pre-copy loop        +-+-TCP--+->| qemu-kvm (dest)        |
|  |  | - Dirty page tracking  | |:49152|  | - Receive pages        |
|  |  | - Auto-converge        | |      |  | - Build VM state       |
|  |  | - Compression (XBZRLE) | |      |  | - Resume execution     |
|  |  +------------------------+ |      |  +------------------------+
|  +-----------------------------+      |
+---------------------------------------+

Migration Data Flow:
  +---------------+                              +---------------+
  | Source QEMU   |  ---- Memory Pages ---->     | Dest QEMU     |
  |               |  ---- Device State ---->     |               |
  |               |  ---- CPU Registers --->     |               |
  +-------+-------+                              +-------+-------+
          |                                              |
          v                                              v
  +---------------+    Ceph RADOS (shared)       +---------------+
  | RBD Volume    | <===========================>| RBD Volume    |
  | (no copy)     |    Same pool/image           | (same refs)   |
  +---------------+                              +---------------+
```

### Network Cutover Sequence
```
Time --------------------------------------------------------------------->

Source Host                              Destination Host
-----------                              ----------------
VM running, packets                      VM paused (receiving
flowing via source port                  memory pages)
     |                                        |
     |  +-- PAUSE VM -----------------------+ |
     |  |  Transfer final dirty pages       | |
     |  |  Transfer device state            | |
     |  |  Transfer CPU registers           | |
     |  +-----------------------------------+ |
     |                                        |
     |         +--- NETWORK CUTOVER ---+      |
     |         |                       |      |
     v         v                       v      v
  1. Nova updates Neutron port binding:
     PUT /v2.0/ports/{port_id}
       {"port": {"binding:host_id": "dest-host"}}
     
  2. OVN Southbound DB updates:
     - Delete old port_binding on source chassis
     - Create new port_binding on dest chassis
     
  3. ovn-controller on dest:
     - Install OpenFlow rules for VM port
     - Send GARP (Gratuitous ARP) from VM's MAC
     
  4. ovn-controller on source:
     - Remove OpenFlow rules for VM port
     
  5. Physical switches:
     - Learn new MAC location from GARP
     - Update forwarding tables
     
  <-- Total cutover: 2-10 ms -->
     |                                   RESUME VM
     |                                   VM running on
     |                                   destination
  Cleanup:                                    |
  - Release resources                         |
  - Delete libvirt domain                     |
  - Update Placement API                      |
```

### Pre-copy Algorithm State Machine
```
                    +--------------+
                    |   INACTIVE   |
                    +------+-------+
                           | migrate_start()
                           v
                    +--------------+
                    | PRE_MIGRATE  |  Pre-checks, claim resources
                    |   _CHECKS    |  on destination
                    +------+-------+
                           | checks_passed
                           v
                    +--------------+
                    |  PREPARING   |  Setup dest: create domain XML,
                    |  DESTINATION |  connect networks, mount storage
                    +------+-------+
                           | dest_ready
                           v
                    +--------------+
         +-------->|  ITERATING   |  Transfer dirty pages
         | dirty   |  (PRE-COPY)  |  Track convergence
         | pages   +------+-------+
         | remain         | converged OR max_iterations
         |                v
         |         +--------------+
         +---------| CONVERGENCE  |---- dirty_rate > bandwidth?
                   |   CHECK      |         |
                   +------+-------+         | yes
                          | converged       v
                          |          +--------------+
                          |          |AUTO_CONVERGE | Throttle CPU
                          |          |  / XBZRLE    | or compress
                          |          +------+-------+
                          |                 | now converged
                          v                 |
                   +--------------+ <-------+
                   |  VM_PAUSED   |  Pause VM, final page transfer
                   +------+-------+
                          | state_transferred
                          v
                   +--------------+
                   |  NETWORK     |  Port binding update, GARP
                   |  CUTOVER     |
                   +------+-------+
                          | cutover_complete
                          v
                   +--------------+
                   |  RESUMING    |  Resume VM on destination
                   +------+-------+
                          | vm_running
                          v
                   +--------------+
                   |  POST_MIGRATE|  Cleanup source, update DB
                   +------+-------+
                          | cleanup_done
                          v
                   +--------------+
                   |  COMPLETED   |
                   +--------------+
                   
  At any point:
  ERROR --> ROLLBACK --> VM running on source (unchanged)
```

---

## 4. Data Model

### Core Migration Tables

```sql
-- Primary migration tracking table (Nova cell database)
CREATE TABLE migrations (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    uuid                VARCHAR(36) NOT NULL UNIQUE,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    deleted_at          DATETIME DEFAULT NULL,
    deleted             INT DEFAULT 0,
    
    -- Instance reference
    instance_uuid       VARCHAR(36) NOT NULL,
    
    -- Source and destination
    source_compute      VARCHAR(255) NOT NULL,
    source_node         VARCHAR(255) NOT NULL,
    dest_compute        VARCHAR(255) DEFAULT NULL,
    dest_node           VARCHAR(255) DEFAULT NULL,
    dest_host           VARCHAR(255) DEFAULT NULL,
    
    -- Migration type and status
    migration_type      ENUM('migration', 'resize', 'live-migration', 'evacuation') NOT NULL,
    status              VARCHAR(255) NOT NULL DEFAULT 'accepted',
    -- Status values: accepted -> pre-migrating -> running -> post-migrating -> completed
    --                                                      -> failed / cancelled / error
    
    -- Flavor tracking (for resize migrations)
    old_instance_type_id INT DEFAULT NULL,
    new_instance_type_id INT DEFAULT NULL,
    
    -- Cross-cell migration support (Cells v2)
    cross_cell_move     TINYINT(1) DEFAULT 0,
    
    -- Resource tracking
    memory_total        BIGINT DEFAULT NULL,      -- Total memory to migrate (bytes)
    memory_processed    BIGINT DEFAULT NULL,       -- Memory transferred so far
    memory_remaining    BIGINT DEFAULT NULL,       -- Memory remaining
    disk_total          BIGINT DEFAULT NULL,       -- Total disk to migrate (bytes)
    disk_processed      BIGINT DEFAULT NULL,
    disk_remaining      BIGINT DEFAULT NULL,
    
    INDEX idx_migrations_instance (instance_uuid),
    INDEX idx_migrations_source (source_compute),
    INDEX idx_migrations_dest (dest_compute),
    INDEX idx_migrations_status (status),
    INDEX idx_migrations_type_status (migration_type, status),
    INDEX idx_migrations_updated (updated_at),
    
    CONSTRAINT fk_migrations_instance 
        FOREIGN KEY (instance_uuid) REFERENCES instances(uuid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Migration progress tracking (detailed per-iteration stats)
CREATE TABLE migration_progress (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    migration_uuid      VARCHAR(36) NOT NULL,
    created_at          DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    
    -- Iteration tracking
    iteration           INT NOT NULL,
    phase               ENUM('setup', 'precopy', 'postcopy', 'pause', 'cutover', 'cleanup') NOT NULL,
    
    -- Memory transfer stats (from libvirt domJobInfo)
    memory_total_bytes      BIGINT NOT NULL DEFAULT 0,
    memory_processed_bytes  BIGINT NOT NULL DEFAULT 0,
    memory_remaining_bytes  BIGINT NOT NULL DEFAULT 0,
    memory_bps              BIGINT NOT NULL DEFAULT 0,      -- Current transfer rate
    dirty_rate_bps          BIGINT NOT NULL DEFAULT 0,      -- Current page dirty rate
    dirty_sync_count        INT NOT NULL DEFAULT 0,         -- Number of dirty syncs
    
    -- Disk transfer stats (block migration only)
    disk_total_bytes        BIGINT NOT NULL DEFAULT 0,
    disk_processed_bytes    BIGINT NOT NULL DEFAULT 0,
    disk_remaining_bytes    BIGINT NOT NULL DEFAULT 0,
    disk_bps                BIGINT NOT NULL DEFAULT 0,
    
    -- Compression stats (XBZRLE)
    xbzrle_cache_size       BIGINT DEFAULT NULL,
    xbzrle_bytes_transferred BIGINT DEFAULT NULL,
    xbzrle_pages_transferred BIGINT DEFAULT NULL,
    xbzrle_cache_miss_rate  FLOAT DEFAULT NULL,
    xbzrle_overflow         BIGINT DEFAULT NULL,
    
    -- Auto-converge stats
    auto_converge_throttle  INT DEFAULT 0,  -- CPU throttle percentage (0-99)
    
    -- Downtime tracking
    expected_downtime_ms    INT DEFAULT NULL,
    actual_downtime_ms      INT DEFAULT NULL,  -- Only set in final iteration
    setup_time_ms           INT DEFAULT NULL,
    total_time_ms           INT DEFAULT NULL,
    
    INDEX idx_progress_migration (migration_uuid),
    INDEX idx_progress_migration_iter (migration_uuid, iteration),
    
    CONSTRAINT fk_progress_migration 
        FOREIGN KEY (migration_uuid) REFERENCES migrations(uuid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- CPU compatibility records (cached per host pair)
CREATE TABLE migration_cpu_compatibility (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    created_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at          DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    source_host         VARCHAR(255) NOT NULL,
    dest_host           VARCHAR(255) NOT NULL,
    
    -- CPU model information
    source_cpu_model    VARCHAR(255) NOT NULL,       -- e.g., "Cascadelake-Server"
    dest_cpu_model      VARCHAR(255) NOT NULL,        -- e.g., "Icelake-Server"
    baseline_cpu_model  VARCHAR(255) DEFAULT NULL,    -- Negotiated common model
    
    -- Feature flags comparison
    source_cpu_flags    TEXT NOT NULL,                 -- Comma-separated CPU flags
    dest_cpu_flags      TEXT NOT NULL,
    missing_flags       TEXT DEFAULT NULL,             -- Flags on source not on dest
    extra_flags         TEXT DEFAULT NULL,             -- Flags on dest not on source
    
    -- Compatibility result
    is_compatible       TINYINT(1) NOT NULL,
    compatibility_notes TEXT DEFAULT NULL,
    
    UNIQUE INDEX idx_cpu_compat_hosts (source_host, dest_host),
    INDEX idx_cpu_compat_source (source_host),
    INDEX idx_cpu_compat_dest (dest_host)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- NUMA topology snapshot for migration validation
CREATE TABLE migration_numa_topology (
    id                  INT AUTO_INCREMENT PRIMARY KEY,
    migration_uuid      VARCHAR(36) NOT NULL,
    
    -- Which side this topology represents
    host_role           ENUM('source', 'destination') NOT NULL,
    host_name           VARCHAR(255) NOT NULL,
    
    -- NUMA node details (JSON for flexibility)
    numa_topology       JSON NOT NULL,
    /*  Example:
    {
      "cells": [
        {
          "id": 0,
          "cpus": [0,1,2,3,4,5,6,7],
          "mem_kb": 65536000,
          "pagesize_kb": 2048,
          "siblings": [[0,1],[2,3],[4,5],[6,7]],
          "pinned_cpus": [0,1,2,3],
          "pinned_memory_kb": 8388608
        },
        {
          "id": 1,
          "cpus": [8,9,10,11,12,13,14,15],
          "mem_kb": 65536000,
          "pagesize_kb": 2048,
          "siblings": [[8,9],[10,11],[12,13],[14,15]],
          "pinned_cpus": [],
          "pinned_memory_kb": 0
        }
      ]
    }
    */
    
    -- Instance NUMA mapping
    instance_numa_mapping JSON DEFAULT NULL,
    /*  Example:
    {
      "cells": [
        {
          "id": 0,
          "cpuset": [0,1,2,3],
          "memory_mb": 4096,
          "pagesize_kb": 2048,
          "cpu_pinning": {"0": 0, "1": 1, "2": 2, "3": 3}
        }
      ]
    }
    */
    
    is_topology_compatible TINYINT(1) DEFAULT NULL,
    
    INDEX idx_numa_migration (migration_uuid),
    CONSTRAINT fk_numa_migration 
        FOREIGN KEY (migration_uuid) REFERENCES migrations(uuid)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Migration policies and quotas
CREATE TABLE migration_policies (
    id                      INT AUTO_INCREMENT PRIMARY KEY,
    project_id              VARCHAR(36) DEFAULT NULL,  -- NULL = global policy
    
    -- Concurrency limits
    max_concurrent_outbound INT NOT NULL DEFAULT 3,
    max_concurrent_inbound  INT NOT NULL DEFAULT 3,
    max_concurrent_total    INT NOT NULL DEFAULT 5,
    
    -- Bandwidth limits
    bandwidth_limit_mbps    INT NOT NULL DEFAULT 8000,  -- Per migration
    
    -- Timeout and retry
    timeout_seconds         INT NOT NULL DEFAULT 3600,
    max_retries             INT NOT NULL DEFAULT 3,
    retry_delay_seconds     INT NOT NULL DEFAULT 60,
    
    -- Auto-converge settings
    auto_converge_enabled   TINYINT(1) NOT NULL DEFAULT 1,
    auto_converge_initial_pct INT NOT NULL DEFAULT 20,
    auto_converge_increment_pct INT NOT NULL DEFAULT 10,
    
    -- XBZRLE compression
    xbzrle_enabled          TINYINT(1) NOT NULL DEFAULT 1,
    xbzrle_cache_pct        INT NOT NULL DEFAULT 10,  -- % of VM RAM
    
    -- Post-copy settings
    postcopy_enabled        TINYINT(1) NOT NULL DEFAULT 0,
    postcopy_after_precopy_rounds INT NOT NULL DEFAULT 5,
    
    -- Downtime limits
    max_downtime_ms         INT NOT NULL DEFAULT 500,
    downtime_steps          INT NOT NULL DEFAULT 5,
    downtime_delay_ms       INT NOT NULL DEFAULT 1000,
    
    UNIQUE INDEX idx_policy_project (project_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Entity Relationship Diagram
```
+---------------+       +--------------------+       +-----------------------+
|  instances    |       |   migrations       |       | migration_progress    |
|---------------|       |--------------------|       |-----------------------|
| uuid (PK)    |<------| instance_uuid (FK) |       | id (PK)               |
| host          |       | uuid (PK)          |<------| migration_uuid (FK)   |
| vm_state      |       | source_compute     |       | iteration             |
| task_state    |       | dest_compute       |       | phase                 |
| power_state   |       | migration_type     |       | memory_total_bytes    |
| numa_topology |       | status             |       | dirty_rate_bps        |
+---------------+       | memory_total       |       | actual_downtime_ms    |
                        | disk_total         |       +-----------------------+
                        +--------+-----------+
                                 |
                    +------------+------------+
                    |                         |
          +-----------------+   +------------------------+
          | migration_numa_ |   | migration_cpu_         |
          | topology        |   | compatibility          |
          |-----------------|   |------------------------|
          | migration_uuid  |   | source_host            |
          | host_role       |   | dest_host              |
          | numa_topology   |   | source_cpu_model       |
          | instance_numa_  |   | dest_cpu_model         |
          | mapping         |   | is_compatible          |
          +-----------------+   +------------------------+
```

---

## 5. API Design

### REST API

#### Initiate Live Migration
```
POST /v2.1/servers/{server_id}/action
Content-Type: application/json
X-Auth-Token: {keystone_token}

{
    "os-migrateLive": {
        "host": "dest-compute-042",       // null for auto-select
        "block_migration": "auto",         // "auto" | true | false
        "force": false                     // bypass scheduler checks (admin only)
    }
}

Response: 202 Accepted
Headers:
  Location: /v2.1/servers/{server_id}/migrations/{migration_id}
```

#### List Migrations for Server
```
GET /v2.1/servers/{server_id}/migrations
X-Auth-Token: {keystone_token}

Response: 200 OK
{
    "migrations": [
        {
            "id": 42,
            "uuid": "a1b2c3d4-...",
            "status": "running",
            "source_compute": "compute-017",
            "dest_compute": "compute-042",
            "migration_type": "live-migration",
            "memory_total_bytes": 8589934592,
            "memory_processed_bytes": 6442450944,
            "memory_remaining_bytes": 2147483648,
            "disk_total_bytes": 0,
            "disk_processed_bytes": 0,
            "disk_remaining_bytes": 0,
            "created_at": "2025-03-15T10:30:00Z",
            "updated_at": "2025-03-15T10:30:08Z"
        }
    ]
}
```

#### Get Migration Details
```
GET /v2.1/servers/{server_id}/migrations/{migration_id}
X-Auth-Token: {keystone_token}

Response: 200 OK
{
    "migration": {
        "id": 42,
        "uuid": "a1b2c3d4-...",
        "status": "running",
        "source_compute": "compute-017",
        "dest_compute": "compute-042",
        "migration_type": "live-migration",
        "memory_total_bytes": 8589934592,
        "memory_processed_bytes": 7516192768,
        "memory_remaining_bytes": 1073741824,
        "disk_total_bytes": 0,
        "disk_processed_bytes": 0,
        "disk_remaining_bytes": 0,
        "created_at": "2025-03-15T10:30:00Z",
        "updated_at": "2025-03-15T10:30:09Z"
    }
}
```

#### Cancel/Abort Live Migration
```
DELETE /v2.1/servers/{server_id}/migrations/{migration_id}
X-Auth-Token: {keystone_token}

Response: 202 Accepted
```

#### Force Complete Migration (Admin)
```
POST /v2.1/servers/{server_id}/migrations/{migration_id}/action
Content-Type: application/json
X-Auth-Token: {admin_token}

{
    "force_complete": null
}

Response: 202 Accepted
// Forces the VM to pause and complete migration immediately
// Results in higher downtime but guarantees completion
```

### OpenStack CLI Reference

```bash
# Live migrate with auto-destination
openstack server migrate --live-migration <server>

# Live migrate to specific host
openstack server migrate --live-migration --host <dest-host> <server>

# Live migrate with block migration (no shared storage)
openstack server migrate --live-migration --block-migration <server>

# Check migration status
openstack server migration list <server>
openstack server migration show <server> <migration_id>

# Abort a live migration
openstack server migration abort <server> <migration_id>

# Force complete a live migration (admin)
openstack server migration force complete <server> <migration_id>

# Set migration bandwidth limit via libvirt
# (configured in nova.conf, not via CLI)
# [libvirt]
# live_migration_bandwidth = 0   # 0 = unlimited, value in MiB/s

# Monitor migration progress in real-time
watch -n 1 'openstack server migration show <server> <migration_id> -f json | jq .'

# Verify VM is running on new host after migration
openstack server show <server> -c OS-EXT-SRV-ATTR:host
```

### Internal RPC API (Nova Conductor to Compute)
```python
# nova/compute/rpcapi.py (simplified)
class ComputeAPI(object):
    
    def live_migration(self, ctxt, instance, dest, block_migration,
                       migration, migrate_data):
        """Initiate live migration on source compute.
        
        Args:
            ctxt: RequestContext
            instance: Instance object
            dest: destination hostname
            block_migration: bool
            migration: Migration object
            migrate_data: LiveMigrateData object containing:
                - is_shared_instance_path: bool
                - is_shared_block_storage: bool  
                - is_volume_backed: bool
                - src_supports_native_luks: bool
                - dst_supports_native_luks: bool
                - old_vol_attachment_ids: dict
                - vifs: list of VIF migration data
                - serial_listen_ports: list
        """
        version = '6.0'
        cctxt = self.router.client(ctxt).prepare(
            server=_compute_host(None, instance),
            version=version)
        cctxt.cast(ctxt, 'live_migration',
                   instance=instance, dest=dest,
                   block_migration=block_migration,
                   migration=migration,
                   migrate_data=migrate_data)
    
    def pre_live_migration(self, ctxt, instance, block_migration,
                           disk, migrate_data):
        """Prepare destination for live migration.
        
        Called on destination compute. Sets up:
        - Network (plug VIFs, prepare OVS ports)
        - Storage (connect volumes, prepare local disk)
        - Libvirt XML for incoming domain
        """
        version = '6.0'
        cctxt = self.router.client(ctxt).prepare(
            server=migrate_data.host,
            version=version)
        return cctxt.call(ctxt, 'pre_live_migration',
                          instance=instance,
                          block_migration=block_migration,
                          disk=disk,
                          migrate_data=migrate_data)
    
    def post_live_migration_at_destination(self, ctxt, instance,
                                            block_migration):
        """Finalize migration on destination compute.
        
        Called after VM is running on destination:
        - Update port bindings to destination
        - Activate network on destination
        - Update instance host in DB
        """
        pass  # Implementation omitted for brevity
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Pre-copy Memory Migration Algorithm

The pre-copy algorithm is the default and most widely used live migration technique in KVM/QEMU. It iteratively transfers memory pages from source to destination while the VM continues running.

**Algorithm Steps:**

1. **Bulk Transfer (Iteration 0):** Transfer entire VM memory to destination
2. **Dirty Tracking:** Enable KVM dirty page logging (`KVM_MEM_LOG_DIRTY_PAGES`)
3. **Iterative Transfer:** In each subsequent round, transfer only pages dirtied since last round
4. **Convergence Check:** If `dirty_rate < transfer_rate * threshold`, proceed to pause
5. **Stop-and-Copy:** Pause VM, transfer remaining dirty pages + device state + CPU registers
6. **Resume:** Resume VM on destination

**Mathematical Model:**

```
Let:
  M = total VM memory (bytes)
  B = migration bandwidth (bytes/sec)
  D = dirty page rate (bytes/sec)
  r = D/B (dirty ratio, must be < 1 for convergence)

Iteration 0: Transfer M bytes, time = M/B
              Pages dirtied during transfer = D * (M/B) = M*r

Iteration 1: Transfer M*r bytes, time = M*r/B
              Pages dirtied = D * (M*r/B) = M*r^2

Iteration n: Transfer M*r^n bytes, time = M*r^n/B
              Pages dirtied = M*r^(n+1)

Total transfer time = M/B * (1 + r + r^2 + ... + r^n)
                    = M/B * (1 - r^(n+1)) / (1 - r)

As n -> infinity: Total time = M / (B - D)  [geometric series, r < 1]

Final dirty pages at iteration n = M * r^(n+1)
Downtime = M * r^(n+1) / B + device_state_transfer + network_cutover

Example: M=8GB, B=1GB/s, D=200MB/s -> r=0.2
  Iteration 5: dirty pages = 8GB * 0.2^6 = 0.5 MB
  Downtime = 0.5MB / 1GB/s + 2ms + 5ms = 7.5ms (within SLA)
```

**QEMU/KVM Implementation Details:**

```
+-----------------------------------------------------------+
|                    QEMU Migration Thread                   |
|                                                           |
|  migration_thread()                                       |
|  +-- qemu_savevm_state_header()     // Magic + version   |
|  +-- qemu_savevm_state_setup()      // Per-device setup  |
|  |   +-- ram_save_setup()                                 |
|  |   |   +-- Enable dirty tracking                       |
|  |   |   +-- Create dirty bitmap (1 bit per page)        |
|  |   |   +-- Initialize XBZRLE cache                     |
|  |   +-- block_save_setup()         // If block migrate  |
|  |   +-- vfio_save_setup()          // If SR-IOV         |
|  +-- migration_iteration_run()      // Main loop         |
|  |   +-- ram_save_iterate()                               |
|  |   |   +-- Scan dirty bitmap                           |
|  |   |   +-- For each dirty page:                        |
|  |   |   |   +-- Check XBZRLE cache -> compress if hit   |
|  |   |   |   +-- Send page to dest via QEMUFile          |
|  |   |   |   +-- Clear dirty bit                         |
|  |   |   +-- Report progress via migration_state         |
|  |   +-- Check convergence:                               |
|  |   |   +-- remaining < threshold? -> complete           |
|  |   |   +-- iterations > max? -> auto-converge          |
|  |   |   +-- bandwidth_estimate vs dirty_rate            |
|  |   +-- Apply auto-converge throttle if enabled         |
|  +-- migration_completion()                               |
|  |   +-- vm_stop()                  // Pause guest        |
|  |   +-- qemu_savevm_state_complete_precopy()            |
|  |   |   +-- ram_save_complete()    // Final dirty pgs   |
|  |   |   +-- cpu_save()             // CPU registers     |
|  |   |   +-- device_state_save()    // All device state  |
|  |   |   +-- memory_global_dirty_log_stop()              |
|  |   +-- Mark migration complete                         |
|  +-- Return status to libvirt                             |
+-----------------------------------------------------------+
```

**KVM Dirty Page Tracking:**

```
Hardware-assisted dirty tracking (Intel EPT / AMD NPT):

+----------------+     +------------------------+
|  Guest vCPU    |     |  Extended Page Table    |
|                |     |  (EPT/NPT)             |
|  MOV [addr],   |--->|  PTE.Dirty bit = 1     |
|  value         |     |  (hardware sets this)  |
+----------------+     +-----------+------------+
                                   |
                     KVM_GET_DIRTY_LOG ioctl
                                   |
                                   v
                     +------------------------+
                     |  Dirty Bitmap          |
                     |  (userspace, QEMU)     |
                     |  1 bit = 1 page        |
                     |  4 KB page -> 1 bit    |
                     |  8 GB RAM -> 256 KB    |
                     |  bitmap                |
                     +------------------------+

KVM_CLEAR_DIRTY_LOG: Reset dirty bits atomically (per-slot)
  - Uses write-protect (clear PTE writable bit)
  - Page fault on next write -> re-mark dirty
  - Newer: KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2 for fine-grained control
```

#### Q&A: Pre-copy Memory Migration

**Q1: Why does pre-copy fail for write-intensive workloads?**
**A:** When dirty_rate > migration_bandwidth (r > 1), the geometric series diverges. Each iteration produces more dirty pages than it transfers. For a database VM dirtying 2 GB/s with 1 GB/s bandwidth, iteration 0 takes 8s (for 8 GB), creating 16 GB of dirty pages -- more than the original. Without intervention (auto-converge or XBZRLE), the migration runs forever until timeout. The solution is either throttling CPU to reduce dirty rate, compressing pages, or switching to post-copy.

**Q2: How does auto-converge work at the hypervisor level?**
**A:** Auto-converge throttles guest vCPUs by inserting sleep calls in KVM's vCPU scheduling. QEMU's migration thread monitors convergence and signals the throttle subsystem. It starts at `auto_converge_initial` (default 20%) and increments by `auto_converge_increment` (default 10%) each round until dirty rate drops below bandwidth. At 80% throttle, a VM running at 2 GHz effectively runs at 400 MHz. This is a last resort because it severely impacts guest performance during migration. The throttle is released immediately when migration completes.

**Q3: What is the purpose of the XBZRLE compression cache?**
**A:** XBZRLE (Xor Based Zero Run Length Encoding) caches the last sent version of each page. When a dirty page needs retransmission, QEMU XORs the current page with the cached version. If only a few bytes changed, the XOR result is mostly zeros, which compresses extremely well with run-length encoding. Typical compression ratios are 3:1 to 10:1 for workloads that modify small portions of pages (like linked list pointer updates). The cache is sized at a percentage of VM RAM (default 10%, e.g., 800 MB for an 8 GB VM). Cache misses fall back to sending the full page.

**Q4: How does KVM's dirty page tracking interact with huge pages?**
**A:** With 2 MB huge pages, each dirty bit in the bitmap represents 2 MB instead of 4 KB. This means a single byte write dirties an entire 2 MB page, dramatically increasing the effective dirty rate. For a 256 GB VM with huge pages, a random write pattern dirtying 1000 pages/sec means 1000 * 2 MB = 2 GB/s effective dirty rate (vs 1000 * 4 KB = 4 MB/s with regular pages). During migration, QEMU can split huge pages to 4 KB for finer-grained tracking using `KVM_CAP_DIRTY_LOG_PERF_CONTROL`, but this trades TLB performance for migration efficiency.

**Q5: When should an operator switch from pre-copy to post-copy mid-migration?**
**A:** QEMU supports "pre-copy then post-copy" mode where it starts with pre-copy to transfer the bulk of memory, then switches to post-copy if convergence is not reached after N iterations. The switch point should be when: (1) more than 80% of memory has been transferred at least once, (2) the dirty rate shows no sign of decreasing, and (3) the workload can tolerate the latency penalty of demand-paging (typically 0.5-2ms per page fault). Database workloads with random access patterns are poor candidates for post-copy; streaming/sequential workloads are good candidates.

**Q6: What happens to in-flight network packets during the pre-copy pause phase?**
**A:** During the pause phase (typically 5-50ms), the VM cannot process network I/O. Incoming packets are buffered in the host's OVS/OVN bridge queues (typical buffer: 1000 packets per queue). For a 10 Gbps interface at 50ms pause: 10 Gbps * 0.05s = 62.5 MB of data. With typical MTU 1500 bytes, that's ~41,000 packets -- within buffer capacity. TCP connections remain alive because TCP retransmission timeout (200ms-1s) far exceeds the pause duration. UDP packets may be dropped if buffer overflows, which is why the downtime target of < 100ms is critical for real-time applications.

---

### Deep Dive 2: Post-copy Migration with Userfaultfd

Post-copy migration inverts the pre-copy approach: instead of trying to transfer all memory before moving the VM, it moves the VM first and fetches pages on demand.

**Architecture:**

```
Phase 1: Pause and Transfer State (fast)
+----------------+                    +----------------+
| Source Host    |                    | Dest Host      |
|                |                    |                |
| VM PAUSED      | ---- CPU state -->| VM RESUMED     |
|                | ---- Device st -->| (on dest)      |
|                | ---- Dirty bm  -->|                |
| Memory stays   |                    | Memory empty   |
| here (source   |                    | (demand-page)  |
| of truth)      |                    |                |
+----------------+                    +--------+-------+
                                              |
Phase 2: Demand Paging (long-running)         |
                                              |
   userfaultfd page fault <-------------------+
        |                    
        v                    
+--------------------------------------------------+
|  Post-copy Page Fetch Flow:                      |
|                                                  |
|  1. Guest accesses unmapped page                 |
|  2. #PF -> KVM exit -> userfaultfd notification  |
|  3. QEMU migration thread receives fault         |
|  4. Request page from source via TCP/RDMA        |
|  5. Source reads page, sends to dest             |
|  6. Dest installs page via UFFDIO_COPY           |
|  7. Guest vCPU resumes execution                 |
|                                                  |
|  Latency: 0.3-2ms per fault (network dependent) |
|  Background: Source also pushes pages proactively|
+--------------------------------------------------+
```

**Userfaultfd Mechanism:**

```c
// Linux userfaultfd system call:

int uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);

// Register memory range for fault notifications
struct uffdio_register reg = {
    .range = { .start = guest_ram_addr, .len = guest_ram_size },
    .mode = UFFDIO_REGISTER_MODE_MISSING  // Notify on missing pages
};
ioctl(uffd, UFFDIO_REGISTER, &reg);

// Poll for page faults
struct pollfd pfd = { .fd = uffd, .events = POLLIN };
poll(&pfd, 1, -1);

// Read fault details
struct uffd_msg msg;
read(uffd, &msg, sizeof(msg));
// msg.arg.pagefault.address = faulting address

// Resolve fault by copying page data
struct uffdio_copy copy = {
    .dst = msg.arg.pagefault.address & ~(page_size - 1),
    .src = page_data_from_source,
    .len = page_size,
    .mode = 0
};
ioctl(uffd, UFFDIO_COPY, &copy);
// Guest vCPU automatically resumes
```

**Post-copy + Pre-copy Hybrid:**

```
Timeline:
  |-- Pre-copy phase ---------|-- Switch --|-- Post-copy phase ------------|
  
  Pre-copy:
  - Transfer ~80% of pages (bulk transfer, no convergence needed)
  - Build "sent pages" bitmap on destination
  
  Switch to post-copy:
  - Pause VM briefly (~50-100ms)
  - Transfer CPU + device state
  - Resume on destination
  
  Post-copy:
  - Remaining ~20% pages fetched on demand
  - Source continues pushing unsent pages in background
  - Page faults only for the 20% not yet transferred
  
  Advantage: Most of working set already on dest -> fewer faults
  Compared to pure post-copy: ~80% fewer page faults
  Compared to pure pre-copy: Guaranteed to complete (no convergence issue)
```

**Risk: Source Failure During Post-copy:**

```
CRITICAL FAILURE SCENARIO:

  Source dies while pages are split across hosts:
  
  +----------------+                    +----------------+
  | Source Host    |                    | Dest Host      |
  |   X DEAD      |                    |                |
  |                |                    | VM running     |
  | Pages: A,C,E  | <-- LOST          | Pages: B,D,F   |
  | (not yet sent |                    | (received)     |
  |  to dest)     |                    |                |
  +----------------+                    +----------------+
  
  Result: VM crashes with SIGBUS (unrecoverable page fault)
  
  Mitigations:
  1. Network RAID: Mirror pages to 2 destinations (expensive)
  2. Persistent postcopy: Write pages to shared storage as backup
  3. Accept the risk: Post-copy is inherently less safe than pre-copy
  4. Use pre-copy + post-copy hybrid to minimize the risk window
     (send 80% via pre-copy first, only 20% at risk)
```

#### Q&A: Post-copy Migration

**Q1: What is the performance impact of userfaultfd page faults on the running VM?**
**A:** Each page fault stalls the faulting vCPU for the entire round-trip time: the QEMU thread must request the page from the source over the network, receive the 4 KB page, and install it via `UFFDIO_COPY`. With a 25 Gbps RDMA network and 10 microsecond RTT, the minimum latency is ~50 microseconds per page. Over a standard 10 Gbps TCP connection with ~100 microsecond RTT, it's 300-500 microseconds per page. For a database VM with random access across 256 GB, the initial minutes after migration can see 10,000+ faults/second, reducing throughput by 30-50% until the working set is populated. Other vCPUs continue executing unless they also fault.

**Q2: How does background page pushing work alongside demand paging?**
**A:** QEMU runs two parallel threads on the source during post-copy: (1) a demand-paging server that responds to page requests from the destination (high priority), and (2) a background pusher that proactively sends pages sequentially or by predicted access pattern (low priority). The background pusher uses a bitmap to track sent pages and skips already-sent ones. On the destination, incoming background pages are installed via `UFFDIO_COPY` if the page hasn't been demand-faulted already. This reduces future page faults. With 1 GB/s background push rate, a 256 GB VM completes background transfer in ~4 minutes.

**Q3: Can post-copy migration be canceled or rolled back?**
**A:** No. Once the VM resumes on the destination, post-copy migration cannot be rolled back because the VM's state is now split between two hosts. The VM has been executing new instructions on the destination, modifying pages locally. Rolling back would mean losing those modifications. The only options are: (1) complete the migration (let all pages transfer), or (2) if the source dies, accept VM crash. This is the fundamental trade-off: post-copy guarantees completion (no convergence issue) but loses rollback safety. Pre-copy is always safe to cancel because the source VM is authoritative until the final switchover.

**Q4: How does post-copy interact with NUMA-aware memory placement on the destination?**
**A:** This is a significant challenge. With pre-copy, all pages can be placed in the correct NUMA node during transfer because the destination knows the full topology upfront. With post-copy, demand-faulted pages are initially placed on whatever NUMA node the faulting vCPU's memory policy specifies. However, if the vCPU-to-NUMA mapping on the destination differs from the source, pages may land on the wrong NUMA node. The solution is to configure `UFFDIO_COPY` with the destination's NUMA policy via `mbind()` before installing pages, or run a post-migration NUMA rebalancing pass using `move_pages()` syscall. The rebalancing itself adds overhead (typically 2-5 GB/s migration between NUMA nodes).

**Q5: What kernel version requirements exist for post-copy migration?**
**A:** userfaultfd was introduced in Linux 4.3, but practical post-copy migration requires Linux 4.11+ which added `UFFDIO_COPY` for anonymous pages and `UFFDIO_ZEROPAGE` for zero pages. For huge page support (2 MB pages), Linux 4.14+ is required. The `UFFD_FEATURE_MISSING_SHMEM` flag for shared memory (needed for some QEMU memory backends) requires Linux 4.11+. Most production deployments use Linux 5.4+ LTS or later. QEMU 2.9+ supports post-copy with userfaultfd; QEMU 4.0+ is recommended for stability and the hybrid pre+post-copy mode.

**Q6: How do you monitor post-copy migration health and detect stalls?**
**A:** Key metrics to monitor: (1) page fault rate on destination -- should decrease over time as working set populates; if it plateaus at a high rate, the VM has a very large working set. (2) Source page-serve latency -- if source is overloaded serving pages, faults queue up. (3) Network utilization between source and destination -- saturation means faults are bandwidth-limited. (4) Background push progress -- percentage of total pages sent. (5) vCPU halt time -- measures how much time vCPUs spend waiting for pages. In Nova, `migration_progress` records are updated every 60 seconds. Libvirt's `virDomainGetJobStats` returns `memory_postcopy_requests` (pending page fault count) -- if this exceeds a threshold (e.g., 1000), alert on degraded performance.

---

### Deep Dive 3: Network Cutover During Live Migration

The network cutover is the most latency-sensitive part of live migration. It must redirect all network traffic from the source host to the destination host with minimal packet loss.

**OVN-based Network Cutover Sequence:**

```
Step-by-step with timing:

T=0ms: VM paused on source (pre-copy complete)
  |
  +-- T=0-2ms: Transfer remaining dirty pages + CPU state
  |
  v
T=2ms: Nova-compute calls Neutron to update port binding
  |
  |  PUT /v2.0/ports/{port_id}
  |  {"port": {"binding:host_id": "dest-host"}}
  |
  +-- T=2-4ms: Neutron ML2 plugin processes binding update
  |   +-- OVN mechanism driver updates OVN Northbound DB
  |   |   Logical_Switch_Port.options:requested-chassis = "dest-host"
  |   |
  |   +-- ovn-northd translates to Southbound DB:
  |   |   Port_Binding.chassis = <dest-chassis-uuid>
  |   |
  |   +-- Southbound DB OVSDB monitors fire
  |
  +-- T=4-6ms: ovn-controller on DESTINATION host
  |   +-- Detects new Port_Binding for its chassis
  |   +-- Installs OpenFlow rules in local OVS:
  |   |   - Match on VM's MAC/IP -> output to VM's tap port
  |   |   - Update ARP responder for VM's IP
  |   |   - Install conntrack rules for security groups
  |   +-- Sends GARP (Gratuitous ARP):
  |   |   arp who-has <VM_IP> is-at <VM_MAC>
  |   |   (Broadcast to all ports in the network)
  |   +-- Sends RARP for L2 switch MAC learning
  |
  +-- T=4-6ms: ovn-controller on SOURCE host (parallel)
  |   +-- Detects Port_Binding removed from its chassis
  |   +-- Removes OpenFlow rules for VM port
  |   +-- Flushes conntrack entries for VM's flows
  |
  +-- T=6-8ms: Physical network convergence
  |   +-- Top-of-rack (ToR) switches:
  |   |   - Receive GARP -> update MAC table
  |   |   - VXLAN/Geneve tunnel endpoint updates
  |   +-- If BGP EVPN (L2VPN):
  |   |   - BGP UPDATE with new MAC mobility ext community
  |   |   - All spine/leaf switches update forwarding
  |   +-- If VXLAN with flood-and-learn:
  |       - GARP flooded to all VTEPs
  |       - Remote VTEPs update FDB entries
  |
  v
T=8-10ms: Network fully converged
  |
  +-- T=10ms: Resume VM on destination
  |
  +-- Total network cutover: ~8-10ms (within 100ms SLA)
```

**OVS Flow Update Detail:**

```
Source Host OVS Flows (BEFORE migration):
  table=0: in_port=tap-vm1 -> goto table=10 (ingress ACL)
  table=10: ip,nw_src=10.0.1.5 -> goto table=20 (L2 lookup)
  table=20: dl_dst=fa:16:3e:xx:yy:zz -> output:patch-to-br-int
  table=65: ct_state=+est -> goto table=70 (security group allow)

Source Host OVS Flows (AFTER migration):
  [All flows for tap-vm1 DELETED]
  
Destination Host OVS Flows (AFTER migration):
  table=0: in_port=tap-vm1 -> goto table=10 (NEW)
  table=10: ip,nw_src=10.0.1.5 -> goto table=20 (NEW)  
  table=20: dl_dst=fa:16:3e:xx:yy:zz -> output:patch-to-br-int (NEW)
  table=65: ct_state=+est -> goto table=70 (NEW, security group)
  
  // GARP generation:
  table=70: arp,arp_op=1,arp_spa=10.0.1.5 -> output:IN_PORT,FLOOD
```

**Handling Stateful Connections (conntrack):**

```
Problem: VM has active TCP connections with conntrack state on source host.
         After migration, destination host has no conntrack state.

Solution options:

1. OVN Stateful Migration (Default):
   - Security group rules use conntrack (ct) in OVS
   - Source host: ct entries for VM's flows (e.g., TCP ESTABLISHED)
   - After migration: first packet from VM hits ct_state=+new on dest
   - OVN allows this as a "continuation" via zone-based conntrack
   - Return traffic: remote hosts still have ct state, so replies work
   - Slight window (~100ms) where asymmetric routing can cause drops
   
2. Conntrack Transfer (advanced, not in upstream OVN):
   - Export conntrack entries: conntrack -L -s <VM_IP>
   - Import on destination: conntrack -I ...
   - Requires kernel conntrack-tools cooperation
   - Complex, rarely implemented in practice

3. Application-level resilience:
   - TCP retransmits handle short interruptions (< 200ms is invisible)
   - UDP applications must tolerate brief drops
   - Most real-world applications survive 10-50ms migration cutover
```

**Floating IP Migration:**

```
Scenario: VM has floating IP 203.0.113.50 -> fixed IP 10.0.1.5

Before migration:
  External router (Neutron L3 agent / OVN gateway):
    - NAT rule: 203.0.113.50 -> 10.0.1.5 (DNAT)
    - NAT rule: 10.0.1.5 -> 203.0.113.50 (SNAT)
  
  With DVR (Distributed Virtual Router):
    - NAT happens on SOURCE compute host
    - After migration, NAT must happen on DEST compute host
    
  DVR floating IP cutover:
    1. Destination host: Install NAT rules locally
       iptables -t nat -A PREROUTING -d 203.0.113.50 -j DNAT --to 10.0.1.5
       (Or OVN equivalent: NAT logical flow on destination chassis)
    2. Source host: Remove NAT rules
    3. Send GARP for 203.0.113.50 from dest external bridge
    4. Physical router updates ARP cache for floating IP
    
  With centralized routing (non-DVR):
    - NAT happens on network node (unchanged by migration)
    - Only internal port binding changes (simpler)
    - But adds extra hop through network node (latency)
```

#### Q&A: Network Cutover

**Q1: What happens to in-flight packets that were routed to the source host during cutover?**
**A:** During the 5-10ms cutover window, packets in transit to the source host's OVS can arrive after the VM port has been removed. These packets are dropped by OVS (no matching flow, default drop). For TCP, this triggers retransmission at the sender after RTO (typically 200ms minimum). The retransmitted packet arrives at the destination host via the updated forwarding path. For a 10ms cutover, the packet loss window at 10 Gbps is ~12.5 MB, or about 8,300 packets. In practice, most of these are buffered in the ToR switch or host NIC ring buffers and delivered successfully once the GARP propagates. UDP streams (like VoIP) may experience a brief glitch audible as a ~10ms gap.

**Q2: How does GARP propagation work in a VXLAN overlay network?**
**A:** In a VXLAN network, the GARP from the destination host is encapsulated in a VXLAN header and sent to all remote VTEPs (VXLAN Tunnel Endpoints) in the same VNI (Virtual Network Identifier). With flood-and-learn, the GARP is sent to the multicast group for that VNI -- all compute hosts in the network receive it and update their local FDB (Forwarding Database) to map the VM's MAC to the new VTEP IP. With OVN/BGP EVPN, the MAC mobility is signaled via BGP UPDATE messages with a MAC Mobility extended community (sequence number incremented). This is faster than flood-and-learn because only the routing control plane is involved, not data plane flooding. Convergence time: flood-and-learn ~10-50ms, BGP EVPN ~100-500ms (but pre-programmed via OVN Southbound DB update).

**Q3: How do you handle migration when the VM uses SR-IOV (direct hardware NIC access)?**
**A:** SR-IOV bypasses OVS entirely -- the VM has a VF (Virtual Function) directly mapped to its address space via VFIO. During migration, the SR-IOV VF must be hot-unplugged from the VM on the source and a new VF hot-plugged on the destination. This is called "bond-based SR-IOV migration": Nova temporarily adds a virtio NIC, bonds it with the SR-IOV VF, removes the SR-IOV VF (traffic failovers to virtio), migrates via virtio, hot-plugs SR-IOV VF on destination, bonds it, and removes virtio. Total downtime is higher (~1-5 seconds) due to VF plug/unplug operations. Alternatively, some NICs support "switchdev mode" where SR-IOV offload rules are managed through OVS, allowing transparent migration with slightly lower performance than full hardware bypass.

**Q4: What is the impact on east-west traffic between VMs on the same host when one migrates?**
**A:** If VM-A and VM-B are on the same host communicating via OVS br-int (local switching, no network hairpin), and VM-A migrates to another host, the traffic path changes from local to remote. Before: VM-A tap -> br-int -> VM-B tap (zero network hops, microsecond latency). After: VM-A tap -> dest br-int -> VXLAN tunnel -> source br-int -> VM-B tap (one network hop, ~100-200 microsecond latency). The 100x latency increase can impact latency-sensitive applications. Nova's scheduler can use the `ServerGroupAffinityFilter` to keep co-located VMs together, and the live migration task can check for affinity group violations before proceeding.

**Q5: How does network cutover work with multiple NICs / multi-homed VMs?**
**A:** A VM with multiple Neutron ports (e.g., management + data + storage networks) requires all ports to be migrated atomically. Nova's `LiveMigrationTask` iterates through all VIFs and calls `pre_live_migration` for each port on the destination. During cutover, Neutron updates all port bindings in a single transaction to avoid a split-brain where some ports are on the source and others on the destination. If any port binding update fails, the entire migration is rolled back. The order matters: storage network ports should be migrated last (if using iSCSI/NFS) to ensure I/O continues during memory transfer. With OVN, all port bindings in the same transaction are processed atomically by ovn-northd.

**Q6: How does the migration system handle MTU mismatches between source and destination networks?**
**A:** MTU mismatches can cause silent packet drops during migration. If the source host is on a network with MTU 9000 (jumbo frames) but the migration path traverses a segment with MTU 1500, large memory transfer packets will be fragmented or dropped. Nova's pre-migration checks should verify path MTU. In practice, migration traffic uses a dedicated migration network (configured via `live_migration_inbound_addr` in nova.conf) with guaranteed jumbo frame support. For the data plane after migration, Neutron stores the network MTU in the `networks.mtu` column and the destination OVS configures the same MTU on the tap interface. If MTU differs between source and destination provider networks (misconfiguration), Neutron rejects the migration at the pre-check stage.

---

### Deep Dive 4: CPU Compatibility and NUMA-Aware Migration

CPU compatibility is a hard constraint for live migration: if the destination host lacks CPU features that the guest is using, the VM will crash with illegal instruction exceptions.

**CPU Model Hierarchy:**

```
CPU model compatibility chain (Intel):

  Oldest <--------------------------------------------------> Newest
  
  Penryn -> Westmere -> SandyBridge -> IvyBridge -> Haswell
  -> Broadwell -> Skylake-Server -> Cascadelake-Server 
  -> Cooperlake -> Icelake-Server -> SapphireRapids

  Rule: Can migrate from OLDER -> NEWER (destination has superset)
        CANNOT migrate from NEWER -> OLDER (destination lacks features)
        
  Example:
    Cascadelake -> Icelake: OK (Icelake has all Cascadelake features + more)
    Icelake -> Cascadelake: FAIL (Cascadelake lacks AVX-512 VNNI from Icelake)

QEMU CPU model modes:
  1. host-passthrough: Guest sees exact host CPU
     - Best performance (all features exposed)
     - Migration ONLY to identical CPU model
     - Used for: HPC, latency-sensitive workloads
     
  2. host-model: Guest sees host CPU with known QEMU model name
     - QEMU maps host CPU to closest known model
     - Migration to same or newer generation
     - Small performance loss (some features may be hidden)
     
  3. custom: Guest sees specific CPU model (e.g., "Cascadelake-Server-v4")
     - Explicit feature set, fully portable
     - Migration to any host supporting that model
     - May hide useful features (e.g., no AVX-512 if model lacks it)
     - Recommended for cloud environments

  4. maximum: Guest sees all features QEMU can emulate
     - Includes features from multiple CPU generations
     - Migration only to same QEMU version + same host CPU
     - Not recommended for migration
```

**Nova CPU Compatibility Check:**

```
Pre-migration CPU compatibility verification:

Nova Conductor (LiveMigrationTask):
  |
  +-- 1. Get source instance CPU model:
  |   |   instance.system_metadata['image_hw_cpu_model'] 
  |   |   or nova.conf [libvirt] cpu_model = "Cascadelake-Server-v4"
  |   |
  |   +-- Libvirt XML on source:
  |       <cpu mode='custom' match='exact'>
  |         <model fallback='forbid'>Cascadelake-Server-v4</model>
  |         <feature policy='require' name='avx512f'/>
  |         <feature policy='require' name='avx512bw'/>
  |         <feature policy='disable' name='pdpe1gb'/>
  |       </cpu>
  |
  +-- 2. Query destination compute capabilities:
  |   |   Nova Placement API: GET /resource_providers/{dest_rp}/traits
  |   |   Required traits: CUSTOM_CPU_CASCADELAKE, HW_CPU_X86_AVX512F, etc.
  |   |
  |   +-- Libvirt on destination:
  |       virConnectCompareCPU(dest_conn, source_cpu_xml)
  |       Returns: VIR_CPU_COMPARE_SUPERSET    -> compatible
  |                VIR_CPU_COMPARE_IDENTICAL    -> compatible
  |                VIR_CPU_COMPARE_INCOMPATIBLE -> reject
  |
  +-- 3. Check specific required features:
  |   |   Source features that MUST be present on destination:
  |   |   - AVX-512F, AVX-512BW (if VM is using vector instructions)
  |   |   - AES-NI (if VM does encryption)
  |   |   - TSX (Transactional Synchronous Extensions, if enabled)
  |   |   - SGX (if VM uses enclaves)
  |   |
  |   +-- Missing feature -> migration blocked with clear error:
  |       "Cannot migrate: dest lacks required CPU feature 'avx512bw'"
  |
  +-- 4. If compatible: proceed with migration
      If incompatible: raise MigrationPreCheckError
```

**NUMA Topology Migration:**

```
NUMA-aware migration requirements:

Source Host NUMA Layout:
  +---------------------------------------------+
  |  NUMA Node 0           NUMA Node 1          |
  |  CPUs: 0-15            CPUs: 16-31          |
  |  RAM: 128 GB           RAM: 128 GB          |
  |                                             |
  |  VM-A pinned here:     (other VMs)          |
  |  vCPU 0 -> pCPU 4                          |
  |  vCPU 1 -> pCPU 5                          |
  |  vCPU 2 -> pCPU 6                          |
  |  vCPU 3 -> pCPU 7                          |
  |  Memory: 16 GB on      0 GB on             |
  |          Node 0         Node 1              |
  +---------------------------------------------+

Destination Host MUST provide:
  +---------------------------------------------+
  |  NUMA Node 0           NUMA Node 1          |
  |  CPUs: 0-15            CPUs: 16-31          |
  |  RAM: 128 GB           RAM: 128 GB          |
  |                                             |
  |  Need 4 free pCPUs     (or here, as long    |
  |  on SAME NUMA node     as all on one node)  |
  |  + 16 GB free RAM                           |
  |  on SAME NUMA node                          |
  +---------------------------------------------+

Nova Placement API resource query:
  GET /allocation_candidates?
    resources=VCPU:4,MEMORY_MB:16384,DISK_GB:50&
    required=CUSTOM_NUMA_TOPOLOGY&
    member_of=<dest_aggregate_uuid>
    
  The scheduler's NUMATopologyFilter checks:
  1. Destination has a NUMA node with >= 4 free pCPUs
  2. Same NUMA node has >= 16 GB free RAM  
  3. If huge pages required: NUMA node has >= 8192 free 2MB pages
  4. If CPU pinning: specific pCPUs available (not overcommitted)
  5. If emulator thread pinning: extra pCPU available

NUMA topology mismatch handling:
  Source: 2-socket, 16 cores/socket (NUMA nodes 0,1)
  Dest:   4-socket, 8 cores/socket  (NUMA nodes 0,1,2,3)
  
  VM needs: 4 vCPUs on 1 NUMA node
  Source mapping: NUMA 0 (16 cores available)
  Dest mapping: NUMA 2 (8 cores available) -> valid, 4 < 8
  
  The NUMA node IDs don't need to match, only the topology
  constraints (cores per node, memory per node) must be satisfiable.
```

**Huge Page Migration Considerations:**

```
VM configured with 2 MB huge pages:
  <memoryBacking>
    <hugepages>
      <page size='2048' unit='KiB'/>
    </hugepages>
  </memoryBacking>

Pre-migration check:
  Source: VM using 8192 x 2MB pages = 16 GB (NUMA node 0)
  Destination: Must have 8192 free 2MB huge pages on ONE NUMA node
  
  Check: cat /sys/devices/system/node/node0/hugepages/hugepages-2048kB/free_hugepages
  
  If destination has only 4096 free 2MB pages on each NUMA node:
  -> Migration FAILS (can't split across NUMA nodes for this VM)
  
  If destination has 8192 free 2MB pages on node 1:
  -> Migration succeeds, VM remapped to NUMA node 1

1 GB huge pages:
  Even stricter: 16 GB VM = 16 x 1GB pages
  Must have 16 contiguous 1GB pages on destination
  1GB pages can only be reserved at boot time (boot parameter hugepagesz=1G hugepages=16)
  Very limited migration flexibility
```

#### Q&A: CPU Compatibility and NUMA

**Q1: How do you handle a heterogeneous cluster with mixed CPU generations?**
**A:** The recommended approach is to define a "baseline CPU model" that represents the lowest common denominator across all hosts in a migration domain (availability zone or host aggregate). For example, if you have Cascadelake and Icelake hosts, set `cpu_model = Cascadelake-Server-v4` in nova.conf for all hosts. This ensures all VMs can migrate freely between any hosts. The performance cost is that Icelake-specific features (like AVX-512 VNNI) are hidden from guests. For workloads that need those features, create a separate host aggregate with `cpu_model = Icelake-Server-v2` and accept that those VMs can only migrate within the Icelake aggregate. Nova's Placement API traits (`CUSTOM_CPU_ICELAKE`) enforce this partitioning.

**Q2: What happens if a VM is using a CPU feature that was exposed but shouldn't have been?**
**A:** This is called "CPU feature leakage" and happens with `host-passthrough` mode. If a VM on a Cascadelake host is using AVX-512 VNNI instructions (exposed via passthrough) and needs to migrate to a Skylake host (no VNNI), the migration will fail at the libvirt `virConnectCompareCPU` check. The VM is now "pinned" to Cascadelake-or-newer hosts. To fix this proactively: (1) never use `host-passthrough` in production clouds (use `host-model` or `custom`), (2) if already deployed, use `virsh cpu-baseline` to compute the common CPU model and hot-remove features via `virsh update-device` (risky, may crash VM if feature is in use), (3) accept reduced migration flexibility and use Placement traits to constrain scheduling.

**Q3: How does NUMA pinning interact with overcommit ratios?**
**A:** NUMA-pinned VMs (with `hw:cpu_policy=dedicated`) are NOT subject to CPU overcommit. Each pinned vCPU gets exclusive access to a physical CPU -- overcommit ratio is effectively 1:1 for these VMs. Non-pinned VMs (`hw:cpu_policy=shared`) can be overcommitted according to `cpu_allocation_ratio` (e.g., 16:1). During migration, a pinned VM requires the exact number of free, unpinned pCPUs on the destination's NUMA node. The Placement API tracks this via `PCPU` (dedicated) vs `VCPU` (shared) resource classes. A host with 32 pCPUs might allocate 16 as PCPU (for dedicated VMs) and 16 as VCPU (for shared VMs, overcommitted to 256 VCPUs). Migration of a pinned VM reduces the destination's PCPU inventory.

**Q4: What is emulator thread pinning and why does it matter for migration?**
**A:** QEMU runs emulator threads (I/O threads, main loop, migration thread) in addition to vCPU threads. By default, emulator threads float across all host CPUs, causing cache pollution and jitter for latency-sensitive workloads. With emulator thread pinning (`hw:emulator_threads_policy=share` or `isolate`), these threads are pinned to dedicated pCPUs separate from vCPU pins. During migration, the destination must have an additional pCPU available for the emulator thread on the same NUMA node. With `isolate` policy, each VM needs one extra pCPU. For a 4-vCPU VM with isolated emulator thread: 5 pCPUs required on the destination NUMA node. This is an easy pre-check to miss, causing migration failures in heavily packed hosts.

**Q5: How do you handle CPU microcode version differences during migration?**
**A:** CPU microcode patches can add or remove CPU features (e.g., Intel disabled TSX via microcode update for security). If source host has microcode version X (TSX enabled) and destination has version Y (TSX disabled), a VM using TSX will crash after migration. Nova and libvirt don't natively track microcode versions. The operational solution is: (1) ensure all hosts in a migration domain have the same microcode version (use Ansible/Puppet to enforce), (2) after microcode updates, run `virsh capabilities` on each host and update Placement traits, (3) use host aggregates to separate hosts with different microcode (rare, but needed during rolling microcode updates). Some operators add a `CUSTOM_MICROCODE_<version>` trait to the Placement API for tracking.

**Q6: Can you migrate a VM between AMD and Intel hosts?**
**A:** Generally no. AMD and Intel CPUs have fundamentally different instruction set extensions and CPU model namespaces. Even if the basic x86-64 instruction set is the same, features like AVX implementation, memory ordering guarantees, and virtualization extensions (Intel VT-x vs AMD-V) differ at the microarchitecture level. QEMU's CPU model definitions are vendor-specific: "Cascadelake-Server" is Intel-only; "EPYC-Rome" is AMD-only. `virConnectCompareCPU` will return INCOMPATIBLE for cross-vendor comparisons. The only exception is if you use a generic CPU model like `qemu64` that exposes only the common x86-64 baseline -- but this hides so many features (no SSE4, no AVX, no AES-NI) that it's impractical for production workloads. The solution is to maintain separate host aggregates per CPU vendor.

---

## 7. Scheduling & Resource Management

### Migration Scheduling

```
Migration scheduling is two-phase:

Phase 1: Destination Selection (if not specified)
  Nova Conductor -> Nova Scheduler:
    select_destinations(request_spec, instance)
    
  Scheduler applies filters:
    1. AvailabilityZoneFilter:  Same AZ (or cross-AZ if allowed)
    2. ComputeFilter:           Host is up and enabled
    3. RamFilter:               Enough RAM (with overcommit)
    4. DiskFilter:              Enough disk (block migration)
    5. ComputeCapabilitiesFilter: Matches extra_specs
    6. NUMATopologyFilter:      NUMA constraints satisfiable
    7. PciPassthroughFilter:    SR-IOV VFs available
    8. ServerGroupAntiAffinityFilter: Respect anti-affinity
    9. SameHostFilter:          Exclude source host
    10. DifferentHostFilter:    If specified
    11. AggregateMultiTenancyIsolation: Tenant segregation
    
  Weighers:
    1. RAMWeigher:              Prefer hosts with most free RAM
    2. MetricsWeigher:          Custom metrics (CPU load, etc.)
    3. ServerGroupSoftAffinityWeigher: Soft affinity preference
    4. CrossCellWeigher:        Prefer same cell (Cells v2)
    5. BuildFailureWeigher:     Avoid recently failed hosts
    
Phase 2: Resource Claiming (Placement API)
  POST /allocations/{consumer_uuid}
  {
    "allocations": {
      "{dest_rp_uuid}": {
        "resources": {
          "VCPU": 4,
          "MEMORY_MB": 8192,
          "DISK_GB": 50
        }
      }
    },
    "project_id": "...",
    "user_id": "...",
    "consumer_generation": null  // New allocation
  }
  
  Placement atomically:
    1. Checks inventory has capacity
    2. Creates allocation records
    3. Returns 204 or 409 (race condition)
    
  On 409 (another migration claimed last resources):
    Conductor retries with alternate host from scheduler list
    (scheduler returns top-N candidates for exactly this purpose)
```

### Migration Concurrency Control

```
Per-host migration limits (nova.conf):
  [DEFAULT]
  max_concurrent_live_migrations = 3  # Per host (inbound + outbound)
  
Implementation:
  Nova-compute maintains a semaphore:
    _live_migration_semaphore = eventlet.Semaphore(max_concurrent)
    
  Before starting migration:
    with self._live_migration_semaphore:
        self._do_live_migration(...)
        
  If semaphore full -> migration queued (blocks in eventlet greenthread)
  
  Problem: If all 3 slots are outbound, inbound migrations are blocked
  Solution: Separate inbound/outbound semaphores (proposed, not yet upstream)

Bandwidth management:
  Total host bandwidth:        25 Gbps
  Migration bandwidth per VM:   8 Gbps (configured)
  Max concurrent:               3
  Worst case:                   24 Gbps (96% of link)
  
  Risk: Starve production VM traffic
  Mitigation:
    1. QoS on migration traffic (DSCP marking + switch policer)
    2. Dedicated migration network (separate NIC/VLAN)
    3. Time-based scheduling (migrate during off-peak hours)
    4. nova.conf: live_migration_bandwidth = 4000  # MiB/s per migration
       Limits QEMU's migration thread bandwidth via libvirt API
```

### Resource Double-Booking During Migration

```
During live migration, resources are claimed on BOTH source and destination:

Timeline:
  T0: VM running on source (allocations on source RP)
  T1: Migration starts -> claim on destination RP (double allocation)
  T2: VM paused on source, resuming on destination
  T3: VM running on destination -> release source allocation
  
  Between T1-T3: Both hosts report the VM's resources as used
  
  Impact on scheduling:
    - Other VMs see less available capacity on destination (correct)
    - Source still shows VM resources as used (correct - VM still there)
    - Total cluster capacity appears reduced by 1 VM's worth (temporary)
  
  Nova's Placement API handles this:
    T1: POST /allocations/{instance_uuid}
        consumer_uuid = instance_uuid
        allocations = {dest_rp: {VCPU:4, MEMORY:8192}}
        // This MOVES the allocation from source to dest atomically
        // (reshape allocations in-place)
        
    BUT: During migration, source still needs the resources!
    Solution: Migration uses a "migration allocation":
        consumer_uuid = migration_uuid (separate consumer)
        allocations = {dest_rp: {VCPU:4, MEMORY:8192}}
        
    After completion:
        1. Delete migration_uuid allocations
        2. Update instance_uuid allocations: source_rp -> dest_rp
        
    This ensures:
        - Source resources held by instance allocation (until migration completes)
        - Dest resources held by migration allocation (during migration)
        - No double-counting in cluster capacity
```

---

## 8. Scaling Strategy

### Scaling Challenges and Solutions

```
1. Mass Migration During Host Maintenance:
   Problem: Need to evacuate 40 VMs from a host quickly
   
   Solution: Parallel migration with staggered start:
     - 3 concurrent outbound migrations (default limit)
     - Average 10s per VM -> 40/3 * 10 = ~133 seconds
     - With pipeline: VM1 starts, VM2 starts 1s later, VM3 1s later
     - Each completes in ~10s, next starts immediately
     - Effective throughput: ~3 VMs every 10 seconds
   
2. Rolling Upgrade of 2000-Host Cluster:
   Problem: Upgrade all hosts with minimal disruption
   
   Solution: Wave-based migration with capacity planning:
     Wave size: 50 hosts (2.5% of cluster)
     VMs per wave: 50 * 40 = 2,000 VMs
     Target hosts: 1,950 remaining hosts
     Extra capacity needed: 2,000 VMs / 1,950 hosts = ~1 extra VM/host
     Pre-check: Ensure each remaining host has capacity for 1 extra VM
     Wave duration: ~5 minutes (migration) + 15 minutes (upgrade) = 20 min
     Total: 40 waves * 20 minutes = ~13 hours
     
3. Large VM Migration (1 TB RAM):
   Problem: Pre-copy may not converge; transfer time is very long
   
   Solution: Hybrid pre-copy + post-copy with XBZRLE:
     Phase 1: Pre-copy with XBZRLE (transfer ~800 GB, 80% of memory)
       Time: 800 GB / 1 GB/s = 800 seconds ~ 13 minutes
     Phase 2: Switch to post-copy for remaining 200 GB
       Pause + transfer state: ~200ms
       Background push: 200 GB / 1 GB/s = 200 seconds
       Demand faulting: working set (~20%) faults in ~40 seconds
     Total: ~15 minutes, downtime ~200ms
```

#### Q&A: Scaling Strategy

**Q1: How do you handle the "thundering herd" problem when many hosts are evacuated simultaneously?**
**A:** When multiple hosts are evacuated (e.g., rack power event affecting 40 hosts x 40 VMs = 1,600 VMs), all evacuating at once would: (1) overwhelm the scheduler with 1,600 concurrent `select_destinations` calls, (2) saturate the migration network, (3) cause resource contention on destination hosts. The solution is hierarchical rate limiting: Nova Conductor processes evacuations in batches (configurable via `max_concurrent_builds`), the scheduler returns multiple candidate hosts for load spreading, and each host's migration semaphore limits inbound concurrency to 3. Additionally, Watcher or Blazar can pre-plan host evacuation order using bin-packing algorithms to minimize the number of intermediate migrations. In practice, even a large rack failure is handled in 5-10 minutes with proper orchestration.

**Q2: How does live migration scale when using Cells v2 with multiple cells?**
**A:** In Cells v2, each cell has its own database and message queue. Live migration within a cell works as described above. Cross-cell migration (moving a VM from Cell A to Cell B) is significantly more complex because it requires: (1) creating a new instance record in the destination cell's database, (2) transferring the instance's metadata, block device mappings, network info, etc., (3) updating the API cell database's instance mapping to point to the new cell, and (4) performing the actual live migration. Cross-cell migration was added in the Victoria release. The `cross_cell_move` flag in the migrations table tracks this. Performance-wise, cross-cell migration adds ~2-5 seconds of overhead for the database operations but the actual memory transfer time is the same.

**Q3: What is the impact of live migration on Ceph storage backend performance?**
**A:** With shared storage (Ceph RBD), live migration only transfers memory -- no disk I/O on the migration path. However, during migration, the VM continues doing storage I/O to Ceph. The migration itself doesn't add Ceph load. After migration, the VM's Ceph client (QEMU RBD driver) reconnects to the same Ceph pool from the new host. Ceph OSD locality may change: if the primary OSD was on the source host's rack, reads now traverse the network. Ceph's CRUSH map can be tuned with read affinity to prefer local replicas. For block migration (no shared storage), the local disk is copied over the network at 500 MB/s, which does NOT involve Ceph at all -- it's a direct host-to-host transfer via QEMU's NBD (Network Block Device) protocol.

**Q4: How do you prioritize migrations when resources are constrained?**
**A:** Migration priority is not natively supported in Nova but can be implemented via operator tooling. Priority levels: (1) Emergency evacuation (host hardware failure) -- highest priority, preempts other migrations, (2) Planned maintenance (host reboot/upgrade) -- medium priority, scheduled during maintenance windows, (3) Rebalancing (load distribution) -- lowest priority, can be deferred. Implementation: an external orchestrator (e.g., Watcher) maintains a priority queue of migration requests. It uses Nova's migration API with rate limiting: emergency migrations get 100% of migration bandwidth; maintenance gets remaining bandwidth; rebalancing only runs when no other migrations are active. The orchestrator also checks destination host load before initiating migrations to avoid overloading destination hosts.

**Q5: How do you handle migration failures at scale and prevent cascading failures?**
**A:** At scale, a single migration failure (e.g., network timeout, destination OOM) can cascade if not handled properly. Safeguards: (1) Circuit breaker pattern: if 3+ migrations to the same destination fail within 5 minutes, stop scheduling to that host and mark it for investigation. Nova's `BuildFailureWeigher` deprioritizes hosts with recent failures. (2) Exponential backoff: failed migrations retry with delays of 60s, 120s, 240s up to `max_retries=3`. (3) Global rate limiting: if overall migration failure rate exceeds 10%, pause all non-emergency migrations and alert operators. (4) Resource cleanup: failed migrations must release destination allocations (Placement API) and rollback port bindings (Neutron). Nova Conductor's `_cleanup_live_migration` handles this, but operators should verify with periodic reconciliation jobs that check for orphaned allocations.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios and Recovery

```
Scenario 1: Source host crashes during pre-copy
  State: VM was running on source, memory partially copied to dest
  Impact: VM is LOST on source (host down), partial copy on dest is USELESS
  Recovery:
    - Nova detects host failure via servicegroup (heartbeat timeout)
    - Evacuate instance: nova evacuate <server> [--host <new_host>]
    - If shared storage: boot from same root disk on new host (data preserved)
    - If local disk: VM data is lost (must restore from backup)
    - Migration record updated: status = 'error'
    - Destination cleanup: delete partial domain, release allocations

Scenario 2: Destination host crashes during pre-copy
  State: VM still running on source (it's the authority during pre-copy)
  Impact: Migration fails, but VM is SAFE on source
  Recovery:
    - Source nova-compute detects migration failure (libvirt event)
    - Rolls back: release destination allocations, revert port bindings
    - VM continues running on source uninterrupted
    - Migration record: status = 'failed'
    - Auto-retry with different destination (if configured)

Scenario 3: Source host crashes during post-copy
  State: VM running on destination, but some pages still on source
  Impact: CRITICAL - VM will crash with SIGBUS on unmapped page access
  Recovery:
    - Destination detects source is down
    - Any page fault for pages still on source -> unrecoverable
    - VM receives SIGBUS -> guest kernel panic or process crash
    - Mitigation: persistent postcopy (experimental) writes pages to
      shared storage
    - Best practice: avoid post-copy for critical VMs, or ensure source HA

Scenario 4: Network partition during migration
  State: Migration in progress, source and destination lose connectivity
  Impact: Migration stalls, VM continues on source
  Recovery:
    - QEMU migration thread detects TCP timeout (configurable, default 60s)
    - Migration automatically cancelled
    - If during post-copy: CATASTROPHIC (same as source crash)
    - If during pre-copy: VM safe on source, migration cancelled
    - Nova conductor: status = 'failed', cleanup allocations

Scenario 5: RabbitMQ failure during migration
  State: Nova-compute can't send RPC to conductor/dest
  Impact: Migration orchestration stalls, but libvirt migration continues
  Recovery:
    - Libvirt migration is a direct TCP connection (not via RabbitMQ)
    - Memory transfer continues even without Nova RPC
    - Problem: post-migration steps (port binding, DB update) can't execute
    - Nova-compute retries RPC with exponential backoff
    - If RabbitMQ recovers: post-migration completes normally
    - If RabbitMQ stays down: migration completes at libvirt level but
      Nova DB is inconsistent -> requires manual reconciliation
    - Nova's _heal_instance_info_cache periodic task will eventually fix state

Scenario 6: Libvirt/QEMU crash during migration
  State: QEMU process on source or destination dies
  Impact: Migration fails, VM state depends on which side crashed
  Recovery:
    Source QEMU crashes:
      - VM is lost (QEMU process was the VM)
      - Need evacuation from backups/shared storage
    Destination QEMU crashes:
      - Source QEMU detects migration failure
      - Source VM unpaused, continues running
      - Migration record: status = 'error'
      - Cleanup destination resources
```

### Migration Rollback Procedure

```
Rollback is supported ONLY during pre-copy phase:

Pre-copy rollback (any time before VM pause):
  1. Cancel QEMU migration: virDomainMigrateAbort()
  2. QEMU stops transferring pages, resumes normal operation
  3. Source nova-compute: _rollback_live_migration()
     a. Delete destination domain (if created)
     b. Unplug VIFs on destination
     c. Disconnect volumes on destination
     d. Release Placement allocations for migration
     e. Revert Neutron port bindings (if changed)
  4. Update migration record: status = 'cancelled'
  5. Update instance: task_state = None (back to normal)
  
  VM downtime during rollback: 0 (VM never paused)

Post-copy rollback: NOT POSSIBLE
  - VM state is split between hosts
  - Cannot reconstruct complete state on either host
  - Only option: let post-copy complete or accept VM loss
```

### Health Checks and Self-Healing

```
Periodic health checks related to migration:

1. Migration stuck detection (nova-conductor):
   - Every 60 seconds: check for migrations in 'running' state
   - If updated_at > timeout (default 3600s): auto-cancel
   - If libvirt reports migration completed but Nova status != completed:
     Force status update and run cleanup

2. Orphaned allocation cleanup (nova-manage):
   - nova-manage placement heal_allocations
   - Finds instances where Placement allocations don't match host
   - Common after migration failures that didn't clean up properly

3. Port binding consistency check:
   - Compare instance.host with port.binding:host_id
   - If mismatch: instance on host-A but port bound to host-B
   - Auto-fix: update port binding to match instance host
   - Run periodically or after migration failures

4. Resource provider inventory audit:
   - Compare actual host resources (from libvirt) with Placement inventory
   - Detect: migration leaked resources (claimed but no VM)
   - Fix: delete orphaned allocations
```

---

## 10. Observability

### Key Metrics

```
Migration-specific metrics (Prometheus/StatsD):

# Migration counts
nova_live_migrations_total{status="completed|failed|cancelled|error",source_host,dest_host}
nova_live_migrations_in_progress{host}
nova_live_migrations_queued{host}

# Migration timing
nova_live_migration_duration_seconds{quantile="0.5|0.9|0.99",migration_type}
nova_live_migration_downtime_ms{quantile="0.5|0.9|0.99"}
nova_live_migration_setup_time_seconds{quantile="0.5|0.9|0.99"}

# Memory transfer
nova_live_migration_memory_total_bytes{migration_uuid}
nova_live_migration_memory_transferred_bytes{migration_uuid}
nova_live_migration_memory_remaining_bytes{migration_uuid}
nova_live_migration_dirty_rate_bytes_per_sec{migration_uuid}
nova_live_migration_transfer_rate_bytes_per_sec{migration_uuid}
nova_live_migration_iterations{migration_uuid}

# Auto-converge
nova_live_migration_cpu_throttle_pct{migration_uuid}
nova_live_migration_xbzrle_cache_miss_rate{migration_uuid}

# Network cutover
nova_live_migration_network_cutover_ms{migration_uuid}
nova_port_binding_update_duration_ms{quantile="0.5|0.9|0.99"}

# Failures
nova_live_migration_failures_total{reason="cpu_incompatible|numa_mismatch|timeout|network|libvirt_error|resource_claim"}
nova_live_migration_rollback_total{host}

# Resource impact
nova_host_migration_bandwidth_usage_bytes_per_sec{host}
nova_host_migration_cpu_overhead_pct{host}
```

### Dashboards

```
Migration Operations Dashboard:
+---------------------------------------------------------------------+
|  LIVE MIGRATION OVERVIEW                        [Last 24 Hours]     |
+---------------------------------------------------------------------+
|                                                                     |
|  Completed: 1,847  |  Failed: 23  |  In Progress: 7  |  Queued: 3  |
|  Success Rate: 98.8%                                                |
|                                                                     |
|  Avg Duration: 12.4s  |  P99 Duration: 47.2s  |  P99 Downtime: 42ms|
|                                                                     |
+---------------------------------------------------------------------+
|  MIGRATION PROGRESS (Active)                                        |
|  +---------------------------------------------------------------+  |
|  | VM: web-prod-042   src:cn-017 -> dst:cn-042                   |  |
|  | [========================------] 78%  RAM: 6.2/8.0 GB         |  |
|  | Transfer: 980 MB/s  Dirty: 120 MB/s  Iter: 3  ETA: 2s        |  |
|  +---------------------------------------------------------------+  |
|  | VM: db-replica-007  src:cn-031 -> dst:cn-055                  |  |
|  | [========----------------------] 31%  RAM: 79/256 GB          |  |
|  | Transfer: 950 MB/s  Dirty: 890 MB/s  Iter: 1  ETA: 195s      |  |
|  | !! Auto-converge: 20% CPU throttle                            |  |
|  +---------------------------------------------------------------+  |
|                                                                     |
+---------------------------------------------------------------------+
|  FAILURE BREAKDOWN (Last 24h)                                       |
|  CPU Incompatible:   ========  8                                    |
|  Timeout:            ======    6                                    |
|  NUMA Mismatch:      ====      4                                    |
|  Network Error:      ===       3                                    |
|  Resource Claim:     ==        2                                    |
+---------------------------------------------------------------------+
```

### Logging Strategy

```
Migration-related log entries (structured JSON logging):

# Migration started
{
  "level": "INFO",
  "event": "live_migration_started",
  "migration_uuid": "a1b2c3d4-...",
  "instance_uuid": "e5f6g7h8-...",
  "source_host": "compute-017",
  "dest_host": "compute-042",
  "migration_type": "pre-copy",
  "block_migration": false,
  "vm_memory_gb": 8,
  "vm_vcpus": 4
}

# Iteration progress
{
  "level": "DEBUG",
  "event": "live_migration_iteration",
  "migration_uuid": "a1b2c3d4-...",
  "iteration": 3,
  "memory_remaining_mb": 320,
  "dirty_rate_mbps": 200,
  "transfer_rate_mbps": 980,
  "expected_downtime_ms": 12
}

# Auto-converge triggered
{
  "level": "WARNING",
  "event": "live_migration_auto_converge",
  "migration_uuid": "a1b2c3d4-...",
  "throttle_pct": 20,
  "dirty_rate_mbps": 890,
  "transfer_rate_mbps": 950,
  "reason": "dirty_rate_exceeds_bandwidth"
}

# Migration completed
{
  "level": "INFO",
  "event": "live_migration_completed",
  "migration_uuid": "a1b2c3d4-...",
  "total_time_seconds": 12.4,
  "downtime_ms": 18,
  "iterations": 5,
  "memory_transferred_gb": 9.2,
  "cpu_throttle_applied": false
}

# Migration failed
{
  "level": "ERROR",
  "event": "live_migration_failed",
  "migration_uuid": "a1b2c3d4-...",
  "error": "MigrationPreCheckError: Destination host lacks CPU feature avx512bw",
  "phase": "pre_check",
  "rollback_status": "completed"
}
```

### Alerting Rules

```yaml
# Prometheus alerting rules for migration
groups:
  - name: live_migration_alerts
    rules:
      - alert: MigrationSuccessRateLow
        expr: |
          rate(nova_live_migrations_total{status="completed"}[1h]) /
          rate(nova_live_migrations_total[1h]) < 0.95
        for: 15m
        labels:
          severity: warning
        annotations:
          summary: "Live migration success rate below 95%"
          
      - alert: MigrationStuck
        expr: nova_live_migration_duration_seconds > 1800
        for: 5m
        labels:
          severity: critical
        annotations:
          summary: "Live migration stuck for >30 minutes"
          
      - alert: MigrationDowntimeHigh
        expr: |
          histogram_quantile(0.99, nova_live_migration_downtime_ms) > 500
        for: 10m
        labels:
          severity: warning
        annotations:
          summary: "P99 migration downtime exceeds 500ms SLA"
          
      - alert: MigrationAutoConvergeActive
        expr: nova_live_migration_cpu_throttle_pct > 50
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Migration auto-converge throttling CPU >50%"
          
      - alert: HostMigrationConcurrencyMaxed
        expr: nova_live_migrations_in_progress >= 3
        for: 10m
        labels:
          severity: info
        annotations:
          summary: "Host at max concurrent migrations"
```

---

## 11. Security

### Migration Security Architecture

```
Threat Model for Live Migration:

1. Memory Eavesdropping:
   - VM memory transmitted over network in cleartext by default
   - Contains sensitive data: passwords, encryption keys, PII
   - Mitigation: TLS-encrypted migration transport
   
   Configuration (nova.conf):
     [libvirt]
     live_migration_with_native_tls = true  # QEMU native TLS
     # OR
     live_migration_tunnelled = true         # libvirt tunnelled (slower)
   
   QEMU TLS setup:
     - Each compute host has X.509 certificate
     - CA-signed, verified by destination
     - TLS 1.3 with AES-256-GCM
     - Performance impact: ~5-10% throughput reduction
     
   Certificate management:
     /etc/pki/libvirt/servercert.pem   # Host certificate
     /etc/pki/libvirt/serverkey.pem    # Private key
     /etc/pki/CA/cacert.pem            # CA certificate
     # Distributed via Ansible/Puppet, rotated annually

2. Migration Target Spoofing:
   - Attacker pretends to be a valid destination host
   - Receives VM memory dump (full compromise)
   - Mitigation: Mutual TLS (mTLS) authentication
     - Both source and destination verify certificates
     - Certificates bound to host identity in Keystone
     
3. Unauthorized Migration:
   - Non-admin user migrates VMs to compromised host
   - Policy: only cloud admin can specify destination host
   - oslo.policy: "os_compute_api:servers:migrations:index": "rule:admin_api"
   - Users can only request migration (no host choice); scheduler selects

4. Resource Exhaustion Attack:
   - Malicious tenant starts many migrations simultaneously
   - Exhausts migration bandwidth, impacts other tenants
   - Mitigation: Per-tenant migration quota (custom, not upstream)
   - Rate limiting in HAProxy: max 10 migration requests/minute per project

5. Post-copy Page Interception:
   - During post-copy, pages fetched over network on demand
   - MITM could intercept/modify pages in transit
   - Mitigation: same TLS encryption as pre-copy
   - Additional: page checksums (not in upstream QEMU, custom patch)
```

### Keystone Integration for Migration

```
Migration authorization flow:

1. User requests live migration:
   POST /v2.1/servers/{id}/action
   X-Auth-Token: <user_token>
   
2. Nova API validates token with Keystone:
   GET /v3/auth/tokens (token validation)
   Response includes: roles, project_id, domain_id
   
3. Policy check (nova/policy.yaml):
   # Who can initiate live migration
   "os_compute_api:os-migrate-server:migrate_live": "rule:admin_api"
   
   # Admin-only: specify destination host
   # Project-member: can request migration (if policy allows)
   
4. For cross-region migration (Keystone federation):
   - Source region validates token locally (Fernet)
   - Maps federated identity to local project
   - Destination region must trust source Keystone
   - Service-to-service auth: application credentials
   
5. Audit trail (Oslo notification):
   {
     "event_type": "compute.instance.live_migration.pre.start",
     "payload": {
       "instance_id": "...",
       "user_id": "...",
       "project_id": "...",
       "source_host": "...",
       "dest_host": "...",
       "request_id": "req-..."
     }
   }
```

---

## 12. Incremental Rollout

### Phased Deployment Strategy

```
Phase 1: Lab Validation (Week 1-2)
  - Deploy migration system in test environment
  - Test matrix:
    +--------------------+-----------+-----------+----------+
    | Scenario           | Pre-copy  | Post-copy | Block    |
    +--------------------+-----------+-----------+----------+
    | Small VM (2 GB)    | Test      | Test      | Test     |
    | Medium VM (32 GB)  | Test      | Test      | Test     |
    | Large VM (256 GB)  | Test      | Test      | N/A      |
    | Idle workload      | Test      | Test      | Test     |
    | CPU-intensive      | Test      | Test      | Test     |
    | Memory-intensive   | Test      | Test      | Test     |
    | Network-intensive  | Test      | Test      | Test     |
    | Multi-NIC          | Test      | Test      | Test     |
    | SR-IOV             | Test      | N/A       | Test     |
    | NUMA-pinned        | Test      | Test      | Test     |
    | Huge pages         | Test      | Test      | Test     |
    +--------------------+-----------+-----------+----------+
  - Validate: downtime < 100ms, success rate > 99%

Phase 2: Canary Deployment (Week 3-4)
  - Enable on 1 host aggregate (50 hosts)
  - Migrate only non-production / dev VMs
  - Monitor all metrics: success rate, downtime, duration
  - Gate: 99%+ success rate, P99 downtime < 100ms

Phase 3: Production Pilot (Week 5-6)
  - Enable for 1 availability zone (500 hosts)
  - Allow operator-initiated migrations (maintenance)
  - No automated rebalancing yet
  - Gate: successful maintenance window with zero VM incidents

Phase 4: Full Production (Week 7-8)
  - Enable cluster-wide
  - Enable automated rebalancing (via Watcher)
  - Enable post-copy for large VMs (>64 GB, opt-in)
  - Full SLA: < 100ms downtime, > 99.5% success rate

Phase 5: Advanced Features (Week 9-12)
  - Cross-cell migration
  - Cross-region migration (if applicable)
  - NUMA-aware automated rebalancing
  - Machine learning-based migration prediction
```

#### Q&A: Incremental Rollout

**Q1: How do you validate that live migration doesn't impact running workload performance?**
**A:** Run before/after performance benchmarks on the migrating VM: (1) CPU: run `sysbench cpu` and compare IOPS before and after migration -- should be identical. (2) Memory: run `stream` benchmark for memory bandwidth -- should be within 5% (NUMA placement may differ). (3) Network: run `iperf3` between the VM and a fixed endpoint -- should be identical (new path may differ by microseconds). (4) Storage: run `fio` random read/write -- should be identical for shared storage; slight variation for Ceph due to OSD locality change. (5) Application-level: run the application's own benchmarks (e.g., TPS for databases). During migration, monitor with `perf` for any CPU throttling (auto-converge) or page fault spikes (post-copy). Critical metric: the VM's internal clock (`clock_gettime`) should not show gaps > 100ms (the downtime window).

**Q2: What is the rollback plan if live migration causes unexpected issues in production?**
**A:** Immediate rollback: (1) Disable automated migrations via `nova service-disable --reason "migration issues" <host> nova-compute` or set `max_concurrent_live_migrations = 0` in nova.conf and restart. (2) Any in-progress pre-copy migrations can be cancelled via `openstack server migration abort`. (3) In-progress post-copy migrations CANNOT be cancelled -- must wait for completion. (4) If a migration left a VM in an inconsistent state (running on dest but DB says source), use `nova-manage cell_v2 list_instances` to find inconsistencies and `nova-manage placement heal_allocations` to fix. (5) If widespread issues, put cluster in "maintenance mode" via API rate limiting to prevent new migrations while investigating. Root cause analysis uses the structured logs and migration_progress table to identify the failure pattern.

**Q3: How do you test CPU compatibility across a heterogeneous cluster before rolling out?**
**A:** Build a CPU compatibility matrix using automation: (1) For each host, collect `virsh capabilities | xmllint --xpath '//cpu' -` to get the full CPU model and features. (2) Run `virsh cpu-baseline` against all host pairs to determine the common baseline. (3) Create a compatibility graph: edge between host A and B if `virsh cpu-compare A_cpu.xml B_cpu.xml` returns SUPERSET or IDENTICAL. (4) Identify disconnected components (hosts that cannot migrate between each other). (5) Configure host aggregates matching the connected components. (6) Test actual migration between each component pair in lab with a synthetic VM that exercises all CPU features (run `cpuid` and `stress-ng --matrix` to use AVX-512). This process should be automated and run after any firmware/microcode update.

**Q4: How do you safely enable post-copy migration given its risk of VM loss?**
**A:** Post-copy should be enabled incrementally: (1) Start with non-critical VMs only: development instances, stateless web servers, cattle-not-pets. Use a flavor extra_spec `hw:live_migration_policy=post-copy-allowed` that tenants opt into. (2) Require shared storage (Ceph) for any post-copy VM so disk state is always safe. (3) Enable hybrid mode first: pre-copy for 5 rounds, then switch to post-copy only if pre-copy hasn't converged. This means most migrations complete as pre-copy (safe), and only ~5% of problematic VMs use post-copy. (4) Monitor `memory_postcopy_requests` metric -- high values indicate the VM has a scattered working set and is suffering performance degradation. (5) Set up alerting for source host health during post-copy -- if source reports hardware errors, attempt to accelerate background page push to minimize the risk window.

**Q5: What is the process for upgrading the migration system itself (QEMU/libvirt versions)?**
**A:** QEMU and libvirt upgrades must be carefully coordinated because the migration protocol version must be compatible between source and destination. Process: (1) Read QEMU release notes for migration compatibility guarantees -- QEMU guarantees backward compatibility (newer QEMU can accept migrations from older QEMU) but NOT forward compatibility. (2) Upgrade destination hosts first (newer QEMU can receive from older). (3) Verify with test migration between old-source and new-destination. (4) Then upgrade source hosts (now all hosts are new). (5) Libvirt version must be >= the version that supports the QEMU version's migration features. (6) Never upgrade source and destination simultaneously -- always have a working migration path. (7) Keep one wave of hosts at the old version until all migrations from old hosts complete. The entire cluster upgrade takes 2-3 maintenance windows.

---

## 13. Trade-offs & Decision Log

| # | Decision | Options Considered | Choice | Rationale |
|---|----------|--------------------|--------|-----------|
| 1 | Default migration type | Pre-copy vs Post-copy vs Hybrid | Pre-copy | Safest: VM always recoverable on source; post-copy risks VM loss on source failure |
| 2 | Convergence strategy | Auto-converge vs XBZRLE vs Downtime increase | Auto-converge + XBZRLE | Combined approach: XBZRLE for compression, auto-converge as last resort; downtime increase alone can exceed SLA |
| 3 | Migration transport | TLS-encrypted vs Tunnelled vs Cleartext | Native TLS (QEMU) | 5% overhead acceptable for security; tunnelled adds libvirt bottleneck; cleartext exposes VM memory |
| 4 | Migration network | Shared with VM traffic vs Dedicated VLAN | Dedicated VLAN | Prevents migration traffic from impacting tenant network; easier QoS; 10-25 Gbps dedicated bandwidth |
| 5 | Storage backend | Shared (Ceph) vs Local disk | Shared (Ceph) primary | Eliminates disk migration (10x faster); enables instant evacuation; slight I/O latency penalty vs local NVMe |
| 6 | CPU model policy | host-passthrough vs host-model vs custom | Custom baseline per aggregate | Maximum migration flexibility; some features hidden but acceptable for 95% of workloads |
| 7 | Max concurrent migrations | 1 vs 3 vs 5 per host | 3 per host | Balance between evacuation speed and network/IO impact; 3 x 8Gbps = 24Gbps within 25Gbps link capacity |
| 8 | Post-copy enablement | Enabled for all vs Opt-in vs Disabled | Opt-in per flavor/image | Risk of VM loss too high for default; tenants who need it for large VMs can opt in with understanding of risks |
| 9 | NUMA enforcement | Strict (fail if no match) vs Relaxed (allow cross-NUMA) | Strict for pinned VMs, relaxed for shared | Performance-sensitive workloads (pinned) need NUMA guarantee; shared VMs tolerate cross-NUMA placement |
| 10 | Migration timeout | 30min vs 1hr vs 2hr | 1 hour (configurable) | Large VMs (1TB RAM) need ~15 min; 1hr provides safety margin; operators can adjust per-workload |
| 11 | Block migration support | Full support vs Deprecate | Support but discourage | Some deployments lack shared storage; block migration is 10x slower but necessary as fallback |
| 12 | Dirty page tracking | Software (bitmap) vs Hardware (EPT/NPT dirty bits) | Hardware when available | 10x less overhead than software tracking; all modern Intel/AMD CPUs support EPT/NPT dirty logging |

---

## 14. Agentic AI Integration

### AI-Driven Migration Optimization

```
Agentic AI Layer for Live Migration:

+---------------------------------------------------------------+
|                    AI Migration Orchestrator                    |
|  +-----------------------------------------------------------+|
|  |  Predictive Migration Engine                               ||
|  |                                                            ||
|  |  Input signals:                                            ||
|  |  - Host CPU/memory utilization trends (24h rolling)        ||
|  |  - VM workload patterns (daily/weekly cycles)              ||
|  |  - Hardware health predictions (disk SMART, ECC errors)    ||
|  |  - Planned maintenance windows                             ||
|  |  - Network topology and bandwidth availability             ||
|  |                                                            ||
|  |  Outputs:                                                  ||
|  |  - Proactive migration recommendations                    ||
|  |  - Optimal migration schedule (minimize disruption)        ||
|  |  - Destination selection (beyond basic scheduling)         ||
|  |  - Migration type selection (pre-copy vs hybrid)           ||
|  +-----------------------------------------------------------+|
|                                                                |
|  +-----------------------------------------------------------+|
|  |  Migration Parameter Tuning Agent                          ||
|  |                                                            ||
|  |  For each migration, dynamically tune:                     ||
|  |  - Bandwidth limit (based on network congestion)           ||
|  |  - Auto-converge thresholds (based on workload type)       ||
|  |  - XBZRLE cache size (based on page update pattern)        ||
|  |  - Max downtime (based on VM's SLA tier)                   ||
|  |  - Pre-copy iterations before post-copy switch             ||
|  |                                                            ||
|  |  Learning: Reinforcement learning on migration outcomes    ||
|  |  Reward: minimize(downtime * SLA_weight + duration)        ||
|  |  State: {vm_size, dirty_rate, bandwidth, cpu_model, ...}   ||
|  |  Action: {bandwidth_limit, converge_threshold, type, ...}  ||
|  +-----------------------------------------------------------+|
|                                                                |
|  +-----------------------------------------------------------+|
|  |  Anomaly Detection for Migration Health                    ||
|  |                                                            ||
|  |  Detect unusual patterns:                                  ||
|  |  - Migration taking 10x longer than predicted              ||
|  |  - Dirty rate suddenly spikes (workload change)            ||
|  |  - Network bandwidth drops mid-migration                   ||
|  |  - Multiple migrations failing to same destination         ||
|  |                                                            ||
|  |  Automated responses:                                      ||
|  |  - Cancel and retry with different parameters              ||
|  |  - Switch from pre-copy to hybrid mid-migration            ||
|  |  - Throttle concurrent migrations if network saturated     ||
|  |  - Alert operator if anomaly requires human judgment       ||
|  +-----------------------------------------------------------+|
+---------------------------------------------------------------+

Example AI-driven migration decision:

  Input:
    - Host compute-017 has 2 failing DIMMs (predicted failure in 48h)
    - 40 VMs on compute-017, mix of sizes (2GB to 256GB)
    - Current time: 2:00 AM (low traffic period)
    - Network utilization: 15% (plenty of bandwidth)
    
  AI Decision:
    1. Prioritize VMs by criticality (SLA tier)
    2. Schedule large VMs (256 GB) first (they take longest)
    3. Use pre-copy with XBZRLE for all except the 256 GB database VM
    4. Use hybrid pre-copy + post-copy for the 256 GB VM
    5. Set bandwidth to 10 Gbps (network is underutilized)
    6. Spread destinations across 3 racks (avoid single point of failure)
    7. Expected completion: 2:45 AM (well before morning traffic spike)
    8. Alert operator if any migration fails after 2 retries
```

### Natural Language Operations Interface

```
Operator: "Migrate all VMs off compute-017 before the maintenance window at 6 AM"

AI Assistant Analysis:
  - Host: compute-017 (40 VMs, 320 GB total RAM)
  - Maintenance window: 6:00 AM (4 hours from now)
  - Available bandwidth: 10 Gbps migration network
  - VM breakdown: 35 small (2-8GB), 3 medium (32GB), 2 large (64GB)
  
  Estimated migration time:
    35 small VMs: 35 * 10s / 3 concurrent = ~117s = 2 min
    3 medium VMs: 3 * 32s / 3 concurrent = ~32s = 0.5 min
    2 large VMs:  2 * 64s / 2 concurrent = ~64s = 1 min
    Total: ~3.5 minutes + 2 min buffer = 5.5 minutes
    
  Response: "I'll evacuate all 40 VMs from compute-017. Estimated completion 
  in 6 minutes. Starting with the 2 large VMs (64 GB each) to maximize 
  parallelism. Shall I proceed?"
  
  Operator: "Yes, proceed but skip vm-batch-007, it's being decommissioned"
  
  AI: "Understood. Migrating 39 VMs, excluding vm-batch-007. Starting now.
  Progress updates every 60 seconds."
```

---

## 15. Complete Interviewer Q&A Bank

### Architecture and Design (Q1-Q5)

**Q1: Walk me through what happens when an operator runs `openstack server migrate --live-migration <server>`. Cover every component involved.**

**A:** The flow crosses six components:

1. **CLI to Nova API**: The CLI sends `POST /v2.1/servers/{id}/action` with the `os-migrateLive` body. Nova API authenticates via Keystone, checks the oslo.policy rule `os_compute_api:os-migrate-server:migrate_live` (admin required), validates the instance exists and is ACTIVE.

2. **Nova API to Nova Conductor** (via RabbitMQ): API sends an RPC cast `live_migrate_instance(instance, host, block_migration, force)` to the conductor. The conductor creates a `LiveMigrationTask` which orchestrates the entire process.

3. **Conductor to Scheduler** (if no host specified): Conductor calls `select_destinations` with the instance's resource requirements. The scheduler runs through filters (ComputeFilter, RamFilter, NUMATopologyFilter, etc.) and weighers to select the best destination. Returns top-N candidates.

4. **Conductor to Placement API**: Claims resources on the destination. `POST /allocations/{migration_uuid}` with the instance's resource requirements against the destination resource provider. If 409 (resource race), tries next candidate.

5. **Conductor pre-checks**: Verifies CPU compatibility (via Placement traits or libvirt `virConnectCompareCPU`), NUMA topology feasibility, shared storage connectivity, hypervisor version compatibility.

6. **Conductor to Source Compute** (via RabbitMQ): Sends RPC `live_migration(instance, dest, block_migration, migration, migrate_data)`.

7. **Source Compute to Destination Compute** (via RabbitMQ): Calls `pre_live_migration()` on destination. Destination prepares: creates libvirt XML, plugs VIFs into OVS, connects Ceph RBD volumes, creates target directory.

8. **Source Compute to Libvirt to QEMU**: Calls `virDomainMigrateToURI3()` which instructs QEMU to start the pre-copy migration. QEMU opens a TCP connection to destination's QEMU on port 49152+ and begins transferring memory pages.

9. **QEMU pre-copy iterations**: Multiple rounds of dirty page transfer. Source nova-compute polls `virDomainGetJobStats()` every second and updates the migration_progress DB records.

10. **Convergence, Pause, Transfer**: When remaining dirty pages < threshold, QEMU pauses the VM, sends final pages + device state + CPU registers.

11. **Source Compute to Neutron**: Updates port bindings via `PUT /v2.0/ports/{id}` with `binding:host_id = dest-host`. OVN Southbound DB updates, OpenFlow rules installed on destination, GARP sent.

12. **Destination QEMU**: Resumes VM. Guest begins executing on destination.

13. **Source Compute to Destination Compute**: Calls `post_live_migration_at_destination()`. Destination confirms VM running, updates instance.host in DB.

14. **Source Compute cleanup**: Deletes source domain, unplugs VIFs, disconnects volumes, releases source Placement allocations.

15. **Conductor**: Updates migration status to 'completed', clears instance task_state.

---

**Q2: How would you design live migration for a VM with 1 TB of RAM that's running an in-memory database with 3 GB/s dirty page rate?**

**A:** This is one of the hardest migration scenarios. With 3 GB/s dirty rate and typical 1 GB/s migration bandwidth, pre-copy alone will never converge (r = 3.0 > 1). My approach:

**Option A: Maximize bandwidth + XBZRLE**
- Use dedicated 100 Gbps RDMA migration link: effective 12 GB/s bandwidth
- Enable XBZRLE with 10% cache (100 GB cache) for ~3x compression
- Effective bandwidth: 12 * 3 = 36 GB/s > 3 GB/s dirty rate
- Pre-copy converges in ~5 iterations, total time ~90 seconds, downtime ~50ms
- Cost: requires 100 Gbps NIC and RDMA infrastructure

**Option B: Hybrid pre-copy + post-copy (standard 25 Gbps network)**
- Pre-copy phase: transfer at 3 GB/s (25 Gbps) for 5 minutes. Transfers ~900 GB (most of memory), but can't converge. 90% of pages sent at least once.
- Switch to post-copy: pause for ~200ms, transfer state.
- Resume on destination: 10% of pages (100 GB) demand-faulted. Working set is typically 20% of total (200 GB), but 90% already transferred. Only ~20 GB of working set needs demand-faulting. At 3 GB/s, faults resolve in ~7 seconds of degraded performance.
- Background push: remaining pages transfer in ~33 seconds.
- Risk: source failure during post-copy loses up to 100 GB of pages.
- Mitigation: keep source alive and monitored until background push completes.

**Option C: Auto-converge with application awareness**
- Coordinate with the database application (e.g., pause replication intake)
- Reduce dirty rate during migration from 3 GB/s to <1 GB/s
- Application-level: set database to read-only mode for 30 seconds
- Pre-copy completes during read-only window
- Total downtime including read-only: ~30 seconds (application-aware)
- Suitable only if brief read-only is acceptable

I'd recommend **Option B** for most cases, with **Option A** if the infrastructure supports 100 Gbps RDMA, and **Option C** if the application team agrees to a maintenance window.

---

**Q3: Compare and contrast shared-storage live migration vs block migration. When would you choose each?**

**A:**

| Aspect | Shared Storage (Ceph RBD) | Block Migration (Local Disk) |
|--------|---------------------------|------------------------------|
| Disk transfer | None (same pool) | Full disk copy over network |
| Migration time (100GB disk, 8GB RAM) | ~10 seconds | ~210 seconds |
| Network bandwidth | RAM only (~8 Gbps) | RAM + Disk (~12 Gbps sustained) |
| Storage I/O during migration | Normal (Ceph handles) | Degraded (disk being copied) |
| Rollback | Clean (disk unchanged) | Complex (partial disk copy) |
| Storage cost | 3x replication overhead (Ceph) | Local disk only |
| I/O latency | ~200us (network to OSD) | ~50us (local NVMe) |
| Failure domain | Ceph cluster failure affects all VMs | Single host failure affects fewer VMs |

**Choose shared storage when:** Frequent migrations needed (maintenance, rebalancing); fast evacuation is critical (host failure recovery); running many VMs per host (40+ VMs); disaster recovery requirements (Ceph replication provides it).

**Choose block migration when:** I/O latency is critical (databases on local NVMe: 50us vs 200us); small cluster where Ceph operational overhead isn't justified; high-throughput storage workloads (local NVMe: 3 GB/s vs Ceph: 500 MB/s); cost-sensitive (local disk is cheaper than 3x replicated Ceph).

**Hybrid approach:** Boot disk on Ceph (for fast migration) + ephemeral data on local NVMe (for I/O performance). Use Cinder volume types to let tenants choose.

---

**Q4: How do you handle live migration in a multi-tenant environment where tenants have different SLA requirements?**

**A:** Multi-tenant migration requires differentiated treatment:

1. **SLA Tiers via Flavors:** Tier 1 (Platinum): guaranteed < 50ms downtime, pre-copy only (safest), dedicated migration bandwidth 10 Gbps. Tier 2 (Gold): guaranteed < 200ms downtime, hybrid allowed, shared bandwidth. Tier 3 (Silver): best-effort, may use auto-converge with 50% throttle, lowest priority. Implemented via flavor extra_specs: `hw:live_migration_policy`, `hw:live_migration_max_downtime`, `hw:live_migration_bandwidth`.

2. **Scheduling Priority:** Emergency evacuations override all tiers. During planned maintenance, Tier 1 VMs migrate first (ensure best conditions), then Tier 2, then Tier 3.

3. **Resource Isolation:** Dedicated migration VLAN for Tier 1 tenants to prevent noisy-neighbor bandwidth contention. QoS marking (DSCP) on migration traffic with switch-level priority queuing.

4. **Quota Enforcement:** Limit concurrent migrations per project to prevent one tenant from monopolizing migration resources. Default: 3 concurrent, Tier 1 gets 5.

5. **Transparency:** Tenant notification before migration (configurable lead time). Post-migration report: downtime measured, SLA compliance confirmed.

---

**Q5: Explain the security implications of live migration and how you would secure the migration data path.**

**A:** Live migration transmits the VM's entire memory contents over the network, which includes everything in the VM's address space: passwords in memory, encryption keys, TLS session keys, database records, authentication tokens.

**Threat vectors:** (1) Passive eavesdropping: Network sniffer captures memory pages in transit, full VM compromise. (2) Active MITM: Attacker intercepts and modifies pages, code injection into running VM. (3) Unauthorized destination: Rogue host receives migration, VM memory stolen. (4) Replay attack: Captured migration stream replayed to spin up cloned VM.

**Security controls:** (1) Transport encryption: Enable QEMU native TLS (`live_migration_with_native_tls = true`). TLS 1.3 with mutual authentication. Each compute host has an X.509 certificate signed by the cluster CA. (2) Network isolation: Migration traffic on a dedicated VLAN/VRF, inaccessible from tenant networks. Physical network ACLs restrict migration ports (TCP 49152-49215) to compute-to-compute only. (3) Authorization: Only cloud admins can specify migration destination hosts (oslo.policy). All migration actions are audited via Oslo notifications to SIEM. (4) Certificate management: Use short-lived certificates (90-day rotation) distributed via Vault/Barbican. Certificate revocation list (CRL) checked before each migration. (5) Post-migration verification: After migration, compare checksums of device state to detect tampering.

---

### Operations and Troubleshooting (Q6-Q10)

**Q6: A live migration has been running for 45 minutes and shows no sign of completing. Walk me through your debugging process.**

**A:** Systematic debugging approach:

1. **Check migration status and progress:**
   ```bash
   openstack server migration show <server> <migration_id> -f json
   ```
   If `memory_remaining` isn't decreasing, dirty rate >= transfer rate (not converging).

2. **Check libvirt job stats on source compute:**
   ```bash
   virsh domjobinfo <instance-name>
   ```
   Key fields: `Data remaining`, `Dirty rate`, `Memory bandwidth`, `Iteration`.

3. **Identify root cause -- common scenarios:**

   **Scenario A: Dirty rate exceeds bandwidth (most common)** -- `Dirty rate: 2.1 GB/s`, `Memory bandwidth: 1.0 GB/s`, ratio 2.1, will never converge. Fix: Enable auto-converge or abort and restart with XBZRLE.

   **Scenario B: Bandwidth is lower than expected** -- `Memory bandwidth: 100 MB/s` (expected 1 GB/s), network issue. Check: `iperf3` between source and destination migration IPs. Possible: migration network congestion, NIC link speed negotiated low, QoS policer limiting.

   **Scenario C: Migration stuck in "preparing destination"** -- Destination pre_live_migration hung. Check destination nova-compute logs. Common: Ceph RBD connection timeout, Neutron port plug failure, libvirt domain create error.

   **Scenario D: QEMU migration paused (waiting for disk)** -- Block migration with large disk: check `Disk remaining` in domjobinfo.

4. **Decision: fix or abort.** If fixable (bandwidth issue, tune parameters): fix and monitor. If unfixable (fundamental dirty rate issue): abort migration or force-complete (accepts higher downtime).

---

**Q7: After a live migration completes, users report that the VM is running but its network connectivity is broken. What's wrong and how do you fix it?**

**A:** This is a network cutover failure. Debugging steps:

1. **Verify VM is running on destination:** `openstack server show <server> -c OS-EXT-SRV-ATTR:host -c status`. Confirm: status=ACTIVE, host=dest-host.

2. **Check port binding:** `openstack port show <port_id> -c binding_host_id -c status`. If `binding_host_id` still points to source, Neutron port binding update failed. Fix: `openstack port set --host <dest-host> <port_id>`.

3. **Check OVS flows on destination:** `ovs-ofctl dump-flows br-int | grep <vm_mac>`. If no flows, ovn-controller hasn't installed rules yet. Check: `ovn-sbctl list port_binding | grep <port_id>` -- verify chassis is set to dest.

4. **Check GARP was sent:** `tcpdump -i br-ex arp | grep <vm_ip>`. If no GARP, physical switches haven't learned new MAC location. Fix: manually send GARP from dest host.

5. **Check security group flows:** If conntrack rules are missing, security group not applied on dest. Fix: restart ovn-controller on dest host to re-sync with Southbound DB.

6. **Common root causes:** Neutron ML2 plugin timeout during port binding update; OVN Southbound DB replication lag; physical switch slow to learn (spanning-tree reconvergence); VM has static MAC that doesn't match port security.

---

**Q8: How do you handle a situation where you need to evacuate a host that has VMs with SR-IOV passthrough devices?**

**A:** SR-IOV VMs are the hardest to live migrate because the NIC VF is directly mapped to the VM's address space, bypassing the hypervisor.

**Approach 1: Bond-based failover (if configured)** -- VM has both a virtio NIC (migration-capable) and an SR-IOV VF, bonded. During migration: unplug SR-IOV VF, traffic failovers to virtio bond member, live migrate normally, plug new SR-IOV VF on destination, re-add to bond. Downtime: ~1-2 seconds for VF plug/unplug operations.

**Approach 2: Cold migration (if no bond)** -- VM must be shut down (power off). nova stop, nova migrate, nova start. Downtime: 30-60 seconds.

**Approach 3: Switchdev mode SR-IOV (newer hardware)** -- NIC operates in switchdev mode (e.g., Mellanox ConnectX-6). OVS offload rules to hardware, but NIC is managed like software bridge. Live migration works because QEMU can snapshot VF state. Downtime: ~100ms.

**For the host evacuation:** Identify all SR-IOV VMs, live migrate bonded VMs normally, schedule 1-minute downtime per non-bonded VM for cold migration, migrate non-SR-IOV VMs in parallel.

---

**Q9: Explain how you would implement automated workload rebalancing using live migration.**

**A:** Automated rebalancing uses OpenStack Watcher or a custom service to continuously optimize VM placement.

**Architecture:** Telemetry (Gnocchi/Prometheus) feeds Watcher, which drives Nova Scheduler for Live Migration.

**Rebalancing Algorithm:** (1) Data collection: Every 5 minutes, collect per-host metrics (CPU, memory, network, storage). (2) Imbalance detection: Calculate standard deviation of host utilization; if stddev > threshold (e.g., 15% CPU), trigger rebalancing. (3) Migration planning: Use bin-packing algorithm (FFD) to compute target placement minimizing stddev. (4) Cost estimation: For each proposed migration, estimate duration, downtime, network cost. Only proceed if benefit > cost. (5) Execution: Submit migrations ordered by highest benefit first, respect concurrency limits. (6) Validation: After all migrations, re-measure utilization.

**Key considerations:** Oscillation prevention (30-minute cool-down), respect affinity/anti-affinity constraints, time-awareness (avoid peak hours), NUMA awareness, cost function: `migration_cost = downtime_penalty * SLA_weight + bandwidth_consumed + destination_utilization_increase`.

---

**Q10: A large VM (128 GB RAM) migration shows 99% memory transferred but has been stuck at 99% for 10 minutes. What's happening?**

**A:** The "stuck at 99%" symptom is characteristic of the VM dirtying the last 1% faster than it can be transferred. At 99%, ~1.28 GB remains. If the dirty rate for those specific pages is high (hot pages), the migration enters a tight loop.

**Root cause:** The remaining 1% of pages are the hottest pages (e.g., database buffer pool, active heap). They're being re-dirtied within the time it takes to transfer them. Check: `virsh domjobinfo <vm>` -- if dirty rate exceeds bandwidth for the remaining data, the hot set can't converge.

**Solutions (in order of preference):** (1) **Force-complete:** `openstack server migration force complete <server> <migration_id>`. Immediately pauses the VM and sends remaining pages. Downtime: 1.28 GB / 1 GB/s = 1.3 seconds. (2) **Increase bandwidth:** Temporarily increase migration bandwidth to overcome dirty rate. (3) **Switch to post-copy:** If QEMU supports mid-migration switch, enable post-copy for the remaining 1%. (4) **Application coordination:** Ask the tenant to temporarily pause their write-heavy workload for 2 seconds.

The last 1% is where most migration tuning effort goes. It's a mathematical certainty that the hottest pages converge last.

---

### Advanced Topics (Q11-Q16)

**Q11: How does libvirt communicate with QEMU during migration, and what happens if that communication channel breaks?**

**A:** Libvirt communicates with QEMU via a UNIX domain socket using QMP (QEMU Machine Protocol) -- a JSON-based protocol at `/var/lib/libvirt/qemu/domain-1-<instance>/monitor.sock`.

During migration, libvirt uses QMP commands: `migrate` (start), `query-migrate` (poll progress), `migrate-set-parameters` (adjust bandwidth/downtime), `migrate-cancel` (abort), `migrate-pause`/`migrate-continue`.

**If the QMP socket breaks (libvirtd crashes/restarts):** QEMU continues running as a separate process. The in-progress migration continues (QEMU-to-QEMU TCP connection is independent of libvirt). Libvirtd reconnects on restart and re-discovers the running domain. Nova-compute's libvirt driver detects the reconnection and calls `virDomainGetJobStats()` to re-sync migration status.

**If the QEMU-to-QEMU TCP connection breaks:** Source QEMU's migration thread gets a TCP error. Migration is automatically cancelled. Source VM continues running (if pre-copy). Source VM is lost if post-copy (destination had already taken over). QEMU reports migration failure to libvirt via QMP event `MIGRATION` with status `failed`.

---

**Q12: Describe how you would implement live migration for a VM that has PCI passthrough devices (GPU, FPGA) that don't support migration.**

**A:** PCI passthrough devices (VFIO) have direct hardware access, making their state non-migrateable.

**Solutions by device type:**

1. **GPU (NVIDIA A100, etc.):** NVIDIA's vGPU (GRID) supports live migration with NVIDIA driver saving GPU memory and state. Full passthrough is not migratable. Workaround: Application checkpoint (CUDA `cudaDeviceSynchronize` + save context), stop VM, cold migrate, restore on dest GPU.

2. **FPGA (Intel/Xilinx):** FPGA bitstreams are device-specific. Migration requires quiescing FPGA, saving bitstream state, reprogramming destination FPGA. Typically cold migration only.

3. **Generic approach for unmigrateable devices:** Graceful degradation pattern -- VM has both passthrough device AND software fallback. Before migration: hot-unplug passthrough device, fall back to software path. Migrate via standard live migration. After migration: hot-plug passthrough device on destination. For VMs that cannot tolerate device removal: mark as non-migratable (`hw:pci_passthrough_enable_migration = false`), host maintenance requires VM shutdown.

---

**Q13: How would you design cross-region live migration (e.g., US-East to EU-West)?**

**A:** Cross-region live migration is extremely challenging due to WAN characteristics: 80-120ms RTT, 1-10 Gbps WAN link, 0.1-1% packet loss, and GDPR data residency concerns.

**Design decisions:** (1) TCP tuning: Window size must accommodate BDP (Bandwidth-Delay Product): 10 Gbps x 80ms = 100 MB. Use TCP BBR congestion control. (2) Pre-copy with aggressive compression: Enable XBZRLE + multifd (8 channels). (3) Migration time estimate for 8 GB VM at 1.25 GB/s: ~8 seconds + 80ms RTT penalty. (4) Network cutover is the hard part: options include BGP anycast, DNS failover, or stretch L2 via EVPN/VXLAN over WAN. (5) Storage: Must use cross-region replicated storage (Ceph RBD mirror). Start async replication days before migration. (6) Regulatory: Encrypt all traffic (IPsec/WireGuard). Log data movement for GDPR Article 46 compliance.

---

**Q14: How does live migration interact with Cinder volume attachments and iSCSI/FC storage?**

**A:** Volume-backed instances have a different migration path than ephemeral-disk instances:

**Shared storage (Ceph RBD):** Both hosts access the same Ceph cluster. During migration only memory is transferred. After migration, source QEMU closes RBD image, destination continues using it.

**iSCSI storage:** (1) Pre-migration: Cinder creates new iSCSI connection from dest to target (`os-initialize_connection`). Both hosts have active sessions to same LUN. (2) During migration: memory transfer happens normally. (3) Post-migration: Source disconnects (`os-terminate_connection`). Risk: brief dual-access; Cinder's LVM driver uses SCSI reservations.

**Fibre Channel:** Similar to iSCSI but uses FC zoning. Zone changes can take 5-30 seconds for fabric reconfiguration.

**Encrypted volumes (LUKS):** Encryption key is in VM's memory (dm-crypt). After migration, key is present on destination (transferred with memory). If using Barbican for key management, key retrieval happens on dest during `pre_live_migration`.

---

**Q15: What is the difference between `nova live-migration` and `nova evacuate`? When would you use each?**

**A:** These serve fundamentally different purposes:

| Aspect | Live Migration | Evacuation |
|--------|---------------|------------|
| Source host state | Running | Down/dead |
| VM state preserved | Yes (full, including memory) | Partial (disk only, if shared storage) |
| Downtime | < 100ms | Minutes (time to detect + rebuild) |
| In-memory state | Preserved | Lost |
| Automatic trigger | No (operator initiates) | Can be automatic via Masakari |
| Resource cleanup | Source resources released after migration | Source resources released via fencing |
| Fencing required | No | Yes (IPMI/stonith to prevent split-brain) |

**Evacuation process:** (1) Detect host failure (Nova servicegroup heartbeat timeout, 60s). (2) Fence failed host (IPMI power-off). (3) `nova evacuate <server>`. (4) Nova creates new instance on destination. (5) If shared storage: attach same volumes, boot from same root disk. (6) If not: rebuild from original image (data loss). (7) Network ports rebound.

**Masakari** is OpenStack's HA service that automates evacuation: monitors host health via pacemaker/corosync, on host failure automatically evacuates all VMs.

---

**Q16: How would you benchmark and validate that your live migration system meets the < 100ms downtime SLA?**

**A:** Comprehensive validation requires measuring downtime from multiple perspectives:

**1. Guest-side measurement (most accurate):** Inside the VM, run a tight timestamp loop at 1ms resolution. After migration, analyze gaps in timestamps to find the maximum gap (the downtime).

**2. Network-side measurement:** From an external host, ping the VM at 1ms intervals. Count consecutive lost pings during migration -- each lost ping is approximately 1ms of downtime.

**3. Application-side measurement:** Run a continuous HTTP health check at 10ms intervals. Find the window where health checks fail or time out.

**4. Libvirt/QEMU reported downtime:** `virsh domjobinfo <instance> --completed | grep "Total downtime"`. This is QEMU's self-reported VM pause duration (does NOT include network cutover time).

**5. Comprehensive test matrix:** Test across VM sizes (2GB to 256GB), workload types (idle, CPU-intensive, memory-intensive, network-intensive), migration types (pre-copy, hybrid, block). Gate criteria: P99 downtime < 100ms for standard VMs (< 64 GB).

**6. Automated regression testing:** CI/CD pipeline runs migration tests before each Nova upgrade. Gate: no merge if P99 downtime > 100ms for standard VMs. Alerting: if production P99 downtime trends above 80ms, investigate before SLA breach.

---

## 16. References

### OpenStack Documentation
- [Nova Live Migration Documentation](https://docs.openstack.org/nova/latest/admin/live-migration-usage.html)
- [Nova Configuration: Libvirt](https://docs.openstack.org/nova/latest/configuration/config.html#libvirt)
- [Nova Live Migration Internals](https://docs.openstack.org/nova/latest/reference/live-migration.html)
- [Placement API](https://docs.openstack.org/placement/latest/)
- [Neutron ML2 Plugin Architecture](https://docs.openstack.org/neutron/latest/admin/config-ml2.html)

### QEMU/KVM References
- [QEMU Migration Documentation](https://www.qemu.org/docs/master/devel/migration.html)
- [KVM Dirty Page Tracking](https://www.kernel.org/doc/html/latest/virt/kvm/api.html#kvm-get-dirty-log)
- [QEMU Pre-copy Migration Algorithm](https://www.qemu.org/docs/master/devel/migration/main.html)
- [userfaultfd(2) - Linux Manual Page](https://man7.org/linux/man-pages/man2/userfaultfd.2.html)

### Libvirt References
- [virDomainMigrateToURI3](https://libvirt.org/html/libvirt-libvirt-domain.html#virDomainMigrateToURI3)
- [Libvirt Migration Guide](https://libvirt.org/migration.html)
- [virConnectCompareCPU](https://libvirt.org/html/libvirt-libvirt-host.html#virConnectCompareCPU)

### Research Papers
- Clark, C. et al. "Live Migration of Virtual Machines" (NSDI 2005) -- Original pre-copy algorithm
- Hines, M. et al. "Post-Copy Live Migration of Virtual Machines" (VEE 2009)
- Sahni, S. et al. "A Survey of Live VM Migration Techniques" (2012)
- Ibrahim, K. et al. "Optimizing Pre-Copy and Post-Copy Live Migration" (IEEE Cloud 2017)

### Production Operations
- [Red Hat OpenStack Platform: Live Migration Guide](https://access.redhat.com/documentation/en-us/red_hat_openstack_platform/)
- [Canonical Charmed OpenStack: Migration Operations](https://ubuntu.com/openstack)
- [CERN OpenStack: Large-Scale Migration Experience](https://openstack-in-production.blogspot.com/)

### Network References
- [OVN Architecture](https://www.ovn.org/en/architecture/)
- [VXLAN RFC 7348](https://tools.ietf.org/html/rfc7348)
- [BGP EVPN RFC 7432](https://tools.ietf.org/html/rfc7432)
- [Gratuitous ARP (GARP)](https://wiki.wireshark.org/Gratuitous_ARP)
