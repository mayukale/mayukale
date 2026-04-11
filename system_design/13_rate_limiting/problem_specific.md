# Problem-Specific Design — Rate Limiting (13_rate_limiting)

## API Rate Limiter

### Unique Functional Requirements
- Per-configurable-dimension enforcement: user_id, API key, IP, endpoint glob, tenant, global
- Three tiers: free (100 req/min), pro (1,000 req/min), enterprise (10,000 req/min)
- Standard response headers on every request: X-RateLimit-Limit, X-RateLimit-Remaining, X-RateLimit-Reset, Retry-After
- HTTP 429 with JSON error body on rate limit exceeded
- Per-second burst limit + per-minute sustained limit applied simultaneously
- Rules engine: admins define rules with glob endpoint matching, priority ordering
- Soft limit (80% warning header) + hard limit (429 block)
- 500K RPS throughput target; 8M RPS peak (3× headroom with 16M ops/s to Redis)

### Unique Components / Services
- **Rate Limit Middleware** (in-process per gateway node): extracts client identity, fetches rules from in-process LRU cache, executes Redis Lua script, sets response headers; zero external network hops on cache hit
- **In-Process Rules Cache** (per gateway pod): LRU cache refreshed from Redis every 30 s; prevents rules DB round-trip per request
- **Redis Cluster (Counter Store)**: authoritative for sliding window counters; 6 shards; key TTL = window_seconds × 2; 640 MB for 4 M keys; 16 M ops/s peak
- **Rules Store (MySQL)**: source of truth for all rate limit rules and tiers; `rate_limit_rules` table + `client_overrides` table; small dataset (< 20 MB)
- **Rules Admin API**: internal CRUD for rules management; 30 s cache propagation

### Unique Data Model
- **Redis counter key**: `rl:{dimension}:{client_value}:{window_seconds}:{window_bucket}`; `window_bucket = floor(unix_timestamp / window_seconds)`; TTL = `window_seconds × 2`
- **Redis rules cache keys**: `rules:all`, `rules:tier:{tier_name}`, `rules:override:{client_type}:{val}`; 60 s TTL
- **MySQL rate_limit_rules**: id, rule_name, dimension ENUM(api_key/user_id/ip/tenant/endpoint/global), tier, endpoint_glob, limit_count, window_seconds, burst_limit, algorithm ENUM(sliding_window_counter/token_bucket), priority, is_active; indexes on (dimension, tier), (is_active), (priority)
- **MySQL client_overrides**: client_type, client_value, rule_id FK, override_limit, is_exempt, expires_at; UNIQUE not required (FK to rule handles conflict)

### Algorithms

**Sliding Window Counter (Primary):**
```lua
current_bucket = floor(now / window)
prev_bucket = current_bucket - 1
prev_count = GET(prev_key) or 0
current_count = GET(current_key) or 0
elapsed = now - (current_bucket * window)
weight = 1.0 - (elapsed / window)
estimated = floor(prev_count * weight) + current_count
if estimated >= limit: RETURN REJECT
INCR(current_key); EXPIRE(current_key, window * 2)
RETURN ALLOW
```
- O(1) memory; < 1% error; adopted by Cloudflare, Stripe, Kong

**Token Bucket (Secondary — burst-tolerant APIs):**
- tokens = min(capacity, tokens + elapsed × (limit/window)); consume 1 token per request
- O(1) memory; allows controlled bursts via bucket_size

**Combined Lua Script (atomic sliding window + burst check):**
- Keys: [current_key, prev_key, burst_key]
- First checks burst_key (1 s window fixed counter); if burst_limit exceeded → reject with -1
- Then checks sliding window; if window limit exceeded → reject with -2
- Both checks pass: INCR current_key + INCR burst_key; EXPIRE burst_key 1

**Algorithm Comparison:**

| Algorithm | Memory/Client | Burst | Boundary Accuracy | Complexity |
|---|---|---|---|---|
| Fixed Window | O(1) ~160B | 2× burst | Poor | Very Low |
| Sliding Window Log | O(N) ~50KB | No | Perfect | Medium |
| **Sliding Window Counter** | **O(1) ~320B** | **Minimal** | **< 1% error** | **Medium** |
| Token Bucket | O(1) ~80B | Configurable | Excellent | Medium-High |
| Leaky Bucket | O(queue) | No | Perfect (output) | Very High |

### Scale Numbers
- 10 M registered consumers; 2 M daily active; peak: 8 M RPS (3× average)
- Free tier: 1.6M clients × 5 req/min avg = 133K RPS
- Pro tier: 360K clients × 200 req/min avg = 1.2M RPS
- Enterprise tier: 40K clients × 2,000 req/min avg = 1.33M RPS
- Total average: 2.67M RPS; peak 8M RPS
- Rate limit checks: 16M ops/s at peak (2 checks per request: per-minute + per-second)
- Audit log (429 events): ~8K/s × 500 B = 4 MB/s; ~14 GB/day compressed

### Unique Scalability / Design Decisions
- **Two Redis keys per client per window (not one)**: current_bucket + prev_bucket; atomic Lua reads both in one round-trip; no need for ZADD + ZRANGEBYSCORE (sliding log) which would be O(N) per request at high request rates
- **In-process LRU for rules cache**: rules change rarely (< 1,000 updates/day); storing rules in-process eliminates one Redis round-trip per request; 30 s staleness acceptable — same as cache miss rate
- **Dimension hierarchy for rule matching**: rules evaluated in priority order; lower priority number = higher priority; most-specific rule (api_key > user_id > ip > global) wins; avoids scanning all rules for every request via priority-indexed lookup

### Key Differentiator
API Rate Limiter's uniqueness is its **multi-dimensional rule engine with configurable tiers**: serving 8M RPS across free/pro/enterprise tiers via per-dimension enforcement (user_id, API key, IP, endpoint glob, tenant), with a full-featured admin API for hot rule updates, standard HTTP 429 headers (X-RateLimit-*), and a Lua-script-based combined burst + sliding window counter in a single Redis atomic operation — designed as a standalone distributed rate limit service, not an embedded library.

---

## Throttling Service

### Unique Functional Requirements
- Service-to-service throttling within a microservices fleet (500 services × 20 instances = 10,000 service instances)
- Total in-fleet RPS: 50M RPS (10K instances × 5K RPS each)
- Priority classification P0 (critical, 5%)–P3 (batch, 20%) with absolute priority for P0
- Adaptive throttling: dynamically adjust token bucket rate based on downstream health (AIMD)
- Starvation prevention: P3 guaranteed processing within 2 minutes
- Graceful degradation: cached response / default value / partial response instead of 503
- No Redis on hot path — all decisions in-process (zero network hops)

### Unique Components / Services
- **Priority Classifier** (in-process): assigns P0–P3 based on request metadata; < 0.1 ms
- **Admission Controller** (in-process): check health + queue depth + circuit breaker + quota → allow/queue/shed
- **Priority Queue (MLFQ)** (in-process): 4-level bounded queue; per-priority depth limits; dequeue in priority order; starvation prevention via max wait timestamps
- **Outbound Throttle Client** (in-process per downstream): token bucket + circuit breaker + client-side throttle + quota; checks in order: circuit → client-side → quota → token bucket
- **Adaptive Threshold Engine** (control plane): computes optimal per-service rates using EWMA-smoothed health metrics (error rate, p99 latency); AIMD algorithm
- **Policy Store (PostgreSQL)**: source of truth for throttle policies; JSONB for complex configs
- **Metrics Aggregator (Prometheus/TSDB)**: aggregates per-instance health metrics; 1-hour history for adaptive algorithm
- **Push Gateway (gRPC streams)**: maintains persistent streams to all 10K service instances; pushes threshold updates < 5 s; fallback to 5 s REST polling if stream fails

### Algorithms

**AIMD (Additive Increase, Multiplicative Decrease) — Primary:**
```python
if is_overloaded(smoothed_error_rate, smoothed_latency_ms):
    current_rps = max(min_rps, current_rps * 0.5)  # halve
else:
    current_rps = min(max_rps, current_rps + max_rps * 0.05)  # +5%

# EWMA smoothing (alpha=0.3):
smoothed = alpha * observed + (1-alpha) * smoothed
```
- Industry-proven (TCP AIMD since 1988); stable; EWMA prevents oscillation
- Additive increase: +5% of max_rps per healthy second
- Multiplicative decrease: halve on overload (50% factor)

**Google Client-Side Throttling (Secondary — fallback when control plane unavailable):**
```go
probability = max(0, (requests - K * accepts) / (requests + 1))
// K=2; when 90% rejected, pre-reject 80%
```
- Self-calibrating; no control plane dependency; O(1) in-process tracking

**Multi-Level Priority Queue with Starvation Prevention:**
```go
maxDepths = [P0: totalMax/2, P1: totalMax/2, P2: totalMax/4, P3: totalMax/4]
starvationLimits = [P0: ∞, P1: 5000ms, P2: 30000ms, P3: 120000ms]
// On dequeue: check starvation first (oldest low-priority may get promoted)
// Then dequeue from highest non-empty priority
```

**Circuit Breaker State Machine:**
- CLOSED → OPEN (error_rate > threshold within window_seconds)
- OPEN → HALF_OPEN (after recovery_time_seconds)
- HALF_OPEN → CLOSED (1% probe traffic succeeds) or OPEN (probe fails)

**Circuit Breaker Tuning by Service Type:**
| Service Type | Error Threshold | Latency Threshold | Window | Recovery Time |
|---|---|---|---|---|
| Critical payment | 1% | 200 ms | 5s | 60s |
| Core API | 5% | 500 ms | 10s | 30s |
| ML/Recommendations | 10% | 2,000 ms | 30s | 15s |
| Background batch | 20% | 10,000 ms | 60s | 10s |

### Unique Data Model
- **In-process Go structs**: `TokenBucket` (Rate, BurstSize, Tokens, LastRefill, MinRate, MaxRate, sync.Mutex), `CircuitBreaker` (State, ErrorRate, P99LatencyMs, RequestCount, ErrorCount, LastStateChange), `QuotaCounter` (HourlyCount, DailyCount, HourlyLimit, DailyLimit), `PriorityQueue` (4 channels + depth limits)
- **PostgreSQL throttle_policies**: caller_service, callee_service, callee_endpoint, max_rps, burst_rps, priority_shed_thresholds JSONB ({"P3": 0.8, "P2": 0.9, "P1": 0.95}), adaptive_enabled, circuit_breaker_config JSONB, quota_config JSONB, graceful_degradation_config JSONB, version (optimistic lock)

### Scale Numbers
- 500 services, 20 instances each = 10,000 service instances; 50M in-fleet RPS
- 1M throttled requests/sec (2% of 50M); 0.5M shed/sec
- Control plane: 500 adaptive updates/sec; 100K metrics/sec; 500 threshold pushes/sec
- Storage: 40 MB in-process token bucket state (10K instances × 20 buckets × 200 bytes)
- Audit log: ~26 TB/day → 2.6 TB/day compressed

### Unique Scalability / Design Decisions
- **In-process SDK over sidecar proxy (Envoy ext_authz)**: sidecar adds 2–5 ms per request (loopback TCP + ext_authz RPC); in-process < 0.5 ms; at 50M RPS, 4 ms × 50M = 200M ms/s = significant throughput cost; in-process eliminates network hop entirely
- **gRPC persistent stream (push) over REST polling**: push achieves < 5 s propagation; REST polling at 1 s interval = 50M × 1 HTTP/s = 50M baseline RPS just for config sync; persistent stream uses H2 multiplexing with O(1) message delivery
- **Priority queue over weighted fair queue**: MLFQ gives P0 absolute priority (critical payment operations never shed); WFQ would allow P0 to be slowed by P3 load; starvation prevention (120s for P3) handles batch jobs without blocking higher-priority indefinitely

### Key Differentiator
Throttling Service's uniqueness is its **adaptive service-mesh throttling with in-process AIMD**: zero-network-hop decisions (< 0.5 ms), 4-level priority queue (P0–P3) with MLFQ starvation prevention, AIMD dynamic rate adjustment based on EWMA-smoothed health metrics, Google's client-side throttle as fallback, and graceful degradation (cached/default/partial response) — designed for service-to-service throttling within a fleet, not external API rate limiting.
