# System Design: API Gateway

> **Relevance to role:** An API gateway is the front door to every internal and external service in a cloud infrastructure platform. For a bare-metal IaaS / Kubernetes role, you must understand how to route millions of requests per second across heterogeneous backends (Java, Python, gRPC), enforce authentication at the edge, protect upstream services via rate limiting and circuit breaking, and integrate with service meshes. This is a first-class infrastructure component that sits on the critical path of every API call.

---

## 1. Requirement Clarifications

### Functional Requirements

| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Request routing | Path-based, host-based, header-based, method-based routing to upstream clusters |
| FR-2 | Authentication & authorization | JWT validation, API key lookup, OAuth2 token introspection, mTLS client cert |
| FR-3 | Rate limiting | Per-consumer, per-IP, per-route, per-API-key with configurable windows |
| FR-4 | SSL/TLS termination | Terminate TLS at the gateway, optionally re-encrypt to upstream (mTLS) |
| FR-5 | Request/response transformation | Header injection/removal, body rewriting, protocol transcoding (REST to gRPC) |
| FR-6 | Circuit breaking | Track upstream error rates, open circuit on threshold, half-open probe |
| FR-7 | Observability | Access logs, distributed tracing (OpenTelemetry), metrics (latency histograms, error rates) |
| FR-8 | gRPC support | Native gRPC proxying, gRPC-web transcoding for browser clients |
| FR-9 | WebSocket support | Upgrade HTTP to WebSocket, maintain long-lived connections |
| FR-10 | Plugin/middleware architecture | Custom request processing via plugin chain |

### Non-Functional Requirements

| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Latency overhead | < 2 ms p99 added latency for passthrough routing |
| NFR-2 | Throughput | 500K+ RPS per gateway cluster |
| NFR-3 | Availability | 99.999% (< 5.26 min downtime/year) |
| NFR-4 | Zero-downtime config reload | Route/plugin changes without dropping connections |
| NFR-5 | Horizontal scalability | Linear throughput scaling with added instances |
| NFR-6 | Multi-tenancy | Isolate configuration and rate limits per tenant/team |

### Constraints & Assumptions

- Infrastructure runs on bare-metal servers with Kubernetes orchestration.
- Upstream services are a mix of Java (Spring Boot), Python (FastAPI/Flask), and gRPC services.
- Existing DNS resolves external traffic to gateway VIPs via BGP Anycast or MetalLB.
- Configuration is stored in a centralized control plane (etcd or PostgreSQL) and pushed to data plane nodes.
- TLS certificates are managed by an internal PKI (e.g., Vault + cert-manager).

### Out of Scope

- CDN / edge caching (separate system).
- WAF deep packet inspection (handled by a dedicated WAF layer in front of the gateway).
- API lifecycle management / developer portal (separate product concern).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|------------|-------|
| Total internal services | Given | 2,000 microservices |
| External API consumers | Given | 50,000 registered API keys |
| Peak external RPS | 50K consumers x 10 req/s avg | 500,000 RPS |
| Peak internal (east-west via gateway) | 2,000 services x 200 RPS avg | 400,000 RPS |
| Total peak RPS | External + internal | 900,000 RPS |
| Avg request size | Headers + small JSON body | 2 KB |
| Avg response size | JSON payload | 5 KB |
| Connections per gateway node | 100K+ concurrent (HTTP/2 multiplexing) | 100,000 |

### Latency Requirements

| Path | Target |
|------|--------|
| Gateway passthrough (no auth) | < 1 ms p50, < 2 ms p99 |
| With JWT validation | < 3 ms p50, < 5 ms p99 |
| With rate limit check (local) | < 1.5 ms p50, < 3 ms p99 |
| With rate limit check (distributed, Redis) | < 5 ms p50, < 10 ms p99 |
| gRPC-web transcoding | < 3 ms p50, < 8 ms p99 |

### Storage Estimates

| Data | Calculation | Size |
|------|------------|------|
| Route configurations | 2,000 services x 10 routes avg x 2 KB | 40 MB |
| API key store | 50,000 keys x 512 bytes | 25 MB |
| Rate limit counters (Redis) | 50,000 consumers x 100 routes x 64 bytes | 320 MB |
| Access logs (1 day) | 900K RPS x 86,400s x 500 bytes | ~38 TB/day |
| Metrics (Prometheus TSDB, 15s scrape) | 50 gateway nodes x 5,000 series x 8 bytes x 5,760 scrapes/day | ~11 GB/day |

### Bandwidth Estimates

| Direction | Calculation | Value |
|-----------|------------|-------|
| Ingress (client to gateway) | 900K RPS x 2 KB | 1.8 GB/s (14.4 Gbps) |
| Egress (gateway to client) | 900K RPS x 5 KB | 4.5 GB/s (36 Gbps) |
| Gateway to upstream | 900K RPS x 2 KB (forwarded) | 1.8 GB/s |
| Upstream to gateway | 900K RPS x 5 KB (response) | 4.5 GB/s |

---

## 3. High Level Architecture

```
                                    ┌─────────────────────────────────────────┐
                                    │           CONTROL PLANE                 │
                                    │                                         │
                                    │  ┌─────────┐  ┌──────────┐  ┌───────┐  │
                                    │  │  Admin   │  │  Config  │  │ Route │  │
                                    │  │   API    │  │  Store   │  │Compiler│  │
                                    │  │(REST/gRPC)│ │(etcd/PG) │  │       │  │
                                    │  └────┬─────┘  └────┬─────┘  └───┬───┘  │
                                    │       │             │            │       │
                                    │       └─────────────┼────────────┘       │
                                    │                     │ xDS / push         │
                                    └─────────────────────┼───────────────────┘
                                                          │
               ┌──────────────────────────────────────────┼────────────────────┐
               │                                          │                    │
     Clients   │              DATA PLANE (N nodes)        │                    │
    ─────────► │  ┌──────────────────────────────────────────────────────┐     │
    (HTTPS/    │  │              GATEWAY NODE                            │     │
     gRPC/     │  │                                                      │     │
     WS)       │  │  ┌─────────┐  ┌──────────────────────────────────┐  │     │
               │  │  │  TLS    │  │         FILTER CHAIN             │  │     │
               │  │  │Termina- │─►│ Auth → RateLimit → Transform →  │  │     │
               │  │  │  tion   │  │ CircuitBreak → Router → Upstream │  │     │
               │  │  └─────────┘  └──────────────────────────────────┘  │     │
               │  │       │                    │              │          │     │
               │  │       ▼                    ▼              ▼          │     │
               │  │  ┌─────────┐       ┌────────────┐  ┌──────────┐    │     │
               │  │  │  Cert   │       │   Redis    │  │ Upstream │    │     │
               │  │  │  Store  │       │ (Rate Lim) │  │ Clusters │    │     │
               │  │  │ (SDS)   │       └────────────┘  └──────────┘    │     │
               │  └──────────────────────────────────────────────────────┘     │
               │                                                               │
               │  ┌──────────┐  ┌──────────┐  ┌───────────────────┐           │
               │  │ Metrics  │  │ Tracing  │  │   Access Logs     │           │
               │  │(Prometheus)│ │(OTel/    │  │ (stdout → Fluentd │           │
               │  │          │  │ Jaeger)  │  │  → Elasticsearch) │           │
               │  └──────────┘  └──────────┘  └───────────────────┘           │
               └───────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| Admin API | REST/gRPC interface for managing routes, plugins, consumers, certificates |
| Config Store (etcd/PostgreSQL) | Persistent storage for gateway configuration; source of truth |
| Route Compiler | Translates high-level route configs into data-plane-native config (e.g., Envoy xDS snapshots) |
| TLS Termination | Decrypts inbound TLS using SNI-based certificate selection |
| Filter Chain | Ordered pipeline of request/response processing: auth, rate limiting, transformation, circuit breaking |
| Redis (Rate Limiting) | Distributed counter store for global rate limit enforcement |
| Upstream Clusters | Backend service groups with health checking, load balancing, and circuit breaking |
| Cert Store (SDS) | Secret Discovery Service for dynamic certificate rotation |
| Observability Stack | Prometheus metrics, OpenTelemetry tracing, structured access logs |

### Data Flows

**External request flow:**
1. Client sends HTTPS request to gateway VIP (BGP Anycast / MetalLB).
2. Gateway performs TLS termination using SNI to select the correct certificate.
3. Request enters the filter chain: authentication (JWT/API key) -> rate limit check -> request transformation -> circuit breaker check.
4. Router matches request to upstream cluster using path/host/header rules.
5. Gateway selects backend via load balancing algorithm (least connections, consistent hash).
6. Request is forwarded (optionally re-encrypted with mTLS to upstream).
7. Response traverses the filter chain in reverse (response transformation, metrics recording).
8. Response is sent back to client.

**Configuration update flow:**
1. Admin updates route via Admin API.
2. Config Store is updated atomically.
3. Route Compiler generates new xDS snapshot with incremented version.
4. Data plane nodes receive delta update via xDS streaming gRPC.
5. Hot reload applies new routing rules without dropping existing connections.

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Routes define how requests are matched and forwarded
CREATE TABLE routes (
    id              UUID PRIMARY KEY,
    name            VARCHAR(255) NOT NULL UNIQUE,
    host            VARCHAR(255),           -- Host header match
    path_prefix     VARCHAR(1024),          -- Path prefix match
    path_regex      VARCHAR(1024),          -- Path regex match
    methods         VARCHAR(255)[],         -- GET, POST, etc.
    headers         JSONB,                  -- Header match rules
    upstream_id     UUID NOT NULL REFERENCES upstreams(id),
    strip_prefix    BOOLEAN DEFAULT false,
    priority        INT DEFAULT 0,          -- Higher wins on conflict
    enabled         BOOLEAN DEFAULT true,
    created_at      TIMESTAMPTZ DEFAULT NOW(),
    updated_at      TIMESTAMPTZ DEFAULT NOW(),
    version         BIGINT DEFAULT 1
);

-- Upstreams represent backend service clusters
CREATE TABLE upstreams (
    id              UUID PRIMARY KEY,
    name            VARCHAR(255) NOT NULL UNIQUE,
    algorithm       VARCHAR(50) DEFAULT 'round_robin',  -- round_robin, least_conn, consistent_hash
    hash_on         VARCHAR(50),            -- header, cookie, ip for consistent hashing
    health_check    JSONB,                  -- {active: {path, interval, threshold}, passive: {error_rate}}
    connect_timeout INT DEFAULT 5000,       -- ms
    read_timeout    INT DEFAULT 30000,      -- ms
    write_timeout   INT DEFAULT 30000,      -- ms
    retries         INT DEFAULT 2,
    circuit_breaker JSONB,                  -- {max_connections, max_pending, max_requests, max_retries}
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

-- Targets are individual backend instances
CREATE TABLE targets (
    id              UUID PRIMARY KEY,
    upstream_id     UUID NOT NULL REFERENCES upstreams(id),
    host            VARCHAR(255) NOT NULL,
    port            INT NOT NULL,
    weight          INT DEFAULT 100,
    health_status   VARCHAR(20) DEFAULT 'healthy',
    metadata        JSONB,                  -- zone, rack, etc.
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

-- Consumers represent API clients (human or service)
CREATE TABLE consumers (
    id              UUID PRIMARY KEY,
    username        VARCHAR(255) UNIQUE,
    custom_id       VARCHAR(255) UNIQUE,    -- External system reference
    tags            VARCHAR(255)[],
    rate_limit      JSONB,                  -- {requests_per_second: 100, burst: 200}
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

-- API keys for authentication
CREATE TABLE api_keys (
    id              UUID PRIMARY KEY,
    consumer_id     UUID NOT NULL REFERENCES consumers(id),
    key_hash        VARCHAR(64) NOT NULL,   -- SHA-256 hash of the key
    key_prefix      VARCHAR(8) NOT NULL,    -- First 8 chars for lookup
    scopes          VARCHAR(255)[],
    expires_at      TIMESTAMPTZ,
    enabled         BOOLEAN DEFAULT true,
    created_at      TIMESTAMPTZ DEFAULT NOW()
);
CREATE INDEX idx_api_keys_prefix ON api_keys(key_prefix);

-- Plugins/middleware configuration per route or global
CREATE TABLE plugins (
    id              UUID PRIMARY KEY,
    name            VARCHAR(255) NOT NULL,  -- rate-limit, jwt-auth, cors, etc.
    route_id        UUID REFERENCES routes(id),       -- NULL = global
    consumer_id     UUID REFERENCES consumers(id),    -- NULL = all consumers
    config          JSONB NOT NULL,
    enabled         BOOLEAN DEFAULT true,
    priority        INT DEFAULT 0,
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

-- TLS certificates
CREATE TABLE certificates (
    id              UUID PRIMARY KEY,
    sni             VARCHAR(255)[] NOT NULL, -- Server Name Indication hostnames
    cert_pem        TEXT NOT NULL,
    key_pem_encrypted TEXT NOT NULL,          -- Encrypted with KMS
    expires_at      TIMESTAMPTZ NOT NULL,
    created_at      TIMESTAMPTZ DEFAULT NOW()
);
CREATE INDEX idx_certificates_sni ON certificates USING GIN(sni);
```

### Database Selection

| Store | Technology | Rationale |
|-------|-----------|-----------|
| Configuration (routes, upstreams, plugins) | PostgreSQL 16 | ACID transactions, JSONB for flexible plugin configs, robust replication |
| Configuration cache (data plane) | etcd | Already present in Kubernetes; used for xDS snapshot versioning and leader election |
| Rate limit counters | Redis Cluster (6+ nodes) | Sub-millisecond reads/writes, atomic INCR with TTL, cluster mode for sharding |
| API key cache | Local in-memory (LRU) + Redis | Hot keys cached locally; Redis as shared invalidation layer |
| Access logs | Elasticsearch (via Fluentd) | Full-text search, time-series indexing, Kibana dashboards |
| Metrics | Prometheus + Thanos | Time-series, PromQL, long-term storage via Thanos |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| routes | `(host, path_prefix)` composite | Fast route matching |
| routes | `priority DESC` | Priority-ordered route evaluation |
| api_keys | `key_prefix` B-tree | Fast API key prefix lookup |
| certificates | `sni` GIN | SNI-based cert selection |
| plugins | `(route_id, consumer_id)` composite | Plugin chain assembly per request |
| targets | `(upstream_id, health_status)` | Healthy target selection |

---

## 5. API Design

### Management APIs

```
# Routes
POST   /api/v1/routes                      → Create route
GET    /api/v1/routes                      → List routes (paginated)
GET    /api/v1/routes/{id}                 → Get route
PUT    /api/v1/routes/{id}                 → Update route
DELETE /api/v1/routes/{id}                 → Delete route

# Upstreams
POST   /api/v1/upstreams                   → Create upstream
GET    /api/v1/upstreams/{id}              → Get upstream
PUT    /api/v1/upstreams/{id}              → Update upstream
GET    /api/v1/upstreams/{id}/health       → Get upstream health status
POST   /api/v1/upstreams/{id}/targets      → Add target to upstream
DELETE /api/v1/upstreams/{id}/targets/{tid} → Remove target

# Consumers
POST   /api/v1/consumers                   → Create consumer
GET    /api/v1/consumers/{id}              → Get consumer
POST   /api/v1/consumers/{id}/api-keys     → Generate API key
DELETE /api/v1/consumers/{id}/api-keys/{kid} → Revoke API key

# Plugins
POST   /api/v1/routes/{id}/plugins         → Attach plugin to route
POST   /api/v1/plugins                     → Create global plugin
PUT    /api/v1/plugins/{id}                → Update plugin config
DELETE /api/v1/plugins/{id}                → Remove plugin

# Certificates
POST   /api/v1/certificates                → Upload certificate
GET    /api/v1/certificates                → List certificates
DELETE /api/v1/certificates/{id}           → Remove certificate

# Status / Debug
GET    /api/v1/status                      → Gateway cluster health
GET    /api/v1/config/dump                 → Full running config dump
POST   /api/v1/config/validate             → Validate config without applying
```

### Data Plane Behavior

```
Request Processing Pipeline:

Client Request
     │
     ▼
┌─────────────┐
│ TLS Handler │ ← SNI-based cert selection
└──────┬──────┘
       │
       ▼
┌─────────────┐
│ HTTP Parser │ ← HTTP/1.1, HTTP/2, gRPC detection
└──────┬──────┘
       │
       ▼
┌─────────────────────────┐
│    Route Matcher         │ ← Trie-based path matching + host + headers
│    (O(log n) lookup)     │
└──────┬──────────────────┘
       │
       ▼
┌─────────────────────────┐
│    Plugin Chain          │ ← Ordered by priority
│    1. IP Restriction     │
│    2. Authentication     │ ← JWT / API Key / OAuth2
│    3. Rate Limiting      │ ← Token bucket (local) or sliding window (Redis)
│    4. Request Transform  │ ← Header inject, body rewrite
│    5. Circuit Breaker    │ ← Check upstream health state
│    6. CORS               │
└──────┬──────────────────┘
       │
       ▼
┌─────────────────────────┐
│    Load Balancer         │ ← Per-upstream algorithm selection
│    + Health-aware        │
└──────┬──────────────────┘
       │
       ▼
┌─────────────────────────┐
│    Upstream Connection   │ ← Connection pooling, optional mTLS
│    + Retry Logic         │
└──────┬──────────────────┘
       │
       ▼
┌─────────────────────────┐
│  Response Pipeline       │
│  1. Response Transform   │
│  2. Metrics Recording    │
│  3. Access Log Emit      │
│  4. Trace Span Close     │
└─────────────────────────┘
```

**gRPC-Web Transcoding Detail:**
- Browser sends `Content-Type: application/grpc-web+proto` over HTTP/1.1.
- Gateway decodes gRPC-web framing (base64 or binary).
- Re-frames as native gRPC (HTTP/2) to upstream.
- Response path reverses the transcoding.
- Supports unary and server-streaming; client-streaming/bidi requires WebSocket transport.

---

## 6. Core Component Deep Dives

### Deep Dive 1: Route Matching Engine

**Why it's hard:**
A gateway with 10,000+ routes must match every incoming request in microseconds. Naive linear scanning is O(n). Routes can overlap (path prefix vs. exact, regex vs. literal). Priority ordering, host-based virtual hosting, and header-based routing create a complex multi-dimensional matching problem.

**Approaches:**

| Approach | Latency | Memory | Flexibility | Complexity |
|----------|---------|--------|-------------|------------|
| Linear scan | O(n) per request | Low | High (any match logic) | Low |
| Trie (radix tree) | O(k) where k = path depth | Medium | Good for path prefix | Medium |
| Hash map (exact match) | O(1) amortized | Medium | Only exact paths | Low |
| Compiled regex table | O(n) worst case | Low | Highest | High |
| Two-phase: host hash + path trie | O(1) + O(k) | Medium | Good balance | Medium |

**Selected approach: Two-phase (host hash + radix trie) with regex fallback**

**Justification:** Most routes are host + path-prefix, which this handles in O(1) + O(k). The small subset of regex routes falls through to a compiled regex table. This matches Envoy's internal route matching architecture.

**Implementation detail:**

```
Phase 1: Host Resolution
  ┌──────────────────────┐
  │  HashMap<Host, Trie> │   host → dedicated path trie
  │  "*" → default trie  │   wildcard host as fallback
  └──────────┬───────────┘
             │
Phase 2: Path Matching (within host's trie)
  ┌──────────────────────────────────────────┐
  │  Radix Trie                               │
  │  /api/v1/users → Route A                  │
  │  /api/v1/users/{id} → Route B (parameterized) │
  │  /api/v2/* → Route C (prefix)             │
  └──────────┬───────────────────────────────┘
             │
Phase 3: Header/Method Refinement
  ┌──────────────────────┐
  │ Filter matched routes │
  │ by method + headers   │
  │ Select highest prio   │
  └───────────────────────┘
             │
Phase 4: Regex Fallback (only if no trie match)
  ┌──────────────────────┐
  │ Iterate regex routes  │
  │ in priority order     │
  └───────────────────────┘
```

**Failure modes:**
- **Route conflict:** Two routes with same host/path/method → resolved by priority field; if equal, last-updated wins with a warning log.
- **Regex catastrophic backtracking:** Regex routes must pass RE2 safety check (no backtracking) at config time.
- **Config push race:** xDS versioning ensures atomic config snapshots; partial updates are impossible.

**Interviewer Q&As:**

> **Q1: How do you handle route updates without dropping in-flight requests?**
> A: We use Envoy's drain-based approach. The new route table is installed atomically, but connections already matched to the old route complete on the old config. New connections use the new route table. The data plane maintains two route tables briefly during transition.

> **Q2: What happens if two routes have overlapping path prefixes?**
> A: Longer prefix wins (most specific match). If lengths are equal, the route with higher priority value wins. If priorities are also equal, we log a warning and use the route that was created first (stable sort by created_at).

> **Q3: How would you support A/B routing (send 10% of traffic to v2)?**
> A: Use weighted route rules. The route matcher returns a weighted cluster, and the load balancer selects the target cluster based on consistent hashing of a request attribute (e.g., user ID header) to ensure sticky assignment. This is equivalent to Envoy's `weighted_clusters` in route configuration.

> **Q4: How does the route matching engine handle thousands of regex routes?**
> A: We strongly discourage regex routes at scale. If needed, we compile them into a Hyperscan (Intel) multi-pattern matcher that evaluates all patterns in a single pass over the input string, giving O(input_length) regardless of pattern count. But the real answer is: redesign your URL scheme to use prefix matching.

> **Q5: How do you validate a new route config won't break existing traffic?**
> A: The Admin API's `/config/validate` endpoint compiles the new route table and runs it against a shadow request corpus (sampled from production access logs). It reports any routes that would change matching behavior. We also support canary config pushes (push to 1 gateway node first, verify metrics, then roll to all).

> **Q6: Can the route matcher support gRPC service/method routing?**
> A: Yes. gRPC requests use HTTP/2 with path format `/<package.Service>/<Method>`. The trie handles this natively since it is just a path. We also support matching on the `grpc-service` header for gRPC-web.

---

### Deep Dive 2: Distributed Rate Limiting

**Why it's hard:**
Rate limiting must be globally consistent (a consumer's limit applies across all gateway nodes), low latency (cannot add 50 ms to every request), and accurate under high concurrency. Local-only rate limiting allows burst-through proportional to the number of gateway nodes. Distributed rate limiting requires a shared counter store, introducing a network hop on every request.

**Approaches:**

| Approach | Accuracy | Latency | Complexity | Failure Mode |
|----------|----------|---------|------------|-------------|
| Local token bucket per node | Low (N x limit) | Sub-microsecond | Low | Over-admits by Nx |
| Centralized Redis (fixed window) | High | 1-5 ms (Redis RTT) | Medium | Redis failure = no limiting |
| Centralized Redis (sliding window log) | Highest | 2-8 ms | High | Memory-heavy at high RPS |
| Centralized Redis (sliding window counter) | High | 1-5 ms | Medium | Small inaccuracy at window boundaries |
| Hybrid: local + periodic sync | Medium-High | Sub-ms local, async sync | Medium | Brief over-admission during sync gaps |

**Selected approach: Hybrid with sliding window counters in Redis**

**Justification:** Use a local token bucket for first-pass filtering (catches obvious over-limit). For requests that pass local check, do an async Redis sliding window counter check. If Redis is unavailable, fall back to local-only with degraded accuracy. This gives sub-millisecond latency for most requests while maintaining global accuracy.

**Implementation detail:**

```
Request arrives
     │
     ▼
┌─────────────────────┐
│ Local Token Bucket   │ ← Pre-allocated tokens = global_limit / num_nodes
│ (per consumer/route) │    Refill rate = global_rate / num_nodes
└──────┬──────────────┘
       │
       ├── DENIED (local bucket empty) → Return 429
       │
       ▼ ALLOWED locally
┌─────────────────────┐
│ Redis INCR + EXPIRE  │ ← Sliding window counter
│ Key: rl:{consumer}:  │    Two keys: current window + previous window
│      {route}:{window}│    Score = prev_count * overlap% + current_count
└──────┬──────────────┘
       │
       ├── DENIED (global count > limit) → Return 429 + update local bucket
       │
       ▼ ALLOWED globally
   Forward to upstream

Redis Sliding Window Counter Algorithm:
  current_window = floor(now / window_size)
  previous_window = current_window - 1
  elapsed = now - current_window * window_size
  overlap = 1 - (elapsed / window_size)
  
  effective_count = redis.GET(prev_window) * overlap + redis.INCR(curr_window)
  
  if effective_count > limit:
      return RATE_LIMITED
```

**Redis command (atomic Lua script):**
```lua
local curr_key = KEYS[1]
local prev_key = KEYS[2]
local limit = tonumber(ARGV[1])
local window = tonumber(ARGV[2])
local now = tonumber(ARGV[3])
local curr_window = math.floor(now / window)
local elapsed = now - curr_window * window
local overlap = 1 - (elapsed / window)

local prev_count = tonumber(redis.call('GET', prev_key) or 0)
local curr_count = redis.call('INCRBY', curr_key, 1)
redis.call('EXPIRE', curr_key, window * 2)

local effective = math.floor(prev_count * overlap) + curr_count
if effective > limit then
    redis.call('DECRBY', curr_key, 1)
    return {0, effective, limit}  -- denied
end
return {1, effective, limit}  -- allowed
```

**Failure modes:**
- **Redis down:** Fall back to local token bucket. Global accuracy degrades but requests are not blocked unnecessarily (fail-open). Alert on Redis health.
- **Redis latency spike:** If Redis check takes > 10 ms, skip it (circuit breaker on Redis itself) and use local bucket.
- **Clock skew between nodes:** Use Redis server time (`TIME` command) for window calculation, not local clock.
- **Hot key (one consumer generating extreme RPS):** Redis cluster shards by key; a single hot consumer hits one shard. Use local rejection (local bucket will be exhausted first) before hitting Redis.

**Interviewer Q&As:**

> **Q1: How do you handle rate limit quota allocation when gateway nodes scale up or down?**
> A: Local token bucket allocation is recalculated on node join/leave events (detected via Kubernetes endpoint changes or gossip protocol). During transition, we briefly over-allocate (sum of old + new allocations) for one window period. The Redis global counter is the true limit, so no actual over-admission occurs.

> **Q2: What rate limiting headers do you return to clients?**
> A: We return standard headers: `X-RateLimit-Limit` (total), `X-RateLimit-Remaining` (remaining in window), `X-RateLimit-Reset` (Unix timestamp of window reset), and `Retry-After` (seconds until retry is safe) on 429 responses.

> **Q3: How do you implement per-consumer AND per-route rate limits simultaneously?**
> A: Multiple rate limit dimensions are checked independently. A request must pass ALL applicable limits. Evaluation order: global limit, per-consumer limit, per-route limit, per-consumer-per-route limit. We short-circuit on the first denial.

> **Q4: How would you implement a distributed rate limiter without Redis?**
> A: Option 1: Use a gossip-based counter (like Netflix's Concurrency Limits library) where nodes periodically share their local counts. Option 2: Use a Raft-based consensus store (etcd) but this adds more latency. Option 3: Accept local-only with over-admission and use it as a soft limit, enforcing hard limits at the application layer.

> **Q5: How do you prevent a burst that arrives at multiple nodes simultaneously from exceeding the limit?**
> A: The Redis atomic Lua script serializes counter increments. At extreme concurrency, Redis processes ~100K operations/second per key. For limits above this, we accept brief over-admission (bounded by num_nodes x local_bucket_capacity) as an acceptable trade-off for latency.

> **Q6: How do you handle rate limit state during gateway node restarts?**
> A: Local token bucket state is lost on restart, but it refills conservatively (starts at 0 tokens, not full). Redis state persists across node restarts since it is external. The new node acquires its share of the local quota from the control plane.

---

### Deep Dive 3: Authentication Pipeline

**Why it's hard:**
The gateway must validate every request's identity in < 5 ms while supporting multiple auth mechanisms (JWT, API keys, OAuth2 tokens, mTLS). JWT validation requires JWKS key fetching and rotation. API key lookup must not become a bottleneck. OAuth2 token introspection requires calling an external IdP, adding latency. All auth must be configurable per-route.

**Approaches:**

| Approach | Latency | Security | Complexity | Offline Capability |
|----------|---------|----------|------------|-------------------|
| JWT local validation | < 1 ms | Good (if keys rotated) | Low | Full (no external call) |
| API key hash lookup (Redis) | 1-3 ms | Good | Low | No (needs Redis) |
| OAuth2 token introspection | 5-50 ms | Highest (real-time revocation) | High | No (needs IdP) |
| mTLS client certificate | 0 ms (TLS handshake) | Highest | Medium | Full |
| JWT + short-lived + revocation list | < 2 ms | High | Medium | Mostly (bloom filter for revoked) |

**Selected approach: Multi-strategy with JWT as primary, API keys for programmatic access, mTLS for service-to-service**

**Implementation detail:**

```
Request
  │
  ▼
┌───────────────────────────┐
│ Auth Strategy Selector     │
│ (based on route config)    │
└──────┬────────────────────┘
       │
       ├── Authorization: Bearer <JWT>
       │   └─► JWT Validator
       │       ├── Decode header (alg, kid)
       │       ├── Fetch JWKS from cache (or refresh if kid unknown)
       │       ├── Verify signature (RS256/ES256)
       │       ├── Check exp, nbf, iss, aud claims
       │       ├── Check revocation bloom filter
       │       └── Extract claims → set X-User-Id, X-Roles headers
       │
       ├── X-API-Key: <key>
       │   └─► API Key Validator
       │       ├── Extract prefix (first 8 chars)
       │       ├── Lookup in local LRU cache
       │       ├── If miss: lookup in Redis by prefix → get hash
       │       ├── SHA-256(full_key) == stored_hash?
       │       ├── Check scopes, expiry, enabled
       │       └── Resolve consumer → set X-Consumer-Id header
       │
       ├── Client certificate present
       │   └─► mTLS Validator
       │       ├── TLS handshake already verified cert chain
       │       ├── Extract SPIFFE ID from SAN
       │       ├── Map SPIFFE ID → service identity
       │       └── Set X-Service-Id header
       │
       └── OAuth2 opaque token
           └─► Token Introspection
               ├── POST to IdP /introspect endpoint
               ├── Cache response for token TTL (max 60s)
               ├── Check active=true, scope, exp
               └── Extract sub → set X-User-Id header
```

**Failure modes:**
- **JWKS endpoint down:** Use cached JWKS (cache for hours, not minutes). If cache is cold AND JWKS is down, reject with 503 (not 401) to distinguish infra failure from auth failure.
- **Bloom filter false positive on revocation:** Follow up with Redis lookup for definitive revocation check. False positive rate tuned to < 0.1%.
- **API key cache poisoning:** Cache entries are keyed by hash(key), not the key itself. TTL of 60 seconds limits exposure window of a revoked key.

**Interviewer Q&As:**

> **Q1: How do you handle JWT key rotation without downtime?**
> A: JWKS contains multiple keys identified by `kid`. The IdP publishes the new key before signing tokens with it. The gateway fetches JWKS periodically (every 5 minutes) and on `kid` cache miss. Old keys remain valid until all tokens signed with them expire. This gives a graceful rotation window.

> **Q2: How do you handle token revocation for JWTs (which are stateless)?**
> A: We maintain a bloom filter of revoked JWT IDs (jti claims). The revocation service publishes revoked JTIs to a Kafka topic. Each gateway node consumes the topic and updates its local bloom filter. On bloom filter positive, we do a definitive check against Redis. JWTs have short lifetimes (15 minutes) to bound the revocation window.

> **Q3: How do you support multiple auth methods on the same route?**
> A: Routes can be configured with `auth_strategies: ["jwt", "api_key"]` and a mode: `any` (first successful wins) or `all` (all must pass). The `any` mode tries strategies in order and short-circuits on success. The `all` mode is used for defense-in-depth (e.g., mTLS + JWT).

> **Q4: What do you do if the OAuth2 introspection endpoint is slow?**
> A: We set a strict timeout (200 ms) on introspection calls. On timeout, we check the local cache for a previous introspection result for this token. If no cache hit, we return 503 with `Retry-After` header. We also circuit-break the introspection endpoint: if error rate exceeds 50% over 30 seconds, we stop calling it and fail open/closed based on policy.

> **Q5: How do you prevent API key enumeration attacks?**
> A: API keys are 256-bit random values. We rate limit failed authentication attempts per source IP (10 failures/minute). We store only SHA-256 hashes. The prefix index (first 8 chars) is for performance, not security: an attacker knowing the prefix still has 2^224 possibilities.

> **Q6: How do you propagate auth context to upstream services?**
> A: The gateway sets trusted headers (`X-User-Id`, `X-Roles`, `X-Scopes`, `X-Consumer-Id`). Upstream services are configured to trust these headers ONLY when the request comes from the gateway (verified via mTLS identity or a shared HMAC). The gateway strips any client-supplied values for these headers.

---

### Deep Dive 4: Plugin/Middleware Architecture

**Why it's hard:**
A plugin system must be extensible (teams add custom logic), safe (a buggy plugin cannot crash the gateway), performant (minimal overhead per plugin in the chain), and deterministic (execution order matters). Supporting both compiled plugins (for performance) and scripted plugins (for flexibility) adds complexity.

**Approaches:**

| Approach | Performance | Safety | Dev Experience | Examples |
|----------|-------------|--------|---------------|----------|
| Lua scripts (OpenResty) | Good (LuaJIT ~30K RPS/core) | Sandboxed | Fast iteration | Kong |
| WebAssembly (Wasm) | Good (~80% of native) | Memory-safe sandbox | Moderate | Envoy Wasm, proxy-wasm |
| Native C/C++ filters | Best | No sandboxing | Slow iteration | Envoy native filters |
| Go plugins (shared objects) | Good | No sandbox | Good | Traefik |
| External gRPC callout | Variable (network hop) | Full isolation | Best | Envoy ext_proc |

**Selected approach: Wasm for custom plugins + native filters for core functionality + ext_proc for complex cases**

**Justification:** Wasm gives near-native performance with memory safety sandboxing. Core filters (auth, rate limiting) are native C++ for maximum performance. Complex/infrequent logic (e.g., custom billing metering) uses ext_proc to avoid gateway restarts.

**Implementation detail:**

```
Plugin Chain Execution:

┌──────────────────────────────────────────────────┐
│                   Request Phase                    │
│                                                    │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐        │
│  │ Native   │  │  Wasm    │  │ ext_proc │        │
│  │ Plugin 1 │─►│ Plugin 2 │─►│ Plugin 3 │─► ...  │
│  │ (auth)   │  │ (custom) │  │ (billing)│        │
│  └──────────┘  └──────────┘  └──────────┘        │
│                                                    │
│  Each plugin can:                                  │
│  - Read/modify headers                             │
│  - Read/modify body                                │
│  - Short-circuit (return response immediately)     │
│  - Add metadata for downstream plugins             │
│  - Pause execution (for async ext_proc call)       │
└──────────────────────────────────────────────────┘

Wasm Runtime per worker thread:
  ┌─────────────────────┐
  │   Wasm VM Instance  │ ← One per worker thread (no sharing)
  │   ┌──────────────┐  │
  │   │ Plugin Code   │  │ ← Compiled .wasm module
  │   │ Linear Memory │  │ ← Isolated 4 GB address space
  │   │ Host Bindings │  │ ← proxy_wasm ABI for header access, logging
  │   └──────────────┘  │
  └─────────────────────┘
```

**Failure modes:**
- **Wasm plugin panic:** The VM instance is destroyed and recreated. The request gets a 500. No impact on other requests.
- **Wasm plugin infinite loop:** CPU time limit per invocation (default 5 ms). Exceeding it kills the VM instance.
- **ext_proc service down:** Circuit breaker opens after 5 consecutive failures. Plugin is bypassed with a configurable policy (fail-open or fail-closed).
- **Plugin ordering conflict:** Priorities are explicit integers. Conflicts are caught at config validation time.

**Interviewer Q&As:**

> **Q1: How do you hot-reload a Wasm plugin without restarting the gateway?**
> A: Wasm modules are loaded from a content-addressable store. When a new version is pushed, the control plane sends an update via xDS. Each worker thread instantiates the new module for new requests while existing requests complete on the old module. Old module is garbage collected once all references are released.

> **Q2: How do you limit the blast radius of a buggy plugin?**
> A: Three layers: (1) Wasm sandbox prevents memory corruption. (2) CPU time limit (5 ms default) prevents infinite loops. (3) Memory limit (32 MB default) prevents OOM. Additionally, plugins can be canary-deployed to a subset of gateway nodes first.

> **Q3: How do plugins communicate state between request and response phases?**
> A: Plugins store per-request metadata in a shared context map (string key-value pairs). For example, the auth plugin sets `consumer_id` in the context, and the rate limit plugin reads it. This context is thread-local and request-scoped.

> **Q4: Can you support Lua plugins for backward compatibility with Kong?**
> A: Yes. We can compile Lua to Wasm using the `wasmoon` project, or run a LuaJIT interpreter within a Wasm module. Performance is ~60% of native Lua, which is acceptable for most use cases. Alternatively, we support the proxy-wasm ABI which allows Lua, Rust, Go, and AssemblyScript plugins.

> **Q5: What is the performance overhead of the Wasm plugin chain?**
> A: Per-plugin overhead is ~20-50 microseconds for a simple header manipulation. A chain of 5 Wasm plugins adds ~200 microseconds. This is 10x better than ext_proc (which adds 1-2 ms per plugin due to gRPC call overhead) and ~3x worse than native C++ filters.

> **Q6: How do you handle plugins that need to make async external calls (e.g., lookup a blocklist)?**
> A: Wasm plugins can yield execution and resume when the async call completes (using proxy-wasm async HTTP call API). The worker thread moves on to other requests. When the response arrives, the original request is resumed. This is similar to Envoy's async filter model.

---

## 7. Scaling Strategy

**Horizontal scaling:**
- Gateway nodes are stateless (all state in Redis/PostgreSQL/etcd). Add nodes behind the L4 load balancer (IPVS or MetalLB).
- Target: 20,000 RPS per gateway node (Envoy-based). 50 nodes = 1M RPS.
- Autoscale based on CPU utilization (target 60%) or connection count.

**Configuration scaling:**
- xDS delta updates (only changed routes pushed) reduce config push overhead from O(total_routes) to O(changed_routes).
- Shard route tables by domain/host for very large deployments (> 100K routes).

**Rate limiting scaling:**
- Redis Cluster with 6+ shards, hash slot distribution by consumer key.
- For extreme throughput (> 1M rate limit checks/s), use Redis pipelining and batch checks.

**TLS scaling:**
- TLS handshake is CPU-intensive. Use hardware acceleration (AES-NI, QAT) on bare-metal.
- TLS session tickets reduce repeat handshake cost by 10x.
- Offload to dedicated TLS termination tier for > 100K new TLS connections/second.

**Interviewer Q&As:**

> **Q1: How do you handle a 10x traffic spike during a product launch?**
> A: Pre-scale the gateway fleet (Kubernetes HPA with custom metrics). Redis rate limit counters handle increased load. Circuit breakers protect upstream services that cannot scale as fast. We also enable response caching at the gateway for cacheable endpoints.

> **Q2: What is the bottleneck as you scale the gateway fleet?**
> A: The config push plane (xDS) becomes the bottleneck at > 500 nodes. Each node maintains a gRPC stream to the control plane. Solution: shard the control plane by assigning node groups to different xDS server instances. Envoy supports ADS (Aggregated Discovery Service) federation.

> **Q3: How do you handle scaling across multiple data centers?**
> A: Each data center has its own gateway fleet with local Redis for rate limiting. A global control plane replicates configuration to all data centers. Rate limits are either per-DC (simpler) or global (requires cross-DC Redis sync via CRDTs or periodic reconciliation).

> **Q4: How do you scale WebSocket connections (which are long-lived)?**
> A: WebSocket connections pin to a specific gateway node for their lifetime. This creates uneven load distribution. We use connection draining: when scaling down, mark a node as draining; it stops accepting new connections but keeps existing ones alive until they close or a timeout expires.

> **Q5: What metrics do you monitor to know when to scale?**
> A: Primary: request rate (RPS), p99 latency, active connection count, CPU utilization. Secondary: error rate (5xx), rate limit rejection rate, upstream circuit breaker trip rate, TLS handshake rate. Alert on: p99 > 2x baseline, CPU > 70%, connection count > 80% of limit.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Detection | Impact | Mitigation | RTO |
|---------|-----------|--------|------------|-----|
| Single gateway node crash | L4 LB health check (TCP SYN every 3s) | Connections on that node dropped | L4 LB removes node; clients retry (HTTP retries) | < 10s |
| All gateway nodes in AZ | AZ-level BGP health monitoring | AZ traffic blackholed | Anycast withdraws AZ routes; traffic shifts to other AZs | < 30s (BGP convergence) |
| Redis cluster down | Sentinel/Cluster health checks | Rate limiting degrades to local-only | Local token bucket fallback; alert ops | Degraded until Redis recovers |
| PostgreSQL config DB down | Replication lag monitor, connection pool errors | No config changes possible; existing config still works | Read replicas for read path; promote replica for write | < 60s (auto-failover) |
| etcd cluster lost quorum | etcd health endpoint, leader election failure | xDS updates stall; data plane config frozen | Data plane continues on last-known config; restore etcd from backup | Config frozen until quorum restored |
| Control plane crash | Liveness probe, xDS stream disconnect | No config updates; data plane unaffected | Kubernetes restarts control plane pod; xDS reconnects automatically | < 30s |
| TLS certificate expiry | cert-manager renewal + Nagios expiry check | TLS handshake failures for affected SNIs | Auto-renewal 30 days before expiry; alert at 14 days | Immediate (if renewed) |
| Upstream service down | Active + passive health checks | Requests to that upstream fail | Circuit breaker opens; return 503 with retry-after; upstream removed from pool | Automatic (health check interval) |
| DNS resolution failure | DNS query timeout monitoring | New connections to DNS-named upstreams fail | Cache DNS results aggressively (30s min TTL); use IP-based upstreams as fallback | Cache TTL duration |
| Plugin/Wasm crash | Per-request error tracking | Single request fails with 500 | Wasm VM re-instantiated; plugin can be disabled via control plane | Per-request (no global impact) |
| Network partition (control/data) | xDS stream heartbeat timeout (30s) | Data plane config stale | Data plane operates on last-known-good config indefinitely | Config stale until healed |

---

## 9. Security

**TLS & Encryption:**
- TLS 1.3 by default; TLS 1.2 with strong cipher suites as fallback.
- HSTS headers with `max-age=31536000; includeSubDomains; preload`.
- Perfect Forward Secrecy (PFS) using ECDHE key exchange.
- Certificate pinning for critical internal services.
- mTLS between gateway and upstream services using SPIFFE/SPIRE identities.

**Input Validation:**
- Maximum header size: 8 KB per header, 64 KB total headers.
- Maximum URI length: 8 KB.
- Maximum request body: configurable per route (default 10 MB).
- HTTP request smuggling prevention: strict HTTP parsing mode (reject ambiguous requests).
- Reject HTTP/0.9 and HTTP/1.0 without `Host` header.

**DDoS Protection:**
- Connection rate limiting per source IP (max 100 new connections/second).
- Slow-read/slow-write protection: minimum data rate per connection (1 KB/s).
- SYN flood protection via SYN cookies (kernel-level).
- IP blocklist/allowlist with hot-reload capability.

**Secrets Management:**
- TLS private keys encrypted at rest with KMS; decrypted only in memory.
- API keys stored as SHA-256 hashes (never plaintext).
- Redis connections authenticated with ACL (Redis 6+).
- Admin API requires mTLS + RBAC (role-based access control).

**Audit & Compliance:**
- All admin API calls logged with caller identity, action, and timestamp.
- Configuration change log (who changed what, when, with diff).
- PCI DSS: tokenize/mask credit card numbers in access logs.
- SOC2: access logs retained for 90 days minimum.

---

## 10. Incremental Rollout

**Phase 1 (Weeks 1-4): Foundation**
- Deploy Envoy-based gateway in parallel with existing infrastructure (shadow mode).
- Mirror 1% of production traffic; compare responses with existing gateway.
- Establish baseline metrics (latency, error rate, throughput).

**Phase 2 (Weeks 5-8): Internal Traffic**
- Route internal (east-west) traffic through the new gateway.
- Enable JWT auth for internal services.
- Deploy Redis cluster for distributed rate limiting.

**Phase 3 (Weeks 9-12): External Traffic Migration**
- Migrate external traffic route by route (lowest-risk routes first).
- Enable API key auth and rate limiting for external consumers.
- Run old and new gateways in parallel with DNS-based traffic splitting.

**Phase 4 (Weeks 13-16): Full Production**
- Complete migration of all routes.
- Decommission old gateway infrastructure.
- Enable advanced features (gRPC-web transcoding, Wasm plugins).

**Phase 5 (Ongoing): Optimization**
- Performance tuning (connection pooling, TLS session tickets).
- Plugin marketplace for teams.
- Self-service route management via GitOps.

**Rollout Q&As:**

> **Q1: How do you validate the new gateway matches the old one's behavior?**
> A: Shadow/mirror mode: send a copy of every request to the new gateway, compare response status codes and headers (not bodies, which may vary). Log discrepancies. Require < 0.01% mismatch rate before proceeding.

> **Q2: How do you handle rollback if the new gateway has issues?**
> A: DNS-based traffic splitting allows instant rollback (update DNS weight to 0 for new gateway). For routes already migrated, the old gateway config is preserved (frozen, not deleted) for 30 days. Rollback is a config push, not a deployment.

> **Q3: How do you migrate rate limit state from the old gateway to the new one?**
> A: Rate limit state is ephemeral (counters reset each window). During migration, both gateways share the same Redis cluster. Rate limit keys are prefixed with a version identifier so old and new gateways do not interfere.

> **Q4: How do you ensure zero downtime during the TLS certificate migration?**
> A: Both old and new gateways serve the same certificates (from the same Vault/cert-manager). SNI routing ensures the correct certificate is selected regardless of which gateway handles the request. We verify certificate serving with automated TLS probes before shifting traffic.

> **Q5: How do you handle teams that have custom plugins on the old gateway?**
> A: We provide a migration toolkit that transpiles old plugin formats to proxy-wasm (Wasm). For complex plugins, we offer the ext_proc escape hatch (run old plugin as a gRPC service). We set a 6-month deprecation timeline for the old plugin format.

> **Q6: How do you measure success of each rollout phase?**
> A: Key metrics per phase: (1) p99 latency delta vs. old gateway (must be < 10% regression). (2) Error rate delta (must be < 0.01% increase). (3) Rate limit accuracy (must be within 5% of configured limit). (4) Auth failure rate (must not increase). Weekly rollout review meetings with SRE and service owners.

---

## 11. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale | Trade-off |
|----------|-------------------|--------|-----------|-----------|
| Gateway engine | Nginx, Kong, Envoy, HAProxy | Envoy | xDS API for dynamic config, gRPC native support, Wasm extensibility, C++ performance | Steeper learning curve than Nginx; larger binary size |
| Config store | etcd only, PostgreSQL only, both | PostgreSQL + etcd | PostgreSQL for rich queries and ACID; etcd for xDS snapshotting and leader election | Two systems to operate; but each plays to its strengths |
| Rate limiting store | Redis, Memcached, etcd | Redis Cluster | Atomic Lua scripts, TTL support, cluster mode | Single point of failure for rate limiting; mitigated by local fallback |
| Auth primary | JWT, OAuth2 introspection, API keys | JWT (primary) + API keys | JWT is stateless and fast; API keys for programmatic access | JWT revocation is eventually consistent (bloom filter delay) |
| Plugin system | Lua, Wasm, native, ext_proc | Wasm (primary) + ext_proc (escape hatch) | Wasm sandboxing + performance; ext_proc for full isolation | Wasm toolchain maturity still evolving; ext_proc adds latency |
| Route matching | Linear scan, trie, hash | Two-phase (host hash + radix trie) | O(1) + O(k) for common case; handles 10K+ routes | Regex routes fall through to linear scan; discouraged at scale |
| TLS termination | At gateway, at upstream, both | At gateway with optional re-encrypt | Centralizes cert management; reduces upstream complexity | Gateway sees plaintext; must be trusted |
| Control plane HA | Active-passive, active-active | Active-active with leader election | No failover delay; all instances serve reads | Config write conflicts resolved by etcd CAS |

---

## 12. Agentic AI Integration

### AI-Powered Traffic Management

**Anomaly Detection Agent:**
- Continuously monitors request patterns per route (RPS, error rate, latency distribution).
- Uses time-series forecasting (Prophet/LSTM) to detect anomalous traffic spikes.
- Automatically adjusts rate limits or triggers circuit breakers before upstream overload.
- Example: detects a 3x traffic spike on `/api/v1/search` at 2 AM (unusual for this route) and proactively tightens rate limits while alerting on-call.

**Intelligent Routing Agent:**
- Analyzes upstream latency and error metrics in real-time.
- Dynamically adjusts routing weights to shift traffic away from degraded backends.
- Uses reinforcement learning to optimize traffic distribution for minimum global p99 latency.
- Constraint: never shift more than 20% of traffic in a single decision (bounded blast radius).

**Configuration Validation Agent:**
- Reviews route configuration changes before applying them.
- Checks for: conflicting routes, unreachable upstreams, invalid regex patterns, security misconfigurations.
- Generates a risk score (low/medium/high) based on: number of affected routes, traffic volume of affected routes, type of change.
- High-risk changes require human approval; low-risk changes are auto-approved.

**Auto-Scaling Agent:**
- Predicts traffic demand 30 minutes ahead using historical patterns + external signals (marketing campaigns, product launches).
- Pre-scales gateway fleet before demand arrives.
- Learns from past scaling events to improve prediction accuracy.

**Security Agent:**
- Detects credential stuffing attacks by analyzing auth failure patterns across consumers.
- Identifies API key leakage by detecting usage from unexpected source IP ranges.
- Suggests IP blocklist updates based on abuse pattern analysis.
- Automatically quarantines suspected compromised API keys (requires human confirmation to revoke).

### Integration Architecture

```
┌─────────────────────────────────────────────────┐
│               AI Agent Control Loop              │
│                                                   │
│  ┌──────────┐  ┌───────────┐  ┌──────────────┐  │
│  │ Observe  │  │  Decide   │  │    Act        │  │
│  │(metrics, │─►│(ML models,│─►│(config push,  │  │
│  │ logs,    │  │ rules,    │  │ rate limit    │  │
│  │ traces)  │  │ LLM)      │  │ adjust, alert)│  │
│  └──────────┘  └───────────┘  └──────────────┘  │
│       ▲                              │            │
│       │         Feedback loop        │            │
│       └──────────────────────────────┘            │
│                                                   │
│  Safety guardrails:                               │
│  - Max 20% traffic shift per decision             │
│  - Rate limits can only be tightened, not relaxed │
│  - All actions logged with reasoning trace        │
│  - Human approval for high-risk changes           │
│  - Automatic rollback if error rate spikes        │
└─────────────────────────────────────────────────┘
```

---

## 13. Complete Interviewer Q&A Bank

> **Q1: Why use an API gateway instead of letting each service handle auth/rate-limiting?**
> A: Centralized cross-cutting concerns reduce code duplication across 2,000 services, ensure consistent security enforcement, simplify certificate management, and provide a single point for observability. Without a gateway, every service must implement JWT validation, rate limiting, and TLS termination independently, leading to inconsistencies and security gaps.

> **Q2: How does your gateway handle gRPC load balancing (which needs per-RPC balancing, not per-connection)?**
> A: HTTP/2 multiplexes multiple RPCs over a single TCP connection. L4 load balancers only balance at the connection level, causing hot-spotting. Our gateway operates at L7 and terminates HTTP/2, making independent load balancing decisions per gRPC call. This is critical because a single HTTP/2 connection can carry thousands of concurrent RPCs.

> **Q3: What happens when the gateway itself becomes a bottleneck?**
> A: We horizontally scale the gateway fleet behind an L4 load balancer (IPVS or MetalLB). The gateway is stateless, so adding nodes is linear scaling. We also implement request coalescing (dedup identical concurrent requests to the same upstream) and response caching for cacheable endpoints.

> **Q4: How do you handle request body buffering for large uploads?**
> A: By default, we stream request bodies (no buffering) to minimize memory usage and latency. For routes that require body inspection (e.g., request transformation), we buffer up to the configured max body size. For large file uploads, we bypass the filter chain and proxy directly to the upstream with chunked transfer encoding.

> **Q5: How do you implement canary deployments at the gateway level?**
> A: Weighted routing: route 5% of traffic for `/api/v1/users` to the canary upstream, 95% to stable. Weighting is based on consistent hashing of user ID (sticky canary assignment). Promote to 100% after metrics validation. The gateway also supports header-based routing for developer testing (`X-Canary: true`).

> **Q6: How does your gateway handle back-pressure from slow upstream services?**
> A: Multiple layers: (1) Connection pool limit per upstream (max 1024 connections). (2) Pending request queue with bounded size (max 100). (3) Circuit breaker trips if error rate exceeds threshold. (4) Timeout per request (connect, read, write independently configurable). (5) Retry budget: max 20% of requests can be retries to prevent retry storms.

> **Q7: How do you prevent a single tenant from monopolizing gateway resources?**
> A: Per-tenant resource quotas: max connections, max RPS, max bandwidth. Implemented via the rate limiting plugin with tenant identification from auth context. Additionally, connection-level fairness: each tenant's requests are queued separately, and a weighted fair queuing scheduler prevents starvation.

> **Q8: How do you handle API versioning at the gateway?**
> A: Three supported patterns: (1) URI-based: `/v1/users` and `/v2/users` route to different upstreams. (2) Header-based: `Accept: application/vnd.api.v2+json` selects the version. (3) Query parameter: `?version=2`. The gateway normalizes all patterns into an internal version context that the router uses.

> **Q9: What is the difference between your API gateway and a service mesh sidecar?**
> A: The API gateway handles north-south traffic (external clients to internal services) with features like API key management, consumer rate limiting, and public TLS termination. The service mesh sidecar handles east-west traffic (service to service) with features like mTLS, circuit breaking, and fine-grained traffic control. Some organizations use the gateway for both, but this conflates concerns and makes scaling harder.

> **Q10: How do you debug a request that is being incorrectly routed?**
> A: The gateway exposes a debug endpoint: `GET /debug/route?host=X&path=Y&headers=Z` that returns the matched route, filter chain, and upstream without executing the request. In production, we add a `X-Gateway-Debug: true` header (only from trusted IPs) that returns routing metadata in response headers (`X-Route-Id`, `X-Upstream-Id`, `X-Filter-Chain`).

> **Q11: How do you handle WebSocket upgrade through the gateway?**
> A: The gateway detects the `Upgrade: websocket` header and `Connection: Upgrade` header. It performs the HTTP upgrade handshake, then switches the connection to raw TCP proxying (no further HTTP processing). WebSocket connections have separate timeout settings (idle timeout, max duration) from HTTP requests.

> **Q12: How do you implement request deduplication at the gateway?**
> A: For idempotent endpoints, clients send an `Idempotency-Key` header. The gateway stores the response for a given key in Redis (TTL = 24 hours). On duplicate request, the cached response is returned. This prevents double-charges, double-creates, etc. The cache key includes the consumer ID to prevent cross-tenant collisions.

> **Q13: How do you handle graceful shutdown of a gateway node?**
> A: SIGTERM triggers: (1) Stop accepting new connections (deregister from L4 LB health check). (2) Set a drain timeout (default 30 seconds). (3) Existing requests complete normally. (4) Long-lived connections (WebSocket) are sent a close frame. (5) After drain timeout, force-close remaining connections. Kubernetes `preStop` hook + terminationGracePeriodSeconds manage this lifecycle.

> **Q14: How do you handle configuration drift between gateway nodes?**
> A: xDS streaming ensures all nodes converge to the same configuration version. The control plane tracks each node's last-acknowledged config version. If a node falls behind (stale > 60 seconds), it is flagged in monitoring. If stale > 5 minutes, it is removed from the L4 LB and restarted.

> **Q15: How would you implement a gateway for a multi-region active-active deployment?**
> A: Each region has its own gateway fleet, control plane, and Redis cluster. Configuration is replicated across regions via the Git-based config store (GitOps). Rate limits can be per-region (simpler) or global (requires cross-region Redis replication with conflict-free counters). Anycast DNS directs clients to the nearest region. Failover: if a region's gateway fleet is unhealthy, DNS health checks remove the Anycast route, and traffic shifts to the next-nearest region.

> **Q16: How do you handle request tracing across the gateway?**
> A: The gateway generates or propagates a trace ID (W3C `traceparent` header or `X-Request-ID`). Each filter in the chain creates a child span. The upstream request includes the propagated trace context. This creates a complete trace from client through gateway filters to upstream service. We use OpenTelemetry SDK with sampling (1% of requests, 100% of errors).

---

## 14. References

- **Envoy Proxy Architecture:** https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/
- **Kong Gateway:** https://docs.konghq.com/gateway/latest/
- **proxy-wasm specification:** https://github.com/proxy-wasm/spec
- **Envoy xDS Protocol:** https://www.envoyproxy.io/docs/envoy/latest/api-docs/xds_protocol
- **Sliding Window Rate Limiting:** https://blog.cloudflare.com/counting-things-a-lot-of-different-things/
- **SPIFFE/SPIRE Identity Framework:** https://spiffe.io/
- **gRPC Load Balancing:** https://grpc.io/blog/grpc-load-balancing/
- **OpenTelemetry Gateway Instrumentation:** https://opentelemetry.io/docs/
- **Katran (Facebook's L4 LB):** https://github.com/facebookincubator/katran
- **RFC 6585 (429 Too Many Requests):** https://tools.ietf.org/html/rfc6585
