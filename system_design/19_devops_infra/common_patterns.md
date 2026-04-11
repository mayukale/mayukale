# Common Patterns — DevOps Infra (19_devops_infra)

## Common Components

### PostgreSQL as Source of Truth for Configuration and Metadata
- All four systems store their authoritative state in PostgreSQL with full versioning; read paths are served by Redis cache or replicas
- cicd_pipeline: organizations, projects, pipeline_runs, stages, jobs, agents, artifacts, deployments, audit_log
- feature_flag_service: flags, flag_environments, variants, flag_rules, rule_conditions, segments, audit_events
- config_management: namespaces, config_keys, config_values, config_versions (immutable history), namespace_snapshots, access_policies
- ab_testing_platform: experiments, variants, metrics, experiment_metrics, layers, experiment_results (pre-computed snapshots), audit trail

### Kafka as Change Event Bus
- All four use Kafka to decouple the management write path from delivery/processing consumers; enables multiple independent consumers without coupling
- cicd_pipeline: `webhook-events` topic (partitioned by org_id for isolation); pipeline lifecycle events → notification dispatcher, audit processor
- feature_flag_service: `FlagChangedEvent` on every mutation → Config Snapshot Service + SSE Server + audit processor
- config_management: `ConfigChangedEvent` partitioned by org_id × namespace → Snapshot Builder + SSE Server + K8s ConfigMap Sync Operator
- ab_testing_platform: `exp-events` topic (partitioned by user_id for session-level metric locality) → ClickHouse consumer (micro-batch every 5 s)

### Redis for Hot-Path Caching and Distributed Locking
- All four use Redis for the read fast-path and for atomic operations that would be expensive in PostgreSQL
- cicd_pipeline: job priority queue (`ZSET queue:jobs:org:{org_id}` with Lua atomic dequeue); assigned jobs hash; agent heartbeat tracking
- feature_flag_service: per-org per-environment flag snapshot (`snapshot:{org_id}:{env_id}` MessagePack serialized); ETag for conditional GET; SSE connection routing
- config_management: namespace snapshots (small ones in Redis, large ones in S3 keyed by namespace_snapshots.storage_key); ETag-based polling; `304 Not Modified` on no change
- ab_testing_platform: experiment assignment cache; active experiment snapshots for in-process SDK evaluation

### SSE (Server-Sent Events) for Real-Time Change Propagation
- All four push changes to connected SDK instances or internal consumers via SSE or long-poll; avoids polling overhead
- feature_flag_service: SSE Server maintains per-org per-environment SSE connections; sends delta payload (only changed flags, not full snapshot) on `FlagChangedEvent`; 50 K persistent SSE connections
- config_management: SSE Server organizes connections by org × namespace × environment; sends `{key, value, version}` delta on `ConfigChangedEvent`; 100 K SSE connections; on reconnect, full snapshot sync to catch missed deltas
- cicd_pipeline: notification dispatcher sends job status via WebSocket/SSE to developer browsers; not the same protocol but same event-driven pattern
- ab_testing_platform: not SSE-based for flag delivery (delegates to Feature Flag Service); results dashboard served from pre-computed PostgreSQL snapshots refreshed every 5 min

### ETag-Based Polling Fallback (304 Not Modified)
- feature_flag_service and config_management both support ETag-based polling for SDKs that cannot maintain persistent SSE connections (mobile, embedded, legacy)
- Mechanism: SDK sends `If-None-Match: {etag}` header; server returns `304 Not Modified` if snapshot unchanged (ETag = SHA-256 of snapshot content); returns `200` with new snapshot + new ETag if changed
- Guarantees no delta is missed (full snapshot on 200 response); polling interval can be aggressive (10 s) because 304 is cheap

### Immutable Append-Only Audit Log
- All four maintain an immutable audit log of all mutations with actor, timestamp, before/after state, and change reason
- cicd_pipeline: `audit_log` table (append-only; no UPDATE/DELETE on this table); S3 Object Lock for archive; fields: actor_id, actor_type (USER/CI_BOT/SYSTEM), action, details JSONB, ip_address
- feature_flag_service: `audit_events` table; fields: event_type (flag.enabled, flag.rule.updated, etc.), before_state JSONB, after_state JSONB, change_reason TEXT
- config_management: `audit_events` table; fields: config_key, environment, before_value (masked if is_sensitive), after_value, changed_by
- ab_testing_platform: experiment state machine transitions logged; guardrail trigger events logged; all auto-stop decisions with statistical justification

### MurmurHash3 for Deterministic User Bucketing
- feature_flag_service and ab_testing_platform both use MurmurHash3 for user bucket assignment; same hash function, same formula, enabling the A/B platform to delegate assignment to the Feature Flag SDK
- `bucket = MurmurHash3(seed.flag_key.user_key) % 10000` (feature_flag_service)
- `bucket = MurmurHash3(experiment_id + "." + user_id) % 10000` (ab_testing_platform)
- Properties: (a) deterministic — same user always gets same bucket; (b) independent across experiments via experiment_id/flag_key salt; (c) sub-microsecond computation; (d) no network call or DB lookup

### ClickHouse for Event Analytics
- ab_testing_platform: `experiment_exposures` and `experiment_events` tables in ClickHouse; columnar MergeTree storage; partitioned for fast experiment-level aggregation; Statistical Computation Engine queries ClickHouse every 5 min for p-value computation

## Common Databases

### PostgreSQL
- All four; source of truth for configurations, metadata, audit trails; ACID transactions; immutable version history via append-only tables; read path offloaded to Redis cache

### Redis
- All four; hot-path cache for snapshots, job queues, distributed locks, rate limiters; ETag storage for conditional GET; atomic Lua scripts for lock-free operations

### Kafka
- All four; change event bus; ordered per partition; RF=3; decouples management write path from delivery consumers

### ClickHouse
- ab_testing_platform; raw exposure and metric event storage; columnar aggregation for statistical computation; partitioned by experiment_id × user_id

### S3
- cicd_pipeline: log archives, artifact storage, deployment manifest snapshots
- config_management: large namespace snapshots (> Redis limit), pipeline config GitOps versions
- ab_testing_platform: audit log archives for compliance

## Common Communication Patterns

### Management API → Kafka → Consumer Chain
- All four: Management API validates and writes to PostgreSQL, then publishes a change event to Kafka; downstream consumers (snapshot builder, SSE server, audit processor) react independently; failure in any consumer doesn't block the management write

### In-Process SDK Evaluation (No Network Per Request)
- feature_flag_service: server-side SDK downloads full ruleset at startup; evaluates in-process < 0.1 ms (no network); SSE connection provides push updates
- ab_testing_platform: delegates user bucketing to feature_flag_service SDK; evaluation is in-process; exposure events batched and flushed to Kafka every 5 s

## Common Scalability Techniques

### Snapshot-Based Distribution (Full Snapshot on Init, Delta on Change)
- feature_flag_service: SDK downloads full 50 MB MessagePack snapshot on startup; SSE delivers deltas thereafter; reconnection triggers full snapshot re-fetch to prevent drift
- config_management: SDK downloads full namespace snapshot (1 MB typical) on startup; SSE delivers key-value deltas; local disk cache provides startup resilience when config service is temporarily unavailable

### Priority Queue with Aging for Job Scheduling
- cicd_pipeline: job priority score = `urgency_level × 1000 - queued_duration_seconds`; aging prevents starvation of low-priority jobs; Lua atomic pop guarantees no double-dispatch

## Common Deep Dive Questions

### Why not evaluate feature flags via a network call per request?
Answer: At 1 M RPS, a 5–50 ms network call per evaluation = 5,000–50,000 ms of cumulative per-second latency overhead per application instance — unacceptable. In-process evaluation using a locally cached ruleset drops this to < 0.1 ms. The tradeoff is that changes take a few seconds to propagate (SSE latency), not zero. For business-critical kill switches, this < 5 s propagation is acceptable. For applications that absolutely need instant reflection, they can poll at high frequency (10 s) as a secondary guarantee.
Present in: feature_flag_service, ab_testing_platform

### Why use MurmurHash3 for bucketing instead of a DB lookup?
Answer: DB lookups would make bucketing an O(1) DB read per request, adding 1–5 ms latency and creating a hot-path dependency on the database. MurmurHash3 is computed locally in < 1 μs, is deterministic (same user always gets the same bucket), and is independent across experiments via the salt (experiment_id). The distribution is uniform and unpredictable enough to be effectively random. Stripe, LaunchDarkly, and Statsig all use hash-based bucketing for the same reasons.
Present in: feature_flag_service, ab_testing_platform

## Common NFRs

- **Flag/config evaluation latency**: < 0.1 ms in-process (server-side SDK)
- **Change propagation latency**: < 5 s end-to-end via SSE (change committed → all connected SDKs updated)
- **Availability**: 99.99% for SDK evaluation (continues from in-process cache if service is down); 99.9% for management API
- **Audit retention**: all changes logged immutably; 7-year retention for compliance
- **Throughput**: 500 K builds/day (cicd); 1 M+ flag evaluations/s (feature flags); 100 K SSE connections sustained
