# System Design: Feature Flag Service

> **Relevance to role:** A cloud infrastructure platform engineer designs the platform primitives that all application teams depend on. A feature flag service is a foundational control-plane component: it gates feature rollouts, enables emergency kill switches, powers A/B tests, and decouples deployment from release. At infrastructure scale, this service must handle millions of evaluations per second with zero added latency (in-process SDK evaluation), provide real-time flag updates, and operate with extreme reliability since a flag service outage can block all rollouts or disable critical features.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|-------------|
| FR-1 | Create and manage feature flags with types: boolean, string, number, JSON. |
| FR-2 | Define targeting rules: user ID, user group, percentage rollout, environment, custom attributes. |
| FR-3 | In-process SDK evaluation: flag evaluation happens locally with zero network latency. |
| FR-4 | Real-time flag updates: SDK receives flag changes within 5 seconds. |
| FR-5 | Emergency kill switch: instantly disable a feature across all services. |
| FR-6 | Gradual percentage rollout: ramp from 0% to 100% with metric correlation. |
| FR-7 | Environment-aware: separate flag values for dev, staging, production. |
| FR-8 | Flag lifecycle management: track flag creation date, owner, linked Jira ticket, and provide cleanup reminders to prevent flag debt. |
| FR-9 | Audit log: every flag change recorded with who, what, when. |
| FR-10 | SDKs for Java, Python, Go, JavaScript (server-side and client-side). |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Flag evaluation latency (in-process) | < 1 ms |
| NFR-2 | Flag update propagation time | < 5 s (real-time push) |
| NFR-3 | Service availability | 99.99% (52 min downtime/year) |
| NFR-4 | Evaluations per second (platform-wide) | 10M+ (in-process, no server load) |
| NFR-5 | Number of flags | 10,000+ |
| NFR-6 | SDK initialization time | < 2 s |
| NFR-7 | Graceful degradation when service is unavailable | Return cached defaults |

### Constraints & Assumptions
- 3,000 engineers, 1,500 microservices.
- Java (Spring Boot) and Python are the primary languages.
- Flags must be evaluable without network calls (SDK stores all rules locally).
- Flag updates propagated via Server-Sent Events (SSE) or long-poll.
- Multi-environment: each environment has separate flag states.

### Out of Scope
- Client-side (browser/mobile) feature flags (different security model: rules must not leak targeting logic).
- Full A/B testing platform (experiment lifecycle, statistical analysis).
- Feature flag as configuration management (flags for tuning parameters like timeouts, batch sizes).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Services using flags | 1,500 total, 80% adoption | 1,200 services |
| Service instances (pods/VMs) | 1,200 x avg 10 instances | 12,000 |
| Flag evaluations per instance per second | Avg 100 (hot paths check flags frequently) | 100/s |
| Total evaluations/second (platform-wide) | 12,000 x 100 | 1.2M/s |
| Peak evaluations/second | 3x average | 3.6M/s |
| Flag definitions | 10,000 across all services | 10,000 |
| Flag changes per day | 50 changes (create/update/toggle) | 50/day |
| SSE connections (one per instance) | 12,000 instances | 12,000 concurrent |

**Note:** Evaluations are in-process (SDK). The server only serves flag definitions and updates. Server load is from SSE connections and flag CRUD, not evaluations.

### Latency Requirements

| Operation | Target |
|-----------|--------|
| Flag evaluation (in-process) | < 1 ms (typically < 0.1 ms) |
| Flag update propagation (SSE) | < 5 s |
| SDK initialization (first load) | < 2 s |
| Flag CRUD API response | < 200 ms |
| Dashboard page load | < 1 s |

### Storage Estimates

| Item | Calculation | Value |
|------|-------------|-------|
| Flag definitions | 10,000 flags x 5 KB avg (rules, targeting) | 50 MB |
| Audit log events | 50 changes/day x 365 x 2 KB | ~36 MB/year |
| Evaluation analytics | 1.2M/s x 100 bytes x 1% sampling | ~1 GB/day |
| This is a metadata-heavy, data-light system. | | |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| SSE flag updates | 50 changes/day x 5 KB x 12,000 connections | 3 GB/day (burst, ~negligible sustained) |
| SDK initialization payload | 10,000 flags x 5 KB = 50 MB (filtered per-service: ~500 KB) | 12,000 instances x 500 KB = 6 GB/day (at deploy time) |
| CRUD API traffic | 50 requests/day x 10 KB | ~500 KB/day |

---

## 3. High Level Architecture

```
                      +----------------------+
                      |   Flag Management    |
                      |   Dashboard (React)  |
                      +----------+-----------+
                                 |
                            REST API
                                 |
                      +----------v-----------+
                      |   Flag Management    |
                      |   Service (Java)     |
                      |   - CRUD APIs        |
                      |   - Targeting rules  |
                      |   - Audit logging    |
                      +--+------+-------+----+
                         |      |       |
              +----------+  +---v---+   +----------+
              |             | MySQL |              |
              v             +-------+              v
     +--------+--------+                  +--------+--------+
     |  SSE/Streaming   |                  |  Audit Log      |
     |  Gateway          |                  |  (Kafka ->     |
     |  (pushes updates  |                  |   Elasticsearch)|
     |   to SDKs)        |                  +-----------------+
     +--------+----------+
              |
              | SSE connection per instance
              |
   +----------v-----------+
   |                       |
   |  +---+  +---+  +---+ |
   |  |SDK|  |SDK|  |SDK| |     Service instances (12,000)
   |  |   |  |   |  |   | |
   |  +---+  +---+  +---+ |
   |                       |
   |  In-process flag      |
   |  evaluation cache     |
   +-----------------------+
              |
              | Application code:
              |   if (flagService.isEnabled("new_checkout", user)) {
              |       // new code path
              |   }
              |
```

### Component Roles

| Component | Role |
|-----------|------|
| **Flag Management Dashboard** | React web UI for creating, editing, toggling flags. Shows flag status, targeting rules, audit history, lifecycle metrics. |
| **Flag Management Service** | Java Spring Boot service. CRUD for flags and targeting rules. Validates rules. Publishes change events. Serves initial flag payload to SDKs. |
| **MySQL** | Stores flag definitions, targeting rules, environment configs. Source of truth. |
| **SSE/Streaming Gateway** | Maintains SSE connections with all SDK instances. When a flag changes, pushes the update to all connected clients. Backed by Redis Pub/Sub for horizontal scaling. |
| **SDK (in-process)** | Library embedded in each service. Maintains a local cache of all relevant flags. Evaluates targeting rules locally with zero network latency. Connects to SSE gateway for real-time updates. |
| **Audit Log** | Flag changes published to Kafka, consumed into Elasticsearch for searchable history. Immutable. |

### Data Flows

1. **Flag Creation:** Admin creates flag in Dashboard -> Dashboard calls REST API -> Service validates and writes to MySQL -> Service publishes event to Redis Pub/Sub -> SSE Gateway pushes update to all connected SDKs.
2. **SDK Initialization:** Service starts -> SDK calls `/api/v1/flags?service={name}&env={env}` -> Receives filtered flag payload -> Stores in local cache -> Opens SSE connection for updates.
3. **Flag Evaluation:** Application code calls `sdk.isEnabled("new_checkout", user)` -> SDK evaluates targeting rules against local cache -> Returns boolean in < 1 ms -> No network call.
4. **Flag Update:** Admin toggles flag -> MySQL updated -> Redis Pub/Sub event -> SSE Gateway pushes to all SDKs -> SDKs update local cache -> Next evaluation uses new value. End-to-end < 5 seconds.
5. **Kill Switch:** Admin enables kill switch flag -> Same flow as flag update, but the flag has `priority: emergency` which bypasses any batching/debouncing -> Propagation < 2 seconds.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Feature flag definitions
CREATE TABLE feature_flags (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    flag_key        VARCHAR(255) NOT NULL,              -- e.g., "new_checkout_flow"
    name            VARCHAR(255) NOT NULL,              -- human-readable name
    description     TEXT,
    flag_type       ENUM('boolean', 'string', 'number', 'json') NOT NULL DEFAULT 'boolean',
    owner_team      VARCHAR(255) NOT NULL,              -- e.g., "checkout-team"
    owner_email     VARCHAR(255) NOT NULL,
    jira_ticket     VARCHAR(50),                        -- e.g., "PROJ-1234"
    tags            JSON,                               -- ["backend", "checkout", "experiment"]
    lifecycle_status ENUM('active', 'stale', 'permanent', 'deprecated') NOT NULL DEFAULT 'active',
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    stale_after     DATETIME,                           -- flag cleanup reminder date
    UNIQUE KEY uk_flag_key (flag_key),
    INDEX idx_owner (owner_team),
    INDEX idx_lifecycle (lifecycle_status),
    INDEX idx_stale (stale_after)
) ENGINE=InnoDB;

-- Per-environment flag configuration
CREATE TABLE flag_environments (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    flag_id         BIGINT NOT NULL,
    environment     ENUM('dev', 'staging', 'production') NOT NULL,
    enabled         BOOLEAN NOT NULL DEFAULT FALSE,     -- master toggle
    default_value   TEXT NOT NULL,                       -- value when no targeting rule matches
    version         INT NOT NULL DEFAULT 1,             -- optimistic locking for concurrent updates
    updated_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    updated_by      VARCHAR(255) NOT NULL,
    FOREIGN KEY (flag_id) REFERENCES feature_flags(id) ON DELETE CASCADE,
    UNIQUE KEY uk_flag_env (flag_id, environment)
) ENGINE=InnoDB;

-- Targeting rules (ordered, first-match wins)
CREATE TABLE targeting_rules (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    flag_env_id     BIGINT NOT NULL,
    rule_order      INT NOT NULL,                       -- evaluation order (1 = highest priority)
    rule_name       VARCHAR(255) NOT NULL,              -- e.g., "Internal users"
    conditions      JSON NOT NULL,                      -- targeting conditions (see below)
    serve_value     TEXT NOT NULL,                       -- value to serve when rule matches
    enabled         BOOLEAN NOT NULL DEFAULT TRUE,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (flag_env_id) REFERENCES flag_environments(id) ON DELETE CASCADE,
    INDEX idx_flag_env_order (flag_env_id, rule_order)
) ENGINE=InnoDB;

/*
Example conditions JSON:
{
  "operator": "AND",
  "clauses": [
    {
      "attribute": "user_id",
      "op": "in",
      "values": ["user-123", "user-456"]
    },
    {
      "attribute": "country",
      "op": "eq",
      "values": ["US"]
    }
  ]
}

Percentage rollout rule:
{
  "operator": "AND",
  "clauses": [
    {
      "attribute": "percentage",
      "op": "rollout",
      "values": ["25"],           // 25% of users
      "rollout_key": "user_id"    // hash on user_id for consistency
    }
  ]
}
*/

-- Audit log (flag changes)
CREATE TABLE flag_audit_log (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    flag_id         BIGINT NOT NULL,
    environment     VARCHAR(20) NOT NULL,
    action          ENUM('created', 'updated', 'toggled', 'rule_added', 'rule_removed',
                         'rule_updated', 'deleted', 'archived') NOT NULL,
    changed_by      VARCHAR(255) NOT NULL,
    previous_value  JSON,                               -- snapshot before change
    new_value       JSON,                               -- snapshot after change
    change_comment  TEXT,
    created_at      DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_flag_time (flag_id, created_at),
    INDEX idx_time (created_at)
) ENGINE=InnoDB;

-- Flag evaluation analytics (sampled)
CREATE TABLE flag_evaluations (
    id              BIGINT AUTO_INCREMENT PRIMARY KEY,
    flag_key        VARCHAR(255) NOT NULL,
    environment     VARCHAR(20) NOT NULL,
    service_name    VARCHAR(255) NOT NULL,
    evaluated_value TEXT NOT NULL,
    context_hash    CHAR(32) NOT NULL,                  -- MD5 of evaluation context
    timestamp       DATETIME NOT NULL,
    INDEX idx_flag_time (flag_key, timestamp),
    INDEX idx_service (service_name)
) ENGINE=InnoDB
PARTITION BY RANGE (TO_DAYS(timestamp)) (
    PARTITION p_current VALUES LESS THAN (TO_DAYS(CURRENT_DATE + INTERVAL 1 DAY)),
    PARTITION p_future VALUES LESS THAN MAXVALUE
);
```

### Database Selection

| Store | Engine | Rationale |
|-------|--------|-----------|
| Flag definitions + rules | MySQL 8.0 | Transactional consistency; low write volume (~50/day); reliable ACID semantics for flag toggles. |
| SSE event distribution | Redis Pub/Sub | Low-latency pub/sub for broadcasting flag changes to SSE gateway instances. Ephemeral (not stored). |
| Audit log (long-term) | Elasticsearch | Searchable audit history. "Show me all flag changes by alice in the last 30 days." |
| Evaluation analytics | ClickHouse | Columnar store for high-volume analytics. "How many times was flag X evaluated to true/false last week?" |
| SDK local cache | In-memory (ConcurrentHashMap) | Zero-latency evaluation. Thread-safe. Refreshed via SSE. |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| feature_flags | (flag_key) UNIQUE | Flag lookup by key (SDK initialization) |
| feature_flags | (owner_team) | "Show all flags owned by team X" |
| feature_flags | (stale_after) | Flag cleanup: find flags past their stale date |
| flag_environments | (flag_id, environment) UNIQUE | Environment-specific config lookup |
| targeting_rules | (flag_env_id, rule_order) | Ordered rule evaluation |
| flag_audit_log | (flag_id, created_at) | Audit history per flag |

---

## 5. API Design

### REST Endpoints

```
# Flag CRUD
POST   /api/v1/flags                                   # Create flag
GET    /api/v1/flags                                    # List flags (filterable by team, status, tag)
GET    /api/v1/flags/{key}                              # Get flag details
PUT    /api/v1/flags/{key}                              # Update flag metadata
DELETE /api/v1/flags/{key}                              # Archive flag (soft delete)

# Environment configuration
GET    /api/v1/flags/{key}/environments/{env}           # Get flag config for environment
PUT    /api/v1/flags/{key}/environments/{env}/toggle     # Enable/disable flag
PUT    /api/v1/flags/{key}/environments/{env}/default    # Set default value

# Targeting rules
POST   /api/v1/flags/{key}/environments/{env}/rules      # Add targeting rule
PUT    /api/v1/flags/{key}/environments/{env}/rules/{id}  # Update rule
DELETE /api/v1/flags/{key}/environments/{env}/rules/{id}  # Delete rule
PUT    /api/v1/flags/{key}/environments/{env}/rules/reorder  # Reorder rules

# SDK endpoints
GET    /api/v1/sdk/flags?service={name}&env={env}        # Get all flags for a service (SDK init)
GET    /api/v1/sdk/stream?service={name}&env={env}        # SSE stream for flag updates

# Audit
GET    /api/v1/flags/{key}/audit                          # Audit history for a flag
GET    /api/v1/audit?changed_by={user}&since={date}        # Audit by user/date

# Analytics
GET    /api/v1/flags/{key}/analytics                       # Evaluation counts by value
GET    /api/v1/flags/stale                                  # Flags past their stale date

# Kill switch
POST   /api/v1/flags/{key}/kill                             # Emergency disable (fast path)
```

**Example: Create a flag**
```json
POST /api/v1/flags
{
  "flag_key": "new_checkout_flow",
  "name": "New Checkout Flow",
  "description": "Redesigned checkout with one-click purchase",
  "flag_type": "boolean",
  "owner_team": "checkout-team",
  "owner_email": "checkout-team@company.com",
  "jira_ticket": "CHECKOUT-567",
  "tags": ["checkout", "experiment"],
  "stale_after": "2026-07-01T00:00:00Z"
}
```

**Example: Add a percentage rollout rule**
```json
POST /api/v1/flags/new_checkout_flow/environments/production/rules
{
  "rule_name": "Gradual rollout - 25%",
  "rule_order": 2,
  "conditions": {
    "operator": "AND",
    "clauses": [
      {
        "attribute": "percentage",
        "op": "rollout",
        "values": ["25"],
        "rollout_key": "user_id"
      }
    ]
  },
  "serve_value": "true"
}
```

### CLI

```bash
# Flag management
flagctl create --key new_checkout_flow --type boolean --team checkout-team --ticket CHECKOUT-567
flagctl list --team checkout-team --status active
flagctl get new_checkout_flow
flagctl toggle new_checkout_flow --env production --on
flagctl toggle new_checkout_flow --env production --off

# Targeting rules
flagctl rule add new_checkout_flow --env production \
    --name "Internal users" --order 1 \
    --condition 'user_group in ["employees"]' \
    --value true

flagctl rule add new_checkout_flow --env production \
    --name "25% rollout" --order 2 \
    --condition 'percentage rollout 25 on user_id' \
    --value true

# Kill switch
flagctl kill new_checkout_flow --env production --reason "High error rate detected"

# Analytics
flagctl analytics new_checkout_flow --env production --window 7d

# Flag cleanup
flagctl stale --list                     # Show stale flags
flagctl archive new_checkout_flow        # Archive a cleaned-up flag

# Evaluate (testing)
flagctl evaluate new_checkout_flow --env production \
    --context '{"user_id": "user-123", "country": "US"}'
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: In-Process SDK Evaluation Engine

**Why it's hard:**
The SDK must evaluate flags in < 1 ms, be thread-safe (thousands of concurrent evaluations), maintain a synchronized local cache of flag rules, and handle updates without blocking evaluations. A remote API call per evaluation is not acceptable at 1.2M evaluations/second.

**Approaches:**

| Approach | Evaluation Latency | Availability | Complexity |
|----------|-------------------|--------------|------------|
| **Remote API call per evaluation** | 5-50 ms (network) | Depends on server | Low |
| **SDK with local cache + polling** | < 1 ms (in-process) | Always available (cached) | Medium |
| **SDK with local cache + SSE push** | < 1 ms (in-process) | Always available (cached) | Medium-High |
| **SDK with compiled rules (code generation)** | < 0.01 ms | Always available | Very High |
| **Sidecar proxy (evaluate in sidecar)** | 1-5 ms (localhost) | Depends on sidecar | Medium |

**Selected approach: SDK with local cache + SSE push updates**

**Justification:** In-process evaluation eliminates network latency entirely. SSE push ensures flag updates propagate in < 5 seconds (vs. polling's 30-60s interval). The local cache makes the SDK resilient to flag service outages. This is the architecture used by LaunchDarkly, Split.io, and Unleash.

**Implementation detail - Java SDK:**

```java
/**
 * Feature Flag SDK for Java services.
 * Thread-safe, zero-latency evaluation, real-time updates via SSE.
 */
public class FeatureFlagClient implements AutoCloseable {
    
    private final ConcurrentHashMap<String, FlagDefinition> flagCache = new ConcurrentHashMap<>();
    private final String serviceName;
    private final String environment;
    private final String serverUrl;
    private final ExecutorService sseExecutor;
    private volatile boolean initialized = false;
    private final CountDownLatch initLatch = new CountDownLatch(1);
    
    public FeatureFlagClient(String serverUrl, String serviceName, String environment) {
        this.serverUrl = serverUrl;
        this.serviceName = serviceName;
        this.environment = environment;
        this.sseExecutor = Executors.newSingleThreadExecutor(
            r -> new Thread(r, "feature-flag-sse")
        );
        initialize();
    }
    
    private void initialize() {
        // Load initial flag state
        try {
            String url = String.format("%s/api/v1/sdk/flags?service=%s&env=%s",
                serverUrl, serviceName, environment);
            HttpResponse<String> response = HttpClient.newHttpClient()
                .send(HttpRequest.newBuilder(URI.create(url)).build(),
                      HttpResponse.BodyHandlers.ofString());
            
            List<FlagDefinition> flags = parseFlags(response.body());
            for (FlagDefinition flag : flags) {
                flagCache.put(flag.getKey(), flag);
            }
            initialized = true;
            initLatch.countDown();
            
            // Start SSE listener for real-time updates
            startSSEListener();
            
        } catch (Exception e) {
            log.warn("Failed to initialize flags from server, using defaults", e);
            initLatch.countDown(); // unblock callers, they'll get defaults
        }
    }
    
    /**
     * Evaluate a boolean flag. Zero-latency (in-process).
     */
    public boolean isEnabled(String flagKey, EvaluationContext context) {
        FlagDefinition flag = flagCache.get(flagKey);
        if (flag == null) {
            log.debug("Flag '{}' not found, returning false", flagKey);
            return false;
        }
        
        if (!flag.isEnabled()) {
            return false;
        }
        
        // Evaluate targeting rules in order (first match wins)
        for (TargetingRule rule : flag.getRules()) {
            if (!rule.isEnabled()) continue;
            if (evaluateConditions(rule.getConditions(), context)) {
                return Boolean.parseBoolean(rule.getServeValue());
            }
        }
        
        // No rule matched, return default
        return Boolean.parseBoolean(flag.getDefaultValue());
    }
    
    /**
     * Evaluate a string flag (for multivariate flags).
     */
    public String getStringValue(String flagKey, EvaluationContext context, String defaultValue) {
        FlagDefinition flag = flagCache.get(flagKey);
        if (flag == null || !flag.isEnabled()) {
            return defaultValue;
        }
        
        for (TargetingRule rule : flag.getRules()) {
            if (!rule.isEnabled()) continue;
            if (evaluateConditions(rule.getConditions(), context)) {
                return rule.getServeValue();
            }
        }
        
        return flag.getDefaultValue();
    }
    
    private boolean evaluateConditions(ConditionGroup conditions, EvaluationContext context) {
        boolean isAnd = "AND".equals(conditions.getOperator());
        
        for (Clause clause : conditions.getClauses()) {
            boolean clauseResult = evaluateClause(clause, context);
            
            if (isAnd && !clauseResult) return false;    // short-circuit AND
            if (!isAnd && clauseResult) return true;      // short-circuit OR
        }
        
        return isAnd; // AND: all passed; OR: none passed
    }
    
    private boolean evaluateClause(Clause clause, EvaluationContext context) {
        if ("percentage".equals(clause.getAttribute()) && "rollout".equals(clause.getOp())) {
            return evaluatePercentageRollout(clause, context);
        }
        
        String contextValue = context.getAttribute(clause.getAttribute());
        if (contextValue == null) return false;
        
        return switch (clause.getOp()) {
            case "eq" -> clause.getValues().contains(contextValue);
            case "neq" -> !clause.getValues().contains(contextValue);
            case "in" -> clause.getValues().contains(contextValue);
            case "not_in" -> !clause.getValues().contains(contextValue);
            case "starts_with" -> clause.getValues().stream().anyMatch(contextValue::startsWith);
            case "ends_with" -> clause.getValues().stream().anyMatch(contextValue::endsWith);
            case "contains" -> clause.getValues().stream().anyMatch(contextValue::contains);
            case "regex" -> clause.getValues().stream().anyMatch(v -> contextValue.matches(v));
            case "gt" -> Double.parseDouble(contextValue) > Double.parseDouble(clause.getValues().get(0));
            case "lt" -> Double.parseDouble(contextValue) < Double.parseDouble(clause.getValues().get(0));
            default -> false;
        };
    }
    
    /**
     * Deterministic percentage rollout using consistent hashing.
     * Same user always gets the same result for the same flag.
     */
    private boolean evaluatePercentageRollout(Clause clause, EvaluationContext context) {
        int percentage = Integer.parseInt(clause.getValues().get(0));
        String rolloutKey = clause.getRolloutKey();
        String keyValue = context.getAttribute(rolloutKey);
        
        if (keyValue == null) return false;
        
        // Hash the flag key + user key to get deterministic bucket
        String hashInput = flagCache.get(clause.getAttribute()) != null
            ? clause.getAttribute() + ":" + keyValue
            : keyValue;
        int hash = murmurHash3(hashInput);
        int bucket = Math.abs(hash % 100);
        
        return bucket < percentage;
    }
    
    /**
     * SSE listener for real-time flag updates.
     */
    private void startSSEListener() {
        sseExecutor.submit(() -> {
            while (!Thread.currentThread().isInterrupted()) {
                try {
                    String sseUrl = String.format(
                        "%s/api/v1/sdk/stream?service=%s&env=%s",
                        serverUrl, serviceName, environment
                    );
                    
                    HttpClient client = HttpClient.newBuilder()
                        .connectTimeout(Duration.ofSeconds(5))
                        .build();
                    
                    HttpRequest request = HttpRequest.newBuilder(URI.create(sseUrl))
                        .header("Accept", "text/event-stream")
                        .build();
                    
                    client.send(request, HttpResponse.BodyHandlers.ofLines())
                        .body()
                        .forEach(line -> {
                            if (line.startsWith("data:")) {
                                String data = line.substring(5).trim();
                                handleFlagUpdate(data);
                            }
                        });
                    
                } catch (Exception e) {
                    log.warn("SSE connection lost, reconnecting in 5s", e);
                    try { Thread.sleep(5000); } catch (InterruptedException ie) { break; }
                }
            }
        });
    }
    
    private void handleFlagUpdate(String jsonData) {
        try {
            FlagUpdateEvent event = parseUpdateEvent(jsonData);
            switch (event.getType()) {
                case "update":
                    flagCache.put(event.getFlag().getKey(), event.getFlag());
                    log.info("Flag updated: {}", event.getFlag().getKey());
                    break;
                case "delete":
                    flagCache.remove(event.getFlagKey());
                    log.info("Flag removed: {}", event.getFlagKey());
                    break;
            }
        } catch (Exception e) {
            log.error("Failed to process flag update", e);
        }
    }
    
    @Override
    public void close() {
        sseExecutor.shutdownNow();
    }
}
```

**EvaluationContext:**

```java
public class EvaluationContext {
    private final Map<String, String> attributes;
    
    public EvaluationContext(String userId) {
        this.attributes = new HashMap<>();
        this.attributes.put("user_id", userId);
    }
    
    public EvaluationContext withAttribute(String key, String value) {
        this.attributes.put(key, value);
        return this;
    }
    
    public String getAttribute(String key) {
        return attributes.get(key);
    }
}

// Usage in application code:
EvaluationContext ctx = new EvaluationContext("user-123")
    .withAttribute("country", "US")
    .withAttribute("user_group", "beta_testers")
    .withAttribute("plan", "enterprise");

if (featureFlags.isEnabled("new_checkout_flow", ctx)) {
    // New checkout code path
} else {
    // Old checkout code path
}
```

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Flag server unavailable during SDK init | SDK has no flags | Use local file fallback (baked into Docker image at build time). Stale by up to one deploy cycle. |
| SSE connection drops | Flag updates not received | Auto-reconnect with exponential backoff. SDK continues using cached values. |
| SSE event lost (network glitch) | One flag update missed | SDK periodically does a full refresh (every 5 min) as a safety net. |
| Flag cache corrupted (OOM, bug) | Wrong flag evaluations | SDK catches exceptions and returns default value. Metrics track cache size and evaluation errors. |
| Thread contention on ConcurrentHashMap | Slightly elevated latency | ConcurrentHashMap uses lock-striping (16 segments default). At 100 evaluations/s/thread, contention is negligible. |

**Interviewer Q&As:**

**Q1: Why in-process evaluation instead of a remote API call?**
A: At 1.2M evaluations/second platform-wide, remote API calls would require a massive flag service fleet (even at 10K RPS/instance, that's 120 instances). In-process evaluation has zero network latency (< 0.1 ms vs. 5-50 ms for remote), no availability dependency (cached values work offline), and no bandwidth cost. The trade-off: SDK complexity and stale data (up to 5 seconds).

**Q2: How do you ensure deterministic percentage rollouts?**
A: We hash `flag_key + user_id` using MurmurHash3 and take `abs(hash % 100)`. This is deterministic: the same user always gets the same bucket for the same flag. When the rollout percentage increases from 25% to 50%, users in buckets 0-24 remain in the treatment (no flip-flopping), and users in buckets 25-49 are added. Different flags get different assignments because the flag key is part of the hash input.

**Q3: How do you handle flag evaluation for anonymous users (no user_id)?**
A: Use an alternative rollout key: session_id, device_id, or a randomly generated identifier stored in a cookie. If no identifier is available, fall back to the default value (flag off). For server-to-server traffic without a user context, use the request_id as the rollout key (each request gets an independent coin flip).

**Q4: How does the SDK handle a flag that doesn't exist in the cache?**
A: Returns the SDK-level default (e.g., `false` for boolean, `""` for string). Logs a warning. This is critical for resilience: if a flag is created in the management service but the SSE update hasn't arrived yet, the SDK must not crash. The warning log helps developers catch typos in flag keys during development.

**Q5: How do you test flag targeting rules?**
A: The management service has a "test evaluation" endpoint: `POST /api/v1/flags/{key}/evaluate` with a sample context. The dashboard shows which rule matched and why. SDK also has a `evaluateWithDetails` method that returns the matched rule, not just the value. This is used in integration tests.

**Q6: What about Java-specific thread safety concerns?**
A: `ConcurrentHashMap` is thread-safe for reads and writes. The SSE update thread writes to the map; application threads read. Since `ConcurrentHashMap.get()` is lock-free, there's no contention on reads. For atomic rule updates (multiple related flags), we use a copy-on-write pattern: build a new map with all updated flags, then swap the reference atomically via `volatile`.

---

### Deep Dive 2: Real-Time Flag Propagation

**Why it's hard:**
12,000 SDK instances must receive flag updates within 5 seconds. SSE connections are long-lived HTTP connections that don't play well with load balancers (health checks, connection limits, idle timeouts). The system must handle reconnections gracefully and ensure no updates are lost.

**Approaches:**

| Approach | Propagation Latency | Complexity | Connection Overhead |
|----------|-------------------|------------|---------------------|
| **Polling (30s interval)** | Up to 30 s | Low | Low (periodic HTTP) |
| **Polling (5s interval)** | Up to 5 s | Low | Medium (frequent HTTP) |
| **SSE (Server-Sent Events)** | < 2 s | Medium | High (12K persistent connections) |
| **WebSocket** | < 1 s | High | High (12K persistent connections) |
| **gRPC streaming** | < 1 s | High | High |
| **SSE with Redis Pub/Sub fanout** | < 3 s | Medium | High, but horizontally scalable |

**Selected approach: SSE with Redis Pub/Sub for horizontal scaling**

**Justification:** SSE is simpler than WebSocket (unidirectional, automatic reconnection built into EventSource API). Redis Pub/Sub distributes events to all SSE gateway instances, enabling horizontal scaling. Each gateway instance holds a subset of connections.

**Implementation detail:**

```
Flag Change -> MySQL -> Redis Pub/Sub -> SSE Gateway (N instances) -> 12K SDK instances

Gateway instance 1: 4,000 SSE connections
Gateway instance 2: 4,000 SSE connections
Gateway instance 3: 4,000 SSE connections
```

```java
// SSE Gateway (Spring Boot with WebFlux)
@RestController
public class SSEController {
    
    private final RedisMessageListenerContainer redisContainer;
    private final ObjectMapper objectMapper;
    
    @GetMapping(value = "/api/v1/sdk/stream", produces = MediaType.TEXT_EVENT_STREAM_VALUE)
    public Flux<ServerSentEvent<String>> stream(
        @RequestParam String service,
        @RequestParam String env
    ) {
        return Flux.create(sink -> {
            // Subscribe to Redis Pub/Sub channel
            String channel = String.format("flag-updates:%s", env);
            
            MessageListener listener = (message, pattern) -> {
                try {
                    FlagUpdateEvent event = objectMapper.readValue(
                        message.getBody(), FlagUpdateEvent.class
                    );
                    
                    // Filter: only send events relevant to this service
                    if (event.isRelevantTo(service)) {
                        sink.next(ServerSentEvent.<String>builder()
                            .event("flag-update")
                            .data(objectMapper.writeValueAsString(event))
                            .id(String.valueOf(event.getVersion()))
                            .build());
                    }
                } catch (Exception e) {
                    log.error("Error processing flag update", e);
                }
            };
            
            redisContainer.addMessageListener(listener, new ChannelTopic(channel));
            
            sink.onDispose(() -> {
                redisContainer.removeMessageListener(listener);
            });
        });
    }
}
```

**Handling SSE connection limits:**

- Each SSE gateway instance handles up to 5,000 connections (limited by file descriptors and memory).
- 12,000 connections / 5,000 per instance = 3 gateway instances minimum (run 5 for redundancy).
- Load balancer distributes SSE connections across gateway instances using round-robin.
- Connection keep-alive: gateway sends a heartbeat event every 30s to prevent proxy/LB idle timeouts.

**Failure modes:**
| Failure | Impact | Mitigation |
|---------|--------|------------|
| Redis Pub/Sub down | Gateway can't receive events | Gateway falls back to polling MySQL every 5s. SDKs unaware. |
| SSE gateway crash | Subset of SDKs lose connection | SDK auto-reconnects (EventSource built-in retry). LB health check removes dead gateway. |
| LB idle timeout kills SSE connection | SDK disconnected | Heartbeat every 30s keeps connection alive. SDK reconnects on disconnect. |
| Network partition between gateway and Redis | Gateway stale | Health check marks gateway as unhealthy; LB routes new connections to healthy instances. |
| Burst of 100 flag changes in 1 second | SSE flooded; SDK processing overhead | Debounce on gateway: batch changes within 1s window into a single SSE event. |

**Interviewer Q&As:**

**Q1: Why SSE instead of WebSocket?**
A: SSE is simpler: it's built on HTTP (works through all proxies, load balancers, CDNs), has automatic reconnection in the browser's `EventSource` API, and is unidirectional (server -> client), which is all we need (SDKs don't send data to the server). WebSocket's bidirectionality is unnecessary overhead. For server-side Java/Python SDKs, SSE client libraries are simpler and more reliable.

**Q2: How do you handle SDK reconnection after a long network outage?**
A: SDK includes a `Last-Event-ID` header on reconnection. The gateway stores the last 1,000 events in Redis (with version numbers). On reconnection, it replays events since the client's last-seen version. If the client is too far behind (gap > 1,000 events), the gateway sends a full flag snapshot instead.

**Q3: How do you scale to 100,000 SDK instances?**
A: Add more SSE gateway instances (100K / 5K per instance = 20 instances). Redis Pub/Sub scales well for this use case (one channel, 20 subscribers). If Redis becomes a bottleneck, use Redis Cluster with multiple channels (shard by environment or service group). The flag management service and MySQL are unaffected (still ~50 writes/day).

**Q4: How do you ensure no flag updates are lost?**
A: Three layers of protection: (1) SSE push for real-time (< 5s). (2) Full refresh polling every 5 minutes (catches any missed SSE events). (3) On SDK restart, initialize from the server's current state. The full refresh is the ultimate consistency mechanism -- SSE is an optimization, not the only path.

**Q5: What about latency between flag change and SDK update in a different region?**
A: MySQL is in a single region (primary). Redis Pub/Sub is also single-region. SSE gateways in other regions subscribe to Redis via cross-region network. Latency = MySQL write (~5ms) + Redis Pub/Sub (~1ms) + cross-region network (~50-100ms) + SSE delivery (~10ms) = ~70-120ms. Well within the 5s target. For stricter requirements, deploy read replicas and Redis per region.

**Q6: How do you handle the thundering herd when a flag changes and 12,000 SDKs update simultaneously?**
A: The update is a small JSON payload (~5 KB). Total bandwidth: 12K x 5 KB = 60 MB. Spread across 5 gateway instances, each sends 12 MB -- trivial for modern servers. The SDKs process the update in-memory (ConcurrentHashMap put), which takes < 1ms. No thundering herd because there's no server-side processing triggered by the update.

---

### Deep Dive 3: Flag Lifecycle Management (Preventing Flag Debt)

**Why it's hard:**
Feature flags accumulate over time. "Temporary" flags become permanent. Dead code behind flags is never cleaned up. After 2 years, a codebase has 500 flags, half of which are stale. Evaluating stale flags wastes CPU, confuses developers, and creates maintenance burden. This is "flag debt."

**Approaches:**

| Approach | Effectiveness | Developer Friction |
|----------|--------------|-------------------|
| **No management (hope for the best)** | 0% cleanup | None |
| **Documentation + reminders** | ~20% cleanup | Low |
| **Stale detection + automated alerts** | ~50% cleanup | Low-Medium |
| **Enforced expiry (flag auto-disables after date)** | ~80% cleanup | Medium |
| **Code analysis + automated PR creation** | ~90% cleanup | Low |

**Selected approach: Stale detection + automated alerts + code analysis PRs**

**Implementation detail:**

```python
# Flag Lifecycle Manager (runs daily via cron)

class FlagLifecycleManager:
    def __init__(self, db, github_client, slack_client):
        self.db = db
        self.github = github_client
        self.slack = slack_client
    
    def run_daily_check(self):
        stale_flags = self.find_stale_flags()
        
        for flag in stale_flags:
            staleness_days = (datetime.now() - flag.stale_after).days
            
            if staleness_days <= 7:
                # First reminder
                self.send_slack_reminder(flag, "Flag is past its stale date. Please clean up.")
            
            elif staleness_days <= 30:
                # Escalation: weekly reminder + create code cleanup PR
                if staleness_days % 7 == 0:
                    self.create_cleanup_pr(flag)
                    self.send_slack_reminder(flag, "Auto-generated cleanup PR created.")
            
            elif staleness_days <= 90:
                # Escalation: notify team lead
                self.notify_team_lead(flag)
                self.send_slack_reminder(flag, "OVERDUE: Flag is 30+ days past stale date.")
            
            else:
                # Flag auto-marked as deprecated
                self.mark_deprecated(flag)
                self.send_slack_reminder(flag, "Flag auto-deprecated after 90 days past stale date.")
    
    def find_stale_flags(self):
        return self.db.query("""
            SELECT * FROM feature_flags 
            WHERE lifecycle_status = 'active'
            AND stale_after IS NOT NULL
            AND stale_after < NOW()
        """)
    
    def create_cleanup_pr(self, flag):
        """
        Scan the codebase for flag usage and create a PR that removes
        the flag checks (replaces with the dominant value).
        """
        # Find all files referencing this flag
        repos = self.find_repos_using_flag(flag.flag_key)
        
        for repo in repos:
            # Analyze: what value does this flag evaluate to 99% of the time?
            dominant_value = self.get_dominant_value(flag.flag_key)
            
            # Generate code changes
            changes = self.generate_flag_removal(repo, flag.flag_key, dominant_value)
            
            if changes:
                self.github.create_pr(
                    repo=repo,
                    branch=f"flag-cleanup/{flag.flag_key}",
                    title=f"Remove stale feature flag: {flag.flag_key}",
                    body=f"""
## Flag Cleanup

Flag `{flag.flag_key}` has been evaluating to `{dominant_value}` for 100% of traffic since {flag.stale_after}.

This PR removes the flag check and keeps the `{dominant_value}` code path.

Owner: {flag.owner_team}
Jira: {flag.jira_ticket}
Stale since: {flag.stale_after}

Auto-generated by Flag Lifecycle Manager.
                    """,
                    changes=changes
                )
```

**Dashboard metrics for flag health:**

| Metric | Calculation | Purpose |
|--------|-------------|---------|
| Total active flags | COUNT(lifecycle_status='active') | Track growth |
| Stale flags | COUNT(stale_after < NOW()) | Debt indicator |
| Flag age distribution | Histogram of (NOW() - created_at) | Identify long-lived "temporary" flags |
| Flag evaluation uniformity | % of flags evaluating to same value for 100% of traffic | These are candidates for cleanup |
| Flags without stale_after | COUNT(stale_after IS NULL AND lifecycle_status='active') | Missing metadata |
| Cleanup PRs merged rate | PRs merged / PRs created | Effectiveness of automation |

**Interviewer Q&As:**

**Q1: How do you set the stale_after date for a flag?**
A: At creation time, the developer sets `stale_after` (required field). Default is 90 days from creation. For permanent flags (kill switches, feature toggles for compliance), `lifecycle_status` is set to `permanent`, and `stale_after` is null. Permanent flags require team lead approval.

**Q2: What if a flag is still needed past its stale date?**
A: The developer extends the stale date via `flagctl extend new_checkout_flow --days 30`. This requires a comment explaining why. Extensions are limited (max 3 extensions, max 6 months total). After that, the flag must be promoted to `permanent` (requires approval) or cleaned up.

**Q3: How do you find which code references a flag?**
A: The lifecycle manager searches across all repos for the flag key string (`isEnabled("new_checkout_flow")`). It uses GitHub Code Search API or a local code search index (Sourcegraph). Results include file path, line number, and surrounding code, which is used to generate the cleanup PR.

**Q4: What if removing a flag breaks the application?**
A: The cleanup PR runs the full CI pipeline (build + test) before being submitted for review. If tests fail, the PR is not created, and the flag is flagged for manual investigation. The PR description includes instructions for the developer to review and test.

**Q5: How do you prevent flag proliferation?**
A: (1) Flag creation requires `jira_ticket` (links to a feature ticket). (2) `stale_after` is mandatory. (3) Weekly report shows each team's flag count and stale flag count. (4) Gamification: leaderboard of teams with the lowest flag debt.

**Q6: How does flag cleanup work for flags used across multiple services?**
A: Cross-service flags are tagged `scope: global`. The lifecycle manager identifies all services using the flag (via code search and evaluation analytics). Cleanup requires coordinating PRs across all services. The flag is only archived after all code references are removed.

---

## 7. Scheduling & Resource Management

### Flag Service Resource Requirements

| Component | Resources | Instances |
|-----------|-----------|-----------|
| Flag Management Service | 2 CPU, 4 Gi memory | 3 (HA) |
| SSE Gateway | 4 CPU, 8 Gi memory (connection-heavy) | 5 |
| MySQL | 4 CPU, 16 Gi memory | 1 primary + 2 replicas |
| Redis (Pub/Sub) | 2 CPU, 4 Gi memory | 3-node Sentinel |

This is a lightweight service. Total resource footprint: ~40 CPU cores, ~100 Gi memory.

### SDK Resource Usage (Per Application Instance)

| Resource | Usage |
|----------|-------|
| Memory (flag cache) | 500 KB - 5 MB (depends on number of flags and rule complexity) |
| CPU (evaluation) | < 0.01 CPU cores at 100 eval/s (evaluations are < 0.1ms each) |
| Network (SSE) | 1 persistent HTTP connection (~10 KB/s average, mostly heartbeats) |
| Threads | 1 thread for SSE listener |

SDK overhead is negligible compared to the application's own resource usage.

---

## 8. Scaling Strategy

### Scaling SSE Connections

| Connections | Gateway Instances | Notes |
|-------------|-------------------|-------|
| 12,000 | 5 | Current state; comfortable headroom |
| 50,000 | 12 | Each handles ~4,200 connections |
| 100,000 | 25 | Need to tune OS file descriptor limits (ulimit 100K) |
| 500,000 | 120 | Consider switching to gRPC streaming for efficiency |

### Interviewer Q&As

**Q1: How do you scale the flag service for 100,000 microservice instances?**
A: The server only handles flag CRUD (50 req/day) and SSE connections. CRUD scales vertically (MySQL handles this easily). SSE connections scale horizontally (add gateway instances behind a load balancer). At 100K connections, the bottleneck is Redis Pub/Sub fan-out (100K messages per flag change). If this becomes an issue, use Redis Cluster with per-environment channels, or switch to Kafka for event distribution.

**Q2: How do you handle flag evaluation latency at 10M evaluations/second?**
A: Evaluations are in-process; the server is not involved. Each SDK instance handles its own evaluations from local cache. 10M evaluations/second across 100K instances = 100 evaluations/s per instance, which is trivial for in-memory HashMap lookups.

**Q3: What if the SDK initialization payload is too large (100,000 flags)?**
A: Filter flags per-service. Each flag has a `scope` (list of services that need it). The SDK init endpoint returns only flags relevant to the requesting service. If a service uses 100 flags out of 100K, the init payload is 100 x 5 KB = 500 KB. For truly global flags, use tag-based filtering.

**Q4: How does scaling affect flag update propagation latency?**
A: Propagation latency is dominated by Redis Pub/Sub (1ms) + SSE push (10ms). Adding more gateway instances doesn't increase latency because Redis Pub/Sub delivers to all subscribers simultaneously. The limiting factor is the number of concurrent SSE writes per gateway instance, which is bounded by thread pool size and network buffer.

**Q5: How do you handle a DDoS on the flag service?**
A: The SDK doesn't make frequent calls to the server (only on init and reconnection). Even if the server is down, SDKs continue evaluating from cache. The server is behind a rate limiter (API gateway): 100 req/s for CRUD, 50K req/s for SSE init. The real attack surface is the SSE gateway (connection exhaustion), mitigated by connection limits per source IP and maximum total connections.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Impact | Detection | Mitigation | Recovery Time |
|---|---------|--------|-----------|------------|---------------|
| 1 | Flag service completely down | SDKs can't initialize on new deploy; existing SDKs use cached flags | Health check (HTTP 200 on /health) | SDKs use local file fallback for init. Cached flags remain valid. | Service restart: < 1 min |
| 2 | Redis down | SSE updates not propagated; SDKs have stale flags | Redis Sentinel health check | Gateways poll MySQL every 5s. SDKs do full refresh every 5 min. | Redis failover: < 30s |
| 3 | MySQL down | Can't create/update flags; reads serve from cache | MySQL health check; replication lag | Read replica serves reads. Flag changes queued in gateway memory (< 1 min buffer). | MySQL failover: < 30s |
| 4 | SSE gateway crash (1 of 5) | 2,400 SDKs disconnected | Gateway health check; connection count drop | SDKs auto-reconnect to other gateways (LB re-routes). | < 10s (reconnection) |
| 5 | Network partition: SDK can't reach gateway | SDK has stale flags | SDK logs connection error | SDK continues with cached flags. Periodic retry. | Partition heals |
| 6 | Bad flag update (misconfigured rule) | Feature broken for affected users | Error rate spike in application metrics | Kill switch: `flagctl kill bad_flag --env prod`. Propagates in < 5s. | < 10s (kill switch) |
| 7 | All SSE gateways down | No flag updates for any SDK | Zero connections metric | SDKs fall back to polling every 5 min. Flag changes are delayed, not lost. | Gateway restart: < 1 min |
| 8 | Flag service deployment (rolling update) | Brief SSE disconnections during pod replacement | Expected during deploys | Graceful shutdown: gateway sends "close" SSE event, SDKs reconnect to healthy instances. | < 30s per pod replacement |

### Kill Switch Design (Ultra-Reliable)

The kill switch is a specialized flag operation that must work even when the flag service is degraded:

```
Kill switch activated
    |
    +-> MySQL: UPDATE flag_environments SET enabled = false WHERE ...
    |
    +-> Redis Pub/Sub: PUBLISH "flag-updates:production" { "type": "kill", "flag_key": "..." }
    |       |
    |       +-> SSE Gateways push to all SDKs (< 2s)
    |
    +-> Emergency broadcast: direct HTTP POST to all known gateway IPs (bypass LB)
    |
    +-> Slack: "@here KILL SWITCH activated for flag X by alice"
    |
    +-> PagerDuty: alert on-call
```

Kill switch has a dedicated fast path: no validation delays, no approval gates, no batching. Propagation target: < 2 seconds.

---

## 10. Observability

### Key Metrics

| Metric | Type | Alert Threshold | Source |
|--------|------|-----------------|--------|
| `flag_evaluations_total` | Counter by (flag, value) | N/A (analytics) | SDK |
| `flag_evaluation_duration_ms` | Histogram | p99 > 1 ms | SDK |
| `flag_update_propagation_seconds` | Histogram | p95 > 5 s | SSE Gateway |
| `sse_connections_active` | Gauge per gateway | < 1,000 (instance down?) | SSE Gateway |
| `sse_reconnection_rate` | Counter | > 100/min (instability) | SDK |
| `flag_cache_size` | Gauge | > 10,000 (too many flags?) | SDK |
| `stale_flags_count` | Gauge | > 100 (flag debt) | Lifecycle Manager |
| `flag_changes_total` | Counter by (action, user) | > 50/hour (mass change?) | Flag Service |
| `sdk_init_duration_seconds` | Histogram | p95 > 5 s | SDK |
| `kill_switch_activated` | Counter | > 0 (immediate investigation) | Flag Service |
| `flag_uniform_evaluation_count` | Gauge | > 500 (cleanup candidates) | Analytics |

### Dashboards
1. **Flag Health:** Active flags, stale flags, flag creation/cleanup rate, flag debt trend.
2. **SDK Health:** SSE connection count, reconnection rate, init latency, evaluation latency.
3. **Flag Usage:** Per-flag evaluation counts, value distribution, most-used flags.
4. **Operations:** Kill switch activations, flag changes timeline, audit log.

---

## 11. Security

| Control | Implementation |
|---------|---------------|
| RBAC | Flag management: team members can edit their own flags. Production toggles require `flag:production` role. Kill switch requires `flag:kill` role (on-call + team leads). |
| Audit logging | Every flag change recorded with user identity, timestamp, previous/new value. Immutable in Elasticsearch. |
| SDK authentication | SDK authenticates with a service-specific API key (rotated monthly). Key embedded in pod via k8s Secret. |
| Flag data classification | Targeting rules may contain user IDs. Flag evaluations log only aggregate counts, not individual user decisions. |
| Encryption | Flag data encrypted at rest (MySQL TDE). SSE uses TLS. SDK API key transmitted over HTTPS. |
| Rate limiting | CRUD API: 100 req/s per user. SSE init: 1,000 req/s (handles reconnection storms). Kill switch: no rate limit (emergency path). |
| Separation of environments | Flag states are separate per environment. A staging flag change cannot affect production. SDK connects to its environment's endpoint. |
| Change review | For production flags with > 10% user impact, require 2-person approval (approver != requester). |

---

## 12. Incremental Rollout Strategy

### Rolling Out the Feature Flag Service Itself

**Phase 1: Infrastructure (Week 1-2)**
- Deploy flag service, MySQL, Redis, SSE gateways.
- Create SDKs for Java and Python.
- Internal dogfooding: flag service team uses it for their own features.

**Phase 2: Pilot (Week 3-4)**
- 5 volunteer teams (10 services) adopt the SDK.
- Create initial flags for ongoing feature development.
- Validate: SDK init time, evaluation latency, SSE propagation time, kill switch response time.

**Phase 3: Expand (Week 5-8)**
- 20 additional teams adopt.
- Build dashboard with analytics and lifecycle management.
- Run first flag cleanup cycle.

**Phase 4: Platform adoption (Month 3+)**
- All new features must use feature flags (policy).
- Provide migration tooling for teams using config-file-based flags.
- SDK bundled in the company's service template (scaffold new service -> flags built-in).

### Rollout Q&As

**Q1: How do you migrate teams from hard-coded feature toggles to the flag service?**
A: Provide a migration script that: (1) scans code for common toggle patterns (e.g., `if (config.get("feature.new_checkout"))` in Java). (2) Creates corresponding flags in the flag service. (3) Generates SDK code to replace config lookups. (4) Creates a PR. The migration is gradual: old config toggles and new flags can coexist.

**Q2: How do you handle the SDK being a new dependency for all services?**
A: The SDK is a lightweight library (< 1 MB, zero transitive dependencies for the core). It's added to the company's base library (included in all services by default). Initialization is non-blocking: if the flag server is unreachable, the SDK returns defaults, and the service starts normally.

**Q3: What if the flag service has a critical bug that affects all services?**
A: SDKs cache flags locally and continue evaluating even if the server is completely down. The kill switch path is independent of the main service (direct HTTP to gateways). In the worst case, we can push a static flag file to all pods via ConfigMap update (takes ~5 min).

**Q4: How do you handle backward compatibility when updating the SDK?**
A: SDK follows semantic versioning. Breaking changes (rare) increment the major version. The server supports multiple SDK versions simultaneously (response format is versioned via `Accept` header). We maintain backward compatibility for at least 2 major versions.

**Q5: How do you measure the success of the flag service rollout?**
A: Metrics: (1) Adoption rate (% of services using SDK). (2) Flag usage (evaluations/day). (3) Deployment frequency (should increase with flags). (4) Incident recovery time (should decrease with kill switches). (5) Flag debt ratio (stale flags / active flags, target < 20%).

---

## 13. Trade-offs & Decision Log

| Decision | Option Chosen | Alternative | Rationale |
|----------|---------------|-------------|-----------|
| Evaluation model | In-process (SDK local cache) | Remote API, sidecar | Zero latency, zero server dependency. At 1.2M eval/s, remote is impractical. Sidecar adds 1-5ms and deployment complexity. |
| Update mechanism | SSE push | Polling, WebSocket, gRPC stream | SSE: simple, HTTP-native, auto-reconnect. Polling too slow for kill switch. WebSocket adds bidirectional complexity we don't need. |
| Horizontal fanout | Redis Pub/Sub | Kafka, NATS | Redis Pub/Sub is simple, low-latency, fire-and-forget (matches SSE's delivery model). Kafka adds persistence we don't need for ephemeral events. |
| Flag storage | MySQL | PostgreSQL, DynamoDB | Low write volume (50/day). MySQL is familiar to ops team. No need for DynamoDB's scale. |
| Percentage rollout hashing | MurmurHash3 | SHA-256, CRC32 | MurmurHash3 is fast, good distribution, widely used in feature flag systems. SHA-256 is overkill. CRC32 has worse distribution. |
| SDK language | Java first, then Python | Go, JavaScript first | Java is our primary backend language. Python is second. Go and JavaScript SDKs follow based on demand. |
| Lifecycle management | Stale detection + automated PRs | Manual only, enforced expiry | Automated cleanup is the most effective at reducing flag debt without high developer friction. Enforced expiry is too aggressive (could break features). |
| Kill switch design | Dedicated fast path (bypass batching) | Same path as normal toggle | Kill switch must propagate in < 2s. Normal toggles can take 5s. Dedicated path ensures emergency response is never delayed. |

---

## 14. Agentic AI Integration

### AI-Powered Flag Intelligence

| Use Case | Implementation |
|----------|---------------|
| **Auto-generate cleanup PRs** | AI agent reads application code around flag references, understands the control flow, and generates correct code removal (not just deleting the `if` statement, but choosing the correct branch). Uses LLM with code context. |
| **Flag impact analysis** | Before a flag toggle, agent predicts impact by analyzing: traffic volume, user segment size, downstream dependencies. "Enabling flag X will affect ~50,000 users and increase load on payment-svc by ~5%." |
| **Anomaly detection on flag change** | Agent monitors application metrics (error rate, latency, throughput) after a flag change. Detects regressions and auto-disables the flag if anomalies detected. "Flag 'new_checkout' enabled 3 minutes ago. Error rate increased from 0.1% to 2.3%. Auto-killing flag." |
| **Flag conflict detection** | Agent analyzes targeting rules across all flags and detects conflicts: "Flag A targets users in group 'beta' with value=true. Flag B targets users in group 'beta' with value=false. These flags may interact unexpectedly." |
| **Optimal rollout strategy** | Agent recommends rollout percentages based on historical data. "For checkout flags, 1% for 24h, then 10%, then 50%, then 100% has worked best based on past rollouts. Average time-to-rollback at 1%: 2h vs 15min at 10%." |
| **Natural language flag creation** | "Create a flag that enables the new checkout flow for 10% of US enterprise customers." Agent translates to: flag_key: `new_checkout_flow`, targeting: `country=US AND plan=enterprise AND rollout=10%`. |

---

## 15. Complete Interviewer Q&A Bank

**Q1: Why build an internal feature flag service instead of using LaunchDarkly or Split?**
A: (1) Cost: at 1.2M evaluations/second, SaaS pricing is prohibitive (~$100K+/month). (2) Data sovereignty: flag targeting rules contain user IDs and business logic. Keeping this in-house avoids sending sensitive data to a third party. (3) Customization: we need deep integration with our CI/CD pipeline, monitoring stack, and deployment controller. (4) Latency: in-process SDK evaluation is identical to what LaunchDarkly offers; the server component is simpler to build.

**Q2: How does percentage rollout work without user ID duplication?**
A: MurmurHash3 on `flag_key + ":" + user_id` gives a hash. `abs(hash) % 100` gives a bucket (0-99). User in bucket 0-24 gets the flag at 25%. When we increase to 50%, users in buckets 0-49 get the flag. The key insight: users in 0-24 don't change (no flip-flopping). Including `flag_key` in the hash input means different flags give different bucket assignments to the same user, avoiding correlation.

**Q3: What happens if two admins update the same flag simultaneously?**
A: Optimistic locking via the `version` column in `flag_environments`. Each update increments the version. If two concurrent updates have the same base version, the second fails with a conflict error (HTTP 409). The admin must refresh and retry.

**Q4: How do you handle flag evaluation in a batch processing context (no user)?**
A: For batch jobs, the evaluation context can use a job_id, batch_id, or a fixed string. If the flag should apply uniformly (on/off for the entire batch), use a context with a static key. If it should apply per-record, use the record ID as the rollout key.

**Q5: How do you correlate flag rollout with application metrics?**
A: The flag service exposes evaluation analytics (% of evaluations returning true/false over time). We overlay this with application metrics in Grafana. When a flag is ramped from 10% to 25%, the dashboard shows the inflection point alongside error rate and latency. This helps identify whether metric changes are caused by the flag rollout.

**Q6: What's the risk of a feature flag service being a single point of failure?**
A: Minimal, because of in-process evaluation. If the flag server is completely down: (1) Existing service instances continue evaluating from cache (no impact). (2) New service instances use the local fallback file. (3) Flag changes can't be made until the server recovers (inconvenient but not catastrophic). The server is deployed with HA (3 replicas, MySQL Multi-AZ, Redis Sentinel).

**Q7: How do you handle feature flags in a monolith vs. microservices?**
A: Same SDK, different scoping. In a monolith, all flags are loaded into one SDK instance. In microservices, each service's SDK loads only its relevant flags. The flag service supports both: the `service` query parameter filters flags for microservices, while a monolith requests all flags.

**Q8: How do you test feature-flagged code?**
A: (1) Unit tests: SDK provides a `TestFeatureFlagClient` that allows setting flag values directly (no server needed). (2) Integration tests: set flag values via API before running tests. (3) Contract tests: test both code paths (flag on and flag off) to ensure both are correct. (4) CI pipeline: run test suite twice (once with flag on, once with flag off) for critical flags.

**Q9: How do you handle flag dependencies (flag B should only be enabled if flag A is enabled)?**
A: We don't enforce dependencies at the flag service level (that would add coupling). Instead, the application code checks both flags: `if (flags.isEnabled("A") && flags.isEnabled("B"))`. The dashboard shows dependency documentation (metadata, not enforcement). If strict enforcement is needed, a "prerequisite flag" field in the targeting rule conditionally evaluates flag B only if flag A is true.

**Q10: How do you implement emergency kill switches?**
A: A kill switch is a regular boolean flag with special metadata (`tag: kill-switch`, `lifecycle_status: permanent`). The kill switch is always evaluated first (highest priority targeting rule). The kill API (`POST /api/v1/flags/{key}/kill`) sets `enabled=false` and broadcasts on the emergency channel (bypasses debouncing). SDKs prioritize kill events in their update processing.

**Q11: How do you handle flag state during blue-green deployments?**
A: Both Blue and Green environments use the same flag service. Flags are per-environment (production), not per-deployment. Both versions evaluate the same flags. If a flag should only apply to the new version (Green), use a targeting rule that matches on application version: `app_version >= 1.2.4`.

**Q12: What's the maximum number of targeting rules per flag?**
A: Practically, < 20 rules. Each evaluation iterates through rules sequentially (first match). With 20 rules and 5 clauses each, evaluation is still < 0.1ms (100 comparisons is trivial for a CPU). If a flag has > 20 rules, it's probably too complex and should be split into multiple flags.

**Q13: How do you handle client-side (browser) feature flags securely?**
A: Client-side SDKs have different security requirements: targeting rules must not be sent to the client (they reveal business logic and user segmentation). Instead, the server evaluates the flag and sends only the result (value) to the client. This is done via a "relay proxy" that the client-side SDK calls, which evaluates flags on the server and returns values.

**Q14: How do you handle gradual rollout across environments (dev -> staging -> prod)?**
A: Each environment has independent flag state. Workflow: (1) Enable flag in dev (100%). (2) Test. (3) Enable in staging (100%). (4) Integration test. (5) Enable in production at 1%, ramp gradually. The environments are completely independent -- enabling in dev has no effect on production.

**Q15: How do you measure the business impact of a feature flag?**
A: The analytics system correlates flag evaluation with business metrics. For an A/B test: segment users into treatment (flag=true) and control (flag=false), then compare conversion rate, revenue, engagement. This requires the analytics pipeline to join flag evaluation data with business events on user_id. We export sampled evaluation events to the data warehouse for this analysis.

**Q16: What happens when you have 10,000 flags? Does SDK performance degrade?**
A: SDK initialization loads all relevant flags (filtered by service). If a service uses 200 flags (max), the in-memory cache is ~1 MB. Evaluation iterates rules for one flag (not all 10,000). Performance is O(rules_per_flag), not O(total_flags). The only concern is cache memory, which is proportional to flag count per service, not total flags.

---

## 16. References

- LaunchDarkly Architecture: https://launchdarkly.com/blog/how-launchdarkly-works/
- Split.io SDK Architecture: https://www.split.io/product/sdk/
- Unleash (Open Source Feature Flags): https://docs.getunleash.io/
- Martin Fowler, "Feature Toggles": https://martinfowler.com/articles/feature-toggles.html
- Google Experiments Platform: https://research.google/pubs/pub36500/
- MurmurHash3: https://en.wikipedia.org/wiki/MurmurHash
- Server-Sent Events: https://developer.mozilla.org/en-US/docs/Web/API/Server-sent_events
- OpenFeature Standard: https://openfeature.dev/
- Redis Pub/Sub: https://redis.io/docs/manual/pubsub/
