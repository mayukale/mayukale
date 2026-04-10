# System Design: Blue-Green Deployment System

> **Relevance to role:** A cloud infrastructure platform engineer must design deployment strategies that enable zero-downtime releases for services running on Kubernetes, bare-metal servers, and VMs. Blue-green deployment is the foundational pattern: maintain two identical environments, switch traffic atomically, and roll back instantly. Interviewers expect you to discuss the infrastructure cost implications (2x environments), database compatibility challenges, load balancer vs. DNS-based switching, and how blue-green integrates with CI/CD pipelines and Kubernetes-native tooling.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|-------------|
| FR-1 | Maintain two identical production environments: **Blue** (current live) and **Green** (new version). |
| FR-2 | Deploy new version to the idle environment (Green) without affecting live traffic. |
| FR-3 | Run smoke tests and health checks against Green before switching traffic. |
| FR-4 | Switch traffic from Blue to Green atomically (< 1 second for LB-based, < 5 min for DNS-based). |
| FR-5 | Rollback by switching traffic back to Blue (instant, no redeployment needed). |
| FR-6 | Support blue-green for Kubernetes workloads (Service selector swap). |
| FR-7 | Support blue-green for bare-metal/VM services (load balancer backend swap). |
| FR-8 | Support database schema changes with backward compatibility. |
| FR-9 | Integrate with CI/CD pipeline: pipeline triggers deploy to Green, smoke test, switch, and monitors. |
| FR-10 | Provide a canary step before full switch (optional: route small % of traffic to Green first). |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Zero dropped requests during traffic switch | 0 errors attributable to switch |
| NFR-2 | Traffic switch time | < 1 s (LB-based) |
| NFR-3 | Rollback time | < 5 s |
| NFR-4 | Smoke test suite runtime | < 2 min |
| NFR-5 | Maximum additional infrastructure cost | 2x during deploy window, 1x after teardown |
| NFR-6 | System availability | 99.99% (52 min downtime/year) |

### Constraints & Assumptions
- Services are stateless (state lives in external databases, caches, or object storage).
- Database schema changes are backward-compatible (both Blue and Green code must work with the same DB schema).
- Load balancers are under our control (HAProxy, Envoy, or cloud ALB).
- Kubernetes clusters use standard Service and Deployment objects.
- Bare-metal fleet uses HAProxy or Envoy as the edge load balancer.

### Out of Scope
- Stateful services with in-process state (require special handling beyond this design).
- Multi-region blue-green (each region does its own blue-green independently).
- Feature flag integration (covered in `feature_flag_service.md`).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Services managed | Given | 500 |
| Deployments/day (across all services) | 500 services x 2 deploys/week / 5 working days | ~200/day |
| Peak deployments/hour | 3x average (post-standup) | ~25/hour |
| Instances per service (avg) | Varies: 3-50 pods, 5-200 bare-metal hosts | avg 20 |
| Total instances managed | 500 services x 20 avg | 10,000 |
| RPS per service (avg, during switch) | Varies widely | 1K-100K RPS |
| In-flight requests at switch time | 100K RPS x 0.1s avg latency | ~10K |

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Traffic switch (LB backend swap) | < 1 s |
| Traffic switch (DNS-based) | < 5 min (depends on TTL) |
| Smoke test suite | < 2 min |
| Green environment provisioning (k8s) | < 3 min (pod scheduling + readiness) |
| Green environment provisioning (bare-metal) | < 10 min (binary deploy + health check) |
| Rollback | < 5 s |
| Full deploy cycle (deploy + test + switch) | < 15 min |

### Storage Estimates

| Item | Calculation | Value |
|------|-------------|-------|
| Deployment records | 200/day x 365 | 73,000/year |
| Record size | ~2 KB per deployment | ~146 MB/year |
| Environment snapshots (for audit) | 200/day x 10 KB | ~730 MB/year |
| This system is lightweight; the actual cost is in compute resources (2x during deploy). |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| Image pull during Green provisioning | 150 MB image x 20 pods | 3 GB per deploy |
| Health check traffic | 20 pods x 1 req/s x 200 bytes | negligible |
| LB config updates | < 1 KB per switch | negligible |

**Cost of 2x infrastructure during deploy:**
- Average deploy window: 15 minutes.
- Idle Green environment cost: 20 pods x 2 CPU x $0.048/CPU-hour / 4 (15 min) = $0.48/deploy.
- At 200 deploys/day: ~$96/day = ~$2,880/month.
- After switch, old Blue can be scaled down. If kept as hot standby for 30 min: add $2,880/month.
- Total incremental cost: ~$5,760/month for instant rollback capability.

---

## 3. High Level Architecture

```
                    +------------------+
                    |    CI/CD         |
                    |    Pipeline      |
                    +--------+---------+
                             |
                    1. Deploy to Green
                             |
                             v
                    +--------+---------+
                    |  Blue-Green      |
                    |  Controller      |
                    +--+-----+-----+--+
                       |     |     |
           +-----------+     |     +------------+
           |                 |                  |
           v                 v                  v
   +-------+------+  +------+-------+  +-------+------+
   | Smoke Test   |  | Load Balancer|  | Monitoring   |
   | Runner       |  | Manager      |  | Checker      |
   +-------+------+  +------+-------+  +-------+------+
           |                 |                  |
           |    2. Tests     |                  |
           |    pass         |                  |
           |                 |                  |
           v                 v                  v
   +-------+------+  +------+-------+  +-------+------+
   | Test Results  |  | LB / Ingress|  | Prometheus   |
   |              |  | (HAProxy /  |  | / Grafana    |
   |              |  |  Envoy /    |  |              |
   |              |  |  k8s Svc)   |  |              |
   +--------------+  +------+-------+  +--------------+
                            |
               3. Switch traffic
                            |
            +---------------+---------------+
            |                               |
     +------v------+                 +------v------+
     |   BLUE      |   (old live)    |   GREEN     |  (new version)
     |  Environment|                 |  Environment|
     |             |                 |             |
     | Pods/Hosts  |                 | Pods/Hosts  |
     | v1.2.3      |                 | v1.2.4      |
     +-------------+                 +-------------+
            |                               |
            +---------- Shared DB ----------+
            |        (backward-compatible   |
            |         schema)               |
            +-------------------------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **CI/CD Pipeline** | Triggers deployment to Green environment, waits for smoke test results, requests traffic switch. |
| **Blue-Green Controller** | Orchestrates the entire lifecycle: provision Green, run tests, switch traffic, monitor, teardown old. Implemented as a Kubernetes controller or a standalone service. |
| **Smoke Test Runner** | Executes a suite of health checks and functional tests against the Green environment before it receives production traffic. |
| **Load Balancer Manager** | Abstracts the traffic switching mechanism (LB backend swap, k8s Service selector, DNS update). |
| **Monitoring Checker** | After switch, monitors error rates and latency. If thresholds are breached, triggers automatic rollback. |
| **Blue Environment** | Current production serving live traffic (version N). |
| **Green Environment** | New version (N+1) deployed and tested but not yet receiving production traffic. |
| **Shared Database** | Both environments point to the same database. Schema must be compatible with both versions. |

### Data Flows

1. **Deploy:** Pipeline triggers Controller -> Controller provisions Green (k8s Deployment or bare-metal push) -> Green pods become ready.
2. **Test:** Controller invokes Smoke Test Runner against Green's internal endpoint -> Tests pass/fail -> Controller decides proceed or abort.
3. **Switch:** Controller tells LB Manager to swap backends -> LB Manager updates LB config -> Traffic flows to Green.
4. **Monitor:** Controller starts 5-minute post-switch monitoring period -> Monitoring Checker queries Prometheus for error rate and p99 latency -> If thresholds breached, trigger rollback.
5. **Rollback (if needed):** Controller tells LB Manager to swap back to Blue -> Traffic returns to Blue -> Green is investigated.
6. **Cleanup:** After bake period (30 min), Controller scales down Blue (or keeps as warm standby).

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Blue-green environment pairs
CREATE TABLE environments (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    service_name    VARCHAR(255) NOT NULL,
    slot            ENUM('blue', 'green') NOT NULL,
    version         VARCHAR(128) NOT NULL,              -- e.g., "v1.2.4" or commit SHA
    image_digest    CHAR(71),                           -- sha256:hex
    status          ENUM('provisioning', 'ready', 'live', 'draining', 'idle', 'terminated') NOT NULL,
    instance_count  INT NOT NULL,
    health_status   ENUM('healthy', 'degraded', 'unhealthy', 'unknown') NOT NULL DEFAULT 'unknown',
    endpoint        VARCHAR(512),                       -- internal URL for testing
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_service_slot (service_name, slot),
    INDEX idx_status (status)
) ENGINE=InnoDB;

-- Deployment events
CREATE TABLE deployments (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    service_name    VARCHAR(255) NOT NULL,
    from_version    VARCHAR(128) NOT NULL,
    to_version      VARCHAR(128) NOT NULL,
    from_slot       ENUM('blue', 'green') NOT NULL,     -- which was live before
    to_slot         ENUM('blue', 'green') NOT NULL,     -- which is live after
    status          ENUM('pending', 'deploying', 'testing', 'switching', 'monitoring',
                         'completed', 'rolled_back', 'failed', 'cancelled') NOT NULL DEFAULT 'pending',
    triggered_by    VARCHAR(255) NOT NULL,
    pipeline_run_id BIGINT,
    smoke_test_passed BOOLEAN,
    switch_at       DATETIME,                           -- when traffic was switched
    rollback_at     DATETIME,                           -- if rolled back
    error_message   TEXT,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    finished_at     DATETIME,
    INDEX idx_service_status (service_name, status),
    INDEX idx_created (created_at)
) ENGINE=InnoDB;

-- Smoke test results
CREATE TABLE smoke_test_results (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    deployment_id   BIGINT NOT NULL,
    test_name       VARCHAR(255) NOT NULL,
    status          ENUM('passed', 'failed', 'skipped', 'timed_out') NOT NULL,
    duration_ms     INT NOT NULL,
    error_message   TEXT,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (deployment_id) REFERENCES deployments(id),
    INDEX idx_deployment (deployment_id)
) ENGINE=InnoDB;

-- Traffic switch audit log
CREATE TABLE traffic_switches (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    deployment_id   BIGINT NOT NULL,
    switch_type     ENUM('blue_to_green', 'green_to_blue', 'rollback') NOT NULL,
    mechanism       ENUM('lb_backend', 'k8s_service', 'dns', 'envoy_route') NOT NULL,
    initiated_by    VARCHAR(255) NOT NULL,               -- "controller", "operator:alice"
    pre_switch_rps  INT,
    post_switch_rps INT,
    duration_ms     INT NOT NULL,                        -- how long the switch took
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (deployment_id) REFERENCES deployments(id),
    INDEX idx_deployment (deployment_id)
) ENGINE=InnoDB;
```

### Database Selection

| Store | Engine | Rationale |
|-------|--------|-----------|
| Deployment state | MySQL 8.0 | Transactional state machine transitions; familiar operational model; low write volume (~200/day). |
| Audit logs (long-term) | S3 + Elasticsearch | Immutable audit; full-text search for investigations. |
| Traffic switch state (real-time) | Redis | Fast reads for "which slot is live?" query; sub-ms latency for LB integration. |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| environments | (service_name, slot) UNIQUE | Quick lookup: "what's running in Blue for service X?" |
| deployments | (service_name, status) | Dashboard: "show active deployments for service X" |
| deployments | (created_at) | Time-range queries for audit |
| smoke_test_results | (deployment_id) | "Show all test results for deployment Y" |

---

## 5. API Design

### REST Endpoints

```
# Environment management
GET    /api/v1/services/{name}/environments          # Get Blue + Green status
GET    /api/v1/services/{name}/environments/{slot}    # Get specific slot details

# Deployment lifecycle
POST   /api/v1/services/{name}/deploy                # Start a blue-green deployment
       Body: { "version": "v1.2.4", "image": "registry/svc@sha256:...", "auto_switch": true }
GET    /api/v1/deployments/{id}                       # Get deployment status
POST   /api/v1/deployments/{id}/switch                # Manually switch traffic to Green
POST   /api/v1/deployments/{id}/rollback              # Rollback to Blue
POST   /api/v1/deployments/{id}/cancel                # Cancel deployment (teardown Green)

# Smoke tests
GET    /api/v1/deployments/{id}/smoke-tests           # Get smoke test results
POST   /api/v1/deployments/{id}/smoke-tests/rerun     # Re-run smoke tests

# Health & traffic
GET    /api/v1/services/{name}/live-slot              # Which slot is currently live?
GET    /api/v1/services/{name}/traffic-stats           # RPS, error rate per slot
```

**Example: Trigger deployment**
```json
POST /api/v1/services/order-svc/deploy
{
  "version": "v1.2.4",
  "image": "registry.internal/org/order-svc@sha256:abc123...",
  "auto_switch": true,
  "smoke_test_suite": "standard",
  "monitoring_window_minutes": 5,
  "rollback_on_error_rate": 0.01
}

Response 201:
{
  "deployment_id": 4567,
  "status": "deploying",
  "target_slot": "green",
  "estimated_completion": "2026-04-09T10:45:00Z"
}
```

### CLI

```bash
# Start deployment
bgctl deploy --service order-svc --version v1.2.4 --image registry.internal/org/order-svc@sha256:abc123

# Check status
bgctl status --service order-svc

# Manually switch traffic
bgctl switch --service order-svc --to green

# Rollback
bgctl rollback --service order-svc

# Show which slot is live
bgctl live --service order-svc

# Watch deployment progress
bgctl watch --deployment 4567

# List recent deployments
bgctl history --service order-svc --limit 20
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Traffic Switching Mechanism

**Why it's hard:**
Traffic switching must be zero-downtime: no dropped requests, no split-brain (traffic going to both versions simultaneously), and no connection draining issues. The mechanism differs dramatically between Kubernetes (Service selector swap), bare-metal (LB backend swap), and DNS-based switching (TTL propagation delay).

**Approaches:**

| Approach | Switch Time | Rollback Time | Dropped Requests | Cost |
|----------|-------------|---------------|------------------|------|
| **Kubernetes Service selector swap** | < 1 s | < 1 s | 0 (graceful) | Low (k8s native) |
| **Load balancer backend swap (HAProxy/Envoy)** | < 1 s | < 1 s | 0 (connection draining) | Medium (LB management) |
| **DNS-based switching (Route 53)** | 30 s - 5 min (TTL) | 30 s - 5 min | Some (stale DNS cache) | Low (but slow) |
| **Envoy xDS dynamic route** | < 100 ms | < 100 ms | 0 (hot route update) | Medium (Envoy control plane) |

**Selected approach: Kubernetes Service selector swap (k8s) + Envoy xDS (bare-metal)**

**Justification:** For Kubernetes, a Service selector swap is the simplest and most reliable mechanism. The `kube-proxy` (or Cilium/Calico) updates iptables/eBPF rules in milliseconds. For bare-metal, Envoy's xDS API allows dynamic backend updates without reload, achieving sub-second switching with full connection draining.

**Implementation detail - Kubernetes:**

```yaml
# Blue Deployment
apiVersion: apps/v1
kind: Deployment
metadata:
  name: order-svc-blue
  labels:
    app: order-svc
    slot: blue
spec:
  replicas: 10
  selector:
    matchLabels:
      app: order-svc
      slot: blue
  template:
    metadata:
      labels:
        app: order-svc
        slot: blue
        version: v1.2.3
    spec:
      containers:
        - name: order-svc
          image: registry.internal/org/order-svc@sha256:old...
          ports:
            - containerPort: 8080
          readinessProbe:
            httpGet:
              path: /health/ready
              port: 8080
            periodSeconds: 5
          lifecycle:
            preStop:
              exec:
                command: ["sh", "-c", "sleep 10"]  # graceful drain

---
# Green Deployment
apiVersion: apps/v1
kind: Deployment
metadata:
  name: order-svc-green
  labels:
    app: order-svc
    slot: green
spec:
  replicas: 10
  selector:
    matchLabels:
      app: order-svc
      slot: green
  template:
    metadata:
      labels:
        app: order-svc
        slot: green
        version: v1.2.4
    spec:
      containers:
        - name: order-svc
          image: registry.internal/org/order-svc@sha256:new...
          # same config as blue

---
# Service (traffic switch = change selector)
apiVersion: v1
kind: Service
metadata:
  name: order-svc
spec:
  selector:
    app: order-svc
    slot: blue               # <-- SWITCH: change to "green"
  ports:
    - port: 80
      targetPort: 8080
```

**Traffic switch operation (controller logic):**

```python
# Blue-Green Controller - Kubernetes traffic switch
from kubernetes import client, config

def switch_traffic(service_name: str, target_slot: str, namespace: str = "production"):
    """
    Atomically switch traffic by updating the Service selector.
    """
    config.load_incluster_config()
    v1 = client.CoreV1Api()
    
    # Verify target slot is ready
    apps_v1 = client.AppsV1Api()
    deployment_name = f"{service_name}-{target_slot}"
    deployment = apps_v1.read_namespaced_deployment(deployment_name, namespace)
    
    if deployment.status.ready_replicas != deployment.spec.replicas:
        raise Exception(
            f"Target deployment not ready: "
            f"{deployment.status.ready_replicas}/{deployment.spec.replicas} replicas ready"
        )
    
    # Patch Service selector
    body = {
        "spec": {
            "selector": {
                "app": service_name,
                "slot": target_slot
            }
        }
    }
    
    v1.patch_namespaced_service(service_name, namespace, body)
    
    # Record switch time for audit
    return {
        "service": service_name,
        "target_slot": target_slot,
        "switched_at": datetime.utcnow().isoformat(),
        "ready_replicas": deployment.status.ready_replicas
    }
```

**Implementation detail - Bare-metal (Envoy xDS):**

```python
# Envoy xDS control plane - switch backends
def switch_envoy_backends(
    service_name: str,
    target_backends: list[str],  # ["10.0.1.1:8080", "10.0.1.2:8080", ...]
    envoy_control_plane_url: str
):
    """
    Update Envoy's cluster endpoints via xDS API.
    Envoy performs graceful connection draining automatically.
    """
    cluster_config = {
        "cluster_name": f"{service_name}-production",
        "endpoints": [
            {
                "lb_endpoints": [
                    {"endpoint": {"address": {"socket_address": {"address": host, "port_value": int(port)}}}}
                    for backend in target_backends
                    for host, port in [backend.split(":")]
                ]
            }
        ]
    }
    
    # Push to Envoy control plane (e.g., go-control-plane or Envoy's Admin API)
    response = requests.post(
        f"{envoy_control_plane_url}/v3/discovery:endpoints",
        json=cluster_config
    )
    response.raise_for_status()
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Service selector patch fails (k8s API down) | Traffic stays on Blue (safe) | Retry 3x; if API is truly down, alert and manual intervention |
| Green pods not ready when switch attempted | Switch blocked | Controller verifies `readyReplicas == replicas` before switching |
| In-flight requests dropped during switch | Brief error spike | `preStop` hook sleeps 10s to allow existing connections to complete. Kubernetes removes pod from Service endpoints before sending SIGTERM. |
| Envoy xDS update rejected (bad config) | Traffic stays on old backends (safe) | Validate config before pushing; Envoy rejects invalid configs gracefully |
| Split-brain: Service selector partially applied | Some kube-proxys route to Blue, others to Green | K8s Service update is atomic in etcd; kube-proxy reconciles within seconds |

**Interviewer Q&As:**

**Q1: How do you handle in-flight requests during the traffic switch?**
A: Kubernetes handles this gracefully. When the Service selector changes, kube-proxy updates iptables/eBPF rules on each node. New connections go to Green. Existing TCP connections to Blue pods continue until they complete. The `preStop` hook in Blue pods adds a 10-second delay before the container receives SIGTERM, ensuring in-flight requests finish. For HTTP/2 and gRPC, the GOAWAY frame signals clients to reconnect, which they do against Green.

**Q2: Why not use DNS-based switching?**
A: DNS has a TTL problem. Even with TTL=30s, some DNS resolvers (and client libraries like Java's `InetAddress` cache) ignore TTL and cache for minutes or hours. This means traffic "leaks" to the old environment for an unpredictable duration after the switch. LB-based switching is deterministic and instant. DNS is acceptable only when LB-based switching isn't possible (e.g., cross-cloud failover).

**Q3: How do you prevent two simultaneous blue-green deployments for the same service?**
A: The `deployments` table has a status constraint: only one deployment per service can be in a non-terminal state (`pending`, `deploying`, `testing`, `switching`, `monitoring`). The controller acquires a distributed lock (Redis `SETNX` with TTL) before starting a new deployment. If a deployment is already in progress, the new one is rejected with a clear error message.

**Q4: What happens if the Green environment fails its smoke tests?**
A: Traffic stays on Blue (Green never received production traffic). The controller marks the deployment as `failed`, tears down Green pods, and sends a notification. The Blue environment is unaffected. The developer investigates the smoke test failure and pushes a fix, which triggers a new deployment.

**Q5: How do you handle WebSocket connections during the switch?**
A: WebSocket connections are long-lived and would be disrupted by the switch. We handle this by: (1) Not immediately terminating Blue pods -- they stay alive for up to 5 minutes to allow existing WebSocket connections to complete gracefully. (2) The `preStop` hook for Blue pods waits for active WebSocket connections to close (up to the graceful termination period). (3) Clients reconnect to the new Green endpoint.

**Q6: Can you do a partial switch (canary) before the full blue-green swap?**
A: Yes. With Istio/Envoy, we can split traffic: 5% to Green, 95% to Blue. This is a canary-before-blue-green pattern. We observe Green at 5% for 5 minutes, then switch to 100%. This reduces risk but adds complexity (need service mesh). For simpler setups, we rely on thorough smoke tests as the pre-switch validation.

---

### Deep Dive 2: Database Compatibility During Blue-Green

**Why it's hard:**
Both Blue (v1.2.3) and Green (v1.2.4) point to the same database. If Green requires a schema change, it must be backward-compatible -- both versions must work. This is the hardest part of blue-green deployments and the most common interview topic.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Expand-contract (additive migrations only)** | Both versions work; proven pattern | Requires discipline; multiple deploy cycles for destructive changes |
| **Dual-write** | Both versions read/write their own columns | Complex; data inconsistency risk |
| **Database-per-environment** | Full isolation | Data divergence; need to sync state; impractical for most apps |
| **Feature flags on schema** | Toggle which schema version the code uses | Adds code complexity; testing burden |

**Selected approach: Expand-contract with migration phases**

**Justification:** This is the industry standard (used by GitHub, Shopify, Stripe). It's simple to reason about, requires no special tooling, and works with any database. The key insight: every schema change is split into phases, each deployed independently.

**Implementation detail:**

Example: Rename column `user_name` to `display_name`:

```
Phase 1: EXPAND (Deploy v1.2.4, keep Blue v1.2.3 running)
  Migration: ALTER TABLE users ADD COLUMN display_name VARCHAR(255);
  Application code (v1.2.4): writes to BOTH user_name AND display_name
  Application code (v1.2.3, Blue): reads/writes user_name only (unaffected)

Phase 2: BACKFILL (Background job)
  UPDATE users SET display_name = user_name WHERE display_name IS NULL;
  -- Runs in batches of 1000, throttled, during off-peak

Phase 3: MIGRATE READS (Deploy v1.2.5)
  Application code: reads from display_name, writes to both
  Blue-green switch: v1.2.5 becomes live

Phase 4: CONTRACT (Deploy v1.2.6)
  Application code: reads/writes only display_name
  Migration: ALTER TABLE users DROP COLUMN user_name;
  -- Only after v1.2.5 is confirmed stable (no rollback to v1.2.4)
```

**Migration safety in CI/CD pipeline:**

```python
# Migration validator (runs in CI pipeline)
def validate_migration(migration_sql: str, current_version: str, new_version: str) -> list:
    """
    Checks that a migration is backward-compatible with the current live version.
    """
    violations = []
    
    # Parse SQL statements
    statements = parse_sql(migration_sql)
    
    for stmt in statements:
        # DROP COLUMN is not backward-compatible
        if stmt.type == 'ALTER' and 'DROP COLUMN' in stmt.text.upper():
            violations.append(
                f"DROP COLUMN detected: {stmt.text}. "
                f"Current version {current_version} may still read this column. "
                f"Use expand-contract pattern instead."
            )
        
        # NOT NULL without DEFAULT is not backward-compatible
        if stmt.type == 'ALTER' and 'NOT NULL' in stmt.text.upper() and 'DEFAULT' not in stmt.text.upper():
            violations.append(
                f"NOT NULL without DEFAULT: {stmt.text}. "
                f"Current version {current_version} may insert rows without this column."
            )
        
        # RENAME COLUMN is not backward-compatible
        if stmt.type == 'ALTER' and 'RENAME COLUMN' in stmt.text.upper():
            violations.append(
                f"RENAME COLUMN detected: {stmt.text}. "
                f"Use expand-contract: add new column, dual-write, then drop old."
            )
        
        # DROP TABLE
        if stmt.type == 'DROP':
            violations.append(f"DROP TABLE detected: {stmt.text}. Not backward-compatible.")
    
    return violations
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Migration breaks Blue code | Blue starts throwing errors on existing traffic | CI validator catches backward-incompatible changes; migration runs in staging first |
| Backfill takes too long (large table) | Blue and Green have inconsistent data for the new column | Throttle backfill; use `pt-online-schema-change` for large tables; backfill is idempotent |
| Rollback after contract phase | Can't rollback to v1.2.4 because old column is dropped | Never drop old column until new version is stable for > 48 hours; contract is a separate deploy |
| Concurrent migrations | Deadlocks or inconsistent schema | Advisory lock in migration tool (Flyway/Liquibase); only one migration runs at a time |

**Interviewer Q&As:**

**Q1: What if a migration takes 2 hours to run (adding an index on a 500M-row table)?**
A: Use `pt-online-schema-change` (Percona) or `gh-ost` (GitHub) for MySQL. These tools create a shadow table, copy data in chunks, and atomically swap table names. The migration runs in the background without locking the original table. For PostgreSQL, `CREATE INDEX CONCURRENTLY` avoids locking. The pipeline runs the migration as a background job and proceeds to deploy once the migration is confirmed complete.

**Q2: How do you handle a situation where Blue and Green have different ORM schemas?**
A: The ORM (Hibernate/JPA) in Green has the new column mapped; Blue's ORM doesn't know about it. This is fine if the new column is nullable (Blue ignores it on reads, doesn't write it). Green writes to the new column. The key rule: never make a column `NOT NULL` in the same deploy as creating it, because Blue can't populate it.

**Q3: How do you handle database connection pooling when both Blue and Green are active?**
A: Both environments share the same connection pool (via a connection proxy like ProxySQL or PgBouncer). Total connections = Blue pods + Green pods. During the deploy window, we temporarily increase the max connections on the database (or reduce per-pod pool size). After Blue is scaled down, connections drop to normal.

**Q4: What about NoSQL databases (Elasticsearch, DynamoDB)?**
A: Same expand-contract principle. For Elasticsearch, index mappings are additive (you can add fields but not remove or change type). For DynamoDB, new attributes are simply ignored by old code. The contract phase involves a reindex (Elasticsearch) or a backfill (DynamoDB) to clean up old attributes.

**Q5: How do you test that the migration is backward-compatible?**
A: CI pipeline runs the migration against a staging database, then runs both v1.2.3 and v1.2.4 test suites against the migrated schema. If v1.2.3 tests fail on the new schema, the migration is not backward-compatible. This is automated as a "backward compatibility test" stage in the pipeline.

**Q6: What happens if you need to rollback past a migration?**
A: If we rolled Green back to Blue, the expand-phase migration is still applied (the new column exists). Blue ignores it. This is fine. If we need to roll back the migration itself (rare), we run a compensating migration that undoes the expand. This is why we separate expand from contract -- the expand is always rollback-safe.

---

### Deep Dive 3: Smoke Testing and Health Verification

**Why it's hard:**
Smoke tests must validate that the new Green environment works correctly before it receives production traffic. But testing in a production-like environment (not staging) with real dependencies (real database, real caches) requires careful isolation to avoid polluting production data.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Health endpoint check only** | Simple, fast | Misses functional regressions |
| **Synthetic transactions (read-only)** | Tests real dependencies, no side effects | Doesn't test writes |
| **Synthetic transactions with test data** | Full functional coverage | Risk of polluting production data |
| **Shadow traffic (replay production requests)** | Tests with real patterns | Complex; responses may differ (non-deterministic) |
| **Canary with real traffic at low %** | Real-world validation | Some users see bugs; must combine with monitoring |

**Selected approach: Layered verification (health + synthetic reads + optional canary)**

**Implementation detail:**

```python
# Smoke test suite
class BlueGreenSmokeTest:
    def __init__(self, green_endpoint: str, timeout_seconds: int = 120):
        self.green = green_endpoint
        self.timeout = timeout_seconds
    
    def run_all(self) -> dict:
        results = {}
        
        # Layer 1: Health checks
        results['health_ready'] = self._check_health('/health/ready')
        results['health_live'] = self._check_health('/health/live')
        
        # Layer 2: Dependency checks
        results['db_connectivity'] = self._check_health('/health/db')
        results['cache_connectivity'] = self._check_health('/health/cache')
        results['downstream_svc'] = self._check_health('/health/dependencies')
        
        # Layer 3: Functional smoke tests (read-only)
        results['api_list_orders'] = self._test_api('GET', '/api/v1/orders?limit=1', expected_status=200)
        results['api_get_config'] = self._test_api('GET', '/api/v1/config', expected_status=200)
        
        # Layer 4: Synthetic write test (with test data flag)
        results['api_create_test_order'] = self._test_api(
            'POST', '/api/v1/orders',
            body={"item": "smoke-test-item", "test": True},  # test flag prevents real processing
            expected_status=201
        )
        
        # Layer 5: Performance baseline
        results['latency_check'] = self._check_latency('/api/v1/orders?limit=10', max_p95_ms=200)
        
        return results
    
    def _check_health(self, path: str) -> dict:
        try:
            resp = requests.get(f"{self.green}{path}", timeout=5)
            return {'status': 'passed' if resp.status_code == 200 else 'failed',
                    'response_code': resp.status_code, 'latency_ms': resp.elapsed.total_seconds() * 1000}
        except Exception as e:
            return {'status': 'failed', 'error': str(e)}
    
    def _check_latency(self, path: str, max_p95_ms: float, iterations: int = 50) -> dict:
        latencies = []
        for _ in range(iterations):
            resp = requests.get(f"{self.green}{path}", timeout=5)
            latencies.append(resp.elapsed.total_seconds() * 1000)
        
        p95 = sorted(latencies)[int(0.95 * len(latencies))]
        return {'status': 'passed' if p95 <= max_p95_ms else 'failed',
                'p95_ms': p95, 'threshold_ms': max_p95_ms}
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Smoke test false positive (tests pass, but real traffic fails) | Bad version serves production | Add canary step: route 5% real traffic to Green for 5 min before full switch |
| Smoke test false negative (tests fail due to test infra, not Green) | Deployment blocked unnecessarily | Retry smoke tests once. If still failing, check test infra health. |
| Smoke test pollutes production data | Test orders in production database | Use `test: true` flag in test data, cleaned up by a scheduled job. Or use a dedicated test namespace in the DB. |
| Smoke test timeout (Green is slow to start) | Deployment delayed | Increase readiness probe timeout; pre-warm JVM (CDS/AOT for Spring Boot). |

**Interviewer Q&As:**

**Q1: How do you test the Green environment without exposing it to production traffic?**
A: Green pods have a separate internal Service (e.g., `order-svc-green-internal`) that's accessible only from the CI/CD pipeline's test pods and not from the production ingress. Smoke tests hit this internal endpoint. Network policies ensure no external traffic reaches Green until the switch.

**Q2: How long should smoke tests run?**
A: Target < 2 minutes. Smoke tests are not a full regression suite; they verify critical paths (health, connectivity, basic API calls). Full regression tests ran earlier in the CI pipeline. Smoke tests are the final gate to catch environment-specific issues (misconfigured secrets, bad database connection, missing config).

**Q3: What if the smoke test suite itself has a bug?**
A: Smoke tests are versioned alongside the application code (in the same repo). They go through code review. We also run the smoke tests against the existing Blue environment periodically (cron) to ensure they pass against the known-good version. If smoke tests start failing against Blue, it's a test bug, not a deployment issue.

**Q4: How do you handle smoke tests for a service with no read-only endpoints?**
A: For write-heavy services (e.g., a log ingestion service), we use a dedicated test topic or database table. The smoke test writes a known test event, verifies it was processed, then cleans up. The `test: true` flag in the event header tells the service to route it to the test path.

**Q5: What if Green passes smoke tests but fails under production load?**
A: This is the gap between smoke tests and real traffic. To close it, we add a canary step: after smoke tests pass, route 5% of production traffic to Green for 5 minutes. Monitor error rate and latency. If they're within thresholds, proceed to full switch. This catches load-dependent issues that smoke tests miss.

**Q6: How do you measure smoke test effectiveness?**
A: Track two metrics: (1) "smoke test catch rate" -- deployments where smoke tests failed and the version was later confirmed buggy. (2) "smoke test miss rate" -- deployments where smoke tests passed but a rollback was needed. If miss rate > 5%, the smoke test suite needs more coverage.

---

## 7. Scheduling & Resource Management

### Environment Lifecycle

```
                Idle                     Provisioning
                 +                           +
                 |  Deploy triggered          |  Pods scheduled,
                 |  Controller creates        |  pulling images
                 |  Green Deployment          |
                 v                           v
            +---------+              +----------+
            |  Idle   +---deploy---->| Provision |
            |         |              |  -ing     |
            +---------+              +----+------+
                 ^                        |
                 |                   All pods ready
                 |                        |
            +----+-----+            +-----v----+
            |Terminated|<--cleanup--|  Ready   |
            |          |            |          |
            +----------+            +----+-----+
                 ^                       |
                 |                  Traffic switched
                 |                       |
            +----+-----+           +-----v----+
            | Draining |<--switch--| Live     |
            |          |           |          |
            +----------+           +----------+
```

### Resource Allocation

| Phase | Resource Usage | Notes |
|-------|---------------|-------|
| Idle | 0 pods (Green scaled to 0) | No cost |
| Provisioning | Green: N pods starting | Cluster autoscaler provisions new nodes if needed |
| Ready + Testing | Blue: N pods (live) + Green: N pods (idle) | 2x cost during this window (~5-15 min) |
| Live + Draining | Green: N pods (live) + Blue: N pods (draining) | 2x cost during drain window (~5 min) |
| Post-cleanup | Green: N pods (live) + Blue: 0 pods | 1x cost (normal) |
| With hot standby | Green: N pods (live) + Blue: N pods (idle standby) | 2x cost permanently (for instant rollback) |

### Cost Optimization

- **Scale-to-zero idle environment:** After successful switch and bake period, scale the old environment to zero. Rollback takes longer (need to re-scale) but saves 50% cost.
- **Smaller standby:** Keep old environment at 20% replica count (2 of 10 pods). Enough for instant rollback at reduced capacity; autoscaler brings up remaining 8 pods in ~90s.
- **Spot instances for Green during testing:** Since Green is not serving production traffic during testing, run it on cheaper spot instances. Switch to on-demand before traffic switch.

---

## 8. Scaling Strategy

### Scaling Blue-Green for Large Fleets

| Challenge | Solution |
|-----------|----------|
| Provisioning 100+ pods for Green | Pre-pull images on nodes (DaemonSet image warmer). Use `podTopologySpreadConstraints` for even distribution. |
| Green provisioning timeout (large service) | Increase timeout proportionally. Monitor per-pod scheduling latency. |
| Multiple services deploying simultaneously | Queue deployments per-service. Allow parallelism across services (different Deployments, independent). |
| Bare-metal fleet with 500 hosts | Blue-green at the load balancer level. Green = second set of processes on same hosts (different ports) or second host group. |

### Interviewer Q&As

**Q1: How do you blue-green deploy a service with 500 instances across 3 regions?**
A: Sequential region deployment: region 1 Green comes up, smoke test, switch. If healthy for 30 min, proceed to region 2. Each region is independent. If region 1 fails, rollback region 1 without affecting other regions. The Controller manages per-region state with a `region` dimension in the `environments` table.

**Q2: How does blue-green work for bare-metal services with 200 hosts?**
A: Option A: Two host groups (100 hosts each). Blue runs on group A, Green on group B. LB switches traffic between groups. Downside: need 2x hosts permanently. Option B: On each host, run Blue on port 8080 and Green on port 8081. HAProxy routes to the active port. Switch = change HAProxy backend port. This avoids 2x hosts but requires port management.

**Q3: How do you handle database connection limits with 2x instances?**
A: During the deploy window, total connections = Blue + Green. If the database has 500 max connections and each pod uses 10, Blue (50 pods) uses 500. Green (50 pods) would need another 500, exceeding limits. Solution: use a connection proxy (ProxySQL, PgBouncer) that multiplexes connections. Or temporarily reduce per-pod pool size during deploys.

**Q4: How do you avoid thundering herd when Green pods all start simultaneously?**
A: Use `maxSurge` / `maxUnavailable` equivalent in the Green Deployment's rollout strategy (though Blue-Green creates a new Deployment). To avoid thundering herd on startup (all pods hitting the database at once), use random jitter on startup delays and implement connection backoff.

**Q5: What happens when you have 50 services all doing blue-green deploys at the same time?**
A: Each service's deployment is independent (separate Deployments, separate Services). The shared resources are: (1) Kubernetes API server (CRD creation rate). We batch API calls. (2) Node capacity. Cluster autoscaler handles this. (3) Database connections (see Q3). (4) Artifact registry bandwidth. Pre-pull images to nodes. We implement a deployment rate limiter (max 10 concurrent blue-green deploys) to prevent resource exhaustion.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Impact | Detection | Mitigation | Recovery Time |
|---|---------|--------|-----------|------------|---------------|
| 1 | Green pods fail to start (bad image, missing config) | Deployment stuck in `provisioning` | Pod status `CrashLoopBackOff`, readiness probe fails | Controller times out after 5 min, marks deployment `failed`, tears down Green | < 5 min |
| 2 | Smoke tests pass but Green crashes under real traffic | Production errors after switch | Error rate spike in Prometheus | Monitoring Checker triggers automatic rollback to Blue (< 5s) | < 30 s |
| 3 | Blue-Green Controller crashes during switch | Unknown state: which slot is live? | Controller pod restart | Controller reads authoritative state from k8s Service selector (or LB config) on startup and reconciles | < 15 s |
| 4 | Database migration fails | Green can't start properly | Migration job fails; deployment blocked | Controller doesn't proceed to smoke test; deployment marked `failed` | Manual intervention |
| 5 | Network partition between controller and k8s API | Can't switch traffic | k8s API timeout | Exponential backoff retry; traffic stays on Blue (safe) | Partition heals |
| 6 | Both Blue and Green crash simultaneously | Full outage | Both Deployments have 0 ready pods | Extremely unlikely (different versions, different failure modes). Mitigate: keep at least one environment always running. PagerDuty alert. | Minutes (manual) |
| 7 | Load balancer failure | No traffic reaches either environment | Health check from external monitoring (Datadog, Pingdom) | LB redundancy: active-passive LB pair. DNS failover to standby LB. | < 30 s (LB failover) |
| 8 | Rollback fails (Blue was already terminated) | Can't go back | Controller can't find Blue Deployment | Never terminate Blue until Green has been stable for bake period. Keep Blue as hot standby for 30 min minimum. | Re-deploy Blue (~3-5 min) |

### Reliability Guarantees

- **Atomic switch:** The Service selector update is a single etcd write. It either succeeds or fails; no partial state.
- **Rollback always available:** Blue is never terminated until the deployment is marked `completed` after the bake period. During `monitoring` phase, rollback is < 5s.
- **Idempotent operations:** The Controller can be safely restarted at any point. It reads current state from k8s (which environment has the Service selector) and resumes.
- **No split-brain:** Only one Service object exists per service. Its selector points to exactly one slot at a time.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold | Source |
|--------|------|-----------------|--------|
| `deployment_status` | Gauge per (service, slot) | `failed` state | Controller |
| `deployment_duration_seconds` | Histogram | p95 > 900 s (15 min) | Controller |
| `traffic_switch_duration_ms` | Histogram | p95 > 2000 ms | LB Manager |
| `smoke_test_pass_rate` | Gauge | < 95% (over 1 week) | Smoke Test Runner |
| `post_switch_error_rate` | Gauge | > 1% (5 min window) | Prometheus (application) |
| `post_switch_p99_latency_ms` | Gauge | > 2x pre-switch baseline | Prometheus (application) |
| `rollback_count` | Counter | > 3/week (systemic issue) | Controller |
| `green_provisioning_time_seconds` | Histogram | p95 > 300 s | Controller |
| `environment_idle_hours` | Gauge | > 24 h (cost waste) | Controller |
| `active_deployments_count` | Gauge | > 20 (capacity concern) | Controller |

### Dashboards
1. **Deployment Health:** Active deployments, success rate, duration distribution, rollback rate.
2. **Environment Status:** Per-service: which slot is live, version, replica count, health status.
3. **Switch Analysis:** Switch duration distribution, error rates before/after switch, rollback frequency.
4. **Cost:** Active Green environments, idle duration, estimated cost overhead.

### Alerts
- Deployment stuck in `deploying` or `testing` for > 15 min.
- Post-switch error rate > rollback threshold but automatic rollback not triggered (controller issue).
- Green environment idle for > 2 hours (stuck deployment, cost waste).

---

## 11. Security

| Control | Implementation |
|---------|---------------|
| RBAC | Only CI/CD pipeline service account can trigger deployments. Manual switch requires `deploy:production` role. Rollback is allowed for on-call engineers (`deploy:rollback` role). |
| Audit logging | Every deployment, switch, and rollback is recorded in `deployments` and `traffic_switches` tables with `triggered_by` field linked to identity provider. |
| Image verification | Green Deployment spec uses image digest (not tag). Controller verifies cosign signature before provisioning Green. |
| Network isolation | Green pods can access the database and internal services but are not reachable from production ingress until the switch. Network policy enforces this. |
| Secrets management | Green pods use the same Vault secrets as Blue (same service account). No secret divergence between environments. |
| Configuration integrity | Environment-specific config (Kustomize overlays) is in a Git repo with PR review requirement. No ad-hoc config changes. |

---

## 12. Incremental Rollout Strategy

### Rolling Out the Blue-Green System Itself

Phase 1: **Pilot (1 week):** Enable blue-green for 3 stateless, low-traffic services. Validate smoke tests, switch timing, rollback flow.

Phase 2: **Expand (2 weeks):** Enable for 20 services across 3 teams. Include one service with database migrations to validate expand-contract workflow.

Phase 3: **Default (ongoing):** Make blue-green the default deployment strategy for all new services. Existing services opt-in.

Phase 4: **Mandatory (after 2 months):** Blue-green required for all production deployments. Legacy rolling-update strategy deprecated.

### Rollout Q&As

**Q1: How do you roll out blue-green to a service that currently uses rolling updates?**
A: First deploy creates both Blue and Green Deployments from the current running version (same image). Verify the switch works (Blue -> Green -> Blue) with no version change. Then enable the full pipeline integration (new version deploys to Green, smoke test, switch). This separates infrastructure validation from application deployment.

**Q2: What's the rollout strategy for a database-backed service transitioning to blue-green?**
A: Start with read-only services (no migrations). Then add services with simple additive migrations (add column). Build confidence with the expand-contract pattern on low-risk tables. Finally, enable for services with complex schema changes. Document the expand-contract pattern in a runbook and train developers.

**Q3: How do you handle a service that can't do blue-green (stateful, singleton)?**
A: Some services (e.g., a leader-elected singleton like a job scheduler) can't have two instances running simultaneously. For these, use a maintenance window: stop Blue, start Green. Or use a "blue-green with leader election": Green starts but doesn't become leader until Blue is shut down. The leader election (via k8s Lease or etcd) ensures only one is active.

**Q4: How do you measure the success of the blue-green rollout?**
A: Key metrics: (1) Deployment success rate before/after (target: > 95% -> > 99%). (2) MTTR for failed deployments (target: < 5 min with instant rollback). (3) Deployment frequency (teams deploy more often when they trust the process). (4) Rollback count (should decrease over time as smoke tests improve).

**Q5: How do you train 3,000 engineers on blue-green deployments?**
A: (1) Internal documentation with diagrams and examples. (2) A `bgctl` CLI that abstracts the complexity. (3) Pipeline templates that include blue-green steps by default. (4) Office hours where the platform team helps with migration. (5) A Slack channel for questions. (6) Automated enforcement: pipelines without smoke tests are flagged.

---

## 13. Trade-offs & Decision Log

| Decision | Option Chosen | Alternative | Rationale |
|----------|---------------|-------------|-----------|
| Traffic switch mechanism (k8s) | Service selector swap | Istio VirtualService, DNS | Simplest, no extra dependencies. Istio adds complexity. DNS has TTL issues. |
| Traffic switch mechanism (bare-metal) | Envoy xDS | HAProxy reload, DNS | Envoy xDS is dynamic (no reload). HAProxy requires SIGHUP (brief connection drop). |
| Database compatibility | Expand-contract | Dual databases, feature flags | Industry standard; proven; doesn't require database duplication. |
| Smoke test approach | Layered (health + read + optional canary) | Full regression, shadow traffic | Smoke tests are fast (< 2 min). Full regression is too slow. Shadow traffic is complex. |
| Idle environment cost | Scale to 0 after bake period | Keep hot standby permanently | 50% cost saving. Rollback after bake period requires redeployment anyway (Blue version is stale). |
| Controller implementation | Custom k8s controller (Go) | Argo Rollouts blue-green strategy | Argo Rollouts is a viable alternative. Custom controller gives us bare-metal support and integration with our approval system. |
| Green provisioning | New k8s Deployment + HPA | Scale existing Deployment (rolling update hybrid) | Clean separation between Blue and Green. Easier to reason about. Rolling update hybrid loses "instant rollback" property. |

---

## 14. Agentic AI Integration

### AI-Powered Blue-Green Operations

| Use Case | Implementation |
|----------|---------------|
| **Smoke test generation** | AI agent analyzes service's API routes and generates smoke test cases automatically. Feeds on OpenAPI spec and historical traffic patterns. |
| **Anomaly detection post-switch** | AI agent monitors metrics after traffic switch and detects subtle anomalies that fixed thresholds miss (e.g., gradual latency increase, changed response body distribution). Uses isolation forest algorithm on Prometheus metrics. |
| **Deployment timing advisor** | Agent analyzes traffic patterns and recommends optimal deployment windows (low-traffic periods). Also warns if a deployment is triggered during peak hours. |
| **Database migration safety** | Agent reviews SQL migration files and predicts whether they're backward-compatible. Uses a trained classifier on historical migration successes/failures. Augments the rule-based validator with ML-based pattern detection. |
| **Rollback decision support** | After a switch, agent monitors 20+ metrics and provides a confidence score: "87% confidence Green is healthy" or "Recommend rollback: latency regression detected in /api/v1/orders (p99: 450ms vs 200ms baseline)." |
| **Cost optimizer** | Agent tracks idle environment duration across all services and recommends scale-down policies. "Service X keeps Green running for 2 hours post-switch on average. Recommend scaling to 0 after 15 minutes to save $340/month." |

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through a blue-green deployment from start to finish.**
A: (1) CI/CD pipeline builds and tests the new version. (2) Pipeline triggers the Blue-Green Controller with the new image digest. (3) Controller identifies the idle slot (Green). (4) Controller creates/updates the Green Deployment with the new image. (5) Green pods start, pull image, pass readiness probes. (6) Controller runs smoke tests against Green's internal endpoint. (7) If tests pass, Controller updates the Service selector to point to Green. (8) Traffic instantly shifts to Green. (9) Controller monitors error rates for 5 minutes. (10) If healthy, deployment is marked `completed`, and Blue is scaled down after 30 minutes. (11) If unhealthy, Controller flips back to Blue (rollback) in < 5 seconds.

**Q2: What's the biggest risk with blue-green deployments?**
A: Database compatibility. If the new version requires a schema change that's not backward-compatible, Blue will break when the migration runs. The expand-contract pattern mitigates this but adds deployment complexity (multiple phases). The second risk is cost: 2x resources during the deploy window, which at scale (500 services, 10,000 pods) is significant.

**Q3: Blue-green vs. canary -- when do you use each?**
A: Blue-green: when you need instant rollback and can afford 2x resources. Best for stateless services with clear health indicators. Canary: when you want to validate with a small percentage of real traffic before full rollout, and you can tolerate a gradual rollout. Canary is preferred for user-facing services where subtle bugs only manifest under diverse traffic. Best approach: canary before blue-green -- route 5% to Green, verify, then switch 100%.

**Q4: How do you handle a long-running background job during a blue-green switch?**
A: Background jobs should be idempotent and use a distributed lock (Redis or database advisory lock). During the switch, Blue's job instance will finish its current batch and not pick up new work (Blue is draining). Green's job instance starts picking up new work. If both try to process the same work, idempotency ensures no duplication. For strict single-execution, use a leader election lease that transfers when Blue scales down.

**Q5: How do you blue-green deploy a service that uses local file storage?**
A: Local file storage is a problem for blue-green because Green has a different filesystem. Solution: migrate to shared storage (S3, NFS, PVC with ReadWriteMany). If local storage is required, use a migration step: before the switch, copy files from Blue to Green. But this adds deployment time and complexity. Best practice: design services to be stateless.

**Q6: How do you handle session stickiness during a blue-green switch?**
A: Sessions should be externalized (Redis, database). Both Blue and Green read sessions from the shared store. After the switch, user requests go to Green, which reads the existing session from Redis. No session loss. If sessions are in-memory (bad), users will lose their session on switch. This is another reason to externalize state.

**Q7: How do you handle blue-green for gRPC services with long-lived connections?**
A: gRPC clients maintain persistent connections. When the Service selector changes, new connection requests go to Green, but existing connections to Blue remain. The gRPC client must implement reconnection logic (default in most gRPC libraries: `grpc.keepalive_time_ms`). The Blue Deployment's `preStop` hook sends gRPC GOAWAY frames to gracefully close connections and force clients to reconnect (to Green).

**Q8: How do you automate the decision to rollback?**
A: Post-switch monitoring compares Green's metrics against Blue's pre-switch baseline. Rollback triggers: (1) Error rate > 1% (absolute) or > 3x baseline. (2) p99 latency > 2x baseline. (3) Any pod in CrashLoopBackOff. (4) Health endpoint returns non-200. The Controller polls Prometheus every 10 seconds during the monitoring window (5 min). If any trigger fires, rollback is immediate.

**Q9: How do you handle configuration differences between Blue and Green?**
A: Configuration is managed via ConfigMaps/Secrets in Kubernetes, versioned in the GitOps repo. Blue and Green can have different ConfigMaps (e.g., `order-svc-blue-config`, `order-svc-green-config`). However, shared config (database URLs, external service endpoints) should be identical. The Controller validates that critical config keys match between Blue and Green before switching.

**Q10: What happens if a rollback is triggered but Blue was already scaled down?**
A: This is why we keep Blue as a hot standby for the bake period (30 min). If Blue was scaled down, we must re-create the Blue Deployment from the previous image digest (stored in the `environments` table). This takes 2-5 minutes (pod scheduling + image pull). To avoid this, the Controller doesn't scale down Blue until the bake period expires.

**Q11: How do you handle blue-green across multiple Kubernetes clusters?**
A: Each cluster has its own Blue-Green Controller. A global orchestrator triggers deployments cluster-by-cluster. Cluster 1: switch to Green, bake 30 min. Cluster 2: switch. Cluster 3: switch. If cluster 1 rollback triggers, the orchestrator pauses deployment to clusters 2 and 3. Global state is tracked in a central database.

**Q12: How do you handle DNS caching in Java applications?**
A: Java's `InetAddress` caches DNS indefinitely by default when a `SecurityManager` is installed. For blue-green with DNS switching (not recommended), you must set `networkaddress.cache.ttl=30` in `java.security`. For LB-based switching (recommended), DNS caching doesn't matter because the DNS name (service VIP) stays the same; only the backends behind the VIP change.

**Q13: How do you test the blue-green system itself?**
A: (1) Integration tests: deploy a test service, switch Blue -> Green -> Blue, verify zero errors. Run daily. (2) Chaos test: kill Green pods during switch, verify rollback works. (3) Load test: simulate 100K RPS during switch, verify zero dropped requests. (4) Failure injection: make smoke tests fail, verify deployment is aborted.

**Q14: How do you handle blue-green with Helm?**
A: Two Helm releases: `order-svc-blue` and `order-svc-green`. The `values.yaml` for each specifies the version and the Service selector. The switch is a Helm upgrade of the Service to point to the new release. ArgoCD can manage both releases and the Service as a single Application with multiple manifests.

**Q15: What's the maximum acceptable time for a blue-green deployment end-to-end?**
A: Target < 15 min for a standard service (20 pods). Breakdown: Green provisioning (3 min) + smoke tests (2 min) + switch (1 s) + monitoring (5 min) + buffer (5 min). For large services (100+ pods), provisioning may take 5-10 min. If exceeding 30 min, investigate bottlenecks (slow image pull, slow readiness probes, slow smoke tests).

**Q16: How do you communicate deployment status to stakeholders?**
A: Slack bot posts to the service's channel: (1) "Deployment started: order-svc v1.2.4" with a link to the dashboard. (2) "Smoke tests passed" (3) "Traffic switched to Green" (4) "Deployment complete: order-svc v1.2.4 is live" or "Rollback triggered: reverted to v1.2.3, reason: error rate exceeded 1%". PagerDuty for automatic rollbacks (on-call should investigate).

---

## 16. References

- Martin Fowler, "BlueGreenDeployment": https://martinfowler.com/bliki/BlueGreenDeployment.html
- Kubernetes Deployments: https://kubernetes.io/docs/concepts/workloads/controllers/deployment/
- Argo Rollouts Blue-Green: https://argoproj.github.io/argo-rollouts/features/bluegreen/
- Envoy xDS Protocol: https://www.envoyproxy.io/docs/envoy/latest/api-docs/xds_protocol
- GitHub gh-ost (Online Schema Changes): https://github.com/github/gh-ost
- Percona pt-online-schema-change: https://docs.percona.com/percona-toolkit/pt-online-schema-change.html
- ProxySQL: https://proxysql.com/documentation/
- Cosign Image Signing: https://docs.sigstore.dev/cosign/overview/
- Expand-Contract Pattern (Evolutionary Database Design): https://martinfowler.com/articles/evodb.html
