# Common Patterns — OpenStack Cloud (14_openstack_cloud)

## Common Components

### MySQL + Galera Cluster (Synchronous Multi-Master)
- All 5 systems store authoritative state in MySQL with Galera synchronous certification-based replication; zero data loss on single node failure; 3–5 node cluster minimum
- cloud_control_plane: `keystone` 500 MB, `nova_api` 1 GB, `nova_cell0–cell2` 30 GB total, `neutron` 2 GB, `cinder` 1 GB, `glance` 200 MB, `heat` 500 MB, `barbican` 200 MB, `placement` 500 MB — total ~40 GB
- All tables share field patterns: `id (INT AUTO_INCREMENT)` or `uuid (VARCHAR(36))`, `created_at DATETIME`, `updated_at DATETIME`, `deleted INT DEFAULT 0` (soft delete), `project_id VARCHAR(36)` with index
- Target: 5,000 TPS; write latency 5–20 ms (certification + network broadcast)

### RabbitMQ Quorum Queues (Raft Consensus)
- 4 of 5 systems use RabbitMQ for OpenStack RPC; quorum queues (Raft, not classic mirrored); 3-node cluster; 64 GB RAM per node
- cloud_control_plane: 10,000 messages/sec target; exchange `nova` with routing keys `scheduler`, `conductor`, `compute.<hostname>`
- openstack_nova_compute: `nova` exchange + `neutron` exchange; RPC Call (synchronous, reply queue) for `select_destinations`; RPC Cast (async fire-and-forget) for `build_instances`
- vm_live_migration: live migration progress streamed via oslo.messaging; migration bandwidth events
- Failover: < 10 s for new queue leader election; quorum queue writes require ACK from majority (2 of 3)

### Keystone Fernet Tokens (Local Cryptographic Validation)
- All 5 systems authenticate via Keystone; Fernet tokens are self-contained (HMAC + encryption); validated locally without database call
- Token validation: < 5 ms (local Fernet) / < 50 ms (Memcached cache hit) / ~80 ms (cross-region fetch, infrequent)
- Fernet key rotation: every 30 min; 3-stage key ring: `0` (staged) → `1` (secondary/validates old) → `2` (primary/signs new)
- multi_region: single global Keystone (US-East); per-region Keystone proxy caches tokens + catalog (5-min TTL, ~50 KB catalog); token issuance cross-region (~80 ms RTT) but validation is local
- All services validate tokens via `oslo.middleware.auth_token`; no Keystone call on hot path

### HAProxy + Keepalived (VIP with < 3 s VRRP Failover)
- All 5 systems front API services with HAProxy; active-passive VIP via keepalived; VRRP failover < 3 s
- cloud_control_plane: SSL termination, health checks, connection pooling, rate limiting; 10,000 concurrent API connections
- multi_region: GeoDNS (GSLB) resolves to regional HAProxy VIPs; 100 Gbps DCI links between regions
- nova_compute: nova-api stateless, N instances behind HAProxy; nova-scheduler stateless, N instances
- API targets: P99 < 500 ms; availability 99.99% (52 min/year)

### Placement API (Resource Provider Inventories + Allocation Candidates)
- 3 of 5 systems (cloud_control_plane, nova_compute, live_migration) use Placement API for resource tracking
- `resource_providers`: `id`, `uuid`, `generation` (optimistic locking), `parent_provider_uuid` (for nested: NUMA, GPU)
- `inventories`: `resource_class_id` (VCPU=0, MEMORY_MB=1, DISK_GB=2, CUSTOM_*), `total`, `reserved`, `allocation_ratio` (overcommit)
- `allocations`: `consumer_id` (instance UUID), `used INT`
- `traits`: `HW_CPU_X86_AVX2`, `COMPUTE_STATUS_DISABLED`, `CUSTOM_*`
- nova-conductor calls `GET /allocation_candidates` (SQL pre-filter) → reduces 10,000 hosts to ~100 candidates in < 50 ms; scheduler applies fine-grained filters on that subset (< 200 ms total)

### oslo.messaging Abstraction (RPC + Fanout)
- All 5 systems use oslo.messaging over RabbitMQ; unified retry/backoff/connection-pooling
- **RPC Call** (synchronous): `nova-conductor → nova-scheduler.select_destinations`; 60 s default timeout; dedicated reply queue per caller
- **RPC Cast** (async): `nova-api → nova-conductor.build_instances`; fire-and-forget; no reply
- **Fanout** (broadcast): `neutron-server → all OVS/OVN agents`; security group rule propagation; target < 3 s to all 10,000 hosts
- Retry: exponential backoff, `max_retries` configurable; heartbeat detection for dead consumers

### Soft-Delete Pattern (All Tables)
- All 5 systems mark records deleted with `deleted INT DEFAULT 0` or `deleted_at DATETIME`; records never physically removed during normal operation; GC process purges old rows
- Enables historical queries, audit, and crash recovery without losing referential integrity
- Common indexes: `(deleted, created_at)` on high-volume tables (instances, migrations)

## Common Databases

### MySQL 8.0 + Galera
- All 5; synchronous multi-master; wsrep_cluster_address lists all controller nodes; 5,000 TPS; zero data loss; total ~40 GB for all OpenStack services

### RabbitMQ (Quorum Queues)
- 4 of 5; 3-node Raft; 10,000–50,000 messages/sec; < 10 s leader election; quorum writes

### Memcached / Redis
- cloud_control_plane (explicit): < 1 ms cache hit; token + catalog caching; 4 GB per controller
- multi_region: Keystone proxy caches tokens/catalog locally per region

### Ceph RBD (Shared Storage)
- openstack_nova_compute + vm_live_migration: shared RBD images eliminate disk copy during live migration; same `rbd://pool/image` volume accessible from source and dest

## Common Communication Patterns

### Two-Phase Scheduling (SQL Pre-Filter + In-Process Filters)
- nova_compute + live_migration: Phase 1 — Placement API SQL `GET /allocation_candidates` narrows 10,000 hosts to ~100; Phase 2 — nova-scheduler applies AvailabilityZoneFilter, RamFilter, DiskFilter, NUMATopologyFilter, ServerGroupAntiAffinityFilter in < 200 ms total

### Cells v2 (Horizontal Scaling via Database Partitioning)
- cloud_control_plane + nova_compute: `nova_cell0` (failed instances), `nova_cell1`, `nova_cell2`; separate DB per cell; separate nova-conductor per cell; scales Nova beyond single-DB bottleneck; `nova_api` DB holds `cell_mappings` and `instance_mappings` for routing

## Common Scalability Techniques

### CPU/RAM Overcommit Ratios
- nova_compute: `cpu_allocation_ratio = 2.0` (256 pCPUs → 512 vCPUs available); `ram_allocation_ratio = 1.5` (128 GB → 192 GB available); `disk_allocation_ratio = 1.0` (no disk overcommit); GPU hosts: 1.0/1.0 (no overcommit)
- Placement API `inventories.allocation_ratio` column stores per-resource-class ratio; enforcement via `total × allocation_ratio - reserved - used`

### Distributed Control Plane (OVN, Nova Scheduler)
- neutron_network: OVN replaces per-host L3/DHCP/metadata agents with `ovn-controller` daemon; receives compiled logical flows from OVN Northbound DB via OVSDB; eliminates centralized agent bottleneck
- nova_compute: nova-scheduler stateless; multiple instances; no shared state; Placement API is the coordination point

## Common Deep Dive Questions

### How does Nova scheduler avoid querying all 10,000 hosts on every VM create?
Answer: Two-phase approach: (1) Placement API pre-filter — SQL `GET /allocation_candidates?resources=VCPU:4,MEMORY_MB:8192&required=HW_CPU_X86_AVX2` returns only hosts with sufficient inventory in < 50 ms; narrows candidates from 10,000 to ~100; (2) Nova scheduler applies fine-grained filters (NUMA topology, affinity groups, AZ, image properties) on the ~100 candidates in < 200 ms. Total scheduling decision: < 200 ms. Claims resources via `PUT /allocations/{consumer_id}` (optimistic, uses `resource_providers.generation` for conflict detection).
Present in: openstack_nova_compute_design, vm_live_migration_system

### How do you validate Keystone tokens without a database round-trip on every API call?
Answer: Fernet tokens are self-contained: encrypted payload contains token expiry, user/project/role data, signed with HMAC. Validation is pure local crypto — no DB lookup. The only cross-service call is on first validation (cache miss) via `oslo.middleware.auth_token`. Subsequent calls for the same token within the TTL window use Memcached (< 1 ms). In multi-region, Fernet key ring (30-min rotation cycle, 3 keys) is distributed to all regions so any region can validate tokens issued by global Keystone without any cross-region call.
Present in: cloud_control_plane_design, multi_region_cloud_platform

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **API Availability** | 99.99% (52 min/year); VRRP failover < 3 s |
| **API Latency P99** | < 500 ms; token validation < 5 ms (local) / < 50 ms (cache) |
| **Write Throughput** | 5,000 TPS (Galera), 10,000 msg/s (RabbitMQ), 2,000 API req/s |
| **VM Boot** | < 60 s end-to-end (API → SSH-accessible) |
| **Scheduler Decision** | < 200 ms for 10,000-host cluster |
| **Network Provisioning** | < 5 s port create → DHCP ready; < 3 s SG rule propagation |
| **Live Migration Downtime** | < 100 ms network cutover; < 500 ms total pause |
| **Scale** | 10,000 hosts / region; 400,000 VMs / region; 5 regions = 2M VMs total |
| **Storage** | 50 TB Glance / region; 2 PB Cinder / region; 100 Gbps DCI inter-region |
| **Recovery** | < 30 s single node failure; < 10 s RabbitMQ leader election |
