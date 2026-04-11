# Problem-Specific Design — Search (05_search)

## Google Search

### Unique Functional Requirements
- Web crawling: continuously discover and fetch 15 billion pages per day from the open web
- Index 100 billion documents; re-crawl average page within 7 days
- Breaking news indexed within 5 minutes via real-time delta index
- PageRank-based ranking using the full web link graph
- Spell correction ("Did you mean: …") with < 20ms p99
- Snippet generation: title, URL, and highlighted passage for each result
- Search verticals: Web, Images, News, Videos
- SafeSearch filtering (domain blocklist + content classifier)
- Robots.txt and crawl budget enforcement per domain

### Unique Components / Services
- **URL Frontier**: priority queue of URLs to crawl; politeness scheduling (one request per domain per delay window); backed by Kafka partitioned by domain hash
- **Crawlers (Googlebot)**: distributed fetchers that follow hyperlinks from seed URLs; respect robots.txt, crawl-delay, noindex directives; adaptive rate limiting on 429 responses
- **Raw Page Store**: immutable blob store (GCS equivalent) storing gzip-compressed raw HTML; RF=3, 11-nine durability
- **Content Processor**: parses HTML; extracts text; language detection via CLD3; SimHash near-duplicate detection; tokenization; link extraction
- **Indexer/Writer**: builds inverted index postings; merges segment files using custom binary format (not Lucene) — zero JVM GC overhead, SIMD-optimized
- **PageRank Computation**: distributed power iteration on MapReduce/Dataflow; damping factor d=0.85; TrustRank for link spam resistance; runs weekly on full link graph; scores cached in-memory on serving fleet (800 GB total)
- **Ranker**: ML model (LambdaMART) + BERT re-ranker; re-ranks top-1,000 BM25 candidates to top-10 final results; features include PageRank, anchor text signals, user signals
- **Snippet Generator**: fetches stored text passage; locates query term context window; formats for SERP display
- **SpamBrain**: ML classifier for web spam detection; Penguin algorithm detects unnatural link patterns
- **WAND (Weak AND)**: top-K retrieval algorithm that skips ~99% of posting list entries that cannot beat current threshold score

### Unique Data Models
- **documents** (Bigtable or equivalent): doc_id, URL, canonical_url, content_hash, simhash, language, page_rank, fetch_timestamp, next_crawl_time, http_status, crawl_depth, is_canonical
- **postings** (custom binary segment): term_id, doc_id, term_frequency, field_mask (bitmask: title/body/anchor/URL), position_list (for phrase queries)
- **terms**: term_id, term, doc_frequency (IDF denominator)
- **links**: src_doc_id, dst_doc_id, anchor_text (for PageRank and anchor text ranking signals)
- **doc_metadata** (Bigtable): title, meta_description, og_image_url, publish_date, word_count, outbound_link_count, inbound_link_count, content_category
- **ngram_corrections**: query_token, correction, confidence, frequency (for spell correction noisy channel model)
- **Posting list binary format**: Term Dictionary Block → Posting List Block (delta-encoded doc IDs) → Skip List Block (every 128th posting for skipping) → Bloom Filter

### Unique Scalability / Design Decisions
- **Custom binary segment format (not Lucene/Elasticsearch)**: 1,000× lower latency; full SIMD vectorization control; no JVM GC pauses; accepts higher operational complexity in exchange for performance
- **Two-tier index (base + delta)**: base index covers all 100B documents, rebuilt weekly; delta index covers new/updated pages and news, updated continuously; query time merges both; news appears in results within 5 minutes via delta
- **WAND top-K retrieval**: skip documents whose max possible BM25 score cannot beat the current k-th best; skips ~99% of posting list traversal; requires sorted posting lists and max-score upper bounds per term
- **SimHash for near-duplicate detection**: locality-sensitive hash of content; compare 64-bit SimHash values with Hamming distance threshold; prevents indexing boilerplate/mirror pages
- **Distributed PageRank via MapReduce/Dataflow**: power iteration over link graph (100B nodes, trillions of edges); dangling mass redistribution; weekly batch; scores stored in-process on serving fleet
- **Politeness scheduling**: URL Frontier partitions by domain hash; one Kafka consumer per partition enforces per-domain crawl delay; respects robots.txt crawl-delay directive
- **Hedged requests**: if a shard does not respond within 10ms, re-issue the same request to a replica; takes whichever reply arrives first; reduces tail latency from P99 outliers

### Key Differentiator
Google Search is the only problem in this folder that includes a **web crawling infrastructure** (URL Frontier, Crawlers, Raw Page Store, link graph) and **link-graph-based ranking** (PageRank, TrustRank, anchor text): the entire pipeline — from crawling the web to computing PageRank via distributed power iteration to serving results via WAND — is unique to this problem and absent from the others.

---

## Typeahead / Autocomplete

### Unique Functional Requirements
- Return top-K suggestions (K = 5–10) for each keypress in < 100ms p99 / < 20ms p50
- Rankings reflect global query frequency; personalized suggestions for authenticated users (30% weight)
- Trending topics must appear in suggestions within 15 minutes
- Multi-language support: 100+ locales, each with an independent trie
- Typo tolerance: minor prefix typos should still return relevant suggestions
- k-anonymity: a query must have been issued by ≥ 1,000 distinct users before entering the corpus (privacy protection)
- Blacklisting: offensive/illegal/brand-unsafe suggestions suppressed

### Unique Components / Services
- **PATRICIA Compressed Trie**: primary serving data structure; top-K suggestions pre-cached at every node (O(1) lookup); Unicode-aware for 100+ languages; stored in-process (serving heap); one trie instance per locale
- **Blue-Green Trie Hot-Swap**: new trie built offline, validated (query count ±5%, golden set comparison, canary test), then atomically swapped into serving; zero downtime; allows daily full rebuilds
- **Flink Streaming Job**: consumes Kafka query log stream; rolling 24h window; computes EMA-based trending score; velocity = current_count / max(ema, 1); outputs (query, score) pairs to Redis delta layer
- **Personalisation Store (Redis ZSET)**: per-user `{user_id, locale}` → ZSET of (query_normalized, score); ZADD on each query; ZREMRANGEBYRANK to cap at last 500 entries; recency decay via half-life ~7 days
- **Spell Correction (secondary trie)**: SymSpell / Symmetric Delete Algorithm — pre-generates all edit-distance-1 variants and stores in trie; O(L) typo-tolerant lookup where L = prefix length

### Unique Data Models
- **query_stats** (PostgreSQL): query_normalized, query_display, language, country_code, frequency_7d, frequency_24h, frequency_1h, trend_score, last_updated
- **trie_snapshots**: snapshot_id, locale, gcs_path (GCS binary blob), query_count, build_duration_ms, is_active, checksum_sha256
- **suggestion_blacklist**: term_hash, term_normalized, category, added_at, expires_at
- **user_query_history**: user_id, locale, query_normalized, query_display, last_used, use_count
- **TrieNode** (in-memory binary): children (HashMap), is_end (bool), top_k_cache (SuggestionEntry array with query_str, score, trend_boost), access_count (u32)
- **Delta layer** (Redis ZSET): ZRANGEBYLEX `[prefix, [prefix\xff` for prefix scan; 15-minute TTL per trending entry; merged with trie results at API server

### Unique Scalability / Design Decisions
- **Shard by locale (not by key range)**: one fully independent trie per (language, country_code) pair; locales have independent update cadences; no cross-locale scoring noise; if a single locale (e.g., English US) is too large, sub-shard by first 2 characters (~50 machines)
- **Top-K pre-cached at every trie node**: O(1) lookup instead of O(subtree) DFS at query time; critical for < 5ms server-side latency budget; trade-off: trie memory grows (each node stores top-K suggestion structs)
- **50ms client-side debounce**: client waits 50ms after last keypress before sending RPC; reduces request volume by ~60%; invisible to user (typing speed averages 200ms between keystrokes)
- **0.7 global + 0.3 personal score merge**: prevents filter bubble while providing personalization; cold-start users (no history) naturally get 100% global
- **k-anonymity threshold (≥1,000 distinct users)**: private or rare queries never surface as suggestions; protects user privacy and prevents gossip propagation

### Key Differentiator
Typeahead is the only problem in this folder where the serving data structure is a **custom compressed trie with pre-cached top-K at every node** — rather than an inverted index — because the query is always a prefix rather than individual terms, and the response time budget (< 20ms p50) leaves no room for any tree traversal at serve time; all work is precomputed and stored at each node during the offline build.

---

## Elasticsearch (System Design)

### Unique Functional Requirements
- Multi-tenant search engine: multiple clients index and query their own indices simultaneously
- Near-real-time (NRT) indexing: documents searchable within 1 second of indexing
- Full-text search with BM25 scoring + structured filtering (bool, range, term, nested)
- Aggregations: terms (faceted navigation), date_histogram, min/max/avg/sum/percentile (TDigest), cardinality (HyperLogLog++)
- Index Lifecycle Management (ILM): automatic rollover and tiered retirement (Hot → Warm → Cold → Delete)
- Cluster management: automatic shard allocation, rebalancing, and failure recovery via Raft-based master election
- Schema evolution: dynamic mapping; explicit mapping management; reindex API for field type changes

### Unique Components / Services
- **Lucene Segment**: the core storage unit; immutable once written; contains inverted index, DocValues (column store), stored fields, Bloom filter, norms, live docs bitset; multiple segments merged periodically via TieredMergePolicy
- **Translog (WAL)**: write-ahead log on each primary shard; fsync every 5 seconds; Lucene commit every 30 minutes; ensures acknowledged writes survive process crash
- **In-Memory Index Buffer**: accumulates documents; flushed to a new Lucene segment on `refresh()` (default every 1 second) — this is what makes documents searchable (NRT mechanism)
- **Master Nodes (3 dedicated)**: maintain cluster state (shard allocation, node membership, index settings); Raft-based election in Elasticsearch 7.0+ (replaces old `minimum_master_nodes` split-brain risk)
- **Coordinating Nodes**: stateless; parse query DSL; scatter to correct shards; merge partial results; no data stored
- **ILM (Index Lifecycle Management)**: Hot phase (NVMe, rollover at 50 GB or 1 day, forcemerge to 1 segment) → Warm phase (HDD, 7 days, 0 replicas, read-only) → Cold phase (30 days, cold allocation, searchable snapshots on S3/GCS) → Delete (90 days)
- **DocValues**: column-oriented storage (opposite of inverted index) for fields used in sorting and aggregations; avoids JVM heap pressure (Fielddata); stored on disk, read via OS page cache
- **TieredMergePolicy**: merge when a tier has ≥10 segments; 10×10 MB → 1×100 MB; reduces segment count and speeds up queries
- **Completion Suggester**: FST (Finite State Transducer)-based trie stored in the index; used for autocomplete within Elasticsearch; complement to full `typeahead` system

### Unique Data Models
- **Products index** (JSON mapping): product_id (keyword), title (text), description (text), brand (keyword), category (text), price (scaled_float), rating (half_float), in_stock (boolean), attributes (nested), suggest (completion type)
- **Time-series log index** (via Data Stream): @timestamp (date), level (keyword), service (keyword), trace_id (keyword), message (text), latency_ms (integer), status_code (short); partitioned by toYYYYMMdd; auto-rollover at 50 GB or 1 day
- **Segment file components**: `.tim` (term dictionary), `.doc` (posting lists), `.pos` (position lists), `.pay` (payloads), `.dvd/.dvm` (DocValues), `.fdx/.fdt` (stored fields), `.fnm` (field names), `.liv` (live docs bitset), `.si` (segment info), `.bloom_filter`
- **BM25 formula**: `score = IDF × TF_normalized`; TF saturation K1=1.2; length normalization B=0.75
- **ILM policy phases**: hot (rollover at 50 GB or 1 day) → warm (7 days, 0 replicas, forcemerge to 1 segment) → cold (30 days, searchable snapshots) → delete (90 days)

### Unique Scalability / Design Decisions
- **Raft-based cluster state management (ES 7.0+)**: eliminates split-brain; quorum-based master election; cluster state changes atomic via Raft log; old `minimum_master_nodes` required manual configuration and was error-prone
- **OS page cache as the most critical cache**: Lucene segment files are memory-mapped (mmap); OS page cache is the primary read cache; recommendation: half of total RAM for JVM heap, half left for OS page cache; never over-allocate JVM heap
- **DocValues instead of Fielddata for aggregations**: Fielddata loads inverted index into JVM heap on aggregation — OOM risk; DocValues stores column-oriented data on disk, accessed via OS page cache — predictable memory
- **Tiered storage with searchable snapshots**: cold tier mounts indices directly from S3/GCS without local copy; reduces hot node SSD cost 5–10× for log data; slightly higher query latency on cold tier
- **`search_after` + Point-In-Time (PIT) for deep pagination**: `from`/`size` pagination scans all matching documents regardless of depth — O(from + size) cost; `search_after` is O(page_size) constant cost; PIT creates a consistent view of the index for multi-page pagination
- **Lazy deletion with live docs bitset**: deleted documents immediately masked in `.liv` file for fast logical delete; physical removal deferred to next segment merge; storage temporarily inflated but write path stays fast
- **Multi-tenancy via index-level isolation**: separate index per tenant for strong isolation; shared index with `_source` routing for small tenants; hybrid for medium workloads
- **Bulk API with 500–1,000 doc batches**: amortizes per-request network overhead; `refresh_interval=30s` during bulk indexing reduces refresh overhead (default 1s creates too many small segments under load)

### Key Differentiator
The Elasticsearch file uniquely designs the **search engine infrastructure itself** (not just how to use a search engine), making Lucene internals (NRT refresh, translog, segment merge, DocValues), Raft-based cluster management, ILM tiered storage, and aggregation internals (HyperLogLog++, TDigest) the core deep dives — concerns that are invisible abstractions in the other three problems.

---

## Tag-Based Search

### Unique Functional Requirements
- Multi-tag boolean queries: AND, OR, NOT semantics with exact correctness (no approximation for intersection/union)
- Tag cloud: weighted visualization of top-10K tags sized by 7-day frequency
- Tag normalization pipeline: Unicode NFC, lowercase, whitespace trimming, synonym resolution — prevents duplicate tags
- Admin tag management: merge tags (with reversibility and audit log), split tags, rename tags
- Related tags: given a tag, return co-occurring tags scored by NPMI (Normalized Pointwise Mutual Information)
- Items have up to 100 tags each; 10M distinct tags; 100M content items

### Unique Components / Services
- **Roaring Bitmaps**: compressed bitset representation for mega-tags (> 1M items); 40× compression ratio vs Redis ZSET; O(|A| + |B|) intersection/union; stored in-memory on search nodes; only used for tags exceeding the ZSET size threshold
- **Dual-Write Architecture**: every tag event writes to both Redis (hot path) and PostgreSQL (durable ground truth) via Kafka consumer — Redis for < 5ms serving, PostgreSQL for ACID correctness and fallback
- **Apache Spark (nightly batch)**: computes full co-occurrence matrix (100M items × up to 100 tags = 500M association pairs); computes NPMI scores per tag pair; refreshes frequency_7d columns; top-20 related tags per tag stored in Redis ZSET
- **Tag Index Writer**: Kafka consumer (100 instances for 100 partitions); writes `ZADD tag:inv:{tag_id} {timestamp} {item_id}` to Redis and batches inserts to PostgreSQL every 100ms
- **Tiered Posting List Storage**: < 100 items → PostgreSQL item_tags (rare tag, B-tree index); 100–1,000,000 items → Redis ZSET (medium tag); > 1,000,000 items → Roaring Bitmap in-memory on search nodes (mega-tag)

### Unique Data Models
- **tags** (PostgreSQL): tag_id, name, display_name, description, frequency (total), frequency_7d, frequency_24h, trend_score, is_active, canonical_tag_id (for merged tags)
- **tag_aliases** (PostgreSQL): alias_name, canonical_tag_id (supports tag merge/rename without breaking existing references)
- **item_tags** (PostgreSQL, hash-sharded by item_id, 20 shards): item_id, tag_id, tagged_at, tagged_by
- **tag_cooccurrence** (PostgreSQL): (tag_id_a, tag_id_b), cooccurrence_count, npmi_score, computed_at
- **Redis structures**: `tag:inv:{tag_id}` (ZSET: member=item_id, score=tagged_at); `tag:cloud` (ZSET: top-10K, score=frequency_7d); `tag:trending:{locale}` (ZSET: score=trend_velocity); `tag:related:{tag_id}` (ZSET: top-20 related tags, score=npmi_score); `tag:count:{tag_id}` (counter); `tag:count:24h:{tag_id}` (counter with 25h TTL)
- **Autocomplete trie**: PATRICIA trie covering top-500K tags; rebuilt every 10 minutes; snapshotted to Redis as binary blob for service restart recovery

### Unique Scalability / Design Decisions
- **Tiered posting list storage by tag popularity**: storing all tags in Redis ZSET would require terabytes; storing all in Roaring Bitmap wastes CPU on small sets; the three-tier strategy (PostgreSQL / Redis ZSET / Roaring Bitmap) matches data structure to tag cardinality
- **AND query optimization: process smallest posting list first**: intersecting A∩B∩C when |A|<|B|<|C| gives O(|A|) loop iterations instead of O(|C|); reduces work by orders of magnitude on selective multi-tag queries
- **NPMI (not raw co-occurrence) for related tags**: raw co-occurrence favors common tags (every post has "technology") regardless of real association; NPMI normalizes by individual tag frequency; measures how much the co-occurrence exceeds chance: `npmi = log(P(A,B) / (P(A)×P(B))) / -log(P(A,B))`
- **Tag merge as background async job with audit log**: tag merge updates tag_aliases and canonical_tag_id fields; invalidates Redis ZSET for the merged tag; reversible via audit log replay; protected by Redis SETNX distributed lock to prevent concurrent merges on the same tag
- **k-anonymity threshold + blacklist at both write and query time**: offensive tags blocked at ingestion (Tag Index Writer checks blacklist before writing) and at query time (API layer filters results) — defense in depth
- **Kafka partition count = sharding factor**: 100 partitions for 100 Tag Index Writer instances; adding partitions and consumers scales write throughput linearly

### Key Differentiator
Tag-based search is the only problem in this folder focused on **exact boolean multi-set operations** (AND/OR/NOT on posting lists with correct results) rather than relevance-scored full-text matching — requiring three distinct posting list data structures (PostgreSQL / Redis ZSET / Roaring Bitmap) selected by tag cardinality, combined with the unique **tag management operations** (merge, split, normalize, NPMI-based related tags) that have no equivalent in the other three problems.
