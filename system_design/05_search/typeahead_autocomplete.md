# System Design: Typeahead / Autocomplete

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Prefix Matching**: As the user types each character, the system returns the top-K (K=5 to 10) suggested completions for the current prefix string.
2. **Ranking**: Suggestions are ranked by global popularity (query frequency) as the primary signal, with optional personalisation as a secondary signal.
3. **Personalisation**: For authenticated users, recently typed or clicked queries should boost their personal suggestions.
4. **Real-time Updates**: Trending topics (viral news events, breaking stories) must surface in suggestions within 15 minutes of spiking.
5. **Multi-language Support**: System handles queries in any of 100+ languages; suggestions are scoped to the user's language/locale.
6. **Typo Tolerance**: Minor typos in the prefix should still produce relevant completions (fuzzy prefix matching).
7. **Blacklisting**: Offensive, illegal, or brand-unsafe suggestions must be suppressed.

### Non-Functional Requirements

1. **Latency**: P99 < 100 ms; P50 < 20 ms end-to-end (client keypress to suggestions rendered). The system must feel instantaneous.
2. **Availability**: 99.99% uptime. Autocomplete outage degrades UX but doesn't block search; graceful degradation is acceptable.
3. **Throughput**: ~10× the base search QPS = ~1 million requests/second (each search query triggers 5-10 autocomplete RPCs as user types).
4. **Consistency**: Eventual consistency acceptable. A new trending term appearing with 15-minute lag is fine.
5. **Scale**: Index covers top-1 billion distinct queries (covering ~95% of all search query frequency mass).
6. **Data Freshness**: Suggestion weights updated from query logs every 10 minutes for trending detection; full recomputation daily.

### Out of Scope

- Full semantic query understanding (intent classification, entity resolution) — handled by the search backend.
- Spell correction for completely unrelated words (handled by the spell correction pipeline, not autocomplete).
- Voice input autocomplete.
- Autocomplete for search fields inside third-party apps (different product, different SLA).

---

## 2. Users & Scale

### User Types

| User Type          | Description                                                                   |
|--------------------|-------------------------------------------------------------------------------|
| Anonymous User     | No personalisation; receives global popularity-ranked suggestions.            |
| Authenticated User | Receives personalised suggestions blending personal history with global ranks.|
| Locale-specific    | Suggestions scoped to language + country (e.g., 'fr-FR' vs 'fr-CA').         |

### Traffic Estimates

Assumptions:
- Base search QPS: ~100,000 queries per second.
- Each search session involves ~6 character keypresses on average before the user either selects a suggestion or submits.
- Autocomplete requests per search session: 6.
- Peak-to-average ratio: 3x.

| Metric                              | Calculation                                       | Result            |
|-------------------------------------|---------------------------------------------------|-------------------|
| Autocomplete RPC QPS (average)      | 100,000 QPS × 6 keypresses                        | 600,000 RPS       |
| Autocomplete RPC QPS (peak)         | 600,000 × 3                                       | 1,800,000 RPS     |
| Unique prefixes to handle           | 1B distinct queries × avg 6 prefix lengths        | 6B prefix entries |
| Query log events per day            | 8.5B searches × 1 logged entry each               | 8.5B events/day   |
| Query log ingest rate               | 8.5B / 86,400                                     | ~98,400 events/s  |

### Latency Requirements

| Operation                         | P50 Target | P99 Target | Notes                                       |
|-----------------------------------|------------|------------|---------------------------------------------|
| Prefix lookup (server side)       | 5 ms       | 20 ms      | Network + compute combined                  |
| End-to-end (keypress to render)   | 15 ms      | 100 ms     | Includes client debounce, network, render   |
| Suggestion weight update          | —          | 15 minutes | For trending queries                        |
| Blacklist enforcement             | < 1 ms     | < 5 ms     | Applied in-process before response          |

### Storage Estimates

Assumptions:
- Top-1 billion distinct query strings in the trie.
- Average query length: 25 characters.
- Trie node stores: character (1 byte), children pointer (8 bytes), is_end flag (1 byte), top-K suggestions list (K=10 × 50 bytes each = 500 bytes at leaf/popular nodes).
- Many nodes are shared (prefix sharing), so effective storage is less than 1B × 25 chars.

| Component                         | Calculation                                              | Size        |
|-----------------------------------|----------------------------------------------------------|-------------|
| Trie nodes (prefix sharing ~70%)  | 1B queries × 25 chars × 0.3 unique nodes × 10 bytes     | ~75 GB      |
| Cached top-K per node             | 1B queries × 10 × 50 bytes average (only popular nodes) | ~500 GB     |
| Query frequency counters          | 1B queries × 8 bytes (int64 count)                       | ~8 GB       |
| Personalisation store (per user)  | 1B users × 100 recent queries × 50 bytes                 | ~5 TB       |
| Blacklist                         | 10M blacklisted terms × 50 bytes                         | ~500 MB     |
| **Total (global suggestions)**    |                                                          | ~600 GB RAM |

600 GB of RAM for the trie + top-K cache fits on ~10 machines with 64 GB RAM each (with replication); or 3 machines with 256 GB RAM each. This is the key insight: the trie fits in memory.

### Bandwidth Estimates

| Flow                              | Calculation                                        | Bandwidth   |
|-----------------------------------|----------------------------------------------------|-------------|
| Autocomplete requests             | 600,000 RPS × 200 bytes (avg prefix query)         | 120 MB/s    |
| Autocomplete responses            | 600,000 RPS × 1 KB (10 suggestions × 100 bytes ea.)| 600 MB/s    |
| Query log streaming ingest        | 98,400 events/s × 200 bytes (query + metadata)     | ~20 MB/s    |

---

## 3. High-Level Architecture

```
         ┌──────────────────────────────────────────────────┐
         │                  User / Browser                   │
         │  (types "clima..." → debounced keypress handler)  │
         └──────────────────────┬───────────────────────────┘
                                │ GET /autocomplete?q=clima&lang=en
                                ▼
         ┌──────────────────────────────────────────────────┐
         │              CDN Edge (PoP)                       │
         │   (cache very short-prefix results: 1-2 chars,    │
         │    TTL=60s; long-tail prefixes bypass CDN)        │
         └──────────────────────┬───────────────────────────┘
                                │ cache miss
                                ▼
         ┌──────────────────────────────────────────────────┐
         │            Load Balancer (L7)                     │
         │   (routes by lang/locale shard)                   │
         └──────────────────────┬───────────────────────────┘
                                │
         ┌──────────────────────▼───────────────────────────┐
         │          Autocomplete API Servers                 │
         │   (stateless; handles auth, rate limiting,        │
         │    personalisation merge, blacklist filtering)    │
         └──────┬───────────────────────────┬───────────────┘
                │                           │
   ┌────────────▼──────────┐    ┌───────────▼────────────────┐
   │   Trie Serving Nodes  │    │   Personalisation Store     │
   │  (in-memory trie,     │    │  (Redis / Bigtable;         │
   │   sharded by locale;  │    │   user_id → recent queries) │
   │   top-K cached per    │    └────────────────────────────┘
   │   prefix node)        │
   └───────────────────────┘
           ▲
           │ periodic trie updates (every 10 min)
           │
   ┌───────┴───────────────────────────────────────────────┐
   │               Suggestion Builder (Offline)             │
   │                                                        │
   │  Query Log Stream                                      │
   │  (Kafka ← search frontend logs every query)           │
   │       ↓                                                │
   │  Stream Processor (Flink/Dataflow)                     │
   │  (rolling 24h window; count query frequency;           │
   │   trending: exponential moving average;                │
   │   output: sorted (query, score) pairs)                 │
   │       ↓                                                │
   │  Trie Builder Job (batch, 10 min cadence)              │
   │  (builds new trie from top-1B queries by score;        │
   │   serialises to binary; hot-swaps on serving nodes)   │
   └────────────────────────────────────────────────────────┘
```

**Component Roles:**

| Component                  | Role                                                                                     |
|----------------------------|------------------------------------------------------------------------------------------|
| CDN Edge                   | Caches responses for very short prefixes (1-3 chars) which are extremely hot and stable. |
| Load Balancer              | Routes requests to trie serving nodes, partitioned by locale (language + country).       |
| Autocomplete API Server    | Handles HTTP, auth, rate limiting; merges global + personalised suggestions; filters blacklist. |
| Trie Serving Nodes         | Holds in-memory compressed trie; performs O(L) prefix lookup (L = query length); returns top-K. |
| Personalisation Store      | Key-value store: user_id → list of recent/frequent personal queries with boost scores.   |
| Query Log Stream (Kafka)   | Receives every search query as an event; source of ground truth for popularity.          |
| Stream Processor (Flink)   | Computes rolling 24-hour query frequency counts and trending scores in real-time.        |
| Trie Builder               | Periodically serialises the top-1B queries into a new trie snapshot; ships to serving nodes. |

**Primary Use-Case Data Flow (user types "clima"):**

1. User types 'c', 'l', 'i', 'm', 'a' with 50ms debounce → one RPC fired with `q=clima&lang=en-US`.
2. CDN edge: cache miss (long-tail prefix).
3. API server: extract `lang=en-US`, route to en-US trie shards.
4. Trie serving node: traverse trie from root → 'c' → 'l' → 'i' → 'm' → 'a'. At the 'a' node, read cached top-10 completions: ["climate change", "climate change effects", "climate definition", ...].
5. API server: if user is authenticated, fetch personal suggestions from Personalisation Store (Redis lookup by user_id, O(1)).
6. Merge: interleave personal suggestions (if prefix matches) with global top-10. Re-rank using: `score = 0.7 × global_score + 0.3 × personal_score`.
7. Blacklist filter: remove any suggestion matching blacklist (hash set lookup).
8. Return top-5 to client in < 20 ms.

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────
-- Query Frequency Log (aggregated)
-- ─────────────────────────────────────────
CREATE TABLE query_stats (
    query_normalized  VARCHAR(512) PRIMARY KEY,  -- lowercase, whitespace-normalised query
    query_display     VARCHAR(512) NOT NULL,      -- display form (preserves capitalisation for proper nouns)
    language          CHAR(8)      NOT NULL,      -- BCP-47 language tag
    country_code      CHAR(2),                    -- ISO 3166-1 alpha-2; NULL = global
    frequency_7d      BIGINT       NOT NULL DEFAULT 0,  -- count in last 7 days
    frequency_24h     BIGINT       NOT NULL DEFAULT 0,  -- count in last 24 hours
    frequency_1h      BIGINT       NOT NULL DEFAULT 0,  -- count in last 1 hour (for trending)
    trend_score       FLOAT        NOT NULL DEFAULT 0,  -- EMA-based trending score
    last_updated      TIMESTAMPTZ  NOT NULL,
    INDEX idx_lang_freq (language, frequency_7d DESC),
    INDEX idx_trend (language, trend_score DESC)
);

-- ─────────────────────────────────────────
-- Trie Snapshot Metadata (for hot-swap)
-- ─────────────────────────────────────────
CREATE TABLE trie_snapshots (
    snapshot_id       BIGSERIAL    PRIMARY KEY,
    locale            CHAR(8)      NOT NULL,    -- e.g. 'en-US'
    created_at        TIMESTAMPTZ  NOT NULL,
    gcs_path          VARCHAR(512) NOT NULL,    -- path to serialised binary trie on GCS
    query_count       BIGINT       NOT NULL,    -- number of queries in this snapshot
    build_duration_ms INT,
    is_active         BOOLEAN      DEFAULT FALSE,
    checksum_sha256   CHAR(64)
);

-- ─────────────────────────────────────────
-- Blacklist
-- ─────────────────────────────────────────
CREATE TABLE suggestion_blacklist (
    term_hash         CHAR(64)     PRIMARY KEY,  -- SHA-256 of normalised term
    term_normalized   VARCHAR(512) NOT NULL,
    category          VARCHAR(64),               -- 'profanity', 'legal', 'brand_safety'
    added_at          TIMESTAMPTZ  NOT NULL,
    added_by          VARCHAR(128),              -- system or reviewer ID
    expires_at        TIMESTAMPTZ                -- NULL = permanent
);

-- ─────────────────────────────────────────
-- Personalisation (per user recent queries)
-- ─────────────────────────────────────────
-- Stored in Redis as a sorted set (ZSET) keyed by user_id:locale
-- Key:   "autocomplete:personal:{user_id}:{locale}"
-- Value: ZSET of (query_string → score) where score = recency_timestamp × frequency_weight
-- Max entries per user: 100 (TTL: 90 days)

-- Relational representation for reference:
CREATE TABLE user_query_history (
    user_id           BIGINT       NOT NULL,
    locale            CHAR(8)      NOT NULL,
    query_normalized  VARCHAR(512) NOT NULL,
    query_display     VARCHAR(512) NOT NULL,
    last_used         TIMESTAMPTZ  NOT NULL,
    use_count         INT          NOT NULL DEFAULT 1,
    PRIMARY KEY (user_id, locale, query_normalized),
    INDEX idx_user_locale_recent (user_id, locale, last_used DESC)
);
```

**Trie Node (in-memory, binary format):**

```
struct TrieNode {
    children:     HashMap<char, *TrieNode>   // 8 bytes per pointer; sparse
    is_end:       bool                        // 1 byte
    top_k_cache:  [SuggestionEntry; K]        // K=10; 500 bytes
    access_count: u32                         // hit counter for cache warming
}

struct SuggestionEntry {
    query_str:    String                      // 50 bytes avg
    score:        f32                         // 4 bytes (normalised popularity score 0..1)
    trend_boost:  f32                         // 4 bytes (real-time trending multiplier)
}
```

### Database Choice

| Component              | Option                         | Pros                                              | Cons                                          | Selected? |
|------------------------|--------------------------------|---------------------------------------------------|-----------------------------------------------|-----------|
| Trie serving           | Custom in-memory trie          | O(L) lookup; full control; fits in RAM            | Complex to build; no automatic persistence    | YES       |
| Trie serving           | Redis + prefix scan (SCAN)     | Easy ops; built-in replication                    | O(N) prefix scan; not suitable for 1B entries | No        |
| Trie serving           | Elasticsearch prefix query     | Easy to operate; fuzzy support                    | JVM overhead; not optimised for typeahead QPS | No        |
| Trie serving           | Sorted Set with prefix range   | Simple; Redis ZRANGEBYLEX for prefix              | Requires full linear scan of matching prefix  | No        |
| Personalisation store  | Redis ZSET                     | O(log N) per operation; TTL; fast eviction        | Memory cost; persistence via AOF/RDB          | YES       |
| Personalisation store  | Bigtable/DynamoDB              | Durable; scales to billions of users              | Higher latency (~5ms vs <1ms for Redis)       | Fallback  |
| Query stats / building | Apache Kafka + Flink           | High throughput; exactly-once; windowed counts    | Operational complexity                        | YES       |
| Blacklist              | In-memory hash set (app layer) | Microsecond lookup; zero network I/O              | Must be loaded at startup; no persistence     | YES       |

**Justifications:**
- **Custom in-memory trie**: At 1.8M RPS with P99 < 100 ms, the trie lookup must be sub-millisecond. A custom trie with pre-cached top-K at each node achieves O(L) lookup where L ≤ 30. Redis ZRANGEBYLEX is O(log N + M) where M is the number of matching entries, and requires a full prefix scan — unsuitable.
- **Redis ZSET for personalisation**: Per-user recent queries (max 100 entries) must be retrieved in < 2 ms. Redis sorted sets provide O(log N) ZADD/ZRANGE operations. TTL prevents stale data accumulation.

---

## 5. API Design

All endpoints served over HTTPS. Rate limits: 100 RPS per IP for anonymous, 1,000 RPS per authenticated user.

### Autocomplete Suggestions Endpoint

```
GET /autocomplete/suggestions
Host: www.google.com

Query Parameters:
  q       string  REQUIRED  Current prefix string, max 512 chars
  lang    string  OPTIONAL  BCP-47 locale, default 'en' (inferred from Accept-Language)
  country string  OPTIONAL  ISO 3166-1 alpha-2, e.g. 'US'; for geo-specific trending
  k       int     OPTIONAL  Number of suggestions, default 5, max 10
  client  string  OPTIONAL  Client type: 'web', 'mobile', 'api'; affects response shape

Authorization: Bearer <session_token>  (optional; enables personalisation)

Response: 200 OK
Content-Type: application/json
Cache-Control: public, max-age=60   (for short prefixes); no-store (for personalised)
X-Request-ID: <uuid>
X-Served-By: autocomplete-shard-us-east-42

{
  "prefix": "clima",
  "suggestions": [
    {
      "text": "climate change",
      "display_text": "climate change",
      "score": 0.982,
      "is_trending": false,
      "source": "global"
    },
    {
      "text": "climate change effects",
      "display_text": "climate change effects",
      "score": 0.943,
      "is_trending": true,
      "trend_velocity": 2.4,
      "source": "global"
    },
    {
      "text": "climate definition",
      "display_text": "climate definition",
      "score": 0.876,
      "is_trending": false,
      "source": "personal"
    }
  ],
  "latency_ms": 8
}

Errors:
  400 Bad Request       — q parameter missing
  429 Too Many Requests — Retry-After header included
```

### Trending Suggestions Endpoint (internal / analytics)

```
GET /autocomplete/trending
Authorization: Bearer <internal_token>

Query Parameters:
  lang    string  REQUIRED  Locale
  window  string  OPTIONAL  'hour' | 'day', default 'hour'
  k       int     OPTIONAL  Top-K trending, default 20

Response: 200 OK
{
  "window": "hour",
  "lang": "en-US",
  "trending": [
    { "query": "earthquake california", "velocity": 15.3, "current_count": 450000 },
    { "query": "superbowl score",       "velocity": 12.1, "current_count": 380000 }
  ]
}
```

### Blacklist Management Endpoint (ops)

```
POST /autocomplete/blacklist
Authorization: Bearer <ops_token>
Content-Type: application/json

Body:
{
  "terms": ["offensive phrase here"],
  "category": "profanity",
  "expires_at": null
}

Response: 200 OK
{
  "added": 1,
  "already_present": 0,
  "effective_immediately": true
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Trie Data Structure & Top-K Caching

**Problem it solves:**
Given a prefix string (e.g., "clima"), the system must return the K most popular completions matching that prefix out of 1 billion candidate strings, in under 5 ms at 1.8M RPS.

**Approaches Comparison:**

| Approach                          | Lookup Time         | Memory        | Top-K Support       | Update Complexity | Notes                              |
|-----------------------------------|---------------------|---------------|---------------------|-------------------|------------------------------------|
| Naive linear scan                 | O(N)                | O(N)          | O(N log K) heap     | O(1)              | N=1B: completely infeasible        |
| Sorted array + binary search      | O(log N)            | O(N)          | O(K) scan after     | O(N)              | No prefix grouping; slow insert    |
| Hash map (prefix → list)          | O(1)                | O(N × L)      | Pre-sorted          | O(L) per query    | Memory explodes (L prefixes each)  |
| Trie (no caching)                 | O(L)                | O(N × σ)      | O(subtree) DFS      | O(L)              | DFS at each node too slow          |
| Trie + top-K cache at each node   | O(L)                | O(N × σ + K×N)| O(1) read           | O(L × log K)      | **OPTIMAL — selected approach**    |
| Ternary Search Tree               | O(L × log σ)        | Lower         | DFS needed          | O(L)              | Better memory; slower lookup       |
| PATRICIA trie (compressed)        | O(L)                | Lower         | Cache needed        | Complex           | Good memory/speed tradeoff         |

**Selected Approach: Compressed Trie (PATRICIA) with Pre-cached Top-K at Every Node**

Core idea: At every internal node of the trie, pre-cache the top-K most popular completions from the entire subtree rooted at that node. A lookup for prefix P traverses to node P (O(L) steps) and reads the cached list (O(1)). No subtree traversal needed at query time.

```
// Trie node structure
type TrieNode struct {
    children    map[rune]*TrieNode    // sparse children map (Unicode-aware)
    isEnd       bool                   // true if this node represents a complete query
    score       float32                // popularity score of this query (0 if not end)
    topK        []Suggestion           // cached top-K suggestions for this prefix
    // Compressed: if only one child chain with no branches, collapse into single edge
    compressedEdge string              // non-nil only for PATRICIA compressed edges
}

type Suggestion struct {
    Query     string
    Score     float32
    Trending  bool
}

// Build trie from sorted (query, score) list
func BuildTrie(queries []QueryScore, k int) *TrieNode {
    root := &TrieNode{}
    for _, qs := range queries {
        insert(root, qs.Query, qs.Score)
    }
    // Post-order traversal to populate topK caches
    populateTopK(root, k)
    return root
}

func insert(node *TrieNode, query string, score float32) {
    for _, ch := range query {
        if node.children == nil {
            node.children = make(map[rune]*TrieNode)
        }
        if node.children[ch] == nil {
            node.children[ch] = &TrieNode{}
        }
        node = node.children[ch]
    }
    node.isEnd = true
    node.score = score
}

// Populate top-K cache: post-order DFS
// Returns top-K suggestions from this subtree
func populateTopK(node *TrieNode, k int) []Suggestion {
    if node == nil {
        return nil
    }
    // Collect all suggestions from children
    all := make([]Suggestion, 0)
    for _, child := range node.children {
        childSuggestions := populateTopK(child, k)
        all = append(all, childSuggestions...)
    }
    if node.isEnd {
        all = append(all, Suggestion{Score: node.score})
    }
    // Sort by score descending; keep top K
    sort.Slice(all, func(i, j int) bool { return all[i].Score > all[j].Score })
    if len(all) > k {
        all = all[:k]
    }
    node.topK = all
    return all
}

// Lookup: O(L) where L = len(prefix)
func Lookup(root *TrieNode, prefix string, k int) []Suggestion {
    node := root
    for _, ch := range prefix {
        child, ok := node.children[ch]
        if !ok {
            return nil  // no completions for this prefix
        }
        node = child
    }
    return node.topK  // O(1): pre-cached
}
```

**Memory optimisation — PATRICIA compression:**
Chains of nodes with only one child (no branching) are collapsed into a single node with a `compressedEdge` string. This reduces the number of nodes by ~70% for typical English word tries.

**Example:**
```
Without compression:    c → l → i → m → a → t → e
With PATRICIA:         c → (limate) node  [single edge, 6 chars]
```

**Top-K cache update on new popular query:**
When a query's score increases (from real-time trending or daily recount), we need to update the top-K caches of all ancestor nodes in the trie (L ancestors for a query of length L). With K=10 and L=25, this is 25 min-heap insertions — O(L log K) total. Acceptable for offline batch updates; for real-time updates, we maintain a separate delta suggestion layer (see §6.2).

**Interviewer Q&A:**

1. **Q: How does storing top-K at every node affect memory? Doesn't it explode for a 1B-query trie?**
   A: The trie for 1B queries has approximately 1B × average_shared_prefix_length unique nodes. But we only store top-K caches at the most frequently accessed nodes (determined by access_count hit counter). Leaf nodes and rarely-accessed deep nodes store no cache; only nodes from the root down to depth ~10 (covering ~99% of queries, since most queries are 3-10 chars of prefix) store the full top-K. This reduces the top-K storage from 1B nodes × 500 bytes to ~10^7 frequently-accessed nodes × 500 bytes = ~5 GB. The access_count is used to lazily populate the cache on a background thread.

2. **Q: How do you handle Unicode characters (Chinese, Arabic, Emoji) in the trie?**
   A: The trie uses Unicode code points (rune in Go, char32 in C++) as edge labels. For CJK characters (Chinese/Japanese/Korean), the branching factor per node is higher (thousands of possible characters vs 26 for ASCII) but the query length in characters is shorter (Chinese queries average 4-8 characters). The sparse children map (hash map per node) handles high branching factors without wasting memory on empty slots. For Arabic RTL queries, the system normalises direction before insertion.

3. **Q: The trie is rebuilt every 10 minutes. How do you avoid downtime during the swap?**
   A: Blue-green deployment at the data structure level: the serving node holds two trie references (`active_trie` and `standby_trie`). The builder loads the new trie into `standby_trie` (takes ~2 minutes to deserialise 600 GB from GCS). When ready, an atomic pointer swap updates `active_trie`. In-flight requests on the old trie complete normally; new requests read the new trie. Old trie's memory is freed by the GC after all references are dropped. Zero downtime, ~1 ms for the pointer swap itself.

4. **Q: What if the same query appears in multiple languages? How is the trie partitioned?**
   A: One trie instance per locale (language + country code). The load balancer routes requests to the correct locale shard based on the `lang` parameter. "clima" in `es-MX` and "clima" in `es-ES` are served by different tries with region-specific popularity data. This also allows independent update cadences per locale and isolates locale-specific failures.

5. **Q: How does top-K caching handle ties in score?**
   A: Ties are broken by recency (more recently queried wins), then alphabetically (for determinism). The comparison function in the sort is: `if scores equal: compare last_seen_timestamp desc; if still equal: compare query_str asc`. This ensures stable, deterministic ordering that prevents flickering suggestions.

---

### 6.2 Distributed Trie & Real-Time Updates

**Problem it solves:**
A single trie for 1B queries is ~600 GB — too large for one machine. Additionally, trending queries (viral tweets, breaking news) must surface within 15 minutes. These two requirements drive the distributed and real-time architecture.

**Approaches Comparison:**

| Approach                              | Scalability | Freshness    | Complexity | Notes                                    |
|---------------------------------------|-------------|--------------|------------|------------------------------------------|
| Single-node trie (replication only)   | Limited     | Batch-only   | Low        | Single machine bottleneck at 600 GB      |
| Shard trie by first character         | Medium      | Batch-only   | Low        | Hotspot: 'a' prefix much more popular    |
| Shard by hash of first 2 chars        | High        | Batch-only   | Medium     | Even distribution; 26²=676 possible shards|
| Shard by locale                       | High        | Batch-only   | Low        | Natural partitioning; isolation          |
| Two-layer: base trie + delta layer    | High        | Real-time    | High       | Best freshness; complexity manageable    |

**Selected: Shard by Locale + Two-Layer (Base Trie + Real-Time Delta Layer)**

```
System architecture for real-time updates:

Query Log → Kafka Topic (partitioned by locale)
                │
                ▼
     Flink Streaming Job (per locale partition)
       - Rolling 1-hour window counts
       - Compute EMA trend score:
           trend_score_new = α × count_last_10min + (1-α) × trend_score_old
           α = 0.3 (weighting recent data more heavily)
       - Emit (query, updated_score) when score changes > 5%
                │
                ▼
     Delta Suggestions Store (Redis Sorted Set, per locale)
       Key: "delta:en-US"
       Value: ZSET { "climate change earthquake": score=15.3_trending, ... }
       TTL per entry: 30 minutes (auto-expire stale trending queries)
                │
                ▼
     Autocomplete API Server (at query time):
       1. Lookup base trie → [s1, s2, ..., s10]
       2. Lookup delta ZSET: ZRANGEBYLEX "delta:en-US" [prefix ... [prefix\xFF
          → returns trending queries matching prefix
       3. Merge: interleave trending from delta with base trie results
          trending items get score multiplier: base_score × (1 + trend_boost)
       4. Re-rank merged list; return top-5
```

**Trending Score Computation:**

```python
# Exponential Moving Average for trend detection
# Runs in Flink with 10-minute tumbling windows

class TrendingAggregator:
    def __init__(self, alpha=0.3, threshold=2.0):
        self.alpha = alpha           # EMA smoothing factor
        self.threshold = threshold   # trend_score > threshold → "trending"
        self.state = {}              # query → (ema_count, last_update)

    def process_window(self, window_counts: dict[str, int]):
        """Called every 10 minutes with new window query counts"""
        for query, count in window_counts.items():
            if query in self.state:
                prev_ema, _ = self.state[query]
                new_ema = self.alpha * count + (1 - self.alpha) * prev_ema
            else:
                new_ema = count
            self.state[query] = (new_ema, current_time())

        # Detect trending: current count >> EMA (spike detection)
        for query, count in window_counts.items():
            ema, _ = self.state[query]
            # Velocity = ratio of current count to historical average
            velocity = count / max(ema, 1)
            if velocity > self.threshold:
                emit_trending(query, velocity, count)
```

**Interviewer Q&A:**

1. **Q: How do you handle the case where a trending query doesn't exist in the base trie at all (completely new query)?**
   A: The delta layer handles this. The Redis ZSET `delta:en-US` is scanned for prefix matches at query time using `ZRANGEBYLEX`. A new trending query like "earthquake oakland" would appear in the delta store within 10 minutes of its traffic spike, even if it's not in the base trie (which is rebuilt daily). The ZRANGEBYLEX approach: keys in the ZSET are sorted lexicographically, so a range scan `["earthquake o", "earthquake o\xFF"]` efficiently retrieves all matching entries. Complexity: O(log N + M) where N = delta set size and M = number of matches; with N typically < 100,000 trending queries, this is fast.

2. **Q: How does the two-layer approach handle duplicates — a query in both the base trie and the delta layer?**
   A: The merge function deduplicates by query string. If a query appears in both layers, the delta score acts as a multiplicative boost: `final_score = base_score × (1 + trend_velocity / 10)`. A query with base_score=0.5 and trend_velocity=5 gets final_score = 0.5 × 1.5 = 0.75, elevating it above non-trending queries with higher base popularity.

3. **Q: What prevents the delta layer from growing indefinitely with all trending queries?**
   A: Three mechanisms: (1) TTL on Redis entries — each trending query entry expires after 30 minutes of no updates. If the trend dies, the EMA falls below threshold and the entry is removed. (2) Max delta size — if the delta set exceeds 1M entries, we apply LRU eviction on the lowest-velocity entries. (3) Daily recount — when the base trie is rebuilt daily, all trending queries that have sustained popularity are incorporated into the base trie, so they no longer need to live in the delta layer.

4. **Q: How do you scale the Kafka → Flink pipeline to handle 98,400 query log events per second?**
   A: Kafka is partitioned by locale (100 partitions for 100 locales). Each Flink job instance reads one partition, processes its locale's queries, and emits to Redis. 100 parallel Flink workers handle 98,400 / 100 = ~984 events/second per partition — trivially manageable. Flink's windowed aggregation (10-minute tumbling windows with checkpointing) provides exactly-once semantics. The Redis writes (ZADD) are O(log N) and batched: Flink outputs a batch of score updates every 10 minutes rather than per-event.

5. **Q: How would you scale the trie sharding if a single locale (e.g., English) is too large for one machine?**
   A: Shard the English trie by the first 2 characters of the prefix. The load balancer extracts the first 2 chars of the query prefix and routes to the appropriate shard. 26 × 26 = 676 possible 2-char prefixes, but in practice only ~200 are common in English. Each shard handles a subset: shard 1 handles "aa"-"az", shard 2 handles "ba"-"bz", etc. This distributes memory across ~50 machines for English. The scatter/gather overhead is zero since each query has a deterministic shard — no fan-out needed.

---

### 6.3 Personalisation at Scale

**Problem it solves:**
Global popularity rankings return the same suggestions to everyone. A user who frequently searches for "python programming" should see "python flask tutorial" as a suggestion when they type "pyt", even if "python snake" is globally more popular.

**Approaches Comparison:**

| Approach                           | Quality    | Latency     | Privacy    | Storage  | Notes                               |
|------------------------------------|------------|-------------|------------|----------|-------------------------------------|
| No personalisation                 | Baseline   | Zero        | Perfect    | None     | Misses user intent entirely         |
| Last N queries                     | Medium     | Low         | Good       | Low      | Simple; poor long-term model        |
| Collaborative filtering            | High       | High        | Poor       | High     | "Users like you searched..." risky  |
| Per-user query frequency vector    | High       | Medium      | Acceptable | Medium   | Good balance; selected approach     |
| On-device personalisation          | High       | Zero server | Perfect    | Device   | Best privacy; limited by device mem |

**Selected: Per-User Query Frequency Vector with Recency Weighting (Server-Side)**

```python
# Personalisation at query time

def get_personal_suggestions(user_id: str, prefix: str, locale: str, k: int) -> list[Suggestion]:
    # Step 1: fetch user's recent query history from Redis (ZREVRANGEBYSCORE)
    redis_key = f"autocomplete:personal:{user_id}:{locale}"
    # Get top-100 user queries by score (recency × frequency weight)
    user_queries = redis.zrevrange(redis_key, 0, 99, withscores=True)
    # user_queries: [("python flask tutorial", 0.95), ("python pandas", 0.88), ...]

    # Step 2: filter to those matching the current prefix
    matching = [
        Suggestion(query=q, score=s, source="personal")
        for q, s in user_queries
        if q.startswith(prefix)   # O(100) — bounded and fast
    ]

    # Step 3: apply recency decay
    now = time.time()
    for s in matching:
        last_used = redis.hget(f"autocomplete:personal_meta:{user_id}", s.query)
        age_days = (now - float(last_used)) / 86400
        s.score *= math.exp(-0.1 * age_days)  # decay: half-life of ~7 days

    return sorted(matching, key=lambda x: x.score, reverse=True)[:k]

def update_personal_history(user_id: str, query: str, locale: str):
    """Called when user submits a search (not on every autocomplete keypress)"""
    redis_key = f"autocomplete:personal:{user_id}:{locale}"
    now = time.time()
    # Compute recency-weighted score:
    # First use: score = 1.0 (timestamp as tie-break)
    # Repeated use: score increments (ZINCRBY)
    score = now / 1e10  # normalise timestamp to 0..1 range (valid for ~300 years)
    redis.zadd(redis_key, {query: score}, incr=True)
    redis.expire(redis_key, 90 * 86400)  # 90-day TTL on the key
    # Trim to max 100 entries (evict oldest)
    redis.zremrangebyrank(redis_key, 0, -101)  # keep top 100 by score
```

**Merging Personal + Global Suggestions:**

```
def merge_suggestions(global_top10, personal_top5, prefix):
    merged = {}
    # Add global suggestions
    for s in global_top10:
        merged[s.query] = s.score * 0.7  # global weight

    # Add/boost personal suggestions
    for s in personal_top5:
        if s.query in merged:
            # Query exists globally: apply personalisation boost
            merged[s.query] += s.score * 0.3
        else:
            # Personal query not in global top-10: add with lower weight
            merged[s.query] = s.score * 0.3

    # Sort by final score, return top-5
    return sorted(merged.items(), key=lambda x: x[1], reverse=True)[:5]
```

**Interviewer Q&A:**

1. **Q: How do you prevent personal query history from leaking between users or sessions?**
   A: Redis keys are scoped to user_id (opaque internal identifier, not email). The API server enforces that a request with session_token X can only access the Redis key for the user_id associated with that token — validated by the auth middleware before any Redis call. Keys are never exposed to clients. For extra isolation, the personalisation Redis cluster is a separate instance from all other caches, with IAM-level access control restricting which services can read/write.

2. **Q: What about users who search very rarely — their history is sparse. How do you handle cold-start?**
   A: Cold-start users (< 5 personal queries) fall back to pure global suggestions for the first few queries. As the personal history accumulates, the personalisation weight in the merge formula increases gradually: `personal_weight = min(0.3, 0.05 × len(personal_history))`. This means a user with 6+ personal queries reaches full personalisation. The degradation is graceful — the user never sees empty suggestions.

---

## 7. Scaling

### Horizontal Scaling

**Trie Serving Nodes:**
- Each locale shard is replicated 3x across different availability zones.
- The trie is read-only (updated only during the swap); all 3 replicas can serve reads simultaneously (no leader needed for reads).
- Addition of a new replica: copy the trie binary from GCS, deserialise, and join the pool. No data migration required.

**API Servers:**
- Fully stateless; scale horizontally behind the load balancer.
- Session affinity is NOT required (user_id lookup goes to Redis, not the API server).
- Auto-scaling policy: CPU > 60% or RPS > 800K triggers +10% capacity addition.

### DB Sharding

| Store                  | Sharding Key           | Strategy                                                        |
|------------------------|------------------------|-----------------------------------------------------------------|
| Trie (per locale)      | locale (en-US, fr-FR…) | One trie instance per locale; replicated 3x                     |
| Personalisation Redis  | hash(user_id) % 64     | 64 Redis shards; consistent hashing for rebalancing             |
| Query stats (Postgres) | hash(query_normalized) | Range sharding by query hash; used only for batch rebuild       |

### Replication

- Trie: 3 read replicas per locale shard. All replicas receive new trie binaries simultaneously on rebuild.
- Redis personalisation: Redis Cluster with 1 primary + 2 replicas per shard. Writes go to primary; reads distributed across all replicas (eventual consistency).

### Caching

| Layer                    | What's Cached                           | TTL    | Technology       |
|--------------------------|-----------------------------------------|--------|------------------|
| CDN edge                 | 1-3 char prefix responses (global only) | 60s    | CDN (Cloudflare/Akamai) |
| API server in-process    | Last 10K popular prefix→suggestions     | 30s    | LRU (in-process) |
| Trie top-K               | Top-K suggestions per trie node         | ∞ (until trie swap) | In-memory (trie itself) |
| Personalisation          | Per-user history (100 queries)          | 90 days| Redis ZSET       |

### CDN Strategy

- Short prefixes (1-3 chars: "a", "go", "the") are the same for all global users. These are cache-friendly and served from CDN with TTL=60s. They account for ~20% of autocomplete traffic but are served without hitting backend.
- Prefixes ≥ 4 chars or personalised requests bypass CDN (`Cache-Control: no-store`) and go directly to API servers.
- CDN key: `{prefix}:{locale}:{k}` (personalised responses excluded from CDN).

**Interviewer Q&A:**

1. **Q: With 1.8M RPS peak, how many trie serving machines do you need?**
   A: Assumptions: each trie lookup takes ~0.5 ms of CPU (O(L) traversal + top-K read from memory). Each machine handles ~2,000 lookups/second single-threaded; with 32 cores and async I/O, ~50,000 RPS per machine. For 1.8M peak RPS across ~50 locales, the English trie (say 60% of traffic = 1.08M RPS) needs 1,080,000 / 50,000 = ~22 machines × 3 replicas = 66 machines. All 50 locales together need ~150-200 machines. This is very manageable.

2. **Q: How do you handle the Redis personalisation cluster being a bottleneck at 1.8M RPS?**
   A: Not all 1.8M requests require personalisation. (a) Anonymous users (~70% of traffic) skip personalisation entirely. (b) Debouncing: the client sends autocomplete requests only after 50ms of inactivity, reducing effective RPS. (c) Request coalescing: the API server batches Redis lookups for the same user_id within a 5ms window (common for fast typists). (d) In-process cache: each API server caches the last 1,000 user personalisation lookups for 10 seconds. With these mitigations, Redis handles ~100K personal-lookup RPS — distributed across 64 shards = 1,562 RPS per shard, trivially manageable.

3. **Q: How do you ensure the trie rebuild doesn't degrade query quality at the moment of the swap?**
   A: The new trie is fully built and validated before the swap. Validation checks: (1) Query count matches expected range (±5% of previous trie). (2) Top-100 global suggestions match human-curated golden set. (3) A canary check: run 1,000 test queries against the new trie and compare results with the old trie; flag large divergences. Only after passing validation is the atomic pointer swap executed. The swap takes ~1 ms; in-flight requests complete on the old trie (no lock needed for readers).

4. **Q: How would you handle autocomplete for search across a private enterprise corpus (e.g., internal documents)?**
   A: Two changes: (1) The trie is built from document titles and frequent user search terms within the enterprise, not from public query logs. (2) The trie must be filtered by ACL at query time — a user should only see suggestions for documents they have access to. This requires either per-user tries (infeasible) or post-retrieval ACL filtering (fetch top-50 suggestions, filter to those the user can access, return top-5). ACL lookup cost is mitigated by caching the user's permission set per session.

5. **Q: What's your strategy for handling offensive content in suggestions?**
   A: Three-layer defence: (1) Blacklist (proactive) — known offensive terms are in a hash set; any suggestion matching is removed before returning. Hash lookup is O(1). (2) Classifier (reactive) — a text classifier (trained on labelled offensive content) runs on new query terms before they're admitted to the trie during rebuild. (3) Rate-limit escalation — if a prefix generates an unusually high fraction of blacklisted suggestions (e.g., a query modified to evade the blacklist), a circuit breaker disables suggestions for that prefix temporarily and alerts the moderation team.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario                      | Impact                                    | Mitigation                                                                          | Recovery |
|---------------------------------------|-------------------------------------------|-------------------------------------------------------------------------------------|----------|
| Trie serving node failure             | Reduced capacity for a locale             | 3 replicas; load balancer removes failed node; remaining 2 absorb traffic            | < 30s    |
| Trie rebuild failure (bad data)       | Stale suggestions served                  | Validation before swap; if rebuild fails, serve previous trie; alert sent            | Auto     |
| Redis personalisation cluster failure | No personalisation; global-only responses | Graceful degradation: API server detects Redis error, returns global suggestions     | Minutes  |
| Kafka lag (query log delay)           | Trending detection slowed                 | Not critical to core functionality; staleness within 30min still acceptable          | Auto     |
| CDN outage                            | All traffic hits API servers              | API servers over-provisioned for this case; rate limiting prevents cascade           | Minutes  |
| Blacklist update failure              | Offensive suggestions may appear          | Immutable blacklist copy in API server memory; refreshed every 5 min; fail-safe keeps old copy | Auto |

### Failover

- **Trie replicas**: Health checks every 5 seconds. Failed replica removed from pool within 10 seconds. New replica promoted from warm standby (already loaded) in < 30 seconds.
- **Redis Cluster**: Built-in Redis Sentinel detects primary failure; promotes a replica in ~30 seconds. Writes during this window are buffered in API server memory (max 30 seconds of writes, then discarded — personalisation is best-effort).
- **Region-level failure**: DNS weighted routing shifts traffic to adjacent regions within 2 minutes.

### Retries & Idempotency

- Autocomplete API is fully idempotent (read-only). Clients retry on 5xx with exponential backoff (100ms, 200ms, 400ms; max 3 retries).
- Redis personalisation writes are fire-and-forget; if a write fails, the user's next query will simply re-record the query. No data loss of search results — only personalisation history.

### Circuit Breaker

- Per-locale circuit breaker on trie lookups: if error rate > 20% for a locale shard in 5 seconds, circuit opens; responses return empty suggestions (graceful degradation) for that locale. Circuit half-opens after 10 seconds.
- Redis circuit breaker: if Redis call exceeds 5ms P99 for 10 consecutive requests, personalisation is bypassed for the next 30 seconds (serve global-only).

---

## 9. Monitoring & Observability

### Key Metrics

| Metric                          | Type      | Alert Threshold           | Dashboard         |
|---------------------------------|-----------|---------------------------|-------------------|
| Autocomplete latency P50/P99    | Histogram | P99 > 100 ms              | Typeahead Perf    |
| RPS (requests per second)       | Counter   | Drop > 20% from baseline  | Traffic           |
| Trie swap success rate          | Counter   | Any failure               | Index Health      |
| Personalisation cache hit rate  | Gauge     | < 50%                     | Personalisation   |
| Trending detection lag          | Gauge     | > 20 min                  | Freshness         |
| Blacklist enforcement rate      | Counter   | Spike > 2× baseline       | Content Safety    |
| Redis error rate                | Gauge     | > 0.1%                    | Infrastructure    |
| Suggestion click-through rate   | Gauge     | Drop > 5% week-over-week  | Quality           |
| Trie memory usage per node      | Gauge     | > 90% of capacity         | Capacity          |

### Distributed Tracing

- Every autocomplete request receives a `trace_id`. Spans: `api_server` → `trie_lookup` + `redis_personal_lookup` (parallel) → `blacklist_filter` → `merge`.
- Sampling: 1% of all requests; 100% of requests with latency > 50 ms (tail sampling).

### Logging

- Structured JSON per request: `{trace_id, user_id_hash (anonymised), prefix_length, locale, latency_ms, suggestions_returned, personalised: bool}`.
- Prefix is NOT logged in full (privacy); only its SHA-256 hash and length are logged.
- Metrics exported to Prometheus; dashboards in Grafana.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                     | Chosen                                    | Alternative                      | Why                                                                         |
|------------------------------|-------------------------------------------|----------------------------------|-----------------------------------------------------------------------------|
| Trie vs Elasticsearch        | Custom in-memory trie                     | Elasticsearch prefix query       | 100x lower latency; no JVM GC; full control over top-K caching             |
| Shard by locale              | One trie per locale                       | One global trie                  | Locale isolation; independent update cadences; no cross-locale noise        |
| Top-K caching                | Cached at every node                      | DFS at query time                | O(1) lookup vs O(subtree) DFS; critical for < 5ms server latency           |
| Real-time delta layer        | Redis ZSET delta + base trie              | Rebuild full trie for every update| Full rebuild takes 10 min; delta enables < 15 min freshness                |
| Personalisation storage      | Redis ZSET                                | Bigtable                         | Sub-millisecond personalisation lookup; ZSET operations fit perfectly       |
| Blacklist enforcement        | In-process hash set                       | Remote API call                  | Zero latency; remote call would add 5-20ms per request                     |
| CDN for short prefixes       | Yes, TTL=60s                              | No CDN (always hit backend)      | 1-3 char prefixes are cache-friendly; reduces backend load by ~20%         |
| Personalisation weight       | 30% personal, 70% global                 | 100% personal                    | Avoids filter bubble; ensures quality even for sparse personal history      |

---

## 11. Follow-up Interview Questions

1. **Q: How would you add fuzzy/typo-tolerant autocomplete (e.g., "gogle" → "google search")?**
   A: Approach 1 — Symmetric Delete Algorithm: for each trie entry, pre-index all 1-edit-distance variants (deletions only, since deletions are symmetric with insertions) into a secondary trie. A query "gogle" generates edit-1 deletions ("ogle", "ggle", "gole", "goge", "gogl") and looks them up in the secondary trie. Approach 2 — SymSpell: same idea but optimised with a hash map. Approach 3 — Separate spell-correction pass before trie lookup (adds latency). We prefer approach 1 (secondary edit-distance trie) for latency, accepting 2x memory overhead.

2. **Q: How would you handle autocomplete for a long-tail vertical (e.g., medical terminology) where common queries are very different from web search?**
   A: Separate trie for the vertical, built from the domain-specific query logs (e.g., PubMed searches) and domain vocabulary (medical ontologies like SNOMED CT). The main autocomplete system routes to the appropriate trie based on the search context (detected via URL/page context). Medical trie would have a smaller query corpus but higher precision requirements (misspelling a drug name is worse than a general query).

3. **Q: How do you ensure autocomplete doesn't leak unpublished or private information (e.g., if someone queries a private document title)?**
   A: The suggestion corpus is built ONLY from aggregated query logs, not from document content or titles directly. A query enters the corpus only if it was typed by a minimum number of distinct users (e.g., ≥ 1,000 unique users) in the aggregation window. This k-anonymity threshold prevents single-user private queries from appearing as suggestions. Private enterprise deployments require ACL-filtered suggestions (see Section 7, Q4).

4. **Q: How would you A/B test whether a new ranking algorithm for autocomplete suggestions improves user experience?**
   A: Primary metric: suggestion selection rate (did the user click a suggestion rather than typing the full query?). Secondary: subsequent search success (did the search that followed have a low bounce rate?). A/B setup: 1% of traffic receives new ranking algorithm; control group receives current algorithm. Stratify by locale and query volume to avoid confounding. A 0.5% improvement in suggestion selection rate on 1.8M RPS = 9,000 additional suggestions clicked per second — easily statistically significant within 24 hours.

5. **Q: How does the system handle extremely long queries (e.g., a user pastes a URL into the search box)?**
   A: Hard length limit: the API rejects queries > 512 chars (returns empty suggestions). For queries > 50 chars, autocomplete is likely unhelpful (these are typically precise copy-pastes); the client-side JavaScript skips autocomplete requests for inputs > 50 chars. The trie is also bounded to queries ≤ 100 chars (anything longer is too rare to be useful as a suggestion).

6. **Q: How would you support real-time personalised autocomplete where the user's very latest query (from 2 seconds ago) can immediately influence suggestions?**
   A: As the user types one query and hits submit, the personalisation store update (Redis ZADD) is async and immediate. However, the API server's in-process personalisation cache (10-second TTL) might not have the latest data. Solution: bypass the in-process cache for the user's own lookups (always hit Redis directly for authenticated users). Redis latency is ~1 ms; this is acceptable. The trade-off: slightly higher Redis load per authenticated request.

7. **Q: How do you handle autocomplete for structured search (e.g., "site:example.com climate")? The prefix includes operators.**
   A: Structured query operators are parsed before trie lookup. A parser splits `"site:example.com clima"` into operator tokens (`site:example.com`) and the free-text prefix (`clima`). The trie lookup is performed on the free-text prefix only; results are post-filtered to apply the operator constraint. Operator-specific suggestions (e.g., common site: operators) are pre-computed and stored in a separate small trie.

8. **Q: What happens to autocomplete quality during a major global event (e.g., World Cup final) with a 10x query spike?**
   A: The trending detection (EMA-based) will rapidly elevate event-related queries. The delta Redis ZSET updates in real-time. The CDN absorbs much of the spike for short prefixes. Auto-scaling adds API server capacity within 2-3 minutes based on CPU and RPS metrics. The main risk is the Flink → Redis pipeline lagging; mitigated by provisioning Flink at 5x normal capacity to absorb spikes.

9. **Q: How would you implement "search history" — showing the user's own past queries before suggesting popular ones?**
   A: This is similar to personalisation but with a different UX: show personal history first (as a separate "history" section), then global suggestions below. Implementation: fetch personal queries from Redis for any prefix match; display them with a different UI treatment (e.g., clock icon). The merge function always places personal history above global suggestions, regardless of score. A "clear history" button removes the user's Redis key.

10. **Q: How do you handle autocomplete for voice input where the "prefix" is a complete spoken utterance with uncertainty?**
    A: Voice ASR produces N-best hypotheses with confidence scores (e.g., "climate change" 0.9, "climate chains" 0.1). Autocomplete is called with all N hypotheses; results are scored and weighted by the ASR confidence. The trie lookup returns top-K for each hypothesis; results are merged with confidence-weighted scores. Additionally, voice autocomplete latency budget is more relaxed (users expect a slight delay after speaking vs. immediate feedback while typing).

11. **Q: Describe how you would build autocomplete for a ride-sharing app where suggestions are place names.**
    A: Key differences: (1) The corpus is geographic data (place names, addresses, POIs) not query logs. Build the trie from OpenStreetMap / Google Places data. (2) Suggestions must be geo-biased: "Downtown" in New York vs San Francisco. The trie is prefixed by geohash of the user's current location, then by the text prefix. Or: all place names in the trie; results re-ranked by distance from user. (3) Freshness matters differently: new restaurants or businesses should appear within 24 hours. (4) At a given location, results should be top-K by proximity × popularity.

12. **Q: How does the system behave when a query prefix could match both popular and highly personal suggestions with equal scores?**
    A: The merge formula uses a small deterministic tiebreaker: personal suggestions win over global suggestions when scores are equal (within a 5% tolerance band). Rationale: personalization is opt-in and represents explicit user intent; showing a user their own recent searches is almost always correct. If the user consistently ignores a personal suggestion (tracked via suggestion click logs), its personal boost decays (negative feedback loop).

13. **Q: How would you implement autocomplete for a multi-word query where only the LAST word is being typed?**
    A: The client sends the full input string. The server identifies the last incomplete word (last token) and looks it up in a word-completion trie. It also uses the preceding words as context for a bigram/trigram model: after "climate", the next word is much more likely to be "change" than "chicken". Results are scored by: `score = prefix_match_score × P(completion | preceding_words)`. The bigram probabilities are precomputed from the query log and stored as a matrix or as conditional tries (one trie per preceding word).

14. **Q: What's your approach to autocomplete for a search engine that must work offline on mobile?**
    A: On-device trie with a subset of suggestions. Download a compressed trie (~10 MB) containing the top-100K global queries for the user's locale, updated weekly over WiFi. On the device, store the user's personal history (last 100 queries) in local storage. The offline autocomplete function merges on-device global trie + local personal history — exactly the same algorithm, but all local. Network autocomplete is used as a background enrichment when online; results are blended with the local suggestions.

15. **Q: How do you measure the quality of the autocomplete system?**
    A: Primary metrics: (1) Suggestion acceptance rate (% of sessions where user clicked a suggestion vs. typed full query). (2) Time saved (average characters typed before selection vs. query length — measures effort reduction). (3) Search quality post-selection (CTR on results when search originated from autocomplete vs. manual typing). (4) Staleness of trending suggestions (lag between a topic trending and it appearing in autocomplete). (5) False negative rate (user types a query that should have been a suggestion but wasn't). Measure (5) by sampling submitted queries and checking if they were or should have been in the suggestion set.

---

## 12. References & Further Reading

1. **Google Blog — "How Autocomplete Works"** (2018). https://blog.google/products/search/how-search-works-autocomplete/

2. **Shim, J. et al. (2011)** — "Efficient Top-K Query Processing in Peer-to-Peer Networks." *VLDB 2011*. (Covers distributed top-K algorithms applicable to distributed trie.)

3. **Xiao, C. et al. (2013)** — "TrieJoin: Efficient Trie-based String Similarity Joins with Edit-Distance Constraints." *VLDB 2009*. (Edit-distance-tolerant trie traversal.)

4. **Navarro, G. (2001)** — "A Guided Tour to Approximate String Matching." *ACM Computing Surveys, 33(1)*. (Foundational text on fuzzy string matching including tries.)

5. **Sedgewick, R. & Wayne, K. (2011)** — *Algorithms* (4th ed.), Chapter on Tries (TST, PATRICIA). Princeton / Addison-Wesley. https://algs4.cs.princeton.edu/

6. **Huang, J. et al. (2015)** — "Efficient Location-Aware Prefix Search." *SIGMOD 2015*. (Geo-aware autocomplete.)

7. **Xian, Y. et al. (2022)** — "Search-Oriented Pretraining for Autocomplete." *SIGIR 2022*. (Neural autocomplete ranking.)

8. **Apache Flink Documentation** — Stateful Stream Processing and Windowed Aggregations. https://flink.apache.org/

9. **Redis Documentation** — Sorted Sets (ZADD, ZRANGEBYLEX, ZREVRANGEBYSCORE). https://redis.io/docs/data-types/sorted-sets/

10. **Damme, T. (2009)** — "Symmetric Delete Spelling Correction" (SymSpell algorithm). https://wolfgarbe.medium.com/1000x-faster-spelling-correction-algorithm-2012-8701fcd87a5f
