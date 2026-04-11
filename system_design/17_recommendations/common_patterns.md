# Common Patterns — Recommendations (17_recommendations)

## Common Components

### Multi-Stage Candidate Generation → Ranking → Filtering Pipeline
- All three systems use a funnel that narrows from a large candidate pool to a small ranked output — never run the expensive ranking model against every item
- recommendation_engine: 100 M items → 500 ANN candidates → DCN-v2 scoring → 20 returned
- collaborative_filtering: 100 M items → 500 FAISS ANN candidates (or pre-computed Redis ZSET) → dot-product scoring → 20 returned
- news_feed_ranking: social inbox (1,200) + trending (100) + interest-based ANN (100) + viral (50) = 1,500 candidates → 500 heuristic pre-filter → MTL DNN scoring → top-100 diversity-filtered → 25 returned per page

### Kafka for Engagement Event Ingestion
- All three accept feedback / interaction events asynchronously via Kafka; ingest returns 202 Accepted in < 50 ms with no synchronous write to the DB
- recommendation_engine: `raw.feedback`, `raw.impressions`, `raw.conversions` topics; 4 B events/day (~46 K RPS)
- collaborative_filtering: `cf.interactions.explicit` (ratings) and `cf.interactions.implicit` (behaviors) topics; 3 B events/day (~35 K RPS)
- news_feed_ranking: engagement events topic (impressions, clicks, dwell, reactions); 30 B events/day (~347 K RPS)
- Events are idempotent by event_id; duplicate event_id within 24 hours silently dropped

### Redis Feature Store for Hot-Path Embedding and Feature Serving
- All three cache user embeddings, item embeddings, and feature vectors in Redis for sub-millisecond serving latency during inference
- recommendation_engine: `emb:user:{user_id}` → HSET {vec: 1 KB binary, model_ver, ts}; `emb:item:{item_id}` → HSET; `feat:user:{user_id}` → HSET {clicks_7d, avg_watch_frac, ...}; Bloom filter `filter:user:{user_id}` for seen-item suppression (~10 KB per user)
- collaborative_filtering: `cf:user_factors:{domain}:{user_id}` → HSET {vec: 512 B binary, bias, ver}; `cf:item_sim:{domain}:{item_id}` → ZSET of similar item_id → similarity_score (TTL 25h); `cf:user_topn:{domain}:{user_id}` → ZSET top-100 pre-computed (TTL 2h)
- news_feed_ranking: `feed:inbox:{user_id}` → ZSET post_id → timestamp (TTL 7d); `feed:post:eng:{post_id}` → HSET engagement counters; `feed:seen:{user_id}` → Bloom filter post_id (TTL 30d); `feed:trending:global` → ZSET (TTL 1h)

### ClickHouse for Interaction Storage (Append-Only, Analytical)
- All three store interaction/event logs in ClickHouse for fast columnar aggregation queries used in feature computation and model training
- recommendation_engine: raw feedback events (4 B/day → 290 TB/year); 10–100× compression over row-store PostgreSQL; MergeTree partitioned by event_ts
- collaborative_filtering: 3 B events/day; ALS training data loaded via ClickHouse SQL query (`SELECT user_id, item_id, log1p(COUNT(*)) AS rating FROM interactions GROUP BY ...`); 100 B non-zero entries total
- news_feed_ranking: 30 B events/day (likes, comments, shares, clicks); MergeTree ordered by user_id for fast per-user scans; feeds CTR normalization pipeline

### Offline Batch Training + Online Serving Pattern
- All three train models offline (Spark + Airflow nightly) and serve from in-memory or Redis-cached artifacts with a freshness gap of up to 24 hours for batch components; real-time feature updates narrow the gap to 5 minutes via Flink stream processing
- recommendation_engine: nightly DCN-v2 retrain on 8× A100 80 GB GPUs (~6 hours/epoch); TF Serving for versioned model deployment with canary support
- collaborative_filtering: nightly ALS on 100 c5.2xlarge executors (2.5 hours for 100 B non-zero entries × 15 iterations); Spark writes user/item factors to S3 Parquet → Redis Loader job
- news_feed_ranking: nightly MTL DNN retrain + LightGBM GBDT pre-ranker; Flink updates engagement counters in Redis within 30 seconds of events arriving

### S3 + Delta Lake for Offline Feature Snapshots
- All three use S3 + Delta Lake (ACID transactions on Parquet) for training data and feature snapshots; point-in-time correct reads enable reproducible training runs; time-travel queries for debugging
- recommendation_engine: 290 TB/year interaction log; 10 GB model artifacts per version
- collaborative_filtering: 15 TB raw interaction log; 307 GB user+item factor tables per retrain
- news_feed_ranking: 36.5 TB/year post storage; 3 TB/year user engagement history

### TF Serving for Versioned Model Deployment
- recommendation_engine and news_feed_ranking use TF Serving for model inference with zero-downtime canary updates, gRPC proto interface, and GPU batching
- collaborative_filtering uses in-process NumPy vectorized dot products (no GPU required — 500 candidates × 128 factors is trivially fast on CPU)

### 202 Accepted Feedback Ingestion (Async, Idempotent)
- All three return 202 Accepted immediately after enqueueing to Kafka; no synchronous DB write on the ingest path; idempotent by event_id; downstream Kafka consumers handle storage asynchronously

## Common Databases

### Kafka
- All three; engagement event bus; decouples ingest from processing; provides replay; topics partitioned by user_id for session-level locality; RF=3

### Redis Cluster
- All three; hot-path feature store for embeddings, item similarities, and engagement counters; Bloom filter for seen-item suppression; trending ZSETs; write-through from batch pipeline

### ClickHouse
- All three; columnar storage for interaction logs and engagement events; vectorized aggregation for feature computation; 10–100× compression vs. row-store; MergeTree engine

### S3 + Delta Lake
- All three; offline training data, feature snapshots, and model artifacts; ACID Parquet transactions; time-travel for reproducibility; cost-effective at scale

### PostgreSQL
- All three; catalog metadata (users, items, experiments, operator configs); ACID for mutations; not on the real-time serving hot path

## Common Communication Patterns

### gRPC for Model Inference
- recommendation_engine and news_feed_ranking: TF Serving gRPC; batched inference; Protobuf serialization; versioned model management
- collaborative_filtering: in-process NumPy dot products (no external gRPC call)

### Flink for Real-Time Feature Updates
- All three use Flink (or Spark Streaming) to maintain streaming feature updates — engagement counters, trending signals, user recency features — within 5 minutes of event ingestion

## Common Scalability Techniques

### FAISS IVF_PQ In-Process Index for ANN Retrieval
- recommendation_engine and collaborative_filtering load FAISS in-process on serving pods; avoids network hop for candidate retrieval; dual-index pattern (main IVF_PQ + delta flat index for items < 24h old) handles real-time item additions

### Bloom Filter for Seen-Item Suppression
- All three maintain a per-user Bloom filter in Redis for fast suppression of already-seen/interacted items from recommendation results; false positives acceptable (item occasionally suppressed when it shouldn't be)

### Popularity Fallback for Cold-Start
- All three serve popularity-ranked items (`cf:popular:{domain}:global` / trending ZSETs) when a user has insufficient interaction history; collaborative_filtering blends CF with popularity for users with < 10 interactions

## Common Deep Dive Questions

### How do you handle cold-start for new users with no interaction history?
Answer: Three-pronged approach: (1) Onboarding quiz or implicit signals (geo, device, time of day) are used to initialize a user embedding from the centroid of similar-profile users; (2) Content-based proxy — item content embeddings from the item tower (text, category, tags) allow retrieval before interaction data exists; (3) Popularity-weighted trending items serve as the bottom-layer fallback. As a user accumulates interactions (threshold typically 10–20), the system transitions to personalized CF/two-tower retrieval. For new items: content embedding computed immediately on publication; item appears in ANN index within minutes; exploration boost multiplier guarantees early impressions.
Present in: recommendation_engine, collaborative_filtering, news_feed_ranking

### Why return 202 Accepted for feedback/interaction events instead of synchronous writes?
Answer: Interaction event volume (4–30 B/day) exceeds what a transactional database can absorb synchronously. Returning 202 after Kafka publish means ingest latency is O(Kafka) = < 10 ms rather than O(DB write) = 5–50 ms. The tradeoff is eventual consistency — a user's interaction may take up to 5 minutes to affect their recommendations, which is acceptable for recommendation systems. The event_id deduplication window (24 hours) prevents double-counting from client retries.
Present in: recommendation_engine, collaborative_filtering, news_feed_ranking

## Common NFRs

- **Ingest latency**: 202 Accepted < 50 ms P99 for all three
- **Serving latency**: P99 < 50–200 ms end-to-end (recommendation_engine < 150 ms; collaborative_filtering < 100 ms; news_feed_ranking < 200 ms)
- **Freshness**: batch model retrain every 24 hours; real-time feature updates within 5 minutes via Flink
- **Availability**: 99.99% with fallback to popularity-based ranking when ML models unavailable
- **Scale**: 100 M–2 B users, 100 M items, 1 M+ RPS serving (news_feed_ranking)
- **Privacy**: no PII in feature stores; user IDs pseudonymized; GDPR erasure within 30 days
