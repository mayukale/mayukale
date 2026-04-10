# System Design: Yelp / Nearby Places

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Business Search**: Allow users to search for businesses by keyword, location, category, price range, and attributes (e.g., outdoor seating, pet-friendly).
2. **Business Profile**: Display full business details — name, address, phone, website, hours, photos, attributes, menu, and aggregate ratings.
3. **Review System**: Authenticated users can write, edit, and delete reviews with a star rating (1–5), text body, photos, and date. Reviews are visible to all users.
4. **Rating Aggregation**: Automatically compute and display aggregate star rating and review count for each business.
5. **Photo Storage & Display**: Users and business owners can upload photos. Photos are displayed on business profiles and review pages. Support cover photo selection.
6. **Check-Ins**: Users can check in to a business. Check-in counts are displayed publicly. Users see check-in history in their profiles.
7. **Recommendation Engine**: Surface personalized or trending businesses based on user history, popularity, and relevance to the query.
8. **Business Hours & Attributes Filtering**: Filter search results by currently open businesses; filter by attributes (accepts credit cards, has WiFi, etc.).
9. **Business Owner Tools**: Business owners can claim/manage their profile, respond to reviews, add photos, and update hours/attributes.
10. **User Profiles**: Users have profiles showing their review history, check-in count, photos uploaded, and "useful/funny/cool" votes received.

### Non-Functional Requirements

- **Availability**: 99.99% uptime for search and profile reads; 99.9% for write operations (reviews, check-ins).
- **Search Latency**: p99 ≤ 200 ms for search queries; p50 ≤ 60 ms.
- **Review Write Latency**: p99 ≤ 500 ms (user-submitted review persisted and visible).
- **Photo Upload Latency**: p99 ≤ 2 s for photo upload acknowledgment (async processing acceptable).
- **Scalability**: 100 M DAU; 10 M business listings globally; 500 M total reviews; 10 B photos stored.
- **Consistency**: Review counts and aggregate ratings: eventual consistency within 30 s. Individual reviews: read-your-writes (a user sees their own review immediately after submitting).
- **Durability**: Reviews and photos must never be lost. Reviews are the core business asset.
- **Search Freshness**: New businesses and reviews appear in search within 60 s of creation.
- **Content Moderation**: Spam/fake reviews detected and filtered within 24 h.
- **GDPR/CCPA Compliance**: Users can export and delete their data within 30 days.

### Out of Scope

- Full e-commerce (ordering food, booking reservations) — these integrate via external APIs.
- Real-time chat between users or between user and business owner.
- Full social network features (following friends' activity feeds at Twitter scale).
- Fraud detection (though noted as a future component).
- Delivery and logistics tracking.

---

## 2. Users & Scale

### User Types

| Type | Description | Primary Operations |
|---|---|---|
| Consumer | Searches for, reads reviews of, and checks in to businesses | Read: search, profile, reviews; Write: review, check-in, photos |
| Business Owner | Manages their business listing | Write: hours, photos, owner response to reviews |
| Reviewer | Power user who writes many reviews ("Elite" user on Yelp) | Write: reviews, photos; Read: all |
| Anonymous User | Browses without logging in | Read-only: search, profile, reviews |
| Content Moderator | Internal team enforcing review policies | Admin: flag, remove, edit reviews |
| Search Bot (crawler) | External search engines indexing business pages | Read: business profile pages |

### Traffic Estimates

**Assumptions**:
- 100 M DAU, 300 M MAU.
- Average consumer: 3 page views/day (search + 2 profile views).
- 5% of DAU write a review per day → 5 M reviews/day.
- 10% of DAU check in per day → 10 M check-ins/day.
- 2% of DAU upload a photo → 2 M photo uploads/day.
- Peak factor: 3× (Friday/Saturday evenings — peak restaurant searching).
- 10 M businesses globally; average 50 reviews/business = 500 M total reviews.
- Average photo size after processing: 500 KB (web-optimized JPEG).

| Metric | Calculation | Result |
|---|---|---|
| Search RPS (avg) | 100 M × 3 / 86 400 | ~3 500 RPS |
| Search RPS (peak) | 3 500 × 3 | ~10 500 RPS |
| Profile page RPS (avg) | 100 M × 2 / 86 400 | ~2 300 RPS |
| Review write RPS (avg) | 5 M / 86 400 | ~58 RPS |
| Review write RPS (peak) | 58 × 3 | ~174 RPS |
| Check-in write RPS | 10 M / 86 400 | ~116 RPS |
| Photo upload RPS | 2 M / 86 400 | ~23 RPS |
| Rating recomputation events/s | 174 reviews/s trigger rating recompute | ~174 events/s |
| Read/write ratio | (3 500 + 2 300) : (58 + 116 + 23) | ~29:1 (heavily read-dominant) |

### Latency Requirements

| Operation | p50 Target | p99 Target | Reason |
|---|---|---|---|
| Business search | 40 ms | 200 ms | Core product experience |
| Business profile load | 30 ms | 150 ms | High-traffic page |
| Reviews page (first page) | 40 ms | 200 ms | Critical for trust/decision |
| Review submission | 200 ms | 500 ms | User expects confirmation |
| Photo upload ACK | 200 ms | 2 000 ms | Async processing acceptable |
| Check-in write | 100 ms | 300 ms | Background action |
| Rating aggregate refresh | 5 s | 30 s | Eventual; user tolerates small lag |

### Storage Estimates

| Data Type | Calculation | Raw Size | Replicated (3×) |
|---|---|---|---|
| Business records | 10 M × 2 KB | 20 GB | 60 GB |
| Reviews (text) | 500 M × 500 B avg | 250 GB | 750 GB |
| Review metadata | 500 M × 200 B | 100 GB | 300 GB |
| Ratings aggregate | 10 M businesses × 100 B | 1 GB | 3 GB |
| Photos (processed, S3) | 10 B photos × 500 KB | 5 PB | 15 PB (multi-region) |
| Photo thumbnails (3 sizes) | 10 B × 3 × 50 KB | 1.5 PB | 4.5 PB |
| Check-ins | 10 M/day × 365 × 5 yr × 100 B | ~18 TB | 54 TB |
| User profiles | 300 M × 500 B | 150 GB | 450 GB |
| Search index (Elasticsearch) | 10 M businesses × 5 KB indexed doc | 50 GB | 150 GB (3 replicas) |
| Geospatial index | ~500 MB in Redis | ~500 MB | 1.5 GB |

### Bandwidth Estimates

| Flow | Calculation | Result |
|---|---|---|
| Search response egress | 10 500 RPS × 20 KB (10 results × 2 KB each) | 210 MB/s |
| Profile page egress | 2 300 RPS × 50 KB (text + metadata; photos served separately from CDN) | 115 MB/s |
| Photo egress from CDN | Assume 10 M photo views/day / 86 400 × 500 KB | ~58 GB/s total CDN egress |
| Review write ingress | 174 RPS × 2 KB | ~350 KB/s |
| Photo upload ingress | 23 RPS × 5 MB (original before processing) | ~115 MB/s |

---

## 3. High-Level Architecture

```
                     ┌──────────────────────────────────────────────┐
                     │                  CLIENTS                      │
                     │  iOS  │  Android  │  Web Browser  │  API SDK  │
                     └──────┬───────────┴──────┬─────────┴──────────┘
                            │                  │
                    ─────────▼──────────────────▼─────────
                    │    CDN (CloudFront / Fastly)         │
                    │  (photos, static assets, HTML shell) │
                    ──────────────────┬───────────────────
                                       │
                    ─────────────────── ▼──────────────────
                    │         API Gateway / Load Balancer   │
                    │    (Auth, Rate Limit, Request Router) │
                    ─────────────────────────────────────────
                              │          │          │
             ┌────────────────▼──┐  ┌────▼──────┐  ┌────────────▼──────────┐
             │   Search Service  │  │  Business │  │    Review Service      │
             │  (Elasticsearch + │  │  Profile  │  │  (CRUD reviews,        │
             │   geo-filter)     │  │  Service  │  │   voting, moderation)  │
             └─────┬─────────────┘  └─────┬─────┘  └────────────┬──────────┘
                   │                      │                      │
        ┌──────────▼──────────┐   ┌───────▼────────┐  ┌─────────▼──────────┐
        │  Elasticsearch      │   │  Business DB   │  │   Review DB         │
        │  (full-text + geo)  │   │  (PostgreSQL + │  │   (PostgreSQL,      │
        └─────────────────────┘   │   PostGIS)     │  │    sharded)         │
                                  └───────┬────────┘  └──────────┬──────────┘
                                          │                       │
                            ┌─────────────▼───────────────────────▼──────┐
                            │              Event Bus (Kafka)              │
                            │  Topics: review-created, rating-update,    │
                            │           check-in, photo-processed        │
                            └────────────┬──────────────┬────────────────┘
                                         │              │
                          ┌──────────────▼──┐  ┌────────▼─────────────┐
                          │  Rating Agg Svc  │  │  Search Indexer Svc  │
                          │  (recomputes     │  │  (writes business    │
                          │   star avg on    │  │   doc to ES on       │
                          │   review event)  │  │   change event)      │
                          └──────────────────┘  └──────────────────────┘
                                         │
                          ┌──────────────▼──────────────┐
                          │   Photo Service              │
                          │  (upload → S3 → Lambda      │
                          │   resize → CDN invalidate)  │
                          └─────────────────────────────┘
                                         │
                          ┌──────────────▼──────────────┐
                          │  Recommendation Engine       │
                          │  (offline: collaborative     │
                          │   filtering; online:         │
                          │   real-time feature store)  │
                          └─────────────────────────────┘
```

**Component Roles**:

- **CDN**: Serves photo thumbnails, original photos (presigned URLs), static web assets. Cache headers: `max-age=31536000` for photos (content-addressed by hash), shorter for profile HTML.
- **API Gateway**: JWT validation, API key auth, per-user rate limiting, WAF (block scraping), request routing to microservices.
- **Search Service**: Handles `GET /search` queries. Combines full-text Elasticsearch query with a geo-filter (using Elasticsearch geo_distance or geo_bounding_box). Applies business hours filter, category filter, attributes filter. Calls Recommendation Engine for personalized ranking boost.
- **Business Profile Service**: Returns full business detail including photos, current hours, aggregate rating. Reads from Business DB (source of truth) and caches in Redis.
- **Review Service**: Handles review creation, editing, deletion, voting (useful/funny/cool), and owner responses. Writes to Review DB, publishes `review-created` event to Kafka.
- **Rating Aggregation Service**: Kafka consumer on `review-created` / `review-deleted` / `review-updated` topics. Recomputes the business's aggregate rating and review count; writes to Rating Cache (Redis) and Business DB.
- **Search Indexer Service**: Kafka consumer on business-update and review-update topics. Maintains the Elasticsearch index up to date. Incremental indexing: only the changed document fields are re-indexed.
- **Photo Service**: Accepts photo uploads. Stores originals in S3. Triggers Lambda function to generate 3 thumbnail sizes (150×150, 400×300, 1200×900). Writes photo metadata to Photo DB. Publishes `photo-processed` event to Kafka (triggers CDN cache warming).
- **Recommendation Engine**: Offline component (Spark batch job running nightly) that produces user-business affinity scores using collaborative filtering (ALS on check-in + review matrix). Online component queries the Feature Store for real-time signals (current user session, trending businesses).

**Primary Use-Case Data Flow (Business Search)**:

1. User types "sushi near me" → app sends `GET /search?q=sushi&lat=37.77&lon=-122.41&radius=2000&open_now=true`.
2. API Gateway validates auth, rate-limits, routes to Search Service.
3. Search Service builds Elasticsearch query: `{bool: {must: [{multi_match: {query: "sushi", fields: ["name^3","category^2","description"]}}, {geo_distance: {distance: "2km", location: {lat:37.77,lon:-122.41}}}], filter: [{term: {is_active: true}}, {term: {open_now_bucket: true}}]}}`.
4. Elasticsearch returns top-100 candidate business IDs with BM25 text scores.
5. Search Service re-ranks with composite score (text score × distance decay × rating boost).
6. Fetch business summary (name, rating, price, photo thumbnail, distance) from Redis cache; cache miss falls back to Business DB.
7. Return JSON response with top-20 results + pagination cursor.

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────────
-- Business
-- ─────────────────────────────────────────────

CREATE TABLE business (
    business_id       UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    name              VARCHAR(256) NOT NULL,
    slug              VARCHAR(256) UNIQUE NOT NULL,  -- URL-friendly name
    lat               DOUBLE PRECISION NOT NULL,
    lon               DOUBLE PRECISION NOT NULL,
    geog              GEOGRAPHY(POINT, 4326) GENERATED ALWAYS AS
                          (ST_SetSRID(ST_MakePoint(lon, lat), 4326)::geography) STORED,
    address_line1     VARCHAR(256),
    address_line2     VARCHAR(256),
    city              VARCHAR(128) NOT NULL,
    state_province    VARCHAR(64),
    postal_code       VARCHAR(16),
    country_code      CHAR(2)      NOT NULL,
    phone             VARCHAR(32),
    website           VARCHAR(512),
    email             VARCHAR(256),
    category_id       INT          NOT NULL REFERENCES business_category(id),
    price_level       TINYINT,                       -- 1($) to 4($$$$)
    aggregate_rating  NUMERIC(3,2) NOT NULL DEFAULT 0,
    review_count      INT          NOT NULL DEFAULT 0,
    check_in_count    INT          NOT NULL DEFAULT 0,
    cover_photo_id    UUID         REFERENCES business_photo(photo_id) DEFERRABLE,
    hours_json        JSONB,                         -- OpenHours spec per day
    attributes_json   JSONB,                         -- { wifi: true, parking: "lot", ... }
    owner_user_id     UUID         REFERENCES user_account(user_id),
    is_claimed        BOOLEAN      NOT NULL DEFAULT FALSE,
    is_closed         BOOLEAN      NOT NULL DEFAULT FALSE,  -- permanently closed
    is_active         BOOLEAN      NOT NULL DEFAULT TRUE,
    created_at        TIMESTAMP    NOT NULL DEFAULT NOW(),
    updated_at        TIMESTAMP    NOT NULL DEFAULT NOW(),

    INDEX idx_geog          USING GIST (geog),
    INDEX idx_category      (category_id),
    INDEX idx_city_category (city, category_id),
    INDEX idx_rating        (aggregate_rating DESC),
    INDEX idx_slug          (slug)
);

CREATE TABLE business_category (
    id          SERIAL       PRIMARY KEY,
    name        VARCHAR(64)  UNIQUE NOT NULL,  -- "restaurant","bar","salon"...
    parent_id   INT          REFERENCES business_category(id),  -- hierarchy
    alias       VARCHAR(64)  UNIQUE NOT NULL   -- URL-safe alias
);

-- ─────────────────────────────────────────────
-- Reviews
-- ─────────────────────────────────────────────

-- Sharded by business_id range or hash for horizontal scaling
CREATE TABLE review (
    review_id       UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    business_id     UUID         NOT NULL REFERENCES business(business_id),
    user_id         UUID         NOT NULL REFERENCES user_account(user_id),
    rating          TINYINT      NOT NULL CHECK (rating BETWEEN 1 AND 5),
    text_body       TEXT,
    useful_count    INT          NOT NULL DEFAULT 0,
    funny_count     INT          NOT NULL DEFAULT 0,
    cool_count      INT          NOT NULL DEFAULT 0,
    is_visible      BOOLEAN      NOT NULL DEFAULT TRUE,  -- FALSE = removed by moderation
    owner_response  TEXT,
    owner_responded_at TIMESTAMP,
    created_at      TIMESTAMP    NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMP    NOT NULL DEFAULT NOW(),

    UNIQUE (business_id, user_id),  -- one review per user per business
    INDEX idx_business_created (business_id, created_at DESC),
    INDEX idx_user             (user_id),
    INDEX idx_business_rating  (business_id, rating)
);

-- Review votes (useful/funny/cool)
CREATE TABLE review_vote (
    id          BIGSERIAL    PRIMARY KEY,
    review_id   UUID         NOT NULL REFERENCES review(review_id),
    user_id     UUID         NOT NULL REFERENCES user_account(user_id),
    vote_type   VARCHAR(8)   NOT NULL CHECK (vote_type IN ('useful','funny','cool')),
    created_at  TIMESTAMP    NOT NULL DEFAULT NOW(),
    UNIQUE (review_id, user_id, vote_type)
);

-- ─────────────────────────────────────────────
-- Photos
-- ─────────────────────────────────────────────

CREATE TABLE business_photo (
    photo_id        UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    business_id     UUID         NOT NULL REFERENCES business(business_id),
    uploaded_by     UUID         REFERENCES user_account(user_id),
    review_id       UUID         REFERENCES review(review_id),  -- NULL if standalone
    s3_key_original VARCHAR(512) NOT NULL,
    s3_key_thumb_sm VARCHAR(512) NOT NULL,   -- 150×150
    s3_key_thumb_md VARCHAR(512) NOT NULL,   -- 400×300
    s3_key_thumb_lg VARCHAR(512) NOT NULL,   -- 1200×900
    width_px        INT,
    height_px       INT,
    caption         VARCHAR(512),
    label           VARCHAR(32),             -- "food","interior","exterior","menu"
    is_visible      BOOLEAN      NOT NULL DEFAULT TRUE,
    created_at      TIMESTAMP    NOT NULL DEFAULT NOW(),
    INDEX idx_business (business_id, created_at DESC)
);

-- ─────────────────────────────────────────────
-- Check-Ins
-- ─────────────────────────────────────────────

CREATE TABLE check_in (
    check_in_id   UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    business_id   UUID         NOT NULL REFERENCES business(business_id),
    user_id       UUID         NOT NULL REFERENCES user_account(user_id),
    checked_in_at TIMESTAMP    NOT NULL DEFAULT NOW(),
    note          VARCHAR(256),
    INDEX idx_business_time (business_id, checked_in_at DESC),
    INDEX idx_user_time     (user_id, checked_in_at DESC)
);

-- ─────────────────────────────────────────────
-- User Accounts
-- ─────────────────────────────────────────────

CREATE TABLE user_account (
    user_id         UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
    username        VARCHAR(64)  UNIQUE NOT NULL,
    email           VARCHAR(256) UNIQUE NOT NULL,
    password_hash   VARCHAR(256) NOT NULL,
    display_name    VARCHAR(128),
    bio             TEXT,
    avatar_photo_s3 VARCHAR(512),
    review_count    INT          NOT NULL DEFAULT 0,
    check_in_count  INT          NOT NULL DEFAULT 0,
    photo_count     INT          NOT NULL DEFAULT 0,
    useful_total    INT          NOT NULL DEFAULT 0,
    is_elite        BOOLEAN      NOT NULL DEFAULT FALSE,
    city            VARCHAR(128),
    created_at      TIMESTAMP    NOT NULL DEFAULT NOW()
);
```

### Database Choice

| Option | Pros | Cons | Use Case Fit |
|---|---|---|---|
| **PostgreSQL + PostGIS** | ACID, rich geo functions, JSONB for attributes, mature | Vertical scale ceiling; geo queries slow at 10 M businesses without sharding | Business metadata, reviews, user accounts |
| **MySQL (Vitess)** | Horizontal sharding via Vitess, proven at scale | No PostGIS equivalent; geo must be handled externally | Reviews DB if sharding required |
| **Cassandra** | Extremely high write throughput, time-series native | No ACID, no secondary indexes, tombstone overhead, complex to operate | Check-in history (append-only, time-series) |
| **Elasticsearch** | Full-text search + geo, aggregations, near-real-time index | Eventually consistent, write-heavy index maintenance, not a primary store | Search index |
| **Redis** | Sub-millisecond reads, rich data structures, pub/sub | Volatile without persistence; expensive per GB | Rating/review count cache, session store |
| **S3** | Cheap, durable, globally scalable object store | High latency for metadata; no query capability | Photo storage |
| **DynamoDB** | Serverless, auto-scale, global tables, predictable latency | No geo queries, limited secondary index options, higher per-request cost | User session / feature flags |

**Selected Choices**:
- **Business metadata**: PostgreSQL + PostGIS — JSONB attributes field enables flexible filtering without schema changes. PostGIS handles admin geo queries. For search, data is duplicated into Elasticsearch.
- **Reviews**: PostgreSQL sharded by `business_id` hash — co-locates all reviews for a business on one shard for efficient page fetches. UNIQUE(business_id, user_id) enforces one-review-per-user. Shard count: 32 shards × ~15 GB/shard = 480 GB total, manageable.
- **Photos**: S3 for objects (10 B × 500 KB = 5 PB), PostgreSQL for metadata. S3 gives 11 nines durability; CDN integration is native.
- **Check-ins**: Cassandra with partition key `(business_id, year_month)` and clustering column `checked_in_at DESC`. Writes are pure appends; reads are by business ordered by time.
- **Search index**: Elasticsearch 8.x with `geo_point` field on business documents. Mappings include all searchable fields plus a `geo_distance` filter.
- **Rating/review count cache**: Redis — aggregate_rating and review_count for 10 M businesses × 50 B = 500 MB, fits in a small Redis instance. Serves the search and profile page hot path without DB queries.

---

## 5. API Design

```
Base URL: https://api.yelp.com/v3/

Auth: Bearer JWT for user endpoints. API key for public search endpoints.
Pagination: Cursor-based (opaque token) for all list endpoints.

──────────────────────────────────────────────────────────────────
1. Business Search
──────────────────────────────────────────────────────────────────

GET /businesses/search
  Auth:       Optional (personalization requires JWT)
  Params:
    term          string  optional  "sushi", "brunch", "oil change"
    location      string  optional  "San Francisco, CA" (geocoded server-side)
    latitude      float   optional  (preferred over location string)
    longitude     float   optional
    radius        int     optional  Meters, max 40 000 (default 10 000)
    categories    string  optional  Comma-separated aliases ("restaurants,bars")
    locale        string  optional  BCP-47 language + region
    limit         int     optional  1–50 (default 20)
    offset        int     optional  0–1000 for simple pagination
    sort_by       enum    optional  best_match|rating|review_count|distance
    price         string  optional  "1,2,3" ($ $$ $$$)
    open_now      bool    optional
    open_at       int     optional  Unix timestamp for "open at that time"
    attributes    string  optional  "hot_and_new,wifi,outdoor_seating,accepts_credit_cards"
  Response 200:
    {
      "total": 2341,
      "businesses": [
        {
          "id": "uuid",
          "alias": "nobu-san-francisco",
          "name": "Nobu",
          "image_url": "https://cdn.yelp.com/photos/{hash}_o.jpg",
          "url": "https://www.yelp.com/biz/nobu-san-francisco",
          "review_count": 1847,
          "categories": [{"alias":"japanese","title":"Japanese"}],
          "rating": 4.3,
          "coordinates": {"latitude":37.786,"longitude":-122.406},
          "price": "$$$$",
          "location": { "address1":"...", "city":"San Francisco", "state":"CA", "zip":"94108" },
          "phone": "+14154381700",
          "distance": 1240.3,
          "is_closed": false
        }
      ],
      "region": {"center":{"latitude":37.7749,"longitude":-122.4194}}
    }
  Rate limit: 500 requests/day (free), custom for paid tiers

──────────────────────────────────────────────────────────────────
2. Business Details
──────────────────────────────────────────────────────────────────

GET /businesses/{id}
  Auth:       Optional
  Response 200: Extended business object including full hours, attributes,
                photos array (first 3), special_hours, messaging flags.
  Cache-Control: public, s-maxage=60  (60s CDN cache; business data rarely changes)

──────────────────────────────────────────────────────────────────
3. Business Reviews
──────────────────────────────────────────────────────────────────

GET /businesses/{id}/reviews
  Auth:       Optional
  Params:
    sort_by   enum    optional  newest|highest_rated|lowest_rated|most_useful (default: newest)
    limit     int     optional  1–50 (default 20)
    cursor    string  optional  Pagination cursor
    locale    string  optional
  Response 200:
    {
      "reviews": [
        {
          "id": "uuid",
          "url": "https://www.yelp.com/biz/nobu-sf#review-uuid",
          "text": "Amazing omakase, chef's knife skills are...",
          "rating": 5,
          "user": { "id":"uuid", "name":"Alex T.", "avatar_url":"...", "review_count":142 },
          "time_created": "2024-03-15",
          "useful_count": 42,
          "funny_count": 5,
          "cool_count": 19,
          "owner_response": null,
          "photos": [ { "url":"...", "caption":"Dragon roll" } ]
        }
      ],
      "total": 1847,
      "next_cursor": "eyJvZmZzZXQiOjIwfQ=="
    }
  Rate limit: 1000 req/day per API key

──────────────────────────────────────────────────────────────────
4. Submit Review
──────────────────────────────────────────────────────────────────

POST /businesses/{id}/reviews
  Auth:       Required (JWT)
  Body:
    {
      "rating": 5,
      "text": "Exceptional omakase experience...",
      "photo_ids": ["uuid1", "uuid2"]  // pre-uploaded photo UUIDs
    }
  Response 201: Created review object
  Idempotency-Key: header required (UUID); prevents duplicate on retry
  Rate limit: 1 review per user per business (enforced by DB UNIQUE constraint)
              plus 5 new reviews per user per day (fraud protection)

──────────────────────────────────────────────────────────────────
5. Vote on Review
──────────────────────────────────────────────────────────────────

POST /reviews/{review_id}/votes
  Auth:       Required (JWT)
  Body: { "vote_type": "useful" }  // useful|funny|cool
  Response 204: No Content
  Idempotency: Duplicate votes are ignored (UNIQUE constraint)

──────────────────────────────────────────────────────────────────
6. Photo Upload
──────────────────────────────────────────────────────────────────

POST /photos/upload-url
  Auth:       Required (JWT)
  Body: { "content_type": "image/jpeg", "business_id": "uuid" }
  Response 200:
    {
      "upload_url": "https://s3.amazonaws.com/yelp-photos/uploads/...?presigned=...",
      "photo_id": "uuid",  // reference to use in review submission
      "expires_at": "2024-04-01T13:00:00Z"
    }
  Notes: Client uploads directly to S3 using presigned URL (avoids routing through API servers).
         Photo Service receives S3 event notification and processes the image asynchronously.

GET /photos/{photo_id}/status
  Auth:       Required
  Response 200: { "status": "pending|processed|failed", "urls": { "sm":..., "md":..., "lg":... } }

──────────────────────────────────────────────────────────────────
7. Check-In
──────────────────────────────────────────────────────────────────

POST /businesses/{id}/checkins
  Auth:       Required (JWT)
  Body: { "note": "Great happy hour deals!" }
  Response 201: { "check_in_id": "uuid", "checked_in_at": "..." }
  Rate limit: 1 check-in per user per business per 24 h

──────────────────────────────────────────────────────────────────
8. Business Autocomplete
──────────────────────────────────────────────────────────────────

GET /autocomplete
  Auth:       Optional
  Params:
    text      string  required  Partial input text
    latitude  float   optional
    longitude float   optional
    locale    string  optional
  Response 200:
    {
      "terms": [{"text":"Sushi"},{"text":"Sashimi"}],
      "businesses": [{"id":"uuid","name":"Sushi Ran","distance_m":450}],
      "categories": [{"alias":"sushi","title":"Sushi Bars"}]
    }
  Rate limit: 300 req/s per key
```

---

## 6. Deep Dive: Core Components

### 6.1 Review System (Write Path, Aggregate Ratings, Voting)

**Problem it solves**: Accept 174 review writes/s peak at low latency, maintain a correct aggregate star rating per business within 30 s of each new review, enforce one-review-per-user-per-business, prevent double writes on retry, and serve the most relevant reviews for a business profile page.

**Approaches Comparison (Rating Aggregation)**:

| Approach | Consistency | Write Throughput | Complexity | Read Latency |
|---|---|---|---|---|
| Synchronous in-transaction recompute | Strong (immediately consistent) | Low (full table scan per review) | Low | Fast (no cache needed) |
| Incremental update with delta formula | Strong (exactly correct) | Medium (one UPDATE per review) | Low | Fast |
| Async Kafka event → consumer recomputes | Eventual (30 s lag) | High | Medium | Fast (cached result) |
| Materialized view (PostgreSQL) | Near-real-time (trigger-based) | Medium | Low | Fast | Very Fast |
| Batch recompute (hourly) | Stale (up to 1 h) | Very High | Low | Fast | Unacceptable for Yelp |

**Selected Approach**: Incremental delta update on review write (synchronous DB UPDATE) + async Kafka event to update Redis cache.

**Detailed Reasoning**: The aggregate rating can be maintained with O(1) arithmetic using a running sum and count:
- `new_avg = (old_avg × old_count + new_rating) / (old_count + 1)`
- This is a single atomic `UPDATE` in the same transaction as the review insert, ensuring the business table always has a correct aggregate. No full table scan required.
- The Redis cache is updated asynchronously via a Kafka event consumer (Rating Aggregation Service), which handles cache invalidation. This keeps the write transaction thin and fast.

**Implementation Detail (Review Write Path)**:

```python
class ReviewService:
    def __init__(self, db: Database, redis: Redis, kafka: KafkaProducer):
        self.db = db
        self.redis = redis
        self.kafka = kafka

    def submit_review(self, business_id: str, user_id: str,
                       rating: int, text: str,
                       photo_ids: list[str],
                       idempotency_key: str) -> dict:
        """
        Idempotent review submission.
        Returns existing review if idempotency_key was already processed.
        """
        # 1. Check idempotency key cache (Redis, TTL 24h)
        cached = self.redis.get(f"idempotency:{idempotency_key}")
        if cached:
            return json.loads(cached)  # Return previously created review

        # 2. Check for existing review by this user for this business
        existing = self.db.query_one(
            "SELECT review_id FROM review WHERE business_id=%s AND user_id=%s",
            (business_id, user_id)
        )
        if existing:
            raise ConflictError("User already reviewed this business")

        # 3. Rate-limit check: max 5 reviews per user per day
        if self._daily_review_count(user_id) >= 5:
            raise RateLimitError("Daily review limit exceeded")

        review_id = generate_uuid()
        now = datetime.utcnow()

        # 4. Write review + update business aggregate in a single transaction
        with self.db.transaction():
            # Insert review
            self.db.execute("""
                INSERT INTO review (review_id, business_id, user_id, rating, text_body, created_at)
                VALUES (%s, %s, %s, %s, %s, %s)
            """, (review_id, business_id, user_id, rating, text, now))

            # Link pre-uploaded photos to this review
            for photo_id in photo_ids:
                self.db.execute(
                    "UPDATE business_photo SET review_id=%s WHERE photo_id=%s AND uploaded_by=%s",
                    (review_id, photo_id, user_id)
                )

            # Incremental aggregate update (O(1), no table scan)
            self.db.execute("""
                UPDATE business
                SET aggregate_rating = (aggregate_rating * review_count + %s) / (review_count + 1),
                    review_count = review_count + 1,
                    updated_at = NOW()
                WHERE business_id = %s
            """, (rating, business_id))

            # Increment user review count
            self.db.execute(
                "UPDATE user_account SET review_count = review_count + 1 WHERE user_id = %s",
                (user_id,)
            )

        # 5. Publish Kafka event (outside transaction — fire and forget)
        event = {
            "event_type": "review_created",
            "review_id": review_id,
            "business_id": business_id,
            "user_id": user_id,
            "rating": rating,
            "created_at": now.isoformat()
        }
        self.kafka.send("review-events", key=business_id, value=json.dumps(event))

        # 6. Invalidate Redis cache for this business
        self.redis.delete(f"business:{business_id}")
        self.redis.delete(f"reviews:{business_id}:*")  # pattern delete via SCAN

        # 7. Store idempotency key → review_id mapping (24h TTL)
        result = {"review_id": review_id, "created_at": now.isoformat()}
        self.redis.setex(f"idempotency:{idempotency_key}", 86400, json.dumps(result))

        return result

    def _daily_review_count(self, user_id: str) -> int:
        key = f"user_reviews_today:{user_id}:{datetime.utcnow().date()}"
        count = self.redis.incr(key)
        if count == 1:
            self.redis.expire(key, 86400)  # TTL until midnight
        return count - 1  # Return before this attempt
```

**Interviewer Q&As**:

1. **Q: How do you prevent a single user from gaming the rating system by submitting many reviews?**
   A: Multiple layers: (1) Database UNIQUE(business_id, user_id) prevents more than one review per user per business at the data layer. (2) Rate limiting: 5 new reviews per user per day (Redis counter with daily TTL). (3) Yelp's "Recommendation Software" (anti-spam ML): reviews from new accounts, accounts with no friends, and accounts with suspicious patterns (same IP as business owner, many 5-star reviews for related businesses) are not shown by default (filtered, not deleted). (4) Device fingerprinting: multiple accounts from the same device are flagged.

2. **Q: How do you handle review deletion for the aggregate rating? The incremental update formula doesn't work in reverse simply.**
   A: On deletion, the inverse formula: `new_avg = (old_avg × old_count - deleted_rating) / (old_count - 1)`. This is exact arithmetic. The edge case is `old_count = 1` → `new_avg = 0` (no reviews). To avoid floating-point drift over thousands of updates, a nightly recompute job recalculates ratings from scratch (SELECT AVG(rating), COUNT(*) FROM review WHERE business_id = X) and corrects any accumulated drift. This also handles bulk moderation deletions.

3. **Q: How do you sort reviews — what algorithm determines which reviews are shown first?**
   A: Default sort is "Yelp Sort" — a weighted score combining: (1) user's Elite status (higher weight for elite reviewers); (2) review helpfulness votes (useful_count as primary signal); (3) recency (log decay over time); (4) review length/completeness (penalty for <20-word reviews). The score is precomputed when a review receives new votes and stored as `yelp_score FLOAT` on the review row. The `GET /reviews?sort_by=best_match` query orders by this column. Recency-based sort uses `created_at DESC`.

4. **Q: What is the data model for the "owner response to review" feature?**
   A: Owner responses are stored directly on the review row: `owner_response TEXT` and `owner_responded_at TIMESTAMP`. A business owner's JWT includes a `business_owner_for: [list_of_business_ids]` claim validated by the API Gateway. The `PATCH /reviews/{review_id}/owner-response` endpoint checks that the caller owns the business associated with the review. Storing it on the review row avoids a JOIN and simplifies pagination since the response is always fetched with its review.

5. **Q: How do you handle the edge case where a business gets acquired and all old reviews need to be migrated to the new business_id?**
   A: A data migration job: (1) create a new business entity or update the existing one; (2) update all review rows `SET business_id = new_id WHERE business_id = old_id` (within a transaction on the reviews shard); (3) recompute aggregate rating for new_id from scratch; (4) deactivate or merge old_id; (5) set up a 301 redirect from the old URL slug to the new one. If reviews span multiple shards (sharded by business_id), each affected shard runs the migration independently within a single job.

---

### 6.2 Full-Text Search + Geo Filtering (Elasticsearch)

**Problem it solves**: Enable users to search "cheap ramen near Golden Gate Park open now" — combining natural language full-text matching, geographic proximity filtering, categorical filtering, business-hours filtering, attribute filtering, and relevance ranking — in under 200 ms p99.

**Approaches Comparison**:

| Approach | Full-Text | Geo Filter | Attribute Filter | Scalability | Operational Cost |
|---|---|---|---|---|---|
| **PostgreSQL full-text + PostGIS** | Moderate (tsvector/tsquery) | Excellent (ST_DWithin) | Good (JSONB contains) | Single node | Low |
| **Elasticsearch** | Excellent (BM25, tokenization, synonyms) | Good (geo_distance, geo_bounding_box) | Good (term filters) | Excellent (sharding) | Medium |
| **Solr** | Excellent | Good | Good | Good | High |
| **Typesense** | Good (optimized for typo tolerance) | Basic | Basic | Medium | Low |
| **Meilisearch** | Good | Basic | Good | Medium | Low |
| **Custom inverted index** | Possible | Must build | Must build | Engineering cost | Very High |

**Selected Approach**: Elasticsearch 8.x.

**Detailed Reasoning**: Elasticsearch natively handles the combination of BM25 full-text scoring, `geo_distance` filter, `term` filters (category, price_level), and `script_score` for custom re-ranking — all in a single query. For Yelp's scale (10 M documents), Elasticsearch can shard across 10 primary shards (1 M docs/shard) with 1 replica each, handling 10 500 search RPS easily (each shard handles ~1 050 RPS).

**Elasticsearch Mapping and Query**:

```json
// Mapping for the 'businesses' index
{
  "mappings": {
    "properties": {
      "business_id":        { "type": "keyword" },
      "name":               { "type": "text", "analyzer": "standard",
                              "fields": { "keyword": { "type": "keyword" } } },
      "description":        { "type": "text", "analyzer": "english" },
      "category_aliases":   { "type": "keyword" },
      "category_titles":    { "type": "text", "analyzer": "standard" },
      "price_level":        { "type": "byte" },
      "aggregate_rating":   { "type": "float" },
      "review_count":       { "type": "integer" },
      "location":           { "type": "geo_point" },
      "city":               { "type": "keyword" },
      "state":              { "type": "keyword" },
      "country":            { "type": "keyword" },
      "is_active":          { "type": "boolean" },
      "is_closed":          { "type": "boolean" },
      "attributes":         { "type": "flat_object" },
      "open_now_cache":     { "type": "boolean" },
      "hours":              { "type": "object", "enabled": false }
    }
  }
}
```

```json
// Search query: "cheap ramen near SF, open now, with WiFi"
{
  "query": {
    "function_score": {
      "query": {
        "bool": {
          "must": [
            {
              "multi_match": {
                "query": "ramen",
                "fields": ["name^3", "category_titles^2", "description"],
                "type": "best_fields",
                "fuzziness": "AUTO"
              }
            }
          ],
          "filter": [
            { "term": { "is_active": true } },
            { "term": { "is_closed": false } },
            { "terms": { "price_level": [1, 2] } },
            { "term": { "attributes.wifi": true } },
            { "term": { "open_now_cache": true } },
            {
              "geo_distance": {
                "distance": "10km",
                "location": { "lat": 37.7749, "lon": -122.4194 }
              }
            }
          ]
        }
      },
      "functions": [
        {
          "gauss": {
            "location": {
              "origin": { "lat": 37.7749, "lon": -122.4194 },
              "scale": "2km",
              "offset": "100m",
              "decay": 0.5
            }
          },
          "weight": 3
        },
        {
          "field_value_factor": {
            "field": "aggregate_rating",
            "factor": 1.2,
            "modifier": "square",
            "missing": 1
          },
          "weight": 2
        },
        {
          "field_value_factor": {
            "field": "review_count",
            "factor": 0.1,
            "modifier": "log1p",
            "missing": 0
          },
          "weight": 1
        }
      ],
      "score_mode": "sum",
      "boost_mode": "multiply"
    }
  },
  "size": 20,
  "_source": ["business_id", "name", "aggregate_rating", "review_count",
              "location", "category_aliases", "price_level"]
}
```

**Keeping `open_now_cache` fresh**: A cron job re-evaluates `open_now_cache` for all businesses every 5 minutes using their `hours_json`. Changes trigger a partial document update to Elasticsearch via the bulk API. This avoids real-time hours computation in the query path.

**Interviewer Q&As**:

1. **Q: Why not use Elasticsearch's geo_point with geo_distance directly in the query for ranking instead of your custom Gaussian decay function?**
   A: Elasticsearch's `geo_distance` query is a hard filter (inside/outside radius) with no score contribution. To incorporate distance into the relevance score (closer = higher score), we use the `gauss` function in `function_score`. The Gaussian decay function with `scale=2km` means a business at 2 km from the center has 50% of the score of a business at the origin, which is a semantically meaningful distance penalty. A raw distance-based sort would ignore text relevance entirely.

2. **Q: How do you keep the Elasticsearch index in sync with the PostgreSQL business table?**
   A: Change Data Capture (CDC) using Debezium monitoring PostgreSQL's WAL. Every INSERT/UPDATE/DELETE on the `business` table emits a change event to a Kafka topic (`business-changes`). The Search Indexer Service (a Kafka consumer) transforms the change event into an Elasticsearch document and calls the bulk index API. Lag target: <30 s. On indexer crash, the Kafka offset ensures no changes are missed on restart.

3. **Q: How does Elasticsearch handle the open_now filter for businesses in different time zones?**
   A: Server-side time zone handling: each business document stores its `timezone` (e.g., "America/New_York"). The `open_now_cache` boolean is recomputed per business in its local timezone every 5 minutes by the cron job. During transition periods (daylight saving changes), the cron job recalculates all affected businesses. This is simpler than performing a real-time timezone-aware computation in the Elasticsearch query.

4. **Q: How do you handle synonyms in search? "Coffee shop" vs. "café" vs. "coffeehouse"?**
   A: Elasticsearch synonym filters in the analyzer chain: a custom `synonym_filter` maps equivalent terms at index time and/or query time. Yelp maintains a business-domain synonym graph: `{coffee shop, cafe, coffeehouse, espresso bar}` all resolve to the same token bucket. Category expansion: a query for "burger" also matches businesses in the "American" category if no "burger" matches are found within the radius (fall-back expansion via a second query with `should` clauses).

5. **Q: What happens to the search index when you do a large-scale data update (e.g., a new attribute field added to all 10 M businesses)?**
   A: Elasticsearch supports zero-downtime index mappings changes via alias + reindex: (1) create a new index `businesses_v2` with the updated mapping; (2) run the `_reindex` API to copy all documents from `businesses_v1` to `businesses_v2` (takes ~30 min for 10 M docs); (3) atomically swap the `businesses` alias from `v1` to `v2`; (4) delete `v1` after confirming `v2` is healthy. During reindex, writes go to both `v1` (via write alias) and `v2` (via the CDC consumer that re-indexes changed docs); search traffic reads from `v1` until the alias swap.

---

### 6.3 Photo Storage & Processing Pipeline

**Problem it solves**: Accept 23 photo uploads/s at peak, store originals durably (5 PB total), generate three thumbnail sizes, serve photos globally via CDN at low latency, and associate photos with the correct business and review.

**Approaches Comparison**:

| Approach | Storage Cost | Processing | Scalability | CDN Integration |
|---|---|---|---|---|
| Store originals only, resize on demand | Low storage | High compute per view | Poor (CPU-bound per request) | Difficult (dynamic images) |
| Pre-generate fixed thumbnail sizes | Higher storage (×4) | Low per view | Excellent | Native (static files) |
| Image CDN (Cloudinary, Imgix) | Storage + per-transform fee | Managed | Excellent | Built-in | Expensive at Yelp's scale |
| Client-side resize before upload | Lower storage | No server compute | Good | Native | Quality unpredictable |

**Selected Approach**: Client uploads original to S3 (presigned URL). S3 triggers Lambda (or a Flink job) to generate 3 standard sizes. Thumbnails stored in S3 under content-addressed paths. All served via CloudFront CDN.

**Implementation Detail**:

```python
# Lambda function triggered by S3 ObjectCreated event
import boto3
from PIL import Image
import io

s3 = boto3.client('s3')
THUMBNAIL_SPECS = [
    ("sm", 150, 150),
    ("md", 400, 300),
    ("lg", 1200, 900),
]

def handler(event, context):
    # Parse S3 event
    bucket = event["Records"][0]["s3"]["bucket"]["name"]
    original_key = event["Records"][0]["s3"]["object"]["key"]
    # key format: "uploads/{photo_id}/original.jpg"
    photo_id = original_key.split("/")[1]

    # Download original from S3
    response = s3.get_object(Bucket=bucket, Key=original_key)
    original_bytes = response["Body"].read()

    img = Image.open(io.BytesIO(original_bytes))

    # Auto-orient based on EXIF data (fix rotated phone photos)
    img = ImageOps.exif_transpose(img)

    # Convert to RGB (handle PNG transparency)
    if img.mode in ("RGBA", "P"):
        img = img.convert("RGB")

    thumbnail_keys = {}
    for (size_label, max_w, max_h) in THUMBNAIL_SPECS:
        thumb = img.copy()
        thumb.thumbnail((max_w, max_h), Image.LANCZOS)

        # Center-crop to exact dimensions for sm and md
        if size_label in ("sm", "md"):
            thumb = ImageOps.fit(thumb, (max_w, max_h), Image.LANCZOS)

        # Compress to JPEG with progressive encoding
        buf = io.BytesIO()
        thumb.save(buf, format="JPEG", quality=85, optimize=True, progressive=True)
        buf.seek(0)

        # Content-addressed key: hash of original photo_id + size label
        dest_key = f"photos/{photo_id}/{size_label}.jpg"
        s3.put_object(
            Bucket=bucket,
            Key=dest_key,
            Body=buf,
            ContentType="image/jpeg",
            CacheControl="public, max-age=31536000, immutable",
            Metadata={"photo-id": photo_id, "size": size_label}
        )
        thumbnail_keys[size_label] = dest_key

    # Update photo metadata DB (via internal API call)
    update_photo_metadata(photo_id, status="processed",
                          keys=thumbnail_keys,
                          width=img.width, height=img.height)

    # Publish photo-processed event to Kafka for downstream consumers
    kafka_producer.send("photo-events", {
        "event_type": "photo_processed",
        "photo_id": photo_id,
        "business_id": get_business_id_for_photo(photo_id),
        "thumbnail_keys": thumbnail_keys
    })
```

**Interviewer Q&As**:

1. **Q: How do you handle photo moderation (no NSFW content)?**
   A: Two-stage moderation: (1) Async ML scan immediately after photo processing — a content moderation model (AWS Rekognition or an internal model) classifies the image. If NSFW confidence > 70%, the photo is hidden and queued for human review. (2) User reports: any user can flag a photo; above a threshold of reports, the photo is auto-hidden pending review. (3) Business owner photos bypass the ML scan threshold (lower bar) because owners have accepted terms of service. Human review completes within 24 h via a moderation queue tool.

2. **Q: How do you ensure photo URLs remain valid even if you change CDN providers?**
   A: Photo URLs use a virtual domain (`https://s3.yelp-cdn.com/photos/{photo_id}/lg.jpg`) that abstracts the underlying CDN. The DNS CNAME for `s3.yelp-cdn.com` points to the active CDN. Switching CDN providers requires only a DNS change + warming the new CDN's cache. S3 paths remain stable because they are content-addressed (photo_id is immutable).

3. **Q: How do you display photos for a business in a specific order (cover photo first, then most-engaged photos)?**
   A: The `business_photo` table has an `is_cover` boolean. The `cover_photo_id` on the business table points to the designated cover photo. For the remaining photos, the "engagement score" is precomputed: `engagement = useful_count_of_review × 0.5 + user_votes_on_photo × 1.0`. The Photo Service returns photos ordered by `is_cover DESC, engagement_score DESC`. This is computed offline nightly and stored as `display_rank INT` on each photo row.

---

## 7. Scaling

### Horizontal Scaling

- **Search Service**: Stateless; scale to 10 500 search RPS with ~50 pods. Elasticsearch cluster scales independently (10 shards × 1 M docs, 1 replica = 20 index nodes in the ES cluster).
- **Review Service**: Stateless for reads. Writes are thin (transaction + Kafka publish). Scale to 174 write RPS easily with 10 pods; reads scale to thousands of RPS.
- **Photo Service**: Stateless upload acknowledgment pods. Lambda auto-scales for processing. S3 scales infinitely.
- **Business Profile Service**: Stateless; reads from Redis cache first. Scale with pod count proportional to profile page RPS.

### DB Sharding

- **Review DB**: 32 PostgreSQL shards, hash-partitioned by `business_id`. A business with 10 000 reviews has all reviews on a single shard — efficient for the "reviews for business X" read pattern. Shard map stored in ZooKeeper.
- **Business DB**: Single PostgreSQL + PostGIS for metadata (10 M rows, 20 GB — fits in memory on a modern server). Read replicas for horizontal read scaling.
- **Check-in (Cassandra)**: Partition key `(business_id, year_month)` distributes load across the Cassandra cluster. Consistency level QUORUM for writes, ONE for reads.

### Replication

- PostgreSQL: Primary + 2 read replicas per shard. Streaming replication, hot-standby.
- Elasticsearch: 1 replica per shard (20 nodes total for 10 primary shards). Writes go to primary shard; reads distributed across all replicas.
- S3: Multi-region replication to US-East and EU-West for disaster recovery.
- Redis: Cluster with 6 nodes (3 masters + 3 replicas). `WAIT 1 0` for near-synchronous replication of critical data.

### Caching

| Cache Layer | Technology | TTL | Content |
|---|---|---|---|
| Business summary | Redis Hash `business:{id}` | 60 s | name, rating, review_count, price, hours |
| Review page 1 | Redis String `reviews:{id}:page1` | 30 s | First page of reviews (most common request) |
| User profile | Redis Hash `user:{id}` | 5 min | Display name, review count, avatar URL |
| Search result | Redis String `search:{hash(params)}` | 10 s | Paginated search result for common queries |
| Photo thumbnails | CloudFront | 1 year (immutable) | All photo sizes |
| Open now cache | Redis `open_now:{biz_id}` | 5 min | Boolean, refreshed by cron job |

### CDN

- CloudFront distribution for all photo content. Photo URLs are immutable (content-addressed); `Cache-Control: max-age=31536000, immutable`.
- Business profile HTML is server-rendered at first request then cached at CDN for 60 s (short TTL to reflect rating changes).
- API responses for the search endpoint: 10 s TTL for common city-level queries (quantized coordinates).

**Interviewer Q&As**:

1. **Q: How do you handle "hot businesses" — a restaurant that gets 1 000× normal traffic after a viral news article?**
   A: The Redis cache for `business:{id}` absorbs the spike (TTL 60 s, sub-millisecond reads). The Business Profile Service pods are stateless and auto-scale. The Reviews page is the bottleneck: `reviews:{id}:page1` cache with 30 s TTL ensures at most 1 DB query per 30 s regardless of traffic. For the viral scenario, the ops team can manually extend the TTL to 5 min for the specific business ID. The Elasticsearch search result for that business is served from the search cache.

2. **Q: How do you paginate reviews efficiently when a business has 50 000 reviews?**
   A: Offset-based pagination (`LIMIT 20 OFFSET 40000`) is O(offset) — unacceptably slow for deep pages. Cursor-based pagination: the cursor encodes `{last_review_id, last_created_at, sort_mode}`. The SQL query becomes `WHERE (created_at, review_id) < (cursor_created_at, cursor_review_id) ORDER BY created_at DESC LIMIT 20`. This is O(log N) using the compound index `(business_id, created_at DESC)`. Deep pages are rare for user-facing UIs; the cursor design makes it efficient regardless.

3. **Q: How do you ensure the Elasticsearch index and the PostgreSQL business table don't diverge?**
   A: CDC via Debezium provides near-real-time sync. For divergence detection: a nightly reconciliation job samples 1% of businesses, compares PostgreSQL values with Elasticsearch document values for key fields (aggregate_rating, review_count, is_closed), and triggers a re-index of any discrepancy. Full reindex runs weekly (takes ~2 h for 10 M docs using Elasticsearch reindex API with 4 parallel workers).

4. **Q: What happens to search results if the Elasticsearch cluster goes down?**
   A: Search falls back to PostgreSQL PostGIS + full-text search (`tsvector` columns on `business.name` and `business.description`). The fallback is slower (p99 ~2 s vs. 200 ms) but correct. The API Gateway detects the Elasticsearch circuit breaker open and routes to the fallback handler. Users see a slightly degraded experience ("search may be slower right now"). Business profile and review pages are unaffected (served from Redis cache + PostgreSQL, independent of Elasticsearch).

5. **Q: How would you design the system to handle GDPR "right to be forgotten" for a user with 500 reviews?**
   A: A deletion pipeline triggered by the user's deletion request: (1) anonymize the `user_account` row (set `display_name = "Deleted User"`, null out `email`, `bio`, `avatar`); (2) replace review `text_body` with "[Review removed]" and set `user_id` to a system-wide "deleted_user" sentinel; (3) preserve review ratings (business aggregate rating must remain accurate; ratings are not personal data under GDPR); (4) delete check-ins, photos, and votes associated with the user; (5) send a Kafka event that triggers Elasticsearch partial update (anonymize user info in review snippets in the search index); (6) S3 lifecycle rule deletes user's original photos (thumbnails via S3 batch delete). Completion within 30 days as required.

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Mitigation | Recovery |
|---|---|---|---|
| PostgreSQL primary failure | Review writes fail | Patroni automatic failover to replica within 30 s | <30 s |
| Elasticsearch node failure | Search capacity reduced | Automatic shard rebalancing to healthy nodes | <2 min |
| Redis cluster node failure | Cache miss spike | Cluster failover to replica; degraded latency (DB fallback) | <10 s |
| S3 partial outage | Photo uploads fail; cached photos still served | Presigned upload retry with exponential backoff; CDN serves cached photos | Transparent to viewers |
| Kafka broker failure | Review events delayed | RF=3; producer retries; Kafka auto-leader-election | <30 s |
| Search Indexer consumer crash | ES index becomes stale | Kafka offset replay on restart; staleness bounded by consumer downtime | <2 min catch-up |
| Photo Lambda cold start | Photo processing delayed | Lambda provisioned concurrency for top-N functions; user sees "photo processing" status | 1–3 s |
| Review spam surge | Fake reviews published | Real-time rate limit (5/day/user) + ML filter catches within 24 h | 24 h for ML; immediate for rate limit |

**Retries & Idempotency**:
- All review writes require a client-provided `Idempotency-Key` header (UUID). The API stores the key in Redis for 24 h and returns the cached response for duplicates.
- Photo upload presigned URLs are idempotent: re-uploading the same photo_id replaces the existing object in S3 (same key → same result).
- Kafka consumers use `enable.auto.commit=false` and manual offset commit after successful processing to prevent data loss on crash.

**Circuit Breaker**: Implemented at the API Gateway and in the Search Service (around Elasticsearch calls). If ES error rate > 5% in 10 s, breaker opens and requests fall back to PostgreSQL. Recovers automatically after 30 s.

---

## 9. Monitoring & Observability

| Metric | Description | Alert Threshold |
|---|---|---|
| `search_latency_p99_ms` | Elasticsearch search query latency | >200 ms |
| `review_write_latency_p99_ms` | End-to-end review submission latency | >500 ms |
| `rating_cache_freshness_seconds` | Age of rating in Redis vs. PostgreSQL | >60 s |
| `es_indexer_lag_seconds` | Lag between business update and ES index update | >60 s |
| `photo_processing_queue_depth` | Lambda backlog for photo resizing | >1 000 |
| `review_spam_rate` | % of reviews flagged by ML model | >5% |
| `cdn_cache_hit_rate` | CloudFront photo cache hit rate | <95% |
| `db_replication_lag_ms` | PostgreSQL replication lag to replicas | >5 000 ms |
| `redis_eviction_rate` | Keys evicted per second | >0 |
| `es_cluster_health` | Elasticsearch cluster health status | yellow or red |

**Distributed Tracing**: OpenTelemetry. Search requests trace: `API GW → Search Svc → ES query → Redis hydration → Response`. Review writes trace: `API GW → Review Svc → DB transaction → Kafka publish`. All spans include business_id for correlation.

**Logging**: Structured JSON. PII fields (user email, full review text) excluded from logs; log-level filtering applied. Review write logs capture: `{business_id, user_id_hash, rating, review_length_chars, latency_ms}`. Photo upload logs: `{photo_id, business_id, file_size_bytes, processing_time_ms}`.

**Error Budget**: 99.99% availability = 52 min/year. SLO error budget tracked via a Prometheus-based SLO dashboard. On error budget burn rate > 5×, an on-call page fires. On 10× burn rate, incident bridge opened.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option A | Option B (Chosen) | Reason |
|---|---|---|---|
| Search engine | PostgreSQL full-text | Elasticsearch | ES provides superior BM25 + geo + aggregations combo; scales to 10 M docs with sub-200 ms response |
| Rating aggregation | Full recompute on every review | Incremental delta update + nightly correction | Delta update is O(1) vs O(N); nightly correction catches drift |
| Photo storage | Managed CDN (Cloudinary) | S3 + Lambda + CloudFront | At 5 PB scale, managed CDN cost is prohibitive; S3+Lambda is far cheaper |
| Review DB sharding | Shard by user_id | Shard by business_id | Reviews are almost always fetched by business; co-locating them eliminates cross-shard joins |
| Pagination | Offset-based | Cursor-based | Offset degrades O(N) at depth; cursor is O(log N) with the right index |
| Hours filter | Real-time computation in query | Pre-computed open_now_cache (5 min TTL) | Real-time timezone computation in the query loop adds 10–20 ms; pre-computation is ~0 ms |
| Photo URL strategy | Dynamic URLs with query params | Content-addressed immutable URLs | Immutable URLs enable 1-year CDN TTL; no cache invalidation needed |

---

## 11. Follow-up Interview Questions

1. **Q: How would you implement the "Yelp Elite Squad" — how does a user achieve Elite status?**
   A: Elite status is determined by a scoring model run monthly: inputs include review count (past 12 months), review quality score (avg useful votes per review), photo count, event attendance, profile completeness, and account age. A threshold score grants Elite status (valid for 1 year). The scoring model runs as a nightly batch Spark job on the user activity table. Elite status is stored as `is_elite BOOLEAN` on `user_account` and cached in Redis. Elite users receive a weighted boost in the review sort algorithm.

2. **Q: How do you detect fake or competitor-sabotage reviews (e.g., a rival business posting 1-star reviews)?**
   A: Multi-signal detection: (1) IP/device fingerprinting — reviews from the same IP subnet as the business's IP block are flagged; (2) velocity detection — >3 reviews for the same business from accounts created in the same 24 h window; (3) social graph distance — reviewers with no connection to existing Yelp users (no friends, no common check-ins) have their reviews "not recommended"; (4) text similarity — ML model detects templated/copy-paste reviews (cosine similarity between reviews of same business); (5) behavioral patterns — accounts that only review competitors of a specific business.

3. **Q: How would you design a "trending businesses" feature?**
   A: A "trending score" = delta in check-in and review velocity over the past 24 h vs. 30-day baseline. Computed by a Flink job that counts check-in events and review events per business per hour. The trending score is written to Redis Sorted Set `trending:{city}:{category}` (score = trending delta). The Search Service uses this sorted set to boost trending businesses in search results. A CDN-cached "Trending in San Francisco" endpoint is refreshed every 15 min.

4. **Q: How would you add a "deals and offers" feature (Yelp Deals)?**
   A: A new `deal` table linked to `business`. Deals have start/end timestamps, discount percentage, redemption code, and remaining_count. The Business Profile Service includes active deals in the profile response. Redemption requires: (1) user claims deal (UPDATE remaining_count - 1 WHERE remaining_count > 0, optimistic lock); (2) a unique redemption_code is generated per user; (3) code validated at point of sale via a partner API. The Deal Service is isolated to avoid coupling with the core review/search path.

5. **Q: How would you implement a "similar businesses" feature on the business profile page?**
   A: Three approaches at different complexities: (1) Fast/simple: fetch top-5 businesses in the same category within 2 km, excluding the current business; (2) Medium: content-based filtering — represent each business as a TF-IDF vector over review text; find K-nearest neighbors using cosine similarity (precomputed nightly, stored as `similar_business_ids ARRAY` on each business); (3) Collaborative filtering: businesses visited by users who also visited the current business (co-visit matrix, factorized via ALS). The production system uses approach (3) for authenticated users and approach (1) for anonymous users.

6. **Q: How would you design the review moderation queue?**
   A: The ML spam model runs asynchronously after each review submission (Kafka consumer on `review-created` topic). Flagged reviews (confidence > 60%) are written to a `moderation_queue` table with status=`pending`. The moderation tool is an internal React app backed by a `ModerationService`. Moderators see the review, the business, the reviewer's history, and the ML score. Actions: `approve` (review becomes visible), `remove` (is_visible=FALSE), `warn` (user receives a policy warning), `ban` (user_account.is_banned=TRUE). SLA: high-confidence spam (>90%) auto-removed; medium confidence (60–90%) reviewed by human within 24 h.

7. **Q: How do you handle time zones for business hours across different countries?**
   A: Each business stores its `timezone` as an IANA time zone string (e.g., "America/New_York", "Europe/Paris"). The `hours_json` stores local business hours (e.g., "Mon 11:00-22:00" in the business's local time). The `open_now` filter: the precompute cron job runs every 5 minutes; for each business, it evaluates `is_open(hours_json, now_in_local_timezone(timezone))` and updates `open_now_cache`. The cron job runs globally; businesses are sharded by time zone offset to distribute the compute load.

8. **Q: How would you build the "Yelp for Business" dashboard (metrics for business owners)?**
   A: A separate analytics service backed by a data warehouse (Snowflake/BigQuery). Daily ingestion from Kafka: every check-in, profile view, and search impression that resulted in a click is logged to the data warehouse. The Business Dashboard API queries pre-aggregated tables: `profile_views_7d`, `search_impressions_7d`, `review_count_change`, `photo_views_7d`. The 7-day aggregates are computed nightly by a Spark job. Real-time metrics (e.g., "people looking at your business right now") use a separate Redis counter with 1-hour TTL.

9. **Q: What changes to the architecture if Yelp needs to expand to 100× more countries with local data (200 M businesses)?**
   A: Elasticsearch scales horizontally — increase primary shards from 10 to 200 (1 M docs/shard). PostgreSQL for business metadata needs to move to a globally distributed database (Google Spanner or CockroachDB) or geo-partitioned sharding (businesses in EU stored in EU region). Review DB: increase shard count from 32 to 320. Photo storage: S3 already infinite-scale; just add regional buckets for data residency compliance. The geo index (Redis) scales by adding nodes. The main new requirement is data sovereignty: EU GDPR mandates EU-user data stays in the EU, requiring region-aware routing at the API Gateway.

10. **Q: How would you design autocomplete for the search bar?**
    A: Two data sources: (1) business names — a sorted set in Redis `autocomplete:{prefix}` containing top-50 matching business names by popularity; (2) category labels — a finite set (~1 000 categories), stored in a prefix trie in application memory. On keypress, the client sends `GET /autocomplete?text=su&lat=...&lon=...`. The API queries Redis for the prefix sorted set (O(log N) lookup) and the in-memory category trie simultaneously. Results are merged: categories first (exact prefix match), then businesses (nearest by distance among prefix matches). Redis sorted set is rebuilt nightly with the top-1 000 most-searched business names per city.

11. **Q: How would you add internationalization and multi-language business names?**
    A: Business names are stored in their native script (Japanese kanji, Arabic RTL, etc.). The `locale` query parameter on search sets the preferred language for category labels and response text. For businesses with names in multiple languages (e.g., a business in Hong Kong has both Chinese and English names), a separate `business_translation` table stores `{business_id, locale, name, description}`. The Search Service queries the appropriate translation table when a non-default locale is requested. Elasticsearch indices are created per-locale (one index per major language) to enable language-appropriate tokenization (e.g., Japanese ICU tokenizer for Japanese names).

12. **Q: How do you handle the CAP theorem trade-off in the review system?**
    A: The review system prioritizes CP (Consistency + Partition Tolerance) for the write path: reviews use PostgreSQL with synchronous writes and UNIQUE constraints — we accept that writes may fail during a partition rather than allowing duplicate reviews (which would be an unacceptable inconsistency). For the read path (search results, rating display), we accept eventual consistency (AP): search results may show a slightly stale rating for up to 30 s after a new review. This trade-off reflects the product's trust requirements: review integrity (no duplicates, no lost reviews) is more important than instant read propagation.

13. **Q: How do you measure recommendation quality?**
    A: Online metrics: click-through rate (CTR) on recommended businesses, click-to-navigation rate (user opened directions), review rate (user reviewed a business they found via recommendation). Offline metrics: precision@K and recall@K against held-out user-visit pairs. A/B test framework compares new recommendation models against the control. Novelty metric: fraction of recommended businesses the user has not seen before (balance between familiar and new). Serendipity metric: how surprising and relevant the recommendations are (measured via post-session survey sampling).

14. **Q: How would you build the check-in feature with gamification (badges, milestones)?**
    A: Check-ins are written to Cassandra. After each check-in, a `check-in` Kafka event triggers the Badge Service. The Badge Service evaluates badge criteria (e.g., "50 check-ins", "First check-in in Tokyo", "5 consecutive Friday check-ins") against the user's check-in history, aggregated in Redis counters and Cassandra time-series. When a badge is earned, a notification event is published and the user's profile is updated (`badges JSONB` array on `user_account`). Badge criteria are stored as a configuration DSL evaluated at runtime, allowing new badges to be added without code changes.

15. **Q: How would you design the data model to support Yelp's "Collections" feature (user-curated lists of businesses)?**
    A: A `collection` table: `{collection_id, user_id, name, description, is_public, created_at}`. A `collection_item` table: `{collection_id, business_id, added_at, note}`. Collections are fetched by `user_id` (user's profile page) or by `collection_id` (share URL). Business-level collections lookup: `SELECT collection_id FROM collection_item WHERE business_id = X` — how many users have saved this business. The number of saves is a signal for the recommendation engine (high saves = popular with engaged users). Collection items are indexed in Elasticsearch to support "Collections mentioning 'ramen in Seattle'" search.

---

## 12. References & Further Reading

- Yelp Engineering Blog. "Billions of Rows a Second: Yelp's Real-Time Data Pipeline." https://engineeringblog.yelp.com/2016/07/billions-of-rows.html
- Yelp Engineering Blog. "How Yelp Handles Millions of Reviews a Month." https://engineeringblog.yelp.com/2017/02/building-reliable-reviews.html
- Elasticsearch Documentation. "Function Score Query." https://www.elastic.co/guide/en/elasticsearch/reference/current/query-dsl-function-score-query.html
- Elasticsearch Documentation. "Geo Distance Query." https://www.elastic.co/guide/en/elasticsearch/reference/current/query-dsl-geo-distance-query.html
- AWS Documentation. "Using presigned URLs." https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-presigned-url.html
- PostgreSQL Documentation. "Full Text Search." https://www.postgresql.org/docs/current/textsearch.html
- Debezium Documentation. "PostgreSQL Connector." https://debezium.io/documentation/reference/stable/connectors/postgresql.html
- Apache Cassandra Documentation. "Data Modeling." https://cassandra.apache.org/doc/latest/cassandra/data_modeling/
- Apache Kafka Documentation. "Idempotent Producer." https://kafka.apache.org/documentation/#producerconfigs_enable.idempotence
- CloudFront Documentation. "Cache-Control." https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/Expiration.html
- Hu, Y., Koren, Y., Volinsky, C. (2008). "Collaborative Filtering for Implicit Feedback Datasets." *ICDM 2008*. (ALS-based recommendation)
- Koren, Y., Bell, R., Volinsky, C. (2009). "Matrix Factorization Techniques for Recommender Systems." *Computer*, 42(8), 30–37.
- Alex Xu. "System Design Interview Vol. 1." Chapter: Design a Proximity Service. ByteByteGo (2020).
