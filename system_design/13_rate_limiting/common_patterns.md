# Common Patterns — Rate Limiting (13_rate_limiting)

## Common Components

### Redis as Counter Store
- Both use Redis for storing rate limit counters with sub-millisecond latency
- In api_rate_limiter: Redis Cluster (Counter Store) holds sliding window counters; key format `rl:{dimension}:{client_value}:{window_seconds}:{window_bucket}`; TTL = window_seconds × 2; 640 MB for 4M keys
- In throttling_service: in-process token buckets (not Redis-based for hot path); Redis used via control plane for threshold distribution; in-process is primary for zero-network-hop decisions

### PostgreSQL for Configuration (Rules / Policies)
- Both use PostgreSQL as the durable source of truth for rate limit rules and throttle policies
- In api_rate_limiter: `rate_limit_rules` table (dimension, tier, endpoint_glob, limit_count, window_seconds, burst_limit, algorithm, priority, is_active) + `client_overrides` table for per-client exceptions
- In throttling_service: `throttle_policies` table (caller_service, callee_service, max_rps, burst_rps, priority_shed_thresholds JSONB, circuit_breaker_config JSONB, quota_config JSONB); `priority_rules` table for P0–P3 classification

### Kafka for Async Audit Logging
- Both use Kafka to decouple 429/throttle event logging from the hot request path
- In api_rate_limiter: `Audit Topic` Kafka topic; 429 events (8M RPS × 0.1% rejection = 8K/s) → S3 + Athena for long-term audit; ~14 GB/day compressed
- In throttling_service: `Throttle Events` Kafka topic; 1M throttled requests/sec × 300 bytes → 300 MB/s; S3 Intelligent Tiering + Athena for 90-day hot, long-term cold

### In-Process Caching for Configuration
- Both cache rules/policies in-process to eliminate network round-trips for the hot path
- In api_rate_limiter: LRU cache per gateway node (30 s refresh interval from Redis rules cache); `rules:all`, `rules:tier:{tier}`, `rules:override:{client}:{val}` keys; 60 s TTL in Redis rules cache
- In throttling_service: in-process token buckets, circuit breaker state, quota counters all held in process memory; control plane pushes updates via gRPC streams; no Redis round-trip on hot path

### Sliding Window Counter / Token Bucket Algorithms
- Both support the same core algorithms for rate limiting, with different primary selection
- api_rate_limiter PRIMARY: Sliding Window Counter (O(1) memory, < 1% error at boundaries); SECONDARY: Token Bucket (burst-tolerant APIs); Sliding Window Log rejected (too much memory)
- throttling_service PRIMARY: Token Bucket with dynamic rate (AIMD updates rate in real time); SECONDARY: Google's Client-Side Throttle (accept-rate formula for fallback when control plane unavailable)

### Circuit Breaker Pattern
- Both implement circuit breakers, but at different levels of the stack
- In api_rate_limiter: circuit breaker on backing store (Redis) failure → fail-open (allow traffic); local fallback counter
- In throttling_service: explicit circuit breaker state machine (CLOSED → OPEN → HALF_OPEN) per downstream dependency; OPEN after 5 failures/30 s; HALF_OPEN after 60 s recovery; error_threshold + latency_threshold tunable per service type

### Latency p99 Targets (Sub-Millisecond for Hot Path)
- Both require sub-millisecond overhead on the hot request path
- api_rate_limiter: p99 < 3 ms (Redis cache hit); p99 < 10 ms (cache miss fallback)
- throttling_service: p99 < 0.5 ms (in-process token bucket); p99 < 3 ms (threshold sync from control plane async)

### Graceful Degradation on Backing Store Failure
- Both fail-open when the rate limit backing store is unavailable (availability > perfect accuracy)
- api_rate_limiter: if Redis unavailable → fail-open (allow traffic); 99.99% availability requirement
- throttling_service: if control plane down → continue with cached thresholds; client-side throttle + circuit breakers remain active

## Common Databases

### Redis
- Both; counter storage (api_rate_limiter) or auxiliary caching (throttling_service); sub-millisecond operations; Lua scripting for atomic check-and-increment

### PostgreSQL
- Both; ACID source of truth for rules/policies; JSONB for flexible configuration; row-level locking for concurrent updates; small datasets (< 1 MB rules/policies)

### Kafka + S3/Athena
- Both; async audit trail for rejected/throttled requests; long-term queryable archive

## Common Communication Patterns

### Lua Scripting for Atomic Operations
- Both use Redis Lua scripts for atomicity on multi-step counter operations
- api_rate_limiter: Lua script combines: prev_key GET, current_key GET, estimated_count calculation, INCR current_key, EXPIRE — all atomic in one network round-trip
- throttling_service: similar patterns in token bucket refill + consume atomicity

### Admin API for Runtime Configuration
- Both expose internal REST APIs for managing rules/policies without restart
- api_rate_limiter: `GET/POST/PUT/DELETE /v1/ratelimit/rules`; `POST /v1/ratelimit/overrides`; rules take effect within 30 s (cache refresh interval)
- throttling_service: `GET/POST/PUT /v1/throttle/policies`; `POST /v1/throttle/override`; emergency overrides propagate within 5 s via gRPC push

## Common Scalability Techniques

### Horizontal Scaling via Stateless API Layer
- Both scale API gateway / service nodes horizontally; state externalized to Redis/in-process

### Eventual Consistency Accepted (< 100 ms propagation window)
- Both accept that a rate limit change propagates to all nodes within < 100 ms (not synchronous) — prevents distributed coordination overhead

## Common Deep Dive Questions

### Which rate limiting algorithm should you use?
Answer: Depends on requirements. Fixed Window Counter: O(1), simplest, but allows 2× burst at window boundaries — only for simple internal limits. Sliding Window Log: perfect accuracy, but O(N) memory at limit (50 KB per client) — impractical at millions of clients. Sliding Window Counter: O(1) memory, < 1% error via weighted approximation — default for high-scale APIs (used by Cloudflare, Stripe, Kong). Token Bucket: O(1), allows controlled bursts (bucket_size > limit/window) — for APIs where burst is acceptable (batch endpoints). AIMD Token Bucket: O(1), dynamically adjusts rate based on downstream health — for service-to-service throttling where load shedding is needed.
Present in: api_rate_limiter, throttling_service

### How do you prevent thundering herd when rate limits expire?
Answer: For token bucket / fixed window: exponential backoff with jitter on client side (Retry-After header value + random delay). For service throttling: priority queue (P0–P3) absorbs burst by shedding lower-priority requests first; P0 always gets through. For idempotent requests: queue failed requests and replay when window resets rather than dropping. For sliding window: the weighted approximation inherently smooths boundary effects — no spike at reset.
Present in: api_rate_limiter, throttling_service

## Common NFRs

- **Overhead per request**: < 3–5 ms p99 for rate limit check (including Redis round-trip)
- **In-process decision**: < 0.1–0.5 ms p99 (no network hop)
- **Availability**: 99.99%; fail-open on backing store failure
- **Consistency**: eventual; < 100 ms propagation window acceptable
- **Accuracy**: < 1% over-admission at burst boundaries for sliding window counter
- **Auditability**: log every rejected/throttled request with client identity, rule matched, timestamp
- **Configuration hot-reload**: rules/policies take effect within 5–30 s without service restart
