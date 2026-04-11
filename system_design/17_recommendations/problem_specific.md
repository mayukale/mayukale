# Problem-Specific Design — Recommendations (17_recommendations)

## Recommendation Engine

### Unique Functional Requirements
- Personalized ranked recommendations across multiple surfaces (homepage, sidebar, email, push, widgets) via unified API
- Multi-surface serving at 500,000 RPS peak (prime-time streaming platform scale)
- Diversity controls: operator-configurable fraction of underexplored categories; Maximal Marginal Relevance (MMR) applied post-ranking
- A/B experimentation: configurable traffic split to experimental models without code deploy

### Unique Components / Services
- **Two-Tower Embedding Model**: User tower [Dense 512 → ReLU → Dense 256 → L2 Normalize] + Item tower [Dense 512 → ReLU → Dense 256 → L2 Normalize]; 256-dim output; in-batch softmax loss: `L = -1/B × Σᵢ log(exp(uᵢ·vᵢ/τ) / Σⱼ exp(uᵢ·vⱼ/τ))` where τ=0.07; generates `emb:user:{user_id}` and `emb:item:{item_id}` in Redis
- **FAISS IVF_PQ Index**: `IndexIVF_PQ` with nlist=4096 Voronoi cells, M=32 PQ sub-spaces, nbits=8; 100 M items × 32 bytes/PQ code = 3.2 GB per pod; nprobe=64 → recall@100 = 98.3%, P99 < 8 ms; nightly 4-hour rebuild on 32 CPU cores; delta flat index for items published < 24 hours ago
- **DCN-v2 Ranking Model**: Cross Network (K=6 layers: `xₖ₊₁ = x₀ × W xₖᵀ + bₖ + xₖ`) + Deep Network [512 → 256 → 128 → 64, ReLU + BatchNorm]; 150 total features (50 user + 60 item + 40 cross); label: watch_fraction > 0.5 OR explicit like; 4 B positives + 40 B sampled negatives per epoch; Adam lr=1e-4, batch_size=4096; 8× A100 80 GB, ~6 hours/epoch
- **MMR Diversity**: post-ranking selection on top-50 re-ranked items: `MMR_score(i) = λ × relevance(i) - (1-λ) × max_{j∈selected} sim(i,j)`; λ=0.8 for search sidebar, λ=0.6 for homepage; category entropy tracked as A/B diversity metric
- **Served Recommendation Logger**: every response logged with model_version, feature snapshot, experiment_bucket, rec_token for offline analysis; 600 M calls/day × 500 B = 300 GB/day

### Unique Data Model
- **items**: item_id, item_type, category_l1/l2, tags TEXT[], creator_id, age_restricted, geo_restrictions TEXT[], spam_score, avg_rating, freshness=1/(1+age_hours/48)
- **user_item_filters**: user_id, item_id, reason (not_interested/already_purchased/blocked) — drives Bloom filter updates within 30 s of explicit signal
- **experiment_assignments**: experiment_id, user_id, bucket_name, assigned_at — supports multi-model A/B without code deploy
- **Redis**: `filter:user:{user_id}` → Bloom filter ~10 KB per user for seen-item suppression; `trending:global` / `trending:cat:{cat_id}` → ZSET TTL 1h (Flink-maintained)
- **S3 + Delta Lake**: 290 TB/year interaction log; 500 GB user embeddings; 100 GB item embeddings

### Algorithms

**Two-Tower candidate retrieval pseudocode:**
```python
def retrieve_candidates(user_id, surface, n_candidates=500):
    user_vec = redis.hget(f"emb:user:{user_id}", "vec")  # 1 KB binary
    if user_vec is None:
        user_vec = get_cold_start_vector(user_id)         # category centroid
    query = np.frombuffer(user_vec, dtype=np.float32)
    distances, item_ids = faiss_index.search(query.reshape(1,-1), n_candidates)
    trending_ids = redis.zrevrange("trending:global", 0, 50)
    item_ids = merge_with_dedup(item_ids[0], trending_ids, max_total=n_candidates)
    return item_ids  # ~500 candidates
```

### Key Differentiator
Recommendation Engine's uniqueness is its **Two-Tower + FAISS IVF_PQ + DCN-v2 serving stack**: in-batch softmax (τ=0.07) Two-Tower model generates 256-dim embeddings (500 GB user + 100 GB item); FAISS IVF_PQ (nlist=4096, M=32, nbits=8, nprobe=64) gives recall@100=98.3% in < 8 ms from 3.2 GB in-process index; DCN-v2 with K=6 cross layers explicitly models 150-feature interaction space in 30 ms; MMR with configurable λ per surface controls exploration-exploitation; dual delta-index pattern handles new items within minutes of publication.

---

## Collaborative Filtering

### Unique Functional Requirements
- Item-based CF with human-readable explanations ("Because you liked X") — pre-computed item-item similarity
- ALS Matrix Factorization for holistic user-item latent factor scoring
- Multi-domain support: movies, books, music share infrastructure with domain-isolated models
- Explicit (star ratings 1–5) and implicit (views/clicks/purchases) feedback handled as separate model variants

### Unique Components / Services
- **ALS Matrix Factorization**: `R ≈ PQᵀ + bᵤ + bᵢ + μ`; rank F=128, regularization λ=0.1, implicitPrefs=True, α=40 (confidence `cᵤᵢ = 1 + 40 × fᵤᵢ`); maxIter=15; Spark MLlib ALS on 100 c5.2xlarge executors; 100 B non-zero entries × 15 iterations = 2.5 hours; convergence: RMSE 1.45 → 0.81 by iteration 15; ALS update rule: `pᵤ = (QᵀQ + λI)⁻¹ Qᵀ rᵤ`
- **Item-Item Similarity Pre-computation**: top-200 similar items per item; 100 M items × 2.4 KB/item = 240 GB stored in Redis Cluster as `cf:item_sim:{domain}:{item_id}` → ZSET (TTL 25h); cosine similarity over ALS factor vectors; serving: O(1) Redis ZSET lookup
- **User-User CF Decision**: pre-computing 500 M × 500 top neighbors = 3 PB — infeasible; decision: user-user CF is served at query-time via ALS factor dot product using FAISS over the 51 GB item factor matrix; only item-item is pre-computed
- **Serving Pseudocode**: `predicted = μ + bᵤ + bᵢ + pᵤ · qᵢ`; item factors fetched from Redis in batch; vectorized dot products (500 candidates × 128 dims) via NumPy; result cached in `cf:user_topn:{domain}:{user_id}` ZSET TTL 2h
- **Streaming ALS Update**: experimental; for power users, re-computes user factor `pᵤ_new = (QᵀQ + λI)⁻¹ Qᵀ rᵤ_updated` in Flink every 30 min if > 5 new interactions; pushes updated vector to Redis within 5 min; item factors remain fixed between nightly retrains

### Unique Data Model
- **Redis Key Patterns**: `cf:item_sim:{domain}:{item_id}` → ZSET top-200 similar items; `cf:user_factors:{domain}:{user_id}` → HSET {vec 512B, bias, ver}; `cf:item_factors:{domain}:{item_id}` → HSET; `cf:user_history:{domain}:{user_id}` → ZSET item_id → timestamp top-50 (TTL 1h); `cf:popular:{domain}:global` → ZSET for cold-start
- **Item-item similarity**: 240 GB in Redis Cluster (100 M items × 200 neighbors × 12 B/entry); re-computed nightly by Spark Similarity Pipeline
- **User MF factors**: 256 GB (500 M users × 512 B); item MF factors: 51 GB (100 M items × 512 B)
- **Interaction matrix**: 1.2 TB sparse CSR (12 B/entry × 100 B non-zero)
- **cf.interactions.explicit** / **cf.interactions.implicit**: separate Kafka topics; separate ALS model variants per domain

### Key Differentiator
Collaborative Filtering's uniqueness is its **ALS implicit feedback (α=40, c=1+40f) + 240 GB pre-computed item-item Redis ZSETs**: Hu et al. (2008) confidence weighting distinguishes strong positives (heavily consumed) from weak negatives (unobserved); F=128 rank converges in 15 iterations / 2.5 hours on 100 executors; item-item top-200 pre-computation (240 GB) yields sub-millisecond serving with explainable "Because you liked X" reason; user-user CF served via ALS factors to avoid infeasible 3 PB similarity table; streaming ALS update re-computes individual user vectors in Flink without full retrain.

---

## News Feed Ranking

### Unique Functional Requirements
- 1 M feed requests per second at peak (Facebook/2 B user scale); P99 < 200 ms end-to-end
- New posts from followed accounts must appear in feed within 30 seconds of publishing; viral engagement signals reflected within 5 minutes
- Push-on-write fan-out for accounts with < 10 K followers; pull-on-read for accounts > 10 K followers (celebrity path)
- Diversity enforcement: max 3 consecutive posts from same author, max 5 per author in top-25, minimum 20% non-followed-account posts

### Unique Components / Services
- **5-Stage Pipeline**:
  - Stage 1: Candidate Generation — social inbox Redis ZSET (1,200 posts last 72h) + trending injection (100) + ANN interest-based (100) + viral non-followed (50) = 1,500 total
  - Stage 2: Feature Assembly — 80 post features (engagement velocity, spam_score, freshness, author quality, social context) + 30 user features + 10 cross features = 120 total
  - Stage 3: Heuristic pre-filter (1,500 → 500): remove spam_score > 0.7, age > 72h AND velocity < 0.05, blocked/muted authors; then MTL DNN ranking on 500
  - Stage 4: Filtering — seen Bloom filter, spam/clickbait secondary thresholds, not_interested suppression, geo restrictions
  - Stage 5: Diversity enforcement + author/freshness boosts (verified × affinity > 0.7: +0.05; age < 30 min: +0.03; ≥ 3 friends engaged: +0.04)
- **Multi-Task Learning DNN**: shared bottom + task towers for CTR, share rate, comment rate, dwell time; engagement score formula uses weights: share=5×, comment=3×, like=1×, skip=-0.5×, report=-5×; LightGBM GBDT used as fast pre-ranker (2–5 ms CPU) for 1,500→500 pruning before DNN
- **CTR Normalization — Wilson Score**: `lower_bound = (p̂ + z²/2n - z√(p̂(1-p̂)/n + z²/4n²)) / (1 + z²/n)` where z=1.96; prevents low-sample posts from gaming CTR metric
- **Freshness Decay**: `freshness_score = 1 / (1 + age_minutes)`; age < 30 min gets +0.03 additive ranking boost
- **Fan-out Architecture**: push-on-write for ≤ 10K followers → writes to `feed:inbox:{follower_id}` Redis ZSET at publish time; pull-on-read for > 10K followers → `feed:celebrity_posts:{celebrity_id}` ZSET checked on each feed request; threshold configurable
- **Viral Detection**: Flink maintains 1-hour sliding window of engagement_velocity; posts crossing top-0.01% threshold force-added to `feed:trending:{scope}` ZSET; share events trigger secondary fan-out of original post to sharer's followers

### Unique Data Model
- **posts** (Cassandra, partition key = post_id): post_id, author_user_id, content_type, media_refs, topic_tags[], spam_score, clickbait_score; 100 M posts/day, Cassandra handles 1,157 writes/s
- **feed:inbox:{user_id}** → ZSET post_id → publish_timestamp (TTL 7d); 500 M active users × 100 candidates × 8 bytes = 400 GB total
- **feed:post:eng:{post_id}** → HSET {like_count, share_count, comment_count, click_count, ctr_1h, share_rate_1h, engagement_velocity} (TTL 72h); updated by Flink INCR within 30 s
- **Social graph**: MySQL sharded by follower_user_id (handles 200 B edges at 1-hop depth); Redis caches top-150 followees per user sorted by affinity_score; 2-hop traversal not required for feed
- **experiment_results**: A/B bucketing for ranking model variants; 5 B users → 6 GB experiment assignment table

### Algorithms

**Engagement Score (training label):**
```python
weights = {'share': 5.0, 'comment': 3.0, 'like': 1.0,
           'click': 0.5, 'dwell_normalized': 2.0,
           'skip': -0.5, 'not_interested': -3.0, 'report': -5.0}
```

**Dwell Time Normalization:**
- text < 280 chars: expected 8 s; text > 280 chars: expected 20 s; image: 10 s; video: 0.5 × duration; link: 15 s
- `normalized_dwell = actual_dwell / expected_dwell`; dwell < 0.3 = skip signal

### Key Differentiator
News Feed Ranking's uniqueness is its **5-stage MTL DNN pipeline at 1 M RPS with hybrid push-pull fan-out**: LightGBM GBDT pre-ranker (2–5 ms, 97% recall@200) reduces DNN load by 7.5×; MTL DNN optimizes simultaneously for CTR, share, comment, dwell; Wilson score confidence-adjusts CTR to prevent low-sample gaming; 10K follower threshold separates push-on-write (Redis ZADD at publish) from pull-on-read (celebrity ZSET checked at read time) — the only problem that must handle simultaneous fan-out to hundreds of millions of inboxes AND sub-200 ms serving.

---

## Common Deep Dive (Across All Three)

### Why use FAISS IVF_PQ instead of exact cosine search?
Answer: Exact cosine search over 100 M items × 256 dims = 100 M dot products per request. At float32: 100 M × 256 × 4 bytes = 100 GB read per query — impossible at serving latency. IVF_PQ reduces this by (a) IVF: only searching nprobe=64 of 4096 Voronoi cells (64/4096 = 1.5% of items) and (b) PQ: representing each 256-dim vector as 32 × 8-bit codes (32 bytes vs. 1024 bytes) for 32× compression. Memory: 100 M × 32 bytes = 3.2 GB fits in one pod. Recall@100 = 98.3% at nprobe=64 — the 1.7% recall gap is validated to produce < 0.5% NDCG@10 degradation vs. exact search.
Present in: recommendation_engine, collaborative_filtering
