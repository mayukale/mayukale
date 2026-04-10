# System Design: CLI Client for Infrastructure Platform

> **Relevance to role:** A cloud infrastructure platform engineer must provide a CLI that is as powerful as the web portal. Engineers use CLIs in CI/CD pipelines, automation scripts, and daily terminal workflows. The CLI must handle auth (OAuth2 device flow, service accounts), state management (contexts, configs), and provide excellent UX (output formatting, shell completion, error handling). This is a core developer experience concern.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|-------------|--------|
| FR-1 | Authentication | Interactive login (OAuth2 device flow), service account tokens for CI/CD |
| FR-2 | Configuration | `~/.infra-cli/config.yaml` storing endpoint, tokens, default context |
| FR-3 | Context management | Switch between dev/staging/prod environments |
| FR-4 | Resource management | Create, list, show, delete VMs, bare-metal, K8s clusters, storage |
| FR-5 | Bare-metal reservation | Reserve with time windows, GPU type, count |
| FR-6 | K8s cluster lifecycle | Create cluster, get kubeconfig, scale, upgrade, delete |
| FR-7 | Job submission | Submit batch jobs with resource requirements |
| FR-8 | Quota management | View quota usage per project |
| FR-9 | Output formatting | Table (default), JSON, YAML, wide table, custom columns |
| FR-10 | Shell completion | Bash, Zsh, Fish auto-completion |
| FR-11 | Pagination | Handle large result sets with `--limit` and `--offset` or auto-pagination |
| FR-12 | AI query | Natural language infrastructure queries |
| FR-13 | Offline mode | Cached data for read-only commands when API unreachable |
| FR-14 | Plugin system | Extend CLI with custom commands |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Startup time | < 100ms (no heavy framework initialization) |
| NFR-2 | Binary size | < 30MB (single static binary) |
| NFR-3 | Cross-platform | Linux (amd64, arm64), macOS (amd64, arm64), Windows (amd64) |
| NFR-4 | Installation | Homebrew, apt, yum, direct binary download, Docker image |
| NFR-5 | Backward compatibility | Semver; no breaking changes in minor versions |
| NFR-6 | Token security | Tokens encrypted at rest in config file (OS keyring integration) |
| NFR-7 | Telemetry | Optional anonymous usage telemetry (opt-in) |

### Constraints & Assumptions
- The CLI communicates with the same REST API as the web portal.
- Go is the implementation language (fast startup, single binary, excellent CLI libraries).
- Cobra + Viper for command parsing and configuration.
- The platform API is versioned (`/api/v1/`, `/api/v2/`).
- Service accounts use long-lived API tokens (not OAuth2).

### Out of Scope
- GUI / TUI (terminal UI) -- this is a pure CLI.
- Direct infrastructure access (SSH, IPMI) -- the CLI manages resources, not connects to them.
- Log streaming from production systems -- separate logging tool.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Total CLI users | 40% of 10,000 developers | 4,000 |
| Daily active CLI users | 30% of CLI users | 1,200 |
| Commands per user per day | ~15 (list, show, create, etc.) | 15 |
| Total CLI API calls/day | 1,200 x 15 | 18,000 |
| CI/CD pipeline invocations/day | 500 pipelines x 5 CLI calls each | 2,500 |
| Total API calls/day from CLI | 18,000 + 2,500 | 20,500 |
| Peak API calls/sec | 20,500 / 86,400 x 5 (peak) | ~1.2 RPS |
| Binary downloads/month | New version every 2 weeks, 4,000 users | ~4,000 |

### Latency Requirements
| Operation | Target |
|-----------|--------|
| CLI startup to first output | < 200ms |
| `list` commands | < 1s |
| `show` commands | < 500ms |
| `create` commands (accepted) | < 2s |
| Shell completion response | < 300ms |
| Token refresh (automatic) | < 1s (transparent to user) |

### Storage Estimates

| Data | Calculation | Size |
|------|-------------|------|
| Config file | YAML with contexts + tokens | < 5KB |
| Cache directory | Cached list results, templates | < 10MB |
| Completion scripts | Per shell | < 50KB each |
| Binary size | Go binary with all commands | ~25MB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| API calls | 1.2 RPS x 5KB avg response | ~6 KB/s peak |
| Binary download | 25MB x 4,000/month | ~40 MB/day avg |

---

## 3. High Level Architecture

```
+----------------------------------------------+
|              Developer Terminal               |
|                                               |
|  $ infra-cli machines list --type gpu         |
|                                               |
+---------------------+------------------------+
                      |
         +------------v-------------+
         |     infra-cli Binary     |
         |  (Go, Cobra + Viper)     |
         |                          |
         |  +--------------------+  |
         |  | Command Router     |  |
         |  | (Cobra commands)   |  |
         |  +--------+-----------+  |
         |           |              |
         |  +--------v-----------+  |
         |  | Auth Manager       |  |
         |  | (OAuth2 / SA token)|  |
         |  +--------+-----------+  |
         |           |              |
         |  +--------v-----------+  |
         |  | Config Manager     |  |
         |  | (~/.infra-cli/)    |  |
         |  +--------+-----------+  |
         |           |              |
         |  +--------v-----------+  |
         |  | HTTP Client        |  |
         |  | (retry, timeout,   |  |
         |  |  token refresh)    |  |
         |  +--------+-----------+  |
         |           |              |
         |  +--------v-----------+  |
         |  | Output Formatter   |  |
         |  | (table/JSON/YAML)  |  |
         |  +--------+-----------+  |
         |           |              |
         |  +--------v-----------+  |
         |  | Cache Layer        |  |
         |  | (offline mode)     |  |
         |  +--------------------+  |
         +-----------+--------------+
                     |
                     | HTTPS
                     |
         +-----------v--------------+
         |   Platform API Gateway   |
         |   (api.infra.company.com)|
         +--------------------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **Command Router (Cobra)** | Parses CLI args, dispatches to command handlers, manages flags and subcommands |
| **Auth Manager** | Handles OAuth2 device flow for interactive login, service account token loading, automatic token refresh |
| **Config Manager (Viper)** | Reads/writes `~/.infra-cli/config.yaml`, manages multiple contexts (environments), handles defaults |
| **HTTP Client** | Makes API calls with auth headers, retry logic, timeout handling, rate limit backoff |
| **Output Formatter** | Renders API responses as tables, JSON, YAML, or wide tables based on `--output` flag |
| **Cache Layer** | Stores recent API responses in `~/.infra-cli/cache/` for offline mode and faster repeated queries |

---

## 4. Data Model

### Core Entities & Schema

The CLI is stateless (no local database). Its "data model" is the configuration files and cache.

**Config File: `~/.infra-cli/config.yaml`**

```yaml
# API version this config was created with
config_version: 1

# Current active context
current_context: staging

# Context definitions
contexts:
  dev:
    endpoint: https://api.dev.infra.company.com
    api_version: v1
    default_project: my-team
    default_output: table
    auth:
      type: oauth2
      token_file: ~/.infra-cli/tokens/dev.json
  staging:
    endpoint: https://api.staging.infra.company.com
    api_version: v1
    default_project: my-team
    default_output: table
    auth:
      type: oauth2
      token_file: ~/.infra-cli/tokens/staging.json
  prod:
    endpoint: https://api.infra.company.com
    api_version: v1
    default_project: my-team
    default_output: table
    auth:
      type: oauth2
      token_file: ~/.infra-cli/tokens/prod.json
  ci-prod:
    endpoint: https://api.infra.company.com
    api_version: v1
    default_project: platform-team
    auth:
      type: service_account
      token: ${INFRA_SA_TOKEN}  # environment variable reference

# Global settings
settings:
  telemetry: false
  color: auto          # auto, always, never
  pager: less          # pager for long output
  timeout: 30s         # default HTTP timeout
  retries: 3           # default retry count
```

**Token File: `~/.infra-cli/tokens/staging.json`**

```json
{
  "access_token": "eyJhbG...",
  "refresh_token": "dGhpcyB...",
  "token_type": "Bearer",
  "expires_at": "2026-04-09T12:00:00Z",
  "scope": "openid profile email",
  "issuer": "https://company.okta.com/oauth2/default"
}
```

**Cache Directory Structure: `~/.infra-cli/cache/`**

```
~/.infra-cli/cache/
  staging/
    templates.json          # cached template list (TTL: 1h)
    machines_gpu.json       # cached machine list for gpu type (TTL: 5m)
    quota_ml-team.json      # cached quota for ml-team (TTL: 5m)
    completion_cache.json   # dynamic completions (resource names, project names)
  metadata.json             # cache TTL metadata
```

### Database Selection

N/A -- the CLI has no database. All state is in files:
- Config: YAML (human-readable, editable).
- Tokens: JSON (programmatic access, encrypted at rest via OS keyring when available).
- Cache: JSON (fast read/write, TTL-based expiry).

### Indexing Strategy

N/A -- no database. Cache lookup is by filename (direct filesystem path).

---

## 5. API Design

### REST Endpoints (Consumed by CLI)

The CLI consumes the same REST API as the web portal. Key endpoints:

```
# Authentication
POST /oauth2/device/authorize    -> { device_code, user_code, verification_uri }
POST /oauth2/token               -> { access_token, refresh_token, expires_in }

# Resources
GET    /api/v1/resources?type=vm&status=active&project_id=10&limit=50&offset=0
POST   /api/v1/resources          { resource spec }
GET    /api/v1/resources/{uid}
DELETE /api/v1/resources/{uid}

# Machines (bare-metal inventory)
GET    /api/v1/machines?type=gpu&status=available&limit=50
GET    /api/v1/machines/{hostname}

# Reservations
POST   /api/v1/reservations       { type, count, start, duration, project }
GET    /api/v1/reservations?status=active&user=alice
DELETE /api/v1/reservations/{id}

# Clusters
POST   /api/v1/clusters           { name, version, nodes, node_type }
GET    /api/v1/clusters/{name}
GET    /api/v1/clusters/{name}/kubeconfig
DELETE /api/v1/clusters/{name}

# Jobs
POST   /api/v1/jobs               { image, cpu, memory, gpu, command }
GET    /api/v1/jobs/{id}
GET    /api/v1/jobs/{id}/logs?follow=true

# Quotas
GET    /api/v1/projects/{name}/quotas

# Templates
GET    /api/v1/templates?category=vm

# AI
POST   /api/v1/ai/query           { question: "..." }
```

### CLI Design

#### Command Hierarchy

```
infra-cli
  ├── login                    # OAuth2 device flow login
  ├── logout                   # Clear stored tokens
  ├── whoami                   # Show current user + context
  ├── config
  │   ├── set-context          # Switch active context
  │   ├── get-contexts         # List all contexts
  │   ├── add-context          # Add a new context
  │   ├── delete-context       # Remove a context
  │   ├── set                  # Set a config value
  │   └── view                 # View current config
  ├── machines
  │   ├── list                 # List bare-metal machines
  │   └── show                 # Show machine details
  ├── reserve
  │   (top-level shortcut for reservations create)
  ├── reservations
  │   ├── list                 # List reservations
  │   ├── show                 # Show reservation details
  │   └── cancel               # Cancel a reservation
  ├── resources
  │   ├── list                 # List all resources
  │   ├── show                 # Show resource details
  │   ├── create               # Create a resource
  │   └── delete               # Delete a resource
  ├── clusters
  │   ├── create               # Create K8s cluster
  │   ├── list                 # List clusters
  │   ├── show                 # Show cluster details
  │   ├── kubeconfig           # Get kubeconfig
  │   ├── scale                # Scale cluster nodes
  │   ├── upgrade              # Upgrade K8s version
  │   └── delete               # Delete cluster
  ├── jobs
  │   ├── submit               # Submit a batch job
  │   ├── list                 # List jobs
  │   ├── show                 # Show job details
  │   ├── logs                 # Stream job logs
  │   └── cancel               # Cancel a job
  ├── quota
  │   └── show                 # Show project quota
  ├── templates
  │   ├── list                 # List available templates
  │   └── show                 # Show template details
  ├── ask                      # AI natural language query
  ├── completion               # Generate shell completion script
  │   ├── bash
  │   ├── zsh
  │   └── fish
  └── version                  # Show CLI version
```

#### Detailed Command Specifications

**1. Login (OAuth2 Device Flow)**

```bash
$ infra-cli login
Attempting to log in to https://api.staging.infra.company.com...

Your device code is: ABCD-EFGH
Open this URL in your browser: https://company.okta.com/activate
Waiting for authentication... (press Ctrl+C to cancel)

Polling... ━━━━━━━━━━━━━━━━━━━━ 15s

✓ Successfully logged in as alice@company.com
  Context: staging
  Token expires: 2026-04-09T20:00:00Z

$ infra-cli login --service-account
Enter service account token: ****************************
✓ Service account token stored for context: ci-prod
```

**Implementation (OAuth2 Device Flow):**

```go
func loginCmd() *cobra.Command {
    cmd := &cobra.Command{
        Use:   "login",
        Short: "Authenticate with the infrastructure platform",
        RunE: func(cmd *cobra.Command, args []string) error {
            ctx := cmd.Context()
            cfg := config.Current()

            // Step 1: Request device code
            deviceResp, err := authClient.RequestDeviceCode(ctx, cfg.Endpoint)
            // POST /oauth2/device/authorize
            // Body: { client_id, scope: "openid profile email" }
            // Response: { device_code, user_code, verification_uri, expires_in, interval }

            fmt.Fprintf(os.Stderr, "Your device code is: %s\n", deviceResp.UserCode)
            fmt.Fprintf(os.Stderr, "Open this URL in your browser: %s\n", deviceResp.VerificationURI)

            // Try to open browser automatically
            _ = browser.Open(deviceResp.VerificationURI)

            // Step 2: Poll for token
            token, err := authClient.PollForToken(ctx, deviceResp, cfg.Endpoint)
            // POST /oauth2/token
            // Body: { grant_type: "urn:ietf:params:oauth:grant-type:device_code",
            //         device_code, client_id }
            // Response (pending): { error: "authorization_pending" }
            // Response (success): { access_token, refresh_token, expires_in, token_type }

            // Step 3: Store token
            err = tokenStore.Save(cfg.CurrentContext, token)

            fmt.Fprintf(os.Stdout, "✓ Successfully logged in as %s\n", token.Email())
            return nil
        },
    }
    cmd.Flags().Bool("service-account", false, "Login with a service account token")
    return cmd
}
```

**2. Config Context Management**

```bash
$ infra-cli config get-contexts
  CURRENT   NAME      ENDPOINT                                   PROJECT
  *         staging   https://api.staging.infra.company.com      my-team
            dev       https://api.dev.infra.company.com          my-team
            prod      https://api.infra.company.com              my-team

$ infra-cli config set-context prod
✓ Switched to context "prod" (https://api.infra.company.com)

$ infra-cli config set-context staging
✓ Switched to context "staging" (https://api.staging.infra.company.com)

$ infra-cli config add-context \
    --name eu-prod \
    --endpoint https://api.eu.infra.company.com \
    --project eu-team
✓ Context "eu-prod" added

$ infra-cli config set default_output json
✓ Set default_output = json for context "staging"

$ infra-cli config view
current_context: staging
contexts:
  staging:
    endpoint: https://api.staging.infra.company.com
    default_project: my-team
    default_output: json
    auth:
      type: oauth2
      status: authenticated (expires in 7h 23m)
```

**3. Bare-Metal Reservation**

```bash
$ infra-cli reserve \
    --type gpu-h100 \
    --count 4 \
    --start "2026-04-10T08:00:00Z" \
    --duration 8h \
    --project ml-team
Checking quota for project ml-team...
  GPU quota: 8/20 used, requesting 4 (32 total after reservation)
  ✓ Quota available

Checking availability...
  ✓ 4 gpu-h100 servers available for 2026-04-10 08:00-16:00 UTC

Estimated cost: $160.00 (4 servers x $5.00/hr x 8h)

Reservation created:
  ID:        res-7f8a9b0c
  Type:      gpu-h100
  Count:     4
  Servers:   gpu-rack3-srv01, gpu-rack3-srv02, gpu-rack3-srv03, gpu-rack3-srv04
  Start:     2026-04-10T08:00:00Z
  End:       2026-04-10T16:00:00Z
  Status:    confirmed
  Project:   ml-team

$ infra-cli reservations list --status active --user alice
ID            TYPE       COUNT  START                   END                     STATUS     PROJECT
res-7f8a9b0c  gpu-h100   4      2026-04-10T08:00:00Z    2026-04-10T16:00:00Z    confirmed  ml-team
res-3d4e5f6a  gpu-a100   2      2026-04-11T00:00:00Z    2026-04-12T00:00:00Z    confirmed  ml-team

$ infra-cli reservations list --status active --user alice --output json
[
  {
    "id": "res-7f8a9b0c",
    "type": "gpu-h100",
    "count": 4,
    "servers": ["gpu-rack3-srv01", "gpu-rack3-srv02", "gpu-rack3-srv03", "gpu-rack3-srv04"],
    "start": "2026-04-10T08:00:00Z",
    "end": "2026-04-10T16:00:00Z",
    "status": "confirmed",
    "project": "ml-team",
    "cost_estimate": 160.00
  }
]

$ infra-cli reservations cancel res-7f8a9b0c
Are you sure you want to cancel reservation res-7f8a9b0c? [y/N] y
✓ Reservation res-7f8a9b0c cancelled. Quota released.
```

**4. Machine Inventory**

```bash
$ infra-cli machines list --type gpu --status available
HOSTNAME          TYPE       RACK       DATACENTER  GPU       CPU             RAM     STATUS
gpu-rack3-srv01   gpu-h100   rack-03    us-east-1   8xH100    AMD EPYC 9654   2TB     available
gpu-rack3-srv02   gpu-h100   rack-03    us-east-1   8xH100    AMD EPYC 9654   2TB     available
gpu-rack5-srv07   gpu-a100   rack-05    us-east-1   8xA100    AMD EPYC 7763   1TB     available
gpu-rack5-srv08   gpu-a100   rack-05    us-east-1   8xA100    AMD EPYC 7763   1TB     available

Showing 4 of 4 results

$ infra-cli machines list --type gpu --status available --output wide
HOSTNAME          TYPE       RACK       DC          GPU       GPU_MEM   CPU              CORES  RAM    DISK         NETWORK    IPMI_IP         LAST_MAINT
gpu-rack3-srv01   gpu-h100   rack-03    us-east-1   8xH100    640GB     AMD EPYC 9654    192    2TB    8x3.84TB     100Gbps    10.0.100.1      2026-03-15
gpu-rack3-srv02   gpu-h100   rack-03    us-east-1   8xH100    640GB     AMD EPYC 9654    192    2TB    8x3.84TB     100Gbps    10.0.100.2      2026-03-15
...

$ infra-cli machines show gpu-rack3-srv01
Hostname:       gpu-rack3-srv01
Type:           gpu-h100
Status:         available
Datacenter:     us-east-1
Rack:           rack-03
Position:       U22-U26

Hardware:
  CPU:          AMD EPYC 9654 (192 cores)
  RAM:          2048 GB DDR5
  GPU:          8x NVIDIA H100 80GB SXM5
  GPU Memory:   640 GB total
  Disk:         8x 3.84TB NVMe SSD (30.72 TB total)
  Network:      4x 100Gbps (bonded)

Management:
  IPMI IP:      10.0.100.1
  BMC Version:  2.14
  BIOS Version: 3.2.1

Maintenance:
  Last:         2026-03-15 (firmware update)
  Next:         2026-05-01 (scheduled disk replacement)

Current Reservations:  None
```

**5. Kubernetes Cluster Management**

```bash
$ infra-cli clusters create \
    --name my-k8s \
    --version 1.29 \
    --nodes 5 \
    --node-type cpu-epyc-64c \
    --project ml-team
Creating Kubernetes cluster "my-k8s"...
  Version:    1.29.2
  Nodes:      5x cpu-epyc-64c
  Network:    Cilium CNI
  Project:    ml-team

Provisioning ━━━━━━━━━━━━━━━━━━━━ 100% (3m 42s)
  ✓ Control plane ready
  ✓ Worker nodes joined (5/5)
  ✓ CoreDNS running
  ✓ Cilium healthy

Cluster "my-k8s" is ready!
  API Server: https://my-k8s.k8s.infra.company.com:6443
  Get kubeconfig: infra-cli clusters kubeconfig --name my-k8s

$ infra-cli clusters kubeconfig --name my-k8s > ~/.kube/config
✓ Kubeconfig written. Test with: kubectl get nodes

$ infra-cli clusters list --project ml-team
NAME      VERSION   NODES   STATUS    AGE     PROJECT
my-k8s    1.29.2    5       running   5m      ml-team
staging   1.28.8    3       running   30d     ml-team

$ infra-cli clusters scale --name my-k8s --nodes 10
Scaling cluster "my-k8s" from 5 to 10 nodes...
  Adding 5x cpu-epyc-64c nodes...

Scaling ━━━━━━━━━━━━━━━━━━━━ 100% (2m 15s)
  ✓ 10/10 nodes ready

$ infra-cli clusters upgrade --name my-k8s --version 1.30
Upgrading cluster "my-k8s" from 1.29.2 to 1.30.1...
  ⚠ This will perform a rolling upgrade of all nodes.
  Proceed? [y/N] y

Upgrade in progress...
  ✓ Control plane upgraded to 1.30.1
  ✓ Node 1/10 upgraded
  ✓ Node 2/10 upgraded
  ...
  ✓ Node 10/10 upgraded

Cluster "my-k8s" upgraded to 1.30.1
```

**6. Job Submission**

```bash
$ infra-cli jobs submit \
    --image myapp:latest \
    --cpu 8 \
    --memory 32Gi \
    --gpu 2 \
    --command "python train.py --epochs 100" \
    --project ml-team \
    --name training-run-42
Job submitted:
  ID:        job-5a6b7c8d
  Name:      training-run-42
  Image:     myapp:latest
  Resources: 8 CPU, 32Gi RAM, 2 GPU
  Status:    queued
  Project:   ml-team

$ infra-cli jobs list --project ml-team
ID            NAME               IMAGE           RESOURCES          STATUS    DURATION  STARTED
job-5a6b7c8d  training-run-42    myapp:latest    8C/32Gi/2GPU       running   12m       2026-04-09T10:00
job-1a2b3c4d  eval-run-41        myapp:latest    4C/16Gi/1GPU       completed 45m       2026-04-09T08:00
job-9e8f7a6b  data-preprocess    etl:v2          16C/64Gi/0GPU      failed    5m        2026-04-09T07:30

$ infra-cli jobs logs job-5a6b7c8d
[2026-04-09T10:00:05Z] Starting training with 100 epochs
[2026-04-09T10:00:06Z] Loading dataset from /data/train.parquet
[2026-04-09T10:00:12Z] Epoch 1/100 - loss: 2.3456 - accuracy: 0.1234
[2026-04-09T10:00:18Z] Epoch 2/100 - loss: 1.8765 - accuracy: 0.2345
...

$ infra-cli jobs logs job-5a6b7c8d --follow
(streams logs in real-time, similar to `kubectl logs -f`)

$ infra-cli jobs logs job-5a6b7c8d --tail 20
(shows last 20 lines)
```

**7. Quota Management**

```bash
$ infra-cli quota show --project ml-team
Project: ml-team

RESOURCE         USED     RESERVED   LIMIT (SOFT)   LIMIT (HARD)   UTILIZATION
vcpu             128      16         200            256            56%  ████████░░░░░░
memory_gb        512      64         800            1024           56%  ████████░░░░░░
gpu              8        4          16             20             60%  █████████░░░░░
disk_tb          5.0      0.5        8.0            10.0           55%  ████████░░░░░░
vm_count         12       1          30             50             26%  ████░░░░░░░░░░
bare_metal       2        0          5              5              40%  ██████░░░░░░░░
k8s_clusters     2        0          5              5              40%  ██████░░░░░░░░

⚠ GPU utilization approaching soft limit (60% of hard limit, 75% of soft limit)
```

**8. AI Query**

```bash
$ infra-cli ask "which GPU servers are available next week?"
Querying AI assistant...

Based on current inventory and reservations:

Available GPU servers for 2026-04-13 to 2026-04-19:
  H100 (8x80GB):  6 servers available all week
  A100 (8x80GB):  3 servers (2 reserved Mon-Wed by team-alpha)
  L40S (4x48GB):  12 servers available all week

Recommendation:
  For ML training, H100 servers offer the best performance.
  To reserve: infra-cli reserve --type gpu-h100 --count 4 --start 2026-04-13 --duration 7d --project ml-team

$ infra-cli ask "show me the most expensive resources in my project"
Querying AI assistant...

Top 5 most expensive resources in project ml-team:

RESOURCE             TYPE        COST/HOUR   RUNNING    TOTAL COST
gpu-training-01      bare_metal  $40.00      14d        $13,440.00
gpu-training-02      bare_metal  $40.00      14d        $13,440.00
ml-k8s-cluster       k8s (10n)  $15.00      30d        $10,800.00
data-processing-vm   vm          $8.50       7d         $1,428.00
staging-cluster      k8s (3n)   $4.50       30d        $3,240.00

Total monthly spend: ~$42,348.00 (78% of budget)

💡 Suggestion: gpu-training-01 and gpu-training-02 have been running for 14 days.
   If training is complete, releasing them would save $80/hr ($57,600/month).
```

**9. Shell Completion**

```bash
# Generate and install completion
$ infra-cli completion bash > /etc/bash_completion.d/infra-cli
$ infra-cli completion zsh > "${fpath[1]}/_infra-cli"
$ infra-cli completion fish > ~/.config/fish/completions/infra-cli.fish

# Usage (after shell restart)
$ infra-cli mach<TAB>
machines

$ infra-cli machines <TAB>
list   show

$ infra-cli machines list --type <TAB>
gpu-h100   gpu-a100   gpu-l40s   cpu-epyc-128c   cpu-epyc-64c

$ infra-cli reservations cancel <TAB>
res-7f8a9b0c   res-3d4e5f6a

$ infra-cli config set-context <TAB>
dev   staging   prod   eu-prod
```

**Implementation:**

```go
func completionCmd() *cobra.Command {
    cmd := &cobra.Command{
        Use:   "completion [bash|zsh|fish]",
        Short: "Generate shell completion script",
        Args:  cobra.ExactArgs(1),
        RunE: func(cmd *cobra.Command, args []string) error {
            switch args[0] {
            case "bash":
                return cmd.Root().GenBashCompletion(os.Stdout)
            case "zsh":
                return cmd.Root().GenZshCompletion(os.Stdout)
            case "fish":
                return cmd.Root().GenFishCompletion(os.Stdout, true)
            default:
                return fmt.Errorf("unsupported shell: %s", args[0])
            }
        },
    }
    return cmd
}

// Dynamic completion for resource names
func machineTypeCompletion(cmd *cobra.Command, args []string, toComplete string) ([]string, cobra.ShellCompDirective) {
    client := getAPIClient(cmd)
    types, err := client.GetMachineTypes(cmd.Context())
    if err != nil {
        // Fall back to cached types
        types = cache.GetMachineTypes()
    }
    return types, cobra.ShellCompDirectiveNoFileComp
}
```

**10. Error Handling**

```bash
# Authentication error
$ infra-cli machines list
Error: authentication required
  Your session has expired. Run 'infra-cli login' to re-authenticate.
  Exit code: 1

# Permission error
$ infra-cli resources delete vm-prod-001
Error: permission denied
  You do not have 'developer' role in project 'production'.
  Contact project admin: admin@company.com
  Exit code: 1

# Quota exceeded
$ infra-cli reserve --type gpu-h100 --count 10 --start 2026-04-10 --duration 8h --project ml-team
Error: quota exceeded
  Requested: 10 gpu (80 GPU units)
  Available: 8 gpu (of 20 hard limit, 12 currently used)
  Reduce count to 8 or request quota increase from project admin.
  Exit code: 1

# API unavailable
$ infra-cli machines list
Warning: API unreachable (connection timeout after 30s)
  Showing cached data (last updated: 5 minutes ago)

HOSTNAME          TYPE       RACK       DATACENTER  STATUS      (cached)
gpu-rack3-srv01   gpu-h100   rack-03    us-east-1   available   (cached)
...

# Rate limited
$ infra-cli machines list
Error: rate limited
  Too many requests. Retry after 30 seconds.
  (Automatic retry in 30s... press Ctrl+C to cancel)
```

**Implementation:**

```go
func humanizeHTTPError(resp *http.Response, body []byte) error {
    switch resp.StatusCode {
    case 401:
        return &CLIError{
            Message: "authentication required",
            Hint:    "Your session has expired. Run 'infra-cli login' to re-authenticate.",
            Code:    1,
        }
    case 403:
        var apiErr APIError
        json.Unmarshal(body, &apiErr)
        return &CLIError{
            Message: "permission denied",
            Hint:    fmt.Sprintf("You do not have '%s' role in project '%s'.", apiErr.RequiredRole, apiErr.Project),
            Code:    1,
        }
    case 429:
        retryAfter := resp.Header.Get("Retry-After")
        return &CLIError{
            Message:    "rate limited",
            Hint:       fmt.Sprintf("Too many requests. Retry after %s seconds.", retryAfter),
            RetryAfter: parseDuration(retryAfter),
            Code:       1,
        }
    case 409:
        return &CLIError{
            Message: "conflict",
            Hint:    "Resource already exists or operation conflicts with current state.",
            Code:    1,
        }
    default:
        return &CLIError{
            Message: fmt.Sprintf("API error (HTTP %d)", resp.StatusCode),
            Hint:    string(body),
            Code:    1,
        }
    }
}
```

#### Global Flags

```
--context string      Override active context (default: from config)
--project string      Override default project (default: from context config)
--output string       Output format: table, json, yaml, wide (default: table)
--no-color            Disable colored output
--verbose             Show HTTP request/response details
--timeout duration    HTTP request timeout (default: 30s)
--dry-run             Show what would be done without executing
--yes                 Auto-confirm destructive operations
--quiet               Suppress non-essential output
```

#### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | General error (API error, validation error) |
| 2 | Usage error (invalid flags, missing arguments) |
| 3 | Authentication error (expired token, not logged in) |
| 4 | Permission error (insufficient role) |
| 5 | Resource not found |
| 64 | Timeout |

---

## 6. Core Component Deep Dives

### 6.1 Authentication & Token Management

**Why it's hard:** The CLI must support two very different auth flows (interactive OAuth2 for humans, static tokens for CI/CD) while keeping tokens secure at rest, refreshing them transparently, and handling edge cases (concurrent token refresh, token revocation, expired refresh tokens).

| Approach | Pros | Cons |
|----------|------|------|
| **OAuth2 Authorization Code + PKCE (browser redirect)** | Standard, well-supported | Requires local HTTP server for callback; firewall issues |
| **OAuth2 Device Flow** | No local server needed; works over SSH | Requires user to open browser separately; polling delay |
| **API Key (static token)** | Simplest; great for CI/CD | No automatic expiry; risk of token leak |
| **mTLS (client certificates)** | Very secure; mutual auth | Complex certificate management; hard for developers to set up |

**Selected approach: OAuth2 Device Flow for interactive, API keys for CI/CD.**

**Justification:** Device flow is the standard for CLI tools (GitHub CLI, Azure CLI use it). It works seamlessly over SSH sessions where there's no browser. API keys are the pragmatic choice for CI/CD -- they're simple, can be rotated, and are scoped to service accounts.

**Implementation:**

```go
type AuthManager struct {
    tokenStore  TokenStore       // File-based or OS keyring
    httpClient  *http.Client
    config      *Config
    mu          sync.Mutex       // Prevents concurrent refresh
}

// Transparent token refresh middleware
func (a *AuthManager) AuthenticatedRequest(req *http.Request) (*http.Response, error) {
    token, err := a.getValidToken()
    if err != nil {
        return nil, fmt.Errorf("not authenticated: run 'infra-cli login'")
    }
    req.Header.Set("Authorization", "Bearer "+token.AccessToken)

    resp, err := a.httpClient.Do(req)
    if err != nil {
        return nil, err
    }

    // If 401 and we have a refresh token, try refresh
    if resp.StatusCode == 401 && token.RefreshToken != "" {
        a.mu.Lock()
        defer a.mu.Unlock()

        // Double-check: another goroutine may have already refreshed
        currentToken, _ := a.tokenStore.Load(a.config.CurrentContext)
        if currentToken.AccessToken == token.AccessToken {
            // Token hasn't been refreshed yet, do it now
            newToken, refreshErr := a.refreshToken(token)
            if refreshErr != nil {
                return nil, fmt.Errorf("session expired: run 'infra-cli login'")
            }
            a.tokenStore.Save(a.config.CurrentContext, newToken)
            token = newToken
        } else {
            token = currentToken
        }

        // Retry with new token
        req.Header.Set("Authorization", "Bearer "+token.AccessToken)
        return a.httpClient.Do(req)
    }

    return resp, nil
}

// Token storage with OS keyring integration
type TokenStore interface {
    Save(context string, token *OAuthToken) error
    Load(context string) (*OAuthToken, error)
    Delete(context string) error
}

// Keyring-backed store (macOS Keychain, Linux Secret Service, Windows Credential Manager)
type KeyringTokenStore struct {}

// File-based fallback (encrypted with machine-specific key)
type FileTokenStore struct {
    basePath string  // ~/.infra-cli/tokens/
}
```

**Token lifecycle:**
1. User runs `infra-cli login`.
2. CLI initiates OAuth2 device flow with Okta.
3. User authenticates in browser, approves device.
4. CLI receives access_token (1h TTL) + refresh_token (30d TTL).
5. Tokens stored in OS keyring (or encrypted file).
6. On every API call, AuthManager checks token expiry.
7. If access_token expired but refresh_token valid: automatic refresh (transparent).
8. If refresh_token expired: prompt user to re-login.

**Failure modes:**
- **Concurrent refresh:** Two CLI processes running simultaneously both detect expired token. The mutex prevents double-refresh; second process uses the refreshed token from the first.
- **Keyring unavailable:** Fall back to encrypted file storage. Warn user that tokens are stored in file.
- **Refresh token revoked (e.g., user deactivated):** Refresh returns 401. Clear stored tokens, prompt re-login.
- **Clock skew:** Token expiry check uses a 30-second buffer (consider token expired 30s before actual expiry).

**Interviewer Q&As:**

**Q1: Why device flow instead of authorization code flow with PKCE?**
A: Device flow works in headless environments (SSH into a remote machine, tmux sessions) where the CLI cannot open a local browser or listen on a local port. Authorization code + PKCE requires either opening a browser directly or spinning up a local HTTP server for the callback, both of which fail in headless scenarios. Device flow only requires the user to visit a URL on any device.

**Q2: How do you handle the case where a user's Okta account is deactivated while they have a valid token?**
A: The access token continues to work until it expires (max 1h). For immediate revocation, the API gateway checks a token blacklist (Redis-backed, populated by webhook from Okta on user deactivation). When the token is blacklisted, the API returns 401 and the CLI prompts re-login (which will fail since the account is deactivated).

**Q3: How do service account tokens work in CI/CD?**
A: Service accounts are created by project admins in the portal. Each SA gets a long-lived API token (rotatable, revocable). The CI/CD pipeline sets `INFRA_SA_TOKEN` env var. The CLI detects this environment variable and uses it directly (no OAuth2 flow). SA tokens are scoped to a specific project and role (typically `developer`).

**Q4: How do you prevent token theft from the config file?**
A: (1) Primary: OS keyring integration (macOS Keychain, Linux Secret Service, Windows Credential Manager) -- tokens never touch disk unencrypted. (2) Fallback: File-based storage with 600 permissions and encrypted with a machine-derived key (machine ID + user ID). (3) Access token TTL is 1h, limiting exposure window. (4) `infra-cli logout` explicitly clears tokens.

**Q5: What if the user needs to authenticate non-interactively (e.g., in a Docker container)?**
A: Three options: (1) Service account token via `INFRA_SA_TOKEN` env var (recommended for automation). (2) Pre-authenticated config file mounted into the container (less secure). (3) `infra-cli login --token <token>` for piping a token from a secrets manager.

**Q6: How do you handle multi-user scenarios (shared machines)?**
A: Each user has their own `~/.infra-cli/` directory (in their home dir). Tokens are per-user. On shared machines, we strongly recommend OS keyring over file-based storage. The CLI warns if config file permissions are too open (>600).

---

### 6.2 Output Formatting & UX

**Why it's hard:** Different use cases need different output formats. Interactive users want pretty tables. Scripts need machine-parseable JSON. CI/CD pipelines need exit codes and minimal output. The same data must render well in all formats, handle variable-width terminals, and degrade gracefully for very long values.

| Approach | Pros | Cons |
|----------|------|------|
| **Fixed-width columns** | Simple, predictable | Wastes space; truncates on narrow terminals |
| **Dynamic column sizing** | Adapts to data and terminal width | Complex layout algorithm |
| **Template-based (Go templates)** | Infinitely customizable | Users must learn template syntax |
| **Multiple built-in formats + custom columns** | Best of both worlds | More code to maintain |

**Selected approach: Dynamic column sizing with multiple built-in formats + custom columns.**

**Implementation:**

```go
type OutputFormatter struct {
    format      string    // table, json, yaml, wide, custom
    columns     []string  // for custom format
    noHeaders   bool
    noColor     bool
    termWidth   int
}

func (f *OutputFormatter) Render(data interface{}) error {
    switch f.format {
    case "json":
        enc := json.NewEncoder(os.Stdout)
        enc.SetIndent("", "  ")
        return enc.Encode(data)

    case "yaml":
        enc := yaml.NewEncoder(os.Stdout)
        return enc.Encode(data)

    case "table", "wide":
        return f.renderTable(data)

    case "custom":
        return f.renderCustomColumns(data)
    }
    return nil
}

func (f *OutputFormatter) renderTable(data interface{}) error {
    table := tablewriter.NewWriter(os.Stdout)
    table.SetAutoWrapText(false)
    table.SetAutoFormatHeaders(true)
    table.SetHeaderAlignment(tablewriter.ALIGN_LEFT)
    table.SetAlignment(tablewriter.ALIGN_LEFT)
    table.SetBorder(false)
    table.SetColumnSeparator("  ")
    table.SetNoWhiteSpace(true)
    table.SetTablePadding("  ")

    // Dynamic column widths based on terminal size
    termWidth := f.getTerminalWidth()
    columns := f.getColumnsForFormat(data)
    widths := f.calculateColumnWidths(columns, data, termWidth)
    table.SetColMinWidth(widths...)

    // Render
    table.SetHeader(columns)
    for _, row := range f.extractRows(data) {
        table.Append(row)
    }
    table.Render()

    return nil
}

// Custom columns: --output custom-columns=NAME:.name,STATUS:.status,GPU:.spec.gpu.count
func (f *OutputFormatter) renderCustomColumns(data interface{}) error {
    // Parse column specs (NAME:jsonpath)
    // Apply jsonpath to each item
    // Render as table
}
```

**Pagination:**

```go
// Automatic pagination for large result sets
func listWithPagination(client *APIClient, path string, params url.Values) ([]json.RawMessage, error) {
    var allItems []json.RawMessage
    limit := 100
    offset := 0

    for {
        params.Set("limit", strconv.Itoa(limit))
        params.Set("offset", strconv.Itoa(offset))

        resp, err := client.Get(path, params)
        if err != nil {
            return nil, err
        }

        var page PageResponse
        json.Unmarshal(resp, &page)

        allItems = append(allItems, page.Items...)

        if len(allItems) >= page.Total || len(page.Items) == 0 {
            break
        }
        offset += limit
    }

    return allItems, nil
}

// User-facing: --limit and --offset flags
// If user specifies --limit, use single-page mode
// If not specified, auto-paginate and show all results
```

**Progress indicators:**

```go
// For long-running operations (cluster create, provisioning)
func showProgress(ctx context.Context, client *APIClient, resourceUID string) error {
    spinner := progressbar.NewOptions(-1,
        progressbar.OptionSetDescription("Provisioning"),
        progressbar.OptionSpinnerType(14),
    )

    for {
        resource, err := client.GetResource(ctx, resourceUID)
        if err != nil {
            return err
        }

        switch resource.Status {
        case "active":
            spinner.Finish()
            fmt.Println("✓ Resource ready!")
            return nil
        case "failed":
            spinner.Finish()
            return fmt.Errorf("provisioning failed: %s", resource.ErrorMessage)
        default:
            spinner.Add(1)
            time.Sleep(2 * time.Second)
        }
    }
}
```

**Failure modes:**
- **Terminal width detection fails (piped output):** Default to 80 columns. Detect piped output via `os.Stdout.Fd()` check; if piped, use JSON format automatically.
- **Very long field values (e.g., 100-char hostname):** Truncate with `...` in table mode; show full value in JSON/YAML mode.
- **Non-UTF8 terminals:** Strip ANSI color codes when `--no-color` is set or `TERM=dumb`.

**Interviewer Q&As:**

**Q1: How do you decide what columns to show in `table` vs `wide` mode?**
A: `table` shows the 5-6 most important columns (name, type, status, project, age). `wide` shows all columns (15+), assuming a wide terminal. The column set is defined per resource type in a column registry. Users can customize with `--output custom-columns=...`.

**Q2: How do you handle pagination for interactive vs scripted usage?**
A: Interactive (terminal detected): auto-paginate and pipe through pager (`less`) for large results. Scripted (piped output): dump all results as JSON without pager. The user can override with `--limit` and `--offset` for manual pagination.

**Q3: How do you handle the output for long-running operations like cluster creation?**
A: Two modes: (1) Interactive (default): show a progress spinner with status updates, block until done. (2) Background (`--async` flag): return immediately with resource ID, user polls with `show` command.

**Q4: How do you handle color in different terminals?**
A: We use `fatih/color` library in Go. Color is auto-detected: enabled if stdout is a terminal, disabled if piped. `--no-color` flag overrides. We also respect `NO_COLOR` environment variable (https://no-color.org/).

**Q5: How does the custom column format work?**
A: Similar to kubectl: `--output custom-columns=NAME:.metadata.name,GPU:.spec.gpu.count`. We parse JSON paths and apply them to each result item. This is powered by a simple JSONPath evaluator.

**Q6: How do you format timestamps?**
A: By default, relative time for recent events ("5m ago", "2h ago", "3d ago") and absolute ISO 8601 for older events. `--output json` always uses ISO 8601. Users can set `settings.time_format: absolute` in config.

---

### 6.3 Offline Mode & Caching

**Why it's hard:** The CLI must gracefully handle API unavailability (network issues, maintenance windows). Caching must be correct (no stale data causing wrong decisions) while being useful (showing recent data is better than showing nothing).

| Approach | Pros | Cons |
|----------|------|------|
| **No caching** | Always fresh data; simple | CLI useless without API |
| **Aggressive caching (everything)** | Works fully offline | Stale data risk; disk usage |
| **Selective caching (read-only, TTL-based)** | Balance of freshness and availability | Must carefully choose what to cache |
| **Write-through cache** | Always consistent | No offline benefit for writes |

**Selected approach: Selective caching with TTL, read-only offline mode.**

**Implementation:**

```go
type CacheManager struct {
    basePath string     // ~/.infra-cli/cache/{context}/
    ttls     map[string]time.Duration
}

var defaultTTLs = map[string]time.Duration{
    "templates":     1 * time.Hour,     // Templates change rarely
    "machine_types": 30 * time.Minute,  // Hardware inventory changes slowly
    "machines":      5 * time.Minute,   // Availability changes frequently
    "quotas":        5 * time.Minute,   // Quotas change with provisioning
    "completions":   15 * time.Minute,  // Shell completion data
}

func (c *CacheManager) Get(key string) ([]byte, bool) {
    path := filepath.Join(c.basePath, key+".json")
    meta, err := c.loadMeta(key)
    if err != nil || time.Now().After(meta.ExpiresAt) {
        return nil, false  // Cache miss or expired
    }
    data, err := os.ReadFile(path)
    if err != nil {
        return nil, false
    }
    return data, true
}

func (c *CacheManager) Set(key string, data []byte, ttl time.Duration) error {
    path := filepath.Join(c.basePath, key+".json")
    os.MkdirAll(filepath.Dir(path), 0700)
    if err := os.WriteFile(path, data, 0600); err != nil {
        return err
    }
    return c.saveMeta(key, CacheMeta{
        ExpiresAt: time.Now().Add(ttl),
        CachedAt:  time.Now(),
    })
}

// Middleware: try API, fall back to cache
func (c *CacheManager) CachedGet(client *APIClient, path string, cacheKey string) ([]byte, error) {
    // Try API first
    data, err := client.Get(path)
    if err == nil {
        // Update cache
        ttl := c.ttls[strings.Split(cacheKey, "_")[0]]
        c.Set(cacheKey, data, ttl)
        return data, nil
    }

    // API failed; try cache
    if cached, ok := c.Get(cacheKey); ok {
        fmt.Fprintf(os.Stderr, "Warning: API unreachable. Showing cached data (age: %s)\n",
            time.Since(c.getMeta(cacheKey).CachedAt).Truncate(time.Second))
        return cached, nil
    }

    // No cache either
    return nil, fmt.Errorf("API unreachable and no cached data available: %w", err)
}
```

**Cache invalidation:**
- Write operations (`create`, `delete`) invalidate related caches (e.g., creating a reservation invalidates `machines` and `quotas` cache).
- `infra-cli cache clear` command for manual invalidation.
- Cache files have 0600 permissions.

**Failure modes:**
- **Stale cache shows deleted resource:** Acceptable in offline mode; the `(cached)` label warns the user. Write operations always go to API (no offline writes).
- **Cache corruption:** JSON parse failure triggers cache clear for that key; fresh fetch on next attempt.
- **Disk full:** Cache writes fail silently; CLI continues without caching.

**Interviewer Q&As:**

**Q1: Can users create resources in offline mode?**
A: No. Write operations always require API connectivity. Attempting a write in offline mode returns: "Error: API unreachable. Write operations require connectivity." This prevents inconsistent state.

**Q2: How do you prevent the cache from growing unbounded?**
A: (1) TTL-based expiry -- expired entries are lazily deleted on next access. (2) Max cache size: 50MB (configurable). When exceeded, LRU eviction. (3) Cache is per-context, so switching contexts doesn't pollute cache.

**Q3: What if the cached data is very stale (e.g., from 2 days ago)?**
A: We show a prominent warning with the cache age. If cache age exceeds 1 hour, we also output: "Warning: Cache is very stale. Data may be significantly out of date." The user can run `infra-cli cache clear` to force fresh data.

**Q4: How do you handle cache across CLI upgrades?**
A: Cache includes a `format_version` field. If the CLI version changes the cache format, old cache entries are ignored (treated as misses) and refreshed.

**Q5: Why not use SQLite for the cache?**
A: Overhead. JSON files are simpler, have no dependencies (no CGO needed for SQLite), and our cache is small (< 10MB). File-per-key also avoids database corruption issues.

**Q6: How does caching interact with shell completion?**
A: Shell completion uses cached data aggressively (15-min TTL for resource names, project names, etc.). This provides fast completion without API calls on every tab press. Stale completions are acceptable since they're suggestions, not authoritative.

---

### 6.4 HTTP Client & Resilience

**Why it's hard:** The CLI communicates with a remote API over unreliable networks. It must handle retries, timeouts, rate limiting, and connection issues without frustrating the user with long hangs or unhelpful errors.

| Approach | Pros | Cons |
|----------|------|------|
| **Naive http.Client** | Simple | No retries, no rate limit handling |
| **Custom retry middleware** | Full control over retry behavior | More code to maintain |
| **hashicorp/go-retryablehttp** | Battle-tested, configurable | Adds dependency |
| **Custom with go-retryablehttp + interceptors** | Combines proven retry logic with custom middleware | Slightly more complex |

**Selected approach: Custom HTTP client with go-retryablehttp + auth/logging interceptors.**

**Implementation:**

```go
type InfraHTTPClient struct {
    inner       *retryablehttp.Client
    authManager *AuthManager
    config      *Config
    logger      *slog.Logger
}

func NewInfraHTTPClient(auth *AuthManager, cfg *Config) *InfraHTTPClient {
    retryClient := retryablehttp.NewClient()
    retryClient.RetryMax = cfg.Settings.Retries        // default 3
    retryClient.RetryWaitMin = 1 * time.Second
    retryClient.RetryWaitMax = 30 * time.Second
    retryClient.CheckRetry = customRetryPolicy
    retryClient.Backoff = retryablehttp.LinearJitterBackoff
    retryClient.HTTPClient.Timeout = cfg.Settings.Timeout  // default 30s

    return &InfraHTTPClient{
        inner:       retryClient,
        authManager: auth,
        config:      cfg,
    }
}

// Custom retry policy: retry on 429, 502, 503, 504, and connection errors
// Do NOT retry on 400, 401, 403, 404, 409
func customRetryPolicy(ctx context.Context, resp *http.Response, err error) (bool, error) {
    if err != nil {
        // Retry on connection errors
        return true, nil
    }
    switch resp.StatusCode {
    case 429:
        // Rate limited: retry after Retry-After header
        return true, nil
    case 502, 503, 504:
        // Server error: retry
        return true, nil
    default:
        return false, nil
    }
}

func (c *InfraHTTPClient) Do(req *http.Request) (*http.Response, error) {
    // Add auth header
    token, err := c.authManager.GetValidToken()
    if err != nil {
        return nil, &CLIError{Message: "not authenticated", Hint: "Run 'infra-cli login'", Code: 3}
    }
    req.Header.Set("Authorization", "Bearer "+token.AccessToken)
    req.Header.Set("User-Agent", fmt.Sprintf("infra-cli/%s", version.Version))
    req.Header.Set("Accept", "application/json")

    // Verbose logging
    if c.config.Verbose {
        c.logRequest(req)
    }

    // Execute with retries
    retryReq, _ := retryablehttp.FromRequest(req)
    resp, err := c.inner.Do(retryReq)

    if c.config.Verbose && resp != nil {
        c.logResponse(resp)
    }

    // Handle 401 with token refresh
    if resp != nil && resp.StatusCode == 401 {
        newToken, refreshErr := c.authManager.RefreshToken(token)
        if refreshErr != nil {
            return nil, &CLIError{Message: "session expired", Hint: "Run 'infra-cli login'", Code: 3}
        }
        req.Header.Set("Authorization", "Bearer "+newToken.AccessToken)
        retryReq, _ = retryablehttp.FromRequest(req)
        return c.inner.Do(retryReq)
    }

    return resp, err
}
```

**Verbose mode output:**

```bash
$ infra-cli machines list --type gpu --verbose
> GET /api/v1/machines?type=gpu&status=available HTTP/1.1
> Host: api.staging.infra.company.com
> Authorization: Bearer eyJhbG...(truncated)
> Accept: application/json
> User-Agent: infra-cli/1.5.2

< HTTP/1.1 200 OK
< Content-Type: application/json
< X-Request-Id: req-abc123
< X-RateLimit-Remaining: 95
< X-RateLimit-Reset: 2026-04-09T10:01:00Z

HOSTNAME          TYPE       RACK       DATACENTER  STATUS
...
```

**Failure modes:**
- **All retries exhausted:** Show final error with all retry attempts logged (in verbose mode).
- **TLS certificate error:** Show clear error: "TLS certificate verification failed. If using a self-signed certificate, set `settings.insecure_skip_verify: true` in config."
- **DNS resolution failure:** Show: "Cannot resolve hostname. Check your network connection and endpoint URL."

**Interviewer Q&As:**

**Q1: How do you handle rate limiting gracefully?**
A: The client reads the `Retry-After` header and waits the specified time before retrying. If no header, it uses exponential backoff with jitter (1s, 2s, 4s). The user sees: "Rate limited. Retrying in Xs..." If `--quiet` is set, the retry happens silently.

**Q2: What's the difference between `--timeout` and retry delays?**
A: `--timeout` is per-request timeout (how long to wait for a single HTTP request). Retries create new requests. Total wall-clock time for a command is: `timeout * (retries + 1) + sum(retry_delays)`. Default: `30s * 4 = ~2 min max`.

**Q3: How do you handle long-polling for log streaming?**
A: `jobs logs --follow` uses HTTP long-polling or WebSocket. The HTTP client timeout is set to 0 (infinite) for streaming endpoints. We use chunked transfer encoding, reading line by line. A context cancellation (Ctrl+C) cleanly closes the connection.

**Q4: How do you handle proxy environments (corporate proxies)?**
A: The Go HTTP client automatically reads `HTTP_PROXY`, `HTTPS_PROXY`, and `NO_PROXY` environment variables. Additionally, the config supports explicit proxy settings: `settings.proxy: http://proxy.company.com:8080`.

**Q5: How do you test the HTTP client?**
A: (1) Unit tests with `httptest.NewServer` for mocking API responses. (2) Integration tests against a real staging API. (3) Chaos tests: inject random latency and errors using a proxy (like Toxiproxy) to verify retry and timeout behavior.

**Q6: How do you handle large response bodies (e.g., very long job logs)?**
A: Streaming. The HTTP client reads the response body as a stream, piping chunks to stdout. We never buffer the entire response in memory. For `jobs logs --follow`, we read line-by-line indefinitely until the job completes or the user presses Ctrl+C.

---

## 7. Scheduling & Resource Management

### CLI's Role in Scheduling

The CLI is a thin client; scheduling logic lives server-side. The CLI provides a user-friendly interface:

1. **Reservation creation:** `infra-cli reserve` sends the reservation request to the API. The server-side scheduler checks availability, resolves conflicts, and allocates servers.

2. **Scheduling visibility:**
   ```bash
   $ infra-cli machines availability --type gpu-h100 --from 2026-04-10 --to 2026-04-17
   DATE          AVAILABLE   TOTAL   RESERVED_BY
   2026-04-10    6           12      team-alpha(4), team-beta(2)
   2026-04-11    8           12      team-alpha(4)
   2026-04-12    10          12      team-gamma(2)
   2026-04-13    12          12      (none)
   ...
   ```

3. **Job scheduling:** `infra-cli jobs submit` sends the job to the server-side job scheduler (which handles queue priority, resource matching, and preemption).

4. **Cluster scheduling:** `infra-cli clusters create` triggers server-side K8s cluster provisioning, which includes node allocation from the bare-metal pool.

### Resource Conflict Resolution

The CLI doesn't resolve conflicts -- it presents them to the user:

```bash
$ infra-cli reserve --type gpu-h100 --count 8 --start 2026-04-10 --duration 8h --project ml-team
Error: insufficient capacity
  Requested: 8 gpu-h100 servers for 2026-04-10 08:00-16:00 UTC
  Available: 6 servers

  Conflicts:
    gpu-rack3-srv03: reserved by team-alpha (res-111, priority: high)
    gpu-rack3-srv04: reserved by team-alpha (res-111, priority: high)

  Options:
    1. Reduce count to 6: infra-cli reserve --type gpu-h100 --count 6 ...
    2. Choose a different time: infra-cli machines availability --type gpu-h100
    3. Request preemption (if your priority is higher): infra-cli reserve ... --priority critical
```

---

## 8. Scaling Strategy

The CLI itself doesn't need horizontal scaling -- it's a local binary. Scaling concerns are about the API it calls and the distribution infrastructure.

### Distribution Scaling

| Challenge | Solution |
|-----------|----------|
| Binary hosting for 4,000 users | CDN-backed release server (GitHub Releases, S3 + CloudFront) |
| Auto-update mechanism | `infra-cli update` checks GitHub Releases API; downloads new binary |
| Package manager distribution | Homebrew tap, apt/yum repo, Chocolatey package |
| Docker image for CI/CD | `ghcr.io/company/infra-cli:latest` published on release |

### API Scaling (from CLI perspective)

```bash
# The CLI should handle API scaling transparently
# - Connection pooling (Go's default HTTP client handles this)
# - Retries with backoff (handled by go-retryablehttp)
# - Rate limit compliance (read Retry-After headers)
# - Graceful degradation (cache for offline mode)
```

### Interviewer Q&As

**Q1: How do you handle CLI version compatibility with the API?**
A: The CLI sends its version in the `User-Agent` header. The API can return a `X-CLI-Min-Version` header. If the CLI version is below the minimum, it warns: "Your CLI version (1.3.0) is outdated. Minimum required: 1.5.0. Run 'infra-cli update'." We never hard-block old CLIs; we warn and eventually deprecate endpoints.

**Q2: How do you roll out a new CLI version without breaking existing users?**
A: (1) Semantic versioning: minor versions add features, patch versions fix bugs. (2) No breaking changes without major version bump. (3) New API fields are always additive (old CLIs ignore unknown fields). (4) `infra-cli update` for manual update. (5) Auto-update check on startup (once per day, non-blocking).

**Q3: What if the API adds a new resource type -- does the CLI need an update?**
A: For basic CRUD operations, the CLI can use generic resource commands (`infra-cli resources list --type new-thing`). But for a first-class experience (dedicated subcommand, specific flags, formatted output), a CLI update is needed. We decouple API changes from CLI releases via feature flags.

**Q4: How do you handle 10,000 concurrent CLI users?**
A: The CLI is stateless and runs locally. The burden is on the API, which handles 10,000 users via standard web scaling (load balancer, horizontal API pods, read replicas). From the CLI perspective, we ensure fair rate limiting (100 req/min per user) so no single user monopolizes API capacity.

**Q5: How do you handle CLI performance for users in different geographic regions?**
A: (1) Multiple API endpoints per region (configured in contexts). (2) CDN-served binary downloads. (3) Caching reduces API round-trips. (4) Compression (gzip) on API responses. (5) Connection reuse (HTTP/2 multiplexing when available).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | User Experience |
|---------|--------|-----------|------------|-----------------|
| API timeout | Command fails | HTTP timeout | Retry with backoff (3 attempts) | "Retrying... (attempt 2/3)" |
| API 500 error | Command fails | HTTP status code | Retry with backoff | "Server error. Retrying..." |
| API 429 rate limit | Command delayed | HTTP status + Retry-After | Wait and retry | "Rate limited. Retrying in 30s..." |
| Auth token expired | Command fails initially | 401 response | Auto-refresh; if fails, prompt login | Transparent (or "Session expired, run login") |
| DNS resolution fail | All commands fail | Connection error | Check network; suggest VPN | "Cannot resolve api.infra.company.com. Check network." |
| TLS cert error | All commands fail | TLS handshake error | Warn; suggest cert update | "TLS certificate error. Contact IT." |
| Config file corrupt | CLI won't start | YAML parse error | Create backup; reinitialize | "Config corrupt. Backup saved. Run 'infra-cli config init'" |
| Cache corrupt | Stale/missing data | JSON parse error | Clear cache for key | Silent recovery; fresh fetch |
| Disk full | Can't write config/cache | OS error | Warn; continue without cache | "Warning: cannot write cache (disk full)" |
| Binary outdated | Missing features, potential API compat | Version check on startup | Suggest update | "CLI v1.3.0 is outdated. Latest: v1.6.0" |

### Graceful Degradation

The CLI follows a "degrade gracefully, never crash" philosophy:

1. **No network:** Fall back to cache for reads; clear error for writes.
2. **Slow network:** Show spinner; respect timeout; retry transparently.
3. **Auth failure:** Clear error with actionable hint (run `login`).
4. **Unexpected API response:** Show raw response body in verbose mode; show generic error otherwise.
5. **Panics:** Recover from panics in all command handlers; show: "An unexpected error occurred. Please report this bug with --verbose output."

---

## 10. Observability

### Key Metrics

The CLI itself emits metrics via optional anonymous telemetry (opt-in):

| Metric | Type | Purpose |
|--------|------|---------|
| `cli.command.count` | Counter | Command usage frequency (which commands are popular) |
| `cli.command.latency` | Histogram | End-to-end command latency |
| `cli.command.error_rate` | Rate | Error rate by command and error type |
| `cli.auth.refresh_count` | Counter | Token refresh frequency |
| `cli.auth.login_count` | Counter | Login frequency |
| `cli.cache.hit_rate` | Rate | Cache effectiveness |
| `cli.version.distribution` | Gauge | Distribution of CLI versions in use |
| `cli.platform.distribution` | Gauge | OS/arch distribution (linux/darwin/windows, amd64/arm64) |
| `cli.api.latency` | Histogram | API call latency (from CLI perspective) |
| `cli.api.retry_count` | Counter | Retry frequency (indicates API instability) |
| `cli.offline.fallback_count` | Counter | How often offline mode is used |

### Debug Logging

```bash
$ INFRA_CLI_DEBUG=1 infra-cli machines list
[DEBUG] Loading config from ~/.infra-cli/config.yaml
[DEBUG] Current context: staging
[DEBUG] Loading token from keyring: staging
[DEBUG] Token valid (expires in 3h 42m)
[DEBUG] GET https://api.staging.infra.company.com/api/v1/machines?type=gpu&status=available
[DEBUG] Response: 200 OK (142ms, 2.3KB)
[DEBUG] Cache updated: machines_gpu (TTL: 5m)

HOSTNAME          TYPE       RACK       DATACENTER  STATUS
...
```

---

## 11. Security

### Auth & AuthZ

See Deep Dive 6.1 for authentication details.

**Authorization enforcement:**
- The CLI does not enforce authorization -- the API does.
- The CLI sends the JWT token with every request; the API validates permissions.
- The CLI displays permission errors with actionable hints.

### SSO Integration

- OAuth2 Device Flow with corporate Okta (OIDC).
- Supports SAML-initiated login (redirect to IdP for authentication).
- MFA is enforced by the IdP, transparent to the CLI.

### RBAC Enforcement

- RBAC is server-side. The CLI displays the user's current role:
  ```bash
  $ infra-cli whoami
  User:     alice@company.com
  Context:  staging
  Project:  ml-team (role: developer)
  Token:    valid (expires in 3h 42m)
  ```

### Token Security

| Threat | Mitigation |
|--------|------------|
| Token in config file | OS keyring primary; encrypted file fallback; 600 permissions |
| Token in shell history | `login --service-account` reads from stdin, not command line |
| Token in process listing | Token passed via header, not command-line argument |
| Token in verbose output | Access token truncated in verbose logging: `Bearer eyJhbG...(truncated)` |
| Token in CI/CD logs | `$INFRA_SA_TOKEN` never echoed; CLI masks env var values in debug output |

### Additional Security

- **TLS enforcement:** CLI rejects non-HTTPS endpoints (configurable override for local dev).
- **Certificate pinning:** Optional `settings.ca_cert` for custom CA certificates.
- **Binary verification:** Release binaries are signed; `infra-cli update` verifies signatures.
- **Supply chain:** Dependencies vendored; SBOM published with each release.

---

## 12. Incremental Rollout

### Phase 1: Core CLI (Weeks 1-3)
- Command structure (Cobra setup).
- OAuth2 device flow login.
- Config management (contexts, config file).
- `machines list/show` commands.
- Table and JSON output formatting.

### Phase 2: Resource Management (Weeks 4-6)
- `reservations` create/list/cancel.
- `resources` create/list/show/delete.
- `quota show`.
- Shell completion (bash/zsh/fish).
- Error handling with human-readable messages.

### Phase 3: K8s & Jobs (Weeks 7-9)
- `clusters` create/list/kubeconfig/scale/upgrade/delete.
- `jobs` submit/list/logs/cancel.
- Progress indicators for long operations.
- Pagination for large result sets.

### Phase 4: Polish & AI (Weeks 10-12)
- Offline mode with caching.
- `ask` command (AI integration).
- Auto-update mechanism.
- Plugin system.
- Telemetry (opt-in).
- Package manager distribution (Homebrew, apt, yum).

### Rollout Q&As

**Q1: How do you get developers to adopt the CLI over the web portal?**
A: (1) Make it faster: `infra-cli reserve` is quicker than navigating the portal. (2) Integrate with workflows: `infra-cli` in CI/CD pipelines for automated provisioning. (3) Scriptability: JSON output + exit codes enable shell scripting. (4) Convenience: shell completion, offline mode, context switching.

**Q2: How do you handle backwards compatibility when you need to rename a command?**
A: Add the new command name as an alias, keep the old name working. Print a deprecation warning: "Warning: 'infra-cli servers' is deprecated. Use 'infra-cli machines' instead." Remove the old name in the next major version (6+ months later).

**Q3: How do you test the CLI across platforms (Linux, macOS, Windows)?**
A: GitHub Actions matrix build: test on ubuntu-latest, macos-latest, windows-latest. Each platform runs unit tests and integration tests against a staging API. We also test on ARM64 via cross-compilation + QEMU.

**Q4: How do you handle the CLI when the API introduces a breaking change?**
A: API versioning (`/api/v1/`, `/api/v2/`). The CLI supports multiple API versions. On breaking API change, a new CLI major version is released that targets the new API version. Old CLI continues to work with old API version during the deprecation period.

**Q5: How do you handle feature flags in the CLI?**
A: The API returns feature flags in a `GET /api/v1/features` endpoint (cached 1h). The CLI checks feature flags before showing experimental commands. Hidden experimental commands are available via `infra-cli --experimental`.

---

## 13. Trade-offs & Decision Log

| Decision | Chosen | Alternative | Why |
|----------|--------|-------------|-----|
| Language | Go | Python (Click), Rust (Clap) | Go: single binary, fast startup, excellent CLI ecosystem (Cobra/Viper); Rust: slower compile times, steeper learning curve; Python: requires runtime, slow startup |
| CLI framework | Cobra + Viper | urfave/cli, Kong | Cobra: most popular Go CLI framework; used by kubectl, docker, gh; excellent completion support |
| Auth (interactive) | OAuth2 Device Flow | Auth Code + PKCE, API Key | Device flow works in headless/SSH; no local server needed |
| Auth (CI/CD) | Service account API key | OAuth2 client credentials | Simpler for CI/CD; no token refresh needed; scoped and rotatable |
| Token storage | OS Keyring + encrypted file fallback | Plain text file, Vault integration | Keyring is most secure for desktop; file fallback for headless servers; Vault is overkill for client-side |
| Config format | YAML | TOML, JSON, INI | YAML: human-readable, supports comments, familiar to infra engineers (k8s uses it) |
| Output default | Table | JSON | Table is more readable for interactive use; JSON via `--output json` for scripting |
| Caching | Selective TTL-based file cache | SQLite, Redis, no cache | Simple, no dependencies, good enough for our cache size (< 10MB) |
| Distribution | Homebrew + apt + binary download | Snap, Flatpak | Homebrew: standard for macOS; apt: standard for Linux; binary: universal fallback |
| Auto-update | Check on startup + manual `update` command | Forced auto-update, no update mechanism | Non-intrusive check respects user control; forced updates break CI/CD pipelines |

---

## 14. Agentic AI Integration

### Natural Language Interface

The `infra-cli ask` command integrates with an AI backend to translate natural language questions into infrastructure queries and actions.

**Architecture:**

```
+------------------+     +-------------------+     +-----------------+
| infra-cli ask    |---->| AI Gateway API    |---->| LLM (Claude)    |
| "which GPUs are  |     | /api/v1/ai/query  |     | with tool use   |
|  free next week?"|     +--------+----------+     +--------+--------+
                                  |                         |
                         +--------v-------------------------v--------+
                         | Tool Executor                              |
                         | - search_machines(type, status, date_range)|
                         | - list_reservations(date_range)            |
                         | - check_quota(project)                     |
                         | - estimate_cost(spec)                      |
                         | - get_metrics(resource, time_range)        |
                         +-------------------------------------------+
```

**Implementation:**

```go
func askCmd() *cobra.Command {
    cmd := &cobra.Command{
        Use:   "ask [question]",
        Short: "Ask a natural language question about infrastructure",
        Args:  cobra.MinimumNArgs(1),
        RunE: func(cmd *cobra.Command, args []string) error {
            question := strings.Join(args, " ")
            client := getAPIClient(cmd)

            fmt.Fprintf(os.Stderr, "Querying AI assistant...\n\n")

            // Stream response for real-time output
            stream, err := client.PostStream("/api/v1/ai/query", map[string]string{
                "question": question,
                "project":  config.Current().DefaultProject,
            })
            if err != nil {
                return err
            }

            for chunk := range stream {
                fmt.Print(chunk.Text)
            }
            fmt.Println()

            return nil
        },
    }
    return cmd
}
```

**Capabilities:**
- Resource discovery: "Show me all idle GPU machines"
- Cost analysis: "What's the cheapest way to run 100 training jobs?"
- Capacity planning: "Will we have enough GPUs for the Q3 ML sprint?"
- Troubleshooting: "Why did my last job fail?"
- What-if analysis: "What if I scale my cluster to 20 nodes?"

**Safety:**
- AI can only query -- it cannot create, modify, or delete resources.
- Responses include a disclaimer: "AI-generated response. Verify critical information."
- Commands suggested by the AI are shown but not auto-executed.
- Rate limited: 10 AI queries per user per hour.

### AI-Assisted Command Suggestion

```bash
$ infra-cli --help-ai "I need to set up infrastructure for a new ML project"
Based on your request, here's a suggested workflow:

1. Create a project:
   infra-cli projects create --name ml-new-project --budget 5000

2. Set up a K8s cluster for orchestration:
   infra-cli clusters create --name ml-cluster --version 1.29 --nodes 5 --node-type cpu-epyc-64c --project ml-new-project

3. Reserve GPU servers for training:
   infra-cli reserve --type gpu-h100 --count 4 --start tomorrow --duration 7d --project ml-new-project

4. Check quota:
   infra-cli quota show --project ml-new-project

Would you like me to explain any of these commands in detail?
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through the complete flow when a user runs `infra-cli reserve --type gpu-h100 --count 4 --start 2026-04-10 --duration 8h --project ml-team`.**
A: (1) Cobra parses the command and flags. (2) Config Manager loads the current context from `~/.infra-cli/config.yaml`. (3) Auth Manager loads the token from OS keyring, checks expiry (refreshes if needed). (4) HTTP Client sends `POST /api/v1/reservations` with the reservation spec and Bearer token. (5) Server validates, checks quota, checks availability, creates reservation. (6) Server returns 201 with reservation details. (7) Output Formatter renders the response as a table (or JSON if `--output json`). (8) Cache Manager invalidates `machines` and `quotas` cache entries.

**Q2: How do you design the CLI to be scriptable (used in shell scripts and CI/CD)?**
A: (1) Meaningful exit codes (0 success, 1-5 specific errors). (2) `--output json` for machine-parseable output. (3) `--yes` flag to skip confirmation prompts. (4) `--quiet` flag to suppress non-essential output. (5) Errors go to stderr, data goes to stdout. (6) Piped output auto-detection (disable colors, disable pager). (7) `INFRA_SA_TOKEN` env var for non-interactive auth.

**Q3: How does `infra-cli clusters kubeconfig --name my-k8s > ~/.kube/config` work?**
A: The CLI calls `GET /api/v1/clusters/my-k8s/kubeconfig`. The API generates a kubeconfig with: (1) cluster CA certificate, (2) API server endpoint, (3) user credentials (either a short-lived client certificate or an OIDC token configuration that uses the user's existing Okta auth). The CLI outputs the raw kubeconfig YAML to stdout, which shell redirection writes to `~/.kube/config`.

**Q4: How do you handle the case where a user runs two CLI commands simultaneously that both need token refresh?**
A: The Auth Manager uses a mutex (`sync.Mutex`) around the refresh operation. The first command to detect token expiry acquires the lock and refreshes. The second command waits on the lock. When the lock is released, the second command checks if the token was already refreshed (by comparing access_token values) and uses the new token without re-refreshing. This prevents duplicate refresh calls and race conditions on the token file.

**Q5: Why did you choose Go over Python for the CLI?**
A: (1) Single static binary: no runtime dependency, trivial distribution. (2) Fast startup: < 50ms vs Python's 200-500ms import time. (3) Cross-compilation: `GOOS=darwin GOARCH=arm64 go build` for any platform. (4) Cobra ecosystem: completion, man page generation, help formatting. (5) Concurrency: goroutines for parallel API calls (e.g., fetching machine list + quota simultaneously). Python Click is excellent but the startup time and packaging story (virtualenvs, pip) are worse for CLIs.

**Q6: How do you implement `infra-cli jobs logs job-123 --follow`?**
A: The CLI sends `GET /api/v1/jobs/job-123/logs?follow=true`. The server responds with `Transfer-Encoding: chunked` and streams log lines as they appear. The CLI reads the response body line-by-line using `bufio.Scanner` and prints each line to stdout. When the job completes, the server closes the connection. If the user presses Ctrl+C, the CLI cancels the context, which closes the HTTP connection cleanly.

**Q7: How do you handle CLI configuration across a team (e.g., everyone needs the same endpoint and defaults)?**
A: Support a "team config" file that can be distributed via git or a package: `/etc/infra-cli/config.yaml` (system-wide) + `~/.infra-cli/config.yaml` (user-specific). User config overrides system config. Teams can also distribute a `.infra-cli.yaml` in their git repo root (project-level defaults). Precedence: CLI flags > env vars > user config > project config > system config.

**Q8: How do you make the CLI self-documenting?**
A: (1) Every command has `Short` and `Long` descriptions in Cobra. (2) `infra-cli <command> --help` shows usage, flags, and examples. (3) Man pages auto-generated from Cobra commands: `infra-cli docs man > /usr/share/man/man1/infra-cli.1`. (4) Examples are embedded in command definitions and shown in help text. (5) `infra-cli docs` opens online documentation in browser.

**Q9: How do you handle breaking changes in the CLI output format?**
A: (1) Table format can change between versions (it's for human consumption). (2) JSON format is stable: fields are never removed or renamed within a major version. (3) New fields are additive. (4) We publish a JSON schema for each resource type. (5) Scripts should use `--output json` and parse with `jq`, never parse table output.

**Q10: How do you debug issues when a user reports "the CLI isn't working"?**
A: (1) Ask user to run with `--verbose` flag (shows HTTP request/response). (2) Ask user to set `INFRA_CLI_DEBUG=1` (shows internal state: config loading, token management, cache hits). (3) `infra-cli version` shows CLI version, Go version, build date, OS/arch. (4) `infra-cli config view` shows current config (tokens redacted). (5) All this info can be pasted into a bug report.

**Q11: How do you handle the case where the user's local clock is skewed (affecting token expiry)?**
A: We compare the local time against the server's `Date` response header. If clock skew exceeds 60 seconds, we warn: "Warning: Your local clock is skewed by 5 minutes from the server. This may cause authentication issues." For token expiry checks, we use the server's time as the reference.

**Q12: How do you implement `infra-cli update`?**
A: (1) CLI calls GitHub Releases API to get the latest version. (2) Compares with current version (semver comparison). (3) If newer version available, downloads the binary for the user's OS/arch. (4) Verifies checksum (SHA-256) and signature (GPG or cosign). (5) Replaces the current binary (platform-specific: on Unix, rename; on Windows, rename + restart). (6) Shows changelog summary.

**Q13: How do you handle very large outputs (e.g., listing 10,000 resources)?**
A: (1) Auto-pagination: the CLI fetches pages of 100 items from the API. (2) For table output: pipe through configured pager (`less`). (3) For JSON output: stream JSON array items, don't buffer all in memory. (4) `--limit` flag to cap results: `infra-cli resources list --limit 50`. (5) Server-side filtering to reduce data: `--status active --type vm`.

**Q14: How do you implement the plugin system?**
A: Similar to git/kubectl: (1) Plugin is any executable named `infra-cli-<name>` on the PATH. (2) `infra-cli <name>` invokes the plugin. (3) Plugin receives config via environment variables (`INFRA_CLI_CONTEXT`, `INFRA_CLI_TOKEN`, `INFRA_CLI_ENDPOINT`). (4) `infra-cli plugin list` shows installed plugins. (5) Plugins can be distributed via a plugin registry or package managers.

**Q15: How do you handle confirmation for destructive operations?**
A: (1) Default: interactive confirmation prompt ("Are you sure? [y/N]"). (2) `--yes` flag to skip confirmation (for scripts). (3) Extra-destructive operations (like deleting a K8s cluster) require typing the resource name: "Type the cluster name to confirm deletion: my-k8s". (4) `--dry-run` flag to preview the operation without executing.

**Q16: How would you add support for a new infrastructure type without changing the CLI?**
A: The generic `infra-cli resources` command supports any resource type via `--type`. New types are automatically available for basic CRUD. For a richer experience (dedicated subcommand), either: (1) release a CLI update, or (2) use the plugin system to add `infra-cli-<new-type>` as a plugin that can be installed independently.

---

## 16. References

1. **Cobra** - Go CLI framework: https://github.com/spf13/cobra
2. **Viper** - Go configuration management: https://github.com/spf13/viper
3. **OAuth2 Device Authorization Grant** - RFC 8628: https://datatracker.ietf.org/doc/html/rfc8628
4. **go-retryablehttp** - Retryable HTTP client: https://github.com/hashicorp/go-retryablehttp
5. **kubectl** - Reference CLI design: https://kubernetes.io/docs/reference/kubectl/
6. **GitHub CLI (gh)** - Reference CLI UX: https://cli.github.com/
7. **12 Factor CLI Apps** - Design principles: https://medium.com/@jdxcode/12-factor-cli-apps-dd3c227a0e46
8. **No Color** - Convention for respecting color preferences: https://no-color.org/
9. **XDG Base Directory Specification** - Config file locations: https://specifications.freedesktop.org/basedir-spec/latest/
10. **Charm** - Go TUI/CLI libraries (lipgloss, bubbletea): https://charm.sh/
