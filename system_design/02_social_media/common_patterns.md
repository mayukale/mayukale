# Common Patterns — Social Media (02_social_media)

## Common Components

### Redis Cluster
- Used as: precomputed feed/timeline cache, post/tweet object cache, engagement counters, trending data, rate limiting, bloom filters
- In twitter: Sorted Set `timeline:{user_id}` (up to 800 tweet_ids, TTL 7 days), tweet object cache, like counters (INCR), trending sorted sets by region
- In instagram: feed cache `feed:{user_id}` (500 post_ids, TTL 3 days), post object cache, viewer counts, seen-post bloom filter
- In facebook_news_feed: friendship cache (5-min TTL), feature store hot features (5-min TTL), post object cache
- In linkedin_feed: connection degree cache (1-hour TTL), bloom filters for 1st-degree, idempotency keys, trending feeds (5-min refresh)
- In tiktok: FYP candidate cache `fyp:{user_id}` (200 candidates, TTL 2h), following feed cache (TTL 3 days), trending hashtags/sounds/videos, user embeddings

### Kafka (Event Bus)
- Used in all five problems as the async backbone decoupling writes from fan-out and analytics
- In twitter: topics `tweet.created`, `tweet.deleted`, `follow.event`, `fanout.tasks`; partitioned by `author_id`
- In instagram: topics `media.uploaded`, `post.created`, `story.created`, `follow.event`, `analytics.events`; partitioned by `media_id`
- In facebook_news_feed: topics `post.created`, `post.deleted`, `reaction.event`, `feed.fanout`, `social.edge.created`, `notification.event`, `realtime.feed.update`
- In linkedin_feed: topics `post.created`, `post.deleted`, `post.went_viral`, reaction/comment events; RF=3
- In tiktok: topics `video.uploaded`, `video.validated`, `video.processed`, `interaction.event`, `follow.event`, `fyp.feedback`, `video.trending`; dead-letter queue for failed events

### CDN
- Used in all five problems for media caching at the edge; TLS termination in some
- In twitter: Cloudflare / Akamai; `Cache-Control: max-age=31536000, immutable` for media
- In instagram: CloudFront + Fastly + Akamai (dual provider for redundancy); photos `max-age=86400`, immutable media `max-age=2592000`
- In facebook_news_feed: Facebook's own CDN; photos `max-age=86400`, recent uploads `max-age=3600`
- In linkedin_feed: Akamai / Azure CDN; profile photos `max-age=86400`, post media `max-age=2592000, immutable`
- In tiktok: Akamai / Cloudflare / BytePlus (own CDN); 1,000+ PoPs; `Cache-Control: immutable` for HLS segments; pre-warmed for predicted viral videos

### Object Storage (S3 / Equivalent)
- Used in all five for media blobs
- In twitter: S3/GCS for images and transcoded videos
- In instagram: S3 staging + production bucket; WebP format; pre-signed URLs for direct upload
- In facebook_news_feed: Haystack (proprietary) for photo/video blobs + S3 for audit logs (WORM)
- In linkedin_feed: Azure Blob Storage / S3
- In tiktok: S3 staging bucket + primary origin (us-east-1 + 4 replicas) for HLS segments, thumbnails, all quality variants; pre-signed URLs for multipart upload

### Fan-out Worker Service
- Used in all five problems; each implements hybrid push/pull fan-out
- In twitter: Fanout Worker consumes `tweet.created`; pushes to Redis timeline lists; celebrity threshold: 1M followers
- In instagram: Fan-out Worker consumes `post.created`; pushes post_id to Redis feed caches; celebrity threshold: 1M followers
- In facebook_news_feed: MultiFeed Fan-out Workers; pushes to Cassandra MultiFeed store; threshold: 100K followers for pages
- In linkedin_feed: Feed Fanout Workers; pushes to Venice feed cache; threshold: 5,000 connections or 100K followers for company pages
- In tiktok: Following Feed Fan-out Worker; pushes video_id to Redis following feed cache; threshold: 1M followers for top creators

### Precomputed Feed Cache per User
- All five store a ranked list of post/tweet/video IDs per user so the feed read path is a simple cache read
- Twitter: Redis Sorted Set (800 entries, 7-day TTL)
- Instagram: Redis Sorted Set (500 entries, 3-day TTL)
- Facebook: Cassandra `user_feed` table (5,000 entries)
- LinkedIn: Venice/Voldemort key-value store (full ranked feed JSON)
- TikTok: Redis Sorted Set for FYP (200 candidates, 2h TTL) and following feed (500 entries, 3-day TTL)

### Snowflake ID Generator
- Used in Twitter, Instagram, Facebook, and TikTok for globally unique, time-ordered 64-bit IDs
- Bit layout: [1 sign][41 ms timestamp][10 machine/datacenter][12 sequence]; epoch offset per service
- Eliminates centralized ID bottleneck; IDs are naturally sortable by creation time

### Elasticsearch (or Equivalent Full-Text Search)
- Used in Twitter, Instagram, TikTok (Elasticsearch), and LinkedIn (Galene — Lucene-based custom equivalent)
- Role: inverted index for full-text search on post bodies, hashtags, usernames, captions
- Twitter: 30-day rolling index with `like_count` and `retweet_count` fields for ranking
- Instagram: search on posts, hashtags, captions; also used by Explore
- LinkedIn: Galene indexes posts, profiles, companies
- TikTok: captions, hashtags, usernames, sound names; also semantic search via FAISS embeddings

## Common Databases

### MySQL (or MySQL-based distributed DB)
- Used in Twitter, Instagram, Facebook, LinkedIn for strongly consistent data (user profiles, follow graph, auth)
- Twitter: users + follows; sharded via Vitess by `user_id`
- Instagram: follow graph; sharded via Vitess by `follower_id`
- Facebook: social graph via TAO (MySQL-backed with Memcache caching layer); sharded via Vitess
- LinkedIn: Espresso (MySQL-based distributed DB) for all core entities (posts, members, connections, reactions, comments)

### Cassandra
- Used in Twitter, Instagram, and Facebook for write-heavy social data (posts, likes, comments, notifications) with tunable consistency
- Twitter: `tweets`, `tweets_by_user`, `notifications`; RF=3; QUORUM writes, ONE reads for timelines
- Instagram: posts, comments, likes, stories (with TTL); RF=3 multi-DC; LOCAL_QUORUM writes
- Facebook: MultiFeed precomputed feeds (user_id partitioned), reactions, comments; RF=3; LOCAL_QUORUM writes

### Redis (as primary cache store — see Common Components above)

### Object Storage (see Common Components above)

## Common Queues / Event Streams

### Kafka
- See Common Components above; universally RF=3
- Dead-letter queue pattern used in TikTok and Twitter for fan-out failures
- Kafka offset not committed until downstream operation succeeds (idempotent replay on crash)

### Apache Flink (Stream Processor)
- Used in Instagram, TikTok, and mentioned for Facebook/Twitter trending
- Role: sliding/tumbling window aggregation, trending detection (3-sigma spike), real-time feature computation
- Outputs to ClickHouse (TikTok/Instagram) or Redis trending sorted sets (all)

## Common Communication Patterns

### REST over HTTPS
- All five expose REST APIs for clients; HTTPS via CDN/API Gateway
- Standard CRUD on posts: POST /v1/posts (or /tweets or /videos), GET /v1/feed/home, POST/DELETE likes, etc.

### gRPC (Internal Services)
- LinkedIn and TikTok explicitly use gRPC between microservices with Protobuf; implied in others
- Trace context propagated via gRPC metadata

### WebSocket (Real-time)
- Facebook, Instagram, TikTok use WebSocket for real-time delivery (DMs, live comments, feed updates)
- Long-polling mentioned as fallback in Facebook and Instagram

### Hybrid Push-Pull Fan-out
- Universal pattern across all five: push fan-out for regular users, pull at read time for accounts above a follower/connection threshold
- Threshold varies: 1M followers (Twitter, Instagram, TikTok), 100K followers (Facebook pages), 5K connections (LinkedIn)
- Push path: write post_id to every follower's cached feed at write time
- Pull path: merge posts from high-follower accounts at read time by querying the author's post list

### Content-Addressed Immutable Media URLs
- All five use hash-based or version-tagged media keys in object storage + CDN
- Ensures CDN can cache indefinitely with `immutable` Cache-Control; avoids invalidation

## Common Scalability Techniques

### Hybrid Fan-Out (see above)

### Hash-Based Sharding on Primary Key
- All five shard their social graph (follows) and post stores by `user_id` or `author_id` or `post_id`
- Consistent hashing (Cassandra vnodes, Vitess) or modulo sharding with planned re-sharding

### Replication (Primary-Replica / Multi-DC)
- All use async replication across availability zones and regions
- Cassandra: NetworkTopologyStrategy RF=3; LOCAL_QUORUM writes, LOCAL_ONE reads
- MySQL: semi-synchronous replication; at least 1 replica acks before commit
- Multi-region setup for geo-locality and disaster recovery

### Circuit Breakers
- All five mention circuit breakers on at least their ML ranking, main database, and cache paths
- Fallback: chronological or trending feed when ranking service fails; empty or stale feed when DB unreachable

### Auto-Scaling on CPU and Latency
- All services are stateless and scale horizontally; ASG triggers on CPU > 60% or p99 latency spike

### ML-Based Feed Ranking (4/5 problems)
- Instagram: two-tower neural network + FAISS ANN + lightweight gradient-boosted ranker
- Facebook: GBDT pre-scorer (2,000 candidates) → Deep Cross Network neural ranker (500 candidates)
- LinkedIn: multi-stage cascade with professional relevance scoring and diversity enforcement
- TikTok: two-tower ANN candidate retrieval → multi-task neural ranker (jointly predicts watch%, like, comment, share)
- All use offline model retraining + online feature serving from a feature store (Redis, Pinot, Scuba)

## Common Deep Dive Questions

### How do you handle the celebrity/hotspot fan-out problem?
Answer: Hybrid push-pull. For accounts below the follower threshold, push post_id to every follower's precomputed feed cache at write time. Above the threshold, skip push fan-out; at read time, pull and merge posts from the author's post list. Threshold is tunable at runtime. This caps write amplification while keeping feed reads fast.
Present in: twitter, instagram, facebook_news_feed, linkedin_feed, tiktok

### How do you generate globally unique, time-ordered IDs at high throughput?
Answer: Snowflake IDs: 64-bit integer with 41-bit millisecond timestamp, 10-bit machine ID (assigned from ZooKeeper), 12-bit sequence counter. Produces up to 4,096 IDs/ms per machine, naturally sortable, no coordination needed at query time.
Present in: twitter, instagram, facebook_news_feed, tiktok

### How do you detect and surface trending topics/hashtags in near real-time?
Answer: Flink sliding window (5-minute window, 1-minute slide) counts hashtag frequency; velocity signal = current rate vs. rolling average, threshold at 3 sigma. Count-Min Sketch (Twitter) reduces memory from O(unique terms) to constant width×depth with ~1% error. Results written to Redis Sorted Set every 60 seconds.
Present in: twitter, tiktok, instagram (Explore)

### How do you ensure idempotency on feed writes?
Answer: Redis ZADD with same (member, score) is a no-op. DB inserts use `ON CONFLICT DO NOTHING` or `ON DUPLICATE KEY IGNORE`. Kafka offset committed only after downstream writes succeed; replay is safe. Optional `X-Idempotency-Key` header stored in Redis for 24h.
Present in: twitter, instagram, facebook_news_feed, linkedin_feed, tiktok

### How do you keep the feed fresh despite replication lag?
Answer: Write-through cache immediately after post creation. Fan-out Worker writes post_id to follower caches before Kafka offset is committed. For reads that miss the cache, fall back to the database with a read-replica that has < 100ms lag same-AZ.
Present in: twitter, instagram, facebook_news_feed, linkedin_feed, tiktok

### How do you handle multi-region active-active deployment?
Answer: GeoDNS routes each user to their nearest region. Cassandra or equivalent uses NetworkTopologyStrategy with async cross-DC replication (~100–200ms lag). EU data residency enforced by pinning EU users to EU clusters. Local quorum writes avoid cross-DC coordination overhead.
Present in: twitter, instagram, facebook_news_feed, linkedin_feed, tiktok

## Common NFRs

- **Availability**: 99.99% uptime across all five problems
- **Read-heavy workload**: Feed reads dominate; read:write ratio ranges from 20:1 to 100:1
- **Eventual consistency on feeds**: Posts appearing in follower feeds within seconds to minutes is acceptable; strong consistency only for unique entities (user accounts, follow edges)
- **Celebrity/viral content resilience**: System must handle accounts with millions of followers posting without manual intervention
- **Low feed read latency**: p99 targets range from 200ms (Twitter) to 500ms (Facebook, LinkedIn); all below 1 second
- **Durability**: Posts are durable once write is acknowledged; no silent data loss
- **Personalization**: Feeds are personalized (either by time or by ML ranking); not one-size-fits-all
