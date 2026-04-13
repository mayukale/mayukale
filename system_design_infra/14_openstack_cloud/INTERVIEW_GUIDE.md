# INTERVIEW GUIDE — Infra-14: OpenStack & Cloud

Reading Infra Pattern 14: OpenStack & Cloud — 5 problems, 7 shared components

---

## STEP 1 — PATTERN ORIENTATION

This pattern covers the infrastructure that makes a private cloud work. OpenStack is the dominant open-source IaaS platform — the same architectural DNA that AWS, GCP, and Azure use internally, just fully exposed. The five problems in this pattern each zoom in on a different layer or concern of a production cloud:

1. **Cloud Control Plane Design** — the nervous system: all API services, message bus, databases, and load balancing across controller nodes
2. **Multi-Region Cloud Platform** — geographic federation: shared identity, image replication, cross-region networking, and disaster recovery
3. **OpenStack Neutron Network Design** — tenant networking: VXLAN overlays, OVN distributed control, security groups, DVR
4. **OpenStack Nova Compute Design** — VM lifecycle: scheduling via Placement API, the conductor/scheduler/compute pipeline, and Cells v2 scaling
5. **VM Live Migration System** — zero-downtime VM movement: pre-copy iterative page transfer, post-copy for write-heavy VMs, network cutover via GARP

The **7 shared components** that appear across most or all five problems are: **MySQL/Galera** (synchronous multi-master state store), **RabbitMQ quorum queues** (async RPC bus), **Keystone Fernet tokens** (local-validation identity), **HAProxy + Keepalived** (VIP load balancing with < 3s failover), **Placement API** (resource provider inventory and scheduling pre-filter), **oslo.messaging** (RPC Call/Cast/Fanout abstraction), and the **soft-delete pattern** (all tables mark rows deleted rather than physically removing them).

When you see an OpenStack question in an interview, you are being tested on whether you understand distributed systems in the context of a stateful, multi-service infrastructure platform. The interviewer wants to know: can you reason about consistency (Galera certification failures), messaging reliability (RabbitMQ split-brain), scheduling at scale (Placement API pre-filtering), and zero-downtime operations (live migration, rolling upgrades)?

---

## STEP 2 — MENTAL MODEL

**Core idea:** A cloud control plane is a distributed operating system kernel for physical infrastructure. It turns REST API calls into reality on hardware — and the hard part is doing that reliably when any individual component (a DB node, a message broker node, a controller) can fail at any moment.

**Real-world analogy:** Think of an airport. There's a centralized dispatch center (the control plane — Nova, Neutron, Keystone, and friends running on controller nodes) that coordinates everything. Individual gates (compute hosts with nova-compute) operate semi-autonomously — they can handle a landing even if the dispatch center goes quiet for 30 seconds. But you need dispatch to book new flights, change gates, and track passenger manifests. The message bus (RabbitMQ) is the radio system between dispatch and the gates. The database (Galera cluster) is the official logbook — all three controller nodes hold identical copies and synchronize writes before acknowledging them. HAProxy is the front door of the airport: it doesn't matter which desk is open, your request gets routed to a working one.

**Why this is hard:**

- **Coordination at scale:** You have 10,000 compute hosts each sending heartbeats, resource updates, and RPC messages. Designing a message topology so RabbitMQ doesn't collapse under 30,000 queues (10,000 hosts × 3 queues each) requires real care.
- **Multi-service consistency:** When a VM is created, six services touch state in order: Keystone (auth), Placement (resource claim), nova-api DB, nova-conductor, nova-compute, Neutron (port bind), Glance (image download). Any failure mid-pipeline leaves orphaned allocations or stuck states.
- **Synchronous replication tradeoffs:** Galera writes are synchronous across all cluster members — which gives you zero data loss but also means write latency depends on the slowest node. A single slow controller can tank your entire API's write throughput.
- **Live migration impossibility framing:** Moving a running VM's memory between physical hosts without the guest noticing is fundamentally a race between how fast you can copy dirty pages and how fast the guest writes new ones. If the guest writes faster than you can transmit, you never converge — and that's a real scenario with database workloads.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

These four or five questions establish the scope and will dramatically change your architecture:

**Q1: How many compute hosts are we targeting?** The answer determines whether you need Cells v2 (mandatory beyond ~1,000 hosts to avoid a single DB bottleneck) and how many RabbitMQ queues you'll be managing. At 10,000 hosts, that's ~30,000 queues per cell — a very different sizing than 100 hosts.

**Q2: Are we designing a single region or a multi-region deployment?** A single region means a single Keystone, single set of controller nodes, and no cross-region replication concerns. Multi-region immediately raises questions about identity federation, global quotas, image replication, and DCI bandwidth budgeting.

**Q3: What is the primary network backend — OVN or legacy OVS agents?** This completely changes the Neutron architecture. OVN distributes L3 and DHCP logic to every compute host via ovn-controller, eliminating centralized network nodes. Legacy OVS requires dedicated network nodes running L3/DHCP/metadata agents.

**Q4: Do workloads require GPU passthrough, CPU pinning, or huge pages?** These require NUMA-aware scheduling and eliminate overcommit ratios, which changes scheduler complexity and Placement API resource class modeling substantially.

**Q5: What is the RTO/RPO requirement in the event of a full region failure?** RPO < 15 minutes means you need Ceph RBD mirroring, not just periodic snapshots. RTO < 30 minutes means automated failover with pre-staged DNS and pre-seeded images in the target region.

**What each answer changes:**
- Single region, 1,000 hosts, no GPU → simple 3-controller deployment, single Galera cluster, OVN recommended
- Multi-region, 50,000 hosts, GPU workloads → Cells v2, per-region Galera, Fernet key distribution, NUMA filters, dedicated placement resource classes for GPUs
- RTO < 5 minutes → you cannot rely on manual DNS failover; you need GSLB with automated health-check-based failover

**Red flags in the interview:**
- Candidate says "I'd use PostgreSQL instead of MySQL" without knowing that OpenStack's oslo.db and SQLAlchemy models are deeply tied to MySQL semantics and Galera only supports MySQL. ❌
- Candidate proposes active-active database writes without understanding Galera's certification-based replication and the performance impact of write-set conflicts. ❌
- Candidate conflates "highly available" with "geographically distributed" — an HA control plane within a single AZ is not the same as a DR-capable multi-region deployment. ❌

---

### 3b. Functional Requirements

**Core capabilities any OpenStack cloud must provide:**
- Authenticate and authorize all API requests via Keystone (Fernet tokens, RBAC policies)
- Create, manage, and delete virtual machine instances (Nova compute lifecycle)
- Provide isolated tenant networks with DHCP, routing, and floating IPs (Neutron networking)
- Persist all service state with zero data loss on single node failure (Galera-backed databases)
- Route all inter-service RPC calls asynchronously (RabbitMQ message bus)
- Present a unified HTTPS API surface with load balancing and health checking (HAProxy VIP)

**Scope clarification:**
- Are we designing the control plane only, or also the data plane (compute hosts, Ceph storage)?
- Does the system need to support live migration, or only cold migration?
- Is Heat orchestration (stacks) in scope, or just base compute/network/storage?

**Clear statement of the system:** You are building a distributed infrastructure platform that accepts declarative resource requests from users via REST APIs and reliably translates those into running virtual machines with network connectivity, persisting all state durably and exposing operations like live migration and multi-region failover.

---

### 3c. Non-Functional Requirements

The important NFRs to derive (and the trade-offs they force):

**API Availability: 99.99%** (< 52 minutes downtime per year)
- This requires ≥ 3 controller nodes (survive one failure with quorum), VRRP-based VIP failover < 3 seconds, and Galera synchronous replication.
- ✅ Synchronous Galera gives you zero data loss on failover
- ❌ Synchronous replication adds 5–20 ms write latency vs. async replication; the slowest node in the cluster determines write throughput

**API Latency P99 < 500 ms**
- Token validation must be local (Fernet, < 5 ms). If you're doing a Keystone DB round-trip on every API call, you will blow this budget.
- Memcached caches tokens after first validation (< 1 ms subsequent hits)
- ❌ Every additional hop (cross-region Keystone call, slow Galera write under flow control) adds latency that compounds across multi-service requests

**Scheduling Throughput: 500+ decisions/second**
- This forces the two-phase scheduling design: Placement API SQL pre-filter reduces 10,000 hosts to ~100 candidates in < 50 ms, then nova-scheduler applies fine-grained filters on the small candidate set in < 200 ms total
- ❌ Without Placement API pre-filtering, the scheduler would need to evaluate all 10,000 hosts in Python — this was the pre-Placement bottleneck that caused Nova scaling problems pre-Pike release

**Live Migration Downtime < 100 ms (network cutover) / < 500 ms (total VM pause)**
- This is only achievable with shared storage (Ceph RBD) so disk is not migrated
- Requires pre-copy convergence: dirty page rate < migration bandwidth × convergence threshold
- ❌ For write-heavy VMs (dirty rate > bandwidth), pre-copy will not converge — requiring either auto-converge (CPU throttling with performance impact) or post-copy (risk of data loss if source dies)

**Scale: 10,000 hosts / region, 400,000 VMs / region**
- This forces Cells v2: separate DB and conductor per cell prevents any single MySQL instance from becoming a bottleneck
- RabbitMQ sizing: 30,000 queues × 30 KB = ~900 MB per cell — must size broker RAM accordingly

---

### 3d. Capacity Estimation

**The formula you need to know for a 10,000-host region:**

```
API throughput:     5,000 req/s (Nova: 2,000 + Keystone: 1,000 + Neutron: 500 + other: 1,500)
DB throughput:      5,000 TPS across all service databases
MQ throughput:      10,000 messages/s (all RPC combined)
Concurrent conns:   10,000 (HAProxy fronting all services)
```

**Anchor numbers to memorize:**

| Resource | Number |
|---|---|
| Controller nodes | 5 (survive 2 failures) |
| DB size (all services) | ~40 GB total across all OpenStack DBs |
| Largest DB | nova_cell DBs, ~30 GB total for cells 0–2 |
| Keystone DB | ~500 MB (small — tokens are ephemeral Fernet, not stored) |
| RabbitMQ queues at 10K hosts | ~30,000 per cell; ~900 MB RAM overhead |
| Memcached per controller | 4 GB (token + catalog cache) |
| VM boot target | < 60 seconds end-to-end (API to SSH-accessible) |
| Scheduler decision | < 200 ms (10K host cluster) |
| Live migration (8 GB RAM VM, shared storage) | ~10 seconds total; ~18 ms downtime |
| Live migration (256 GB RAM, write-heavy VM) | Does not converge without auto-converge or post-copy |
| Network port provisioning | < 5 seconds (port create to DHCP-ready) |
| Security group rule propagation | < 3 seconds to all 10,000 hosts |

**Migration bandwidth math (walk through this in an interview):**
```
VM RAM: 8 GB
Migration link: 8 Gbps = 1 GB/s
Dirty rate (typical active workload): 200 MB/s

Iteration 1: Transfer 8 GB    → 8.0s  (dirties 1.6 GB)
Iteration 2: Transfer 1.6 GB  → 1.6s  (dirties 320 MB)
Iteration 3: Transfer 320 MB  → 0.32s (dirties 64 MB)
Iteration 4: Transfer 64 MB   → 0.06s (dirties 12.8 MB)
Final pause: transfer 12.8 MB → 0.013s

Convergence check: 200 MB/s < 1,000 MB/s × 0.3 = 300 → CONVERGES
Total: ~10s; downtime: ~18 ms
```

**Architecture implications of your estimates:**
- 40 GB of total DB data → fit on NVMe SSDs on controller nodes, no separate storage tier needed
- 30,000 RabbitMQ queues → need 3-node RabbitMQ cluster with 64 GB RAM each; monitor queue depth per host
- 5,000 TPS → Galera synchronous replication is fine; need to watch wsrep_flow_control_paused (should stay near 0)
- 10,000 concurrent API connections → HAProxy must be tuned with sufficient `maxconn` and worker processes

**Time budget for capacity in an interview:** 4–6 minutes. Do the VM migration math, quote the DB size, and explain why 5 controller nodes vs. 3.

---

### 3e. High-Level Design

**The 5 core components to always draw first:**

1. **HAProxy + Keepalived (VIP layer)** — single entry point for all API traffic; VRRP failover < 3s; SSL termination; health checks on backends; rate limiting to protect the control plane

2. **API Services Layer (stateless)** — Keystone, Nova-API, Neutron-server, Cinder-API, Glance-API, Placement-API, Heat-API, Barbican-API all run active-active behind HAProxy; each is stateless (state lives in Galera); N instances per service for HA

3. **RabbitMQ Cluster (quorum queues)** — 3-node Raft cluster; all inter-service RPC flows through here; topic exchanges per service (nova, neutron, cinder); routing key per target (conductor, scheduler, compute.hostname); quorum queues require ACK from majority before confirming write; failover < 10s

4. **MySQL/Galera Cluster** — 3 or 5 nodes; synchronous multi-master; certification-based replication; all OpenStack services have separate databases on the same cluster; ~40 GB total; write latency 5–20 ms; read from any node; Galera flow control is your main operational concern at high write rates

5. **Worker Services (stateful consumers)** — nova-conductor, nova-scheduler, heat-engine; these are RabbitMQ consumers; multiple instances for HA; nova-scheduler is stateless (no shared state beyond what it reads from Placement API); nova-conductor mediates DB access for nova-compute

**Data flow for "create VM" (whiteboard this end-to-end):**
```
Client → External LB → HAProxy VIP → nova-api (one of N)
nova-api: validate Fernet token (local, < 5ms) → check RBAC →
          write instance record to nova_api DB →
          RPC Cast to nova-conductor
nova-conductor: GET /placement/allocation_candidates →
                RPC Call to nova-scheduler → select_destinations
nova-scheduler: evaluate ~100 candidates from Placement →
                PUT /placement/allocations (claim resources) →
                return selected host
nova-conductor: update cell DB, RPC Cast to nova-compute.<host>
nova-compute: download image from Glance →
              POST /v2.0/ports to Neutron (port binding) →
              libvirt/KVM creates VM → update cell DB via conductor
```

**Key decisions to mention:**
- Cells v2 for scaling: why the nova_api DB is separate from cell DBs, and how cell_mappings routes requests to the right cell DB + conductor
- Fernet tokens: explain why you never store tokens in the database (they're self-contained encrypted payloads), and why that matters for performance
- Placement API as the scheduling pre-filter: explain why you can't do scheduler decisions on all 10,000 hosts in the Python process

**Whiteboard order:** Draw the VIP/HAProxy first (it's the entry point the interviewer can follow). Then draw the 3 controller nodes each with API services, RabbitMQ node, and Galera node. Then draw compute hosts with nova-compute. Draw arrows showing the VM create flow. Then zoom into one component when the interviewer prompts.

---

### 3f. Deep Dive Areas

**Area 1: Galera Cluster — Certification-Based Replication and Failure Modes**

This is the most common deep-dive because it's the least intuitive part of the control plane.

**The problem:** You need a database that survives controller node failures without losing data, without complex primary/replica promotion, and without downtime.

**The solution:** Galera uses synchronous multi-master replication with write-set certification. When you write to any Galera node, that write-set (the set of rows modified) is broadcast to all other nodes before the transaction commits. Each node independently certifies the write-set against its recent transaction history to detect conflicts. If no conflict, all nodes commit. If a conflict exists, the losing transaction is rolled back.

**Key behaviors to explain:**
- All nodes accept reads AND writes (active-active, unlike traditional primary-replica)
- Write latency = local commit time + network RTT to slowest node + certification time (~5 ms on local network)
- wsrep_flow_control_paused: if one node falls behind, Galera pauses all writes across the cluster. This metric being > 0 means you have a performance problem on one node.
- On node failure: cluster continues if strict majority remains (quorum). 3-node cluster survives 1 failure. 5-node cluster survives 2 failures.
- On network partition: the minority partition sets itself to non-primary state and refuses writes. This prevents split-brain.

**Unprompted trade-offs you should volunteer:**
- ✅ Zero data loss on single node failure (synchronous)
- ✅ Any node can serve reads without replica lag
- ❌ Write-heavy workloads can trigger flow control, stalling the entire cluster
- ❌ Galera certification conflicts increase with high-concurrency writes to the same rows (workaround: use optimistic locking at the application layer, or separate heavily-written tables)
- ❌ Adding a new node requires SST (State Snapshot Transfer) which blocks the donor node — schedule this during maintenance windows

---

**Area 2: Nova Scheduler + Placement API — Two-Phase Scheduling**

Interviewers love this because it illustrates how to scale a filtering problem from O(N) to O(1).

**The problem:** You have 10,000 compute hosts. For every VM create request (up to 150/sec at peak), the scheduler needs to find a host with enough vCPU, RAM, disk, matching availability zone, anti-affinity group, NUMA topology, etc. Doing all that in a Python loop over 10,000 hosts takes seconds — unacceptable.

**The solution:** Two-phase approach.

Phase 1 (SQL, < 50 ms): The Placement API executes a single optimized SQL query against resource_providers, inventories, allocations, and traits tables. The query filters on quantitative resources (VCPU: 4, MEMORY_MB: 8192) and required traits (HW_CPU_X86_AVX2, COMPUTE_STATUS_DISABLED=absent). This returns ~100 candidate hosts. The SQL is efficient because Placement is designed specifically for this — it's not a general-purpose database query.

Phase 2 (In-process filters, < 150 ms additional): nova-scheduler applies finer-grained Python filters on the ~100 candidates: AvailabilityZoneFilter, ServerGroupAntiAffinityFilter, NUMATopologyFilter (complex NUMA cell matching), ImagePropertiesFilter (checks CPU hardware capabilities against image metadata). This is fast because the candidate set is tiny.

Resource claiming: After selecting a host, the scheduler calls `PUT /placement/allocations/{consumer_id}`. This uses optimistic locking via the `generation` field on resource_providers — if two schedulers both try to claim the same host's last vCPU simultaneously, one will get a 409 Conflict and retry with a new candidate. This is how Nova avoids double-booking without distributed locks.

**Unprompted trade-offs:**
- ✅ Scales to 10,000+ hosts without degradation
- ✅ Placement API is a separate service — can scale it independently if scheduling load is extreme
- ❌ The `generation` optimistic lock can cause retry storms during burst VM creation — mitigated by running multiple scheduler instances that spread the load
- ❌ Placement API is authoritative for resource tracking — if a nova-compute heartbeat fails to update Placement, you get phantom resources; nova provides the `nova-manage placement heal_allocations` command to reconcile

---

**Area 3: VM Live Migration — Pre-Copy Convergence and Post-Copy**

This is the deepest technical topic in the pattern and signals senior/staff readiness.

**The problem:** Move a running VM from host A to host B with < 100 ms network downtime. The VM's memory is constantly changing (dirty pages). You can't just pause it and copy — that would take 8 seconds for an 8 GB VM and much longer for large VMs.

**The solution (pre-copy, default approach):** Iteratively copy memory pages while the VM runs. Track which pages get dirtied after the initial copy. Re-copy dirty pages. Repeat until the remaining dirty data is small enough that you can pause the VM, copy the final residual, and resume on the destination before the 100 ms threshold is hit.

**Convergence condition:** `dirty_rate < migration_bandwidth × convergence_threshold`
- A typical 8 GB VM with a 200 MB/s dirty rate on an 8 Gbps link: converges after ~5 iterations, ~10s total, ~18 ms downtime. ✅
- A 256 GB database VM writing at 2 GB/s on a 1 GB/s link: dirty_rate > bandwidth → never converges. ❌

**For non-converging VMs, three options:**

1. **Auto-converge:** Throttle the VM's vCPUs progressively (20% → 40% → 60%) until its dirty rate drops below the migration bandwidth. This eventually works but degrades the VM's performance during migration — bad for latency-sensitive workloads.

2. **Post-copy:** Immediately pause the VM, transfer CPU registers and device state to the destination (< 100 ms for small state), resume execution on the destination. Pages are demand-paged from the source via userfaultfd as the destination VM accesses them. Risk: if the source host dies while pages are still split between source and destination, the VM is lost. Use only for VMs where brief unavailability is preferable to degraded performance during migration.

3. **XBZRLE compression:** XOR-based run-length encoding of dirty pages achieves ~3:1 compression. Effectively triples the useful transfer bandwidth. For the 256 GB VM example: 1 GB/s × 3 = 3 GB/s effective > 2 GB/s dirty rate → converges.

**Network cutover sequence (this is the actual < 100 ms step):**
1. Pause VM on source
2. Transfer final dirty pages + CPU registers
3. Nova calls Neutron: `PUT /v2.0/ports/{port_id}` with `binding:host_id = dest_host`
4. OVN Southbound DB updates port binding
5. ovn-controller on destination installs OpenFlow rules and sends GARP (Gratuitous ARP)
6. Physical switches learn new MAC location from GARP
7. Resume VM on destination
Total: 2–10 ms for network cutover.

**Unprompted trade-offs:**
- ✅ Pre-copy is safe: source VM is still running if anything fails; easy rollback
- ❌ Pre-copy takes longer for large RAM VMs (proportional to RAM, not just working set)
- ✅ Post-copy is fast for large RAM VMs (only transfers working set)
- ❌ Post-copy: source failure = VM loss; every page fault during post-copy adds latency via userfaultfd
- ✅ Ceph RBD shared storage eliminates disk migration entirely (same RBD image accessible from both hosts)
- ❌ Local disk VMs require block migration which is 100× slower (100 GB disk at 500 MB/s = 200s vs. 10s for shared storage)

---

### 3g. Failure Scenarios

**Scenario 1: RabbitMQ node failure during a VM create in flight**

A nova-conductor has sent a build_instances RPC cast. The RabbitMQ node holding the queue dies mid-delivery.

With quorum queues (Raft): the queue leader election completes in < 10s. Messages that were acknowledged by a majority before the node died are preserved on surviving nodes. Messages that were not yet acknowledged are re-delivered. nova-conductor's oslo.messaging layer will detect the connection failure via heartbeat timeout and reconnect to the cluster. The VM create operation will either complete (if the message was delivered) or be retried by conductor (with idempotent logic).

**Senior framing:** "I'd ask how we handle the case where nova-compute received the build message and already created the VM when the message was redelivered. Nova uses task_state tracking in the instance table — if the instance is already BUILDING or ACTIVE, conductor ignores the duplicate."

---

**Scenario 2: Galera minority partition (network split across controller nodes)**

If 2 of 5 controller nodes can no longer reach the other 3, the minority partition sets wsrep_cluster_status to "Non-primary" and refuses all write operations. This is by design — it prevents split-brain writes that would diverge the state of the two partitions.

**Senior framing:** "The failure here isn't the DB refusing writes — that's correct behavior. The failure mode is the Pacemaker/Corosync cluster not having fenced the partitioned nodes. If a partitioned node still has the VIP and accepts API requests but can't write to DB, you get 500 errors appearing externally while the cluster is actually healthy on the majority side. The fix is proper STONITH fencing policy that ensures the VIP moves off nodes that lose Galera quorum."

---

**Scenario 3: Live migration — source host fails during pre-copy**

The source nova-compute dies while memory pages are being copied to the destination. The destination VM is in PAUSED state and has a partial copy of memory.

nova-conductor's LiveMigrationTask detects the failure via RPC timeout. It rolls back: the destination VM is deleted from libvirt, Placement allocations on the destination are released, the instance record stays ACTIVE on the source (which is now down), and the instance state is set to ERROR for operator review. There is no automatic failover — the operator must evacuate the instance to a working host.

**Senior framing:** "This is why evacuation and live migration are separate code paths in Nova. Evacuation is for when the source is known dead. Live migration assumes the source is healthy throughout. If you want transparent recovery, you need application-level clustering (e.g., Pacemaker inside the VM) or a shared-nothing architecture."

---

**Scenario 4: Galera flow control triggered by a write spike**

wsrep_flow_control_paused rises to 0.4 (40% of time paused). All writes across all OpenStack services slow to a crawl. API requests start timing out.

**Root cause:** One Galera node is slower (CPU contention, I/O saturation, GC pause). The other nodes broadcast write-sets faster than it can certify them, so it signals flow control.

**Mitigation:** Isolate the slow node (remove it from the wsrep_cluster_address on the others), investigate the bottleneck, fix it (usually disk I/O — upgrade to NVMe or reduce write load), then rejoin. During the degraded period, temporarily redirect writes to the healthy 2 nodes (still achieves quorum). Longer term: place Galera on dedicated NVMe nodes separate from API services.

---

## STEP 4 — COMMON COMPONENTS

These 7 components appear across most or all five problems. Know each one cold.

---

### MySQL + Galera Cluster

**Why it's used:** OpenStack was designed for relational data. All service state (instances, ports, networks, volumes, images, secrets, stacks, resource providers, allocations) is highly relational with foreign keys and transactional semantics. MySQL + Galera provides synchronous multi-master replication (no primary/replica promotion needed on failover), which is the correct choice when you cannot tolerate any data loss on controller node failure.

**Key configuration:**
```ini
wsrep_cluster_name = openstack-galera
wsrep_cluster_address = gcomm://controller-1,controller-2,controller-3
wsrep_provider = /usr/lib/galera/libgalera_smm.so
wsrep_sst_method = rsync  # or mariabackup for production
```
Each OpenStack service gets its own database on the shared cluster: keystone (~500 MB), nova_api (~1 GB), nova_cell1/2 (~30 GB combined), neutron (~2 GB), cinder (~1 GB), placement (~500 MB), glance (~200 MB), heat (~500 MB), barbican (~200 MB). Total: ~40 GB.

The critical monitoring metric is **wsrep_flow_control_paused** (should be 0.0; anything above 0.05 indicates a lagging node). Also watch **wsrep_cluster_size** (should equal your node count; drop means node left cluster) and **wsrep_local_state** (4 = synced, which is healthy; 2 = donor, 1 = joiner both mean degraded).

**What you lose without it:** Without Galera (using traditional primary-replica), any controller node failure requires manual or automated primary promotion. During promotion, there's a window where no writes are accepted (typically 30–60 seconds), which violates your 99.99% availability SLA. You also risk losing the last few seconds of commits if the replica was lagging.

---

### RabbitMQ Quorum Queues (Raft Consensus)

**Why it's used:** OpenStack services communicate via asynchronous RPC because the work (booting a VM, binding a port, creating a volume snapshot) happens on different hosts than where the API request arrives. RabbitMQ provides the message durability, routing, and delivery guarantees that make this safe. Quorum queues (introduced in RabbitMQ 3.8) replaced the old "classic mirrored queues" and use Raft consensus for queue leadership, which is fundamentally more reliable (classic mirrored queues had a known split-brain bug).

**Key configuration:** Topic exchange per service (`nova`, `neutron`, `cinder`). Routing keys direct messages to specific consumers: `conductor`, `scheduler`, `compute.hostname-001`, etc. Each compute host has ~3 queues (compute, compute.<hostname>, and a reply queue for synchronous RPC calls). At 10,000 hosts, that's ~30,000 queues per cell.

oslo.messaging wraps RabbitMQ with two patterns:
- **RPC Call** (synchronous): nova-conductor → nova-scheduler `select_destinations`. Uses a dedicated reply queue. 60-second default timeout.
- **RPC Cast** (async, fire-and-forget): nova-api → nova-conductor `build_instances`. No reply expected.
- **Fanout** (broadcast): neutron-server → all OVS/OVN agents for security group rule updates. Target: < 3 seconds to all 10,000 hosts.

**What you lose without it:** Without a message bus, every nova-api instance would need a direct TCP connection to every nova-compute host to dispatch work. At 10,000 hosts that's 10,000 × N connections from N API instances — combinatorially unscalable. The message bus decouples producers from consumers.

---

### Keystone Fernet Tokens

**Why it's used:** Authentication on the hot path (every API request, every middleware check) must be fast. Fernet tokens solve this by being self-contained encrypted payloads — validation is pure local cryptography (HMAC verify + decrypt), no database lookup required. A Fernet-validated token takes < 5 ms. A database-backed UUID token required a SELECT against the tokens table on every request — that's 5,000 DB queries per second just for token validation on a busy control plane, completely impractical.

**Key configuration:** Three-stage key ring in `/etc/keystone/fernet-keys/`:
- Key `0`: staged (will become primary on next rotation)
- Key `1`: secondary (validates tokens signed by old primary — allows graceful rotation)
- Key `2`: primary (signs new tokens)

Keys rotate every 30 minutes. In multi-region deployments, the key repository must be synchronized to all regions before rotation completes — otherwise, a region with the old key ring cannot validate tokens signed with the new primary. Typical sync mechanism: Ansible/Puppet/Salt push to all controllers before calling `keystone-manage fernet_rotate`.

After the first validation of a token, the result is cached in Memcached with the token's TTL. Subsequent calls from the same token hit Memcached in < 1 ms. Every API service validates tokens via `oslo.middleware.auth_token` — no Keystone network call on the hot path.

**What you lose without it:** UUID tokens required a database row per token and a SELECT on every API call. This made Keystone a write-heavy database service that scaled poorly. Fernet moves token validation entirely out of the database, making Keystone's DB ~500 MB and reads-only for catalog/policy data.

---

### HAProxy + Keepalived

**Why it's used:** All OpenStack API services run as multiple stateless instances (typically N instances per service per controller, where N = vCPU count / 4). You need something in front of them to load balance, health check, terminate SSL, and provide a stable VIP (Virtual IP) that doesn't change when a controller node fails. HAProxy is the de facto standard for this in OpenStack deployments. Keepalived provides VRRP-based VIP management so the VIP floats to a healthy HAProxy instance if the current holder fails.

**Key configuration:**
- VIP managed by keepalived: one controller holds the VIP at a time (active-passive VIP); all HAProxy instances route traffic if you place an external LB in front
- VRRP failover time: < 3 seconds (VRRP hello interval: 1s; dead interval: 3s)
- HAProxy backend health checks: HTTP check on each API service's `/healthcheck` endpoint every 5 seconds; mark DOWN after 3 consecutive failures
- Connection pooling: `maxconn` set based on worker count per service (typically 4–8 workers per API instance × 3 instances = 12–24 backend workers per service)
- Rate limiting: `http-request track-sc0 src table` + `http-request deny deny_status 429 if` — protects against noisy tenants hammering the API

**What you lose without it:** Without HAProxy, clients would need to know individual controller node IPs and implement their own retry/failover logic. Without Keepalived, the VIP is a SPOF — if the controller holding the VIP fails, the IP is gone until manual intervention.

---

### Placement API

**Why it's used:** Nova needed a way to track resource inventories (how many vCPUs, GB of RAM, GB of disk each compute host has available) and resource allocations (which VMs are consuming which resources on which host) that was both accurate and queryable efficiently. The Placement API is that service — a purpose-built resource accounting system. Its SQL schema is designed specifically for `GET /allocation_candidates` — a query that returns hosts meeting resource requirements — which is the core of the scheduler's pre-filtering step.

**Key schema concepts:**
- `resource_providers`: one row per compute host (or per NUMA node, or per GPU — nested providers are supported via `parent_provider_uuid`)
- `inventories`: one row per (resource_provider, resource_class) pair. Stores `total`, `reserved`, `allocation_ratio` (the overcommit multiplier). Available = `total × allocation_ratio - reserved - SUM(allocations.used)`
- `allocations`: one row per (consumer, resource_provider, resource_class). Consumer is typically an instance UUID.
- `traits`: boolean capabilities per resource provider. Examples: `HW_CPU_X86_AVX2`, `COMPUTE_STATUS_DISABLED`, `CUSTOM_NUMA_PASSTHROUGH`.
- `generation`: optimistic lock on each resource_provider. The `PUT /allocations` endpoint uses this for conflict detection — if two schedulers try to claim the last resource on the same host simultaneously, one gets a 409 Conflict and retries with a different candidate.

**Default overcommit ratios:**
- `cpu_allocation_ratio = 2.0` → 256 physical CPUs = 512 vCPUs available
- `ram_allocation_ratio = 1.5` → 128 GB physical = 192 GB vCPUs available  
- `disk_allocation_ratio = 1.0` → no overcommit (disk is not elastic)
- GPU hosts: all ratios = 1.0 (no overcommit ever)

**What you lose without it:** Before Placement (pre-Nova Pike release), the scheduler queried `compute_nodes` for all hosts on every scheduling decision. At 10,000 hosts with 500 requests/second, this was 5 million SQL rows scanned per second just for scheduling — completely unscalable. Placement's SQL pre-filter reduces this to ~100 candidates per scheduling decision.

---

### oslo.messaging Abstraction

**Why it's used:** oslo.messaging is the OpenStack RPC library that sits on top of RabbitMQ (or other transports, though RabbitMQ is universal in production). It provides a consistent API for RPC Call, RPC Cast, and Fanout patterns across all OpenStack services, with built-in retry logic, connection pooling, heartbeat detection, and timeout handling.

**Key patterns:**
- **RPC Call**: `rpc.call(context, target, 'select_destinations', args)` — blocks until response or timeout (60s default). Used for nova-conductor → nova-scheduler (needs result). Uses a dedicated reply queue per caller.
- **RPC Cast**: `rpc.cast(context, target, 'build_instances', args)` — fire-and-forget; caller does not wait. Used for nova-api → nova-conductor (202 Accepted returned to client immediately).
- **Fanout**: `rpc.fanout_cast(context, target, 'security_group_rules_for_devices', args)` — broadcasts to ALL consumers of a topic. Used for security group rule propagation to all compute hosts simultaneously.

**What you lose without it:** Direct RabbitMQ usage requires each service to implement its own connection pooling, retry logic, message serialization, routing key conventions, and heartbeat handling. oslo.messaging standardizes this so all OpenStack services behave consistently. Debugging is also much easier because oslo.messaging logs follow a uniform format.

---

### Soft-Delete Pattern

**Why it's used:** No OpenStack service physically deletes rows during normal operation. Instances, ports, volumes, images all get a `deleted = 1` flag (or `deleted_at DATETIME`) set when the resource is destroyed. Physical deletion happens via a separate archival/purge process (`nova-manage db archive_deleted_rows`, `neutron-db-manage purge`). This pattern serves several purposes: crash recovery (if nova-compute dies after deleting the libvirt domain but before Neutron releases the port, you can reconcile against the deleted-but-not-purged records), audit trails (compliance requires history), and referential integrity during multi-step operations.

**Key configuration:** All high-volume tables index on `(deleted, created_at)` so live queries (`WHERE deleted=0`) are efficient even when the table has millions of soft-deleted rows.

**What you lose without it:** Physical deletes in a distributed system where multiple services hold references to the same resource UUID are dangerous. If nova deletes an instance row while a concurrent Neutron RPC is still using the instance_uuid as a foreign key reference in the port table, you get constraint violations or orphaned port records. Soft-delete keeps the reference valid until all services have finished their work.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Cloud Control Plane Design

**What makes this problem unique:**
This is the "whole system at once" problem — you're designing the shared infrastructure that all other services depend on. The unique concern is running 10 different API services (Keystone, Nova, Neutron, Cinder, Glance, Placement, Heat, Barbican, Octavia, Designate) behind a single HAProxy VIP while keeping their databases isolated. The key architectural tension here is between **co-location** (all services on 3–5 controller nodes, simpler, cost-efficient) vs. **disaggregation** (separate DB nodes, separate MQ nodes, separate API nodes — better isolation, harder to operate).

The RabbitMQ exchange layout is specific to this problem: you must know that each service gets its own topic exchange (`nova`, `neutron`, `cinder`) and that compute host queues scale linearly with the fleet size.

**Differentiator in two sentences:** The Cloud Control Plane problem is about orchestrating the entire API surface as a single HA unit — every design decision (VIP failover, Galera sizing, RabbitMQ topology) must account for the combined load of all 10 services simultaneously. The key uniqueness vs. the other problems is the multi-service coordination and the Pacemaker/Corosync cluster resource management (fencing, STONITH) that ensures VIPs only live on healthy nodes.

---

### Multi-Region Cloud Platform

**What makes this problem unique:**
This is the only problem where you have a globally shared service (Keystone) and per-region independent services. The hard problems here are: (1) **Fernet key distribution** — keys rotate every 30 minutes and must be synced to all regions before rotation, otherwise cross-region token validation breaks; (2) **global quotas** — a user could create 400K VMs across 5 regions if per-region quotas are independent; (3) **image replication** — you don't want every new region to pull a 10 GB image over the DCI on every VM create; (4) **DR failover** — automated DNS + Ceph RBD mirror promotion sequence.

The single global Keystone is a deliberate design choice with a clear trade-off: token issuance requires a cross-region call (~80 ms to US-East from EU-West), but token validation is local (< 5 ms) because any region can decrypt the Fernet token using the shared key ring. The per-region Keystone proxy caches the catalog and tokens locally to absorb the auth burst without cross-region calls.

**Differentiator in two sentences:** The Multi-Region problem is uniquely about **federated identity with local validation performance** and **cross-region data consistency trade-offs** — specifically, what you centralize (Keystone, global quotas) vs. what you replicate (images) vs. what you partition (Nova, Neutron, Cinder, Glance DBs with their own Galera clusters). Unlike the single-region problems, the failure modes here are geographic: region-wide outages, DCI link failures, and the choice of RPO/RTO targets drives your replication strategy.

---

### OpenStack Neutron Network Design

**What makes this problem unique:**
Neutron is the most architecturally complex OpenStack service because it bridges the control plane (the API, ML2 plugin, database) with the actual data plane (OVS/OVN flows on every compute host). The unique tension is between **centralized network nodes** (traditional OVS model with dedicated L3/DHCP/metadata agents) vs. **distributed virtual routing with OVN** (no network nodes; every compute host runs ovn-controller and performs local L3/DHCP).

The OVN architecture eliminates the centralized L3 bottleneck: in the old OVS model, all tenant east-west and north-south traffic would be hairpinned through 3 network nodes, each of which was an SPOF and bandwidth bottleneck. With OVN, east-west traffic between VMs on different hosts goes directly host-to-host via VXLAN without touching a network node. The OVN control plane (Northbound DB → compile logical flows → Southbound DB → ovn-controller on every host) uses OVSDB Raft replication for the control databases.

The security group propagation challenge is also unique: 250,000 rules need to reach 10,000 hosts in < 3 seconds on any rule change. OVN's ovn-controller listens to incremental Southbound DB changes and only updates affected OpenFlow entries — this is fundamentally different from the old OVS model which would broadcast the full security group state to affected agents.

**Differentiator in two sentences:** The Neutron problem is uniquely about **ML2 plugin composition** (type drivers × mechanism drivers) and the decision between centralized agent topology vs. OVN's distributed model — a choice that determines whether your network scales linearly with compute hosts or requires dedicated network infrastructure. The hard uniqueness is security group rule propagation at scale (250K rules, 10K hosts, < 3s) and VXLAN VNI allocation management for 20,000 tenant networks.

---

### OpenStack Nova Compute Design

**What makes this problem unique:**
Nova's uniqueness is the **Cells v2 horizontal scaling architecture** and the **nova-conductor security boundary**. Cells v2 is not optional at scale — beyond ~1,000 compute hosts, a single nova DB becomes a bottleneck. The solution: separate the nova_api database (global: flavors, cell_mappings, instance_mappings) from per-cell databases (nova_cell1, nova_cell2: instances, migrations, compute_nodes). Each cell has its own conductor and its own RabbitMQ cluster, so one cell's DB or MQ failure does not affect other cells.

nova-conductor's role is often misunderstood: it's not just a message router — it's the **security boundary** that prevents nova-compute (running on untrusted hypervisor hosts that could be compromised) from having direct database credentials. All DB writes from nova-compute go through conductor via RPC. This means a compromised nova-compute daemon cannot directly corrupt the instance database.

The scheduling stack (Placement API + nova-scheduler filters) is also specific to this problem: the filter list, the overcommit ratios, and the `generation`-based optimistic locking for concurrent allocation claims.

**Differentiator in two sentences:** The Nova Compute problem is uniquely about **VM lifecycle state management at 10,000-host scale** via Cells v2 database partitioning and the nova-conductor security boundary — the design ensures neither a single DB bottleneck nor a compromised hypervisor host can compromise the integrity of the resource accounting system. The two-phase scheduler (Placement SQL pre-filter + Python filter pipeline) is the core architectural innovation that makes sub-200ms scheduling decisions on a 10,000-host fleet.

---

### VM Live Migration System

**What makes this problem unique:**
This is the most **algorithm-centric** design problem in the pattern. The core of the design is the pre-copy iterative convergence algorithm and the decision tree for handling non-convergent VMs (auto-converge CPU throttling vs. post-copy demand paging vs. XBZRLE compression). No other problem in this set requires you to reason about memory bandwidth, dirty page rates, and convergence math.

The unique architecture decision is the **migration strategy chooser**: given a VM's memory size, estimated dirty rate, and configured migration bandwidth, which strategy do you pick? Pre-copy is safe but time-bounded by convergence. Post-copy is fast for large VMs but has the critical risk that source host failure = VM loss. Auto-converge guarantees convergence but degrades the VM's CPU performance during migration. XBZRLE is transparent to the VM but requires CPU for compression and a cache in DRAM.

The network cutover sequence (Neutron port rebinding → OVN Southbound DB update → ovn-controller GARP → switch MAC table update) is also specific to this problem and must happen in < 10 ms after the VM is paused.

**Differentiator in two sentences:** The VM Live Migration problem is uniquely about **iterative memory transfer convergence** — a race between your transfer bandwidth and the guest's dirty page rate — and the architectural trade-offs between pre-copy (safe, slower for write-heavy VMs), post-copy (fast, dangerous on source failure), and hybrid approaches (XBZRLE compression, auto-converge CPU throttling). The network cutover is a separate hard constraint (< 100 ms) that requires Neutron port rebinding, OVN flow reprogramming, and GARP to happen atomically from the network's perspective.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions

**Q: What is the role of Keystone in OpenStack and why do you need it?**

**Keystone is the identity service** — it authenticates users, issues Fernet tokens, manages projects/domains/roles, and maintains the service catalog. Every API request to any OpenStack service starts with the client presenting a Keystone token, which is validated locally using Fernet cryptography. Without Keystone, you'd have no unified authentication and no way for Nova to know which user is calling it or what they're allowed to do. The service catalog is also critical — clients use it to discover the URL for Nova in each region (e.g., `nova.us-east.cloud.example.com/v2.1`) rather than hardcoding endpoints.

---

**Q: What is Galera Cluster and why does OpenStack use it instead of standard MySQL replication?**

**Galera provides synchronous multi-master replication** using write-set certification, so any node can accept writes and all nodes are always in sync. Standard MySQL replication (primary-replica) is asynchronous — the replica can lag behind the primary, and on primary failure you risk losing the last few seconds of commits. OpenStack's 99.99% availability requirement means zero data loss on controller node failure — only synchronous replication delivers this. Galera also eliminates the need for primary promotion logic since all nodes are equal peers, simplifying failover to just removing the failed node from the cluster.

---

**Q: What is the difference between RPC Call and RPC Cast in oslo.messaging?**

An **RPC Call** is synchronous — the caller blocks waiting for a response until timeout (default 60 seconds). Nova-conductor uses this when calling nova-scheduler's `select_destinations` because it needs the scheduling result before it can proceed. An **RPC Cast** is asynchronous, fire-and-forget — the caller does not wait for any response. Nova-api uses this when calling nova-conductor's `build_instances` because it has already returned a 202 Accepted to the client and doesn't need to wait for the VM to actually boot. Using Cast where possible reduces latency and avoids thread blocking on the API tier.

---

**Q: Why does OpenStack need Cells v2 and what does it actually solve?**

**Cells v2 is a database partitioning scheme** for Nova that allows you to scale beyond the limits of a single MySQL database. Without Cells v2, all 400,000 instance records, all 10,000 compute node heartbeats, and all migration state for a 10,000-host deployment live in one database. At scale, that single DB becomes a write throughput bottleneck. Cells v2 splits this into a global `nova_api` database (holds flavors, instance_mappings, cell_mappings — small and rarely written) and per-cell databases (`nova_cell1`, `nova_cell2` — each with a separate Galera cluster and conductor). Routing from nova_api to the right cell happens via the `cell_mappings` table which maps instance UUIDs to their cell.

---

**Q: What is the Placement API and what was Nova doing before it existed?**

The **Placement API tracks resource provider inventories and allocations** — it is the authoritative record of what resources (vCPU, RAM, disk, GPUs) each compute host has, how much is committed, and what's available. Before Placement (pre-Pike release), the nova-scheduler queried the `compute_nodes` table directly for all hosts on every scheduling decision. At 10,000 hosts with 500 scheduling requests per second, that's 5 million DB rows scanned per second — the scheduler was a consistent bottleneck. Placement replaced this with a purpose-built API optimized for the `GET /allocation_candidates` query that returns only viable candidates in < 50 ms.

---

**Q: What is DVR in Neutron and why would you use it?**

**DVR (Distributed Virtual Routing)** moves L3 routing and floating IP NAT from centralized network nodes onto each compute host. In the traditional centralized model, all east-west traffic between VMs on different hosts (even on the same rack) and all north-south traffic must hairpin through 3 dedicated network nodes — creating a bandwidth bottleneck and centralized failure point. With DVR, east-west routing happens locally on the source compute host (no network node traversal), and north-south traffic with floating IPs is NATted directly on the compute host's `br-ex` bridge. OVN goes further — it doesn't use DVR terminology but achieves the same distributed routing natively.

---

**Q: What happens to a Galera cluster when one node fails?**

If the cluster has 3 nodes and one fails, the **remaining 2 nodes continue operating** — they still have quorum (2 > N/2). The cluster switches to 2-node mode; any subsequent node failure would lose quorum and the cluster would halt writes. When the failed node rejoins, it performs IST (Incremental State Transfer) if it has been down briefly (missed writes fit in the gcache buffer) or SST (State Snapshot Transfer — full copy) if it has been down too long. During SST, the donor node enters the "donor/desynced" state. The critical monitoring concern: if all 3 nodes lose contact simultaneously (network partition), the node that cannot see the others sets itself to "Non-primary" and refuses writes — this is intentional split-brain prevention.

---

### Tier 2 — Deep Dive Questions

**Q: Walk me through exactly how a Fernet token gets validated at a Nova API call, and what happens on cache miss vs. cache hit.**

When nova-api receives an HTTP request, the **oslo.middleware.auth_token** middleware extracts the X-Auth-Token header. It checks the Memcached pool for a cached result using the token as the key. On **cache hit** (< 1 ms): it reads the cached token info (user_id, project_id, roles, expiry) and proceeds with RBAC check — no Keystone or DB involved. On **cache miss**: the middleware decrypts the Fernet token locally using the key at the primary position in `/etc/keystone/fernet-keys/`. Fernet uses AES-256-CBC encryption + HMAC-SHA256 — pure local computation, < 5 ms. The decrypted payload contains user, project, roles, issued_at, and expiry. If expiry has passed, the token is rejected. If valid, the result is written to Memcached with the token's remaining TTL, and the request proceeds. The only time there's a Keystone network call is when middleware encounters a token it truly cannot validate locally — which shouldn't happen in normal operation if key rotation is properly synchronized.

The trade-off to volunteer: Fernet keys rotate every 30 minutes. During key rotation, tokens signed by the old primary key are still valid — they're validated using the secondary key (key `1`). But if you rotate without distributing the new key to all controllers first, any controller with only the old key ring will reject newly-issued tokens. The solution is to always push new keys before rotating: `ansible-playbook distribute_fernet_keys.yml && keystone-manage fernet_rotate`.

---

**Q: How does the nova-scheduler avoid race conditions when two schedulers simultaneously try to claim the last VCPU on the same host?**

This is the **optimistic locking problem** solved by the `generation` field on `resource_providers`. When the scheduler calls `PUT /allocations/{consumer_id}`, the request body includes the `resource_provider_generation` value it observed when querying allocation candidates. The Placement API executes the allocation as a conditional update: `UPDATE resource_providers SET generation = generation + 1 WHERE id = ? AND generation = ?`. If two schedulers both saw generation=5 and both try this update, only one will succeed (the one that updates first gets generation=6; the second sees 0 rows updated and returns 409 Conflict). The losing scheduler receives a 409 Conflict response and retries with the next candidate from its sorted list. This requires no distributed lock, no coordination service — just a single atomic DB UPDATE with a generation check.

The trade-off to volunteer: During burst VM creation (150 creates/sec), you can get many 409s on popular hosts (e.g., hosts with the most free vCPUs). Each retry burns a scheduling cycle. Mitigation: run multiple nova-scheduler instances so they process different VM creates independently; use the `shuffle_best_same_weighed_hosts` option to randomize among equally-weighted top candidates so not all schedulers pick the same host.

---

**Q: Explain the pre-copy vs. post-copy live migration trade-offs and when you'd choose each.**

Pre-copy is the default and the safer option. The VM keeps running on the source throughout migration. Dirty pages are copied iteratively until the residual is small enough to transfer in under 100 ms during a brief pause. Pre-copy's failure mode is clean: if anything goes wrong, the source VM is still running, and you just cancel the migration. The risk is non-convergence: if the VM's **dirty rate exceeds your migration bandwidth**, you never reach convergence. The convergence condition is `dirty_rate < bandwidth × threshold` (e.g., 200 MB/s < 1000 MB/s × 0.3). Active database servers writing at 2+ GB/s on a 1 GB/s migration link will never converge without intervention.

Post-copy inverts the approach: immediately pause the VM on source, transfer CPU state and dirty bitmap to destination, then resume execution on the destination before all memory has been transferred. The destination fetches missing pages on-demand via userfaultfd page faults as the VM accesses them. Post-copy is fast for large-memory VMs (only transfers working set, not full RAM) but has a critical risk: **if the source host fails while the VM is demand-paging from it, you lose the VM** — there's no way to reconstruct the missing pages. Use post-copy for large-memory VMs (> 64 GB) where performance during migration matters more than migration safety, and only when source hardware health is confirmed. Never use post-copy for VMs that cannot tolerate unrecoverable failures.

---

**Q: How does OVN eliminate the centralized network node bottleneck, and what are the operational trade-offs?**

OVN replaces neutron-openvswitch-agent, neutron-l3-agent, neutron-dhcp-agent, and neutron-metadata-agent on each compute host with a single **ovn-controller** daemon. ovn-controller subscribes to the OVN Southbound Database (a Raft-replicated OVSDB cluster) and receives compiled logical flows that represent the network's desired state. It translates these into actual OpenFlow rules in the local OVS bridge. Because every compute host runs its own ovn-controller, L3 routing and DHCP are handled locally — no traffic needs to travel to a central network node for east-west routing or DHCP responses.

The control plane architecture is: neutron-server writes logical topology to the OVN Northbound DB → ovn-northd compiles logical flows → writes to OVN Southbound DB → ovn-controller on each host reads delta updates and programs OVS flows.

Operational trade-offs: ✅ Linear scaling (adding compute hosts adds distributed network capacity proportionally). ✅ Eliminates centralized L3 SPOFs. ✅ Security group changes propagate as Southbound DB deltas (< 3s to 10,000 hosts via incremental OVSDB streaming). ❌ Debugging is harder — flow problems are distributed across thousands of hosts rather than concentrated on 3 network nodes. ❌ ovn-northd and the Northbound/Southbound DB clusters are new failure points; you need to monitor their Raft health. ❌ The OVN Southbound DB can grow large (~500 MB for 400K VMs); OVSDB has known performance cliffs at extreme scale.

---

**Q: How does cross-region token validation work in a multi-region OpenStack deployment?**

The key insight is that **Fernet token validation is cryptographic, not database-dependent**. When a user authenticates against the global Keystone in US-East, they get a Fernet token. When they take that token to EU-West's nova-api, the oslo.middleware.auth_token middleware on EU-West validates the token locally using the Fernet key ring in `/etc/keystone/fernet-keys/` on the EU-West controllers. Because the same key ring (synced from global Keystone) exists in every region, any region can validate any token without calling US-East. This is how a 300 ms cross-region RTT doesn't show up on every single API call.

The only cross-region call happens when a user first authenticates (token issuance goes to global Keystone, ~80 ms RTT to US-East from EU-West). The per-region Keystone proxy caches both tokens (so repeat auth calls in the same 5-minute window are local) and the service catalog (~50 KB, 5-minute TTL, so nova-api in EU-West knows where to find Glance in EU-West without a cross-region lookup). Key rotation is the coordination challenge: keys must be pushed to all regions before `keystone-manage fernet_rotate` is called, or regions with stale keys will reject tokens signed by the new primary.

---

**Q: What is the nova-conductor, and why does it exist as a separate service rather than being built into nova-compute?**

nova-conductor serves two distinct purposes. First, it's a **security boundary**: nova-compute runs on hypervisor hosts, which are considered an untrusted boundary — a compromised nova-compute daemon on a multi-tenant hypervisor should not be able to directly manipulate the Nova database (e.g., setting other tenants' VMs to DELETED state or modifying resource accounting). By requiring nova-compute to make RPC calls to nova-conductor for all DB writes, you ensure that even a fully compromised nova-compute can only affect operations that conductor's authorization logic permits.

Second, conductor handles **complex multi-step operations** that require orchestration beyond what compute can do alone: the full VM build workflow (calling Placement API, selecting a host, routing to the correct cell DB, cascading to nova-compute); live migration orchestration (pre-checks, resource claiming on destination, source ↔ destination RPC coordination, cleanup on source); and resize confirm/revert (which requires DB writes across both the source cell and the nova_api DB). These workflows have too many failure modes and compensating actions to implement safely inside nova-compute.

---

### Tier 3 — Staff+ Stress Test Questions

**Q: Your 5-node Galera cluster is showing wsrep_flow_control_paused = 0.6 on 2 out of 5 nodes during peak hours. Nova API P99 latency has jumped from 200ms to 4 seconds. Walk me through your diagnosis and remediation in priority order.**

First priority is **stop the bleeding**: identify which Galera nodes are lagging by checking `SHOW STATUS LIKE 'wsrep_local_recv_queue_avg'` on all nodes. High receive queue on a node means it's the bottleneck. Check the write profile on that node — is it CPU-bound (high `wsrep_cert_failures`, indicating conflict storms), I/O-bound (check `wsrep_apply_window` and disk iostat), or network-constrained? If the nodes are physically healthy, remove them from the cluster temporarily (`SET GLOBAL wsrep_desync = ON`) to let them catch up without pausing the write stream. This degrades safety (those 2 nodes are temporarily out of sync) but restores write throughput immediately.

Second priority is **root cause**: if it's I/O, check if a nova-compute heartbeat storm caused a write burst (periodic sync of 10K compute_nodes rows), or if Glance image uploads went through the Galera nodes (shouldn't — image data should be in Ceph). If it's CPU, check for a Galera certification conflict storm — this happens when multiple services are doing UPDATE on the same rows (e.g., the `quota_usages` table during a burst VM create). Mitigation: switch quota enforcement to oslo.limit or a distributed counter rather than per-row SQL pessimistic locks.

Long-term fix: consider **disaggregating Galera** from the controller nodes onto dedicated DB hardware with NVMe SSDs. Also consider **per-service DB clusters** for high-write services (nova cells can have their own Galera cluster separate from Keystone/Neutron/Heat).

---

**Q: You're tasked with designing a live migration system that must support 2,000 concurrent migrations during a rolling kernel upgrade across 10,000 hypervisor hosts. The migration window is 4 hours. Walk through how you'd design the migration wave controller and what guard rails you'd put in place.**

**Wave design:** Divide 10,000 hosts into waves of 50 hosts each (200 waves total). Each wave: (a) pre-check all 50 hosts (CPU compat with all possible destinations, NUMA topology available on alternatives), (b) trigger 50 × 3 = 150 concurrent migrations, (c) wait for all to complete or timeout, (d) apply kernel update and reboot those 50 hosts, (e) verify healthy before next wave.

Time budget: 4 hours = 14,400 seconds / 200 waves = 72 seconds per wave. Each wave has 2,000 VMs × 10s average migration = 133 seconds at 150 concurrent migrations. That's already over budget. So either increase concurrency to 6 outbound/host or reduce wave size to 30 hosts. At 30 hosts × 6 concurrent = 180 parallel migrations per wave; 1,200 VMs / 180 = 6.7 seconds average wait; with 10s per migration, that's feasible.

**Guard rails:** Per-host rate limit (max_concurrent_outbound = 6, max_concurrent_inbound = 6) — this is the `nova.conf` migration concurrency setting. Circuit breaker: if > 10% of migrations in a wave fail (stuck or error state), halt the upgrade and page operations. Pre-check CPU compatibility using the cached `migration_cpu_compatibility` table rather than re-running virCPUCompare for every migration. Reserve 30% of the migration network bandwidth for live application traffic — don't saturate the spine at 1.2 Tbps out of a 25.6 Tbps total capacity. Monitor memory dirty rates across the wave — if p95 migration time exceeds 30s, slow the wave controller to avoid piling up MIGRATING instances.

**Failure recovery:** Track wave state in a dedicated migration controller service (or use Nova's existing host aggregates + scheduling hints). On wave failure, individual VM migration failures are handled by Nova's rollback — the VM stays ACTIVE on source. On host failure mid-wave, use nova-manage cell_v2 discover_hosts to reconcile, then run evacuate on the failed host's VMs.

---

**Q: A tenant reports that security group rule changes they make via the Neutron API are inconsistently enforced — some of their 500 VMs pick up the rule change within 1 second, others take 30+ seconds. Diagnose the root cause and fix it.**

**Diagnosis path:** The security group fanout goes: neutron-server writes to Neutron DB → publishes RPC fanout via RabbitMQ to `neutron.security_group_rules_for_devices` → all neutron-openvswitch-agents (or ovn-controllers) on all 10K hosts receive the message → agents program iptables/OpenFlow rules locally.

The 30-second lag on some VMs means some compute hosts are not receiving the fanout within SLA. Check: (1) Are the slow hosts running healthy neutron-openvswitch-agent or ovn-controller processes? (`openstack network agent list --host <hostname>` — look for is_alive=False). (2) Check RabbitMQ queue depth for the fanout exchange — if any consumer has a large queue backlog, it's processing slowly. (3) Check if the slow hosts have high system load causing the agent to lag processing incoming messages (oslo.messaging uses a thread pool for RPC consumers).

**Root cause possibilities:** (a) A nova-compute rolling upgrade left some hosts with older ovn-controller versions that process Southbound DB events slowly. (b) RabbitMQ routing: in legacy OVS mode, the fanout goes through all agents' queues simultaneously; if some agents' queues are backlogged (agent busy handling a burst of new VMs), rule updates queue behind them. (c) OVN mode: if ovn-controller on slow hosts is reconnecting to the Southbound DB (transient network issue), it processes a full resync on reconnect, which is O(number of flows on host) — 50,000 flows × 0.1ms = 5s. That explains a 30-second delay if the resync is serialized.

**Fix:** For OVN: ensure ovn-controller OVSDB connection is stable (check `ovs-vsctl show` for manager connection status); upgrade to OVN version with incremental processing. For legacy OVS: use `neutron-openvswitch-agent --config-file` to increase `rpc_response_timeout` and `executor_thread_pool_size`. Long term: migrate to OVN which uses OVSDB incremental change propagation rather than full fanout, eliminating the per-host queue bottleneck.

---

**Q: You need to design a zero-downtime OpenStack control plane upgrade from version 2023.1 (Antelope) to 2024.2 (Dalmatian) across 5 regions, each with 3 controller nodes and 10,000 compute hosts. What is your sequencing and what are the hardest problems to solve?**

**Sequencing principle:** OpenStack supports N-1 rolling upgrades — during a rolling upgrade, old and new API versions coexist. The sequence within each region is: (1) database schema migration (`nova-manage db sync` in offline migration mode, or online with expand/contract pattern), (2) upgrade and restart API services (stateless, immediate traffic shift), (3) upgrade workers (conductor, scheduler, heat-engine), (4) upgrade compute hosts in waves (nova-compute, neutron-openvswitch-agent/ovn-controller).

**Hardest problems:**

Database expand/contract migrations: Between releases, some tables get new columns, renamed columns, or dropped columns. The expand phase adds the new column (backward compatible — old code ignores unknown columns). The contract phase drops the old column (only safe once all code is on new version). If you accidentally run contract before all nova-compute instances are upgraded, old nova-compute daemons referencing the dropped column will crash. Solution: use `nova-manage db online_data_migrations` and `nova-manage db purge` before running contract.

RabbitMQ protocol compatibility: Between major OpenStack releases, oslo.messaging versions and RPC API versions change. During the window when old conductor is receiving messages from new nova-compute (or vice versa), oslo.messaging uses version-pinned RPC: `nova.conf [upgrade_levels] compute = auto` allows the new conductor to speak old RPC with old compute and new RPC with new compute. The risk is forgetting to remove this pin after upgrade, leaving a performance-degraded mixed-mode RPC protocol in production.

Multi-region sequencing: Upgrade regions in sequence (US-East first, then US-West, etc.) — never upgrade two regions simultaneously. Global Keystone is your dependency anchor: upgrade Keystone last (or first, if it's backward compatible with all service versions). If Keystone's token format changes, all regions must complete the upgrade within the token TTL window (24 hours by default) to avoid cross-region auth failures.

---

## STEP 7 — MNEMONICS

**Mnemonic 1: "GRAMPS" — the 6 layers of an OpenStack request**

- **G** — Gateway (HAProxy + Keepalived VIP)
- **R** — Router (oslo.messaging, RabbitMQ — routes work to workers)
- **A** — API services (Keystone, Nova, Neutron, Cinder — stateless, active-active)
- **M** — Metadata store (MySQL/Galera — the authoritative state)
- **P** — Placement (resource accounting, scheduling pre-filter)
- **S** — Scheduler/Conductor/Compute workers (where work happens)

When a VM create request arrives: it enters at **G** (HAProxy), hits **A** (nova-api validates via Keystone, writes initial state to **M**), routes via **R** (RabbitMQ cast to conductor), which calls **P** (Placement pre-filter), triggers **S** (scheduler selects host, conductor routes to nova-compute), and the final state goes back to **M** (Galera).

**Mnemonic 2: "FDC" — the three live migration failure modes and their solutions**

- **F** — Fails to converge (dirty rate > bandwidth) → XBZRLE compression or Auto-converge CPU throttle
- **D** — Doesn't land (source failure during post-copy) → VM lost; never use post-copy without understanding this
- **C** — CPU incompatibility (destination lacks CPU flags the VM was pinned to) → reject migration upfront via pre-check; use host-model CPU type in flavor

---

**Opening one-liner for the interview:**

"OpenStack is a distributed API-to-hypervisor translation layer — the interesting system design problems are all about what happens when the distributed parts disagree: the Galera node that's one write-set behind, the RabbitMQ quorum that can't elect a leader fast enough, or the VM whose dirty page rate races ahead of your migration bandwidth."

Use this as your 30-second framing when an interviewer asks "tell me how you'd approach designing a cloud infrastructure platform." It signals that you understand the hard problems are not "what services exist" but "how do you handle the distributed systems failure modes."

---

## STEP 8 — CRITIQUE

### Well-Covered in the Source Material

The source files are genuinely strong on:

- **Galera internals**: certification-based replication, wsrep_flow_control_paused as a key metric, SST vs. IST for node recovery. This is detailed and accurate.
- **Two-phase scheduling (Placement API + nova-scheduler)**: the numerical example (10,000 → 100 candidates in < 50 ms) and the generation-based optimistic locking are well explained.
- **Live migration convergence math**: the iteration-by-iteration calculation (8 GB VM, 8 Gbps link, 200 MB/s dirty rate → 5 iterations, ~10s total, ~18 ms downtime) is the most concrete quantitative example in the pattern.
- **OVN architecture**: the OVN NB DB → ovn-northd → OVN SB DB → ovn-controller pipeline is clearly described, as is the distinction from legacy OVS agent topology.
- **Fernet token mechanics**: the 3-key rotation ring, local validation, Memcached caching hierarchy are all correct and operationally relevant.

### Missing, Shallow, or Needs Caution

**Missing: Ceph RBD + OpenStack integration depth.** The files mention Ceph RBD as the shared storage backend for live migration and state "disk copy = 0 with Ceph" but don't explain how this works (all nova-compute hosts mount the same Ceph pool; the RBD image is referenced by the same `rbd://pool/imageName` URI from source and destination; libvirt talks to Ceph RADOS directly; migration is pure memory). In an interview, you need to be able to explain why Ceph makes live migration 100× faster than local disk migration.

**Missing: OpenStack networking failure modes at scale.** The Neutron file covers the happy path (OVN architecture, DVR) but doesn't deeply cover what happens when the OVN Southbound DB Raft cluster loses quorum (all existing VMs continue to forward traffic on existing flows, but no new VMs get ports, no security group changes propagate). This "control plane down, data plane up" property is a critical interviewer topic.

**Shallow: Rolling upgrade sequencing.** The source files mention "rolling upgrades without service interruption" as a requirement but never explain the database expand/contract migration pattern or the RPC versioning pinning requirement. This is a very common senior interview topic for OpenStack platform engineers.

**Shallow: Quota management at scale.** The multi-region file mentions global quotas but doesn't address the CAP theorem problem: you either have a centralized quota store (consistency, but cross-region latency on every resource create) or per-region quotas (fast, but no global cap enforcement). A thoughtful answer should mention using a distributed counter service or accepting eventual consistency for quota enforcement.

**Caution: Post-copy described as "avoids non-convergence" without flagging data loss risk strongly enough.** The source material notes the risk but understates it. In an interview, if you recommend post-copy without immediately noting "source host failure = VM lost, no recovery possible," an experienced interviewer will catch you. The files cover this but it deserves more emphasis.

**Caution: RabbitMQ quorum queue semantics.** The files correctly distinguish quorum queues from classic mirrored queues but don't explain the durability trade-off: quorum queues have higher write latency than classic queues (because they require majority ACK before confirming the publish). At 10,000 messages/second, the 3-node quorum queue overhead is acceptable, but at very high rates (50K+ msg/s), you may need to evaluate lazy queue configurations or message batching.

### Senior Probes — Gotcha Questions to Watch For

- **"Why can't you just use PostgreSQL for OpenStack?"** The answer is not "I don't like PostgreSQL" — it's that oslo.db and all OpenStack SQLAlchemy models use MySQL-specific features (utf8mb4 character set conventions, specific index behaviors, Galera's MySQL-compatible wire protocol). PostgreSQL with Patroni would be technically feasible as an architectural choice but would require forking the DB migration scripts and is not supported by OpenStack's upstream toolchain.

- **"If Fernet tokens are validated locally, how does Keystone revoke a token before it expires?"** This is the classic Fernet trade-off question. The answer: Keystone cannot revoke an individual Fernet token — there is no revocation mechanism. Instead, OpenStack relies on short token TTLs (default: 1 hour) and lists of revoked events (project deactivation, user password change) that are distributed to all services and checked against token payload fields. This means there's a window between revocation event and effective enforcement equal to the token TTL. If you need immediate revocation (e.g., security incident), you must rotate the Fernet keys, which invalidates ALL active tokens for all users — a blunt instrument.

- **"What happens to in-flight RPC messages when RabbitMQ fails over?"** With quorum queues, messages acknowledged by a majority of replicas before the leader failed are preserved on surviving nodes. Messages in-flight (published but not yet majority-ACKed) may be lost. Oslo.messaging handles this via exponential backoff reconnect — nova-conductor will re-queue the message. But this means a message might be processed twice (if the original consumer completed the task before the broker lost the ACK). Every RPC handler in OpenStack must be idempotent or check for duplicate processing.

- **"A user says their VM is stuck in BUILDING state for 20 minutes. What are your top 5 diagnostic steps?"** (1) Check `nova show <id>` for task_state — is it SCHEDULING, SPAWNING, BLOCK_DEVICE_MAPPING? (2) Check nova-conductor logs for the instance UUID — is there a Placement API error, a scheduler reject, or a DB timeout? (3) Check nova-compute logs on the target host — did libvirt return an error? (4) Check Neutron for the port state — is the port still DOWN? (5) Run `nova-manage placement heal_allocations` to check for orphaned Placement allocations blocking the host from being selected. This answer demonstrates operational knowledge beyond pure design.

### Traps in the Design Interview

- **Trap: Designing Galera as primary-replica.** If you say "I'd have a primary Galera node and two replicas," you've misunderstood Galera. Galera is multi-master — all nodes accept writes. There's no primary. What you configure is `wsrep_cluster_address` pointing to all nodes. The interviewer will catch this immediately.

- **Trap: Proposing Redis for the OpenStack token store.** Keystone doesn't use a token store for Fernet tokens — they're self-contained. Redis is irrelevant here. Memcached is used for token validation caching (which is different). If you propose Redis, the interviewer will ask "why not Memcached?" and you need to know the answer (Memcached is simpler, lower latency, and the existing OpenStack oslo.cache integration uses Memcached natively — Redis adds no benefit for this use case).

- **Trap: Claiming OVN and OVS are interchangeable.** They share the same underlying OVS data plane but have completely different control planes. OVN uses OVSDB Raft-replicated databases and compiles logical network state to physical flows. OVS uses neutron-openvswitch-agent with direct RabbitMQ communication to neutron-server. The operational model, debugging tools, and failure modes are different. If you say "it doesn't matter which one you pick," the interviewer will probe harder.

- **Trap: Ignoring the Cells v2 cell0 database.** Cell0 is a special cell that stores instances which failed to schedule (no host was found, or the selected host rejected the instance). Nova-api DB and cell0 must be accessible for accurate `openstack server list` to show instances in ERROR state with useful messages. Candidates who describe Cells v2 without mentioning cell0 are missing an important operational detail.

---

*End of Interview Guide — Infra-14: OpenStack & Cloud*
