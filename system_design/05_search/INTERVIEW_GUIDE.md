Reading Pattern 5: Search — 4 problems, 9 shared components

---

# Pattern 05: Search — Complete Interview Study Guide

**Problems covered:** Google Search, Typeahead Autocomplete, Tag-Based Search, Elasticsearch  
**Shared components identified:** Inverted Index, Redis (hot-path), In-Process Memory Cache, CDN, Kafka, Circuit Breakers, Distributed Tracing, PostgreSQL, Flink Trending Pipeline

---

## STEP 1 — ORIENTATION

**What "Search" means as a system design pattern:**

Search is the family of problems where you turn a corpus of content into a structure that lets users find the right items in milliseconds. The corpus can be 100 billion web pages, 1 billion query strings, 100 million tagged articles, or 500 million product documents. The query can be a free-text phrase, a keypress prefix, a boolean tag expression, or a structured filter. What all four problems share is the same fundamental pipeline: **ingest content → build an index → serve queries fast**.

**The four problems at a glance:**

| Problem | Scale | Latency target | Primary data structure | Unique challenge |
|---|---|---|---|---|
| Google Search | 100B docs, 100K QPS | P99 < 200ms | Custom binary inverted index segments | Crawling + PageRank + freshness |
| Typeahead Autocomplete | 1B queries, 1.8M RPS | P99 < 100ms, P50 < 20ms | Compressed PATRICIA trie | Per-keypress speed + trending + personalization |
| Tag-Based Search | 100M items, 10M tags, 50K QPS | P99 < 50ms | Sorted posting lists + Roaring Bitmaps | Exact boolean AND/OR/NOT correctness |
| Elasticsearch | 1T docs (cluster), 1K QPS | P99 < 200ms | Lucene segments with translog | NRT indexing + aggregations + cluster management |

---

## STEP 2 — MENTAL MODEL

**The core idea: pre-invert the relationship.**

The naive approach to search is: for every query, scan all your content and find what matches. That's a table scan. It works when you have 1,000 documents. It breaks at 1 million. It is completely impossible at 100 billion.

The solution that all four problems share is the **inverted index**: you flip the relationship from "document → words" to "word → documents." You do this work once, offline, at write time. Then at query time, looking up which documents contain the word "climate" is a single dictionary lookup that returns a pre-sorted list, not a scan of the entire corpus.

**Real-world analogy:** Think about the index at the back of a textbook. The textbook is your corpus. The index is your inverted index. Without the index, finding every page that mentions "photosynthesis" means reading the entire book. With the index, you flip to "photosynthesis" and immediately see "pages 47, 112, 203, 341." You did the hard work once (building the index) so every reader can find information instantly.

Now scale that up: instead of a 400-page textbook, you have 100 billion web pages. The index is multi-petabyte, sharded across thousands of machines, and it needs to be updated continuously as new pages are discovered. The query has to finish in 50 milliseconds. And 100,000 people are querying it simultaneously. That is why this is hard.

**Why search is actually hard, not just "use an index":**

Three interacting tensions make search difficult in interviews.

First, **freshness vs. serving speed**. The index that makes queries fast is a static, pre-computed structure. But the web changes constantly. Breaking news must appear in 5 minutes. A new viral query must appear in autocomplete in 15 minutes. A newly-tagged article must be searchable in under 1 second. Rebuilding the full index takes days. The solution — a two-tier base+delta architecture — adds complexity and requires careful query-time merging.

Second, **precision vs. recall vs. latency**. For a query with 3 terms, returning only documents that contain all 3 (AND) is precise but may miss good results. Returning documents with any of the 3 (OR) finds more but is harder to rank and slower to execute. The posting lists for common words can have hundreds of millions of entries. Algorithms like WAND (Weak AND) skip most of those entries, but only with careful precomputed upper bounds. Getting this right under a 200ms budget requires deep engineering.

Third, **memory vs. cost vs. correctness**. The hot path must be in RAM — disk I/O at the required QPS is not feasible. But RAM is expensive. The inverted index for Google's top 5 billion documents takes ~60 petabytes compressed; keeping it in RAM requires thousands of machines. You have to make smart tiering decisions (hot/warm/cold) and accept that less-popular documents live on slower storage.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these early. The answers change the architecture dramatically.

**1. What is the corpus?**
Ask: "Are we indexing the open web, or a bounded internal corpus?" A web crawler (Google Search) adds the URL Frontier, Googlebot, politeness scheduling, robots.txt compliance, and link-graph computation — none of which exist in the other three problems. A bounded corpus (Elasticsearch, tag-based search) removes all of that complexity and tightens freshness windows dramatically.

**2. What is the query model?**
Ask: "Is this free-text relevance search, prefix matching, exact boolean filtering, or structured queries with aggregations?" Free-text search needs BM25, stemming, and a learned re-ranker. Prefix matching needs a trie, not an inverted index. Boolean tag filtering needs exact set-intersection algorithms. Aggregations (faceted navigation) need DocValues, not an inverted index. Getting this wrong leads to proposing the wrong data structure in your HLD.

**3. What are the freshness requirements?**
Ask: "When new content is published, how quickly must it appear in search results?" The answer anchors your write-path architecture. Under 1 second means Elasticsearch's NRT refresh model (in-memory buffer → segment on refresh). Under 1 minute means Kafka → streaming indexer → small delta index. Under 5 minutes means the Google Search news pipeline. Under 24 hours means a daily batch rebuild. Each has a very different cost profile.

**4. Does the search need to be personalised?**
Ask: "Should different users see different results for the same query?" Personalisation adds an entirely new store (per-user query history, interest profiles), a score merging step at query time, a privacy concern (k-anonymity, consent), and a cold-start problem for new users. If not needed, the serving path is simpler and the trie/index can be fully public.

**5. What is the scale of queries vs. documents?**
Ask: "How many QPS are we targeting, and how large is the index?" These anchor your capacity estimation. Autocomplete is 10x the QPS of full-text search (each keypress fires a request). Tag-based search at 50K QPS with a 15 GB index fits on 5 machines. Google Search at 100K QPS with a petabyte-scale index requires thousands of machines. The answer determines whether you can keep the entire hot index in RAM on a small cluster or need sharding from day one.

**What changes based on the answers:**
- Open web corpus → add full crawl pipeline (URL Frontier, Crawlers, Raw Page Store, link graph)
- Free-text → inverted index + BM25 + spell correction + ML re-ranker
- Prefix matching → PATRICIA trie + top-K cache at each node + blue-green swap
- Exact boolean → sorted posting list intersection + Roaring Bitmaps for mega-tags
- Aggregations → DocValues column store + distributed aggregation merge
- Personalisation → Redis ZSET per user + score merging + privacy controls
- < 1s freshness → NRT segment model (Elasticsearch-style) or Kafka+delta index

**Red flags (things a weak candidate does):**
- Jumping straight to "I'd use Elasticsearch" without asking whether you're designing Elasticsearch itself or a downstream search system
- Saying "full-text search" when the requirements clearly describe prefix-based autocomplete — they use completely different data structures
- Proposing a relational database with LIKE '%query%' for full-text search at any meaningful scale
- Not asking about freshness requirements before proposing architecture
- Treating "search" as if there is one universal design

---

### 3b. Functional Requirements

**Core requirements across all four problems:**

1. **Index content**: accept content at write time and make it searchable
2. **Query content**: accept a query at read time and return matching content ranked by relevance (or sorted by recency, depending on the problem)
3. **Low latency**: all four problems have P99 targets in the 20–200ms range

**Scope per problem:**

For **Google Search**: Web crawling + indexing + ranking (BM25 + PageRank + ML re-ranker) + spell correction + snippet generation + search verticals (web, news, images).

For **Typeahead Autocomplete**: Prefix lookup returning top-K globally popular completions + personalisation for authenticated users + trending queries within 15 minutes + blacklisting.

For **Tag-Based Search**: Multi-tag boolean queries (AND/OR/NOT with exact correctness) + tag autocomplete + tag cloud (top-10K tags by frequency) + trending tags + related tags (NPMI co-occurrence) + admin tag management (merge/split/rename).

For **Elasticsearch**: Document indexing (NRT, within 1 second) + free-text full-text search + structured filtering with boolean logic + aggregations (terms, date histogram, percentiles, cardinality) + index lifecycle management + cluster management.

**Clear statement of what the system does:**
The system transforms a stream of content writes into a durable index, then answers read queries against that index with latency under a specified budget, returning ranked or filtered results with sufficient freshness for the use case.

---

### 3c. Non-Functional Requirements

**Derive NFRs from the problem, not from a checklist.**

**Latency** — derive from user perception. Autocomplete must feel instantaneous (< 20ms P50, < 100ms P99) because users are typing and any noticeable lag kills the feature. Full-text search can be slightly more forgiving (< 50ms P50, < 200ms P99). Aggregation queries in Elasticsearch are heavier and justify 500ms P99.

**Availability** — derive from the cost of the feature being down. Search is often the primary revenue driver (Google Search → ads). Target 99.99% (< 53 minutes downtime/year). Autocomplete down degrades UX but doesn't block search; graceful degradation is acceptable.

**Consistency** — search is almost always eventually consistent and that is fine. Tag cloud can lag by minutes. Trending queries can take 10-15 minutes to appear. Index freshness for new web pages is days. The exception: exact multi-tag AND/OR/NOT queries must be correct (no approximation), and Elasticsearch acknowledges a write only after it is durable (translog fsync).

**Durability** — no data loss for indexed content. Google Search replicates raw pages 3x on GCS. Elasticsearch acknowledges writes after translog fsync. Tag metadata lives in PostgreSQL (ACID). The index itself can be rebuilt from source data, so losing it is a performance problem, not a data loss problem — but rebuilding takes time (days for web scale), so replication is still required.

**Key trade-offs to surface explicitly:**

✅ Freshness vs. serving simplicity: a two-tier base+delta architecture gives freshness at the cost of complexity (merge at query time, two data structures to maintain)  
❌ Rebuilding the full index for every update: simple but completely infeasible — a web-scale full rebuild takes days  
✅ In-memory serving for latency: keeping the hot index in RAM gives sub-millisecond lookups  
❌ Disk-based serving for cost: NVMe random reads are 1,000x slower than DRAM; at 100K QPS fan-out to 500 shards, disk I/O cannot meet latency budgets  
✅ Eventual consistency for aggregate data: tag clouds, trending, PageRank can all be seconds to minutes stale without user impact  
❌ Strong consistency for all search data: would require synchronous distributed transactions on every write, destroying write throughput  

---

### 3d. Capacity Estimation

**The formula that applies to all four problems:**

Start with: daily volume → QPS → storage → bandwidth

**Anchor numbers to memorize:**

- Google Search: 8.5 billion searches/day → ~98,400 average QPS, ~246,000 peak QPS; index = 100 billion documents; P99 < 200ms
- Autocomplete: 10x base search QPS because each search involves ~6 keypresses → ~600K average RPS, ~1.8M peak RPS; trie = ~600 GB in RAM
- Tag-Based Search: 100M content items × 5 tags avg = 500M associations; 6 GB inverted index + 6 GB forward index = ~15 GB total, fits on 3-5 nodes
- Elasticsearch: 500M product docs × 2 KB → 1 TB raw, 525 GB compressed + inverted index overhead; 3.1 TB total cluster for a mid-scale deployment

**Architecture-determining calculation for autocomplete:**

600 GB for the complete trie + top-K cache fits on ~10 machines with 64 GB RAM each. This is the key insight that justifies the in-memory trie architecture. Once you demonstrate this, the rest of the design follows logically: you can keep the entire trie in RAM, shard by locale, and serve with O(L) lookup where L is the prefix length.

**Architecture-determining calculation for Google Search:**

At 100K QPS with fan-out to 500 shards, each shard handles 200 QPS. A single query requires random reads into multiple posting lists. NVMe SSD random read latency is ~100 µs; DRAM is ~100 ns — 1,000x faster. With a 200ms P99 budget, you simply cannot afford SSD random I/O on the critical path for the hot tier. This justifies the in-memory binary segment approach.

**Architecture-determining calculation for tag-based search:**

The full inverted index (500M (tag, item) pairs × 12 bytes = 6 GB) plus forward index (~6 GB) plus tag metadata (~280 MB) totals ~15 GB. This fits entirely in RAM on a single 64 GB machine. With replication (3x) and operational overhead, 3-5 nodes suffice. This is a very different scale from Google Search and justifies a much simpler cluster design.

**Time allocation in interview:** Spend 5-7 minutes on capacity estimation. State assumptions explicitly (average query length, average document size, compression ratio). Arrive at 2-3 numbers that drive architecture decisions (fits in RAM? how many shards?). Do not spend more than 10 minutes here — it's a means to an end, not the goal.

---

### 3e. High-Level Design (HLD)

**4-6 core components for all search problems:**

1. **Write path / ingestion layer** — accepts new content and routes it to the index builder
2. **Index building layer** — transforms raw content into the serving data structure
3. **Serving / query layer** — stateless servers that receive queries and route to index shards
4. **Index storage tier** — the actual data structure (inverted index, trie, etc.) sharded across machines
5. **Cache layer** — Redis for hot results, CDN for public aggregate endpoints
6. **Async analytics pipeline** — Kafka → Flink for trending detection; offline batch for PageRank, co-occurrence, etc.

**Data flow for a query (applies to all four):**

User query → CDN edge (cache check for short prefixes or popular queries) → Load balancer → Stateless API / query processor → Scatter to index shards in parallel → Each shard computes local top-K → Gather partial results → Min-heap merge → Apply re-ranking or personalization → Return response.

**Data flow for a write (applies to all four):**

Content arrives → Published to Kafka (durable, replayable) → Index writer consumer picks it up → Normalizes and tokenizes content → Updates in-memory buffer or posting list in Redis → Flushes to durable segment / PostgreSQL asynchronously → Background job updates trending scores, tag clouds, co-occurrence.

**Whiteboard order (draw in this sequence):**

1. Start with the user on the left and the response on the right — draw the query path first
2. Add the index storage tier (this is the center of the system)
3. Add the stateless query processor / API server between user and index
4. Add the cache layer (Redis on the side, CDN at the edge)
5. Add the write path: content source → Kafka → index writer → index storage
6. Add the async analytics path: Kafka → Flink → Redis trending store

**Key decisions to state aloud at each component:**

- Why stateless query servers? Because they hold no data; scaling them is just adding more instances behind a load balancer
- Why Kafka between the write path and the index writer? Decouples producer from consumer, gives durability, enables replay if the index writer falls behind or needs to rebuild
- Why keep hot index in RAM? 1,000x latency advantage over disk for random access at high QPS
- Why scatter-gather (fan-out to all shards)? Because the query can match documents on any shard; you can't predict which shard has the best results without checking all of them
- Why min-heap merge? O(S log K) where S = number of shards and K = candidates per shard, to merge partial top-K lists into the global top-K without full sort

---

### 3f. Deep Dive Areas

The three areas interviewers probe most deeply across all four problems:

**Deep Dive 1: Index data structure selection**

*Problem:* Why do you use a trie for autocomplete but an inverted index for full-text search? Why do some tags use Redis ZSETs and others use Roaring Bitmaps?

*Solution:* The data structure must match the query semantics.

For **full-text search** (Google Search, Elasticsearch): queries are individual terms, not prefixes. You need to look up "climate" and get all documents containing it, ranked by BM25. An inverted index gives O(1) term lookup returning a sorted posting list. A trie is suboptimal because you traverse character by character for no benefit — you want term lookup, not prefix matching.

For **autocomplete** (Typeahead): the query is always a prefix. You need all strings that start with "clima". A PATRICIA-compressed trie with top-K cached at every node gives O(L) lookup (L = prefix length) and O(1) result retrieval from the cache. An inverted index would need to scan all terms starting with "clima" — expensive for long-tail prefixes.

For **tag search**: the query is exact boolean operations on tag strings. For rare tags (< 100 items), a B-tree in PostgreSQL handles the low volume. For medium tags (100 to 1M items), a Redis Sorted Set gives O(log N) insertion and range queries. For mega-tags (> 1M items like "programming"), Roaring Bitmaps compress the bitset 40x compared to Redis ZSETs and support SIMD-accelerated bitset AND/OR operations.

*Unprompted trade-offs to raise:*

✅ Trie top-K cache: O(1) retrieval at query time; trades memory (each node stores K suggestions) for speed  
❌ Trie without caching: O(subtree) DFS traversal at every query — too slow at 1.8M RPS  
✅ Roaring Bitmaps for mega-tags: compressed (40x), fast bitset AND (SIMD-accelerated)  
❌ Plain sorted arrays for mega-tags: correct but memory-intensive and slower on bitwise AND  

**Deep Dive 2: Freshness — keeping the index up to date without rebuilding it**

*Problem:* The index is a static pre-computed structure. Content changes continuously. Rebuilding the full index takes hours to days. How do you achieve 1-second to 15-minute freshness without constant full rebuilds?

*Solution:* Two-tier base+delta architecture.

The **base index** is a large, immutable, highly-optimized structure rebuilt on a slow schedule (daily for autocomplete, weekly for Google Search). It covers the bulk of the corpus and is optimized for fast reads (compressed, sorted, cached).

The **delta layer** is a small, continuously-updated structure that captures recent changes. In Google Search, it's an in-memory delta index rebuilt from the streaming crawler pipeline, updated every few minutes. In autocomplete, it's a Redis ZSET per locale containing trending queries (built by Flink from the Kafka query log stream, updated every 10 minutes). In Elasticsearch, the delta is the in-memory IndexWriter buffer, flushed to a new Lucene segment on each 1-second refresh.

At query time, results from both layers are merged. The delta layer takes precedence for recency-biased queries.

*The Elasticsearch NRT mechanism specifically:* A Lucene `commit()` (full fsync to disk) is expensive. A `refresh()` creates a new in-memory segment from the IndexWriter buffer and makes it searchable — no fsync required. Elasticsearch calls `refresh()` every 1 second. The document is not yet durably on disk after a refresh (that happens asynchronously), but it IS searchable. Durability is guaranteed by the translog (WAL), which is fsynced on every acknowledge. If the node crashes, the translog is replayed on restart.

*Unprompted trade-offs to raise:*

✅ Delta + base: freshness without full rebuild cost  
❌ Full rebuild frequently: simple, but rebuild time grows with corpus size; infeasible for web scale  
✅ NRT 1-second refresh: documents searchable quickly without fsync overhead  
❌ Commit on every write: durable immediately, but fsync latency dominates write throughput  

**Deep Dive 3: Tail latency at scale — how do you hit P99 targets with fan-out to hundreds of shards?**

*Problem:* Google Search fans out to 500+ shards. The query finishes when the LAST shard responds. The P99 of any individual shard (~5ms) becomes the P99 of the overall query plus merge overhead. Slow shards (GC pauses, network blips, hot keys) dominate tail latency.

*Solution:* Three techniques in combination.

First, **parallel fan-out**: all shard requests are issued simultaneously (async scatter), not sequentially. Latency is determined by the slowest shard, not the sum of all shards. Without this, 500 shards × 5ms = 2,500ms — totally unacceptable.

Second, **hedged requests**: if a shard hasn't responded within a deadline (10ms for Google Search, problem-specific), re-issue the same request to a replica of that shard. The first response wins; the other is cancelled. This converts a slow P99 tail (GC pause on the primary) into near-P50 behavior (the replica is likely in a better state). Hedging doubles the number of requests in the worst case but is capped to only fire on stragglers.

Third, **query result caching**: for popular queries (Google's top-1M queries have a cache hit rate of 40-60% with TTL=1h), the entire SERP result is cached in Redis. Cache hits skip the scatter-gather entirely. Cache stampede is prevented by probabilistic early expiration (PER) — 1% of requests near expiry bypass the cache proactively.

*Unprompted trade-offs to raise:*

✅ Hedged requests: reduces P99 significantly; cost is extra replica load in the tail  
❌ Without hedging: one slow shard (JVM GC pause, network issue) ruins the P99 for all users querying that shard  
✅ Result caching: massive throughput reduction for hot queries; simple  
❌ Cache stampede without PER: when a popular query expires, simultaneous cache misses all hit the backend (thundering herd), causing a latency spike for that query  

---

### 3g. Failure Scenarios

**Frame failures at the senior level:** "What is the user experience when X fails? Is it a graceful degradation or a hard failure? What is the recovery time? Did we lose data?"

**Scenario 1: A single index shard goes down**
Impact: In Google Search, 0.2% of queries return incomplete results (the shard that failed is absent from scatter-gather). In Elasticsearch, if the failed shard has a replica, the coordinator immediately routes to the replica — no user impact. If no replica exists, that portion of the index is unavailable until the node recovers.
Mitigation: Replicate every shard (RF=3 for Google Search, RF=2 minimum for Elasticsearch). Circuit breaker on the query processor excludes failed shards and returns partial results rather than erroring.
Senior framing: "I'd argue for at least 3 replicas on the hot tier despite the cost, because a single-replica failure during business hours means either downtime or degraded quality, and neither is acceptable for a revenue-generating search product."

**Scenario 2: Redis goes down (the hot-path cache)**
Impact: In tag-based search, queries fall back to PostgreSQL — latency degrades from 5ms to 50-100ms. In autocomplete, personalization fails; the system falls back to global-only suggestions. In Google Search, the result cache is empty; all queries hit the index serving tier, potentially causing a 3-5x spike in backend load.
Mitigation: Circuit breaker detects Redis failures; fall back to the durable store (PostgreSQL for tag search) or degraded mode (global-only autocomplete). Load shedding on the index tier to prevent cascade failure.
Senior framing: "Redis is a cache, not the system of record. Every read path must have a defined behavior when Redis is unavailable — either falling back to a slower durable store or returning a degraded response. Never let a cache failure become a full outage."

**Scenario 3: The Flink trending pipeline falls behind**
Impact: Trending queries/tags stop being updated. Suggestions show stale popularity rankings. No user-visible error — the baseline trie continues serving.
Mitigation: TTL on trending entries in Redis (30 minutes for autocomplete) means they expire gracefully. SLO alert fires when Flink consumer lag exceeds a threshold (e.g., > 5 minutes behind). The base trie continues serving normally.
Senior framing: "This is a graceful degradation, not a failure. Trending detection is a best-effort enrichment on top of the baseline service. As long as the base trie is current, users still get good suggestions — just without real-time trending. I'd instrument consumer lag as an SLO metric and page on extended lag."

**Scenario 4: Bad index write (corrupted segment or mapping conflict)**
Impact: In Elasticsearch, a mapping conflict (trying to index a field as both `text` and `keyword`) causes indexing errors for affected documents. In Google Search, a corrupted segment causes degraded recall for documents in that shard.
Mitigation: Elasticsearch uses dynamic mapping (disabled in production) → all mappings are explicit and validated in CI. Google Search validates segment checksums (CRC) on write; automatic rollback to the previous segment if CRC fails. Index aliases + blue-green indexing allow zero-downtime rollback for Elasticsearch.
Senior framing: "I'd treat index changes (mapping changes in Elasticsearch, new index segment formats in Google) the same as code deploys: validate in staging with production-sized data, use blue-green rollout, have an automated rollback trigger."

---

## STEP 4 — COMMON COMPONENTS

Every component that appears in two or more of the four problems, with why it's used and what breaks without it.

---

### Inverted Index (Core Data Structure)

**Why used:** The fundamental data structure that makes search possible at scale. Maps terms (or tags) to sorted lists of document IDs (posting lists). Transforms query execution from O(N_documents) to O(|posting_list|) — from scanning everything to looking up only the documents that matter.

**How it appears in each problem:**

In **Google Search**: Custom binary segment files (Lucene-inspired but without JVM). Delta-encoded doc IDs, PForDelta compression, skip lists every 128th posting for fast random access, Bloom filters for negative term existence checks. Sharded to 500-1,000 shards. The WAND (Weak AND) algorithm skips ~99% of posting list entries for top-K retrieval, achieving sub-millisecond lookup per shard.

In **Typeahead Autocomplete**: Not a classical inverted index but a PATRICIA-compressed trie — same semantics (prefix → ranked list of completions) with O(L) lookup where L is the prefix length. Top-K completions cached at every trie node for O(1) result retrieval.

In **Elasticsearch**: Lucene segments form the inverted index. Each segment is immutable once written. Multiple segments per shard, periodically merged by TieredMergePolicy. Stores term frequencies, positions, offsets. Bloom filter per segment for fast negative checks.

In **Tag-Based Search**: Redis Sorted Sets per tag (`tag:inv:{tag_id}`), mapping tag → sorted list of (item_id, score=tagged_at). Augmented with Roaring Bitmaps for tags with > 1M items (compressed bitset, 40x more memory-efficient than ZSET for dense sets).

**Key configuration:**
- Google Search: 500-1,000 shards, each 100-200M documents, 30-60 GB RAM per shard; posting lists delta-encoded + PForDelta compressed
- Elasticsearch: recommended shard size 10-50 GB primary; `refresh_interval=1s` for NRT; `best_compression` codec saves ~30% storage
- Tag search: three-tier strategy — PostgreSQL for rare tags (< 100 items), Redis ZSET for medium tags (100-1M items), Roaring Bitmap for mega-tags (> 1M items)

**What breaks without it:** Every query becomes a full table scan. At 100K documents, this is slow. At 100 million documents, it's seconds per query. At 100 billion documents, it's completely impossible within any reasonable latency budget.

---

### Redis (Hot-Path Cache and Index Store)

**Why used:** Redis provides sub-millisecond key-value lookup (hash map, sorted set, string) with high throughput. It bridges the gap between the durable but slower persistent store (PostgreSQL, disk segments) and the serving latency requirement (< 5ms for the hot path).

**Key configuration per problem:**

In **Google Search**: Full SERP JSON cached for top-1M queries, TTL=1 hour, LRU eviction. Probabilistic early expiration (PER) prevents cache stampede — 1% of requests near expiry trigger a background refresh proactively.

In **Typeahead Autocomplete**: Redis ZSETs for personalisation (per user, key = `autocomplete:personal:{user_id}:{locale}`, ZADD/ZREVRANGEBYSCORE). Delta suggestions ZSET for trending queries (ZRANGEBYLEX for prefix scan). TTL of 30 minutes per trending entry for automatic expiration.

In **Elasticsearch**: External Redis or Varnish cache at the application layer for popular search responses. Elasticsearch itself uses JVM heap caches internally (request cache, query cache, fielddata cache), but Redis is recommended at the application tier.

In **Tag-Based Search**: Primary hot-path inverted index (`tag:inv:{tag_id}` ZSET). Tag cloud (`tag:cloud` ZSET, top-10K by 7-day frequency, refreshed every 5 minutes). Trending tags (`tag:trending:{locale}` ZSET, updated by Flink). Related tags per tag (`tag:related:{tag_id}` ZSET with NPMI scores). Trending counters (`tag:count:24h:{tag_id}` with 25-hour TTL).

**What breaks without it:** 
- Without the result cache in Google Search: all 100K QPS hit the index serving tier; backend load spikes 3-5x; latency increases as index servers saturate
- Without Redis ZSETs in tag-based search: every tag query goes to PostgreSQL; latency increases from 5ms to 50-100ms, likely breaking the P99 < 50ms SLA at 50K QPS
- Without the personalisation store in autocomplete: authenticated users get global-only suggestions; the "personalization" feature does not exist

---

### In-Process Memory Cache

**Why used:** Eliminates network hops entirely. The fastest possible cache — accessing data already loaded into the application process's heap takes nanoseconds. Used for hot lookup tables that don't change frequently and fit in a single machine's memory.

**Per problem:**

In **Google Search**: PageRank scores for all 100 billion documents loaded into the serving fleet's collective RAM (800 GB total across the fleet); snippet cache (30min TTL, LRU); spell correction mappings (10M correction pairs, daily refresh).

In **Typeahead Autocomplete**: The trie itself lives in the serving process heap (not in Redis). The API server maintains an LRU cache of the last 10,000 popular prefix → suggestion mappings (TTL 30s) to serve cache hits without even traversing the trie.

In **Elasticsearch**: JVM heap caches — request cache (1% of heap, for `size=0` aggregation-only queries), query cache (10% of heap, caches filter bitsets for frequently used filters), field data cache (40% of heap — but avoid this; use DocValues instead). Most importantly: the OS page cache holds Lucene segment files in memory via mmap — this is the single most critical performance factor in Elasticsearch.

In **Tag-Based Search**: PATRICIA trie for autocomplete (rebuilt every 10 minutes, served from process heap). Tag metadata LRU cache (bidirectional tag_id ↔ name map, avoiding database roundtrips on every query).

**What breaks without it:** Every lookup that now costs nanoseconds becomes a Redis call (0.1-1ms) or a PostgreSQL call (1-10ms). At 1.8M RPS for autocomplete, even a 1ms Redis call for every trie lookup would consume 1,800ms of total work per second per serving node — completely infeasible.

---

### CDN (For Aggregated / Public Endpoints)

**Why used:** Caches identical responses at the edge (close to users geographically), reducing round-trip latency and shielding backend servers from load on non-personalized, repeatedly-requested responses.

**Key config:**
- **Autocomplete**: 1-3 character prefix responses cached at CDN edge, TTL=60s. Very short prefixes (like "a", "th") are identical for all users with the same locale — extremely high cache hit rate. Long-tail prefixes (8+ characters) bypass CDN because they're unique and rarely repeated.
- **Tag-Based Search**: Tag cloud (TTL=300s, 5 minutes), related tags per tag (TTL=3600s, 1 hour), autocomplete short prefixes (TTL=30s). These are the same for all users; CDN reduces backend load significantly.
- **Google Search**: SERP results are NOT CDN-cached (personalized, time-sensitive). Only static assets (JS, CSS, images) are CDN-cached.
- **Elasticsearch**: Not applicable for the search API itself (internal service). Application-layer caching at a reverse proxy (Varnish) serves a similar purpose for popular, static queries.

**What breaks without it:** Short-prefix autocomplete queries (typing "a", "th", "py") are identical for millions of users. Without CDN, all those requests hit backend trie servers — possibly millions of QPS for a single 2-character prefix like "an". The backend would need to be sized for this burst rather than the long-tail request distribution.

---

### Kafka (Event Stream and Write Buffer)

**Why used:** Kafka decouples content producers (web crawlers, tag events from users, new documents) from content consumers (index writers, trending pipelines, analytics jobs). It provides durability (messages are replicated on disk), replay capability (re-process events if the index writer crashes), backpressure handling (consumers process at their own pace), and enables fan-out to multiple consumers.

**Per problem:**

In **Google Search**: URL Frontier backed by Kafka, partitioned by domain hash. Enforces per-domain politeness (one partition = one domain = one crawler at a time with enforced delay). Enables replay if the crawl queue corrupts.

In **Typeahead Autocomplete**: Query log stream, partitioned by locale (100 partitions for 100 locales). Each search query is logged as an event. Flink consumes this stream for trending detection. 98,400 events/second, each ~200 bytes = ~20 MB/s — well within Kafka's capacity.

In **Tag-Based Search**: `tag_events` topic (item_id, tags[], action, timestamp), 100+ partitions. Tag Index Writer instances (100 of them) each consume one partition. New tags are searchable within seconds of the event being produced (bounded by consumer lag, typically 10-100ms).

**What breaks without it:** Without Kafka, the write path is synchronous — the content producer must wait for the index writer to confirm. If the index writer is slow (rebuilding a segment, doing a merge), the content producer is blocked. Kafka absorbs bursts, enables multiple consumers of the same write event (index writer + trending pipeline + analytics all consume the same event), and provides the durable audit trail needed for reprocessing.

---

### Circuit Breakers + Graceful Degradation

**Why used:** Prevents cascade failures. When a dependency (a shard, Redis, the Flink pipeline) starts failing, a circuit breaker stops sending requests to it after a threshold failure rate. This allows the failed component to recover without being hammered by retries, and allows the upstream service to return degraded-but-valid responses.

**Per problem:**

In **Google Search**: Per-shard circuit breaker on the query processor. If a shard returns errors on more than 50% of requests in a 10-second window, the circuit opens and that shard is excluded from scatter-gather. Results are returned from remaining shards (degraded recall: ~0.2% fewer results). Circuit reopens after 30 seconds with a half-open probe request.

In **Typeahead Autocomplete**: Per-locale circuit breaker. Error rate > 20% on a locale's trie shard → fall back to global-only suggestions (no personalization). Redis failure → fall back to global suggestions only.

In **Elasticsearch**: Built-in JVM circuit breakers — request circuit breaker (60% of heap), fielddata circuit breaker (40% of heap), in-flight requests circuit breaker (100% of heap). When a breaker trips, the request fails with HTTP 503 and a `CircuitBreakingException` rather than crashing the JVM. The client receives an actionable error to back off.

In **Tag-Based Search**: Redis failure → fall back to PostgreSQL (latency degrades from 5ms to 50-100ms but queries still work). Kafka consumer rebalance → slight staleness in Redis (stale reads, not failed reads).

**What breaks without it:** A slow or failing shard/dependency causes all requests to hang waiting for the timeout. Under high load, this means all serving threads are blocked waiting for a failed component. The entire service becomes unavailable — a cascade failure. Circuit breakers contain the blast radius.

---

### Distributed Tracing

**Why used:** Search queries fan out to dozens to hundreds of components (index shards, ranker, snippet generator, personalisation store). When P99 latency degrades, you need to know which hop is slow. Distributed tracing propagates a `trace_id` through all service calls, enabling a flamegraph of the entire request across the distributed system.

**Per problem:**

In **Google Search**: Dapper-style tracing (the original inspiration for OpenTelemetry/Jaeger). Sampling: 0.1% of all requests; 100% of requests with latency > 500ms (tail sampling). Span hierarchy: `frontend → query_processor → index_shard[0..N] + metadata_store → ranker → snippet_generator`.

In **Typeahead Autocomplete**: 1% sampling + 100% for latency > 50ms.

In **Elasticsearch**: OpenTelemetry spans for cluster internals.

In **Tag-Based Search**: Jaeger or Zipkin.

**What breaks without it:** When the P99 latency alert fires at 3am, you have no way to determine whether the bottleneck is in the trie lookup, the Redis personalisation fetch, the scoring merge, or the CDN response. Debugging requires adding instrumentation and waiting for another incident. Distributed tracing makes post-incident investigation from O(hours) to O(minutes).

---

### PostgreSQL

**Why used:** ACID-compliant, durable, strongly consistent relational store. Used as the system of record for structured metadata — not on the hot read path, but as the ground truth from which in-memory caches can be warmed and Redis can be rebuilt.

**Per problem:**

In **Typeahead Autocomplete**: `query_stats` table (normalized query, frequency_7d, frequency_24h, trend_score, language). Source of truth for the offline Trie Builder job.

In **Tag-Based Search**: `tags` (canonical tag definitions, aliases, frequency), `item_tags` (the join table — forward index), `tag_cooccurrence` (NPMI scores computed nightly). PostgreSQL is the durable fallback when Redis is unavailable.

In **Elasticsearch**: Companion store — Elasticsearch is NOT a primary database. Application data lives in PostgreSQL (or DynamoDB); CDC (Change Data Capture) or explicit dual-writes sync changes to Elasticsearch.

**What breaks without it:** In tag-based search, if Redis loses data (a node restarts without persistence, or a configuration error), there is no way to rebuild the posting lists without a durable source. PostgreSQL provides the ground truth that lets you rebuild Redis from scratch on a restart.

---

### Kafka → Apache Flink (Trending Detection Pipeline)

**Why used:** Trending queries or trending tags are driven by a spike in frequency over a short time window. You cannot precompute trending — it must be detected in real-time from the stream of user events. Flink provides exactly-once stateful stream processing with windowed aggregations and checkpointing. Redis ZSET stores the trending output for fast serving.

**Pattern (same in both autocomplete and tag search):**

Query/tag events flow into Kafka (partitioned by locale/tag) → Flink computes rolling or tumbling windows (10-minute tumbling, 24-hour rolling) → Trending score via Exponential Moving Average (EMA): `score_new = α × count_window + (1-α) × score_old` where α ≈ 0.3 → velocity = current_count / max(EMA, 1); velocity > threshold means trending → write top trending items to Redis Sorted Set with TTL → API server merges trending results with base index/trie results at query time.

**Key config:**
- α = 0.3 (weights recent data more; half-life ~20 minutes)
- Velocity threshold ≈ 2.0 (current count must be 2x the EMA to qualify as trending)
- TTL on Redis trending entries: 30 minutes (auto-expires when trend fades)
- Kafka partitions = number of parallel Flink workers = number of locales (100)

**What breaks without it:** Trending queries/tags lag by days (until the next daily batch rebuild of the base index/trie). During a breaking news event ("earthquake california" spikes to 450,000 searches/hour), users see no autocomplete suggestions for the most relevant query. This is a critical UX failure.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Google Search

**Unique things about this problem:**

The entire crawl infrastructure is unique to Google Search. A **URL Frontier** (priority queue of URLs to crawl, backed by Kafka partitioned by domain hash, with politeness scheduling enforcing one request per domain per delay window) has no analog in the other three problems. **Crawlers (Googlebot)** are distributed fetchers that follow hyperlinks, respect robots.txt, enforce crawl budgets, and adaptively back off on 429 responses. The **Raw Page Store** (GCS-equivalent blob store, gzip-compressed HTML, 3x replicated) accumulates raw content at 17.4 TB/second of inbound bandwidth.

**PageRank** is unique to Google Search: distributed power iteration over a link graph of 100 billion nodes and ~1 trillion edges. MapReduce/Dataflow jobs run weekly. Dangling nodes (no outbound links) redistribute their mass uniformly. Spider traps (cycles) are neutralized by the 15% random-jump probability (damping factor d=0.85). TrustRank seeds from manually-verified high-trust domains (Wikipedia, .gov sites) to resist link spam. PageRank scores (800 GB total) are cached in-memory across the serving fleet.

**WAND (Weak AND) top-K retrieval** is unique: for each term, precompute the maximum BM25 contribution any document can make (stored in the term dictionary). During retrieval, skip documents whose maximum possible score cannot beat the current k-th best score. Skips ~99% of posting list entries. Requires sorted posting lists and precomputed upper bounds.

**Snippet generation** and **spell correction** have no equivalent in the other three problems at this depth. Spell correction uses a hybrid noisy channel model + query log mining. Snippet generation stores a pre-extracted "snippet pool" (top-10 sentences per document) at index time to avoid fetching full document text on the query path.

**Two-sentence differentiator:** Google Search is the only problem in this folder that includes a web crawling infrastructure (URL Frontier, Crawlers, Raw Page Store, link graph) and link-graph-based ranking (PageRank, TrustRank, anchor text signals). The entire pipeline — from discovering pages via hyperlink traversal to computing PageRank via distributed power iteration to serving results via WAND with custom binary segments — is unique to this problem and absent from the others.

---

### Typeahead Autocomplete

**Unique things about this problem:**

The serving data structure is a **PATRICIA-compressed trie, not an inverted index**. This is the most important differentiator. The query is always a prefix (not a term), and the response must arrive in < 20ms P50. The trie pre-caches top-K suggestions at every internal node using a post-order DFS during the offline build — so a lookup traverses L nodes (L = prefix length) and reads the cached list in O(1), with zero subtree traversal at serve time.

**Blue-green trie hot-swap** is unique: the serving node holds two trie references (`active_trie`, `standby_trie`). The Trie Builder job builds a new trie (takes ~2 minutes for 600 GB), deserializes it into `standby_trie`, validates it (query count within ±5% of previous, golden query set comparison, canary test), then atomically swaps the pointer. Zero downtime, ~1ms for the swap itself.

**Client-side 50ms debounce** reduces request volume by ~60%: the client waits 50ms after the last keypress before sending an RPC. Since users type at an average of 200ms between keystrokes, a 50ms debounce fires one RPC per character rather than multiple.

**k-anonymity**: a query must have been issued by at least 1,000 distinct users before entering the suggestion corpus. This prevents private or embarrassing one-off queries from appearing as suggestions.

The **0.7 global + 0.3 personal score merge** is carefully calibrated: enough personalization to be useful for authenticated users, but not so much that the suggestions feel like a filter bubble. Cold-start users (no history) get 100% global suggestions automatically.

**Two-sentence differentiator:** Typeahead is the only problem where the serving data structure is a custom compressed trie with pre-cached top-K at every node rather than an inverted index, because the query is always a prefix and the sub-20ms P50 budget leaves no room for any on-the-fly tree traversal. Personalization, k-anonymity, 50ms client debounce, and blue-green trie hot-swap are all unique to this problem's combination of real-time UX constraints and privacy requirements.

---

### Tag-Based Search

**Unique things about this problem:**

**Exact boolean multi-set operations** distinguish tag-based search from all the others. The query is `python AND tutorial AND NOT beginner`. The result must be exactly correct — no approximation, no BM25 scoring, no approximate nearest-neighbor. This requires sorted posting list intersection (merge-sort-based AND), union (merge-based OR), and set difference (merge-based NOT). The `NOT` operator is always required to be combined with an `AND` clause (standalone NOT would return 80M results).

**Query optimization via size ordering**: for `AND` queries, process the smallest posting list first. If the lists have sizes [10K, 50K, 200K], intersecting 10K ∩ 50K → ~2K, then 2K ∩ 200K → ~50, is O(62K) work. The naive order (200K ∩ 50K first → 20K, then 20K ∩ 10K) is O(270K). This is a 4x speedup for selective multi-tag queries.

**Tiered posting list storage by tag cardinality** (< 100 items → PostgreSQL, 100-1M → Redis ZSET, > 1M → Roaring Bitmap) has no equivalent in the other three problems. It's driven by the enormous variance in tag frequency: the tag "programming" might have 80M items while "zuul-proxy-spring-boot-filter" has 3 items.

**Tag management operations** (merge, split, normalize, NPMI-based related tags) are unique. Tag merge is a background async job: remaps item_tags in PostgreSQL, runs `ZUNIONSTORE` in Redis to merge posting lists, registers alias in `tag_aliases`, marks source tag inactive. Protected by a Redis `SETNX` distributed lock. Reversible via audit log replay.

**NPMI (Normalized Pointwise Mutual Information)** for related tags normalizes by individual tag frequency so that mega-tags like "technology" don't appear as "related" to everything. `NPMI = log(P(A,B)/(P(A)×P(B))) / -log(P(A,B))`.

**Two-sentence differentiator:** Tag-based search is the only problem focused on exact boolean multi-set operations (AND/OR/NOT on posting lists with correct results, not ranked relevance scoring), requiring three distinct posting list data structures selected by tag cardinality: PostgreSQL for rare tags, Redis ZSET for medium tags, and Roaring Bitmaps for mega-tags. Tag management operations (merge, split, NPMI-based related tags) and the tiered posting list architecture have no equivalent in the other three problems.

---

### Elasticsearch

**Unique things about this problem:**

You are designing the search engine infrastructure itself, not a downstream application that uses a search engine. The deep dives are about **Lucene internals** (segment files, NRT refresh, translog, TieredMergePolicy), **Raft-based cluster management**, **ILM tiered storage**, and **aggregation internals** — all of which are invisible abstractions in the other three problems.

**DocValues** is the key architectural concept unique to this problem. DocValues is a column-oriented store (doc_id → value) as opposed to the inverted index (term → doc_ids). Aggregations (count per brand, price histogram) require iterating matched document IDs and looking up their field values — the DocValues column layout is O(N_matching) with excellent cache locality. The alternative (Fielddata) loads the inverted index into JVM heap on-demand, causing OOM errors on high-cardinality fields. Never aggregate on `text` fields; use `.keyword` sub-fields backed by DocValues.

**NRT mechanism in detail**: `IndexWriter.refresh()` (not `commit()`) creates a new in-memory Lucene segment from the buffer and makes it searchable — no fsync. Default refresh_interval is 1 second. Documents are searchable within 1 second but not yet durably on disk. Durability is guaranteed by the translog (WAL), which is fsynced per-batch. Lucene `commit()` happens every 30 minutes.

**Split brain and Raft**: before Elasticsearch 7.0, split brain was prevented by manual configuration of `minimum_master_nodes`. Misconfiguration was common (operators setting it to 1 in a 3-node cluster). Post-7.0, Raft consensus handles master election automatically with quorum = ceil((N+1)/2). With 3 master-eligible nodes, quorum = 2; a network partition creates groups of 2 and 1; only the group of 2 can form consensus.

**ILM (Index Lifecycle Management)** phases — Hot (NVMe SSD, active indexing, rollover at 50 GB or 1 day, forcemerge to 1 segment) → Warm (HDD, 7 days, 0 replicas, read-only) → Cold (30 days, searchable snapshots mounted from S3/GCS without local copy — 5-10x cost reduction) → Delete (90 days). Applicable to time-series log/event data, not product search indices.

**Shard count immutability**: shard count is fixed at index creation. Changing it requires `_split` (double count), `_shrink` (halve count), or full `_reindex`. This is a major operational pain point. Rule: over-provision shards slightly at creation (you can't easily add them later). Target 10-50 GB per primary shard.

**Two-sentence differentiator:** Elasticsearch is the only problem where you are designing the search engine infrastructure itself rather than a downstream application, making Lucene internals (NRT refresh via `refresh()` vs `commit()`, translog WAL, segment merge policy, DocValues column store), Raft-based cluster management to prevent split-brain, and ILM tiered storage (hot/warm/cold/delete) the core deep dives. The aggregation correctness problems (terms aggregation inaccuracy across shards, fielddata OOM risk, HyperLogLog++ cardinality estimation) and shard immutability constraints are operational concerns unique to Elasticsearch among the four problems.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (2-4 sentence answers)

**Q1: What is an inverted index and why is it used for search?**

An **inverted index** maps each term to a sorted list of document IDs that contain that term (called a posting list). It pre-computes the term-to-document relationship at write time so that at query time, looking up which documents contain "climate" is a single dictionary lookup rather than scanning all documents. The sorted posting lists enable efficient intersection (AND), union (OR), and set difference (NOT) using linear-time merge algorithms. Without an inverted index, search at scale is simply impossible — every query would be O(N_documents).

**Q2: What is BM25 and why is it better than TF-IDF for ranking search results?**

**BM25 (Best Match 25)** is a probabilistic ranking function that scores documents by relevance to a query. Like TF-IDF, it rewards rare terms (high IDF) that appear frequently in a document (high TF). BM25 improves on raw TF-IDF in two ways: it saturates the term frequency contribution (doubling a term's frequency in a document doesn't double its score — controlled by parameter k1=1.2), which prevents keyword stuffing from gaming the score; and it normalizes for document length (controlled by b=0.75), so a term appearing once in a 3-word document scores higher than the same term appearing once in a 3,000-word document.

**Q3: What is PageRank and what problem does it solve?**

**PageRank** is a link-graph-based authority score: a page's PageRank is proportional to the sum of the PageRank scores of all pages that link to it, divided by their outbound link count. It models the behavior of a random web surfer who follows links randomly with damping factor d=0.85 (15% chance of jumping to a random page). PageRank solves the keyword stuffing problem: a page can stuff keywords to score high on BM25, but it cannot fake inbound links from authoritative pages. PageRank is computed via distributed power iteration on MapReduce, running weekly over the full link graph (100B nodes, ~1T edges), and converges in 50-100 iterations.

**Q4: What is the difference between `refresh` and `commit` in Elasticsearch/Lucene?**

In Lucene, `commit()` makes all indexed documents durably persistent on disk (via fsync) and creates a new Lucene commit point — this is an expensive operation. `refresh()` flushes the in-memory IndexWriter buffer to a new in-memory segment and makes those documents searchable, but does NOT fsync — it is cheap (milliseconds). Elasticsearch calls `refresh()` every 1 second by default, which is why documents become searchable (NRT) within 1 second. Durability is handled separately by the **translog** (WAL), which is fsynced on every acknowledged write. A full `commit()` happens every 30 minutes or on an explicit flush.

**Q5: Why does Typeahead use a trie instead of an inverted index?**

Autocomplete queries are always prefix-based: "give me all completions that start with 'clima'". An inverted index is optimized for exact term lookup — finding documents that contain the complete word "climate". Scanning the inverted index for all terms starting with "clima" requires iterating the term dictionary (potentially millions of entries). A PATRICIA-compressed **trie** traverses exactly L nodes (L = prefix length) to reach the prefix, then reads the pre-cached top-K completions in O(1). At 1.8 million RPS with a P50 budget of 20ms, the O(L) + O(1) trie lookup is the only viable option; any approach requiring a dictionary scan would fail the latency budget.

**Q6: What is the scatter-gather pattern and when is it used in search?**

**Scatter-gather** (also called fan-out and merge) is used when data is sharded across multiple nodes and a query might match data on any shard. The query processor "scatters" the query to all shards simultaneously (async, in parallel), each shard returns its local top-K results, and the query processor "gathers" the partial results and merges them (using a min-heap) into the global top-K. Google Search fans out to 500-1,000 shards; Elasticsearch fans out to all primary or replica shards for each index. The latency is determined by the slowest shard, not the sum — which is why hedged requests are used to handle stragglers.

**Q7: What is the difference between Redis Sorted Set (ZSET) and Roaring Bitmaps for posting lists?**

A **Redis ZSET** stores (member, score) pairs in a skip list + hash map, providing O(log N) insertion and O(log N + M) range queries. It's great for medium-sized posting lists (100K items) where you need range queries by score (e.g., items tagged recently). A **Roaring Bitmap** is a compressed bitset: for item IDs in [0, N), each bit is set if that item is in the set. Bitwise AND (intersection) is O(max(|A|,|B|)/64) with SIMD acceleration. For mega-tags with > 1M items, Roaring Bitmaps are 40x more memory-efficient than Redis ZSETs and support faster intersection at the cost of losing the score (you can't sort by tagged_at within a Roaring Bitmap).

---

### Tier 2 — Deep Dive Questions (with why + trade-offs)

**Q1: How does WAND (Weak AND) achieve sub-millisecond posting list traversal for competitive queries?**

WAND exploits the observation that for top-K retrieval, most documents in the posting lists cannot possibly beat the current k-th best score. At index build time, for each (term, shard), precompute `UB(term) = max BM25 contribution that term can make to any document's score` (computed from the document with the highest TF and shortest length for that term). During retrieval, sort the term iterators by their current doc_id. Find the "pivot" — the first term whose cumulative upper-bound sum exceeds the current k-th best score threshold. If the term before the pivot already points past the pivot doc, skip directly to the pivot doc. If not, this document cannot beat the threshold for certain terms — skip it. WAND skips ~99% of posting list entries for competitive multi-term queries on real web data, reducing per-shard query time from O(|posting_list|) to O(small constant × log N).

Trade-off: WAND requires that upper-bound scores be precomputed and stored in the term dictionary (small memory cost), and that posting lists be sorted by doc_id (not by score) — if you want score-sorted posting lists (different optimization for other retrieval models), WAND's skip condition doesn't apply. WAND is also approximate in the sense that the upper bounds must be correct (they are by construction) but the skip condition must be tight to be effective — loose upper bounds reduce skipping.

**Q2: Why does Elasticsearch recommend leaving half of RAM for OS page cache rather than allocating all of it to JVM heap?**

Lucene (which Elasticsearch uses internally) reads segment files via memory-mapped files (`mmap`). When a segment file is accessed, the OS loads the relevant pages into the OS page cache. Subsequent accesses to the same data are served from page cache at near-DRAM speed without any JVM involvement. If you give too much RAM to the JVM heap, there is little memory left for OS page cache, so every Lucene read results in an actual disk I/O — catastrophic for latency. The recommended split is: JVM heap = min(32 GB, half of RAM) — this enables compressed object pointers (< 32 GB heap) and avoids JVM GC pressure; the rest of RAM is available for OS page cache to hold hot Lucene segments. A 64 GB machine should run with a 32 GB JVM heap and have ~30 GB (after OS) available for page cache.

The other side of this trade-off: setting JVM heap too small means aggregations and sorting operations (which need JVM heap for in-flight computation) run out of memory, triggering circuit breakers (HTTP 503). The right answer depends on your query mix: aggregation-heavy workloads want more heap; search-heavy workloads want more page cache.

**Q3: How do you prevent the thundering herd when a popular cached query expires?**

When a popular query's cache entry expires, the next request must recompute the result (expensive: scatter to all shards, gather, re-rank). But hundreds of requests arrive simultaneously at the expiry boundary — all of them miss the cache simultaneously and all try to recompute. Each computation fires 500 shard RPCs. The load spike can overwhelm the index serving tier.

The **probabilistic early expiration (PER)** solution: when a cached item has less than 10% of its TTL remaining, each incoming request has a small probability (1%) of treating the entry as expired and proactively refreshing it in the background. The vast majority of requests still serve the cached value; one request is selected to refresh. This smooths out the expiry cliff into a gradual refresh. Alternative: distributed locking (`SET NX` in Redis) — the first request that misses the cache acquires a lock and recomputes; all concurrent requests wait for the lock holder to populate the cache, then serve the newly-cached value.

**Q4: How does Roaring Bitmap achieve 40x compression over a Redis ZSET for mega-tags?**

A Redis ZSET for a tag with 10 million items stores 10 million (item_id, score) pairs. At ~16 bytes per entry (8-byte int64 item_id, 8-byte float64 score), that's ~160 MB per mega-tag. A Roaring Bitmap for the same 10 million items out of a universe of 100 million items represents each item as 1 bit in a compressed bitset. Roaring Bitmaps use a hybrid representation: for dense chunks (many items from a range of 65,536 consecutive IDs), store as a 65,536-bit bitarray (8 KB); for sparse chunks, store as a sorted array of 16-bit offsets. For typical real-world distributions, Roaring Bitmaps compress 10M item IDs to ~4-5 MB — a 32-40x reduction. Additionally, bitwise AND (intersection) is accelerated by SIMD instructions: 64 item IDs can be checked simultaneously in a single CPU instruction.

The trade-off: Roaring Bitmaps lose the score component — you can no longer sort by `tagged_at` timestamp. For use cases requiring time-sorted results from a mega-tag, you'd need to maintain both a Roaring Bitmap (for fast set intersection) and a secondary sorted structure (for time-based ordering of the intersection result).

**Q5: Why is Kafka partitioned by domain hash for Google's URL Frontier, and what does this enable?**

The URL Frontier must enforce **politeness**: crawlers should send at most one request per domain per delay window (often 1-5 seconds, as specified by `robots.txt` `Crawl-delay` or by default convention). If multiple crawler instances all crawl the same domain simultaneously, they can overwhelm the target server. By partitioning the Kafka topic by domain hash, all URLs for a given domain land in the same partition — consumed by the same crawler instance. Each crawler instance processes its partition sequentially within the domain's crawl delay, naturally enforcing the politeness constraint. No cross-instance coordination is needed (no distributed lock required) because the partitioning itself provides the mutual exclusion. Changing the number of partitions (and thus consumers) scales crawl throughput linearly while maintaining domain-level politeness.

**Q6: What is the aggregation inaccuracy problem in Elasticsearch's distributed terms aggregation, and how do you mitigate it?**

With N shards, the coordinating node asks each shard for its top-K buckets (e.g., top-10 brands by document count). If "Sony" has 100 documents on shard 1 but only 40 on shard 2 (not enough to rank in shard 2's top-10), the coordinating node sees Sony's shard-1 count (100) but misses Sony's shard-2 count. The final count for Sony is 100, not the correct 140. The severity depends on how unevenly documents are distributed across shards.

Mitigations: (1) Increase the `shard_size` parameter — request more buckets per shard (e.g., `shard_size=50` when `size=10`) so that less-common-per-shard terms are still included. The coordinating node then merges 50 × N_shards candidates and returns the top 10. Default `shard_size = max(size × 1.5 + 10, size)`. (2) For exact results, set `execution_hint: "map"` — enumerate all values on every shard (exact but O(N) per shard for high-cardinality fields, not recommended). (3) Minimize cross-shard distribution by using custom routing for multi-tenant indices (documents from the same tenant on the same shard).

**Q7: How does the two-tier index architecture handle query-time merging of base and delta results without doubling latency?**

In Google Search, the delta index (news/freshness tier) is small (typically < 1% of total indexed documents), kept entirely in memory, and answers queries in < 1ms. The base index answering happens in parallel via scatter-gather to shards (5-15ms). The query processor merges results from both in O(K log 2) where K is the result count — trivial. The total latency is dominated by the base index query, not the merge. In autocomplete, the delta layer is a Redis ZSET per locale; at query time, the API server issues two parallel lookups: trie node lookup (in-process, < 1ms) and Redis ZRANGEBYLEX on the delta ZSET (1-2ms). The merge step (interleave and re-rank top-5) is O(K) in-process. Both lookups complete within the 20ms P50 budget.

---

### Tier 3 — Staff+ Stress Test Questions (reason aloud)

**Q1: Google Search serves 100K QPS at P99 < 200ms. If a new regulatory requirement mandates that all SERP results must be filtered against a real-time user consent profile (user has opted out of certain domains) — what changes, and what is the new architecture?**

This is a Staff-level question because it adds a per-user state dependency to what is currently a mostly stateless hot path.

First, acknowledge the constraint: the consent profile check must be per-user, consistent, and low-latency (can't add 100ms to the 200ms budget). The profile must be queryable on the critical path.

Architecture change: store consent profiles in a low-latency distributed KV store keyed by user_id (Bigtable or Redis Cluster). Profile lookup must be < 10ms P99. The frontend server performs the lookup in parallel with sending the query to the query processor — not serially. The query processor receives both the query result and the consent profile simultaneously and applies the filter as a post-processing step (O(K) where K = 10 results).

The hard question: what if the consent profile store is unavailable? Option A: fail-safe (deny all uncertain — show no results). Option B: fail-open (show results, apply consent filtering when available). Option C: serve the unfiltered SERP but log it as a consent violation for auditing. The right answer is regulatory-dependent; in GDPR contexts, fail-safe is required.

What gets harder: cache invalidation. The result cache (full SERP JSON) can no longer be user-agnostic. You'd need to either key the cache by (query, user_id) — which makes the cache nearly useless — or apply consent filtering after cache retrieval (cache the base SERP, filter at serve time). The latter is correct: cache the ranked result, but never cache the filtered-per-user view.

Performance impact: adds one parallel RPC (~5-10ms) on the critical path; filter step is O(K) in-process. Net impact on P50: +5ms. On P99: depends on consent store P99. This should be manageable within the 200ms budget.

**Q2: You're designing autocomplete for a new product. During load testing you discover that queries starting with "a", "th", and "in" account for 30% of all autocomplete traffic. The CDN caches these with TTL=60s. During a breaking news event, the top suggestions for "a" must change within 5 minutes to surface "assassination attempt" (a trending query). How do you reconcile the CDN cache TTL with this freshness requirement?**

This is a real tension: CDN TTL=60s is fine for stability; 5-minute freshness for breaking news requires that the CDN cache be invalidated within 5 minutes.

Several solutions, each with trade-offs:

Option A: Reduce CDN TTL for short prefixes to 30s. Pro: simpler, no custom invalidation logic. Con: 30s TTL on "a" means the CDN serves a stale response for up to 30s, and with millions of requests/hour, the CDN must refresh the cache every 30s — significantly more origin load. For very short prefixes (1-2 chars), the origin load from a 30s TTL at high traffic is substantial.

Option B: Keep TTL=60s but add a CDN cache invalidation webhook. When the trending pipeline detects a new trending query with a short prefix, it fires a cache purge API call to the CDN (all major CDNs — Fastly, Cloudflare — support this). The response for affected prefixes is immediately stale and the next request fetches fresh results. Pro: high TTL for stability + fast invalidation when needed. Con: CDN purge APIs have rate limits; cascading purges during a major news event (dozens of trending queries changing simultaneously) could hit rate limits.

Option C: Don't cache trending-eligible short prefixes at CDN. Keep a server-side flag: if the prefix has had any trending query update in the last 10 minutes, set `Cache-Control: no-store`. Otherwise, serve with 60s TTL. Pro: precise invalidation scope. Con: adds per-request logic to determine cacheability.

The preferred answer at Staff level is Option B with a fallback to Option A: use CDN purge webhooks triggered by the Flink trending pipeline for time-sensitive breaking news, with a lower TTL (30s) as a safety net for the cases where the webhook doesn't fire in time. Instrument CDN purge rate and alert if approaching rate limits.

**Q3: Your tag-based search system is handling 50K QPS. A single tag ("javascript") has 2 million items. A query for "javascript AND react" runs daily from a scheduled API job that exports all matching items for downstream ML training. This single job generates 500 QPS of "javascript AND react" queries (each query asks for a page of 100 items, cursor-based). This is crushing the Redis ZSET serving. How do you fix this?**

This is a bulk vs. interactive traffic isolation problem.

Root cause: the ML training job is using the interactive search API (designed for human users, P99 < 50ms, 50K QPS budget) for a batch export workload. These are fundamentally different workloads with different SLAs.

Solution 1 — Separate the batch export path from interactive search: add a dedicated bulk export API that reads directly from PostgreSQL (not Redis), uses a streaming cursor over the `item_tags` table, and runs during off-peak hours. The ML training job should not go through the real-time search API at all. PostgreSQL can handle bulk sequential reads efficiently; this is exactly the workload it's designed for. Interactive Redis-backed search is not designed for 500 QPS of cursor-based pagination over a single tag.

Solution 2 — Rate limiting by client: enforce rate limits at the API gateway by API key. The ML training job's API key is limited to, say, 10 QPS of tag search queries. The job takes 50x longer to complete, but doesn't impact interactive users.

Solution 3 — Offline pre-computation: if the ML training job needs all "javascript AND react" items daily, precompute this set nightly (via Spark batch job over PostgreSQL, where it's cheap) and store the result in a blob store. The ML job reads the blob directly — no search system involvement.

The correct senior answer combines Solutions 1 and 3: separate the export concern entirely from the search system, use the appropriate tool (PostgreSQL + batch job for bulk exports, Redis + search API for interactive queries), and use rate limiting as a safety net.

**Q4: Elasticsearch is running with JVM heap at 28 GB on a 64 GB node. Operations reports that queries are occasionally hitting the fielddata circuit breaker and returning 503. What is happening and how do you fix it without increasing hardware?**

The fielddata circuit breaker trips when loading the inverted index for a `text` field (Fielddata) into JVM heap exceeds 40% of heap (40% × 28 GB = 11.2 GB). This is a misconfiguration of the index mapping, not an infrastructure problem.

Diagnosis: identify which queries are triggering fielddata loading — look for aggregations or `sort` operations on `text` fields. Fielddata is only triggered by aggregating/sorting on `text` fields (which don't have DocValues). If you see queries like `"aggs": {"by_title": {"terms": {"field": "title"}}}` — that's the problem. "title" is a `text` field.

Fix without increasing hardware: add a `.keyword` sub-field to the problematic `text` field in the mapping: `"title": {"type": "text", "fields": {"keyword": {"type": "keyword"}}}`. The `keyword` type stores DocValues (column-oriented storage on disk, OS page cache), not Fielddata (JVM heap). Change the aggregation to `{"field": "title.keyword"}`. This change requires a full reindex (the `keyword` sub-field did not exist before and DocValues are computed at index time).

For immediate mitigation without reindex: disable Fielddata on the text field (`"fielddata": false` in the mapping) so the breaker cannot be triggered. Queries that relied on Fielddata will error with a clear message, enabling the team to fix them before re-enabling.

Long-term: enforce in CI that no `text` fields are aggregated or sorted without a `.keyword` sub-field. Add integration tests that attempt aggregations on text fields and fail the build if Fielddata loading is triggered.

**Q5: The autocomplete system's trie rebuild job runs every 24 hours and takes 3 hours to build for the English locale. During those 3 hours, the trie is stale (built from yesterday's query log data). How would you reduce the effective staleness without reducing rebuild time (which is constrained by the compute budget)?**

The constraint is: rebuild takes 3 hours, compute budget prevents running it more frequently, yet stale suggestions hurt UX for trending topics.

Current state: base trie is up to 48 hours stale at worst (built from yesterday's data, takes 3 hours to build, then serves for 24 hours until the next build is ready). The delta layer (Flink → Redis ZSET) already handles queries for trending queries within 15 minutes.

The real question is: what does "staleness" mean for the base trie? The base trie covers static popularity — the frequency distribution of the top-1 billion queries changes slowly. For truly new trending queries (appearing in the last few hours), the delta layer already handles them. For rising-but-not-viral queries (growing in popularity over days), the trie is stale.

Solution 1 — Incremental trie updates instead of full rebuilds: instead of building the entire trie from scratch, apply daily incremental score updates to the existing trie. Compute a delta of changed query scores (queries whose frequency changed by > 5% in the last 24 hours) and apply these as targeted top-K cache updates on the affected trie nodes. The full rebuild is still needed weekly (to clean up deleted queries and recompute exact scores), but daily freshness is achieved via incremental patch.

Solution 2 — Overlap rebuild with serving using blue-green more aggressively: start the next rebuild immediately after the current one completes (continuous rolling builds). With a 3-hour build time and 24-hour schedule, the trie is always at most 27 hours stale. If you reduce the schedule to 12 hours, you get < 15 hours staleness with the same compute budget (same number of compute-hours per day, just more frequent smaller jobs if data is incrementally available).

Solution 3 — Accept the staleness for base trie, invest in a more aggressive delta layer: instead of 10-minute delta updates, move to 1-minute delta updates. The delta layer covers all freshness requirements; the base trie just needs to be a stable background. This is the lowest-effort engineering change.

The Staff-level answer recognizes that this is a cost/freshness/complexity trade-off: Solution 3 is the right short-term fix (operational simplicity, no changes to the trie build pipeline); Solution 1 is the right long-term architectural improvement.

---

## STEP 7 — MNEMONICS

**Mnemonic 1 — The search pipeline: "C-I-Q-R-S"**

Remember the five stages every search system goes through:

- **C**rawl/Collect — acquire the content (web crawl, content API, event stream)
- **I**ndex — transform content into a lookup data structure (inverted index, trie, bitmap)
- **Q**uery — receive a query, route to the right index shards, scatter in parallel
- **R**ank/Return — merge partial results, re-rank (BM25 + PageRank + ML model), return top-K
- **S**erve fresh — handle updates via delta index, Kafka + Flink streaming, NRT refresh

Any search system interview question is asking about one or more of these five stages. If you forget what to talk about next, go back to C-I-Q-R-S and ask "which stage is this question probing?"

**Mnemonic 2 — The three freshness solutions: "Batch, Delta, Stream"**

When asked "how do you keep the index fresh?", your answer is always one of three options depending on the required latency:

- **Batch**: acceptable staleness > 1 hour → full index rebuild on a schedule (daily, weekly). Simplest, lowest cost.
- **Delta**: staleness 1-60 minutes → two-tier base+delta architecture. Base rebuilt slowly; delta updated from Kafka → consumer. Merge at query time.
- **Stream**: staleness < 1 minute → NRT segment model (Elasticsearch `refresh_interval=1s`) or continuous streaming indexer. Highest complexity and cost.

**Opening one-liner for any search interview:**

"Search is fundamentally about inverting the document-term relationship at write time so that at query time, you're doing lookups rather than scans. The hard parts are keeping the inverted structure fresh as content changes, serving queries fast enough at scale with fan-out to hundreds of shards, and making the right data structure choice — inverted index for full-text, compressed trie for prefix autocomplete, or Roaring Bitmaps for exact set operations on tags."

Say this in the first 60 seconds. It tells the interviewer you understand the core trade-off, you know the design space, and you're ready to pick the right tool for the specific problem.

---

## STEP 8 — CRITIQUE

### Well-covered in the source material:

- **Inverted index internals** are very thorough: WAND algorithm, BM25 formula, delta encoding, PForDelta compression, skip lists, Bloom filters. The Google Search and Elasticsearch files both cover this deeply with pseudocode.
- **Trie mechanics** are excellent: PATRICIA compression, top-K pre-caching, blue-green hot-swap, Unicode handling, memory calculations. The autocomplete file is the strongest source material.
- **Capacity estimation** is rigorous: every problem has detailed storage, bandwidth, and QPS estimates with explicit assumptions and derived architecture implications.
- **Failure scenarios** are thorough: each problem covers specific failures (shard down, Redis down, Flink lag) with impact, mitigation, and recovery time.
- **Elasticsearch Lucene internals** (NRT, translog, segment merge, DocValues vs Fielddata, split brain, ILM) are very well covered.
- **Tag management operations** (merge, NPMI, normalization pipeline) are detailed with pseudocode.

### Missing, shallow, or worth supplementing:

**Vector search / semantic search** is entirely absent. In 2025-2026, almost every major search system (Google, Elastic, product search) has added vector similarity search (ANN — Approximate Nearest Neighbor) as a complement to BM25. FAISS, HNSW, ScaNN, and Elasticsearch's `dense_vector` type are not mentioned. A Staff-level candidate should be able to discuss hybrid retrieval (BM25 + vector ANN) and how to build the ANN index (HNSW graph construction, FAISS IVF, product quantization for compression).

**Personalization in Google Search** is explicitly out of scope per the source file, but interviewers frequently ask about it. Know that it involves: a per-user topic interest vector, a click+dwell-time signal aggregator, a profile store (Bigtable keyed by user_id), and a re-ranking step that blends the standard BM25+PageRank score with a user-interest similarity score. Weight personalization conservatively (10% of final score) to avoid filter bubbles.

**Multi-region consistency** is mentioned but not deeply analyzed. When Google Search fans out across multiple PoPs (US-East, EU, APAC), different regions may serve slightly different index states (replication lag). How does this affect results? What is the maximum acceptable replication lag? This is a Staff-level question that could come up.

**Cold start for the autocomplete trie**: the source material discusses the trie being built from existing query logs. What happens for a brand new deployment with no query logs? The source files don't address this. Answer: seed the trie with known popular queries from a general-purpose query dataset (e.g., Wikipedia article titles, common English phrases), then gradually replace with organic query log data over the first few weeks.

**Write-heavy scenarios in tag search**: the source material handles 5,000 new items tagged per second (125,000 tag writes/second at peak). But what happens during a bulk import event (a customer imports 10 million pre-tagged items at once)? The source files don't explicitly address import batching strategies, backpressure handling, or how to avoid Redis key explosion during a bulk import.

### Senior probes (questions that will expose gaps):

1. "You mentioned Roaring Bitmaps for mega-tags. How would you handle a query like 'programming AND javascript AND react AND python AND tutorial' where ALL FIVE tags are mega-tags?" (Answer: SIMD-accelerated bitset AND; order by estimated result count descending, using tag frequency as a proxy; result converges quickly because even mega-tag intersections are much smaller than individual sets.)

2. "Your autocomplete trie stores top-K at every node. A trending query appears at depth 12 in the trie. How many trie nodes need their top-K cache updated, and what is the cost?" (Answer: All 12 ancestor nodes. Each update is O(log K) min-heap insertion. Total cost: O(L × log K) = O(12 × log 10) ≈ O(40) operations. Acceptable for batch update; done by Trie Builder at rebuild time, not in real-time.)

3. "Elasticsearch's shard count is immutable. Your index has grown 5x since you created it with 5 shards. Shards are now 500 GB each and query latency has degraded. Walk me through the zero-downtime migration." (Answer: Create a new index with 25 shards. Use `_reindex` API or dual-write. Update the index alias atomically to point to the new index. During reindex, use the old index alias for reads. After reindex completes and is validated, atomic alias swap. After swap, continue dual-writes briefly in case of rollback, then stop dual-write.)

4. "The Google Search delta index covers news freshness for breaking stories. What is the maximum size the delta index can grow to before it degrades query performance? How do you control its size?" (Answer: Delta index is bounded by the number of documents updated in the freshness window. Set a max age policy: documents older than X hours graduate from delta to base index on next base merge. When the base index is rebuilt, the delta is reset. Keep delta < 1-5% of base index size to maintain sub-millisecond per-shard delta lookup.)

5. "Redis is your hot-path store for tag inverted indexes. Your Redis Cluster has a FLUSHALL event (operator error). How do you restore service without downtime?" (Answer: Tag search falls back to PostgreSQL immediately via circuit breaker. Start parallel Redis warming from PostgreSQL: `SELECT item_id, tag_id, tagged_at FROM item_tags ORDER BY tag_id` and ZADD to Redis. At 125K writes/second, a 500M-row table takes ~1 hour to warm. During warming, PostgreSQL serves tag search at degraded latency (50-100ms instead of 5ms). Alert if PostgreSQL response time exceeds 200ms (approaching SLA breach). After warming, Redis serves hot path again. Root cause: add Redis FLUSHALL protection (ACL or rename-command in Redis config); require human confirmation before executing.)

### Traps to avoid:

- **"I'd use Elasticsearch"** as an answer to "design Google Search." Elasticsearch is not designed for web-scale; it doesn't have a crawler; it lacks WAND; JVM GC pauses at 100K QPS would be catastrophic. At web scale, custom binary index segments are necessary.
- **Confusing trie and inverted index** for autocomplete. Trie is for prefix matching. Inverted index is for exact term matching. Using an inverted index for autocomplete requires scanning the entire term dictionary for each prefix — infeasible at 1.8M RPS.
- **Forgetting the write path** when drawing the HLD. Many candidates draw a beautiful query path and then hand-wave "and somehow new content gets indexed." The write path (content source → Kafka → index writer → durable store) is half the system.
- **Not distinguishing base index from delta layer** when discussing freshness. Saying "we rebuild the index frequently" without acknowledging the cost is a mistake. The two-tier base+delta architecture is the correct answer; commit to it.
- **Over-engineering the trie sharding** before establishing it fits in RAM. Do the math first (600 GB for English trie). Then shard by locale (one trie per language+country). Only sub-shard by first 2 characters if a single locale exceeds single-machine memory (~256 GB with generous headroom).
- **Not raising trade-offs proactively** during deep dives. Senior candidates pre-empt follow-up questions by saying "we chose Roaring Bitmaps for mega-tags at the cost of losing the time-sort order — here's how we handle that case." Candidates who only answer the question asked without anticipating the follow-up sound junior.

---

*End of Interview Guide — Pattern 05: Search*
