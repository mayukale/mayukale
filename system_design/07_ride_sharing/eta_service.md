# System Design: ETA Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Point-to-point ETA**: Given an origin and destination, compute the estimated travel time and distance via road network.
2. **Driver-to-rider pickup ETA**: Given a driver's current GPS position and a rider's pickup location, compute the ETA with real-time traffic.
3. **Fare estimate ETA**: Compute ETA and distance for the fare estimation screen before a rider requests a ride (origin to destination).
4. **En-route ETA updates**: During an active trip, recalculate and push updated ETA to the rider every 30 seconds (or on significant route deviation).
5. **Multi-waypoint ETA**: Support up to 5 waypoints for pool ride route planning.
6. **Historical pattern learning**: Incorporate time-of-day and day-of-week speed patterns into base edge weights.
7. **Real-time traffic incorporation**: Adjust road graph edge weights using live probe data from active drivers and third-party traffic feeds.
8. **ML-based ETA correction**: Apply a learned correction factor to graph-computed ETAs to reduce systematic bias (e.g., parking time, traffic signal delays not captured in graph).
9. **Accuracy metrics**: Track and expose ETA accuracy (predicted vs. actual), broken down by city, hour, route length, and traffic condition.
10. **Route polyline**: Return an encoded polyline of the recommended route for map display.
11. **Alternative routes**: Return up to 3 alternative routes with their ETAs for the driver app.

### Non-Functional Requirements

- **Latency**: ETA computed in < 500 ms (p50) and < 1.5 s (p99) for any city-pair query.
- **Throughput**: Handle 16,700 ETA requests per second at peak (from active trip ETA refresh + matching + fare estimates).
- **Accuracy**: Mean Absolute Percentage Error (MAPE) < 15% on trip ETAs. P95 absolute error < 5 minutes for trips < 30 minutes.
- **Graph freshness**: Road graph updated from traffic data within 5 minutes of traffic event.
- **Availability**: 99.99% — ETA is on the critical path for fare estimates, matching, and active trip tracking.
- **Scalability**: Support 500,000 active trips + 462 new ride requests/s + 531 fare estimates/s globally.
- **Road graph size**: Globally, OSM road network has ~900 M nodes and ~1.1 B edges. City-scoped subgraphs typically 1–50 M nodes.

### Out of Scope

- Turn-by-turn voice navigation
- Public transit routing (bus, subway)
- Bicycle or pedestrian routing (outside vehicle routing)
- Traffic incident reporting (consumed from third-party, not created here)
- Map tile generation and serving

---

## 2. Users & Scale

### User Types

| Caller | Query Type | Frequency | Latency Requirement |
|---|---|---|---|
| Fare Service | Origin → Destination ETA (pre-request) | 531/s (fare estimates) | < 500 ms |
| Matching Service | Driver position → Pickup ETA | 531/s (matching decisions) | < 500 ms |
| Trip Service (en-route refresh) | Driver position → Destination (via current pickup/dropoff) | 500K active trips / 30 s | 16,667/s |
| Rider App (push, via Trip Service) | Same as en-route refresh | 16,667/s | Async; result pushed via WebSocket |
| Driver App | Driver position → Pickup / Destination | Same as en-route | Async |
| Driver App (alternative routes) | 3 alternative routes | 462 trips starting/s | < 1 s |
| Pool Matching Service | Multi-waypoint route ETA | ~46/s (10% of trips are pool) | < 1 s |

### Traffic Estimates

**Assumptions**:
- 500,000 active trips, ETA refreshed every 30 s → 16,667 ETA requests/s (dominant load)
- Fare estimate requests: 531/s (from Fare Service section)
- Matching pickup ETA: 531/s
- Starting trip alternative routes: 462/s
- Pool waypoint ETAs: 46/s
- Peak factor: 1.5×

| Request Type | Rate (avg) | Rate (peak) | Weight per query |
|---|---|---|---|
| En-route ETA refresh | 16,667/s | 25,000/s | Light (single A* from current pos) |
| Fare estimate ETA | 531/s | 800/s | Medium (full origin→dest A*) |
| Matching pickup ETA | 531/s | 800/s | Light (short driver→pickup) |
| Alternative routes (3×) | 462/s × 3 | 700/s × 3 | Heavy (3× A* with variation) |
| Multi-waypoint pool | 46/s | 70/s | Heavy (multiple A* calls) |
| **Total ETA queries/s** | **18,237/s** | **27,770/s** | — |

**Graph computation cost**:
- Average Dijkstra/A* on a city graph: 100 K nodes visited per query, 1 µs per node operation
- Cost per ETA query: ~100 ms (naive); with bidirectional A* and CH: ~2–5 ms
- Total CPU at peak: 27,770 q/s × 3 ms avg = 83,310 ms/s = 84 CPU cores minimum

| Metric | Calculation | Result |
|---|---|---|
| ETA queries/s (peak) | 27,770 | 27,770/s |
| CPU cores for graph computation | 27,770 × 3 ms / 1000 | ~84 cores (for routing alone) |
| Graph memory per city | 50 M nodes × 32 B + 100 M edges × 32 B | ~4.8 GB per large city |
| Cities globally | ~500 major cities served | — |
| Total graph memory (if all in RAM) | 500 cities × 5 GB avg | 2.5 TB (distributed across pods) |
| Traffic weight cache | 100 M edges × 8 B per edge weight | 800 MB per large city |
| Historical speed patterns | 100 M edges × 24 hours × 7 days × 4 B | 67 GB per large city (stored on disk, LRU-cached) |

### Latency Requirements

| Operation | P50 | P99 | Notes |
|---|---|---|---|
| A* routing (short trip, < 5 km) | 5 ms | 20 ms | Driver pickup ETA |
| A* routing (medium trip, 5–30 km) | 20 ms | 80 ms | Fare estimate, en-route |
| A* routing (long trip, > 30 km) | 50 ms | 200 ms | Airport runs |
| Contraction Hierarchy (CH) query | 1 ms | 5 ms | With precomputed CH |
| ML correction inference | 2 ms | 10 ms | Post graph computation |
| Full ETA response (graph + ML) | < 50 ms | < 200 ms | Before network round-trip |
| End-to-end (client to client) | < 500 ms | < 1.5 s | Including network |

### Storage Estimates

| Data | Size | Volume | Storage |
|---|---|---|---|
| Road graph (nodes + edges, per large city) | ~5 GB | 500 cities | 2.5 TB total on disk |
| Contraction Hierarchy auxiliary data | ~2× graph size | Same | 5 TB total |
| Historical speed patterns (7-day rolling) | 67 GB / large city | 100 large cities | 6.7 TB |
| Real-time edge weight delta store | 800 MB / large city | 100 large cities | 80 GB (in-memory or Redis) |
| ETA request/response log | 500 B | 18,237/s × 86,400 | ~788 GB/day |
| Accuracy metrics (aggregated) | 200 B | 20 M trips/day | 4 GB/day |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| Inbound ETA requests | 27,770/s × 200 B (origin, dest, speed) | ~5.5 MB/s in |
| Outbound ETA responses | 27,770/s × 1 KB (ETA + polyline) | ~27.8 MB/s out |
| Traffic weight updates (Kafka) | 100 K edge updates/min × 16 B | ~27 KB/s |
| Graph updates (OSM) | ~100 MB/day (incremental diffs) | ~1.2 KB/s (trickle) |

---

## 3. High-Level Architecture

```
                 ┌────────────────────────────────────────────────┐
                 │              ETA SERVICE CALLERS                │
                 │  Fare Service │ Matching Service │ Trip Service │
                 └───────────────────────┬────────────────────────┘
                                         │ gRPC
                                         ▼
                          ┌──────────────────────────────┐
                          │     API GATEWAY / LB          │
                          │  (gRPC load balancer,         │
                          │   routed by city_id affinity) │
                          └──────────────┬────────────────┘
                                         │
            ┌────────────────────────────▼──────────────────────────┐
            │                   ETA SERVICE POOL                     │
            │   (stateful pods; each pod loads city subgraph         │
            │    into memory; routed by city_id consistent hash)     │
            │                                                         │
            │  ┌────────────────────────────────────────────────┐    │
            │  │             PER-REQUEST PIPELINE                │    │
            │  │  1. Snap origin/dest to road node (map-match)  │    │
            │  │  2. Load edge weights (realtime + historical)   │    │
            │  │  3. Run bidirectional A* / CH query             │    │
            │  │  4. Decode route to polyline                    │    │
            │  │  5. Apply ML correction factor                  │    │
            │  │  6. Return ETA, distance, polyline              │    │
            │  └────────────────────────────────────────────────┘    │
            └──────────┬─────────────────────────────────────────────┘
                       │
         ┌─────────────┴───────────────────────────────────────┐
         │                                                       │
 ┌───────▼──────────┐  ┌──────────────────┐   ┌───────────────▼──────┐
 │  ROAD GRAPH STORE│  │  TRAFFIC WEIGHT  │   │  ML CORRECTION        │
 │  (per-city       │  │  CACHE           │   │  SERVICE              │
 │   serialized     │  │  (Redis per city │   │  (TF Serving /        │
 │   CH graph,      │  │   edge_id →      │   │   lightweight XGBoost │
 │   blob storage)  │  │   weight_delta)  │   │   model per city)     │
 └───────────────────┘  └──────┬───────────┘   └──────────────────────┘
                                │
              ┌─────────────────┼────────────────────────────┐
              │                 │                             │
   ┌──────────▼──────┐  ┌───────▼───────────┐  ┌────────────▼────────┐
   │  TRAFFIC INGEST │  │  HISTORICAL SPEED  │  │  MAP GRAPH UPDATER  │
   │  PIPELINE       │  │  AGGREGATOR        │  │  (OSM diffs,        │
   │  (Kafka:        │  │  (Flink: per-edge  │  │   new roads,        │
   │   location-upd  │  │   speed by time    │  │   road closures)    │
   │   → edge speeds)│  │   window → S3)     │  │   → rebuild CH)     │
   └─────────────────┘  └────────────────────┘  └─────────────────────┘

   ┌──────────────────────────────────────────────────────────────┐
   │              ACCURACY MONITORING PIPELINE                     │
   │  On trip completion: compare predicted ETA vs actual duration │
   │  → Write to accuracy_log table → Grafana dashboard           │
   └──────────────────────────────────────────────────────────────┘
```

**Component Roles**:

| Component | Role |
|---|---|
| ETA Service Pod | Loads city road graph in memory; runs A*/CH routing; applies ML correction; serves gRPC requests |
| Road Graph Store | Blob storage (S3/GCS) for serialized CH graphs per city; loaded at pod startup |
| Traffic Weight Cache | Redis per city: maps edge_id → speed_delta (km/h offset from historical baseline); updated every 5 min by Traffic Ingest Pipeline |
| Traffic Ingest Pipeline | Kafka consumer on `location-updates`; aggregates driver probe speeds per road segment; computes edge weight deltas; publishes to Redis |
| Historical Speed Aggregator | Flink streaming job; maintains rolling 7-day average speed per edge per (hour, day_of_week); stores to S3, preloaded into graph edges |
| ML Correction Service | Takes graph-computed ETA + contextual features (time, weather, special events, trip length); outputs a correction multiplier |
| Map Graph Updater | Ingests OSM diffs daily; merges new/changed road segments; triggers CH rebuild for affected city partitions |
| Accuracy Monitoring | Compares predicted ETA at trip start vs. actual trip duration; stores in accuracy_log; feeds ML retraining pipeline |

---

## 4. Data Model

### Entities & Schema

**Road Graph (in-memory per ETA pod, serialized to S3)**:

```go
// Road graph node (intersection or road segment junction)
type Node struct {
    NodeID    int64
    Lat       float32
    Lng       float32
    // Contraction Hierarchy (CH) properties
    Rank      int32      // CH rank — higher = contracted later
    IsShortcut bool      // Is this a CH shortcut edge?
}

// Road edge (directed, one per direction for two-way roads)
type Edge struct {
    EdgeID      int64
    FromNodeID  int64
    ToNodeID    int64
    DistanceM   float32    // Physical distance in meters
    MaxSpeedKmh float16    // Posted speed limit
    RoadClass   uint8      // MOTORWAY=0, TRUNK=1, PRIMARY=2, ... RESIDENTIAL=7
    Flags       uint8      // OneWay, Toll, Ferry, HOV flags
    // Dynamic weight (set from traffic data at runtime)
    CurrentSpeedKmh float32  // Updated from probe data / traffic API
    // Derived travel time (recomputed when CurrentSpeedKmh changes)
    TravelTimeS float32      // = DistanceM / (CurrentSpeedKmh × 1000/3600)
}

// Contraction Hierarchy shortcut edge (created during CH preprocessing)
type ShortcutEdge struct {
    EdgeID          int64
    FromNodeID      int64
    ToNodeID        int64
    TotalTravelTimeS float32
    MidNodeID       int64   // Intermediate node (for path unpacking)
    Via1EdgeID      int64   // First original edge in this shortcut
    Via2EdgeID      int64   // Second original edge
}
```

**Graph serialization to S3**: Nodes and edges are serialized as flat binary arrays (Apache Arrow format for zero-copy memory-map loading). A 50 M node + 100 M edge graph serializes to ~3 GB (compressed with LZ4). Loading time from S3: ~30 s at 100 MB/s EBS throughput.

```sql
-- ============================================================
-- POSTGRESQL: ETA Accuracy Log
-- ============================================================
CREATE TABLE eta_accuracy_log (
    log_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    trip_id         UUID NOT NULL,
    city_id         INT NOT NULL,
    prediction_type VARCHAR(20) NOT NULL,   -- PICKUP_ETA|TRIP_ETA|FARE_ESTIMATE
    predicted_at    TIMESTAMPTZ NOT NULL,
    predicted_eta_s INT NOT NULL,           -- seconds
    actual_eta_s    INT,                    -- NULL until trip completes
    distance_km     NUMERIC(8,3),
    absolute_error_s INT,                  -- |predicted - actual|
    percentage_error NUMERIC(6,3),         -- ABS_ERROR / actual
    route_type      VARCHAR(20),           -- HIGHWAY|URBAN|MIXED
    had_traffic     BOOLEAN,
    time_of_day_hour SMALLINT,
    day_of_week     SMALLINT,
    graph_algorithm VARCHAR(20),            -- ASTAR|CH|HYBRID
    ml_correction_factor NUMERIC(6,4),
    city_id_idx     INT
);

SELECT create_distributed_table('eta_accuracy_log', 'city_id');

CREATE INDEX idx_eta_acc_trip ON eta_accuracy_log (trip_id);
CREATE INDEX idx_eta_acc_city_time ON eta_accuracy_log (city_id, predicted_at DESC);

-- ============================================================
-- POSTGRESQL: Road Segment Speed Baseline (seeded from historical)
-- Updated daily by Historical Speed Aggregator
-- ============================================================
CREATE TABLE road_speed_baselines (
    edge_id         BIGINT NOT NULL,
    city_id         INT NOT NULL,
    hour_of_day     SMALLINT NOT NULL,     -- 0–23
    day_of_week     SMALLINT NOT NULL,     -- 0=Monday, 6=Sunday
    avg_speed_kmh   NUMERIC(5,2) NOT NULL,
    sample_count    INT NOT NULL,
    updated_at      TIMESTAMPTZ DEFAULT NOW(),
    PRIMARY KEY (edge_id, hour_of_day, day_of_week)
);

-- ============================================================
-- POSTGRESQL: Graph Version Metadata
-- ============================================================
CREATE TABLE graph_versions (
    version_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    city_id         INT NOT NULL,
    osm_timestamp   TIMESTAMPTZ NOT NULL,
    node_count      BIGINT NOT NULL,
    edge_count      BIGINT NOT NULL,
    s3_path         TEXT NOT NULL,         -- s3://graphs/city-{id}/v{ts}.bin
    ch_built        BOOLEAN DEFAULT FALSE,
    ch_build_time_s INT,
    deployed_at     TIMESTAMPTZ,
    is_active       BOOLEAN DEFAULT FALSE,
    build_status    VARCHAR(20) DEFAULT 'PENDING',
    created_at      TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_graph_versions_city ON graph_versions (city_id, created_at DESC);
```

**Redis Traffic Cache**:
```
# Real-time edge speed cache per city
Key:  traffic:{city_id}:edge_speeds
Type: Hash
Field: edge_id (string)
Value: current_speed_kmh (float as string)
TTL:  10 min (auto-expires if Traffic Ingest stops)

# Alternative: sorted set for "recently updated edges" (for incremental graph refresh)
Key:  traffic:{city_id}:updated_edges
Type: Sorted Set
Score: unix timestamp of last update
Member: edge_id
TTL: 10 min

# Probe count per edge (confidence of speed estimate)
Key:  traffic:{city_id}:edge_probe_count
Type: Hash
Field: edge_id
Value: count of probe readings in last 5 min
```

### Database Choice

| Store | Data | Selection | Justification |
|---|---|---|---|
| **In-process memory** | Road graph, CH auxiliary structures | Selected | Zero-latency graph traversal; graph is read-only at query time; serialized from S3 on startup |
| **S3 / GCS (blob)** | Serialized CH graph files | Selected | Durable, cheap, simple versioning; loaded once at pod startup |
| **Redis** | Real-time edge weight deltas | Selected | Sub-millisecond hash lookup by edge_id; TTL provides natural expiry; small dataset (800 MB / city) |
| **PostgreSQL** | Accuracy log, speed baselines, graph versions | Selected | Low write volume; SQL analytics needed for accuracy reporting and ML feature engineering |
| **Apache Parquet / S3** | Historical speed patterns | Selected | Column-oriented; efficient for time-range scans; Flink native format; 67 GB/city stored cheaply |
| Neo4j / graph DB | Road network | Rejected | Graph DBs add network overhead per traversal; in-memory custom graph is 10–100× faster |
| DynamoDB | Edge speeds | Rejected | 1–10 ms reads; Redis at < 1 ms is required for the hot query path |

**Justification for in-memory graph**: The A* and CH algorithms require random-access traversal of neighbor lists — O(log k) heap operations per node visited, ~100 K nodes per query. Any I/O (disk, network) per node lookup would make this orders of magnitude slower. Keeping the graph in process memory eliminates I/O entirely. The cost is per-pod memory (~5 GB for a large city) and the constraint that pods are stateful (they own a city's graph). This is managed by consistent-hash routing on city_id at the load balancer.

---

## 5. API Design

All internal gRPC APIs. External REST endpoints for driver app (alternative routes display).

---

### ComputeETA (gRPC — primary internal API)

```protobuf
service ETAService {
  rpc ComputeETA(ETARequest) returns (ETAResponse);
  rpc ComputeETABatch(ETABatchRequest) returns (ETABatchResponse);
  rpc ComputeMultiWaypointETA(MultiWaypointETARequest) returns (MultiWaypointETAResponse);
  rpc GetAlternativeRoutes(AlternativeRoutesRequest) returns (AlternativeRoutesResponse);
}

message ETARequest {
  int32  city_id      = 1;
  double origin_lat   = 2;
  double origin_lng   = 3;
  double dest_lat     = 4;
  double dest_lng     = 5;
  ETARequestType request_type = 6;  // PICKUP_ETA|TRIP_ETA|FARE_ESTIMATE
  float  origin_speed_kmh = 7;      // current driver speed (for heading-aware snap)
  float  origin_heading   = 8;      // degrees (for map matching)
  string trip_id      = 9;          // for accuracy tracking (can be empty for fare estimates)
  bool   include_polyline = 10;     // false for matching (saves bandwidth)
  bool   include_alternatives = 11; // false by default
}

message ETAResponse {
  int32  eta_seconds         = 1;
  double distance_km         = 2;
  float  ml_correction_factor = 3;
  string polyline_encoded    = 4;   // Google Encoded Polyline format; empty if not requested
  int32  graph_eta_seconds   = 5;   // pre-ML correction (for debugging)
  double confidence          = 6;   // 0.0–1.0 (lower during low-probe-count periods)
  TrafficLevel traffic_level = 7;   // NONE|LIGHT|MODERATE|HEAVY
  int64  origin_node_id      = 8;   // matched road node
  int64  dest_node_id        = 9;
}

message ETABatchRequest {
  repeated ETARequest requests = 1;
  // Used by Matching Service: score 50 driver-to-pickup ETAs in one RPC
  int32 city_id = 2;
}

message ETABatchResponse {
  repeated ETAResponse responses = 1;
}

message MultiWaypointETARequest {
  int32 city_id = 1;
  repeated Waypoint waypoints = 2;  // origin, pickup1, pickup2, dropoff1, dropoff2
  string trip_id = 3;
}

message Waypoint {
  double lat = 1;
  double lng = 2;
  string label = 3;  // ORIGIN|PICKUP|DROPOFF
}

message MultiWaypointETAResponse {
  repeated int32 segment_eta_seconds = 1;  // ETA between consecutive waypoints
  int32 total_eta_seconds = 2;
  double total_distance_km = 3;
  string full_polyline_encoded = 4;
}
```

### REST: Alternative Routes (Driver App)

```
GET /v1/eta/alternatives
Auth: Driver JWT
Query:
  origin_lat:     float (required)
  origin_lng:     float (required)
  dest_lat:       float (required)
  dest_lng:       float (required)
  city_id:        int (required)

Response 200:
{
  "routes": [
    {
      "rank":          1,
      "eta_seconds":   840,
      "distance_km":   7.2,
      "polyline":      "_p~iF~ps|U_ulLnnqC_mqNvxq`@",
      "summary":       "US-101 N",
      "traffic_level": "MODERATE"
    },
    {
      "rank":          2,
      "eta_seconds":   1020,
      "distance_km":   6.8,
      "polyline":      "...",
      "summary":       "Market St",
      "traffic_level": "HEAVY"
    },
    {
      "rank":          3,
      "eta_seconds":   1140,
      "distance_km":   8.1,
      "polyline":      "...",
      "summary":       "Van Ness Ave",
      "traffic_level": "LIGHT"
    }
  ]
}
```

### Internal: Get ETA Accuracy Stats

```
GET /v1/internal/eta/accuracy?city_id=1&from=2026-04-09T00:00Z&to=2026-04-09T23:59Z
Auth: Admin JWT

Response 200:
{
  "city_id":          1,
  "period":           "24h",
  "total_predictions": 184320,
  "mape_pct":          11.4,
  "mae_seconds":       78,
  "p50_error_s":       45,
  "p95_error_s":       210,
  "p99_error_s":       480,
  "by_hour": [
    { "hour": 8, "mape_pct": 14.2, "mae_s": 92 },
    { "hour": 17, "mape_pct": 18.5, "mae_s": 140 },
    ...
  ],
  "by_route_type": {
    "highway": { "mape_pct": 8.2 },
    "urban":   { "mape_pct": 14.7 },
    "mixed":   { "mape_pct": 11.8 }
  }
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Road Graph Representation & Routing Algorithm

**Problem It Solves**: Given two geographic points, find the fastest route through a road network containing 50+ million nodes and 100+ million directed edges, in < 50 ms. Standard Dijkstra on a graph this size visits millions of nodes per query — far too slow. We need preprocessing to accelerate queries.

**Approaches Comparison**:

| Algorithm | Preprocessing | Query Time | Memory Overhead | Accuracy | Handles Dynamic Weights |
|---|---|---|---|---|---|
| Dijkstra (plain) | None | O((V+E) log V) ≈ minutes on large graph | None | Exact | Yes |
| A* with heuristic | None | O(E log V) ≈ seconds | None | Exact (admissible h) | Yes |
| **Bidirectional A*** | None | O(E^0.5 log V) ≈ 100 ms | None | Exact | Yes |
| **Contraction Hierarchies (CH)** | Hours (offline) | < 1 ms | 2× graph size | Exact | Partial (with updates) |
| OSRM (CH-based) | Hours | < 1 ms | Large | Exact | Via weight overlay |
| ALT (A* + Landmarks) | Minutes | 10–50 ms | Moderate | Exact | Yes |
| Hub Labeling | Days | Sub-ms | Very large (TB) | Exact | No |
| Arc Flags | Hours | 10–100 ms | Moderate | Exact | Limited |

**Selected: Contraction Hierarchies (CH) with Dynamic Weight Overlay**

**Why CH**: CH preprocessing reduces query time from hundreds of milliseconds (bidirectional A*) to < 1 ms for long-distance queries, and < 5 ms for urban queries with frequent turns. The precomputed shortcut edges eliminate intermediate nodes, allowing the query to run on a much sparser "highway hierarchy" graph. The weight overlay (described below) preserves real-time traffic adaptability.

**CH Preprocessing**:

```
1. Node ordering: Assign each node a contraction rank using:
   edge_difference = edges_added_by_contracting - edges_removed
   Plus: search space size (lower = better) + uniformity (spread out contractions)
   Use lazy evaluation of rank (recompute on demand)

2. Node contraction: Contract nodes in rank order.
   For each neighbor pair (u, v) through contracted node w:
     If the shortest path u → v goes through w (no shortcut already exists):
       Add shortcut edge u → v with weight = weight(u,w) + weight(w,v)

3. CH graph = original graph + shortcut edges
   Typical: 100 M original edges + 100–150 M shortcut edges

4. Serialization: Write CH graph to Apache Arrow binary format
   Serialize: node_array (NodeID, Rank, Lat, Lng), edge_array (FromID, ToID, TravelTimeS, IsShortcut)
   Size: ~3 GB for 50M node + 200M edge CH graph (LZ4 compressed)
```

**CH Query Algorithm**:

```go
// Bidirectional CH query
func (g *CHGraph) QueryETA(originNode, destNode int64) (float32, []int64) {
    // Forward Dijkstra from origin (upward in CH rank)
    // Backward Dijkstra from destination (also upward — same direction in CH)
    // Meeting point = node with minimum (forward_dist[v] + backward_dist[v])

    fwdDist := make(map[int64]float32)
    bwdDist := make(map[int64]float32)
    fwdPrev := make(map[int64]int64)
    bwdPrev := make(map[int64]int64)

    fwdQueue := newMinHeap()
    bwdQueue := newMinHeap()

    fwdDist[originNode] = 0
    bwdDist[destNode]   = 0
    fwdQueue.Push(0, originNode)
    bwdQueue.Push(0, destNode)

    bestTotal := float32(math.MaxFloat32)
    meetNode  := int64(-1)

    for !fwdQueue.Empty() || !bwdQueue.Empty() {
        // Alternate forward/backward steps for balance
        if !fwdQueue.Empty() {
            u, uDist := fwdQueue.Pop()
            if uDist > bestTotal { break }
            for _, edge := range g.ForwardEdges[u] {
                // CH: only relax edges to nodes with HIGHER rank
                if g.Rank[edge.ToNode] > g.Rank[u] {
                    newDist := uDist + g.EdgeWeight(edge)
                    if prev, ok := fwdDist[edge.ToNode]; !ok || newDist < prev {
                        fwdDist[edge.ToNode] = newDist
                        fwdPrev[edge.ToNode] = u
                        fwdQueue.Push(newDist, edge.ToNode)
                    }
                    // Check meeting point
                    if bDist, ok := bwdDist[edge.ToNode]; ok {
                        if total := newDist + bDist; total < bestTotal {
                            bestTotal = total
                            meetNode  = edge.ToNode
                        }
                    }
                }
            }
        }
        // Symmetric backward step (omitted for brevity)
    }

    // Unpack path using fwdPrev/bwdPrev + shortcut resolution
    path := unpackPath(fwdPrev, bwdPrev, meetNode, originNode, destNode)
    return bestTotal, path
}

// Edge weight uses dynamic traffic overlay
func (g *CHGraph) EdgeWeight(edge *Edge) float32 {
    if delta, ok := g.trafficCache.Get(edge.EdgeID); ok {
        // Apply speed delta from real-time traffic
        effectiveSpeed := max(5.0, edge.MaxSpeedKmh + delta)  // min 5 km/h
        return edge.DistanceM / (effectiveSpeed * 1000.0/3600.0)
    }
    // Fall back to historical speed for this edge's hour/day
    return edge.HistoricalTravelTimeS
}
```

**Dynamic Weight Overlay**: Real-time traffic data is stored in a separate hash map (`trafficCache`) rather than mutating the precomputed CH shortcut edge weights. This is critical because: if edge weights change, CH shortcuts become invalid (a shortcut's weight was computed assuming specific constituent edge weights). The overlay only affects the original edges; shortcut weights remain precomputed. This introduces a small optimality loss when real-time conditions differ significantly from the CH precomputed weights, but in practice < 5% degradation in route quality (because major highways — where traffic matters most — have many shortcut edges redundantly, and the overlay correctly adjusts them via original edge weights that the shortcuts traverse).

**Path Unpacking**: The CH query returns a set of original + shortcut edges. Shortcut edges must be recursively unpacked to recover the actual road-level path for polyline generation. Unpacking is O(path length) after the query.

**Interviewer Q&A**:

Q1: CH preprocessing takes hours. How do you handle daily road network updates (new roads, closures)?
A: We use a two-level approach. (1) Minor updates (speed limits, one-way changes): We maintain a "weight override" map (edge_id → new_weight) loaded on top of the existing CH. No reprocessing needed; the override is applied during query. (2) Major structural updates (new road, removed road): These invalidate CH shortcuts. We trigger an incremental CH rebuild for the affected geographic partition (city-district level). Using the RoutingKit library's incremental CH update algorithm, re-contracting 1% of affected nodes takes ~5 minutes (vs. hours for full rebuild). (3) Full CH rebuild: Triggered weekly or after large OSM imports; runs on a dedicated Spark cluster offline.

Q2: Your dynamic weight overlay introduces suboptimality for shortcuts. How large is the accuracy impact?
A: Theoretically, if a CH shortcut edge bypasses a road with an updated speed, the shortcut's stored weight becomes stale. Analysis: CH shortcuts are densest in high-rank areas (major highways), which are also where real-time traffic matters most. Studies (Geisberger et al., 2008) show that CH with time-dependent weights (TDCH) has < 3% route quality loss compared to optimal time-dependent Dijkstra for typical urban networks. Our traffic overlay updates edge weights every 5 minutes; the stale window is at most 5 minutes of potential suboptimality. For the vast majority of queries (short urban trips < 10 minutes), the overlay is highly accurate because shortcut edges are rare in the low-rank dense urban graph.

Q3: How do you handle directed roads correctly? A one-way street from A to B must not allow routing from B to A.
A: Each physical road segment is stored as two directed edges: A→B and B→A. One-way streets have only the legal direction edge; the reverse edge is omitted from the graph. CH respects directionality: contraction only adds shortcut u→v if the witness path (shortest through w) is itself directional. The edge `Flags` field includes a `OneWay` bit. Map data is sourced from OpenStreetMap where `oneway=yes` tags are well-maintained. During graph loading, the graph builder reads OSM's `oneway` attribute and creates the appropriate set of directed edges.

Q4: How does your CH handle a ferry connection or toll road that the rider wants to avoid?
A: Avoidance constraints are implemented as edge exclusions. Before running the CH query, the service checks the rider's preferences (stored in trip request): avoid tolls, avoid highways, prefer eco-route (minimize distance, not time). Excluded edges are assigned an infinite travel time in the edge weight function. Since CH shortcuts may pass through toll roads, this could exclude valid shortcuts. For user-preference-driven avoidance, we fall back to bidirectional A* (not CH) — because CH cannot correctly handle edge exclusions without re-preprocessing. Bidirectional A* with avoidance constraints on a city graph runs in < 100 ms, acceptable for this less-common path.

Q5: What happens to routing quality during a major traffic incident (highway closed, large accident)?
A: A highway closure is detected via: (1) Third-party traffic incident API (HERE/Google Maps Real-Time Traffic). (2) Dramatic drop in probe speed for affected edges (speed drops from 100 km/h to 0). The Traffic Ingest Pipeline updates the affected edge weights to `MaxFloat32` (effectively infinity) in the Redis traffic cache. The ETA Service's weight overlay returns `MaxFloat32` for these edge IDs; A* / CH naturally routes around them. If a CH shortcut passes through the closed road, the shortcut's constituent original edges are individually checked — the shortcut resolves to infinite cost, forcing the algorithm to find alternative paths. Recovery: when incident clears, probe speeds normalize → Redis weights updated → next ETA request routes correctly again. Maximum delay: 5 minutes (Traffic Ingest cache refresh interval).

---

### 6.2 Real-Time Traffic Incorporation

**Problem It Solves**: Base edge weights (historical averages) are accurate for average conditions but fail during rush hour, accidents, events, and weather. Incorporating real-time driver probe data makes ETAs significantly more accurate during these high-error periods. The challenge is aggregating 562,500 GPS points/second into meaningful per-edge speed estimates with < 5-minute lag.

**Approaches Comparison**:

| Source | Coverage | Latency | Accuracy | Cost |
|---|---|---|---|---|
| **Driver probe data (own fleet)** | Roads with active drivers | ~5 min | High (real drivers) | Free (already collected) |
| **Third-party traffic API (HERE, Google)** | All roads | < 5 min | High | $$$$ (per query) |
| **Incident reports (manual)** | Major incidents only | Variable | Categorical (closed/open) | Free (HERE) |
| **Historical patterns only** | All roads | Offline | Moderate | Free |
| **Satellite/aerial imagery** | Research only | Hours | Limited | Research |

**Selected: Driver Probe Data (primary) + Third-Party API (secondary)**

**Implementation Detail — Traffic Ingest Pipeline**:

```python
# Flink streaming job: location-updates → edge speeds

class TrafficAggregationJob(FlinkStreamingJob):

    AGGREGATION_WINDOW_S = 300   # 5-minute tumbling window
    MIN_PROBES_FOR_UPDATE = 3    # Need at least 3 observations per edge per window

    def process_stream(self, location_stream):
        return (
            location_stream
            # Filter: only GPS points with speed data and on active trips
            .filter(lambda p: p.speed_kmh > 0 and p.trip_id is not None)
            # Map GPS point to (edge_id, speed_kmh)
            .map(self.map_to_edge_speed)  # uses road-snap data
            # Key by (edge_id, city_id)
            .key_by(lambda x: (x.edge_id, x.city_id))
            # Tumbling window: aggregate speeds per edge per 5 min
            .window(TumblingEventTimeWindows.of(Time.seconds(300)))
            .aggregate(SpeedAggregator())
            # Filter out low-confidence edges
            .filter(lambda agg: agg.probe_count >= self.MIN_PROBES_FOR_UPDATE)
            # Compute speed delta vs. historical baseline
            .map(self.compute_delta)
            # Write to Redis
            .sink(RedisSink())
        )

    def map_to_edge_speed(self, gps_point):
        # Road-snap: find which edge the GPS point is on
        # Uses a cached reverse index: H3 cell → nearby edges
        edge_id = road_snap_cache.snap(gps_point.lat, gps_point.lng,
                                        gps_point.heading, gps_point.city_id)
        return EdgeSpeedObservation(
            edge_id=edge_id,
            city_id=gps_point.city_id,
            speed_kmh=gps_point.speed_kmh,
            timestamp=gps_point.recorded_at
        )

class SpeedAggregator:
    def aggregate(self, window: List[EdgeSpeedObservation]) -> EdgeSpeedAggregate:
        speeds = [o.speed_kmh for o in window]
        # Use harmonic mean (better for speeds — handles congestion correctly)
        harmonic_mean = len(speeds) / sum(1/s for s in speeds if s > 0)
        return EdgeSpeedAggregate(
            edge_id=window[0].edge_id,
            city_id=window[0].city_id,
            mean_speed_kmh=harmonic_mean,
            probe_count=len(speeds),
            window_end=max(o.timestamp for o in window)
        )

    def compute_delta(self, agg: EdgeSpeedAggregate) -> EdgeSpeedDelta:
        # Look up historical baseline for this edge at current hour/day
        hist = db.get_baseline(agg.edge_id, current_hour(), current_day_of_week())
        delta = agg.mean_speed_kmh - hist.avg_speed_kmh if hist else 0.0
        return EdgeSpeedDelta(
            edge_id=agg.edge_id,
            city_id=agg.city_id,
            speed_delta_kmh=delta,
            absolute_speed_kmh=agg.mean_speed_kmh,
            probe_count=agg.probe_count,
            confidence=min(1.0, agg.probe_count / 20.0)  # 20 probes = full confidence
        )
```

**Redis write (after Flink)**:
```python
def write_to_redis(deltas: List[EdgeSpeedDelta], city_id: int):
    pipe = redis.pipeline()
    for delta in deltas:
        # Update edge speed
        pipe.hset(f'traffic:{city_id}:edge_speeds',
                  str(delta.edge_id), delta.absolute_speed_kmh)
        # Track probe counts for confidence
        pipe.hset(f'traffic:{city_id}:edge_probe_count',
                  str(delta.edge_id), delta.probe_count)
    pipe.expire(f'traffic:{city_id}:edge_speeds', 600)  # 10 min TTL
    pipe.expire(f'traffic:{city_id}:edge_probe_count', 600)
    pipe.execute()
```

**Coverage gap handling**: Our fleet doesn't drive all roads. For roads with no probe data in the last 5 minutes, the ETA Service falls back to historical speed for the current time window. For low-confidence roads (< 3 probes), we blend: `effective_speed = confidence × probe_speed + (1 - confidence) × historical_speed`.

**Third-party traffic integration**: HERE Maps Real-Time Traffic API (or Google Maps Roads API) provides per-segment speed data for major arterials. We query this API every 5 minutes for the top-1,000 highest-traffic edges per city (where our probe coverage may be sparse). Results are merged with probe data, preferring probe data when confidence ≥ 0.5.

**Interviewer Q&A**:

Q1: How do you handle the road-snap step in the Flink pipeline without adding latency?
A: Road-snapping in the Flink pipeline is done with a pre-built H3 reverse index: `h3_cell (resolution 12) → [nearby edge_id list]`. This is loaded into each Flink task manager's JVM heap from S3 at startup (~200 MB per city). The snap for each GPS point: (1) Compute H3 cell at resolution 12. (2) Look up candidate edges. (3) Score each candidate by angular similarity between driver heading and edge direction + perpendicular distance. (4) Return best edge. This takes ~10 µs per GPS point — well within Flink's processing capacity.

Q2: How does the harmonic mean differ from arithmetic mean for speed aggregation, and why is it better?
A: Arithmetic mean of speeds is biased toward high speeds. If 5 probes show 100 km/h and 5 probes show 10 km/h (half-congested), arithmetic mean = 55 km/h — optimistic. Harmonic mean = 10 / (5/100 + 5/10) = 10 / (0.05 + 0.5) = 18.2 km/h — more conservative and more accurate for travel time (because travel time = distance/speed, and averaging travel times is the inverse of averaging speeds). The harmonic mean is the correct statistic for "average speed over a distance". Empirically, harmonic mean reduces mean absolute ETA error by 7% compared to arithmetic mean in congested urban environments.

Q3: Traffic data has 5-minute update lag. How do you handle a sudden traffic event (major accident) that blocks a highway?
A: For the first 5 minutes: our probe data shows a rapid speed decrease on the affected edges (active drivers report 0–5 km/h instead of 100 km/h). If probe_count ≥ 3 (minimum), the Flink window emits an intermediate result (using a session window or a smaller 1-minute sliding window for anomaly detection). We also subscribe to third-party traffic incident webhooks (HERE provides an incident stream). On incident receipt: immediately write `speed=0` to Redis for affected edge IDs, bypassing the 5-minute window. ETA Service picks up the change within 100 ms. The incident webhook integration reduces time-to-detect from 5 minutes to < 1 minute for major incidents.

Q4: How do you handle the ETA Service reading stale traffic data while a Flink update is in progress?
A: The ETA Service reads from Redis at query time (per-edge hash lookup). The Flink pipeline writes atomically per-edge with HSET — a single edge weight is updated atomically; there is no "partial update" state visible to ETA Service readers. There is a 5-minute staleness window where old weights are used, which is acceptable. We track the `last_traffic_update_at` Redis key per city — if it's > 10 minutes old, the ETA Service logs a "stale traffic data" warning and returns ETAs with lower confidence scores to callers.

Q5: What's the minimum driver density needed for probe data to be useful?
A: Analysis: An edge must have ≥ 3 probes in 5 minutes to qualify for update. With 1.5 M drivers in 500 cities → 3,000 avg drivers/city. A city's road network has ~10,000 edges with significant traffic. If each edge needs 3 probes/5 min, and each driver covers ~5 edges/5 min (at 30 km/h, ~250 m/min, ~1 edge per 200 m), total probes/city/5 min = 3,000 drivers × 5 probes = 15,000 probes for 10,000 edges → 1.5 probes/edge/5 min on average. Just below the threshold. In practice, traffic concentrates on major roads where driver density is 10–30× higher. Small cities (< 300 drivers) supplement heavily with third-party API. We dynamically adjust `MIN_PROBES_FOR_UPDATE` to 1 for cities with low coverage.

---

### 6.3 ML-Based ETA Correction

**Problem It Solves**: Graph-based routing (even with real-time traffic) systematically underestimates ETA in certain conditions: (1) Parking/lane search time at destination (not in graph). (2) Intersection signal delay (graph assumes average green phase). (3) Pedestrian crossings, school zones, road work zones. (4) Weather effects (rain reduces speed 10–20% vs. normal). (5) Event-day congestion patterns not captured in 7-day history. The ML correction layer learns these systematic biases from historical prediction errors.

**Approaches Comparison**:

| Approach | Accuracy Gain | Latency | Complexity | Explainability |
|---|---|---|---|---|
| No correction (graph only) | Baseline | 0 ms | None | High |
| Rule-based multipliers (e.g., +10% for rain) | +5% MAPE improvement | 0.1 ms | Low | High |
| **Linear regression on residuals** | +8% MAPE improvement | 0.5 ms | Low | High |
| **Gradient Boosted Trees (XGBoost)** | +12% MAPE improvement | 1–3 ms | Medium | Medium |
| Neural network (LSTM) | +15% MAPE improvement | 10–50 ms | High | Low |
| Ensemble (XGBoost + context rules) | +13% MAPE improvement | 2–4 ms | Medium | Medium |

**Selected: XGBoost per city (gradient boosted trees)**

**Feature Engineering**:

```python
# Features for ML correction model
def build_eta_features(request: ETARequest, graph_eta_s: int,
                        traffic_level: str, city: City) -> dict:
    now = datetime.now(city.timezone)
    return {
        # Graph output
        'graph_eta_seconds':     graph_eta_s,
        'graph_distance_km':     request.distance_km,
        'avg_speed_kmh':         request.distance_km / (graph_eta_s / 3600),

        # Temporal features
        'hour_of_day':           now.hour,
        'day_of_week':           now.weekday(),     # 0=Monday
        'is_weekend':            now.weekday() >= 5,
        'is_rush_hour':          now.hour in [7,8,9,17,18,19],
        'minutes_since_midnight': now.hour*60 + now.minute,

        # Route characteristics
        'route_type':            classify_route(request),   # HIGHWAY/URBAN/MIXED
        'turns_per_km':          request.turns / request.distance_km,
        'traffic_lights_per_km': count_traffic_lights(request.path) / request.distance_km,
        'highway_fraction':      highway_edge_seconds / graph_eta_s,

        # Traffic features
        'traffic_level_encoded': {'NONE':0,'LIGHT':1,'MODERATE':2,'HEAVY':3}[traffic_level],
        'avg_edge_confidence':   mean(probe_confidence for edge in request.path),
        'low_probe_edge_frac':   fraction of edges with probe_count < 3,

        # Contextual
        'weather_rain_mm_h':     weather_api.get_rain_rate(request.origin_lat, request.origin_lng),
        'special_event_nearby':  check_event_calendar(request.origin, now),
        'surge_multiplier':      get_surge(request.city_id, request.origin),  # proxy for demand

        # City features (one-hot encoded during training)
        'city_id':               request.city_id,
        'city_region':           city.region,  # US_WEST, EU, ASIA, etc.
    }
```

**Model Training**:

```python
# Offline training pipeline (runs daily)
def train_eta_correction_model(city_id: int):
    # Load last 90 days of accuracy_log for this city
    df = db.query("""
        SELECT e.*, t.actual_duration_s, t.trip_id
        FROM eta_accuracy_log e
        JOIN trips t ON e.trip_id = t.trip_id
        WHERE e.city_id = %s
          AND e.prediction_type = 'TRIP_ETA'
          AND t.status = 'COMPLETED'
          AND e.predicted_at > NOW() - INTERVAL '90 days'
    """, city_id)

    # Target: actual_duration / graph_predicted = correction factor
    df['correction_factor'] = df['actual_eta_s'] / df['graph_eta_seconds']
    # Cap extreme values (trip took 10× longer = outlier, not model signal)
    df = df[df['correction_factor'].between(0.5, 3.0)]

    X = build_features_batch(df)
    y = df['correction_factor']

    model = xgb.XGBRegressor(
        n_estimators=200,
        max_depth=6,
        learning_rate=0.05,
        subsample=0.8,
        colsample_bytree=0.8,
        objective='reg:squarederror',
        eval_metric='mae',
        n_jobs=4
    )
    model.fit(X, y, eval_set=[(X_val, y_val)], early_stopping_rounds=20, verbose=False)

    # Evaluate: MAPE improvement vs. baseline (correction_factor = 1.0)
    baseline_mape = mean_absolute_percentage_error(y_val, ones(len(y_val)))
    model_mape    = mean_absolute_percentage_error(y_val, model.predict(X_val))
    log.info(f"City {city_id}: baseline MAPE {baseline_mape:.2%} → model MAPE {model_mape:.2%}")

    # Save model to S3 (versioned)
    model.save_model(f's3://eta-models/city-{city_id}/v{today}.json')
    update_active_model(city_id, today)
```

**Online Inference** (in ETA Service pod, after graph computation):

```python
def apply_ml_correction(features: dict, city_id: int) -> float:
    model = model_registry.get(city_id)  # loaded in-process at startup (XGBoost ~5 MB)
    if model is None:
        return 1.0   # no model for this city — no correction

    X = numpy.array([list(features.values())])
    correction = float(model.predict(X)[0])
    # Clip to sane range (never predict more than 2× or less than 0.5×)
    correction = max(0.5, min(2.0, correction))
    return correction

def compute_final_eta(graph_eta_s: int, correction: float) -> int:
    return round(graph_eta_s * correction)
```

**Model Update Flow**: Daily retraining job (Airflow) runs for each city in parallel. New model uploaded to S3. ETA Service pods watch for model updates via S3 event notification → hot-reload the model in memory (atomic swap of the model registry entry). No restart required. Rollback: if new model increases city-level MAPE by > 5% in the first 2 hours (monitored by accuracy pipeline), auto-rollback to previous model version.

**Interviewer Q&A**:

Q1: The ML model gives a correction factor of 1.8× for a 10-minute graph ETA — that's 18 minutes. How do you build rider trust in such a large correction?
A: Large corrections occur in specific contexts: rush hour + heavy rain + high-traffic route. In these cases, the correction is accurate — historical data shows these conditions add 70–80% to graph ETAs. We communicate uncertainty to the rider: when `confidence < 0.7`, the UI shows "18–22 min" (a range) rather than a point estimate. The range is computed as `(correction × graph_eta) ± 2 × residual_std_from_model_eval`. Showing an honest range builds more trust than a falsely precise single number that frequently misses.

Q2: What features are the most important predictors in the XGBoost model? How do you measure this?
A: XGBoost's feature importance (`gain` metric — average gain per split using that feature) typically ranks: (1) `is_rush_hour` (45% gain) — biggest single factor in ETA error. (2) `traffic_level_encoded` (20%). (3) `weather_rain_mm_h` (10%). (4) `graph_eta_seconds` (8%) — longer trips have larger absolute errors. (5) `traffic_lights_per_km` (6%). (6) `special_event_nearby` (5%). (7) Others (6%). SHAP values are computed monthly for model interpretability reports — used to explain to city operations teams why ETAs are systematically off in specific neighborhoods.

Q3: How do you detect model drift — when the model's correction factor is no longer accurate due to changed road conditions?
A: The Accuracy Monitoring Pipeline computes rolling MAPE per city every hour. Alert conditions: (1) City MAPE > 20% for 2 consecutive hours → P2 alert → ML team investigates. (2) Specific route type (e.g., highway) MAPE > 30% → likely infrastructure change (new highway opened, lane closures). (3) Correction factor distribution shifts significantly (K-S test with p < 0.05 comparing last week to last 7 days) → trigger emergency retraining. Emergency retraining uses only the last 7 days of data (vs. 90 days normally) to quickly adapt to regime changes.

Q4: A major infrastructure change happens (a new highway opens). How long until the ETA model adapts?
A: Two complementary timelines: (1) Road graph update: The new highway appears in OSM within 1–7 days of opening (contributed by mappers). Our Map Graph Updater picks up the OSM diff, adds the new edges, rebuilds the affected city's CH (< 6 hours), deploys new graph. From this point, the routing algorithm uses the new highway. (2) ML model adaptation: The model was trained on pre-highway data. Drivers start using the new highway; probe data flows in immediately. The model re-trains nightly with the latest 90 days but the highway data is sparse at first. After ~7 days (enough probe data and trip completions), the correction factors for new-highway routes normalize. Temporary mitigation: lower the ML correction weight for edges added in the last 7 days (`is_new_edge` feature with coefficient that reduces correction magnitude).

Q5: How would you extend the ML model to predict not just ETA but ETA distribution — returning a confidence interval?
A: Replace the XGBoost regression model with a quantile regression model. XGBoost supports quantile regression via `objective='reg:quantileerror', quantile_alpha=[0.1, 0.5, 0.9]` — predicting the 10th, 50th, and 90th percentiles of the correction factor. The ETA Service returns: `{ "eta_p50": 18, "eta_p10": 15, "eta_p90": 24 }`. The rider app displays "18 min (typically 15–24 min)". Alternatively, Gaussian Process regression gives full posterior distributions but is much slower (O(N²) per inference). Quantile XGBoost inference is equally fast as point estimation (< 3 ms) and is the practical choice.

---

## 7. Scaling

### Horizontal Scaling

**ETA Pods are stateful** (each holds a city graph in memory). Horizontal scaling requires correct routing:

| Mechanism | Description |
|---|---|
| Consistent hashing on city_id | Load balancer routes all queries for `city_id=X` to the same pod (or its replicas). Prevents redundant graph loading. |
| Pod replicas per city | High-traffic cities (NYC, London, Mumbai) get 3–5 pod replicas; small cities share a pod. |
| City assignment map | Stored in ZooKeeper/etcd: `city_id → pod_group`. Updated when pods scale in/out. |
| Graph loading at startup | Pod reads city assignment from etcd on start, loads graph from S3, warms up Redis traffic cache, starts serving. Startup time: ~60 s for a large city. |
| Kubernetes StatefulSet | Pods use StatefulSet for stable hostnames; consistent hash routes by pod hostname. |

| City Traffic | Pods Needed | Memory per Pod | CPU Allocation |
|---|---|---|---|
| Large city (NYC, London, SF) | 5 replicas | 16 GB RAM (5 GB graph + 2 GB CH + 2 GB ML models + heap) | 4 vCPU |
| Medium city (Denver, Dublin) | 2 replicas | 8 GB RAM | 2 vCPU |
| Small city (< 100K trips/day) | 1 pod (shared with 5 other small cities) | 4 GB RAM | 1 vCPU |

**Overflow routing**: If a pod's CPU exceeds 80%, it signals the load balancer to shed new requests to a sibling replica (not a different city pod). The sibling must also have the same city graph loaded — hence, minimum 2 replicas per active city for load balancing.

### DB Sharding

**eta_accuracy_log**: Sharded by `city_id` in Citus (same as other trip-related tables). City-scoped analytics queries never require cross-shard joins.

**road_speed_baselines**: Not sharded — this is a reference table (low write volume: updated once per 5-minute window per edge) read by all ETA pods. Hosted on a dedicated PostgreSQL instance with read replicas. 100 M edges × 168 time slots × 8 B = ~135 GB total. Fits on a single 1 TB SSD-backed PostgreSQL instance. ETA pods cache frequently accessed baselines in process memory (LRU cache, 512 MB).

### Replication

| Component | Replication | Failover |
|---|---|---|
| ETA Service pods | K8s StatefulSet with 2+ replicas per city; anti-affinity rules spread to different nodes | K8s replaces failed pod; consistent hash routes around failed pods in < 5 s |
| Road graphs (S3) | S3 cross-region replication | S3's 11-nines durability; regional failover reads from DR region |
| Traffic Redis cache | Redis Sentinel: 1 master + 2 replicas | Sentinel auto-promotes in < 10 s; ETA pods fall back to historical weights during gap |
| PostgreSQL (accuracy log) | Primary + 1 sync replica + cross-region async | Patroni failover < 30 s |

### Caching Strategy

| Data | Cache | TTL | Eviction |
|---|---|---|---|
| Graph edges + CH shortcuts | In-process JVM/Go heap | Infinite (pod lifetime) | Pod restart |
| Historical speed baselines | In-process LRU (512 MB) | Until pod restart | LRU eviction |
| Real-time traffic weights | Redis Hash per city | 10 min auto-expire | TTL; overwritten on update |
| ML correction models | In-process (5 MB per model per city) | Reload on new S3 version | Hot-reload atomic swap |
| Map-match (road-snap) reverse index | In-process (200 MB per city) | Pod lifetime | Pod restart |
| ETA result cache (for identical requests) | Redis KV: `hash(origin_node, dest_node, hour) → (eta, polyline)` | 2 min | TTL |

**ETA result cache**: Many fare estimate requests have identical routes (common origin-destination pairs). A 2-minute cache with the key = `hash(origin_node_id, dest_node_id, 10min_window)` avoids redundant A* computations. Cache hit rate in production: ~15–25% (saves ~4,000 routing computations/s at peak).

### Interviewer Q&A

Q1: An ETA pod crashes while serving queries for New York. How quickly do riders notice?
A: NYC has 5 replica pods. The load balancer detects the pod failure via health check in < 5 seconds (gRPC health check protocol, 1-second interval). Remaining 4 pods immediately absorb the traffic — at 80% capacity each, this is sustainable. K8s starts a replacement pod which takes ~60 seconds to load the NYC graph and become ready. During those 60 seconds, the 4 remaining pods handle 100% of NYC traffic. If all 5 pods were at 95% capacity (rare), incoming requests may see latency spikes during the 60-second window. Mitigation: min_replicas = 5 with CPU target 60% (not 80%) for top-10 highest-traffic cities.

Q2: How does the ETA result cache handle dynamic traffic changes? A cached ETA might be 5 minutes old when traffic changed.
A: The cache key includes a `traffic_window` component: `floor(current_unix_time / 300)` — a 5-minute bucket. When the traffic weight cache is refreshed by Flink (every 5 minutes), the current 5-minute window bucket changes, effectively invalidating all cached ETAs for the new window. The cache TTL (2 minutes) is shorter than the traffic refresh interval (5 minutes), ensuring no ETA is served that was computed before the current traffic data was published. In the worst case, a cached ETA is from the start of the current 2-minute window, which used the latest 5-minute traffic update — acceptable staleness.

Q3: How do you handle a city that grows from 100K trips/day to 500K trips/day (rapid growth market)?
A: City-level scaling is controlled by the pod assignment map in etcd. The capacity planning service monitors city-level request rates and automatically adjusts replica count recommendations. When NYC-equivalent cities grow: (1) Operator or auto-scaler increases `replicas: 5 → 10`. (2) K8s StatefulSet adds 5 new pods; each loads the graph from S3 in 60 s. (3) Consistent hash redistribution: some city_id hash ranges shift to new pods. (4) Old pods' graphs are still valid — no data migration needed. The main operational concern is S3 transfer costs when loading large graphs at startup; mitigated by caching compressed graphs on EBS volumes (faster local reload on restart).

Q4: Global routing is needed for an edge case — a rider near a city boundary whose pickup is in City A but the optimal route goes through City B. How is this handled?
A: Cross-city routing is uncommon but real (e.g., Oakland to San Francisco through Berkeley). City graphs overlap at boundaries — we include a 5 km buffer zone of neighboring city's road data when building each city's CH graph. This ensures routes that briefly transit adjacent cities are correctly modeled without requiring cross-pod queries. For trips explicitly spanning cities (e.g., NYC to Philadelphia), a separate "regional" graph is built covering both metro areas, and these trips are routed through a dedicated regional ETA pod.

Q5: How would you scale from 500 cities to 10,000 cities (expanding to all mid-sized cities globally)?
A: At 10,000 cities, most are small (< 10K trips/day). Scaling strategy: (1) Small cities share pods — one pod with 8 GB RAM can hold graphs for 5–10 small cities simultaneously. (2) Lazy graph loading: graph is loaded on the first request for a city (with a 60-second cold start latency), then kept warm for 1 hour. Rarely-active cities are evicted from memory after 1 hour of no queries. (3) Tiered infrastructure: top 500 cities → dedicated pods with always-warm graphs. Remaining 9,500 → shared pods with LRU graph eviction. (4) CH preprocessing at scale: distributed CH build on Spark for new cities (1 city/hour on a 50-node Spark cluster). 10,000 cities → 10,000 / 1 per hour = ~417 days if sequential, but parallelized → < 10 days total for the full batch on launch.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| ETA pod crash | ETA unavailable for affected city for 60 s | K8s liveness probe + gRPC health check | Replicas absorb traffic; K8s restarts pod and reloads graph |
| Redis traffic cache unavailable | ETA uses historical weights only | Redis Sentinel alert; TTL expiry | ETA Service detects missing Redis keys → use historical baselines; log degraded mode |
| S3 graph unavailable | New pod cannot load graph | S3 access failure on startup | Retry with exponential backoff up to 5 min; use last EBS-cached version if available |
| Flink Traffic Ingest job fails | Traffic weights go stale (> 10 min) | Kafka consumer lag alert; Redis TTL auto-expires | Weights auto-expire from Redis after 10 min; ETA falls back to historical; Flink auto-restarts on checkpoint restore |
| ML model load fails | Correction factor = 1.0 (no correction) | Model registry lookup failure | Log and metric; continue with graph-only ETA (acceptable degradation, ~10% higher MAPE) |
| PostgreSQL accuracy log unavailable | Accuracy not recorded | gRPC error | Trip accuracy logging is async and non-blocking; logged to dead-letter queue; replayed on recovery |
| Graph data corruption (S3 object corrupt) | Pod crashes on deserialization | Pod crash loop → K8s detects; graph checksum fails | S3 versioning keeps previous graph version; roll back to previous version; alert |
| Third-party traffic API (HERE) unavailable | No external traffic enrichment | HTTP 5xx from HERE client | Fall back to probe-only traffic; log incident; no rider-visible impact for cities with good probe coverage |

### Retries & Idempotency

- **ETA ComputeETA RPC**: Fully deterministic and stateless — same inputs produce same output (given same traffic state). Callers may safely retry on timeout; no side effects.
- **Accuracy log write**: Async, non-critical. Written to a Kafka topic first (`eta-accuracy-events`); PostgreSQL writer consumes from Kafka. If DB write fails, Kafka retains the event for 24 hours for retry.
- **ML model hot-reload**: Atomic pointer swap in the model registry. If new model deserialization fails, the old model pointer is retained (no intermediate bad state).
- **Traffic cache write (Flink → Redis)**: Redis pipeline HSET is idempotent — overwriting the same edge weight twice is harmless.

### Circuit Breakers

```
ETA Service → Redis (traffic weights):
  failure_threshold: 5 errors in 2 s
  open_duration: 30 s
  fallback: use historical speed baselines from in-process LRU cache
  half-open: 1 probe per 10 s

ETA Service → ML Correction Service (if external):
  failure_threshold: 3 errors in 1 s
  open_duration: 60 s
  fallback: correction_factor = 1.0 (no ML correction)
  alert: log metric `ml_correction_disabled = true` per city

ETA Service → HERE Traffic API:
  failure_threshold: 3 timeouts in 10 s
  open_duration: 120 s
  fallback: probe data only; no external enrichment
```

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Owner |
|---|---|---|---|
| `eta.latency_p99` per city | Histogram | > 1.5 s | ETA On-call |
| `eta.latency_p50` per city | Histogram | > 200 ms (degradation) | ETA On-call |
| `eta.requests_per_second` | Counter | < 50% of baseline | ETA On-call |
| `eta.accuracy.mape` per city (rolling 1h) | Gauge | > 20% | ML Team |
| `eta.accuracy.mae_seconds` per city | Gauge | > 120 s | ML Team |
| `eta.traffic_cache.freshness_seconds` | Gauge | > 600 s (stale) | Traffic Infra |
| `eta.traffic_cache.edge_coverage_pct` | Gauge | < 30% of major edges | Traffic Infra |
| `eta.ml_correction_factor.mean` per city | Gauge | > 1.8 or < 0.7 (systematic bias) | ML Team |
| `eta.graph_version.age_hours` per city | Gauge | > 168 h (1 week) | Maps Team |
| `eta.result_cache.hit_rate` | Gauge | < 5% (cache not helping) | Perf Team |
| `eta.ch_query_time_p99` | Histogram | > 50 ms | ETA On-call |
| `eta.pod_graph_load_time_s` | Gauge | > 120 s (slow graph load) | Infra |
| `eta.ml_model_prediction_time_p99` | Histogram | > 20 ms | ML Team |

### Distributed Tracing

Critical trace for a fare estimate ETA request:
```
[Fare Service: ComputeFareEstimate]         total: 380ms
  [ETA Service: ComputeETA gRPC]            total: 45ms
    [Road snap (origin)]                    0.3ms
    [Road snap (dest)]                      0.3ms
    [Historical speed baseline load]        0.5ms (LRU cache hit)
    [Redis traffic cache lookup]            1.2ms (50 edge lookups)
    [CH query (bidirectional)]              8ms (urban 7 km trip)
    [Path unpack + polyline encode]         2ms
    [ML feature extraction]                 1ms
    [XGBoost inference]                     2ms
    [Total ETA Service internal]            ~15ms
  [Network round-trip (same datacenter)]    ~30ms
```

Samples: 20% of requests traced; 100% for requests > 500 ms (tail-based sampling).

### Logging

ETA Service structured log per request:
```json
{
  "level": "INFO",
  "service": "eta",
  "trace_id": "def456",
  "trip_id": "trip-uuid",
  "city_id": 1,
  "origin_lat": 37.7749,
  "origin_lng": -122.4194,
  "dest_lat": 37.3382,
  "dest_lng": -121.8863,
  "request_type": "FARE_ESTIMATE",
  "graph_eta_s": 1240,
  "ml_correction": 1.08,
  "final_eta_s": 1339,
  "distance_km": 7.2,
  "traffic_level": "MODERATE",
  "probe_coverage": 0.74,
  "ch_query_ms": 8,
  "ml_inference_ms": 2,
  "total_ms": 15,
  "graph_version": "2026-04-08T10:00:00Z",
  "origin_node_id": 123456789,
  "dest_node_id": 987654321
}
```

Accuracy log (written asynchronously after trip completion):
```json
{
  "trip_id": "trip-uuid",
  "predicted_eta_s": 1339,
  "actual_duration_s": 1220,
  "absolute_error_s": 119,
  "percentage_error": 9.75,
  "correction_factor_used": 1.08
}
```

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Selected | Alternative | Reason |
|---|---|---|---|
| Routing algorithm | CH with dynamic weight overlay | Bidirectional A* | CH query < 5 ms vs. A* 50–200 ms; satisfies < 50 ms internal latency |
| Graph storage | In-process memory (loaded from S3) | Neo4j / PostGIS | Zero I/O per graph traversal; Neo4j adds 1–10 ms network per hop × 100K hops = impossible |
| Traffic data | Driver probe (primary) + HERE API (secondary) | HERE API only | Probe data is cheaper, real-time, and covers our most-driven roads; HERE supplements low-density roads |
| Traffic aggregation window | 5-minute tumbling window | 1-minute rolling | 1 minute has too few probes per edge (< 3 on average); 5 minute gives statistical significance |
| ML correction model | XGBoost per city | Global LSTM | Per-city XGBoost captures city-specific patterns; LSTM 10–50 ms inference violates < 5 ms requirement; XGBoost 2 ms acceptable |
| Edge weight update strategy | Overlay map (separate from CH) | CH re-preprocessing | CH re-preprocessing takes hours; overlay map updates in < 100 ms; accuracy loss < 5% |
| Accuracy tracking | PostgreSQL with async write | In-memory counters | PostgreSQL enables SQL-based ML feature engineering and historical analysis; async write avoids blocking queries |
| Graph update cadence | Daily OSM diff + weekly full rebuild | Continuous streaming | Continuous OSM updates require streaming CH rebuilds (research-level difficulty); daily diffs capture 99% of changes promptly |
| ETA cache | Redis 2-min cache on (origin_node, dest_node, window) | No cache | 15–25% hit rate saves 4,000 routing computations/s at peak; minimal complexity |
| Pod scheduling | Stateful (city affinity, consistent hash) | Stateless (any pod handles any city) | Stateless requires either graph replication to every pod (2.5 TB total) or remote graph fetch per query (I/O per traversal — unusable) |
| Alternative routes | 3× A* with penalty diversity | Single-path CH + variants | 3× A* is cleaner; CH alternative routes require complex CH modification. 3× A* on < 30 km city routes takes < 50 ms |

---

## 11. Follow-up Interview Questions

Q1: How does the ETA Service handle a trip that crosses multiple cities (e.g., NYC to Philadelphia)?
A: City-crossing trips are identified at trip creation: if origin and destination are in different cities (or the straight-line distance exceeds the city's bounding box), they are routed to a "regional" ETA pod that holds a multi-city graph. We build regional graphs for common cross-city corridors (NYC-NJ-PA, SF Bay Area, LA County) using the same CH pipeline but with a larger geographic scope. The regional graph is ~5× larger than a city graph (~25 GB RAM per regional pod). A dedicated regional ETA pod handles these queries, routed by the load balancer when `city_id` differs between origin and destination.

Q2: Describe how you would incorporate real-time accident data into ETA calculations.
A: Accidents are detected via: (1) Third-party incident streams (HERE, Google Incidents API) — provides `{lat, lng, severity, affected_road_segments}`. (2) Rapid probe speed drops on specific edges (< 5 km/h for edges normally at 80+ km/h for > 3 consecutive probes in 2 minutes) — anomaly detection in Flink. On incident detection: the Traffic Ingest Pipeline marks affected edge_ids with `speed=0` (full closure) or `speed=20%_of_normal` (partial blockage) in Redis. ETA Service picks up the change within < 1 minute (combining the 5-minute Flink window with the real-time incident webhook). Incident clearance is detected by speed normalization or via an incident-cleared webhook.

Q3: How do you test ETA accuracy improvements from new model versions or traffic sources without exposing all users to the change?
A: Shadow testing: run the new model/traffic source in parallel with the production system. For each ETA request: compute production ETA (current system) + shadow ETA (new system). Store both in the accuracy log. Compare actual trip duration against both predictions after trips complete. No rider impact — shadow predictions are never sent to the client. After 24 hours with ≥ 10,000 trip samples, if shadow MAPE is lower than production MAPE by > 5% with statistical significance (paired t-test, p < 0.05), gradually roll out the new system (1% → 5% → 25% → 100%).

Q4: What is the accuracy impact of using CH with a 5-minute-old traffic overlay during a rapidly evolving rush-hour congestion event?
A: During rapidly evolving congestion: probe speeds on affected edges decrease over 15–30 minutes. Our Flink window emits an update every 5 minutes. Across the first 5-minute window, the ETA uses pre-congestion weights → systematic underestimate of 10–30% for affected routes. After the first Flink cycle updates the Redis cache, ETAs improve significantly. Net impact on MAPE: rush-hour congestion events contribute ~3% additional MAPE points during the 5-minute onset window. Mitigation: use a 1-minute sliding window for edges above a "congestion anomaly" threshold (speed drops > 50% in 1 minute), and a 5-minute tumbling window for steady-state. This hybrid reduces the onset lag from 5 minutes to 1 minute for sudden incidents.

Q5: How does the ETA Service handle a completely new city with no historical speed data and no probe vehicles yet?
A: Cold-start city with zero historical data: (1) Use OSM's `maxspeed` tags as edge weights. Where `maxspeed` is missing (common in OSM), use road class defaults (MOTORWAY=100 km/h, TRUNK=80, PRIMARY=60, SECONDARY=50, RESIDENTIAL=30). (2) Apply global default correction factors by city type (e.g., dense European city: correction = 1.4× during peak hours). (3) No ML model for < 30 days of data — correction_factor = 1.0. (4) After first 30 days of operation (≥ 1,000 completed trips), a city-specific model is trained. Accuracy during cold-start is worse (~25% MAPE) but improves rapidly with data accumulation. Cities are publicly launched after 14 days of internal testing to warm the speed baselines.

Q6: How would you compute ETA for a shared (pool) ride where the driver must pick up two riders at different locations before any dropoff?
A: Multi-waypoint ETA uses the `ComputeMultiWaypointETA` RPC. For a 4-waypoint pool trip (origin → pickup_A → pickup_B → dropoff_A → dropoff_B): we compute 4 sequential A* queries (each leg: origin→A, A→B, B→dropA, dropA→dropB). The total ETA is the sum of leg ETAs. For matching compatibility (deciding whether to add a second pool rider to an in-progress trip), we compute: (1) current_driver_pos → new_pickup → original_dropoff (with detour). (2) Compare detour_time against the compatibility threshold (< 5 minutes added for existing passenger). This compatibility ETA computation happens in the Matching Service's pool-matching path and must complete in < 200 ms — feasible since it's 2 short A* queries.

Q7: What are the ETA accuracy implications of GPS point sparsity during battery-optimization mode (8-second updates)?
A: During highway driving at 8-second intervals, driver position moves ~222 m between updates. The en-route ETA recalculation at 30-second intervals uses the driver's last known position + estimated current position via dead reckoning (speed × time since last GPS). Dead-reckoning error over 30 seconds at 100 km/h = 833 m. The ETA computation uses this estimated position as the new origin. Position error of 833 m corresponds to ~30 seconds of additional routing uncertainty at 100 km/h. ETA impact: ±30 seconds on a 30-minute trip = ±1.7% relative error. This is acceptable and is reflected in the confidence interval returned to the rider.

Q8: How does the ETA Service integrate with third-party navigation apps? A driver may use Google Maps or Waze instead of the Uber app.
A: The ETA Service is entirely server-side. Whether the driver uses the Uber app or Google Maps for navigation, the ETA calculation is based on: (1) The driver's GPS position (from the Location Tracking Service). (2) The optimal route computed by our routing engine (independent of what the driver is following). When a driver deviates from our recommended route (using a different navigation app), the en-route ETA recalculation detects this: `current_driver_position` is far from the expected position on our route. We re-run A* from the driver's actual current position, computing the ETA for the new (deviation-corrected) path. The rider's ETA is automatically updated. No integration with Google Maps or Waze is needed — GPS position is the single source of truth.

Q9: How would you design a "surge-aware ETA" — where ETA increases during high-demand because drivers are slower to navigate congested pickup areas?
A: Current ETA is purely based on road speed (probe data + historical). During surge, additional factors not captured in probe data include: (1) Parking/waiting time at busy pickup zones (concerts, bars). (2) Slower pedestrian crossings as crowds spill into streets. Proposed addition: the ML correction features include `surge_multiplier` and `special_event_nearby`. If these features are correlated in training data with longer actual trip times (they are — analysis shows 1.8× surge correlates with +8% actual ETA), the model learns to apply a larger correction factor during surge. Additionally, we can add an explicit `pickup_zone_density` feature: count of active driver pick-ups and drop-offs in the H3 cell around the pickup location in the last 5 minutes. This captures parking-area congestion not reflected in road speeds.

Q10: How do you handle ETA computation for a ferry or bridge that charges tolls and has scheduled departure times?
A: Ferries are special edges in the road graph with: (1) Scheduled departure times (hourly or bi-hourly). (2) Travel time = sailing time + wait time until next departure. These are modeled as "ferry edges" with a time-dependent weight function: `weight(t) = sailing_time + time_until_next_departure(t)`. Time-dependent CH (TDCH) can handle this but is significantly more complex to precompute. Simpler approach: ferry edges are handled as special-case routes. Before running CH, we check if the optimal path includes a ferry edge (known by edge type flag). If yes, we run a time-dependent Dijkstra for the ferry portion and CH for the road portions, splicing the results. Ferry departure schedules are stored in a separate lookup table, updated from transit data sources (GTFS ferry schedules).

Q11: How would you build a model that predicts ETA not just for the current moment but for a scheduled ride 2 hours from now?
A: Future-time ETA: instead of using current traffic weights, use the historical speed baselines for the scheduled time window (e.g., "Tuesday 8:30 AM" baseline speeds). The ML correction features include `hours_until_departure` and the scheduled `hour_of_day`/`day_of_week`. For scheduled rides, we call the ETA Service with `scheduled_departure_time` parameter. The ETA pod uses the historical baseline for that time window from `road_speed_baselines` (queried from PostgreSQL or preloaded in the LRU cache). ML correction uses the historical average correction factor for that time window from training data. ETA uncertainty is higher for future predictions (prediction interval widens); we return `confidence = 0.6` for predictions > 30 minutes ahead.

Q12: What would you change in this architecture to support scooters or bikes, where road routing is fundamentally different?
A: Separate road graph for micro-mobility: (1) OSM provides dedicated cycleway, footway, and bicycle path data. Build a city graph using only bikeable edges (OSM `bicycle=yes` or `access` tags). (2) Speed model: e-scooter max 25 km/h; bike 15–25 km/h; no traffic-jam behavior (lane-splitting, sidewalk routing). (3) No CH needed: bike trip graphs are much smaller (< 5 M edges per city for cycling paths only); bidirectional A* completes in < 10 ms. (4) Separate ETA pods for micro-mobility (small memory footprint). (5) No ML correction model initially (no historical data); add after 30 days of trip data. (6) Battery optimization is irrelevant for non-motorized scooters (GPS updates can be frequent without battery concern).

Q13: How would you measure and improve ETA accuracy specifically for airport pickup/dropoff?
A: Airport zones have distinct ETA patterns: terminal loop traffic, variable parking garage exit times, taxi queue dynamics. Current model features include `special_event_nearby` (airport is an always-on "event"). Improvements: (1) Add `is_airport_origin`/`is_airport_dest` binary features. (2) Add `airport_terminal_queue_depth` (from historical patterns at that airport on that hour/day). (3) Track airport-specific MAPE separately in the accuracy log (`route_type = AIRPORT`). Monitoring shows airport ETAs typically have 20–25% MAPE vs. 11% for non-airport routes. With airport-specific features, the model reduces this to ~14%. Further improvement: real-time airport queue data (from airport authority open APIs where available, e.g., Heathrow, SFO).

Q14: How does the ETA Service maintain accuracy after Daylight Saving Time transitions?
A: Historical speed baselines are stored by `hour_of_day` in the local city timezone. When DST transitions occur (clocks spring forward or fall back), the `hour_of_day` calculation must use the city's timezone-aware clock, not UTC. The city timezone is stored in `cities.timezone` (e.g., `America/Los_Angeles`). The ETA Service uses `pytz` or Go's `time.Location` to compute local time for baseline lookups. DST transition day: the missing 2 AM hour (spring forward) has no baseline data — we interpolate between 1 AM and 3 AM baselines. The extra 1 AM hour (fall back) uses the average of the two 1 AM observations from the historical training data. Edge case: Flink's event-time windows handle DST correctly by operating in UTC internally and converting to local time only for feature extraction.

Q15: How would you design an ETA service for a city with significant construction that changes road topology weekly?
A: Construction zones are modeled as temporary edge weight changes: (1) Construction management APIs (Waze for Cities, municipal data feeds) provide lane closure schedules with start/end times and affected road segments. (2) The Map Graph Updater applies these as time-bounded edge attribute changes: `edge.weight_override = 3× normal` during construction hours, `1.0×` outside hours. (3) CH shortcuts through construction zones are handled by the overlay map (same mechanism as real-time traffic). (4) Weekly road topology changes (lane additions/removals) require partial CH rebuild. We batch these for the weekly rebuild cycle, with interim weight overrides covering the construction period. (5) Monitoring: construction zones are flagged in the accuracy log; elevated MAPE during construction periods alerts the maps team to ensure the construction data feed is current.

---

## 12. References & Further Reading

1. **Geisberger, R. et al. (2008): Contraction Hierarchies: Faster and Simpler Hierarchical Routing in Road Networks** — https://algo2.iti.kit.edu/schultes/hwy/contract.pdf
2. **OSRM (Open Source Routing Machine) — CH Implementation** — https://github.com/Project-OSRM/osrm-backend
3. **RoutingKit: Contraction Hierarchies with Dynamic Updates** — https://github.com/RoutingKit/RoutingKit
4. **Google OR-Tools: Vehicle Routing** — https://developers.google.com/optimization/routing
5. **Uber Engineering — Estimating Time of Arrival (ETA)** — https://eng.uber.com/eta/
6. **Lyft Engineering — ETA at Scale** — https://eng.lyft.com/eta-at-scale
7. **Newson, P., Krumm, J. (2009): Hidden Markov Map Matching** — ACM SIGSPATIAL 2009 (map matching for road snapping)
8. **OpenStreetMap Road Network Data** — https://wiki.openstreetmap.org/wiki/Downloading_data
9. **HERE Real-Time Traffic API** — https://developer.here.com/documentation/traffic/dev_guide/topics/overview.html
10. **Apache Flink Documentation — Window Operations** — https://nightlies.apache.org/flink/flink-docs-release-1.18/docs/dev/datastream/operators/windows/
11. **XGBoost Documentation — Quantile Regression** — https://xgboost.readthedocs.io/en/stable/tutorials/quantile_regression.html
12. **Google Encoded Polyline Algorithm** — https://developers.google.com/maps/documentation/utilities/polylinealgorithm
13. **Apache Arrow: In-Memory Columnar Format** — https://arrow.apache.org/ — Used for efficient graph serialization
14. **Dijkstra, E.W. (1959): A note on two problems in connexion with graphs** — Numerische Mathematik, 1(1):269–271
15. **Martin Kleppmann — Designing Data-Intensive Applications** (O'Reilly, 2017) — Chapter 6 (Partitioning) and Chapter 11 (Stream Processing)
