# Problem-Specific Design — Social Media (02_social_media)

## Twitter

### Unique Functional Requirements
- Posts (tweets) limited to 280 characters of text
- Retweet and quote-tweet mechanics (re-sharing another user's tweet as-is, or with added commentary)
- Reply threading (replies linked to original tweet by `reply_to_id`)
- Direct messaging between users
- Trending topics by region, updated within 60 seconds
- User profile pages showing their tweet history

### Unique Components / Services
- **Trend Aggregation Service** (Apache Flink / Storm): counts hashtag/keyword frequency over 5-minute sliding windows using Count-Min Sketch; writes top-50 trending terms per region to Redis every 60 seconds
- **Snowflake ID Generator Service** (deep-dived): machine IDs leased from ZooKeeper with 30-second refresh; spin-wait to next millisecond on sequence overflow
- **ZooKeeper**: manages Snowflake machine ID leases across the cluster

### Unique Data Models
- `tweets` (Cassandra): tweet_id, user_id, body, media_keys, reply_to_id, retweet_of_id, like_count, retweet_count, reply_count, is_deleted, created_at
- `tweets_by_user` (Cassandra): PRIMARY KEY (user_id, tweet_id DESC) for profile page range scans
- `notifications` (Cassandra): PRIMARY KEY (user_id, notif_id DESC)
- `follows` (MySQL): PRIMARY KEY (follower_id, followee_id) + secondary index on (followee_id, follower_id)
- Redis trending: key `trending:{region}:{window}` → Sorted Set (score = frequency count, TTL 6 minutes)
- Timeline pagination: opaque cursor encoding last-seen tweet_id; `next_cursor=null` when exhausted

### Unique Scalability / Design Decisions
- **Count-Min Sketch for trending**: reduces memory from O(unique terms × windows × regions) to O(width × depth) with ~1% error
- **Trending velocity score**: velocity = (count_last_5min − avg_count_prev_60min) / stddev; threshold at 3 sigma; guards against manipulation via account-age weighting, IP deduplication, and synthetic-term blocklist
- **Soft deletes with 30-day hard delete**: tweet `is_deleted=true` immediately; background job hard-deletes after 30 days
- **Search index**: Elasticsearch with 30-day rolling retention; tweet_id, user_id, body (analyzed), hashtags (keyword), like_count
- **MySQL follow graph sharded via Vitess**: `follower_id % N`; secondary index scatter-gather for follower lists
- **Notification deduplication**: same `(actor_id, object_id, type)` within 30 minutes collapsed into one notification
- **Fan-out dead-letter queue**: failed fan-out writes go to DLQ; Fanout Worker has 5-second per-message timeout and 1,000-entry Redis pipeline batch

### Key Differentiator
Twitter's defining design problem is Snowflake ID generation and the Count-Min Sketch trending pipeline — short-form text posts mean media complexity is low, but the need for globally unique time-ordered IDs at 25K+/sec and real-time trend detection at regional granularity are its signature deep dives.

---

## Instagram

### Unique Functional Requirements
- Photo and short video uploads with captions and hashtags
- Stories: ephemeral content with 24-hour TTL, auto-deleted after expiry
- Explore page: content discovery from accounts the user does not follow
- Syntax-highlighted caption rendering with hashtag/mention linking
- Direct messaging (DMs) with end-to-end encryption (Signal Protocol)
- Reels: short-form video with adaptive bitrate delivery (HLS/DASH)
- Copyright detection on photos and videos

### Unique Components / Services
- **Media Processor fleet** (GPU workers): video transcoding to multiple output formats (thumbnail_150, thumbnail_300, standard_640, original WebP); GPU-accelerated
- **Story Expiry Worker**: async cleanup — removes S3 objects and purges CDN 30 minutes before story TTL expires; Redis bloom filter `has_active_stories` pre-checks before Cassandra query
- **FAISS (Approximate Nearest Neighbor)**: vector similarity search over 128-dim user and post embeddings for Explore candidate retrieval
- **Two-Tower Neural Network**: user tower + post tower generating 128-dim embeddings for Explore and feed ranking
- **PhotoDNA API**: matches uploaded images against known copyright database; external integration
- **AudD**: audio fingerprinting service for Reels sound attribution
- **Key Distribution Service + Signal Protocol**: end-to-end encryption for DMs
- **Memcached with SSD backing**: warm-tier cache for posts viewed > 1,000 times in 24 hours
- **Dual CDN (Fastly + Akamai)**: two simultaneous CDN providers to eliminate vendor lock-in and single point of failure

### Unique Data Models
- `stories` (Cassandra): PRIMARY KEY (user_id, story_id) WITH TTL 86400; row-level TTL enforces 24-hour expiry at storage layer
- `story_views` (Cassandra): PRIMARY KEY (story_id, viewer_id)
- Feed cache: key `feed:{user_id}` (Sorted Set, score = post_id for time ordering, max 500 entries, TTL 3 days)
- Stories sorted set: unseen-first, then by recency; bloom filter `has_active_stories:{user_id}` for O(1) pre-check
- Explore: MMR (Maximal Marginal Relevance) for diversification + epsilon-greedy exploration (15% unexplored category slots)
- Reels: video embeddings from VideoMAE; loop count tracked separately from view count

### Unique Scalability / Design Decisions
- **Pre-signed S3 URLs for upload**: direct client-to-S3 upload eliminates Upload Service bandwidth bottleneck; server only issues a pre-signed URL
- **Cassandra TTL for Stories**: storage-layer TTL enforcement means no separate scheduler needed to prevent serving expired story data
- **Story CDN purge**: Cache-Control `max-age=3600` limits exposure window; preemptive CDN invalidation 30 minutes before expiry as defense-in-depth
- **WebP format for photo storage**: 25–34% smaller than JPEG at same quality; fallback JPEG for legacy clients
- **Explore cold-start**: onboarding interest tags → recent popular posts → early personalization (epsilon-greedy exploration rate)
- **Feed ranking**: shallow 2-layer neural network (~500K parameters), TensorRT for CPU inference, < 3ms inference latency; features: relationship strength, post freshness (exponential decay), engagement velocity
- **Client-side SQLite cache**: last 20 tweets/posts cached in mobile app for offline browsing, 5-minute TTL

### Key Differentiator
Instagram's defining architectural complexity is the dual challenge of ephemeral Stories (requiring TTL-based storage-layer expiry + multi-layer CDN purge) and the Explore page (requiring two-tower ANN candidate retrieval + multi-stage ML ranking for content from non-followed accounts), on top of a media-first architecture with GPU transcoding and multi-CDN delivery.

---

## Facebook News Feed

### Unique Functional Requirements
- Feed content types include text posts, photos, videos, shared links, events, life events, and check-ins
- Emoji reactions (6 reaction types: like, love, haha, wow, sad, angry) — not just a binary like
- Privacy filtering: per-post audience settings (Public, Friends, Friends of Friends, Specific Friends, Only Me, Custom) enforced at 100% correctness — no false positives allowed
- Group posts from groups the user is a member of
- Sponsored content (ads) interleaved at specific positions in the feed
- "See First" override: up to 30 accounts whose posts always appear in positions 1–3
- Real-time feed updates without full page reload (WebSocket / long-poll)
- "On This Day" feature: daily Spark/Flink batch job injecting memory posts

### Unique Components / Services
- **TAO** (The Associations and Objects store): MySQL-backed distributed graph database with a Memcache caching layer achieving >99% cache hit rate; stores all social graph objects and associations in Thrift-encoded format
- **Memcache (McSqueal)**: MySQL binary log tailing for sub-100ms Memcache invalidation within a region; McSqueal propagates binlog events to invalidate Memcache entries
- **MultiFeed Store** (Cassandra): precomputed ranked candidate list per user (`user_feed` table, capped at 5,000 entries); separate from the final ranked feed shown to users
- **Ranking & Scoring Service**: two-stage cascade — cheap GBDT pre-scorer on top 2,000 candidates → Deep Cross Network neural ranker on top 500
- **Scuba**: Facebook's internal time-series TSDB used as the feature store for the ranking model
- **Haystack**: Facebook's proprietary blob storage for photos and videos (equivalent: S3 + custom metadata index)
- **Real-time Notification Service**: WebSocket or long-polling for surfacing new posts without page reload

### Unique Data Models
- **TAO Objects table**: id, otype (1=User, 2=Post, 3=Page, 4=Group), data (Thrift-encoded), version, updated_at
- **TAO Associations table**: id1, atype (1=Friend, 2=Liked, 3=Followed, 4=MemberOf, 5=PostedBy, 6=CommentedOn), id2, time, data
- **posts** (MySQL): post_type (1=text, 2=photo, 3=video, 4=link, 5=share), audience_type (1=Public, 2=Friends, 3=FriendsOfFriends, 4=Custom, 5=OnlyMe), shared_post_id, location_id, reaction_counts (JSON per type), is_deleted
- **post_audience_rules** (MySQL): post_id, rule_type, target_ids — for Custom audience evaluation
- **user_feed** (Cassandra): user_id, post_id (Snowflake), author_id, source_type (1=friend, 2=page, 3=group, 4=suggested), pre_rank_score, is_seen, added_at; 5,000 entry cap
- **reactions** (Cassandra): PRIMARY KEY (post_id, user_id, reaction_type)
- **Feature Store** (Scuba): user_affinity_score, post_engagement_velocity, user_content_type_preference, 30+ features
- 7-year privacy audit log in WORM S3 Glacier (immutable, append-only)

### Unique Scalability / Design Decisions
- **Privacy filtering as a 100%-correct subsystem**: fail-closed (deny on uncertainty or timeout); Bloom filters for Friends-of-Friends checks; cache invalidated on friend-removal Kafka events; Friends path checks TAO with 5-minute friendship cache; batch privacy check to avoid per-post serialized lookups
- **Thrift encoding**: all TAO object data serialized as Thrift blobs for efficient binary storage and cross-language deserialization
- **McSqueal binlog tailing**: MySQL binary log tailed by McSqueal to invalidate Memcache entries within < 100ms in the same region — avoids double-write inconsistency
- **Cold return optimization**: users offline > 3 days trigger async MultiFeed catch-up job on next login
- **Group fan-out tiering**: small groups (< 1,000 members) use push fan-out; large groups use pull at read time
- **Misinformation defense**: third-party fact-checker integration, repeat-sharer penalty, engagement velocity anomaly detection, reshare friction (interstitial before resharing flagged content)
- **Cascade ranking with guardrails**: post-ranking multipliers for content category suppression (fast policy changes without model retraining)

### Key Differentiator
Facebook's defining complexity is the **privacy filtering system**: 100% correctness is required across multiple audience types (Friends, Friends-of-Friends, Custom) at billions of feed impressions per day, driving a dedicated fail-closed privacy subsystem backed by TAO's graph traversals and Bloom filter approximations — a constraint that does not appear at this level of strictness in any other problem in this folder.

---

## LinkedIn Feed

### Unique Functional Requirements
- Professional feed: posts from 1st-degree connections, followed pages, followed hashtags, and algorithmically injected 2nd-degree viral content
- 7 LinkedIn-specific reaction types (Like, Celebrate, Love, Insightful, Curious, etc.)
- Job recommendations interleaved in the feed based on the user's professional profile
- "People You May Know" (PYMK) recommendations injected into the feed
- Connection degree filtering: posts respect 1st/2nd/3rd-degree visibility rules
- Post editing and restoration within a time window
- Comment threading with parent_comment_id
- Articles (long-form) and documents (PDF) as post types

### Unique Components / Services
- **Espresso**: LinkedIn's proprietary MySQL-based distributed database for all core entities (posts, members, connections, reactions, comments); semi-synchronous replication; multi-master
- **Venice / Voldemort**: LinkedIn's internal key-value store for precomputed feed cache, PYMK recommendations, connection degree lookups, viral tracking
- **Apache Pinot**: real-time OLAP database; ingests from Kafka streams; sub-second queries on engagement signals; used for viral detection and feature serving
- **Galene**: LinkedIn's Lucene-based internal search engine (equivalent to Elasticsearch but custom)
- **Samza / Spark**: stream (Samza) and batch (Spark) jobs for connection degree pre-materialization; incremental O(degree) updates on new connections
- **PYMK Service**: nightly Spark job generating People You May Know recommendations; stored in Venice
- **Viral Amplification Service**: detects viral posts via Pinot query (reaction velocity + company/industry diversity); injects viral 2nd-degree posts at read time (not push fan-out)
- **Connection Degree Service**: computes 1st/2nd/3rd-degree relationships for privacy filtering; backed by Redis bloom filters and Venice materialized data

### Unique Data Models
- **Espresso posts**: virality_score (FLOAT), audience_type (ENUM: public, connections_only, custom), visibility_list (JSON), media_keys (JSON), content_type (text, article, photo, video, document, poll)
- **Espresso members**: headline, current_company_id, industry, seniority_level, browsing_mode, is_open_to_work
- **Venice feed cache**: key = member_id → JSON array of feed items (post_id, author_id, source_type, rank_score, engagement_score)
- **Pinot member_engagement_signals**: (member_id, action, post_id, ts) — real-time ingested from Kafka
- **Pinot post_virality_metrics**: (post_id, likes_1h, comments_1h, reposts_1h, company_diversity, industry_diversity) — used for viral detection
- **Redis bloom filter** per active member: 1st-degree membership filter (3 KB each × 300M active = 900 GB); 1% false positive rate for 2nd-degree checks
- **Connection degree** (Venice): `connection_degree:{viewer_id}:{author_id}` → degree (1/2/3/null)
- **PYMK** (Venice): `pymk:{member_id}` → list of (candidate_id, reason_string, score)
- 7-year privacy audit log in immutable WORM storage (enterprise compliance)

### Unique Scalability / Design Decisions
- **Bloom filter for 2nd-degree**: full materialization would require 48 trillion pairs (1B² / 2); bloom filter approximation (1% false positive, 3 KB each) makes this tractable
- **Viral injection at read time via Pinot**: avoids push fan-out to all 2nd-degree connections (would be 8.75B writes/sec peak); Pinot OLAP query detects viral posts in real time; read path injects them
- **Long engagement window (7 days)**: professional posts have week-long engagement lifecycles; contrast with Twitter's rapid decay
- **Request coalescing at Venice**: concurrent reads of the same viral post_id coalesced to reduce redundant computation
- **Fan-out capped at 1,000 Venice writes/sec per post_id** to prevent hot-post write storms
- **Rate limiting for viral write path**: max 1,000 Venice writes/second per post_id
- **Post-ranking multipliers for policy**: content category suppression (suppress political, personal content) applied as post-ranking score multipliers, not baked into model training — allows rapid policy changes without retraining

### Key Differentiator
LinkedIn's defining design challenge is **connection degree computation at scale** (1B members, 2nd-degree graph of up to 48 trillion pairs) and **professional context ranking** (content relevance is measured by company diversity, industry diversity, and seniority-weighted reactions rather than raw engagement) — both of which require LinkedIn-specific solutions (Espresso, Venice, Pinot, bloom filters) not needed in consumer social feeds.

---

## TikTok

### Unique Functional Requirements
- For You Page (FYP): recommendation-first feed of videos from accounts the user does not follow; personalization starts within 3–5 video interactions
- Video upload with captions, hashtags, sounds (audio tracks), and effects
- Duet: side-by-side reaction video referencing another video (duet_of_video_id)
- Stitch: cut into another video's clip as intro (stitch_of_video_id)
- Sounds/audio track system: track attribution, trending sounds, licensed audio
- Live streaming: RTMP ingest, LLHLS (Low-Latency HLS, 0.5s segments) delivery, gift economy
- Creator fund monetization: qualified_views-based daily earnings with immutable append-only ledger
- Family Pairing: parent-child account linking with content controls
- Screen time management (daily limit enforcement)

### Unique Components / Services
- **Video Processing Pipeline** (Kafka-triggered parallel DAG): Video Validator → [Transcoder (GPU), Thumbnail Generator, Feature Extractor] in parallel → CDN Uploader → TiKV Metadata Updater → Recommendation Indexer
- **TiKV** (RocksDB + Raft): distributed key-value store for video metadata and likes; Raft-based 3-way replication; leader election < 10s
- **FAISS (GPU-accelerated, in-memory)**: ANN retrieval over 128-dim user and video embeddings; nightly full rebuild + Redis buffer for videos uploaded in past 24h
- **Multi-task Neural Ranking Model**: jointly predicts watch completion %, like, comment, share, follow probability; ONNX format for online inference
- **ResNet-50 + VGGish**: ResNet for visual frame embeddings (128-dim), VGGish for audio embeddings; run on GPU workers during video processing
- **Flink Trending Pipeline**: counts hashtag/sound usage velocity from `interaction.event` stream; detects 3-sigma anomalies; manages trend lifecycle states (rising, peak, declining, archive); updates Redis every 60 seconds
- **Live Comment Service**: WebSocket cluster handling 1M+ concurrent connections for top creator streams; Redis Pub/Sub for comment fan-out
- **Gift Service**: processes gift events during Live streams; accumulates counts in Redis; renders real-time effects
- **BytePlus CDN**: ByteDance's own CDN infrastructure (1,000+ PoPs); pre-warms for predicted viral videos using ClickHouse velocity query
- **QUIC / HTTP3**: video delivery protocol for mobile optimization (reduces connection setup and handles packet loss better than TCP)
- **LLHLS**: Low-Latency HLS with 0.5-second segments for Live streaming; RTMP for broadcaster ingest

### Unique Data Models
- **videos** (TiKV/MySQL sharded by video_id): hls_manifest_key, thumbnail_key, qualities_keys (JSON for 240p/360p/480p/720p/1080p), visual_embedding (128-dim float32), audio_fingerprint (SHA-256), avg_watch_pct, replay_rate, processing_status (pending/processing/live/failed), region_restrictions (JSON), duet_of_video_id, stitch_of_video_id
- **sounds** (MySQL): sound_id, original_video_id, title, artist_name, is_licensed, audio_key, waveform_key, usage_count
- **interaction_events** (ClickHouse, append-only): event_type (play/pause/skip/like/comment/share/replay/save/follow_from_video/profile_click), watch_pct (0.0–1.0), source (fyp/following/search/trending), session_id; partitioned by month, TTL 365 days
- **family_pair** (MySQL): parent_id, child_id, created_at
- FYP cache: `fyp:{user_id}` (Redis Sorted Set, score = recommendation_score, 200 candidates, TTL 2h)
- Top creator followers: `top_creator_followers:{creator_id}` (Redis Set for accounts > 1M followers)
- Creator fund ledger: immutable append-only, daily aggregate qualified_views from ClickHouse

### Unique Scalability / Design Decisions
- **ClickHouse for interaction events at 578K events/sec**: columnar storage with 10:1 compression; ML training uses 365-day window; TTL partition-based cleanup; OLAP queries for viral detection (100K plays/1h + 0.7 completion rate + 3+ regions)
- **Progressive transcoding**: first 60 seconds of video go live within 30-second SLA while remaining transcoding completes; client pre-buffers 5 videos ahead and prefetches manifest + first 3 seconds of next video at 30% watch time
- **FAISS nightly rebuild**: full index rebuild nightly for optimal ANN quality; Redis buffer holds embeddings for videos uploaded in past 24h to bridge the gap
- **Adaptive exploration rate**: ε = 0.3 for first 50 interactions, ε = 0.5 for "break the loop" signal; prevents filter bubbles
- **Safety classification fast-path**: if frame 1 flagged with > 0.99 confidence, skip remaining frames (cost optimization)
- **7 interaction signal types**: play, pause, skip, like, comment, share, replay, save, follow_from_video, profile_click — richer signal than binary like/dislike; skip timing (2s vs. 15s) adds granular negative signal
- **Parallel microservice DAG for video processing**: transcoding, thumbnail generation, feature extraction run in parallel after validation; GPU batch inference at ~500 frames/sec per V100
- **Pre-signed S3 URLs for multipart video upload**: 30-minute expiry; no server bandwidth needed for video ingest
- **H.265/HEVC encoding**: 40% smaller than H.264 for popular videos; reduces CDN egress cost at scale
- **Dual geo-restriction enforcement**: CDN-level + API-level — defense-in-depth for legal compliance; prevents bypass via direct CDN URL

### Key Differentiator
TikTok's defining architecture is that the primary feed is **recommendation-first, not social-graph-first**: unlike all other problems in this folder where the feed is anchored in who you follow, TikTok's FYP is driven entirely by a multi-task neural ranking model over video embeddings, making the recommendation engine (two-tower FAISS retrieval + ONNX ranker) and the video processing pipeline (GPU transcoding + feature extraction + LLHLS delivery) the core technical challenges rather than the social graph or fan-out.
