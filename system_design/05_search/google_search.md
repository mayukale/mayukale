# System Design: Google Search

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Web Crawling**: The system must continuously discover and fetch web pages across the public internet, following hyperlinks from seed URLs.
2. **Indexing**: Extracted text content must be parsed, tokenized, and stored in an inverted index mapping terms to document lists.
3. **Query Processing**: Users submit free-text queries; the system returns an ordered list of relevant documents.
4. **Ranking**: Results must be ranked by relevance using a combination of content signals (TF-IDF, BM25) and link-graph signals (PageRank).
5. **Snippet Generation**: Each result must include a title, URL, and a short text snippet with query terms highlighted in context.
6. **Spell Correction**: Misspelled queries must be detected and a corrected suggestion ("Did you mean: …") displayed.
7. **Freshness**: News and time-sensitive content must surface within minutes; general web pages within days.
8. **Search Verticals**: Web, images, news, and videos are in scope; specialized knowledge panels are stretch goals.

### Non-Functional Requirements

1. **Latency**: P99 query response < 200 ms end-to-end; P50 < 50 ms.
2. **Availability**: 99.99% uptime (< 53 minutes downtime per year).
3. **Scale**: Index covers ~100 billion documents; system handles ~100,000 queries per second (QPS) globally.
4. **Crawl Throughput**: Crawl and re-crawl ~15 billion pages per day.
5. **Freshness SLA**: Breaking news indexed within 5 minutes; average page re-crawled within 7 days.
6. **Consistency**: Eventual consistency acceptable — stale index for low-traffic pages is tolerable.
7. **Durability**: No data loss for indexed content; raw page store replicated 3x.

### Out of Scope

- Personalized ranking based on user search history (covered separately under personalization systems).
- Paid search advertising (a separate auction system integrates downstream).
- Voice or natural-language conversational search.
- Indexing content behind authentication walls.
- Real-time social media firehose indexing.

---

## 2. Users & Scale

### User Types

| User Type         | Description                                                                 |
|-------------------|-----------------------------------------------------------------------------|
| Anonymous Searcher| The vast majority; submits queries, reads results, no account required.     |
| Signed-in User    | Enables SafeSearch preferences, search history, and personalisation signals.|
| Webmaster         | Submits sitemaps, uses Search Console to influence crawl priority.          |
| Internal Crawler  | Automated Googlebot agents; not human but dominant traffic producers.       |

### Traffic Estimates

Assumptions:
- World population: 8 billion; internet users: ~5.5 billion.
- Google handles ~8.5 billion searches per day (Statista, 2024).
- Peak-to-average ratio: 2.5x (traffic spikes around breaking news).

| Metric                        | Calculation                                              | Result         |
|-------------------------------|----------------------------------------------------------|----------------|
| Queries per day               | Given assumption                                         | 8.5 B/day      |
| Average QPS                   | 8,500,000,000 / 86,400                                   | ~98,400 QPS    |
| Peak QPS                      | 98,400 × 2.5                                             | ~246,000 QPS   |
| Pages crawled per day         | ~15 B pages (assumption: full index refreshed ~7 days)   | 15 B/day       |
| Crawler fetch rate            | 15,000,000,000 / 86,400                                  | ~173,600 fetches/s |
| New pages discovered per day  | ~5% of crawled (net growth of the web)                   | 750 M/day      |

### Latency Requirements

| Operation                  | P50 Target | P99 Target | Rationale                                              |
|----------------------------|------------|------------|--------------------------------------------------------|
| Query → ranked results     | 50 ms      | 200 ms     | Human perception of "instant" is < 100 ms             |
| Index write (crawl → live) | — (async)  | 5 min      | News freshness SLA                                     |
| Crawler fetch per page     | 1 s        | 10 s       | Timeout avoids blocking crawler threads on slow hosts  |
| Spell correction           | 5 ms       | 20 ms      | Runs in-process with query pipeline, must be negligible|

### Storage Estimates

Assumptions:
- Average raw HTML page: 100 KB.
- After compression (gzip): ~20 KB.
- Extracted text per page: ~10 KB.
- Inverted index entry (term + posting): 12 bytes.
- Average terms per page: 1,000.
- Total indexed pages: 100 billion.

| Component             | Calculation                                               | Size         |
|-----------------------|-----------------------------------------------------------|--------------|
| Raw page store        | 100 B pages × 20 KB compressed                           | 2,000 PB     |
| Extracted text store  | 100 B pages × 10 KB                                       | 1,000 PB     |
| Inverted index        | 100 B pages × 1,000 terms × 12 bytes                     | 1,200 PB     |
| PageRank scores store | 100 B URLs × 8 bytes (float64)                            | 800 GB       |
| URL frontier queue    | 15 B URLs in queue × 128 bytes avg URL                    | ~1.9 TB      |
| Metadata store        | 100 B pages × 200 bytes (title, date, lang, canonical)   | 20 TB        |
| **Total (raw)**       |                                                           | **~4,200 PB**|

Note: In practice Google uses aggressive deduplication (SimHash near-duplicate detection) and tiered storage. Hot index tier (~top 5 B docs) fits in ~60 PB of SSD; warm and cold tiers use HDD and tape.

### Bandwidth Estimates

| Flow                              | Calculation                                          | Bandwidth       |
|-----------------------------------|------------------------------------------------------|-----------------|
| Crawler inbound (fetching pages)  | 173,600 fetches/s × 100 KB avg                       | ~17.4 TB/s      |
| Query result responses outbound   | 98,400 QPS × 30 KB avg response (10 results + meta)  | ~2.9 TB/s       |
| Index replication (3 replicas)    | Raw writes to index: ~750 M new pages/day × 12 KB    | ~104 GB/s       |

---

## 3. High-Level Architecture

```
                         ┌─────────────────────────────────────────┐
                         │              User / Browser              │
                         └───────────────────┬─────────────────────┘
                                             │ HTTPS query
                                             ▼
                         ┌─────────────────────────────────────────┐
                         │           Global Load Balancer           │
                         │   (Anycast DNS, GeoDNS routing)          │
                         └───────────────────┬─────────────────────┘
                                             │
                         ┌───────────────────▼─────────────────────┐
                         │           Frontend Servers               │
                         │  (Query parsing, spell check, SafeSearch,│
                         │   session handling, A/B experiment layer) │
                         └───────────────────┬─────────────────────┘
                                             │ parsed query + features
               ┌─────────────────────────────▼──────────────────────────┐
               │                   Query Processor                       │
               │  1. Term expansion (synonyms, stemming)                  │
               │  2. Index lookup (scatter to index shards)               │
               │  3. Score merging (BM25 × PageRank × freshness)         │
               │  4. Top-K selection (min-heap, k=1000 candidates)        │
               └──────┬──────────────────────────┬─────────────────────┘
                      │                          │
          ┌───────────▼──────────┐   ┌───────────▼──────────┐
          │   Index Serving Tier  │   │  Metadata / Doc Store │
          │  (Inverted Index      │   │  (title, URL, date,   │
          │   sharded by hash(URL)│   │   language, PageRank) │
          │   in-memory + SSD)    │   │  (Bigtable / Spanner) │
          └───────────────────────┘   └───────────────────────┘
                      │
          ┌───────────▼──────────────────────────────────────┐
          │               Ranker / Scoring Engine             │
          │  (ML model: LambdaMART / BERT re-ranker, 1000→10) │
          └───────────────────────┬──────────────────────────┘
                                  │ top 10 docIDs + scores
          ┌───────────────────────▼──────────────────────────┐
          │              Snippet Generator                    │
          │  (Fetch doc text, locate query terms, build       │
          │   context window, highlight matches)              │
          └───────────────────────┬──────────────────────────┘
                                  │ final SERP JSON
                                  ▼ back to Frontend → User

─────────────────────── CRAWL & INDEX PIPELINE ─────────────────────────

 ┌──────────────┐    ┌──────────────┐    ┌──────────────┐    ┌────────────┐
 │  URL Frontier │───▶│   Crawlers   │───▶│  Raw Page    │───▶│  Content   │
 │  (priority   │    │  (Googlebot  │    │  Store (GCS) │    │  Processor │
 │   queue,     │    │   agents,    │    │  compressed  │    │  (parse,   │
 │   politeness │    │   ~1M hosts) │    │  HTML/text)  │    │  extract,  │
 │   scheduler) │    └──────────────┘    └──────────────┘    │  tokenise) │
 └──────────────┘                                            └─────┬──────┘
        ▲ new URLs discovered                                      │
        │                                               ┌──────────▼──────────┐
        │                                               │   Indexer / Writer  │
        └───────────────────────────────────────────────│   (build inverted   │
                                                        │    index postings,  │
                                                        │    update PageRank) │
                                                        └─────────────────────┘
```

**Component Roles:**

| Component           | Role                                                                                    |
|---------------------|-----------------------------------------------------------------------------------------|
| Global Load Balancer| Routes queries to nearest data center; GeoDNS pins users to low-latency PoP.           |
| Frontend Servers    | Parse query string, apply spell correction, inject A/B experiment buckets.              |
| URL Frontier        | Maintains priority queue of URLs to crawl; enforces politeness (robots.txt, rate limits).|
| Crawlers (Googlebot)| Fetches raw HTML; follows links; respects crawl budget per domain.                      |
| Raw Page Store      | Immutable blob store (GCS equivalent); stores gzip-compressed HTML.                    |
| Content Processor   | Parses HTML, extracts text, detects language, deduplicates (SimHash), tokenises.        |
| Indexer/Writer      | Builds inverted index postings and periodically merges segment files.                   |
| Index Serving Tier  | Sharded in-memory inverted index; answers term-lookup scatter/gather queries.           |
| Metadata Store      | Per-URL metadata: title, fetch date, PageRank, language, canonical URL.                 |
| Query Processor     | Coordinates scatter to index shards, merges partial top-K lists.                       |
| Ranker              | Applies learned ranking model to re-rank top-1000 candidates to top-10.                |
| Snippet Generator   | Fetches stored text, locates best passage containing query terms, formats for display.  |

**Primary Use-Case Data Flow (user query "climate change effects"):**

1. User types query → Browser sends GET `/search?q=climate+change+effects`.
2. Load balancer routes to nearest PoP → Frontend server.
3. Frontend normalises query (lowercase, strip punctuation), checks spell correction model — no correction needed.
4. Query Processor tokenises: `["climate", "change", "effects"]`; expands synonyms: adds `"global_warming"`.
5. Scatter: fan-out to all ~500 index shards in parallel, each returns local top-50 docIDs + scores.
6. Merge: min-heap merge of 500×50 = 25,000 candidates → global top-1000.
7. Ranker loads feature vectors for top-1000 docs (PageRank, freshness, authority) → ML model outputs top-10.
8. Snippet Generator fetches text for 10 docs, computes best snippet window per doc.
9. JSON response assembled; Frontend adds SafeSearch filtering; response returned to browser in < 200 ms.

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────
-- URL / Document Registry
-- ─────────────────────────────────────────
CREATE TABLE documents (
    doc_id          BIGINT PRIMARY KEY,          -- internal stable ID, assigned at discovery
    url             VARCHAR(2048) NOT NULL,
    canonical_url   VARCHAR(2048),               -- after redirect resolution
    content_hash    CHAR(64),                    -- SHA-256 of raw content for change detection
    simhash         BIGINT,                      -- 64-bit SimHash for near-duplicate detection
    language        CHAR(5),                     -- BCP-47 e.g. 'en-US'
    page_rank       FLOAT,                       -- current PageRank score (updated weekly)
    fetch_timestamp TIMESTAMPTZ,                 -- last successful crawl time
    next_crawl_time TIMESTAMPTZ,                 -- scheduled recrawl (priority scheduling)
    http_status     SMALLINT,                    -- last HTTP status code
    crawl_depth     INT,                         -- hop distance from seed URLs
    is_canonical    BOOLEAN DEFAULT TRUE,
    INDEX idx_next_crawl (next_crawl_time),
    INDEX idx_simhash    (simhash)
);

-- ─────────────────────────────────────────
-- Inverted Index (conceptual schema; in practice stored as custom binary segments)
-- ─────────────────────────────────────────
-- Posting list entry (one row per (term, document) occurrence)
CREATE TABLE postings (
    term_id         BIGINT   NOT NULL,           -- FK to terms table
    doc_id          BIGINT   NOT NULL,           -- FK to documents
    term_frequency  INT      NOT NULL,           -- count of term in document
    field_mask      SMALLINT NOT NULL,           -- bitmask: title=1, body=2, anchor=4, URL=8
    position_list   BYTEA,                       -- variable-length encoded positions for phrase queries
    PRIMARY KEY (term_id, doc_id)
);

-- Term dictionary
CREATE TABLE terms (
    term_id         BIGINT PRIMARY KEY,
    term            VARCHAR(128) NOT NULL UNIQUE,
    doc_frequency   BIGINT,                      -- number of docs containing this term (for IDF)
    INDEX idx_term  (term)
);

-- ─────────────────────────────────────────
-- Link Graph (for PageRank)
-- ─────────────────────────────────────────
CREATE TABLE links (
    src_doc_id      BIGINT NOT NULL,
    dst_doc_id      BIGINT NOT NULL,
    anchor_text     VARCHAR(256),                -- anchor text of the link (used as ranking signal)
    PRIMARY KEY (src_doc_id, dst_doc_id),
    INDEX idx_dst   (dst_doc_id)                 -- for computing inbound links
);

-- ─────────────────────────────────────────
-- Document Metadata (served at result display time)
-- ─────────────────────────────────────────
CREATE TABLE doc_metadata (
    doc_id          BIGINT PRIMARY KEY,
    title           VARCHAR(512),
    meta_description VARCHAR(1024),
    og_image_url    VARCHAR(2048),
    publish_date    TIMESTAMPTZ,
    last_modified   TIMESTAMPTZ,
    word_count      INT,
    outbound_link_count INT,
    inbound_link_count  BIGINT,                  -- denormalised from links table
    content_category VARCHAR(64)                 -- e.g. 'news', 'ecommerce', 'blog'
);

-- ─────────────────────────────────────────
-- Spell Correction Model Data
-- ─────────────────────────────────────────
CREATE TABLE ngram_corrections (
    query_token     VARCHAR(128) PRIMARY KEY,
    correction      VARCHAR(128) NOT NULL,
    confidence      FLOAT,
    frequency       BIGINT                       -- how often this correction was accepted by users
);
```

**Inverted Index Segment Format (binary, not SQL):**

In production, the inverted index is stored as immutable segment files (inspired by Lucene):
```
Segment file layout:
  [Term Dictionary Block]  — sorted terms with offset pointers into posting lists
  [Posting List Block]     — delta-encoded doc IDs + term frequencies + position lists
  [Skip List Block]        — every 128th posting for O(log n) random access
  [Bloom Filter]           — probabilistic term existence check
```

### Database Choice

| Option                   | Pros                                                         | Cons                                                        |
|--------------------------|--------------------------------------------------------------|-------------------------------------------------------------|
| Custom binary segments   | Maximum read throughput; cache-aligned; no SQL overhead      | Complex to build and maintain; no ad-hoc queries            |
| Bigtable / HBase         | Scales to petabytes; good for sparse wide rows               | Higher latency than in-memory; eventual consistency         |
| PostgreSQL               | Mature, ACID, full-text search via GIN indexes               | Does not scale to 100B documents without massive sharding   |
| Elasticsearch            | Built-in inverted index, good for operational search         | Not designed for web-scale; GC pauses; JVM memory overhead  |
| Spanner                  | Global strong consistency, SQL interface                     | Overkill for read-heavy serving; higher write latency       |

**Selected approach:**
- **Inverted Index Serving**: Custom binary segment files (Lucene-inspired), loaded into RAM on serving nodes. Justification: at 100K QPS with < 200 ms P99, every microsecond matters. Custom binary format eliminates SQL parse overhead and enables SIMD-accelerated intersection of posting lists. Memory-mapped files allow the OS page cache to act as an L2 cache layer.
- **Metadata & Document Store**: Cloud Bigtable (or equivalent wide-column store). Justification: sparse schema (not all documents have all fields), excellent point-lookup performance at petabyte scale, linear horizontal scalability by row-key range sharding.
- **Link Graph**: Custom adjacency-list store on distributed file system (GFS/Colossus), read in bulk during periodic PageRank computation jobs (MapReduce/Dataflow). Does not need low-latency serving.
- **Crawl Frontier Queue**: Apache Kafka with partitioning by domain hash. Justification: durable, high-throughput, enables replay and politeness throttling per partition (domain).

---

## 5. API Design

All endpoints are internal (not public) or served via HTTPS at google.com/search. Rate limiting is enforced at the load balancer layer using token buckets: 100 requests/second per IP for anonymous users, 1,000/s for verified API partners.

### Search Query Endpoint

```
GET /search
Host: www.google.com
Authorization: Bearer <session_token>  (optional; absent for anonymous)

Query Parameters:
  q          string   REQUIRED  Raw query string, max 2048 chars
  num        int      OPTIONAL  Results per page, default 10, max 100
  start      int      OPTIONAL  Pagination offset (0-indexed), default 0, max 900
  hl         string   OPTIONAL  UI language (BCP-47), default 'en'
  gl         string   OPTIONAL  Country code for geolocation bias, e.g. 'US'
  safe       string   OPTIONAL  SafeSearch level: 'off' | 'medium' | 'strict'
  tbs        string   OPTIONAL  Time-based filter: 'qdr:h' (past hour), 'qdr:d', 'qdr:w'
  filter     int      OPTIONAL  Duplicate filtering: 1=on (default), 0=off
  tbm        string   OPTIONAL  Vertical: 'isch' (images), 'nws' (news), '' (web)

Response: 200 OK
Content-Type: application/json
X-Request-ID: <uuid>

{
  "query": {
    "original": "climate change efects",
    "corrected": "climate change effects",
    "tokens": ["climate", "change", "effects"]
  },
  "total_results_estimate": 4200000000,
  "search_time_ms": 47,
  "results": [
    {
      "rank": 1,
      "doc_id": "18472649283746",
      "url": "https://www.nasa.gov/climate",
      "title": "Climate Change: How Do We Know? – NASA",
      "snippet": "The effects of <em>climate change</em> include rising temperatures, more frequent extreme weather events...",
      "display_url": "nasa.gov › climate",
      "date": "2024-11-15",
      "score": 0.9823
    }
  ],
  "spell_suggestion": {
    "text": "Did you mean: climate change effects",
    "corrected_query": "climate change effects"
  },
  "pagination": {
    "current_page": 1,
    "next_start": 10,
    "has_more": true
  }
}

Errors:
  400 Bad Request    — q parameter missing or exceeds max length
  429 Too Many Requests — rate limit exceeded; Retry-After header included
  503 Service Unavailable — index serving tier degraded; fallback to cached results
```

### Internal Crawl Submission Endpoint (Webmaster / Search Console)

```
POST /webmasters/v1/sitemap
Authorization: Bearer <oauth2_token>  (requires verified site ownership)
Content-Type: application/json

Body:
{
  "site_url": "https://example.com/",
  "sitemap_url": "https://example.com/sitemap.xml"
}

Response: 200 OK
{
  "status": "submitted",
  "sitemap_url": "https://example.com/sitemap.xml",
  "urls_discovered": 1523,
  "estimated_crawl_start": "2024-11-16T08:00:00Z"
}
```

### Internal Index Health Endpoint (ops/monitoring)

```
GET /internal/index/health
Authorization: Bearer <internal_service_token>

Response:
{
  "shard_count": 512,
  "shards_healthy": 511,
  "shards_degraded": 1,
  "total_docs_indexed": 98432000000,
  "index_lag_seconds": 42,
  "last_full_crawl_completion": "2024-11-15T03:22:00Z"
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Inverted Index Construction & Serving

**Problem it solves:**
Given a query with N terms, the system must identify, out of 100 billion documents, which documents contain those terms and return them ranked by relevance in under 200 ms. A full linear scan of 100B documents is impossible; the inverted index pre-computes the mapping `term → sorted list of (doc_id, score)` so term lookup is O(1) and intersection is O(|posting_list|).

**Approaches Comparison:**

| Approach                        | Lookup Time     | Build Complexity | Memory          | Phrase Query | Notes                              |
|---------------------------------|-----------------|------------------|-----------------|--------------|------------------------------------|
| Linear scan (no index)          | O(N_docs)       | None             | None            | Yes          | Completely infeasible at scale     |
| Hash map term → doc list        | O(1) lookup     | Medium           | High            | No           | No ordering; can't rank cheaply    |
| Sorted inverted index (disk)    | O(log V + P)    | High             | Low (disk)      | Yes          | Standard approach; disk I/O slow   |
| Sorted inverted index (memory)  | O(log V)        | High             | Very High       | Yes          | Google's actual approach           |
| Compressed index (PForDelta)    | O(log V + P/k)  | Very High        | Medium (RAM)    | Yes          | Best balance; decode only needed   |
| Signature files / bit vectors   | O(N_docs/64)    | Low              | Medium          | No           | OK for small corpora, not at scale |

**Selected Approach: Compressed In-Memory Inverted Index with WAND (Weak AND) Top-K Retrieval**

Construction pipeline:
1. **Map phase**: Each crawled document is tokenised; a local per-document term frequency map is built.
2. **Reduce/Merge phase**: A distributed sort-merge (MapReduce) groups all (term, doc_id, tf) triples by term, producing posting lists sorted by doc_id.
3. **Compression**: Posting lists are delta-encoded (store gaps between consecutive doc_IDs, which are small) then compressed with PForDelta or variable-byte encoding. A 4-byte doc_id gap becomes ~1.5 bytes average.
4. **Segment files**: Immutable SST-style files written per shard. Merges happen offline; read path is always on complete segments.

Serving algorithm — **WAND (Weak AND)**:

```
// WAND: skip documents that cannot possibly beat the current k-th best score
// Input: query terms T, index I, k=1000
// Output: top-k (doc_id, score) pairs

function WAND_TopK(terms, k):
    // initialise one iterator per term, pointing to first posting
    iterators = [PostingIterator(I[t]) for t in terms]
    heap = MinHeap(capacity=k)           // min-heap keyed by score
    threshold = 0.0                      // score of k-th best candidate so far

    while not all_exhausted(iterators):
        // Sort iterators by their current doc_id (ascending)
        sort iterators by current_doc_id

        // Find pivot: first term whose upper-bound score sum exceeds threshold
        acc = 0
        pivot_idx = -1
        for i, it in enumerate(iterators):
            acc += upper_bound_score[it.term]    // precomputed max BM25 contrib
            if acc >= threshold:
                pivot_idx = i
                break

        if pivot_idx == -1:
            break  // no document can beat threshold; done

        pivot_doc = iterators[pivot_idx].current_doc_id

        // Check if all terms before pivot already point to pivot_doc
        if iterators[0].current_doc_id == pivot_doc:
            // Full evaluation: compute exact BM25 score for pivot_doc
            score = compute_BM25(pivot_doc, terms)
            if heap.size < k or score > heap.min():
                heap.push(pivot_doc, score)
                if heap.size == k:
                    threshold = heap.min()
            // Advance all iterators past pivot_doc
            for it in iterators:
                it.advance_to(pivot_doc + 1)
        else:
            // Advance first iterator to pivot_doc (skip low-scoring docs)
            iterators[0].advance_to(pivot_doc)

    return heap.to_sorted_list()
```

WAND skips ~99% of the posting list entries for competitive queries, achieving sub-millisecond lookup per shard.

**BM25 Scoring Formula:**

```
BM25(d, q) = Σ_{t ∈ q} IDF(t) × (tf(t,d) × (k1 + 1)) / (tf(t,d) + k1 × (1 - b + b × |d|/avgdl))

where:
  IDF(t)  = log((N - df(t) + 0.5) / (df(t) + 0.5) + 1)
  tf(t,d) = term frequency of t in document d
  |d|     = document length in tokens
  avgdl   = average document length across corpus
  k1      = 1.2 (term saturation parameter)
  b       = 0.75 (length normalisation parameter)
```

**Interviewer Q&A:**

1. **Q: Why store the index in RAM rather than on SSD?**
   A: At 100K QPS with a fan-out to 500 shards, each shard handles 200 QPS. A single query requires random-access reads into multiple posting lists. NVMe SSD random read latency is ~100 µs; DRAM is ~100 ns — 1,000x faster. With a 200 ms P99 budget and ~500 µs of other overhead per hop, we cannot afford SSD random I/O on the critical path. The hot tier (top 5B docs, ~60 PB compressed → ~6 PB after aggressive compression) is kept in RAM across a large cluster.

2. **Q: How do you handle phrase queries like "New York Times"?**
   A: Position lists stored in the posting entries. For a phrase query, after finding documents in the intersection of postings for "New", "York", "Times", we verify the positions are consecutive (pos_York = pos_New + 1, pos_Times = pos_New + 2). Position lists are compressed separately and only decoded for the final candidate set to avoid the decompression cost on all 100B documents.

3. **Q: How do you update the index when a page changes without rebuilding from scratch?**
   A: We use a two-tier index: a large immutable base index (rebuilt weekly via MapReduce) and a small real-time delta index (updated continuously via a streaming pipeline like Pub/Sub → streaming indexer). At query time, results from both are merged. The delta index is small enough to keep fully in memory and is rebuilt from scratch every few hours.

4. **Q: How does WAND know the upper-bound score for a term?**
   A: During index construction, for each (term, shard), we precompute `UB(term) = max BM25 contribution that term can make to any document's score`. This is the BM25 score of the document with the highest term frequency for that term and shortest document length. These upper-bound scores are stored in the term dictionary and loaded into memory with the iterator. They are pessimistic (upper bounds) to ensure correctness.

5. **Q: What happens to index quality during a crawler outage?**
   A: Index staleness increases but results remain available. The serving tier serves from the existing index. We track `last_crawl_timestamp` per document; the ranking model penalises documents not refreshed within expected recrawl windows. When the crawler recovers, priority queues are reweighted toward high-PageRank and time-sensitive domains to recover freshness fastest. Critical news/top-1000 domains have a dedicated high-frequency crawler fleet that is independent and more resilient.

---

### 6.2 PageRank Algorithm

**Problem it solves:**
Text-only relevance (BM25) is easily spammed — a page can stuff keywords but have no editorial value. PageRank uses the web's hyperlink graph as a proxy for authority: a page linked to by many authoritative pages is itself more authoritative.

**Approaches Comparison:**

| Approach                          | Quality    | Compute Cost   | Spam Resistance | Notes                              |
|-----------------------------------|------------|----------------|-----------------|-------------------------------------|
| Random baseline (no link signal)  | Low        | Zero           | None            | Easy to spam with keyword stuffing  |
| Inbound link count                | Medium     | Low            | Low             | All links weighted equally          |
| TrustRank                         | High       | Medium         | High            | Seeds from known-good sites         |
| Naive PageRank (full matrix)      | High       | O(N²)          | Medium          | Infeasible for 100B nodes           |
| Power Iteration PageRank          | High       | O(N + E) / iter| Medium          | Practical; 50-100 iterations        |
| Personalised PageRank             | Very High  | High (per user)| Medium          | Expensive; approximated             |
| HITS (Hubs & Authorities)         | High       | High           | Medium          | Query-time; too slow for web scale  |

**Selected Approach: Distributed Power Iteration PageRank on MapReduce/Dataflow**

The standard PageRank formulation:

```
PR(u) = (1 - d) + d × Σ_{v → u} PR(v) / OutDegree(v)

where:
  d = 0.85 (damping factor; probability of following a link vs random jump)
  v → u means page v has a link to page u
  OutDegree(v) = total number of outbound links from v
```

Distributed MapReduce pseudocode:

```
// Each iteration is one MapReduce job
// Input: (url, current_pr, [outbound_links])

MAP(url, pr, links):
    // Distribute this page's PageRank equally to all pages it links to
    for link in links:
        EMIT(link, pr / len(links))
    // Preserve graph structure for next iteration
    EMIT(url, DANGLING_SENTINEL if len(links)==0 else STRUCTURE_MARKER + links)

REDUCE(url, contributions):
    link_structure = extract and re-emit STRUCTURE_MARKER
    dangling_sum = sum of DANGLING_SENTINEL values (handled globally)
    new_pr = (1 - d) / N + d × (sum(contributions) + dangling_mass / N)
    EMIT(url, new_pr, link_structure)

// Run 50-100 iterations until max |PR_new - PR_old| < ε = 1e-6
```

**Practical considerations:**
- **Dangling nodes** (pages with no outbound links) "leak" PageRank; their mass is redistributed uniformly to all pages each iteration.
- **Spider traps** (cycles of pages that keep PageRank within a subgraph) are neutralised by the `(1-d)` term (15% chance of random jump to any page).
- **Link spam** is further handled by TrustRank: seed a separate propagation from manually verified high-trust domains (Wikipedia, gov sites) and discount links from low-trust neighborhoods.
- Convergence: with d=0.85 on a real web graph, PageRank typically converges in 50-100 iterations. At 100B nodes and ~1 trillion edges, each iteration is a multi-hour job; runs weekly on dedicated compute.

**Interviewer Q&A:**

1. **Q: Why is the damping factor set to 0.85?**
   A: Brin and Page's original paper chose 0.85 empirically. It represents the probability that a random web surfer follows a link on the current page (rather than jumping to a random page). Values closer to 1 make PageRank more sensitive to link structure (better quality signal) but slower to converge and more vulnerable to spider traps. Values much lower than 0.85 dilute the link signal. 0.85 has been validated as a practical balance over decades.

2. **Q: How do you handle the web's scale — 100B nodes and 1T edges don't fit in a single machine's memory?**
   A: The MapReduce approach naturally partitions the graph: each mapper handles one page and emits contributions to its link targets. The shuffle phase groups contributions by destination URL. No single machine ever needs the full graph. With 10,000 workers each processing 10M nodes/edges, each iteration completes in hours. Google's Pregel system further optimised this with a BSP (Bulk Synchronous Parallel) model that avoids full shuffle overhead.

3. **Q: PageRank is computed on the full graph weekly. How do you handle new high-quality sites that publish today?**
   A: Two mechanisms: (1) Freshness boost — a separate real-time signal boosts newly crawled, high-traffic pages regardless of PageRank. (2) Incremental PageRank — for newly discovered high-indegree pages (e.g., a viral news article linked from many existing high-PR pages), we can run localised PageRank on the subgraph of pages within 2 hops, which is computationally feasible within minutes.

4. **Q: How does PageRank interact with the ranking model? Is it the primary signal?**
   A: No. PageRank is one of 200+ ranking signals fed into a learned ranking model (LambdaMART or a neural ranker). BM25 relevance to the query typically dominates for information retrieval quality; PageRank acts as a tiebreaker for equally relevant pages and as a spam deterrent. For navigational queries ("Facebook login"), domain-level PageRank is very influential. For long-tail informational queries, content quality signals dominate.

5. **Q: How do you prevent a cabal of sites from artificially inflating each other's PageRank through reciprocal links?**
   A: Several defences: (1) Link quality weighting — anchor text diversity and IP diversity of linkers are checked; reciprocal links from the same IP block are discounted. (2) TrustRank — propagation from manually verified seeds; sites far from trusted seeds get their link equity discounted. (3) Penguin algorithm — detects unnatural link velocity (sudden link spike) and applies penalties. (4) Nofollow attribute — `rel="nofollow"` links pass no PageRank.

---

### 6.3 Spell Correction

**Problem it solves:**
A significant fraction of queries (historically ~10-15%) contain misspellings. Without correction, queries like "clmate chaange efects" would match very few or no documents, giving a poor user experience.

**Approaches Comparison:**

| Approach                          | Accuracy  | Latency   | Coverage        | Notes                                     |
|-----------------------------------|-----------|-----------|-----------------|-------------------------------------------|
| Edit distance (Levenshtein) only  | Medium    | Medium    | Limited vocab   | No context; "teh" → "the" or "ten"?       |
| N-gram language model             | High      | Low       | Broad           | Needs large corpus; good contextual rank  |
| Noisy channel model               | High      | Medium    | Broad           | P(observed|intended) × P(intended)        |
| Query log mining                  | Very High | Very Low  | High-freq only  | "clmate" → "climate" seen millions of times|
| Neural sequence-to-sequence       | Highest   | High      | Broadest        | Requires GPU; latency may exceed budget   |
| Hybrid (log mining + noisy channel| Very High | Low       | Broad           | Best practical balance                    |

**Selected Approach: Hybrid Noisy Channel Model + Query Log Mining**

```
// Noisy channel: P(correction | misspelling) ∝ P(misspelling | correction) × P(correction)

function spell_correct(query_token):
    // Step 1: check if token exists in vocabulary (fast hash lookup)
    if token in vocabulary:
        return token  // correctly spelled, no correction needed

    // Step 2: query log mining — check if this exact string has a known correction
    if token in correction_cache:
        return correction_cache[token]  // O(1) lookup

    // Step 3: generate candidates within edit distance 1 and 2
    candidates = generate_edits(token, max_distance=2)
    candidates = [c for c in candidates if c in vocabulary]

    // Step 4: score each candidate with noisy channel model
    scored = []
    for c in candidates:
        // P(misspelling | correction): keyboard adjacency model
        // (probability of making these specific keystrokes given intended word)
        error_prob = keyboard_error_model(token, c)

        // P(correction): unigram language model from web corpus
        language_prob = unigram_lm[c]

        // Contextual boost: if other query tokens form coherent phrase with c
        context_score = bigram_context_score(c, adjacent_tokens)

        scored.append((c, error_prob × language_prob × context_score))

    best = argmax(scored)
    if scored[best].score > CONFIDENCE_THRESHOLD:
        return best
    return token  // no confident correction; return original

// Keyboard error model: captures that 'e' adjacent to 'r' means P("teh"→"the") > P("teh"→"ten")
// Uses confusion matrix built from query logs: when user accepts a suggestion, that's a training signal
```

**Interviewer Q&A:**

1. **Q: How do you generate edit-distance-1 candidates efficiently without checking all dictionary words?**
   A: Deletion, insertion, substitution, and transposition operations on the query token generate a finite set of strings (26 × len + len × 26 + len × 26 + len candidates). For a 6-character word, this is roughly 200 candidates. We check these against a hash set of the vocabulary (100M words), which takes microseconds. For edit-distance-2, we apply edit-1 generation twice, which is feasible for short tokens.

2. **Q: What about real-word errors — "their" vs "there"?**
   A: These require a language model. A bigram/trigram language model trained on query logs detects that "I went their" is less probable than "I went there". This is the contextual score component in the algorithm above. For search queries specifically, we also check whether the alternative form retrieves substantially more or higher-quality results — if so, we suggest it.

3. **Q: How do you avoid over-correcting brand names, technical terms, or new slang?**
   A: The vocabulary is built from the crawled web corpus, so legitimate rare words that appear frequently enough are in it. Additionally, high query frequency suppresses correction — if "iphone" appears 1 billion times in the query log uncorrected, it is added to the no-correction list even if it wasn't initially in the dictionary. We maintain a "do not correct" blocklist updated from query logs.

---

## 7. Scaling

### Horizontal Scaling

**Index Serving Tier:**
- Sharding strategy: consistent hash of URL's domain + path hash → shard ID. All posting list entries for documents on a given shard reside on the same physical machines.
- Each shard is replicated 3x across different racks (rack-aware placement) for fault tolerance.
- Shard count: 500–1,000 shards. With 100B documents, each shard holds 100-200M documents. At ~300 bytes average per document in the inverted index, each shard requires ~30–60 GB of RAM — manageable on modern 256 GB RAM machines.
- Query fan-out: every query goes to ALL shards (scatter/gather). The query processor aggregates partial results using a min-heap merge.

**Crawler Fleet:**
- Stateless crawler processes: any crawler can process any URL.
- URL Frontier partitioned by domain hash across workers to enforce per-domain politeness (max 1 request/second per domain).
- Auto-scaling: crawler fleet scales horizontally; each worker is a simple fetch + parse process.

**Query Processor:**
- Stateless; scale horizontally behind load balancer.
- Each query processor instance fans out to all index shards; receives and merges partial results.
- Caching: popular queries cached in Redis cluster (LRU, TTL = 1 hour). Cache hit rate on a Zipfian query distribution is ~40-60%.

### DB Sharding

| Store             | Sharding Key          | Strategy                                                          |
|-------------------|-----------------------|-------------------------------------------------------------------|
| Inverted index    | hash(domain + path)   | Consistent hashing; 500-1000 shards; 3 replicas each             |
| Document metadata | doc_id range          | Range partitioning; lexicographic by doc_id for sequential scans |
| Link graph        | src_doc_id hash       | Hash sharding; bulk-read during PageRank computation             |
| Query cache       | hash(normalised_query)| Redis cluster; 16,384 hash slots                                 |

### Replication

- **Index shards**: 3 replicas per shard (leader + 2 followers). Reads distributed across replicas (eventual consistency acceptable). Writes (index updates) go through leader; replicated asynchronously to followers.
- **Metadata store (Bigtable)**: 3-way replication within a region; cross-region replication for DR.
- **Crawl data (raw pages)**: 3x geo-redundant replication on GCS; standard durability policy (11 nines).

### Caching

| Cache Layer       | Technology  | What's Cached                          | TTL     | Eviction  |
|-------------------|-------------|----------------------------------------|---------|-----------|
| Result cache      | Redis        | Full SERP JSON for top-1M queries      | 1 hour  | LRU       |
| Snippet cache     | In-process   | Computed snippets for top-10 results   | 30 min  | LRU       |
| PageRank scores   | In-memory    | All 100B PageRank scores (800 GB total)| Weekly  | Full flush |
| Spell correction  | In-process   | 10M query→correction mappings          | Daily   | Full flush |
| Taxonomy/verticals| Memcached    | Query classification results           | 10 min  | LRU       |

### CDN

- Static assets (JS, CSS, images on SERP) served from edge CDN nodes globally.
- SERP HTML itself is NOT cached at CDN (personalised and time-sensitive); only static resources are.
- Google's own global backbone (B4 network) routes query traffic between PoPs, avoiding public internet latency.

**Interviewer Q&A:**

1. **Q: With 500 shards, every query causes 500 network round trips. How do you keep this under 50 ms?**
   A: Three techniques: (1) All 500 RPCs are issued in parallel (async scatter), so latency is determined by the SLOWEST shard, not the sum. (2) We use hedged requests — after 10 ms, if a shard hasn't responded, we re-issue the request to a second replica. The first to respond wins; the other is cancelled. (3) Shards are co-located in the same datacenter on a 10 Gbps internal network; RPC latency is ~1-2 ms. Total scatter/gather is typically 5-15 ms.

2. **Q: How do you prevent the "thundering herd" problem when the result cache expires for a popular query?**
   A: Cache stampede prevention: we use probabilistic early expiration (PER) — when a cached item has < 10% TTL remaining, a small random fraction of requests (1%) bypass the cache and refresh it proactively. Alternatively, the first request that misses the cache gets a lock (using SET NX in Redis); all other concurrent requests wait for the lock holder to repopulate the cache.

3. **Q: How does sharding by domain + path hash affect hotspot risk? (e.g., all Wikipedia pages on one shard)**
   A: The hash is computed on the full URL (domain + path), so Wikipedia pages spread across all shards. The domain is included only to co-locate all pages of a site for potential crawl-side optimizations, but the path component provides sufficient entropy to distribute load. We monitor per-shard QPS and flag imbalances; consistent hashing with virtual nodes allows re-balancing by splitting hot shards.

4. **Q: How do you scale the indexing write path as crawl throughput grows?**
   A: The write path is fully decoupled from the read path. Indexers write new segment files to a staging area; the serving tier atomically swaps in new segments during a merge. This "segment rotation" happens without downtime. Multiple indexer workers operate in parallel, each processing a subset of the crawl queue. Adding more indexer workers scales write throughput linearly.

5. **Q: Why use GeoDNS to route users to nearest PoP rather than a global anycast load balancer?**
   A: Google actually uses both. Anycast routes the TCP connection to the nearest PoP. GeoDNS provides application-level control: it can redirect users away from overloaded regions or to a region with a more up-to-date index. Anycast alone can't differentiate between a healthy and a degraded PoP — DNS-based routing allows health-check-driven traffic steering.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario                    | Impact                                 | Mitigation                                                                    | Recovery Time |
|-------------------------------------|----------------------------------------|-------------------------------------------------------------------------------|---------------|
| Single index shard failure          | 0.2% of queries return incomplete results | Immediate failover to replica; hedged requests catch latency spike          | < 30 seconds  |
| Query processor cluster failure     | Full query outage in a region          | DNS failover to another region; stateless, no local state to recover          | < 2 minutes   |
| Crawler fleet outage                | Index staleness increases              | Serving continues normally; staleness metric triggers SRE alert               | Hours (acceptable) |
| Metadata store unavailable          | No snippets; degraded SERP             | Cache layer serves stale snippets; graceful degradation (show URL only)       | Minutes (cache TTL) |
| PageRank computation job failure    | Stale ranking scores                   | Previous week's scores remain; job is idempotent, restart from checkpoint     | Hours (acceptable) |
| Network partition between PoPs      | Elevated latency, possible split-brain | Each PoP operates independently; DNS weighted routing shifts traffic          | Immediate     |
| Cache cluster failure               | All queries hit backend; load spike    | Query processor rate-limits itself; load shedding for low-priority queries    | Minutes       |
| Bad index update (corrupted segment)| Degraded result quality for affected shards | Segment-level checksums; automatic rollback to previous segment on CRC fail | < 1 minute    |

### Failover Strategy

- **Active-Active across 3+ regions**: All regions serve live traffic normally. DNS weights distribute load (e.g., US-East 40%, US-West 30%, EU 30%).
- **Index shard failover**: Each shard has a primary and 2 standby replicas. If primary fails, election promotes a standby (Raft-based leader election) in < 10 seconds.
- **Hedged requests**: For tail latency, queries to slow shards are re-issued to another replica after a 10 ms deadline.

### Retries & Idempotency

- All query RPCs are idempotent (read-only); retries are safe.
- Crawler fetches use exponential backoff with jitter (1s, 2s, 4s, 8s, max 5 retries) before marking a URL as temporarily unavailable.
- Index writes are append-only (new segment files); no in-place updates, so partial failures leave the previous segment intact.

### Circuit Breaker

- Each query processor maintains per-shard circuit breakers. If a shard returns errors > 50% of requests in a 10-second window, the circuit opens and that shard is excluded from the scatter fan-out. Results are returned from remaining shards (graceful degradation: fewer results, lower recall).
- Circuit reopens (half-open probe) after 30 seconds; if the probe succeeds, shard is restored to rotation.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric                       | Type      | Alert Threshold          | Dashboard           |
|------------------------------|-----------|--------------------------|---------------------|
| Query latency P50/P99        | Histogram | P99 > 500 ms             | SERP Perf           |
| QPS (queries per second)     | Counter   | < 50% of baseline        | Traffic             |
| Index staleness (avg age)    | Gauge     | > 24 hours for news docs | Freshness           |
| Shard availability %         | Gauge     | < 99%                    | Index Health        |
| Cache hit rate               | Gauge     | < 30%                    | Cache               |
| Crawler fetch success rate   | Gauge     | < 90%                    | Crawler             |
| Crawl queue depth            | Gauge     | > 10x normal             | Crawler             |
| Spell correction acceptance rate| Gauge | < 60%                    | Query Quality       |
| Error rate (5xx)             | Counter   | > 0.1% of requests       | SRE                 |
| PageRank computation lag     | Gauge     | > 10 days since last run | Ranking             |

### Distributed Tracing

- Every query is assigned a `trace_id` (UUID) at the frontend. Propagated via gRPC metadata to all downstream services (index shards, ranker, snippet generator).
- Span hierarchy: `frontend` → `query_processor` → `index_shard[0..N]` + `metadata_store` → `ranker` → `snippet_generator`.
- Traces sampled at 0.1% for all traffic; 100% sampling for P99 > 500 ms (tail sampling).
- Tooling: Dapper (Google's internal tracing, inspiration for OpenTelemetry/Jaeger).

### Logging

- **Structured JSON logs** at each service: timestamp, trace_id, service, method, latency_ms, status_code, doc_ids_returned.
- **Query logs** (anonymised, sampled): used for spell correction training, trending query detection, and A/B experiment analysis.
- **Crawl logs**: per-URL fetch results (status, size, redirect chain) → streamed to BigQuery for analysis.
- Log retention: 30 days hot (BigQuery), 1 year cold (GCS).

---

## 10. Trade-offs & Design Decisions Summary

| Decision                          | Option Chosen                          | Alternative Considered          | Why Chosen                                                                            |
|-----------------------------------|----------------------------------------|---------------------------------|---------------------------------------------------------------------------------------|
| Index storage                     | Custom in-memory binary segments       | Elasticsearch / Lucene          | 1000x lower latency; full control over SIMD optimisations; no JVM GC pauses          |
| Freshness approach                | Two-tier index (base + delta)          | Rebuild full index frequently   | Rebuild is days; delta updates within minutes for time-sensitive content              |
| Ranking algorithm                 | Learned model (LambdaMART) + BM25 + PR| Pure BM25                       | Machine-learned ranking captures hundreds of signals; pure BM25 is gameable           |
| Shard strategy                    | Hash by URL                            | Range by URL alphabetically     | Hash distribution avoids hotspots; alphabetical would put all ".com" on same shards  |
| Spell correction                  | Hybrid noisy channel + query logs      | Pure neural seq2seq             | Neural is more accurate but too slow (10+ ms vs 1 ms); hybrid achieves 95% quality  |
| PageRank update frequency         | Weekly batch                           | Real-time incremental           | Full graph is too large for real-time; weekly batch is sufficient for ranking quality |
| Cross-region failover             | Active-Active DNS                      | Active-Passive warm standby     | Active-Active uses all capacity; DNS routing allows gradual traffic shifting          |
| Cache granularity                 | Full SERP JSON                         | Per-document score cache        | Full SERP cache has highest hit rate for top queries; simpler invalidation            |

---

## 11. Follow-up Interview Questions

1. **Q: How would you design a system to index and search for images rather than text?**
   A: Image search requires a different indexing strategy. For text-based image search, we index the alt text, surrounding text, caption, and page context. For visual similarity search, we extract CNN feature embeddings (e.g., from a ResNet50 or CLIP model) and build an Approximate Nearest Neighbor index (HNSW or FAISS) over the embedding space. The query can then be an image (reverse image search) or text (CLIP enables cross-modal search). Scale challenge: 100B images × 2,048-dim float32 = ~800 PB of embeddings; requires aggressive dimensionality reduction (PCA to 256 dims) and product quantisation for compression.

2. **Q: How does Google handle search for low-resource languages with limited crawl data?**
   A: Cross-lingual transfer learning: a multilingual BERT-style model trained on 100 languages can transfer ranking knowledge from high-resource languages (English) to low-resource languages. Additionally, translation-based augmentation: pages in low-resource languages are machine-translated and indexed under the translated terms as well, improving recall. Language detection is handled by a CLD3 (Compact Language Detector) model.

3. **Q: How would you implement SafeSearch filtering at 100K QPS?**
   A: Two approaches in combination: (1) Blocklist filtering — known adult domains are flagged in the URL metadata store; matching results are removed pre-display at O(1) per result. (2) ML classifier — a content classifier (image + text) labels documents at crawl time with an adult content score stored in the document metadata. SafeSearch applies a threshold filter on this score at display time. The classifier runs offline during indexing, so it adds zero query-time latency. The overall SafeSearch check is O(k) where k=10 is the number of results to display.

4. **Q: How do you ensure the crawler doesn't overload small websites?**
   A: Politeness mechanisms: (1) robots.txt is fetched for every domain and honoured (Crawl-delay directive). (2) The URL frontier enforces a minimum inter-request interval per domain (default 1 second). (3) Crawl budgets are assigned per domain based on domain size and PageRank; small sites get proportionally smaller budgets. (4) The crawler detects 429 (Too Many Requests) and backs off exponentially. (5) Adaptive rate reduction: if a site's response latency increases by 2x, the crawl rate is halved.

5. **Q: How do you detect and handle web spam (keyword stuffing, link farms)?**
   A: Multi-layer defence: (1) Content quality signals: ratio of visible text to total HTML, keyword density anomaly detection, text readability scores. (2) Link graph analysis: Penguin algorithm detects unnatural inbound link patterns (sudden spikes, links from link farms). (3) TrustRank: propagates trust from seed sites; pages in low-trust neighbourhoods are downranked. (4) Manual penalties: the web spam team manually reviews and penalises egregious violators; penalties stored as a sitewide score multiplier. (5) Machine learning: SpamBrain classifier trained on human-labelled spam data.

6. **Q: How would you design the system to support search result personalisation based on user history?**
   A: User history (click signals, dwell time) is stored in a user activity log and aggregated into a per-user interest profile (topic vector). At query time, the re-ranker fetches the user's interest vector and adjusts document scores using dot-product similarity between the document's topic vector and the user's interest vector. Key challenges: (1) Privacy — profiles are aggregated and stored in anonymised form; not used without consent. (2) Latency — profile lookup must complete within the overall 200 ms budget; stored in a low-latency key-value store (Bigtable) keyed by user ID. (3) Filter bubble — personalisation is weighted conservatively (e.g., 10% of total score) to avoid extreme personalisation.

7. **Q: How does the query processing pipeline handle multi-word queries differently from single-word queries?**
   A: For single-word queries, we simply look up the posting list for that term. For multi-word queries, we must decide between: (1) AND semantics (return only documents containing all terms — higher precision, lower recall), (2) OR semantics (return documents with any term — higher recall, harder to rank), (3) Phrase queries (terms must appear adjacent). In practice, Google uses relaxed AND: documents with all terms are ranked first, then documents with most terms, using WAND-style threshold scoring. The scoring model naturally penalises missing terms through IDF-weighted BM25 contributions.

8. **Q: How do you measure and improve search quality?**
   A: Several feedback loops: (1) Human evaluation: search quality raters follow the Search Quality Evaluator Guidelines (SQEG) to rate SERP pages on Needs Met and Page Quality scales. (2) A/B testing: ranking changes are tested on a random 1% of traffic; primary metric is clicks but corrected for position bias. (3) Click-through rate (CTR) by position and query: lower than expected CTR at position 1 indicates relevance problems. (4) Long clicks / dwell time: a click followed by a long session on the destination page is a positive signal; a quick return ("pogo-sticking") is negative. (5) Zero-result rate: no results for a query is a critical failure; monitored continuously.

9. **Q: How would you change the design if you needed to support real-time indexing with < 1 minute freshness?**
   A: Expand the delta index tier: instead of a delta updated every few hours, run a true streaming indexer (Kafka consumers → in-memory posting list updates). Trade-off: the delta index must be queried on every request, increasing memory and CPU cost. To bound the delta index size, we set a max age (e.g., documents > 1 hour old graduate to the base index on next merge). The streaming path requires careful handling of document updates (invalidate old doc_id, insert new posting) and ordering guarantees.

10. **Q: How does the system handle highly ambiguous queries like "apple" (fruit vs. company)?**
    A: Query understanding classifies intent: (1) Entity recognition detects "Apple" as a named entity (company, probability 0.7) or common noun (fruit, probability 0.3). (2) Context signals: the user's location, device, previous queries in the session influence disambiguation. (3) The SERP may serve a mixed result set covering multiple interpretations, with a Knowledge Panel (Apple Inc.) prominently shown if entity confidence is high. (4) The query may trigger a disambiguation prompt: "Showing results for Apple (company). Search instead for: apple (fruit)".

11. **Q: Explain how index compression works in practice.**
    A: Posting lists store delta-encoded doc IDs (differences between consecutive doc IDs rather than absolute values). For a typical posting list sorted by doc ID, gaps are small (mean < 1,000 for common terms). These small integers compress well with variable-byte encoding (1 byte for values < 128) or PForDelta (packs 128 values together, stores most as small exceptions). Benchmark: a raw 4-byte doc ID becomes ~1.5 bytes compressed — a 2.5x compression ratio. For the full index, this matters enormously: 1,200 PB compressed to ~480 PB.

12. **Q: How would you handle a "Google bomb" — coordinated anchor text manipulation to rank a page for an unrelated query?**
    A: Historical Google bombs exploited the anchor text signal (links with text "miserable failure" pointing at a page → that page ranks for "miserable failure"). Defences: (1) Anchor text diversity — require that anchor text manipulation comes from diverse, high-trust domains to have significant effect. (2) Topical coherence — penalise pages where the anchor text topics are inconsistent with the page's own content topic. (3) Manual intervention — egregious cases can have the anchor text signal manually zeroed out. (4) Algorithm updates — reduce the weight of exact-match anchor text in ranking, increase weight of context and co-citation signals.

13. **Q: How does the system scale snippet generation? It requires fetching document text for top-10 results.**
    A: Document text is stored in the metadata/document store (Bigtable) and cached aggressively. Key optimisation: we store a pre-extracted "snippet pool" for each document at index time — the 10 most important sentences (by TF-IDF of sentences). At query time, snippet generation only needs to select the best sentence from the pool matching query terms, which is O(k × sentences_per_pool) where k=10 and pool size=10. This avoids fetching full document text on the critical path. Full-text fetch is a fallback for cache misses.

14. **Q: How would you design the system to support federated search (combining web, images, news, maps results)?**
    A: Each vertical (web, images, news, maps) is an independent search system. A federated query processor fans out the query to all relevant verticals in parallel, collects results within the overall latency budget, and the SERP blending algorithm (trained ML model) determines how many results from each vertical to show and in which positions. The blending model is trained on human preferences: e.g., news results should appear above fold for timely queries, image results for visual queries. Each vertical respects the same SLA (200 ms total) so inter-vertical communication must be tight (< 100 ms for vertical results).

15. **Q: What changes would you make if this system needed to run on-premises for an enterprise rather than as a web-scale public search engine?**
    A: Enterprise search is several orders of magnitude smaller (millions vs. billions of documents) but adds requirements: access control (documents visible only to authorised users), connector-based indexing (SharePoint, Confluence, Slack), and different relevance signals (recency within the org, author trust). Design changes: (1) Replace custom binary index with Elasticsearch (justified at this scale; easier to operate). (2) Add per-document ACL filtering at query time — after retrieval, filter out documents the querying user lacks permission for. (3) Real-time indexing via webhooks from source systems (no need for web crawlers). (4) Replace PageRank with collaboration graph signals (who works with whom, document engagement metrics).

---

## 12. References & Further Reading

1. **Brin, S. & Page, L. (1998)** — "The Anatomy of a Large-Scale Hypertextual Web Search Engine." *Proceedings of WWW7*. (Original Google paper; describes crawling, indexing, and PageRank.) https://dl.acm.org/doi/10.1145/3442381.3449891

2. **Dean, J. & Ghemawat, S. (2004)** — "MapReduce: Simplified Data Processing on Large Clusters." *OSDI 2004*. (Foundation for distributed PageRank and batch indexing.) https://dl.acm.org/doi/10.1145/1327452.1327492

3. **Macdonald, C. et al. (2012)** — "On the Efficiency of Selective Search." *CIKM 2012*. (Selective/tiered index strategies for efficiency at scale.)

4. **Broder, A. et al. (2003)** — "Efficient Query Evaluation using a Two-Level Retrieval Process." *CIKM 2003*. (WAND algorithm for top-K retrieval.)

5. **Robertson, S. & Zaragoza, H. (2009)** — "The Probabilistic Relevance Framework: BM25 and Beyond." *Foundations and Trends in Information Retrieval, 3(4).* (Definitive BM25 reference.) https://dl.acm.org/doi/10.1561/1500000019

6. **Burges, C. et al. (2005)** — "Learning to Rank using Gradient Descent (RankNet)." *ICML 2005*. (Foundation of learned ranking models.)

7. **Kirchhoff, K. & Yang, M. (2005)** — "Improved Language Modeling for Statistical Machine Translation." Spell correction noisy channel model principles.

8. **Google Search Central Documentation** — https://developers.google.com/search/docs (Crawling, indexing, and ranking documentation.)

9. **Zhao, Y. et al.** — "Orion: Google's Software-Defined Networking." (Google's B4 WAN; underpins low-latency inter-datacenter query routing.)

10. **Malin, B. & Sweeney, L.** — "k-Anonymization" principles applied to query log anonymisation for spell correction training.

11. **Manning, C., Raghavan, P., Schütze, H. (2008)** — *Introduction to Information Retrieval.* Cambridge University Press. (Comprehensive IR textbook; covers inverted indexes, BM25, PageRank.) https://nlp.stanford.edu/IR-book/

12. **Baeza-Yates, R. & Ribeiro-Neto, B. (2011)** — *Modern Information Retrieval* (2nd ed.). Addison-Wesley. (Advanced IR algorithms and systems.)
