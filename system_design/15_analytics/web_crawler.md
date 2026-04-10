# System Design: Web Crawler

---

## 1. Requirement Clarifications

### Functional Requirements

1. **URL Discovery**: Given a set of seed URLs, discover and crawl all reachable pages on the web, extracting hyperlinks and recursively scheduling them for crawling.
2. **Politeness Compliance**: Respect `robots.txt` directives (Disallow, Crawl-delay, User-agent rules) and per-domain crawl rate limits to avoid overloading origin servers.
3. **Content Fetching**: Download HTML (and optionally CSS, JS, images, PDFs, sitemaps) via HTTP/1.1 and HTTP/2.
4. **Content Deduplication**: Detect and skip near-duplicate pages (same content under different URLs) using fingerprinting to avoid redundant storage and processing.
5. **Link Extraction**: Parse fetched HTML and extract all canonical outbound links, normalizing them (absolute URL, lowercase scheme/host, remove tracking params).
6. **Recrawl Scheduling**: Re-fetch previously crawled URLs at appropriate intervals based on content change frequency (high-change pages like news: hours; static pages: weeks).
7. **Crawl Prioritization**: Assign priority to URLs based on signals (PageRank estimate, domain authority, freshness requirement, URL depth) to maximize information value per crawl slot.
8. **Storage**: Persist crawled content (raw HTML), extracted links, crawl metadata (HTTP status, timestamp, content hash), and page-level metadata for downstream consumers (search indexers, ML pipelines).

### Non-Functional Requirements

1. **Scale**: Crawl 1 billion pages per day across the open web (~100M domains).
2. **Throughput**: Sustain 10,000–15,000 HTTP requests per second at peak.
3. **Politeness**: Never exceed 1 request per second per domain by default; honor explicit `Crawl-delay` directives; honor `robots.txt` within 24 hours of changes.
4. **Freshness**: High-priority pages (news, product pages) re-crawled within 24 hours of change; low-priority pages within 30 days.
5. **Storage Efficiency**: Store raw crawled content with deduplication (identical pages stored once); target < 500 TB for 1 month of crawl data.
6. **Availability**: Crawler infrastructure available > 99.9% (8.7 hours downtime/year). Crawl gaps up to 1 hour are acceptable.
7. **Correctness**: URL frontier must not crawl the same URL more than once per crawl interval. No duplicate scheduling.
8. **Extensibility**: Easy to add new content parsers (PDF, XML sitemaps, JSON-LD) and new downstream consumers without modifying core crawler.

### Out of Scope

- Search index building (ranking, inverted index construction).
- JavaScript rendering / headless browser crawling (Googlebot's second-wave rendering).
- Deep web / dark web crawling (requires authentication or Tor).
- Real-time feed crawling (RSS/Atom — different scheduling semantics).
- Crawling behind login walls.
- Legal/compliance framework for `noindex`/`noarchive` directives (downstream indexer's concern).

---

## 2. Users & Scale

### User Types

| Actor | Description |
|---|---|
| Crawler Workers | Distributed fetch workers pulling URLs from the frontier |
| Seed Injector | Operator tool to bootstrap with seed URLs or upload sitemap files |
| URL Frontier | Internal service managing the crawl queue and politeness |
| Content Processor | Downstream consumer parsing fetched pages (link extraction, text extraction) |
| Dedup Service | Component detecting near-duplicate content before storage |
| Recrawl Scheduler | Component analyzing crawl history to schedule re-fetches |
| Search Indexer | Downstream consumer reading crawled content for indexing |
| Ops Team | Monitoring crawl coverage, freshness, error rates |

### Traffic Estimates

**Assumptions**:
- Target: 1 billion pages crawled per day.
- Average HTML page size: 50 KB (uncompressed). With gzip: ~15 KB on the wire.
- 70% of URLs return HTML; 15% redirect; 10% return 4xx/5xx; 5% other (PDF, image, etc.).
- Average links per page: 40 (raw) → 15 after normalization and dedup.
- DNS TTL cache hit rate: 90% (many URLs share domains).
- Average HTTP response time: 300 ms (includes DNS + TCP + TLS + TTFB + transfer).

| Metric | Calculation | Result |
|---|---|---|
| Target crawl rate | 1B pages / 86,400 s | **11,574 pages/s** |
| Design target (1.5× headroom) | 11,574 × 1.5 | **~17,000 req/s** |
| Bandwidth: wire download | 17,000 × 15 KB | **255 MB/s = 2 Gbps** |
| Bandwidth: decompressed storage | 17,000 × 50 KB × 70% HTML | **595 MB/s raw** |
| New links discovered/day | 1B × 15 new links | 15B links/day |
| New unique links (80% already seen) | 15B × 20% | 3B new URLs/day |
| URL frontier size (30-day rolling) | 3B × 30 / 10 (crawled ratio) | ~9B URLs in frontier |
| DNS lookups/s (uncached) | 17,000 × 10% (cache miss) | 1,700 DNS lookups/s |
| Robots.txt fetches/day | 100M unique domains × 1/7 (weekly refresh) | ~14M robots.txt/day = 163/s |

### Latency Requirements

| Operation | Target |
|---|---|
| URL dispatch from frontier to worker | < 10 ms |
| DNS resolution (cached) | < 1 ms |
| DNS resolution (uncached) | < 50 ms |
| robots.txt check before fetch | < 2 ms (from cache) |
| HTTP fetch completion (p50) | < 300 ms |
| HTTP fetch timeout | 10 s (hard) |
| Content dedup check | < 5 ms |
| Link extraction per page | < 20 ms |
| URL normalization + dedup + frontier enqueue | < 10 ms |
| Recrawl scheduling decision | < 5 min after crawl completion |

### Storage Estimates

| Data | Size/Record | Volume/Day | Retention | Total |
|---|---|---|---|---|
| Raw HTML (compressed, Snappy) | ~10 KB/page | 1B × 10 KB = 10 TB/day | 30 days | 300 TB |
| Crawl metadata (URL, status, timestamps, hash) | ~200 B | 1B × 200 B = 200 GB/day | 90 days | 18 TB |
| URL frontier (in-queue URLs) | ~150 B | 9B URLs × 150 B | Rolling | ~1.35 TB |
| DNS cache | ~100 B | 100M domains × 100 B | 4-hour TTL | 10 GB |
| robots.txt cache | ~5 KB | 100M domains × 5 KB | 24-hour TTL | 500 GB |
| Content fingerprint store | 8 B (SimHash) | 30B pages × 8 B | Permanent | 240 GB |
| Link graph (edges) | ~20 B | 15B edges/day × 20 B = 300 GB/day | 90 days | 27 TB |

### Bandwidth Estimates

| Flow | Rate |
|---|---|
| HTTP download (raw) | 2 Gbps aggregate |
| Content write to object store (compressed) | 1 Gbps |
| Link extraction to frontier (Kafka) | ~50 MB/s |
| Frontier → Workers (URL dispatch) | ~5 MB/s |
| DNS upstream queries | ~30 MB/s |

---

## 3. High-Level Architecture

```
                ┌──────────────────────────────────────────────────────────────────┐
                │                    FRONTIER MANAGEMENT                           │
                │                                                                  │
                │  ┌───────────────┐    ┌──────────────────┐    ┌──────────────┐  │
                │  │  Seed URLs /  │───▶│  URL Normalizer  │───▶│  URL Dedup   │  │
                │  │  Sitemap      │    │  & Validator     │    │  (Bloom/     │  │
                │  │  Injector     │    │                  │    │   Redis SET)  │  │
                │  └───────────────┘    └──────────────────┘    └──────┬───────┘  │
                │                                                       │          │
                │  ┌────────────────────────────────────────────────────▼───────┐  │
                │  │                    URL FRONTIER                            │  │
                │  │  ┌─────────────────────┐    ┌────────────────────────┐    │  │
                │  │  │  Priority Queue     │    │  Politeness Scheduler  │    │  │
                │  │  │  (per-domain FIFO   │    │  - robots.txt cache    │    │  │
                │  │  │   within buckets)   │    │  - per-domain rate     │    │  │
                │  │  │  Backed by Redis    │    │    limiter (token      │    │  │
                │  │  │  Sorted Sets        │    │    bucket per domain)  │    │  │
                │  │  └──────────┬──────────┘    └────────────┬───────────┘    │  │
                │  └────────────┼──────────────────────────────┼───────────────┘  │
                └───────────────┼──────────────────────────────┼──────────────────┘
                                │ URL dispatch                 │ robots.txt OK?
                                ▼                              │
                ┌───────────────────────────────────────────────────────────────┐
                │                   FETCH TIER                                  │
                │                                                               │
                │  ┌──────────────────────────────────────────────────────┐    │
                │  │           Fetch Worker Pool (×N machines)            │    │
                │  │                                                       │    │
                │  │  ┌──────────────┐  ┌─────────────┐  ┌────────────┐  │    │
                │  │  │  DNS Cache   │  │  HTTP/2     │  │  Rate      │  │    │
                │  │  │  (in-process │  │  Client     │  │  Limiter   │  │    │
                │  │  │   + Redis)   │  │  Pool       │  │  (domain)  │  │    │
                │  │  └──────────────┘  └──────┬──────┘  └────────────┘  │    │
                │  └─────────────────────────────┼────────────────────────┘    │
                └───────────────────────────────┼────────────────────────────── ┘
                                                 │ HTTP Response
                                                 ▼
                ┌───────────────────────────────────────────────────────────────┐
                │                 CONTENT PROCESSING TIER                      │
                │                                                               │
                │  ┌──────────────────────────────────────────────────────┐    │
                │  │             Content Processor (per fetched page)     │    │
                │  │                                                       │    │
                │  │  1. Content dedup check (SimHash fingerprint)        │    │
                │  │  2. Store raw HTML to Object Store (S3/GCS)          │    │
                │  │  3. Extract links (HTML parser)                      │    │
                │  │  4. Normalize + validate extracted URLs               │    │
                │  │  5. Enqueue new URLs to URL Frontier                 │    │
                │  │  6. Update crawl metadata DB (status, timestamp)     │    │
                │  │  7. Emit crawled page event to downstream consumers  │    │
                │  └──────────────────────────────────────────────────────┘    │
                └───────────────────────────────────────────────────────────────┘
                        │                │                 │
                        ▼                ▼                 ▼
               ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐
               │ Object Store │  │  Crawl Meta  │  │  Message Queue   │
               │ (S3: raw     │  │  DB (Cassand-│  │  (Kafka: crawled │
               │  HTML pages) │  │  ra/DynamoDB)│  │   pages topic)   │
               └──────────────┘  └──────────────┘  └──────────────────┘
                                                           │
                                              ┌────────────┼────────────┐
                                              ▼            ▼            ▼
                                       Search Indexer  ML Pipeline  Link Graph
                                                                     Builder
```

**Component Roles**:

- **URL Normalizer & Validator**: Canonicalizes URLs (lowercase scheme/host, resolve `../`, strip fragments, decode percent-encoding, remove UTM parameters, enforce max URL length 2048 chars). Rejects URLs with disallowed schemes (ftp, mailto, javascript), private IP ranges (RFC 1918 — prevents SSRF), and URLs exceeding 5 path segments below seed (depth limit).
- **URL Dedup Store**: Maintains a set of all ever-seen URLs. Uses a multi-level approach: Bloom filter for fast "never seen" check, followed by exact-match in Cassandra for frontier scheduling. Prevents the same URL from being enqueued multiple times.
- **URL Frontier**: Core data structure managing the crawl queue. Organized as per-domain FIFO sub-queues, combined into a priority-ordered back queue. The frontier is the scheduling brain of the crawler — it controls which URL gets fetched next while respecting politeness.
- **Politeness Scheduler**: Enforces crawl-delay and rate limits. Maintains per-domain token buckets (default: 1 token/second; overridden by `Crawl-delay` in robots.txt). A domain is only made available to the front queue when its token bucket has a token.
- **robots.txt Cache**: Fetches and caches `robots.txt` for each domain (24-hour TTL). Before dispatching any URL from a domain, the frontier checks this cache. New domains trigger a robots.txt pre-fetch.
- **Fetch Workers**: Stateless HTTP clients. Each worker manages a pool of persistent HTTP/2 connections (up to 6 concurrent streams per connection, up to 100 connections per worker to different domains). Workers implement timeout (10s connection, 30s total), redirect following (max 5), and response streaming (stop reading after 5 MB).
- **DNS Cache**: In-process LRU cache with 4-hour TTL, backed by a shared Redis cache. Avoids the 50 ms DNS resolution latency for repeat crawls of the same domain.
- **Content Processor**: Receives the HTTP response and coordinates: dedup fingerprint, storage, link extraction, metadata update, and downstream notification. Runs as a Kafka consumer (async from fetch) to decouple fetch throughput from processing speed.
- **Content Dedup**: Computes SimHash (64-bit) of page content. Compares against a fingerprint store. Near-duplicate pages (Hamming distance ≤ 3) are flagged and not re-stored but their crawl metadata is updated.
- **Recrawl Scheduler**: Periodic job (hourly) that analyzes crawl metadata to identify URLs due for re-fetch. Uses a change frequency model (EMA of observed change intervals) to set next-crawl-time for each URL.

**Primary Use-Case Data Flow (seed URL → stored page)**:

1. Seed URL `https://news.example.com` injected via Seed Injector.
2. URL Normalizer canonicalizes: `https://news.example.com/` (adds trailing slash).
3. URL Dedup checks: not in Bloom filter → definitely new. Adds to Bloom filter.
4. URL added to URL Frontier with initial priority score based on domain authority.
5. Politeness Scheduler checks robots.txt cache for `news.example.com`. Cache miss → triggers background robots.txt fetch. URL placed in pending queue.
6. robots.txt fetched: `Allow: /; Crawl-delay: 2`. Domain gets token bucket rate = 0.5/s.
7. Token bucket has token → URL dispatched to available Fetch Worker.
8. Fetch Worker: DNS lookup (cache miss → resolves to 93.184.216.34 → cached 4 hours). Opens HTTP/2 connection. Sends `GET / HTTP/2` with `User-Agent: ExampleBot/1.0 (+https://example.com/bot)`.
9. Server responds: 200 OK, Content-Type: text/html, Content-Encoding: gzip, 28 KB.
10. Worker decompresses, streams response (< 5 MB check). Publishes `FetchResult` to Kafka `fetched_pages` topic.
11. Content Processor consumes from Kafka:
    a. Computes SimHash of text content → no near-duplicates found.
    b. Stores raw HTML to S3: `s3://crawl-store/2026/04/09/news.example.com/HASH.html.gz`
    c. HTML parser extracts 42 links. Normalizer reduces to 18 unique, valid, non-duplicate URLs.
    d. 18 URLs enqueued to URL Frontier via Kafka `new_urls` topic.
    e. Crawl metadata row written to Cassandra: `{url_hash, url, status=200, crawled_at, content_hash, next_crawl_at}`.
    f. `crawled_page` event emitted to Kafka for downstream Search Indexer.
12. 18 extracted URLs repeat the cycle from step 2.

---

## 4. Data Model

### Entities & Schema

**url_record** (Cassandra — primary crawl tracking table):
```sql
CREATE TABLE url_record (
    url_hash        BIGINT,          -- xxHash64(normalized_url), partition key
    url             TEXT,            -- full normalized URL
    domain          TEXT,            -- extracted hostname
    scheme          TEXT,            -- http or https
    status          INT,             -- last HTTP status code (200, 301, 404, etc.)
    crawled_at      TIMESTAMP,       -- last successful crawl time
    first_seen_at   TIMESTAMP,       -- when URL was first discovered
    content_hash    BIGINT,          -- SimHash-64 of last fetched content
    etag            TEXT,            -- HTTP ETag header, for conditional GETs
    last_modified   TIMESTAMP,       -- HTTP Last-Modified, for conditional GETs
    next_crawl_at   TIMESTAMP,       -- scheduled next crawl (recrawl scheduler sets this)
    crawl_depth     INT,             -- hops from seed URL
    priority_score  FLOAT,           -- 0.0 to 1.0, higher = crawl sooner
    url_length      INT,
    redirect_target TEXT,            -- if last response was 3xx
    error_count     INT,             -- consecutive errors, used for back-off
    PRIMARY KEY (url_hash)
) WITH CLUSTERING ORDER BY (url_hash ASC)
  AND default_time_to_live = 7776000;  -- 90-day TTL for dead URLs
```

**domain_record** (Cassandra — per-domain metadata):
```sql
CREATE TABLE domain_record (
    domain          TEXT PRIMARY KEY,
    robots_txt      TEXT,            -- cached robots.txt content
    robots_fetched  TIMESTAMP,       -- when robots.txt was last fetched
    crawl_delay_ms  INT,             -- from robots.txt (default 1000ms)
    is_blocked      BOOLEAN,         -- manual block (spam/malware)
    page_count      BIGINT,          -- total pages crawled from this domain
    authority_score FLOAT,           -- domain-level importance signal
    last_crawled    TIMESTAMP
);
```

**crawl_metadata** (Cassandra — time-series of crawl events):
```sql
CREATE TABLE crawl_log (
    url_hash        BIGINT,
    crawled_at      TIMESTAMP,
    status          INT,
    content_hash    BIGINT,
    content_changed BOOLEAN,         -- true if content_hash differs from previous
    fetch_time_ms   INT,
    response_size_b INT,
    worker_id       TEXT,
    PRIMARY KEY (url_hash, crawled_at)
) WITH CLUSTERING ORDER BY (crawled_at DESC)
  AND default_time_to_live = 7776000;
```

**frontier_queue** (Redis Sorted Sets — active URL frontier):
```
Key: frontier:back:{domain_hash}        Type: ZSET, score=priority, member=url_hash
Key: frontier:domain_ready              Type: ZSET, score=next_available_at, member=domain_hash
Key: domain:token:{domain}              Type: String (float, token count), TTL-refreshed
Key: robots:{domain}                    Type: String (robots.txt content), TTL=86400s
Key: dns:{hostname}                     Type: String (IP address), TTL=14400s
```

**content_fingerprint** (Redis SET + Cassandra for overflow):
```
Redis:    Key: simhash:{shard}    Type: SET of 64-bit SimHash values (64 shards by hash prefix)
Cassandra: simhash_store (simhash BIGINT PRIMARY KEY, url TEXT, crawled_at TIMESTAMP)
```

**Object Store structure (S3)**:
```
s3://crawl-raw/{year}/{month}/{day}/{hour}/{domain}/{url_hash}.html.gz
s3://crawl-raw/{year}/{month}/{day}/{hour}/{domain}/{url_hash}.meta.json
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| Cassandra | URL records, crawl log | Linear horizontal scale, wide-row model fits time-series crawl log, tunable consistency, excellent write throughput (100K writes/s per node) | No ad-hoc secondary indexes, requires careful data modeling, eventual consistency | **Selected for url_record and crawl_log** — write-heavy, key-value access pattern by url_hash fits perfectly |
| MySQL / PostgreSQL | URL records | ACID, familiar, secondary indexes | Doesn't scale to 10B+ row URL table without aggressive sharding | Rejected — poor write scalability at this volume |
| Redis | URL frontier, DNS cache, rate limiters, robots.txt cache | Sub-ms latency for sorted set operations (ZADD O(log N), ZRANGEBYSCORE O(log N + M)), native TTL, Lua scripting for atomic frontier operations | Memory-limited, not durable primary store | **Selected for URL frontier and all caches** — sorted sets are exactly the data structure needed for priority queues |
| Apache Kafka | URL pipeline, fetched pages pipeline | Durable ordered log, decouples producers from consumers, replay capability | Not a database; requires consumer offset management | **Selected as the inter-component message bus** |
| S3 / GCS | Raw HTML content storage | Near-infinite scale, very cheap (~$23/TB/month), no infrastructure management | High latency (50–200 ms per object), no query capability | **Selected for raw HTML** — content is write-once, read-rarely; only object storage is economically viable |
| HBase | URL fingerprint store | Strong consistency, fast key lookups | Complex ops (HDFS dependency), JVM overhead | Rejected — Cassandra is simpler with similar performance |

---

## 5. API Design

### URL Frontier: Worker Dispatch API (Internal gRPC)

```protobuf
// Fetch worker requests URLs to crawl
service FrontierService {
  // Worker requests a batch of URLs to fetch
  rpc GetNextUrls(GetNextUrlsRequest) returns (GetNextUrlsResponse);
  
  // Worker reports fetch result
  rpc ReportFetchResult(FetchResultRequest) returns (FetchResultResponse);
  
  // Submit newly discovered URLs to frontier
  rpc SubmitUrls(SubmitUrlsRequest) returns (SubmitUrlsResponse);
}

message GetNextUrlsRequest {
  string worker_id = 1;
  int32  batch_size = 2;            // max 100 URLs per request
  repeated string domain_hints = 3; // domains this worker has warm connections to
}

message GetNextUrlsResponse {
  repeated UrlAssignment assignments = 1;
  int32 frontier_depth = 2;          // total URLs remaining in frontier
}

message UrlAssignment {
  int64  url_hash = 1;
  string url = 2;
  string domain = 3;
  float  priority = 4;
  string etag = 5;           // for conditional GET
  string last_modified = 6;  // for conditional GET
}

message FetchResultRequest {
  int64     url_hash = 1;
  string    url = 2;
  int32     http_status = 3;
  int64     content_hash = 4;    // SimHash of content
  int32     content_size_bytes = 5;
  int32     fetch_time_ms = 6;
  string    etag = 7;
  string    last_modified = 8;
  string    redirect_url = 9;    // populated for 3xx
  string    error_message = 10;  // for failed fetches
  string    s3_key = 11;         // where raw HTML was stored
  repeated string extracted_urls = 12;  // up to 200 extracted URLs
}
```

### Seed URL Injection API (REST — Operator Interface)

```
POST /v1/seeds
Authorization: Bearer <operator_token>
Content-Type: application/json

Request:
{
  "urls": [
    "https://news.ycombinator.com",
    "https://en.wikipedia.org"
  ],
  "priority": "high",          // high|normal|low
  "recrawl_interval_hours": 24,
  "max_depth": 5
}

Response 202 Accepted:
{
  "job_id": "seed-2026040901",
  "urls_accepted": 2,
  "urls_rejected": 0,
  "estimated_start": "2026-04-09T14:00:05Z"
}
```

### Crawl Status Query API (REST — Monitoring)

```
GET /v1/crawl/status?domain=news.ycombinator.com
Authorization: Bearer <ops_token>

Response 200:
{
  "domain": "news.ycombinator.com",
  "pages_crawled": 48231,
  "pages_in_frontier": 1420,
  "last_crawled_at": "2026-04-09T13:58:42Z",
  "crawl_delay_ms": 1000,
  "robots_txt_fetched_at": "2026-04-09T10:00:00Z",
  "is_blocked": false,
  "authority_score": 0.87,
  "recent_errors": [
    { "url": "https://news.ycombinator.com/item?id=XXXXX", "status": 429, "time": "2026-04-09T13:57:00Z" }
  ]
}

GET /v1/crawl/url?url=https://en.wikipedia.org/wiki/Web_crawler
Response 200:
{
  "url_hash": -4539201983420,
  "url": "https://en.wikipedia.org/wiki/Web_crawler",
  "status": 200,
  "crawled_at": "2026-04-09T12:31:05Z",
  "next_crawl_at": "2026-04-16T12:31:05Z",
  "content_hash": 8372910293847561,
  "crawl_depth": 2,
  "s3_key": "crawl-raw/2026/04/09/12/en.wikipedia.org/HASH.html.gz"
}
```

---

## 6. Deep Dive: Core Components

### Core Component 1: URL Frontier Design

**Problem it solves**: The URL frontier is the most critical component of a web crawler. It must: (1) hold billions of pending URLs efficiently, (2) return the highest-priority URL that is also polite to fetch right now (domain hasn't been accessed recently), (3) ensure no URL is returned twice, and (4) handle domain-level fairness (don't exhaust all budget on one domain). Naively, a single priority queue can't satisfy politeness without O(domains) complexity per dequeue. The key insight is separating domain-level rate control from URL priority.

**Approach Comparison**:

| Approach | Data Structure | Politeness | Fairness | Throughput | Memory |
|---|---|---|---|---|---|
| Single FIFO queue | Queue | None | None | Very high | Low |
| Single priority queue | Heap | None | None (top domains monopolize) | High | Low |
| Per-domain queues (Mercator design) | N domain FIFOs + priority queue of domains | Yes (domain-level gating) | Yes | High | O(D) for D domains |
| Two-level back/front queue (Cho & Garcia-Molina) | B back-queues + F front-queues (domain-mapped) | Yes (front queue assignment) | Yes | High | O(B+F) |
| Time-partitioned queue | Sorted set by next_crawl_time | Yes (just respect score) | Medium | Very high | Medium |
| Redis ZSET per domain + domain availability ZSET | Sorted sets | Yes (domain ZSET of readiness) | Yes | Very high | O(URLs + Domains) |

**Selected Approach: Two-Level Frontier with Redis Sorted Sets**

Architecture:
- **Back Queues**: One Redis ZSET per domain hash bucket (1024 buckets). Each bucket's ZSET holds `(url_hash, priority_score)` pairs for URLs from domains that hash to that bucket. Score = `-priority` (negated so ZPOPMIN returns highest-priority URL). Approximately 100K domains active at any time → average 100 domains per bucket.
- **Front Queues (Domain Readiness)**: A single Redis ZSET: `frontier:domain_ready` where score = `timestamp_when_domain_is_next_available` and member = `domain_hash`. Workers do `ZRANGEBYSCORE frontier:domain_ready 0 {current_time} LIMIT 1` to find a domain that can be crawled right now.
- **Dispatcher**: Fetches ready domain from front queue, then fetches highest-priority URL from that domain's back queue. Atomic Lua script ensures the domain is not double-dispatched.

**Implementation Pseudocode**:

```python
class URLFrontier:
    BACK_QUEUE_BUCKETS = 1024
    DEFAULT_CRAWL_DELAY_MS = 1000

    def __init__(self, redis_cluster, cassandra_session):
        self.redis = redis_cluster
        self.db = cassandra_session

    def enqueue(self, url: str, priority: float = 0.5, depth: int = 0):
        """Add a URL to the frontier. Idempotent."""
        normalized = self.normalize_url(url)
        url_hash = xxhash.xxh64(normalized).intdigest()
        domain = self.extract_domain(normalized)
        domain_hash = xxhash.xxh32(domain).intdigest()
        bucket = domain_hash % self.BACK_QUEUE_BUCKETS
        
        # Check if already in frontier or crawled recently
        # (Two-level dedup: Bloom filter for fast path, Cassandra for confirmation)
        if self.bloom_filter.contains(url_hash):
            record = self.db.execute(
                "SELECT next_crawl_at FROM url_record WHERE url_hash = %s",
                (url_hash,)
            ).one()
            if record and record.next_crawl_at > datetime.utcnow():
                return  # Not due for recrawl
        
        # Add to Bloom filter
        self.bloom_filter.add(url_hash)
        
        # Add to back queue (Redis ZSET: score = -priority so ZPOPMIN = highest priority)
        back_queue_key = f"frontier:back:{bucket}"
        self.redis.zadd(back_queue_key, {str(url_hash): -priority})
        
        # Store URL metadata in Cassandra
        self.db.execute_async(
            """INSERT INTO url_record 
               (url_hash, url, domain, first_seen_at, crawl_depth, priority_score, next_crawl_at)
               VALUES (%s, %s, %s, %s, %s, %s, %s)
               IF NOT EXISTS""",
            (url_hash, normalized, domain, datetime.utcnow(), depth, priority,
             datetime.utcnow())
        )

    def get_next_url(self, worker_id: str) -> Optional[UrlAssignment]:
        """
        Dispatcher: find a ready domain, pop its highest-priority URL.
        Uses atomic Lua script to prevent race conditions.
        """
        lua_script = """
        local now = tonumber(ARGV[1])
        local domain_ready_key = KEYS[1]
        
        -- Find a domain ready to crawl (score = next_available_time <= now)
        local ready = redis.call('ZRANGEBYSCORE', domain_ready_key, 0, now, 'LIMIT', 0, 10)
        if #ready == 0 then
            return nil
        end
        
        -- Try each ready domain (some back queues may be empty)
        for _, domain_hash in ipairs(ready) do
            local bucket = tonumber(domain_hash) % 1024
            local back_queue_key = 'frontier:back:' .. bucket
            
            -- Pop highest-priority URL from this domain's bucket
            -- Note: We filter by domain_hash prefix in the ZSET member
            -- (In production, per-domain ZSET keys are cleaner; this is simplified)
            local result = redis.call('ZPOPMIN', back_queue_key, 1)
            if #result > 0 then
                local url_hash = result[1]
                local crawl_delay = redis.call('HGET', 'domain:delays', domain_hash) or 1000
                -- Update domain's next available time
                redis.call('ZADD', domain_ready_key, now + tonumber(crawl_delay), domain_hash)
                return {domain_hash, url_hash}
            end
        end
        return nil
        """
        
        result = self.redis.eval(
            lua_script, 1, "frontier:domain_ready", time.time() * 1000
        )
        
        if not result:
            return None
        
        domain_hash, url_hash = result
        
        # Fetch full URL from Cassandra
        row = self.db.execute(
            "SELECT url, etag, last_modified, priority_score FROM url_record WHERE url_hash = %s",
            (int(url_hash),)
        ).one()
        
        if not row:
            return None
        
        return UrlAssignment(
            url_hash=int(url_hash),
            url=row.url,
            domain=self.extract_domain(row.url),
            priority=row.priority_score,
            etag=row.etag,
            last_modified=str(row.last_modified) if row.last_modified else None
        )

    def report_result(self, url_hash: int, status: int, 
                      content_hash: int, next_crawl_at: datetime):
        """Update URL record after fetch completion."""
        self.db.execute(
            """UPDATE url_record SET 
               status = %s, crawled_at = %s, content_hash = %s, 
               next_crawl_at = %s, error_count = 0
               WHERE url_hash = %s""",
            (status, datetime.utcnow(), content_hash, next_crawl_at, url_hash)
        )
```

**Frontier Size and Memory Estimate**:
- Active frontier: 9B URLs × 16 bytes (ZSET member + score) = 144 GB for the ZSET data.
- Redis handles 144 GB with a 200 GB Redis Cluster (4 nodes × 64 GB). At $0.20/GB-hour (ElastiCache), this costs ~$6,720/month — justified for a crawler at this scale.
- Alternative: Use Cassandra as the frontier backing store with Redis only for the "hot" portion of the frontier (top 10M URLs). The Cassandra-backed frontier needs a periodic "refill" job that loads the next batch of high-priority URLs into Redis sorted sets. This hybrid approach reduces Redis memory requirement to ~20 GB (top 1M URLs in Redis at any time).

**Interviewer Q&As**:

Q1: How does the frontier handle the "spider trap" problem — a website generating infinite unique URLs like `?page=1`, `?page=2`, ... `?page=10000000`?
A: Spider traps are identified by several heuristics: (1) URL depth limit: no URL more than 10 hops from a seed is enqueued — the depth field in `url_record` is checked before enqueue. (2) Per-domain page count cap: if a domain has > 1M pages already crawled and they're mostly low-quality (low PageRank, no inbound links from other domains), new URLs from it are given near-zero priority. (3) URL canonicalization: query parameters known to be pagination (`page=`, `p=`, `offset=`) are stripped when they exceed configurable limits (e.g., normalize `?page=N` to `?page=1` for dedup purposes, while still fetching the real URL). (4) URL pattern detection: if the same URL template has been seen > 10,000 times (e.g., `example.com/item/{id}`), extract a "URL template signature" and limit total instances of that template in the frontier.

Q2: How do you ensure robots.txt is honored for every fetch without a Redis lookup on every request?
A: robots.txt is cached in-process in each Fetch Worker (in-memory HashMap with 1-hour TTL, 10K entries per worker). The fetch worker checks this cache before every request. On cache miss, it checks the shared Redis robots.txt cache (24-hour TTL). On Redis miss, it fetches robots.txt from the origin server (this is itself rate-limited: we send a robots.txt fetch before the first URL for a new domain is dispatched). The result is: 99%+ of robot checks are served from in-process cache in < 1 µs. The `robots` Python library or a custom port handles robots.txt parsing (including wildcard patterns, Allow before Disallow precedence).

Q3: The frontier design assumes 100K active domains at any time. How do you handle bursts where 1M new domains are discovered in a day?
A: The domain_ready sorted set grows proportionally. At 1M domains × 16 bytes/entry = 16 MB — negligible. The back-queue buckets absorb more URLs. The real concern is the Cassandra domain_record table insert throughput: 1M new domain lookups/day = ~12 inserts/second — well within Cassandra's 100K writes/second capacity. The robots.txt pre-fetch queue fills up: 1M domains × 1 robots.txt fetch = 1M extra HTTP requests/day = 12 extra req/s — a rounding error against our 17K req/s capacity. The bloom filter for URL dedup needs to handle more distinct URLs: adding 1M domains × average 100 URLs each = 100M new URLs to the Bloom filter. This is within our daily design budget.

Q4: How do you manage frontier fairness so that a few high-authority domains (Wikipedia, Reddit) don't consume the entire crawl budget?
A: The per-domain politeness scheduler enforces a hard ceiling: even Wikipedia with millions of pages can only be crawled at `Crawl-delay` rate (default 1 req/s unless specified). So Wikipedia contributes at most 86,400 crawls/day (~86K pages), which is 0.0086% of our 1B/day budget. The priority scoring system is designed so that high-authority domains have high priority but their page count is bounded by their crawl-delay. In practice, distributing the 1B budget across 100M domains means each domain gets ~10 pages/day — the frontier naturally spreads coverage. We also implement a "domain budget cap" configurable per crawl policy: no more than 10,000 pages/day from any single domain unless explicitly configured otherwise.

Q5: How does the recrawl scheduler decide when to re-crawl a page?
A: We use an exponential moving average of observed change intervals. After each crawl, we check if `content_hash` changed from the previous crawl. If yes, we record the interval (days since last change). `next_crawl_interval = alpha * observed_interval + (1-alpha) * previous_estimate`. Alpha = 0.3 (smooth). Minimum interval: 1 hour (for news pages). Maximum interval: 30 days (for static pages). Pages that consistently return 304 Not Modified (via ETag/Last-Modified conditional GETs) are exponentially backed off up to the 30-day max. Pages that return 404 three times consecutively are removed from the frontier. This is similar to the algorithm described in Cho & Garcia-Molina (2003).

---

### Core Component 2: Content Deduplication with SimHash

**Problem it solves**: The web contains massive duplicate and near-duplicate content: syndicated news articles, mirrored sites, scraped content, paginated views of the same data with trivial differences (ads, nav bars), and canonical vs. non-canonical URLs. Without deduplication: (1) storage costs balloon (identical content stored N times), (2) the search index contains duplicate results, (3) crawl budget is wasted re-fetching seen content. We need to detect near-duplicates (not just exact copies) because near-duplicate pages share 90%+ of content with at most a few changed sentences.

**Approach Comparison**:

| Technique | What it detects | Complexity | Storage | False Positive Rate | Speed |
|---|---|---|---|---|---|
| MD5/SHA-256 hash | Exact duplicates only | O(n) | 16-32 bytes/doc | 0% | Very fast |
| MinHash / LSH | Near-duplicate sets | O(n × k) for k shingles | 128-256 bytes/doc | Configurable ~1% | Fast |
| SimHash (64-bit) | Near-duplicates (Hamming distance) | O(n) | 8 bytes/doc | ~0.4% at threshold=3 | Very fast |
| TF-IDF cosine similarity | Semantic near-duplicates | O(n²) | Large | Tunable | Slow at scale |
| Suffix array dedup | Exact + near-duplicate | O(n log n) | O(n) | Low | Slow at scale |
| Jaccard similarity (exact) | Near-duplicates | O(n²) | None (online) | 0% | Too slow for web scale |

**Selected Approach: SimHash (Charikar 2002)**

SimHash encodes a document into a 64-bit integer such that the Hamming distance between two SimHashes is proportional to 1 - Jaccard similarity of their shingle sets. Two pages with Hamming distance ≤ 3 (out of 64 bits) share ≥ 95% of their 3-gram shingle set — which corresponds to near-duplicate content for the purposes of web crawling (similar to Google's use of SimHash described in Gurmeet Singh Manku et al. 2007).

**SimHash Algorithm & Pseudocode**:

```python
import hashlib
import re
from collections import Counter

class SimHasher:
    HASH_BITS = 64
    SHINGLE_SIZE = 3   # 3-word shingles
    NEAR_DUPLICATE_THRESHOLD = 3  # Hamming distance ≤ 3 = near-duplicate

    def extract_text(self, html: str) -> str:
        """Strip HTML tags, normalize whitespace, lowercase."""
        # Remove script and style tags entirely
        html = re.sub(r'<(script|style)[^>]*>.*?</\1>', '', html, flags=re.DOTALL)
        # Strip remaining HTML tags
        text = re.sub(r'<[^>]+>', ' ', html)
        # Normalize whitespace
        text = ' '.join(text.lower().split())
        return text

    def get_shingles(self, text: str) -> list:
        """Extract overlapping N-word shingles."""
        words = text.split()
        shingles = [' '.join(words[i:i+self.SHINGLE_SIZE]) 
                    for i in range(len(words) - self.SHINGLE_SIZE + 1)]
        return shingles

    def compute_simhash(self, html: str) -> int:
        """
        Compute 64-bit SimHash of a web page.
        
        Algorithm:
        1. Extract text and tokenize into shingles
        2. Hash each shingle to a 64-bit integer
        3. For each bit position, accumulate: +1 if hash bit=1, -1 if hash bit=0
        4. Final hash bit = 1 if accumulator > 0, else 0
        """
        text = self.extract_text(html)
        shingles = self.get_shingles(text)
        
        if not shingles:
            return 0  # Empty page
        
        # Weighted accumulator: v[i] sums +1/-1 contributions to bit position i
        v = [0] * self.HASH_BITS
        
        for shingle in shingles:
            # Hash shingle to 64-bit integer (use MD5, take first 8 bytes)
            h = int.from_bytes(
                hashlib.md5(shingle.encode('utf-8')).digest()[:8], 
                byteorder='big', 
                signed=False
            )
            # For each bit: if bit is set, +1; else -1
            for i in range(self.HASH_BITS):
                if h & (1 << i):
                    v[i] += 1
                else:
                    v[i] -= 1
        
        # Build fingerprint: bit i = 1 if v[i] > 0
        fingerprint = 0
        for i in range(self.HASH_BITS):
            if v[i] > 0:
                fingerprint |= (1 << i)
        
        return fingerprint

    def hamming_distance(self, h1: int, h2: int) -> int:
        """Count differing bits between two 64-bit hashes."""
        xor = h1 ^ h2
        return bin(xor).count('1')

    def is_near_duplicate(self, fingerprint: int, 
                          candidate_fingerprints: list) -> bool:
        return any(
            self.hamming_distance(fingerprint, c) <= self.NEAR_DUPLICATE_THRESHOLD
            for c in candidate_fingerprints
        )
```

**Scaling SimHash Lookup to Billions of Documents**:

The challenge: given a new SimHash h, find if any of the 10B stored hashes has Hamming distance ≤ 3. Naive comparison: 10B × 1 ns = 10 seconds — too slow.

Solution: **Hamming distance lookup via table decomposition** (Moses Charikar's observation):

If Hamming distance ≤ 3 across 64 bits, there must exist at least one 16-bit segment where that segment is identical (by pigeonhole: 3 flipped bits across 4 segments of 16 bits means at least one segment has 0 flips).

We split the 64-bit hash into 4 × 16-bit segments and build a lookup table for each segment:
- Table[segment_0] → set of full SimHash values sharing this segment_0
- Table[segment_1] → set of full SimHash values sharing this segment_1
- ... (4 tables total)

Lookup: compute segment_0..3 of query hash, union the 4 sets, then exact Hamming-distance check only this small candidate set (typically < 100 candidates even in a 10B corpus).

Implementation: 4 Redis Sorted Sets, one per segment position. Key: `simhash:seg{0-3}:{segment_value}`, Value: full 64-bit SimHash. This achieves O(1) near-duplicate detection at web scale.

```python
class SimHashStore:
    """
    Stores and queries 64-bit SimHashes at scale.
    Uses table decomposition for O(1) near-duplicate lookup.
    """
    SEGMENT_COUNT = 4
    SEGMENT_BITS = 16
    
    def __init__(self, redis):
        self.redis = redis
    
    def _get_segments(self, simhash: int) -> list:
        segments = []
        for i in range(self.SEGMENT_COUNT):
            segment = (simhash >> (i * self.SEGMENT_BITS)) & 0xFFFF
            segments.append(segment)
        return segments
    
    def add(self, simhash: int, url: str):
        """Add a new SimHash to all 4 segment tables."""
        segments = self._get_segments(simhash)
        pipe = self.redis.pipeline()
        for i, seg in enumerate(segments):
            key = f"simhash:seg{i}:{seg}"
            pipe.sadd(key, simhash)
            pipe.expire(key, 86400 * 90)  # 90-day retention
        pipe.execute()
    
    def find_near_duplicates(self, query_hash: int, threshold: int = 3) -> list:
        """
        Find all stored SimHashes within Hamming distance `threshold`.
        Returns list of (simhash, hamming_distance) pairs.
        """
        segments = self._get_segments(query_hash)
        
        # Collect candidates from all 4 segment tables
        pipe = self.redis.pipeline()
        for i, seg in enumerate(segments):
            key = f"simhash:seg{i}:{seg}"
            pipe.smembers(key)
        segment_sets = pipe.execute()
        
        # Union all candidates
        candidates = set()
        for s in segment_sets:
            candidates.update(int(h) for h in s)
        
        # Exact Hamming distance check on candidate set
        near_dups = []
        for candidate in candidates:
            dist = bin(query_hash ^ candidate).count('1')
            if dist <= threshold:
                near_dups.append((candidate, dist))
        
        return near_dups
```

**Interviewer Q&As**:

Q1: SimHash is based on word-level shingles. What about pages with the same content in different languages?
A: SimHash will not detect cross-language near-duplicates — the shingles will be entirely different. Cross-language deduplication requires language-agnostic techniques: (1) image fingerprinting for image-heavy pages (pHash of images), (2) structural similarity (DOM tree hash — same structure, different text = same template), (3) URL canonical cross-linking (`<link rel="alternate" hreflang="en" ...>`). For the crawler's purposes, cross-language duplicates are NOT considered duplicates — a French and English version of the same Wikipedia article are different documents. Duplicate detection is intentionally for same-language same-content duplicates.

Q2: Your SimHash table decomposition assumes Hamming distance ≤ 3. What if near-duplicate threshold should be 6?
A: The table decomposition proof requires: the probability that at least one segment is identical = probability that k flipped bits are NOT uniformly distributed across 4 segments. For 3 flips across 4 segments, probability ≥ 1 (by pigeonhole). For 6 flips across 4 segments, at least one segment must be either identical (0 flips) or have 1 flip — not 0 flips. So we'd need to query not only exact segment matches but also all 2^16 one-bit-flip variants of each segment (65,536 queries per segment × 4 segments = 262,144 Redis lookups per query — too expensive). Instead, for threshold ≥ 4, use more segments: split 64 bits into 8 × 8-bit segments. For 6 flips across 8 segments: at least 2 segments are unmodified. Query pairs of matching segments: C(8,2) = 28 table lookups, each returning a small candidate set. This is the Indyk-Motwani locality-sensitive hashing approach generalized.

Q3: How do you handle boilerplate — pages with 90% shared nav/footer/sidebar and 10% unique content (the real article)?
A: We strip boilerplate before SimHash computation. Common boilerplate removal approaches: (1) Content-to-boilerplate ratio: text nodes with high term frequency across many pages are classified as boilerplate. We maintain a per-domain term IDF score; terms appearing in >50% of pages from a domain get near-zero weight in the SimHash computation. (2) DOM position: nav, header, footer, sidebar elements (detected by semantic HTML5 tags or CSS class heuristics) are excluded from SimHash text. (3) Template extraction: if 1000 pages from the same domain share an identical DOM subtree, that subtree is the template; it's excluded from content hashing. After boilerplate removal, even pages that look visually identical (same template, different article) will correctly have high Hamming distance.

Q4: At 10 billion stored SimHashes, how much memory does the 4-segment table decomposition use?
A: Each SimHash is stored 4 times (once per segment table). Each entry: 8 bytes (64-bit SimHash). Total: 10B × 4 × 8 B = 320 GB. This requires a large Redis cluster (~400 GB across 8-10 nodes at 64 GB each). Cost on ElastiCache: ~$0.20/GB-hour × 400 GB × 720 hr = ~$57,600/month — significant but proportional to a web-scale crawler's infrastructure budget. Alternative: store SimHashes on disk in Cassandra, use Redis only for the "hot" segment table (recent 7-day fingerprints in Redis, older in Cassandra). Near-duplicates of very old content are less operationally critical — the crawler would just re-store content, wasting some storage but not breaking correctness.

Q5: How does SimHash handle dynamically generated content — pages with timestamps, personalized content, or session-specific elements?
A: Dynamic elements pollute the SimHash. We strip them before hashing: (1) Date-like strings matching ISO-8601 or common date patterns are replaced with a token `DATE` before hashing. (2) Known tracking/session parameters in URLs and HTML are stripped. (3) Elements with class names like `timestamp`, `date`, `user-specific` are excluded by the boilerplate extractor. (4) JSON-LD structured data (`<script type="application/ld+json">`) containing dates or session data is excluded from text extraction. Despite these measures, some dynamism leaks through. We accept this: a SimHash distance of 3 allows 3 changed tokens, accommodating minor dynamic content without misclassifying truly unique pages as duplicates.

---

### Core Component 3: Politeness Enforcement — robots.txt and Rate Limiting

**Problem it solves**: Aggressive crawling can overwhelm web servers, triggering bans, legal threats, and reputational damage. The Web Robots Exclusion Standard (robots.txt) defines which pages a bot may and may not crawl. Crawl-delay specifies minimum time between requests. Violating these constitutes a legal risk (hiQ v. LinkedIn established that accessing public data is legal; violating robots.txt is ethically problematic and may contribute to CFAA arguments). Beyond ethics, being banned by a server (HTTP 429, IP block) means losing data coverage — hurting the crawler's purpose.

**Rate Limiter Design (Token Bucket per Domain)**:

```python
class DomainRateLimiter:
    """
    Per-domain token bucket rate limiter stored in Redis.
    Allows bursting up to 5 tokens while enforcing average rate.
    """
    
    def __init__(self, redis):
        self.redis = redis
    
    # Lua script for atomic token bucket check-and-consume
    TOKEN_BUCKET_SCRIPT = """
    local key = KEYS[1]
    local rate = tonumber(ARGV[1])          -- tokens per second
    local capacity = tonumber(ARGV[2])      -- max burst capacity
    local now = tonumber(ARGV[3])           -- current time (epoch ms)
    local requested = tonumber(ARGV[4])     -- tokens to consume (always 1)
    
    local last_time = tonumber(redis.call('HGET', key, 'last_time') or now)
    local tokens = tonumber(redis.call('HGET', key, 'tokens') or capacity)
    
    -- Refill tokens based on elapsed time
    local elapsed = (now - last_time) / 1000.0  -- convert to seconds
    local new_tokens = math.min(capacity, tokens + elapsed * rate)
    
    if new_tokens >= requested then
        -- Consume token
        redis.call('HSET', key, 'tokens', new_tokens - requested)
        redis.call('HSET', key, 'last_time', now)
        redis.call('PEXPIRE', key, 3600000)  -- 1 hour TTL
        return {1, math.floor((1 - new_tokens + requested) / rate * 1000)}  -- {allowed, wait_ms}
    else
        -- Insufficient tokens: compute wait time
        local wait_ms = math.ceil((requested - new_tokens) / rate * 1000)
        redis.call('HSET', key, 'tokens', new_tokens)
        redis.call('HSET', key, 'last_time', now)
        redis.call('PEXPIRE', key, 3600000)
        return {0, wait_ms}  -- {denied, wait_ms}
    end
    """
    
    def try_acquire(self, domain: str, rate_per_sec: float = 1.0, 
                    burst: int = 5) -> tuple:
        """
        Attempt to acquire a crawl token for a domain.
        Returns (allowed: bool, wait_ms: int)
        """
        key = f"ratelimit:{domain}"
        result = self.redis.eval(
            self.TOKEN_BUCKET_SCRIPT, 1, key,
            rate_per_sec, burst, int(time.time() * 1000), 1
        )
        return bool(result[0]), int(result[1])


class RobotsChecker:
    """
    Fetches, parses, and caches robots.txt with configurable TTL.
    """
    CACHE_TTL_SECONDS = 86400  # 24 hours
    ROBOTS_FETCH_TIMEOUT = 10  # seconds
    
    def __init__(self, redis, http_client):
        self.redis = redis
        self.http = http_client
        self._local_cache = {}  # in-process cache (LRU, 10K entries)
    
    def is_allowed(self, url: str, user_agent: str = "ExampleBot") -> bool:
        """Check if the given URL is allowed by robots.txt."""
        parsed = urllib.parse.urlparse(url)
        domain = f"{parsed.scheme}://{parsed.netloc}"
        robots_txt = self._get_robots_txt(domain)
        
        if robots_txt is None:
            return True  # No robots.txt = crawl is allowed (RFC convention)
        
        parser = urllib.robotparser.RobotFileParser()
        parser.parse(robots_txt.splitlines())
        return parser.can_fetch(user_agent, url)
    
    def get_crawl_delay(self, domain: str, 
                        user_agent: str = "ExampleBot") -> float:
        """Return crawl delay in seconds (default 1.0 if not specified)."""
        robots_txt = self._get_robots_txt(domain)
        if not robots_txt:
            return 1.0
        
        parser = urllib.robotparser.RobotFileParser()
        parser.parse(robots_txt.splitlines())
        delay = parser.crawl_delay(user_agent)
        return delay if delay else 1.0
    
    def _get_robots_txt(self, domain: str) -> Optional[str]:
        # L1: in-process cache
        if domain in self._local_cache:
            entry = self._local_cache[domain]
            if time.time() < entry['expires_at']:
                return entry['content']
        
        # L2: Redis cache
        cached = self.redis.get(f"robots:{domain}")
        if cached:
            self._local_cache[domain] = {
                'content': cached.decode('utf-8'),
                'expires_at': time.time() + 3600  # 1-hour local TTL
            }
            return cached.decode('utf-8')
        
        # L3: Fetch from origin
        return self._fetch_and_cache(domain)
    
    def _fetch_and_cache(self, domain: str) -> Optional[str]:
        robots_url = f"{domain}/robots.txt"
        try:
            response = self.http.get(robots_url, timeout=self.ROBOTS_FETCH_TIMEOUT,
                                      headers={'User-Agent': 'ExampleBot/1.0'})
            if response.status_code == 200:
                content = response.text[:500_000]  # Cap at 500 KB (DenverPost incident)
                ttl = self.CACHE_TTL_SECONDS
            elif response.status_code in (401, 403):
                content = None  # Treat as "disallow all" for safety
                ttl = 3600
            elif response.status_code == 404:
                content = ""  # No robots.txt = allow all
                ttl = self.CACHE_TTL_SECONDS
            else:
                content = None
                ttl = 300  # Short cache on errors
            
            self.redis.set(f"robots:{domain}", content or "", ex=ttl)
            self._local_cache[domain] = {
                'content': content,
                'expires_at': time.time() + min(ttl, 3600)
            }
            return content
        except Exception as e:
            logger.warning(f"Failed to fetch robots.txt for {domain}: {e}")
            return None  # Conservative: treat as allowed on fetch failure
```

**Interviewer Q&As**:

Q1: robots.txt has a TTL of 24 hours in your design. What if a site updates robots.txt to disallow your bot while you're mid-crawl of their pages?
A: We refresh robots.txt proactively in two situations: (1) Before crawling any new domain for the first time, always fetch fresh robots.txt (no cache used). (2) After receiving a 429 (Too Many Requests) from any domain, re-fetch their robots.txt immediately — they may have added a stricter Crawl-delay. Between refreshes, we accept the race condition: we may crawl 1–2 disallowed pages during the 24-hour window. This is an industry-standard acceptable behavior (Google, Bing, and all major crawlers behave this way). The alternative — checking robots.txt freshness on every single request — would require 17,000 Redis lookups/s just for robots.txt, and is operationally unjustified given the rarity of mid-day robots.txt changes.

Q2: What happens when a server returns 429 Too Many Requests? Does your rate limiter update automatically?
A: A 429 response triggers: (1) Immediate exponential backoff for that domain: current_delay × 2, up to max_delay = 60 seconds. (2) The domain is removed from the `domain_ready` sorted set and re-added with score = `now + backoff_time`. (3) The token bucket rate for that domain is halved (domain_record.crawl_delay_ms × 2). (4) If 5 consecutive 429s are received, the domain's crawl rate is set to 1/10 of default for 1 hour, and an alert fires for operator review. The 429 Retry-After header is respected: if it contains a concrete time, that overrides the exponential backoff.

Q3: How do you handle the `Disallow: *` case where a site blocks all crawling?
A: `Disallow: *` in robots.txt for our user agent means we do not crawl any page from that domain. The domain record is marked with `is_blocked_by_robots = true`. All URLs from this domain in the frontier are purged. Future URL discoveries from other pages linking to this domain are accepted into the URL metadata store (for link graph purposes) but immediately marked `status = ROBOTS_BLOCKED` and not added to the crawl queue. A daily reconciliation job checks if robots.txt for blocked domains has changed (cached version is refreshed daily) — if the block is lifted, the domain's pages are re-enqueued at low priority.

---

## 7. Scaling

### Horizontal Scaling

- **Fetch Workers**: Stateless; scale to N machines. Each machine runs 1000 concurrent async HTTP connections (using Python asyncio + aiohttp or Go's goroutines). At 300 ms average response time and 1000 concurrent connections: 1000 / 0.3 s = 3,333 fetches/s per machine. Need 17,000 / 3,333 ≈ **6 machines** minimum. Deploy 20 machines for fault tolerance and burst capacity.
- **URL Frontier (Redis)**: Redis Cluster with 10 nodes (64 GB each = 640 GB total). Handles 9B URLs × 16 bytes + overhead. ZADD throughput: ~200K ops/s per Redis node, 2M ops/s cluster-wide. At 17K URL dispatches/s × 10 commands per dispatch = 170K ops/s — safely within capacity.
- **Content Processor**: Kafka consumer group with 64 consumers (matching Kafka partition count). Each consumer: HTML parsing takes ~10 ms/page → 100 pages/s per consumer → 6,400 pages/s total. At 17K fetches/s, need 170 consumer pods. Scale the Kafka consumer group to 170 pods.
- **Cassandra**: 10-node cluster. URL records: 1B writes/day × 200 B = 200 GB/day. Cassandra handles 100K writes/s per node, 1M writes/s cluster. Our 11,574 writes/s is easily handled.

### DB Sharding

Cassandra `url_record` table is naturally sharded by `url_hash` (consistent hashing across nodes). No manual sharding needed — Cassandra's partitioner distributes by murmur hash of the partition key. The 10-node cluster with virtual nodes (vnodes=256) provides near-perfect distribution. Hot partitions are avoided because `url_hash` is a uniform hash, not correlated with domain popularity.

### Replication

- Cassandra: `replication_factor = 3`, `LOCAL_QUORUM` reads and writes. Tolerates 1 node failure with no consistency degradation.
- Redis Cluster: Each of 10 primaries has 1 replica in a different AZ. Cluster replication is async but lag is typically < 10 ms. Sentinal auto-failover on primary failure.
- Kafka: Replication factor 3, `acks=all`. Crawler fetch results cannot be lost — a lost message means a crawled page's links are not extracted.
- S3: 11 nines durability by design (cross-AZ replication).

### Caching

- **DNS cache (in-process)**: Per-worker LRU with 10K entries, 4-hour TTL. Eliminates 90% of DNS lookups. This alone saves 17,000 × 90% × 50 ms = ~765 ms of DNS latency per second of crawl throughput.
- **robots.txt cache (in-process → Redis)**: 2-level cache described above. Eliminates 99.9% of robots.txt fetches.
- **HTTP connection pool**: Workers maintain persistent HTTP/2 connections to domains being actively crawled. A connection can serve multiple requests (HTTP/2 multiplexing). Connection pool per domain, max 6 connections, reducing TCP+TLS handshake overhead.

### CDN

- Not applicable to fetch workers (they fetch from origin directly).
- The crawler's own metadata API (monitoring, status queries) is served via a CDN edge for ops dashboard users globally.

### Interviewer Q&As (Scaling)

Q1: How would you scale to 10 billion pages per day (10× current design)?
A: Linear horizontal scaling: 60 fetch worker machines, 640 Cassandra nodes (or scale to Cassandra's documented billion-row scale on fewer nodes with more disk), 640 GB Redis frontier (10 nodes → 100 nodes). The bottleneck at 10× is bandwidth: 20 Gbps inbound (10× 2 Gbps). This requires 10G NICs on fetch workers and bandwidth-optimized cloud instances. Content processing (HTML parsing) becomes the bottleneck: 170K pages/s requires 1700 content processor pods. This is Kubernetes-manageable. The real design change at 10× is the DNS infrastructure: 17K uncached lookups/s → requires a dedicated DNS resolver cluster (unbound/BIND) rather than relying on cloud DNS.

Q2: How do you handle a single mega-domain (Common Crawl has 3 billion URLs from just 100 domains) efficiently?
A: A domain with 3B URLs would dominate the frontier. Our per-domain bucket approach handles this: the 3B URLs are spread across 1024 back-queue buckets by URL hash. The domain is still rate-limited to 1 req/s by the politeness scheduler. At 1 req/s, crawling 3B URLs from one domain would take 95 years — clearly we need domain-level prioritization (crawl the most important 1M pages from this domain, skip the tail). We implement a per-domain crawl budget: no more than 1M pages crawled from any domain per 30-day cycle. URLs beyond the budget are given priority_score = 0 (never dispatched until budget resets).

Q3: How do you distribute the fetch workers across geographies to minimize crawl latency?
A: We deploy fetch workers in multiple regions co-located with concentrations of web servers: US-East (majority of .com domains), EU-West (majority of .eu/.de/.fr), AP-Northeast (Japan/Korea), AP-Southeast (Australia/SEA). GeoDNS for crawl targets isn't applicable (we resolve each domain's actual IP and connect to it). Instead, we use Anycast DNS resolvers in each region and GeoIP-based URL assignment: the URL frontier assigns URLs from European domains to EU workers and Asian domains to AP workers. This reduces round-trip time for fetches from ~150 ms (trans-Atlantic) to ~20 ms (intra-region), nearly halving the 300 ms average fetch time and doubling throughput per worker.

Q4: How does the crawler handle IPv6 and CDN-served sites where DNS returns different IPs per region?
A: We resolve DNS in each crawl region and use the regionally-resolved IP. This is actually desirable — connecting to the CDN POP nearest our fetch workers. DNS TTL from our cache perspective is the DNS record's TTL, not our default 4-hour TTL: we respect the shorter of (DNS record TTL, 4 hours). CDN DNS records often have TTLs of 60–300 seconds to enable fast failover. For sites with extremely short DNS TTLs (< 60s), we bypass our DNS cache and resolve on every request — the overhead is acceptable for the small fraction of such sites.

Q5: What's the strategy for handling a DDoS-like scenario where your crawler accidentally causes a 20K QPS storm on a small site?
A: This should be prevented before it happens: rate limiting at the domain level means the maximum rate any single domain experiences from our crawler is `max(1/s, Crawl-delay)`. A 20K QPS scenario would require 20,000 different domains all pointing to the same origin server (a CDN with many customers on one IP). If we detect this via HTTP 429 responses from multiple domains resolving to the same IP subnet: (1) We maintain a per-IP-block (CIDR /24) request rate counter. (2) If an IP block receives > 100 req/s from our crawler, we throttle all domains resolving to that IP block. (3) On detection of shared hosting, we treat the entire /24 as a single "virtual domain" for rate limiting purposes.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Detection | Mitigation | Data Loss Risk |
|---|---|---|---|---|
| Fetch Worker pod crash | Pod dies mid-fetch | Kafka consumer offset not committed | K8s restarts pod; message is reprocessed from last committed Kafka offset | None (at-least-once) |
| Redis Frontier node failure | Cluster node unreachable | Redis Cluster failover triggers | Sentinel promotes replica within 10s; inflight URLs re-dispatched | < 10 seconds of frontier URLs |
| Cassandra node failure | Connection timeout | Health check failure | Quorum reads/writes route to remaining nodes; RF=3 tolerates 1 failure | None |
| Kafka broker failure | Producer connection error | KafkaProducer exception | Idempotent producer retries; leader election < 10s | None (acks=all) |
| DNS resolver failure | DNS lookup timeout | SERVFAIL or timeout | Fallback to secondary DNS resolver; use stale cached IP (serve stale) | Crawl delay only |
| Target website down | Connection timeout | HTTP error 5xx / timeout | URL re-queued with backoff (30 min, 2 h, 12 h, 24 h, 7 days) | None |
| Content Processor crash | Exception in Kafka consumer | Kafka consumer lag increases | Consumer restarted; reprocesses from last committed offset | None |
| S3 write failure | S3 unavailable | S3Client exception | Retry 3× with exponential backoff; buffer to local SSD and write async | Local SSD buffer up to 10 GB |
| Full AZ failure | All services in one AZ offline | Multi-metric alert | Fetch workers auto-scale in surviving AZ; Cassandra/Redis serve from replicas in other AZs | None |

### Failover

- **Fetch Workers**: Kubernetes Deployment with `minAvailable = 50%`. If an AZ fails, cluster autoscaler provisions replacement pods in surviving AZs within 3 minutes.
- **Redis Frontier**: Cross-AZ replicas. Cluster bus detects node failure within 1 second; automatic leader election within 10 seconds. During election, the domain_ready ZSET is temporarily unavailable — dispatchers pause for < 10s.
- **Cassandra**: Multi-AZ deployment; LOCAL_QUORUM with RF=3 means 2 of 3 nodes must be in the same AZ as the coordinator. A full AZ loss temporarily degrades to a single-AZ cluster for that AZ's data — acceptable.

### Retries & Idempotency

- **Fetch retries**: Failed fetches (network error, 5xx) are re-queued with exponential backoff: 30 min, 2 hr, 12 hr, 24 hr. After 5 failures, URL is marked `status=FAILED_PERMANENT` and not retried unless manually reset.
- **Idempotent content storage**: S3 key = `{year}/{month}/{day}/{hour}/{domain}/{url_hash}.html.gz`. Re-fetching the same URL and storing again simply overwrites the S3 object — idempotent. The `content_hash` in Cassandra is updated on each fetch; if it's unchanged, the S3 write is skipped entirely (content-addressed: same hash → same key → no-op write).
- **URL enqueue idempotency**: The Bloom filter + Cassandra `INSERT IF NOT EXISTS` ensures double-enqueueing is harmless: the second attempt is a no-op.

### Circuit Breaker

Fetch workers implement a per-domain circuit breaker: if > 10 consecutive requests to a domain return 5xx or timeout within 5 minutes, the domain is circuit-opened: no new requests for 30 minutes. After 30 minutes, 1 test request is sent (half-open); if successful, the circuit closes and normal crawling resumes. This prevents hammering a site that's having an outage.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Interpretation |
|---|---|---|---|
| `crawler.fetch_rate` | Counter | < 8K/s (dead man) | Crawler throughput baseline |
| `crawler.success_rate` | Gauge | < 60% | High error rate; many sites down or blocking |
| `frontier.depth` | Gauge | > 20B URLs | Crawl debt accumulating faster than crawl rate |
| `frontier.domain_blocked_pct` | Gauge | > 20% | Many domains blocked (bot detection?) |
| `dedup.near_duplicate_rate` | Gauge | > 30% | Web is full of duplicate content; expected baseline ~15% |
| `politeness.violations` | Counter | > 0 | Any violation = immediate alert; check rate limiter |
| `dns.resolution_p99_ms` | Histogram | > 200 ms | DNS infrastructure slow |
| `kafka.consumer_lag.content_processor` | Gauge | > 500K messages | Content processing falling behind |
| `robots_fetch.failure_rate` | Gauge | > 5% | Network issues or bot detection for robots.txt |
| `recrawl.overdue_url_count` | Gauge | > 1M | Recrawl scheduler falling behind |
| `storage.s3_write_failure_rate` | Counter | > 0.01% | S3 availability issue; local SSD buffer filling |
| `cassandra.p99_write_latency_ms` | Histogram | > 50 ms | Cassandra under pressure |

### Distributed Tracing

Each URL is assigned a `crawl_trace_id` when first enqueued. This ID propagates through: URL Frontier dispatch → Fetch Worker → Content Processor → S3 write → Cassandra update → Kafka emit. A trace shows the complete lifecycle of any page: "URL X was discovered at 14:00:05 by page Y, dispatched to worker W3 at 14:00:08, fetched in 342 ms at 14:00:09, stored to S3 at 14:00:10, link extraction found 18 new URLs at 14:00:11."

### Logging

Structured JSON logs per fetch event: `{url, domain, http_status, fetch_time_ms, content_size, content_hash, worker_id, timestamp, trace_id, robots_allowed, dedup_result}`. Sampled at 1% for INFO (accepted), 100% for WARN (4xx) and ERROR (network failure, robots violation). Shipped via Fluentd to Elasticsearch. Dashboard: Kibana with domain-level drill-down.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Choice | Alternative | Reason | Trade-off Accepted |
|---|---|---|---|---|
| URL Frontier storage | Redis Sorted Sets | Database-backed queue | Sub-ms priority queue operations; domain-level sorted sets are the ideal data structure | Memory cost (~200 GB Redis) |
| Content dedup | SimHash (64-bit) | MD5 (exact only) | Detects near-duplicates (syndicated content, templates) that exact hashing misses | 0.4% false positive rate for truly distinct pages |
| Crawl state DB | Cassandra | MySQL | Linear horizontal scale for 10B+ URL table; write-heavy workload fits Cassandra's LSM-tree model | Eventual consistency; no ad-hoc secondary indexes |
| robots.txt caching | 24-hour TTL, 2-level cache | Per-request check | Reduces robots.txt overhead from 17K checks/s to ~17/s | May crawl newly-disallowed pages for up to 24 hours |
| Fetch worker concurrency | Async I/O (1000 connections per machine) | Thread-per-connection | 1000× better resource efficiency; thread-per-connection at 17K req/s = 17K threads per machine (impossible) | Async programming complexity |
| Content storage | S3 + Snappy compression | Local disk / HDFS | Near-infinite scale, low cost, no infrastructure management; raw HTML is write-once-read-rarely | High per-object latency (50-200 ms); no query capability |
| DNS caching | In-process LRU + Redis, 4-hour TTL | DNS resolver-only | DNS is on every request's critical path; eliminating 90% of DNS lookups is the single biggest latency win | Stale IP addresses for sites that change IPs within 4 hours (rare) |
| Recrawl scheduling | EMA of observed change intervals | Fixed intervals | Adaptive to actual page change behavior; high-change pages get more crawl budget | Requires tracking change history; more complex than fixed schedule |
| Politeness | Token bucket per domain | Fixed delay | Token bucket allows short bursts (e.g., 5 URLs from a large site in rapid succession) while enforcing average rate | Slightly more complex than fixed delay |

---

## 11. Follow-up Interview Questions

Q1: How does your crawler handle JavaScript-rendered content (SPAs built with React/Vue)?
A: The base crawler fetches raw HTML and does not execute JavaScript. This means SPAs that render content client-side return near-empty HTML shells. To handle this, we have a second-pass JavaScript rendering queue (separate from the main crawler): pages flagged as "potentially JS-rendered" (detected by low text-to-HTML ratio, presence of `<div id="app">` or similar SPA markers, or explicit `noindex` on the raw HTML that shouldn't be there) are sent to a headless Chromium pool (running via Playwright). The headless pool renders at 200 pages/s (vs. 17,000 pages/s for raw HTML) — it's a separate, smaller pipeline. This is similar to how Googlebot has two waves: a fast raw HTML crawl and a slower JS rendering queue.

Q2: How do you detect and handle redirect chains (A → B → C → D)?
A: The fetch worker follows redirects up to 5 hops (configurable, matching HTTP spec recommendation). For each intermediate URL in the chain, we log the redirect to Cassandra with `redirect_target` field. The canonical URL (final destination) is what gets stored and indexed. All intermediate URLs are marked `status=301_REDIRECT` with `redirect_target` pointing to the canonical. The URL Frontier deduplication uses the canonical URL — if C → D was already crawled, a future discovery of A or B won't trigger re-crawl because the canonical URL dedup check catches it. Redirect cycles (A → B → A) are detected by tracking seen URLs within the redirect chain and breaking at cycle detection.

Q3: How would you prioritize crawling breaking news URLs within minutes of publication?
A: We integrate with Pubsubhubbub (WebSub) — sites that want their content crawled quickly publish pings to a WebSub hub when pages are updated. Our crawler registers as a WebSub subscriber; ping notifications bypass the normal URL frontier and go directly to a high-priority "real-time queue" with priority_score = 1.0. We also monitor Twitter/social feeds for new URL shares (URL amplification signal) and sitemaps with `<lastmod>` timestamps. For news domains specifically, we configure a shorter recrawl interval (30 minutes for news homepages), extract headline links, and assign them high priority. Combined, breaking news URLs typically reach our fetch workers within 5–10 minutes of publication.

Q4: How does the crawler handle honeypot traps — links specifically designed to catch and block web crawlers?
A: Honeypot links are typically: invisible to human users (CSS `display:none`, zero-size elements, white-on-white text) and present in robots.txt-disallowed paths. Our mitigations: (1) We respect robots.txt — if the honeypot is in a `Disallow` path, we don't crawl it. (2) We do not follow links in invisible HTML elements (we filter out links in `display:none` elements during link extraction). (3) We do not follow links in HTML comments. (4) Rate of IP blocks: if our IP is blocked on a domain (connection reset / 403 with no prior 429), we check if the blocked URL was referenced only from that domain (self-referential honeypot) and add it to a per-domain block list. (5) We use a small set of stable egress IP addresses — if one gets blocked, we have a rotation, but we prefer resolving the underlying cause (too aggressive crawl rate) over IP rotation.

Q5: How do you measure "crawl freshness" — the percentage of your index that accurately represents the current web?
A: Crawl freshness is measured as: percentage of pages in our index where the crawled version matches the current live version. We estimate this by taking a random 1% sample of our crawled URLs daily and re-fetching them, then comparing `content_hash` of the fresh fetch to the stored `content_hash`. If they differ, the page has changed since we last crawled it — it's "stale." Freshness = 1 - stale_rate. Target: 90% freshness for news domains (recrawled frequently), 70% freshness for long-tail content. This metric is computed daily and used to tune the recrawl scheduler's EMA smoothing factor.

Q6: What legal/ethical considerations affect the crawler design?
A: Key considerations: (1) `robots.txt` compliance is industry-standard — all major crawlers honor it. Violation is ethically problematic and potentially legally risky. (2) Terms of Service (ToS) — many sites disallow crawling in ToS. hiQ v. LinkedIn (9th Circuit) held that accessing public data doesn't violate CFAA, but ToS may have other enforcement mechanisms. Our crawler targets only public, non-authenticated web pages. (3) Copyright — we store raw HTML which contains copyrighted content. This is covered by the transformative use doctrine for search (Perfect 10 v. Amazon established this for Google). We honor `noarchive` meta tags by not storing permanent copies. (4) GDPR — if crawled pages contain personal data (EU users' public profiles), we must handle them per GDPR. We do not store personal data in queryable form; raw HTML is stored in encrypted S3 with access controls. (5) Rate limits — even if robots.txt doesn't specify limits, crawling faster than 1 req/s is generally considered impolite for small sites.

Q7: How does your link extraction handle malformed HTML and encoding issues?
A: We use a permissive HTML parser (html5lib in Python or a Go equivalent) that handles malformed HTML by following the HTML5 parsing specification's error recovery rules — the same rules browsers use. This handles unclosed tags, mismatched nesting, etc. Encoding detection: we inspect the HTTP `Content-Type` header's charset parameter first, then look for `<meta charset=...>` or `<meta http-equiv="Content-Type" ...>` in the first 1024 bytes of the document, then fall back to chardet (universal encoding detector) with confidence > 70% required. URLs extracted from `href` attributes are: (1) resolved against the document's base URL (or `<base href>` if present), (2) decoded from HTML entities (`&amp;` → `&`), (3) percent-encoded appropriately for non-ASCII characters, (4) validated against RFC 3986.

Q8: How would you build a politeness-aware distributed crawler where 1000 fetch workers all respect per-domain rate limits without coordination overhead?
A: The key is that all workers share the Redis-based domain_ready sorted set and token buckets. Coordination is implicit through Redis: when Worker A acquires a token for domain D, it updates D's next_available_time in the ZSET atomically (Lua script). Worker B's next ZRANGEBYSCORE query will not see domain D as available until the cooldown expires. This is coordination via shared state rather than explicit peer-to-peer coordination. The overhead: 1 Lua script execution per URL dispatch (~500 µs). At 17K dispatches/s, this is 8.5 seconds of Redis Lua execution per second — but since Lua scripts are atomic, they're sequential on a single Redis instance. With Redis Cluster (10 nodes), each domain_ready key is on one node, and 17K ops/s distributed across 10 nodes = 1.7K ops/s per node, well within Redis's throughput.

Q9: How do you build the link graph — which domains link to which — and why is it valuable?
A: For each crawled page, we record all outbound links in a `link_graph_edges` Cassandra table: `(source_url_hash, target_url_hash, anchor_text, discovered_at)`. With 15B links discovered per day, this table grows at 300 GB/day. We partition by `target_url_hash` so that the set of all pages linking to a given URL (backlinks) is efficiently queryable. The link graph is valuable for: (1) PageRank-style authority computation — how many pages link to URL X determines X's crawl priority. (2) Anchor text aggregation — what phrases do pages use when linking to X helps with search relevance for X. (3) Freshness signals — a sudden spike in pages linking to X (viral content) should trigger immediate re-crawl of X. (4) Spam detection — pages with only low-quality inbound links from spammy domains are lower priority.

Q10: How does your crawler behave during a DDoS attack on your own infrastructure — where millions of malicious crawl requests flood your seed injector?
A: The seed injector is protected by: (1) API authentication (Bearer tokens) — only authorized operators can inject seeds. (2) Per-token rate limiting: 1000 seed URLs per minute per operator. (3) URL validation rejects malicious payloads (internal IP addresses — SSRF prevention, maximum URL length, allowed URL schemes). (4) The URL frontier's Bloom filter naturally absorbs duplicate seed injections — injecting the same URL 1 million times has the same effect as injecting it once. The seed injector is not on the public internet — it's an internal tool accessible only from the operations network. DDoS attacks from the public internet cannot reach it.

Q11: How do you handle a crawler budget — balancing between discovering new URLs and recrawling known URLs?
A: Our 1B pages/day budget is split: (1) New URL discovery: high-priority unknown URLs (depth=0, high-authority domains). (2) Recrawl queue: URLs whose `next_crawl_at <= now`. The split is dynamically adjusted: if the recrawl queue backlog > 100M overdue URLs, we allocate 70% of budget to recrawl. If recrawl backlog is healthy (< 10M overdue), we allocate 70% to new URL discovery. This is a feedback control loop: `recrawl_fraction = min(0.7, max(0.3, overdue_count / target_overdue)`. The priority scoring ensures that high-value new URLs (from high-authority domains) still get crawled even when recrawl backlog is high — they're simply given higher scores than low-priority recrawls.

Q12: What's the strategy for handling sites that require session cookies or CAPTCHAs to render content?
A: Our crawler does not handle authenticated content or CAPTCHA-protected content — this is out of scope by design. For sites that set cookies on first visit and require them for subsequent visits (session-based analytics), we do maintain session cookies within a domain: the HTTP client stores and sends cookies for the duration of a crawl session (until the session expires or we cycle workers). However, CAPTCHAs are a deliberate signal from the site operator that they don't want automated access, which we respect. Sites with CAPTCHA on robots.txt-allowed paths are noted in our domain_record and deprioritized, with the assumption that they're unhappy with crawling.

Q13: How do you test that your robots.txt parser correctly handles all edge cases?
A: We maintain a test suite of 500+ robots.txt snippets covering: wildcard patterns (`Disallow: /private*`), Allow overrides (`Allow: /public Allow /private/publicpage Disallow: /private`), multiple user-agent blocks, `Crawl-delay`, `Sitemap` directives, extremely large files (500 KB), binary garbage in robots.txt, encoding issues (UTF-8 BOM, Latin-1), and conflicting rules. We fuzz-test the parser against the Python `robotparser` reference implementation. We also run a periodic "compliance audit" — fetch robots.txt from the top 100K domains and verify our parser's interpretation matches expectations from the site's apparent intent (validated by checking which pages are/aren't linked from their sitemap).

Q14: How do the various URL normalizations you apply affect precision — could you lose URLs that should be crawled separately?
A: URL normalization has real tradeoffs. Conservative normalizations (lowercase scheme/host, remove default ports, decode percent-encoding) are always safe. Aggressive normalizations (remove query parameters, remove trailing slashes, canonicalize `?session_id=X` URLs) can cause missed pages. Our approach: we apply a configurable normalization profile. The aggressive profile (strips tracking params, removes query params beyond a whitelist) is used for deduplication scoring but the original URL is preserved for fetching. A URL might be "deduped" against a canonical form but still gets fetched as the original — we just don't add it to the frontier twice. The normalization whitelist includes query params known to affect content (`?id=`, `?page=`, `?lang=`) and excludes params known to be tracking-only (`utm_source`, `fbclid`, `gclid`).

Q15: How would you implement a "politeness dial" that lets the crawler be more or less aggressive based on available infrastructure capacity?
A: We expose a global rate multiplier: `crawl_rate_multiplier = 1.0` (normal), adjustable between 0.1 (10% capacity) and 3.0 (aggressive, for initial bootstrapping). This multiplier scales all per-domain token bucket rates: `effective_rate = base_rate × multiplier`. When `multiplier > 1.0`, per-domain rate limits still apply — we don't violate politeness for any individual site. We go faster by crawling more domains in parallel, not faster per-domain. The multiplier is stored in Redis: `SET crawler:rate_multiplier 1.5`. All workers poll this value every 30 seconds and apply it to new token bucket configurations. This enables ops to reduce crawl rate during incidents (to reduce outbound bandwidth costs) or increase it during a bootstrapping phase.

---

## 12. References & Further Reading

1. **"Crawling the Web"** — Sergey Brin and Lawrence Page, *The Anatomy of a Large-Scale Hypertextual Web Search Engine*, Proceedings of WWW7, 1998. The original Google paper describing the architecture of a large-scale web crawler.

2. **"Mercator: A Scalable, Extensible Web Crawler"** — Heydon, A. and Najork, M., *World Wide Web* journal, 1999. The foundational paper describing the two-level frontier design (back queues + front queues) that this design is based on.

3. **"Efficient Crawling Through URL Ordering"** — Cho, J. and Garcia-Molina, H., *Proceedings of WWW7*, 1998. Describes URL prioritization strategies and recrawl frequency estimation.

4. **"Detecting Near-Duplicates for Web Crawling"** — Manku, G.S., Jain, A., and Sarma, A.D., *Proceedings of WWW 2007*. The Google paper describing SimHash for web-scale near-duplicate detection.

5. **"SimHash: Hash Functions for Near-Duplicate Detection"** — Charikar, M.S., *Proceedings of STOC 2002*. Original SimHash paper.

6. **Robots Exclusion Protocol** — IETF RFC 9309. https://www.rfc-editor.org/rfc/rfc9309 — The formal standard for robots.txt, published 2022.

7. **"Common Crawl Architecture"** — Common Crawl Foundation. https://commoncrawl.org/the-data/get-started/ — Describes the architecture of the largest public web crawl (petabyte scale).

8. **Apache Nutch Documentation** — Apache Software Foundation. https://nutch.apache.org/apidocs/apidocs-2.0/ — Open-source web crawler implementation; excellent reference for URL frontier and plugin architecture.

9. **"Optimizing Search Crawlers"** — Google Search Central Documentation. https://developers.google.com/search/docs/crawling-indexing/overview-google-crawlers — Official documentation on how Googlebot behaves; authoritative reference for politeness expectations.

10. **"Approximate Nearest Neighbor Methods in Information Retrieval"** — Indyk, P. and Motwani, R., *Proceedings of STOC 1998*. Locality-sensitive hashing foundations underlying SimHash table decomposition for near-duplicate search.
