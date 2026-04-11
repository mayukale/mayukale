# Common Patterns — Security & Access (12_security_and_access)

## Common Components

### Envoy + ext_authz Filter as Security Enforcement Point
- All 5 systems use Envoy proxy as the on-the-wire enforcement point; ext_authz calls out to a validation service before forwarding the request
- api_key_management: Envoy ext_authz → Key Validation Service; SHA-256 hash lookup in Redis cache; < 1 ms cached, < 5 ms uncached
- rbac_authorization_system: Envoy ext_authz → Authorization Gateway (OPA embedded); Rego policy evaluation; < 1 ms in-process
- zero_trust_network_design: Envoy sidecar enforces mTLS + Istio AuthorizationPolicy; no separate ext_authz call — cert chain validated at TLS handshake
- certificate_lifecycle_management: Envoy validates X.509 cert chain (leaf → intermediate → root) as part of mTLS; no runtime authz call
- secrets_management_system: Vault Agent sidecar handles auth before pod starts; Vault API calls use TLS

### Cache-First Validation (Sub-millisecond at Hot Path)
- 4 of 5 systems serve hot-path decisions from cache; cold path hits authoritative store only on miss
- api_key_management: Redis hash → key metadata + permissions; TTL-invalidated on revocation via pub/sub (< 5 s)
- rbac_authorization_system: in-process LRU (100K entries, 60s TTL) + Redis cluster for cross-node consistency
- zero_trust_network_design: Cilium eBPF map lookup at kernel level — < 0.1 ms per packet; identity-based (label-derived numeric ID, not IP)
- secrets_management_system: Vault Agent sidecar caches secrets in-memory with TTL-aware invalidation; reduces Vault server calls by 10–100×
- certificate_lifecycle_management: no application-layer cache; validation in Envoy kernel TLS path

### Deny-by-Default Security Model
- All 5 systems explicitly default to deny; no implicit permissions; every request must carry a valid credential
- api_key_management: unrecognized key hash → 401; expired key → 401; no key → 401
- rbac_authorization_system: `default allow := false` in Rego; deny rules override allow rules
- secrets_management_system: no valid auth method → 403 Forbidden; every path requires explicit policy capability
- certificate_lifecycle_management: mTLS mutual authentication required; unknown/expired cert → TLS handshake failure
- zero_trust_network_design: CiliumNetworkPolicy default-deny on all pods; every flow must match an explicit ALLOW rule

### Audit Logging with Zero-Loss Guarantee
- All 5 systems log every authorization decision (allow AND deny); audit stream must survive infrastructure failures
- Shared pipeline: event → Kafka (at-least-once, replication 3) → Elasticsearch 8.x hot (90-day) → S3 cold (7-year)
- api_key_management: every key validation, lifecycle event, leak detection event
- rbac_authorization_system: every OPA decision with evaluation trace; partitioned by hour
- secrets_management_system: every secret read, auth event, lease operation via Vault audit backend
- certificate_lifecycle_management: every issuance/renewal/revocation with serial number
- zero_trust_network_design: every network flow verdict, policy decision; `zt_policy_decisions` partitioned by hour

### Kubernetes Integration (Admission Webhooks + CRDs)
- All 5 systems have first-class Kubernetes integration
- certificate_lifecycle_management: cert-manager Certificate CRD → Issuer → k8s Secret; validating webhook for CRD correctness
- rbac_authorization_system: OPA/Gatekeeper admission webhooks; Kubernetes Role/RoleBinding sync controller
- secrets_management_system: Vault Secrets Operator syncs Vault paths → k8s Secret via CRDs; Vault Agent injector webhook
- zero_trust_network_design: CiliumNetworkPolicy CRDs; Istio VirtualService/DestinationRule/AuthorizationPolicy CRDs
- api_key_management: Kubernetes ServiceAccount tokens used for initial auth; API key issued per service

### Policy Propagation via Kafka CDC (< 5 s End-to-End)
- 4 of 5 systems propagate policy/configuration changes via Kafka CDC; avoids polling and cache staleness
- rbac_authorization_system: Debezium CDC from MySQL roles/permissions → Kafka → OPA bundle update on all sidecars
- api_key_management: revocation events fanout via Redis pub/sub and Kafka `key.revocations` topic; all gateway nodes invalidate cache within < 5 s
- secrets_management_system: Vault lease expiry → revocation events → downstream consumers via Kafka
- zero_trust_network_design: CiliumNetworkPolicy changes propagate via Kubernetes API watch → Cilium agent on each node; < 5 s cluster-wide
- certificate_lifecycle_management: uses cert-manager Watch on Certificate CRDs; no Kafka

## Common Databases

### MySQL 8.0 (Primary Metadata Store)
- All 5 systems; stores identity metadata, permissions, audit history, lifecycle state; ACID guarantees; semi-sync replication

### Redis Cluster (Cache + Pub/Sub)
- 4 of 5 (not secrets_management which uses Vault Raft internal storage); sub-millisecond decision cache; pub/sub for revocation fanout; Lua scripts for atomic rate limit operations

### Elasticsearch 8.x (Audit Search)
- 4 of 5 (not certificate_lifecycle_management which uses Vault built-in audit); security audit log search; 90-day hot retention; time-partitioned indices

### Kafka (Event Bus + CDC)
- 4 of 5 (not certificate_lifecycle_management); policy change propagation; revocation events; audit stream; exactly-once via idempotent producer + transaction

### Vault Raft Integrated Storage
- secrets_management_system only (central secret store); 5-node cluster; 3-of-5 quorum; Raft consensus; 256-bit master key protected by HSM unseal

## Common Communication Patterns

### Sidecar Pattern (Vault Agent / SPIRE Agent / Cilium Agent / Envoy)
- certificate_lifecycle_management: SPIRE Agent DaemonSet — delivers SVIDs via Unix domain socket Workload API; cert rotation at 67% TTL
- secrets_management_system: Vault Agent sidecar — auto-auth, in-memory cache, template rendering into pod filesystem
- zero_trust_network_design: Cilium Agent DaemonSet (eBPF enforcement); Envoy sidecar per pod (mTLS)
- Key property: sidecars absorb request volume from central service; provide local caching and fallback

### HSM-Protected Key Material
- certificate_lifecycle_management: Root CA private key in air-gapped HSM; intermediate CAs in Vault HSM-backed PKI
- secrets_management_system: Production auto-unseal via PKCS#11 HSM; non-prod via Transit seal (separate Vault cluster)

## Common Scalability Techniques

### In-Process Cache + Distributed Cache (Two-Tier)
- rbac_authorization_system: LRU 100K entries (in-process, 0 ms) → Redis cluster (< 1 ms) → MySQL (< 5 ms)
- api_key_management: Redis cluster only (no per-process LRU); < 1 ms

### eBPF for Line-Rate Kernel Enforcement
- zero_trust_network_design: Cilium identity model — numeric IDs derived from pod labels (not IP); eBPF map lookups at kernel path; < 0.1 ms overhead; 10M+ policy evals/sec

### Async Usage Aggregation (Decouple Hot Path from Analytics)
- api_key_management: usage events → Kafka `key.usage.events` → Usage Aggregator (per key/minute metrics); hot path never waits for analytics write
- secrets_management_system: audit backend → syslog → Kafka; Vault server never blocked by audit write latency

## Common Deep Dive Questions

### How do you propagate revocations to all edge nodes within seconds without a distributed transaction?
Answer: Kafka pub/sub fanout: (1) Revocation event written to Kafka topic with replication=3; (2) All gateway/proxy nodes subscribe and invalidate their local Redis cache entry immediately on message receipt; (3) If Redis cache is not yet invalidated (window < 5 s), the invalidated TTL-based entry expires; (4) Worst case: key works for TTL duration (e.g., 30 s) after revocation — explicitly documented in SLA. For immediate revocation (key compromise), force-delete from Redis via pub/sub; for routine rotation, TTL expiry is sufficient.
Present in: api_key_management, rbac_authorization_system, secrets_management_system

### How do you provide sub-millisecond authorization without querying a database on every request?
Answer: Two-tier cache architecture: (1) In-process LRU (OPA: 100K entries, 0 ms; API Key: per-process Redis client with pipelining); (2) Redis cluster (< 1 ms); (3) MySQL authoritative store on cache miss only. Cache invalidation via Kafka CDC + pub/sub ensures changes propagate within < 5 s. Cache key must be stable and not include mutable context (e.g., timestamp). TTL is set conservatively (60 s for RBAC, 30 s for API keys) to bound inconsistency window.
Present in: api_key_management, rbac_authorization_system

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.999% (RBAC), 99.99% (API Key, Certs, Secrets, Zero Trust data plane) |
| **Latency (cached)** | < 0.1 ms eBPF (ZT), < 1 ms (API Key, RBAC), < 1 ms Vault Agent (Secrets) |
| **Latency (uncached)** | < 5 ms (API Key uncached, RBAC cold), < 50 ms Vault server, < 100 ms SPIRE |
| **Throughput** | 10M policy evals/sec (Cilium eBPF), 500K decisions/sec (RBAC), 100K validations/sec (API Key), 10K reads/sec (Vault), 5K SVID/sec (SPIRE) |
| **Propagation** | < 5 s (revocations, policy changes); < 30 s (Vault seal/unseal) |
| **Audit** | 100% coverage on all systems; 90-day hot (Elasticsearch), 7-year cold (S3) |
| **Scale** | 500 nodes, 50K pods, 1M+ mTLS connections, 50K roles/200K bindings, 100K secret paths |
