# System Design: LinkedIn Feed

---

## 1. Requirement Clarifications

### Functional Requirements
- Users see a personalized professional feed composed of posts from 1st-degree connections, followed pages, followed hashtags, and algorithmically suggested content
- Users can post text updates, articles, photos, videos, documents (PDFs), and polls
- Users can like (and react), comment, and share/repost posts
- Feed content is filtered by professional relevance and connection degree (1st, 2nd, 3rd degree)
- Users can follow hashtags and see posts tagged with those hashtags in their feed
- Notifications for reactions, comments, shares, and new followers
- "People You May Know" (PYMK) recommendations integrated into the feed
- Connection degree filtering: posts from 2nd/3rd-degree connections surfaced via "suggested" mechanism
- Company page posts appear in feed for followers of that company
- Job recommendations interleaved in feed based on user's professional profile
- Stories (newsletters, articles) from creators the user follows

### Non-Functional Requirements
- Availability: 99.99% uptime — professional platform; outages affect business networking and job searches
- Feed read latency: p99 < 500ms — professional context; users are more tolerant than entertainment apps but still expect responsive UX
- Post write latency: p99 < 300ms — professional posts often represent considered, important career content
- Privacy model: respect "Connections only", "Public", and per-post visibility settings
- Connection degree filtering must be correct — 3rd-degree connections should not appear as 1st-degree
- Eventual consistency on feed freshness: posts appear in connections' feeds within 30 seconds
- Feed must handle viral professional content (a post going from 100 to 1M likes in 24 hours) without degradation
- Scale: 1 billion registered members, ~300 million MAU, ~150 million DAU (assumption based on public LinkedIn reports)

### Out of Scope
- LinkedIn Jobs full search and application tracking system
- LinkedIn Learning (video courses)
- LinkedIn Sales Navigator
- LinkedIn Recruiter platform
- LinkedIn Premium subscription management
- Direct messaging (LinkedIn Messages)

---

## 2. Users & Scale

### User Types
- **Job seekers**: Browse feed for career news, industry insights, PYMK — medium engagement
- **Professionals/employees**: Share career milestones, industry thoughts — periodic posting
- **Thought leaders/creators**: Post frequently, have large followings — high engagement drivers
- **Company page admins**: Post company updates, job postings, thought leadership — content generators
- **Recruiters**: Monitor feeds for talent signals — read-heavy, niche behavior

### Traffic Estimates

| Metric | Value | Reasoning |
|--------|-------|-----------|
| Registered members | 1 billion | Publicly reported figure |
| MAU | 300 million | ~30% of registered members active monthly |
| DAU | 150 million | ~50% of MAU active daily (professional platform, weekday-skewed) |
| Feed loads/day | 7.5 billion | 150M DAU × 50 feed loads/day (lower than Instagram due to professional, focused sessions) |
| Feed read QPS (avg) | ~86,800 | 7.5B / 86,400 |
| Feed read QPS (peak) | ~350,000 | ~4× peak (Monday morning surge in US/EU timezones) |
| Posts/day | 3 million | ~0.02 posts/day per DAU; professional posts are considered, less frequent |
| Post write QPS (avg) | ~35 | 3M / 86,400 |
| Post write QPS (peak) | ~140 | 4× peak multiplier |
| Reactions/day | 300 million | ~100 reactions per post × 3M posts (LinkedIn posts get high engagement) |
| Comments/day | 50 million | |

Note: LinkedIn's scale is ~10–20× smaller than Facebook in terms of daily activity volume, but posts receive higher per-post engagement (professional network effect amplification).

### Latency Requirements
- **Feed read p99: 500ms** — users are in a professional context, have slightly more patience than entertainment apps, but expect a responsive experience; beyond 500ms is noticeably slow
- **Post write acknowledgment p99: 300ms** — professional posts often tie to career moments; fast confirmation important
- **PYMK recommendations: 1s** — recommendations can be computed slightly slowly as they're displayed secondary to the main feed
- **Notification delivery p99: 5s** — professional notifications are important but not time-critical

### Storage Estimates

| Data Type | Size/record | Records/day | Retention | Total |
|-----------|-------------|-------------|-----------|-------|
| Post text | 2 KB avg | 3M/day | Forever | 6 GB/day → ~2 TB/year |
| Post metadata | 500 bytes | 3M/day | Forever | 1.5 GB/day |
| Photos in posts | 3 MB avg | 500K/day | Forever | 1.5 TB/day |
| Videos in posts | 100 MB avg | 100K/day | Forever | 10 TB/day |
| Documents (PDFs) | 2 MB avg | 200K/day | Forever | 400 GB/day |
| User profiles | 10 KB | 1M new/day | Forever | 10 GB/day |
| Connection graph edges | 16 bytes | 10M new/day | Forever | 160 MB/day |
| Reactions | 30 bytes | 300M/day | Forever (ranking signals) | 9 GB/day |
| Comments | 500 bytes | 50M/day | Forever | 25 GB/day |
| Endorsements/Skills | 50 bytes | 5M/day | Forever | 250 MB/day |
| Feed cache (precomputed) | 2 KB/user | 150M active | Rolling 7 days | 300 GB cache |

### Bandwidth Estimates

| Direction | Calculation | Result |
|-----------|-------------|--------|
| Inbound post text | 35 QPS × 2 KB | ~70 KB/s (negligible) |
| Inbound photo uploads | 500K/day × 3MB / 86400 | ~17 MB/s |
| Inbound video uploads | 100K/day × 100MB / 86400 | ~116 MB/s |
| Outbound feed (text+meta) | 86,800 QPS × 8 posts × 2 KB | ~1.4 GB/s |
| Outbound media (CDN) | 86,800 QPS × 2 media posts × 500KB | ~87 GB/s CDN edge |
| CDN origin fill (5% miss rate) | | ~4.4 GB/s origin |

---

## 3. High-Level Architecture

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │                            CLIENTS                                    │
  │          iOS / Android / Web (LinkedIn.com) / LinkedIn API            │
  └──────────────────────────────┬───────────────────────────────────────┘
                                 │ HTTPS / HTTP2
                                 ▼
  ┌──────────────────────────────────────────────────────────────────────┐
  │              CDN (Akamai / Azure CDN)                                 │
  │   Media delivery: profile photos, post images, videos, documents     │
  └──────────────────────────────┬───────────────────────────────────────┘
                                 │
                                 ▼
  ┌──────────────────────────────────────────────────────────────────────┐
  │          API Gateway / Load Balancer (Azure Front Door / internal)    │
  │          Auth (OAuth 2.0), rate limiting, routing, A/B assignment     │
  └────────┬──────────────────┬────────────────────┬──────────────────────┘
           │                  │                    │
  ┌────────▼────────┐  ┌──────▼───────┐  ┌────────▼──────────────────────┐
  │  Post Service   │  │ Feed Service │  │ Identity / Connection Service │
  │  - Create post  │  │ - Ranked     │  │ - Connection graph            │
  │  - Edit, delete │  │   feed gen   │  │ - Degree computation          │
  │  - Media refs   │  │ - Privacy    │  │ - PYMK recommendations        │
  └────────┬────────┘  │   filter     │  └───────────────────────────────┘
           │           │ - Pro signal │
           │           │   ranking    │
           │           └──────┬───────┘
           │                  │
           └──────────────────┴──────────────┐
                              ▼              │
  ┌───────────────────────────────────────────────────────────────────┐
  │                    Kafka Message Bus                               │
  │  Topics: post.created, post.updated, reaction.event,             │
  │          connection.formed, follow.event, feed.fanout             │
  └──────────┬──────────────────────────────────────────────────────┘
             │
   ┌─────────┼───────────────────────────────────────────────────┐
   │         │                                                   │
   ▼         ▼                                                   ▼
┌────────────────────┐  ┌─────────────────────────────────┐  ┌────────────────┐
│  Feed Fanout       │  │  Viral Amplification Service    │  │ Notification   │
│  Worker            │  │  - Detect viral posts           │  │ Service        │
│  - Push post to    │  │  - 2nd-degree amplification     │  │ - Push/email   │
│    connections'    │  │  - Re-fanout to extended network│  │ - In-app       │
│    feed cache      │  └─────────────────────────────────┘  └────────────────┘
│  - Degree-aware    │
│    routing         │
└────────────────────┘

  ┌───────────────────────────────────────────────────────────────────┐
  │                        DATA STORES                                 │
  │                                                                     │
  │  ┌──────────────────┐  ┌──────────────────┐  ┌─────────────────┐  │
  │  │ Post Store       │  │ Feed Cache       │  │ Connection      │  │
  │  │ (Espresso/MySQL) │  │ (Venice — Voldem.│  │ Graph           │  │
  │  │ LinkedIn's own   │  │ backed store)    │  │ (Espresso +     │  │
  │  │ Espresso DB      │  │ Precomp feed     │  │ Samza streams)  │  │
  │  └──────────────────┘  └──────────────────┘  └─────────────────┘  │
  │  ┌──────────────────────────────────────────────────────────────┐  │
  │  │  Feature Store (Pinot / Samza derived stores)                │  │
  │  │  Per-user engagement signals, post virality metrics          │  │
  │  └──────────────────────────────────────────────────────────────┘  │
  │  ┌──────────────────────────────────────────────────────────────┐  │
  │  │  Search Index (Galene — LinkedIn's internal Lucene-based)    │  │
  │  └──────────────────────────────────────────────────────────────┘  │
  └───────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **API Gateway**: Routes requests, enforces OAuth 2.0, applies rate limits (LinkedIn is more aggressive than consumer apps: 500 API calls/day for standard apps). Assigns A/B experiments.
- **Post Service**: Handles post CRUD. Writes to Espresso (LinkedIn's own MySQL-based distributed database). Publishes `post.created` to Kafka.
- **Feed Service**: Primary read path. Fetches precomputed feed from Venice (LinkedIn's derived data store backed by Voldemort key-value store). Applies re-ranking with professional signals, privacy filtering, and injects PYMK/job recommendations.
- **Identity/Connection Service**: Manages the professional graph. Computes connection degrees (1st, 2nd, 3rd) via BFS on the connection graph. Powers PYMK via graph traversal.
- **Feed Fanout Worker**: Consumes `post.created`. Pushes post to 1st-degree connections' feed caches. For high-engagement posts, triggers 2nd-degree amplification.
- **Viral Amplification Service**: Monitors post engagement velocity. When a post from a connection exceeds engagement thresholds, it's re-fanned out to the original poster's 2nd-degree connections (connections of connections who engaged).
- **Kafka**: Durable event bus. LinkedIn famously uses Kafka at massive scale (LinkedIn is where Kafka was created).
- **Venice / Voldemort**: LinkedIn's internal derived data store. Feeds are precomputed documents stored in Venice (key = member_id, value = sorted list of feed items with scores).
- **Espresso**: LinkedIn's MySQL-based distributed database, used for post metadata and user profiles.
- **Pinot**: Apache Pinot is LinkedIn's OLAP data store for real-time analytics and feature serving.
- **Galene**: LinkedIn's internal search infrastructure built on Apache Lucene.

**End-to-End Flow — Reading the Feed:**
1. Client sends `GET /v1/feed` with OAuth token.
2. API Gateway validates token, routes to Feed Service.
3. Feed Service queries Venice for precomputed feed items (member_id → [post_ids + scores]).
4. Feed Service queries Connection Service to determine 1st-degree connections and degree of each post's author.
5. Privacy filter applied: "Connections only" posts visible only to 1st-degree connections.
6. Ranking Service re-ranks with real-time signals (current engagement velocity) from Pinot Feature Store.
7. PYMK injection: insert 1–2 "People You May Know" cards into feed positions 3 and 8.
8. Job injection: insert 1 job recommendation card into position 5 based on user profile (out of scope for feed design).
9. Hydrate post objects from Espresso (or cache), return ranked feed response.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- Posts (Espresso/MySQL, sharded by post_id)
-- Espresso is MySQL-based, so standard SQL schema applies
-- ============================================================
CREATE TABLE posts (
    post_id          BIGINT PRIMARY KEY,      -- LinkedIn's LID (64-bit)
    author_id        BIGINT NOT NULL,          -- Member ID or company page ID
    author_type      TINYINT,                  -- 1=member, 2=company_page
    post_type        TINYINT,                  -- 1=text, 2=photo, 3=video,
                                               -- 4=article, 5=document, 6=poll
    text_content     TEXT,                     -- Max 3000 chars for posts
    article_url      VARCHAR(500),             -- If post_type=article, external URL
    media_keys       JSON,                     -- Array of S3/Azure Blob keys
    hashtags         JSON,                     -- Array of hashtag strings
    mentioned_ids    JSON,                     -- Array of mentioned member/page IDs
    visibility       TINYINT,                  -- 1=public, 2=connections, 3=connections+followers
    like_count       INT DEFAULT 0,
    reaction_counts  JSON DEFAULT '{}',        -- {"like":N, "celebrate":N, "support":N, ...}
    comment_count    INT DEFAULT 0,
    repost_count     INT DEFAULT 0,
    impression_count BIGINT DEFAULT 0,
    virality_score   FLOAT DEFAULT 0.0,        -- Computed by viral detection service
    is_deleted       BOOLEAN DEFAULT FALSE,
    created_at       DATETIME DEFAULT NOW(),
    updated_at       DATETIME DEFAULT NOW() ON UPDATE NOW(),
    INDEX idx_author (author_id, created_at DESC),
    INDEX idx_hashtags (hashtags(100))         -- Prefix index for hashtag search
);

-- Poll details (if post_type = 6)
CREATE TABLE poll_options (
    post_id      BIGINT,
    option_id    INT,
    option_text  VARCHAR(150),
    vote_count   INT DEFAULT 0,
    PRIMARY KEY (post_id, option_id)
);

CREATE TABLE poll_votes (
    post_id    BIGINT,
    voter_id   BIGINT,
    option_id  INT,
    voted_at   DATETIME DEFAULT NOW(),
    PRIMARY KEY (post_id, voter_id)
);

-- ============================================================
-- Members (Espresso/MySQL — relational with strong consistency)
-- ============================================================
CREATE TABLE members (
    member_id         BIGINT PRIMARY KEY,
    public_identifier VARCHAR(100) UNIQUE,     -- linkedin.com/in/{this}
    first_name        VARCHAR(100),
    last_name         VARCHAR(100),
    headline          VARCHAR(220),
    profile_photo_key VARCHAR(255),
    industry_id       INT,
    location_id       INT,
    current_company_id BIGINT,
    is_open_to_work   BOOLEAN DEFAULT FALSE,
    is_hiring         BOOLEAN DEFAULT FALSE,
    connection_count  INT DEFAULT 0,
    follower_count    INT DEFAULT 0,
    is_influencer     BOOLEAN DEFAULT FALSE,   -- LinkedIn-certified creator
    created_at        DATETIME DEFAULT NOW(),
    INDEX idx_public_id (public_identifier)
);

-- ============================================================
-- Connection Graph (Espresso + derived/materialized in Samza)
-- ============================================================
CREATE TABLE connections (
    member_id_1  BIGINT NOT NULL,
    member_id_2  BIGINT NOT NULL,
    connected_at DATETIME DEFAULT NOW(),
    strength     FLOAT DEFAULT 0.5,          -- Computed: interaction frequency
    PRIMARY KEY (member_id_1, member_id_2),
    INDEX idx_member2 (member_id_2, member_id_1)
);

-- ============================================================
-- Follows (members following companies, influencers, hashtags)
-- ============================================================
CREATE TABLE follows (
    follower_id  BIGINT NOT NULL,
    entity_id    BIGINT NOT NULL,
    entity_type  TINYINT,                    -- 1=member, 2=company, 3=hashtag
    followed_at  DATETIME DEFAULT NOW(),
    PRIMARY KEY (follower_id, entity_id, entity_type),
    INDEX idx_entity (entity_id, entity_type)
);

-- ============================================================
-- Reactions (Espresso/MySQL)
-- ============================================================
CREATE TABLE reactions (
    post_id        BIGINT NOT NULL,
    reactor_id     BIGINT NOT NULL,
    reaction_type  TINYINT,                  -- 1=like, 2=celebrate, 3=support,
                                             -- 4=funny, 5=love, 6=insightful, 7=curious
    reacted_at     DATETIME DEFAULT NOW(),
    PRIMARY KEY (post_id, reactor_id),
    INDEX idx_reactor (reactor_id, reacted_at DESC)
);

-- ============================================================
-- Comments (Espresso/MySQL — threaded, not as high write rate
-- as Instagram/Facebook; professional comments are considered)
-- ============================================================
CREATE TABLE comments (
    comment_id       BIGINT PRIMARY KEY,
    post_id          BIGINT NOT NULL,
    author_id        BIGINT NOT NULL,
    body             TEXT,                   -- Max 1250 chars
    parent_comment_id BIGINT,               -- For nested replies (max 2 levels)
    reaction_counts  JSON DEFAULT '{}',
    is_deleted       BOOLEAN DEFAULT FALSE,
    created_at       DATETIME DEFAULT NOW(),
    INDEX idx_post (post_id, created_at ASC)
);

-- ============================================================
-- Feed Cache (Venice / Voldemort — logical schema)
-- Key:   member_id (BIGINT)
-- Value: JSON array of feed items, sorted by rank_score DESC
-- Schema per item: {post_id, author_id, source_type, rank_score,
--                   connection_degree, added_at}
-- Max:   1000 items per member
-- TTL:   7 days
-- ============================================================

-- ============================================================
-- Feature Store (Apache Pinot — OLAP, real-time aggregation)
-- Tables (Pinot real-time tables):
-- - member_engagement_signals (member_id, action, post_id, ts)
-- - post_virality_metrics (post_id, likes_1h, comments_1h, reposts_1h,
--                          unique_companies_engaged, geographic_spread_score)
-- - member_interest_profile (member_id, industry_weights, skill_weights,
--                             company_type_pref, content_type_pref)
-- ============================================================
```

### Database Choice

| Database | Use Case | Pros | Cons |
|----------|----------|------|------|
| Espresso (MySQL-based) | Posts, members, connections, reactions | ACID for professional data (connections, profiles are high-correctness); LinkedIn's own battle-tested distributed MySQL | Operational complexity of custom DB; requires LinkedIn-internal expertise |
| Venice / Voldemort | Precomputed feed cache, derived data | High read throughput; purpose-built for LinkedIn's derived data patterns; supports batch and streaming updates | Not a general-purpose DB; LinkedIn-internal |
| Apache Pinot | Feature store, real-time analytics | Sub-second OLAP queries on streaming data; real-time ingestion from Kafka; LinkedIn open-sourced it | Complex operations; not suited for transactional workloads |
| Apache Kafka | Event streaming | LinkedIn created Kafka; proven at LinkedIn's scale for all async pipelines | Requires careful partition management; operational overhead |
| Azure Blob Storage / S3 | Media blobs | Unlimited scale; native CDN integration | Not queryable |
| Galene (Lucene-based) | Search | Full-text search across posts, profiles, companies; LinkedIn-internal | LinkedIn-specific; equivalent to Elasticsearch for design purposes |

**Decision:** Using Espresso (MySQL-based) for primary data because LinkedIn values data consistency for professional network data. A connection formed must be immediately queryable — eventual consistency on professional relationships is not acceptable (e.g., a recruiter immediately seeing a just-connected candidate). Venice for feed cache because LinkedIn's derived data store was specifically designed for feed-at-scale precomputation patterns.

---

## 5. API Design

```
GET /v1/feed
  Description: Get personalized professional feed
  Auth: OAuth 2.0 Bearer token (required)
  Query params:
    cursor: string (opaque pagination cursor)
    count: int (default 10, max 25)
    filter: "all"|"connections"|"following"|"jobs"
  Response 200: {
    feed_items: [{
      item_type: "post"|"pymk_card"|"job_card"|"ad"|"newsletter",
      post: {
        post_id: string,
        author: {
          member_id: string,
          name: string,
          headline: string,
          profile_photo_url: string,
          connection_degree: 1 | 2 | 3 | null,
          is_following: bool
        },
        content: {
          text: string | null,
          media: [{ url, type, thumbnail_url, width, height }] | null,
          document: { url, title, page_count } | null,
          poll: { options: [{ id, text, vote_count }], total_votes: int,
                  has_voted: bool, my_vote: int | null } | null
        },
        hashtags: string[],
        reactions: { like: int, celebrate: int, support: int, funny: int,
                     love: int, insightful: int, curious: int },
        my_reaction: string | null,
        comment_count: int,
        repost_count: int,
        impression_count: int,
        created_at: ISO8601,
        visibility: string
      } | null,
      pymk_card: {
        suggested_members: [{
          member_id, name, headline, profile_photo_url,
          mutual_connections_count: int, reason: string
        }]
      } | null
    }],
    next_cursor: string | null,
    session_id: string
  }

POST /v1/posts
  Description: Create a post
  Auth: OAuth 2.0 Bearer token (required)
  Rate limit: 150 posts/day (text), 50 photo/video posts/day
  Request: {
    text: string (max 3000 chars for posts, 125000 chars for articles),
    post_type: "text"|"photo"|"video"|"article"|"document"|"poll",
    media_ids: string[],       -- From /v1/media/upload
    hashtags: string[],        -- Max 30 hashtags
    visibility: "public"|"connections"|"connections_and_followers",
    poll_options: string[] | null,   -- For polls: 2-4 options
    poll_duration_days: int | null   -- 1, 3, 7, or 14 days
  }
  Response 201: {
    post_id: string,
    created_at: ISO8601,
    share_url: string           -- Public URL for the post
  }

PUT /v1/posts/{post_id}
  Description: Edit a post (within 24 hours of creation)
  Auth: Bearer token (author only)
  Request: { text: string, hashtags: string[] }
  Response 200: { post_id, updated_at }

DELETE /v1/posts/{post_id}
  Description: Delete a post
  Auth: Bearer token (author only)
  Response 204

POST /v1/posts/{post_id}/reactions
  Description: React to a post (like, celebrate, support, etc.)
  Auth: Bearer token (required)
  Request: { reaction_type: "like"|"celebrate"|"support"|"funny"|"love"|"insightful"|"curious" }
  Response 200: { my_reaction: string, reaction_counts: {...} }

DELETE /v1/posts/{post_id}/reactions
  Description: Remove reaction
  Auth: Bearer token (required)
  Response 200: { my_reaction: null, reaction_counts: {...} }

GET /v1/posts/{post_id}/comments
  Description: Get comments for a post
  Auth: Bearer token (required — privacy check)
  Query params: cursor, count (default 10), sort: "top"|"recent"
  Response 200: {
    comments: [{
      comment_id, author: { member_id, name, headline, photo_url, connection_degree },
      body, reaction_counts, replies: Comment[],
      created_at
    }],
    next_cursor: string | null
  }

POST /v1/posts/{post_id}/comments
  Description: Comment on a post
  Auth: Bearer token (required)
  Request: { body: string (max 1250 chars), parent_comment_id: string | null }
  Response 201: { comment_id, body, created_at }

POST /v1/posts/{post_id}/repost
  Description: Repost/share a post to own network
  Auth: Bearer token (required)
  Request: { text: string | null, visibility: string }
  Response 201: { post_id: string }

POST /v1/connections
  Description: Send a connection request
  Auth: Bearer token (required)
  Request: { target_member_id: string, message: string | null }
  Response 201: { status: "pending" }

PUT /v1/connections/{member_id}
  Description: Accept or decline connection request
  Auth: Bearer token (required)
  Request: { action: "accept"|"decline" }
  Response 200: { status: "connected"|"declined" }

POST /v1/follows
  Description: Follow a member, company, or hashtag
  Auth: Bearer token (required)
  Request: { entity_id: string, entity_type: "member"|"company"|"hashtag" }
  Response 201: { following: true }

GET /v1/feed/hashtag/{hashtag}
  Description: Get posts for a specific hashtag
  Auth: Bearer token (required)
  Query params: cursor, count, sort: "top"|"recent"
  Response 200: { posts: Post[], next_cursor: string | null }
```

---

## 6. Deep Dive: Core Components

### Component: Professional Network Feed Ranking

**Problem it solves:**
LinkedIn's feed ranking is fundamentally different from Twitter or Facebook because: (1) the primary signal is professional relevance, not pure entertainment engagement; (2) connection degree matters — a post from your direct manager is more valuable than from a 3rd-degree contact; (3) professional content has longer engagement lifespans (a thought leadership post may get comments for weeks); (4) viral amplification patterns are different — professional content can spread rapidly through company-specific networks. The ranking algorithm must balance: professional relevance, connection affinity, content quality, recency, and diversity of topics and industries.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Chronological by connection degree | 1st degree first, then 2nd, within each degree by recency | Transparent, simple | Ignores relevance; top connections may all post about same topic at same time |
| Engagement-weighted (Facebook-style) | Sort by predicted engagement (likes × weight + comments × weight) | Captures quality | Favors already-viral content; underweights niche professional content |
| Two-factor: relevance × recency | Score = professional_relevance_score × time_decay | Balances timeliness and relevance | Hard to define "professional relevance" — requires domain-aware model |
| Cascade ML model (LinkedIn's actual approach) | Multi-stage: engagement prediction → professional relevance filter → diversity injection | Best quality | Complexity; requires large labeled training set |

**Selected Approach: Multi-stage cascade with professional signals**

LinkedIn's published research (Project Escoffier, Feed Quality Score) describes a multi-stage pipeline:
1. **Candidate retrieval**: Fetch ~2000 candidates from Venice feed cache (precomputed from connections and follows).
2. **Engagement prediction**: Lightweight GBDT predicts probability of like/comment/share.
3. **Professional relevance scoring**: Rule-based and ML filter for professional content quality — penalizes personal content, political content (LinkedIn reduced political feed in 2022), engagement bait.
4. **Diversity enforcement**: Ensure no more than 3 posts from same author, reasonable topic spread.
5. **Viral amplification injection**: 2nd-degree amplified posts inserted based on virality scores from Pinot.
6. **Return top-N ranked items**.

**Professional Relevance Signals:**

```python
professional_signals = {
    # Content quality
    'is_professional_topic': bool,      # NLP classifier: career/industry/business topics
    'has_engagement_bait': bool,        # "Like if you agree", "Tag someone" patterns
    'spam_probability': float,          # Spam/phishing classifier score
    'originality_score': float,         # Is this original content or reposted from elsewhere?

    # Viewer-author professional relationship
    'same_industry': bool,              # Author in same industry as viewer
    'same_company': bool,               # Author at same company as viewer
    'connection_degree': int,           # 1, 2, 3, or None (follower only)
    'interaction_recency_days': float,  # Days since last interaction with this author
    'connection_strength': float,       # Frequency of past interactions (comments, reactions)

    # Post virality in professional network
    'unique_companies_count': int,      # How many distinct companies have engaged
    'seniority_weighted_reactions': float,  # Reactions from senior professionals weighted higher
    'industry_spread_score': float,     # How many industries have engaged
    'expert_engagement': bool,          # Did LinkedIn-recognized experts engage?

    # Temporal
    'post_age_hours': float,
    'engagement_velocity_1h': float,
    'engagement_velocity_24h': float,
}

def rank_feed(viewer_id, candidates):
    # Stage 1: Engagement probability prediction (fast GBDT)
    eng_scores = engagement_predictor.predict(viewer_id, candidates)

    # Stage 2: Professional relevance score
    rel_scores = professional_relevance_model.predict(viewer_id, candidates)

    # Stage 3: Combined score with connection degree multiplier
    combined = []
    for post_id, eng_score, rel_score in zip(candidates, eng_scores, rel_scores):
        degree = get_connection_degree(viewer_id, posts[post_id].author_id)
        degree_boost = {1: 2.0, 2: 1.3, 3: 0.8, None: 0.5}[degree]

        final_score = (eng_score * 0.4 + rel_score * 0.6) * degree_boost
        combined.append((post_id, final_score))

    ranked = sorted(combined, key=lambda x: -x[1])

    # Stage 4: Diversity: max 2 posts from same author per 10 items
    diversified = apply_author_diversity(ranked, max_per_author=2, window=10)

    # Stage 5: Inject 2nd-degree viral content
    viral_candidates = get_viral_2nd_degree_posts(viewer_id)
    final = inject_at_positions(diversified, viral_candidates,
                                 positions=[4, 9, 14])
    return final[:25]
```

**Interviewer Q&As:**

Q: LinkedIn reduced political content in their feed. How do you implement "content category suppression" without breaking the ranking model?
A: Content category suppression is implemented as a post-ranking filter, not a ranking model change. Process: (1) A multi-label text classifier (fine-tuned BERT) categorizes each post into categories: [career, industry_news, political, personal_life, entertainment, business]. (2) At ranking time, after scores are computed, posts in suppressed categories (political) have their final score multiplied by a suppression factor (e.g., 0.1 — not zero, allowing rare political content if connection strength is extremely high). (3) A hard cap: max 1 political post per 25 feed items. (4) The suppression factors are configurable via a feature flag, allowing LinkedIn to tune suppression rates by region (different norms in different countries) without model retraining.

Q: How do you detect "engagement bait" on a professional platform where "like if you agree" is common?
A: Professional engagement bait patterns differ from Facebook's. LinkedIn-specific patterns: "Comment your job title if...", "Drop a 🎉 if you've ever...", "Tag a colleague who...". Detection: (1) Regex patterns for common engagement bait phrases (fast, low-latency, applied at post creation). (2) Feature in ranking model: historical engagement-to-impression ratio anomalies (posts that get unusually high comment rates but low dwell time post-click are likely bait). (3) User feedback loop: "I don't want to see this type of post" signals feed into a weekly model update. (4) Cross-referencing post engagement patterns: if a post gets 10× more reactions than comments for the author's historical ratio, flag as potential bait.

Q: How does LinkedIn weigh "viral in professional circles" vs. "viral generally"?
A: General virality (total likes) can be inflated by automation or emotionally charged non-professional content. LinkedIn's virality metrics are profession-weighted: `professional_virality_score = Σ reaction_weight × seniority_weight × industry_diversity_bonus`. Reactions from members with verified job titles at recognizable companies get higher weight than anonymous accounts. Industry diversity bonus: a post liked by members from 5+ different industries (measured by SIC code of current employer) scores higher than a post liked only within one industry (which may reflect in-group bias rather than broadly relevant content). These are computed in Pinot from the reaction stream.

Q: What happens when a CEO posts to their company page's 2M followers? How does your fanout handle it?
A: Company pages with > 100K followers use pull-based fan-out (same celebrity strategy). Company page posts are NOT written to each follower's Venice feed. Instead: (1) Feed Service identifies which company pages the user follows (from the follows table, cached in Redis per session). (2) For those pages, queries Espresso for their recent posts directly. (3) Merges with the precomputed connection feed from Venice. (4) Ranks the merged candidate set. This is the LinkedIn equivalent of the Twitter/Instagram celebrity fan-out problem, extended to company pages.

Q: How do you measure "professional quality" of a post objectively enough to train an ML model?
A: Labels for the professional quality model come from: (1) Human editorial labels: LinkedIn has a content quality team that rates posts for a ground-truth dataset. (2) Proxy signals: posts that lead to profile views of the author, connection requests from post viewers, or job application referrals are retrospectively labeled "high quality." (3) Long-tail engagement: professional quality posts tend to get comments days or weeks after posting (vs. entertainment content that spikes and dies). (4) Share behavior: posts shared with a thoughtful repost text (not just "interesting!") signal substantive content. These signals train a quality classifier that outputs `professional_quality_score ∈ [0,1]`.

---

### Component: Viral Content Amplification System

**Problem it solves:**
LinkedIn's most unique feed behavior: a post from a 1st-degree connection can organically reach a user's 2nd-degree connections when multiple 1st-degree connections engage with it. This "social proof + professional relevance" signal drives much of LinkedIn's feed engagement. The system must detect when a post is trending in a professional network and amplify it to the extended network appropriately, without creating echo chambers or overwhelming feeds with one viral post.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Naive 2nd-degree fan-out | When post crosses reaction threshold, fan-out to all 2nd-degree connections | Simple logic | Fan-out explosion: a 500-connection account × 200 connections each = 100K fan-out writes per viral threshold crossing |
| Social proof signals in ranking | Add "X connections liked this" to post features; ranker decides visibility | No extra fan-out; elegant | Requires post to already be in candidate set — can't surface completely new posts |
| Viral detection + targeted injection | Detect viral posts via Pinot; inject into 2nd-degree feeds at read time | No fan-out explosion; fresh content | Requires read-time lookup of viral posts; adds latency |
| Samza-based stream processing | Kafka → Samza detects viral thresholds; writes to Venice with "viral_injection" flag | Real-time detection; scalable | Eventual consistency on viral propagation (acceptable) |

**Selected Approach: Pinot-based viral detection + read-time injection**

```python
# Viral Detection (Samza/Flink job consuming Kafka reactions stream)
class ViralPostDetector:
    def process_reaction_event(self, reaction_event):
        post_id = reaction_event['post_id']
        reactor_id = reaction_event['reactor_id']

        # Update Pinot counters (handled by Pinot's Kafka consumer)
        # This is automatic via Pinot real-time table ingestion

        # Check if virality thresholds crossed (query Pinot)
        metrics = pinot.query(f"""
            SELECT post_id, COUNT(*) as reactions_1h,
                   COUNT(DISTINCT company_id) as company_diversity,
                   COUNT(DISTINCT industry_id) as industry_diversity
            FROM reactions
            WHERE post_id = '{post_id}'
              AND reacted_at > NOW() - INTERVAL 1 HOUR
        """)

        virality_score = (
            metrics.reactions_1h * 1.0 +
            metrics.company_diversity * 5.0 +       # Company diversity is high signal
            metrics.industry_diversity * 3.0
        )

        if virality_score > VIRAL_THRESHOLD:  # e.g., > 100
            # Mark post as viral in Venice
            venice.put(f"viral_post:{post_id}",
                       {'virality_score': virality_score,
                        'author_id': reaction_event['author_id'],
                        'expires_at': now() + timedelta(hours=48)})

            # Publish viral event for 2nd-degree injection
            kafka.publish('post.went_viral',
                          {'post_id': post_id, 'virality_score': virality_score})

# Feed Service: inject viral posts at read time
def get_viral_2nd_degree_posts(viewer_id):
    # Get 1st-degree connections
    connections_1st = connection_service.get_connections(viewer_id, degree=1)

    # For each 1st-degree connection, get viral posts their connections engaged with
    # (2nd-degree viral amplification)
    viral_post_ids = []
    for conn_id in connections_1st[:50]:  # Cap to 50 connections for performance
        viral_by_friend = venice.get(f"viral_by_member:{conn_id}", default=[])
        viral_post_ids.extend(viral_by_friend)

    # Deduplicate and sort by virality_score
    unique_viral = {}
    for pid in viral_post_ids:
        if pid not in unique_viral:
            info = venice.get(f"viral_post:{pid}")
            if info:
                unique_viral[pid] = info['virality_score']

    return sorted(unique_viral.keys(), key=lambda p: -unique_viral[p])[:10]
```

**Interviewer Q&As:**

Q: When a post "goes viral" on LinkedIn, how do you prevent it from appearing 5 times in the same feed (from 5 different connections all engaging with it)?
A: Story deduplication (same as Facebook approach): (1) The viral post is detected as a single entity by post_id. (2) At feed read time, when merging the precomputed Venice feed (which may contain the post from multiple fan-out sources — friend A liked it, friend B commented) with the viral injections, we deduplicate by post_id. (3) The deduplicated post is displayed with a "social proof" annotation: "Alice, Bob, and 3 others in your network found this insightful" — showing 5 engagers instead of showing the post 5 times. (4) The ranking score for the deduplicated post uses the maximum score across all the duplicate sources, not a sum (to avoid inflating rank for content with many weak signals).

Q: How do you prevent a single viral post from dominating the LinkedIn feed for days?
A: Temporal decay applied to virality score: `virality_score_effective = virality_score × e^(-λ × hours_since_peak)` where λ is tuned so a post reaches 50% of peak virality score after 24 hours. Additionally: (1) Per-user impression tracking: if a user has already seen this viral post (impression recorded), its virality injection priority drops to zero. (2) Maximum injection frequency: a single post can be injected into the same user's feed at most once. (3) Feed session diversity: across one session (tracked by session_id), hard cap of 3 viral injection slots total, regardless of how many posts meet the virality threshold.

Q: LinkedIn members can be inactive for weeks. How does viral content reach them when they return?
A: When a user returns after > 7 days, their Venice feed cache is stale or expired. On cold return: (1) Feed Service detects cold return (last_active > 3 days). (2) Triggers a "catch-up" feed mode: query Pinot for top viral posts from the past 7 days among the user's connection network. (3) These posts are shown in a special "Highlights while you were away" section. (4) Standard feed follows with current precomputed/rebuilt candidates. The viral catch-up is based on accumulated engagement signals from Pinot, not on real-time fan-out — so even posts that peaked and declined are still surfaceable if they were highly relevant.

---

### Component: Connection Degree Computation

**Problem it solves:**
LinkedIn's defining feature is the connection degree between members (1st, 2nd, 3rd+ degree). This degree determines: feed candidate inclusion, privacy of profile visibility, search result ordering, and recruiter access. For a 1B member graph with billions of connection edges, computing degrees efficiently is a non-trivial distributed graph problem.

**Implementation Detail:**

Connection degree between member A and member B is defined as:
- 1st: Direct connection (edge in connections table)
- 2nd: Share at least one mutual 1st-degree connection
- 3rd: Share a 2nd-degree connection (at most 2 hops from a mutual connection)
- 3rd+: More than 3 hops

For feed generation, we need: for each post in the candidate set, what is the degree between the viewer and the post's author?

```python
class ConnectionDegreeService:
    def __init__(self):
        # Pre-materialized: all (member_A, member_B, degree) pairs for degree 1 and 2
        # Stored in Venice (derived from Samza graph BFS job)
        self.venice = Venice()
        self.connection_cache = Redis()

    def get_degree(self, viewer_id, author_id):
        # Fast path: check Redis cache (TTL = 1 hour)
        cache_key = f"degree:{min(viewer_id, author_id)}:{max(viewer_id, author_id)}"
        cached = self.connection_cache.get(cache_key)
        if cached:
            return int(cached)

        # Venice lookup: pre-materialized degree table
        degree = self.venice.get(f"connection_degree:{viewer_id}:{author_id}")

        if degree is None:
            # Not in 1st or 2nd degree materialized table
            # Check 3rd degree: does viewer have any 2nd-degree connection with author?
            degree = self._compute_3rd_degree(viewer_id, author_id)

        self.connection_cache.setex(cache_key, 3600, str(degree))
        return degree

    def get_degree_batch(self, viewer_id, author_ids):
        # Batch multi-get from Venice + Redis for efficiency
        # Used at feed read time for 50-100 candidates simultaneously
        cache_keys = {f"degree:{min(viewer_id, aid)}:{max(viewer_id, aid)}": aid
                      for aid in author_ids}
        cached_results = self.connection_cache.mget(list(cache_keys.keys()))
        # ... merge cached and uncached lookups

# Offline pre-materialization (Samza/Spark job runs hourly)
# Input: connections table (stream of new connections)
# Output: Venice table "connection_degree:{id1}:{id2}" for all 1st and 2nd degree pairs

# For N members with average 400 connections each:
# 1st degree pairs: 400M × 400M (bounded by actual connections) ≈ N × avg_degree
# 2nd degree pairs: (400 avg connections)^2 = 160,000 per member × 300M active members
# = 48 trillion pairs — too large to fully materialize
# Solution: materialize only for active members (DAU × avg_degree^2 is manageable)
# Use bloom filters for 2nd-degree membership test for inactive members
```

**Interviewer Q&As:**

Q: How do you keep connection degree calculations fresh when connection graph changes in real-time?
A: Connection degree pre-materialization for 1st degree is maintained incrementally: when A connects with B, a Samza stream job updates: (1) Venice entry `connection_degree:A:B = 1` and vice versa. (2) All of A's 1st-degree connections now have degree 2 to B (if they didn't have a shorter path already). This 2nd-degree update is O(avg_degree) = O(400) per new connection. (3) Invalidate related Redis cache entries via Kafka invalidation events. For 3rd degree, approximate computation acceptable — use bloom filter of A's 2nd-degree network.

Q: Full materialization of 2nd-degree pairs is 48 trillion entries — too large. How do you actually compute "degree 2" at query time?
A: Instead of materializing all pairs: (1) Materialize each member's 1st-degree connection list (set of member_ids) in Venice (Venice supports set data structures). (2) At query time, "is B in A's 2nd-degree?" = "does intersection of A's connections and B's connections have any members?" (3) This set intersection is done using Bloom filters per member stored in Redis: `bloom_1st_degree:{member_id}` — each member has a Bloom filter of their 1st-degree connections (400 items → ~3KB Bloom filter with 1% false positive rate). (4) Degree-2 check: query Bloom filter `bloom_1st_degree:{B}` for any of A's 1st-degree connections. O(degree_A) Bloom filter lookups, ~400 operations at 1μs each = 0.4ms.

---

## 7. Scaling

### Horizontal Scaling

**Post Service**: Very low write QPS (35/sec average). Scales easily with 5–10 instances. Bottleneck is Espresso writes — use connection pooling + Espresso's multi-master support for write distribution.

**Feed Service**: 86,800 read QPS average, 350,000 peak. With each instance handling ~5,000 QPS (Venice reads are batch-efficient), need ~70 instances average, 70 peak. Auto-scale on CPU using Kubernetes HPA.

**Feed Fanout Workers**: At 35 posts/sec × 500 avg connections = 17,500 Venice writes/sec — very manageable. Scale with 20–50 Kafka consumer instances.

**Connection Degree Service**: 86,800 feed reads × 10 candidate authors = 868,000 degree lookups/sec. With Redis caching (>95% hit rate), actual Venice/Espresso queries ~43,000/sec. 20 Connection Service instances at 2K QPS each suffices.

### Database Scaling

**Sharding:**

*Espresso (posts)*: Sharded by `post_id % num_shards`. Each shard is a MySQL instance with Espresso's routing layer. With 3M posts/day × 365 days × 3 years = ~3.3B posts total, with average 2 KB each = ~6.6 TB of text metadata across all shards. 100 shards × 66 GB each — manageable.

*Espresso (connections)*: Sharded by `member_id_1 % num_shards`. Reverse index (member_id_2 → connections) stored in a separate shard group for efficient reverse lookups.

*Venice (feed cache)*: Key-value store naturally partitioned by member_id. With 300M DAU × 1000 items × 200 bytes = 60 TB of feed cache — requires a large Venice cluster, but Venice is designed for this (LinkedIn runs Venice at petabyte scale internally).

**Replication:**

*Espresso*: Multi-master with primary writes and read replicas. Semi-synchronous replication (at least 1 replica acknowledges). LinkedIn operates Espresso across multiple data centers with async cross-datacenter replication.

*Venice*: Built for high-read-throughput with multiple replicas per partition. Supports push-based updates (Samza stream pushes precomputed values to Venice replicas) rather than pull-based.

**Caching:**

- **Venice (feed candidates)**: Primary feed cache — precomputed, extremely high read throughput. Backed by Voldemort (LinkedIn's internal key-value store).
- **Redis (connection degree cache)**: Degree pairs cached with 1-hour TTL. Prevents repeated Espresso/Venice lookups for the same (viewer, author) pair across multiple feed loads.
- **Redis (bloom filter)**: 1st-degree bloom filter per active member (3KB each × 300M active members = 900 GB Redis storage). Used for O(1) approximate degree-2 checks.
- **Pinot (feature store)**: OLAP for real-time feature serving. Not a cache — authoritative source for real-time engagement metrics.
- **CDN (Akamai/Azure CDN)**: Media delivery. Profile photos: `max-age=86400`. Post media: `max-age=2592000, immutable`. Document previews: `max-age=3600`.

**CDN:**

LinkedIn serves media through Azure CDN (Microsoft acquired LinkedIn). Content-addressed keys ensure immutable caching. Profile photos use a versioned URL scheme (hash in path, new URL on update). Documents (PDFs) are stored in Azure Blob Storage and served via CDN with pre-generated thumbnail previews (page 1 shown as image in feed).

### Interviewer Q&As on Scaling

Q: How does LinkedIn handle the "super connector" — a member with 30,000+ connections — for fan-out?
A: Same celebrity strategy: above a threshold (~5,000 connections), switch from push fan-out to pull-based. Super connectors' posts are not pushed to each connection's Venice feed. Instead: (1) Feed Service checks if viewer follows any super connectors (Redis set: `followed_super_connectors:{member_id}`). (2) Fetches super connector recent posts from Espresso directly. (3) Merges with Venice precomputed feed. (4) This lookup is bounded: maximum ~20 followed super connectors × last 5 posts each = 100 post fetches at read time. Additionally, super connector posts often qualify as viral, so they'd enter the viral injection channel described earlier.

Q: Venice holds 60 TB of feed cache. How do you evict stale entries?
A: Venice entries have per-member TTL (7 days of inactivity). For active members (daily users), entries are continuously updated by fan-out writes and never expire. For inactive members (no logins in 7 days), entries are evicted on expiry. Cold return: Venice miss triggers on-demand feed reconstruction as described. Storage management: Venice uses log-structured storage with compaction (similar to Cassandra LSM-tree). Regular compaction runs reduce space amplification. Venice's storage is tiered: hot data (active DAU) in SSD, warm/cold in HDD.

Q: How do you handle the "viral post creates thundering herd" problem at the Venice layer?
A: When a viral detection event is published, many Fan-out Workers and the Viral Injection pipeline all simultaneously try to read/write Venice for the viral post. Mitigations: (1) Single-writer principle: only one designated Viral Amplification Service writes viral annotations to Venice; other services read-only. (2) Fan-out batching: Fan-out Workers batch up to 500 Venice writes per Kafka commit, reducing write operations from O(reactions) to O(reactions/500). (3) Cache coalescing at Venice: multiple concurrent reads for the same viral post_id are served from a single cache entry (similar to request coalescing in Memcache). (4) Rate limiting on viral write path: max 1000 Venice writes/second per post_id.

Q: LinkedIn has a strong "People You May Know" recommendation engine. How does it integrate with the feed without adding latency?
A: PYMK recommendations are precomputed offline (nightly Spark job on the connection graph: 2-hop graph traversal, mutual connection count, company co-workers, profile viewers). Precomputed recommendations stored in Venice: `pymk:{member_id}` → list of (candidate_id, reason_string, score). Feed Service reads this from Venice as part of the same batch read that fetches feed candidates — no additional network call. PYMK cards injected at fixed positions (positions 3 and 8 in each page). The PYMK Venice entry is refreshed daily, so recommendations are at most 24 hours stale — acceptable for a slow-changing feature.

Q: How does LinkedIn scale to support 1 billion member profile reads during feed hydration?
A: Profile reads during feed generation (fetching author profile data for each post in the feed) are the most expensive per-request operation. Mitigation strategy: (1) Profile data is highly cacheable — members update profiles infrequently. Redis cache `profile:{member_id}` with 1-hour TTL; hit rate >95%. (2) Feed Service batches all profile fetches for a single feed page into one Redis MGET call (10–25 profile keys per feed page = single round trip). (3) LinkedIn's internal microservice graph uses Protocol Buffers + gRPC for efficient binary serialization of profile objects — ~2× more efficient than JSON. (4) Partial profile (id, name, headline, photo_key, connection_degree) sufficient for feed display — avoid fetching full profile including experience/education/skills sections until user clicks through.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Impact | Mitigation |
|-----------|-------------|--------|------------|
| Post Service crash | Post creation fails | User's post not created | Stateless; LB reroutes; client retries with idempotency key |
| Venice node failure | Feed cache miss for affected partition | Feed reconstructed from Espresso + Pinot (slower) | Venice replication (N+1); fallback to on-demand feed generation |
| Espresso shard failure | Posts on that shard unreadable | Posts from affected authors missing from feed | Semi-sync replication; Espresso auto-failover to replica; RPO < 1 transaction |
| Kafka broker failure | Fan-out events delayed | Feeds stale; posts don't appear in connections' feeds | Kafka RF=3; new leader elected in <30s; no event loss; fan-out eventually consistent |
| Connection Degree Service unavailable | Degree not available for privacy check | Default to "unknown degree" — show only public posts (fail closed) | Circuit breaker with safe fallback |
| Pinot OLAP unavailable | Feature store queries fail | Ranking falls back to pre-scoring from Venice meta; viral injection paused | Circuit breaker; ranking degrades gracefully to engagement pre-score |
| CDN outage | Media serving fails in affected region | Images/videos don't load; text-only feed served | CDN failover to backup PoP; text-first progressive loading |
| Viral Amplification Service crash | Viral posts not detected | Viral content distribution reduced | Non-critical; Kafka event replay resumes detection on recovery |

### Failover Strategy

- **Multi-region**: LinkedIn operates across multiple Azure regions. GeoDNS routes users to nearest healthy region. Espresso cross-region async replication (~200ms lag). Venice multi-region with read from local replica.
- **Connection degree degraded mode**: If degree computation fails, privacy filter defaults to excluding all non-Public posts from the feed. This is correct (over-exclusive) rather than wrong (over-inclusive). A post intended for "Connections only" must not leak.
- **Feed cold fallback**: If both Venice AND Espresso are unavailable, serve a "trending on LinkedIn" feed (cached globally in Redis, refreshed every 5 minutes) rather than a 503 error.

### Retries & Idempotency

- **Post creation**: Client generates UUID idempotency key. Post Service stores `(idempotency_key → post_id)` in Redis with 24h TTL. Duplicate POST returns same post_id.
- **Reaction write**: Espresso PK on `(post_id, reactor_id)` ensures idempotent reaction writes. `INSERT ... ON DUPLICATE KEY UPDATE reaction_type = ?` handles reaction type changes.
- **Fan-out writes to Venice**: Venice PUT is idempotent (overwrite same key with same/updated value). Kafka replay produces same Venice state.
- **Connection formation**: `INSERT INTO connections ... ON DUPLICATE KEY IGNORE` prevents duplicate connections from double-tap bug.

### Circuit Breaker

- Ranking Service circuit: opens on >30% error rate over 10s. Fallback: Venice pre-score ordering (connection degree + recency sort only).
- Connection Degree circuit: opens on >20% timeout rate. Fallback: degree = 3 for all (conservative; reduces feed quality but maintains privacy).
- Viral Injection circuit: opens on >50% Pinot query failure. Fallback: skip viral injection entirely; serve standard precomputed feed only.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Why It Matters |
|--------|------|-----------------|----------------|
| Feed read p99 latency | Histogram | > 500ms | Core SLA |
| Post write success rate | Ratio | < 99.9% | Write availability |
| Venice cache hit rate | Ratio | < 92% | Feed reconstruction triggered too often |
| Connection degree cache hit rate | Ratio | < 90% | Degree computation under-cached |
| Viral detection lag (reaction to Venice update) | Gauge | > 5 minutes | Viral amplification SLA |
| Fan-out Kafka lag | Gauge | > 50K messages | Posts not reaching connections' feeds |
| Privacy filter error rate | Counter | > 0 | Privacy correctness |
| Pinot query p99 | Histogram | > 100ms | Feature store latency bottleneck |
| PYMK click-through rate | Ratio | < 1% (sustained) | Recommendation quality signal |
| Post engagement rate (likes+comments per impression) | Ratio | Sudden drop > 30% | Feed quality regression |
| Ranking model feature staleness | Gauge | > 600s | Features out of date |
| CDN cache hit rate | Ratio | < 88% | CDN efficiency |
| Connection degree computation latency p99 | Histogram | > 50ms | Graph traversal performance |

### Distributed Tracing

- OpenTelemetry SDK across all services. Trace propagated via HTTP headers and Kafka message headers.
- Critical traces: feed read path (API Gateway → Feed Service → Venice + Pinot + Connection Service → Ranking → PYMK injection → Post hydration).
- Degree computation traced separately: span "connection_degree_check" per post candidate — reveals if degree computation is the feed latency bottleneck.
- Privacy audit trace: every `privacy_decision` span logged with `{viewer_id_hash, post_id, audience, decision, reason}` — separate immutable log stream.

### Logging

- Structured JSON logs: `{timestamp, trace_id, service, member_id_hash, post_id, action, latency_ms, success, error_code}`.
- Feed generation log: `{member_id_hash, session_id, candidates_fetched, privacy_filtered_count, viral_injected_count, ranking_model_v, total_latency_ms}`.
- Privacy log: append-only, WORM storage, 7-year retention (enterprise compliance).
- Key alerts: `"venice_miss": true` count spike → feed reconstruction load; `"privacy_decision": "denied"` for `audience_type: "public"` → bug.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|----------|---------------|-----------------|--------|
| Feed pre-computation | Venice precomputed candidates | Pure fan-out on read at query time | 300M MAU × 500 avg connections × 2 posts/day = 300B reads/day for pure pull — not feasible |
| Viral amplification | Read-time injection via Pinot detection | Push fan-out to all 2nd-degree connections | 2nd-degree push fan-out: 500 connections × 500 connections × 35 posts/sec = 8.75B writes/sec peak — infeasible |
| Professional relevance signal | Multi-label NLP classifier + professional engagement weight | Raw engagement count only | Raw engagement is easily gamed by non-professional content; professional relevance ensures feed remains career-focused |
| Connection degree computation | Bloom filter + materialized 1st-degree + batch query | Full BFS at request time | Full BFS at request time for 150M DAU × 10 degree checks each = 1.5B graph traversals/day — too expensive |
| Privacy on degree unknown | Fail closed (exclude post) | Fail open (include post) | Privacy correctness is a business and legal requirement on a professional platform |
| Reaction model | 7 reaction types (LinkedIn-specific) | Binary like only | Professional context has distinct emotional register: "Celebrate" for promotions, "Insightful" for thought leadership — richer signal for ranking quality |
| Post longevity | Long engagement window (7 days vs Twitter's 1 hour) | Rapid time decay | Professional posts (career advice, industry analysis) have week-long engagement cycles; short decay misses substantive long-tail engagement |
| PYMK delivery | Pre-computed in Venice, injected at feed read time | Separate API call per page load | Separate API call adds 100–200ms to feed load time; Venice pre-computation brings PYMK to near-zero marginal cost |
| Content category suppression | Post-ranking score multiplier | Training out of ranking model | Training out requires expensive retraining; runtime multiplier allows rapid policy changes without model lifecycle overhead |

---

## 11. Follow-up Interview Questions

**Q1: How would you design LinkedIn's "Open to Work" feature and its feed implications?**
A: "Open to Work" is a profile flag (`is_open_to_work = true`) that adds a frame to the profile photo. Privacy setting: "Recruiters only" (hidden from current employer using a blocklist of company domains) or "Everyone". Feed implication: when a user sets "Open to Work", this change event is published to Kafka, triggering a partial re-ranking of their profile in recruiter search results. For feed: posts from a user who recently set "Open to Work" receive a temporary boost in PYMK recommendations for recruiters in the same industry. This is a deliberate product choice to increase job seeker visibility.

**Q2: How would you design LinkedIn's newsletter/article publishing for the feed?**
A: LinkedIn Articles/Newsletters are long-form content (up to 125,000 chars). Data model: separate `articles` table in Espresso with `post_id`, `headline`, `body_html`, `cover_image_key`, `author_id`, `published_at`. Newsletter subscriptions: `newsletter_subscriptions (subscriber_id, author_id, subscribed_at)`. Feed injection: newsletter articles appear as a special card in the feed for subscribers (higher feed priority than normal posts). Email delivery: separate email digest pipeline (Kafka → Email Service → SendGrid/SES). Article indexing in Galene for discovery. LinkedIn Learning articles are a separate product but share the feed injection mechanism.

**Q3: How would you implement the "Who's Viewed Your Profile" feature?**
A: Profile view events fire when any user views a profile: `profile_view (viewer_id, viewee_id, timestamp, viewer_context)`. Stored in Espresso with TTL partitioning: last 90 days for Premium, last 5 days for free. Privacy: viewer_id anonymized for free tier viewers viewing free tier profiles ("someone from Microsoft"). Aggregated in Pinot: `COUNT(viewer_id)` grouped by `(viewee_id, day)`. Profile view feed card: "15 people viewed your profile this week" injected into feed (position 2 or 3) once per week per user, personalized with viewer company/title aggregates. Drives Premium conversion.

**Q4: How would you design the "Skills Endorsements" feature and integrate it into ranking?**
A: Skills endorsements: `endorsements (endorser_id, endorsee_id, skill_id, endorsed_at)` in Espresso. Endorsement notifications sent via Kafka → Notification Service. Feed ranking integration: (1) A post author with many high-quality endorsements in a relevant skill (e.g., 500+ "Machine Learning" endorsements) gets a higher `author_authority_score` for ML-related posts. (2) This score is computed offline daily: Spark job aggregates endorsements per skill per member, normalizes by endorsement recency and endorser connection quality. (3) `author_authority_score` becomes a ranking feature in the professional relevance model — experts in a topic rank higher for topic-relevant content.

**Q5: How do you handle the "quiet quitting" privacy concern — users browsing jobs while employed?**
A: LinkedIn has explicit "Private mode" for job searching: when enabled, profile views appear as "A LinkedIn member" to everyone (including direct connections). Implementation: `browsing_mode` field in member session context. Profile view event still recorded for aggregate analytics (viewee sees "someone in your network viewed your profile" without identity). When saving job posts or following company pages: these activities are never shared in feed (they don't generate Kafka events that trigger feed fan-out). The `is_open_to_work = "recruiters_only"` setting uses a blocklist of company domain names (e.g., `@google.com` emails) to filter which recruiters can see the frame — preventing the current employer's HR from seeing it.

**Q6: How would you implement LinkedIn's "Company Analytics" for page admins?**
A: Impression events: `post_impression (post_id, viewer_id, viewer_company_id, viewer_seniority, ts)` tracked client-side, batched and sent to Analytics Ingest Service. Raw events → Kafka → Pinot real-time tables. Page admin dashboard queries Pinot for: unique impressions, reactions, comments, shares, follower demographics (industry, seniority, location). All broken down by time range and demographic. Privacy: aggregate only — no individual viewer identity shown to page admins (comply with GDPR). Minimum threshold: demographics only shown if count > 30 to prevent re-identification.

**Q7: How does LinkedIn handle multi-language content in the professional feed?**
A: Language detection: NLP language classifier runs on post text at creation time. Language tag stored on post. Feed Service includes viewer's language preferences (from profile + browser `Accept-Language` header). Ranking adjustment: posts in the viewer's primary language get +20% boost; posts in known secondary languages get standard score; posts in unknown languages get −50% penalty. LinkedIn has also implemented machine translation (MT) for posts — a "See translation" link appears for non-native-language posts. MT is done at display time (not at storage time) using Azure Cognitive Services, cached per (post_id, target_language) in Redis with 7-day TTL.

**Q8: What happens to the LinkedIn feed when there's a major industry announcement (e.g., a major tech layoff)?**
A: Traffic spike: a major industry event (e.g., a large tech company layoff) causes: (1) Massive spike in post writes (everyone sharing opinions). (2) Massive spike in reactions and comments on those posts. (3) Viral detection triggers on many related posts simultaneously. (4) Feed Fanout Workers experience queue buildup. Mitigations: (1) Kafka absorbs the write spike (configured with sufficient partition count for the post write topic). (2) Fanout Workers auto-scale via Kubernetes HPA on Kafka lag metric. (3) Viral Amplification Service rate limits viral injections: max 5 viral posts per feed load, even during spikes. (4) Feed reads unaffected by write spike (Venice already has precomputed feeds; the spike affects only feed freshness, not feed availability). (5) Trending hashtags related to the event surface at top of feed (standard hashtag feed feature).

**Q9: How would you design LinkedIn's "Events" integration into the feed?**
A: Events in feed: when a user RSVPs to an event, connections see it as a feed item ("John is attending TechConf 2026"). Event post creation: `events (event_id, host_id, title, start_time, location, attendee_count)` in Espresso. Event feed item generated on RSVP: triggers `event.rsvp` Kafka event → Fan-out Worker pushes to connection feeds with event_card type. Feed ranking: event proximity boost — events in viewer's city or industry rank higher. Events within 7 days get time-sensitive boost in ranking. Post-event: attendees' posts with the event hashtag are aggregated into an "Event Highlights" card shown to attendees' connections for 3 days after the event.

**Q10: How would you implement professional content moderation on LinkedIn?**
A: Three-layer pipeline: (1) Automated: ML classifiers for spam, hate speech, CSAM, dangerous misinformation — runs at post creation time. High-confidence blocks are auto-rejected (< 1% false positive rate target). (2) Reactive: user reports trigger a review queue. Posts with > 5 reports per 1,000 views are fast-tracked for human review. (3) Proactive: periodic sweeps by trust & safety team on trending content. Special professional considerations: LinkedIn's moderation must account for content that is acceptable in a professional context (e.g., discussions of workplace discrimination) that might be flagged by consumer-oriented models. A separate professional context model is trained on LinkedIn-specific labeled data. Moderation decisions are logged in an immutable audit trail.

**Q11: How does LinkedIn rank endorsements and recommendations for feed relevance?**
A: Written recommendations ("I worked with Jane for 3 years...") are feed events: when user A writes a recommendation for user B, it appears in A's connections' feeds as a high-trust professional endorsement signal. Ranking treatment: recommendations from senior professionals (VP+) or from cross-company endorsers (not same current employer) get higher professional relevance score — they're harder to game than endorsements from direct colleagues. The recommendation text is analyzed by NLP to extract skills mentioned, which updates the endorsee's `author_authority_score` for those skills. Recommendations are also indexed in Galene for search ("find software engineers recommended for Python").

**Q12: How would you design "LinkedIn Learning" post injection into the feed?**
A: LinkedIn Learning courses are separate content objects (`learning_content` table in Espresso). Feed injection: (1) When a member completes a course, a "Jane earned a certificate in Python" card appears in Jane's connections' feeds (social proof + professional signal). (2) Course recommendations are injected into the feed (position 6 or later) based on skills gaps detected from profile data and job postings the user has viewed. These are treated as "sponsored" positions within the feed ranking pipeline but use profile-derived targeting rather than advertiser bidding. (3) Completion events → Kafka → Fan-out Worker → Venice with `item_type = "learning_card"`. Impression and click tracking via standard analytics pipeline.

**Q13: How does LinkedIn avoid becoming a Twitter-like platform for political discourse?**
A: Active feed suppression: (1) Content classifier identifies political vs. professional content. Posts classified as political with > 80% confidence have their virality_score multiplied by 0.1, severely limiting viral amplification. (2) Political posts from non-Connections (e.g., from people the user merely follows) are suppressed more aggressively than from 1st-degree connections. (3) Comment ranking on viral posts: comments that increase political discourse (detected via sentiment/topic classification) are ranked lower, showing substantive professional comments first. (4) Trending hashtag filtering: political hashtags excluded from LinkedIn trending topics (separate political content classifier applied to trending detection). These are active product decisions to maintain LinkedIn's professional identity.

**Q14: Walk through the data flow when a connection deletes a post.**
A: `DELETE /v1/posts/{post_id}`: (1) Post Service sets `is_deleted = true` in Espresso (soft delete). (2) Publishes `post.deleted {post_id, author_id}` to Kafka. (3) Fan-out Worker consumes event, removes post_id from all Venice feed entries. This is O(connections) Venice writes — same scale as the original fan-out. (4) CDN cache purge API called for media keys associated with the post. (5) Galene search index receives a delete document event — post de-indexed within 60 seconds. (6) Pinot virality table: post_id's virality metrics naturally expire via TTL. (7) Reactions and comments: soft-deleted alongside post (cascade: `UPDATE comments SET is_deleted = true WHERE post_id = ?`). (8) Post remains in Espresso with `is_deleted = true` for 30 days (to handle GDPR data export requests and abuse investigation), then hard-deleted by a scheduled purge job.

**Q15: How would you evaluate whether a ranking algorithm change improved the LinkedIn feed?**
A: Multi-metric evaluation framework: (1) Engagement metrics: CTR on posts, comments per impression, repost rate, time spent per session. (2) Professional quality metrics: profile view rate post-feed-session (did users click through to profiles? — professional intent signal), job application rate from feed (ultimate LinkedIn business metric), connection request rate after feed sessions. (3) Negative signals: "hide post" rate, "unfollow" rate, report rate. (4) Diversity metrics: unique authors per session, industry spread of content, hashtag topic spread. (5) A/B test infrastructure: experiment buckets assigned at API Gateway, statistical significance via internal experimentation platform (LinkedIn uses a system called "XLNT" internally). Minimum detectable effect size: 0.5% improvement in 7-day retention. Test duration: 2 weeks minimum to capture weekly cycles (LinkedIn usage is strongly weekday-heavy; short tests overfit to specific days of week).

---

## 12. References & Further Reading

- Das, S. et al. "LinkedIn's Real-time Monitoring and Alerting System." LinkedIn Engineering Blog, 2015. — Describes the monitoring stack.
- Garg, A. et al. "Project Voldemort: Reliable Distributed Storage." LinkedIn Engineering Blog. — Venice's backing store.
- LinkedIn Engineering Blog: "The Evolution of Feed Quality at LinkedIn." — Describes professional signal integration in feed ranking.
- LinkedIn Engineering Blog: "How LinkedIn Scaled its Feed with Venice." — Venice derived data store for feed pre-computation.
- Kreps, J., Narkhede, N., Rao, J. "Kafka: A Distributed Messaging System for Log Processing." NetDB 2011. — LinkedIn created Kafka; this is the original paper.
- Shraer, A. et al. "Espresso: Operating LinkedIn's Distributed Database." LinkedIn Engineering Blog, 2012. — Describes Espresso MySQL-based distributed database.
- Apache Pinot documentation: pinot.apache.org — LinkedIn's OLAP data store for real-time feature serving.
- Noulas, A. et al. "An Empirical Study of Geographic User Activity Patterns in Foursquare." ICWSM 2011. — Relevant for location-based professional feed signals.
- "Designing Data-Intensive Applications" by Martin Kleppmann (O'Reilly, 2017).
- LinkedIn Engineering Blog: "Managing the LinkedIn Hashtag Feed at Scale." — Hashtag follow and feed injection.
- Bernstein, A. et al. "EdgeNet: LinkedIn's Approach to Real-Time Feed Ranking." — Internal research; referenced in LinkedIn Engineering Blog posts.
- Fan, J. et al. "Connected: The Surprising Power of Our Social Networks and How They Shape Our Lives" — Christakis & Fowler, 2009. Relevant academic background on network degree and information propagation.
- LinkedIn Engineering Blog: "Reducing Feed Latency at LinkedIn." — Performance optimization post directly relevant to latency targets.
- LinkedIn's GitHub (github.com/linkedin) — Open-source projects including Kafka, Venice, Pinot, Azkaban.
