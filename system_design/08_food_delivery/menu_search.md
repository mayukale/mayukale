# System Design: Menu & Restaurant Search

---

## 1. Requirement Clarifications

### Functional Requirements

- **Restaurant discovery**: consumers can search for restaurants near a given location, filtered by cuisine type, dietary options, price tier, minimum rating, maximum delivery fee, and open/closed status
- **Menu item search**: consumers can search for specific dishes (e.g., "tacos", "pad thai", "gluten-free burger") and receive results ranked by relevance across all nearby restaurants
- **Dietary filter support**: tag-based filtering for common dietary attributes (vegan, vegetarian, gluten-free, halal, kosher, nut-free); filters applied at both restaurant and item levels
- **Relevance ranking**: results ranked by a blend of text relevance, distance, restaurant rating, review count, delivery time, and personalized signals (past orders, dietary preferences)
- **Real-time menu updates**: when a merchant marks an item as 86'd (unavailable), or adds a new item, or pauses the restaurant, search results and menu pages reflect the change within 30 seconds
- **Autocomplete / typeahead**: as a consumer types in the search box, return suggestions (restaurant names, dish names, cuisine types) with latency < 100 ms
- **Geographic filtering**: all queries are anchored to a consumer's delivery address; results outside the configured delivery radius for each restaurant are excluded
- **Sorting options**: sort by relevance, rating, estimated delivery time, delivery fee, number of reviews

### Non-Functional Requirements

- **Search latency**: p99 ≤ 200 ms for restaurant list queries; p99 ≤ 150 ms for autocomplete suggestions
- **Index freshness**: menu availability changes reflected in search within 30 seconds of merchant action
- **Availability**: 99.99% (search is read path; even if write pipeline is degraded, stale index serves queries)
- **Scalability**: sustain 8 500 search requests/second at peak (calculated in Section 2)
- **Relevance quality**: measured by click-through rate (CTR) and order conversion; p-NDCG@10 ≥ 0.7 in offline evaluation
- **Consistency**: eventual consistency is acceptable; a 30-second window of stale results is tolerable
- **Security**: search results must not leak confidential merchant pricing or unpublished items; consumer location data not persisted beyond the search session

### Out of Scope

- ML model training pipeline and A/B testing framework (referenced but not designed)
- Full-text search within user reviews (separate moderation system)
- Image-based search ("show me restaurants with dishes that look like this photo")
- Voice search (relies on ASR layer upstream)
- Multi-language / multi-locale full design (noted where locale affects schema)

---

## 2. Users & Scale

### User Types

| Actor              | Description                                               |
|--------------------|-----------------------------------------------------------|
| Consumer           | Issues search queries from mobile or web apps             |
| Merchant           | Triggers index updates via menu management actions        |
| Internal ML System | Reads search logs to train ranking models                 |
| Internal Ops       | Runs bulk catalog migrations (e.g., new city onboarding)  |

### Traffic Estimates

**Assumptions**
- 37 M MAU consumers; each session involves ~5 search-related interactions (restaurant list, autocomplete, item search, filters applied, menu view)
- Average 3 sessions per month per active consumer
- Peak-to-average multiplier: 3× during dinner hours
- Separate from order placement traffic; search traffic is higher because many browse without ordering

| Metric                              | Calculation                                                                    | Result               |
|-------------------------------------|--------------------------------------------------------------------------------|----------------------|
| Search interactions per month       | 37 M users × 3 sessions/month × 5 interactions/session                        | 555 M/month          |
| Average search requests per second  | 555 M / (30 × 86 400)                                                         | ~214 req/s           |
| Peak search requests per second     | 214 × 3 (peak multiplier) × 1.33 (additional typeahead factor)                | ~855 req/s           |
| Autocomplete requests per second    | Assume 3 keystrokes per search × 855 search req/s                             | ~2 565 autocomplete/s|
| Total peak query load               | 855 + 2 565 + 1 500 (menu page loads) + 3 580 (browse/filter)                | ~8 500 req/s (est.)  |
| Index write rate (menu updates)     | 700 K merchants × avg 10 item changes/day / 86 400                            | ~81 writes/s avg     |
| Peak index write rate               | Assume morning menu prep: 10× burst                                           | ~810 writes/s peak   |
| Elasticsearch index documents       | 700 K restaurants + 70 M menu items (100 items/restaurant avg)                | ~70.7 M documents    |

### Latency Requirements

| Operation                        | p50 target  | p99 target   | Notes                                             |
|----------------------------------|-------------|--------------|---------------------------------------------------|
| Restaurant list query            | 30 ms       | 200 ms       | Includes geo-filter, ranking, cache lookup        |
| Menu item search                 | 40 ms       | 200 ms       | Full-text across 70 M documents                  |
| Autocomplete suggestion          | 20 ms       | 100 ms       | Completion index; separate index from main search |
| Menu page load (single restaurant)| 15 ms      | 80 ms        | Usually served from Redis/CDN cache               |
| Index update propagation         | —           | 30 s         | Merchant marks item unavailable; reflected in search |

### Storage Estimates

| Data                            | Calculation                                                            | Result       |
|---------------------------------|------------------------------------------------------------------------|--------------|
| Restaurant index documents      | 700 K × 2 KB per document (fields + vectors)                          | ~1.4 GB      |
| Menu item index documents       | 70 M × 1 KB per document                                              | ~70 GB       |
| Autocomplete completion index   | ~5 M unique terms × 200 bytes                                         | ~1 GB        |
| Search event logs (30 days)     | 8 500 req/s × 86 400 s/day × 30 days × 500 bytes/event               | ~11 TB       |
| Personalization feature vectors | 37 M consumers × 256-dim float32 vector = 37 M × 1 KB                | ~37 GB       |
| Elasticsearch inverted index overhead | Typically 3–5× raw document size                                | ~360 GB total|
| CDN cached responses            | ~50 K unique geo-radius + filter combinations × 20 KB                 | ~1 GB        |

### Bandwidth Estimates

| Flow                              | Calculation                                                           | Result         |
|-----------------------------------|-----------------------------------------------------------------------|----------------|
| Search API response egress        | 8 500 req/s × 15 KB avg response (10 results, thumbnails excluded)   | 127 MB/s       |
| Menu page API egress              | 1 500 req/s × 20 KB avg full menu                                    | 30 MB/s        |
| Menu image CDN egress             | 2 000 image loads/s × 80 KB avg (thumbnail)                          | 160 MB/s       |
| Index update write ingress        | 810 writes/s × 2 KB per document                                      | 1.6 MB/s       |
| Total estimated peak egress       | —                                                                     | ~320 MB/s      |

---

## 3. High-Level Architecture

```
  ┌────────────────────────────────────────────────────────────────────────────┐
  │                              CLIENTS                                       │
  │  Consumer App (iOS/Android/Web) — search bar, filter panel, browse page   │
  └───────────────────────────────┬────────────────────────────────────────────┘
                                  │ HTTPS
                     ┌────────────▼──────────────┐
                     │    API Gateway / CDN Edge   │
                     │  (Auth, Rate Limit, Cache)  │
                     └────────────┬───────────────┘
                                  │
         ┌────────────────────────┼──────────────────────┐
         │                        │                      │
┌────────▼──────────┐  ┌──────────▼──────────┐  ┌───────▼───────────────┐
│   Search Service  │  │ Autocomplete Service │  │   Menu Fetch Service  │
│  (query parsing,  │  │  (prefix-tree /      │  │  (single restaurant   │
│   geo-filter,     │  │   completion index,  │  │   menu, cached)       │
│   ranking, blend) │  │   < 100 ms target)   │  └───────────────────────┘
└────────┬──────────┘  └──────────────────────┘
         │
         │  query
         ▼
┌────────────────────────────────────────────────────────────────────────┐
│                     Elasticsearch Cluster                              │
│  ┌──────────────────────────────┐  ┌───────────────────────────────┐  │
│  │  restaurant_index            │  │  menu_item_index              │  │
│  │  (name, cuisine, rating,     │  │  (item_name, description,     │  │
│  │   geo_point, delivery_fee,   │  │   dietary_flags, restaurant,  │  │
│  │   is_active, is_paused,      │  │   price, is_available)        │  │
│  │   prep_time, review_count)   │  └───────────────────────────────┘  │
│  └──────────────────────────────┘                                     │
│  ┌──────────────────────────────┐                                     │
│  │  autocomplete_index          │                                     │
│  │  (suggest field, weight)     │                                     │
│  └──────────────────────────────┘                                     │
└──────────────────────────────────────────────────────────────────────┘
         ▲
         │ index writes (near-realtime)
         │
┌────────┴──────────────────────────────────────────────────────────────┐
│                      Index Update Pipeline                             │
│                                                                        │
│  Merchant Action                                                       │
│  (REST API PUT)  ──► Menu Service ──► Kafka (menu-change-events) ──►  │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │  Search Indexer (Kafka Consumer)                                  │ │
│  │  - Consumes menu-change-events                                    │ │
│  │  - Transforms to Elasticsearch document format                   │ │
│  │  - Issues partial update (_update API) to ES                     │ │
│  │  - Publishes cache-invalidation event                            │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                                                        │
│  Scheduled Full Re-index (nightly, offline)                           │
│  (reads from PostgreSQL source of truth, rebuilds all indices)        │
└───────────────────────────────────────────────────────────────────────┘

┌──────────────────────┐  ┌────────────────────────────────────────────┐
│   Redis Cache        │  │   PostgreSQL (Source of Truth)             │
│  (menu pages,        │  │  (merchants, menu_items, categories,       │
│   popular restaurant │  │   dietary_flags — canonical data)          │
│   list by geo-tile,  │  └────────────────────────────────────────────┘
│   session/user prefs)│
└──────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│  Personalization Service                                              │
│  (user preference vectors, past order history, dietary prefs)        │
│  Called async; result blended into ranking score by Search Service   │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│  CDN (CloudFront / Cloudflare)                                        │
│  - Caches restaurant list pages by (geo_tile, filters_hash)          │
│  - Caches full menu pages per restaurant_id                          │
│  - Purged by cache-invalidation events from Search Indexer           │
└──────────────────────────────────────────────────────────────────────┘
```

**Component roles:**

- **API Gateway**: Validates consumer JWT, enforces rate limits (100 search/min per consumer), routes to Search Service or Autocomplete Service.
- **Search Service**: Accepts structured query parameters (location, filters, sort, query text). Builds an Elasticsearch query with geo-distance filter, boolean filters for dietary/availability, text match, and function_score for ranking. Fetches personalization vector from Personalization Service (async, non-blocking with 20 ms deadline). Merges and returns paginated results.
- **Autocomplete Service**: Maintains a dedicated ES completion suggester index (edge n-gram tokenizer). Returns top-5 suggestions in <100 ms for partial queries. Backed by a separate small index for speed isolation.
- **Menu Fetch Service**: Returns the full menu for a single restaurant. Nearly always served from Redis cache (TTL 60 s) or CDN. Populates cache on cache miss from PostgreSQL.
- **Search Indexer**: Kafka consumer that transforms menu/restaurant change events into Elasticsearch partial updates. Handles retries, deduplication, and ordering. Partitioned by `merchant_id` to ensure ordered updates per restaurant.
- **Personalization Service**: Given a `consumer_id`, returns a relevance boost vector for cuisine types and dietary preferences inferred from order history. Served from Redis. Updated nightly by an offline ML pipeline.

**Primary use-case data flow (consumer searches "vegan tacos near me"):**

1. Consumer's app sends `GET /v1/search/restaurants?lat=37.77&lng=-122.41&q=vegan+tacos&dietary=vegan`.
2. API Gateway validates JWT and routes to Search Service.
3. Search Service: parses `q` into tokens; identifies dietary filter `vegan`.
4. Fetches consumer personalization vector from Redis (or returns neutral vector on miss).
5. Builds ES query: `bool` with `must=match(name/cuisine/items.name, "tacos")`, `filter=[geo_distance(5km), term(is_active, true), term(is_paused, false), term(dietary_flags, vegan)]`, wrapped in `function_score` with custom scoring (see Section 6.1).
6. ES query executes in ~20–40 ms across the cluster.
7. Search Service re-ranks top 50 ES results using the personalization vector (linear blend).
8. Returns top 20 results with pagination cursor. Consumer app renders list.
9. Consumer taps a restaurant → `GET /v1/restaurants/{id}/menu` → Redis cache hit in <5 ms.

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────────
-- MERCHANTS (source of truth, in PostgreSQL)
-- (duplicated from DoorDash.md for completeness)
-- ─────────────────────────────────────────────
CREATE TABLE merchants (
    merchant_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    name            TEXT NOT NULL,
    slug            TEXT UNIQUE NOT NULL,
    description     TEXT,
    cuisine_types   TEXT[],
    lat             DOUBLE PRECISION NOT NULL,
    lng             DOUBLE PRECISION NOT NULL,
    rating          NUMERIC(3,2),
    review_count    INT NOT NULL DEFAULT 0,
    price_tier      SMALLINT CHECK (price_tier BETWEEN 1 AND 4), -- $ $$ $$$ $$$$
    is_active       BOOLEAN NOT NULL DEFAULT true,
    is_paused       BOOLEAN NOT NULL DEFAULT false,
    prep_time_min   INT NOT NULL DEFAULT 15,
    min_order_cents INT NOT NULL DEFAULT 0,
    delivery_radius_km NUMERIC(5,2) NOT NULL DEFAULT 5.0,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE menu_items (
    item_id             UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    merchant_id         UUID REFERENCES merchants(merchant_id),
    category_id         UUID REFERENCES menu_categories(category_id),
    name                TEXT NOT NULL,
    description         TEXT,
    price_cents         INT NOT NULL,
    image_url           TEXT,
    is_available        BOOLEAN NOT NULL DEFAULT true,
    dietary_flags       TEXT[],   -- normalized tags
    calorie_count       INT,
    popularity_score    NUMERIC(8,4) NOT NULL DEFAULT 0, -- derived from order frequency
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ─────────────────────────────────────────────
-- SEARCH EVENTS (append-only, for ML training)
-- ─────────────────────────────────────────────
CREATE TABLE search_events (
    event_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    consumer_id     UUID,           -- nullable for anonymous sessions
    session_id      UUID NOT NULL,
    query_text      TEXT,
    lat             DOUBLE PRECISION NOT NULL,
    lng             DOUBLE PRECISION NOT NULL,
    filters_applied JSONB,          -- {dietary:['vegan'], cuisine:['mexican']}
    sort_by         TEXT,
    result_count    INT,
    clicked_result  UUID,           -- merchant_id or item_id clicked
    click_position  INT,            -- rank position of clicked result
    converted       BOOLEAN,        -- resulted in an order
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);
-- Partitioned monthly; retained 90 days in hot storage, 1 year in S3

-- ─────────────────────────────────────────────
-- CONSUMER SEARCH PREFERENCES (for personalization)
-- ─────────────────────────────────────────────
CREATE TABLE consumer_search_preferences (
    consumer_id         UUID PRIMARY KEY REFERENCES users(user_id),
    cuisine_affinities  JSONB,  -- {mexican: 0.9, thai: 0.6, ...}
    dietary_preferences TEXT[], -- persistent dietary flags
    price_tier_pref     SMALLINT,
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

**Elasticsearch Document Schemas:**

```json
// restaurant_index mapping
{
  "mappings": {
    "properties": {
      "merchant_id":       { "type": "keyword" },
      "name":              { "type": "text", "analyzer": "english", "fields": { "keyword": { "type": "keyword" } } },
      "slug":              { "type": "keyword" },
      "description":       { "type": "text", "analyzer": "english" },
      "cuisine_types":     { "type": "keyword" },
      "dietary_flags":     { "type": "keyword" },
      "location":          { "type": "geo_point" },
      "delivery_radius_km":{ "type": "float" },
      "rating":            { "type": "float" },
      "review_count":      { "type": "integer" },
      "price_tier":        { "type": "byte" },
      "prep_time_min":     { "type": "integer" },
      "delivery_fee_cents":{ "type": "integer" },
      "is_active":         { "type": "boolean" },
      "is_paused":         { "type": "boolean" },
      "open_now":          { "type": "boolean" },
      "popularity_score":  { "type": "float" },
      "suggest": {
        "type": "completion",
        "analyzer": "simple",
        "preserve_separators": true,
        "preserve_position_increments": true,
        "max_input_length": 50
      }
    }
  },
  "settings": {
    "number_of_shards": 5,
    "number_of_replicas": 1,
    "analysis": {
      "analyzer": {
        "english": {
          "type": "standard",
          "stopwords": "_english_"
        }
      }
    }
  }
}

// menu_item_index mapping
{
  "mappings": {
    "properties": {
      "item_id":           { "type": "keyword" },
      "merchant_id":       { "type": "keyword" },
      "merchant_name":     { "type": "text", "fields": { "keyword": { "type": "keyword" } } },
      "merchant_rating":   { "type": "float" },
      "merchant_location": { "type": "geo_point" },
      "merchant_is_active":{ "type": "boolean" },
      "merchant_is_paused":{ "type": "boolean" },
      "item_name":         { "type": "text", "analyzer": "english", "fields": { "keyword": { "type": "keyword" } } },
      "description":       { "type": "text", "analyzer": "english" },
      "price_cents":       { "type": "integer" },
      "dietary_flags":     { "type": "keyword" },
      "is_available":      { "type": "boolean" },
      "calorie_count":     { "type": "integer" },
      "popularity_score":  { "type": "float" },
      "category_name":     { "type": "keyword" }
    }
  },
  "settings": {
    "number_of_shards": 10,
    "number_of_replicas": 1
  }
}
```

### Database Choice

| Database / Technology    | Pros                                                             | Cons                                                              | Role in this system              |
|--------------------------|------------------------------------------------------------------|-------------------------------------------------------------------|----------------------------------|
| Elasticsearch 8.x        | Full-text search; geo-queries; faceting; aggregations; fast reads | Not a primary store; eventual consistency; index management overhead | Primary search index              |
| PostgreSQL               | ACID; rich SQL; JSONB; PostGIS for geo                          | Full-text search is limited (no advanced relevance tuning); scaling reads is harder | Source of truth for catalog data  |
| Redis                    | Sub-ms latency; built-in TTL; Sorted Sets for leaderboards      | Memory-only; limited query capability                             | Menu page cache; autocomplete rate limit; user preference cache |
| Solr                     | Similar to ES; strong faceting                                   | Less community momentum; harder to operate vs. ES                 | Not selected                     |
| Algolia                  | Managed search; typo tolerance built-in; fast to integrate      | Per-query pricing prohibitive at 8 500 req/s (est. $50K+/month)  | Not selected at this scale        |
| Typesense                | Open-source; typo-tolerant; easier ops than ES                  | Smaller community; less mature geo-query support                  | Considered for autocomplete only  |

**Selected: Elasticsearch 8.x as the primary search engine**

Justification by specific properties:
- `geo_distance` filter in ES executes natively on the geo_point field type using a BKD tree; radius queries are O(log N) per shard and typically <10 ms for 700K documents.
- `function_score` query type enables blending BM25 text relevance with arbitrary numeric fields (rating, review_count, prep_time, delivery_fee) without a separate re-ranking step.
- `completion` suggester with edge n-gram indexing provides the sub-100 ms autocomplete path without a separate system.
- Horizontal sharding: the 70 M menu item documents across 10 shards keeps each shard at ~7 M documents, well within the 50 M/shard practical limit for search performance.
- Near real-time indexing: ES default refresh interval of 1 s (tunable to 500 ms) ensures index freshness within the 30-second SLA.

**PostgreSQL** remains the authoritative source of truth; Elasticsearch is treated as a derived, eventually-consistent projection. All writes go to PostgreSQL first, then stream to ES via Kafka.

---

## 5. API Design

```
Base URL:    https://api.doordash.com/v1
Auth:        Bearer <JWT> (anonymous search allowed for lat/lng queries with reduced personalization)
Pagination:  cursor-based (?cursor=<token>&limit=20)
Rate limits: 100 search req/min per consumer; 500 req/min per IP (anonymous)
```

### Search Endpoints

```
GET /search/restaurants
    Auth: optional (degrades personalization when absent)
    Query params:
      lat          REQUIRED float         Consumer delivery address latitude
      lng          REQUIRED float         Consumer delivery address longitude
      q            optional string        Free-text query ("tacos", "pizza place")
      cuisine      optional string[]      Filter: cuisine type(s)
      dietary      optional string[]      Filter: vegan, vegetarian, gluten-free, halal, kosher
      min_rating   optional float         Filter: minimum star rating (1.0–5.0)
      max_delivery_fee_cents optional int Filter: max delivery fee
      price_tier   optional int[]         Filter: 1=$, 2=$$, 3=$$$, 4=$$$$
      open_now     optional bool          Filter: only open restaurants (default true)
      sort_by      optional enum          relevance|rating|delivery_time|delivery_fee (default relevance)
      cursor       optional string        Pagination cursor
      limit        optional int           Results per page (default 20, max 50)
    Rate limit: 100 req/min per consumer
    Response 200:
      {
        "restaurants": [
          {
            "merchant_id": "uuid",
            "name": "string",
            "slug": "string",
            "cuisine_types": ["string"],
            "dietary_flags": ["string"],
            "rating": 4.7,
            "review_count": 1203,
            "price_tier": 2,
            "prep_time_min": 15,
            "delivery_fee_cents": 199,
            "estimated_delivery_min": 28,
            "is_open": true,
            "distance_km": 1.4,
            "thumbnail_url": "https://...",
            "highlight": "vegan • tacos • burritos"   // matched snippet
          }
        ],
        "next_cursor": "eyJvZmZzZXQiOjIwfQ==",
        "total_count": 87
      }
    Cache: CDN cache 30 s keyed on (geo_tile_h3_r7, cuisine_hash, dietary_hash, sort_by)

GET /search/items
    Auth: optional
    Query params:
      lat          REQUIRED float
      lng          REQUIRED float
      q            REQUIRED string        Item search query
      dietary      optional string[]
      max_price_cents optional int
      sort_by      optional enum          relevance|price_asc|price_desc|popularity
      cursor       optional string
      limit        optional int           Default 20, max 50
    Rate limit: 100 req/min
    Response 200:
      {
        "items": [
          {
            "item_id": "uuid",
            "item_name": "Vegan Tacos",
            "description": "string",
            "price_cents": 1199,
            "dietary_flags": ["vegan"],
            "merchant_id": "uuid",
            "merchant_name": "Taqueria El Sol",
            "merchant_rating": 4.6,
            "distance_km": 1.2,
            "is_available": true,
            "image_url": "https://..."
          }
        ],
        "next_cursor": "...",
        "total_count": 34
      }

GET /search/suggest
    Auth: none required
    Query params:
      q            REQUIRED string        Partial query (min 2 chars)
      lat          REQUIRED float
      lng          REQUIRED float
      limit        optional int           Default 5, max 10
    Rate limit: 300 req/min per consumer (autocomplete calls are more frequent)
    Response 200:
      {
        "suggestions": [
          { "text": "Vegan Tacos", "type": "item",    "merchant_name": "Taqueria El Sol" },
          { "text": "Vegan Thai",  "type": "cuisine",  "merchant_name": null },
          { "text": "Veggie Grill","type": "restaurant","merchant_name": "Veggie Grill" }
        ]
      }
    Cache: Redis 10 s keyed on (q_prefix, geo_tile_h3_r6)

GET /restaurants/{restaurant_id}/menu
    Auth: optional
    Response 200:
      {
        "merchant": { ...merchant fields... },
        "categories": [
          {
            "category_id": "uuid",
            "name": "Tacos",
            "display_order": 1,
            "items": [
              {
                "item_id": "uuid",
                "name": "Vegan Tacos",
                "description": "...",
                "price_cents": 1199,
                "image_url": "...",
                "is_available": true,
                "dietary_flags": ["vegan","gluten-free"],
                "calorie_count": 420,
                "modifiers": [ ... ]
              }
            ]
          }
        ]
      }
    Cache: CDN 60 s; Redis 60 s; Surrogate-Key: merchant:{id}

GET /search/filters/options
    Auth: none
    Response: { "cuisines": ["mexican","thai",...], "dietary_flags": ["vegan",...], "price_tiers": [1,2,3,4] }
    Cache: CDN 1 hour (static reference data)
```

### Internal / Merchant Write Endpoints (triggering index updates)

```
PUT  /internal/search/index/restaurant/{merchant_id}
     Called by: Menu Service after any merchant profile change
     Body: full restaurant document
     Response: 202 Accepted (async index update via Kafka)

PUT  /internal/search/index/item/{item_id}
     Called by: Menu Service after any item change
     Body: full item document
     Response: 202 Accepted

DELETE /internal/search/index/item/{item_id}
     Called by: Menu Service when item is permanently removed
     Response: 202 Accepted

POST /internal/search/reindex/merchant/{merchant_id}
     Called by: Ops tooling (full restaurant rebuild)
     Response: 202 Accepted; returns job_id for progress polling
```

---

## 6. Deep Dive: Core Components

### 6.1 Relevance Ranking

**Problem it solves:**
A search for "tacos near me" might match hundreds of restaurants. The order in which they appear determines CTR and ultimately order conversion. A naive distance-only ranking surfaces far restaurants with better names. A naive rating-only ranking surfaces high-rated restaurants the consumer has never heard of. The ranking function must blend text relevance, location, quality signals, business objectives, and personalization to maximize p-NDCG@10 (the probability that the top 10 results are what the consumer would have ordered from).

**Approaches comparison:**

| Approach                                  | Description                                                        | Pros                                                       | Cons                                                              |
|-------------------------------------------|--------------------------------------------------------------------|------------------------------------------------------------|-------------------------------------------------------------------|
| BM25 text score only                      | Elasticsearch default relevance                                    | No config needed; handles typos via fuzziness              | Ignores distance, rating, business signals; poor UX              |
| Distance rank only                        | Sort by nearest restaurant                                         | Simple; fast                                               | A low-rated restaurant 0.5 km away beats a 4.9-star one at 1 km  |
| Weighted linear combination (manual)      | `score = w1*bm25 + w2*(1/distance) + w3*rating + w4*review_count` | Interpretable; fast; no model serving                      | Weights must be hand-tuned; no personalization; static           |
| ES `function_score` multi-signal          | ES native multi-signal blend with decay functions                  | No separate re-ranker; single query; customizable          | Still manual weights; personalization requires extra join         |
| Learning-to-Rank (LTR) with LightGBM      | Gradient-boosted model trained on click/order labels               | Best offline NDCG; learns complex interactions             | Requires feature store; model serving latency; training pipeline  |
| Two-stage: ES recall + online re-ranker   | ES returns top 100 candidates; a fast ML model re-ranks           | Best quality; ES handles recall efficiently                | Extra network hop (~5–10 ms); requires feature serving            |

**Selected: ES `function_score` with personalization re-blend (production pragmatic approach)**

Stage 1 — Elasticsearch `function_score` query:
```json
{
  "query": {
    "function_score": {
      "query": {
        "bool": {
          "must": [
            { "multi_match": {
                "query": "vegan tacos",
                "fields": ["name^3", "description^1", "cuisine_types^2"],
                "type": "best_fields",
                "fuzziness": "AUTO"
            }}
          ],
          "filter": [
            { "geo_distance": { "distance": "5km", "location": { "lat": 37.77, "lon": -122.41 } } },
            { "term": { "is_active": true } },
            { "term": { "is_paused": false } },
            { "term": { "dietary_flags": "vegan" } }
          ]
        }
      },
      "functions": [
        {
          "gauss": {
            "location": {
              "origin": { "lat": 37.77, "lon": -122.41 },
              "scale": "2km",
              "decay": 0.5
            }
          },
          "weight": 2.0
        },
        {
          "field_value_factor": {
            "field": "rating",
            "factor": 1.2,
            "modifier": "log1p",
            "missing": 3.0
          },
          "weight": 1.5
        },
        {
          "field_value_factor": {
            "field": "review_count",
            "factor": 0.01,
            "modifier": "log1p",
            "missing": 0
          },
          "weight": 0.5
        },
        {
          "field_value_factor": {
            "field": "popularity_score",
            "factor": 1.0,
            "modifier": "log1p",
            "missing": 0
          },
          "weight": 1.0
        }
      ],
      "score_mode": "sum",
      "boost_mode": "multiply"
    }
  },
  "size": 50,
  "from": 0
}
```

Stage 2 — Personalization re-blend (in Search Service, post-ES):
```
for each result in es_results[:50]:
    cuisine_affinity = consumer_affinities.get(result.cuisine_types[0], 0.5)
    dietary_match    = 1.0 if consumer.dietary_prefs ⊆ result.dietary_flags else 0.8
    final_score = 0.7 * result.es_score + 0.2 * cuisine_affinity + 0.1 * dietary_match

re-ranked = sort(es_results, key=final_score, reverse=True)[:20]
```

The personalization data comes from the Personalization Service (Redis read, <2 ms). If Redis is unavailable, stage 2 is skipped and ES scores are returned directly (degraded but not broken).

**Why this over full LTR:** This system achieves >0.7 p-NDCG@10 in offline testing at significantly lower operational complexity. LTR would add: feature store infrastructure, model versioning, online serving (Triton/TorchServe), shadow mode testing, and model drift monitoring. These are justified at Google or Amazon scale; for a team owning search on a food delivery platform, the function_score + personalization blend approach delivers 90% of the benefit at 20% of the operational cost. We plan to add LTR as a follow-on project once feature store infrastructure is shared across teams.

**Interviewer Q&As:**

Q1: How do you measure whether the ranking is actually good?
A: Three metrics: (1) p-NDCG@10: offline metric computed against held-out click/order labels in the `search_events` table. Baseline BM25-only p-NDCG@10 = 0.52; function_score = 0.68; + personalization = 0.73. (2) Online CTR: percentage of searches where a result is clicked. Measured via A/B test. (3) Search-to-order conversion rate: percentage of search sessions that result in an order within 30 minutes.

Q2: How do you tune the function_score weights?
A: Coordinate-ascent over the weight vector, minimizing offline NDCG loss against the `search_events` labeled dataset (last 7 days of consumer clicks and orders). Run in a nightly Spark job. Changed weights are deployed via a configuration feature flag (no code deploy needed). Monitored by comparing CTR in shadow mode before full rollout.

Q3: How do you handle "restaurant has no reviews yet" (cold start)?
A: The `review_count` factor uses `modifier: log1p` which handles zero gracefully (log1p(0) = 0). The `rating` field uses `missing: 3.0` (neutral) for new restaurants. New restaurants also receive a "New!" badge boost (a temporary function_score weight of +0.5 applied for the first 30 days after onboarding) to surface them to consumers who might be interested in trying them, calibrated to avoid displacing high-quality incumbents entirely.

Q4: What's the impact of including delivery fee in ranking?
A: We do not currently factor delivery fee into the `function_score` for relevance ranking — it is available as a sort option and as a filter, but not a ranking signal. Reason: boosting low-delivery-fee restaurants would disadvantage distant restaurants (who need higher fees to pay Dashers) and would create a perverse incentive for DoorDash as a marketplace (lower-fee restaurants earn us less commission). We surface delivery fee clearly on each card and let consumers apply a max_fee filter themselves.

Q5: How do you handle multilingual queries (a consumer types "pollo asado" in a primarily English index)?
A: Three mechanisms: (1) The English analyzer in ES already handles some stemming but not cross-language. (2) A synonym mapping file maps common foreign-language food terms to English equivalents: `pollo asado → grilled chicken`, `pho → vietnamese noodle soup`. This synonym file is maintained by the content team and loaded as a custom ES analysis plugin. (3) For fully non-English locales, a separate index per locale with a locale-appropriate analyzer (e.g., `french` or `cjk` for Chinese/Japanese/Korean) is created. Locale is determined from the `Accept-Language` header.

---

### 6.2 Real-Time Menu Update Pipeline

**Problem it solves:**
When a merchant marks an item as 86'd (sold out for the night), consumers currently browsing that restaurant's menu should not be able to add that item to their cart, and search results should not surface the restaurant as having that item available. The window from merchant action to search index update must be ≤ 30 seconds. With 700 K restaurants and 70 M indexed items, we cannot afford a full re-index on every change.

**Approaches comparison:**

| Approach                             | Description                                                        | Pros                                              | Cons                                                           |
|--------------------------------------|--------------------------------------------------------------------|---------------------------------------------------|----------------------------------------------------------------|
| Polling (indexer reads DB every N s) | Indexer polls `menu_items WHERE updated_at > last_poll`           | Simple; no additional infrastructure              | Minimum latency = poll interval; high DB read load             |
| Synchronous ES update in Menu API    | Menu API writes to both PostgreSQL and ES in the same request     | Simple code path; immediate update                | ES write failure blocks merchant API response; tight coupling  |
| Async via Kafka + Indexer consumer   | Menu API writes to PG + publishes to Kafka; Indexer consumes & updates ES | Decoupled; reliable; at-least-once delivery | Eventual consistency; ~1–5 s Kafka lag                        |
| CDC via Debezium (WAL-based)         | Debezium reads PostgreSQL WAL; streams every row change to Kafka  | No application code change; captures all changes | Operational complexity; WAL retention requirements             |
| Dual-write + change data capture hybrid | Application writes to Kafka explicitly; CDC as a safety net    | Explicit control; CDC catches missed writes       | Complexity; possible duplicate processing                      |

**Selected: Async via Kafka + Indexer consumer**

This approach provides the best balance of simplicity, reliability, and freshness:

1. **Merchant action**: Merchant taps "mark item unavailable" in their tablet app. App calls `PUT /merchant/menu/items/{item_id}/availability` with `{ "is_available": false }`.

2. **Menu Service**: Writes `UPDATE menu_items SET is_available=false, updated_at=now() WHERE item_id=$id` to PostgreSQL. Then publishes to Kafka topic `menu-change-events`:
   ```json
   {
     "event_type": "item_availability_changed",
     "item_id": "uuid",
     "merchant_id": "uuid",
     "is_available": false,
     "updated_at": "2024-11-15T18:32:00Z",
     "event_id": "uuid"
   }
   ```
   The Kafka publish happens **after** the PostgreSQL commit. If Kafka is unavailable, the write still succeeds to PostgreSQL (the source of truth), and the indexer catches up via a scheduled reconciliation job.

3. **Search Indexer** (Kafka consumer group, partitioned by `merchant_id`):
   - Deserializes the event.
   - Checks Redis deduplication key `indexed_event:{event_id}` (10-minute TTL). If seen, skips.
   - Issues an Elasticsearch partial update:
     ```
     POST /menu_item_index/_update/{item_id}
     { "doc": { "is_available": false } }
     ```
   - Simultaneously publishes a cache invalidation event: `PUBLISH menu-cache-invalidation merchant:{merchant_id}`.

4. **Cache invalidation**: Redis subscriber on the Menu Fetch Service busts the Redis key `menu:{merchant_id}`. The CDN cache is purged via a Surrogate-Key `PURGE merchant:{merchant_id}` HTTP call to CloudFront/Cloudflare. CDN purge propagates to all edge nodes within ~5 seconds globally.

5. **Elasticsearch refresh**: ES index refresh interval is set to 1 second on both indices. After the partial update lands, the next refresh makes it visible to search queries. Total end-to-end latency: Kafka lag (< 1 s) + ES partial update (< 100 ms) + refresh interval (1 s) = ~2–3 s in the happy path. Well within the 30-second SLA.

6. **Reconciliation job**: Runs every 5 minutes. Queries PostgreSQL for all items `WHERE updated_at > (now() - 5 minutes)` and issues idempotent ES upserts. This catches any events that were dropped by the Kafka pipeline (e.g., Kafka broker restart). This is a safety net, not the primary path.

**Implementation detail — handling restaurant pause:**
```
PUT /merchant/pause { "paused": true }
  → Menu Service updates merchants.is_paused = true
  → Publishes "restaurant_paused" event
  → Indexer updates restaurant_index doc: is_paused=true
  → CDN purge for restaurant list pages in all geo tiles where this restaurant appears
```
Because the restaurant can appear in search results for many geo-tile cache keys, the CDN purge uses tag-based invalidation with `Surrogate-Key: merchant:{id}` applied to all cached responses that include the restaurant. This ensures no consumer sees a paused restaurant in their search results after the 5-second CDN purge propagation.

**Interviewer Q&As:**

Q1: What happens if the Kafka consumer (Search Indexer) is down for 2 hours?
A: The Kafka partition stores messages until consumed. Consumer lag accumulates (at 81 writes/s average × 7 200 s = ~583 K messages). When the consumer restarts, it replays from its last committed offset. Replaying 583 K ES partial updates at maximum throughput (ES can handle ~10 K updates/s per node) would take ~58 seconds on a 3-node cluster — fully within the 30-second SLA after recovery. The reconciliation job also runs every 5 minutes and catches any gap.

Q2: Can the Kafka publish and PostgreSQL write ever get out of sync?
A: Yes. If the application crashes between the PostgreSQL commit and the Kafka publish, the PostgreSQL write is durable but Kafka never receives the event. This is the "outbox problem." Our 5-minute reconciliation job is the mitigation. A stronger mitigation would be the Transactional Outbox pattern (write to an `outbox` table in the same PG transaction; Debezium reads the WAL and publishes to Kafka), which eliminates this gap entirely at the cost of CDC infrastructure complexity. We plan to migrate to CDC once Debezium is deployed for order events (shared infrastructure).

Q3: How do you handle the case where a menu item is rapidly toggled (available → unavailable → available) by a stressed merchant during a rush?
A: Kafka messages for the same `item_id` arrive in order because they are partitioned by `merchant_id` (ensuring FIFO within a restaurant). The Indexer processes them in order and applies the last state. However, if two updates are ingested in the same ES bulk request, the second overwrites the first correctly. Rapid toggling is rate-limited at the merchant API layer (10 availability changes per item per minute) to prevent accidental spam.

Q4: Why partial update (ES `_update` API) rather than a full document replace?
A: A full replace requires the Indexer to first fetch the current document from PostgreSQL (to populate all fields), then push the full document. This doubles the PostgreSQL read load and increases Indexer complexity. ES `_update` with a `doc` patch is safer, faster, and reduces the window where a stale version of unrelated fields (like rating) could overwrite a recent update from a different code path. The risk of partial update is a corrupt document if the field set diverges from the mapping — mitigated by schema validation in the Indexer before issuing the update.

Q5: How would you scale the Search Indexer to handle a burst of 10 000 menu changes per second (e.g., end-of-day automatic 86'ing)?
A: Scale the Kafka consumer group horizontally. Number of consumers ≤ number of partitions (we use 50 partitions for `menu-change-events` partitioned by `merchant_id`). Each consumer issues ES bulk updates (batch of 100 operations per request, reducing API call overhead). 50 consumers × 100 ops/request × 10 requests/s = 50 000 ops/s throughput, well above the 10 000 ops/s requirement. ES itself can handle this via horizontal shard distribution across nodes.

---

### 6.3 Autocomplete / Typeahead

**Problem it solves:**
As a consumer types "veg" into the search bar, they should see suggestions like "Vegan Tacos", "Vegetarian Thai", "Veggie Grill" within 100 ms. Slow autocomplete creates a perception of a laggy app and reduces search engagement. The challenge is achieving sub-100 ms latency at scale (2 565 requests/s) while returning personalized, geo-relevant, and contextually appropriate suggestions.

**Approaches comparison:**

| Approach                            | Description                                                      | Pros                                             | Cons                                                         |
|-------------------------------------|------------------------------------------------------------------|--------------------------------------------------|--------------------------------------------------------------|
| ES Completion Suggester             | Dedicated `completion` field with weighted suggestions           | Native to ES; very fast (<10 ms); FST-based      | No full-text fuzzy matching; geo-filtering limited           |
| ES Edge N-gram Index                | Index terms as n-grams (3-gram, 4-gram, 5-gram)                 | Supports partial match anywhere in string        | Larger index size; more complex mapping                      |
| Trie in Redis                       | In-memory prefix tree on top terms                              | Extremely fast; sub-ms                           | Limited to top-N terms; no geo-filter; complex geo ranking  |
| Dedicated Autocomplete Service (e.g. Typesense) | Separate system with built-in typo tolerance     | Excellent typo handling; simple API              | Another system to operate; ~20 ms latency; geo-rank complex |
| Precomputed top-K per prefix        | For each prefix up to length 5, precompute top-5 results in Redis| Sub-ms; deterministic                           | Combinatorial explosion (26^5 = ~11M prefixes); not geo-aware|

**Selected: ES Completion Suggester with a Redis prefix cache for the top-1000 most common queries**

**Hybrid implementation:**

1. **ES Completion Suggester** is the primary path. The `autocomplete_index` has a `suggest` field of type `completion`. Each suggestion has a `weight` (based on historical search frequency and order conversion rate) and an optional `contexts` parameter for geo-filtering (ES geo-context suggester narrows completions to suggestions associated with the consumer's H3 cell at resolution 6).

   ```json
   // Suggestion document inserted during merchant onboarding or item creation
   {
     "input": ["Veggie Grill", "Veggie", "veg"],
     "weight": 450,
     "contexts": { "geo": [{ "lat": 37.77, "lon": -122.41, "precision": 6 }] }
   }
   ```

   Query:
   ```
   POST /autocomplete_index/_search
   {
     "suggest": {
       "restaurant-suggest": {
         "prefix": "veg",
         "completion": {
           "field": "suggest",
           "size": 5,
           "contexts": { "geo": [{ "lat": 37.77, "lon": -122.41, "precision": 6 }] }
         }
       }
     }
   }
   ```
   Latency: 5–15 ms from ES.

2. **Redis prefix cache** for the global top-1000 queries (by frequency in `search_events`, computed nightly). For these common prefixes (e.g., "pi", "ta", "chi"), results are served directly from Redis ZSET, bypassing ES entirely. Redis lookup: <1 ms. Cache is populated by the nightly popularity job and has a 24-hour TTL with background refresh.

3. **Debouncing**: The client sends autocomplete requests only after 150 ms of idle typing (debounce). This reduces the 2 565 req/s estimate to ~800 req/s in practice.

4. **Geo-cell caching**: Autocomplete results are relatively stable within an H3 resolution-6 cell (~36 km² area). Suggestions are cached in Redis as `suggest:{h3_cell_r6}:{prefix}` with a 60-second TTL, reducing ES query load by ~80%.

**End-to-end latency budget:**
- Redis prefix cache hit: 1 ms round-trip from the Autocomplete Service to Redis + <1 ms Redis operation = ~3 ms total
- Redis geo-cell cache hit: ~3 ms
- ES Completion Suggester (cache miss): ~15 ms ES + ~3 ms network = ~18 ms
- Total p99 including Autocomplete Service processing and API Gateway: ~30 ms cache hit / ~80 ms ES path
- p99 target of 100 ms is met comfortably.

**Interviewer Q&As:**

Q1: How do you handle typos in autocomplete? "pizzza" should still suggest "pizza."
A: The ES Completion Suggester does not support fuzzy matching natively (it's FST-based prefix matching). For typo tolerance, we run a parallel query on the `restaurant_index` using `multi_match` with `fuzziness: AUTO` at a higher timeout tolerance (but this is the slower full-text path, not completion). In practice, we use a lightweight client-side edit-distance check: if the completion suggester returns 0 results, the client triggers a full search query instead. We also publish common misspellings as additional `input` entries in the completion document (e.g., "pizza" completion doc also has "pizzza", "piiza" as input variants — a curated list from search logs where high-volume zero-result queries occur).

Q2: How do you ensure autocomplete results don't suggest paused or closed restaurants?
A: The completion document in the autocomplete index has a `contexts` field that includes `available: true/false`. When a restaurant is paused, the Indexer updates its completion document's context to `available: false`. The autocomplete query includes `contexts: { available: [true] }`, filtering out unavailable restaurants at the ES level. This update goes through the same Kafka pipeline as menu changes with a sub-30-second SLA.

Q3: How do you rank autocomplete suggestions? Why is "Veggie Grill" shown before "Vegan Thai Kitchen"?
A: The `weight` field in each completion document is updated nightly from the `search_events` table:
```sql
SELECT clicked_result, COUNT(*) as click_count
FROM search_events
WHERE occurred_at > now() - interval '7 days'
  AND clicked_result IS NOT NULL
GROUP BY clicked_result
ORDER BY click_count DESC
```
The weight is proportional to the 7-day rolling click count for that restaurant/item. A restaurant with 500 clicks/week gets weight 500; one with 50 gets weight 50. The completion suggester naturally returns higher-weight suggestions first for the same prefix.

Q4: How would you scale autocomplete to 10× the current load (25 000 req/s)?
A: Current path: 800 effective req/s (after debouncing and cache hits). At 10×, we'd have ~8 000 effective req/s after cache effects. Mitigation path: (1) Increase Redis geo-cell cache size and TTL (reduces ES load). (2) Scale Autocomplete Service horizontally — it's stateless. (3) Add a CDN edge cache for the most common prefix+geo-tile combinations (e.g., "pi" + SF downtown cell returns the same results to 1 000s of concurrent users — one CDN edge response serves all). (4) Scale ES autocomplete_index shard count from 3 to 9 shards across more nodes. With these measures, 25 000 req/s is feasible.

Q5: How would you personalize autocomplete results (show the consumer's past-ordered restaurants first)?
A: After fetching the top-10 ES/Redis completions for the prefix, intersect with the consumer's `recent_restaurants` set (stored in Redis, populated from order history, TTL 30 days). Restaurants the consumer has ordered from in the last 30 days get a +200 weight boost applied in-memory by the Autocomplete Service before returning the top 5 to the client. This computation adds <1 ms and requires no extra network call since consumer profile data is already in the per-request auth context.

---

## 7. Scaling

### Horizontal Scaling

**Search Service**: Stateless; Kubernetes pods auto-scaled on CPU (target 60%). At 8 500 req/s with p99 200 ms processing: 8 500 × 0.2 s = 1 700 concurrent requests. At 100 concurrent req/pod: ~17 pods. Maintain 25 pods at peak for headroom.

**Autocomplete Service**: Stateless; lighter weight. 10 pods handle 8 000 req/s with sub-100 ms processing comfortably.

**Elasticsearch**: Scale by adding data nodes. 70 M menu item documents at ~1 KB each = ~70 GB of index data (plus 3–5× overhead for inverted index). A 10-node cluster with 200 GB SSD per node provides ~2 TB of usable index storage, well above requirements. Search throughput scales with the number of nodes since each shard processes queries in parallel.

### Elasticsearch Index Sharding Strategy

- `restaurant_index`: 5 primary shards, 1 replica each → 10 shards total across 10 nodes = 1 shard per node. Each primary shard holds ~140 K restaurants (700 K / 5). This ensures geo-distance queries fan out to 5 shards in parallel, not 1.
- `menu_item_index`: 10 primary shards, 1 replica each → 20 shards total. Each primary shard holds ~7 M items (70 M / 10). Shard routing: route by `merchant_id` (custom routing in ES) so all items for a restaurant land on the same shard — reduces fan-out for single-restaurant menu queries from 10 to 1.
- `autocomplete_index`: 3 primary shards, 1 replica. Small index (~1 GB); few shards minimize cross-shard completion merge overhead.

### Replication

- Elasticsearch: 1 replica per primary shard provides read scalability (searches can be served by replica) and fault tolerance (1-node failure is survived without data loss). Replicas are not co-located with their primary (ES default behavior).
- PostgreSQL: Aurora Multi-AZ writer + 3 read replicas. Menu Fetch Service reads from read replicas.
- Redis: 1 replica per master node in Redis Cluster.

### Caching Strategy

| Data                        | Layer        | TTL      | Invalidation                                          |
|-----------------------------|--------------|----------|-------------------------------------------------------|
| Menu page (full menu)       | Redis        | 60 s     | `menu-cache-invalidation` Kafka event                |
| Menu page (full menu)       | CDN          | 60 s     | Surrogate-Key purge on item/restaurant change         |
| Restaurant list by geo-tile | CDN          | 30 s     | Purge on restaurant status/pause change               |
| Autocomplete by prefix+geo  | Redis        | 60 s     | Nightly refresh for top-1000; TTL expiry for long-tail |
| Personalization vectors     | Redis        | 24 hr    | Nightly ML pipeline refresh                          |
| Filter options (cuisines)   | CDN          | 1 hr     | Deploy-time invalidation                             |

### CDN Strategy

- Restaurant list pages are parameterized by geo-tile (H3 resolution 7 + adjacent cells), filter hash, and sort order. Cache key includes these parameters. A 30-second TTL means at most 30 s of stale restaurant status data in search results — acceptable per NFRs.
- Menu pages are cached per `restaurant_id`. Surrogate-Key purge ensures cache is invalidated within 5 s of any item change.
- Autocomplete responses are NOT cached at CDN level due to high cardinality of (prefix, geo) combinations. Redis handles this caching at the Autocomplete Service layer.

**Interviewer Q&As:**

Q1: How do you handle Elasticsearch cluster rebalancing during peak hours?
A: Rebalancing (shard relocation) is disabled during configured maintenance blackout windows (Friday 5 PM to Sunday midnight). Node additions and removals are scheduled for Tuesday/Wednesday 2–5 AM. ES cluster settings: `cluster.routing.rebalance.enable: none` is set via API before any maintenance operation during peak periods and re-enabled after.

Q2: What happens if an Elasticsearch node goes down during a search query?
A: If the downed node held a primary shard, ES automatically promotes the replica to primary in ~1 second (controlled by `index.unassigned.node_left.delayed_timeout`). Queries served during the 1-second window that were routing to the downed shard return partial results (ES returns what it has, with a `_shards.failed` count in the response). The Search Service handles partial results gracefully: if `_shards.failed > 0`, it logs a warning and returns results from healthy shards. The consumer sees slightly fewer results rather than an error.

Q3: How does search scale differently in a new city launch vs. a mature market?
A: In a new city, the merchant catalog is small (100–500 restaurants). A single Elasticsearch node easily handles this. The architecture automatically scales because queries fan out across fewer shards (less data, faster responses). The challenge in new city launches is cold-start: no search event history means no popularity weights. We initialize popularity scores from an offline dataset of national restaurant chain popularity and seed local weights from Yelp/Google Places public data (via API partnership) until platform-specific data accumulates.

Q4: How would you handle a "zero results" search query gracefully?
A: If the geo-filtered search returns 0 results: (1) Retry with expanded radius (5 km → 10 km → 20 km) and surface a "Showing restaurants up to 10 km away" notice. (2) If still zero: fall back to dietary filter relaxation — remove the strictest dietary filter, display a "We couldn't find vegan options, but here are nearby restaurants" message. (3) Log the zero-result query in `search_events` with `result_count=0`. These are reviewed weekly to identify missing cuisine coverage or spelling issues to address in the synonym file.

Q5: How would you implement search result personalization at higher fidelity than the current approach?
A: The next evolution is a two-stage LTR system: (1) ES retrieves the top 200 candidates using the current function_score query. (2) A LightGBM model (trained on `search_events` clicks/orders as labels and ~50 features: consumer order history embedding, current time/day, weather, past interactions with each restaurant) re-ranks the top 200 to return the final top 20. This adds ~10–15 ms for model inference (served via a low-latency model endpoint, ONNX runtime on CPU, co-located with the Search Service). Feature engineering requires a Feature Store (e.g., Feast or AWS SageMaker Feature Store). Estimated NDCG improvement: +0.05 over current approach.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario                    | Impact                                          | Detection                                    | Mitigation                                                               |
|-------------------------------------|-------------------------------------------------|----------------------------------------------|--------------------------------------------------------------------------|
| Elasticsearch cluster down          | All search queries fail                         | Health check; circuit breaker                | Serve degraded results from PostgreSQL + PostGIS full-text search (fallback mode); alert on-call |
| Single ES shard unavailable         | Partial search results (fewer matches)          | `_shards.failed` in ES response              | Return partial results with a log warning; ES promotes replica ~1 s      |
| Kafka consumer (Indexer) down       | Menu changes not propagating to ES index        | Consumer lag metric > threshold               | Reconciliation job catches up every 5 min; stale data within 30s SLA    |
| Redis cache miss storm              | All requests fall through to ES simultaneously  | Spike in ES latency / CPU                    | Cache lock (singleflight/mutex pattern) prevents thundering herd         |
| PostgreSQL read replica lag         | Menu Fetch Service returns stale menu data      | Replication lag metric > 5 s                 | Route to primary writer for reads temporarily; alert                    |
| CDN purge failure                   | Stale restaurant/menu data served up to 60 s   | CDN purge API error rate metric              | TTL-based expiry is the safety net; retry CDN purge 3× with backoff     |
| Personalization Service down        | Ranking loses personalization signal            | Health check; circuit breaker                | Degrade gracefully: skip personalization re-blend; return ES scores only |
| Search Service pod OOM              | Search requests fail for affected pod           | Kubernetes OOM kill; pod restart             | Other pods absorb traffic during restart (<30 s); HPA adds new pod      |
| Network partition between regions   | Cross-region requests fail                      | Latency spike + error rate                   | Route53 failover to healthy region; read-only mode for affected region  |
| Bulk index update causing ES hot spot | One shard receives disproportionate writes     | ES shard CPU/disk metric                     | Custom routing by `merchant_id` distributes writes; bulk size capped at 100 ops |

### Retries & Idempotency

- Search Service retries ES queries once on `503` with 50 ms delay (ES is returning if it can, or circuit breaker opens).
- Search Indexer retries Kafka-failed ES update 3× with exponential backoff (100 ms, 200 ms, 400 ms). After 3 failures, publishes to a dead-letter topic `menu-change-events-dlq` and alerts on-call.
- CDN purge retried 3× with 1-second delays.
- All ES partial update operations are idempotent (same `doc` applied twice produces the same result).

### Circuit Breaker

- Search Service → Elasticsearch: opens if error rate > 30% in 10 s; fallback is PostgreSQL full-text search.
- Search Service → Personalization Service: opens if p99 latency > 20 ms or error rate > 20%; fallback is no personalization.
- Menu Fetch Service → PostgreSQL read replica: opens on replication lag > 5 s; fallback routes to primary writer.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric                                   | Type      | Alert Threshold         | Purpose                                              |
|------------------------------------------|-----------|-------------------------|------------------------------------------------------|
| search_latency_p99_ms                    | Histogram | > 200 ms                | Consumer-facing SLA violation                        |
| autocomplete_latency_p99_ms              | Histogram | > 100 ms                | Autocomplete SLA                                     |
| search_zero_result_rate                  | Counter   | > 15% of queries        | Poor coverage or broken filter logic                 |
| elasticsearch_shard_count_failed         | Gauge     | > 0                     | ES cluster health                                    |
| kafka_consumer_lag_indexer               | Gauge     | > 5 000 messages        | Menu update pipeline backlog                         |
| index_update_propagation_seconds_p95     | Histogram | > 30 s                  | Freshness SLA violation                              |
| es_cluster_health_status                 | Enum      | != green                | ES cluster degraded                                  |
| cache_hit_rate_menu_pages                | Gauge     | < 90%                   | Cache inefficiency / cold start                      |
| search_click_through_rate                | Counter   | < 40%                   | Ranking quality degradation                          |
| search_to_order_conversion_rate          | Counter   | < 15% (varies by market)| Overall funnel health                                |
| es_index_refresh_lag_ms                  | Histogram | > 5 000 ms              | Index write throughput issue                         |
| personalization_service_error_rate       | Counter   | > 5%                    | Ranking degradation                                  |

### Distributed Tracing

- OTEL instrumented across API Gateway → Search Service → Elasticsearch → Personalization Service.
- `trace_id` injected into ES queries via `ext` header (ES supports passthrough of custom headers for logging).
- Search event logged with `trace_id` in `search_events` table, enabling correlation of a specific consumer search to the underlying ES query, ES shard responses, and personalization service call.
- Sampling: 100% error traces; 5% success traces for search (higher than order path due to debugging value of search traces); tail-based override for queries > 300 ms.

### Logging

- Structured JSON with: `service`, `trace_id`, `consumer_id`, `query_text` (truncated to 100 chars), `lat_h3_cell` (not raw lat/lng — privacy), `result_count`, `es_latency_ms`, `personalization_ms`, `total_latency_ms`.
- `query_text` is hashed before logging for PII-sensitive queries (though food queries rarely contain PII).
- Search event analytics pipeline: logs stream to Kafka topic `search-events-raw` → Spark Streaming aggregates CTR, conversion, and NDCG metrics hourly → writes to BigQuery for the ML team.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                            | Choice Made                                    | Alternative                           | Rationale                                                              | Trade-off Accepted                                                 |
|-------------------------------------|------------------------------------------------|---------------------------------------|------------------------------------------------------------------------|--------------------------------------------------------------------|
| Search engine                       | Elasticsearch 8.x                             | Algolia / Typesense                   | Cost at scale; control over ranking; geo-query performance             | Operational burden; cluster tuning required                        |
| Ranking approach                    | ES function_score + personalization blend     | Full LTR (LightGBM)                   | Achieves 0.73 NDCG@10 at 20% operational cost of full LTR             | 0.05 NDCG gap vs. full LTR; planned future upgrade                |
| Index update mechanism              | Kafka async + Indexer consumer                | Synchronous ES write in Menu API      | Decoupling; reliability; no blocking merchant API on ES latency        | ~2–3 s propagation delay (well within 30 s SLA)                  |
| Autocomplete                        | ES Completion Suggester + Redis prefix cache  | Dedicated Typesense cluster           | Re-uses ES infrastructure; sub-100 ms with caching                    | Limited typo tolerance in completion path; addressed via fallback |
| Geo caching unit                    | H3 resolution 7 for restaurant list cache     | Lat/lng bounding box                  | H3 cells are uniform size; cache key stability; no edge-case overlap   | Cell boundary artifacts (restaurant just across a cell edge may appear/disappear) |
| Source of truth                     | PostgreSQL; ES as derived index               | ES as primary store                   | ACID guarantees for catalog data; ES eventual consistency is acceptable | Dual-write complexity; reconciliation job required                 |
| CDN for search results              | Cache by geo-tile + filter hash               | No CDN (all dynamic)                  | 30-second TTL covers majority of traffic; 90%+ cache hit rate estimated | Up to 30 s stale data; restaurant status may lag                  |

---

## 11. Follow-up Interview Questions

**Q1: How would you design support for "trending dishes" as a search ranking signal?**
A: Track `order_count_last_24h` per `item_id` via a Redis counter incremented on each order event. Nightly batch job writes this into the `popularity_score` field in the menu item ES document. For real-time trending (last 1 hour), maintain a Redis Sorted Set `trending:items:{h3_cell_r6}` updated on each order with a decaying score (ZINCRBY with a time-decay factor). Search Service fetches the top-20 trending item IDs from Redis and applies a +0.3 function_score boost for those items in the ES query.

**Q2: How would you handle seasonal menu availability (lunch menu vs. dinner menu)?**
A: Add `available_from_hour` and `available_to_hour` fields to `menu_items`. The ES index includes these. At query time, the Search Service adds a `range` filter on `available_from_hour <= current_hour AND available_to_hour >= current_hour`. Since this filter depends on the current time, CDN caching TTL is reduced to 5 minutes for restaurants with time-based menus to avoid serving lunch items during dinner hours. Menu Fetch Service applies the same filter when building the menu page response.

**Q3: How would you design ghost kitchen search? (One kitchen, multiple virtual restaurant brands on one address.)**
A: A ghost kitchen has one physical location (`lat`, `lng`) but N virtual brands (N separate `merchant_id` rows pointing to the same physical address). Each brand has its own menu, name, and cuisine type in the search index. From a search perspective, they are independent restaurants. The only difference is that a single Dasher picking up from the address may see multiple orders from different brands — but this is a dispatch concern, not a search concern. We may add a `kitchen_id` field to group brands from the same physical location so we can deduplicate on a map view.

**Q4: How would you support dietary filters that require item-level granularity in restaurant-level search?**
A: A restaurant is tagged with a dietary flag (e.g., `has_vegan_options`) if any of its available items carry that flag. This is computed nightly: `UPDATE merchants SET dietary_flags = array_agg(DISTINCT unnest(mi.dietary_flags)) FROM menu_items mi WHERE mi.merchant_id = merchants.merchant_id AND mi.is_available = true GROUP BY merchant_id`. This derived flag is indexed in the `restaurant_index`. An item-level availability change triggers a Kafka event that updates this derived flag if the last vegan item on a menu is marked unavailable (the restaurant should lose the `vegan` tag). This logic runs in the Search Indexer.

**Q5: How would you implement "past orders" as a search boost?**
A: At search time, the Search Service fetches the consumer's last 10 distinct `merchant_id`s from the Personalization Service (sourced from PostgreSQL orders, cached in Redis). For each of those merchants that appears in the ES result set (top-50 candidates), apply a `+1.0` boost in the final personalization re-blend. Merchants not in the result set are unaffected. This ensures the consumer sees "their usual places" near the top, improving conversion without displacing highly relevant new results.

**Q6: How would you design the search experience for a consumer who changes their delivery address mid-session?**
A: The delivery address lat/lng is passed with every search request. When the address changes, the app re-issues the search request with the new coordinates. No server-side session state depends on the address — it is purely a query parameter. The Redis prefix cache (autocomplete) is keyed on H3 cell; if the new address is in a different cell, a fresh cache lookup occurs. CDN cache also keys on H3 cell, so different results are correctly served. The consumer's `default_address_id` can be updated via `PUT /users/{id}/default-address` which invalidates the personalization cache entry.

**Q7: How would you measure the ROI of adding personalization to search ranking?**
A: A/B test: randomly assign consumers to control (function_score only) and treatment (function_score + personalization). Measure: (1) Search-to-order conversion rate (primary metric). (2) Time-to-first-click (lower = better ranking). (3) Average order value (personalization may surface restaurants the consumer likes more, leading to higher spend). (4) Repeat order rate over 7 days (personalization may improve loyalty). Run for 2 weeks minimum with >95% statistical significance at 5% minimum detectable effect size. Sample size: ~200K consumers per arm (available from 37M MAU).

**Q8: How would you handle "no delivery available to my address" — the restaurant list is empty because no restaurants deliver to a remote area?**
A: First, check if any restaurants have a delivery radius covering the address. If none: (1) Surface a "No restaurants deliver to this location yet" message. (2) Offer pickup-only mode: show restaurants within 5 km that offer pickup. (3) Show a waitlist sign-up form to capture demand signal for market expansion. The pickup query is a different ES query: `geo_distance` filter anchored to the restaurant's location rather than the consumer's address, and sort by `distance_to_consumer`.

**Q9: How would you implement "best match" search when the consumer has not entered a query (browsing mode)?**
A: In browse mode (`q` is absent), the ES query drops the `multi_match` clause and uses only the `function_score` with geo, rating, review_count, popularity, and personalization signals. This is effectively a "recommended restaurants near you" ranking. We add a time-of-day bias: breakfast hours (6–10 AM) boost `cuisine_types: [breakfast, coffee]`; dinner hours (5–10 PM) boost `cuisine_types: [dinner, sushi, steakhouse]`. These time-of-day boosts are a `function_score` filter function with a `filter: {range: {current_hour: {gte: 17, lte: 22}}}` condition.

**Q10: How would you design the "Explore" tab that shows curated collections (e.g., "Best Sushi in SF")?**
A: Curated collections are content-managed: a `collections` table in PostgreSQL stores `(collection_id, title, merchant_ids[], rules_json)`. `rules_json` encodes the selection logic (e.g., `{cuisine: sushi, city: SF, min_rating: 4.5, sort: review_count_desc, limit: 10}`). The collection is materialized nightly into a precomputed list by the Collection Service, stored in Redis (`collection:{id}` → ordered list of `merchant_ids`). The API `GET /collections/{id}` fetches from Redis and hydrates each merchant from the menu cache. Collections are refreshed if any member restaurant changes status. This avoids real-time search for a feature that is by definition editorially curated and slow-changing.

**Q11: How would you support search in a different language for international markets?**
A: Each international market gets a language-specific ES index (`restaurant_index_fr`, `restaurant_index_ja`). A locale-aware analyzer is configured per index (e.g., `french` analyzer for French, `kuromoji` analyzer for Japanese). The API Gateway routes to the appropriate ES index based on the `Accept-Language` header. Menu data is stored in PostgreSQL with a `locale` column per `menu_items` row for translated content. The nightly translation pipeline uses DeepL/Google Translate API for merchant-submitted content and human review for accuracy.

**Q12: What's the consistency model between the PostgreSQL source of truth and the Elasticsearch index? When can they diverge?**
A: They diverge by design — Elasticsearch is an eventually-consistent projection. Divergence can occur: (1) During Kafka consumer lag (Indexer behind by seconds to minutes). (2) During ES partial update failures (3 retries exhausted, item in DLQ). (3) During a full re-index (scheduled nightly; in-flight changes during re-index are applied on top via the update pipeline). The 5-minute reconciliation job is the backstop. We accept that a consumer could see an item in search results that is no longer available; this is caught at cart/checkout time when availability is checked against PostgreSQL (the source of truth).

**Q13: How would you handle a DDoS attack targeting the search API?**
A: Layer 1: Cloudflare/AWS Shield at the CDN edge — rate limits by IP, challenges suspicious traffic. Layer 2: API Gateway rate limits (100 req/min per authenticated consumer; 500 req/min per IP for unauthenticated). Layer 3: Search Service request queue depth limits — if the queue exceeds 1 000 pending requests, new requests return 429. Layer 4: ES circuit breaker — prevents the backend from being overwhelmed. For volumetric attacks, Cloudflare's DDoS protection absorbs traffic at the edge before it reaches origin. We also use CAPTCHAs for anonymous search sessions exhibiting bot-like patterns (perfectly regular 1-req/s cadence, no user-agent variety).

**Q14: How would you implement "order again" quick access in search?**
A: A separate `GET /consumers/{id}/reorder-suggestions` endpoint returns the consumer's top-5 most recently ordered restaurants that are currently open. This is a simple SQL query against the `orders` table (last 30 days, `status=delivered`), joined with `merchants` to check `is_active AND NOT is_paused AND open_now`. It runs against a read replica and is cached in Redis for 5 minutes per consumer. The response is displayed as a horizontal scroll strip above the main search results in the app — not a search query but a personalized recommendations widget.

**Q15: How do you validate that the Elasticsearch index is consistent with PostgreSQL after a full re-index?**
A: After a full re-index completes, run a validation job: (1) Count-match check: `SELECT COUNT(*) FROM menu_items WHERE is_available=true` vs. ES `_count` API query. If counts differ by > 0.1%, alert. (2) Spot-check: Sample 1 000 random `item_id`s from PostgreSQL and verify their `is_available` and `dietary_flags` fields match ES. (3) Zero-result regression test: Run 100 canonical test queries that are expected to return results; alert if any return 0 results. (4) Performance test: Measure p99 search latency for 60 seconds post-reindex to detect any shard imbalance.

---

## 12. References & Further Reading

- DoorDash Engineering Blog — "Building Faster Indexing with Apache Kafka and Elasticsearch" (2021): https://doordash.engineering/2021/07/14/open-source-search-indexing/
- DoorDash Engineering Blog — "Taming Content Freshness with In-Sync Menu Management" (2021): https://doordash.engineering/2021/02/23/taming-content-freshness-with-insync-menu-management/
- Elasticsearch Reference — Completion Suggester: https://www.elastic.co/guide/en/elasticsearch/reference/current/search-suggesters.html
- Elasticsearch Reference — Function Score Query: https://www.elastic.co/guide/en/elasticsearch/reference/current/query-dsl-function-score-query.html
- Elasticsearch Reference — Geo Distance Filter: https://www.elastic.co/guide/en/elasticsearch/reference/current/query-dsl-geo-distance-query.html
- Elasticsearch Reference — Near Real-Time Search: https://www.elastic.co/guide/en/elasticsearch/reference/current/near-real-time.html
- Kleppmann, Martin — "Designing Data-Intensive Applications" (O'Reilly, 2017) — Chapter 3 (Storage and Retrieval), Chapter 11 (Stream Processing)
- Uber Engineering Blog — "H3: Uber's Hexagonal Hierarchical Spatial Index" (2018): https://eng.uber.com/h3/
- Netflix TechBlog — "Netflix Recommendations: Beyond the 5 Stars" (Learning-to-Rank): https://netflixtechblog.com/netflix-recommendations-beyond-the-5-stars-part-1-55838468f429
- Apache Kafka Documentation — Consumer Groups: https://kafka.apache.org/documentation/#intro_consumers
- AWS ElastiCache (Redis) Geo Commands: https://redis.io/docs/data-types/geospatial/
- Debezium PostgreSQL Connector Documentation: https://debezium.io/documentation/reference/connectors/postgresql.html
- O'Connor, Ian — "Introduction to Information Retrieval" (Cambridge, 2008) — Chapter 8 (Evaluation in Information Retrieval, NDCG): https://nlp.stanford.edu/IR-book/
