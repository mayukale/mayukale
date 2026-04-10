# System Design: Amazon Product Page

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Product Catalog Display** — Serve a product detail page (PDP) with title, description, images, bullet points, technical specifications, and A+ content (rich HTML/media blocks authored by sellers or brand owners).
2. **Pricing & Availability** — Show real-time price (including sale price, strikethrough price), stock status ("In Stock", "Only 3 left"), and estimated delivery date.
3. **Seller Information** — Display the "sold by / fulfilled by" badge, seller rating, and link to the seller storefront. For multi-seller listings, show the buy-box winner and secondary offers.
4. **Reviews & Ratings** — Aggregate star ratings (1–5), display review count, render paginated written reviews with helpfulness voting, verified-purchase badge, and image/video attachments.
5. **Questions & Answers (Q&A)** — Allow customers to submit questions, allow sellers/community to answer, and surface top Q&A pairs on the PDP.
6. **Price History** — Expose an API (and optional embedded widget) showing historical price over time so customers can verify deal legitimacy.
7. **Recommendation Widgets** — "Frequently Bought Together", "Customers Who Bought This Also Bought", "Sponsored Products", "Similar Items" — each powered by different ML signals.
8. **Inventory Display** — Show quantity warnings (e.g., "Only 2 left in stock") for low-stock situations. Threshold: surface warning when qty ≤ 10 units.
9. **Variant Selection** — Color, size, material, and other attribute variations map to individual child ASINs (Amazon Standard Identification Numbers).

### Non-Functional Requirements

1. **Performance** — P99 page-load latency ≤ 200 ms for the above-the-fold HTML; P99 full page ≤ 500 ms (excluding client-side JS hydration).
2. **Availability** — 99.99% uptime (≤ 52.6 min downtime/year). Product pages directly affect revenue; every 100 ms of added latency costs ~1% conversion.
3. **Consistency** — Pricing and inventory must be fresh within 5 seconds (near-real-time). Review aggregates may be eventually consistent within 60 seconds.
4. **Scalability** — Handle Amazon-scale traffic: ~600 M unique product pages (ASINs) globally, peak ~15 M page views/hour during events like Prime Day.
5. **Read-Heavy** — Read:Write ratio ≈ 10,000:1 for product content; ratios are different per component (reviews: ~100:1).
6. **Search Engine Optimization** — Server-side rendered (SSR) HTML for crawlability.
7. **Security** — Rate-limit review submissions; prevent fake reviews; protect price-history endpoints from scraping abuse.
8. **Internationalization** — Content localized per marketplace (amazon.com, amazon.co.uk, amazon.de, etc.).

### Out of Scope

- Checkout flow (covered in Shopping Cart design)
- Seller onboarding and listing creation pipeline
- Advertising auction system (Sponsored Products are consumed as a widget but the auction engine is a separate design)
- Fraud detection for reviews (covered as a note only)
- Mobile app rendering layer (assumed to use the same APIs)

---

## 2. Users & Scale

### User Types

| User Type | Description | Access Pattern |
|---|---|---|
| Anonymous Shopper | Not logged in; majority of web traffic | Read-only; receives cached content |
| Authenticated Shopper | Logged in; personalized price, Prime badge, recommendations | Read with personalization signals |
| Seller / Brand Owner | Submits A+ content, responds to Q&A | Write (low frequency) |
| Internal Systems | Inventory service, pricing engine, ML pipelines | Write via internal APIs |
| Search Crawlers | Googlebot, Bingbot | Read SSR HTML; rate-limited |

### Traffic Estimates

**Assumptions:**
- Amazon serves ~2.5 B product page views/day globally (based on publicly cited ~300 M active customers, ~8–9 page views/customer/day average).
- Peak factor: 4× during Prime Day, Black Friday events.
- Average PDP payload: ~120 KB HTML (SSR), ~800 KB total with images (served via CDN separately).

| Metric | Calculation | Result |
|---|---|---|
| Daily page views | 2.5 B/day (assumption, Amazon-scale) | 2,500,000,000/day |
| Average RPS (reads) | 2.5 B / 86,400 s | ~28,900 RPS |
| Peak RPS (4× Prime Day) | 28,900 × 4 | ~115,600 RPS |
| Reviews per day (new) | ~5 M new reviews/day (industry estimate) | 5,000,000/day |
| Review write RPS | 5 M / 86,400 | ~58 RPS (negligible) |
| Q&A submissions/day | ~500 K/day (10% of review volume) | 500,000/day |
| Price updates/day | ~50 M SKU price changes/day (dynamic pricing) | 578 updates/RPS |
| Inventory updates/day | ~200 M events (purchases + restocks) | 2,314 events/RPS |

### Latency Requirements

| Component | Target (P50) | Target (P99) | Rationale |
|---|---|---|---|
| Product page SSR (above-the-fold) | 50 ms | 200 ms | Direct revenue impact; Google CWV threshold |
| Full page render (server) | 100 ms | 500 ms | Acceptable for below-fold widgets |
| Pricing API | 10 ms | 50 ms | Must be fresh; served from in-memory cache |
| Reviews fetch (paginated) | 20 ms | 100 ms | Below-fold; slight delay tolerable |
| Recommendations API | 20 ms | 80 ms | Can use stale-while-revalidate |
| Price history API | 50 ms | 200 ms | Not critical path |

### Storage Estimates

**Assumptions:**
- 600 M active ASINs globally.
- Each product record averages 10 KB (title, description, specs, metadata).
- A+ content averages 50 KB per ASIN (30% of ASINs have A+ content: ~180 M).
- Each review averages 2 KB text + metadata.
- Total reviews globally: ~5 B cumulative.
- Price history: 1 price point per SKU per hour retained for 2 years.

| Entity | Record Size | Count | Total Storage |
|---|---|---|---|
| Product catalog records | 10 KB | 600 M | 6 TB |
| A+ content (HTML blobs) | 50 KB | 180 M | 9 TB |
| Product images (original) | 3 MB avg | 600 M × 5 images | 9 PB (S3) |
| Product images (thumbnails, CDN) | 50 KB avg | 600 M × 5 × 3 sizes | 450 TB (CDN) |
| Reviews (text + metadata) | 2 KB | 5 B | 10 TB |
| Review images/videos | 500 KB avg | 200 M (4% have media) | 100 TB |
| Rating aggregates | 100 B | 600 M | 60 GB |
| Q&A pairs | 1 KB | 2 B (assumption) | 2 TB |
| Price history records | 20 B | 600 M × 8,760 hrs × 2 yrs | ~105 TB |
| Inventory snapshots | 100 B | 600 M × 10/day retained 30 days | ~180 GB |

### Bandwidth Estimates

| Traffic Type | Calculation | Result |
|---|---|---|
| Inbound HTML reads (SSR) | 115,600 RPS × 120 KB (peak) | ~13.9 GB/s peak |
| CDN image egress | 115,600 RPS × 300 KB (3 images avg, CDN hit) | ~34.7 GB/s peak (CDN bears this) |
| Origin image requests (CDN miss ~5%) | 0.05 × 115,600 × 300 KB | ~1.7 GB/s |
| Review API responses | 115,600 × 10% (review scroll) × 5 KB | ~58 MB/s |
| Price update fan-out (Kafka) | 578 RPS × 200 B | ~116 KB/s (internal) |
| Inventory event ingestion | 2,314 RPS × 100 B | ~231 KB/s (internal) |

---

## 3. High-Level Architecture

```
                          ┌──────────────────────────────────────────────────────┐
                          │                    CLIENTS                           │
                          │   Browser  ·  Mobile App  ·  Bots/Crawlers           │
                          └───────────────────────┬──────────────────────────────┘
                                                  │ HTTPS
                                                  ▼
                          ┌──────────────────────────────────────────────────────┐
                          │                 CDN (CloudFront / Akamai)            │
                          │  - Caches static assets (images, JS, CSS)            │
                          │  - Caches SSR HTML for anonymous users (TTL 5s)      │
                          │  - Edge compute for personalization headers           │
                          └───────────────────────┬──────────────────────────────┘
                                                  │ Cache miss / dynamic
                                                  ▼
                          ┌──────────────────────────────────────────────────────┐
                          │            API Gateway / Load Balancer               │
                          │  - TLS termination, DDoS protection                  │
                          │  - Routes /dp/{asin} to PDP Service                  │
                          │  - Routes /reviews, /qa, /price-history to sub-svcs  │
                          └──────┬───────────────┬──────────────────┬────────────┘
                                 │               │                  │
               ┌─────────────────▼──┐   ┌────────▼────────┐  ┌────▼────────────┐
               │   PDP SSR Service   │   │  Reviews Service │  │  Q&A Service    │
               │  (Node/React SSR)   │   │  (Go microservice)│  │  (Go/Java)     │
               │  Aggregates data    │   │  CRUD + aggreg.  │  │  CRUD + rank    │
               │  from all services  │   │  Helpfulness vote │  │  Answer ranking │
               └────────┬────────────┘   └────────┬────────┘  └────┬────────────┘
                        │                          │                │
          ┌─────────────┼──────────────────────────┼────────────────┼────────────┐
          │             ▼                          ▼                ▼            │
          │  ┌─────────────────┐    ┌────────────────────┐  ┌──────────────┐    │
          │  │ Product Catalog  │    │   Reviews DB        │  │  Q&A DB      │    │
          │  │   Service        │    │  (Cassandra/DynamoDB│  │  (DynamoDB)  │    │
          │  │  (Catalog DB)    │    │   sharded by ASIN)  │  │              │    │
          │  └────────┬────────┘    └────────────────────┘  └──────────────┘    │
          │           │                                                           │
          │  ┌────────▼──────────┐  ┌──────────────────┐  ┌───────────────────┐ │
          │  │  Pricing Service   │  │ Inventory Service │  │ Recommendations   │ │
          │  │  (Redis + RDBMS)   │  │ (Redis atomic +   │  │ Service (ML       │ │
          │  │  Real-time price   │  │  Aurora Postgres)  │  │  embeddings,      │ │
          │  │  buy-box selection │  │  Stock qty, holds  │  │  pre-computed)    │ │
          │  └────────────────────┘  └──────────────────┘  └───────────────────┘ │
          │                                                                       │
          │  ┌──────────────────┐    ┌──────────────────┐  ┌───────────────────┐ │
          │  │ Price History Svc│    │  Seller Service   │  │  A+ Content Svc   │ │
          │  │ (TimescaleDB /   │    │  (MySQL + cache)  │  │  (S3 + DynamoDB   │ │
          │  │  ClickHouse)     │    │  Seller profile,  │  │   metadata)       │ │
          │  │  Time-series data│    │  ratings          │  │                   │ │
          │  └──────────────────┘    └──────────────────┘  └───────────────────┘ │
          │                                                                       │
          │  ┌─────────────────────────────────────────────────────────────────┐ │
          │  │               Shared Infrastructure Layer                        │ │
          │  │  Redis Cluster (L2 cache)  ·  Kafka (event bus)                 │ │
          │  │  Elasticsearch (search/reviews full-text)  ·  S3 (blobs)        │ │
          │  └─────────────────────────────────────────────────────────────────┘ │
          └───────────────────────────────────────────────────────────────────────┘

Event Flows:
  Purchase/Inventory change ──► Kafka ──► Inventory Service ──► Redis invalidation
  New Review submitted       ──► Kafka ──► Review Aggregator ──► Rating cache update
  Price change               ──► Kafka ──► Pricing Service   ──► CDN soft-purge
```

**Component Roles:**

| Component | Role |
|---|---|
| CDN (CloudFront) | Caches SSR HTML (short TTL), all static assets; absorbs 80–90% of traffic |
| API Gateway | TLS termination, auth token validation, rate limiting, routing |
| PDP SSR Service | Orchestrates parallel fan-out calls to all downstream services; renders final HTML |
| Product Catalog Service | Source of truth for product metadata (title, description, specs, images) |
| Pricing Service | Real-time price, buy-box winner selection across multiple sellers |
| Inventory Service | Stock quantity, low-stock warnings, display-level reservation |
| Reviews Service | CRUD for reviews, aggregate star ratings, helpfulness votes |
| Q&A Service | Customer question submission, answer ranking, top-Q&A surfacing |
| Recommendations Service | Pre-computed ML recommendations served as low-latency lookup |
| Price History Service | Time-series store of price snapshots for widget and transparency |
| Seller Service | Seller profile, aggregate seller rating, storefront link |
| A+ Content Service | Rich brand content blobs (HTML/images) stored in S3, metadata in DynamoDB |
| Redis Cluster | L2 distributed cache for pricing, inventory counts, rating aggregates |
| Kafka | Async event bus for inventory updates, review events, price changes |
| Elasticsearch | Full-text search over reviews; faceted filtering |

**Primary Use-Case Data Flow (User loads /dp/B09XYZ):**

1. Browser requests `amazon.com/dp/B09XYZ`.
2. CDN checks its cache. If anonymous request and within TTL (5 s), serves cached HTML — done.
3. On cache miss, request reaches API Gateway → PDP SSR Service.
4. PDP SSR Service fans out **in parallel** to: Catalog, Pricing, Inventory, Recommendations, Seller, A+ Content services.
5. Reviews Service returns top 3 reviews asynchronously (rest are lazy-loaded).
6. Each service checks Redis (L2 cache) before hitting its DB.
7. PDP SSR Service assembles all data, renders HTML server-side, returns response.
8. CDN caches the rendered HTML (TTL 5 s for authenticated, 30 s for anonymous).

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- PRODUCT CATALOG (stored in DynamoDB / Aurora globally)
-- ============================================================
CREATE TABLE products (
    asin            CHAR(10)        NOT NULL,   -- Amazon Standard Identification Number
    marketplace_id  SMALLINT        NOT NULL,   -- 1=US, 2=UK, 3=DE, etc.
    title           VARCHAR(500)    NOT NULL,
    brand           VARCHAR(200),
    category_id     BIGINT          NOT NULL,
    subcategory_id  BIGINT,
    bullet_points   JSONB,                      -- Array of up to 5 bullet strings
    description     TEXT,
    technical_specs JSONB,                      -- Key-value pairs of specs
    parent_asin     CHAR(10),                   -- NULL for standalone; set for child variants
    variation_attrs JSONB,                      -- e.g. {"color":"Red","size":"M"}
    primary_image   VARCHAR(500),               -- S3 key
    image_keys      JSONB,                      -- Array of S3 keys
    is_active       BOOLEAN         NOT NULL DEFAULT TRUE,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (asin, marketplace_id)
);

CREATE INDEX idx_products_parent ON products(parent_asin) WHERE parent_asin IS NOT NULL;
CREATE INDEX idx_products_category ON products(category_id, subcategory_id);

-- ============================================================
-- PRICING  (Aurora Postgres; hot data mirrored to Redis)
-- ============================================================
CREATE TABLE pricing (
    asin                CHAR(10)        NOT NULL,
    marketplace_id      SMALLINT        NOT NULL,
    seller_id           BIGINT          NOT NULL,
    list_price_cents    INT             NOT NULL,   -- Original / MSRP
    sale_price_cents    INT             NOT NULL,   -- Current offer price
    currency_code       CHAR(3)         NOT NULL DEFAULT 'USD',
    is_buy_box_winner   BOOLEAN         NOT NULL DEFAULT FALSE,
    prime_eligible      BOOLEAN         NOT NULL DEFAULT FALSE,
    condition           VARCHAR(20)     NOT NULL DEFAULT 'new',  -- new, used, refurbished
    quantity_available  INT             NOT NULL DEFAULT 0,
    valid_from          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    valid_to            TIMESTAMPTZ,               -- NULL = currently active
    PRIMARY KEY (asin, marketplace_id, seller_id, valid_from)
);

CREATE INDEX idx_pricing_buybox ON pricing(asin, marketplace_id, is_buy_box_winner)
    WHERE is_buy_box_winner = TRUE AND valid_to IS NULL;

-- ============================================================
-- PRICE HISTORY  (TimescaleDB hypertable; partitioned by time)
-- ============================================================
CREATE TABLE price_history (
    asin            CHAR(10)        NOT NULL,
    marketplace_id  SMALLINT        NOT NULL,
    recorded_at     TIMESTAMPTZ     NOT NULL,
    price_cents     INT             NOT NULL,
    seller_id       BIGINT          NOT NULL
);

SELECT create_hypertable('price_history', 'recorded_at', chunk_time_interval => INTERVAL '1 week');
CREATE INDEX idx_ph_asin_time ON price_history(asin, marketplace_id, recorded_at DESC);

-- ============================================================
-- INVENTORY  (Redis for hot path; Aurora for durable state)
-- ============================================================
CREATE TABLE inventory (
    asin                CHAR(10)        NOT NULL,
    warehouse_id        INT             NOT NULL,
    quantity_on_hand    INT             NOT NULL DEFAULT 0,
    quantity_reserved   INT             NOT NULL DEFAULT 0,   -- Soft holds from carts
    quantity_available  INT GENERATED ALWAYS AS (quantity_on_hand - quantity_reserved) STORED,
    last_updated        TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (asin, warehouse_id)
);

-- Denormalized aggregate cached in Redis as:
--   HASH key: inv:{asin}  field: total_available  value: INT

-- ============================================================
-- REVIEWS  (Cassandra; partition key = asin)
-- ============================================================
-- Cassandra CQL representation:
--
-- CREATE TABLE reviews (
--     asin            text,
--     review_id       uuid,
--     user_id         bigint,
--     marketplace_id  smallint,
--     rating          tinyint,          -- 1-5
--     title           text,
--     body            text,
--     verified_purchase boolean,
--     helpful_count   int,
--     not_helpful_count int,
--     image_keys      list<text>,
--     video_keys      list<text>,
--     status          text,             -- published, pending_moderation, removed
--     created_at      timestamp,
--     PRIMARY KEY ((asin, marketplace_id), created_at, review_id)
-- ) WITH CLUSTERING ORDER BY (created_at DESC, review_id ASC)
--   AND default_time_to_live = 0;

-- Rating aggregate (SQL, updated via streaming aggregation):
CREATE TABLE rating_aggregates (
    asin                CHAR(10)        NOT NULL,
    marketplace_id      SMALLINT        NOT NULL,
    total_reviews       INT             NOT NULL DEFAULT 0,
    sum_ratings         BIGINT          NOT NULL DEFAULT 0,
    count_1star         INT             NOT NULL DEFAULT 0,
    count_2star         INT             NOT NULL DEFAULT 0,
    count_3star         INT             NOT NULL DEFAULT 0,
    count_4star         INT             NOT NULL DEFAULT 0,
    count_5star         INT             NOT NULL DEFAULT 0,
    average_rating      DECIMAL(3,2)    NOT NULL DEFAULT 0.00,
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (asin, marketplace_id)
);

-- ============================================================
-- QUESTIONS & ANSWERS  (DynamoDB)
-- ============================================================
-- DynamoDB schema (conceptual):
--
-- Table: questions
--   PK: asin#marketplace_id  (partition key)
--   SK: question_id          (sort key, ULID for time-ordering)
--   question_text: String
--   asker_user_id: Number
--   helpful_count: Number
--   created_at: String (ISO-8601)
--   status: String  (published | moderation)
--
-- Table: answers
--   PK: question_id          (partition key)
--   SK: answer_id            (sort key)
--   answer_text: String
--   answerer_user_id: Number
--   answerer_type: String    (seller | customer | amazon)
--   helpful_count: Number
--   created_at: String

-- ============================================================
-- SELLERS  (MySQL / Aurora)
-- ============================================================
CREATE TABLE sellers (
    seller_id           BIGINT          NOT NULL AUTO_INCREMENT,
    seller_name         VARCHAR(200)    NOT NULL,
    storefront_url      VARCHAR(500),
    positive_feedback_pct DECIMAL(5,2),
    total_ratings       INT             NOT NULL DEFAULT 0,
    joined_at           DATE,
    is_amazon_fulfilled BOOLEAN         NOT NULL DEFAULT FALSE,
    PRIMARY KEY (seller_id)
);

-- ============================================================
-- A+ CONTENT  (S3 + DynamoDB metadata)
-- ============================================================
-- DynamoDB Table: aplus_content
--   PK: asin             (partition key)
--   SK: marketplace_id   (sort key)
--   s3_key: String       -- points to rendered HTML blob in S3
--   version: Number
--   last_updated: String
--   status: String       (published | draft)

-- ============================================================
-- RECOMMENDATIONS  (pre-computed, stored in DynamoDB + Redis)
-- ============================================================
-- DynamoDB Table: recommendations
--   PK: asin#marketplace_id
--   SK: widget_type      -- 'frequently_bought_together' | 'also_bought' | 'similar'
--   recommended_asins: List<String>  -- ordered list of up to 20 ASINs
--   computed_at: String
--   model_version: String
```

### Database Choice

| Component | Options Considered | Selected | Justification |
|---|---|---|---|
| Product Catalog | MySQL, DynamoDB, MongoDB, PostgreSQL | **DynamoDB (with Aurora for complex queries)** | ASIN is a natural partition key; DynamoDB provides single-digit millisecond reads at any scale; catalog reads are key-value lookups; Aurora used for search/browse queries needing joins |
| Pricing (hot) | Redis, Cassandra, DynamoDB | **Redis (primary) + Aurora (durable)** | Redis HASH operations are O(1); price must be read in <5 ms; Aurora is the write-ahead durable store with Redis as read-through cache; Redis TTL invalidation on price change events |
| Reviews | Cassandra, DynamoDB, PostgreSQL | **Apache Cassandra** | Reviews are partitioned by ASIN — Cassandra's partition-key model maps perfectly; write-heavy (millions of new reviews/day); wide-row model handles paginated reads by time efficiently; tunable consistency (QUORUM for writes, LOCAL_ONE for reads) |
| Price History | InfluxDB, TimescaleDB, ClickHouse, DynamoDB | **TimescaleDB** | Natively handles time-series with hypertable auto-partitioning; PostgreSQL SQL compatibility simplifies analytics queries for the history widget; continuous aggregates for hourly rollups eliminate expensive scans |
| Q&A | DynamoDB, MySQL, Cassandra | **DynamoDB** | Low-cardinality access pattern (by ASIN); sparse data (not all ASINs have Q&A); DynamoDB's flexible schema handles varying answer counts without schema migrations; Global Secondary Index on helpful_count for ranking |
| Seller Info | MySQL, DynamoDB | **Aurora MySQL** | Seller data is relational (seller → offers → ratings); moderate scale (~5 M sellers globally); Aurora handles read replicas for scale without sharding complexity |
| Recommendations | Redis, DynamoDB, Cassandra | **Redis (hot) + DynamoDB (cold)** | Top recommendations fit in a single Redis key per ASIN/widget; sub-millisecond retrieval; DynamoDB as durable backing store; eviction policy: allkeys-lru |
| Inventory | Redis, Aurora, DynamoDB | **Redis (display qty) + Aurora (authoritative)** | Display quantity (the "Only 3 left" badge) reads from Redis for speed; authoritative deducted inventory lives in Aurora with row-level locking; Redis refreshed every 30 s via Kafka consumer |

---

## 5. API Design

All endpoints require:
- **Auth**: `Authorization: Bearer <JWT>` for authenticated users; anonymous requests use session cookie.
- **Rate Limits**: Applied per IP and per user_id. Limits noted per endpoint.
- **Versioning**: URI versioning `/v1/`.
- **Pagination**: Cursor-based for large result sets (reviews, Q&A).

```
# ─────────────────────────────────────────────────────────────
# 1. GET Product Detail
# ─────────────────────────────────────────────────────────────
GET /v1/dp/{asin}?marketplace_id=1

Response 200:
{
  "asin": "B09XYZ1234",
  "title": "...",
  "brand": "...",
  "bullet_points": ["...", "..."],
  "description": "...",
  "technical_specs": { "weight": "1.2 lbs", "dimensions": "6x4x2 in" },
  "images": ["https://cdn.example.com/img1.jpg", "..."],
  "variation_attributes": { "color": "Red", "size": "M" },
  "sibling_asins": ["B09XYZ1235", "B09XYZ1236"],
  "a_plus_content_url": "https://aplus.example.com/B09XYZ1234.html",
  "pricing": {
    "list_price_cents": 4999,
    "sale_price_cents": 3999,
    "currency": "USD",
    "buy_box_seller_id": 12345,
    "prime_eligible": true
  },
  "inventory": {
    "in_stock": true,
    "quantity_warning": "Only 3 left in stock",
    "estimated_delivery": "2026-04-11"
  },
  "rating_summary": {
    "average_rating": 4.3,
    "total_reviews": 12847,
    "histogram": { "1": 320, "2": 215, "3": 890, "4": 3102, "5": 8320 }
  },
  "seller": {
    "seller_id": 12345,
    "name": "TechWorld LLC",
    "positive_feedback_pct": 98.2,
    "total_ratings": 45000,
    "fulfilled_by_amazon": true
  }
}

Auth: Optional (anonymous allowed; JWT enables Prime price display)
Rate Limit: 1000 req/min per IP; 5000 req/min per authenticated user
Cache: CDN TTL 30s (anonymous), bypass CDN for authenticated

# ─────────────────────────────────────────────────────────────
# 2. GET Reviews (paginated)
# ─────────────────────────────────────────────────────────────
GET /v1/dp/{asin}/reviews
  ?marketplace_id=1
  &sort_by=helpful|recent|critical   (default: helpful)
  &filter_rating=5                   (optional, 1-5)
  &filter_verified=true              (optional)
  &cursor=<opaque_cursor>
  &limit=10                          (max 20)

Response 200:
{
  "reviews": [
    {
      "review_id": "uuid",
      "user_display_name": "John D.",
      "rating": 5,
      "title": "Excellent product",
      "body": "...",
      "verified_purchase": true,
      "helpful_count": 342,
      "created_at": "2025-11-15T10:22:00Z",
      "images": ["https://cdn.example.com/rev_img1.jpg"]
    }
  ],
  "next_cursor": "eyJjcmVhdGVkX2F0IjoiMjAyNS0...",
  "total_count": 12847
}

Auth: Optional
Rate Limit: 500 req/min per IP
Cache: 60s Redis cache keyed by (asin, sort_by, filter_rating, cursor)

# ─────────────────────────────────────────────────────────────
# 3. POST Review Submission
# ─────────────────────────────────────────────────────────────
POST /v1/dp/{asin}/reviews

Request Body:
{
  "rating": 4,
  "title": "Good but has quirks",
  "body": "...",
  "image_upload_tokens": ["tok_abc123"]  // pre-signed S3 upload tokens
}

Response 201:
{
  "review_id": "uuid",
  "status": "pending_moderation",
  "estimated_publish_time": "within 48 hours"
}

Auth: Required (JWT); must have purchased the ASIN
Rate Limit: 5 review submissions/day per user_id; 1 review/ASIN/user
Idempotency: Idempotency-Key header required; duplicate within 24h returns 409

# ─────────────────────────────────────────────────────────────
# 4. POST Review Helpfulness Vote
# ─────────────────────────────────────────────────────────────
POST /v1/reviews/{review_id}/vote
{
  "vote": "helpful" | "not_helpful"
}

Response 200: { "helpful_count": 343 }
Auth: Required
Rate Limit: 100 votes/hour per user_id

# ─────────────────────────────────────────────────────────────
# 5. GET Questions & Answers
# ─────────────────────────────────────────────────────────────
GET /v1/dp/{asin}/qa
  ?cursor=<cursor>
  &limit=5
  &sort_by=helpful|recent   (default: helpful)

Response 200:
{
  "questions": [
    {
      "question_id": "01HX...",
      "question_text": "Is this compatible with iPhone 15?",
      "helpful_count": 89,
      "created_at": "2026-01-10T08:00:00Z",
      "top_answer": {
        "answer_id": "01HY...",
        "answer_text": "Yes, fully compatible.",
        "answerer_type": "seller",
        "helpful_count": 45
      }
    }
  ],
  "next_cursor": "..."
}

Auth: Optional (required for submit)
Rate Limit: 200 req/min per IP

# ─────────────────────────────────────────────────────────────
# 6. POST Submit Question
# ─────────────────────────────────────────────────────────────
POST /v1/dp/{asin}/qa/questions
{ "question_text": "Does it come with a warranty?" }

Response 201: { "question_id": "01HZ...", "status": "published" }
Auth: Required
Rate Limit: 10 questions/day per user_id

# ─────────────────────────────────────────────────────────────
# 7. GET Price History
# ─────────────────────────────────────────────────────────────
GET /v1/dp/{asin}/price-history
  ?marketplace_id=1
  &range=90d|1y|all   (default: 90d)

Response 200:
{
  "currency": "USD",
  "data_points": [
    { "timestamp": "2025-11-01T00:00:00Z", "price_cents": 4299 },
    { "timestamp": "2025-12-01T00:00:00Z", "price_cents": 3999 }
  ]
}

Auth: Optional
Rate Limit: 100 req/min per IP (scraping risk: add CAPTCHA for headless signatures)
Cache: Redis TTL 5 min (data changes only when price changes)

# ─────────────────────────────────────────────────────────────
# 8. GET Recommendations
# ─────────────────────────────────────────────────────────────
GET /v1/dp/{asin}/recommendations
  ?widget=frequently_bought_together|also_bought|similar|sponsored
  &limit=10

Response 200:
{
  "widget": "frequently_bought_together",
  "items": [
    { "asin": "B08XYZ111", "title": "...", "price_cents": 1999, "image_url": "..." }
  ]
}

Auth: Optional (JWT enables personalized ranking)
Rate Limit: 500 req/min per IP
Cache: Redis TTL 5 min; stale-while-revalidate 60 s
```

---

## 6. Deep Dive: Core Components

### 6.1 PDP Page Assembly — Parallel Fan-Out with Bounded Latency

**Problem it solves:**
The product page requires data from ~8 distinct microservices (catalog, pricing, inventory, reviews, Q&A, recommendations, seller, A+ content). If called sequentially, worst-case latency adds up (8 × 50 ms = 400 ms). The PDP SSR Service must assemble all data and render HTML within 200 ms P99.

**Approach Comparison:**

| Approach | Latency | Complexity | Fault Tolerance | Notes |
|---|---|---|---|---|
| Sequential service calls | O(n × latency) ≈ 400 ms | Low | Poor (one failure blocks all) | Unacceptable for P99 target |
| Parallel fan-out (all or nothing) | O(max latency) ≈ 50 ms | Medium | Poor (one timeout delays page) | Better but fragile |
| Parallel fan-out with per-service timeouts and fallbacks | O(max latency with timeout) ≈ 50–80 ms | High | Good (partial results served) | **Selected** |
| GraphQL Federation (BFF) | O(max resolver latency) | Very High | Good | Overkill for internal SSR; adds resolver overhead |
| Pre-aggregated page cache | O(cache lookup) ≈ 1 ms | Medium | Excellent (cache hit) | Works for popular ASINs; cold path still needed |

**Selected Approach — Parallel Fan-Out with Timeouts and Graceful Degradation:**

```
PDP SSR Service receives GET /dp/B09XYZ1234:

Step 1: Check page-level cache (Redis, TTL 5s for anonymous)
        Key: pdp_html:{asin}:{marketplace_id}:{anonymous}
        → Cache HIT: return immediately (P50 ~2 ms)
        → Cache MISS: proceed to Step 2

Step 2: Launch parallel goroutines/promises for each service:
        ┌─────────────────────────────────────────────────┐
        │  Promise.all([                                   │
        │    catalogSvc.get(asin),      timeout: 100ms    │
        │    pricingSvc.get(asin),      timeout: 50ms     │
        │    inventorySvc.get(asin),    timeout: 50ms     │
        │    reviewsSvc.getTop3(asin),  timeout: 80ms     │
        │    sellerSvc.get(sellerId),   timeout: 80ms     │
        │    aplusSvc.get(asin),        timeout: 100ms    │
        │    recsSvc.get(asin),         timeout: 80ms     │
        │  ])                                              │
        └─────────────────────────────────────────────────┘

Step 3: For each timed-out or failed service, apply fallback:
        - Pricing timeout → use last cached price + "Price may have changed" note
        - Inventory timeout → show "Check availability" (hide quantity warning)
        - Reviews timeout → show "Reviews loading..." with client-side lazy load
        - A+ content timeout → hide A+ section (not critical above fold)
        - Recommendations timeout → hide widget (non-blocking)

Step 4: Render SSR HTML, store in Redis (TTL 5s anonymous, 0 TTL authenticated)
        Return HTML response.
```

**Implementation Detail — Circuit Breaker per Downstream Service:**

Each downstream call is wrapped with a circuit breaker (Hystrix pattern):
- **Closed state**: Normal operation; track failure rate in a sliding 10-second window.
- **Open state**: Triggered when failure rate exceeds 50% in the window; immediately returns fallback without making the network call; reduces latency for all users during incidents.
- **Half-open state**: After a 30-second cool-down, allows one probe request; if successful, transitions back to Closed.

Redis key for page cache: `pdp:html:{asin}:{mkt_id}:anon` (5 s TTL) and `pdp:html:{asin}:{mkt_id}:auth:{user_id}` (TTL 0 — do not cache personalized pages in shared Redis; cache only in CDN edge with `Vary: Cookie`).

**Interviewer Q&As:**

Q1: How do you prevent the PDP SSR Service from becoming a bottleneck?
A: The service is stateless, horizontally scalable behind a load balancer. Each instance uses async I/O (Node.js event loop or Go goroutines) so thousands of concurrent requests share a small thread pool. Auto-scaling triggers on CPU and P99 latency metrics. At peak we deploy ~500 PDP SSR pods across 3 AZs.

Q2: What happens if the Pricing Service is down?
A: The circuit breaker opens after detecting >50% failure rate. Subsequent PDP requests use the last-known price from Redis (TTL 60 s). The page renders with a disclaimer: "Price last updated at [timestamp]." This avoids showing stale prices indefinitely while not blocking page load. We also emit a PagerDuty alert so the pricing team is notified within 1 minute.

Q3: The page is SSR, but personalization varies per user. How do you cache efficiently?
A: We split the page into a cacheable "product shell" (catalog, pricing, reviews — same for all users) and a "personalization sidecar" (Prime eligibility, wishlist state, purchase history badge). The shell is cached at CDN with a 30-second TTL. The sidecar is fetched client-side via a small XHR after the initial page load, keyed by session cookie. This pattern, called "fragment caching" or "edge-side includes (ESI)," lets us cache 95% of the page while personalizing the remaining 5%.

Q4: How do you handle the cold start problem for newly listed ASINs?
A: When a new ASIN is published, the Catalog Service emits a Kafka event. A "cache warmer" consumer pre-populates Redis with empty/default values for pricing, inventory, and aggregates. This ensures the first user request hits a warm cache path. For recommendation widgets, we fall back to category-level recommendations until the ASIN accumulates enough co-purchase data (typically 24–48 hours).

Q5: How does the PDP handle variant switching (e.g., changing color from Red to Blue)?
A: Each variant is a distinct child ASIN (e.g., B09XYZ1234 = Red/M, B09XYZ1235 = Blue/M). The parent ASIN stores a mapping of variation attributes to child ASINs. When the user selects a different color, the client updates the URL to the child ASIN and fetches the new page. This is a full page navigation (preserves SEO and back-button behavior). Pricing, inventory, and images are per-child-ASIN, ensuring the user always sees accurate data for their selected variant.

---

### 6.2 Reviews & Ratings Aggregation

**Problem it solves:**
Reviews require two distinct access patterns: (a) fetching paginated reviews sorted by helpfulness/recency, and (b) serving the aggregate rating (average stars, histogram) on every PDP at high speed. The aggregate must reflect newly submitted reviews within ~60 seconds without scanning millions of rows per ASIN per request.

**Approach Comparison:**

| Approach | Aggregate Freshness | Read Latency | Write Complexity | Notes |
|---|---|---|---|---|
| COUNT/AVG query at read time | Real-time | High (full table scan per ASIN) | None | Unacceptable at 600 M ASINs × 15 M req/hr |
| Materialized view (DB-level) | Near-real-time (background refresh) | Low | Low | Requires DB support; refresh lag depends on DB engine |
| Async stream aggregation (Kafka + consumer) | ~5–30 s lag | Low (Redis lookup) | Medium | **Selected** |
| Lambda architecture (batch + speed layer) | Seconds (speed) + hours (batch) | Low | High | Overkill; Kafka streaming alone sufficient |
| Event Sourcing with CQRS | Real-time | Low | High | Good pattern but significant operational complexity for marginal gain |

**Selected Approach — Kafka Streaming Aggregation:**

```
Review submitted
      │
      ▼
Review Service validates + writes to Cassandra
      │
      ▼
Kafka topic: review-events
  { "event": "review_published",
    "asin": "B09XYZ1234",
    "marketplace_id": 1,
    "rating": 5,
    "review_id": "uuid" }
      │
      ▼
Rating Aggregator Consumer (Kafka consumer group: rating-agg)
  - Reads event
  - Executes atomic SQL update:
      UPDATE rating_aggregates
      SET
        total_reviews = total_reviews + 1,
        sum_ratings   = sum_ratings + :rating,
        count_5star   = count_5star + CASE WHEN :rating = 5 THEN 1 ELSE 0 END,
        -- ... other star counts
        average_rating = (sum_ratings + :rating)::decimal / (total_reviews + 1),
        updated_at    = NOW()
      WHERE asin = :asin AND marketplace_id = :marketplace_id;
  - On success: updates Redis cache:
      HSET rating:{asin}:{mkt_id} avg 4.3 total 12847 hist_5 8320 ...
      EXPIRE rating:{asin}:{mkt_id} 300   -- 5 min TTL
```

**Cassandra Schema Justification for Review Storage:**

Partition key `(asin, marketplace_id)` groups all reviews for a product on the same Cassandra node — a single partition read returns paginated results without cross-node scatter. Clustering key `(created_at DESC, review_id)` enables efficient time-ordered pagination. For "most helpful" sort, a secondary index or separate materialized view sorted by `helpful_count DESC` is maintained. Cassandra's tunable consistency (`QUORUM` for review writes to ensure durability across 3 replicas; `LOCAL_ONE` for reads to minimize latency) balances correctness with speed.

**Helpfulness Vote Idempotency:**

```
POST /v1/reviews/{review_id}/vote  { "vote": "helpful" }

1. Check Redis SET: voted_users:{review_id}  — O(1) SISMEMBER
2. If already voted: return 409 Conflict
3. Add to Redis SET: SADD voted_users:{review_id} {user_id}  — EXPIRE 90d
4. Async Kafka event: review-vote-events
5. Consumer: UPDATE reviews SET helpful_count = helpful_count + 1 ...
   (atomic, idempotent — Redis gate prevents double-counting)
```

**Interviewer Q&As:**

Q1: What if the Kafka consumer for rating aggregation falls behind during a product launch with thousands of simultaneous reviews?
A: Consumer lag is monitored via Kafka consumer group offsets. If lag exceeds 10,000 events, we scale out the consumer group horizontally. Because rating aggregates use `UPDATE ... SET count = count + 1` (additive), events are order-independent — parallelism is safe. We also use Kafka's `at-least-once` delivery with idempotent writes keyed on `review_id` to avoid double-counting if a consumer retries.

Q2: How do you prevent fake reviews at the data layer?
A: The Reviews Service enforces: (a) verified-purchase check — user_id must appear in the order history for that ASIN before submitting, (b) rate limit of 1 review/ASIN/user, (c) device fingerprint and IP reputation scoring at submission time. The ML fraud model runs asynchronously post-submission and can flip `status` from `published` to `removed` — triggering a compensating event on Kafka to decrement rating aggregates.

Q3: The average rating is stored as a precomputed value. What happens if the system crashes mid-update?
A: The Aurora rating_aggregates table uses a single `UPDATE` statement, which is atomic at the database level. Even if the consumer crashes mid-batch, Kafka's `at-least-once` semantics will re-deliver the event. The re-delivery is handled idempotently: we store `processed_review_ids` in a bloom filter (Redis) and skip already-applied events. Full correctness is guaranteed by a nightly batch job that recomputes aggregates from raw Cassandra data and reconciles against the Aurora table.

Q4: How do you serve reviews sorted by "most helpful" given Cassandra's append-only clustering?
A: We maintain a separate Cassandra materialized view (or a denormalized table) with partition key `(asin, marketplace_id)` and clustering key `(helpful_count DESC, review_id)`. Every helpfulness vote triggers an asynchronous update. Because Cassandra MVs update synchronously, we trade some write amplification for read efficiency. Alternatively, we use Elasticsearch — reviews are indexed by ASIN, and a query with `sort: helpful_count:desc` provides fast, flexible sorting without Cassandra MV overhead.

Q5: How does the review pagination cursor work to avoid skip-scan inefficiency?
A: Instead of OFFSET-based pagination (which requires Cassandra to scan and discard N rows), we use keyset pagination. The cursor encodes `(created_at, review_id)` of the last-seen review, base64-encoded. The next page query uses `WHERE (created_at, review_id) < (:last_created_at, :last_review_id)` which maps directly to Cassandra's clustering key comparison — a single-pass scan with no skip.

---

### 6.3 Pricing Service — Real-Time Buy-Box Selection

**Problem it solves:**
A single ASIN can have offers from dozens of sellers at different prices, shipping speeds, and conditions. The "buy box" is the featured offer displayed prominently. It must update within seconds of a price change, serve reads in <10 ms, and be consistent enough that the price shown on the PDP matches the price charged at checkout.

**Approach Comparison:**

| Approach | Staleness | Read Latency | Write Complexity | Notes |
|---|---|---|---|---|
| Query Aurora on every PDP request | Real-time | 5–20 ms (index scan) | None | At 115K RPS, creates Aurora hotspot on popular ASINs |
| Redis cache of current buy-box winner | ~5 s | <1 ms | Medium (cache invalidation) | **Selected for reads** |
| Precomputed in Kafka stream (streaming buy-box) | ~2–5 s | <1 ms (DynamoDB lookup) | High | Good alternative; adds complexity |
| CDN-cached price (long TTL) | Minutes | <1 ms | Low | Violates freshness requirement; price mismatch at checkout |

**Selected Approach:**

```
Price Change Event Flow:
  Seller updates offer price via Seller API
        │
        ▼
  Pricing DB (Aurora) — UPDATE pricing SET sale_price_cents = :new_price
        │
        ▼
  DB trigger OR application code emits Kafka event:
  { "asin": "...", "marketplace_id": 1, "seller_id": 123,
    "new_price_cents": 3799, "event": "price_updated" }
        │
        ▼
  Buy-Box Selector Consumer:
    1. Fetch all active offers for (asin, marketplace_id) from Aurora
    2. Apply buy-box algorithm:
       score = w1*(1/price) + w2*(fulfillment_type) + w3*(seller_rating)
       winner = offers.argmax(score)
    3. SET Redis HASH:
       HSET price:{asin}:{mkt_id}
         buy_box_seller_id  123
         list_price_cents   4999
         sale_price_cents   3799
         prime_eligible     1
         updated_at         1744214400
       EXPIRE price:{asin}:{mkt_id}  30   -- 30-second TTL fallback
```

The Redis cache is authoritative for PDP reads. A 30-second TTL ensures that even if the invalidation event is lost (Kafka producer failure), the cache expires and the next read triggers a cache-miss path that queries Aurora directly. This prevents permanent stale pricing.

**Price Consistency at Checkout:**
When the user clicks "Add to Cart," the Cart Service re-validates the price against Aurora (bypassing Redis cache). If the price changed between page load and add-to-cart (>5 s is possible), the cart shows a "Price updated since you last viewed this item" message with the current price. This guarantees the charged price is never stale.

**Interviewer Q&As:**

Q1: What prevents the buy-box selector from creating a thundering herd on Aurora when thousands of price changes fire simultaneously during a competitive pricing event?
A: The buy-box selector consumer uses a debounce pattern: when multiple price-change events arrive for the same ASIN within a 500 ms window (detected via Kafka key-based compaction), they are collapsed into a single buy-box recalculation. Additionally, the consumer maintains an in-memory LRU cache of recent recalculations — if the same ASIN was processed within 2 seconds, it skips the Aurora query and reuses the result. This reduces Aurora load by ~80% during bulk reprice events.

Q2: How do you handle the scenario where a seller submits a maliciously low price (e.g., $0.01) due to a bug?
A: The Pricing Service validates every incoming price against a floor-price rule (minimum price per category, stored in a rules engine). If the price is below floor, the update is rejected with a 422 error and an alert is sent to the seller. Additionally, the buy-box algorithm includes a sanity check: if the new winner price is >50% below the previous buy-box price, a human review flag is set and the old buy-box winner is retained for up to 30 minutes pending review.

Q3: How does the system ensure the price shown on the PDP matches the price charged at checkout, given caching?
A: This is the "price promise" problem. The PDP shows the Redis-cached price (up to 30 s stale). At checkout time, the Order Service performs a synchronous price re-check against Aurora with a `SELECT FOR SHARE` lock. If the price increased, the customer is notified before completing the order. If it decreased, the customer automatically gets the lower price. This window of inconsistency (max 30 s) is explicitly accepted in our SLA with sellers.

Q4: How do you scale the Buy-Box Selector Consumer to handle 578 price updates/second?
A: The Kafka topic is partitioned by `(asin % num_partitions)` — each partition handles a disjoint set of ASINs. With 64 partitions and a consumer group of 64 instances, each instance handles ~9 updates/second, well within its capacity. During peak events (competitive repricing), we scale the consumer group to 128 instances via Kubernetes HPA triggered by consumer group lag metrics.

Q5: What is the buy-box algorithm, and is it deterministic?
A: The algorithm is deterministic given the same inputs. Amazon's actual algorithm is proprietary, but a representative model: `score = (1/price) * 0.40 + (FBA_bonus) * 0.30 + (seller_rating/100) * 0.20 + (in_stock) * 0.10` where FBA_bonus = 1 if fulfilled by Amazon, 0.7 otherwise. The winner is `argmax(score)`. For equal scores, we break ties by seller_id (deterministic ordering). The weights are stored in a config service and can be A/B tested per marketplace without code changes.

---

## 7. Scaling

### Horizontal Scaling

- **PDP SSR Service**: Stateless; deploy on Kubernetes with HPA triggered at 70% CPU. Peak capacity: 500 pods × 100 RPS/pod = 50,000 RPS per region. 3 regions (US, EU, APAC) = 150,000 RPS globally.
- **Reviews Service**: Stateless Go service; scales independently. Read replicas of Cassandra absorb read traffic; only writes go to the primary ring.
- **Pricing Service**: Stateless; scales to 200 pods. Redis cluster handles all reads.
- **Kafka Consumers**: Scale by adding consumer instances up to the partition count (64–256 partitions per topic).

### DB Sharding

- **Cassandra (Reviews)**: Natively sharded via consistent hashing on the partition key `(asin, marketplace_id)`. A 12-node cluster with replication factor 3 handles the write/read throughput. Automatic token rebalancing on node addition.
- **Aurora MySQL (Pricing, Sellers)**: No sharding needed at current seller scale (~5 M sellers). Aurora supports 15 read replicas; writes go to the primary writer. If sharding becomes necessary, shard by `seller_id % N` (sellers table) and `asin_hash % N` (pricing table).
- **DynamoDB (Catalog, Q&A, Recommendations)**: Natively partitioned by DynamoDB; managed by AWS. Monitor for hot partitions (popular ASINs) and use DynamoDB DAX for microsecond caching.
- **TimescaleDB (Price History)**: Hypertable partitioned by `recorded_at` (weekly chunks). Space partitioning by `asin_hash` prevents single-node hotspots for popular ASINs. Retention policy: drop chunks older than 2 years.

### Replication

- **All Aurora clusters**: Multi-AZ synchronous replication (3 AZs) for HA. Asynchronous cross-region replica for the DR region (RPO ≤ 1 s).
- **Cassandra**: RF=3 within a datacenter (NetworkTopologyStrategy). RF=3 in a second datacenter for multi-region reads.
- **Redis Cluster**: 3 primary shards × 1 replica each. Sentinel for automatic failover within 15 s. Redis is NOT the source of truth — cache misses fall through to Aurora/DynamoDB.

### Caching

| Layer | Technology | TTL | Eviction | What's Cached |
|---|---|---|---|---|
| L1 (in-process) | Go/Node in-memory LRU (ristretto) | 500 ms | LRU, 256 MB/pod | Hot product catalog structs |
| L2 (distributed) | Redis Cluster | 5–300 s | allkeys-lru | Prices, inventory counts, rating aggregates, recs |
| L3 (CDN edge) | CloudFront | 30 s (anon HTML), 86400 s (images) | LRU | SSR HTML (anonymous), all static assets |
| Application DB read replicas | Aurora read replicas | N/A | N/A | Offloads SELECT queries from primary |

Cache invalidation strategy: **Event-driven invalidation via Kafka**. When a product attribute changes, the Catalog Service publishes a `catalog-updated` event. A cache-invalidator consumer calls `DEL pdp:html:{asin}:*` in Redis and issues a CloudFront soft-purge for `/dp/{asin}`. TTL acts as a safety net.

### CDN

- CloudFront distributions in 20+ global PoPs.
- **SSR HTML**: Cached at edge with `Cache-Control: public, max-age=30, stale-while-revalidate=60`. Anonymous users get edge-served pages. TTL of 30 s balances freshness (pricing, inventory) with offload ratio.
- **Product images**: Immutable content served with `Cache-Control: public, max-age=31536000, immutable`. Image URLs include a content hash — any image update gets a new URL, so old URLs remain cached indefinitely.
- **A+ content HTML**: Cached for 5 minutes; invalidated on seller update via CloudFront invalidation API.

### Interviewer Q&As

Q1: At 15 M page views/hour (Prime Day peak), what is the cache hit ratio needed at CDN to keep origin traffic manageable?
A: 15 M/hr = ~4,167 RPS. With a CDN hit ratio of 90%, origin traffic = 417 RPS. Our origin fleet (500 pods × 100 RPS = 50,000 RPS capacity) handles this with enormous headroom. The 90% hit ratio is achievable because during Prime Day, traffic concentrates on a small set of "deal ASINs" — the long-tail effect is reversed. In practice, top 1,000 deal ASINs absorb 70% of traffic; those pages have near-100% CDN hit ratio after the first second.

Q2: How do you handle cache stampede when a popular ASIN's cache expires?
A: We use the "probabilistic early expiration" technique (also called "fetch-ahead"): when remaining TTL < 20% of original TTL, a random 10% of requests trigger a background refresh (non-blocking) while serving the stale value. This distributes the recomputation load over a window instead of all traffic hitting the origin simultaneously. Additionally, we use Redis `SETNX` with a lock key to allow only one origin request to execute when a cache miss occurs; all other concurrent requests wait (with a timeout fallback to direct Aurora query).

Q3: How do you shard Cassandra as reviews grow beyond the current 12-node cluster?
A: Cassandra's consistent hashing means adding nodes triggers automatic token rebalancing (streaming data from existing nodes to new ones). We add nodes in pairs (to maintain RF=3). The `nodetool status` command tracks rebalancing progress. Zero-downtime scaling: applications continue serving from existing nodes; new nodes gradually absorb their token ranges over hours. We target keeping each node below 1 TB of data and <60% disk utilization to maintain rebalancing headroom.

Q4: Why use Redis Cluster over a single Redis instance with replicas for the L2 cache?
A: A single Redis instance is limited to ~100 GB memory (practical limit per node) and ~1 M ops/s. At our scale (600 M ASINs × multiple cached fields), we need multi-TB cache capacity. Redis Cluster shards data across multiple primary nodes using hash slot partitioning (16,384 slots). With 32 primary nodes × 3 TB each = 96 TB total cache. Write throughput scales linearly with node count. Failure of a single shard only affects 1/32 of the keyspace, limiting blast radius.

Q5: How do you handle read traffic to the Pricing Service during a Redis cluster failover (15-second window)?
A: The Pricing Service has a per-pod in-process L1 cache (ristretto, 256 MB, 5 s TTL) as a fallback. During the 15-second Redis failover, L1 serves pricing reads. L1 hit ratio for hot ASINs (top 1 M) is ~70% even during normal operation. For the remaining 30% that miss L1 during Redis outage, requests fall through to Aurora read replicas — these handle the increased load since Pricing reads are simple PK lookups with sub-5 ms latency.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | Recovery |
|---|---|---|---|---|
| Redis cluster node failure | L2 cache partial degradation (1/32 keyspace) | Sentinel health checks, CloudWatch metrics | Automatic replica promotion (15 s); L1 in-process cache absorbs heat | Automatic; monitoring alert fired |
| Aurora primary failure | No new price/review writes; reads continue on replicas | Aurora health check, connection pool errors | Aurora Multi-AZ auto-failover (20–30 s); application retries with exponential backoff | RTO ~30 s, RPO ~0 (sync replica) |
| Cassandra node failure | 1/N of review data temporarily less available | nodetool status; health probes | RF=3 means 2 surviving replicas serve reads at QUORUM; writes continue | Auto-repair via nodetool repair; replace node in <1 hr |
| PDP SSR Service pod crash | Traffic lost on that pod | K8s liveness probe (3 failed → restart) | Load balancer health check removes pod; traffic reroutes to healthy pods in <10 s | K8s restarts pod automatically |
| Kafka broker failure | Delayed event processing (inventory, price, reviews) | Kafka broker metrics, consumer lag | Kafka replication (RF=3); consumers reconnect to live broker; messages durable on surviving brokers | No message loss; brief lag spike |
| Pricing Service complete outage | PDP shows stale price | Circuit breaker trips at >50% errors | Serve last-known Redis price; display staleness timestamp | Page degrades gracefully; PagerDuty alert |
| CDN PoP outage | Users in affected region see high latency | CloudFront health metrics, Synthetic monitors | DNS failover to next nearest PoP via Route 53 latency routing | Automatic via Route 53, <60 s |
| DDoS on product pages | Origin overload | CloudFront request rate spike, WAF alerts | AWS Shield Advanced + WAF rate-limit rules; block anomalous IPs at edge | Automatic; WAF rules updated in real-time |
| Downstream service slow (>100 ms) | PDP latency SLA breach | Distributed tracing, P99 dashboards | Per-service timeout + circuit breaker; partial page served with fallbacks | Circuit breaker opens; reduced scope response |

### Failover Strategy

- **Active-Active Multi-Region**: US-East-1, EU-West-1, AP-Southeast-1 serve traffic simultaneously via Route 53 latency-based routing. Each region is an independent stack (its own Aurora, Cassandra, Redis).
- **Data Replication**: Cassandra multi-datacenter replication (LOCAL_QUORUM writes to 2 DCs). Aurora cross-region read replica (async, RPO <1 s). DynamoDB Global Tables (active-active, <1 s lag).
- **Regional Failover**: If a region's error rate exceeds 5%, Route 53 health checks trigger weighted failover to the remaining regions within 60 seconds.

### Retries and Idempotency

- All service-to-service calls use exponential backoff: `delay = min(base * 2^attempt + jitter, max_delay)` where `base=100ms`, `max_delay=5s`, `jitter=rand(0, 100ms)`.
- **Maximum retries**: 3 attempts for read calls; 1 retry for write calls (writes are idempotent via `Idempotency-Key` header or DB UPSERT).
- **Review submission idempotency**: `INSERT ... ON CONFLICT (asin, user_id) DO NOTHING` — guaranteed single review per user per ASIN regardless of retries.
- **Price update idempotency**: `INSERT INTO pricing ... ON CONFLICT (asin, marketplace_id, seller_id, valid_from) DO UPDATE SET sale_price_cents = EXCLUDED.sale_price_cents`.

### Circuit Breaker Configuration

```
Per-service circuit breaker settings (Hystrix/Resilience4j):
  - Sliding window size: 10 seconds
  - Failure rate threshold: 50% (opens circuit)
  - Slow call rate threshold: 80% calls > timeout (opens circuit)
  - Wait duration in open state: 30 seconds
  - Permitted calls in half-open state: 5
  - Fallback: return last-cached value or empty/default
```

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Tool |
|---|---|---|---|
| PDP SSR latency P99 | Histogram | >200 ms → Warning; >500 ms → Page | Prometheus + Grafana |
| PDP error rate (5xx) | Counter/Rate | >0.1% → Warning; >1% → Page | Prometheus |
| CDN cache hit ratio | Gauge | <85% → Warning | CloudFront Metrics + Grafana |
| Redis cache hit ratio | Gauge | <80% → Warning | Redis INFO + Prometheus exporter |
| Kafka consumer lag (per topic) | Gauge | >10,000 events → Warning | Confluent Control Center + Prometheus |
| Aurora replication lag | Gauge | >1 s → Warning; >5 s → Page | CloudWatch RDS + Grafana |
| Cassandra read/write latency P99 | Histogram | >50 ms → Warning | Cassandra JMX + Prometheus |
| Price update processing latency | Histogram | >10 s end-to-end → Warning | Custom metric via Kafka event timestamp delta |
| Rating aggregate staleness | Gauge | >120 s → Warning | Custom metric: (NOW - last_updated) per ASIN |
| Circuit breaker state | Enum (0=closed, 1=open) | Any open → Page | Custom metric |
| PDP revenue proxy (add-to-cart rate) | Rate | >20% drop → Page (business metric) | Custom analytics pipeline |

### Distributed Tracing

- **Technology**: OpenTelemetry SDK in all services, exported to Jaeger (or AWS X-Ray in cloud-native setup).
- **Trace propagation**: W3C `traceparent` header injected at API Gateway; propagated through all service calls including Kafka (header-based).
- **Sampling**: 1% random sampling in steady state; 100% sampling when `X-Debug-Trace: true` header is present (for debugging specific ASINs).
- **Key spans tracked**: `pdp_ssr_total`, `catalog_service_get`, `pricing_service_get`, `redis_get`, `cassandra_read`, `aurora_query`, `kafka_publish`, `kafka_consume`.
- **Latency breakdown example**: For a P99 > 200 ms alert, engineers query Jaeger for traces with `pdp_ssr_total > 200ms`, identify the slowest child span (often the Catalog Service DB query), and drill into the Aurora slow query log.

### Logging

- **Structured JSON logs** (all services) with fields: `timestamp`, `service`, `asin`, `user_id` (hashed for PII), `trace_id`, `span_id`, `level`, `message`, `latency_ms`, `status_code`.
- **Log aggregation**: Fluentd → Elasticsearch (ELK) + S3 (long-term archive, 90-day retention).
- **Log levels**: ERROR (alerts), WARN (degradation events), INFO (business events like review submitted), DEBUG (disabled in production; enabled per-pod via config flag without restart).
- **PII policy**: `user_id` is logged as SHA256(user_id + daily_salt) to correlate within a day without exposing raw IDs. IP addresses truncated to /24 subnet.
- **Access logs**: API Gateway logs all requests to S3 via Kinesis Firehose; used for abuse detection and capacity planning.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Alternative Considered | Why Selected | Trade-off Accepted |
|---|---|---|---|
| SSR (Server-Side Rendering) for PDP | CSR (Client-Side Rendering) | SEO crawlability; Google CWV compliance; faster first contentful paint for cold visitors | Higher server-side compute cost; personalization requires client-side supplementation |
| Cassandra for Reviews | PostgreSQL with partitioning | Write scalability (millions/day); partition-key model = perfect schema fit; multi-DC replication | No SQL joins; aggregate queries require separate store; operational complexity higher than Postgres |
| Redis as primary pricing read store | Direct Aurora on every PDP request | Sub-millisecond reads; Aurora cannot sustain 115K RPS price lookups without connection pooling limits | Price can be up to 30 s stale; requires cache invalidation pipeline |
| CDN HTML caching (30 s TTL, anonymous) | No CDN HTML caching | Offloads 90%+ of origin traffic; massive cost and latency reduction | Anonymous users may see 30 s stale pricing/inventory in the HTML — acceptable for display purposes |
| TimescaleDB for Price History | ClickHouse or InfluxDB | PostgreSQL compatibility (team familiarity); native hypertables; continuous aggregates | Less suited for petabyte-scale analytics vs. ClickHouse; requires Postgres operational expertise |
| Event-driven rating aggregation (Kafka) | Real-time DB trigger | Decouples review writes from aggregate computation; fan-out friendly; scalable | Up to 30 s lag in rating updates; requires Kafka operational expertise |
| Fragment caching (shell + personalization sidecar) | Full-page personalized cache | Maximizes CDN cache hit ratio while enabling personalization | Slight client-perceived jitter when personalization sidecar loads (mitigated by skeleton UI) |
| Parallel fan-out with per-service timeouts | GraphQL federation | Simpler implementation; no GraphQL layer overhead; easier debugging | Each new widget requires PDP SSR Service code change; tightly couples PDP to widget existence |
| DynamoDB for Q&A | MySQL/Aurora | Flexible schema (varying answer counts); no relational queries needed; managed scalability | No full-text search natively (use Elasticsearch for Q&A search); eventual consistency |

---

## 11. Follow-up Interview Questions

**Q1: How would you design the A/B testing framework for recommendation widgets on the PDP?**
A: Each recommendation widget request includes a user's assigned experiment bucket (stored in a cookie or the JWT claim). The Recommendations Service accepts a `experiment_id` parameter, fetches the appropriate model variant's pre-computed results from a DynamoDB table keyed by `(asin, experiment_id, widget_type)`, and returns them. The PDP SSR Service emits impression events to Kafka with the experiment_id, which flows into the analytics pipeline for significance testing. The framework supports multi-arm bandits (auto-adjust traffic allocation based on CVR signals) in addition to fixed A/B splits.

**Q2: How would you handle a scenario where a popular ASIN goes viral (10× normal traffic in 30 seconds)?**
A: The CDN absorbs the first wave. For cache misses: (a) the page-level Redis cache (TTL 30 s) means at most 1 origin request per 30 s per CDN PoP; (b) Kubernetes HPA scales PDP SSR pods on CPU — response time is ~60 s (cold start); (c) a "pre-scaling" trigger fires when social media trending signals (detected via a streaming pipeline reading Twitter/X API) cross a threshold, warming the cache and pre-scaling pods before the traffic spike arrives.

**Q3: How do you handle multi-currency and localized pricing across 20 marketplaces?**
A: Each marketplace has its own `marketplace_id`, and pricing rows are independent per marketplace. Exchange rates are fetched hourly from a financial data provider and stored in a `fx_rates` table. The Pricing Service stores all prices in the local marketplace currency (not USD), so no runtime conversion is needed on the read path. The buy-box selection algorithm normalizes to a common unit only for cross-marketplace analytics (offline job).

**Q4: How would you design the price drop alert feature (notify users when price drops below their target)?**
A: Users set a price alert via `POST /v1/dp/{asin}/price-alerts { target_price_cents: 2999 }`. Alerts are stored in DynamoDB: `alerts:{asin}` → list of `(user_id, target_price_cents)`. When a price-change Kafka event fires, an "Alert Evaluator" consumer queries the alert list for the ASIN and compares new price against all targets. Matching alerts are enqueued to a notification Kafka topic, which feeds into the Notification Service (email/push). Because popular ASINs could have millions of alerts, the evaluation uses a sorted set (by target price) — a single range query `ZRANGEBYSCORE alerts:{asin} 0 {new_price}` returns all triggered alerts in O(log N + K).

**Q5: How do you ensure the "Only 3 left in stock" message is accurate?**
A: The `inventory.quantity_available` field in Redis is updated within 30 seconds of actual inventory changes via Kafka events from the Fulfillment Center systems. The display threshold (show warning if qty ≤ 10) is a client-side decision — the API returns the actual quantity, and the UI applies the threshold. For accuracy: (a) we show the warning conservatively (if Redis says 4 but a Kafka event is in-flight, we might show 5 briefly — acceptable); (b) the inventory reserved for pending orders is subtracted from `quantity_available` via the Inventory Service before publishing to Kafka.

**Q6: How would you design the Seller Storefront page?**
A: The Seller Storefront aggregates all listings for a seller (`SELECT * FROM products WHERE seller_id = X`). This query is served from a precomputed DynamoDB table (`seller_listings:{seller_id}`) updated by a Kafka consumer whenever a seller's listing is added/removed. The storefront page includes seller ratings, which are aggregated similarly to product ratings — a separate `seller_rating_aggregates` table updated via streaming.

**Q7: How does search engine crawling interact with your caching strategy?**
A: Googlebots are identified by User-Agent. API Gateway applies a separate rate limit for crawlers (100 req/min per IP). Crawlers bypass the personalization layer (always get anonymous SSR HTML). The CDN serves the same 30-second cached HTML to Googlebots as to anonymous users, keeping crawl costs low. We submit an XML sitemap (auto-generated from DynamoDB catalog, refreshed daily) to accelerate crawl discovery of new ASINs. For SEO signals, structured data (Schema.org Product JSON-LD) is embedded in the SSR HTML.

**Q8: How would you implement "Recently Viewed Items" on the PDP?**
A: The browser records the last 20 ASINs viewed in a first-party cookie or localStorage (client-side, no server round-trip). On PDP load, the client sends a `GET /v1/recommendations/recently-viewed?asins=B09...,B08...` request, which fetches lightweight product summaries (title, image, price) for those ASINs from the Catalog + Pricing services. This is entirely client-side driven — no server-side state needed, which avoids user tracking concerns while delivering the feature.

**Q9: How would you handle product page content in multiple languages?**
A: Each marketplace has its own content records in DynamoDB (partition key includes `marketplace_id`). Translations are stored as separate rows — not as JSON fields within a single row — to enable independent update lifecycles. A Translation Management System (TMS) handles the translation workflow; translated content is published to DynamoDB via the same Catalog Service pipeline. Machine translation (fallback) is triggered when no human translation exists, marked with `translation_source: machine` for quality tracking.

**Q10: What is the impact of Cassandra's eventual consistency model on the review count shown on the PDP?**
A: Cassandra with `QUORUM` write consistency (write to 2/3 replicas before acknowledging) ensures a review is durably stored before the client receives success. However, the `rating_aggregates` table (in Aurora) is updated asynchronously via Kafka — with up to 30 s lag. So the review count on the PDP may lag by up to 30 s after a new review is published. This is explicitly accepted in our design (SLA: review count eventually consistent within 60 s). To minimize surprise, the PDP review section says "X,XXX+ reviews" (using a floor) rather than showing an exact count that might decrement confusingly if we ever recompute.

**Q11: How would you architect the image processing pipeline for product images?**
A: Sellers upload original images via a pre-signed S3 URL. An S3 event triggers a Lambda function (or an image processing service) that: (1) validates format (JPEG/PNG, minimum 1000×1000 px, no watermarks — checked via a CV model), (2) generates 5 standard thumbnail sizes (75×75, 160×160, 500×500, 1000×1000, 2000×2000), (3) converts to WebP with JPEG fallback, (4) uploads thumbnails to S3 with immutable content-hash filenames, (5) updates the DynamoDB catalog record with the image keys. CloudFront serves all images; origin = S3. The pipeline is idempotent (same input image → same content hash → same S3 key, no duplicate storage).

**Q12: How do you prevent review bombing (a coordinated attack of 1-star reviews)?**
A: Multi-layered defenses: (a) Rate limiting: max 1 review per ASIN per user, max 5 reviews per day per user (enforced at API layer). (b) Velocity detection: a streaming anomaly detector (Flink job) monitors review submission rates per ASIN — if 3-sigma above baseline within 1 hour, a hold is placed on new reviews pending ML scoring. (c) ML review quality model: runs within 200 ms of submission and assigns a quality score; low-quality reviews enter a moderation queue before publication. (d) Verified-purchase requirement: for categories with high review abuse history, only verified purchasers can submit reviews.

**Q13: How would you handle a product recall scenario where all inventory must be immediately hidden?**
A: The Catalog Service exposes an internal admin API `PATCH /internal/dp/{asin}/status { "is_active": false }`. This update is written to DynamoDB and emits a `catalog-deactivated` Kafka event. Consumers: (a) CDN invalidator purges all cached HTML for the ASIN immediately, (b) Search index remover de-indexes the ASIN from Elasticsearch within seconds, (c) Inventory Service marks all warehouse stock as `quarantined`. Because CDN invalidation is near-instantaneous (CloudFront API) and the page cache TTL is 30 s max, the ASIN disappears from all user-facing surfaces within ~30 seconds end-to-end.

**Q14: Describe how you would build the price history chart widget on the PDP.**
A: The widget fetches `GET /v1/dp/{asin}/price-history?range=90d`. The response is a list of `(timestamp, price_cents)` tuples. TimescaleDB's continuous aggregate materializes hourly average prices, making this query a scan over a tiny pre-aggregated rollup table rather than raw time-series rows. The chart is rendered client-side (using D3.js or Chart.js) after initial page load, preventing it from blocking the critical path. The API response is cached in Redis for 5 minutes — price history changes only when price changes, and a price-change Kafka event invalidates the cache immediately.

**Q15: How would you design the "Compare with similar items" feature?**
A: The Compare feature is built on the same Recommendations Service. A `GET /v1/dp/{asin}/recommendations?widget=similar&limit=4` returns 4 comparable ASINs. The comparison table is built client-side by fetching the full `technical_specs` JSONB field from the Catalog Service for each ASIN and rendering a unified spec table. The challenge is heterogeneous specs (ASIN A has "RAM" field, ASIN B calls it "Memory") — solved by a "spec normalization" offline ETL job that maps raw spec keys to canonical spec keys per category (e.g., `ram`, `memory`, `RAM` all map to `memory_gb` in the Electronics category). The normalized specs are stored in a separate DynamoDB attribute `canonical_specs` and used exclusively by the comparison widget.

---

## 12. References & Further Reading

1. **Werner Vogels, "Eventually Consistent" (2008)** — ACM Queue, Vol. 6, No. 6. Foundational paper for understanding consistency trade-offs in distributed systems like Cassandra. https://dl.acm.org/doi/10.1145/1466443.1466448

2. **Amazon DynamoDB: A Scalable, Predictably Performant, and Fully Managed NoSQL Database Service (2022)** — USENIX ATC '22. Describes the DynamoDB architecture used for product catalog and Q&A storage. https://www.usenix.org/conference/atc22/presentation/vig

3. **Dynamo: Amazon's Highly Available Key-Value Store (2007)** — Amazon / SOSP '07. The precursor to DynamoDB; explains consistent hashing, vector clocks, and eventual consistency. https://dl.acm.org/doi/10.1145/1294261.1294281

4. **Cassandra: A Decentralized Structured Storage System (2010)** — Lakshman & Malik, SIGOPS. Covers the wide-column model and the partition-key design that makes Cassandra ideal for reviews. https://dl.acm.org/doi/10.1145/1773912.1773922

5. **TimescaleDB: Time-Series Data Storage for PostgreSQL (2017)** — Freedman et al. Technical documentation and blog posts covering hypertables and continuous aggregates. https://docs.timescale.com/

6. **Redis Cluster Specification** — Official Redis documentation describing hash slot partitioning and node failure handling. https://redis.io/docs/reference/cluster-spec/

7. **The Twelve-Factor App** — Herwig, Wiggins. Architectural principles behind stateless, scalable services like the PDP SSR Service. https://12factor.net/

8. **Building Microservices (2nd Edition)** — Sam Newman, O'Reilly 2021. Covers service decomposition, circuit breakers, and the strangler fig pattern referenced in scaling strategy.

9. **Netflix Hystrix Circuit Breaker** — Netflix OSS. The circuit breaker pattern applied to PDP fan-out calls. https://github.com/Netflix/Hystrix/wiki/How-it-Works

10. **Google Web Vitals — Core Web Vitals Thresholds** — The LCP and FID thresholds that drive the <200 ms SSR latency requirement. https://web.dev/vitals/

11. **Designing Data-Intensive Applications** — Martin Kleppmann, O'Reilly 2017. Chapters 5 (replication), 6 (partitioning), and 11 (stream processing) are directly applicable to this design.

12. **Amazon CloudFront Developer Guide — Cache Invalidation** — Documents the CDN invalidation API used for product recall and price change propagation. https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/Invalidation.html

13. **Flink: Stateful Computations over Data Streams** — Apache Flink documentation. Used in the review velocity anomaly detection pipeline. https://flink.apache.org/

14. **OpenTelemetry Specification** — CNCF. The distributed tracing standard used across all PDP services. https://opentelemetry.io/docs/specs/otel/

15. **AWS Aurora — Multi-AZ Deployments and Read Replicas** — Documents the failover behavior (RTO ~30 s) and replica lag characteristics. https://docs.aws.amazon.com/AmazonRDS/latest/AuroraUserGuide/Aurora.Replication.html
