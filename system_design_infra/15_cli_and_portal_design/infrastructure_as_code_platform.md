# System Design: Infrastructure as Code Platform

> **Relevance to role:** A cloud infrastructure platform engineer must understand IaC engine internals -- state management, dependency graphs, plan/apply cycles, provider plugins, and drift detection. This system is the programmatic backbone for reproducible infrastructure provisioning across bare-metal, OpenStack, and Kubernetes. Understanding how Terraform-like systems work internally is essential for building, extending, or debugging IaC platforms at scale.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|-------------|--------|
| FR-1 | Resource Definitions | HCL or YAML syntax to declare infrastructure resources |
| FR-2 | Execution Plan (plan) | Diff between desired state and current state; show what will change |
| FR-3 | Apply | Create, update, or delete resources to match desired state |
| FR-4 | State File | Persistent record of all managed resources and their current attributes |
| FR-5 | State Locking | Prevent concurrent modifications to state (DynamoDB, PostgreSQL advisory locks) |
| FR-6 | Dependency Graph | Topological sort of resources for correct ordering (create VPC before subnet) |
| FR-7 | Provider Plugins | Pluggable architecture: AWS, GCP, OpenStack, bare-metal, Kubernetes providers |
| FR-8 | Import | Bring existing (manually created) resources under IaC management |
| FR-9 | Drift Detection | Periodic comparison of state file vs actual infrastructure; detect manual changes |
| FR-10 | Module System | Reusable, composable infrastructure components with inputs/outputs |
| FR-11 | Workspaces | Multiple environments (dev/staging/prod) from the same configuration |
| FR-12 | Remote Execution | Run plan/apply in a controlled server environment (Terraform Cloud-like) |
| FR-13 | Destroy | Tear down all managed resources in reverse dependency order |
| FR-14 | Output Values | Export resource attributes for use by other systems/modules |
| FR-15 | Variables & Expressions | Input variables, locals, interpolation, conditional logic, iteration |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Plan Performance | < 30s for 100 resources; < 5min for 1,000 resources |
| NFR-2 | Apply Parallelism | Parallel resource creation/updates for independent resources |
| NFR-3 | State File Size | Support up to 100,000 managed resources per workspace |
| NFR-4 | State Consistency | Strong consistency (no partial state writes) |
| NFR-5 | Plugin Startup | Provider plugin initialization < 5s |
| NFR-6 | Audit Trail | Full history of all plan/apply operations |
| NFR-7 | Rollback | Ability to revert to previous state (not infrastructure, but state file) |

### Constraints & Assumptions
- Java implementation (Spring Boot for remote execution service, Jackson for state serialization).
- State stored in S3-compatible storage (MinIO/AWS S3) with DynamoDB/MySQL for locking.
- Provider plugins communicate via gRPC (similar to Terraform's go-plugin but in Java).
- The platform manages: OpenStack VMs, bare-metal servers (via IPMI/Redfish), Kubernetes resources, Ceph storage, MySQL databases, Elasticsearch clusters.
- Target audience: infrastructure engineers and SREs (not application developers).

### Out of Scope
- Application deployment (Helm charts, Kustomize) -- separate tool.
- Configuration management (Ansible, Chef) -- separate tool.
- Policy as code (OPA/Sentinel) -- could be added later as a plan validator.
- Multi-cloud orchestration -- we focus on our internal infrastructure.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| IaC users (infra engineers + SREs) | 500 (subset of 10,000 devs) | 500 |
| Workspaces (environments) | 500 users x 3 envs x 2 projects avg | 3,000 |
| Resources per workspace (avg) | 50 resources | 50 |
| Total managed resources | 3,000 workspaces x 50 | 150,000 |
| Plan operations/day | 500 users x 5 plans avg | 2,500 |
| Apply operations/day | 500 users x 2 applies avg | 1,000 |
| Remote execution runs/day | 80% of applies via remote | 800 |
| Drift detection runs/day | 3,000 workspaces x 1/day | 3,000 |
| Peak concurrent plans | 20% of daily in 2-hour window | ~60 concurrent |
| Peak concurrent applies | 10% of daily in 2-hour window | ~25 concurrent |

### Latency Requirements
| Operation | Target |
|-----------|--------|
| `plan` (50 resources) | < 15s |
| `plan` (500 resources) | < 2min |
| `apply` per resource (avg) | < 30s (depends on provider) |
| `apply` total (50 resources, parallelism=10) | < 3min |
| State read | < 500ms |
| State write | < 1s |
| State lock acquire | < 2s |
| Import (per resource) | < 10s |

### Storage Estimates

| Data | Calculation | Size |
|------|-------------|------|
| State file (50 resources) | 50 x 2KB avg | ~100KB |
| State file (500 resources) | 500 x 2KB avg | ~1MB |
| State file (largest, 5000 resources) | 5000 x 2KB | ~10MB |
| All state files | 3,000 workspaces x 100KB avg | ~300MB |
| State history (30 versions each) | 300MB x 30 | ~9GB |
| Plan outputs (cached 7 days) | 2,500/day x 500KB x 7 | ~8.75GB |
| Audit logs | 3,500 operations/day x 5KB x 365 | ~6.4GB/year |
| Provider plugins (binary cache) | 20 providers x 50MB | ~1GB |
| Total S3 storage | Sum | ~20GB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| State file read/write | 60 concurrent plans x 100KB | ~6MB burst |
| Provider API calls | 60 concurrent x 10 calls/s x 2KB | ~1.2MB/s |
| Plan output to user | 60 concurrent x 50KB | ~3MB burst |
| Plugin download (initial) | 500 users x 50MB (once) | ~25GB total (one-time) |

---

## 3. High Level Architecture

```
+------------------------------------------------------------------+
|                         User Workflow                              |
|                                                                    |
|   $ infra-iac init          (initialize workspace)                |
|   $ infra-iac plan          (show execution plan)                 |
|   $ infra-iac apply         (apply changes)                       |
|   $ infra-iac destroy       (tear down resources)                 |
|   $ infra-iac import <type> <id>  (import existing resource)     |
+----------------------------+-------------------------------------+
                             |
              +--------------v--------------+
              |        CLI / Client         |
              |   (Java CLI or API Client)  |
              +--------------+--------------+
                             |
              +--------------v--------------+
              |   Remote Execution Service  |
              |      (Spring Boot)          |
              |                             |
              |  +------------------------+ |
              |  | Plan Engine            | |
              |  |  - Parse HCL/YAML     | |
              |  |  - Build Dep Graph    | |
              |  |  - Diff State vs      | |
              |  |    Desired            | |
              |  +----------+-------------+ |
              |             |               |
              |  +----------v-------------+ |
              |  | Apply Engine           | |
              |  |  - Topological Walk    | |
              |  |  - Parallel Execution  | |
              |  |  - Retry / Rollback    | |
              |  +----------+-------------+ |
              |             |               |
              |  +----------v-------------+ |
              |  | Provider Manager       | |
              |  |  - Plugin Registry     | |
              |  |  - gRPC to Providers   | |
              |  +----------+-------------+ |
              +--------------+--------------+
                             |
         +-------------------+-------------------+
         |                   |                   |
+--------v--------+ +-------v--------+ +--------v--------+
| State Backend   | | Provider Plugins| | Module Registry |
| (S3 + DynamoDB) | | (gRPC services) | | (S3/Git)        |
|                 | |                 | |                 |
| - State files   | | - OpenStack    | | - Reusable      |
| - State locks   | | - Bare-metal   | |   modules       |
| - State history | | - Kubernetes   | | - Versioned     |
+-----------------+ | - Ceph Storage | +-----------------+
                    | - MySQL        |
                    | - Elasticsearch|
                    +-------+--------+
                            |
              +-------------+-------------+
              |             |             |
      +-------v---+ +------v----+ +------v------+
      | OpenStack | | IPMI/     | | K8s API     |
      | API       | | Redfish   | | Server      |
      +-----------+ +-----------+ +-------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **CLI / Client** | Parses user commands, reads configuration files, invokes local or remote execution |
| **Remote Execution Service** | Runs plan/apply in a controlled environment; provides API for CI/CD integration; manages execution queues |
| **Plan Engine** | Parses HCL/YAML configs, builds dependency graph (DAG), reads current state, computes diff (what to create/update/delete) |
| **Apply Engine** | Walks dependency graph in topological order, creates/updates/deletes resources via providers, updates state file after each operation |
| **Provider Manager** | Loads provider plugins, routes resource operations to correct provider, manages plugin lifecycle |
| **State Backend** | Stores state files (S3), manages state locks (DynamoDB), maintains state version history |
| **Provider Plugins** | Implement CRUD operations for specific infrastructure types (OpenStack, bare-metal, K8s, etc.) via gRPC interface |
| **Module Registry** | Stores and serves reusable infrastructure modules; version management |

### Data Flows

**Plan Flow:**
1. User runs `infra-iac plan`.
2. CLI reads `*.yaml` configuration files from current directory.
3. Parser produces a resource graph (desired state).
4. State backend is consulted: download current state file (with read lock).
5. Provider plugins are initialized; each provider performs `Read` on its resources to get actual current attributes.
6. Plan Engine computes diff: (desired state) vs (current state from providers).
7. Diff is organized by dependency order and presented to user.
8. Plan output is optionally saved to a plan file for subsequent `apply`.

**Apply Flow:**
1. User runs `infra-iac apply` (with or without saved plan).
2. If no saved plan, re-compute plan (same as above).
3. State lock acquired (DynamoDB conditional write).
4. Apply Engine walks the dependency graph topologically.
5. For each resource: call provider plugin (Create/Update/Delete) via gRPC.
6. After each successful operation: update state file incrementally (write to S3).
7. On failure: stop (or continue with `--continue-on-error`); release lock.
8. On success: write final state; release lock; output summary.

---

## 4. Data Model

### Core Entities & Schema

**State File Structure (JSON, stored in S3):**

```json
{
  "version": 4,
  "serial": 42,
  "lineage": "a1b2c3d4-e5f6-7890-abcd-ef1234567890",
  "workspace": "staging",
  "outputs": {
    "api_endpoint": {
      "value": "https://api.staging.internal:8443",
      "type": "string",
      "sensitive": false
    }
  },
  "resources": [
    {
      "module": "",
      "type": "openstack_compute_instance",
      "name": "web_server",
      "provider": "provider.openstack",
      "instances": [
        {
          "index_key": 0,
          "schema_version": 1,
          "attributes": {
            "id": "abc123-def456",
            "name": "web-staging-0",
            "flavor": "m1.large",
            "image": "ubuntu-22.04",
            "network": {
              "fixed_ip": "10.0.1.42",
              "floating_ip": "203.0.113.42"
            },
            "security_groups": ["default", "web"],
            "metadata": {
              "managed_by": "infra-iac",
              "workspace": "staging"
            }
          },
          "dependencies": [
            "openstack_networking_secgroup.web",
            "openstack_networking_subnet.internal"
          ]
        }
      ]
    },
    {
      "module": "",
      "type": "baremetal_server",
      "name": "gpu_training",
      "provider": "provider.baremetal",
      "instances": [
        {
          "index_key": 0,
          "schema_version": 1,
          "attributes": {
            "id": "gpu-rack3-srv01",
            "hostname": "gpu-rack3-srv01",
            "server_type": "gpu-h100-8x",
            "ipmi_ip": "10.0.100.1",
            "os_image": "ubuntu-22.04-cuda-12.2",
            "network_ip": "10.0.5.42",
            "reservation_id": "res-7f8a9b0c"
          },
          "dependencies": []
        }
      ]
    }
  ]
}
```

**State Lock Table (DynamoDB / MySQL):**

```sql
-- MySQL implementation of state lock
CREATE TABLE state_locks (
    lock_id         VARCHAR(255) PRIMARY KEY,      -- workspace identifier
    owner           VARCHAR(255) NOT NULL,          -- user/process that holds the lock
    operation       VARCHAR(50) NOT NULL,           -- 'plan', 'apply', 'destroy'
    created_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at      TIMESTAMP NOT NULL,             -- TTL for stale lock cleanup
    info            JSON,                           -- additional context (hostname, PID)
    version         BIGINT NOT NULL DEFAULT 0       -- optimistic locking
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- DynamoDB equivalent:
-- Table: infra-iac-locks
-- Partition Key: LockID (string)
-- Attributes: Owner, Operation, CreatedAt, ExpiresAt, Info
-- Conditional write: attribute_not_exists(LockID) OR ExpiresAt < :now
```

**Configuration File Schema (YAML, user-authored):**

```yaml
# infra.yaml - workspace configuration
workspace:
  name: staging
  backend:
    type: s3
    config:
      bucket: infra-iac-state
      key: "projects/ml-team/staging/terraform.tfstate"
      region: us-east-1
      dynamodb_table: infra-iac-locks
      encrypt: true

providers:
  openstack:
    auth_url: https://openstack.internal:5000/v3
    tenant_name: ml-team
    credentials_from: vault://secret/openstack/ml-team
  baremetal:
    api_endpoint: https://bm-api.internal:8443
    credentials_from: vault://secret/baremetal/admin
  kubernetes:
    kubeconfig: ~/.kube/config
    context: staging-cluster

# Resource definitions
resources:
  - type: openstack_compute_instance
    name: web_server
    count: 3
    provider: openstack
    config:
      flavor: m1.large
      image: ubuntu-22.04
      network: internal
      security_groups:
        - default
        - web
      metadata:
        environment: staging

  - type: baremetal_server
    name: gpu_training
    provider: baremetal
    config:
      server_type: gpu-h100-8x
      os_image: ubuntu-22.04-cuda-12.2
      project: ml-team

  - type: kubernetes_namespace
    name: ml_namespace
    provider: kubernetes
    depends_on:
      - openstack_compute_instance.web_server
    config:
      name: ml-staging
      labels:
        team: ml
        environment: staging

modules:
  - name: ml_training_stack
    source: registry://modules/ml-training-stack
    version: "2.1.0"
    inputs:
      gpu_count: 4
      gpu_type: h100
      cluster_name: ml-staging
      storage_size_tb: 10

variables:
  environment:
    type: string
    default: staging
  gpu_count:
    type: number
    description: "Number of GPU servers to provision"

outputs:
  api_endpoint:
    value: "${openstack_compute_instance.web_server[0].network.floating_ip}"
  gpu_servers:
    value: "${baremetal_server.gpu_training.*.hostname}"
```

**Execution History (MySQL):**

```sql
CREATE TABLE execution_history (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    workspace_id    VARCHAR(255) NOT NULL,
    operation       ENUM('plan','apply','destroy','import','refresh') NOT NULL,
    status          ENUM('pending','running','succeeded','failed','cancelled') NOT NULL,
    user_id         VARCHAR(255) NOT NULL,
    plan_summary    JSON,          -- { "add": 3, "change": 1, "destroy": 0 }
    state_serial_before BIGINT,
    state_serial_after  BIGINT,
    error_message   TEXT,
    started_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    completed_at    TIMESTAMP NULL,
    duration_ms     BIGINT,
    INDEX idx_exec_workspace (workspace_id, started_at),
    INDEX idx_exec_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE execution_resource_changes (
    id              BIGINT PRIMARY KEY AUTO_INCREMENT,
    execution_id    BIGINT NOT NULL REFERENCES execution_history(id),
    resource_address VARCHAR(500) NOT NULL,  -- e.g., "openstack_compute_instance.web_server[0]"
    action          ENUM('create','update','delete','read','no-op') NOT NULL,
    provider        VARCHAR(100) NOT NULL,
    before_json     JSON,
    after_json      JSON,
    error_message   TEXT,
    duration_ms     BIGINT,
    INDEX idx_erc_execution (execution_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Store | Use Case | Justification |
|-------|----------|---------------|
| S3 (MinIO) | State files, plan outputs, module archives | Object storage with versioning; cheap; supports encryption at rest |
| DynamoDB / MySQL | State locks | Conditional writes for distributed locking; TTL for stale lock cleanup |
| MySQL 8.0 | Execution history, workspace metadata, user settings | Relational queries on execution history; joins for audit reports |
| Git (optional) | Module source, config version control | Natural versioning; code review for IaC changes |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| execution_history | (workspace_id, started_at) | List runs for a workspace in chronological order |
| execution_history | (user_id) | Audit: who ran what |
| execution_resource_changes | (execution_id) | Show resource changes for a specific run |
| state_locks | PRIMARY (lock_id) | Lock lookup by workspace |

---

## 5. API Design

### REST Endpoints (Remote Execution Service)

**Base URL:** `https://iac.infra.company.com/api/v1`

**Authentication:** Bearer JWT (same auth as portal).

**Rate Limiting:** 10 plan/apply operations per user per hour; 100 read operations per minute.

#### Workspaces

```
GET /workspaces
  Query: ?project=ml-team&page=1&size=50
  Response 200:
  {
    "items": [
      {
        "id": "ws-abc123",
        "name": "staging",
        "project": "ml-team",
        "backend": { "type": "s3", "bucket": "infra-iac-state" },
        "resource_count": 47,
        "last_apply": "2026-04-08T15:30:00Z",
        "state_serial": 42,
        "locked": false
      }
    ],
    "total": 12
  }

POST /workspaces
  Body:
  {
    "name": "production",
    "project": "ml-team",
    "backend": {
      "type": "s3",
      "config": {
        "bucket": "infra-iac-state",
        "key": "projects/ml-team/production/terraform.tfstate",
        "dynamodb_table": "infra-iac-locks"
      }
    }
  }
  Response 201: { workspace object }

GET /workspaces/{id}
  Response 200: { detailed workspace object + recent runs }
```

#### Runs (Plan/Apply)

```
POST /workspaces/{id}/runs
  Body:
  {
    "operation": "plan",       // or "apply", "destroy"
    "config_tarball_url": "s3://iac-configs/ml-team/staging/config-v42.tar.gz",
    "variables": {
      "gpu_count": 8,
      "environment": "staging"
    },
    "auto_apply": false,       // if true, apply automatically after plan succeeds
    "parallelism": 10
  }
  Response 202:
  {
    "run_id": "run-xyz789",
    "status": "pending",
    "workspace_id": "ws-abc123",
    "operation": "plan",
    "queued_at": "2026-04-09T10:00:00Z"
  }

GET /workspaces/{id}/runs/{run_id}
  Response 200:
  {
    "run_id": "run-xyz789",
    "status": "planned",
    "operation": "plan",
    "plan_summary": {
      "add": 3,
      "change": 1,
      "destroy": 0,
      "unchanged": 43
    },
    "plan_output_url": "s3://iac-plans/run-xyz789/plan.json",
    "resource_changes": [
      {
        "address": "openstack_compute_instance.web_server[3]",
        "action": "create",
        "provider": "openstack",
        "after": { "flavor": "m1.large", "image": "ubuntu-22.04" }
      },
      {
        "address": "openstack_compute_instance.web_server[0]",
        "action": "update",
        "provider": "openstack",
        "before": { "security_groups": ["default"] },
        "after": { "security_groups": ["default", "web"] }
      }
    ],
    "started_at": "2026-04-09T10:00:05Z",
    "completed_at": "2026-04-09T10:00:18Z",
    "duration_ms": 13000
  }

POST /workspaces/{id}/runs/{run_id}/apply
  Auth: requires approval if cost > threshold
  Body: { "comment": "Approved by @alice" }
  Response 202:
  {
    "run_id": "run-xyz789",
    "status": "applying",
    "message": "Apply started. Monitor progress via GET /runs/{run_id}"
  }

POST /workspaces/{id}/runs/{run_id}/cancel
  Response 200:
  {
    "run_id": "run-xyz789",
    "status": "cancelled"
  }

GET /workspaces/{id}/runs/{run_id}/logs
  Query: ?follow=true
  Response 200 (streaming):
  [2026-04-09T10:00:05Z] openstack_compute_instance.web_server[3]: Creating...
  [2026-04-09T10:00:12Z] openstack_compute_instance.web_server[3]: Created (id: abc-456)
  [2026-04-09T10:00:12Z] openstack_compute_instance.web_server[0]: Updating security_groups...
  [2026-04-09T10:00:15Z] openstack_compute_instance.web_server[0]: Updated
  ...
```

#### State

```
GET /workspaces/{id}/state
  Response 200: { state file JSON }

GET /workspaces/{id}/state/versions
  Response 200:
  {
    "versions": [
      { "serial": 42, "created_at": "2026-04-08T15:30:00Z", "user": "alice@company.com" },
      { "serial": 41, "created_at": "2026-04-07T10:00:00Z", "user": "bob@company.com" }
    ]
  }

GET /workspaces/{id}/state/versions/{serial}
  Response 200: { historical state file JSON }

POST /workspaces/{id}/state/lock
  Body: { "operation": "apply", "info": { "hostname": "ci-runner-01" } }
  Response 200: { "lock_id": "lock-abc", "acquired": true }
  Response 409: { "error": "state locked", "locked_by": "bob@company.com", "locked_at": "..." }

DELETE /workspaces/{id}/state/lock
  Response 200: { "released": true }

POST /workspaces/{id}/state/lock/force-unlock
  Auth: admin only
  Response 200: { "released": true, "previous_owner": "bob@company.com" }
```

#### Import

```
POST /workspaces/{id}/import
  Body:
  {
    "resource_address": "openstack_compute_instance.legacy_server",
    "provider": "openstack",
    "provider_id": "abc123-def456"
  }
  Response 200:
  {
    "imported": true,
    "resource_address": "openstack_compute_instance.legacy_server",
    "attributes": {
      "id": "abc123-def456",
      "name": "legacy-web-01",
      "flavor": "m1.xlarge",
      "image": "centos-7"
    },
    "message": "Resource imported. Add resource block to your config to manage it."
  }
```

#### Drift Detection

```
POST /workspaces/{id}/drift-check
  Response 202:
  {
    "check_id": "drift-abc123",
    "status": "running"
  }

GET /workspaces/{id}/drift-check/{check_id}
  Response 200:
  {
    "check_id": "drift-abc123",
    "status": "completed",
    "drift_detected": true,
    "drifted_resources": [
      {
        "address": "openstack_compute_instance.web_server[0]",
        "attribute": "security_groups",
        "expected": ["default", "web"],
        "actual": ["default", "web", "debug"],
        "drift_type": "attribute_changed"
      }
    ],
    "checked_at": "2026-04-09T10:00:00Z",
    "total_resources": 47,
    "drifted_count": 1
  }
```

### CLI Design

```bash
# Initialize workspace
$ infra-iac init
Initializing workspace...
  Backend: s3 (bucket: infra-iac-state)
  Providers: openstack, baremetal, kubernetes
  Downloading provider plugins...
    - openstack v2.4.0 (45MB)
    - baremetal v1.1.0 (32MB)
    - kubernetes v3.2.0 (38MB)
  ✓ Workspace initialized

# Plan
$ infra-iac plan
Refreshing state... (47 resources)
  openstack_compute_instance.web_server[0]: Refreshing...
  openstack_compute_instance.web_server[1]: Refreshing...
  ...

Execution Plan:
  # openstack_compute_instance.web_server[3] will be created
  + resource "openstack_compute_instance" "web_server[3]" {
      + flavor          = "m1.large"
      + image           = "ubuntu-22.04"
      + name            = "web-staging-3"
      + network.fixed_ip = (known after apply)
      + security_groups  = ["default", "web"]
    }

  # openstack_compute_instance.web_server[0] will be updated in-place
  ~ resource "openstack_compute_instance" "web_server[0]" {
        name            = "web-staging-0"
      ~ security_groups = ["default"] -> ["default", "web"]
    }

Plan: 1 to add, 1 to change, 0 to destroy.

$ infra-iac plan --out=plan.bin
(saves plan to file for later apply)

# Apply
$ infra-iac apply
(re-runs plan, then)
Do you want to apply these changes? (yes/no): yes

openstack_compute_instance.web_server[3]: Creating...
openstack_compute_instance.web_server[3]: Created (id: xyz-789) [12s]
openstack_compute_instance.web_server[0]: Modifying security_groups...
openstack_compute_instance.web_server[0]: Modified [3s]

Apply complete! Resources: 1 added, 1 changed, 0 destroyed.

Outputs:
  api_endpoint = "203.0.113.42"

# Apply saved plan (no re-plan, no confirmation needed)
$ infra-iac apply plan.bin

# Destroy
$ infra-iac destroy
Plan: 0 to add, 0 to change, 47 to destroy.

Do you want to destroy all resources? Type "yes" to confirm: yes

openstack_compute_instance.web_server[2]: Destroying...
openstack_compute_instance.web_server[1]: Destroying...
openstack_compute_instance.web_server[0]: Destroying...
...

Destroy complete! 47 resources destroyed.

# Import existing resource
$ infra-iac import openstack_compute_instance.legacy_server abc123-def456
Importing...
  openstack_compute_instance.legacy_server: Imported (id: abc123-def456)
  Attributes read from OpenStack:
    name: legacy-web-01
    flavor: m1.xlarge
    image: centos-7

Import successful. Add the following to your config:

  - type: openstack_compute_instance
    name: legacy_server
    config:
      flavor: m1.xlarge
      image: centos-7
      name: legacy-web-01

# Drift detection
$ infra-iac drift-check
Checking for drift... (47 resources)

Drift detected in 1 resource:

  ~ openstack_compute_instance.web_server[0]
      security_groups: ["default", "web"] -> ["default", "web", "debug"]
      (Manual change detected: "debug" security group added outside IaC)

Actions:
  1. Apply to revert drift: infra-iac apply (removes "debug" group)
  2. Update config to accept drift: add "debug" to security_groups in config
  3. Ignore drift: infra-iac drift-check --ignore web_server[0].security_groups

# Workspace management
$ infra-iac workspace list
  NAME        RESOURCES   LAST APPLY          STATE SERIAL
  * staging   47          2026-04-08T15:30    42
    production 52         2026-04-07T10:00    38
    dev        15         2026-04-09T09:00    67

$ infra-iac workspace select production
Switched to workspace "production"

# State management
$ infra-iac state list
openstack_compute_instance.web_server[0]
openstack_compute_instance.web_server[1]
openstack_compute_instance.web_server[2]
baremetal_server.gpu_training
kubernetes_namespace.ml_namespace
...

$ infra-iac state show openstack_compute_instance.web_server[0]
# openstack_compute_instance.web_server[0]
id          = "abc123-def456"
name        = "web-staging-0"
flavor      = "m1.large"
image       = "ubuntu-22.04"
network     = {
  fixed_ip    = "10.0.1.42"
  floating_ip = "203.0.113.42"
}
security_groups = ["default", "web"]

$ infra-iac state rm openstack_compute_instance.web_server[2]
Removed openstack_compute_instance.web_server[2] from state.
Note: The actual resource was NOT destroyed. It is now unmanaged.
```

---

## 6. Core Component Deep Dives

### 6.1 Dependency Graph & Execution Engine

**Why it's hard:** Resources have complex interdependencies (subnet depends on VPC, VM depends on subnet and security group). The engine must compute a valid execution order, maximize parallelism for independent resources, handle cycles (which indicate config errors), and manage partial failures (some resources created, others failed).

| Approach | Pros | Cons |
|----------|------|------|
| **Sequential execution (one at a time)** | Simple, predictable | Extremely slow for large configs |
| **Topological sort + parallel levels** | Correct ordering + parallelism within each level | Suboptimal: waits for slowest in each level |
| **Topological sort + semaphore-bounded parallel** | Maximum parallelism; respects dependencies | Complex tracking of completion; error propagation |
| **Event-driven (resource completes -> dependents start)** | Optimal parallelism; no wasted time | Most complex; hardest to debug |

**Selected approach: Topological sort + event-driven parallel execution with configurable concurrency limit.**

**Implementation:**

```java
public class DependencyGraph {
    private final Map<String, ResourceNode> nodes = new LinkedHashMap<>();
    private final Map<String, Set<String>> edges = new HashMap<>();       // resource -> depends_on
    private final Map<String, Set<String>> reverseEdges = new HashMap<>(); // resource -> depended_by

    public void addResource(String address, ResourceConfig config) {
        nodes.put(address, new ResourceNode(address, config));
        edges.put(address, new HashSet<>(config.getDependsOn()));
        for (String dep : config.getDependsOn()) {
            reverseEdges.computeIfAbsent(dep, k -> new HashSet<>()).add(address);
        }
    }

    /**
     * Detect cycles using DFS. A cycle means invalid configuration.
     */
    public Optional<List<String>> detectCycle() {
        Set<String> visited = new HashSet<>();
        Set<String> inStack = new HashSet<>();
        List<String> path = new ArrayList<>();

        for (String node : nodes.keySet()) {
            if (dfs(node, visited, inStack, path)) {
                return Optional.of(path);
            }
        }
        return Optional.empty();
    }

    /**
     * Topological sort using Kahn's algorithm.
     * Returns execution order (resources with no deps first).
     */
    public List<List<String>> topologicalLevels() {
        Map<String, Integer> inDegree = new HashMap<>();
        for (String node : nodes.keySet()) {
            inDegree.put(node, edges.getOrDefault(node, Set.of()).size());
        }

        List<List<String>> levels = new ArrayList<>();
        Queue<String> ready = new LinkedList<>();

        // Level 0: resources with no dependencies
        for (Map.Entry<String, Integer> entry : inDegree.entrySet()) {
            if (entry.getValue() == 0) {
                ready.add(entry.getKey());
            }
        }

        while (!ready.isEmpty()) {
            List<String> currentLevel = new ArrayList<>(ready);
            levels.add(currentLevel);
            ready.clear();

            for (String node : currentLevel) {
                for (String dependent : reverseEdges.getOrDefault(node, Set.of())) {
                    int newDegree = inDegree.get(dependent) - 1;
                    inDegree.put(dependent, newDegree);
                    if (newDegree == 0) {
                        ready.add(dependent);
                    }
                }
            }
        }

        return levels;
    }
}

/**
 * Event-driven parallel executor.
 * Starts resources as soon as all their dependencies are satisfied.
 */
public class ParallelApplyExecutor {
    private final DependencyGraph graph;
    private final ProviderManager providers;
    private final StateManager state;
    private final Semaphore concurrencyLimit;  // e.g., 10 concurrent operations
    private final Map<String, CompletableFuture<ResourceResult>> futures = new ConcurrentHashMap<>();
    private final Set<String> completed = ConcurrentHashMap.newKeySet();
    private final Set<String> failed = ConcurrentHashMap.newKeySet();

    public ApplyResult execute(ExecutionPlan plan) {
        // Start resources with no dependencies
        for (String address : graph.getRoots()) {
            scheduleResource(address, plan.getAction(address));
        }

        // Wait for all to complete
        CompletableFuture.allOf(futures.values().toArray(new CompletableFuture[0]))
            .exceptionally(ex -> null)  // Don't fail-fast; collect all results
            .join();

        return buildResult();
    }

    private void scheduleResource(String address, ResourceAction action) {
        CompletableFuture<ResourceResult> future = CompletableFuture
            .supplyAsync(() -> {
                concurrencyLimit.acquire();
                try {
                    return executeResourceAction(address, action);
                } finally {
                    concurrencyLimit.release();
                }
            })
            .thenApply(result -> {
                if (result.isSuccess()) {
                    completed.add(address);
                    state.updateResource(address, result.getAttributes());
                    // Schedule dependents whose dependencies are all satisfied
                    for (String dependent : graph.getDependents(address)) {
                        if (completed.containsAll(graph.getDependencies(dependent))) {
                            scheduleResource(dependent, plan.getAction(dependent));
                        }
                    }
                } else {
                    failed.add(address);
                    // Skip all transitive dependents
                    skipDependents(address);
                }
                return result;
            });

        futures.put(address, future);
    }
}
```

**Destroy order:** Reverse topological order (delete dependents before their dependencies).

**Failure modes:**
- **Cycle in dependency graph:** Detected before execution begins; error with cycle path shown.
- **Resource creation fails:** Mark failed; skip all transitive dependents; report partial apply.
- **Provider timeout:** Each resource operation has a configurable timeout (default 5min). On timeout, mark as failed; state reflects the pre-operation state (not partially created).
- **State write fails after resource creation:** Resource is "orphaned" (exists in provider but not in state). Drift detection will catch it; `import` can bring it back under management.

**Interviewer Q&As:**

**Q1: How do you handle implicit dependencies (e.g., a VM references a subnet by ID)?**
A: Two types of dependencies: (1) Explicit: `depends_on` in config. (2) Implicit: detected by expression analysis. When a resource's config references another resource's attribute (e.g., `${openstack_networking_subnet.internal.id}`), the parser automatically adds an edge in the dependency graph. This is computed during the parse phase before planning.

**Q2: What's the maximum parallelism you'd use?**
A: Configurable via `--parallelism` flag (default 10). The limit prevents overwhelming provider APIs. For OpenStack, we typically set 5-10 (Nova API rate limits). For Kubernetes, we can go higher (20-30) since the API server handles concurrency well. For bare-metal (IPMI), we limit to 3-5 because IPMI controllers are slow.

**Q3: How do you handle "create before destroy" for replacements?**
A: When a resource must be replaced (e.g., immutable attribute changed), the default is "destroy then create." But for zero-downtime, we support "create before destroy" mode: (1) Create new resource. (2) Update dependents to point to new resource. (3) Destroy old resource. This is opt-in per resource because it temporarily doubles resource usage.

**Q4: What happens if the user kills the apply process mid-execution?**
A: (1) State reflects only completed operations (state is written after each successful resource operation, not batched). (2) The state lock is held until the process exits (or lock TTL expires). (3) Subsequent `plan` will show the delta between the partially-applied state and desired state. (4) `apply` will complete the remaining operations.

**Q5: How does the destroy order work for resources with count > 1?**
A: Resources with `count: 3` create three instances: `[0]`, `[1]`, `[2]`. Each instance is an independent node in the dependency graph. Destroy order respects inter-resource dependencies but instances of the same resource can be destroyed in parallel.

**Q6: How do you visualize the dependency graph for debugging?**
A: `infra-iac graph` outputs DOT format (Graphviz). Example: `infra-iac graph | dot -Tpng > graph.png`. The remote execution UI also shows an interactive graph visualization with resource status (pending/creating/created/failed) colored.

---

### 6.2 State Management & Locking

**Why it's hard:** The state file is the source of truth for what infrastructure exists. Corruption or concurrent modification can lead to orphaned resources (exist but untracked), duplicate creation (state says resource doesn't exist but it does), or data loss. State must be strongly consistent, durable, and support concurrent access from multiple users.

| Approach | Pros | Cons |
|----------|------|------|
| **Local file** | Simple, fast | No sharing, no locking, no backup |
| **Git-backed state** | Version history, review | Merge conflicts, not transactional |
| **S3 + DynamoDB lock** | Durable, shared, lockable, versioned (S3 versioning) | AWS dependency; eventual consistency risk (S3 is read-after-write consistent now) |
| **PostgreSQL/MySQL** | Strong consistency, transactional, advisory locks | Operational overhead of DB; blob storage for large states |
| **Consul / etcd** | Strong consistency, distributed locks | Operational overhead; size limits (etcd: 1.5MB per value) |

**Selected approach: S3 (MinIO) for state storage + MySQL for locking.**

**Justification:** S3 provides cheap, durable, versioned object storage (we already run MinIO). MySQL provides strong consistency for locks (we already run MySQL). This avoids introducing DynamoDB (AWS-specific) while giving us the same semantics.

**Implementation:**

```java
public class S3StateBackend implements StateBackend {
    private final MinioClient minioClient;
    private final StateLocker locker;  // MySQL-backed

    @Override
    public StateFile readState(String workspace) {
        try {
            GetObjectResponse response = minioClient.getObject(
                GetObjectArgs.builder()
                    .bucket(stateBucket)
                    .object(stateKey(workspace))
                    .build()
            );
            String json = new String(response.readAllBytes(), StandardCharsets.UTF_8);
            return objectMapper.readValue(json, StateFile.class);
        } catch (ErrorResponseException e) {
            if (e.errorResponse().code().equals("NoSuchKey")) {
                return StateFile.empty(workspace);
            }
            throw new StateReadException("Failed to read state", e);
        }
    }

    @Override
    @Transactional
    public void writeState(String workspace, StateFile state) {
        // Verify we hold the lock
        if (!locker.isLockHeld(workspace)) {
            throw new StateWriteException("Cannot write state: lock not held");
        }

        // Increment serial
        state.setSerial(state.getSerial() + 1);

        byte[] stateBytes = objectMapper.writeValueAsBytes(state);
        minioClient.putObject(
            PutObjectArgs.builder()
                .bucket(stateBucket)
                .object(stateKey(workspace))
                .stream(new ByteArrayInputStream(stateBytes), stateBytes.length, -1)
                .contentType("application/json")
                .build()
        );

        // S3 versioning automatically keeps history
    }

    @Override
    public LockResult acquireLock(String workspace, String owner, String operation) {
        return locker.tryLock(workspace, owner, operation, Duration.ofMinutes(30));
    }

    @Override
    public void releaseLock(String workspace, String owner) {
        locker.unlock(workspace, owner);
    }
}

public class MySQLStateLocker implements StateLocker {

    @Transactional
    public LockResult tryLock(String workspace, String owner, String operation, Duration ttl) {
        // Try to insert lock row (succeeds only if no lock exists)
        try {
            jdbcTemplate.update(
                "INSERT INTO state_locks (lock_id, owner, operation, expires_at) " +
                "VALUES (?, ?, ?, ?) " +
                "ON DUPLICATE KEY UPDATE " +
                "  owner = IF(expires_at < NOW(), VALUES(owner), owner), " +
                "  operation = IF(expires_at < NOW(), VALUES(operation), operation), " +
                "  expires_at = IF(expires_at < NOW(), VALUES(expires_at), expires_at)",
                workspace, owner, operation, Instant.now().plus(ttl)
            );

            // Check if we got the lock
            String currentOwner = jdbcTemplate.queryForObject(
                "SELECT owner FROM state_locks WHERE lock_id = ?",
                String.class, workspace
            );

            if (owner.equals(currentOwner)) {
                return LockResult.acquired();
            } else {
                StateLock existingLock = getLockInfo(workspace);
                return LockResult.denied(existingLock);
            }
        } catch (Exception e) {
            throw new StateLockException("Failed to acquire lock", e);
        }
    }

    @Transactional
    public void unlock(String workspace, String owner) {
        int updated = jdbcTemplate.update(
            "DELETE FROM state_locks WHERE lock_id = ? AND owner = ?",
            workspace, owner
        );
        if (updated == 0) {
            throw new StateLockException("Lock not held by " + owner);
        }
    }

    @Transactional
    public void forceUnlock(String workspace) {
        jdbcTemplate.update("DELETE FROM state_locks WHERE lock_id = ?", workspace);
    }
}
```

**State serialization (Jackson):**

```java
@JsonIgnoreProperties(ignoreUnknown = true)
public class StateFile {
    @JsonProperty("version")
    private int version = 4;

    @JsonProperty("serial")
    private long serial;

    @JsonProperty("lineage")
    private String lineage;  // UUID, unique per workspace, never changes

    @JsonProperty("workspace")
    private String workspace;

    @JsonProperty("outputs")
    private Map<String, OutputValue> outputs;

    @JsonProperty("resources")
    private List<ResourceState> resources;

    // Incremental update: update a single resource without rewriting entire state
    public void updateResource(String address, Map<String, Object> attributes) {
        for (ResourceState rs : resources) {
            for (InstanceState instance : rs.getInstances()) {
                if (instance.getFullAddress().equals(address)) {
                    instance.setAttributes(attributes);
                    return;
                }
            }
        }
        throw new IllegalStateException("Resource not found in state: " + address);
    }
}
```

**Failure modes:**
- **S3 write fails after lock acquired:** State is not updated; lock auto-expires after TTL. Next operation re-reads old state and re-plans. No data loss.
- **Process crashes while holding lock:** Lock TTL (30 min) ensures eventual release. Stale lock is overridden on next lock attempt after TTL. `force-unlock` for immediate recovery.
- **S3 versioning disabled accidentally:** State history lost; current state still valid. Mitigation: automated check that versioning is enabled; backup to secondary bucket.
- **State file too large (>10MB):** Performance degrades. Mitigation: (1) Split into multiple workspaces. (2) Archive terminated resources. (3) Compress state file (gzip).
- **Concurrent state reads during apply:** Safe because S3 reads are strongly consistent (read-after-write). Other users see the most recent committed state.

**Interviewer Q&As:**

**Q1: Why S3 + MySQL instead of just MySQL for everything?**
A: State files can be large (up to 10MB for 5,000 resources). Storing large blobs in MySQL is inefficient and impacts query performance. S3 is designed for large objects, provides built-in versioning (state history for free), and is much cheaper for storage. MySQL handles only the locking (tiny rows) and execution metadata.

**Q2: How do you handle the case where two users run `apply` simultaneously?**
A: The first user to acquire the state lock wins. The second user gets: "Error: state locked by alice@company.com since 2026-04-09T10:00:00Z (operation: apply). Wait for completion or run force-unlock if stale." The lock is exclusive: no concurrent applies on the same workspace.

**Q3: How do you implement state rollback?**
A: S3 versioning stores every state version. `infra-iac state rollback --serial 41` downloads version 41 from S3 version history and sets it as current. WARNING: this only rolls back the state file, not the actual infrastructure. A subsequent `plan` will show the diff between the rolled-back state and actual infrastructure.

**Q4: What happens if the state file says a resource exists but it's been deleted manually?**
A: During `plan`, the refresh step calls the provider's `Read` operation for each resource. If the provider returns "not found," the plan shows the resource as needing recreation. The user can choose to apply (recreate it) or run `state rm` (remove it from state, accepting the deletion).

**Q5: How do you handle sensitive data in state (e.g., database passwords)?**
A: (1) State file is encrypted at rest in S3 (SSE-S3 or SSE-KMS). (2) State file is encrypted in transit (TLS). (3) Sensitive attributes are marked in the provider schema; they're stored in state but redacted in plan/show output. (4) State file access is controlled by IAM policies (only the IaC service account can read/write).

**Q6: How does the state lineage work?**
A: The `lineage` is a UUID generated when a workspace is first initialized. It never changes. Every state file for that workspace has the same lineage. This prevents accidentally writing state from workspace A to workspace B (the backend verifies lineage match before writing). If lineage mismatches, the write is rejected.

---

### 6.3 Provider Plugin Architecture

**Why it's hard:** The IaC platform must support many infrastructure types (OpenStack, bare-metal, K8s, Ceph, MySQL, Elasticsearch) without hardcoding any of them. Providers must be independently versioned, updatable, and isolatable (a buggy provider shouldn't crash the engine). Provider APIs vary wildly in capability and behavior.

| Approach | Pros | Cons |
|----------|------|------|
| **Compiled-in providers** | Simple; no IPC overhead | Monolithic binary; can't update providers independently |
| **Shared libraries (JNI/.so)** | Low IPC overhead | Version conflicts; crash propagation; platform-specific |
| **gRPC subprocess plugins** | Process isolation; language-agnostic; independent versioning | gRPC overhead (~1ms per call); process management complexity |
| **HTTP REST plugins** | Easy to develop; no gRPC tooling needed | Higher latency; less typed interface |

**Selected approach: gRPC subprocess plugins (Terraform's model, adapted for Java).**

**Implementation:**

```protobuf
// provider.proto - Provider Plugin Interface
syntax = "proto3";

service ProviderService {
    // Schema
    rpc GetSchema(Empty) returns (SchemaResponse);

    // Configuration
    rpc Configure(ConfigureRequest) returns (ConfigureResponse);

    // Resource CRUD
    rpc ReadResource(ReadResourceRequest) returns (ReadResourceResponse);
    rpc PlanResourceChange(PlanResourceChangeRequest) returns (PlanResourceChangeResponse);
    rpc ApplyResourceChange(ApplyResourceChangeRequest) returns (ApplyResourceChangeResponse);

    // Import
    rpc ImportResource(ImportResourceRequest) returns (ImportResourceResponse);

    // Validation
    rpc ValidateResourceConfig(ValidateRequest) returns (ValidateResponse);
}

message ReadResourceRequest {
    string type_name = 1;           // e.g., "openstack_compute_instance"
    bytes current_state = 2;        // JSON-encoded current state
}

message ReadResourceResponse {
    bytes new_state = 1;            // JSON-encoded refreshed state
    repeated Diagnostic diagnostics = 2;
}

message ApplyResourceChangeRequest {
    string type_name = 1;
    bytes prior_state = 2;          // state before change
    bytes planned_state = 3;        // desired state after change
    bytes config = 4;               // raw config values
    ChangeAction action = 5;        // CREATE, UPDATE, DELETE
}

message ApplyResourceChangeResponse {
    bytes new_state = 1;            // actual state after change
    repeated Diagnostic diagnostics = 2;
}

enum ChangeAction {
    CREATE = 0;
    UPDATE = 1;
    DELETE = 2;
    NO_OP = 3;
}
```

**Plugin Manager:**

```java
public class ProviderManager {
    private final Map<String, ProviderPlugin> plugins = new ConcurrentHashMap<>();
    private final Path pluginDir;
    private final PluginRegistry registry;

    /**
     * Load and initialize a provider plugin.
     * Plugin is a separate process communicating via gRPC.
     */
    public ProviderPlugin loadProvider(String name, String version) {
        if (plugins.containsKey(name)) {
            return plugins.get(name);
        }

        // Download plugin binary if not cached
        Path pluginBinary = pluginDir.resolve(name).resolve(version).resolve("plugin");
        if (!Files.exists(pluginBinary)) {
            registry.download(name, version, pluginBinary);
            // Verify checksum
            verifyChecksum(pluginBinary, registry.getChecksum(name, version));
        }

        // Start plugin process
        ProcessBuilder pb = new ProcessBuilder(pluginBinary.toString())
            .redirectErrorStream(true);
        Process process = pb.start();

        // Read gRPC port from plugin stdout (first line)
        BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()));
        int grpcPort = Integer.parseInt(reader.readLine().trim());

        // Create gRPC channel
        ManagedChannel channel = ManagedChannelBuilder
            .forAddress("localhost", grpcPort)
            .usePlaintext()  // localhost, no TLS needed
            .build();

        ProviderPlugin plugin = new ProviderPlugin(name, version, process, channel);
        plugins.put(name, plugin);

        return plugin;
    }

    /**
     * Shut down all plugins gracefully.
     */
    public void shutdown() {
        for (ProviderPlugin plugin : plugins.values()) {
            plugin.getChannel().shutdown();
            plugin.getProcess().destroy();
        }
    }
}

/**
 * Example: OpenStack provider plugin implementation.
 * This runs as a separate Java process.
 */
public class OpenStackProvider extends ProviderServiceGrpc.ProviderServiceImplBase {

    private OSClient.OSClientV3 osClient;

    @Override
    public void configure(ConfigureRequest request, StreamObserver<ConfigureResponse> responseObserver) {
        Map<String, String> config = parseConfig(request.getConfig());
        osClient = OSFactory.builderV3()
            .endpoint(config.get("auth_url"))
            .credentials(config.get("username"), config.get("password"),
                         Identifier.byName(config.get("domain")))
            .scopeToProject(Identifier.byName(config.get("tenant_name")))
            .authenticate();

        responseObserver.onNext(ConfigureResponse.newBuilder().build());
        responseObserver.onCompleted();
    }

    @Override
    public void applyResourceChange(ApplyResourceChangeRequest request,
                                     StreamObserver<ApplyResourceChangeResponse> responseObserver) {
        switch (request.getAction()) {
            case CREATE:
                Server server = osClient.compute().servers().boot(
                    ServerCreate.builder()
                        .name(planned.get("name"))
                        .flavor(planned.get("flavor"))
                        .image(planned.get("image"))
                        .networks(List.of(planned.get("network")))
                        .build()
                );
                // Poll until ACTIVE
                waitForStatus(server.getId(), Server.Status.ACTIVE, Duration.ofMinutes(5));
                // Return new state with all attributes
                break;

            case UPDATE:
                // Update mutable attributes (e.g., security groups, metadata)
                break;

            case DELETE:
                osClient.compute().servers().delete(prior.get("id"));
                waitForDeletion(prior.get("id"), Duration.ofMinutes(5));
                break;
        }
    }
}
```

**Provider for Bare-Metal (IPMI/Redfish):**

```java
public class BareMetalProvider extends ProviderServiceGrpc.ProviderServiceImplBase {

    @Override
    public void applyResourceChange(ApplyResourceChangeRequest request,
                                     StreamObserver<ApplyResourceChangeResponse> responseObserver) {
        switch (request.getAction()) {
            case CREATE:
                // 1. Reserve server from inventory
                String serverId = reserveServer(planned.get("server_type"), planned.get("project"));
                // 2. Set boot device to PXE
                ipmiClient.setBootDevice(serverId, BootDevice.PXE);
                // 3. Power cycle
                ipmiClient.powerCycle(serverId);
                // 4. PXE boot server provisions OS image
                waitForProvision(serverId, Duration.ofMinutes(15));
                // 5. Configure networking
                configureNetwork(serverId, planned.get("network_config"));
                // Return state
                break;

            case DELETE:
                // 1. Power off
                ipmiClient.powerOff(prior.get("id"));
                // 2. Release from inventory
                releaseServer(prior.get("id"));
                break;
        }
    }
}
```

**Failure modes:**
- **Plugin process crash:** The gRPC channel detects the disconnection. The engine marks the current resource operation as failed and skips dependents. Plugin is restarted for subsequent resources.
- **Plugin hangs (infinite loop):** Each gRPC call has a deadline (5 min default). On deadline exceeded, the plugin process is killed (SIGKILL) and the operation fails.
- **Plugin version incompatibility:** The `GetSchema` RPC returns the plugin's protocol version. If incompatible with the engine, initialization fails with a clear error.
- **Network issues between engine and plugin:** N/A -- communication is via localhost gRPC (no network involved).

**Interviewer Q&As:**

**Q1: Why gRPC instead of REST for plugin communication?**
A: (1) Type safety: protobuf schemas enforce the contract. (2) Performance: gRPC is faster than REST for high-frequency calls (we make thousands of calls during a large apply). (3) Bidirectional streaming: useful for long-running operations with progress updates. (4) Code generation: protobuf generates client/server stubs in any language.

**Q2: How do you handle provider authentication securely?**
A: Provider credentials are resolved at runtime from Vault (via `credentials_from: vault://...` in config). The engine fetches credentials from Vault and passes them to the provider's `Configure` RPC. Credentials are never stored in state or plan output.

**Q3: Can providers be written in any language?**
A: Yes. The gRPC interface is language-agnostic. We provide SDKs for Java and Go. A provider can be written in any language that supports gRPC: Python, Rust, TypeScript, etc. The engine doesn't care -- it communicates via the protobuf contract.

**Q4: How do you test a new provider?**
A: Provider testing framework: (1) Unit tests: mock the infrastructure API (e.g., mock OpenStack client). (2) Acceptance tests: run against a real infrastructure (test OpenStack, test K8s cluster). (3) Lifecycle tests: create -> read -> update -> delete cycle for each resource type. (4) Import tests: create resource manually, import it, verify state matches.

**Q5: How do you handle provider rate limiting (e.g., OpenStack API rate limits)?**
A: The `--parallelism` flag limits concurrent operations per provider. Additionally, each provider plugin can implement internal rate limiting (token bucket) for its API calls. The provider returns a retryable error (gRPC `RESOURCE_EXHAUSTED`) and the engine retries with backoff.

**Q6: How do you add support for a new resource type in an existing provider?**
A: Add a new case to the provider's `GetSchema` (declaring the new type), `Read`, `Plan`, and `Apply` handlers. Release a new provider version. Users update their provider version in workspace config. Existing resources are unaffected.

---

### 6.4 Plan Engine (Diff Computation)

**Why it's hard:** The plan must accurately compute the minimal set of changes to transition from current state to desired state. It must handle: attribute-level diffs, immutable vs mutable attributes (changing an immutable attribute forces replacement), computed attributes (values only known after creation), and resource count changes.

| Approach | Pros | Cons |
|----------|------|------|
| **Full object comparison** | Simple | Misses attribute-level semantics (mutable vs immutable) |
| **Schema-aware diff** | Accurate; knows which attributes force replacement | Requires provider schema |
| **Three-way diff (config, state, actual)** | Handles drift | Most complex; three data sources to reconcile |

**Selected approach: Schema-aware three-way diff.**

**Implementation:**

```java
public class PlanEngine {

    /**
     * Compute execution plan: what needs to change.
     */
    public ExecutionPlan computePlan(
            List<ResourceConfig> desiredResources,
            StateFile currentState,
            Map<String, ResourceSchema> providerSchemas) {

        ExecutionPlan plan = new ExecutionPlan();
        Map<String, ResourceState> stateMap = currentState.toMap(); // address -> state

        // Step 1: Refresh -- read actual state from providers
        for (ResourceState rs : currentState.getResources()) {
            ResourceSchema schema = providerSchemas.get(rs.getType());
            Map<String, Object> actual = providers.readResource(rs.getType(), rs.getAttributes());
            rs.setAttributes(actual); // Update state with actual values
        }

        // Step 2: Identify creates, updates, deletes
        Set<String> desiredAddresses = new HashSet<>();

        for (ResourceConfig desired : desiredResources) {
            for (int i = 0; i < desired.getCount(); i++) {
                String address = desired.address(i);
                desiredAddresses.add(address);

                ResourceState current = stateMap.get(address);
                ResourceSchema schema = providerSchemas.get(desired.getType());

                if (current == null) {
                    // Resource doesn't exist -> CREATE
                    plan.addChange(address, ChangeAction.CREATE, null, desired.resolvedConfig(i));
                } else {
                    // Resource exists -> check for changes
                    ResourceDiff diff = computeResourceDiff(desired.resolvedConfig(i),
                                                            current.getAttributes(), schema);
                    if (diff.isEmpty()) {
                        plan.addChange(address, ChangeAction.NO_OP, current.getAttributes(), null);
                    } else if (diff.requiresReplacement()) {
                        // Immutable attribute changed -> REPLACE (delete + create)
                        plan.addChange(address, ChangeAction.REPLACE,
                                       current.getAttributes(), desired.resolvedConfig(i));
                        plan.setReplacementReason(address, diff.getReplacementAttributes());
                    } else {
                        // Mutable attributes changed -> UPDATE
                        plan.addChange(address, ChangeAction.UPDATE,
                                       current.getAttributes(), desired.resolvedConfig(i));
                    }
                }
            }
        }

        // Step 3: Resources in state but not in desired -> DELETE
        for (String address : stateMap.keySet()) {
            if (!desiredAddresses.contains(address)) {
                plan.addChange(address, ChangeAction.DELETE, stateMap.get(address).getAttributes(), null);
            }
        }

        return plan;
    }

    /**
     * Compute diff between desired config and current state for a single resource.
     */
    private ResourceDiff computeResourceDiff(Map<String, Object> desired,
                                              Map<String, Object> current,
                                              ResourceSchema schema) {
        ResourceDiff diff = new ResourceDiff();

        for (String attr : schema.getAttributes()) {
            Object desiredVal = desired.get(attr);
            Object currentVal = current.get(attr);

            if (schema.isComputed(attr) && desiredVal == null) {
                continue; // Computed attribute not set in config, skip
            }

            if (!Objects.equals(desiredVal, currentVal)) {
                AttributeChange change = new AttributeChange(attr, currentVal, desiredVal);
                if (schema.isForceNew(attr)) {
                    change.setForceReplacement(true);
                }
                diff.addChange(change);
            }
        }

        return diff;
    }
}
```

**Plan output format:**

```
# openstack_compute_instance.web_server[3] will be created
+ resource "openstack_compute_instance" "web_server[3]" {
    + flavor          = "m1.large"
    + image           = "ubuntu-22.04"
    + name            = "web-staging-3"
    + network.fixed_ip = (known after apply)    <- computed
    + security_groups  = ["default", "web"]
  }

# openstack_compute_instance.web_server[0] will be updated in-place
~ resource "openstack_compute_instance" "web_server[0]" {
      name            = "web-staging-0"           <- unchanged
    ~ security_groups = ["default"] -> ["default", "web"]
  }

# baremetal_server.old_server will be destroyed
- resource "baremetal_server" "old_server" {
    - hostname    = "old-srv-01"
    - server_type = "cpu-epyc-64c"
  }

# openstack_compute_instance.db_server must be replaced (image is immutable)
-/+ resource "openstack_compute_instance" "db_server" {
      ~ image  = "ubuntu-20.04" -> "ubuntu-22.04" (forces replacement)
        flavor = "m1.xlarge"
      ~ id     = "abc-123" -> (known after apply)
    }

Plan: 2 to add, 1 to change, 2 to destroy.
```

**Failure modes:**
- **Provider Read fails during refresh:** Show warning "Failed to refresh X; using stale state." Plan proceeds with stale state (user is warned).
- **Expression evaluation fails:** Error with line number and context: "Error in web_server.yaml line 12: undefined variable 'subnet_id'."
- **Count change with dependencies:** If count decreases (e.g., 3 -> 2), the engine identifies which instances to delete (highest indices first) and ensures their dependents are updated first.

**Interviewer Q&As:**

**Q1: How do you handle "(known after apply)" values in the plan?**
A: Computed attributes (like IP addresses, generated IDs) are marked in the provider schema as `computed: true`. The plan shows these as "(known after apply)" because their values are only determined when the resource is created. During apply, these values are populated and stored in state.

**Q2: What's the difference between "refresh" and "plan"?**
A: Refresh reads actual state from providers and updates the state file. Plan computes the diff between desired config and (refreshed) state. By default, `plan` includes a refresh step. `plan --refresh=false` skips refresh and uses only the stored state (faster but may be stale).

**Q3: How do you handle a resource that's in state but the provider returns an error on Read?**
A: We distinguish between "not found" (resource deleted externally) and "error" (API failure). Not found: plan shows resource as needing recreation. Error: warning shown; plan uses stale state for that resource and proceeds. The user can re-run `plan` when the provider is healthy.

**Q4: How do you handle attribute ordering (e.g., security groups as a set, not a list)?**
A: The provider schema specifies whether an attribute is a "set" (order-insensitive) or "list" (order-sensitive). For sets, the diff engine sorts both sides before comparison. `["web", "default"]` and `["default", "web"]` are considered equal for set attributes.

**Q5: How do you handle a plan that was computed hours ago -- is it still valid?**
A: A saved plan is serialized with a timestamp and state serial. At apply time, we verify the current state serial matches the plan's recorded serial. If not, the plan is stale (another apply happened in between) and must be re-computed.

**Q6: How does the plan handle modules?**
A: Modules are expanded during the parse phase. Each module resource gets a fully qualified address like `module.ml_training.baremetal_server.gpu[0]`. From the plan engine's perspective, expanded module resources are treated identically to top-level resources.

---

## 7. Scheduling & Resource Management

### Remote Execution Scheduling

The remote execution service manages a queue of plan/apply operations:

1. **Queue:** When a user submits a run (via API or CLI), it enters a FIFO queue per workspace.
2. **Concurrency:** Only one run per workspace at a time (enforced by state lock).
3. **Global concurrency:** Maximum 50 concurrent runs across all workspaces (to protect provider APIs).
4. **Priority:** Destroy operations get priority (freeing resources). Apply operations next. Plans are lowest priority.

```java
@Service
public class RunScheduler {
    private final PriorityBlockingQueue<RunRequest> globalQueue;
    private final Semaphore globalConcurrency = new Semaphore(50);
    private final Map<String, Lock> workspaceLocks = new ConcurrentHashMap<>();

    @Scheduled(fixedDelay = 1000)  // Check every second
    public void processQueue() {
        while (!globalQueue.isEmpty() && globalConcurrency.tryAcquire()) {
            RunRequest run = globalQueue.poll();
            if (run == null) {
                globalConcurrency.release();
                break;
            }

            Lock wsLock = workspaceLocks.computeIfAbsent(run.getWorkspaceId(), k -> new ReentrantLock());
            if (wsLock.tryLock()) {
                executor.submit(() -> {
                    try {
                        executeRun(run);
                    } finally {
                        wsLock.unlock();
                        globalConcurrency.release();
                    }
                });
            } else {
                // Workspace busy, re-queue
                globalQueue.add(run);
                globalConcurrency.release();
            }
        }
    }
}
```

### Drift Detection Scheduling

Drift detection runs as a background scheduled job:

```java
@Component
public class DriftDetectionScheduler {

    @Scheduled(cron = "0 0 2 * * *")  // Daily at 2 AM
    public void runDriftDetection() {
        List<Workspace> workspaces = workspaceRepo.findAll();

        for (Workspace ws : workspaces) {
            driftDetectionQueue.submit(new DriftCheckJob(ws));
        }
    }
}
```

### Resource Lifecycle

The IaC platform manages resource lifecycle through the state file:
- **Creation:** Resource added to state on successful apply.
- **Update:** State updated with new attributes on successful apply.
- **Deletion:** Resource removed from state on successful destroy.
- **Import:** External resource added to state without provisioning.
- **Orphan:** Resource removed from config but user doesn't want to destroy it -> `state rm` removes from state without destroying.

---

## 8. Scaling Strategy

### Horizontal Scaling

| Component | Scaling Approach |
|-----------|-----------------|
| Remote Execution Service | Horizontal API pods (stateless); worker pods for run execution |
| Plan Engine | CPU-bound; scale by worker pod count |
| Apply Engine | I/O-bound (provider API calls); scale by worker pod count + parallelism setting |
| State Backend (S3) | S3 scales infinitely |
| State Locks (MySQL) | Single MySQL for locks (low write volume); failover for HA |
| Provider Plugins | Plugin processes per worker pod; resource-limited by cgroups |
| Module Registry | S3-backed; CDN for distribution |

### Scaling for Large Workspaces (5,000+ resources)

1. **State file splitting:** Very large workspaces should be split into smaller ones (e.g., network, compute, storage).
2. **Targeted plan/apply:** `infra-iac plan --target=openstack_compute_instance.web_server` to plan only specific resources.
3. **Incremental state updates:** After each resource operation, update only the changed resource in state (not rewrite entire file).
4. **Provider connection pooling:** Reuse provider connections across resource operations.

### Interviewer Q&As

**Q1: How do you handle 100 users running `apply` simultaneously?**
A: Each apply is on a different workspace (different state lock). The global concurrency limit (50) prevents overwhelming provider APIs. Beyond 50 concurrent applies, runs queue. The queue is priority-ordered (destroy > apply > plan). Worker pods auto-scale based on queue depth.

**Q2: What's the bottleneck for very large plans (10,000 resources)?**
A: The refresh step -- reading actual state from providers for 10,000 resources. We optimize by: (1) Parallelizing refresh calls per provider (batch APIs where available, e.g., OpenStack list endpoints). (2) Caching provider reads within a plan (no duplicate calls). (3) `--refresh=false` to skip refresh when the user knows state is current.

**Q3: How do you scale the module registry?**
A: Modules are stored in S3 with CloudFront CDN. Module downloads are cached locally (once per version). The registry itself is a simple metadata service (MySQL-backed) that maps module name + version to S3 path.

**Q4: How do you handle state file growth over time?**
A: Archived states (old versions) are moved to S3 Glacier after 30 days. Only the latest N versions (default: 30) are kept in S3 Standard. The active state file grows only with the number of currently managed resources. Terminated resources are removed from state by the destroy operation.

**Q5: How would you shard the system for multi-region?**
A: State is already naturally sharded by workspace. Each region has its own remote execution service and provider endpoints. A global metadata service tracks which workspace is in which region. Users configure their workspace backend to point to the regional S3 bucket and lock table.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | Recovery |
|---------|--------|-----------|------------|----------|
| S3 (MinIO) down | Can't read/write state | S3 health check | Retry with backoff; fail plan/apply if unavailable | Restart MinIO; state is durable on disk |
| MySQL down | Can't acquire/release locks | Connection health check | Retry; fail plan/apply if lock unavailable | Failover to replica; stale locks expire via TTL |
| Provider API down (OpenStack) | Can't create/modify resources | Provider health check (Configure RPC) | Retry per-resource; mark failed resources in plan output | Wait for provider recovery; re-run apply |
| Plugin process crash | Current resource operation fails | gRPC channel state | Restart plugin; retry current resource | Automatic plugin restart |
| Worker pod OOM | Entire apply fails | K8s OOMKilled event | Set memory limits; split large workspaces | Re-run; apply is idempotent (state tracks progress) |
| State corruption | Incorrect resource tracking | Checksum validation on read | Restore from S3 version history | `state rollback --serial N` |
| Network partition (engine <-> provider) | Resource operations fail | gRPC deadline exceeded | Retry with backoff; fail after N retries | Re-run when network recovers |
| State lock stuck (process crash) | Workspace blocked | Lock age monitoring | TTL-based expiry (30 min); force-unlock command | Auto-expire or admin force-unlock |

### Idempotency Guarantees

Apply operations are idempotent by design:
- **Create:** If resource already exists (detected by provider Read), skip creation.
- **Update:** Apply desired attributes regardless of current state.
- **Delete:** If resource already gone (provider returns not found), skip deletion.
- **State update:** Serial number prevents duplicate state writes.

### Recovery Procedure

If an apply fails mid-execution:
1. State file reflects all completed operations (written after each resource).
2. Lock is released (explicitly or via TTL).
3. Re-run `plan` to see remaining changes.
4. Re-run `apply` to complete. Idempotency ensures already-completed operations are no-ops.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold | Description |
|--------|------|-----------------|-------------|
| `iac.plan.duration.p99` | Histogram | > 5min | Plan computation time |
| `iac.apply.duration.p99` | Histogram | > 30min | Apply execution time |
| `iac.apply.resource_duration.p99` | Histogram | > 5min per resource | Per-resource apply time |
| `iac.run.failure_rate` | Rate | > 10% | Failed plan/apply operations |
| `iac.run.queue_depth` | Gauge | > 20 | Queued runs waiting for execution |
| `iac.state.lock_wait_time.p99` | Histogram | > 5min | Time waiting for state lock |
| `iac.state.lock_stuck` | Gauge | > 0 for 30min | Locks held beyond TTL |
| `iac.drift.detected_count` | Counter | > 10/day | Drift detection findings |
| `iac.provider.error_rate` | Rate per provider | > 5% | Provider API error rate |
| `iac.provider.latency.p99` | Histogram per provider | > 30s | Provider API latency |
| `iac.state.size_bytes` | Gauge | > 5MB | State file size (potential perf issue) |
| `iac.workspace.resource_count` | Gauge | > 1000 | Resources per workspace |
| `iac.plugin.restart_count` | Counter | > 0 | Plugin process restarts (stability issue) |

### Logging

Every plan/apply operation produces structured logs:

```json
{
  "timestamp": "2026-04-09T10:00:12Z",
  "level": "INFO",
  "run_id": "run-xyz789",
  "workspace": "staging",
  "operation": "apply",
  "resource": "openstack_compute_instance.web_server[3]",
  "action": "create",
  "provider": "openstack",
  "duration_ms": 12000,
  "status": "success",
  "provider_id": "abc-789"
}
```

---

## 11. Security

### Auth & AuthZ

- **Remote Execution API:** Same JWT-based auth as the portal (Okta OIDC).
- **CLI (local execution):** User authenticates via `infra-iac login` (OAuth2 device flow) or service account token.
- **Provider credentials:** Fetched from Vault at runtime; never stored in state or config.

### SSO Integration

Same as portal: OIDC with Okta. Remote execution service uses Spring Security OAuth2.

### RBAC Enforcement

| Role | Permissions |
|------|------------|
| `viewer` | Read state, view plan output, view execution history |
| `operator` | All viewer + run plan, run apply, import resources |
| `admin` | All operator + force-unlock, delete workspace, manage provider configs |

RBAC is enforced at the workspace level. Users are assigned roles per workspace (or inherit from project).

### Secrets Management

| Secret Type | Storage | Access |
|-------------|---------|--------|
| Provider credentials (OpenStack, IPMI) | HashiCorp Vault | Fetched at runtime via Vault API |
| State encryption key | Vault | Used by S3 SSE-KMS |
| JWT signing key | Vault | Used by auth service |
| Plugin checksums | Module registry (signed) | Verified on download |

### State Security

- **Encryption at rest:** S3 SSE-KMS (customer-managed key in Vault).
- **Encryption in transit:** TLS for all S3 and MySQL connections.
- **Access control:** S3 bucket policy restricts access to the IaC service account.
- **Sensitive attributes:** Marked in provider schema; redacted in logs and plan output.
- **No secrets in state:** Provider credentials are never written to state. Only resource attributes (IDs, IPs, names) are stored.

---

## 12. Incremental Rollout

### Phase 1: Core Engine (Weeks 1-6)
- YAML config parser.
- Dependency graph builder with cycle detection.
- State file read/write (S3 backend).
- State locking (MySQL).
- OpenStack provider plugin (compute instances only).
- Plan engine (diff computation).
- Apply engine (sequential execution).
- CLI: init, plan, apply, destroy.

### Phase 2: Parallelism & More Providers (Weeks 7-10)
- Parallel apply execution (configurable concurrency).
- Bare-metal provider (IPMI/Redfish).
- Kubernetes provider (namespace, deployment, service).
- Import command.
- State management commands (list, show, rm).

### Phase 3: Remote Execution (Weeks 11-14)
- Remote execution service (Spring Boot).
- REST API for plan/apply.
- Execution queue with concurrency management.
- Execution history and logs.
- Web UI for run visualization.

### Phase 4: Advanced Features (Weeks 15-18)
- Module system (registry, versioning, composition).
- Workspace support (multiple environments).
- Drift detection (scheduled + on-demand).
- Ceph storage provider and MySQL/Elasticsearch providers.
- Policy validation (pre-plan checks).

### Rollout Q&As

**Q1: How do you migrate existing infrastructure to IaC management?**
A: Incremental import. Start with one resource type (e.g., VMs). Run `import` for each existing resource. Validate state matches reality. Gradually add more resource types. Never force users to recreate existing infrastructure.

**Q2: How do you handle the learning curve for a new IaC tool?**
A: (1) YAML instead of HCL (more familiar to most engineers). (2) Comprehensive examples in the module registry. (3) `infra-iac validate` command for config syntax checking without applying. (4) Dry-run mode by default (`plan` before `apply`). (5) Internal workshops and documentation.

**Q3: How do you get buy-in from teams already using Terraform?**
A: We don't replace Terraform. Our IaC platform targets internal infrastructure (bare-metal, internal OpenStack, internal K8s) that Terraform doesn't have providers for. For AWS/GCP, teams continue using Terraform. Over time, if our platform proves valuable, teams may migrate voluntarily.

**Q4: How do you handle the transition period where some resources are IaC-managed and some aren't?**
A: The import command is the bridge. Unmanaged resources can coexist with managed ones. IaC only manages resources in its state file; it never touches resources it doesn't know about. Drift detection can scan for unmanaged resources and suggest imports.

**Q5: How do you test the IaC engine itself?**
A: (1) Unit tests for parser, diff engine, graph algorithms. (2) Integration tests with mock providers (in-memory). (3) Acceptance tests with real OpenStack (test environment). (4) End-to-end: full cycle (init -> plan -> apply -> modify -> plan -> apply -> destroy) against staging infrastructure. (5) Fuzzing: random config generation to test parser robustness.

---

## 13. Trade-offs & Decision Log

| Decision | Chosen | Alternative | Why |
|----------|--------|-------------|-----|
| Config language | YAML | HCL, CUE, Jsonnet | YAML is universally known; avoids custom language learning curve; HCL is more expressive but proprietary |
| State backend | S3 + MySQL lock | PostgreSQL for both, Consul, etcd | S3 cheap for large state files; MySQL for reliable locking; avoids new infrastructure (Consul/etcd) |
| Plugin protocol | gRPC | REST, shared library, WASM | Process isolation + type safety + language agnostic; proven by Terraform |
| Implementation | Java (Spring Boot) | Go, Rust | Organizational expertise; Spring ecosystem (Batch, Security, Data); Jackson for state serialization; Go would be better for CLI binary |
| Diff algorithm | Schema-aware three-way diff | Simple object comparison | Accurate: distinguishes mutable vs immutable attributes; handles computed values |
| Execution model | Event-driven parallel | Level-based parallel, sequential | Maximum parallelism; no wasted time waiting for level completion |
| Remote execution | Custom (Spring Boot) | Terraform Cloud, Atlantis | Full control over provider authentication; integration with internal auth/RBAC; no external dependency |
| Module format | YAML + tar.gz | Helm charts, Docker images | Consistent with config format; simpler than Helm; container overhead unnecessary |
| Drift detection | Scheduled + on-demand | Real-time (controller pattern) | Scheduled is simpler and sufficient; real-time would require constant provider polling |
| State history | S3 versioning | Git-backed state, custom versioning | Free with S3 versioning enabled; no additional infrastructure needed |

---

## 14. Agentic AI Integration

### Natural Language IaC Generation

```
User: "I need to set up a staging environment with 3 web servers behind a load balancer,
       a GPU server for ML inference, and a K8s namespace for batch jobs."

AI Agent generates:

  resources:
    - type: openstack_compute_instance
      name: web_server
      count: 3
      config:
        flavor: m1.large
        image: ubuntu-22.04
        security_groups: [default, web]

    - type: openstack_lb_loadbalancer
      name: web_lb
      depends_on: [openstack_compute_instance.web_server]
      config:
        protocol: HTTP
        members: "${openstack_compute_instance.web_server.*.network.fixed_ip}"

    - type: baremetal_server
      name: ml_inference
      config:
        server_type: gpu-h100-8x
        os_image: ubuntu-22.04-cuda-12.2

    - type: kubernetes_namespace
      name: batch_jobs
      config:
        name: batch-staging
        labels:
          environment: staging
          team: platform

  "I've generated the config. Review it with: infra-iac plan
   Estimated cost: $45/hour ($1,080/day)
   Would you like me to adjust any specifications?"
```

### AI-Assisted Drift Resolution

```
$ infra-iac ask "I have drift on web_server[0] -- security group 'debug' was added. Should I keep it?"

AI Agent:
  "I checked the 'debug' security group rules:
   - Allows SSH (port 22) from 0.0.0.0/0
   - Allows all ICMP

   This is a common debugging configuration. However, it's a security risk in staging:
   - SSH from 0.0.0.0/0 is overly permissive.

   Recommendations:
   1. If you need SSH access, add it to your IaC config with a restricted CIDR:
      security_groups: [default, web, ssh-restricted]

   2. If debugging is complete, remove the drift:
      infra-iac apply  (this will remove the 'debug' group)

   3. If you want to keep it temporarily, acknowledge the drift:
      infra-iac drift-check --ignore web_server[0].security_groups --until 2026-04-16"
```

### AI-Assisted Config Review

Before `apply`, the AI can review the plan for common mistakes:

```
$ infra-iac plan --ai-review

Plan: 1 to add, 0 to change, 0 to destroy.

AI Review:
  ⚠ Warning: openstack_compute_instance.db_server is being created without a
    dedicated security group. It will use the 'default' group which allows all
    intra-project traffic. Consider creating a 'database' security group that
    restricts access to port 3306 only.

  ⚠ Warning: No backup configuration detected for baremetal_server.gpu_training.
    GPU servers should have automated snapshot schedules for data protection.

  ✓ Dependency graph looks correct.
  ✓ Resource naming follows organization conventions.
```

### Predictive Capacity Planning

```
$ infra-iac ask "If I apply this config, will we have enough capacity?"

AI Agent:
  "Analyzing your plan against current capacity:

   Resource         Requested   Available   After Apply
   m1.large VMs     3           45          42
   gpu-h100 servers  1           6           5
   K8s namespaces   1           unlimited   N/A

   Capacity is sufficient. However:
   - GPU servers are at 50% utilization. If 2+ teams request GPUs this week,
     you may face contention.
   - Consider reserving the GPU server for your project window."
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Explain the complete lifecycle of `infra-iac apply` from command invocation to completion.**
A: (1) CLI parses YAML config files from current directory. (2) Parser validates syntax, resolves variables, expands modules, builds resource config list. (3) Dependency graph is constructed from explicit `depends_on` and implicit references. (4) Cycle detection runs (error if cycle found). (5) State lock is acquired (MySQL). (6) Current state is read from S3. (7) Refresh: for each resource in state, provider Read is called to get actual attributes. (8) Plan engine computes diff: desired config vs refreshed state. (9) Plan is presented to user; user confirms. (10) Apply engine walks dependency graph (topological order, parallel execution). (11) For each resource: invoke provider (Create/Update/Delete) via gRPC. (12) After each successful operation: update state file in S3. (13) On completion: write final state, release lock, output summary.

**Q2: How does state locking prevent data corruption?**
A: Before any state modification, the process must acquire an exclusive lock (MySQL INSERT with conditional check). Only one process can hold the lock for a given workspace. The lock includes: owner identity, operation type, and TTL. If a second user tries to apply, they get "state locked" error with details about the current lock holder. TTL prevents deadlocks from crashed processes.

**Q3: What happens if the state file says 3 VMs exist but only 2 actually exist?**
A: During the refresh step, the provider's Read operation is called for all 3 VMs. For the missing VM, Read returns "not found." The plan engine marks this VM as needing recreation (it's in the desired config but not in reality). If the user removed this VM from config, the plan shows it as already destroyed (no action needed), and `apply` removes it from state.

**Q4: How do you implement `count` changes (e.g., scaling from 3 to 5 instances)?**
A: Instances are indexed: `[0]`, `[1]`, `[2]`. Scaling to 5 adds `[3]` and `[4]` as new resources (CREATE actions). Scaling from 5 to 3 destroys `[3]` and `[4]` (highest indices, DELETE actions). Instances `[0]`, `[1]`, `[2]` are untouched (NO_OP). This ensures stable indices for existing resources.

**Q5: How would you implement `terraform taint` (force recreation)?**
A: `infra-iac taint openstack_compute_instance.web_server[0]` marks the resource in state with a `tainted: true` flag. On the next plan, the tainted resource is always planned for replacement (delete + create), regardless of whether its config changed. After successful recreation, the taint flag is cleared.

**Q6: How do you handle circular dependencies in the config?**
A: Cycle detection runs before planning (DFS-based algorithm). If a cycle is found, the user gets an error: "Error: Dependency cycle detected: A -> B -> C -> A. Break the cycle by removing one dependency or restructuring your resources." We never attempt to execute a cyclic graph.

**Q7: How does the module system work?**
A: Modules are packaged as tar.gz archives of YAML config files, with a `module.yaml` manifest declaring inputs (variables) and outputs. When a config references a module, the engine: (1) Downloads the module from the registry (versioned). (2) Expands module resources with the module's prefix. (3) Substitutes input variables. (4) Adds module resources to the dependency graph. Module outputs can be referenced by other resources.

**Q8: How do you handle provider credential rotation without affecting IaC?**
A: Provider credentials are resolved at runtime from Vault (`credentials_from: vault://...`). When credentials are rotated in Vault, the next `plan` or `apply` automatically uses the new credentials. No config or state changes needed. The IaC config never contains credentials.

**Q9: What's the difference between `destroy` and removing resources from config?**
A: Removing resources from config and running `apply` destroys only those removed resources. `destroy` destroys ALL resources in the workspace. Under the hood, `destroy` is equivalent to applying an empty config: every resource in state becomes a DELETE action.

**Q10: How do you handle provider-specific retry logic?**
A: Each provider implements its own retry logic for its API. The engine provides a retry framework via the gRPC protocol: providers return `RETRYABLE` status on transient errors. The engine retries with configurable backoff. Additionally, providers can implement internal retries for known transient failures (e.g., OpenStack 409 Conflict during concurrent operations).

**Q11: How do you ensure state file integrity?**
A: (1) State file includes a SHA-256 checksum of the resources section. (2) On read, checksum is verified. (3) S3 versioning provides an audit trail of all state changes. (4) The `lineage` field prevents accidentally overwriting state from a different workspace. (5) The `serial` number is monotonically increasing and checked on write.

**Q12: How would you add policy enforcement (e.g., "all VMs must have encryption enabled")?**
A: Add a policy validation step between plan and apply. Policies are written as rules (OPA/Rego or custom DSL). The plan output (JSON) is evaluated against policies. If any policy fails, the apply is blocked. Example: `deny[msg] { input.resource_changes[_].after.encryption_enabled == false; msg := "All VMs must have encryption enabled" }`.

**Q13: How does import work for resources that have attributes not in the config?**
A: Import reads ALL attributes from the provider and stores them in state. The user then writes a config that matches the important attributes. On the next plan, the diff engine compares config attributes against imported state. Attributes not in the config but in state are ignored (provider-managed). The plan should show no changes if the config matches reality.

**Q14: How do you test the IaC platform against real infrastructure without affecting production?**
A: (1) Dedicated test infrastructure: a separate OpenStack deployment, test bare-metal servers, test K8s clusters. (2) Acceptance tests create real resources, verify state, then destroy. (3) Tests run in isolated projects/tenants. (4) CI/CD runs acceptance tests on every PR. (5) Tests are parallelized across projects for speed.

**Q15: How would you implement "terraform plan --destroy" (show what would be destroyed)?**
A: `infra-iac plan --destroy` computes the plan as if the desired config is empty. Every resource in state becomes a DELETE action. The destroy plan respects reverse dependency order. This lets the user preview destruction without committing.

**Q16: How do you handle resources that take a long time to create (e.g., 15-minute bare-metal provisioning)?**
A: The apply engine supports async resource creation: (1) Initiate creation (IPMI PXE boot). (2) Return immediately with "creating" status. (3) Poll provider at intervals (every 30s) until resource is ready or timeout. (4) Timeout is configurable per resource type (default 5min for VMs, 20min for bare-metal, 15min for K8s clusters). (5) Other independent resources continue in parallel during the wait.

---

## 16. References

1. **Terraform Architecture** - How Terraform works internally: https://developer.hashicorp.com/terraform/internals
2. **Terraform Provider Protocol** - gRPC plugin interface: https://developer.hashicorp.com/terraform/plugin/framework
3. **OpenStack SDK for Java** - openstack4j: https://openstack4j.github.io/
4. **Kahn's Algorithm** - Topological sorting: https://en.wikipedia.org/wiki/Topological_sorting#Kahn's_algorithm
5. **S3 Consistency Model** - Strong read-after-write: https://aws.amazon.com/s3/consistency/
6. **Spring Batch** - Batch processing framework: https://spring.io/projects/spring-batch
7. **Jackson** - JSON serialization for Java: https://github.com/FasterXML/jackson
8. **gRPC Java** - gRPC framework for Java: https://grpc.io/docs/languages/java/
9. **HashiCorp Vault** - Secrets management: https://www.vaultproject.io/
10. **Pulumi Architecture** - Alternative IaC approach (for comparison): https://www.pulumi.com/docs/concepts/
