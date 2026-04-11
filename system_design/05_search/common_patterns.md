# Common Patterns — Search (05_search)

## Common Components

### Inverted Index (Core Data Structure)
- The foundational data structure in all four problems: maps terms/tags to sorted posting lists of document/item IDs
- In google_search: custom binary segment files (Lucene-inspired) with delta-encoded posting lists, skip lists, and Bloom filters; 500–1,000 shards covering 100B documents
- In typeahead_autocomplete: not a classical inverted index but a compressed PATRICIA trie that maps prefixes to ranked suggestion lists — same lookup-by-prefix semantics
- In elasticsearch: Lucene segments form the inverted index; immutable once written; periodically merged; posting lists store term frequency, positions, offsets
- In tag_based_search: Redis Sorted Sets per tag (`tag:inv:{tag_id}`) map tag → sorted list of (item_id, score=tagged_at); augmented with Roaring Bitmaps for mega-tags

### Redis (Hot-Path Cache and Index Store)
- Used in all four for low-latency serving of pre-computed or cached results
- In google_search: result cache (full SERP JSON for top-1M queries, TTL 1h, LRU eviction)
- In typeahead_autocomplete: personalisation store (Redis ZSET per user, ZADD/ZREVRANGEBYSCORE), delta suggestions (ZSET with ZRANGEBYLEX for prefix scan), trending queries
- In elasticsearch: external cache layer (Redis or Varnish) for complete search responses; mentioned for application-level popular query caching
- In tag_based_search: primary hot-path inverted index (ZSET), tag cloud (ZSET top-10K), trending (ZSET per locale), autocomplete trie snapshot (binary blob), co-occurrence scores (ZSET per tag), frequency counters

### In-Process Memory Cache
- All four keep critical hot data in-memory within the serving process to avoid network hops
- In google_search: PageRank scores for all 100B documents (800 GB total in-memory across fleet), snippet cache (TTL 30min, LRU), spell correction mappings (10M entries, daily flush), taxonomy/verticals (Memcached)
- In typeahead_autocomplete: API server LRU cache (last 10K popular prefix→suggestions, TTL 30s), trie top-K cached at every node (O(1) lookup)
- In elasticsearch: JVM heap caches — request cache (1% heap), query cache (10% heap for filter bitsets), field data cache (40% heap); OS page cache for Lucene segment files (most critical)
- In tag_based_search: PATRICIA trie for autocomplete (rebuilt every 10 min, served from process heap), tag metadata LRU cache (tag_id ↔ name bidirectional map)

### CDN (for Aggregated / Public Endpoints)
- Used in three of the four problems to cache non-personalized, frequently identical responses
- In google_search: static assets (JS, CSS, images on SERP); search results themselves are not CDN-cached (personalized)
- In typeahead_autocomplete: 1–3 character prefix responses (TTL=60s); reduces backend load by ~20%
- In tag_based_search: tag cloud (5-min TTL), related tags (1-hour TTL), autocomplete short prefixes (30s TTL); Cloudflare
- In elasticsearch: not used for search responses themselves (per-query)

### Kafka (Query/Event Log Stream)
- Used in google_search, typeahead_autocomplete, and tag_based_search as the durable input stream for index updates and analytics
- In google_search: crawl frontier queue (partitioned by domain hash); enables replay and per-domain politeness scheduling
- In typeahead_autocomplete: query log stream (partitioned by locale, 100 partitions for 100 locales); consumed by Flink for trending detection
- In tag_based_search: `tag_events` topic (item_id, tags, action, timestamp); 100+ partitions; consumed by Tag Index Writer instances

### Circuit Breakers + Graceful Degradation
- All four implement circuit breakers on at least one critical path
- In google_search: per-shard circuit breaker (error rate > 50% in 10s → exclude shard from scatter-gather)
- In typeahead_autocomplete: per-locale circuit breaker (error rate > 20% → return global-only suggestions; Redis failure → fall back to global)
- In elasticsearch: circuit breakers on request, fielddata, in-flight request queues to prevent JVM OOM; 503 returned on open circuit
- In tag_based_search: Redis failure → fallback to PostgreSQL (50ms vs 5ms); Kafka consumer rebalance → stale Redis

### Distributed Tracing
- All four mention distributed tracing for latency diagnosis
- In google_search: Dapper-style tracing (the original inspiration for OpenTelemetry/Jaeger); 0.1% normal sampling, 100% tail sampling for p99 > 500ms
- In typeahead_autocomplete: 1% sampling + 100% for requests with latency > 50ms
- In elasticsearch: OpenTelemetry spans
- In tag_based_search: Jaeger/Zipkin

## Common Databases

### PostgreSQL
- Used in typeahead_autocomplete (query_stats table), elasticsearch (as external system of record), and tag_based_search (tags, item_tags, tag_cooccurrence, tag_aliases)
- Role: durable, ACID-compliant ground truth; not used for hot-path serving
- In tag_based_search explicitly: fallback for search when Redis unavailable (latency degrades from 5ms to 50–100ms)

### Redis (also in Common Components above)

## Common Queues / Event Streams

### Kafka → Apache Flink (Trending Detection)
- Both typeahead_autocomplete and tag_based_search use Kafka → Flink for streaming trending detection
- Pattern: query/tag events flow into Kafka; Flink computes rolling or tumbling windows; trending score via Exponential Moving Average (EMA); results written to Redis Sorted Set
- In typeahead_autocomplete: rolling 24h window; velocity = current_count / max(ema, 1); results to Redis
- In tag_based_search: 10-minute tumbling windows; EMA formula `score = α × count_1h + (1-α) × score_old` (α=0.3); trend_velocity > 2.0 threshold

## Common Communication Patterns

### REST over HTTPS
- All four expose REST APIs; HTTPS for all external communication
- Query parameters carry the search/prefix string, language, locale, filters

### Scatter-Gather (Fan-Out and Merge)
- Google Search and Elasticsearch both use scatter-gather: broadcast query to all shards, collect partial top-K results, merge via min-heap
- Google Search: scatter to 500–1,000 shards; collect top-K per shard; merge with WAND; min-heap merge of partial lists
- Elasticsearch: coordinating node fans out to primary/replica shards; partial search results aggregated and sorted

## Common Scalability Techniques

### Sharding of the Index
- All four shard their index storage
- Google Search: URL hash → shard_id; 500–1,000 shards, each 100–200M documents; consistent hashing with virtual nodes
- Typeahead: shard by locale (one trie per language+country); within large locales, shard by first 2 characters
- Elasticsearch: hash-based (`shard = hash(doc_id) % num_shards`) for product indices; time-based rolling for log indices
- Tag-based search: Redis Cluster with 16,384 hash slots (CRC16-based); PostgreSQL range-sharded by tag_id, hash-sharded by item_id

### Replication for Read Scalability and Fault Tolerance
- All four replicate their index
- Google Search: RF=3 per shard (leader + 2 followers) across racks
- Typeahead: 3 read replicas per locale shard
- Elasticsearch: 1 primary + 1 replica (standard); configurable to 2
- Tag-based search: Redis 1 primary + 2 replicas; PostgreSQL 1 primary + 2 read replicas

### Two-Tier Index (Base + Delta)
- Google Search and Typeahead both use a large immutable base index plus a small real-time delta layer
- In google_search: large immutable base index (rebuilt weekly) + small real-time delta index (updated continuously for news); queries merge results from both layers
- In typeahead_autocomplete: base PATRICIA trie (rebuilt daily, hot-swapped via blue-green) + Redis ZSET delta layer for trending queries (updated every 10 minutes via Flink)

### Posting List Compression
- Google Search and Elasticsearch both compress inverted index posting lists
- In google_search: delta encoding (store gaps between consecutive doc_IDs), variable-byte encoding / PForDelta (2.5x compression)
- In elasticsearch: Lucene delta-encoded posting lists; LZ4/DEFLATE stored fields; `best_compression` codec (-30% storage)

### Query Result Caching with TTL
- All four cache query results, typically keyed by the query string (or prefix), with a short TTL
- Google Search: Redis cache (full SERP JSON, top-1M queries, TTL 1h); probabilistic early expiration (PER) to prevent cache stampede
- Typeahead: in-process LRU (TTL 30s); CDN (TTL 60s for short prefixes)
- Elasticsearch: Redis/Varnish for popular queries; internal request cache (size=0 aggregation queries only)
- Tag-based search: CDN for public aggregate endpoints (5min–1h TTL); Redis for per-tag data

## Common Deep Dive Questions

### How do you serve search results within 200ms p99 at 100K+ QPS?
Answer: Keep the hot path entirely in-memory. Pre-compute and cache all expensive signals (PageRank, tag scores, top-K per trie node) in serving memory. Cache popular full query results with a short TTL (1h for search, 30s for autocomplete). For cold queries: scatter to shards in parallel, gather partial top-K, merge via min-heap. Hedged requests re-issue to a replica after 10ms if no response from primary.
Present in: google_search, typeahead_autocomplete, elasticsearch, tag_based_search

### How do you keep the index fresh for real-time content without rebuilding?
Answer: Two-tier index — large immutable base rebuilt on a slow schedule (daily/weekly) + small delta layer updated continuously (< 1min via Kafka → consumer). On query, merge results from both layers, with delta results taking precedence. For Elasticsearch: 1-second refresh_interval creates a new in-memory Lucene segment from the buffer, making documents searchable within 1 second without a full commit.
Present in: google_search, typeahead_autocomplete, elasticsearch, tag_based_search

### How do you handle trending queries/tags in near real-time?
Answer: Kafka stream of query/tag events → Flink rolling/tumbling window aggregation → Exponential Moving Average (EMA) for velocity score → write top trending to Redis Sorted Set. EMA prevents single-spike noise; velocity = current_count / EMA_baseline with threshold (2–3σ). CDN caches the trending endpoint with a short TTL (30–60s).
Present in: typeahead_autocomplete, tag_based_search, google_search (news freshness pipeline)

### How do you prevent a single hot shard from becoming a bottleneck?
Answer: Use hash-based sharding (not range-based) to distribute load evenly. Add read replicas per shard for read-heavy workloads. For Elasticsearch: use custom routing with `routing.partition_count` to spread hot documents. For tag-based search: mega-tags (>1M items) use Roaring Bitmaps instead of Redis ZSETs to reduce memory.
Present in: google_search, elasticsearch, tag_based_search

### How do you implement typo-tolerant prefix matching efficiently?
Answer: PATRICIA compressed trie with top-K cached at every node. For typo tolerance: secondary trie or Symmetric Delete Algorithm (SymSpell) precomputes all edit-distance-1 variants and stores as trie keys. For Elasticsearch: `search_as_you_type` field type creates n-gram sub-fields; `completion` suggester uses an FST (Finite State Transducer).
Present in: typeahead_autocomplete, google_search (spell correction), elasticsearch, tag_based_search

## Common NFRs

- **Query latency**: p99 < 50–200ms depending on problem; p50 < 20–50ms; autocomplete stricter than full-text search
- **Availability**: 99.9–99.99% across all four problems
- **Eventual consistency acceptable for aggregate/trending data**: tag clouds, trending terms, related tags can lag minutes; exact search results require near-real-time (< 1–15 minutes)
- **Read-heavy workload**: all four are predominantly read-heavy; write-then-read with short freshness window
- **Horizontal scalability**: all index shards are independently scalable; stateless query servers behind load balancer
