# Problem-Specific Details — CLI & Portal Design (15_cli_and_portal_design)

---

## cli_client_for_infrastructure_platform

### Unique Stack
- Language: Go (single static binary, cross-compiled for linux/darwin/windows amd64/arm64)
- CLI framework: Cobra (command routing O(1) dispatch) + Viper (lazy config loading)
- Auth: OAuth2 device flow (no browser; headless/CI compatible); polls `POST /oauth2/token` with `device_code`
- Token storage: `~/.infra-cli/tokens/<context>.json` (OS keyring or file fallback)
- Config: `~/.infra-cli/config.yaml` — multi-context support (dev/staging/prod endpoints)
- Cache: `~/.infra-cli/cache/` — JSON files with embedded TTL metadata (1h for templates, 5 min for machines); offline mode fallback for last-known state
- Shell completions: cached responses (5-min TTL) → sub-300 ms completion; no API call for tab-complete

### Key Algorithms / Design Decisions
- **Startup latency < 100 ms**: No JVM/Python interpreter overhead; Viper reads config only on first command that needs it (lazy init); OAuth tokens read from keyring/file with no network calls until execution
- **Binary size < 30 MB**: Single static binary; no heavy framework initialization
- **Context switching**: `infra-cli config use-context <name>` swaps active endpoint + token file atomically
- **Retry logic**: Exponential backoff on 5xx; surface error codes with actionable messages

### Key Tables / Data Structures
- `~/.infra-cli/config.yaml`:
  ```yaml
  current-context: prod
  contexts:
    - name: prod
      endpoint: https://api.infra.corp.com
      project_id: 42
    - name: staging
      endpoint: https://api.staging.infra.corp.com
      project_id: 17
  ```
- Cache file format: `{ "data": [...], "expires_at": "2025-01-01T12:00:00Z" }`

### NFRs
- 4,000 CLI users; 1.2 RPS peak
- Startup: < 100 ms
- Binary: < 30 MB
- Shell completion: < 300 ms
- API P99: < 2 s for create operations, < 500 ms for list
- Availability: 99.99% (CLI talks to same API as portal)

---

## developer_self_service_portal

### Unique Stack
- Backend: Java Spring Boot; REST API + Temporal.io for durable provisioning workflows
- Workflow engine: Temporal.io — handles retries, timeouts, state persistence across multi-step provisioning
- Auth: OAuth2/OIDC authorization code flow; server-side session in Redis (8h TTL); HttpOnly cookie
- Async events: RabbitMQ for provisioning status updates, workflow events, notification dispatch
- Audit: Elasticsearch 8.x with 7-year ILM; ingestion via RabbitMQ (at-least-once); async to decouple hot path

### Key Algorithms / Design Decisions
- **Quota enforcement**: `SELECT … FOR UPDATE` on quota row → check `used + reserved + requested ≤ hard_limit` → increment `reserved` → commit; on provision success: swap `reserved → used`; on failure: decrement `reserved`; stale reservations (> 30 min without status change) released by background reconciliation every 5 min
- **Cost formula**: `base_hourly + (vcpus × $0.02) + (ram_gb × $0.01) + (gpus × $7.86)`
  - Example: 16 vCPU + 64 GB RAM + 4× H100 = `$0.50 + $0.32 + $0.64 + $31.44 = $32.90/hr`
- **Approval thresholds**: requests estimated > $10K/mo require manager approval; > $50K/mo require VP approval; enforced in Temporal workflow as human-task activity
- **Temporal workflow steps**: validate_quota → reserve_quota → submit_provision → wait_provision_complete → activate_resource → notify_user; each step retried independently on failure

### Key Tables
```sql
users (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  email VARCHAR(255) UNIQUE,
  role ENUM('developer','team_lead','manager','vp','admin'),
  project_id BIGINT,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

projects (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  name VARCHAR(255),
  owner_user_id BIGINT,
  cost_center VARCHAR(64),
  monthly_budget_cents BIGINT,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

quotas (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  project_id BIGINT NOT NULL,
  resource_type ENUM('vcpu','memory_gb','disk_gb','gpu','vm_count'),
  hard_limit INT NOT NULL,
  soft_limit INT NOT NULL,
  used INT DEFAULT 0,
  reserved INT DEFAULT 0,
  INDEX idx_quotas_project (project_id, resource_type)
);

resources (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  resource_uid VARCHAR(64) UNIQUE,
  project_id BIGINT NOT NULL,
  user_id BIGINT NOT NULL,
  resource_type ENUM('vm','bare_metal','k8s_cluster','block_storage'),
  status ENUM('pending_approval','approved','provisioning','active','expiring','deprovisioning','terminated'),
  spec_json JSON,
  expires_at TIMESTAMP NULL,
  cost_per_hour DECIMAL(10,4),
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX idx_resources_project (project_id, status),
  INDEX idx_resources_user (user_id, created_at)
);

workflows (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  temporal_workflow_id VARCHAR(255) UNIQUE,
  resource_uid VARCHAR(64),
  workflow_type ENUM('provision','deprovision','extend','resize'),
  status ENUM('running','completed','failed','cancelled'),
  started_at TIMESTAMP,
  completed_at TIMESTAMP NULL,
  error_message TEXT NULL
);

notifications (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  user_id BIGINT NOT NULL,
  type ENUM('provision_complete','provision_failed','quota_warning','approval_required','expiry_notice'),
  message TEXT,
  read_at TIMESTAMP NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_notifications_user (user_id, read_at)
);

daily_cost_snapshots (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  project_id BIGINT NOT NULL,
  date DATE NOT NULL,
  resource_type ENUM('vm','bare_metal','k8s_cluster','block_storage'),
  resource_count INT,
  total_cost_cents BIGINT,
  breakdown_json JSON,
  UNIQUE KEY uq_cost_snapshot (project_id, date, resource_type)
);
```

### NFRs
- 10,000 registered developers; 3,000 DAU; 6,000 resource provisions/day
- API P50: < 100 ms; P99: < 500 ms
- Provisioning: VM < 3 min; bare-metal < 15 min; K8s cluster < 10 min
- Quota check + reserve: < 50 ms (single DB round-trip with lock)
- Availability: 99.95%
- Audit retention: 7 years (Elasticsearch ILM)

---

## infrastructure_as_code_platform

### Unique Stack
- Backend: Go; gRPC provider plugin architecture (providers run as separate processes)
- State storage: S3/MinIO — versioned state files, 30 historical versions per workspace; encryption at rest
- State locking: DynamoDB conditional write (`attribute_not_exists(lock_id)`) OR MySQL `SELECT … FOR UPDATE`; lock TTL for stale cleanup
- Plan cache: S3 plan outputs cached 7 days; module archives in S3
- Auth: service account tokens for CI/CD pipelines (machine identity, not human OAuth)

### Key Algorithms / Design Decisions
- **Dependency graph**: topological sort (Kahn's algorithm) on resource dependency DAG; parallel execution of independent resource tiers; `depends_on` edges block tier advancement
- **Plan latency**: < 15 s for 50 resources; < 2 min for 500 resources; bottleneck is provider API calls (parallelized per resource tier)
- **State locking flow**: `PUT state_lock WHERE attribute_not_exists(lock_id)` (atomic DynamoDB conditional write) → if conflict: return existing lock owner + operation for error message → lock released on apply complete or TTL expiry
- **Module registry**: SHA-256 content-addressed module archives in S3; semantic versioning; dependency resolution at plan time

### Key Tables
```sql
workspaces (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  name VARCHAR(255),
  project_id BIGINT NOT NULL,
  state_s3_key VARCHAR(512),       -- pointer to current state in S3
  state_version_id VARCHAR(128),   -- S3 version ID
  terraform_version VARCHAR(32),
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX idx_workspaces_project (project_id)
);

execution_history (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  workspace_id BIGINT NOT NULL,
  operation ENUM('plan','apply','destroy'),
  triggered_by VARCHAR(255),       -- user email or CI service account
  status ENUM('pending','planning','applying','completed','failed','cancelled'),
  plan_s3_key VARCHAR(512) NULL,
  started_at TIMESTAMP,
  completed_at TIMESTAMP NULL,
  resource_add INT DEFAULT 0,
  resource_change INT DEFAULT 0,
  resource_destroy INT DEFAULT 0,
  INDEX idx_execution_workspace (workspace_id, started_at)
);

execution_resource_changes (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  execution_id BIGINT NOT NULL,
  resource_address VARCHAR(512),   -- e.g. "aws_instance.web[0]"
  action ENUM('create','update','delete','no_op'),
  provider VARCHAR(64),
  resource_type VARCHAR(128),
  before_json JSON NULL,
  after_json JSON NULL
);

state_lock (
  workspace_id BIGINT PRIMARY KEY,
  lock_id VARCHAR(64) UNIQUE,
  owner VARCHAR(255),              -- user or CI run ID
  operation ENUM('plan','apply','destroy'),
  acquired_at TIMESTAMP,
  expires_at TIMESTAMP,            -- TTL for stale lock cleanup
  version INT DEFAULT 0            -- optimistic locking for unlock
);

module_registry (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  namespace VARCHAR(128),
  module_name VARCHAR(128),
  provider VARCHAR(64),
  version VARCHAR(32),
  s3_archive_key VARCHAR(512),
  sha256_digest CHAR(64),
  published_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_module_version (namespace, module_name, provider, version)
);
```

### NFRs
- 3,000 workspaces; 150,000 managed resources
- Plan: < 15 s (50 resources); < 2 min (500 resources)
- Apply: < 5 min (50 resources); < 20 min (500 resources)
- Concurrent plans: 60; concurrent applies: 25
- State file: 30 historical versions; S3 11-nines durability
- Availability: 99.95%
- Audit retention: 7 years (Elasticsearch ILM)

---

## web_portal_for_iaas

### Unique Stack
- Frontend: React 18+ SPA (~500 KB gzipped JS + CSS); served via CloudFront CDN; edge-cached for returning users
- Backend: Java Spring Boot BFF (Backend For Frontend) + Spring WebSocket
- BFF aggregation: `CompletableFuture.supplyAsync` to 4 backend services simultaneously; dashboard assembled in single response (no client-side waterfall)
- Real-time: Spring WebSocket + Redis pub/sub fan-out; 1,000 concurrent WebSocket connections; 30 s heartbeat ping/pong
- Session: server-side session in Redis (8h TTL); HttpOnly cookie prevents JS access to JWT
- Auth: OAuth2/OIDC authorization code flow; corporate IdP (Okta)

### Key Algorithms / Design Decisions
- **WebSocket channels**:
  - `resource:{uid}` — live status updates for specific resources
  - `project:{id}:activity` — project-scoped activity feed
  - `user:notifications` — per-user notification push
- **BFF fan-out**: simultaneously calls compute service (VM list), network service (VPC/subnet), storage service (volumes), billing service (cost summary); P99 dashboard load < 500 ms (parallel, not sequential ~2 s)
- **Page load**: P50 1.5 s cold (CDN miss + API); 200 ms SPA navigation (cached JS + incremental data fetch); P99 3 s
- **WebSocket event delivery**: Redis pub/sub → BFF WebSocket gateway → client; P50 < 50 ms event delivery

### Key Tables
```sql
user_preferences (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  user_id BIGINT NOT NULL UNIQUE,
  default_project_id BIGINT NULL,
  default_region VARCHAR(64),
  notification_email BOOLEAN DEFAULT TRUE,
  notification_in_app BOOLEAN DEFAULT TRUE,
  theme ENUM('light','dark','system') DEFAULT 'system',
  updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
);

portal_notifications (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  user_id BIGINT NOT NULL,
  type ENUM('provision_complete','provision_failed','quota_warning','approval_required','expiry_notice','cost_alert'),
  title VARCHAR(255),
  body TEXT,
  resource_uid VARCHAR(64) NULL,
  read_at TIMESTAMP NULL,
  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_portal_notif_user (user_id, read_at)
);

dashboard_widgets (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  user_id BIGINT NOT NULL,
  widget_type ENUM('cost_summary','resource_list','quota_usage','activity_feed','alerts'),
  position_x INT,
  position_y INT,
  width INT,
  height INT,
  config_json JSON,
  updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  INDEX idx_widgets_user (user_id)
);

daily_cost_snapshots (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  project_id BIGINT NOT NULL,
  date DATE NOT NULL,
  resource_type ENUM('vm','bare_metal','k8s_cluster','block_storage'),
  resource_count INT,
  total_cost_cents BIGINT,
  breakdown_json JSON,
  UNIQUE KEY uq_cost_snapshot (project_id, date, resource_type)
);
```

### NFRs
- 10,000 registered users; 3,000 DAU; 300 peak concurrent users; 17 RPS peak
- Page load: P50 1.5 s cold; 200 ms SPA navigation; P99 3 s
- WebSocket: 1,000 concurrent connections; P50 event delivery < 50 ms; 30 s heartbeat
- API P50: 50 ms; P99: 300 ms (BFF parallel aggregation)
- BFF dashboard: < 500 ms P99 (4 parallel backend calls)
- Availability: 99.95%
- Session TTL: 8 hours (Redis)
