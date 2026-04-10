# System Design: Distributed Rate Limiter

> **Relevance to role:** Rate limiting is critical across every layer of cloud infrastructure. Bare-metal provisioning APIs must be rate-limited to prevent thundering herds against IPMI controllers. Kubernetes API servers enforce rate limits to protect etcd. OpenStack Nova has per-tenant rate limits. Message brokers (Kafka, RabbitMQ) need producer throttling. Job schedulers must limit submission rates. Every Java/Python service needs both inbound (protect self) and outbound (protect dependencies) rate limiting.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Rate limiting by key | Limit requests per client-ID, API-key, IP, tenant, or custom dimension |
| FR-2 | Multiple algorithms | Token bucket, sliding window counter, fixed window, leaky bucket |
| FR-3 | Multi-tier limits | Per-second, per-minute, per-hour, per-day limits simultaneously |
| FR-4 | Rate limit headers | Return `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset` |
| FR-5 | Configurable policies | Admin defines limits per API, per tenant, with overrides |
| FR-6 | Distributed enforcement | Consistent counting across multiple service instances |
| FR-7 | Graceful degradation | If rate limiter is unavailable, configurable fail-open or fail-closed |
| FR-8 | Adaptive rate limiting | Backpressure-driven limits based on downstream health |
| FR-9 | Burst allowance | Allow short bursts above steady-state rate |
| FR-10 | Quota management | Long-term quotas (e.g., 10,000 API calls/day per tenant) |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Latency overhead | < 1 ms p50, < 5 ms p99 per rate-limit check |
| NFR-2 | Throughput | 1M rate-limit checks/sec cluster-wide |
| NFR-3 | Availability | 99.99%; failure mode = fail-open (don't block traffic) |
| NFR-4 | Accuracy | Within 1% of configured rate over a 1-minute window |
| NFR-5 | Scalability | Support 1M unique rate-limit keys |
| NFR-6 | Consistency | Eventual consistency across instances (AP system) |
| NFR-7 | Memory efficiency | < 1 KB per rate-limit key |

### Constraints & Assumptions
- Deployed as both sidecar (per-pod) and centralized service.
- Redis cluster (6+ nodes) as the distributed counter backend.
- Rate limit policies stored in configuration management (etcd/Consul).
- Clients are Java/Python microservices, API gateways, k8s ingress controllers.
- Some rate limits are hard (must never exceed) and some are soft (best-effort).

### Out of Scope
- DDoS mitigation (handled by network-level firewalls / CDN).
- Connection-level rate limiting (handled by load balancer).
- Cost-based billing (related but separate system).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| API endpoints rate-limited | 200 | Platform APIs |
| Tenants / API keys | 5,000 | Infrastructure tenants |
| Rate-limit keys (endpoint x tenant) | 1,000,000 | 200 x 5,000 |
| Requests per second (total platform) | 500,000 | Aggregate API traffic |
| Rate-limit checks per second | 500,000 | 1 check per request |
| Peak multiplier | 3x | Deployment storms, batch jobs |
| Peak rate-limit checks/sec | 1,500,000 | 500K x 3 |
| Redis operations per check | 2-3 | Read counter + increment + TTL |
| Redis ops/sec | 1,500,000 | 500K x 3 ops |

### Latency Requirements

| Operation | p50 | p99 | Notes |
|-----------|-----|-----|-------|
| Rate-limit check (local sidecar cache) | 0.05 ms | 0.2 ms | In-memory, no network |
| Rate-limit check (Redis) | 0.5 ms | 2 ms | Single Redis round-trip |
| Rate-limit check (Redis, cross-AZ) | 1 ms | 5 ms | Network hop |
| Policy reload | 100 ms | 500 ms | From config management |

### Storage Estimates

| Item | Size per key | Total (1M keys) |
|------|-------------|-----------------|
| Token bucket state | 24 B (tokens: 8B, last_refill: 8B, burst: 8B) | 24 MB |
| Sliding window counter | 48 B (current window + previous window counts) | 48 MB |
| Sliding window log | 8 B x max_entries (e.g., 1000) | 8 GB worst case |
| Redis key overhead | ~100 B (key name, TTL metadata) | 100 MB |
| **Total Redis memory** | | **~200 MB** (token bucket) to **8 GB** (sliding log) |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| App -> Rate Limiter (sidecar) | In-process, no network | 0 |
| App -> Rate Limiter (centralized) | 500K req/sec x 128 B | 64 MB/s |
| Rate Limiter -> Redis | 1.5M ops/sec x 64 B | 96 MB/s |
| Policy updates (config watch) | 10 updates/day x 10 KB | negligible |

---

## 3. High-Level Architecture

```
                    ┌──────────────────────────────────────┐
                    │         API Gateway / Ingress        │
                    │    (Pre-flight rate-limit check)     │
                    └──────────────┬───────────────────────┘
                                   │
               ┌───────────────────▼───────────────────────┐
               │            Service Mesh / Pod             │
               │  ┌──────────────────────────────────┐     │
               │  │     Rate Limiter Sidecar          │     │
               │  │  ┌────────────┐ ┌─────────────┐  │     │
               │  │  │ Local Cache│ │ Algorithm   │  │     │
               │  │  │ (L1)      │ │ Engine      │  │     │
               │  │  └────────────┘ └─────────────┘  │     │
               │  └──────────┬───────────────────────┘     │
               │             │ sync counters               │
               │  ┌──────────▼───────────────────────┐     │
               │  │     Application Service           │     │
               │  │     (Java / Python)               │     │
               │  └──────────────────────────────────┘     │
               └───────────────────┬───────────────────────┘
                                   │
                    ┌──────────────▼───────────────────────┐
                    │      Centralized Rate Limit Service   │
                    │  ┌────────────┐ ┌─────────────────┐  │
                    │  │ Policy     │ │ Aggregation     │  │
                    │  │ Engine     │ │ Service         │  │
                    │  └────────────┘ └─────────────────┘  │
                    └──────────────┬───────────────────────┘
                                   │
               ┌───────────────────▼───────────────────────┐
               │              Redis Cluster                 │
               │  ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐        │
               │  │Shard│ │Shard│ │Shard│ │Shard│        │
               │  │  1  │ │  2  │ │  3  │ │  4  │        │
               │  └─────┘ └─────┘ └─────┘ └─────┘        │
               └───────────────────────────────────────────┘
                                   │
               ┌───────────────────▼───────────────────────┐
               │         Configuration Management          │
               │   (etcd / Consul KV — rate limit policies)│
               └───────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **API Gateway / Ingress** | First line of defense; applies coarse per-IP and per-tenant limits before request reaches services |
| **Rate Limiter Sidecar** | Co-located with application; provides sub-millisecond checks using local counters; syncs periodically with Redis |
| **Local Cache (L1)** | In-memory rate-limit state; absorbs bursts; reduces Redis calls by batching counter updates |
| **Algorithm Engine** | Implements token bucket, sliding window, etc.; selects algorithm per policy |
| **Centralized Rate Limit Service** | Shared service for global rate limits (cross-service); aggregates counts across all instances |
| **Policy Engine** | Loads rate limit policies from config management; evaluates which policy applies to a given request |
| **Redis Cluster** | Distributed counter store; Lua scripts for atomic increment-and-check |
| **Configuration Management** | Stores rate limit policies (limits, windows, algorithms); watched for hot reload |

### Data Flows

**Rate Limit Check (sidecar path):**
1. Request arrives at sidecar.
2. Sidecar extracts rate-limit key (tenant_id + API path).
3. Local cache checked: if within budget, allow immediately.
4. If local cache depleted, synchronous Redis call: Lua script increments counter, checks limit.
5. Redis returns allow/deny + remaining quota.
6. Sidecar sets response headers and allows/rejects request.

**Counter Synchronization (sidecar -> Redis):**
1. Sidecar maintains a local counter per key.
2. Every 100ms (or N requests), sidecar pushes accumulated count to Redis.
3. Sidecar pulls back the global count to calibrate local budget.
4. This batch-sync reduces Redis ops by 10-100x at the cost of slight over-admission.

---

## 4. Data Model

### Core Entities & Schema

```
┌─────────────────────────────────────────────────────────┐
│ RateLimitPolicy                                         │
├─────────────────────────────────────────────────────────┤
│ policy_id       VARCHAR(128)    PK                      │
│ name            VARCHAR(256)                            │
│ match_criteria  JSON            -- tenant, API, IP, etc.│
│ algorithm       ENUM(TOKEN_BUCKET, SLIDING_WINDOW,      │
│                      FIXED_WINDOW, LEAKY_BUCKET)        │
│ limits          LIST<LimitRule>                          │
│ burst_size      INT             -- for token bucket     │
│ fail_open       BOOLEAN         -- behavior on error    │
│ priority        INT             -- higher = matched first│
│ enabled         BOOLEAN                                 │
│ created_at      TIMESTAMP                               │
│ updated_at      TIMESTAMP                               │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ LimitRule                                               │
├─────────────────────────────────────────────────────────┤
│ window_size     DURATION        -- 1s, 1m, 1h, 1d      │
│ max_requests    BIGINT                                  │
│ cost_per_req    INT DEFAULT 1   -- weighted requests    │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ RateLimitCounter (Redis)                                │
├─────────────────────────────────────────────────────────┤
│ key             STRING          -- rl:{tenant}:{api}:   │
│                                    {window_start}       │
│ value           INT             -- current count        │
│ TTL             DURATION        -- auto-expire after    │
│                                    window               │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ TokenBucketState (Redis Hash)                           │
├─────────────────────────────────────────────────────────┤
│ key             STRING          -- tb:{tenant}:{api}    │
│ tokens          FLOAT           -- available tokens     │
│ last_refill     BIGINT          -- epoch_ms of last     │
│                                    refill               │
│ max_tokens      INT             -- bucket capacity      │
│ refill_rate     FLOAT           -- tokens per second    │
└─────────────────────────────────────────────────────────┘
```

### Database Selection

| Store | Use | Rationale |
|-------|-----|-----------|
| **Redis Cluster** | Counter state (hot path) | Sub-millisecond latency; atomic Lua scripts; auto-expire with TTL; built-in clustering |
| **etcd / Consul KV** | Policy storage | Watch-based hot reload; linearizable for policy consistency; existing infra dependency |
| **Local in-memory** | L1 cache, sidecar state | Zero-latency; no network hop; absorbs bursts |
| MySQL | Audit log, quota tracking | Long-term storage; ACID for billing-related quota |

### Indexing Strategy

**Redis key patterns:**
```
# Fixed window counter
rl:fixed:{tenant_id}:{api_path}:{window_start_epoch}
  TTL = window_size + 1s (buffer)
  Example: rl:fixed:tenant-42:/v1/servers:1712620800

# Sliding window (sorted set)
rl:sliding:{tenant_id}:{api_path}
  Members: request timestamps (score = epoch_ms)
  ZREMRANGEBYSCORE to expire old entries

# Token bucket (hash)
rl:tb:{tenant_id}:{api_path}
  Fields: tokens, last_refill, max_tokens, refill_rate
```

---

## 5. API Design

### gRPC Service

```protobuf
service RateLimitService {
  // Check if request should be allowed
  rpc CheckRateLimit(RateLimitRequest) returns (RateLimitResponse);

  // Batch check (multiple keys in one call)
  rpc CheckRateLimitBatch(BatchRateLimitRequest)
      returns (BatchRateLimitResponse);

  // Policy management
  rpc CreatePolicy(CreatePolicyRequest) returns (Policy);
  rpc UpdatePolicy(UpdatePolicyRequest) returns (Policy);
  rpc DeletePolicy(DeletePolicyRequest) returns (Empty);
  rpc ListPolicies(ListPoliciesRequest) returns (ListPoliciesResponse);

  // Quota management
  rpc GetQuota(GetQuotaRequest) returns (QuotaInfo);
  rpc ResetQuota(ResetQuotaRequest) returns (QuotaInfo);
}

message RateLimitRequest {
  string domain = 1;            // e.g., "infra-api"
  repeated Descriptor descriptors = 2;
  int32 hits = 3;               // default 1; weighted requests
}

message Descriptor {
  string key = 1;               // e.g., "tenant_id"
  string value = 2;             // e.g., "tenant-42"
}

message RateLimitResponse {
  enum Code {
    OK = 0;
    OVER_LIMIT = 1;
  }
  Code overall_code = 1;
  repeated RateLimitStatus statuses = 2;
}

message RateLimitStatus {
  Code code = 1;
  RateLimitInfo current_limit = 2;
  int64 limit = 3;              // max requests in window
  int64 remaining = 4;          // requests remaining
  int64 reset_after_ms = 5;     // ms until window resets
  int64 retry_after_ms = 6;     // ms until client should retry
}
```

### REST Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/v1/ratelimit/check` | Check rate limit (body: domain, descriptors, hits) |
| GET | `/v1/ratelimit/quota/{tenant_id}` | Get current quota usage |
| POST | `/v1/ratelimit/quota/{tenant_id}/reset` | Reset quota (admin) |
| GET | `/v1/policies` | List rate limit policies |
| POST | `/v1/policies` | Create policy |
| PUT | `/v1/policies/{policy_id}` | Update policy |
| DELETE | `/v1/policies/{policy_id}` | Delete policy |
| GET | `/v1/ratelimit/stats` | Rate limiter health and stats |

### Rate Limit Response Headers

```http
HTTP/1.1 429 Too Many Requests
X-RateLimit-Limit: 1000
X-RateLimit-Remaining: 0
X-RateLimit-Reset: 1712620860
Retry-After: 30
Content-Type: application/json

{
  "error": "rate_limit_exceeded",
  "message": "Rate limit of 1000 requests per minute exceeded",
  "retry_after_seconds": 30
}
```

### CLI

```bash
# Check rate limit for a key
ratelimit check --domain infra-api --tenant tenant-42 --api /v1/servers

# View quota
ratelimit quota get --tenant tenant-42
# Output:
# Tenant:    tenant-42
# Daily:     8,432 / 10,000  (84.3%)
# Per-min:   42 / 1,000      (4.2%)
# Per-sec:   3 / 100         (3.0%)

# Update policy
ratelimit policy update --id pol-123 --limit 2000 --window 60s

# View stats
ratelimit stats
# Output:
# Total checks/sec:  487,234
# Allow rate:         99.2%
# Deny rate:          0.8%
# Redis latency p99:  1.8ms
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Rate Limiting Algorithms

**Why it's hard:**
Each algorithm has different trade-offs between accuracy, memory, fairness, and burst handling. Choosing the wrong one causes either over-admission (resource overload) or unfair rejection (legitimate users denied).

**Algorithm Comparison:**

| Algorithm | Memory | Accuracy | Burst | Smoothness | Distributed | Complexity |
|-----------|--------|----------|-------|------------|-------------|------------|
| **Fixed Window** | Very low (1 counter) | Low (boundary spike) | 2x burst at boundary | Poor | Easy | Simple |
| **Sliding Window Log** | High (all timestamps) | Perfect | No burst beyond limit | Excellent | Hard (large sorted sets) | Medium |
| **Sliding Window Counter** | Low (2 counters) | Good (~0.003% error) | Small overshoot | Good | Easy | Simple |
| **Token Bucket** | Very low (2 values) | Good | Configurable burst | Good | Medium | Medium |
| **Leaky Bucket** | Low (queue + rate) | Perfect | No burst (smoothed) | Excellent | Medium | Medium |

**Selected: Token Bucket (default) + Sliding Window Counter (for quotas)**

Justification: Token bucket is the industry standard (used by AWS API Gateway, Google Cloud, Envoy). It allows configurable burst (bucket size) while enforcing steady-state rate (refill rate). Sliding window counter is used for quota tracking (daily limits) where accuracy matters more than burst tolerance.

**Token Bucket — Redis Lua Implementation:**

```lua
-- KEYS[1] = rate limit key (e.g., "tb:tenant-42:/v1/servers")
-- ARGV[1] = max_tokens (bucket capacity)
-- ARGV[2] = refill_rate (tokens per second)
-- ARGV[3] = now (current epoch in milliseconds)
-- ARGV[4] = requested_tokens (usually 1)

local key = KEYS[1]
local max_tokens = tonumber(ARGV[1])
local refill_rate = tonumber(ARGV[2])
local now = tonumber(ARGV[3])
local requested = tonumber(ARGV[4])

-- Get current state
local data = redis.call('HMGET', key, 'tokens', 'last_refill')
local tokens = tonumber(data[1])
local last_refill = tonumber(data[2])

-- Initialize if first request
if tokens == nil then
    tokens = max_tokens
    last_refill = now
end

-- Refill tokens based on elapsed time
local elapsed_ms = now - last_refill
local refill = elapsed_ms * refill_rate / 1000.0
tokens = math.min(max_tokens, tokens + refill)

-- Try to consume tokens
local allowed = false
local remaining = 0
local retry_after_ms = 0

if tokens >= requested then
    tokens = tokens - requested
    allowed = true
    remaining = math.floor(tokens)
else
    -- Calculate when enough tokens will be available
    local deficit = requested - tokens
    retry_after_ms = math.ceil(deficit / refill_rate * 1000)
    remaining = 0
end

-- Save state
redis.call('HMSET', key,
    'tokens', tostring(tokens),
    'last_refill', tostring(now))
-- Set TTL to auto-cleanup idle keys (2x the refill time for full bucket)
local ttl = math.ceil(max_tokens / refill_rate) * 2
redis.call('EXPIRE', key, ttl)

return {allowed and 1 or 0, remaining, retry_after_ms, max_tokens}
```

**Sliding Window Counter — Redis Lua Implementation:**

```lua
-- Sliding window counter: weighted average of current and previous window
-- More accurate than fixed window, less memory than sliding log
--
-- KEYS[1] = counter key prefix
-- ARGV[1] = window_size_ms
-- ARGV[2] = max_requests
-- ARGV[3] = now (epoch_ms)
-- ARGV[4] = cost (usually 1)

local prefix = KEYS[1]
local window_ms = tonumber(ARGV[1])
local max_requests = tonumber(ARGV[2])
local now = tonumber(ARGV[3])
local cost = tonumber(ARGV[4])

-- Calculate window boundaries
local current_window = math.floor(now / window_ms)
local current_key = prefix .. ':' .. current_window
local previous_key = prefix .. ':' .. (current_window - 1)

-- Get counts
local current_count = tonumber(redis.call('GET', current_key) or '0')
local previous_count = tonumber(redis.call('GET', previous_key) or '0')

-- Calculate weighted count
-- Weight of previous window = portion of previous window that overlaps
-- with our sliding window
local elapsed_in_current = now - (current_window * window_ms)
local previous_weight = 1.0 - (elapsed_in_current / window_ms)
local weighted_count = current_count + (previous_count * previous_weight)

-- Check limit
if weighted_count + cost > max_requests then
    -- Calculate retry-after
    local need = weighted_count + cost - max_requests
    -- Time until enough of previous window's weight decays
    local retry_ms = math.ceil(need / previous_count * window_ms)
    return {0, max_requests - math.ceil(weighted_count), retry_ms}
end

-- Increment current window counter
redis.call('INCRBY', current_key, cost)
redis.call('PEXPIRE', current_key, window_ms * 2)  -- keep for 2 windows

local remaining = max_requests - math.ceil(weighted_count + cost)
return {1, remaining, 0}
```

**Fixed Window Boundary Problem:**

```
Limit: 100 requests per minute

Timeline:
   Window 1                Window 2
|------- 60s -------|------- 60s -------|
                    ↓
            99 reqs │ 99 reqs
            (last   │ (first
            1 sec)  │ 1 sec)

Result: 198 requests in a 2-second span!
The sliding window counter fixes this by weighing the previous window.

Weighted count at Window 2, t=1s:
  current_count = 99
  previous_count = 99
  previous_weight = 1 - (1/60) = 0.983
  weighted = 99 + (99 * 0.983) = 196.3 → OVER LIMIT

The sliding window counter correctly rejects this.
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Redis down | No rate limiting | Fail-open with local sidecar limits (degraded accuracy) |
| Lua script timeout | Request blocked | Set script timeout; fallback to local decision |
| Clock skew between service instances | Inconsistent window alignment | Use Redis server time (`redis.call('TIME')`) instead of client time |
| Redis key eviction (memory pressure) | Lost counters, over-admission | Reserve memory for rate-limit keys; use separate Redis cluster |

**Interviewer Q&A:**

**Q1: Why use a Lua script in Redis instead of multiple commands?**
A: Atomicity. Without Lua, a GET + check + INCR sequence has a TOCTOU race: between GET and INCR, another request might increment the counter, causing over-admission. Redis executes Lua scripts atomically (single-threaded execution model), eliminating the race.

**Q2: What's the error rate of the sliding window counter?**
A: Cloudflare measured it at 0.003% error rate compared to perfect sliding window. The error comes from assuming uniform distribution of requests in the previous window. In practice, this is negligibly small and the memory savings (2 counters vs thousands of timestamps) are enormous.

**Q3: How does the token bucket differ from the leaky bucket?**
A: Token bucket allows bursts up to bucket capacity, then enforces the average rate. Leaky bucket enforces a constant output rate with no bursts — it queues excess requests and processes them at a fixed rate. Token bucket is better for API rate limiting (users want to burst). Leaky bucket is better for traffic shaping (network, producer throttling).

**Q4: When would you choose fixed window over sliding window?**
A: Only when simplicity and performance are paramount and the 2x boundary burst is acceptable. Fixed window is one Redis INCR + one EXPIRE — the absolute minimum Redis operations. For internal service-to-service limits where a brief 2x spike is tolerable, fixed window is fine.

**Q5: How do you handle weighted requests (some endpoints cost more)?**
A: The `cost` parameter in the rate-limit check. A `GET /servers` costs 1 token; a `POST /servers` (expensive operation) costs 10 tokens. The Lua script deducts `cost` tokens instead of 1. Policy defines the cost mapping.

**Q6: What is the "thundering herd" problem when rate limits reset?**
A: At window boundary, all previously limited clients retry simultaneously. Mitigation: (1) Add jitter to `Retry-After` headers (don't send exact window reset time). (2) Use token bucket (continuous refill, no discrete reset). (3) Stagger window starts per client using client-ID hash.

---

### Deep Dive 2: Distributed Counter Synchronization (Sidecar Architecture)

**Why it's hard:**
A centralized rate limiter (every request goes to Redis) adds latency and creates a single point of failure. A fully local rate limiter (no synchronization) allows N instances to each allow the full rate, giving Nx over-admission. The sidecar model must balance accuracy vs latency by batching counter updates.

**Approaches:**

| Approach | Latency | Accuracy | Failure Mode |
|----------|---------|----------|--------------|
| Centralized (every request to Redis) | 1-5 ms | Perfect | Redis down = no limiting |
| Fully local (no sync) | 0 ms | N x over-admit (N=instance count) | None (but useless) |
| **Sidecar with periodic sync** | **0 ms local, sync every 100ms** | **~5-10% over-admit** | **Graceful degradation** |
| Gossip-based peer counting | 0 ms local, eventual sync | ~10-20% over-admit | Partition = inaccurate |

**Selected: Sidecar with periodic sync + local budget**

```python
class RateLimiterSidecar:
    """
    Co-located with application in same pod.
    Maintains local token budget; syncs with Redis periodically.
    """

    def __init__(self, redis_client, sync_interval_ms=100):
        self.redis = redis_client
        self.sync_interval = sync_interval_ms / 1000.0
        self.local_state = {}       # key -> LocalBucket
        self.pending_counts = {}    # key -> count to sync
        self.sync_thread = Thread(target=self._sync_loop, daemon=True)
        self.sync_thread.start()

    def check(self, key, policy):
        """
        Check rate limit. Returns (allowed, remaining, retry_after).
        This runs in the request hot path — must be < 0.1ms.
        """
        if key not in self.local_state:
            self.local_state[key] = LocalBucket(
                tokens=policy.burst_size,  # Start with full burst
                last_check=time.monotonic()
            )

        bucket = self.local_state[key]

        # Refill tokens based on local allocation
        # Local allocation = global_rate / instance_count
        now = time.monotonic()
        elapsed = now - bucket.last_check
        bucket.last_check = now

        local_rate = policy.rate / self._estimated_instances()
        refill = elapsed * local_rate
        bucket.tokens = min(policy.burst_size, bucket.tokens + refill)

        if bucket.tokens >= 1:
            bucket.tokens -= 1
            self.pending_counts[key] = self.pending_counts.get(key, 0) + 1
            remaining = int(bucket.tokens)
            return (True, remaining, 0)
        else:
            retry_after = (1 - bucket.tokens) / local_rate
            return (False, 0, int(retry_after * 1000))

    def _sync_loop(self):
        """
        Background thread: pushes local counts to Redis,
        pulls back global state, recalibrates local budget.
        """
        while True:
            time.sleep(self.sync_interval)
            try:
                self._do_sync()
            except Exception as e:
                log.warning(f"Rate limiter sync failed: {e}")
                # Continue with local state — graceful degradation

    def _do_sync(self):
        """
        Atomic sync with Redis.
        Pushes accumulated counts, gets back global remaining.
        """
        for key, count in list(self.pending_counts.items()):
            if count == 0:
                continue

            # Lua script: atomically add our count, return global state
            result = self.redis.eval(
                SYNC_SCRIPT,
                keys=[key],
                args=[count, int(time.time() * 1000)]
            )

            global_remaining = result[0]
            global_limit = result[1]
            total_instances = result[2]

            # Recalibrate local budget
            local_share = global_remaining / max(total_instances, 1)
            if key in self.local_state:
                self.local_state[key].tokens = min(
                    self.local_state[key].tokens,
                    local_share
                )

            # Reset pending count
            self.pending_counts[key] = 0

    def _estimated_instances(self):
        """
        Estimate number of service instances sharing this rate limit.
        Updated during sync from Redis (each instance registers).
        """
        return max(self._instance_count, 1)


SYNC_SCRIPT = """
-- Push local count, get global state, register instance
local key = KEYS[1]
local local_count = tonumber(ARGV[1])
local now = tonumber(ARGV[2])

-- Increment global counter by local count
local new_count = redis.call('HINCRBY', key, 'count', local_count)

-- Register this instance (heartbeat)
local instance_key = key .. ':instances'
redis.call('ZADD', instance_key, now, ARGV[3])  -- ARGV[3] = instance_id
-- Remove stale instances (no heartbeat in 5s)
redis.call('ZREMRANGEBYSCORE', instance_key, 0, now - 5000)
local total_instances = redis.call('ZCARD', instance_key)

local limit = tonumber(redis.call('HGET', key, 'limit') or '1000')
local remaining = limit - new_count

return {remaining, limit, total_instances}
"""
```

**Accuracy Analysis:**

```
Scenario: 10 instances, limit = 1000 req/min, sync_interval = 100ms

Without sync: each instance allows 1000/min = 10,000 total (10x over)
With sync every 100ms:
  - Each instance gets ~100 tokens locally between syncs
  - At 100 req/sec/instance, that's 10 requests between syncs
  - Over-admission window: 10 instances x 10 requests = 100 extra
  - Over-admission rate: 100/1000 = 10%

Reducing sync_interval to 50ms:
  - 5 requests between syncs per instance
  - Over-admission: 50/1000 = 5%

Reducing to 10ms:
  - 1 request between syncs
  - Over-admission: 10/1000 = 1%
  - But 100 Redis ops/sec/key from each instance = 1000 ops/sec/key
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Redis unreachable | Local sidecar continues with stale budget | Configurable: fail-open (allow with local limit) or fail-closed (reject all) |
| Sidecar crash | No rate limiting for that pod | Application-level fallback; k8s restarts sidecar container |
| Sync thread stuck | Local budget depletes, then over-admit | Watchdog timer; restart sync thread if no successful sync in 5s |
| Instance count estimate wrong | Budget miscalculated | Redis-side instance registry with heartbeat; converges in 1 sync interval |

**Interviewer Q&A:**

**Q1: How do you decide between sidecar and centralized rate limiting?**
A: Sidecar for latency-sensitive, high-throughput paths (API hot path). Centralized for global rate limits that must be precise (tenant daily quota, billing-relevant limits). Many systems use both: sidecar for per-second burst limits, centralized for per-day quotas.

**Q2: What happens during a rolling deployment (instances scaling up/down)?**
A: The instance count changes. During scale-up, new instances register via Redis heartbeat; existing instances see the count increase and reduce their local budgets. During scale-down, the heartbeat timeout (5s) removes stale instances; remaining instances increase their budgets. There's a brief over/under-admission during the transition.

**Q3: How does Envoy (service mesh) handle rate limiting?**
A: Envoy supports both local rate limiting (per-instance, no coordination) and global rate limiting (calls an external rate limit service via gRPC). Envoy's external rate limit service (typically `ratelimit` by Lyft) uses Redis under the hood. The sidecar model we described is how Istio configures Envoy for rate limiting.

**Q4: Can you do rate limiting without Redis?**
A: Yes. (1) In-memory with gossip protocol (Hazelcast, peer-to-peer counting). (2) Database-backed (MySQL with `INSERT ... ON DUPLICATE KEY UPDATE` — high latency). (3) Deterministic local limiting: give each instance 1/N of the budget statically. (4) Lease-based: each instance leases a portion of the budget from a coordinator. All have worse trade-offs than Redis for this use case.

**Q5: How do you handle rate limiting for serverless / auto-scaled services?**
A: The instance count is highly variable. Use the Redis-registered instance count (heartbeat-based), not a static configuration. Alternatively, use the centralized rate limiter exclusively for serverless workloads, since there's no stable sidecar.

**Q6: What is the circuit breaker vs rate limiter distinction?**
A: Rate limiter protects a service from excessive incoming traffic (server-side protection). Circuit breaker protects a client from calling a failing downstream (client-side protection). They're complementary: rate limiter says "I'm receiving too many requests," circuit breaker says "my dependency is unhealthy, stop calling it." Both can be triggered by similar signals (error rate, latency), but their actions differ.

---

### Deep Dive 3: Adaptive Rate Limiting (Backpressure-Driven)

**Why it's hard:**
Static rate limits are configured based on estimates and load tests. But real-world capacity is dynamic: a degraded database makes the service slower; a noisy neighbor consumes shared resources; a dependency's rate limit changes. Adaptive rate limiting adjusts limits based on actual system health.

**Approaches:**

| Approach | Signal | Pros | Cons |
|----------|--------|------|------|
| Static limits | Configuration | Predictable, simple | Can't adapt to changing conditions |
| **AIMD (Additive Increase, Multiplicative Decrease)** | **Error rate, latency** | **TCP-proven; stable convergence** | Slow to ramp up after recovery |
| Gradient-based | Latency trend | Proactive (acts before saturation) | Sensitive to noise |
| Queue-depth based | Request queue length | Direct signal of overload | Only works for queuing systems |
| ML-based | Multiple signals | Optimal in theory | Complex; training data needed |

**Selected: AIMD with latency and error rate signals**

```python
class AdaptiveRateLimiter:
    """
    Dynamically adjusts rate limits based on downstream health.
    Inspired by TCP congestion control (AIMD).
    """

    def __init__(self, base_limit, min_limit, max_limit):
        self.base_limit = base_limit       # configured limit
        self.current_limit = base_limit
        self.min_limit = min_limit         # floor (e.g., 10% of base)
        self.max_limit = max_limit         # ceiling (e.g., 200% of base)
        self.adjustment_interval = 1.0     # seconds
        self.increase_step = 0.05          # 5% increase per interval
        self.decrease_factor = 0.5         # 50% decrease on overload

        # Health signals
        self.latency_threshold_ms = 100    # p99 latency target
        self.error_rate_threshold = 0.05   # 5% error rate target

    def adjust(self, current_p99_latency_ms, current_error_rate):
        """
        Called every adjustment_interval.
        Uses AIMD: additive increase when healthy,
        multiplicative decrease when overloaded.
        """
        is_overloaded = (
            current_p99_latency_ms > self.latency_threshold_ms or
            current_error_rate > self.error_rate_threshold
        )

        if is_overloaded:
            # Multiplicative decrease: cut limit in half
            self.current_limit = max(
                self.min_limit,
                int(self.current_limit * self.decrease_factor)
            )
            log.warning(
                f"Adaptive rate limit decreased to {self.current_limit} "
                f"(latency={current_p99_latency_ms}ms, "
                f"error_rate={current_error_rate:.2%})"
            )
        else:
            # Additive increase: slowly ramp up
            increase = max(1, int(self.base_limit * self.increase_step))
            self.current_limit = min(
                self.max_limit,
                self.current_limit + increase
            )

        return self.current_limit

    def get_effective_limit(self):
        return self.current_limit


class BackpressureAwareService:
    """
    Service that applies adaptive rate limiting based on
    downstream health signals.
    """

    def __init__(self):
        self.rate_limiter = AdaptiveRateLimiter(
            base_limit=1000,  # 1000 req/min base
            min_limit=100,    # never go below 100
            max_limit=2000    # never exceed 2000
        )
        self.health_checker = DownstreamHealthChecker()

    def handle_request(self, request):
        # Check against current adaptive limit
        limit = self.rate_limiter.get_effective_limit()
        if not self.check_rate_limit(request, limit):
            return RateLimitResponse(
                status=429,
                retry_after=self.calculate_retry_after(limit),
                # Communicate that this is adaptive
                headers={
                    'X-RateLimit-Limit': str(limit),
                    'X-RateLimit-Adaptive': 'true',
                    'X-RateLimit-BaseLimit': str(
                        self.rate_limiter.base_limit
                    )
                }
            )
        return self.process(request)

    def adjustment_loop(self):
        """Runs every second."""
        while True:
            health = self.health_checker.get_metrics()
            new_limit = self.rate_limiter.adjust(
                current_p99_latency_ms=health.p99_latency_ms,
                current_error_rate=health.error_rate
            )
            self.update_redis_limit(new_limit)
            time.sleep(1)
```

**AIMD Behavior Visualization:**

```
Rate Limit
    ^
2000│                                              ╱──
    │                                            ╱
    │                                          ╱
1000│ ────────────────╲                      ╱
    │                  ╲                   ╱
    │                   ╲                ╱
 500│                    ╲──╱──────── ╱
    │                       overload  recovery
 100│ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ── min
    └──────────────────────────────────────────────────►
                                                    Time

Normal → Overload detected → Cut 50% → Recover 5%/interval → Normal
```

**Interviewer Q&A:**

**Q1: Why AIMD over more sophisticated algorithms?**
A: AIMD is proven stable in TCP congestion control (billions of connections). It converges to fairness when multiple clients share a resource. More complex algorithms (PID controllers, ML models) risk oscillation and are harder to reason about during incidents.

**Q2: How do you prevent adaptive rate limiting from cascading failures?**
A: (1) Set a minimum limit (never drop to 0). (2) Use exponential backoff with jitter for the decrease. (3) Circuit breaker on the downstream — if it's truly down, stop sending traffic entirely rather than slowly ramping down. (4) Rate limit the rate of limit changes (max 1 adjustment/sec).

**Q3: Should the downstream service set its own rate limit or should callers adapt?**
A: Both. The downstream publishes its rate limit (either via headers or config). The caller uses adaptive limiting to stay under the published limit even when the downstream is degraded. This is cooperative: the downstream defines the contract, the caller enforces it adaptively.

**Q4: How does this interact with Kubernetes HPA (Horizontal Pod Autoscaler)?**
A: They work at different timescales. Adaptive rate limiting reacts in seconds (cut traffic immediately). HPA reacts in minutes (scale up pods). During a traffic spike: (1) Adaptive rate limiter kicks in immediately, reducing overload. (2) HPA detects sustained load, scales up. (3) As new pods become healthy, adaptive limiter increases limits back. This layered approach prevents overload during the HPA scaling window.

**Q5: How do you handle multi-tier rate limits (per-second AND per-day)?**
A: Check all tiers for every request. If any tier denies, the request is denied. Return the most restrictive remaining count. Example: 100/sec limit with 95 remaining, but daily limit has only 5 remaining — return `X-RateLimit-Remaining: 5` with the daily reset time.

**Q6: What is the "thundering herd" problem after a rate limit window resets?**
A: All limited clients retry simultaneously at window boundary. Solutions: (1) Jitter in `Retry-After` header: `base_retry + random(0, jitter)`. (2) Token bucket (continuous refill, no discrete reset). (3) Exponential backoff on client side. (4) Server-side queue with admission control instead of hard reject.

---

### Deep Dive 4: Rate Limiter Deployment Models

**Why it's hard:**
Where you place the rate limiter in the architecture affects latency, accuracy, and failure modes. Each deployment model has distinct trade-offs.

**Deployment Models:**

```
Model 1: API Gateway (Centralized)
┌─────────┐     ┌──────────┐     ┌─────────┐
│ Client  │────►│ Gateway  │────►│ Service │
│         │     │ (limits) │     │         │
└─────────┘     └──────────┘     └─────────┘
Pro: Single enforcement point; consistent limits
Con: Gateway is SPOF; all traffic passes through

Model 2: Sidecar (Decentralized)
┌─────────┐     ┌────────────────────────┐
│ Client  │────►│ Pod                    │
│         │     │ ┌────────┐ ┌────────┐  │
│         │     │ │Sidecar │→│Service │  │
│         │     │ │(limits)│ │        │  │
│         │     │ └────────┘ └────────┘  │
│         │     └────────────────────────┘
Pro: Low latency; no SPOF
Con: Distributed counting is approximate

Model 3: Library (In-Process)
┌─────────┐     ┌────────────────────────┐
│ Client  │────►│ Service                │
│         │     │ ┌────────────────────┐ │
│         │     │ │ Rate Limit Library │ │
│         │     │ │ (in-process)       │ │
│         │     │ └────────────────────┘ │
│         │     └────────────────────────┘
Pro: Minimal latency; no sidecar overhead
Con: Language-specific; must be integrated per service

Model 4: Service Mesh (Envoy)
┌─────────┐     ┌────────────────────────┐
│ Client  │────►│ Pod                    │
│         │     │ ┌────────┐ ┌────────┐  │
│         │     │ │ Envoy  │→│Service │  │
│         │     │ │(local+ │ │        │  │
│         │     │ │ global)│ │        │  │
│         │     │ └───┬────┘ └────────┘  │
│         │     └─────│──────────────────┘
│         │           │ (global limits)
│         │     ┌─────▼──────────────────┐
│         │     │ Rate Limit Service     │
│         │     │ (centralized)          │
│         │     └────────────────────────┘
Pro: Best of both; language-agnostic; service mesh handles routing
Con: Adds Envoy dependency; 2 network hops for global limits
```

**Selection Matrix for Infrastructure:**

| Use Case | Recommended Model | Reason |
|----------|------------------|--------|
| Kubernetes API server protection | Library (built-in) | API server has built-in rate limiting (`--max-requests-inflight`) |
| Tenant API rate limiting | Gateway + Service Mesh | Consistent per-tenant enforcement at ingress |
| Internal service-to-service | Sidecar / Library | Low latency; approximate accuracy OK |
| Bare-metal provisioning API | Centralized | Must be precise (hardware operations are expensive) |
| Message broker producer throttling | Library (Kafka client) | Kafka client has built-in rate limiting |

**Interviewer Q&A:**

**Q1: In a microservices architecture, who is responsible for rate limiting — the caller or the callee?**
A: Both, for different reasons. The callee (server) rate-limits to protect itself from overload. The caller (client) rate-limits to respect the server's published limits and avoid wasting resources on requests that will be rejected. In practice, the callee's rate limiter is the authoritative enforcement; the caller's is a courtesy that reduces unnecessary traffic.

**Q2: How does rate limiting interact with retries?**
A: Retries amplify traffic. If a service returns 429 and the client retries immediately, it doubles the load. Solution: (1) Clients must respect `Retry-After` headers. (2) Exponential backoff with jitter. (3) Rate-limited requests should not count against the client's retry budget. (4) The rate limiter should track retry traffic separately.

**Q3: How do you rate-limit gRPC streaming calls?**
A: (1) Rate limit stream creation (new streams per second). (2) Rate limit messages within a stream (messages per second per stream). (3) Rate limit bytes per second (bandwidth limiting). gRPC interceptors (Java) or middleware (Python) can enforce all three.

**Q4: What about rate limiting in a multi-tenant bare-metal platform?**
A: Per-tenant limits at multiple levels: (1) API requests/sec per tenant. (2) Concurrent provisioning operations per tenant. (3) Daily provisioning quota per tenant. (4) IPMI operations per minute per rack (hardware limit, not per-tenant). Tenant limits are soft (configurable); hardware limits are hard (cannot be exceeded without damaging equipment).

**Q5: How do you handle burst traffic from legitimate use cases (e.g., batch job submission)?**
A: Token bucket with a generous burst size. If the steady-state rate is 100 req/sec, set burst to 500. This allows batch submissions to burst to 500 without being limited, as long as the average rate stays at 100. For truly large batches, provide a separate "batch" API endpoint with its own limits and async processing.

**Q6: What's the relationship between rate limiting and quota management?**
A: Rate limiting controls the rate of requests (requests per second). Quota management controls the total volume (requests per day, GB per month). They're enforced at different timescales. Rate limits use in-memory counters with sub-second resolution. Quotas use persistent counters (database) with daily/monthly resolution. A request can pass the rate limit but fail the quota check.

---

## 7. Scheduling & Resource Management

### Rate Limiting in Job Scheduling

**Job Submission Rate Limiting:**
```python
class JobSchedulerRateLimiter:
    """
    Limits job submission rates to prevent scheduler overload.
    """

    # Per-tenant limits
    SUBMISSION_LIMITS = {
        "per_second": 50,        # 50 job submissions/sec
        "per_minute": 1000,      # 1000/min sustained
        "per_day": 100_000,      # 100K/day quota
        "concurrent": 500,       # max 500 running jobs
    }

    # Per-resource-pool limits
    POOL_LIMITS = {
        "bare_metal": {"per_minute": 100},   # BM provisioning is slow
        "vm": {"per_minute": 500},            # VM faster
        "container": {"per_minute": 5000},    # Containers fast
    }

    def check_submission(self, tenant_id, job):
        # Check per-tenant rate
        if not self.check_rate(f"tenant:{tenant_id}", self.SUBMISSION_LIMITS):
            return RateLimitResponse(
                denied=True,
                reason="Tenant submission rate exceeded",
                retry_after=self.get_retry_after(f"tenant:{tenant_id}")
            )

        # Check per-resource-pool rate
        pool = job.resource_pool
        if not self.check_rate(f"pool:{pool}", self.POOL_LIMITS[pool]):
            return RateLimitResponse(
                denied=True,
                reason=f"Resource pool '{pool}' submission rate exceeded",
                retry_after=self.get_retry_after(f"pool:{pool}")
            )

        # Check concurrent job limit
        running = self.count_running_jobs(tenant_id)
        if running >= self.SUBMISSION_LIMITS["concurrent"]:
            return RateLimitResponse(
                denied=True,
                reason=f"Concurrent job limit reached ({running}/{self.SUBMISSION_LIMITS['concurrent']})",
                retry_after=60  # check again in 1 min
            )

        return RateLimitResponse(denied=False)
```

**IPMI Rate Limiting (Hardware Protection):**
```
IPMI controllers on bare-metal servers have physical limits:
- Max 10 IPMI sessions simultaneously per BMC
- Max 5 commands per second per BMC
- Rack-level PDU: max 2 power commands per second

Rate limit hierarchy:
  /ipmi/{server_id}/commands    → 5/sec (per-server hardware limit)
  /ipmi/rack/{rack_id}/power    → 2/sec (per-rack PDU limit)
  /ipmi/datacenter/{dc}/bulk    → 50/sec (prevent flash crowds)
```

---

## 8. Scaling Strategy

| Dimension | Current | Scaled | Strategy |
|-----------|---------|--------|----------|
| Rate-limit checks/sec | 500K | 5M | Add Redis shards; more sidecar instances |
| Unique keys | 1M | 10M | Redis Cluster with 16+ shards |
| Policy count | 100 | 1000 | etcd handles easily |
| Sidecar instances | 500 | 5000 | Each independent; scale linearly |
| Redis memory | 200 MB | 2 GB | Scale Redis memory |

**Interviewer Q&A:**

**Q1: What's the bottleneck in a rate-limiting system at scale?**
A: Redis. At 1M+ ops/sec, a single Redis node saturates its single-threaded event loop. Solution: Redis Cluster shards keys across nodes. The Lua script must use single-key operations (hash tags) to run on one shard. At 5M+ ops/sec, the network becomes the bottleneck — use UNIX domain sockets for co-located Redis.

**Q2: How do you handle a hot key (one tenant consuming 50% of traffic)?**
A: (1) Sidecar local limiting absorbs most of the traffic without hitting Redis. (2) Redis key sharding doesn't help (hot key on one shard). (3) Replicate the hot key to a local Redis read replica. (4) For truly hot keys, use in-memory rate limiting only (accept some inaccuracy).

**Q3: How do you rate-limit across multiple data centers?**
A: Option 1: Per-DC rate limits (e.g., 500/sec per DC, not 1000/sec global). Simple but wastes capacity if traffic is unevenly distributed. Option 2: Global rate limit via cross-DC Redis replication (CRDTs or active-active). Accurate but adds latency. Option 3: Hierarchical — local DC limits for burst, global quota service for daily limits. We prefer Option 3 for infrastructure.

**Q4: What happens if Redis loses data (e.g., restart without persistence)?**
A: All counters reset to zero. Every key gets a fresh budget. Brief over-admission until counters rebuild. For quota-critical limits (billing), also track in MySQL and reconcile on Redis restart. For rate limits (per-second), the brief over-admission during Redis restart is acceptable.

**Q5: How do you load-test a rate limiter?**
A: Generate synthetic traffic at 2-3x expected peak. Measure: (1) Does the limiter reject at the configured rate? (2) What's the over-admission rate? (3) What's the added latency? (4) What happens when Redis goes down mid-test? (5) What happens when traffic shifts from uniform to bursty? Use tools like `vegeta`, `wrk`, or custom harnesses.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO |
|---|---------|-----------|--------|----------|-----|
| 1 | Redis single shard down | Redis Sentinel / Cluster failover | Keys on that shard: no limiting (fail-open) or all rejected (fail-closed) | Sentinel promotes replica in 5-30s | 30s |
| 2 | Redis cluster fully down | Health check failure | All centralized rate limiting disabled | Sidecars fall back to local-only limiting | < 1s (degraded mode) |
| 3 | Sidecar crash | k8s liveness probe | Single pod unprotected | k8s restarts sidecar container | 5-10s |
| 4 | Config management (etcd) down | Watch disconnected | Policies can't be updated (stale OK) | Continue with cached policies | 0 (cached) |
| 5 | Network partition (sidecar can't reach Redis) | Sync failure | Sidecar uses local-only; accuracy degrades | Partition heals; sync resumes | Duration of partition |
| 6 | Rate limit policy misconfiguration | Monitoring (sudden spike in 429s or 0 429s) | Over-limiting (blocking legitimate traffic) or under-limiting (resource overload) | Rollback policy; alerts on policy change | < 1 min (manual) |
| 7 | Clock skew in sliding window | Counter misalignment | Slight over/under-admission | Use Redis TIME; or accept (sliding window counter is resilient to small skew) | N/A |
| 8 | Memory pressure on Redis | Redis maxmemory alert | Key eviction; counters lost | Increase memory or reduce key count | 5 min |

### Consensus & Coordination

Rate limiting is an AP system (availability over consistency):
- We prefer over-admitting slightly to blocking legitimate traffic.
- Redis Cluster provides partition tolerance with eventual consistency.
- During a partition, each side of the partition rate-limits independently (potentially allowing 2x the global limit temporarily).
- This is acceptable for rate limiting (unlike distributed locks, where consistency is critical).

**Fail-Open vs Fail-Closed Decision Matrix:**

| Scenario | Fail-Open | Fail-Closed | Recommendation |
|----------|-----------|-------------|----------------|
| Redis down, API traffic | Allow traffic | Block all traffic | **Fail-open** (don't take down the platform because rate limiter is down) |
| Redis down, IPMI operations | Allow with local limit | Block IPMI | **Fail-closed** (hardware damage possible) |
| Config not loaded | Use defaults | Reject until loaded | **Fail-open** with conservative defaults |
| Sidecar not ready | Bypass rate limit | Hold requests | **Fail-open** (platform availability > rate limiting) |

---

## 10. Observability

### Key Metrics

| # | Metric | Type | Alert Threshold | Why |
|---|--------|------|-----------------|-----|
| 1 | `ratelimit.checks.total` | Counter | N/A | Traffic volume |
| 2 | `ratelimit.denied.rate` | Gauge | > 10% sustained | Too many legitimate requests blocked |
| 3 | `ratelimit.latency.p99` | Histogram | > 5ms | Rate limiter adding too much latency |
| 4 | `ratelimit.redis.errors` | Counter | > 0.1% | Redis connectivity issues |
| 5 | `ratelimit.redis.latency.p99` | Histogram | > 3ms | Redis performance degradation |
| 6 | `ratelimit.sidecar.sync.failures` | Counter | > 5/min | Sync loop broken |
| 7 | `ratelimit.over_admission.estimate` | Gauge | > 15% | Distributed counting too inaccurate |
| 8 | `ratelimit.policy.reload.errors` | Counter | > 0 | Config management issues |
| 9 | `ratelimit.adaptive.limit.current` | Gauge | < 20% of base | Downstream severely degraded |
| 10 | `ratelimit.keys.active` | Gauge | > 2M | Approaching Redis memory limits |
| 11 | `ratelimit.fail_open.events` | Counter | > 0 | Rate limiter operating in degraded mode |
| 12 | `ratelimit.burst.utilization` | Gauge | sustained > 80% | Burst budget constantly depleted |
| 13 | `ratelimit.quota.utilization` by tenant | Gauge | > 90% | Tenant approaching daily quota |
| 14 | `ratelimit.429_responses` by endpoint | Counter | sudden spike | Policy change may be incorrect |

---

## 11. Security

| Threat | Mitigation |
|--------|------------|
| Rate limit bypass (forged headers) | Don't trust `X-Forwarded-For` from untrusted sources; use authenticated tenant ID from mTLS/JWT |
| Rate limit enumeration (discovering other tenants' limits) | Per-tenant isolation in responses; only return own limits |
| Rate limit as DoS vector (flood with unique keys) | Limit unique key creation rate; cap total keys per tenant |
| Admin API abuse (setting limits to 0) | RBAC on policy management; require approval for critical policy changes |
| Sidecar bypass (direct pod access) | NetworkPolicy: only allow traffic through sidecar; mutual TLS |
| Redis data manipulation | Redis AUTH; encrypted connections; network isolation |

---

## 12. Incremental Rollout Strategy

### Phase 1: Shadow Mode (Week 1-2)
- Deploy rate limiter in "log-only" mode: check limits but never reject.
- Log would-be rejections with full context.
- Validate accuracy: compare expected vs actual rates.

### Phase 2: Soft Limits (Week 3-4)
- Enable rate limiting on non-critical APIs (health checks, status endpoints).
- Return `429` with generous limits (2x expected peak).
- Monitor for false positives.

### Phase 3: Production Limits (Week 5-8)
- Enable on API gateway for all external-facing APIs.
- Start with conservative limits (1.5x observed peak).
- Gradually tighten to target limits.
- Enable adaptive rate limiting for internal service-to-service calls.

### Phase 4: Enforcement (Week 9-12)
- Strict enforcement on all APIs.
- Enable tenant quota management.
- Remove "shadow mode" code.

**Rollout Q&A:**

**Q1: How do you determine the right rate limit value for a new API?**
A: (1) Analyze historical traffic: p99 rate per tenant over the last 30 days. (2) Set initial limit at 2x p99 (generous). (3) Monitor for 1 week. (4) Tighten to 1.5x p99. (5) Publish the limit in API documentation. (6) Provide a process for tenants to request limit increases.

**Q2: What if rate limiting causes an outage?**
A: Feature flag to disable rate limiting globally or per-API in < 1 minute. All rate limiters default to fail-open. The rate limiter should never be the cause of an outage — it's a safety mechanism, not a critical path component.

**Q3: How do you communicate rate limit changes to tenants?**
A: (1) API documentation with published limits. (2) Rate limit headers in every response. (3) Dashboard showing each tenant's usage vs limits. (4) Email notification 30 days before limit changes. (5) Gradual enforcement: warn (header only) -> soft reject (503 with clear message) -> hard reject (429).

**Q4: How do you handle rate limits during incident response?**
A: Incident response tooling can temporarily raise limits (or disable rate limiting) for specific APIs/tenants. This is an admin action with audit logging. Example: during a mass provisioning event, temporarily raise the bare-metal provisioning rate limit for the affected tenant.

**Q5: How do you test rate limiting in staging?**
A: (1) Load test with synthetic traffic at 2x production peak. (2) Chaos test: kill Redis, verify fail-open behavior. (3) Accuracy test: send exactly N+1 requests at the limit boundary; verify the N+1th is rejected. (4) Multi-tenant test: verify tenant isolation (tenant A's traffic doesn't affect tenant B's limits).

---

## 13. Trade-offs & Decision Log

| # | Decision | Options Considered | Selected | Rationale |
|---|----------|-------------------|----------|-----------|
| 1 | Default algorithm | Token bucket, sliding window, fixed window, leaky bucket | Token bucket | Best burst handling; industry standard (AWS, GCP); configurable |
| 2 | Counter store | Redis, Memcached, in-memory only, DynamoDB | Redis | Lua script atomicity; TTL support; clustering; ecosystem |
| 3 | Deployment model | Centralized, sidecar, library, service mesh | Sidecar + centralized hybrid | Sidecar for latency; centralized for precision |
| 4 | Failure mode | Fail-open, fail-closed | Fail-open (default, configurable) | Rate limiter failure should not cause platform outage |
| 5 | Sync interval | 10ms, 50ms, 100ms, 500ms | 100ms default | Balance accuracy (~5% over-admit) vs Redis load |
| 6 | Multi-tier limits | Single tier, multiple tiers | Multiple (per-second + per-day) | Different concerns at different timescales |
| 7 | Adaptive limiting | Static only, AIMD, ML-based | AIMD | Proven, simple, stable convergence |
| 8 | Configuration | Hardcoded, file-based, config service | Config service (etcd) with watch | Hot reload; version-controlled; auditable |
| 9 | Key cardinality limit | Unlimited, per-tenant cap, global cap | Per-tenant cap (10K keys) | Prevent Redis memory exhaustion from cardinality explosion |
| 10 | Rate limit headers | Standard (X-RateLimit-*), custom, none | Standard (IETF draft) | Interoperability; client library support |

---

## 14. Agentic AI Integration

### AI-Driven Rate Limit Management

**1. Automatic Limit Discovery:**
```python
class RateLimitDiscoveryAgent:
    """
    Analyzes traffic patterns to recommend rate limits for new APIs.
    """

    def recommend_limits(self, api_endpoint, window="30d"):
        # Analyze historical traffic patterns
        traffic = self.metrics.get_request_rates(
            endpoint=api_endpoint, window=window
        )

        p50_rate = percentile(traffic.per_second, 50)
        p99_rate = percentile(traffic.per_second, 99)
        max_burst = max(traffic.per_second)

        return {
            "endpoint": api_endpoint,
            "recommendations": {
                "per_second": {
                    "limit": int(p99_rate * 2),
                    "burst": int(max_burst * 1.5),
                    "rationale": f"2x p99 ({p99_rate}/sec); "
                                 f"burst = 1.5x observed max ({max_burst})"
                },
                "per_minute": {
                    "limit": int(p99_rate * 60 * 1.5),
                    "rationale": "1.5x sustained p99 per minute"
                },
                "per_day": {
                    "limit": int(traffic.daily_total.p99 * 1.3),
                    "rationale": "1.3x p99 daily volume"
                }
            },
            "tenant_breakdown": self._per_tenant_analysis(
                api_endpoint, window
            ),
            "confidence": "HIGH" if len(traffic.samples) > 1000 else "LOW"
        }
```

**2. Anomaly Detection for Rate-Limited Traffic:**
```
Agent monitors:
  - Sudden spike in 429 responses for a specific tenant
  - Rate limit never being hit (limit too generous; reduce waste?)
  - New traffic pattern that doesn't match any policy (unprotected API)

Actions:
  - "Tenant X hit rate limit 500 times in last hour — new batch job?
    Recommend temporary limit increase to 2x for 24h."
  - "API /v1/internal/health has no rate limit policy — adding default."
  - "Adaptive limit for MySQL pool dropped to 10% of base —
    investigating downstream MySQL latency."
```

**3. AI-Driven Capacity Planning:**
```
Agent correlates:
  - Rate limit headroom (current usage vs limit) per tenant
  - Growth trends (linear, exponential, seasonal)
  - Infrastructure capacity (Redis memory, CPU)

Produces quarterly report:
  - "At current growth, tenant-42 will hit daily quota in 6 weeks.
    Recommend proactive increase from 100K to 150K/day."
  - "Redis Cluster will need 2 more shards in Q3 based on key
    growth projections."
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through a complete rate-limit check from client to response.**
A: (1) Client sends HTTP request to API gateway. (2) Gateway extracts tenant ID from JWT, API path from URL. (3) Gateway calls rate-limit sidecar (or in-process library). (4) Sidecar checks local token bucket — if tokens available, allows immediately. (5) If local budget depleted, sidecar calls Redis with Lua script: atomic increment + limit check. (6) Redis returns allow/deny + remaining count. (7) Sidecar returns result to gateway. (8) Gateway sets `X-RateLimit-*` headers. (9) If denied, returns 429 with `Retry-After`. (10) If allowed, forwards to backend service.

**Q2: Why is the sliding window counter better than the fixed window counter?**
A: Fixed window allows 2x burst at the window boundary (last second of window N + first second of window N+1). Sliding window counter uses a weighted average of current and previous window counts, estimating the request count in the true sliding window. Cloudflare measured 0.003% error rate — negligible. Memory is the same (2 counters vs 1).

**Q3: How does Redis Cluster handle rate-limit key sharding?**
A: Redis Cluster hashes keys to 16,384 slots distributed across shards. All rate-limit keys for the same tenant/API land on the same shard (same key prefix). Lua scripts are atomic per shard. Cross-shard Lua scripts aren't supported — this is fine because each rate-limit check is per-key.

**Q4: What is the difference between rate limiting and throttling?**
A: Rate limiting rejects excess requests immediately (429). Throttling delays excess requests (queues them and processes at the allowed rate). Throttling provides better client experience (eventual success) but uses server resources (queue memory). For infrastructure APIs, rate limiting is preferred — clients should handle 429 with backoff.

**Q5: How do you implement per-IP rate limiting when clients are behind a load balancer?**
A: Use `X-Forwarded-For` header, but only from trusted load balancers (verify the source IP of the request is a known LB). Take the leftmost IP in the chain that isn't a known internal IP. For infrastructure platforms, per-tenant-ID limiting is more appropriate than per-IP.

**Q6: How does Kafka implement producer rate limiting?**
A: Kafka brokers have a quota system. Each producer has a byte-rate and request-rate quota. When exceeded, the broker delays the response (throttle) rather than rejecting. The delay is returned in the response, and the client sleeps accordingly. This is "cooperative throttling" — the broker tells the client to slow down rather than forcibly rejecting.

**Q7: What is the "leaky bucket as a meter" vs "leaky bucket as a queue" distinction?**
A: Leaky bucket as a meter: counts requests; if the count exceeds the bucket depth, reject. Similar to token bucket but without burst capability. Leaky bucket as a queue: requests are placed in a FIFO queue and processed at a fixed rate. The queue smooths traffic but adds latency. For rate limiting, the meter model is typical. For traffic shaping, the queue model is typical.

**Q8: How do you handle rate limiting for WebSocket connections?**
A: Three levels: (1) Connection rate limit: max new WebSocket connections per second. (2) Message rate limit: max messages per second per connection. (3) Bandwidth limit: max bytes per second per connection. Enforce at the WebSocket gateway or in Envoy's HTTP connection manager.

**Q9: How does Google Cloud implement rate limiting?**
A: Google uses a distributed token bucket with a central quota server. Each frontend has a local token cache. The frontend pre-fetches tokens from the quota server in bulk (e.g., request 100 tokens at a time). This amortizes the network cost. If the quota server is unreachable, the frontend uses its remaining local tokens. This is similar to our sidecar model.

**Q10: How do you prevent one bad tenant from affecting others?**
A: Tenant isolation. Each tenant has independent rate-limit keys and counters. A tenant exceeding their limit gets 429 responses, but other tenants are unaffected. At the infrastructure level, use separate Redis keyspaces or even separate Redis clusters for critical tenants.

**Q11: How do you handle rate limiting for batch APIs?**
A: Batch APIs (e.g., "create 100 servers") should count each item in the batch against the rate limit, not just the single request. The `hits` or `cost` parameter in the rate-limit check = batch size. If the batch is too large for the remaining budget, reject the entire batch (not partial) and return the max allowed batch size.

**Q12: What's the cost of adding a rate limiter to every request?**
A: With sidecar (local check): < 0.1ms per request, negligible. With Redis check: 0.5-2ms per request. At 500K req/sec with Redis, that's 250K-1M Redis ops/sec. Redis Cluster handles this easily. The main cost is operational (managing Redis, configuring policies), not performance.

**Q13: How do you handle clock differences between sidecar instances for sliding window?**
A: Use Redis server time (`redis.call('TIME')`) for window boundary calculations, not local clock. For pure local checks, small clock differences (< 1s with NTP) cause negligible error in the sliding window counter. For token bucket, the refill calculation uses monotonic time deltas (immune to clock changes).

**Q14: What is "priority-based rate limiting"?**
A: Not all requests are equal. A `DELETE /server` might be higher priority than `GET /servers`. During overload, reject lower-priority requests first. Implementation: multiple token buckets — a "premium" bucket and a "standard" bucket. Premium requests check the premium bucket first; standard requests check the standard bucket. When the standard bucket is empty, premium requests still succeed.

**Q15: How do you implement global rate limits (across all tenants)?**
A: A separate rate-limit key with no tenant qualifier (e.g., `rl:global:/v1/servers`). All tenants share this budget. This protects shared infrastructure (e.g., max 10K VM provisions per minute across the entire platform, regardless of tenant). Global limits are typically enforced centrally (not in sidecars) for accuracy.

**Q16: How would you add rate limiting to an existing service without code changes?**
A: (1) Service mesh sidecar injection: add Envoy sidecar with rate limit filter. Zero code changes. (2) API gateway: add rate limit policy at the gateway layer. (3) Network-level: iptables rate limiting (crude but effective for DDoS). For infrastructure services, the sidecar approach is preferred because it's language-agnostic and policy-driven.

---

## 16. References

1. Veeraraghavan, K. et al. (2016). *Maelstrom: Mitigating Datacenter-level Disasters by Draining Interdependent Traffic Safely and Efficiently*. OSDI. (Facebook rate limiting and traffic management)
2. Cloudflare blog. *How we built rate limiting capable of scaling to millions of domains*. https://blog.cloudflare.com/counting-things-a-lot-of-different-things/
3. Stripe blog. *Scaling your API with rate limiters*. https://stripe.com/blog/rate-limiters
4. IETF Draft. *RateLimit Fields for HTTP*. https://datatracker.ietf.org/doc/draft-ietf-httpapi-ratelimit-headers/
5. Lyft. *ratelimit: Go/gRPC rate limit service*. https://github.com/envoyproxy/ratelimit
6. Redis documentation. *Rate limiting with Redis*. https://redis.io/docs/manual/patterns/rate-limiting/
7. Envoy documentation. *Rate limit filter*. https://www.envoyproxy.io/docs/envoy/latest/configuration/http/http_filters/rate_limit_filter
8. Amazon. *API Gateway throttling*. https://docs.aws.amazon.com/apigateway/latest/developerguide/api-gateway-request-throttling.html
9. Google. *API Design Guide: Quotas*. https://cloud.google.com/apis/design/design_patterns#quota
10. Jacobson, V. (1988). *Congestion Avoidance and Control*. ACM SIGCOMM. (Original AIMD work for TCP)
