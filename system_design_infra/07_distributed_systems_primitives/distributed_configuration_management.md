# System Design: Distributed Configuration Management

> **Relevance to role:** Configuration management is the backbone of infrastructure operations. etcd stores all Kubernetes cluster state. Consul KV holds service configurations and feature flags. HashiCorp Vault manages secrets (database credentials, TLS certificates, API keys) for every service. OpenStack uses configuration files on every node that must stay in sync. Java/Python services need hot-reloadable configuration without restarts. Secret rotation (e.g., MySQL password change) must happen without downtime across hundreds of services simultaneously.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Hierarchical key-value store | Prefix-based namespacing (e.g., `/config/prod/nova-api/db_pool_size`) |
| FR-2 | Configuration versioning | Every change creates a new version; rollback to any previous version |
| FR-3 | Watch/subscribe for hot reload | Clients notified in real-time when configuration changes |
| FR-4 | Environment-specific config | dev / staging / prod overlays with inheritance |
| FR-5 | Secret management | Encrypted at rest; access-controlled; audit-logged; dynamic secrets with TTL |
| FR-6 | Secret rotation | Zero-downtime credential rotation (dual-read period) |
| FR-7 | Schema validation | Validate config values against a schema before write |
| FR-8 | Config templating | Template variables resolved at read time (e.g., `{{.env}}`) |
| FR-9 | Transactions | Atomic multi-key updates (e.g., update DB host + port together) |
| FR-10 | Audit trail | Who changed what, when, with diff |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Read latency | < 1 ms (cached), < 5 ms (uncached linearizable) |
| NFR-2 | Write latency | < 20 ms (Raft consensus) |
| NFR-3 | Availability | 99.99% |
| NFR-4 | Consistency | Linearizable for writes; configurable for reads (stale OK for caching) |
| NFR-5 | Watch notification latency | < 100 ms from write to client notification |
| NFR-6 | Scalability | 1M config keys; 10K concurrent watchers |
| NFR-7 | Secret encryption | AES-256-GCM at rest; TLS in transit |
| NFR-8 | Audit retention | 1 year |

### Constraints & Assumptions
- Kubernetes cluster uses etcd (we extend, not replace).
- Non-k8s services need the same config management capabilities.
- Java services use Spring Cloud Config; Python services use custom client.
- Secrets include: database credentials, API keys, TLS certificates, SSH keys.
- Multi-environment: dev, staging, production (separate clusters).
- Configuration changes are infrequent (< 100/day) but reads are very frequent (100K/sec).

### Out of Scope
- Feature flag management (LaunchDarkly-style percentage rollouts).
- Infrastructure-as-code (Terraform state management).
- Certificate authority operations (Vault PKI backend exists but CA design is separate).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Unique config keys | 50,000 | 500 services x 100 config keys avg |
| Unique secret keys | 10,000 | DB creds, API keys, TLS certs |
| Total keys | 60,000 | config + secrets |
| Config reads per second | 100,000 | 500 services x 10 instances x 20 reads/sec |
| Config writes per day | 100 | Human-driven changes |
| Secret reads per second | 10,000 | Service startup + periodic refresh |
| Secret writes per day | 50 | Rotation events, new secrets |
| Watch subscriptions | 10,000 | 5,000 service instances watching ~2 key prefixes each |
| Watch notifications per day | 10,000 | 100 config changes x 100 watchers avg |

### Latency Requirements

| Operation | p50 | p99 | Notes |
|-----------|-----|-----|-------|
| Config read (local cache) | 0.01 ms | 0.1 ms | In-memory |
| Config read (etcd) | 1 ms | 5 ms | Linearizable read from leader |
| Config write | 5 ms | 20 ms | Raft consensus |
| Secret read (Vault) | 2 ms | 10 ms | Token validation + decryption |
| Watch notification | 10 ms | 100 ms | Raft commit + fan-out |
| Secret rotation | 30 s | 120 s | End-to-end: generate + distribute + verify |

### Storage Estimates

| Item | Size | Total |
|------|------|-------|
| Config key-value (avg 1KB value) | 1 KB | 50 MB |
| Secret key-value (avg 512B encrypted) | 512 B | 5 MB |
| Version history (100 versions per key) | 100 x 1 KB | 5 GB |
| Audit log (1 year) | 256 B per event, 100/day x 365 | 9 MB |
| Raft WAL + snapshots | | 10 GB per node |
| **Total per node** | | **~15 GB** |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Config reads | 100K/sec x 1 KB | 100 MB/s (mostly served from cache) |
| Raft replication | 100 writes/day — negligible | < 1 MB/s |
| Watch notifications | 10K/day x 1 KB | < 1 MB/s |
| Secret reads | 10K/sec x 512 B | 5 MB/s |

---

## 3. High-Level Architecture

```
  ┌─────────────────────────────────────────────────────────────────┐
  │                     Client Applications                         │
  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
  │  │ Java/Spring  │  │ Python       │  │ k8s ConfigMaps /     │  │
  │  │ Cloud Config │  │ Config SDK   │  │ Secrets              │  │
  │  │ @RefreshScope│  │              │  │ (synced from our     │  │
  │  │              │  │              │  │  config store)        │  │
  │  └──────┬───────┘  └──────┬───────┘  └──────────┬───────────┘  │
  └─────────┼─────────────────┼──────────────────────┼──────────────┘
            │ gRPC/HTTP       │ gRPC/HTTP            │ k8s API
            │                 │                      │
  ┌─────────▼─────────────────▼──────────────────────▼──────────────┐
  │                    Config Management Service                     │
  │                                                                  │
  │  ┌────────────────────────────────────────────────────────────┐ │
  │  │                     API Layer                               │ │
  │  │  ┌────────────┐  ┌────────────┐  ┌──────────────────────┐ │ │
  │  │  │ Config API │  │ Secret API │  │ Watch API (streaming)│ │ │
  │  │  └────────────┘  └────────────┘  └──────────────────────┘ │ │
  │  └────────────────────────┬───────────────────────────────────┘ │
  │                           │                                      │
  │  ┌────────────────────────▼───────────────────────────────────┐ │
  │  │                   Core Components                           │ │
  │  │  ┌──────────┐  ┌─────────────┐  ┌───────────────────────┐ │ │
  │  │  │Version   │  │Schema       │  │Watch/Notification     │ │ │
  │  │  │Manager   │  │Validator    │  │Engine                 │ │ │
  │  │  └──────────┘  └─────────────┘  └───────────────────────┘ │ │
  │  │  ┌──────────┐  ┌─────────────┐  ┌───────────────────────┐ │ │
  │  │  │Env       │  │Template     │  │Audit Logger           │ │ │
  │  │  │Resolver  │  │Engine       │  │                       │ │ │
  │  │  └──────────┘  └─────────────┘  └───────────────────────┘ │ │
  │  └────────────────────────┬───────────────────────────────────┘ │
  │                           │                                      │
  │  ┌────────────────────────▼───────────────────────────────────┐ │
  │  │              Storage Layer                                  │ │
  │  │                                                             │ │
  │  │  ┌─────────────────────┐  ┌─────────────────────────────┐ │ │
  │  │  │ etcd (Config KV)    │  │ HashiCorp Vault (Secrets)   │ │ │
  │  │  │ Raft-replicated     │  │ Encrypted, access-controlled│ │ │
  │  │  │ Watch API           │  │ Dynamic secrets, PKI        │ │ │
  │  │  │ 3-5 nodes           │  │ 3-5 nodes (Raft)           │ │ │
  │  │  └─────────────────────┘  └─────────────────────────────┘ │ │
  │  │                                                             │ │
  │  │  ┌─────────────────────┐  ┌─────────────────────────────┐ │ │
  │  │  │ MySQL (Audit Log)   │  │ Git (Config-as-Code backup)│ │ │
  │  │  │                     │  │ Version history             │ │ │
  │  │  └─────────────────────┘  └─────────────────────────────┘ │ │
  │  └─────────────────────────────────────────────────────────────┘ │
  └──────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Config API** | CRUD operations on configuration keys; supports prefix queries, transactions |
| **Secret API** | Read/write secrets via Vault backend; token-based authentication; dynamic secret generation |
| **Watch API** | gRPC streaming endpoint; clients subscribe to key prefixes; notified on changes |
| **Version Manager** | Tracks every config change as a new version; enables rollback; stores diff |
| **Schema Validator** | Validates config values against JSON Schema / protobuf schemas before write |
| **Watch/Notification Engine** | Maintains subscriber list per key prefix; pushes events on commit |
| **Env Resolver** | Resolves environment-specific config using inheritance chain (base -> env -> service) |
| **Template Engine** | Resolves template variables (`{{.db_host}}`) at read time |
| **Audit Logger** | Records all config/secret operations to MySQL for compliance |
| **etcd** | Primary store for non-secret configuration; Raft consensus; Watch API |
| **Vault** | Secret storage; encryption at rest (AES-256-GCM with auto-unseal); dynamic secrets; PKI |
| **MySQL** | Audit log persistence; config version history |
| **Git** | Config-as-code: all config changes committed to a git repo for review/backup |

### Data Flows

**Config Read (hot path):**
1. Service SDK checks local cache (in-memory).
2. Cache miss: SDK calls Config API via gRPC.
3. Config API reads from etcd (linearizable read from leader, or stale from follower).
4. Returns value + version (Raft revision).
5. SDK caches locally; sets up watch for future changes.

**Config Write:**
1. Admin submits config change via CLI/UI.
2. Config API validates against schema.
3. Config API writes to etcd (Raft consensus).
4. On commit, Watch Engine pushes notification to subscribers.
5. Audit Logger records the change.
6. Git backup: change committed to config repo.

**Secret Read:**
1. Service SDK authenticates with Vault (k8s service account JWT or AppRole).
2. Vault validates token, checks ACL policy.
3. Vault decrypts and returns secret.
4. SDK caches secret in memory (never on disk) for configured TTL.
5. On TTL expiry, SDK re-fetches from Vault.

**Secret Rotation:**
1. Rotation trigger (automated schedule or manual).
2. Generate new credential (e.g., new MySQL password).
3. Write new credential to Vault (version N+1).
4. Old credential (version N) kept valid for grace period.
5. All services receive watch notification; fetch new secret.
6. After grace period, old credential invalidated.
7. Verify all services are using new credential.

---

## 4. Data Model

### Core Entities & Schema

```
┌─────────────────────────────────────────────────────────┐
│ ConfigEntry (etcd)                                       │
├─────────────────────────────────────────────────────────┤
│ key             STRING     PK  -- e.g. /config/prod/    │
│                                   nova-api/db_pool_size  │
│ value           BYTES          -- config value           │
│ version         INT64          -- etcd mod_revision      │
│ create_version  INT64          -- etcd create_revision   │
│ lease_id        INT64          -- optional TTL           │
│ metadata        MAP<STR,STR>   -- owner, description     │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ ConfigVersion (MySQL — history)                          │
├─────────────────────────────────────────────────────────┤
│ id              BIGINT     PK AUTO_INCREMENT             │
│ key             VARCHAR(512)                             │
│ value           MEDIUMTEXT                               │
│ version         INT64                                    │
│ previous_value  MEDIUMTEXT                               │
│ changed_by      VARCHAR(128)  -- user or automation      │
│ changed_at      TIMESTAMP                                │
│ change_reason   TEXT                                     │
│ approved_by     VARCHAR(128)  -- approval if required    │
│ INDEX(key, version)                                      │
│ INDEX(changed_at)                                        │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ SecretEntry (Vault KV v2)                                │
├─────────────────────────────────────────────────────────┤
│ path            STRING     -- e.g. secret/data/prod/    │
│                               mysql/nova-api             │
│ data            MAP<STR,STR>  -- encrypted at rest       │
│                                  {"username": "nova",    │
│                                   "password": "..."}     │
│ version         INT            -- auto-incremented       │
│ created_time    TIMESTAMP                                │
│ deletion_time   TIMESTAMP NULLABLE                       │
│ destroyed       BOOLEAN                                  │
│ metadata        MAP<STR,STR>   -- rotation_id, etc.      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ ConfigSchema                                             │
├─────────────────────────────────────────────────────────┤
│ key_pattern     VARCHAR(512)  PK  -- regex/glob          │
│ schema          JSON              -- JSON Schema          │
│ required        BOOLEAN                                   │
│ description     TEXT                                      │
│ examples        JSON                                      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ AuditLog (MySQL)                                         │
├─────────────────────────────────────────────────────────┤
│ id              BIGINT     PK AUTO_INCREMENT             │
│ timestamp       TIMESTAMP                                │
│ actor           VARCHAR(128)                             │
│ action          ENUM(READ, WRITE, DELETE, ROTATE,        │
│                      ROLLBACK)                           │
│ key_path        VARCHAR(512)                             │
│ key_type        ENUM(CONFIG, SECRET)                     │
│ source_ip       VARCHAR(64)                              │
│ success         BOOLEAN                                  │
│ error_message   TEXT NULLABLE                             │
│ INDEX(timestamp)                                         │
│ INDEX(actor, timestamp)                                  │
│ INDEX(key_path, timestamp)                               │
└─────────────────────────────────────────────────────────┘
```

### Database Selection

| Store | Use | Rationale |
|-------|-----|-----------|
| **etcd** | Non-secret configuration | Raft consensus; Watch API for hot reload; linearizable reads; k8s-native |
| **HashiCorp Vault** | Secrets | Purpose-built secret management; encryption at rest; dynamic secrets; lease/renewal; PKI; audit logging |
| **MySQL** | Audit log, config version history | Relational queries (who changed X, what changed in the last hour); long-term retention |
| **Git** | Config-as-code backup | Version control; code review for config changes; disaster recovery |

### Indexing Strategy

**etcd:**
- B+ tree indexed by key (lexicographic order).
- Range queries by prefix: `GET /config/prod/nova-api/` returns all keys under that prefix.
- Revision-based versioning: each key change increments the global revision; enables point-in-time queries.

**Vault:**
- Indexed by path (hierarchical).
- Version history stored per-path (KV v2 backend).
- No range queries (must list paths explicitly).

**MySQL:**
- Audit: `INDEX(key_path, timestamp)` for "show all changes to this key."
- History: `INDEX(key, version)` for rollback queries.

---

## 5. API Design

### gRPC Service

```protobuf
service ConfigService {
  // Config CRUD
  rpc Get(GetRequest) returns (GetResponse);
  rpc Put(PutRequest) returns (PutResponse);
  rpc Delete(DeleteRequest) returns (DeleteResponse);
  rpc List(ListRequest) returns (ListResponse);

  // Transactions
  rpc Transaction(TxnRequest) returns (TxnResponse);

  // Versioning
  rpc GetVersion(GetVersionRequest) returns (GetResponse);
  rpc Rollback(RollbackRequest) returns (PutResponse);
  rpc ListVersions(ListVersionsRequest) returns (VersionList);

  // Watch
  rpc Watch(WatchRequest) returns (stream WatchEvent);

  // Secrets
  rpc GetSecret(GetSecretRequest) returns (SecretResponse);
  rpc PutSecret(PutSecretRequest) returns (SecretResponse);
  rpc RotateSecret(RotateSecretRequest) returns (RotateResponse);
  rpc ListSecretVersions(ListSecretVersionsRequest) returns (VersionList);

  // Schema
  rpc RegisterSchema(SchemaRequest) returns (SchemaResponse);
  rpc ValidateConfig(ValidateRequest) returns (ValidateResponse);
}

message PutRequest {
  string key = 1;
  bytes value = 2;
  string environment = 3;       // prod, staging, dev
  int64 expected_version = 4;   // CAS (compare-and-swap)
  string changed_by = 5;
  string change_reason = 6;
  bool requires_approval = 7;
}

message WatchRequest {
  string key_prefix = 1;        // e.g., "/config/prod/nova-api/"
  int64 start_revision = 2;     // watch from this revision
  bool include_previous = 3;    // include previous value in event
}

message WatchEvent {
  enum Type {
    PUT = 0;
    DELETE = 1;
  }
  Type type = 1;
  string key = 2;
  bytes value = 3;
  bytes previous_value = 4;
  int64 revision = 5;
  string changed_by = 6;
}
```

### REST Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/v1/config/{key}` | Read config value |
| PUT | `/v1/config/{key}` | Write config value |
| DELETE | `/v1/config/{key}` | Delete config |
| GET | `/v1/config/{key}/versions` | List versions |
| POST | `/v1/config/{key}/rollback?version={v}` | Rollback to version |
| GET | `/v1/config?prefix={p}` | List keys by prefix |
| POST | `/v1/config/txn` | Atomic transaction |
| GET | `/v1/secrets/{path}` | Read secret (requires token) |
| POST | `/v1/secrets/{path}` | Write secret |
| POST | `/v1/secrets/{path}/rotate` | Trigger rotation |
| GET | `/v1/schemas` | List registered schemas |
| POST | `/v1/schemas` | Register schema |

### CLI

```bash
# Read config
configctl get /config/prod/nova-api/db_pool_size
# Output: 20

# Set config with reason
configctl set /config/prod/nova-api/db_pool_size 30 \
  --reason "Increasing pool for traffic spike" \
  --changed-by "ops-team"

# Set with CAS (only if current version matches)
configctl set /config/prod/nova-api/db_pool_size 30 \
  --expected-version 42

# List all config for a service
configctl list /config/prod/nova-api/
# Output:
# KEY                                    VALUE   VERSION  UPDATED
# /config/prod/nova-api/db_pool_size     20      42       2026-04-09
# /config/prod/nova-api/db_host          mysql1  38       2026-04-01
# /config/prod/nova-api/log_level        INFO    15       2026-03-15

# View version history
configctl history /config/prod/nova-api/db_pool_size --limit 5
# Output:
# VERSION  VALUE  CHANGED BY   CHANGED AT          REASON
# 42       20     ops-team     2026-04-08T10:00Z   Reverted from 30
# 41       30     ops-team     2026-04-07T14:00Z   Traffic spike
# 40       20     deploy-bot   2026-04-01T09:00Z   Initial deploy
# 39       15     perf-team    2026-03-15T11:00Z   Load test tuning
# 38       10     deploy-bot   2026-03-01T09:00Z   Service creation

# Rollback
configctl rollback /config/prod/nova-api/db_pool_size --to-version 40 \
  --reason "Reverting pool size change"

# Watch for changes (streaming)
configctl watch /config/prod/nova-api/
# [2026-04-09T10:15:30Z] PUT /config/prod/nova-api/log_level = DEBUG
# [2026-04-09T10:45:00Z] PUT /config/prod/nova-api/db_pool_size = 25

# Read secret
configctl secret get secret/prod/mysql/nova-api
# Output:
# PATH:     secret/prod/mysql/nova-api
# VERSION:  3
# DATA:
#   username: nova
#   password: ********

# Rotate secret
configctl secret rotate secret/prod/mysql/nova-api \
  --reason "Quarterly rotation"
# Output:
# Rotation started. Rotation ID: rot-abc123
# New secret version: 4
# Grace period: 300s
# Old secret (version 3) will be invalidated at 2026-04-09T10:20:00Z
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Watch-Based Hot Reload

**Why it's hard:**
Applications must react to configuration changes without restarting. This requires: (1) a reliable notification mechanism that doesn't miss changes, (2) client-side application of changes without breaking ongoing requests, (3) handling configuration changes that require careful sequencing (e.g., database connection pool resize), and (4) gracefully handling the case where a config change causes errors (automatic rollback).

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| Polling (periodic re-read) | Simple; no persistent connection | Latency = poll interval; wastes resources |
| **etcd Watch API (gRPC streaming)** | **Real-time; no missed events (revision-based)** | Requires persistent gRPC connection |
| Webhook (server pushes to client HTTP endpoint) | Client needs no persistent connection | Delivery not guaranteed; client needs webhook server |
| Message queue (Kafka topic for config changes) | Durable; replay-able | Adds Kafka dependency; overkill for low-volume changes |
| File-based (inotify on ConfigMap mount) | k8s native; no custom code | Only works for file-based config; no cross-service coordination |

**Selected: etcd Watch API with client-side SDK**

```python
class ConfigWatchClient:
    """
    Client SDK that watches etcd for configuration changes
    and hot-reloads the application.
    """

    def __init__(self, etcd_client, key_prefix, callback):
        self.client = etcd_client
        self.prefix = key_prefix
        self.callback = callback    # Application-provided reload function
        self.config_cache = {}      # key -> (value, revision)
        self.watch_thread = None
        self.last_revision = 0

    def start(self):
        """Initial load + start watching."""
        # Load all current config
        response = self.client.get_prefix(self.prefix)
        for kv in response.kvs:
            self.config_cache[kv.key] = (kv.value, kv.mod_revision)
            self.last_revision = max(self.last_revision, kv.mod_revision)

        # Notify application of initial config
        self.callback(ConfigEvent(
            type=ConfigEventType.INITIAL_LOAD,
            config=dict(self.config_cache)
        ))

        # Start watch from current revision + 1
        self.watch_thread = Thread(
            target=self._watch_loop, daemon=True)
        self.watch_thread.start()

    def _watch_loop(self):
        """
        Watch for changes starting from last known revision.
        etcd Watch guarantees no missed events: if we specify
        start_revision, etcd sends all events from that revision
        onwards, even if they happened while we were disconnected.
        """
        while True:
            try:
                watch_id = self.client.watch_prefix(
                    self.prefix,
                    start_revision=self.last_revision + 1
                )

                for event in watch_id:
                    self._handle_event(event)

            except WatchConnectionError as e:
                log.warning(f"Watch disconnected: {e}. Reconnecting...")
                time.sleep(1)
                # On reconnect, etcd sends all events since
                # last_revision. No events are missed.

            except CompactedRevisionError:
                # etcd has compacted past our revision.
                # We must do a full re-read.
                log.warning("Revision compacted. Full reload.")
                self.last_revision = 0
                self.start()  # Full reload + restart watch
                return

    def _handle_event(self, event):
        """Process a single config change event."""
        key = event.key
        self.last_revision = event.mod_revision

        if event.type == EventType.PUT:
            old_value = self.config_cache.get(key, (None, 0))[0]
            self.config_cache[key] = (event.value, event.mod_revision)

            log.info(f"Config changed: {key} "
                     f"(rev {event.mod_revision})")

            # Notify application
            self.callback(ConfigEvent(
                type=ConfigEventType.UPDATED,
                key=key,
                value=event.value,
                old_value=old_value,
                revision=event.mod_revision
            ))

        elif event.type == EventType.DELETE:
            self.config_cache.pop(key, None)
            self.callback(ConfigEvent(
                type=ConfigEventType.DELETED,
                key=key,
                revision=event.mod_revision
            ))


class JavaSpringCloudConfigIntegration:
    """
    Integration with Spring Cloud Config for Java services.
    Uses @RefreshScope to hot-reload beans.
    """

    # Spring Cloud Config Server fetches config from our etcd backend
    # Application properties are served via HTTP:
    #   GET /config/{application}/{profile}
    #
    # When etcd fires a watch event:
    # 1. Config Server receives the change.
    # 2. Config Server publishes RefreshRemoteApplicationEvent
    #    via Spring Cloud Bus (backed by Kafka/RabbitMQ).
    # 3. Each application instance receives the event.
    # 4. Spring refreshes @RefreshScope beans — e.g., DataSource
    #    reconfigured with new pool size.
    #
    # Important: @RefreshScope destroys and re-creates the bean.
    # For DataSource, this means:
    #   - New connections use new config.
    #   - In-flight queries on old connections complete normally.
    #   - Old connections are drained (HikariCP handles this).

    # Java code example:
    #
    # @RefreshScope
    # @Configuration
    # public class DatabaseConfig {
    #     @Value("${nova.db.pool.size:20}")
    #     private int poolSize;
    #
    #     @Bean
    #     public DataSource dataSource() {
    #         HikariConfig config = new HikariConfig();
    #         config.setMaximumPoolSize(poolSize);
    #         config.setJdbcUrl("jdbc:mysql://${nova.db.host}/nova");
    #         return new HikariDataSource(config);
    #     }
    # }
    #
    # When poolSize changes in etcd:
    # 1. Watch detects change.
    # 2. Spring destroys old DataSource bean.
    # 3. Spring creates new DataSource with new poolSize.
    # 4. HikariCP's old connections finish; new pool with new size.
    pass
```

**Kubernetes ConfigMap Watch:**

```python
class ConfigMapSync:
    """
    Syncs our config store to Kubernetes ConfigMaps.
    Services that prefer k8s-native config consumption (volume mounts)
    get config changes via ConfigMap updates.
    """

    def __init__(self, config_client, k8s_client):
        self.config = config_client
        self.k8s = k8s_client

    def sync_to_configmap(self, config_prefix, namespace, configmap_name):
        """
        Watch config prefix and sync to a ConfigMap.
        Pod sees the update via volume mount (kubelet sync interval ~1min)
        or via the downward API.
        """
        def on_change(event):
            # Fetch all keys under prefix
            all_config = self.config.get_prefix(config_prefix)

            # Build ConfigMap data
            data = {}
            for key, value in all_config.items():
                # Convert /config/prod/nova-api/db_pool_size -> db_pool_size
                short_key = key.split("/")[-1]
                data[short_key] = str(value)

            # Update ConfigMap
            self.k8s.patch_namespaced_config_map(
                name=configmap_name,
                namespace=namespace,
                body={"data": data}
            )
            log.info(f"Updated ConfigMap {namespace}/{configmap_name}")

        self.config.watch(config_prefix, callback=on_change)
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| etcd Watch connection drops | Missed events until reconnect | SDK reconnects and replays from last_revision; no events lost |
| etcd compaction deletes old revisions | Cannot replay from old revision | Full config reload on CompactedRevisionError |
| Config change causes application error | Service degradation | Canary config changes (apply to 1 instance first); automatic rollback on error rate spike |
| Spring @RefreshScope destroys bean during request | Request fails | Use graceful bean replacement; old bean serves in-flight requests |
| Watch notification storm (100 keys change simultaneously) | Application overwhelmed by reloads | Batch/debounce: collect changes for 100ms, apply once |

**Interviewer Q&A:**

**Q1: How does etcd Watch guarantee no missed events?**
A: etcd Watch is revision-based. Each write creates a new revision (monotonically increasing). The client specifies `start_revision` when creating a watch. etcd streams all events from that revision forward. If the client disconnects and reconnects with the same `start_revision`, etcd replays all events since then. The only case where events are lost is if etcd compacts past the client's revision — then the client must do a full re-read.

**Q2: What is the difference between etcd Watch and ZooKeeper Watcher?**
A: ZooKeeper watchers are one-shot: after firing, the watcher is removed and must be re-registered. Between the event firing and re-registration, changes can be missed. etcd Watch is a persistent stream: events are continuously delivered without re-registration. etcd's model is safer and simpler for config hot-reload.

**Q3: How do you handle config changes that require service restart?**
A: Some config changes (e.g., listening port, TLS certificate) cannot be hot-reloaded. The SDK classifies config keys as "hot-reloadable" or "requires-restart." For restart-required changes, the SDK signals the orchestrator (k8s rolling restart). Policy: prefer hot-reloadable config design. If a key requires restart, document it and trigger a rolling restart via k8s.

**Q4: How does Spring @RefreshScope work under the hood?**
A: Spring wraps @RefreshScope beans in a proxy. When a refresh event is triggered, Spring destroys the existing bean instance and creates a new one with updated @Value properties. The proxy transparently routes to the new instance. In-flight requests on the old instance may fail if the bean is destroyed mid-request. Mitigation: use bean-level locking or design for transient failures.

**Q5: What happens if a config change breaks the application?**
A: (1) Canary deployment of config: change applies to one instance first (via tag-based config: `canary=true`). (2) Health check detects degradation. (3) Automatic rollback: if error rate > threshold within 60 seconds of a config change, revert to previous version. (4) Alert oncall. This is "config change safety" — analogous to code deployment safety.

**Q6: How do you handle config for different environments (dev/staging/prod)?**
A: Hierarchical resolution with override chain: base -> environment -> service -> instance. Key structure: `/config/{env}/{service}/{key}`. If `/config/prod/nova-api/db_pool_size` exists, use it. Otherwise, fall back to `/config/default/nova-api/db_pool_size`, then `/config/default/default/db_pool_size`. This provides shared defaults with per-env and per-service overrides.

---

### Deep Dive 2: Secret Rotation (Zero-Downtime)

**Why it's hard:**
Changing a database password means: (1) the database must accept the new password, (2) all application instances must switch to the new password, and (3) the old password must eventually be revoked. If any step happens out of order, services lose database connectivity. The rotation must be atomic from the application's perspective, even though it involves multiple distributed systems.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| Stop-the-world | Simple; no dual-credential period | Downtime |
| **Dual-credential with grace period** | **Zero downtime; proven pattern** | **Complexity: two valid credentials temporarily** |
| Proxy-based (Vault Agent injects) | Service unaware of rotation | Additional infrastructure (Vault Agent sidecar) |
| Dynamic secrets (Vault database backend) | Each service gets unique, auto-rotating creds | Requires Vault database plugin; credential count explosion |

**Selected: Dual-credential with grace period + Vault backend**

```python
class SecretRotationOrchestrator:
    """
    Orchestrates zero-downtime secret rotation.
    Dual-credential pattern: both old and new credentials are valid
    during a grace period, allowing all services to transition.
    """

    def __init__(self, vault_client, config_client, db_admin_client):
        self.vault = vault_client
        self.config = config_client
        self.db = db_admin_client

    def rotate_database_credential(self, secret_path, db_host, db_user,
                                    grace_period_seconds=300):
        """
        Zero-downtime database credential rotation.

        Timeline:
        T+0:   Generate new password
        T+0:   Set new password on database (old still valid)
        T+1:   Write new credential to Vault (version N+1)
        T+1:   Watch notifications reach all services
        T+60:  Services refresh and start using new credential
        T+300: Grace period ends; revoke old password on database
        T+300: Verify all services using new credential
        """
        rotation_id = f"rot-{uuid.uuid4().hex[:8]}"
        log.info(f"Starting rotation {rotation_id} for {secret_path}")

        # Step 1: Read current secret
        current = self.vault.read(secret_path)
        old_password = current.data["password"]
        old_version = current.version

        # Step 2: Generate new password
        new_password = self._generate_secure_password()

        # Step 3: Add new password to database (both passwords now valid)
        # For MySQL: ALTER USER 'nova'@'%' IDENTIFIED BY 'new_password';
        # But we also keep the old password valid by using a second user
        # or MySQL's dual-password feature (MySQL 8.0.14+):
        self.db.execute(
            f"ALTER USER '{db_user}'@'%' "
            f"IDENTIFIED BY '{new_password}' "
            f"RETAIN CURRENT PASSWORD"
        )
        log.info(f"[{rotation_id}] Database accepts both old and new "
                 f"password for {db_user}")

        # Step 4: Write new credential to Vault
        new_version = self.vault.write(
            secret_path,
            data={
                "username": db_user,
                "password": new_password,
                "rotation_id": rotation_id,
                "rotated_at": datetime.utcnow().isoformat()
            }
        )
        log.info(f"[{rotation_id}] Vault updated to version "
                 f"{new_version}")

        # Step 5: Notify services via config watch
        # (services watching the secret path get notified automatically
        # by Vault's event system or our config watch bridge)

        # Step 6: Wait for grace period
        # During this time, services fetch the new secret and reconnect
        log.info(f"[{rotation_id}] Grace period: {grace_period_seconds}s")
        self._wait_for_services(secret_path, new_version,
                                 grace_period_seconds)

        # Step 7: Revoke old password
        self.db.execute(
            f"ALTER USER '{db_user}'@'%' DISCARD OLD PASSWORD"
        )
        log.info(f"[{rotation_id}] Old password discarded for {db_user}")

        # Step 8: Verify
        verification = self._verify_rotation(secret_path, new_version)
        if not verification.all_services_updated:
            log.error(
                f"[{rotation_id}] Not all services updated! "
                f"Stale: {verification.stale_services}"
            )
            # Alert oncall but don't rollback (old cred already revoked)
            # Stale services will fail and auto-recover by fetching new secret
        else:
            log.info(f"[{rotation_id}] Rotation complete. All services "
                     f"using version {new_version}")

        return RotationResult(
            rotation_id=rotation_id,
            new_version=new_version,
            duration_seconds=time.time() - start_time,
            services_updated=verification.updated_count,
            services_stale=verification.stale_count
        )

    def _wait_for_services(self, secret_path, expected_version,
                            grace_period):
        """
        Wait for all services to acknowledge the new secret.
        Services report their secret version via health check metadata.
        """
        deadline = time.time() + grace_period
        check_interval = 10  # seconds

        while time.time() < deadline:
            status = self._check_service_versions(
                secret_path, expected_version)

            if status.all_updated:
                log.info(f"All {status.total} services updated "
                         f"before grace period ended")
                return

            log.info(f"{status.updated}/{status.total} services "
                     f"using new version. "
                     f"{deadline - time.time():.0f}s remaining")
            time.sleep(check_interval)

        log.warning("Grace period ended; some services may not have "
                    "updated yet")

    def _generate_secure_password(self):
        """Generate a cryptographically secure password."""
        import secrets
        alphabet = (string.ascii_letters + string.digits +
                    "!@#$%^&*()_+-=")
        return ''.join(secrets.choice(alphabet) for _ in range(32))
```

**Vault Dynamic Secrets (Alternative — Per-Service Credentials):**

```python
class VaultDynamicSecretClient:
    """
    Instead of a shared credential, each service instance gets
    a unique, short-lived database credential from Vault.
    Vault creates the user in MySQL with a TTL.
    When the TTL expires, Vault deletes the user.
    """

    def __init__(self, vault_addr, role):
        self.client = hvac.Client(url=vault_addr)
        self.role = role  # e.g., "nova-api-readonly"
        self.credential = None
        self.lease_id = None
        self.lease_duration = None

    def get_credential(self):
        """
        Request a dynamic credential from Vault.
        Vault creates a temporary MySQL user with the role's permissions.
        """
        if self.credential and not self._lease_expired():
            return self.credential

        response = self.client.secrets.database.generate_credentials(
            name=self.role
        )

        self.credential = {
            "username": response["data"]["username"],
            "password": response["data"]["password"]
        }
        self.lease_id = response["lease_id"]
        self.lease_duration = response["lease_duration"]

        log.info(f"Obtained dynamic credential: "
                 f"user={self.credential['username']}, "
                 f"lease={self.lease_duration}s")

        # Schedule renewal at 2/3 of lease duration
        self._schedule_renewal()
        return self.credential

    def _schedule_renewal(self):
        """Renew the lease before it expires."""
        renew_at = self.lease_duration * 2 / 3

        def renew():
            time.sleep(renew_at)
            try:
                self.client.sys.renew_lease(
                    lease_id=self.lease_id)
                log.info(f"Renewed lease {self.lease_id}")
                self._schedule_renewal()  # Schedule next renewal
            except Exception as e:
                log.error(f"Lease renewal failed: {e}")
                # Get a new credential
                self.credential = None
                self.get_credential()

        Thread(target=renew, daemon=True).start()
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| New password set on DB but not in Vault | Services keep using old password (still valid) | Dual-password period; rollback by discarding new password on DB |
| Vault write succeeds but services don't refresh | Services use old password past grace period | Old password revoked; services fail; auto-recovery fetches new secret |
| Database rejects new password (too complex, etc.) | Rotation fails at step 3 | Validate password format before DB operation; rollback cleanly |
| Vault unavailable during rotation | Cannot write new secret | Retry with backoff; rotation is idempotent (same rotation_id) |
| Service crashes during secret refresh | Loses in-memory secret | On restart, fetches latest from Vault (always gets current version) |
| Grace period too short | Some services still using old password when revoked | Monitor service version adoption rate; extend grace period if needed |

**Interviewer Q&A:**

**Q1: What is Vault's seal/unseal mechanism?**
A: Vault stores its master encryption key in a sealed state at startup. To unseal, a threshold (e.g., 3 of 5) of key shares must be provided (Shamir's Secret Sharing). Once unsealed, Vault can decrypt its storage. Auto-unseal delegates this to an external KMS (AWS KMS, GCP KMS, Azure Key Vault) — the KMS holds the master key, and Vault auto-unseals on startup by calling the KMS. Auto-unseal is preferred for production (no human intervention on restart).

**Q2: How does Vault authenticate k8s services?**
A: Kubernetes auth method: (1) Pod has a service account. (2) Pod presents the service account JWT to Vault. (3) Vault validates the JWT by calling the k8s TokenReview API. (4) If valid, Vault maps the service account to a Vault policy (e.g., `sa:nova-api` -> policy `nova-secrets-read`). (5) Vault returns a Vault token scoped to that policy. (6) Service uses the Vault token for subsequent secret reads.

**Q3: What is the difference between Vault KV v1 and v2?**
A: KV v1: simple key-value, no versioning. A write overwrites the previous value permanently. KV v2: versioned. Each write creates a new version. You can read any previous version, soft-delete (mark as deleted but recoverable), or hard-delete (destroy). KV v2 is essential for secret rotation (keep old version during grace period) and audit (know what the secret was at any point in time).

**Q4: How do you handle secret rotation for services that can't hot-reload?**
A: (1) Rolling restart: after writing the new secret, trigger a k8s rolling restart. New pods start with the new secret. (2) Sidecar injection: Vault Agent sidecar fetches secrets and writes them to a shared volume. The application reads from the file. When the file changes, the application re-reads it (or is restarted by the sidecar). (3) Init container: fetches secrets at pod startup; restart required for new secrets.

**Q5: What is Vault's "dynamic secrets" feature and when would you use it?**
A: Dynamic secrets are generated on demand with a TTL. Example: Vault creates a unique MySQL user for each service instance with a 1-hour TTL. When the TTL expires, Vault revokes the user. Benefits: no shared credentials, automatic rotation, blast radius limited to one instance. Drawbacks: high credential count in MySQL (one user per instance), credential creation latency, dependency on Vault availability. Use for: database access, cloud credentials (AWS STS), PKI certificates.

**Q6: How do you handle emergency secret rotation (credential leak)?**
A: (1) Immediately revoke the leaked credential in the database. (2) Generate and deploy new credential via the rotation orchestrator with grace_period=0 (immediate). (3) Services will fail briefly until they refresh from Vault. (4) Alert all service owners. (5) Post-incident: audit who accessed the leaked credential, investigate the leak source, tighten access controls. The acceptable trade-off: brief service disruption is better than ongoing exposure of a leaked credential.

---

### Deep Dive 3: Configuration Validation and Rollback

**Why it's hard:**
A bad config change can take down an entire service fleet. Unlike code deployment (which has staged rollout, canary, gradual traffic shift), config changes often apply instantly to all instances simultaneously via the watch mechanism. One typo in a database hostname can disconnect hundreds of service instances.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| No validation (write anything) | Simple | Bad values propagate immediately |
| **Schema validation before write** | **Catches type errors, range violations** | Requires schema maintenance |
| Canary config (apply to subset first) | Catches runtime errors | Complexity; not all config supports partial application |
| Pre-flight dry-run | Verifies config without applying | Cannot catch all runtime issues |
| Automatic rollback on error | Recovers from bad changes | Detection delay; may oscillate |

**Selected: Schema validation + canary application + automatic rollback**

```python
class ConfigValidationPipeline:
    """
    Multi-stage validation before a config change is applied.
    """

    def __init__(self, schema_store, canary_manager, rollback_manager):
        self.schemas = schema_store
        self.canary = canary_manager
        self.rollback = rollback_manager

    def apply_change(self, key, value, changed_by, reason):
        """
        Pipeline:
        1. Schema validation
        2. Semantic validation (custom rules)
        3. Canary application (optional)
        4. Full rollout
        5. Monitoring (automatic rollback if error spike)
        """
        # Stage 1: Schema validation
        schema = self.schemas.get_schema_for_key(key)
        if schema:
            validation = self._validate_schema(value, schema)
            if not validation.valid:
                return ChangeResult(
                    success=False,
                    error=f"Schema validation failed: "
                          f"{validation.errors}"
                )

        # Stage 2: Semantic validation
        semantic_result = self._validate_semantic(key, value)
        if not semantic_result.valid:
            return ChangeResult(
                success=False,
                error=f"Semantic validation failed: "
                      f"{semantic_result.errors}"
            )

        # Stage 3: Canary (if enabled for this key)
        if self._requires_canary(key):
            canary_result = self.canary.apply_to_canary(key, value)
            if not canary_result.healthy_after_60s:
                return ChangeResult(
                    success=False,
                    error=f"Canary unhealthy: {canary_result.reason}"
                )

        # Stage 4: Apply to all
        write_result = self._write_to_etcd(key, value, changed_by, reason)

        # Stage 5: Monitor
        self.rollback.watch_for_errors(
            key=key,
            applied_version=write_result.version,
            rollback_window_seconds=120,
            error_threshold=0.05  # 5% error rate triggers rollback
        )

        return ChangeResult(success=True, version=write_result.version)

    def _validate_schema(self, value, schema):
        """JSON Schema validation."""
        import jsonschema

        try:
            parsed = json.loads(value) if isinstance(value, str) else value
            jsonschema.validate(parsed, schema)
            return ValidationResult(valid=True)
        except jsonschema.ValidationError as e:
            return ValidationResult(valid=False, errors=[str(e)])

    def _validate_semantic(self, key, value):
        """
        Custom semantic rules beyond schema.
        Examples:
        - db_pool_size must be between 1 and 500
        - db_host must be resolvable
        - log_level must be one of DEBUG/INFO/WARN/ERROR
        """
        rules = self._get_semantic_rules(key)
        errors = []

        for rule in rules:
            if not rule.check(value):
                errors.append(rule.error_message)

        return ValidationResult(valid=len(errors) == 0, errors=errors)

    def _requires_canary(self, key):
        """
        Critical config keys require canary application:
        - Database connection parameters
        - Feature flags affecting request processing
        - Resource limits (pool sizes, timeouts)
        """
        critical_patterns = [
            "*/db_*", "*/mysql_*", "*/redis_*",
            "*/pool_size", "*/timeout_*",
            "*/feature/*"
        ]
        return any(fnmatch.fnmatch(key, p) for p in critical_patterns)


class AutomaticRollback:
    """
    Monitors service health after a config change.
    Automatically rolls back if error rate exceeds threshold.
    """

    def __init__(self, config_client, metrics_client):
        self.config = config_client
        self.metrics = metrics_client

    def watch_for_errors(self, key, applied_version,
                          rollback_window_seconds, error_threshold):
        """
        Monitor error rate for rollback_window_seconds after
        a config change. If error rate exceeds threshold, rollback.
        """
        def monitor():
            baseline_error_rate = self.metrics.get_error_rate(
                service=self._key_to_service(key), window="5m")

            time.sleep(30)  # Let the change propagate

            for _ in range(rollback_window_seconds // 10):
                current_error_rate = self.metrics.get_error_rate(
                    service=self._key_to_service(key), window="1m")

                if current_error_rate > baseline_error_rate + error_threshold:
                    log.error(
                        f"Error rate spike after config change "
                        f"{key}@v{applied_version}: "
                        f"{current_error_rate:.2%} > "
                        f"{baseline_error_rate + error_threshold:.2%}. "
                        f"Rolling back."
                    )
                    self.config.rollback(key, applied_version - 1)
                    alert(f"Auto-rollback: {key} reverted to "
                          f"v{applied_version - 1}")
                    return

                time.sleep(10)

            log.info(f"Config change {key}@v{applied_version} "
                     f"stable after {rollback_window_seconds}s")

        Thread(target=monitor, daemon=True).start()
```

**Interviewer Q&A:**

**Q1: How do you implement config rollback at the etcd level?**
A: etcd stores all revisions (until compaction). To rollback key X to version V: (1) read the value at revision V (`etcdctl get X --rev=V`), (2) write that value back (`etcdctl put X <old_value>`). This creates a new revision (V_new) with the old value. It's not a true "undo" — it's a forward write with the old value. The version history shows: V1=old, V2=new, V3=old (rollback).

**Q2: How do you prevent config changes from causing cascading failures?**
A: (1) Schema validation catches type errors. (2) Canary application tests on one instance. (3) Rate limiting: only one critical config change per 5 minutes (prevent rapid fire changes). (4) Automatic rollback on error spike. (5) Blast radius control: config changes scope to one service, not the whole platform.

**Q3: How does Consul KV compare to etcd for config management?**
A: Both are Raft-based KV stores. Consul KV has: multi-datacenter support (etcd doesn't natively), ACL system, built-in UI for browsing KV. etcd has: better Watch API (gRPC streaming vs Consul's blocking queries), tighter k8s integration, simpler data model. For infrastructure config, etcd is preferred if you're already running k8s. Consul KV is preferred for multi-DC or non-k8s environments.

**Q4: What is the difference between pull-based and push-based config?**
A: Pull-based: service periodically fetches config (polling). Push-based: service subscribes and receives notifications on change (watch). Push is preferred (lower latency, less wasted traffic), but pull is a safer fallback. Our SDK uses push (etcd Watch) with pull fallback (periodic full-reload every 5 minutes in case watch silently fails).

**Q5: How do you handle config for canary deployments?**
A: Two approaches: (1) Canary-specific config key: `/config/prod/nova-api/canary/db_pool_size` overrides `/config/prod/nova-api/db_pool_size` for canary instances. (2) Config targeting: tag canary instances; the config server returns different values based on the requester's tags. We prefer (1) for simplicity.

**Q6: What is the "config drift" problem and how do you detect it?**
A: Config drift: the actual running config diverges from the intended config (e.g., a local override, a manual change, a failed reload). Detection: each service reports its active config hash via health check metadata. A reconciliation job compares active hashes against the source of truth (etcd). Drifted instances are flagged and optionally restarted.

---

## 7. Scheduling & Resource Management

### Config Management in Job Scheduling

**Scheduler Configuration:**
```
/config/prod/scheduler/
  ├── max_concurrent_jobs       = 10000
  ├── default_job_timeout       = 3600  (seconds)
  ├── retry_policy/
  │   ├── max_retries           = 3
  │   ├── backoff_base_ms       = 1000
  │   └── backoff_max_ms        = 30000
  ├── resource_pools/
  │   ├── bare_metal/
  │   │   ├── max_queue_depth   = 500
  │   │   └── provisioning_timeout = 600
  │   ├── vm/
  │   │   ├── max_queue_depth   = 5000
  │   │   └── provisioning_timeout = 300
  │   └── container/
  │       ├── max_queue_depth   = 50000
  │       └── provisioning_timeout = 60
  └── feature_flags/
      ├── enable_preemption     = true
      ├── enable_bin_packing    = true
      └── enable_spot_instances = false
```

All these values are hot-reloadable. The scheduler watches `/config/prod/scheduler/` and applies changes without restart. Changing `max_concurrent_jobs` from 10000 to 15000 takes effect within 100ms.

**Secret Management for Scheduler:**
```
/secrets/prod/scheduler/
  ├── mysql_credentials        → Vault dynamic secret
  ├── kafka_credentials        → Vault static secret
  ├── elasticsearch_credentials → Vault static secret
  └── ipmi_credentials/
      ├── rack-01              → Vault static secret (rotated quarterly)
      ├── rack-02              → Vault static secret
      └── ...
```

---

## 8. Scaling Strategy

| Dimension | Strategy | Detail |
|-----------|----------|--------|
| Read throughput | Client-side caching + watch-based invalidation | 99% of reads served from local cache; etcd only hit on cache miss or watch event |
| Write throughput | Not a bottleneck (< 100 writes/day) | Single etcd cluster handles easily |
| Key count | etcd handles millions of keys | 60K keys is well within limits |
| Watcher count | etcd supports 10K+ watchers per node | Our 10K watchers are manageable |
| Secret reads | Vault can scale to 10K reads/sec with performance replication | Vault Enterprise for high-volume secret reads |
| Multi-datacenter | Separate etcd/Vault clusters per DC | Cross-DC config sync via config-as-code (Git) |

**Interviewer Q&A:**

**Q1: At what scale does etcd become a bottleneck for config management?**
A: etcd handles ~10K writes/sec and 100K reads/sec on SSDs. Config management generates < 100 writes/day and most reads are cached. etcd becomes a bottleneck for config management only if you're using it as a general-purpose database (don't). The main risk is etcd being shared with k8s — k8s API server can saturate etcd. Solution: separate etcd cluster for config management vs k8s cluster state.

**Q2: How do you scale Vault for high-volume secret reads?**
A: (1) Performance Standby Nodes (Vault Enterprise): read replicas that serve read requests. (2) Response Wrapping: Vault returns a one-time-use token; the client unwraps locally. (3) Client-side caching: cache secrets in memory with TTL. (4) Vault Agent: sidecar that caches secrets and auto-renews leases. Most infra deployments need (3) + (4).

**Q3: How do you handle config management across multiple regions?**
A: Each region has its own etcd + Vault clusters. Config changes are authored in a central Git repo and applied to each region via CI/CD. This is the "GitOps" model: Git is the source of truth; per-region config stores are derived. Cross-region config consistency is eventual (CI/CD pipeline propagation delay, typically < 5 minutes).

**Q4: What is the performance impact of schema validation on every write?**
A: Negligible. Writes happen < 100/day. Schema validation (JSON Schema) takes < 1ms. Even if we validated every write, the total overhead is < 100ms/day. The real cost is schema maintenance — keeping schemas up to date as config keys evolve.

**Q5: How do you handle config for short-lived jobs (e.g., serverless functions)?**
A: Two options: (1) Inject config at launch time (from config store to environment variables). (2) SDK fetches config on startup (< 5ms). For serverless, option (1) is preferred (no persistent connection needed for watch). For long-running services, option (2) with watch is preferred.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO |
|---|---------|-----------|--------|----------|-----|
| 1 | etcd node crash | Raft heartbeat timeout | Cluster continues (quorum maintained) | Node restarts, catches up | 0 |
| 2 | etcd cluster down | Config reads fail | Services use cached config (stale but functional) | Restart etcd from snapshots | 5-15 min |
| 3 | Vault sealed (unsealing fails) | Health check | No secret reads | Auto-unseal via KMS; manual unseal | < 1 min (auto) |
| 4 | Vault completely down | Secret reads fail | Services use cached secrets (until TTL expires) | Restart Vault; unseal | 5-10 min |
| 5 | Bad config deployed | Error rate spike | Service degradation | Automatic rollback within 120s | < 2 min |
| 6 | Secret rotation fails midway | Rotation monitoring | Some services on old, some on new credential | Both credentials valid (dual-password); retry rotation | Grace period protects |
| 7 | Watch connection storm | etcd CPU spike | Watch notifications delayed | Client reconnect backoff; etcd rate limiting | Self-healing |
| 8 | Git config repo corruption | Git integrity check | Cannot backup or review config changes | Git backup replicas; etcd is authoritative | Minutes |

### Consensus & Coordination

**etcd Raft for Config Store:**
- 3 or 5 node cluster across AZs.
- Writes go to leader, replicated to majority.
- Reads: linearizable (leader confirms) for critical operations; serializable (any node) for cached reads.
- Compaction: etcd compacts old revisions periodically. Config version history is preserved in MySQL separately.

**Vault Raft:**
- Vault 1.4+ uses integrated Raft storage (replaces Consul backend).
- 3 or 5 Vault nodes, one active, rest standby.
- Active node handles all writes; standby nodes forward to active.
- On active failure, Raft elects new active; auto-unseal via KMS.

---

## 10. Observability

### Key Metrics

| # | Metric | Type | Alert Threshold | Why |
|---|--------|------|-----------------|-----|
| 1 | `config.read.latency.p99` | Histogram | > 10ms | Config reads should be fast (cached) |
| 2 | `config.read.cache_hit_rate` | Gauge | < 90% | Cache not effective; too many misses |
| 3 | `config.write.count` | Counter | > 50/hour | Unusual write activity |
| 4 | `config.watch.active_subscribers` | Gauge | < expected | Services not watching (stale config risk) |
| 5 | `config.watch.notification.latency` | Histogram | > 1s | Slow config propagation |
| 6 | `config.validation.failures` | Counter | > 0 | Someone trying to write invalid config |
| 7 | `config.rollback.count` | Counter | > 0 | Bad config was deployed |
| 8 | `vault.secret.read.latency.p99` | Histogram | > 50ms | Vault performance issue |
| 9 | `vault.secret.read.errors` | Counter | > 0.1% | Authentication or policy issue |
| 10 | `vault.lease.renewal.failures` | Counter | > 0 | Service will lose access when lease expires |
| 11 | `vault.seal_status` | Boolean | sealed=true | Vault is sealed; emergency |
| 12 | `secret.rotation.duration` | Histogram | > 600s | Rotation taking too long |
| 13 | `secret.rotation.failures` | Counter | > 0 | Rotation failed; manual intervention needed |
| 14 | `config.drift.instances` | Gauge | > 0 | Services with stale config |

---

## 11. Security

| Threat | Mitigation |
|--------|------------|
| Unauthorized config read | etcd RBAC: per-key-prefix permissions |
| Unauthorized secret read | Vault policies: per-path ACLs; audit every access |
| Secret leakage in logs | SDK masks secret values in logs; Vault audit log redacts secret data |
| Secret on disk | SDK stores secrets only in memory; Vault Agent uses memory-only cache |
| Encryption at rest | Vault: AES-256-GCM with master key in KMS; etcd: encryption at rest via k8s encryption config |
| Encryption in transit | mTLS for all etcd, Vault, and service communication |
| Privilege escalation (service reads another service's secrets) | Vault policies scoped per service account; k8s namespace isolation |
| Config tampering (malicious change) | Config change audit trail; approval workflow for critical keys; Git-backed review |
| Vault root token compromise | Root token revoked after initial setup; all operations via policy-scoped tokens |
| KMS compromise (auto-unseal) | KMS key rotation; IAM policies restricting KMS access; monitoring KMS audit logs |

### Vault Policy Example

```hcl
# Policy for nova-api service
path "secret/data/prod/mysql/nova-api" {
  capabilities = ["read"]     # Can read its own DB credentials
}

path "secret/data/prod/kafka/*" {
  capabilities = ["read"]     # Can read Kafka credentials
}

path "secret/data/prod/mysql/neutron" {
  capabilities = ["deny"]     # Cannot read Neutron's DB credentials
}

# PKI: request TLS certificates
path "pki/issue/nova-api" {
  capabilities = ["create", "update"]
}
```

---

## 12. Incremental Rollout Strategy

### Phase 1: Config Store (Week 1-4)
- Deploy etcd cluster for config (separate from k8s etcd).
- Migrate non-critical config from files to etcd.
- Client SDK with local cache + watch.
- Parallel: services read from both old (file) and new (etcd).

### Phase 2: Secret Management (Week 5-8)
- Deploy Vault cluster with auto-unseal.
- Migrate static secrets from environment variables / k8s Secrets to Vault.
- Client SDK for secret reads with caching.
- Vault Agent sidecar for services that can't integrate SDK.

### Phase 3: Hot Reload (Week 9-12)
- Enable watch-based hot reload for config changes.
- Spring Cloud Config integration for Java services.
- Python SDK with watch + callback.
- Schema validation for all config keys.

### Phase 4: Secret Rotation (Week 13-16)
- Implement zero-downtime rotation orchestrator.
- First rotation: dev environment MySQL credentials.
- Quarterly rotation schedule for production.
- Dynamic secrets for new services.

**Rollout Q&A:**

**Q1: How do you migrate secrets from k8s Secrets to Vault without downtime?**
A: (1) Write secrets to Vault (copy from k8s). (2) Update service to read from Vault (with fallback to k8s Secret). (3) Verify service reads from Vault (audit log). (4) Remove k8s Secret. (5) Each step is reversible. The service always has at least one source of truth.

**Q2: What if Vault is unavailable at service startup?**
A: (1) Vault Agent sidecar caches secrets to a tmpfs volume; service reads from file even if Vault is down. (2) SDK retries with backoff. (3) k8s readiness probe fails until secrets are loaded; service doesn't receive traffic until ready. (4) For emergency: environment variable fallback (less secure, but available).

**Q3: How do you validate the migration is correct?**
A: For each secret: (1) read from Vault, (2) compare with original source (k8s Secret, env var, file), (3) verify services can connect to the protected resource using the Vault-sourced credential. Automated test for every secret path.

**Q4: What is the rollback plan if Vault introduces latency?**
A: Feature flag in SDK: `secret_source = vault | k8s | env`. Flip to `k8s` to bypass Vault. All secrets remain in k8s Secrets during migration (dual-write). Rollback is instantaneous.

**Q5: How do you handle the initial Vault setup (bootstrap problem)?**
A: (1) Initialize Vault with Shamir key shares (distributed to admins). (2) Configure auto-unseal with cloud KMS. (3) Create admin policy + token. (4) Configure k8s auth method. (5) Create per-service policies. (6) Write initial secrets. All steps are automated via Terraform + Vault CLI. The only manual step is the initial key share distribution.

---

## 13. Trade-offs & Decision Log

| # | Decision | Options | Selected | Rationale |
|---|----------|---------|----------|-----------|
| 1 | Config store | etcd vs Consul KV vs ZooKeeper | etcd | k8s-native; Watch API; Raft consensus; community support |
| 2 | Secret store | Vault vs AWS Secrets Manager vs k8s Secrets | Vault | Cloud-agnostic; dynamic secrets; PKI; rotation; audit |
| 3 | Watch mechanism | etcd Watch vs polling vs webhooks | etcd Watch (gRPC stream) | Real-time; revision-based (no missed events); efficient |
| 4 | Config delivery to k8s | ConfigMap sync vs direct etcd | ConfigMap sync | k8s-native for volume-mount consumers; compatible with existing tooling |
| 5 | Secret caching | In-memory vs disk vs no cache | In-memory with TTL | Balance between availability (cache) and security (no disk persistence) |
| 6 | Config validation | Pre-write vs post-write | Pre-write (schema + semantic) | Prevent bad config from reaching services |
| 7 | Secret rotation | Dual-credential vs dynamic secrets | Dual-credential (default); dynamic secrets (opt-in) | Dual-credential is simpler and works with all databases; dynamic secrets for advanced use cases |
| 8 | Config change workflow | Direct write vs Git PR | Git PR for production; direct write for dev | Production changes reviewed; dev is self-service |
| 9 | Vault storage backend | Consul vs integrated Raft vs S3 | Integrated Raft | Simpler (no external dependency); Raft provides HA; recommended by HashiCorp since 1.4 |
| 10 | Multi-env config | Separate clusters vs namespaced keys | Separate clusters | Complete isolation between environments; no risk of prod config leak to dev |

---

## 14. Agentic AI Integration

**1. Config Anomaly Detection:**
```python
class ConfigAnomalyAgent:
    """
    Detects unusual configuration patterns and recommends fixes.
    """

    def analyze_config_change(self, key, old_value, new_value):
        # Check for common mistakes
        checks = [
            self._check_order_of_magnitude(key, old_value, new_value),
            self._check_typo_in_hostname(key, new_value),
            self._check_resource_limit_sanity(key, new_value),
            self._check_related_keys_consistency(key, new_value),
        ]

        findings = [c for c in checks if c.is_anomaly]
        if findings:
            return Alert(
                severity="WARNING",
                message=f"Config change may be problematic: "
                        f"{'; '.join(f.reason for f in findings)}",
                suggestions=[f.suggestion for f in findings]
            )

    def _check_order_of_magnitude(self, key, old_value, new_value):
        """
        Flag if a numeric value changes by more than 10x.
        """
        try:
            old_num = float(old_value)
            new_num = float(new_value)
            if old_num > 0 and (new_num / old_num > 10
                                or new_num / old_num < 0.1):
                return Finding(
                    is_anomaly=True,
                    reason=f"{key} changed from {old_num} to {new_num} "
                           f"(>{10}x change)",
                    suggestion="Verify this is intentional; "
                               "consider gradual change"
                )
        except ValueError:
            pass
        return Finding(is_anomaly=False)
```

**2. Secret Expiry Prediction:**
```
Agent tracks:
  - Secret rotation history (last N rotations per secret)
  - Rotation schedule compliance
  - Secret age vs policy (e.g., "MySQL passwords must be < 90 days old")

Alerts:
  - "Secret /prod/mysql/nova-api is 85 days old (policy: 90 days).
    Rotation due in 5 days. Triggering automated rotation."
  - "Secret /prod/api-keys/stripe has never been rotated (created 6
    months ago). Recommend immediate rotation."
```

**3. Configuration Optimization:**
```
Agent analyzes:
  - Service performance metrics
  - Current config values
  - Historical config changes and their effect on metrics

Recommendations:
  - "nova-api db_pool_size=20 but connection utilization is 95%.
    Recommend increasing to 30 (observed p99 connection wait time
    will decrease from 50ms to 5ms)."
  - "scheduler max_concurrent_jobs=10000 but peak observed is 2000.
    Reducing to 5000 would free memory for other uses."
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Why separate config from secrets?**
A: Different security requirements. Config (pool sizes, feature flags) is not sensitive; can be logged, compared, searched. Secrets (passwords, API keys) must be encrypted at rest, access-controlled, audit-logged, and never appear in logs or diffs. Using a single store forces secret-level security on all config (unnecessary overhead) or config-level openness on secrets (security risk).

**Q2: How does etcd's linearizable read work?**
A: The leader confirms it's still the leader by sending a heartbeat to a majority. Then it reads from its state machine. This guarantees the read reflects all committed writes up to that point. Cost: one additional network round-trip. For config reads that tolerate staleness, use serializable reads (any node, no confirmation) — much faster.

**Q3: What is the etcd compaction problem for config management?**
A: etcd compacts old revisions to save space. After compaction, you can't watch from a revision older than the compaction point. If a client reconnects after a long disconnection and its last_revision is compacted, it gets a CompactedRevisionError and must do a full reload. Mitigation: compact conservatively (keep 24h of history); client SDK handles full reload gracefully.

**Q4: How does Vault's transit secret engine work?**
A: Vault's transit engine provides encryption-as-a-service without storing the data. You send plaintext to Vault; it returns ciphertext encrypted with a named key. You store the ciphertext in your own database. To decrypt, send ciphertext to Vault; it returns plaintext. This centralizes key management without centralizing data storage. Use case: encrypting PII in MySQL while keeping the encryption key in Vault.

**Q5: How do you handle configuration for database migrations?**
A: Database migration config (schema changes, migration scripts) is versioned in code (Flyway/Liquibase), not in the config store. The config store holds runtime config (pool sizes, timeouts). Migration config is deployed with the application release, not changed independently. This prevents a config change from triggering an unintended migration.

**Q6: What is "configuration as code" and how does it relate to this system?**
A: Configuration as code: config values are stored in a version-controlled repository (Git), reviewed via pull requests, and applied via CI/CD. Our system supports this: config changes are committed to Git, CI validates (schema check), and CD applies to etcd. The etcd state is derived from Git; Git is the source of truth. If etcd data is lost, reconstruct from Git.

**Q7: How do you handle secrets for CI/CD pipelines?**
A: Vault's AppRole auth method: the CI/CD system (Jenkins, GitLab CI) authenticates with a role_id (not secret, can be in config) and secret_id (rotated per pipeline run, fetched from a secure bootstrap). Vault returns a short-lived token scoped to the pipeline's secrets. The token expires when the pipeline finishes. This prevents long-lived CI credentials.

**Q8: What is the "secret zero" problem?**
A: To authenticate to Vault, you need a token. But where do you store the token? If you hardcode it, it's a secret that needs managing — recursion. Solutions: (1) k8s auth: the service account JWT is the "secret zero," provided by k8s. (2) Cloud IAM: the instance's IAM role is the identity. (3) Trusted orchestrator: the deployment system injects a one-time-use wrapped token.

**Q9: How does Spring Cloud Config Server work?**
A: Config Server is a Spring Boot application that serves config via HTTP. Backend can be Git, Vault, etcd, JDBC. Client requests: `GET /{application}/{profile}` (e.g., `GET /nova-api/prod`). Server resolves the config from the backend and returns JSON. Clients use `@Value` + `@RefreshScope` for hot reload. The Bus integration (Kafka/RabbitMQ) pushes refresh events to all instances.

**Q10: How do you handle configuration for a multi-tenant platform?**
A: Tenant-specific config under a tenant-namespaced path: `/config/prod/tenants/{tenant_id}/quota_limit`. Default config under `/config/prod/default/quota_limit`. Resolution: tenant-specific overrides default. This allows per-tenant customization without duplicating all config.

**Q11: What happens if someone accidentally deletes a critical config key?**
A: (1) etcd tombstone: the delete creates a new revision; the old value is in history (until compaction). (2) Immediate alert: watch detects the delete. (3) Automatic restoration: if the key is in the "protected" list, the system automatically restores from version history. (4) Git backup: even if etcd history is lost, the Git repo has the complete history.

**Q12: How do you implement config transactions (atomic multi-key updates)?**
A: etcd supports transactions: `Txn(Compare=[key1.version == X, key2.version == Y], Success=[put(key1, v1), put(key2, v2)], Failure=[])`. This atomically checks preconditions and applies all writes or none. Use case: updating `db_host` and `db_port` together, ensuring they're always consistent.

**Q13: What is the Vault Agent and when do you use it?**
A: Vault Agent is a daemon that runs alongside the application (sidecar in k8s). It handles: (1) Auto-authentication with Vault. (2) Secret caching (in-memory). (3) Template rendering (render config files with secrets). (4) Lease renewal. Use it when: the application cannot integrate the Vault SDK (legacy app, third-party software), or you want to decouple secret management from application code.

**Q14: How do you handle configuration validation for YAML/JSON config files?**
A: (1) JSON Schema for structure validation. (2) Custom validators for semantic rules (e.g., "db_host must resolve in DNS"). (3) Pre-commit hook in Git: validate before allowing commit. (4) CI pipeline: validate before deploying. (5) Runtime validation in the config service: reject writes that fail schema validation.

**Q15: How do you handle emergency config changes (bypassing the normal review process)?**
A: "Break glass" mechanism: (1) Admin authenticates with elevated credentials (MFA required). (2) Direct write to etcd, bypassing Git review. (3) Change is logged with "EMERGENCY" flag. (4) Post-incident: the change is back-ported to Git and reviewed retroactively. (5) Alert: all oncall engineers notified of the emergency change.

**Q16: What is HashiCorp Vault's performance replication (Enterprise)?**
A: Performance replication creates read replicas across regions. The primary cluster handles all writes; replicas handle reads. Replicas are eventually consistent (lag < 1 second typically). This allows services in remote regions to read secrets locally without cross-region latency. Write operations are forwarded to the primary. This is relevant for multi-DC infrastructure deployments.

---

## 16. References

1. etcd documentation. *https://etcd.io/docs/*
2. HashiCorp Vault documentation. *https://developer.hashicorp.com/vault/docs*
3. HashiCorp Consul KV documentation. *https://developer.hashicorp.com/consul/docs/dynamic-app-config/kv*
4. Spring Cloud Config. *https://spring.io/projects/spring-cloud-config*
5. Kubernetes ConfigMaps and Secrets. *https://kubernetes.io/docs/concepts/configuration/*
6. Hashimoto, M. (2018). *Vault Architecture*. https://developer.hashicorp.com/vault/docs/internals/architecture
7. Shamir, A. (1979). *How to Share a Secret*. Communications of the ACM.
8. Ongaro, D. & Ousterhout, J. (2014). *In Search of an Understandable Consensus Algorithm* (Raft). USENIX ATC.
9. Google. *Borg, Omega, and Kubernetes*. ACM Queue, 2016.
10. Netflix. *Archaius: Dynamic Configuration Management*. https://github.com/Netflix/archaius
