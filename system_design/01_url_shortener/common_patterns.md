# Common Patterns — URL Shortener (01_url_shortener)

## Common Components

### Redis Cluster
- Used as: primary cache, rate limit counter, pre-generated key/code pool, negative cache, write counters
- In url_shortener: caches `short_code → long_url` mappings with 24h TTL, stores click count INCR counters, holds pre-generated code pool (SPOP), rate limit tokens via Lua token bucket
- In pastebin: caches paste metadata (512 bytes each) and small content (< 64 KB), holds pre-generated paste key pool (SPOP), stores delete tokens for anonymous pastes, view count INCR counters

### PostgreSQL (Primary Database)
- Used as: source of truth for all structured metadata, user accounts
- In url_shortener: stores `urls` table (short_code PK, long_url, user_id, expires_at, is_active) and `users` table; connection pooled via PgBouncer
- In pastebin: stores `pastes` table (paste_key, user_id, sha256_checksum, visibility, expires_at, size_bytes) and `users` table; no content stored here (goes to S3)

### CloudFront CDN
- Used as: edge cache for responses, TLS termination, DDoS/WAF protection
- In url_shortener: caches HTTP 301/302 redirect responses at ~400+ PoPs; 301s cached indefinitely, 302s with 1-hour TTL; Origin Shield coalesces cache misses
- In pastebin: caches GET /paste/<key> and GET /raw/<key> responses; public pastes with `s-maxage=2592000` (30 days); password-protected pastes with `Cache-Control: private, no-store`

### Key/Code Generator Service
- Used as: decoupled service maintaining a pre-generated random key pool in Redis
- In url_shortener: 7-char base62 codes (62^7 = 3.5 trillion); pool of 10M codes; refills every 60s via background job; fallback is random generation with `INSERT ... ON CONFLICT DO NOTHING`
- In pastebin: 8-char base62 keys (62^8 ≈ 218 trillion); pool target 5M keys; refills when < 500K; distributed lock via Redis to prevent double-refill

### Kafka + Flink + ClickHouse Analytics Pipeline
- Used as: async analytics ingestion and querying
- In url_shortener: Kafka topic `click-events` (24 partitions, partition key: short_code, 7-day retention); Flink enriches events with MaxMind GeoIP2, 1-minute tumbling windows; ClickHouse MergeTree engine for time-series queries
- In pastebin: Kafka publishes view count events on every paste read; Flink aggregates view counts; ClickHouse stores `paste_views` time-series for per-day trends, referrer, country analytics

### L7 Load Balancer
- Used as: path-based HTTP routing to stateless service instances
- In url_shortener: routes read vs write traffic; no sticky sessions; health checks every 5s
- In pastebin: routes /api/* → Write Service; /paste/* and /raw/* → Read Service

### Route53 (DNS)
- Used as: DNS resolution with health-check-based failover
- In url_shortener: maps short.ly to CDN/LB IPs; low TTL (~60s) for fast failover
- In pastebin: resolves pastebin.io to CDN; health-check-based failover to secondary region

## Common Databases

### PostgreSQL
- Primary transactional database in both problems
- Stores all structured metadata, user accounts, and URL/paste index
- Runs with PgBouncer or equivalent connection pooling
- Primary + 2 async read replicas per shard

### ClickHouse
- Columnar OLAP database for analytics queries in both problems
- MergeTree engine; ordered by (primary_key, timestamp)
- Receives data from Flink stream processor
- Handles time-series aggregations, dashboard queries, referrer and geo breakdowns

## Common Queues / Event Streams

### Kafka
- Event streaming backbone for the analytics pipeline in both problems
- Partition key is the primary entity key (short_code or paste_key) to co-locate events
- 7-day retention, replication factor 3
- Message format: JSON with entity key, timestamp, IP, user-agent, referrer

### Flink
- Stream processor consuming Kafka topics in both problems
- Tumbling windows for aggregation
- Enriches events (GeoIP lookup)
- Delivers batch inserts to ClickHouse with exactly-once semantics

## Common Communication Patterns

### REST over HTTP/HTTPS
- All external API traffic uses REST in both problems
- Standard CRUD endpoints for create, read, update, delete of primary entity
- Bulk/list endpoints for user's owned entities
- TLS terminated at CDN

### Authentication
- Bearer JWT (15-min expiry + refresh token in HttpOnly cookie) for web users in both problems
- `X-API-Key` header for programmatic clients in both problems
- Idempotency-Key header (UUID) for retry-safe POST requests in both problems

### Cache-Aside + Write-Through on Creation
- Both problems use cache-aside (check Redis → fallback to DB) for reads
- Both immediately populate Redis after DB write (write-through on creation) to avoid replication lag on first read

## Common Scalability Techniques

### Horizontal Stateless Scaling
- All application services are stateless in both problems
- Auto-scaling groups triggered by CPU > 60% or p99 latency thresholds
- No sticky sessions at load balancer

### Hash-Based Sharding on Primary Key
- Both plan sharding on the primary entity key (short_code or paste_key): `shard_id = crc32(key) % num_shards`
- Start with 4 shards; expand to 16 using consistent hashing to minimize data movement
- Each shard: 1 primary + 2 read replicas

### Primary-Replica Async Replication (PostgreSQL)
- One primary for writes; 2 async read replicas per shard in both problems
- Replication lag ~10-100ms same-AZ
- Write-through cache on creation mitigates lag for first read

### Negative Caching
- Both store NOT_FOUND with a short TTL (5 minutes) in Redis to prevent repeated DB lookups for missing keys
- Both store tombstones for deleted/expired entities

### LRU Eviction in Redis
- Both use `allkeys-lru` eviction policy in Redis
- Both size their Redis cluster to hold the estimated hot working set

## Common Deep Dive Questions

### How do you generate unique short codes / paste keys without collisions?
Answer: Pre-generate a large pool of random codes and store them in a Redis Set. Use SPOP to atomically pop one per request. Persist issued codes to DB with `INSERT ... ON CONFLICT DO NOTHING`. Refill the pool via a background job every 60 seconds. Fallback: generate random code on demand and retry on conflict.
Present in: url_shortener, pastebin

### How do you handle rate limiting?
Answer: Token bucket algorithm implemented as a Redis Lua script. Key format: `rl:ip:<ip>` or `rl:user:<id>`. Anonymous users: 10 ops/hour per IP; free tier: 100-1000/hour; pro API keys: 10,000/hour. Return `X-RateLimit-*` headers and HTTP 429 when exhausted.
Present in: url_shortener, pastebin

### How do you ensure idempotency for write operations?
Answer: Client sends `Idempotency-Key: <uuid>` header. Server uses Redis SET NX with 24h TTL to lock the key during processing, then stores the response. Subsequent requests with the same key return the cached response. DB insert uses `ON CONFLICT DO NOTHING`.
Present in: url_shortener, pastebin

### How do you handle hotspot/viral content?
Answer: CDN absorbs the vast majority of traffic for popular items. Immutable or long-TTL responses get cached at the edge. Auto-scaling + Redis handle remaining origin traffic. CDN Origin Shield coalesces cache misses from multiple PoPs into a single origin request.
Present in: url_shortener, pastebin

### How do you handle failover?
Answer: PostgreSQL uses active-passive with a hot standby (Patroni); RTO ~60s; RPO 0 with sync replication. Redis Cluster has automatic failover (~1-2s per shard). Application services are stateless and active-active behind the load balancer (no failover delay). S3 is inherently multi-AZ.
Present in: url_shortener, pastebin

### How do you count reads/views without adding latency?
Answer: Use Redis INCR for sub-millisecond counter updates. Batch-flush counter increments to PostgreSQL asynchronously. Simultaneously publish events to Kafka for full analytics processing via Flink → ClickHouse. Avoid synchronous DB increments on the hot read path.
Present in: url_shortener, pastebin

### How do you monitor the system?
Answer: OpenTelemetry SDK for distributed tracing (Jaeger); structured JSON logging to Elasticsearch/Datadog; metrics for p99 latency, cache hit rate, DB replication lag, QPS. 100% sampling for errors; 1% tail-based sampling for successes.
Present in: url_shortener, pastebin

## Common NFRs

- **Read availability**: 99.99% (~52 min downtime/year)
- **Write availability**: 99.9%
- **Throughput**: Handle 10x traffic spikes without manual intervention
- **Consistency**: Strong consistency for unique key/code generation; eventual consistency acceptable for analytics and public feeds
- **Durability**: Once created, entities must not silently disappear (explicit deletes allowed; expiry is predictable)
- **Read-heavy workload**: Both problems have a high read:write ratio (~10:1 to 100:1)
- **Latency**: Sub-50ms p99 for the hot read path (redirect or paste retrieval)
