# Common Patterns — Bare Metal & IaaS (01_bare_metal_iaas)

## Common Components

### State Machine for Resource Lifecycle
- All 4 systems model every major resource as an explicit state machine with guarded transitions
- bare_metal_reservation: `requested → pending_approval → confirmed → provisioning → active → draining → released` (also: `cancelled`, `preempted`, `failed`)
- iaas_platform_control_plane: instance `BUILDING → ACTIVE → SHUTOFF → PAUSED → SUSPENDED → REBOOT → DELETED`
- machine_pool_manager: machine `available → reserved → provisioning → in_use → draining → maintenance → failed → decommissioned`; pool `active → draining → frozen → retired`
- self_service_developer_portal: workflow `pending_approval → approved → running → paused → completed → failed → rolling_back`

### Idempotency Key for Exactly-Once Semantics
- All 4 systems accept a client-provided idempotency key on mutation operations; stored as UNIQUE constraint; duplicate requests return existing state
- bare_metal_reservation: `idempotency_key VARCHAR(128) UNIQUE` on `reservations` table
- iaas_platform_control_plane: instance creation idempotency via `instance_uuid CHAR(36) UNIQUE`
- self_service_developer_portal: `workflow_uuid CHAR(36) UNIQUE` on `workflows` table
- machine_pool_manager: `plan_uuid CHAR(36) UNIQUE` on `firmware_upgrade_plans`

### MySQL InnoDB as Single Source of Truth (RPO = 0)
- All 4 systems use MySQL 8.0 InnoDB with semi-synchronous replication; all authoritative state lives here; RPO = 0
- bare_metal_reservation: `SERIALIZABLE` isolation for conflict-critical transactions; `READ COMMITTED` for general reads
- iaas_platform_control_plane: quota enforcement, instance state, host capacity tracking
- machine_pool_manager: pool membership, health history, RMA records, firmware inventory
- self_service_developer_portal: workflow state, approval decisions, budget spend
- Optimistic locking via `version INT UNSIGNED` column on all state-bearing tables; retry on conflict

### Redis Cluster as Hot-Path Cache (Cache-Aside)
- All 4 systems cache the most-queried aggregates in Redis; miss falls through to MySQL; Kafka-driven invalidation
- bare_metal_reservation: machine availability sorted sets keyed by machine type; 10 MB working set
- iaas_platform_control_plane: host capacity cache (10 s TTL); quota in-use counters
- machine_pool_manager: pool membership index; pool state per machine (< 1 ms lookup)
- self_service_developer_portal: session data; dashboard aggregates (< 5 s lag)

### Kafka Event Bus (Exactly-Once, Topic-per-Entity)
- All 4 systems publish lifecycle events to Kafka; consumers include notification service, metering, cache invalidation, health scoring
- Topic naming: `<entity>.<action>` — e.g., `reservation.created`, `machine.state_changed`, `workflow.step.execute`, `machine.ejected`
- Idempotent producer + transactional consumer for exactly-once delivery
- bare_metal_reservation: topics `reservation.*`, `machine.state_changed`
- iaas_platform_control_plane: `instance.*`, `volume.*`, metering events
- machine_pool_manager: `machine.ejected`, health bundle events
- self_service_developer_portal: `workflow.step.execute`, cost events, notification events

### API Gateway (Kong/Envoy) with Rate Limiting + JWT Validation
- All 4 systems front with an API Gateway for TLS termination, JWT/API-key auth, per-tenant rate limiting, API versioning
- bare_metal_reservation: 50 req/min per tenant; global throttle for provisioning ops
- iaas_platform_control_plane: 5,000 API calls/sec global; per-tenant quotas enforced at gateway + Quota Service
- self_service_developer_portal: 10,000 CLI sessions; 5,000 concurrent portal users

### Pessimistic Locking (`SELECT … FOR UPDATE`) for Conflict-Critical Sections
- bare_metal_reservation: MySQL `SELECT … FOR UPDATE` after interval tree pre-check; prevents double-booking race condition
- iaas_platform_control_plane: quota enforcement and host capacity decrement under lock
- Self-service portal: budget hard-limit enforcement uses `SELECT … FOR UPDATE` to prevent concurrent overspend

### Per-AZ Colocation of Workers
- All 4 systems deploy compute-intensive workers (provisioning, health, firmware) in each AZ to minimize BMC/IPMI round-trip latency
- bare_metal_reservation: Provisioning Workers per-AZ (IPMI power cycle, PXE boot, VLAN config)
- iaas_platform_control_plane: image caches per-AZ for fast provisioning; metering consumers per-AZ
- machine_pool_manager: Health Data Collector per-AZ (polls IPMI/BMC every 15 min)

## Common Databases

### MySQL 8.0 InnoDB
- All 4; semi-synchronous replication; RPO = 0; UNIQUE constraints for idempotency; partitioned time-series tables (monthly)

### Redis Cluster
- All 4; sorted sets for availability/pool indexes; hash for capacity counters; pub/sub for real-time invalidation; session data

### Kafka
- All 4; topics partitioned by entity type; exactly-once via idempotent producer + transactional consumer; consumer groups per downstream

### Elasticsearch
- 3 of 4 (bare-metal, IaaS, portal); full-text search on audit logs, resource metadata, templates; not used in machine_pool_manager (uses Prometheus for time-series)

### Prometheus / VictoriaMetrics
- machine_pool_manager primary; also used for observability in all 4; health metrics 1,100 data points/sec; 90-day hot, 1-year cold

### S3 / GCS (Object Storage)
- machine_pool_manager (firmware images, SHA-256 verified); self_service_developer_portal (template files, Terraform state); iaas_platform_control_plane (OS images via Ceph RadosGW)

## Common Communication Patterns

### Async Provisioning via Kafka Step Execution
- bare_metal_reservation: provisioning steps (IPMI power-on, PXE boot, OS install, VLAN config) published as Kafka messages; idempotent retry on failure
- iaas_platform_control_plane: compute/network/storage operations enqueued; VM boot pipeline decoupled from API response
- self_service_developer_portal: `workflow.step.execute` events drive step execution asynchronously; workflow engine polls MySQL at 1 s interval for state

### Webhook + Slack Notifications
- machine_pool_manager: Slack webhook on machine ejection, firmware failure, RMA creation
- self_service_developer_portal: Slack (approval requests, cost alerts), email (SMTP/SES), PagerDuty (critical failures)

## Common Scalability Techniques

### Cell-Based Architecture (Blast Radius Isolation)
- iaas_platform_control_plane: ~5,000 hosts per cell; global super-scheduler routes requests to correct cell; prevents single cell failure from affecting all tenants
- bare_metal_reservation: per-AZ provisioning workers achieve similar isolation
- machine_pool_manager: per-pool capacity forecasting and health tracking operates independently per pool

### Time-Series Data Partitioning (Monthly Ranges)
- All 4 systems partition high-volume append-only tables by `TO_DAYS(timestamp)` month ranges for efficient pruning and bounded query scans
- `metering_events`, `health_checks`, `cost_aggregates` all monthly-partitioned

### Batch Processing with Blast-Radius Controls
- machine_pool_manager: firmware upgrades `batch_size = 10` machines; `max_failure_pct = 5.0%` auto-aborts plan if exceeded
- self_service_developer_portal: template applies batched; Terraform apply per step
- bare_metal_reservation: preemption with configurable grace periods (not all at once)

### Weighted Health Score (0–100) with Threshold Ejection
- bare_metal_reservation: `health_score TINYINT(0–100)` on machines; auto-eject when health drops (threshold = 30)
- machine_pool_manager: composite weighted formula; eject < 30, watch < 60, warn < 90

## Common Deep Dive Questions

### How do you prevent double-booking of machines without a distributed lock service?
Answer: Two-level approach: (1) Optimistic pre-check using in-memory interval tree or Redis availability cache — fast O(log n) overlap detection rejects obvious conflicts without database contact; (2) Pessimistic lock via MySQL `SELECT … FOR UPDATE` to serialize the critical section: read current state, verify no conflict, insert reservation, decrement availability, commit — all within one transaction. The interval tree is rebuilt on service restart from MySQL (source of truth). This avoids a distributed lock service while guaranteeing correctness through database serialization.
Present in: bare_metal_reservation_platform, iaas_platform_control_plane

### How do you achieve RPO = 0 while keeping write latency low?
Answer: MySQL semi-synchronous replication with `AFTER_SYNC` wait point: (1) primary writes to InnoDB redo log; (2) flushes binlog; (3) waits for at least 1 replica ACK; (4) commits and returns to client. Even if primary crashes after flush but before ACK, the replica has the data and becomes the new primary. Latency cost: ~1–3 ms additional vs async replication. Acceptable because write SLO is 200–500 ms P99.
Present in: bare_metal_reservation_platform, iaas_platform_control_plane, machine_pool_manager, self_service_developer_portal

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.99% API (all 4); 99.999% data plane (IaaS); 99.9% portal UI |
| **Write Latency P99** | < 200 ms (reservation), < 500 ms (IaaS API), < 50 ms (pool query), < 2 s (CLI) |
| **Provisioning Latency** | < 15 min bare-metal (all 4); < 60 s VM (IaaS) |
| **Scale** | 50K machines (reservation), 100K servers + 500K VMs (IaaS), 100K machines + 500 pools (pool manager), 5K users (portal) |
| **RPO** | 0 for all authoritative state (reservations, instance state, pool config, workflow/approval) |
| **Consistency** | Strong for writes (conflict, quota, approval); eventual < 5–30 s for dashboards and health scores |
