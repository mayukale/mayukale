# Pattern 12: Maps & Location — Interview Study Guide

Reading Pattern 12: Maps & Location — 4 problems, 7 shared components

---

## STEP 1 — OVERVIEW

This pattern covers the full spectrum of location-aware systems, from serving a rendered map of the entire Earth down to answering "what coffee shop is within 500 meters of me right now." The four problems are:

1. **Google Maps** — Full-stack map: tile rendering, turn-by-turn routing, live traffic ingestion, and ETA at planetary scale (1 B DAU).
2. **Proximity Service** — Core geo-query engine: register entities, search by radius, update positions for 5 M real-time movers (drivers, couriers).
3. **Yelp Nearby** — Discovery platform: text search + geo filter + reviews + ratings + photos for 10 M businesses and 100 M DAU.
4. **Geospatial Index** — Algorithm deep dive: comparing Geohash, Quadtree, and S2 as underlying index structures (the engine all three other problems build on).

**The 7 shared components across all four problems:**
1. Geospatial indexing (Geohash / S2 / Quadtree)
2. Haversine distance post-filter
3. Redis for real-time spatial data
4. Kafka for event streaming and spatial updates
5. CDN for static/cacheable map content
6. Eventual consistency with bounded staleness SLA
7. Three-tier replication (multi-region)

Why this pattern matters in interviews: Every location-aware product — Uber, Lyft, DoorDash, Airbnb, Google, Yelp — requires a geospatial index. Interviewers use these problems to test whether you understand the fundamental tradeoff between index structures, the two-step "coarse index + exact filter" pattern, and how to handle real-time mover updates at millions of RPS.

---

## STEP 2 — MENTAL MODEL

**Core idea**: Convert a 2D coordinate problem into a 1D key-lookup problem.

Latitude and longitude are continuous 2D values. Databases and caches are optimized for 1D key lookups. The entire field of geospatial indexing is fundamentally about encoding a 2D location into a 1D key that preserves enough spatial locality that "nearby in the real world" means "nearby in the index." Once you have that 1D key, everything else — inverted indexes, range scans, neighbor lookups — follows from standard database engineering.

**Real-world analogy**: Imagine you want to find all pizza restaurants within 1 mile of where you're standing. A naive approach is to check every restaurant in the city and compute the distance to each one — that's Dijkstra on the full set. What you actually do is look up your neighborhood on a street grid, check the surrounding blocks, and only measure distance to the ~50 restaurants you can see from there. Geohash is the postal code of that grid: it gives every point on Earth a 6-character "neighborhood name," and all neighbors share a short common prefix.

**Why it's genuinely hard:**

- **Spherical Earth**: longitude lines converge at the poles, so a fixed-precision grid cell in Alaska is much narrower east-west than the same cell in Florida. Index cells that look uniform on a flat grid are physically distorted on a sphere.
- **Antimeridian discontinuity**: the ±180° longitude line cuts through the Pacific Ocean. Two points on opposite sides of that line are physically adjacent but have completely different geohash strings. Your query must handle this edge case explicitly.
- **Real-time movers**: a static coffee shop stays in the same index cell forever. A delivery driver moves cells every few seconds. At 5 M concurrent movers sending updates every 5 seconds, that's 1 M index update operations per second — a write load that will destroy a naively designed index.
- **False positives are unavoidable**: any cell-based index will have a boundary problem. A query circle centered near the edge of a cell will touch the neighboring cell, but not all entities in that neighboring cell are within the query radius. You must always post-filter with exact distance computation.
- **Scale mismatch**: Google Maps serves 694,000 tile requests per second. A typical PostgreSQL server handles ~10,000 queries per second. The gap between "what users demand" and "what a single database can provide" forces every design decision downstream.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these before drawing a single box.

**Q1: What entity types are we indexing — static (businesses) or real-time movers (drivers)?**
What changes: Static entities can be indexed once and rarely updated. Real-time movers need a separate high-write-throughput path with TTL-based auto-expiry. The write path design bifurcates entirely based on this answer.

**Q2: What is the expected search radius range and QPS?**
What changes: A 500 m radius at 100 QPS is a single Redis instance. A 50 km radius at 100,000 QPS requires geohash precision tuning (use coarser cells), more index cells per query, and a cluster. Drives infrastructure sizing.

**Q3: Do we need full-text search combined with the geo filter, or is it pure proximity?**
What changes: Pure proximity → Redis geohash inverted index. Combined text + geo → Elasticsearch with a `geo_distance` filter. Elasticsearch is operationally heavier and has eventual consistency, so you want to know before designing.

**Q4: Do we need routing/directions, or just "find nearby entities"?**
What changes: Directions require a road graph, Contraction Hierarchies preprocessing, and an ETA model. This multiplies the system complexity by 5x. Pure proximity search has no routing at all.

**Q5: What is the consistency requirement — how stale is acceptable?**
What changes: 5 second staleness for movers → Redis with 30 s TTL and async Kafka sync. 60 second staleness for businesses → Kafka-driven eventual consistency is fine. Immediate consistency → synchronous writes, which caps your write throughput.

**Red flags that signal an underprepared candidate:**
- Jumping to "I'll use PostGIS for everything" without discussing read latency at scale.
- Not asking about the write pattern for movers vs. static entities.
- Designing a routing system when the problem just asked for proximity search.
- Storing entity coordinates only in a relational DB and relying on `ST_DWithin` for the real-time search path.

---

### 3b. Functional Requirements

**Core (must have in every version of this problem):**
- Register an entity with a location (lat/lon) and metadata (name, category).
- Search for entities within a radius R meters of a query point.
- Return results sorted by distance.
- Update entity location (for real-time movers: continuously; for businesses: infrequently).
- Delete/deactivate an entity.

**Scope adjustments by problem:**
- Google Maps adds: routing, ETA, turn-by-turn navigation, tile serving, traffic ingestion, autocomplete.
- Yelp adds: full-text search, reviews, ratings, photos, check-ins, recommendation engine.
- Proximity Service: pure radius/K-NN search, focus on write throughput for movers.
- Geospatial Index: algorithm library, no user-facing features.

**Clear statement for an interview:** "Given a location and a radius, return all entities within that radius, ranked by distance, with p99 latency under 100 ms, and no false negatives."

---

### 3c. Non-Functional Requirements

Derive these from the problem — don't memorize, reason aloud:

| NFR | Derived From | Trade-off |
|-----|-------------|-----------|
| **Read latency p99 ≤ 100 ms** | "Users expect instant results on mobile" | ✅ Fast response / ❌ More infrastructure (Redis cluster, not just Postgres) |
| **Write latency p99 ≤ 50 ms** | Movers need ACK before next GPS update | ✅ Low ack latency / ❌ Can't do synchronous DB writes on critical path |
| **Zero false negatives** | Missing a nearby driver = broken product | ✅ Correctness / ❌ Must check 8–9 cells instead of 1, ~52% false positive fetch rate |
| **Eventual consistency ≤ 5–60 s** | Movers at 5 s; businesses at 60 s | ✅ High write throughput via async Kafka / ❌ Brief stale reads |
| **Availability 99.99%** | Google Maps (52 min/year downtime budget) | ✅ High availability / ❌ Requires multi-region, RF=3, no single-node dependencies |
| **Horizontal scale** | 500 M entities, 100 K QPS | ✅ Handles growth / ❌ Distributed coordination overhead (Redis Cluster, Kafka partitioning) |

Key trade-off to call out: **Redis for the hot path vs. PostgreSQL as source of truth.** Redis gives sub-millisecond spatial lookups but is volatile without AOF/RDB persistence and expensive per GB. PostgreSQL gives durable ACID storage and rich queries via PostGIS but can't handle 100K QPS on a single node. The correct answer is both: Redis for real-time query serving, PostgreSQL as the authoritative store with async sync.

---

### 3d. Capacity Estimation

Work through this formula in every interview — it takes 2-3 minutes and instantly signals senior-level thinking.

**Anchor numbers to memorize:**

| Metric | Value | What it implies |
|--------|-------|-----------------|
| Geohash P6 cell size | ~1.2 km × 0.6 km | Standard precision for 1 km radius queries |
| 9 cells per query | center + 8 Moore neighbors | Constant cost regardless of entity count |
| False positive rate at P6 + 1 km radius | ~52% | Expect to fetch and discard ~half of candidates |
| Redis: 500 M entities × 16 B/entry | ~8 GB | Fits in a few Redis nodes |
| Haversine computation: 300 candidates | ~50 µs | Negligible post-filter cost |
| Flink 30s window latency | ~30 s | Traffic freshness SLA |
| CH bidirectional Dijkstra | O(log N) vs O(N) Dijkstra | Sub-second routing at continental scale |

**Formula — search capacity:**
```
Search RPS = (DAU × searches/day) / 86400 × peak_factor
Peak RPS = average RPS × 3 (typical peak factor)
```
- Proximity Service example: 100M users × 5 searches/day / 86400 × 3 = **~17,400 peak RPS**

**Formula — mover update capacity:**
```
Update RPS = concurrent_movers × (1 / update_interval_seconds)
```
- 5 M movers × (1/5 s) = **1 M updates/s average, 3 M at peak**

**Architecture implications:** 1 M update RPS cannot hit PostgreSQL directly (max ~10K writes/s). It must hit Redis (in-memory, ~100K writes/s per node, so you need ~10 shards) and then fan out to Kafka asynchronously. This is a load number that directly forces your architectural choice.

**Time budget in interview**: 3-4 minutes max. Do search RPS, update RPS, and storage size. Skip bandwidth unless the interviewer asks.

---

### 3e. High-Level Design (HLD)

**The 5 mandatory components for any geo search problem:**

1. **API Gateway / Load Balancer** — Auth, rate limiting, TLS termination, route to microservices.
2. **Search Service** — Stateless; converts (lat, lon, radius) → geohash cells → Redis lookups → haversine filter → ranked response.
3. **Geo Index Store (Redis Cluster)** — Geohash inverted index: key = `gh6:{geohash6_cell}`, value = Set of entity_ids.
4. **Entity DB (PostgreSQL + PostGIS)** — Authoritative source of truth for entity metadata; PostGIS for admin queries only, not the search hot path.
5. **Location Update Service + Kafka** (if movers exist) — Write-optimized; immediate Redis write + async Kafka publish → Geo Index Updater consumer handles cell transitions.

**Data flow for the canonical radius search (whiteboard this sequence):**
1. Client: `GET /nearby?lat=37.77&lon=-122.41&radius_m=1000&category=restaurant`
2. API Gateway validates JWT, enforces rate limit (100 RPS/user), routes to Search Service.
3. Search Service: encode (lat, lon) as geohash at precision P (P=6 for 1 km radius). Compute center cell + 8 neighbors (9 cells total).
4. Redis PIPELINE: `SMEMBERS gh6:restaurant:{cell}` × 9 — single round trip.
5. Deduplicate entity_ids. Fetch entity details via Redis HGETALL pipeline.
6. Haversine post-filter: compute exact distance for each candidate, discard those > radius.
7. Sort by distance, paginate, return JSON.

**Whiteboard order** (draw left to right): Client → API Gateway → Search Service → [Redis Geo Index, Entity DB] → response. Add Location Update Service and Kafka on a separate line below if the problem involves movers.

**Key decisions to state explicitly (don't wait to be asked):**
- "I'm using a geohash inverted index in Redis rather than PostgreSQL's PostGIS for the search hot path because PostGIS ST_DWithin cannot handle 17K RPS on a single node."
- "The write path to Redis is synchronous; the PostgreSQL sync happens async via Kafka to avoid serializing mover updates on DB writes."
- "I'm using geohash P6 because cell width (~1.2 km) is approximately twice the target radius (1 km), which guarantees 9 cells cover the query circle."

---

### 3f. Deep Dive Areas

**Deep Dive 1: The geospatial index choice (most-probed)**

The problem: Given 500 M entities on Earth, how do you answer "all entities within 1 km of this point" in under 10 ms?

The solution has two mandatory layers: a coarse index for fast candidate retrieval, and an exact distance computation to remove false positives.

**Geohash inverted index** is the default answer for most problems:
- Encode (lat, lon) as a base-32 string at precision P. Precision 6 → cell ~1.2 km × 0.6 km.
- Store in Redis: key = `gh6:{geohash_string}`, value = a Set of entity_ids.
- For a 1 km radius query: look up center cell + 8 neighbors (9 lookups, pipelined in one Redis round trip).
- Post-filter with haversine. False positive rate ~52% at P6 for 1 km radius — expected and acceptable.
- **O(1) per cell lookup. Total query: O(9) = O(1) effectively.**

✅ Simple to implement, Redis-native, string prefix enables multi-scale index, predictable cost.
❌ ~52% false positive rate. Distortion near poles. Antimeridian discontinuity at ±180°. No K-NN without expanding the ring.

**Quadtree** for adaptive density:
- Tree of rectangular bounding boxes. Each leaf holds up to 50 entities before splitting into 4 quadrants.
- Denser areas (Manhattan) get more levels; oceans stay shallow.
- O(log N) insert/query. ~1 GB for 1 B entities.
- **Best for K-NN**: branch-and-bound traversal with a min-heap prunes subtrees whose nearest possible distance exceeds the K-th result found so far.

✅ Density-adaptive, zero false positives, native K-NN support.
❌ Cannot trivially map to Redis data structures. Tree must live in-memory on a single node or be manually sharded.

**S2 Geometry (Google's approach)**:
- Projects Earth onto 6 cube faces, then recursively subdivides using Hilbert space-filling curve. 30 levels of resolution.
- Each cell gets a 64-bit integer ID. Hilbert curve preserves locality better than Z-order (geohash) — no discontinuities at ±180°.
- Supports exact polygon covering: given any shape, compute the minimal set of S2 cells that cover it exactly.
- **Used by Google Maps for road graph sharding and place indexing at production scale.**

✅ No antimeridian issue. Best for polygon queries. Excellent spatial locality.
❌ More complex to implement. Requires the S2 library. Less intuitive than geohash for simple radius queries.

**When to choose what** (say this in interview without being asked):
- Radius search ≤ 50 km, simple implementation, Redis available → **Geohash P6 inverted index**.
- Highly variable density (cities + oceans), K-NN required, in-memory → **Quadtree**.
- Polygon queries, global routing, no antimeridian issues → **S2**.
- Full-text + geo combined → **Elasticsearch geo_point** (uses geohash internally).

**Deep Dive 2: Real-time mover architecture (asked in Uber/Lyft-style problems)**

The problem: 5 M concurrent drivers each send a GPS update every 5 seconds = 1 M update operations/second. Each update may move the driver across a geohash cell boundary. How do you keep the index correct without destroying read performance?

The solution is a **two-tier write path**:

**Tier 1 (immediate, synchronous):**
Driver app sends `PUT /entities/{id}/location`. Location Update Service immediately writes to Redis Live Location Store: `HSET loc:{entity_id} lat {lat} lon {lon} updated_at {ts}` with a 30-second TTL. This makes the new position available to readers within milliseconds. ACK sent to client.

**Tier 2 (async, Kafka-driven):**
Same write publishes a `location-update` event to Kafka (partitioned by geohash cell, 200 partitions). A Geo Index Updater consumer reads the event. For each update: compare old geohash cell (stored in entity detail hash) vs. new geohash cell. If cells differ: atomically `SREM old_cell entity_id` + `SADD new_cell entity_id`. If cells are the same: no-op (just update entity's lat/lon in the detail hash).

**30-second TTL auto-expiry:** When a driver goes offline and stops sending updates, Redis automatically expires the `loc:{entity_id}` key after 30 seconds. No cleanup job needed. The entity disappears from live location searches automatically.

✅ Immediate write acknowledgment to clients (< 50 ms). Index cell transitions handled async without blocking reads. Auto-cleanup of offline movers.
❌ Up to 5 s lag on index cell transitions (acceptable per requirements). Kafka lag during traffic spikes could temporarily exceed the 5 s SLA.

**Anti-hotspot sharding:** Manhattan's "downtown" area at geohash P6 might contain millions of entities in a single cell. Solution: sub-shard dense cells to P7 precision (~150 m × 150 m). The Search Service detects large cells during query and fans out to sub-shards. This is a detail that separates a good answer from a great one.

**Deep Dive 3: Routing with Contraction Hierarchies (only for Google Maps)**

The problem: a global road graph has ~2 B directed edges. Dijkstra's algorithm is O((V+E) log V) — on a graph this size, that's minutes per query. Users expect routing results in under 2 seconds.

Contraction Hierarchies (CH) solves this with offline preprocessing:

1. **Preprocessing (offline, hours):** Assign each node an "importance" score (edge difference heuristic — how many shortcuts would need to be added if this node were removed). Contract nodes in order of importance from least to most: add shortcut edges that bypass contracted nodes, then remove the contracted node. The final augmented graph is ~3× larger but has a key property: **every shortest path runs only through nodes of increasing importance.**

2. **Query time (milliseconds):** Run bidirectional Dijkstra from source and target simultaneously, but only allow each search to traverse edges going "upward" (toward higher-importance nodes). The two searches meet near the most important node on the path (typically a major highway). Only thousands of nodes are visited instead of millions.

3. **Traffic integration:** CH topology is precomputed once and doesn't change. Only edge weights (travel times) change with traffic. Using **Customizable Route Planning (CRP)**, the road graph is divided into geographic cells. When traffic changes in a cell, only that cell's weight customization step reruns (seconds, not hours).

✅ O(log N) queries vs O(N) for vanilla Dijkstra. Sub-second continental routing. Traffic integration without full re-preprocessing.
❌ Preprocessing takes hours. Shortcuts inflate graph size ~3×. Road closures require per-cell re-customization.

---

### 3g. Failure Scenarios

**Failure 1: Redis Cluster node failure**

The geo index lives in Redis. If a primary node fails, the replica can be promoted in ~30 seconds (Sentinel or Redis Cluster failover). During those 30 seconds, all search queries for entities in that shard fail. **Senior framing**: mitigate by pre-warmed fallback queries: if Redis returns an error, fall back to `SELECT entity_id FROM entity WHERE ST_DWithin(geog, ...) LIMIT 50` on PostgreSQL. This is slower (~500 ms) but correct. Design for graceful degradation, not hard failure.

**Failure 2: Kafka lag spike during mover traffic surge**

If Kafka consumers fall behind, the geohash index cells become stale. Drivers' physical positions diverge from their index positions. **Senior framing**: the staleness SLA is 5 seconds. Monitor Kafka consumer lag as a first-class SLA metric. If lag > 5 s, alert. Auto-scale Kafka consumer group (Kubernetes HPA on custom metrics). Design the index with TTL so stale movers expire automatically even if the index updater is behind.

**Failure 3: Routing service fails during active navigation session**

Users mid-navigation lose rerouting capability. **Senior framing**: the routing algorithm is stateless — any routing server can handle any reroute request (road graph is read-only, shared from distributed storage). Load balance across routing servers. If all fail, fall back to last-computed route (cached in client). Emit a "routing degraded" banner but keep the user on the last known route rather than showing an error.

**Failure 4: Hot cell problem in the geo index**

A large concert or sporting event creates a dense cluster of movers in a tiny area (a geohash P6 cell may have 100,000 taxis in Times Square on New Year's Eve). All writes and reads for that cell hit one Redis key. **Senior framing**: detect hot keys via Redis slow log or key access metrics. Dynamically sub-shard: split the hot P6 cell into 32 P7 sub-cells. Update Search Service to query sub-cells when a cell is flagged as oversized.

---

## STEP 4 — COMMON COMPONENTS

These 7 components appear across all four problems. Know each one cold: why it's used, how it's configured, and what breaks without it.

---

### Component 1: Geospatial Index (Geohash / S2 / Quadtree)

**Why used:** Translates a 2D proximity query into a 1D key-value lookup. Without a spatial index, finding entities within a radius requires scanning all entities and computing distance for each one — O(N) per query, which is unusable at 500 M entities.

**Key configuration:**
- **Geohash precision**: P6 (~1.2 km × 0.6 km) for queries ≤ 5 km. P5 (~4.9 km) for queries ≤ 20 km. Always use center + 8 neighbors (9 total cells) to guarantee zero false negatives.
- **Redis key pattern**: `gh6:{category}:{geohash6_string}` → Redis Set of entity_ids. Including category in the key enables O(1) category-filtered queries without post-processing.
- **S2 level**: Level 14 (~600 m × 600 m cells) for road nodes and place indexing in Google Maps. Level 6 for road graph sharding (~250 km cells, one shard per region).

**What breaks without it:** Every search query does a full table scan. PostgreSQL `ST_DWithin` without the geohash pre-filter is used only for admin bulk queries, not the 17K-RPS real-time path. The system falls over immediately under production load.

**Bonus detail to volunteer**: The false positive rate at geohash P6 for a 1 km radius query is ~52%. You fetch ~9 × (average cell density) candidates and discard about half. This is not a bug — it's the expected cost of using rectangular cells to approximate a circle. The haversine post-filter, which runs on ~300 candidates in ~50 µs, handles it perfectly.

---

### Component 2: Haversine Distance Post-Filter

**Why used:** Geohash (and all cell-based indexes) approximate search circles as rectangles. Entities near cell boundaries are returned as index candidates even when they're outside the actual query radius. The haversine formula computes the exact great-circle distance between two points on a sphere to filter these false positives.

**Key configuration:**
```
a = sin²(Δlat/2) + cos(lat1) × cos(lat2) × sin²(Δlon/2)
distance = 2R × atan2(√a, √(1-a))    where R = 6,371,000 m
```
Applied to every candidate returned by the index. For typical query sizes (50–300 candidates), this takes 50–200 µs — negligible.

**Vincenty vs Haversine**: Haversine assumes a perfect sphere and has ~0.3% error for distances under 1,000 km (~3.3 km error per 1,000 km). For all proximity search use cases (radius ≤ 50 km), this error is less than 150 m — acceptable. Only use Vincenty (oblate spheroid model, millimeter accuracy) for transoceanic routing calculations.

**What breaks without it:** ~52% of returned entities are outside the requested radius. The API claims "find all restaurants within 1 km" but returns restaurants up to 2 km away. Product is broken.

---

### Component 3: Redis for Real-Time Spatial Data

**Why used:** Sub-millisecond access to hot location data. Redis supports all the data structures needed for geo operations natively: Sets for entity membership per cell, Hashes for entity detail records, and the built-in GEORADIUS/GEOSEARCH commands as an alternative to the custom inverted index.

**Key configurations across the four problems:**
- **Geo index**: `gh6:{cell}` → Redis Set of entity_ids. `SADD`, `SREM`, `SMEMBERS` — all O(1) or O(N) on the set size.
- **Live location cache**: `loc:{entity_id}` → Redis Hash with lat, lon, speed, heading. **TTL = 30 seconds** so offline movers auto-expire.
- **Entity detail cache**: `entity:{entity_id}` → Redis Hash with name, category, lat, lon. TTL = 5 minutes.
- **Rating cache (Yelp)**: `biz:{business_id}:rating` → float. TTL = 30 seconds, refreshed by Kafka-driven Rating Aggregator consumer.
- **Route cache (Google Maps)**: `route:{hash(origin+dest+mode)}` → serialized route. TTL = 30 seconds (traffic changes).

**Key config numbers:** Redis Cluster with 3 shards × 2 replicas covers the proximity service workload. `WAIT 1 0` for near-synchronous replica writes. AOF enabled for durability. Maxmemory-policy = allkeys-lru for caches.

**What breaks without it:** Search Service must query PostgreSQL for every request. PostGIS handles ~10K QPS max per node. At 17K peak QPS for proximity service, you need 2+ PostgreSQL nodes plus complex query routing logic. Latency jumps from 10 ms to 100+ ms. The system can technically work but is 10× more expensive and slower.

---

### Component 4: Kafka for Event Streaming and Spatial Updates

**Why used:** Decouples the high-throughput write path (mover location updates, review submissions, probe ingestion) from the lower-throughput downstream processing (index updates, rating aggregation, traffic computation). Provides durable, replayable, partitioned event log with exactly-once semantics via transactions.

**Key configurations:**
- **Proximity Service**: `location-updates` topic with 200 partitions keyed by geohash cell. Ensures all updates for the same geographic area go to the same partition, enabling the Geo Index Updater to process cell transitions without cross-partition coordination.
- **Google Maps**: `probe-events` topic with 6 M RPS ingest. Partitioned by S2 cell of the probe's coordinate. Flink consumers compute median speed per road segment per 30-second tumbling window.
- **Yelp**: `review-events` topic. Rating Aggregator consumer applies incremental formula. `business-updates` topic consumed by Search Indexer to keep Elasticsearch in sync.

**Key config numbers:** Replication factor = 3, min-ISR = 2 (ensures no data loss even if one broker fails). Retention = 7 days for replay capability. Flink checkpoints every 30 seconds for exactly-once traffic aggregation.

**What breaks without it:** Three failure modes:
1. **Location update writes become synchronous all-or-nothing**: if the geo index write fails, the DB write is already committed, leaving them inconsistent.
2. **Traffic aggregation has no replay**: if the Flink job crashes mid-window, you can't reconstruct the last 30 seconds of traffic data without Kafka replay.
3. **Review ratings lag by seconds instead of milliseconds**: the Rating Aggregator would need to poll the database, burning CPU and adding query load.

---

### Component 5: CDN for Static/Cacheable Map Content

**Why used:** The highest-volume read operations in Google Maps and Yelp are for immutable or slowly-changing binary assets: map tiles, Street View imagery, business photos. Serving these from origin servers at 694K RPS for tiles alone would require thousands of servers. CDN edge caches absorb 95%+ of this traffic at the edge, near the user.

**Key configurations:**
- **Google Maps tiles**: `Cache-Control: public, max-age=86400, immutable` for static tiles. Tiles are content-addressed (URL contains style version + XYZ coordinates), so stale caches are impossible. CDN purge is called only when the tile is actually re-rendered during a map update.
- **Yelp photos**: Content-hash URLs (`cdn.yelp.com/photos/{sha256_hash}_lg.jpg`). `Cache-Control: public, max-age=31536000, immutable` (1 year). 10 B photos × 500 KB = 5 PB total, with 80% of traffic served from CDN edge.
- **Proximity Service / Geospatial Index**: No CDN. Entity location data changes every few seconds — no cache is valid long enough to be useful.

**What breaks without it (for Google Maps):** 694K RPS of tile traffic hits origin servers. At ~15 KB per tile, that's 10.4 GB/s of egress from origin. You'd need ~1,000 tile serving instances vs. ~50 with 95% CDN hit rate. Additionally, user-perceived latency for tile fetches goes from ~15 ms (CDN edge) to ~60-100 ms (origin), making map panning feel sluggish.

---

### Component 6: Eventual Consistency with Bounded Staleness SLA

**Why used:** Writing synchronously to all components on every update limits write throughput. All four problems accept some staleness in exchange for much higher write capacity and resilience.

**Staleness windows by problem:**
- **Google Maps traffic**: ≤ 60 seconds end-to-end (probe → Kafka → Flink 30s window → Bigtable write → routing service cache expiry).
- **Proximity Service movers**: ≤ 5 seconds (immediate Redis write → Kafka → cell transition update ≤ 5 s under normal lag).
- **Proximity Service businesses**: ≤ 60 seconds.
- **Yelp ratings**: ≤ 30 seconds (Kafka-driven Rating Aggregator consumer updates Redis cache).
- **Yelp search index**: ≤ 60 seconds (Search Indexer consumer updates Elasticsearch).

**Design pattern**: Write to the primary store (PostgreSQL / Bigtable) first. Publish a Kafka event. Downstream consumers update derived caches/indexes eventually. TTLs act as a safety net: even if a Kafka event is dropped, the cache expires and the next read refreshes from primary within one TTL window.

**What breaks without it:** To maintain synchronous consistency at 1 M location updates/second, you'd need a write operation to simultaneously update PostgreSQL, Redis geo index, Redis live location, Elasticsearch, and Cassandra history — serialized. The latency would be 200+ ms per write instead of 10 ms, and a single slow component blocks all writes. The system would fail under load.

---

### Component 7: Three-Tier Replication (Multi-Region)

**Why used:** A single-region deployment has a single point of failure at the datacenter level. For 99.99% availability (52 minutes of downtime per year), you need the system to survive a full regional outage. Multi-region deployment with three replicas provides durability guarantees and global low-latency reads.

**Key configurations:**
- **Kafka**: RF=3, min-ISR=2. A broker failure still leaves 2 replicas. A partition with 1 ISR can still accept writes but risks data loss on a second failure — alert on min-ISR=1.
- **Redis Cluster**: Primary + 2 replicas per shard. `WAIT 1 0` for near-synchronous replication (at least 1 replica acknowledges before returning to client). Sentinel or Redis Cluster for automatic failover.
- **PostgreSQL**: Primary + 2 read replicas. Reads go to replicas for read-scaling. Writes go to primary. Replicas use streaming replication with async commit for performance.
- **Google Bigtable**: Multi-region cluster groups. Write quorum = 2. Read from nearest cluster (may be slightly behind primary — acceptable given 60 s freshness SLA).
- **GCS (tiles/photos)**: Multi-region bucket automatically replicates across US, EU, APAC data centers.

**What breaks without it:** A single datacenter network partition or power outage takes the entire service offline. For a consumer-facing product with global users, this is unacceptable. The 99.99% SLA requires at minimum active-passive multi-region with sub-minute failover.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Google Maps

**What makes it unique:** Google Maps is the only problem in this folder that builds and serves the map itself. Every other problem in this pattern (Yelp, Proximity Service) assumes a map already exists and queries points on it. Google Maps creates the underlying map data from satellite imagery, maintains it, renders it into 585 M tiles across 23 zoom levels, serves those tiles to 1 B DAU at 694K RPS from 130 CDN points of presence, routes across a 2 B edge road graph using Contraction Hierarchies, and continuously incorporates live traffic from 6 M simultaneous navigation sessions via Flink streaming — none of which any other problem here requires.

**Unique decisions you won't see elsewhere:**
- **Contraction Hierarchies (CH)**: offline preprocessing of the road graph to enable O(log N) route queries instead of O(N). CH introduces "shortcut" edges that bypass less-important nodes. At query time, bidirectional Dijkstra only traverses upward edges, visiting thousands of nodes instead of millions. Unpacking shortcuts reconstructs the full turn-by-turn path.
- **Hidden Markov Model (HMM) map-matching**: raw GPS has ±5-15 m accuracy and can't reliably identify which of two parallel streets a probe is on. HMM matches probe sequences to the road graph by considering both GPS accuracy (emission probability) and road connectivity (transition probability). This is critical for generating accurate traffic speed data from GPS probes.
- **Flink 30-second sliding window**: traffic aggregation is a streaming problem, not a batch problem. The 30-second tumbling window per road segment computes the median speed from all matching probes in that window. Median (not mean) is used because a few stopped vehicles shouldn't drag down the speed estimate for a flowing road.
- **Tile update pipeline with atomic CDN swap**: when map data changes (a new road, a re-labeled building), the pipeline must re-render affected tiles and swap them into CDN without users seeing a torn state. The pipeline: detect affected tile coordinates → re-render PBF tiles → upload to GCS with a new ETag → atomically update tile metadata pointer → issue CDN PURGE for affected tile URLs.

**Two-sentence differentiator:** Google Maps is a map creation and serving platform, not just a map query platform — it ingests raw satellite imagery and OSM data, extracts road geometry via ML, renders 585 million tiles across 23 zoom levels, routes on a 2 billion-edge graph using Contraction Hierarchies, and streams live traffic from 6 million concurrent GPS probes through Flink to update road speeds every 30 seconds. No other problem in this pattern builds the underlying map data, and the scale of every subsystem (694K RPS tile serving, 6M probe RPS, continental-scale CH routing) is an order of magnitude larger than anything in the other three problems.

---

### Proximity Service

**What makes it unique:** Proximity Service is the only problem in this folder that must handle millions of continuously moving entities that appear and disappear from the index dynamically. Static entities (businesses in Yelp, places in Google Maps) are registered once and rarely move. In Proximity Service, 5 M drivers and couriers are each sending position updates every 5 seconds, crossing geohash cell boundaries continuously, and going offline (the entity should vanish from the index within 30 seconds of the last update). Managing the lifecycle of these ephemeral, high-frequency entities requires a dedicated architectural tier that doesn't exist in any other problem here.

**Unique decisions you won't see elsewhere:**
- **30-second TTL auto-expiry in Redis**: live movers are stored with a 30-second TTL (`EXPIRE loc:{entity_id} 30`). Every incoming location update refreshes the TTL. If a driver goes offline, stops sending updates, and the TTL expires, the entity automatically disappears from the live location store — no cleanup cron job, no tombstone record, no soft-delete pipeline.
- **Two-tier write path for cell transitions**: every location update does an immediate Redis write (for search freshness) AND publishes to Kafka (for geo index cell maintenance). The Kafka consumer detects when an entity has moved from one geohash cell to another and atomically executes `SREM old_cell entity_id` + `SADD new_cell entity_id`. Entities within the same cell only get the Redis lat/lon update — no index churn.
- **Anti-hotspot P6→P7 sub-sharding**: dense urban cells (Times Square, Shibuya Crossing) can accumulate millions of entities in a single P6 cell, creating a hot Redis key. The system detects cells exceeding a size threshold and dynamically sub-shards to P7 (150 m × 150 m cells), distributing load across 32 sub-keys.
- **1 M–3 M write RPS is the design constraint, not read RPS**: unlike Yelp (29:1 read/write ratio), Proximity Service is nearly write-symmetric for the real-time mover use case. The write path is the performance bottleneck, not reads.

**Two-sentence differentiator:** Proximity Service's defining challenge is maintaining a correct spatial index for 5 million continuously moving entities generating 1–3 million location updates per second — a write load 50× higher than the search load. Its two-tier write path (immediate Redis + async Kafka cell-transition updater), 30-second TTL auto-expiry, and anti-hotspot P6→P7 sub-sharding together solve a fundamentally different problem than the other three designs, which all assume mostly-static entity positions.

---

### Yelp Nearby

**What makes it unique:** Yelp is the only problem in this folder where the search query is not just "find entities near me" but "find relevant, high-quality businesses near me that match a complex multi-dimensional query including free text, categories, price range, current open hours, dietary attributes, and user preferences." The geo component is one of several equally important signals, and the ranking algorithm blending all these signals is itself a system design problem.

**Unique decisions you won't see elsewhere:**
- **Elasticsearch function_score with multi-signal ranking**: Yelp's search doesn't just sort by distance. The Elasticsearch function_score query combines BM25 text relevance (searching name, description, category with field-specific boosts), Gaussian distance decay (score falls off smoothly with distance, scale=2 km), log-normalized rating boost, and log-normalized review count boost. These are combined with `score_mode=sum, boost_mode=multiply` so that text relevance gates the result — a restaurant irrelevant to the query text cannot rank first just because it's nearby and highly rated.
- **Yelp Sort algorithm for reviews**: individual review ranking combines elite_user_bonus, useful_vote_count × 0.4 weight, recency_decay(created_at), and text_length_bonus. This is a content quality signal, not a geo signal, but it shapes what users see and trust on the profile page.
- **Rating aggregation with incremental delta formula**: `new_avg = (old_avg × old_count + new_rating) / (old_count + 1)` is computed in the same transaction as the review INSERT. This is O(1) — no full table scan. The aggregate is always mathematically correct at transaction commit. Redis cache is updated asynchronously via Kafka.
- **Lambda-based photo resizing**: photo resize is CPU-intensive (~500 ms per photo × 3 target sizes). Instead of resizing synchronously in the API server (which would block the upload response for 1.5 seconds) or provisioning a persistent resize fleet (which would need to scale with sporadic photo upload spikes), Lambda auto-scales per invocation, triggered by S3 upload events. At 23 photo uploads/second average and 3× peak, Lambda handles it without any persistent server provisioning.
- **UNIQUE(business_id, user_id) on reviews**: enforced at the database constraint level, not just application logic. This prevents duplicate reviews even under concurrent submissions. The conflict error surfaces cleanly as a 409 in the API.

**Two-sentence differentiator:** Yelp Nearby transforms a simple "find nearby entities" problem into a multi-signal discovery platform by combining BM25 full-text relevance, Gaussian geo decay, log-normalized rating/review count boosts, and open-hours filtering in a single Elasticsearch function_score query — plus an offline ALS recommendation engine for personalization. No other problem in this pattern handles the review system (write path, voting, aggregation, Yelp Sort algorithm), the photo service (S3 + Lambda resize pipeline), or the content quality signals that separate a restaurant discovery product from a raw proximity search.

---

### Geospatial Index (Reference Design)

**What makes it unique:** This is the only problem in the folder that is purely algorithmic — there is no user, no API, no production service. It is a reference design for the engine that all three other problems build on top of. Its value is in providing side-by-side implementation details of all three major spatial index structures with concrete complexity analysis, memory footprints, and code implementations.

**Unique decisions you won't see elsewhere:**
- **Algorithm comparison table**: Geohash (O(1) lookup, 20 GB for 1 B entities, ~50% false positives, no K-NN) vs. Quadtree (O(log N), 1 GB for 1 B entities, ~0% false positives, native K-NN) vs. S2 (O(log N), 9 GB for 1 B entities, ~0% false positives, best polygon coverage). This is the reference table for every spatial index decision in the other three problems.
- **K-NN via branch-and-bound in quadtree**: the quadtree's K-NN implementation uses a min-heap sorted by the minimum possible distance from the query point to each subtree's bounding box. Subtrees where this minimum distance exceeds the K-th result found so far are pruned. This is O(K log N) with branch-and-bound vs. O(N) for naïve scan.
- **Bulk loading with Z-order / Hilbert sort**: inserting 1 B entities one-by-one into a quadtree is slow and produces poor cache locality (spatially distant entities in the same memory page). Pre-sorting entities by Morton code (Z-order) or Hilbert curve value before bulk insert places spatially proximate entities in adjacent memory pages, giving 5-10× faster bulk load times.
- **Antimeridian handling for geohash**: queries near longitude ±180° must be split into two separate cell lookups because geohash strings on opposite sides of the antimeridian are completely unrelated. S2 avoids this entirely.

**Two-sentence differentiator:** The Geospatial Index deep dive is the algorithm reference manual that all three other problems cite without implementing — it provides the only side-by-side comparison of Geohash, Quadtree, S2, and Redis GEORADIUS with concrete memory estimates, lookup complexity, false-positive rates, K-NN support, and implementation code. Knowing this material cold lets you give authoritative answers to "why geohash instead of quadtree" or "what's the false positive rate at precision 6" in any of the other three interviews without needing to derive it on the whiteboard.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (2–4 sentences each)

**Q1: What is a geohash and why do we use it for proximity search?**

**KEY PHRASE: "1D key that preserves spatial locality"**

A geohash encodes a 2D (lat, lon) coordinate into a 1D string by interleaving binary representations of latitude and longitude bits, then encoding in base-32. The key property is that nearby points on Earth have similar geohash prefixes — this is called preserving spatial locality. We use it for proximity search because it lets us convert a 2D "find all entities within this circle" query into a set of string key lookups in a hash map, which Redis can answer in O(1) per key. The precision level (P6 = ~1.2 km cells) is chosen so that the query circle is covered by at most 9 cells, giving a constant-cost lookup regardless of entity count.

---

**Q2: Why do you check 9 cells instead of just the one cell containing the query point?**

**KEY PHRASE: "zero false negatives requires 8 neighbors"**

If the query point is near the edge of its geohash cell, there are entities just across the boundary that are physically within the search radius but live in the neighboring cell. Checking only the center cell misses them — a false negative. By checking the center cell plus all 8 adjacent cells (the Moore neighborhood), you guarantee that any entity within the search radius is included in the candidate set, regardless of where the query point falls within its cell. The downside is ~52% false positive rate — entities in the corners of neighboring cells that are outside the actual radius — which is filtered out with haversine distance computation afterward.

---

**Q3: What is Contraction Hierarchies and why does Google Maps use it instead of Dijkstra?**

**KEY PHRASE: "bidirectional search only traverses upward edges"**

Dijkstra on a global road graph of 2 billion edges would take minutes per query — totally unusable. Contraction Hierarchies (CH) solves this with an offline preprocessing step: nodes are assigned importance scores and contracted in order, with shortcut edges added to preserve shortest-path correctness. At query time, a bidirectional Dijkstra from source and target is restricted to only traverse edges going upward toward higher-importance nodes. This means both searches explore only thousands of nodes (meeting near a major highway in the hierarchy) instead of millions. The result is sub-second routing on a continental road graph.

---

**Q4: How do you handle the case where a driver goes offline and should no longer appear in search results?**

**KEY PHRASE: "30-second TTL auto-expiry"**

Each real-time mover has a Redis record `loc:{entity_id}` with a 30-second TTL. Every incoming location update refreshes the TTL. If the driver goes offline and stops sending updates, Redis automatically expires the key after 30 seconds — no cleanup job, no soft-delete API call, no background worker. The entity simply vanishes from the live location store when the TTL hits. The geo index cell still contains the entity_id for up to 30 seconds, but the Search Service ignores results where the live location key is missing (treating them as stale/offline). This is a clean, operationally simple solution to the mover lifecycle problem.

---

**Q5: What's the difference between Haversine and Vincenty distance formulas? When do you use each?**

**KEY PHRASE: "Haversine assumes a sphere; 0.3% error acceptable under 1,000 km"**

Haversine assumes Earth is a perfect sphere with radius 6,371 km. It produces ~0.3% error for distances under 1,000 km — that's ≤3 km of error per 1,000 km of actual distance. For all proximity search use cases (radius ≤ 50 km), this means at most ~150 m of error — totally acceptable when the user asked for a 1 km radius. Vincenty uses an oblate spheroid model and is accurate to millimeters, but it's iterative and ~10× slower to compute. Use Haversine for proximity search post-filtering. Use Vincenty only when you need transoceanic accuracy, like long-haul aviation routing.

---

**Q6: Why does Yelp use Elasticsearch instead of just PostGIS for search?**

**KEY PHRASE: "combined text relevance + geo in one query"**

PostGIS handles geographic queries well (`ST_DWithin`, `ST_Distance`) but has no concept of text relevance — it can't score "how well does this business match the query 'sushi'." Elasticsearch combines BM25 full-text scoring with geo_distance filtering in a single query, and applies custom scoring functions (distance decay, rating boost) as part of the ranking. For Yelp's use case where search relevance, geographic proximity, and business quality must all influence ranking, Elasticsearch is the right tool. PostGIS remains the authoritative store for business metadata — Elasticsearch is a derived search index, not the source of truth.

---

**Q7: How do you keep the search index fresh when businesses update their information?**

**KEY PHRASE: "Kafka event → Search Indexer consumer → Elasticsearch partial update"**

When a business updates its hours, category, or name, the Business Profile Service writes to PostgreSQL and publishes a `business-updated` event to Kafka. The Search Indexer Service, a Kafka consumer, reads this event and issues a partial `_update` document to Elasticsearch with only the changed fields. This is a partial update (not a full document reindex), which is faster and reduces Elasticsearch write amplification. The Kafka-driven async approach means the Elasticsearch index is eventually consistent — typically within 60 seconds. A reconciliation job runs every 5 minutes to catch any events that were dropped or failed to apply.

---

### Tier 2 — Deep Dive Questions (why + trade-offs)

**Q1: Why is geohash P6 chosen as the default, and what happens when the query radius is 50 km?**

**KEY PHRASE: "cell width ≈ radius/2 guarantees 9 cells suffice"**

The rule for choosing precision: cell width should be approximately half the query radius. At P6, cells are ~1.2 km wide. For a 1 km radius, 9 cells (covering ~3.6 km × 1.8 km) fully enclose the query circle. If you use P6 for a 50 km radius, you'd need a ring of approximately 100 cells instead of 9 — the query becomes 10× more expensive and the false positive rate explodes. The correct approach for a 50 km radius is to drop to P4 (~39 km cells) or P5 (~4.9 km cells). At P4, 9 cells cover a 117 km × 117 km area, which fully encloses a 50 km circle. ✅ Always match precision to radius for constant-cost queries. ❌ Multi-scale queries (variable radius) require either a dynamic precision selection mechanism or a multi-precision inverted index.

---

**Q2: Walk me through the two-tier write path for location updates. Why is the tier separation necessary?**

**KEY PHRASE: "immediate Redis for freshness, async Kafka for cell transitions"**

The Location Update Service receives 1–3 M location updates per second from movers. Writing synchronously to both Redis (geo index update) and PostgreSQL (persistent store) and Kafka would serialize all writes on the slowest component (PostgreSQL, ~10K writes/s max) — the system falls over immediately at peak load. Tier 1 writes immediately to Redis Live Location Store (`HSET loc:{entity_id}` + TTL refresh) and returns ACK to the client. This keeps end-to-end write latency under 50 ms p99. Tier 2 publishes to Kafka async. The Geo Index Updater consumer handles cell transitions at its own pace — it only needs to run the expensive `SREM old_cell` + `SADD new_cell` when an entity actually crosses a cell boundary (most updates stay within the same cell). ✅ Sub-50 ms write latency at 3 M RPS. Geo index cell transitions handled without blocking writes. ❌ Up to 5 s lag on index cell transitions. If Kafka consumer falls behind, driver positions in the index are stale.

---

**Q3: How does Contraction Hierarchies handle real-time traffic updates without full re-preprocessing?**

**KEY PHRASE: "CRP separates topology from metric — only metric needs updating"**

Pure CH preprocessing is topology-dependent: the importance scores and shortcut edges are computed once and are traffic-independent. CH queries always find the correct shortest path because shortcuts encode the shortest path over contracted nodes at their precomputed weights. When traffic changes, the edge weights (travel times) change but the topology (which nodes exist, which shortcuts were added) doesn't. Customizable Route Planning (CRP) divides the graph into geographic cells. When traffic changes within a cell, only that cell's "weight customization" step re-runs — a seconds-long operation instead of hours. The query still uses the same shortcuts and hierarchy; it just applies updated weights in the affected cells. ✅ Sub-minute traffic freshness without hours of preprocessing. ❌ CRP adds implementation complexity. A major road reclassification (new highway built) still requires full re-preprocessing.

---

**Q4: Explain the Yelp function_score query and why score_mode=sum + boost_mode=multiply matters.**

**KEY PHRASE: "text relevance gates results; distance and rating boost within relevant results"**

The function_score query in Elasticsearch works like this: the base score is BM25 text relevance (how well does the business name/description/category match the query text). Multiple function scores are then defined: a Gaussian decay on geo_distance (score decays smoothly from 1.0 at the business location to 0.5 at 2 km, to 0.1 at 5 km), a logarithmic boost on rating, and a logarithmic boost on review_count. `score_mode=sum` means all function scores are summed together. `boost_mode=multiply` means this sum is multiplied by the BM25 base score to get the final score. The `multiply` is critical: if the BM25 score is near zero (the business is irrelevant to the query text), no amount of distance proximity or high rating will make it rank highly. A highly-rated restaurant 200 meters away that doesn't match "sushi" doesn't appear at the top of a "sushi" search — exactly correct product behavior.

---

**Q5: How would you design the "find K nearest drivers" query for a ride-share dispatch system? What index structure would you choose and why?**

**KEY PHRASE: "K-NN requires expanding ring or branch-and-bound, not fixed-radius cells"**

Geohash inverted index is optimized for fixed-radius queries ("entities within 1 km") but K-NN ("find 3 nearest drivers regardless of distance") is fundamentally different — the search boundary is unknown in advance. Two approaches: First, start with a small radius (e.g., 500 m, which is geohash P7) and expand to P6, P5, P4 until K results are found. This is the "expanding ring" approach — simple but can require multiple Redis round trips. Second, use a Quadtree with branch-and-bound K-NN traversal: maintain a max-heap of the K closest drivers found so far. For each node, compute the minimum possible distance from the query point to the node's bounding box. If this minimum exceeds the K-th result in the heap, prune the entire subtree. This gives O(K log N) K-NN in a single pass. For ride-share where K is typically small (3-5 drivers), the expanding ring approach is simpler to implement and Redis-native. For analytics where K is large (100 nearest), quadtree branch-and-bound is more efficient.

---

**Q6: How do you prevent a "hot cell" problem when there's a concert in a stadium and 50,000 people are all in the same geohash cell?**

**KEY PHRASE: "sub-shard P6→P7 for cells exceeding a size threshold"**

A single geohash P6 cell (~1.2 km × 0.6 km) typically contains tens or hundreds of entities. At a stadium event, it might suddenly contain 50,000 movers, all writing to and reading from a single Redis key. Redis is single-threaded per shard, so this creates a hot key bottleneck. The solution is dynamic sub-sharding: monitor cell size as part of the Location Update Service. When a cell exceeds a configurable threshold (e.g., 10,000 entities), the system splits it into 32 P7 sub-cells and redirects all writes to the sub-cell keys. The Search Service detects that a P6 cell is sub-sharded and fans out queries to all 32 sub-cells (still pipelined in Redis). On entity count dropping back below threshold (event ends), the system merges sub-cells back. ✅ Distributes hot-cell load across 32 keys. ❌ Query fan-out increases from 9 to 9×32=288 Redis commands during a hot event. In practice, P7 cells are still very fast to look up, and pipelining keeps round trips to 1.

---

**Q7: Why does Google Maps use Bigtable for traffic data instead of PostgreSQL?**

**KEY PHRASE: "LSM write path handles 6M probe fan-out; O(1) read on (edge_id, timestamp_bucket) row key"**

The traffic layer receives writes at the rate of the Flink output: one row per road segment per 30-second window. With ~2 B road segments globally and a 30-second window, that's up to ~66 M writes per minute during peak. PostgreSQL's B-tree write path cannot handle this write rate without severe write amplification and table bloat. Bigtable uses an LSM (Log-Structured Merge) tree write path: writes go to a write-ahead log and an in-memory memtable, then compact asynchronously. This gives 10× higher write throughput than B-tree systems at sustained load. The row key `edge_id#timestamp_bucket` provides O(1) point reads for the Routing Service (which needs the current speed for a specific edge) and O(range scan) for the ETA model (which needs the last N minutes of speed history for a specific edge). Neither PostgreSQL nor Redis can match this combination of write throughput + time-series read pattern.

---

### Tier 3 — Staff+ Stress Test Questions (reason aloud)

**Q1: You're designing a multi-modal routing system that combines road navigation with transit (bus, subway). How do you integrate transit schedules with Contraction Hierarchies, and what happens when a bus runs late?**

This is a fundamentally different routing problem because transit introduces a time dimension: the "cost" of boarding a bus depends on when you arrive at the stop. Standard CH assumes edge weights are time-independent (travel time on a road is approximately constant). For multi-modal routing, you use a layered graph: the road network has its own CH preprocessing; the transit network uses RAPTOR (Round-Based Public Transit Optimized Router) which handles scheduled departure times and transfers. The connection between layers is modeled as time-expanded nodes: each transit stop has one node per scheduled departure time, and edges from the road network to transit nodes have a time-dependent weight (the waiting time until the next departure). When a bus runs late, the transit layer's departure times are updated in near-real-time from the GTFS-RT feed. The key design challenge is that CH preprocessing assumes static weights — late buses invalidate the preprocessing. The practical solution is to use CH for the road component only, and RAPTOR for the transit component. RAPTOR doesn't require preprocessing and handles live delays naturally. The two are joined by a transfer graph at interchange points (stations, bus stops within walking distance). A trip running 5 minutes late propagates to all downstream transfers that depended on that connection via a post-processing step that re-evaluates affected legs.

---

**Q2: Your geospatial proximity service is deployed globally. A user in Tokyo queries for nearby drivers, but the geohash index for Tokyo is in a different AWS region than the user's API request (which landed in Singapore). How do you handle geo-distributed reads while maintaining consistency across mover updates that happen globally?**

This is the fundamental tension between geo-distributed deployments and consistency. The approach depends on your staleness SLA. For a 5-second staleness requirement, you can treat each region as a semi-independent read replica of the geo index: the Singapore region keeps a local Redis geohash index for Southeast Asia including Tokyo, updated by a Kafka consumer reading from the Tokyo region's location-updates topic via cross-region replication (Kafka MirrorMaker 2 or MSK Global Replication). A Tokyo driver's position update hits the Tokyo API gateway first, writes to the Tokyo Redis immediately, publishes to Tokyo Kafka, which replicates to Singapore Kafka, which the Singapore Geo Index Updater consumes — total lag ~2-3 seconds for cross-region propagation, within the 5-second SLA. For the user in Singapore querying Tokyo entities, the Singapore Search Service handles the query against its locally-replicated Tokyo index. The tricky edge case: a driver crossing a region boundary (a truck crossing from Korea into Japan). This requires the Location Update Service to dual-write to both regional Kafka topics until the driver has fully settled in the new region. You should acknowledge this edge case in the interview and propose the dual-write window (e.g., dual-write for 5 minutes after a region crossing to ensure both regions have consistent data).

---

**Q3: You're a FAANG senior engineer and your manager asks you to reduce the p99 tail latency for map tile serving from 100 ms to 50 ms. The current CDN hit rate is 95%. Walk me through your complete investigation and improvement strategy.**

Start by decomposing the p99: the 5% CDN miss path (which hits origin at 60-200 ms) disproportionately contributes to the p99. Even if 95% of requests return at 15 ms from CDN, the 5% that miss and take 200 ms will push the p99 well above 50 ms — this is a fat-tail problem driven entirely by cache misses. Three parallel tracks: First, **increase CDN hit rate from 95% to 99%**. The main causes of CDN misses are: (a) cold start on new tiles after a map update (fix: proactively warm the cache by pre-fetching the 500 most-viewed tiles in each city after every tile update pipeline run); (b) cache eviction due to long-tail tile requests from exploratory map panning (fix: tier the CDN — use a smaller hot cache at extreme edge PoPs with the top 1% tiles, and a larger warm cache at regional PoPs for the next 10%); (c) cache bypass for tiles with `Cache-Control: no-cache` (audit the tile update pipeline to ensure only truly changed tiles get cache invalidation, not entire zoom levels). Second, **optimize the origin path for the remaining 1% misses**. Current origin latency is 60-200 ms. Profile: is the 200 ms case from Bigtable cold reads? Add an in-process L1 LRU cache on each tile serving pod (1 GB per pod, caching ~66K tiles, covers the most frequently re-requested tiles during cache stampedes). This should cut origin p99 from 200 ms to 80 ms for cache-warm paths. Third, **address geographic outliers**: tiles in remote regions (central Africa, mid-ocean) are served from a PoP far from the user. Audit CDN PoP coverage against your user geography distribution. If significant traffic comes from regions without a nearby PoP, negotiate CDN expansion or use a second CDN provider with complementary PoP coverage for those regions. Finally, implement a real-time latency attribution dashboard that breaks down p99 by (CDN hit vs. miss) × (geographic region) × (zoom level) to continuously identify regressions.

---

## STEP 7 — MNEMONICS

### Mnemonic: "GHQ-HRK" — the 7 steps of a proximity query

**G**et the geohash cell for the query point.
**H**unt the 8 neighboring cells (Moore neighborhood).
**Q**uery Redis: pipeline SMEMBERS for all 9 cells.
**H**ydrate entity details from Redis Hash.
**R**un haversine on each candidate.
**K**eep only those within radius. Sort. Return.

The mental model this encodes: **coarse index → expand neighborhood → exact filter → hydrate → sort.** Every geo proximity problem follows this exact sequence.

---

### Mnemonic for the algorithm comparison: "GQS — FPK"

| Letter | Index | False Positives | K-NN |
|--------|-------|-----------------|------|
| G | Geohash | ~50% (yes) | No |
| Q | Quadtree | ~0% (no) | Yes |
| S | S2 | ~0% (no) | Yes |

Geohash has **F**alse **P**ositives. Quadtree and S2 support **K**-NN. When you need K-NN or zero false positives, you don't use geohash alone — you use Quadtree or S2.

---

### Opening one-liner for any geo interview

When the interviewer says "design a proximity service" or "design nearby friends," open with:

**"The fundamental challenge in every geo search system is converting a 2D proximity query into a 1D key lookup — and managing the gap between what the coarse index returns and what the user actually asked for. I'll start with geohash-based inverted indexing in Redis, and the moment you tell me the scale and consistency requirements, I'll know exactly which parts of this design need to get more complex."**

This establishes you as someone who understands the underlying algorithm, not just the component names.

---

## STEP 8 — CRITIQUE

### What the source material covers well

The source files provide exceptional depth in several areas that are genuinely important in interviews:

- **Geohash algorithm implementation**: The complete Python encode/decode/neighbors implementation with the precision table (P1 through P12 with exact cell dimensions) is the most thorough coverage you'll find anywhere. Knowing that P6 = 1.2 km × 0.6 km and that false positive rate at P6+1km is exactly ~52% will make you sound like someone who has actually implemented this.

- **Contraction Hierarchies**: The bidirectional Dijkstra implementation with the "upward edges only" invariant and the shortcut unpacking logic is a complete working implementation. Most candidates can name CH; few can explain why it only traverses upward edges during query.

- **Yelp's multi-signal ranking**: The specific ES function_score structure with Gaussian decay parameters (scale=2 km, offset=100 m), the `score_mode=sum, boost_mode=multiply` design choice, and the Yelp Sort formula are all production-realistic details that interviewers love.

- **Two-tier write path for movers**: The Proximity Service's 30-second TTL + Kafka-driven cell transition logic is the clearest explanation of this pattern I've seen in any study material.

- **Capacity numbers**: Every problem includes worked-out calculations with the underlying assumptions made explicit. The 6 M probe RPS calculation for Google Maps, the 1 M location update RPS for Proximity Service, and the 52% false positive rate derivation are all show-your-work examples you can reproduce on a whiteboard.

---

### What is missing or shallow

**Missing: How to handle the antimeridian and poles in production.** The source material mentions the antimeridian problem exists for geohash but doesn't give a complete algorithm for handling it. In a real interview, if asked "your service needs to work in New Zealand (near ±180°)," you should know: (1) detect when `|lon| > 179.5°`, (2) run the query twice — once with `lon + 360°` and once with `lon - 360°`, (3) union and deduplicate results. For poles (`|lat| > 80°`), drop one precision level to get larger cells that compensate for east-west distortion.

**Missing: How to shard the road graph for routing.** The source material says Google Maps uses a "custom distributed graph store partitioned by S2 cell ID" but doesn't explain how cross-partition routes work. In an interview for a routing-focused role, you need to know: geographic partitioning means most local routes stay within one shard (fast). Long-distance routes cross shard boundaries and require multi-hop queries. CH's hierarchy partially solves this — important nodes (major highways) are replicated across shards. A full answer would describe hierarchical graph partitioning where boundary nodes between shards are replicated.

**Shallow: Recommendation engine for Yelp.** The ALS + Feature Store description is correct but high-level. A Staff+ interview might probe: how do you handle the cold-start problem for new businesses (no interaction history)? Answer: fall back to popularity-based ranking (review count × recency) for businesses with fewer than N reviews, transition to ALS-influenced ranking above that threshold. The source material doesn't cover this.

**Shallow: Disaster recovery and data loss scenarios.** The source material covers replication well but doesn't address the "Kafka topic is corrupted" or "Redis Cluster loses all replicas simultaneously" scenarios. For a Staff+ interview, know the answer: Kafka topics can be restored from a snapshot of the source Kafka topic in a different region (MirrorMaker 2). The Redis geo index can be rebuilt by scanning all active entities in PostgreSQL and re-inserting them into the new Redis Cluster (a warm-up job that takes ~30 minutes for 500 M entities at 300K inserts/second).

---

### Senior probes the source material doesn't prepare you for

**"How would you implement exactly-once semantics for the geohash index updater?"**
Kafka exactly-once requires idempotent producers + transactional consumers. The Geo Index Updater should check a processed-event log (Redis set keyed by Kafka offset) before applying each cell transition. If the offset is already in the log, the event was already applied — skip it. This prevents the double-remove scenario where `SREM old_cell entity_id` is applied twice (making the entity disappear) or `SADD new_cell entity_id` is applied twice (harmless for a Set but wastes compute).

**"How do you handle 'open now' filtering at scale in Yelp?"**
Naively, you'd compute for each business whether it's currently open based on its `hours_json`. But this means re-evaluating 10 M business schedules on every query. The source material mentions `open_now_cache` as a boolean field in Elasticsearch — a cron job updates this field every 5 minutes. But this means there's a 5-minute window where a business's "open now" status is stale. The senior probe is: "what if a business changes its hours today?" — the hours change writes to PostgreSQL and publishes a business-update Kafka event. The Search Indexer updates the Elasticsearch document including `open_now_cache` — this propagates in under 60 seconds, well within the 5-minute refresh cycle.

**"Your Flink traffic aggregation job has been offline for 2 hours due to a deploy issue. Traffic data is 2 hours stale. How do you recover without serving incorrect routes?"**
Flink checkpointing enables replay from the last good checkpoint. The Flink job reads from Kafka — Kafka has retained 7 days of probe events. On recovery, Flink resumes from the checkpoint offset and processes 2 hours of buffered probes to regenerate all missed traffic speed windows. During the 2-hour catch-up period, the Routing Service falls back to historical speed profiles (day-of-week + hour-of-day averages) instead of real-time speeds. This is configured via a "traffic freshness" check: if the latest Bigtable traffic row for a segment is more than 10 minutes old, treat it as historical data. This is a critical operational design — the system degrades gracefully, not catastrophically.

---

### Common interview traps in this pattern

**Trap 1: Recommending PostGIS for the search hot path.** PostGIS `ST_DWithin` is a fine answer for admin queries, but it won't handle 17K QPS on a single PostgreSQL node. Always route the real-time search hot path through Redis.

**Trap 2: Using Redis GEORADIUS without explaining its limitations.** GEORADIUS is fine for simple use cases but doesn't support multi-attribute filtering (you'd need one sorted set per category), and its behavior for very large datasets requires careful reasoning about the internal geohash radius approximation. Show you know the limitations, not just that the command exists.

**Trap 3: Forgetting about the read-your-writes requirement for reviews.** The source material mentions "a user sees their own review immediately after submitting" — this is a read-your-writes guarantee. It means after submitting a review, the API response should include the submitted review in the business's review list, even if the Elasticsearch index hasn't been updated yet. Implement by writing the review to PostgreSQL synchronously, then reading from PostgreSQL for the immediate response, and letting the Kafka-driven Elasticsearch update propagate for all subsequent users.

**Trap 4: Over-engineering the routing problem when the question is just proximity.** If asked to "design a service to find nearby restaurants," don't start talking about Contraction Hierarchies. That's routing (Google Maps). The proximity service just needs to find restaurants within a radius — no road graph involved.

**Trap 5: Forgetting that geohash cells are NOT circles.** When an interviewer asks "does your system guarantee it finds all entities within exactly 1 km?", the correct answer is: "The geohash index returns all entities within the 9-cell bounding rectangle (~3.6 km × 1.8 km), which is a superset of the query circle. The haversine post-filter then precisely removes entities outside the 1 km circle. So yes, we guarantee zero false negatives and we handle false positives by exact distance computation."

---

*End of INTERVIEW_GUIDE.md — Pattern 12: Maps & Location*
