# Common Patterns — CLI & Portal Design (15_cli_and_portal_design)

## Common Components

### MySQL as Primary Relational Store
- All 4 systems use MySQL 8.0 for core state: users, projects, quotas, resources, workflows, notifications, cost snapshots, execution history
- Shared field patterns: `id BIGINT PRIMARY KEY AUTO_INCREMENT`, `created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP`, `updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE`, `status ENUM(...)`, `project_id BIGINT` as partition key
- Common indexes: `idx_<table>_project (project_id, status)`, `idx_<table>_user (user_id, created_at)`

### OAuth2 / OIDC Authentication (Okta)
- All 4 systems authenticate via OAuth2/OIDC with corporate IdP
- cli_client: OAuth2 device flow (no browser required for headless/CI use); polls `POST /oauth2/token` with `device_code` until success or timeout; stores tokens in `~/.infra-cli/tokens/<context>.json`
- developer_portal + web_portal: authorization code flow; server-side session in Redis (8h TTL); HttpOnly cookie prevents JS access to JWT
- infrastructure_as_code: service account tokens for CI/CD pipelines

### Resource State Machine (Shared Status ENUM)
- All 4 systems model resources with the same lifecycle: `pending_approval → approved → provisioning → active → expiring → deprovisioning → terminated`
- Shared fields: `resource_uid VARCHAR(64) UNIQUE`, `resource_type ENUM(vm,bare_metal,k8s_cluster,block_storage,...)`, `status ENUM(...)`, `spec_json JSON`, `expires_at TIMESTAMP NULL`, `cost_per_hour DECIMAL(10,4)`
- developer_portal uses Temporal.io for durable workflow execution across this state machine
- web_portal tracks status via WebSocket push (< 50 ms P50 event delivery)

### Quota Enforcement (SELECT FOR UPDATE + Reservation Pattern)
- 3 of 4 systems (cli, developer_portal, web_portal) enforce per-project hard/soft limits with pessimistic locking
- Algorithm: `SELECT … FOR UPDATE` on quota row → check `used + reserved + requested ≤ hard_limit` → increment `reserved` → commit; on success: decrement `reserved`, increment `used`; on failure: decrement `reserved` (release)
- `quotas` table: `project_id`, `resource_type ENUM(vcpu,memory_gb,disk_gb,gpu,vm_count,...)`, `hard_limit`, `soft_limit`, `used`, `reserved`
- Background reconciliation job every 5 min expires stale reservations (> 30 min without status change)

### Audit Trail in Elasticsearch (7-Year ILM)
- 3 of 4 systems (developer_portal, web_portal, infrastructure_as_code) write immutable audit events to Elasticsearch 8.x
- Shared mapping: `event_id KEYWORD`, `timestamp DATE`, `actor_email KEYWORD`, `action KEYWORD`, `resource_uid KEYWORD`, `resource_type KEYWORD`, `project_id KEYWORD`, `cost_cents LONG`
- ILM rollover alias; 3 shards + 1 replica; 7-year retention for compliance
- Ingestion via RabbitMQ (at-least-once); async indexing decouples hot path from write

### Cost Tracking (Per-Hour, Daily Snapshots)
- All 4 systems track resource costs; shared formula: `base_hourly + (vcpus × per_vcpu) + (ram_gb × per_gb_ram) + (gpus × gpu_rate)`
- Example: 16 vCPU + 64 GB RAM + 4× H100: `$0.50 + 16×$0.02 + 64×$0.01 + 4×$7.86 = $32.90/hr`
- `daily_cost_snapshots`: `project_id`, `date DATE`, `resource_type`, `resource_count`, `total_cost_cents BIGINT`, `breakdown_json JSON`; `UNIQUE KEY (project_id, date, resource_type)`

### Rate Limiting (Redis Sliding Window)
- All 4 systems apply per-user rate limiting; Redis sliding window counter; read endpoints: 100 req/min; write endpoints: 20–200 req/min depending on operation

## Common Databases

### MySQL 8.0
- All 4; ACID transactions for quota enforcement and workflow state; SELECT FOR UPDATE for pessimistic locking; read replicas for dashboard queries

### Redis 6.x
- 3 of 4 (developer_portal, web_portal, cli cache); server-side session (8h TTL); pub/sub for WebSocket fan-out; rate limit counters; quota cache (5-min TTL)

### Elasticsearch 8.x
- 3 of 4 (developer_portal, web_portal, infrastructure_as_code); immutable audit search; ILM 7-year retention; 3 shards + 1 replica

### S3 / MinIO
- infrastructure_as_code: state files with versioning (30 historical versions per workspace); plan outputs (7-day cache); module archives; encryption at rest

## Common Communication Patterns

### REST API with Standard Response Envelope
- All 4; `GET/POST/DELETE /api/v1/<resource>`; Bearer JWT auth; envelope: `{data, meta, errors}`; 30 s HTTP timeout (CLI); P99 API response < 500 ms

### WebSocket (Real-Time Status Push)
- web_portal + developer_portal: Spring WebSocket; Redis pub/sub fan-out to 1,000 concurrent connections; channels: `resource:{uid}`, `project:{id}:activity`, `user:notifications`; 30 s heartbeat ping/pong; P50 event delivery < 50 ms

### RabbitMQ (Async Events)
- developer_portal + web_portal: workflow events, provisioning status updates, notification dispatch, audit log ingestion; at-least-once delivery; durable queues

### Parallel Backend Aggregation (BFF Pattern)
- web_portal BFF: `CompletableFuture.supplyAsync` to 4 backend services simultaneously; dashboard summary assembled in single response; eliminates client-side waterfalls

## Common Scalability Techniques

### CDN for Static Assets
- web_portal: CloudFront serves React SPA (~500 KB gzipped JS + CSS); edge caching; no origin hit for returning users

### Stateless Services + Redis Session
- All 4; API services are stateless; session state in Redis (not sticky sessions except for WebSocket); horizontal scale by adding pods

### File-Based / Layer Cache
- cli_client: `~/.infra-cli/cache/` directory; JSON files with TTL metadata (1h for templates, 5 min for machines); offline mode fallback
- infrastructure_as_code: S3 state file as cache; `plan_outputs` cached 7 days; module registry archives in S3

### State Locking (Pessimistic Concurrency Control)
- infrastructure_as_code: DynamoDB conditional write (`attribute_not_exists(lock_id)`) OR MySQL `SELECT … FOR UPDATE`; lock TTL for stale cleanup; `lock_id`, `owner`, `operation`, `expires_at`, `version`
- developer_portal: quota rows locked with `SELECT … FOR UPDATE` during reservation

## Common Deep Dive Questions

### How do you prevent quota over-provisioning under concurrent requests?
Answer: Reservation-based two-phase approach using pessimistic locking: (1) `SELECT … FOR UPDATE` on the quota row acquires row-level lock; (2) check `used + reserved + requested ≤ hard_limit` — if denied, release lock immediately; (3) if allowed, increment `reserved` and commit — lock released; (4) provisioning starts async; (5) on success: swap `reserved → used`; on failure: decrement `reserved`. Background reconciliation every 5 min detects stale reservations (> 30 min) and releases them. The key invariant: `used + reserved` is always an accurate upper bound on committed capacity.
Present in: developer_self_service_portal, web_portal_for_iaas

### How does the CLI achieve < 100 ms startup time with a full feature set?
Answer: Go binary with no heavy framework initialization. Cobra's command routing is O(1) dispatch. Configuration loaded lazily (Viper reads `~/.infra-cli/config.yaml` only on first command that needs it). OAuth tokens read from OS keyring or file — no network calls until command execution. Local file cache (`~/.infra-cli/cache/`) serves tab-completion lookups without API calls. Shell completion scripts use cached responses (5-min TTL) for sub-300 ms completion. Total binary size < 30 MB (single static binary, cross-compiled). No JVM/Python interpreter startup overhead.
Present in: cli_client_for_infrastructure_platform

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.95% portal; 99.99% provisioning API and CLI API |
| **API Latency P50** | 50–100 ms (portal, self-service); < 500 ms (list); < 300 ms (shell completion) |
| **API Latency P99** | 300–500 ms (portal); < 2 s (CLI create); < 1 s (search) |
| **Page Load** | P50 1.5 s (cold), 200 ms (SPA nav); P99 3 s |
| **Provisioning Time** | VM < 3 min; bare-metal < 15 min; K8s cluster < 10 min |
| **Concurrency** | 1,000 portal users; 300 WebSocket; 60 concurrent plans; 25 concurrent applies |
| **Throughput** | 17 RPS peak (portal); 1.2 RPS (CLI); 6 RPS (self-service) |
| **Audit Retention** | 7 years (Elasticsearch ILM) |
| **Session TTL** | 8 hours (Redis); CLI tokens: refreshed transparently |
| **CLI Startup** | < 100 ms; binary < 30 MB |
