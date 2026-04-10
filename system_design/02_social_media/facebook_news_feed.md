# System Design: Facebook News Feed

---

## 1. Requirement Clarifications

### Functional Requirements
- Users see a personalized News Feed of posts from friends, followed pages, and groups they belong to
- Feed content types: text posts, photo posts, video posts, shared links, events, life events, check-ins
- Users can like (and react with emoji reactions), comment, and share posts
- Feed is ranked by a relevance/engagement algorithm (not purely chronological)
- Users can report posts or hide them from their feed
- Privacy filtering: posts respect per-post audience settings (Public, Friends, Friends of Friends, Specific Friends, Only Me, Custom)
- Real-time updates: new posts from friends are surfaced without requiring a full page reload
- Feed includes content from Pages (brands, public figures) the user follows
- Group posts from groups the user is a member of appear in feed
- Sponsored content (ads) interleaved in feed at specific positions

### Non-Functional Requirements
- Availability: 99.99% uptime — Facebook is a global utility; downtime has enormous user and revenue impact
- Feed read latency: p99 < 500ms — ranked, privacy-filtered feeds require more computation than simple chronological feeds
- Post write latency: p99 < 300ms for write acknowledgment (fan-out is async)
- Privacy filtering must be 100% correct — a post shared with "Friends only" must NEVER appear to non-friends; correctness over performance
- Eventual consistency acceptable for feed freshness — posts appear in feeds within seconds to minutes
- Scale: 3 billion MAU, ~2 billion DAU, ~100+ billion feed impressions per day
- The system must handle the social graph of 3 billion nodes and trillions of edges

### Out of Scope
- Facebook Marketplace
- Facebook Groups full feature set (only group posts in feed)
- Facebook Events full feature set
- Messenger (direct messaging)
- Facebook Watch (video streaming)
- Advertising auction system (ad serving positions assumed, not designed)

---

## 2. Users & Scale

### User Types
- **Regular users**: Post, read feed, react, comment — the majority
- **Page admins**: Post on behalf of brands, public figures — content generators
- **Group admins/members**: Post in groups, group content surfaced in feed
- **Read-only lurkers**: Significant population that only consumes content, rarely posts

### Traffic Estimates

| Metric | Value | Reasoning |
|--------|-------|-----------|
| MAU | 3 billion | Publicly reported figure |
| DAU | 2 billion | ~67% DAU/MAU ratio (Facebook's high engagement) |
| Feed loads/day | 100 billion | 2B DAU × 50 feed loads/day avg (scrolling sessions) |
| Feed read QPS (avg) | ~1,157,000 | 100B / 86,400 |
| Feed read QPS (peak) | ~5,000,000 | ~4.3× peak multiplier (global evening peak across timezones) |
| Posts/day | 500 million | ~0.25 posts/day per DAU |
| Post write QPS (avg) | ~5,800 | 500M / 86,400 |
| Post write QPS (peak) | ~25,000 | 4.3× peak |
| Reactions/day | 10 billion | 20 per post × 500M posts, sustained over post lifetime |
| Comments/day | 2 billion | Fewer but heavier writes |

### Latency Requirements
- **Feed read p99: 500ms** — ranked feeds require: (1) fetching candidate posts from multiple sources (friends, pages, groups), (2) privacy filtering per-post, (3) ML ranking inference. 500ms is the outer boundary; target is 200ms for the most optimized path.
- **Feed write (post acknowledgment) p99: 300ms** — user must see their own post immediately; fan-out to friends is async.
- **Privacy filter correctness: 100%** — no latency SLA override; privacy is correctness-critical. If the system can't verify privacy within timeout, the post is excluded from the feed (fail closed).
- **Reaction/comment write p99: 200ms** — interactive social signals; users notice lag.

### Storage Estimates

| Data Type | Size/record | Records/day | Retention | Total |
|-----------|-------------|-------------|-----------|-------|
| Text posts | 1 KB | 200M/day | Forever | 200 GB/day |
| Photo posts (processed) | 5 MB avg (3 sizes) | 200M/day | Forever | 1 TB/day |
| Video posts (compressed) | 200 MB avg | 50M/day | Forever | 10 TB/day |
| Post metadata | 500 bytes | 500M/day | Forever | 250 GB/day |
| Social graph edges (friendship, page follows, group membership) | 16 bytes | 1B new edges/day | Forever | 16 GB/day |
| Reactions | 30 bytes | 10B/day | Forever (for ranking signals) | 300 GB/day |
| Comments | 500 bytes | 2B/day | Forever | 1 TB/day |
| Feed ranking features | 2 KB/user | 2B DAU | Rolling 30 days | 4 TB (rotating) |
| EdgeRank/ranking model weights | ~100 MB per model version | 1 version/day | 90 days | ~9 GB |
| Search index | 2 KB/post | 500M/day | 30 days rolling | 30 TB |

**Total storage**: Media dominates at ~11 TB/day. Non-media ~2 TB/day. Annual: ~4.7 PB (media) + ~730 TB (metadata).

### Bandwidth Estimates

| Direction | Calculation | Result |
|-----------|-------------|--------|
| Inbound post writes (text+meta) | 5,800 QPS × 1.5 KB | ~8.7 MB/s |
| Inbound photo uploads | 200M × 5MB / 86400 | ~11.6 GB/s |
| Inbound video uploads | 50M × 200MB / 86400 | ~115 GB/s |
| Outbound feed reads | 1.15M QPS × 10 posts × 2 KB (text+meta) | ~23 GB/s text |
| Outbound media (CDN) | 1.15M QPS × 3 posts with media × 500KB | ~1.7 TB/s CDN edge |
| Total CDN outbound | With 95% cache hit, origin: | ~85 GB/s origin fill |

---

## 3. High-Level Architecture

```
  ┌───────────────────────────────────────────────────────────────────┐
  │                           CLIENTS                                  │
  │            Web Browser / iOS / Android / API                       │
  └───────────────────────────────┬───────────────────────────────────┘
                                  │ HTTPS
                                  ▼
  ┌───────────────────────────────────────────────────────────────────┐
  │              Global CDN (Facebook's own CDN / Akamai)             │
  │   Static assets, media (photos, videos), shared post previews     │
  └───────────────────────────────┬───────────────────────────────────┘
                                  │
                                  ▼
  ┌───────────────────────────────────────────────────────────────────┐
  │              API Gateway / Edge Proxy (Facebook's PoPs)           │
  │   TLS termination, auth token validation, rate limiting           │
  │   A/B experiment assignment, routing by service type              │
  └────────┬──────────────┬───────────────┬───────────────────────────┘
           │              │               │
  ┌────────▼─────┐  ┌─────▼───────┐  ┌───▼─────────────────────────┐
  │ Post Service │  │ Feed Service│  │ Social Graph Service (TAO)  │
  │ - Create     │  │ - Read feed │  │ - Friends, Page follows      │
  │   post       │  │ - Rank posts│  │ - Group memberships          │
  │ - Media ref  │  │ - Privacy   │  │ - Read/write graph edges     │
  │ - Fan-out    │  │   filter    │  └────────────────┬────────────┘
  └──────┬───────┘  └─────┬───────┘                   │
         │                │                            │
         └────────────────┴────────────────────────────┘
                          │ All services publish to / consume from
                          ▼
  ┌───────────────────────────────────────────────────────────────────┐
  │                     Kafka (Message Bus)                            │
  │  Topics: post.created, post.deleted, reaction.event, feed.fanout  │
  │          social.edge.created, notification.event                   │
  └──────────┬──────────────────────────────────────────────────────┘
             │
   ┌─────────┼────────────────────────────────────────┐
   │         │                                        │
   ▼         ▼                                        ▼
┌──────────────────┐  ┌──────────────────────┐  ┌─────────────────────┐
│ MultiFeed        │  │ Ranking & Scoring     │  │ Notification        │
│ Fan-out Workers  │  │ Service               │  │ Service             │
│ - Aggregate      │  │ - EdgeRank/ML         │  │ - Push, email       │
│   posts per      │  │   scoring             │  │   in-app notifs     │
│   user           │  │ - Feature retrieval   │  └─────────────────────┘
│ - Write to       │  │   from Feature Store  │
│   feed stores    │  └──────────────────────┘
└──────────────────┘

  ┌───────────────────────────────────────────────────────────────────┐
  │                          DATA STORES                               │
  │                                                                     │
  │  ┌────────────────┐  ┌────────────────────┐  ┌──────────────────┐ │
  │  │  TAO (Social   │  │  MultiFeed Store   │  │  Post Store      │ │
  │  │  Graph + Post  │  │  (Precomputed feed │  │  (MySQL +        │ │
  │  │  Cache)        │  │   per user)        │  │  Haystack media) │ │
  │  │  MySQL-backed  │  │  Cassandra         │  │                  │ │
  │  │  Memcache layer│  └────────────────────┘  └──────────────────┘ │
  │  └────────────────┘                                                │
  │  ┌────────────────────────────────────────────────────────────┐   │
  │  │  Feature Store (Scuba / internal time-series)              │   │
  │  │  Per-user engagement features, per-post signals            │   │
  │  └────────────────────────────────────────────────────────────┘   │
  └───────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **TAO (The Associations and Objects)**: Facebook's distributed data store for the social graph and associated objects. Stores objects (users, posts, pages, groups) and associations (friendships, reactions, memberships). Backed by MySQL with a Memcache caching layer. Critical for privacy check ("is user A friends with user B?") and graph traversal.
- **Post Service**: Handles post creation. Writes to Post Store (MySQL for metadata, Haystack for media), publishes `post.created` to Kafka.
- **Feed Service**: Primary read path. Fetches user's precomputed feed from MultiFeed Store, runs privacy filtering, ranking inference, and ad insertion. Returns final ranked feed.
- **MultiFeed Fan-out Workers**: Consumes `post.created` events from Kafka. For each post, looks up the poster's friends/followers via TAO, aggregates the post reference into each friend's feed store entry in Cassandra.
- **Ranking & Scoring Service**: Runs the EdgeRank/ML model to score candidate posts for a specific user. Fetches features from Feature Store. Used both in precomputation and at read time for re-ranking.
- **Social Graph Service (TAO)**: All friendship, page follow, group membership reads and writes. Central to both fan-out (who to push to?) and privacy filtering (is requester allowed to see this post?).
- **Feature Store**: Near-real-time feature data: user's recent engagement history, post's current engagement velocity, affinity scores between users. Updated by Kafka consumers watching reaction/comment events.

**End-to-End Flow — Reading the News Feed:**
1. Client sends `GET /v1/feed` with auth token.
2. API Gateway validates token, routes to Feed Service.
3. Feed Service fetches precomputed feed (list of post_ids + basic metadata) from Cassandra MultiFeed Store for this user.
4. Feed Service calls TAO to verify privacy: for each post in candidate set, check if requesting user satisfies post's audience setting.
5. Posts failing privacy check are removed from candidate set (fail closed).
6. Remaining candidates passed to Ranking & Scoring Service: retrieves per-post and per-user features from Feature Store, runs ML model, returns ranked order.
7. Feed Service fetches full post content for top-N ranked posts from TAO/Post Store.
8. Ad positions (every ~5 posts) filled by Ad Service (out of scope but interleaved in response).
9. Response returned to client with ranked, privacy-filtered, hydrated posts.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- TAO Objects (MySQL — sharded by object_id)
-- TAO models all entities as Objects and Associations
-- ============================================================

-- Objects table (generic — TAO pattern)
CREATE TABLE objects (
    id         BIGINT NOT NULL,       -- Snowflake/Facebook's own ID scheme
    otype      INT NOT NULL,          -- Object type: 1=User, 2=Post, 3=Page, 4=Group
    data       BLOB NOT NULL,         -- Serialized object data (Thrift-encoded)
    version    BIGINT,                -- Optimistic locking version
    updated_at DATETIME,
    PRIMARY KEY (id),
    INDEX idx_type (otype, id)
);

-- Associations table (edges in the social graph)
CREATE TABLE associations (
    id1        BIGINT NOT NULL,       -- Source object ID
    atype      INT NOT NULL,          -- Association type: 1=Friend, 2=Liked, 3=Followed,
                                      -- 4=MemberOf, 5=PostedBy, 6=CommentedOn
    id2        BIGINT NOT NULL,       -- Destination object ID
    time       BIGINT NOT NULL,       -- Creation time (unix ms)
    data       BLOB,                  -- Optional edge metadata
    PRIMARY KEY (id1, atype, id2),
    INDEX idx_reverse (id2, atype, time)  -- For reverse lookups
);
-- Shard by (id1 % num_shards) for locality of outgoing edges

-- ============================================================
-- Posts (MySQL + dedicated post metadata, sharded by post_id)
-- ============================================================
CREATE TABLE posts (
    post_id       BIGINT PRIMARY KEY,
    author_id     BIGINT NOT NULL,
    post_type     TINYINT,            -- 1=text, 2=photo, 3=video, 4=link, 5=share
    text_content  TEXT,               -- Plaintext body, max 63,206 chars
    media_keys    JSON,               -- Array of Haystack blob keys for media
    shared_post_id BIGINT,            -- If this is a share/reshare
    audience_type TINYINT,            -- 1=Public, 2=Friends, 3=FriendsOfFriends,
                                      -- 4=Custom, 5=OnlyMe
    audience_data JSON,               -- For Custom: list of included/excluded user_ids
    location_id   BIGINT,
    reaction_counts JSON,             -- {"like":N, "love":N, "haha":N, "wow":N, "sad":N, "angry":N}
    comment_count INT DEFAULT 0,
    share_count   INT DEFAULT 0,
    is_deleted    BOOLEAN DEFAULT FALSE,
    created_at    DATETIME DEFAULT NOW(),
    updated_at    DATETIME DEFAULT NOW() ON UPDATE NOW(),
    INDEX idx_author (author_id, created_at DESC)
);

-- ============================================================
-- Privacy Rules (for complex custom audience evaluation)
-- ============================================================
CREATE TABLE post_audience_rules (
    post_id          BIGINT,
    rule_type        ENUM('include_list', 'exclude_list', 'include_list_ids'),
    target_ids       JSON,            -- Array of user_ids or list_ids
    PRIMARY KEY (post_id, rule_type)
);

-- ============================================================
-- MultiFeed Store (Cassandra — precomputed feed per user)
-- ============================================================
CREATE TABLE user_feed (
    user_id        BIGINT,
    post_id        BIGINT,            -- Snowflake — encodes timestamp for ordering
    author_id      BIGINT,
    source_type    TINYINT,           -- 1=friend, 2=page, 3=group, 4=suggested
    pre_rank_score FLOAT,             -- Precomputed base score from fan-out time
    is_seen        BOOLEAN DEFAULT FALSE,
    added_at       TIMESTAMP,
    PRIMARY KEY (user_id, post_id)
) WITH CLUSTERING ORDER BY (post_id DESC)
  AND compaction = {'class': 'LeveledCompactionStrategy'};
-- Capped at 5000 entries per user via application logic (remove oldest on overflow)

-- ============================================================
-- Reactions (Cassandra — high write rate)
-- ============================================================
CREATE TABLE reactions (
    post_id        BIGINT,
    user_id        BIGINT,
    reaction_type  TINYINT,           -- 1=like, 2=love, 3=haha, 4=wow, 5=sad, 6=angry
    created_at     TIMESTAMP,
    PRIMARY KEY (post_id, user_id)
);

-- ============================================================
-- Comments (Cassandra — append-heavy)
-- ============================================================
CREATE TABLE comments (
    post_id        BIGINT,
    comment_id     BIGINT,            -- Snowflake
    author_id      BIGINT,
    body           TEXT,
    parent_comment_id BIGINT,         -- For nested comment threads
    reaction_counts JSON,
    is_deleted     BOOLEAN DEFAULT FALSE,
    created_at     TIMESTAMP,
    PRIMARY KEY (post_id, comment_id)
) WITH CLUSTERING ORDER BY (comment_id ASC);

-- ============================================================
-- Feature Store (logical — stored in Scuba/internal TSDB)
-- Per-user features (updated by batch jobs + stream)
-- ============================================================
-- user_affinity_score (user_id, author_id) -> float [0,1]
-- post_engagement_velocity (post_id) -> {likes_per_hour, comments_per_hour}
-- user_content_type_preference (user_id) -> {video: 0.6, photo: 0.3, text: 0.1}
-- user_session_recency (user_id) -> timestamp of last activity
```

### Database Choice

| Database | Use Case | Pros | Cons |
|----------|----------|------|------|
| MySQL (via TAO) | Social graph objects and associations, post metadata | ACID for friend relationships; row-level locking for correct reaction counting; proven at Facebook at petabyte scale | Sharding complexity; cross-shard joins require application-level merge |
| TAO (Memcache over MySQL) | Read-heavy graph traversal | Absorbs >99% of reads at cache layer; sub-millisecond read latency; hierarchical caching (leader/follower cache clusters) | Eventual consistency between cache and MySQL; complex invalidation |
| Cassandra | MultiFeed precomputed feeds, reactions, comments | Handles high write throughput for fanout; wide-column for user_id + post_id time-series; horizontal scaling | No secondary indexes; eventual consistency |
| Haystack | Photo/video blob storage | Purpose-built for billions of small files; avoids filesystem metadata overhead that kills performance at Facebook's scale | Proprietary; not an off-the-shelf solution (equivalent: S3 + custom metadata) |
| Scuba / Internal TSDB | Feature store for ranking | Sub-second feature retrieval; time-series optimized | Not queryable with SQL; proprietary |

**Decision:** TAO (MySQL-backed, Memcache-fronted) chosen for social graph because: (1) Facebook's published architecture shows TAO handles >1 billion reads/second with a Memcache cache hit rate > 99%; (2) association queries (get all friends of user X, get all reactions on post Y) are the most common access pattern — TAO's association model matches exactly; (3) strong consistency on friendship creation (critical for privacy checks) via MySQL primary writes. Cassandra chosen for MultiFeed because: fan-out generates extremely high write QPS (up to 5,000 fan-out writes per normal user post = 29M writes/sec at peak) — Cassandra's LSM-tree excels at this write pattern.

---

## 5. API Design

```
GET /v1/feed
  Description: Fetch ranked, privacy-filtered News Feed
  Auth: Bearer token (required)
  Query params:
    cursor: string (opaque pagination token)
    count: int (default 10, max 50)
    surface: "desktop"|"mobile"|"watch"    -- Affects content format
    include_ads: bool (default true)
  Response 200: {
    feed_items: [{
      type: "post"|"ad"|"suggested_friend"|"memory",
      post: {
        post_id: string,
        author: { user_id, name, profile_picture_url, is_page: bool },
        content: {
          text: string | null,
          media: [{ url, thumbnail_url, type, width, height, duration_s }] | null,
          link_preview: { url, title, description, image_url } | null
        },
        audience: string,    -- Display string: "Friends" / "Public"
        reactions: { like: int, love: int, haha: int, wow: int, sad: int, angry: int },
        my_reaction: string | null,
        comment_count: int,
        share_count: int,
        created_at: ISO8601,
        rank_score: float    -- Optional, for debugging/testing only
      } | null,
      ad: AdObject | null
    }],
    next_cursor: string | null,
    session_id: string      -- For feed session tracking (analytics)
  }
  Cache-Control: no-store (fully personalized)

POST /v1/posts
  Description: Create a new post
  Auth: Bearer token (required)
  Rate limit: 25 posts/day (text), 10 photos/day, 5 videos/day
  Request: {
    text: string (max 63206 chars),
    media_ids: string[] (optional, from /v1/media/upload),
    audience: {
      type: "public"|"friends"|"friends_of_friends"|"custom"|"only_me",
      include_ids: string[],  -- For custom: specific user_ids or list_ids to include
      exclude_ids: string[]   -- For custom: specific user_ids to exclude
    },
    location_id: string | null,
    shared_post_id: string | null
  }
  Response 201: { post_id: string, created_at: ISO8601 }

DELETE /v1/posts/{post_id}
  Description: Soft-delete a post
  Auth: Bearer token (must be author)
  Response 204

POST /v1/posts/{post_id}/reactions
  Description: Add or change a reaction to a post
  Auth: Bearer token (required)
  Request: { reaction_type: "like"|"love"|"haha"|"wow"|"sad"|"angry" }
  Response 200: {
    my_reaction: string,
    reaction_counts: { like: int, love: int, haha: int, wow: int, sad: int, angry: int }
  }

DELETE /v1/posts/{post_id}/reactions
  Description: Remove a reaction (un-react)
  Auth: Bearer token (required)
  Response 200: { my_reaction: null, reaction_counts: {...} }

GET /v1/posts/{post_id}/comments
  Description: Get paginated comments for a post
  Auth: Bearer token (required — privacy check)
  Query params: cursor, count, sort: "top"|"newest"
  Response 200: {
    comments: [{ comment_id, author: {...}, body, reaction_counts, 
                 replies: Comment[], created_at }],
    next_cursor: string | null
  }

POST /v1/posts/{post_id}/comments
  Description: Add a comment
  Auth: Bearer token (required)
  Request: { body: string (max 8000 chars), parent_comment_id: string | null }
  Response 201: { comment_id, body, created_at }

POST /v1/posts/{post_id}/share
  Description: Share a post to own timeline or group
  Auth: Bearer token (required)
  Request: {
    text: string | null,
    audience: AudienceObject,
    destination: "timeline"|"group",
    group_id: string | null
  }
  Response 201: { post_id: string }    -- New post wrapping the share

GET /v1/feed/options/{post_id}
  Description: Get options menu for a feed item (hide, snooze, report)
  Auth: Bearer token (required)
  Response 200: { options: [{ action: string, label: string }] }

POST /v1/feed/hide/{post_id}
  Description: Hide a post from feed (won't show again)
  Auth: Bearer token (required)
  Response 200: { hidden: true }
  Side effect: Updates user preference model in Feature Store
```

---

## 6. Deep Dive: Core Components

### Component: EdgeRank / Feed Ranking Algorithm

**Problem it solves:**
A user with 500 friends who each post twice daily generates 1,000 candidate posts per day. The user reads ~50 posts per day. The ranking algorithm must select the 50 most relevant posts, ordering them by predicted engagement and relevance rather than purely chronological time. The algorithm must balance: recency, affinity (closeness between viewer and poster), engagement signals, content type preferences, diversity (not showing 10 posts from same person), and anti-spam.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Pure chronological | Sort by post creation time descending | Transparent, predictable, no ML needed | Misses posts from close friends if they post at off-hours; drowns in low-quality content |
| EdgeRank (Facebook original) | Affinity × Weight × Time Decay | Interpretable, computationally light | Limited signals; can be gamed by engagement bait |
| Gradient Boosted Trees (GBDT) | ML model over hand-crafted features | High accuracy; fast inference; feature engineering flexibility | Requires labeled data; doesn't generalize to new content types without retraining |
| Neural Ranking (Deep Learning) | Two-tower or DCN model over raw features and embeddings | Captures non-linear interactions; learns representations | Expensive inference; harder to debug/interpret; slower to iterate |
| Cascade ranking (two-stage) | Cheap scorer for top-2000 candidates, expensive ranker for top-50 | Balances cost and quality | Two models to maintain; candidate retrieval quality bounds final quality |

**Selected Approach: Cascade Ranking — lightweight pre-scorer + deep neural ranker**

Facebook's real system (as described in published research) uses a cascade: a cheap logistic regression or GBDT model pre-scores ~2000 candidates from the MultiFeed store, selecting the top 500 for the deep neural ranker. The neural ranker (a Deep Cross Network or similar) scores the top 500 with full feature richness, returning the final top-N ordered list.

**Original EdgeRank Formula (foundation):**
```
Score(edge) = Σ (Affinity(u, e) × Weight(e) × Decay(t))

Where:
  Affinity(u, e) = How closely connected user u is to the edge creator
                   Computed from: mutual friends, interaction frequency,
                   click-through rate on past posts from this creator
  Weight(e)      = Content type weight: video > photo > link > text
                   (Higher weight = more engaging format historically)
  Decay(t)       = e^(-λt) where t = time since post creation
                   λ = decay constant tuned per content type
                   (fresher posts score higher, but quality posts age gracefully)
```

**Full Feature Set for Neural Ranker:**

```python
features = {
    # User-post affinity
    'affinity_score': float,          # Historical CTR on posts by this author
    'mutual_friend_count': int,       # Common friends between viewer and author
    'last_interaction_hours': float,  # Hours since viewer last interacted with author
    'follow_recency_days': float,     # How recently viewer followed/friended author

    # Post quality signals
    'like_count': int,
    'comment_count': int,
    'share_count': int,
    'like_velocity': float,           # Likes per hour since posting
    'comment_velocity': float,
    'hide_rate': float,               # Fraction of viewers who hid this post type from this author
    'negative_feedback_rate': float,  # Report/spam signals

    # Content characteristics
    'post_type': categorical,         # video, photo, text, link
    'is_video': bool,
    'video_completion_rate': float,   # For videos: avg % watched
    'link_domain_quality': float,     # Quality score of linked domain (anti-clickbait)

    # Viewer context
    'viewer_session_length_s': float, # Long session = user has more time
    'viewer_device': categorical,     # Mobile: prefer visual; Desktop: prefer text
    'viewer_time_of_day': float,      # Morning/evening reading habits differ
    'viewer_content_type_affinity': dict,  # Historical engagement by content type

    # Freshness
    'post_age_hours': float,
    'is_trending_in_social_circle': bool,  # Many friends have engaged already
}
```

**Implementation Detail:**

```python
class FeedRanker:
    def __init__(self):
        self.pre_scorer = load_model('gbdt_pre_scorer_v12.pkl')     # Fast
        self.deep_ranker = load_model('dcn_full_ranker_v7.onnx')    # Accurate

    def rank(self, user_id, candidate_post_ids):
        # Stage 1: Pre-score all candidates (cheap)
        # Fetch lightweight features: post_id, author_id, age_hours, post_type
        light_features = feature_store.batch_get_light(candidate_post_ids, user_id)
        pre_scores = self.pre_scorer.predict(light_features)

        # Select top-500 by pre-score
        top_500_ids = [cid for cid, score in
                       sorted(zip(candidate_post_ids, pre_scores),
                              key=lambda x: -x[1])][:500]

        # Stage 2: Full feature fetch for top-500
        full_features = feature_store.batch_get_full(top_500_ids, user_id)

        # Privacy filter: remove posts user doesn't have permission to see
        # Done BEFORE deep ranking to avoid scoring private posts
        allowed_ids = privacy_service.filter(user_id, top_500_ids)
        full_features = {k: v for k, v in full_features.items() if k in allowed_ids}

        # Deep ranker inference (ONNX, batched)
        final_scores = self.deep_ranker.predict(full_features)

        # Apply diversity constraints: max 3 posts from same author in top-10
        ranked = sorted(zip(allowed_ids, final_scores), key=lambda x: -x[1])
        ranked = apply_diversity(ranked, max_per_author=3, window=10)

        return ranked
```

**Interviewer Q&As:**

Q: How does Facebook handle "engagement bait" — posts that say "Comment YES if you love pizza" to game the EdgeRank algorithm?
A: Facebook's published research on engagement bait detection uses a text classifier trained on ~1M human-labeled "engagement bait" posts. Categories: vote baiting, react baiting, share baiting, tag baiting, comment baiting. Posts classified as engagement bait have their EdgeRank `weight` factor set to near-zero, causing them to rank poorly. The classifier runs at post creation time (async, sub-second) and can be retroactively applied to existing posts. False positive rate is critical — over-filtering reduces creator reach and causes support complaints.

Q: Walk me through how EdgeRank handles a new user with no interaction history.
A: Cold start: (1) Affinity score = 0 for all friends initially; use friend's follower count and engagement rate as proxy. (2) Weight defaults to content-type population average. (3) Decay is the only discriminating factor — recent posts rank highest. (4) As the user interacts (likes, comments, scroll depth), affinity scores are updated in real-time in the Feature Store. After ~50 interactions, personalization becomes meaningful. (5) For very new users, "trending in your network" posts (many mutual friends engaged) are up-ranked since social proof is a strong cold-start signal.

Q: How does the ranking model stay current with real-time engagement spikes?
A: Two mechanisms: (1) Feature Store is updated in near-real-time by a Kafka consumer processing reaction/comment events — `like_velocity` and `comment_velocity` features reflect engagement up to 1 second ago. These features are read by the ranker at query time. (2) Ranking models are retrained daily (GBDT) or weekly (neural ranker) using fresh training data. Model hot-swap is zero-downtime (blue-green deployment of model serving instances). Features can be refreshed without model retraining since they're runtime inputs, not baked into model weights.

Q: Should the ranking model be retrained after every viral post?
A: No — retroactive retraining after each viral post would introduce label noise (the model learns that "this post is good" rather than "posts with these features are good"). Instead: viral posts naturally receive high feature values (`like_velocity`, `comment_count`) which the current model already knows to weight positively. Model retraining occurs on schedule with accumulated data, not event-triggered. Gradient updates could theoretically be applied more frequently via online learning, but offline batch retraining is more stable and easier to validate.

Q: What is the "Facebook diversity problem" in feed ranking, and how do you solve it?
A: Without diversity constraints, ranking by pure engagement score would show: (a) all posts from the most engaging accounts (violating the social contract of showing content from all friends), (b) multiple posts from the same person back-to-back. Solutions: (1) Author capping: max 3 posts from same author per 10 feed items (enforced post-ranking). (2) Content type rotation: if last 3 items are videos, suppress video candidates. (3) Time diversity: enforce minimum spread of post creation times across feed items. (4) Story deduplication: if post A was shared by 5 friends, show it once as "5 friends shared this." These are post-ranking filters, not ranking signals, to avoid distorting the model.

---

### Component: Privacy Filtering System

**Problem it solves:**
Facebook's privacy model is the most complex in social media. Post audience settings include: Public, Friends, Friends of Friends, Specific People, All Except Specific People, Only Me, Custom Lists, and Group-restricted. Privacy check must: (1) be correct 100% of the time — privacy violations are a P0 incident; (2) not add > 100ms to feed read latency; (3) handle complex custom rules efficiently; (4) remain correct across friend additions/removals even for cached data.

**All Possible Approaches:**

| Approach | Description | Pros | Cons |
|----------|-------------|------|------|
| Inline privacy check per post at read time | For each candidate post, query TAO for (requester, author) friendship | Always fresh, always correct | O(candidates × friends_check) = too slow; 2000 candidates × 5ms/TAO call = 10 seconds |
| Pre-filter at fan-out time | Only push post to users who can see it during fan-out | Read path is simpler | Fan-out must know all users' privacy settings; fails for dynamic rules (custom lists) |
| Batch privacy check with caching | Cache (user_A, user_B, audience_type) → bool with 5-minute TTL | Balance of correctness and latency | Brief window where friend removal doesn't immediately hide posts |
| ACL-based token check | Precompute a privacy token for each post; token embedded in feed cache | O(1) privacy check at read time | Token must be invalidated on friend/unfollow events (complex) |

**Selected Approach: Batch TAO privacy check with friendship cache**

At feed read time, post candidate_ids arrive with their `audience_type` already stored. Fast path for simple audience types: "Public" — no check needed. "Friends" — batch-check if requester is in author's friends list using TAO (cached in Memcache layer). "Only Me" — check if requester == author. "Custom" — full rule evaluation (slower, rare).

```python
class PrivacyFilter:
    def filter_posts(self, viewer_id, candidate_posts):
        allowed = []
        for post in candidate_posts:
            if self._can_view(viewer_id, post):
                allowed.append(post)
        return allowed

    def _can_view(self, viewer_id, post):
        if post.author_id == viewer_id:
            return True  # Own posts always visible

        if post.audience_type == AUDIENCE_PUBLIC:
            return True  # No check needed

        if post.audience_type == AUDIENCE_ONLY_ME:
            return False  # Already handled above (viewer != author)

        if post.audience_type == AUDIENCE_FRIENDS:
            # TAO association lookup: is (author_id, FRIEND, viewer_id) an edge?
            # Cache key: "friendship:{min(a,b)}:{max(a,b)}" TTL 5 min
            cache_key = f"friendship:{min(post.author_id, viewer_id)}:{max(post.author_id, viewer_id)}"
            cached = redis.get(cache_key)
            if cached is not None:
                return cached == '1'
            is_friend = tao.get_association(post.author_id, ATYPE_FRIEND, viewer_id) is not None
            redis.setex(cache_key, 300, '1' if is_friend else '0')
            return is_friend

        if post.audience_type == AUDIENCE_FRIENDS_OF_FRIENDS:
            # Check: viewer is friend, or viewer is friend-of-friend
            if self._is_friend(viewer_id, post.author_id):
                return True
            # Check if viewer and author share any mutual friends
            viewer_friends = tao.get_associations(viewer_id, ATYPE_FRIEND, limit=500)
            return tao.any_association(post.author_id, ATYPE_FRIEND, viewer_friends)

        if post.audience_type == AUDIENCE_CUSTOM:
            return self._evaluate_custom_rule(viewer_id, post)

        return False  # Default deny — fail closed

    def _evaluate_custom_rule(self, viewer_id, post):
        rules = post_store.get_audience_rules(post.post_id)
        for rule in rules:
            if rule.rule_type == 'include_list':
                if viewer_id in rule.target_ids:
                    return True
            elif rule.rule_type == 'exclude_list':
                if viewer_id in rule.target_ids:
                    return False
        # Default: friends can see if no explicit custom rule overrides
        return self._is_friend(viewer_id, post.author_id)
```

**Interviewer Q&As:**

Q: What happens when two users unfriend each other? How quickly do their "Friends" audience posts disappear from each other's feeds?
A: Unfriending writes to TAO (MySQL) immediately — the association row is deleted. The friendship cache in Redis has a 5-minute TTL. In the worst case, within 5 minutes of unfriending, a post with "Friends" audience could still pass the privacy check if the cache is still warm. This is a deliberate trade-off: 5-minute stale window in exchange for ~10× reduction in TAO reads. For a stricter requirement, we can: (a) publish an invalidation event to Kafka on unfollow that clears the friendship cache entry immediately, or (b) use a shorter TTL (30 seconds) at higher TAO read cost. Facebook's actual system uses cache invalidation on friend removal events.

Q: How do you handle the "Friends of Friends" audience check efficiently for users with 5,000 friends?
A: Fetching all 5,000 friends to check mutual friends would be O(friends) per post — too slow for 2,000 candidates. Optimization: (1) TAO stores friend lists in sorted order; we can check mutual friends using a set intersection on the TAO association index. (2) Pre-compute a Bloom filter of each user's friends: `friends_bloom:{user_id}` (false positive rate ~1%). Check if any of the author's friends appear in the requester's friends Bloom filter. A hit means "likely friends-of-friends" — follow up with exact check. A miss is definitive. This reduces expensive set intersection to a sub-millisecond Bloom filter query 99% of the time.

Q: What happens when a post's audience is changed after it was already fanned out to feeds?
A: Privacy changes to existing posts must propagate retroactively. When audience is updated: (1) Post metadata is updated in MySQL/TAO immediately. (2) A `post.audience_updated` Kafka event is published. (3) A consumer scans all user_feed entries for this post_id in Cassandra (potentially millions of rows for a popular post). (4) For each fan-out entry, re-evaluate privacy. Remove entries that no longer satisfy new audience. (5) This is O(viewers), expensive for viral posts. For common cases (e.g., user changes "Friends" to "Only Me"), we take the simpler approach: leave the post in feed caches but the privacy filter at read time will block it — eventual removal from visible feed. The reader never sees the post again even if still in the feed cache.

Q: Can you describe how Facebook handles "soft privacy" features like restricted lists?
A: Restricted lists are a custom audience exclusion list — users on the restricted list see only "Public" audience posts from the lister, even if they're friends. Implementation: "Restricted" is a special value in the `audience_data` JSON that acts as a per-viewer override. When privacy checking: (1) Determine requester's relationship (friend). (2) Check if requester is on the author's restricted list (a special association type in TAO: ATYPE_RESTRICTED). (3) If restricted, demote audience to "Public" for this viewer — only Public posts visible. This is an O(1) TAO lookup per (author, viewer) pair, cached like the friendship check.

---

### Component: MultiFeed / Fan-out Architecture

**Problem it solves:**
When a user posts, up to 5,000 friends need to see it in their feeds. Facebook's "MultiFeed" system precomputes a personalized candidate list per user (the set of posts they might see), so that feed reads are fast (read from precomputed store) rather than requiring real-time computation across all friends' posts. Fan-out must be durable, scalable, and fast enough that posts appear in feeds within ~30 seconds.

**Implementation Detail:**

```python
# MultiFeed Fan-out Worker
def handle_post_created(event):
    post_id = event['post_id']
    author_id = event['author_id']
    audience_type = event['audience_type']

    # Determine who should receive this post in their feed
    if audience_type == AUDIENCE_ONLY_ME:
        recipients = []  # No fan-out; only in author's own profile
    elif audience_type == AUDIENCE_PUBLIC:
        recipients = get_all_followers(author_id)  # Pages/celebrities
    else:
        recipients = get_friends(author_id)  # Up to 5000

    # Write post reference to each recipient's feed in Cassandra (batched)
    # Compute a basic pre-rank score (cheap, used for pre-sorting without full ranking)
    base_score = compute_base_score(
        post_age_hours=0,
        author_follower_count=event['author_follower_count'],
        post_type=event['post_type']
    )

    batch_size = 500
    for i in range(0, len(recipients), batch_size):
        batch = recipients[i:i + batch_size]
        with cassandra.batch() as b:
            for recipient_id in batch:
                b.execute("""
                    INSERT INTO user_feed (user_id, post_id, author_id, 
                                           source_type, pre_rank_score, added_at)
                    VALUES (%s, %s, %s, %s, %s, %s)
                """, (recipient_id, post_id, author_id, 1, base_score,
                       datetime.utcnow()))

                # Cap feed at 5000 entries: remove oldest
                b.execute("""
                    DELETE FROM user_feed WHERE user_id = %s AND post_id IN (
                        SELECT post_id FROM user_feed WHERE user_id = %s
                        ORDER BY post_id ASC LIMIT 1 OFFSET 5000
                    )
                """, (recipient_id, recipient_id))

# Note: the above cap logic uses application-level management in practice
# (Cassandra doesn't support subqueries; use Redis to track count + ZREMRANGEBYRANK)
```

**Interviewer Q&As:**

Q: Pages with millions of followers (e.g., a major news outlet) would require millions of feed writes per post. How do you handle this?
A: Celebrity/Page fan-out problem solved identically to Twitter: above a threshold (e.g., 100K followers), switch from push fan-out to pull. The page's posts are NOT written to individual user_feed rows. Instead, at Feed Service read time: (1) Identify which high-follower pages the user follows (from TAO, cached in session). (2) Fetch recent posts from those pages directly from the Post Store. (3) Merge with precomputed feed from MultiFeed Store. (4) Run ranking on merged candidate set. This "MultiFeed pull for high-follower sources" reduces write amplification dramatically.

Q: The MultiFeed Cassandra store can have 5000 entries per user. How do you handle pagination for long scroll sessions?
A: Feed Service reads from MultiFeed Store using cursor-based pagination on `post_id` (Snowflake, time-ordered). The cursor encodes the last `post_id` seen by the client. `SELECT WHERE user_id = ? AND post_id < cursor_post_id ORDER BY post_id DESC LIMIT count`. As the user scrolls, progressively older posts are fetched. At the end of the 5000-entry MultiFeed, Feed Service falls back to fan-out on read: it queries the top 20 friends (by affinity score) and fetches their recent posts directly from the Post Store. "You've reached the end of your stories from the past 3 days" is a common UX message here.

---

## 7. Scaling

### Horizontal Scaling

**Feed Service**: Stateless; scales horizontally. At 1.15M read QPS, with each instance handling ~10K QPS via parallel async calls to Cassandra, Redis, and TAO, ~115 instances at average load and ~500 at peak. Auto-scale on CPU and p99 latency.

**Fanout Workers**: Kafka consumer group, one consumer per partition. At peak 25K post writes/sec and average 300 friends/post = 7.5M Cassandra writes/sec. With Cassandra handling 50K writes/sec/node, need ~150 Cassandra nodes and ~200 Fanout Worker instances. Scale consumer count by increasing Kafka partition count.

**Post Service**: Stateless, low write QPS (5,800/sec), scales easily. Primary bottleneck is MySQL writes — use Vitess for horizontal sharding.

### Database Scaling

**Sharding:**

*TAO (MySQL)*: Shard by `object_id % num_shards` (objects table) and by `id1 % num_shards` (associations table). Facebook's public papers describe thousands of MySQL shards. Vitess manages sharding, connection pooling, and resharding operations. Each shard has master + 2 read replicas.

*Cassandra (MultiFeed)*: Consistent hashing on `user_id`. With 500M DAU, feed store is 500M × 5000 × 50 bytes = 125 TB. With 100-node Cassandra cluster (RF=3), each node holds ~1.25 TB — manageable. Vnodes distribute load evenly.

*Cassandra (Reactions, Comments)*: Partitioned by `post_id`. Hot partitions (viral posts with millions of reactions) mitigated by Cassandra's distribution — the post_id partition sits on 3 replicas; reads distributed across replicas.

**Replication:**

*TAO*: Multi-master MySQL with semi-synchronous replication for primary writes. Memcache layer (Facebook's custom Memcache at massive scale) absorbs reads. Replication across 5+ global datacenters for read locality. Leader/follower pool architecture: each geographic region has a regional Memcache pool; reads served locally. Cache invalidations propagated via the McSqueal invalidation system.

*Cassandra*: Multi-DC with `NetworkTopologyStrategy`, RF=3 per DC. `LOCAL_QUORUM` for writes (2/3 in local DC), `LOCAL_ONE` for feed reads.

**Caching:**

- **TAO (Memcache)**: Graph data cached at massive scale. Facebook publishes cache hit rates of 99%+ on TAO. Without this cache, MySQL would need to serve 1B+ reads/second — impossible.
- **MultiFeed Cassandra**: The precomputed feed IS the cache — it's the most important pre-computation in the system.
- **Ranking Feature Store**: Hot user features (active DAU features) cached in Redis with 5-minute TTL. Cache miss hits Scuba.
- **Post object cache**: Hot posts (viral, trending in network) cached in Memcache by post_id. Reduces Post Store reads for recently published viral posts.
- **Privacy check cache**: `friendship:{id1}:{id2}` → bool, 5-minute TTL in Redis. Dramatically reduces TAO reads during privacy filtering of large candidate sets.

**CDN:**

- Facebook uses its own CDN infrastructure globally (Points of Presence at ISPs and data centers worldwide).
- All media (photos, videos) served from CDN. Cache-Control: `max-age=86400` (photos) and `max-age=3600` (recently uploaded). Content-addressed URLs (hash in path = immutable caching).
- Video content: HLS/DASH segments cached at CDN edge. Popular videos (viral) get proactively cached to all major PoPs.
- Origin S3/Haystack for cache misses.

### Interviewer Q&As on Scaling

Q: How does Facebook handle the N+1 query problem when hydrating 50 posts on a feed load?
A: All data fetches are batched. Feed Service fetches 50 post_ids from MultiFeed Store in one Cassandra query. Then batches a single multi-get to TAO for all 50 post objects (and their 50 authors) simultaneously. TAO's Memcache layer serves multi-get responses for all 50 objects in one round trip (using Memcache's multi-get protocol). Reaction counts and comment counts are embedded in the post object (denormalized, updated async). Result: 3 network round trips total for 50 posts, regardless of post count.

Q: Describe how Facebook's Memcache invalidation works when a post is deleted.
A: The McSqueal invalidation system: MySQL binary log is tailed by an invalidation daemon that watches for UPDATE/DELETE operations. When a post row is deleted, McSqueal publishes an invalidation message to a dedicated Memcache invalidation topic (per region). Each Memcache pool in each region receives the invalidation and deletes the cached post object. Propagation latency: < 100ms within a region, < 500ms cross-region. During the invalidation window, stale post objects may be served — this is acceptable given the brief window. For privacy-critical data (friendship status, audience rules), invalidations are prioritized.

Q: How do you prevent Cassandra from being overwhelmed by fan-out writes for viral posts?
A: Viral post fan-out = many concurrent writes to Cassandra. Mitigations: (1) Kafka absorbs burst: Fanout Workers process at their own pace; Kafka topic has enough partition count to distribute load. (2) Cassandra write rate limited per Fanout Worker instance using a token bucket. (3) For large fan-outs, batch writes using `LOGGED BATCH` (atomicity per batch) or unlogged batches (faster, no atomicity needed for feed entries). (4) For viral/celebrity posts, switch to read-time pull as described above. (5) Cassandra write throttling: set `max_counter_cell_size` and coordinator-level timeouts to prevent any single fan-out from monopolizing coordinator resources.

Q: How does Facebook scale to 3 billion users when MySQL has practical limits of ~100M rows per table?
A: Every MySQL table is sharded. Users: shard by `user_id % 10000` = 10,000 shards. Each shard is a separate MySQL instance. Vitess provides a sharding proxy layer that routes queries to the correct shard transparently. Resharding is done live using Vitess's vertical and horizontal split operations. For TAO specifically, Facebook's published architecture describes thousands of MySQL shards each with <100M rows. The Memcache layer means MySQL is only hit on cache misses, keeping per-shard QPS manageable.

Q: If a user goes from 1,000 followers to 10M followers overnight (viral moment), how does the system adapt?
A: The system detects high-follower accounts via a background job that periodically recounts followers and updates the `is_high_follower` flag in the user object. The fan-out routing logic reads this flag before processing. If a user crosses the threshold mid-viral event, some posts during the transition may be push-fanned out (at high cost) before the flag is updated. To handle this: (1) Real-time follower count monitoring with a threshold alarm. (2) Emergency flag update can be triggered manually or automatically when follower_count crosses a hard limit. (3) For the brief window of over-fan-out, Kafka's durability ensures no events are lost — Fanout Workers may lag but won't drop events.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Impact | Mitigation |
|-----------|-------------|--------|------------|
| Feed Service instance crash | In-flight feed requests fail | Users see error; retry succeeds on another instance | LB health check routes away within 5s; stateless, so no session loss |
| MultiFeed Cassandra node failure | Some users' feed reads degrade | Feed read latency spikes; partial feed returned | RF=3, LOCAL_QUORUM tolerates 1-node failure; coordinator retries on different replica |
| TAO Memcache failure | Privacy checks fall back to MySQL | Feed latency spikes from 5ms to 50ms per check; reduced throughput | Memcache pools are redundant (n+1 capacity); cold cache warms in <5 minutes under traffic |
| Fanout Worker crash (mid-batch) | Some friends miss the post in their precomputed feed | Feed may be incomplete | Kafka offset uncommitted; worker replay from last offset; Cassandra write idempotent |
| MySQL shard failure (post store) | Writes to that shard fail; reads from replica | Writes fail for affected user_id range | Semi-sync replication; Vitess promotes replica to master in <60s; RPO < 1 transaction |
| Ranking Service unavailable | Feed ranking degrades | Return chronologically sorted feed as fallback | Circuit breaker: if ranking fails, return pre-scored feed order |
| Privacy Service timeout | Privacy check cannot complete | Post excluded from feed (fail-closed design) | Correct behavior; system errs on side of privacy over availability |
| Kafka broker failure | Fan-out events delayed | Posts appear in friends' feeds with delay | Kafka RF=3 per partition; new leader elected in <30s; no event loss |

### Failover Strategy

- **Multi-region active-active**: Traffic served from multiple regions with GeoDNS. User sessions are region-affine (sticky routing to closest region). MySQL TAO data replicated cross-region asynchronously (~200ms lag acceptable for social graph reads).
- **Failover priority**: User-facing reads (feed) > fan-out writes > analytics events. Under extreme load, fan-out workers are throttled first (feed becomes slightly stale) before user-facing reads are impacted.
- **Degraded mode**: If ranking model is unavailable, serve feed in reverse-chronological order from MultiFeed Store. Surface this to users as "Feed is temporarily simplified."

### Retries & Idempotency

- **Post creation**: Idempotency key enforced by Post Service. Client includes `X-Idempotency-Key: {uuid}`. Duplicate within 24h returns original post_id.
- **Fan-out writes**: `INSERT INTO user_feed ... IF NOT EXISTS` or application-level dedup using post_id PK in Cassandra. Replay-safe: same post_id written twice = idempotent.
- **Reaction write**: `INSERT INTO reactions (post_id, user_id, reaction_type)` with PK on (post_id, user_id) ensures uniqueness. Changing reaction type = UPDATE. Idempotent on retry.
- **Privacy check**: Read-only; inherently idempotent. Re-queries TAO on retry.

### Circuit Breaker

- **Feed ranking circuit**: Opens if > 30% of ranking requests fail within 10s. Fallback: pre-scored chronological order from MultiFeed Store.
- **TAO circuit**: Opens if p99 latency > 200ms for 5 consecutive seconds. Fallback: serve unfiltered candidates for 30s (risk of brief privacy over-inclusion — escalate as incident immediately). Privacy circuit is a last-resort, monitored 24/7.
- **Cassandra circuit**: Opens if > 20% write errors. Fanout Worker stops writing and publishes to dead-letter queue for retry after circuit resets.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Why It Matters |
|--------|------|-----------------|----------------|
| Feed p99 read latency | Histogram | > 500ms | Core SLA |
| Privacy filter error rate | Counter | > 0 (any error) | Privacy correctness is P0 |
| TAO cache hit rate | Ratio | < 98% | Indicates cache degradation affecting read throughput |
| MultiFeed Cassandra write lag (Kafka) | Gauge | > 200K messages | Fan-out falling behind |
| Feed ranking model inference p99 | Histogram | > 200ms | Ranking SLA |
| Post creation success rate | Ratio | < 99.99% | Write availability |
| Privacy check latency p99 | Histogram | > 100ms | Privacy on critical path |
| Cassandra read p99 | Histogram | > 20ms | Storage layer health |
| Reaction write success rate | Ratio | < 99.9% | Engagement reliability |
| Feed diversity score | Custom (avg author entropy per session) | < 0.6 | Feed quality; detects filter bubble |
| Model feature staleness | Gauge | > 300s | Features must be fresh for ranking quality |
| Fan-out worker throughput | Counter | Sudden drop > 50% | Worker fleet health |

### Distributed Tracing

- OpenTelemetry SDK instrumented in all services. Trace propagated via gRPC metadata and HTTP headers.
- Critical trace path: Client request → API Gateway → Feed Service → [Privacy Filter (TAO), MultiFeed Cassandra, Feature Store, Ranking Service, Post Hydration (TAO)] → Response.
- Each span labeled with user_id (sampled, hashed), post_id, service name, duration.
- Privacy violations trigger a P0 alert and automatically sample the full trace for investigation.
- Flame graph visualization in internal tooling (equivalent: Jaeger) to identify latency outliers.

### Logging

- Structured JSON at all service boundaries.
- Feed generation log: `{trace_id, user_id, candidate_count, privacy_filtered_count, ranked_count, total_latency_ms, ranking_model_version, fallback_used: bool}`.
- Privacy audit log: `{post_id, viewer_id, audience_type, result: "allowed"|"denied", reason}` — append-only, immutable, 7-year retention for regulatory compliance. Stored in separate immutable storage (WORM S3 Glacier).
- Alert on: `"privacy_result": "allowed"` when `audience_type = "only_me"` and `viewer_id != author_id` — indicates a critical privacy bug.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Option Rejected | Reason |
|----------|---------------|-----------------|--------|
| Feed generation | Precomputed MultiFeed + ranking at read time | Pure fan-out on read (compute at read) | 3B users × 500 friends × 2 posts/day = 3 trillion read-time computations/day — infeasible |
| Privacy filtering | Fail-closed (deny on uncertainty) | Fail-open (allow on timeout) | Privacy violation is a P0 incident; brief inaccessibility of borderline posts is acceptable |
| Social graph storage | TAO (MySQL + Memcache) | Neo4j / graph database | MySQL handles the association query patterns efficiently; proven at Facebook's scale; Neo4j doesn't scale to 3B nodes |
| Ranking | Cascade (cheap pre-scorer + deep ranker) | Single model for all candidates | Single heavy model over 5000 candidates would take seconds; cascade enables sub-200ms ranking latency |
| Fan-out strategy | Hybrid push-pull (push for regular users, pull for pages) | Pure push | Pages with millions of followers make pure push infeasible (millions of Cassandra writes per post) |
| Post storage | MySQL (metadata) + Haystack (media) | Cassandra for all | MySQL provides ACID for post creation and audience rule enforcement; Haystack purpose-built for blob storage at Facebook's specific scale |
| Reaction counts | Denormalized in post object + async update | Live count from reactions table | Live count from reactions table = join on every feed read; denormalized counts serve in O(1) from post cache |
| Privacy cache TTL | 5-minute friendship cache | No caching | Without cache, TAO reads would increase 100× during feed generation; 5-minute stale window acceptable for friendship status |
| Multi-region consistency | Eventual consistency cross-region | Strong cross-region consistency | Cross-region strong consistency requires synchronous replication = 100–200ms added latency per write; not acceptable at 5,800 writes/sec |
| EdgeRank features | Real-time feature store (sub-second freshness) | Batch-computed daily features | Daily features would miss engagement velocity spikes (a post going viral in 1 hour); real-time features capture current engagement momentum |

---

## 11. Follow-up Interview Questions

**Q1: How would you implement Facebook's "On This Day" / "Memories" feature?**
A: "On This Day" shows users their posts from exactly 1 year ago (and 2, 3, 5 years ago). Implementation: a daily batch job (Spark or Flink) runs at midnight UTC for each timezone, queries `posts_by_user` where `created_at` falls in target date range for each user. Results written to a `memories (user_id, post_id, memory_date, years_ago)` table (Cassandra). Feed Service, when constructing the feed for the day, checks for available memories and injects them as special feed items. Privacy: only show memories to the posting user themselves. ML filter: if the post was later deleted or received high negative feedback, suppress the memory.

**Q2: How do you design "Group posts in News Feed" when a user is a member of 500 groups?**
A: Groups contribute posts to the feed via a separate fan-out pipeline. When a post is created in a group: (1) Fan-out worker fetches group members (can be millions). (2) For large groups, uses the celebrity fan-out approach — don't push to individual feeds; pull at read time. (3) Feed Service maintains a list of groups the user is a member of (TAO: ATYPE_MEMBEROF associations). For small groups (<1000 members), push fan-out. For large groups, pull. Group post quality signals (engagement rate within group vs. outside) additionally weight group posts lower to prevent groups from dominating personal feed.

**Q3: How does Facebook's News Feed handle real-time updates (posts appearing without page refresh)?**
A: Long-polling or WebSocket connection maintained between client and a Real-time Notification Service. When a high-affinity friend (close friend, family) posts: (1) Fan-out worker writes to MultiFeed as normal. (2) Also publishes to a separate Kafka topic `realtime.feed.update`. (3) Real-time Notification Service (consuming from this topic) identifies which connected clients should be notified. (4) Sends a lightweight push notification (not full post data) to the client via WebSocket. (5) Client shows "New posts available" banner; user taps to refresh feed (avoiding jarring mid-scroll inserts). Full post data fetched on refresh.

**Q4: How would you build the "See First" feature (certain friends' posts always at top)?**
A: "See First" is a user-set override that pins specific friends/pages to the top of their feed. Implementation: store `see_first_list (user_id, source_id, added_at)` per user (MySQL, small table). At ranking time: after standard ranking, re-order to ensure any posts from See First sources appear in positions 1–3, regardless of rank score. This is a post-ranking override, not a ranking signal, to ensure deterministic behavior. Limit: maximum 30 See First accounts to prevent the feed from becoming a manually curated list rather than an algorithmic one.

**Q5: How do you detect and demote clickbait links in the News Feed?**
A: Multi-stage approach: (1) Domain reputation score: pre-computed quality score per domain based on historical user engagement (dwell time after click, bounce rate, "like" vs. "hide" ratio). Stored in a lookup table, updated daily via Spark job. Low-reputation domains get lower weight in EdgeRank. (2) Headline analysis: NLP classifier trained on human-labeled clickbait headlines (e.g., "You won't believe...", "10 things..."). Run at post creation time, sets a `clickbait_score` field on the post. (3) User signal: if users who clicked this link quickly returned to Facebook (low dwell time), the link's quality score is further reduced retroactively. Link quality signals flow back into the Feature Store.

**Q6: How does Facebook ensure the News Feed doesn't create political echo chambers?**
A: This is as much a policy decision as a technical one. Technical mitigations: (1) Diversity injection: 10–15% of feed positions are filled with "perspective diversity" content — high-quality posts from outside the user's ideological cluster. (2) Political content de-amplification: Facebook has publicly stated they reduced political content distribution in certain regions. This is implemented as a category classifier (political/non-political) + a feature weight penalty in the ranking model. (3) Cross-partisan bridging score: measure of how much content engages users across political lines; high-bridge content is up-ranked. These are deliberate ranking interventions, separate from the core EdgeRank algorithm.

**Q7: How would you design a "Saved Posts" feature (bookmarking)?**
A: Saves data model: `saved_posts (user_id, post_id, saved_at, collection_id)` in Cassandra (fast append, read by user_id). Collections: `collections (collection_id, user_id, name, created_at)` in MySQL. `GET /v1/saved` returns user's saved posts paginated by `saved_at DESC`. Privacy: only the saving user can see their saved collection. Saves count as a positive engagement signal in the Feature Store — a post being saved (even without a like) signals quality content, which feeds back into ranking for other users.

**Q8: What would you change about the architecture to support 100× more content types (3D, AR objects, NFTs)?**
A: The TAO object model is type-agnostic — new content types are new `otype` values with custom `data` Blob schemas. Media Pipeline extensions needed: (1) 3D models: store `.glb`/`.usdz` files in Haystack, serve via CDN, client-side renderer (WebGL/ARKit). (2) AR effects: 15 MB max, CDN-served with versioning. (3) NFT proofs: on-chain verification hash stored in post metadata; verification service queries blockchain (Ethereum node) at display time. Core feed infrastructure doesn't change — content types add processing pipeline stages but the fan-out, ranking, and serving remain the same.

**Q9: Describe how you would implement feed AB testing at scale.**
A: Facebook serves billions of feed loads/day — A/B testing is critical for ranking model improvements. Architecture: (1) Experiment assignment: at API Gateway, each request is assigned to an experiment bucket based on user_id hash (consistent, so same user always in same bucket). Bucket → feature flags/model versions stored in config service (ZooKeeper or Consul). (2) Feed Service reads experiment config and uses the specified ranking model version. (3) Logging: every feed impression logged with `{experiment_id, bucket, user_id_hash, post_id, position, was_engaged: bool}` to analytics pipeline. (4) Analysis: Flink computes per-experiment engagement rates with statistical significance testing. (5) Guardrail metrics: monitor for p99 latency regressions or privacy error rate increases in any experiment bucket.

**Q10: How do you handle the "multi-account household" problem where a family shares a device?**
A: Account isolation is maintained by session tokens. Each account's feed is separately cached and ranked under their user_id. If multiple accounts are active on the same device: (1) Feed Service uses the `user_id` from the auth token, not device-level signals. (2) Feature Store per-user profiles are not cross-contaminated (engagement signals stored per user_id). (3) The only shared signal is IP-based spam scoring and device fingerprint for abuse detection — these are account-level inputs, not personalization signals. Multi-account profiles on a single device are first-class supported; the API treats each account independently.

**Q11: What is "story aggregation" in the feed context, and how is it implemented?**
A: Story aggregation: if 5 friends all share the same news article, show it once as "5 friends shared this article" rather than 5 separate posts. Implementation: (1) At fan-out time, post canonical URL is extracted from link posts. (2) A deduplication key `(canonical_url, time_window_hour)` is computed and stored in a Redis sorted set with score = count of sharers. (3) At feed read time, if a set of posts share the same deduplication key and count > 1, they're collapsed into one feed item with a summary. (4) Clicking expands the item to show all sharers. This requires careful UX — the collapsed item must clearly show all actors and the original content.

**Q12: How does Facebook handle feed generation for users who have been offline for 7 days?**
A: A 7-day offline user returns to a stale or expired MultiFeed Store entry (5000 entries may be fully stale). Approach: (1) Upon session start, detect "cold return" (last_active > 3 days). (2) Instead of serving stale feed, trigger an on-demand feed rebuild: fetch recent posts from top-30 affinity friends (by affinity score from Feature Store), plus trending posts in the user's interest graph. (3) The rebuilt feed is chronological and unranked initially (fast path: ~200ms). (4) In parallel, async fan-out job catches up the MultiFeed Store for this user. After ~5 minutes, subsequent feed loads use the refreshed MultiFeed. (5) Special "while you were away" UI section summarizes engagement on old posts the user missed.

**Q13: How would you add "content warnings" to the News Feed for potentially sensitive content?**
A: At post creation: ML classifier runs on media and text to detect sensitive categories (graphic violence, nudity, disturbing news). Confidence score stored in post metadata `{sensitivity_type, confidence}`. At feed serving: posts with `confidence > 0.8` for sensitive categories are annotated in the feed response with a `sensitivity_warning` field. Client renders a blurred preview + "View sensitive content?" prompt. Users can configure sensitivity preferences (show/hide per category) in settings, stored in user profile. The content is not hidden from the feed — only the presentation is modified. High-confidence NSFW/violence content is held for human review before distribution (separate trust & safety system).

**Q14: What happens when the ML ranking model produces obviously wrong results (major bug)?**
A: Defense layers: (1) Shadow mode testing: new model runs in shadow (doesn't affect users) and its outputs are compared against the current production model. Divergence above threshold blocks promotion. (2) Canary deployment: promote to 1% of users, monitor engagement and negative feedback metrics for 24 hours before full rollout. (3) Guardrail metrics: hard limits on ranking model output diversity (no single author > 30% of top-10 slots), freshness (no post older than 7 days in top-3 positions). These rules are applied post-inference and override model scores if violated. (4) Rollback: model serving infrastructure supports instant rollback to previous model version by updating a config flag. All model versions are stored and available for 90 days.

**Q15: How do you ensure the feed doesn't amplify misinformation?**
A: Multi-layer approach: (1) Third-party fact-checker integration: posts containing claims fact-checked as false are annotated with `{verdict, checker_name}`. Annotated posts are down-ranked (weight reduction in EdgeRank) and shown with a "Checked by independent fact-checkers" label. (2) Repeat-sharer penalty: users who repeatedly share content later labeled as misinformation have a temporary "sharing quality" score reduction applied to posts they share. (3) Engagement velocity anomaly detection: Flink detects posts with unusual engagement patterns (very fast initial spread, then rapid negative feedback) — flagged for expedited fact-check review. (4) Reshare friction: for flagged content, adding a "reshare" confirmation step reduces velocity spread by ~40% (published research). These are policy-technical combinations; pure technical solutions are insufficient.

---

## 12. References & Further Reading

- Bronson, N. et al. "TAO: Facebook's Distributed Data Store for the Social Graph." USENIX ATC 2013. — Primary reference for TAO architecture, association model, and cache hierarchy.
- Nishtala, R. et al. "Scaling Memcache at Facebook." USENIX NSDI 2013. — McSqueal invalidation, leader/follower pools, regional memcache.
- Bakshy, E. et al. "Exposure to Ideologically Diverse News and Opinion on Facebook." Science, 2015. — Research on echo chambers and News Feed algorithmic effects.
- Facebook Engineering Blog: "News Feed FYI" series — published articles on EdgeRank evolution, engagement bait, clickbait demotion, diverse perspectives.
- Facebook Engineering Blog: "Introducing FBLearner Flow: Facebook's AI Backbone" — ML training infrastructure used for ranking model training.
- Huang, C. et al. "Facebook's Tectonic Filesystem: Efficiency from Exascale." USENIX FAST 2021. — Facebook's unified storage layer replacing Haystack for newer systems.
- He, X. et al. "Practical Lessons from Predicting Clicks on Ads at Facebook." ADKDD 2014. — GBDT+logistic regression combination, directly applicable to pre-scoring stage.
- Wang, R. et al. "Deep & Cross Network for Ad Click Predictions." ADKDD 2017. — DCN model architecture used conceptually in the deep ranking stage.
- "Designing Data-Intensive Applications" by Martin Kleppmann (O'Reilly, 2017) — Chapters 2 (Data Models), 5 (Replication), 6 (Partitioning).
- Kleppmann, M. et al. "Online Event Processing." Queue, 2019. — Kappa architecture and event sourcing patterns relevant to fan-out pipeline.
- Vitess documentation: vitess.io — MySQL horizontal sharding used for post and user tables.
- Apache Kafka documentation: kafka.apache.org — Used throughout the fan-out and ranking pipeline.
- Facebook Engineering Blog: "The Architecture of a Large-Scale Web Search Engine" — privacy in distributed systems context.
