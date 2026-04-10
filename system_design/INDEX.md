# System Design Interview Preparation Library

**63 systems · 20 categories · Staff/Principal FAANG level**

---

## How to Use This Library

Each file follows a consistent 12-section format:
1. Requirement Clarifications
2. Users & Scale (with calculations)
3. High-Level Architecture (ASCII diagram)
4. Data Model (full schema)
5. API Design
6. Deep Dive: Core Components (algorithms + Q&A)
7. Scaling (sharding, replication, caching, CDN)
8. Reliability & Fault Tolerance
9. Monitoring & Observability
10. Trade-offs & Design Decisions Summary
11. Follow-up Interview Questions (15+ per system)
12. References & Further Reading

---

## Category Index

### 01 · URL Shortener
| File | Key Insight |
|------|-------------|
| [url_shortener.md](01_url_shortener/url_shortener.md) | Pre-generated short code pool in Redis (SPOP) eliminates collision risk at write time; 301 vs 302 redirect is a deliberate analytics trade-off |
| [pastebin.md](01_url_shortener/pastebin.md) | Content goes to S3 (not DB) — cost math shows $252/mo S3 vs $1,265/mo RDS for 11 TB; immutability enables aggressive CDN caching |

---

### 02 · Social Media
| File | Key Insight |
|------|-------------|
| [twitter.md](02_social_media/twitter.md) | Hybrid fan-out: pre-compute timelines for users < 1M followers (write), pull-on-read for celebrities — avoids both hotspot writes and read-time fan-in at scale |
| [instagram.md](02_social_media/instagram.md) | Stories use Cassandra TTL for automatic expiry; photo keys are content-addressed (hash-based) enabling global deduplication |
| [facebook_news_feed.md](02_social_media/facebook_news_feed.md) | TAO (MySQL + Memcache) powers the social graph; privacy filtering is fail-closed and runs after ranking to prevent information leakage |
| [linkedin_feed.md](02_social_media/linkedin_feed.md) | Viral amplification computed via Pinot OLAP rather than naive fan-out; connection-degree signals require Bloom filter computation at graph scale |
| [tiktok.md](02_social_media/tiktok.md) | FYP uses 3-stage cascade (two-tower retrieval → multi-task neural ranker → policy filters); cold start resolved in 3–5 interactions via exploration |

---

### 03 · Messaging
| File | Key Insight |
|------|-------------|
| [whatsapp.md](03_messaging/whatsapp.md) | Signal Protocol (X3DH + Double Ratchet) provides forward secrecy; group messages use Sender Keys so sender encrypts once regardless of group size |
| [slack.md](03_messaging/slack.md) | MySQL sharded by workspace_id keeps data co-located; Elasticsearch index per workspace with `member_ids` field for access-control-aware search |
| [discord.md](03_messaging/discord.md) | ScyllaDB (shard-per-core, no JVM GC) over Cassandra for message storage; 64-bit permission bitmask with versioned Redis cache for role resolution |
| [live_comments.md](03_messaging/live_comments.md) | Three-stage AutoMod (Bloom filter → rules → DeBERTa ML); fan-out math at 500K viewers requires tiered Chat Receive Server architecture |

---

### 04 · Video Streaming
| File | Key Insight |
|------|-------------|
| [youtube.md](04_video_streaming/youtube.md) | 2D parallelism in transcoding (N segments × Q qualities); recommendations use two-tower DNN with ScaNN ANN over 800M embeddings |
| [netflix.md](04_video_streaming/netflix.md) | Per-scene dynamic bitrate encoding (convex hull optimization) reduces bandwidth 20%; Open Connect CDN embeds NVMe servers inside ISPs |
| [twitch.md](04_video_streaming/twitch.md) | One dedicated GPU worker per live stream (not shared pool) to meet latency SLA; LHLS partial segments enable sub-3-second latency |
| [video_upload_pipeline.md](04_video_streaming/video_upload_pipeline.md) | TUS resumable upload protocol over S3 Multipart; DRM uses envelope encryption — segment key wrapped by KMS master key |

---

### 05 · Search
| File | Key Insight |
|------|-------------|
| [google_search.md](05_search/google_search.md) | WAND (Weak AND) algorithm prunes posting list traversal for top-K retrieval; PageRank uses power iteration with dangling node normalization |
| [typeahead_autocomplete.md](05_search/typeahead_autocomplete.md) | PATRICIA compressed trie pre-caches top-K at every node (O(L) lookup); real-time trending layer uses Kafka → Flink → Redis ZSET with EMA scoring |
| [elasticsearch.md](05_search/elasticsearch.md) | NRT indexing separates refresh (segment visibility) from flush (translog fsync) from commit; BM25 with per-field boosting; WAND-style DISI iteration for scoring |
| [tag_based_search.md](05_search/tag_based_search.md) | Roaring Bitmaps for mega-tags (millions of items); NPMI-scored co-occurrence matrix (Spark batch) powers related-tag suggestions |

---

### 06 · Storage
| File | Key Insight |
|------|-------------|
| [google_drive.md](06_storage/google_drive.md) | File chunked by content hash (deduplication); Zanzibar-inspired ReBAC for ACLs; hybrid push+cursor sync protocol handles offline-to-online transitions |
| [dropbox.md](06_storage/dropbox.md) | Block-level delta sync sends only changed blocks; LAN sync uses mDNS to avoid round-tripping to server; Magic Pocket uses erasure coding (not replication) |
| [s3_object_store.md](06_storage/s3_object_store.md) | Consistent hashing with virtual nodes for object placement; Reed-Solomon 6+3 erasure coding instead of 3× replication (2× storage savings); strong read-after-write consistency since 2020 |
| [photo_storage.md](06_storage/photo_storage.md) | SHA-256 content-addressable storage for deduplication; MTCNN + FaceNet + FAISS pipeline for face recognition; Cassandra TWCS for time-series photo feeds |

---

### 07 · Ride Sharing
| File | Key Insight |
|------|-------------|
| [uber_lyft.md](07_ride_sharing/uber_lyft.md) | H3 hexagonal cells (resolution 7, ≈5 km²) for surge pricing with EMA smoothing; trip state machine enforced via PostgreSQL triggers |
| [location_tracking.md](07_ride_sharing/location_tracking.md) | Two-tier write path: Redis GEOADD (synchronous, < 1 ms) + Kafka (async, durable); adaptive sampling reduces GPS updates by 40–50% based on speed+state |
| [driver_matching.md](07_ride_sharing/driver_matching.md) | Normalized scoring function (60% distance, 20% rating, 10% acceptance, 10% heading); tiered waterfall: 8-second exclusive offer before parallel broadcast |
| [eta_service.md](07_ride_sharing/eta_service.md) | Contraction Hierarchies (< 5 ms query) with dynamic traffic weight overlay; XGBoost correction model uses quantile regression for confidence intervals |

---

### 08 · Food Delivery
| File | Key Insight |
|------|-------------|
| [doordash.md](08_food_delivery/doordash.md) | Self-hosted OSRM ($840/mo) vs Google Maps API ($240K/mo) at scale; transactional outbox pattern (Debezium CDC → Kafka) for order state changes |
| [menu_search.md](08_food_delivery/menu_search.md) | ES `function_score` with Gaussian distance decay + log-scaled review count; menu changes propagate end-to-end in 2–3 seconds via Kafka → partial ES update |
| [order_tracking.md](08_food_delivery/order_tracking.md) | WebSocket for full-duplex with Redis Pub/Sub for cross-node fan-out; client-side dead reckoning between server updates for smooth 30 FPS map animation |

---

### 09 · E-Commerce
| File | Key Insight |
|------|-------------|
| [amazon_product_page.md](09_ecommerce/amazon_product_page.md) | PDP SSR with parallel fan-out to 8 services + per-service circuit breakers + graceful partial-page fallback; TimescaleDB hypertable for price history |
| [shopping_cart.md](09_ecommerce/shopping_cart.md) | DynamoDB Global Tables for active-active multi-region carts; `TransactWriteItems` for atomic cart merge; Redis Sorted Set + Lua for soft inventory holds |
| [flash_sale.md](09_ecommerce/flash_sale.md) | Redis single-node Lua script for atomic inventory DECR (provably no TOCTOU); virtual queue via Redis Sorted Set (epoch_microseconds as score); reconciliation job every 5 minutes |

---

### 10 · Payments
| File | Key Insight |
|------|-------------|
| [payment_processing.md](10_payments/payment_processing.md) | Two-phase auth+capture with optimistic locking (`version` column); idempotency via Redis SETNX + Postgres fallback; HSM key hierarchy (MK → KEK → DEK) for PCI DSS |
| [stripe_gateway.md](10_payments/stripe_gateway.md) | Webhook delivery via transactional outbox → Kafka → workers with exponential backoff (10 attempts, 72-hour window); network tokenization via VTS/MDES |
| [wallet.md](10_payments/wallet.md) | Single-SQL atomic balance decrement (`UPDATE WHERE balance >= amount`); cross-shard transfers use clearing account saga pattern |
| [ledger.md](10_payments/ledger.md) | Double-entry bookkeeping with `CHECK (total_debit = total_credit)` at DB layer; SHA-256 hash chain with Merkle checkpoints for tamper evidence; INSERT-only grants for immutability |

---

### 11 · Notifications
| File | Key Insight |
|------|-------------|
| [push_notification_service.md](11_notifications/push_notification_service.md) | HTTP/2 multiplexing (1,000 concurrent streams per APNs connection); sharded Kafka fan-out completes 500M device broadcast in < 10 minutes |
| [email_delivery.md](11_notifications/email_delivery.md) | Hybrid in-house SMTP + vendor fallback (9× cost savings at scale); Redis Bloom filter (286 MB for 100M addresses at 0.01% FPR) for suppression list |
| [sms_gateway.md](11_notifications/sms_gateway.md) | SMPP v3.4 persistent sessions with per-carrier connection pools; adaptive carrier routing via DLR feedback loop (Flink → Redis quality scores) |

---

### 12 · Maps & Location
| File | Key Insight |
|------|-------------|
| [google_maps.md](12_maps_location/google_maps.md) | Contraction Hierarchies with bidirectional Dijkstra for < 5 ms routing; Flink HMM map-matching converts raw GPS probes to edge speeds |
| [proximity_service.md](12_maps_location/proximity_service.md) | Geohash inverted index with cell-transition events (1M/s updates → 42K/s index writes); antimeridian and polar edge-case handling documented |
| [yelp_nearby.md](12_maps_location/yelp_nearby.md) | Incremental O(1) rating update formula avoids full recalculation; ES `function_score` with Gaussian distance decay + log-scaled review count |
| [geospatial_index.md](12_maps_location/geospatial_index.md) | Deep dive: geohash bit-interleaving, full quadtree with K-NN branch-and-bound, S2 Hilbert curve outperforms Z-order (no cross-shaped discontinuities) |

---

### 13 · Rate Limiting
| File | Key Insight |
|------|-------------|
| [api_rate_limiter.md](13_rate_limiting/api_rate_limiter.md) | Sliding Window Counter (O(1), < 1% error) selected over Sliding Window Log (perfect but O(N) memory); Redis Lua script for atomic distributed enforcement |
| [throttling_service.md](13_rate_limiting/throttling_service.md) | AIMD + EWMA adaptive throttling (more stable than PID); Google's client-side accept-rate throttle as zero-control-plane fallback; multi-level priority queue with starvation prevention |

---

### 14 · Distributed Systems
| File | Key Insight |
|------|-------------|
| [distributed_cache.md](14_distributed_systems/distributed_cache.md) | Redis Cluster uses 16,384 hash slots; MOVED vs ASK protocol for live resharding; RDB uses fork+COW for non-blocking snapshots |
| [distributed_message_queue.md](14_distributed_systems/distributed_message_queue.md) | Exactly-once semantics requires idempotent producer + transactional 2PC + zombie fencing; High Watermark vs LEO vs committed offset distinction is critical |
| [distributed_lock.md](14_distributed_systems/distributed_lock.md) | Fencing tokens (etcd cluster revision) make locks safe despite GC pauses; Redlock is unsafe under clock drift + GC — Kleppmann's critique is well-founded |
| [distributed_job_scheduler.md](14_distributed_systems/distributed_job_scheduler.md) | `SELECT FOR UPDATE SKIP LOCKED` for exactly-once trigger firing; Kahn's topological sort with fulfillment counters for DAG dependency resolution |
| [consistent_hashing.md](14_distributed_systems/consistent_hashing.md) | Virtual nodes (vnodes) reduce std deviation of load from O(1/N) to O(1/√(NV)); Jump Consistent Hash is O(ln N) and stateless but doesn't support arbitrary node removal |

---

### 15 · Analytics
| File | Key Insight |
|------|-------------|
| [ad_click_aggregation.md](15_analytics/ad_click_aggregation.md) | Two-level dedup: Bloom filter (240 MB, probabilistic) + exact Redis SET (deterministic); Lambda architecture chosen over Kappa for retroactive fraud adjustment |
| [web_crawler.md](15_analytics/web_crawler.md) | Mercator two-level URL Frontier (back queues per domain + Redis readiness sorted set); SimHash Hamming distance via 4×16-bit segment table decomposition for O(1) near-dup |
| [metrics_logging_system.md](15_analytics/metrics_logging_system.md) | HyperLogLog estimators (12 KB each) prevent cardinality explosion; VictoriaMetrics over Prometheus+Thanos (5× write throughput, native clustering) |
| [realtime_leaderboard.md](15_analytics/realtime_leaderboard.md) | Score-range bucketed ZSETs with `cumulative_above` index for O(log N/B) global rank — single monolithic ZSET cannot be sharded in Redis Cluster |

---

### 16 · Auth & Security
| File | Key Insight |
|------|-------------|
| [auth_service.md](16_auth_security/auth_service.md) | Argon2id (memory-hard) over bcrypt; refresh token family-based reuse detection with SERIALIZABLE transaction; TOTP replay prevention via Redis 90s TTL code cache |
| [oauth2_provider.md](16_auth_security/oauth2_provider.md) | Authorization codes enforced single-use via atomic Redis GETDEL; opaque tokens + Redis write-through cache for 57K RPS introspection; Phantom Token pattern |
| [api_key_management.md](16_auth_security/api_key_management.md) | 32-byte CSPRNG → base64url; 8-char prefix reveals only 48 bits leaving 208 bits unguessable; canary tokens detect DB exfiltration; GitHub Secret Scanning partner integration |
| [sso.md](16_auth_security/sso.md) | XSW attack defense: validate signature → extract data by signed element ID (never XPath position); OIDC back-channel logout (RFC 9099) more reliable than SAML SLO |

---

### 17 · Recommendations
| File | Key Insight |
|------|-------------|
| [recommendation_engine.md](17_recommendations/recommendation_engine.md) | Two-tower FAISS IVF_PQ (3.2 GB, recall@100=98.3%) for candidate gen; DCN-v2 ranker for feature crossing; Thompson Sampling bandit for cold start |
| [collaborative_filtering.md](17_recommendations/collaborative_filtering.md) | User-user CF infeasible at 500M users (3 PB similarity table); item-item pre-computed (240 GB Redis) + ALS matrix factorization via distributed Spark; LSH for approximate item similarity |
| [news_feed_ranking.md](17_recommendations/news_feed_ranking.md) | 5-stage pipeline: candidate gen → feature assembly → multi-task DNN → filtering → diversity+boost; Wilson score for CTR normalization; IPS correction for position bias |

---

### 18 · Live & Real-time
| File | Key Insight |
|------|-------------|
| [live_streaming.md](18_live_realtime/live_streaming.md) | RTMP ingest → GPU NVENC transcoder pods → HLS segments on S3 → CDN with manifest pre-push; HyperLogLog for viewer counting; request coalescing prevents CDN origin storms |
| [stock_ticker.md](18_live_realtime/stock_ticker.md) | DPDK kernel bypass for ITCH feed parsing (zero-copy C++); all prices in fixed-point int64 (no float rounding); 5ms last-write-wins batching before WebSocket push |
| [multiplayer_game_backend.md](18_live_realtime/multiplayer_game_backend.md) | Authoritative server at 64 Hz tick; lag compensation rewinds game state to shooter's perceived time; UDP with application-layer reliability for < 50 ms RTT |
| [collaborative_doc.md](18_live_realtime/collaborative_doc.md) | CRDTs (Yjs YATA) over OT for offline support and multi-server deployment; presence decoupled from ops (100ms Redis pub/sub batching); op log + S3 snapshots (not per-edit snapshots — 2000× cheaper) |

---

### 19 · DevOps & Infrastructure
| File | Key Insight |
|------|-------------|
| [cicd_pipeline.md](19_devops_infra/cicd_pipeline.md) | Redis ZSET + Lua atomic script for job deduplication at 37,500 concurrent agents; historical duration bin-packing (LPT heuristic) for test parallelization |
| [feature_flag_service.md](19_devops_infra/feature_flag_service.md) | 10B daily evaluations are in-process (zero network hops); MurmurHash3 (not MD5) for bucketing uniformity; SSE for server SDKs, 30s polling for mobile (battery trade-off) |
| [config_management.md](19_devops_infra/config_management.md) | PostgreSQL over etcd (etcd caps at 8 GB); `vault://` references — config service never holds plaintext secrets; RWLock + atomic pointer swap for zero-blocking hot reload |
| [ab_testing_platform.md](19_devops_infra/ab_testing_platform.md) | mSPRT solves the peeking problem (Ville's inequality); CUPED variance reduction cuts required sample size 20–40%; Bloom filter dedup (not Redis SET) at 165M events/day |

---

### 20 · Miscellaneous
| File | Key Insight |
|------|-------------|
| [ticket_booking.md](20_misc/ticket_booking.md) | Redis SETNX + `SELECT FOR UPDATE` dual-layer seat locking; virtual waiting room queue prevents oversell during flash sales |
| [hotel_booking.md](20_misc/hotel_booking.md) | Date-range availability via PostgreSQL aggregate + partition pruning; overbooking handled airline-style with walkout detection and compensation |
| [calendar_service.md](20_misc/calendar_service.md) | RFC 5545 recurring events: master event + RRULE + exception model (not materialized); Redis sorted-set free/busy cache for conflict detection |
| [code_deployment.md](20_misc/code_deployment.md) | Redis SETNX + PostgreSQL uniqueness as dual-layer deployment lock; all four rollout strategies (blue-green/canary/rolling/recreate) with pseudocode |

---

## Cross-Reference: Shared Patterns

### ID Generation
Systems that use the same **Snowflake ID** pattern (timestamp + datacenter + machine + sequence):
- [twitter.md](02_social_media/twitter.md), [discord.md](03_messaging/discord.md), [youtube.md](04_video_streaming/youtube.md)

Systems that use **pre-generated key pools** (Redis SPOP):
- [url_shortener.md](01_url_shortener/url_shortener.md), [pastebin.md](01_url_shortener/pastebin.md)

---

### Fan-Out Pattern
Systems with **fan-out on write** (push to followers):
- [twitter.md](02_social_media/twitter.md) (hybrid), [instagram.md](02_social_media/instagram.md) (hybrid), [facebook_news_feed.md](02_social_media/facebook_news_feed.md), [news_feed_ranking.md](17_recommendations/news_feed_ranking.md)

Systems with **message fan-out** (chat/notifications):
- [whatsapp.md](03_messaging/whatsapp.md), [slack.md](03_messaging/slack.md), [discord.md](03_messaging/discord.md), [live_comments.md](03_messaging/live_comments.md), [push_notification_service.md](11_notifications/push_notification_service.md)

---

### Geospatial Indexing
All these systems use geohash / S2 / H3 for proximity queries:
- [uber_lyft.md](07_ride_sharing/uber_lyft.md) (H3), [location_tracking.md](07_ride_sharing/location_tracking.md) (H3), [proximity_service.md](12_maps_location/proximity_service.md) (geohash), [yelp_nearby.md](12_maps_location/yelp_nearby.md) (geohash), [geospatial_index.md](12_maps_location/geospatial_index.md) (deep dive), [doordash.md](08_food_delivery/doordash.md) (H3)

---

### Distributed Locking
Systems that implement distributed locks for critical sections:
- [flash_sale.md](09_ecommerce/flash_sale.md) (Redis Lua), [ticket_booking.md](20_misc/ticket_booking.md) (Redis SETNX + PG FOR UPDATE), [distributed_lock.md](14_distributed_systems/distributed_lock.md) (etcd fencing tokens), [code_deployment.md](20_misc/code_deployment.md) (dual-layer), [driver_matching.md](07_ride_sharing/driver_matching.md) (CAS assignment)

---

### Idempotency & Exactly-Once
Systems with critical idempotency requirements:
- [payment_processing.md](10_payments/payment_processing.md), [stripe_gateway.md](10_payments/stripe_gateway.md), [ledger.md](10_payments/ledger.md), [distributed_message_queue.md](14_distributed_systems/distributed_message_queue.md), [distributed_job_scheduler.md](14_distributed_systems/distributed_job_scheduler.md), [wallet.md](10_payments/wallet.md)

---

### Content-Based Deduplication (Hashing)
Systems that use content hashes for deduplication:
- [google_drive.md](06_storage/google_drive.md), [dropbox.md](06_storage/dropbox.md), [photo_storage.md](06_storage/photo_storage.md), [pastebin.md](01_url_shortener/pastebin.md), [web_crawler.md](15_analytics/web_crawler.md) (SimHash for near-dup), [video_upload_pipeline.md](04_video_streaming/video_upload_pipeline.md) (per-chunk SHA-256)

---

### Write-Heavy Time-Series Storage
Systems using Cassandra TWCS or columnar stores for time-series:
- [location_tracking.md](07_ride_sharing/location_tracking.md), [photo_storage.md](06_storage/photo_storage.md), [slack.md](03_messaging/slack.md), [discord.md](03_messaging/discord.md), [metrics_logging_system.md](15_analytics/metrics_logging_system.md), [stock_ticker.md](18_live_realtime/stock_ticker.md)

---

### Recommendation / Ranking Pipeline
Systems sharing the **candidate generation → scoring → filtering → ranking** pattern:
- [tiktok.md](02_social_media/tiktok.md), [youtube.md](04_video_streaming/youtube.md), [netflix.md](04_video_streaming/netflix.md), [recommendation_engine.md](17_recommendations/recommendation_engine.md), [news_feed_ranking.md](17_recommendations/news_feed_ranking.md), [menu_search.md](08_food_delivery/menu_search.md), [yelp_nearby.md](12_maps_location/yelp_nearby.md)

---

### Rate Limiting (Shared Infrastructure)
These systems all depend on the rate limiting patterns in `13_rate_limiting/`:
- [api_rate_limiter.md](13_rate_limiting/api_rate_limiter.md), [throttling_service.md](13_rate_limiting/throttling_service.md)
- Consumers: every public API system (URL shortener, Twitter, Stripe, Google Search, etc.)

---

### CRDT / OT for Conflict Resolution
- [collaborative_doc.md](18_live_realtime/collaborative_doc.md) — Yjs YATA CRDT with full pseudocode
- [google_drive.md](06_storage/google_drive.md) — operational transform for sync conflicts
- [dropbox.md](06_storage/dropbox.md) — conflict copy model

---

### OAuth2 / Auth Dependencies
Every system with user-facing APIs depends on the auth patterns in `16_auth_security/`. Cross-reference:
- Token issuance: [auth_service.md](16_auth_security/auth_service.md)
- Third-party login: [oauth2_provider.md](16_auth_security/oauth2_provider.md)
- Enterprise SSO: [sso.md](16_auth_security/sso.md)
- API keys for B2B: [api_key_management.md](16_auth_security/api_key_management.md)

---

## Quick Reference: Technology Decisions

| Technology | Used In |
|------------|---------|
| **Redis ZSET** | Leaderboard, rate limiter, proximity, typeahead, driver matching, job scheduler |
| **Kafka** | Nearly every system with async decoupling or event streaming |
| **Cassandra / ScyllaDB** | WhatsApp, Slack, Discord, location tracking, photo storage, Netflix |
| **Elasticsearch** | Slack search, Yelp, menu search, tag search, web crawler |
| **Flink** | Ad click aggregation, stock ticker, location tracking, web crawler |
| **FAISS / ANN** | Recommendation engine, collaborative filtering, TikTok FYP, YouTube |
| **Redis Lua scripts** | Rate limiter, flash sale, slow mode, soft inventory holds |
| **PostgreSQL** | URL shortener, payments, auth, ticket/hotel booking |
| **ClickHouse / columnar** | Ad analytics, A/B testing, TikTok events |
| **S3 + CDN** | Video streaming, photo storage, file storage, pastebin |
| **WebSocket** | WhatsApp, Slack, Discord, live comments, order tracking, stock ticker |
| **gRPC** | Internal service-to-service (most microservice systems) |
| **etcd / ZooKeeper** | Distributed lock (etcd preferred for fencing tokens) |

---

*63 files · ~5,000–8,000 lines each · Generated for Staff/Principal-level interview preparation*
