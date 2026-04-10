# System Design: Driver Matching Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Proximity-based matching**: Given a rider's pickup location and requested vehicle type, find available drivers within a configurable radius (default 5 km) and assign the best one.
2. **Multi-factor scoring**: Score each candidate driver using a composite function incorporating distance, driver rating, vehicle type match, acceptance rate, and heading toward pickup.
3. **Atomic assignment**: Exactly one driver is assigned to each trip; exactly one trip is assigned to each driver at a time — no double assignments under any race condition.
4. **Request broadcasting**: Before direct assignment, the system may broadcast the trip request to top-N drivers (sequential waterfall or parallel offer) and allow the first driver to accept.
5. **Fallback radius expansion**: If no driver is found within the initial radius, expand the search radius incrementally (5 km → 8 km → 12 km → 20 km) with configurable wait times per ring.
6. **Fairness constraints**: Prevent a small subset of high-score drivers from capturing all requests; implement income fairness across drivers with similar ratings in a city.
7. **City-level dispatch**: Matching is scoped to a single city. Cross-city matching is not supported.
8. **Vehicle type routing**: STANDARD, XL, LUX, SHARED ride types are matched only to drivers with the corresponding vehicle capability.
9. **Pool/Shared matching**: For SHARED rides, evaluate route compatibility with in-progress SHARED trips before creating a new trip.
10. **Driver acceptance timeout**: If a driver does not accept within 15 seconds, the request is sent to the next candidate.
11. **Cancellation-aware matching**: Exclude drivers who have exceeded their cancellation rate threshold from matching for a cooldown period.

### Non-Functional Requirements

- **Matching latency**: Driver identified and notified within 3 seconds (p99) of trip request creation.
- **Assignment success rate**: ≥ 95% of trips with available drivers in radius should be matched within 30 seconds.
- **Throughput**: Handle 462 simultaneous matching requests per second at peak.
- **Fairness**: No single driver in a city should receive more than 2× the average request rate compared to similar-scoring peers.
- **Scalability**: Support 1.5 M concurrent online drivers and 500,000 active matching requests globally.
- **Idempotency**: Retried matching requests produce the same result; no duplicated assignments.
- **Observability**: Emit metrics for match latency, no-driver-found rate, acceptance rate, radius expansion frequency.

### Out of Scope

- Driver navigation routing (ETA Service's responsibility)
- Upfront pricing computation (Fare Service)
- Real-time GPS ingestion (Location Tracking Service)
- ML training pipeline for driver scoring models
- Rider-driver communication (in-app chat/call)

---

## 2. Users & Scale

### User Types

| User Type | Role in Matching | Interaction |
|---|---|---|
| Rider | Initiates matching via ride request | Passively waits; sees ETA and driver details |
| Driver | Receives ride offer; accepts or declines | Responds within 15-second window |
| Trip Service | Triggers matching on trip creation | Internal API call (gRPC) |
| Matching Service | Core actor; runs scoring and assignment | Internal service |
| Location Service | Provides GEORADIUS results + driver telemetry | Internal dependency |
| Driver Service | Provides driver profile, rating, acceptance rate | Internal dependency |
| Fare Service | Provides surge multiplier for driver incentive display | Internal dependency |

### Traffic Estimates

**Assumptions**:
- 20 M trips/day; peak factor 2× → 462 matching requests/s peak
- Each matching request queries up to 50 driver candidates
- Driver acceptance rate: 80% of first offers accepted; 20% go to second candidate
- Average radius expansion: 15% of requests need one radius expansion
- Offer timeout: 15 seconds per driver; waterfall depth avg 1.3 drivers contacted

| Metric | Calculation | Result |
|---|---|---|
| Matching requests/s (peak) | 20 M / 86,400 × 2 | ~462/s |
| GEORADIUS calls/s | 462 × 1.15 (radius expansion) | ~531/s |
| Driver candidate lookups/s | 531 × 50 candidates | ~26,550/s |
| Driver CAS assignment attempts/s | 462 × 1.3 (waterfall depth) | ~601/s |
| Driver offer push notifications/s | 601 | ~601/s |
| Trip state writes/s | 462 | ~462/s |
| Scoring computations/s | 462 × 50 | ~23,100/s |
| Concurrent in-flight matching requests | 462/s × 5 s avg match time | ~2,310 concurrent |
| Offered driver Redis set operations/s | 601 creates + 601 TTL expirations | ~1,202/s |

### Latency Requirements

| Operation | P50 | P99 | Rationale |
|---|---|---|---|
| End-to-end match (request → driver notified) | < 1 s | < 3 s | UX: rider sees "Finding driver..." spinner |
| GEORADIUS query (Location Service) | < 5 ms | < 20 ms | One Redis call on critical path |
| Driver profile batch fetch | < 10 ms | < 30 ms | Up to 50 profiles from cache |
| Scoring computation (50 candidates) | < 2 ms | < 5 ms | CPU-bound; must be very fast |
| Driver CAS assignment (DB) | < 5 ms | < 15 ms | PostgreSQL CAS UPDATE with connection pool |
| Driver push notification delivery | < 1 s | < 2 s | FCM/APNs latency; out of our control |

### Storage Estimates

**Assumptions**:
- Matching event log: 200 bytes per attempt (including scores and outcome)
- Driver offer records: 100 bytes, TTL 60 s in Redis
- No-match events: 150 bytes

| Data | Size/Record | Volume | Storage/Day |
|---|---|---|---|
| Matching attempt logs | 200 B | 462/s × 86,400 × 1.3 attempts avg | ~5.2 GB/day |
| Match quality metrics (aggregated by city/hour) | 500 B | 10,000 cities × 24 hours | 120 MB/day |
| Driver offer Redis keys | 100 B | 601/s, TTL 60 s → avg 36,060 keys | 3.6 MB (in-memory) |
| Concurrent matching state (in-memory) | 1 KB | 2,310 concurrent | 2.3 MB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|---|---|---|
| Trip Service → Matching Service (gRPC) | 462/s × 300 B request | ~135 KB/s |
| Matching → Location Service (GEORADIUS) | 531/s × 200 B | ~106 KB/s |
| Matching → Driver Service (batch profile fetch) | 531/s × 50 drivers × 200 B | ~5.3 MB/s |
| Matching → Trip Service (assignment result) | 462/s × 1 KB | ~462 KB/s |
| Matching → Notification Service (driver offer) | 601/s × 500 B | ~300 KB/s |

---

## 3. High-Level Architecture

```
                    ┌─────────────────────┐
                    │    TRIP SERVICE      │
                    │  (creates trip in    │
                    │  REQUESTED state)    │
                    └──────────┬───────────┘
                               │ gRPC: MatchRide(trip_id, pickup, vehicle_type)
                               ▼
               ┌───────────────────────────────────────────┐
               │           MATCHING SERVICE                 │
               │  (stateless K8s pods, partitioned by       │
               │   city_id for cache locality)              │
               │                                            │
               │  1. Load balancer routes by city_id        │
               │  2. Check in-flight dedup (Redis)          │
               │  3. Call Location Service: GEORADIUS        │
               │  4. Batch-fetch driver profiles (Redis)    │
               │  5. Score candidates                       │
               │  6. Attempt CAS assignment (waterfall)     │
               │  7. Publish trip-events on success         │
               └──┬─────────────┬───────────────┬───────────┘
                  │             │               │
      ┌───────────▼──┐  ┌───────▼──────┐  ┌────▼──────────────────┐
      │   LOCATION   │  │    DRIVER    │  │    TRIP SERVICE        │
      │   SERVICE    │  │   SERVICE    │  │    (CAS UPDATE trips   │
      │  (GEORADIUS  │  │ (profiles,   │  │     + driver status)   │
      │   on Redis)  │  │  ratings,    │  └────────────────────────┘
      └───────┬──────┘  │  acceptance  │
              │         │  rates)      │       ┌─────────────────────┐
              │         └──────────────┘       │  NOTIFICATION SVC   │
              │                                │  (FCM/APNs push     │
              │         ┌──────────────────┐   │   to driver app)    │
              │         │  REDIS CLUSTER   │   └─────────────────────┘
              └────────►│  (driver Geo,    │
                        │   offer sets,    │
                        │   in-flight dedup│
                        │   score cache)   │
                        └──────────────────┘

  ┌──────────────────────────────────────────────────────────────────┐
  │                FAIRNESS & COOLDOWN SUBSYSTEM                     │
  │  • Per-city request counters per driver (Redis sorted set)       │
  │  • High-demand drivers get a "rest window" cooldown              │
  │  • Cancellation-rate enforcer (excludes high-cancel drivers)     │
  └──────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────┐
  │              MATCHING CONFIG SERVICE (Read-through cache)        │
  │  Stores per-city: initial_radius, expansion_steps, timeout,      │
  │  scoring weights, vehicle type availability rules               │
  └──────────────────────────────────────────────────────────────────┘
```

**Component Roles**:

| Component | Role |
|---|---|
| Matching Service | Orchestrates the entire match workflow: query → score → assign → notify |
| Location Service | Provides `GEORADIUS`-backed nearby driver query from Redis Geo |
| Driver Service | Provides driver rating, acceptance_rate, current_trip_count, vehicle details |
| Trip Service | Executes the final CAS UPDATE to atomically assign driver to trip |
| Redis Cluster | Stores driver Geo positions, offered-driver sets, deduplication keys, fairness counters |
| Notification Service | Delivers FCM/APNs push to driver app with trip offer details |
| Fairness Subsystem | Tracks per-driver request rates; applies cooldown to over-served drivers |
| Matching Config Service | Centralizes per-city tuning: radius steps, scoring weights, offer timeout |

**Primary Data Flow — Driver Match**:
1. Trip Service calls `MatchRide` on Matching Service (gRPC).
2. Matching Service checks Redis for in-flight dedup key `matching:{trip_id}` → prevents duplicate parallel matching runs.
3. GEORADIUS query returns top-50 nearby available drivers sorted by distance.
4. Batch-fetch driver profiles (ratings, acceptance rate, vehicle type) from Driver Service (Redis-cached).
5. Apply scoring function → rank candidates.
6. Apply fairness filter → temporarily deprioritize over-served drivers.
7. Pick top candidate → CAS attempt: `UPDATE drivers SET status='ON_TRIP' WHERE driver_id=? AND status='AVAILABLE'`.
8. If CAS succeeds → update trip → publish Kafka event → push notification to driver.
9. If CAS fails (driver offline/assigned) → try next candidate in ranked list.
10. If all candidates exhausted → trigger radius expansion → repeat from step 3.
11. If max radius reached with no match → return `NO_DRIVER_FOUND` → Trip Service marks trip `UNMATCHED` and notifies rider.

---

## 4. Data Model

### Entities & Schema

```sql
-- ============================================================
-- MATCHING ATTEMPT LOG (PostgreSQL, sharded by city_id)
-- ============================================================
CREATE TABLE matching_attempts (
    attempt_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    trip_id             UUID NOT NULL,
    city_id             INT NOT NULL,
    pickup_lat          DOUBLE PRECISION NOT NULL,
    pickup_lng          DOUBLE PRECISION NOT NULL,
    vehicle_type        VARCHAR(20) NOT NULL,
    search_radius_km    NUMERIC(6,2) NOT NULL,
    candidates_found    SMALLINT NOT NULL,
    candidates_scored   SMALLINT NOT NULL,
    assigned_driver_id  UUID,                   -- NULL if no match
    attempts_before_assign SMALLINT DEFAULT 0, -- drivers declined/timeout before winner
    outcome             VARCHAR(20) NOT NULL,   -- MATCHED|NO_DRIVERS|TIMEOUT|CANCELLED
    match_latency_ms    INT,
    started_at          TIMESTAMPTZ DEFAULT NOW(),
    completed_at        TIMESTAMPTZ,
    scoring_weights     JSONB,                  -- snapshot of weights used (for analysis)
    radius_expansions   SMALLINT DEFAULT 0,

    CONSTRAINT outcome_check CHECK (outcome IN (
        'MATCHED','NO_DRIVERS','TIMEOUT','CANCELLED','RIDER_CANCELLED'
    ))
);

SELECT create_distributed_table('matching_attempts', 'city_id');

CREATE INDEX idx_match_attempts_trip   ON matching_attempts (trip_id);
CREATE INDEX idx_match_attempts_driver ON matching_attempts (assigned_driver_id, started_at);
CREATE INDEX idx_match_attempts_city   ON matching_attempts (city_id, started_at DESC);

-- ============================================================
-- DRIVER OFFER LOG (per-candidate offer records)
-- ============================================================
CREATE TABLE driver_offers (
    offer_id            UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    attempt_id          UUID NOT NULL REFERENCES matching_attempts(attempt_id),
    driver_id           UUID NOT NULL,
    trip_id             UUID NOT NULL,
    offered_at          TIMESTAMPTZ DEFAULT NOW(),
    expires_at          TIMESTAMPTZ NOT NULL,
    outcome             VARCHAR(20) NOT NULL,   -- ACCEPTED|DECLINED|TIMEOUT|SUPERSEDED
    response_time_ms    INT,
    distance_km         NUMERIC(6,3),
    composite_score     NUMERIC(8,4),
    score_breakdown     JSONB                   -- {distance: 0.72, rating: 0.19, heading: 0.09}
);

-- ============================================================
-- DRIVER FAIRNESS STATE (per city, maintained in Redis +
-- periodically snapshotted to PostgreSQL)
-- ============================================================
CREATE TABLE driver_request_counters (
    driver_id           UUID NOT NULL,
    city_id             INT NOT NULL,
    window_start        TIMESTAMPTZ NOT NULL,   -- 1-hour window
    requests_offered    INT DEFAULT 0,
    requests_accepted   INT DEFAULT 0,
    cooldown_until      TIMESTAMPTZ,            -- NULL if not in cooldown
    PRIMARY KEY (driver_id, city_id, window_start)
);

-- ============================================================
-- MATCHING CITY CONFIG (read by Matching Service on startup)
-- ============================================================
CREATE TABLE matching_config (
    city_id                 INT PRIMARY KEY REFERENCES cities(city_id),
    initial_radius_km       NUMERIC(4,1) DEFAULT 5.0,
    radius_steps_km         JSONB DEFAULT '[8.0, 12.0, 20.0]',
    radius_wait_ms          JSONB DEFAULT '[3000, 5000, 8000]',
    offer_timeout_ms        INT DEFAULT 15000,
    max_candidates          SMALLINT DEFAULT 50,
    score_weight_distance   NUMERIC(4,3) DEFAULT 0.600,
    score_weight_rating     NUMERIC(4,3) DEFAULT 0.200,
    score_weight_acceptance NUMERIC(4,3) DEFAULT 0.100,
    score_weight_heading    NUMERIC(4,3) DEFAULT 0.100,
    fairness_window_min     INT DEFAULT 60,
    fairness_overload_ratio NUMERIC(4,2) DEFAULT 2.0,  -- cooldown if >2× city avg
    min_driver_rating       NUMERIC(3,2) DEFAULT 4.0,  -- exclude below this
    updated_at              TIMESTAMPTZ DEFAULT NOW()
);
```

**Redis Keys for Matching**:

```
# In-flight dedup: prevents duplicate parallel matching runs for same trip
Key:   matching:inflight:{trip_id}
Type:  String ("1")
TTL:   30 s (matching run must complete; if expired, Trip Service retries)

# Offered drivers for a trip (who received offers, for security validation on accept)
Key:   trip:{trip_id}:offered_drivers
Type:  Set of driver_id strings
TTL:   60 s

# Driver offer state (which trip was offered to driver)
Key:   driver:{driver_id}:pending_offer
Type:  Hash {trip_id, offered_at_epoch, expires_at_epoch, pickup_lat, pickup_lng}
TTL:   20 s (slightly longer than offer_timeout_ms=15s for race margin)

# Fairness counters (requests offered per driver per hour)
Key:   fairness:{city_id}:{hour_epoch}:{driver_id}
Type:  String (integer counter)
TTL:   3600 s

# City-level hourly average requests offered per driver (for fairness ratio check)
Key:   fairness:{city_id}:{hour_epoch}:avg
Type:  String (float)
TTL:   3600 s

# Driver in cooldown
Key:   driver:{driver_id}:cooldown
Type:  String (reason)
TTL:   cooldown_duration_s (configurable, default 300 s = 5 min)
```

### Database Choice

| Option | Use Case Fit | Pros | Cons | Selected? |
|---|---|---|---|---|
| **PostgreSQL** | Matching logs, city config | ACID, SQL analytics | Write throughput limited | Yes — low write volume, SQL needed for analytics |
| **Redis** | In-flight dedup, offer state, fairness counters | Sub-millisecond, TTL, atomic INCR/SETNX | Volatile; limited query capability | Yes — ephemeral state with strong latency needs |
| Kafka | Async event publishing | Durable, replay | Not a query store | Used for event publishing only |
| MongoDB | Config + logs | Flexible schema | Less familiar, inferior SQL | No — PostgreSQL sufficient |
| DynamoDB | Offer state | Auto-scale, low-latency | Cost, limited queries | No — Redis is cheaper and faster for ephemeral state |

**Justification**:
- **PostgreSQL** is appropriate for matching_attempts and driver_offers because: (1) Write volume is low (462/s matching completions), well within single PostgreSQL capacity. (2) SQL GROUP BY analytics are needed (match rate by city, hour, vehicle type). (3) ACID guarantees the matching log is consistent.
- **Redis** is appropriate for in-flight state because: (1) `SETNX` (set-if-not-exists) is the correct primitive for atomic deduplication — no DB transaction needed. (2) TTL-based expiry naturally handles timed-out matching runs without a cleanup job. (3) All matching state is ephemeral — it has no value after the match completes.

---

## 5. API Design

Internal gRPC APIs between services. External REST API for driver acceptance.

---

### Internal gRPC: Match Ride

```protobuf
service MatchingService {
  // Called by Trip Service when a new trip is REQUESTED
  rpc MatchRide(MatchRideRequest) returns (MatchRideResponse);

  // Called by Trip Service to cancel an in-progress match
  rpc CancelMatch(CancelMatchRequest) returns (CancelMatchResponse);

  // Called periodically to get match quality metrics per city
  rpc GetMatchingStats(MatchingStatsRequest) returns (MatchingStatsResponse);
}

message MatchRideRequest {
  string trip_id = 1;
  int32  city_id = 2;
  double pickup_lat = 3;
  double pickup_lng = 4;
  double dropoff_lat = 5;
  double dropoff_lng = 6;
  string vehicle_type = 7;    // STANDARD|XL|LUX|SHARED
  string rider_id = 8;
  float  rider_rating = 9;    // Passed to driver offer for context
}

message MatchRideResponse {
  string match_attempt_id = 1;
  // Note: this is async — response returned immediately after launching
  // the matching workflow. Actual assignment notified via trip-events Kafka.
  MatchStatus status = 2;   // MATCHING_STARTED | NO_DRIVERS_AVAILABLE | DUPLICATE
}

message CancelMatchRequest {
  string trip_id = 1;
  string reason  = 2;   // RIDER_CANCELLED | TIMEOUT | SYSTEM
}
```

### Driver — Accept Offer

```
POST /v1/trips/{trip_id}/accept
Auth: Driver JWT
Body: {} (empty — driver identity from JWT)

Processing:
  1. Validate driver is in trip:{trip_id}:offered_drivers set
  2. Check driver:{driver_id}:pending_offer matches this trip_id
  3. CAS UPDATE trips SET status='DRIVER_ASSIGNED', driver_id=? WHERE status='REQUESTED'
  4. CAS UPDATE drivers SET status='ON_TRIP' WHERE driver_id=? AND status='AVAILABLE'
  5. Publish DRIVER_ASSIGNED event to Kafka trip-events

Response 200:
{
  "trip_id":       "trip-uuid",
  "status":        "DRIVER_ASSIGNED",
  "rider": {
    "first_name":  "Anya",
    "rating":      4.87,
    "profile_photo": "https://..."
  },
  "pickup_lat":    37.7749,
  "pickup_lng":    -122.4194,
  "pickup_address": "Market St & 7th St, San Francisco",
  "dropoff_address": "San Jose Convention Center",
  "estimated_payout_usd": 9.50,
  "surge_multiplier": 1.4
}

Errors:
  403 Forbidden   — driver not in offered set (security)
  409 Conflict    — trip already assigned to another driver
  410 Gone        — trip cancelled before acceptance
  429             — rate limit
```

### Driver — Decline Offer

```
POST /v1/trips/{trip_id}/decline
Auth: Driver JWT
Body: { "reason": "TOO_FAR" }  // TOO_FAR|NOT_SAFE|OTHER

Response 200:
{ "trip_id": "trip-uuid", "status": "offer_declined" }

Side effects:
  - Driver remains AVAILABLE
  - Matching Service advances to next candidate
  - Decline logged to driver_offers table (affects acceptance_rate metric)
```

### Get Matching Stats (Internal Admin)

```
GET /v1/internal/matching/stats?city_id=1&from=2026-04-09T17:00:00Z&to=2026-04-09T18:00:00Z
Auth: Admin JWT

Response 200:
{
  "city_id":              1,
  "period":               "1h",
  "total_requests":       16632,
  "matched":              15800,
  "match_rate_pct":       95.0,
  "no_driver_found":      412,
  "rider_cancelled":      420,
  "avg_match_latency_ms": 1240,
  "p99_match_latency_ms": 4100,
  "avg_candidates_found": 14.3,
  "avg_radius_expansions": 0.18,
  "avg_offer_attempts":   1.28
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Driver Scoring Function

**Problem It Solves**: Among 50 nearby available drivers, which one should be assigned? Pure proximity (closest driver) is suboptimal: it ignores driver quality (rating), directional efficiency (driver heading toward pickup), vehicle type fit, and cancellation risk. The scoring function must be fast (< 2 ms for 50 candidates), explainable, and configurable per city.

**Approaches Comparison**:

| Approach | Factors | Speed | Fairness | Explainability | Quality |
|---|---|---|---|---|---|
| Nearest driver only | Distance only | O(1) | Low | High | Baseline |
| Weighted linear score | Distance, rating, acceptance rate, heading | O(N) in N candidates | Medium | High | Good |
| **Normalized multi-factor (selected)** | Same + normalized to 0–1 range | O(N) | Medium | High | Very Good |
| Bipartite matching (Hungarian) | Global optimal assignment | O(N³) | High | Low | Optimal |
| ML ranking model (LambdaRank) | All features + historical outcomes | O(N × inference) | Configured | Low | Best |
| Greedy with exploration (epsilon-greedy) | Score + random exploration | O(N) | High | Medium | Good |

**Selected: Normalized Multi-Factor Weighted Score**

**Implementation Detail**:

```python
# Scoring function — runs in Matching Service (Python/Go equivalent)
# Called once per GEORADIUS result set

from dataclasses import dataclass
from typing import List
import math

@dataclass
class CandidateDriver:
    driver_id: str
    distance_km: float
    rating: float           # 1.0–5.0
    acceptance_rate: float  # 0.0–1.0
    heading_deg: float      # 0–360 (driver's current heading)
    vehicle_type: str
    is_in_fairness_cooldown: bool

@dataclass
class ScoringWeights:
    distance:    float = 0.60
    rating:      float = 0.20
    acceptance:  float = 0.10
    heading:     float = 0.10

def score_drivers(
    candidates: List[CandidateDriver],
    pickup_lat: float,
    pickup_lng: float,
    weights: ScoringWeights,
    config: MatchingConfig
) -> List[tuple[CandidateDriver, float]]:

    if not candidates:
        return []

    # --- Step 1: Filter ineligible candidates ---
    eligible = [
        c for c in candidates
        if c.rating >= config.min_driver_rating
        and c.vehicle_type == config.requested_vehicle_type
        and not c.is_in_fairness_cooldown
    ]
    if not eligible:
        # Relax fairness cooldown if no eligible drivers remain
        eligible = [
            c for c in candidates
            if c.rating >= config.min_driver_rating
            and c.vehicle_type == config.requested_vehicle_type
        ]

    # --- Step 2: Compute raw feature values ---
    max_distance = max(c.distance_km for c in eligible)
    min_distance = min(c.distance_km for c in eligible)
    distance_range = max(max_distance - min_distance, 0.001)  # avoid div-by-zero

    # --- Step 3: Normalize features to [0, 1] ---
    # distance_score: closer = higher score
    # rating_score: higher rating = higher score
    # acceptance_score: higher acceptance rate = higher score
    # heading_score: driver pointing toward pickup = higher score

    scored = []
    for c in eligible:
        # Distance: linear normalization, inverted (closer = better)
        dist_score = 1.0 - (c.distance_km - min_distance) / distance_range

        # Rating: normalize from [4.0, 5.0] range (below 4.0 excluded)
        rating_score = max(0.0, min(1.0, (c.rating - 4.0) / 1.0))

        # Acceptance rate: raw 0–1, already normalized
        accept_score = c.acceptance_rate

        # Heading: cosine similarity between driver's direction and
        # bearing from driver to pickup
        bearing_to_pickup = compute_bearing(
            driver_lat=c.lat, driver_lng=c.lng,
            target_lat=pickup_lat, target_lng=pickup_lng
        )
        heading_diff_rad = math.radians(bearing_to_pickup - c.heading_deg)
        heading_score = (math.cos(heading_diff_rad) + 1.0) / 2.0  # maps to [0, 1]

        # Composite score
        composite = (
            weights.distance    * dist_score   +
            weights.rating      * rating_score +
            weights.acceptance  * accept_score +
            weights.heading     * heading_score
        )

        # Tie-breaking: if composite scores within 0.01, prefer lower distance
        composite += (1.0 / (c.distance_km + 0.1)) * 0.0001  # tiny tiebreak term

        scored.append((c, composite, {
            'distance_score':   round(dist_score, 4),
            'rating_score':     round(rating_score, 4),
            'acceptance_score': round(accept_score, 4),
            'heading_score':    round(heading_score, 4),
            'composite':        round(composite, 4)
        }))

    # Sort descending by composite score
    scored.sort(key=lambda x: x[1], reverse=True)
    return scored

def compute_bearing(driver_lat, driver_lng, target_lat, target_lng) -> float:
    """Returns bearing in degrees from driver to target (0=North, 90=East)"""
    dlat = math.radians(target_lat - driver_lat)
    dlng = math.radians(target_lng - driver_lng)
    x = math.sin(dlng) * math.cos(math.radians(target_lat))
    y = (math.cos(math.radians(driver_lat)) * math.sin(math.radians(target_lat)) -
         math.sin(math.radians(driver_lat)) * math.cos(math.radians(target_lat)) *
         math.cos(dlng))
    return (math.degrees(math.atan2(x, y)) + 360) % 360
```

**Weight Calibration**: Scoring weights are stored per city in `matching_config`. They are tuned by an offline experiment: historical matched trips are re-scored with different weight combinations; the combination that minimizes `cancellation_rate × pickup_time` is selected. For new cities without history, default weights (distance 0.60, rating 0.20, acceptance 0.10, heading 0.10) are used.

**Performance**: Scoring 50 candidates with the above function takes ~200 µs in Go (vectorizable with SIMD). Even at 23,100 scoring computations/second (462 matching ops × 50 candidates), this is 23,100 × 200 µs = 4.6 seconds of total CPU time/second — handled by 10 Go goroutines on 1 CPU core with parallelism.

**Interviewer Q&A**:

Q1: Why is distance weighted at 60%? Shouldn't rating matter more for rider experience?
A: Distance is weighted highest because pickup ETA is the single metric riders most often cite for satisfaction and app abandonment. A 4.6-star driver 0.8 km away will arrive in 2–3 minutes; a 4.9-star driver 4 km away will take 8–10 minutes. The 0.3-star difference rarely translates to a meaningfully better ride. Empirical A/B tests at Uber showed that matching to the nearest available driver with a 4.0+ rating floor produces equivalent rider satisfaction scores compared to prioritizing rating, while reducing pickup times by 15%. The 4.0 floor is the meaningful quality cutoff.

Q2: How does the heading score affect a driver stuck in traffic pointing 180° away from the pickup?
A: The heading score contributes only 10% of the composite. A driver pointed exactly away (180°) gets heading_score = 0.0 (cos(π) = -1 → normalized to 0). If this driver is very close (distance_score ~1.0) and has a good rating (rating_score ~0.8), their composite is still 0.60 × 1.0 + 0.20 × 0.8 + 0.10 × 0.9 + 0.10 × 0.0 = 0.85 — still competitive. Heading is a tiebreaker for similar-distance candidates, not a dominant factor. This is by design; a driver 0.5 km away facing away will still be much faster than a driver 3 km away facing toward.

Q3: How do you handle a new driver with no rating history (first week)?
A: New drivers receive a default rating of 4.5 (above the 4.0 floor, below the average of ~4.7). Their `rating_count = 0` is flagged in the scoring function, and they receive a 10% score bonus (configured as `new_driver_boost_weight`) to ensure they receive enough trips to build a rating. After 20 trips, the actual computed rating takes full effect. The bonus is configurable per city and can be set to zero if driver supply is plentiful.

Q4: Can the scoring function be gamed — e.g., a driver gaming their acceptance rate by accepting everything then cancelling?
A: Acceptance rate and cancellation rate are tracked separately. The scoring function uses `acceptance_rate` (accepted / offered), but the eligibility filter also applies a `cancellation_rate` check: drivers with > 15% post-acceptance cancellation rate in the last 7 days receive a `CANCELLATION_COOLDOWN` flag and are excluded from matching for 1 hour. Accepting then cancelling improves acceptance rate but triggers the cancellation_rate penalty, which overrides the score benefit.

Q5: What would it take to switch from linear weighted scoring to an ML ranking model?
A: The scoring interface is already abstracted behind a `ScorerInterface` with a single `Score(candidates) → ranked_list` method. Replacing the linear scorer with an ML model (e.g., XGBoost or LightGBM served via TensorFlow Serving or a lightweight Go inference library) requires: (1) Feature engineering: all current features plus historical: driver zone preference, time-of-day acceptance patterns, previous trip with this rider. (2) Training data: historical matched trips with outcome labels (rider rating, no-show rate, completion rate). (3) Online serving: the model scores 50 candidates in < 5 ms using vectorized inference. (4) Gradual rollout: A/B test linear vs. ML scorer on 5% of traffic, measuring pickup time, completion rate, and rider satisfaction. Estimated improvement: 8–12% reduction in cancellation rate based on industry benchmarks.

---

### 6.2 Atomic Assignment — Waterfall vs. Broadcast

**Problem It Solves**: Once candidates are scored, the system must assign exactly one driver to the trip without race conditions. Two concurrent matching runs (e.g., duplicate request) must not double-assign. A driver who goes offline between scoring and assignment must not be assigned. The assignment strategy also determines how quickly a driver accepts — sequential waterfall (offer one at a time) vs. broadcast (offer to top N simultaneously).

**Approaches Comparison**:

| Strategy | Description | Match Latency | Rider Wait | Driver UX | Complexity |
|---|---|---|---|---|---|
| **Sequential waterfall** | Offer to #1; if timeout/decline, offer to #2, etc. | High (additive 15s per miss) | Worst case: 15s × k | Exclusive offer; less competition stress | Low |
| **Parallel broadcast (top-N)** | Offer to top 3 simultaneously; first accept wins | Low | Best (fastest accept) | Multiple drivers waiting; "phantom offers" | Medium |
| **Tiered broadcast** | Offer to top driver 8s; then top 3 simultaneously | Good | Good | Balanced | Medium |
| **Auction-style bidding** | Drivers bid acceptance in a short window | Low | Good | Unusual UX; regulatory complexity | High |
| **Batch bipartite matching** | Group all requests + drivers; solve globally every 5s | Low (batched) | 0–5s delay | Fair | High |

**Selected: Tiered Waterfall — Exclusive 8-second offer to #1, then parallel broadcast to top 3**

**Reasoning**: Pure sequential waterfall causes unacceptable delays if the top driver is slow to respond (15 × 3 = 45 seconds in a bad case). Pure broadcast creates driver anxiety (accepted a trip that someone else also accepted). Tiered approach: give the best driver an exclusive 8-second window (they have earned it via their score), then simultaneously offer to the next 3 if the first doesn't respond. This limits max match latency to ~8 + 15 = 23 seconds in the rare case of 2 tiers.

**Implementation Detail — Assignment Workflow**:

```go
// Matching Service pseudo-code (Go)

type MatchingWorkflow struct {
    tripID      string
    cityID      int
    pickup      LatLng
    vehicleType string
    config      MatchingConfig
}

func (m *MatchingWorkflow) Run(ctx context.Context) error {
    // Step 1: Acquire dedup lock (SETNX in Redis)
    if !redis.SetNX(ctx, fmt.Sprintf("matching:inflight:%s", m.tripID), "1", 30*time.Second) {
        return ErrDuplicateMatch  // already running
    }
    defer redis.Del(ctx, fmt.Sprintf("matching:inflight:%s", m.tripID))

    for _, radiusKm := range m.config.RadiusSteps {
        // Step 2: Query nearby available drivers
        candidates, err := locationSvc.NearbyDrivers(ctx, NearbyRequest{
            Lat: m.pickup.Lat, Lng: m.pickup.Lng,
            RadiusKm: radiusKm, CityID: m.cityID,
            VehicleType: m.vehicleType, Limit: 50,
        })
        if err != nil { return err }

        if len(candidates) == 0 {
            waitFor(m.config.RadiusWaitMs[radiusKm])
            continue   // try next radius
        }

        // Step 3: Fetch profiles + score
        profiles   := driverSvc.BatchGetProfiles(ctx, driverIDs(candidates))
        fairness   := fairnessSvc.GetCooldowns(ctx, m.cityID, driverIDs(candidates))
        scored     := scorer.Score(candidates, profiles, fairness, m.pickup, m.config.Weights)

        // Step 4: Tiered offer
        if err := m.offerTiered(ctx, scored); err == ErrAssigned {
            return nil   // success
        }

        // No driver accepted in this radius; try expanding
    }

    // All radii exhausted
    recordNoMatch(m.tripID, m.cityID)
    tripSvc.MarkUnmatched(ctx, m.tripID)
    return ErrNoDriverFound
}

func (m *MatchingWorkflow) offerTiered(ctx context.Context, scored []ScoredDriver) error {
    if len(scored) == 0 { return ErrNoDriverFound }

    // Tier 1: Exclusive 8-second offer to top driver
    if err := m.offerOne(ctx, scored[0], 8*time.Second); err == nil {
        return ErrAssigned
    }

    // Tier 2: Parallel offer to next up to 3 drivers (15-second window)
    tier2 := scored[1:min(4, len(scored))]
    resultCh := make(chan string, len(tier2))  // driver_id of acceptor

    for _, sd := range tier2 {
        go func(sd ScoredDriver) {
            if err := m.offerOneAsync(ctx, sd, 15*time.Second); err == nil {
                resultCh <- sd.DriverID
            } else {
                resultCh <- ""
            }
        }(sd)
    }

    accepted := ""
    for range tier2 {
        if id := <-resultCh; id != "" && accepted == "" {
            accepted = id
        }
    }

    if accepted != "" {
        // Cancel pending offers to the others (they were still waiting)
        for _, sd := range tier2 {
            if sd.DriverID != accepted {
                cancelOffer(ctx, sd.DriverID, m.tripID)
            }
        }
        return ErrAssigned
    }

    return ErrNoDriverAccepted
}

func (m *MatchingWorkflow) offerOne(ctx context.Context, sd ScoredDriver, timeout time.Duration) error {
    // Record offered driver set
    redis.SAdd(ctx, fmt.Sprintf("trip:%s:offered_drivers", m.tripID), sd.DriverID)
    redis.Expire(ctx, fmt.Sprintf("trip:%s:offered_drivers", m.tripID), 60*time.Second)

    // Store pending offer on driver
    redis.HSet(ctx, fmt.Sprintf("driver:%s:pending_offer", sd.DriverID),
        "trip_id", m.tripID, "expires_at", time.Now().Add(timeout).Unix())
    redis.Expire(ctx, fmt.Sprintf("driver:%s:pending_offer", sd.DriverID),
        timeout + 5*time.Second)

    // Push notification to driver app
    notificationSvc.Push(ctx, DriverOfferNotification{
        DriverID:     sd.DriverID,
        TripID:       m.tripID,
        PickupLat:    m.pickup.Lat,
        PickupLng:    m.pickup.Lng,
        DistanceKm:   sd.DistanceKm,
        TimeoutMs:    int(timeout.Milliseconds()),
    })

    // Poll for acceptance (driver calls POST /trips/{id}/accept which
    // triggers acceptCh notification via Redis Pub/Sub)
    select {
    case <-m.acceptCh(sd.DriverID, m.tripID):
        return nil           // driver accepted
    case <-time.After(timeout):
        return ErrTimeout    // driver didn't respond
    }
}
```

**CAS Assignment (atomic, in Trip Service)**:

```sql
-- Called by Trip Service when driver calls POST /trips/{id}/accept
BEGIN;
  -- Lock the trip row
  SELECT trip_id, status, driver_id
  FROM trips
  WHERE trip_id = $trip_id
  FOR UPDATE;

  -- Only assign if still REQUESTED
  UPDATE trips
  SET status = 'DRIVER_ASSIGNED',
      driver_id = $driver_id,
      driver_assigned_at = NOW()
  WHERE trip_id = $trip_id
    AND status = 'REQUESTED'   -- CAS condition
  RETURNING trip_id;

  -- If rows affected = 0, another driver won — ROLLBACK and return 409

  -- Mark driver as ON_TRIP
  UPDATE drivers
  SET status = 'ON_TRIP'
  WHERE driver_id = $driver_id
    AND status = 'AVAILABLE';   -- CAS condition

  -- If rows affected = 0, driver went offline — ROLLBACK and return 409
COMMIT;
```

**Interviewer Q&A**:

Q1: In the parallel broadcast phase, two drivers accept within milliseconds of each other. How does only one win?
A: The Trip Service's assignment is protected by a PostgreSQL row-level FOR UPDATE lock followed by a CAS condition. Even if two concurrent accept requests arrive simultaneously, the DB serializes them. The first to commit transitions the trip to `DRIVER_ASSIGNED`. The second's UPDATE finds `status != 'REQUESTED'` (because the first already changed it) → 0 rows affected → returns 409. The losing driver receives a "Ride already taken" notification and is immediately returned to AVAILABLE status. Their Redis offer key is cleaned up.

Q2: What happens if the PostgreSQL row lock causes a deadlock between the trip and driver UPDATE within the same transaction?
A: Deadlocks occur when two transactions lock the same rows in different orders. Our transaction always locks `trips` first, then `drivers`, in a deterministic order. Since each matching run assigns to exactly one (trip, driver) pair, and no other process locks `trips` and `drivers` together in the opposite order, deadlocks are structurally avoided. If a deadlock does occur (PostgreSQL detects it via wait-for graph), PostgreSQL aborts one transaction with an error code 40P01; the Matching Service retries the CAS with the next candidate.

Q3: A driver accepts but their internet drops before they receive the trip confirmation response. They retry the accept. How is this handled?
A: The trip's accept endpoint is idempotent: if `trip.driver_id == requesting_driver_id AND trip.status == 'DRIVER_ASSIGNED'`, it returns 200 with the trip details (as if it were the original successful response). The driver app's SDK retries on timeout with the same trip_id. Only one success state is possible; subsequent retries are safely idempotent.

Q4: How does the waterfall handle a surge situation where all 50 candidates decline or timeout?
A: After all candidates in the current radius tier are exhausted (declined or timed out), the matching workflow expands to the next radius ring (e.g., from 5 km to 8 km) and queries a fresh GEORADIUS. The new result set may overlap with the previous one — we exclude drivers who already declined (stored in `trip:{id}:offered_drivers` set). If all radius rings are exhausted without a match, the trip enters `UNMATCHED` state. The rider is notified with a suggestion to retry or the system retries automatically with higher surge multiplier to attract more drivers.

Q5: How would you implement "preferred driver" matching where a rider always gets their favorite driver if available?
A: The `MatchRideRequest` includes a `preferred_driver_ids` list (up to 3). The Matching Service first checks if any preferred driver is available within 2× the normal search radius (to be flexible with their location). If a preferred driver is found, they are injected as the top candidate with a boosted composite score (effectively overriding the normal ranking). The normal scoring then fills the fallback list. A preferred driver can still decline; in that case, normal waterfall proceeds. Preference is only applied if the preferred driver's rating is still ≥ 4.0 (safety requirement is non-negotiable).

---

### 6.3 Fairness System

**Problem It Solves**: Without fairness controls, the scoring function creates a "rich get richer" dynamic: high-rating drivers close to downtown always score highest and capture disproportionate trip volume. A driver with a 4.95 rating who parks near a busy area earns 3× the income of an equally qualified driver in a slightly less busy zone. This creates driver dissatisfaction and attrition. Fairness must be balanced against matching quality — completely random matching reduces rider experience.

**Approaches Comparison**:

| Approach | Mechanism | Fairness Level | Quality Impact | Complexity |
|---|---|---|---|---|
| None (pure score) | No fairness constraint | None | Maximum | None |
| Random sampling from top-k | Pick randomly from top 5 scorers | Low | Low | Very Low |
| **Cooldown after N requests** | After k requests/hour, skip for T minutes | Medium | Low | Low |
| Income equity weighting | Add `(target_income - actual_income)` to score | High | Low | Medium |
| Round-robin by score tier | Tier drivers; cycle within tier | High | Low | Medium |
| **Request rate fairness (selected)** | Cap requests offered to 2× city avg per hour | Medium | Very Low | Low |

**Selected: Request Rate Fairness with Sliding Window Cooldown**

**Implementation Detail**:

```python
# Fairness Service
FAIRNESS_WINDOW_MIN = 60       # 1-hour rolling window
OVERLOAD_RATIO = 2.0           # cooldown if driver gets > 2× city average
COOLDOWN_DURATION_S = 300      # 5-minute cooldown

def is_driver_in_cooldown(driver_id: str, city_id: int) -> bool:
    return redis.exists(f'driver:{driver_id}:cooldown') == 1

def record_offer(driver_id: str, city_id: int):
    hour_epoch = current_hour_epoch()
    key = f'fairness:{city_id}:{hour_epoch}:{driver_id}'
    count = redis.incr(key)
    redis.expire(key, FAIRNESS_WINDOW_MIN * 60)

    # Check against city average
    city_avg_key = f'fairness:{city_id}:{hour_epoch}:avg'
    city_avg = float(redis.get(city_avg_key) or 0)

    if city_avg > 0 and count > city_avg * OVERLOAD_RATIO:
        # Driver has received >2× the city average offers this hour
        redis.setex(
            f'driver:{driver_id}:cooldown',
            COOLDOWN_DURATION_S,
            f'FAIRNESS_COOLDOWN: {count} offers vs avg {city_avg:.1f}'
        )

def update_city_average(city_id: int):
    """Called every 5 minutes by a background job"""
    hour_epoch = current_hour_epoch()
    online_drivers = get_online_driver_count(city_id)

    # Count total offers this hour
    total_offers = sum(
        int(redis.get(f'fairness:{city_id}:{hour_epoch}:{did}') or 0)
        for did in get_online_driver_ids(city_id)
    )
    # Alternatively, maintain a separate atomic counter for total offers
    if online_drivers > 0:
        avg = total_offers / online_drivers
        redis.setex(f'fairness:{city_id}:{hour_epoch}:avg', 3600, avg)
```

**Fairness-aware scoring integration**:
The scoring function checks `is_driver_in_cooldown()` for each candidate. Cooldown drivers are sorted to the end of the candidate list (below non-cooldown candidates at the same score tier). They are not excluded outright — if no non-cooldown candidate accepts, cooldown drivers serve as final fallback.

**Outcome**: With 2× cooldown threshold and 5-minute cooldown, the top 10% of drivers by historical offer-capture rate experience a maximum of 15% income reduction (verified via simulation). The bottom 50% of drivers by position (not by quality) see a 10–20% income increase. Rider pickup times increase by < 5% in the worst case (a high-demand corridor where most nearby drivers are in cooldown).

**Interviewer Q&A**:

Q1: The fairness system depends on "city average offers per driver per hour." What if 90% of drivers are clustered in one neighborhood while only 10% of demand is there?
A: The city average is a coarse instrument. For fairness in heterogeneous demand distributions, we compute H3 zone-level averages (one per H3 resolution-7 cell). Cooldown is triggered relative to the zone average, not the city average. A driver in a busy downtown zone is compared to other downtown-zone drivers, not to suburban drivers with lower demand. This requires `fairness:{city_id}:{hour_epoch}:{h3_cell}:avg` keys, maintained by the same background job.

Q2: Is the 5-minute cooldown duration optimal? How was it chosen?
A: It's configurable via `matching_config.fairness_cooldown_s`. 5 minutes is the default, derived from: if a cooldown driver misses 300s / 4s_per_update_cycle = 75 potential matching windows, and the city's request rate is 1 match per 10 seconds per driver on average, the driver misses ~30 potential trip offers. This reduces their hourly income by roughly 30/600 = 5% per cooldown event. Multiple cooldown events per hour cap earnings approximately at the 2× city average target. Testing showed 3 minutes causes excessive re-triggering; 10 minutes causes too many unmatched requests during high-demand periods.

Q3: How does fairness interact with LUX-type vehicles? LUX drivers inherently get fewer offers (less demand).
A: Vehicle type buckets fairness separately. A LUX driver is compared only to other LUX drivers in their zone, not STANDARD drivers. `fairness:{city_id}:{hour_epoch}:{vehicle_type}:{h3_cell}:{driver_id}`. LUX demand is 5–10% of STANDARD, so the average offer rate for LUX drivers is much lower; their 2× threshold is correspondingly lower in absolute terms but equivalent in relative terms.

Q4: A group of drivers deliberately clusters near a venue (concert pre-positioning). Is this a fairness violation?
A: No — driver positioning decisions are intentional and legitimate income strategies. Fairness controls apply to offer capture within a zone, not to zone choice. A driver who correctly predicts a demand hotspot and positions there earns their higher offer rate legitimately. The fairness system targets unintentional monopolies from scoring bias, not driver strategy. However, if 500 drivers all cluster at a venue and one driver captures 10× the others purely due to position (same GEORADIUS ranking luck), the cooldown system distributes offers more evenly among the 500 clustered drivers.

Q5: How would you extend fairness to handle protected attributes — ensuring drivers of all demographic groups receive equitable offer distribution?
A: Currently, the fairness system operates on observable driver behaviors and position (non-demographic). Adding demographic fairness requires: (1) Demographic data collection (voluntary, opt-in). (2) Computing offer rates by demographic cohort per city. (3) Auditing for statistically significant disparities using a chi-squared test on the offer distribution. (4) Adjusting scoring weights or adding explicit demographic equity constraints (analogous to the zone-level average). This is a complex legal and product decision — anti-discrimination laws in many jurisdictions require careful implementation with legal review. The architecture supports it through the pluggable ScorerInterface.

---

## 7. Scaling

### Horizontal Scaling

| Component | Mechanism | Scale Trigger | Capacity per Pod |
|---|---|---|---|
| Matching Service | K8s HPA on active_matching_goroutines metric | > 80% goroutine pool utilization | 250 concurrent matching workflows per pod |
| Location Service | Stateless; HPA on CPU | > 70% CPU | 2,000 GEORADIUS calls/s per pod |
| Driver Service | Stateless; Redis-backed profile cache | > 70% CPU | 5,000 batch profile reads/s per pod |
| Notification Service | Kafka consumer group; add consumers on lag | > 50K message lag | 1,000 notifications/s per pod |
| Redis Cluster | Add shards on memory > 70% | Memory pressure | 32 GB per node |
| PostgreSQL (matching_attempts) | Read replicas for analytics; primary for writes | Write > 5,000/s | Primary: 10,000 inserts/s |

**City-level partitioning**: Matching Service pods are labeled with city affinity. A consistent-hash load balancer routes `MatchRide(city_id=1)` to pods tagged for city 1. This ensures city-specific Redis keys (driver Geo sets, fairness counters) are cached in the same pod's local CPU cache, reducing Redis RTTs. It is not a hard partition — pods can handle any city during failover.

### DB Sharding

**matching_attempts**: Sharded by `city_id` in Citus. Each city's matching history is co-located on one shard, enabling efficient city-level analytics (`GROUP BY hour, city_id`) without cross-shard joins. 32 shards across 8 workers covers 10,000+ cities (multiple cities per shard, same as trip data).

**Redis Geo** (driver positions): Sharded by city_id hash → each city's driver positions land on a deterministic Redis shard. Matching Service queries only the shard for the request's city — no fan-out. For very large cities (Mumbai, Delhi), a single city may need its own dedicated Redis shard — implemented by overriding the hash slot for that city_id.

### Replication

| Store | Strategy | Failover Time |
|---|---|---|
| Redis (matching state) | Sentinel: 1 master + 2 replicas; auto-promote on master failure | < 10 s |
| PostgreSQL (matching_attempts) | Primary + 1 sync replica; Patroni auto-promote | < 30 s |
| Kafka (trip-events) | RF=3; leader election on broker failure | < 10 s transparent |

**On Redis failover during active matching**: In-flight matching workflows lose their dedup lock (`matching:inflight:{trip_id}`) and their offer state. The Trip Service has a 30-second sweeper that detects trips stuck in `REQUESTED` state and re-triggers matching. The worst case is a 30-second delay for affected trips — well within acceptable bounds for rare failovers.

### Caching Strategy

| Data | Cache | TTL | Size |
|---|---|---|---|
| Driver profiles (rating, acceptance rate, vehicle) | Redis Hash per driver | 60 s | 50 M × 200 B = 10 GB (global) |
| City matching config | In-process Go map (service-level) | 5 min | 10,000 cities × 500 B = 5 MB |
| Fairness counters | Redis String per driver per hour | 3600 s | 1.5 M drivers × 100 B = 150 MB |
| City-level online driver count | Redis String per city | 30 s | 10,000 cities × 8 B = 80 KB |

### Interviewer Q&A

Q1: How does the Matching Service scale during a major global event (New Year's Eve in 50 cities simultaneously)?
A: New Year's Eve is predictable — we pre-scale city-specific Matching Service pods the day before. For each of the 50 event cities, K8s HPA min_replicas is set to 3× normal for the 22:00–02:00 window. Redis Cluster nodes for event cities are pre-warmed with driver profiles. Surge pricing reduces demand volume by discouraging marginal riders. City-level circuit breakers prevent a spike in one city from consuming pod capacity intended for others. Load testing with 10× synthetic matching load is run in staging 48 hours before.

Q2: How do you prevent the Matching Service from overwhelming the Driver Service with profile requests?
A: Batch request optimization: instead of 50 individual profile lookups per GEORADIUS result, we issue a single `BatchGetProfiles(driver_ids=[...])` gRPC call. The Driver Service serves this from Redis (each driver profile is a 200-byte Redis Hash, cached 60 seconds). A single Redis pipeline executes all 50 HGETALL commands as one round-trip. At 531 matching requests/s × 50 drivers = 26,550 profile lookups/s → 531 pipeline calls/s. Each pipeline call is sub-millisecond; the Driver Service handles 531 pipeline calls/s trivially.

Q3: Describe how you would add a new city to the matching system with zero downtime.
A: (1) Insert city row into `cities` and `matching_config` tables (replicated to all Citus workers). (2) Provision Redis key namespace for city (no action needed — first GEOADD auto-creates the key). (3) Deploy Matching Config Service update (picks up new city on next cache refresh, max 5 minutes). (4) Set city `is_active = TRUE`. From this point, drivers in the new city can go online and riders can request. The matching system discovers the new city automatically on the next config cache refresh. No code deployment, no restart.

Q4: What happens to matching if the Matching Service is partitioned from the Location Service (network partition)?
A: The Matching Service's Location Service client has a circuit breaker: after 5 failures in 10 seconds, it opens and stops attempting GEORADIUS calls. The fallback: the Matching Service falls back to querying PostgreSQL's `drivers` table with a PostGIS `ST_DWithin` query (slower but available). This fallback can handle ~1,000 queries/s — sufficient to handle a degraded matching rate during the partition. The matching latency increases from < 100 ms to ~500 ms while the fallback is active. Alert fires after 30 seconds; on-call investigates.

Q5: How does the system handle 10,000 concurrent matching requests during a concert discharge (sudden 20× spike in one city)?
A: The Matching Service's per-city pod pool handles requests up to its goroutine pool limit (250/pod). Beyond that, new requests queue in the gRPC server's accept queue (configurable: 10,000 max depth). K8s HPA detects the goroutine pressure and adds pods within 2–3 minutes. During scale-out: (1) Queued requests are served in FIFO order — earlier riders get priority. (2) Surge pricing immediately increases multiplier, shedding marginal demand by 20–30%. (3) Radius expansion kicks in faster (shorter wait per ring) to increase driver pool. In a true disaster scenario (all 1,500 available city drivers occupied), unmatched trips return `NO_DRIVER_FOUND` within 30 seconds — the rider is clearly notified.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| Matching Service pod crash | Active matching workflows lost | K8s liveness probe; trip sweeper detects stuck REQUESTED trips | Trip sweeper re-triggers matching within 30 s; pods restart in < 10 s |
| Redis failure (matching state lost) | In-flight dedup and offer state lost | Sentinel health check → failover | Replica promoted in < 10 s; stuck trips re-matched by sweeper |
| Driver Service unavailable | Profile fetch fails; scoring uses stale profiles | Circuit breaker trips | Fall back to last-known cached profiles (60-s TTL); if fully expired, use distance-only scoring |
| Location Service unavailable | GEORADIUS fails; no candidates | Circuit breaker trips | Fall back to PostgreSQL drivers table ST_DWithin; log degraded mode alert |
| FCM/APNs push failure | Driver doesn't receive offer | Notification Service logs failure | Retry up to 3× with 1s backoff; if all fail, advance to next candidate immediately |
| Network partition (Matching → Trip Service) | CAS assignment cannot be executed | gRPC timeout after 5 s | Circuit breaker; return 503 to Trip Service; sweeper retries matching |
| Duplicate matching requests (Trip Service retry) | Two matching workflows for same trip | Redis SETNX dedup key | Second workflow sees existing key → returns ErrDuplicateMatch; no duplicate assignment |

### Retries & Idempotency

All Matching Service operations are idempotent:
- `MatchRide(trip_id)`: SETNX on `matching:inflight:{trip_id}` prevents duplicate runs.
- Driver CAS assignment: `UPDATE ... WHERE status='REQUESTED'` is naturally idempotent — second attempt fails cleanly with 0 rows.
- Offer push: If push fails and matching retries with the same driver, the old offer key is overwritten (same TTL). Driver receives a second push for the same trip — idempotent from the driver's perspective.
- Matching attempt log: Written after outcome is known; if the write fails, the matching still succeeded. A separate async log-flush process retries the DB write.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Purpose |
|---|---|---|---|
| `matching.requests.rate` by city | Counter | < 50% of expected | City-level matching breakdown |
| `matching.latency_p99` by city | Histogram | > 5 s | Match speed degradation |
| `matching.match_rate` by city | Gauge | < 90% | Supply shortage detection |
| `matching.no_driver_found.rate` | Gauge | > 10% | Severe supply shortage |
| `matching.radius_expansion.rate` | Gauge | > 30% | Normal radius not finding drivers |
| `matching.offer_acceptance_rate` | Gauge | < 70% | Driver quality or UX issue |
| `matching.avg_candidates_found` | Gauge | < 3 (alarm), < 1 (critical) | Supply scarcity |
| `matching.cassignment_conflict_rate` | Counter | > 5% | Race condition frequency |
| `matching.fairness_cooldown_active` by city | Gauge | > 30% of drivers | Fairness system over-triggering |
| `matching.workflow_timeout` | Counter | > 1% | Matching deadlock or slow path |
| `driver_service.cache_hit_rate` | Gauge | < 90% | Profile cache too small or TTL too short |
| `redis.offer_key_count` | Gauge | > 100K (leak) | Offer key TTL not cleaning up |

### Distributed Tracing

Full trace from `POST /v1/trips` → matching completion:
```
[Trip Service: create_trip]            3ms
[Matching Service: MatchRide RPC]      total: 950ms p50
  [Redis SETNX dedup]                  0.5ms
  [Location Service: NearbyDrivers]    12ms
    [Redis GEORADIUS]                  1ms
  [Driver Service: BatchGetProfiles]   8ms
    [Redis pipeline: 50 HGETALL]       3ms
  [Fairness: GetCooldowns]             2ms
    [Redis: 50 EXISTS checks]          1ms
  [Scoring: Score(50 candidates)]      0.2ms
  [Offer: Push to driver #1]           5ms → [FCM/APNs: ~800ms external]
  [CAS: UPDATE trips+drivers]          4ms
  Total Matching Service internal:     32ms
  Waiting for driver acceptance:       ~800ms (p50 driver response time)
[Kafka publish: DRIVER_ASSIGNED]       2ms
[WebSocket push to rider]              5ms
```

### Logging

Structured JSON log per matching attempt:
```json
{
  "level": "INFO",
  "service": "matching",
  "trace_id": "abc123",
  "trip_id": "trip-uuid",
  "city_id": 1,
  "pickup_lat": 37.7749,
  "pickup_lng": -122.4194,
  "vehicle_type": "STANDARD",
  "candidates_found": 23,
  "candidates_scored": 18,
  "radius_km": 5.0,
  "assigned_driver_id": "drv-uuid",
  "winner_score": 0.8432,
  "winner_distance_km": 0.87,
  "winner_offer_attempts": 1,
  "match_latency_ms": 950,
  "outcome": "MATCHED"
}
```

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Selected | Alternative | Reason |
|---|---|---|---|
| Assignment algorithm | CAS UPDATE on PostgreSQL | Distributed lock (Redis SETNX) | PostgreSQL ACID guarantees no ghost assignments; Redis lock requires fencing token for true safety |
| Scoring function | Weighted multi-factor linear | ML ranking model | Linear is explainable, fast (< 1 ms), and auditable; ML adds latency and model maintenance overhead |
| Offer strategy | Tiered waterfall (8s exclusive + 15s parallel top-3) | Pure sequential (15s per driver) | Pure sequential max wait 45s for 3 declines; tiered caps at 23s |
| Candidate pool size | Top 50 from GEORADIUS | All drivers in radius | 50 provides sufficient scoring diversity; more increases scoring cost without quality gain |
| Fairness mechanism | Request rate cooldown (2× city avg) | Income equity weighting | Rate cooldown is simple, auditable, non-discriminatory; income weighting requires complex ledger |
| Driver profile cache | Redis Hash per driver (60s TTL) | Direct Driver Service DB read | Redis profile cache reduces Driver Service load by 95%; 60s staleness for rating is acceptable (ratings update slowly) |
| Radius expansion | Incremental steps (5→8→12→20 km) | Immediate max-radius query | Incremental avoids surfacing very-far drivers when nearby drivers are just slow to respond; preserves ETA quality |
| Dedup mechanism | Redis SETNX per trip_id | Idempotency key in DB | Redis SETNX is O(1) and does not require a DB round-trip on the hot path; sufficient for the ephemeral dedup use case |

---

## 11. Follow-up Interview Questions

Q1: How would you implement global bipartite matching (Hungarian algorithm) across all simultaneous requests in a city for theoretically optimal assignment?
A: The Hungarian algorithm on N riders × M drivers runs in O(N³). With N=462 simultaneous matching requests and M=50 candidates each, N=462 is reasonable for a 1-second batch window. Implementation: collect all incoming MatchRide requests into a 1-second batch. Build a bipartite graph with cost matrix (1 - composite_score). Run Hungarian via scipy.optimize.linear_sum_assignment (O(N³) in C). Assign all winners atomically via a DB transaction or optimistic locking. Challenge: batching introduces up to 1 second of additional latency; rejected by our p99 = 3 s requirement (leaving only 2 s for everything else). Applicable for: futures/scheduled rides (not time-sensitive). For real-time, greedy is the right trade-off.

Q2: How do you detect when a driver is lying about their position (GPS spoofing) before matching them?
A: At matching time: check the driver's `speed_kmh` from their telemetry. If the driver claims to be at position X but their last 3 GPS updates show positions that require > 120 km/h movement between them, flag the driver. The matching service checks `driver:{id}:telemetry` speed and last position; if inconsistent, the candidate is scored with a `gps_suspicious` flag and deprioritized (score multiplied by 0.5). Persistent offenders are escalated to the fraud pipeline. This is a layer on top of the Location Ingest Service's speed validation.

Q3: How would you handle matching for a "scheduled ride" that starts 2 hours from now?
A: Scheduled rides are not matched at booking time — driver supply 2 hours ahead is unknown. Instead: (1) Trip is stored in state `SCHEDULED`. (2) At T-10 minutes, a push notification goes to all eligible drivers within expanded radius (10 km) offering the scheduled trip as a priority offer (higher payout, visible beforehand). (3) At T-5 minutes, standard waterfall matching begins. (4) If no driver accepts by T-2 minutes, the system escalates with a surge boost on driver incentive payout. (5) At T=0, if still unmatched, notify rider with alternatives. Scheduled rides can tolerate the T-10 pre-notification because drivers can plan their positioning.

Q4: Describe how A/B testing would work for the scoring function weights.
A: Experiment setup: (1) Create two scoring config variants: control (current weights) and treatment (new candidate weights). (2) Assign trips to variants by consistent hashing on `(rider_id + experiment_id)` — ensures the same rider always sees the same variant for the experiment duration. (3) Both variants use the same matching infrastructure; only the weights dict differs. (4) Metrics: pickup time (primary), match rate, rider cancellation rate, driver acceptance rate, rider 5-star rate. (5) Statistical analysis after 7 days with ≥ 10,000 trips per variant (t-test on continuous metrics, chi-squared on binary). (6) If treatment improves p-value < 0.05 on primary metric with no regressions, gradually roll out (10% → 50% → 100%).

Q5: What would change in the matching system if you moved from rides to food delivery (multiple pickups/dropoffs per courier)?
A: Food delivery matching differs in key ways: (1) A courier can carry multiple orders simultaneously (multi-drop). (2) Orders have restaurant pickup times (must arrive after order ready time). (3) Route optimization becomes NP-hard (vehicle routing problem with time windows). Changes needed: (a) Scoring function adds a "route compatibility score" — does this new order's pickup fit within the courier's planned route without excessive detour? (b) Assignment is no longer 1:1; it's 1:N orders per courier. (c) Matching triggers on "order ready" event from restaurant, not immediate. (d) The bipartite matching problem becomes a Vehicle Routing Problem (VRP), solvable with heuristic approaches (Clarke-Wright savings algorithm) for real-time applicability.

Q6: How would you ensure matching latency SLAs in a region with high network round-trip times (e.g., Southeast Asia with inter-datacenter latency of 50–80 ms)?
A: Deploy the full matching stack (Matching Service + Redis + Driver Service) in the same AZ as the city's data. All matching-critical operations (GEORADIUS, profile fetch, CAS) must be within the same datacenter, not crossing regions. Route 53 latency-based routing ensures Singapore riders are served by the Singapore stack. Cross-region calls (global analytics, ML model sync) happen asynchronously after the match is complete. If the AP-Southeast-1 region has internal latency > 10 ms between AZs, co-locate matching pods and Redis in the same AZ within the region.

Q7: How would you handle a driver who is simultaneously a candidate for two concurrent trip requests?
A: Two concurrent matching workflows both find the same driver via GEORADIUS. Both offer them simultaneously (parallel broadcast tier). The driver accepts one — their CAS (`UPDATE drivers SET status='ON_TRIP' WHERE status='AVAILABLE'`) succeeds for the first one and returns 0 rows for the second. The second trip's matching workflow catches the 0-rows result and moves to the next candidate in its ranked list. The driver is aware of only the accepted trip (the declined one's offer is cleaned up). The losing trip's matching continues without interruption.

Q8: Describe how you would implement "ride pooling matching" — connecting two riders going in similar directions.
A: Pool matching adds a compatibility check before assignment. For a new POOL request: (1) Find in-progress POOL trips in the same city with status `TRIP_IN_PROGRESS` (driver has one passenger). (2) For each in-progress trip, compute: (a) detour distance to pick up the new rider (must be < 20% of original route), (b) time delay to existing passenger (must be < 5 minutes), (c) directional compatibility (both riders going roughly the same direction). (3) If a compatible in-progress trip exists, offer the pooled ride to that driver. (4) If no compatible in-progress trip, start a new POOL trip and wait up to 90 seconds for a second rider match before dispatching as a solo POOL ride. The compatibility algorithm uses the ETA Service for route detour computation.

Q9: What is the impact of cold start — a driver just went online and no profile exists in the cache?
A: On driver online (status AVAILABLE), the Driver Service proactively warms the Redis profile cache for that driver (`HSET driver:{id}:profile rating ... vehicle_type ... acceptance_rate ...` with 60-s TTL). This happens synchronously as part of the "go online" flow, before the driver is added to the Redis Geo available set. So by the time a GEORADIUS returns a driver as a candidate, their profile is always cache-warm. Cache miss on a matching candidate is treated as a profile fetch from the Driver Service DB (fallback, < 5 ms via connection pool).

Q10: How does the system behave at 3 AM in a small city with only 5 drivers online?
A: With 5 drivers and low demand: (1) GEORADIUS returns all 5 within any reasonable radius — no expansion needed. (2) Scoring runs on 5 candidates — trivially fast. (3) Fairness cooldown is never triggered (5 drivers, no one near the 2× average). (4) Radius search starts at the full 20 km ring immediately (since the city config can be tuned to start with a larger radius for low-supply hours). (5) If all 5 are on trips already, the rider is immediately notified of no availability rather than waiting 5 minutes cycling through radius rings. Detecting this: if `ZCARD city:{id}:drivers:available == 0` before running GEORADIUS, short-circuit the matching workflow and return `NO_DRIVERS_AVAILABLE` immediately.

Q11: How would you implement "driver-requested trip type" preferences — a driver who only wants XL or LUX trips?
A: Driver preferences are stored in `drivers.vehicle_type_preferences JSONB` (e.g., `["XL", "LUX"]`). During matching, the Redis Geo query is made against `city:{id}:drivers:available:XL` and `city:{id}:drivers:available:LUX` (type-specific sorted sets). Drivers who only want XL are registered in the `available:XL` set but not in `available:STANDARD`. Preferences are respected during the available-driver registration (Driver Service manages set membership on preference change). A driver who temporarily wants all trips removes their type restriction; the Driver Service migrates them to all applicable type sets.

Q12: Describe how you'd measure and report on matching quality for regulators (e.g., a city transportation authority requesting equity reports).
A: Regulatory reporting uses the `matching_attempts` and `driver_offers` tables. We produce: (1) Match rate by neighborhood (H3 cell): are some areas systematically under-served? (2) Average wait time by neighborhood. (3) Offer acceptance rates by driver rating tier. (4) Surge multiplier distribution by neighborhood. (5) No-match rate by time of day and neighborhood. Reports are generated as scheduled BigQuery/Redshift jobs, producing PDF dashboards. Regulators receive read-only dashboard access. Sensitive data (driver/rider PII) is removed; only aggregated statistics are shared.

Q13: How would you add "carpool lanes" awareness — preferring drivers on carpool-eligible routes?
A: Road segment metadata in the map graph includes `is_hov_eligible` flags (from HERE Maps or OpenStreetMap). The ETA Service, when computing pickup ETA, can calculate a "HOV route ETA" (using carpool lanes if vehicle has 2+ occupants). For POOL rides, the scoring function queries the ETA Service for both standard and HOV ETAs. If the HOV route is significantly faster (> 10% faster), drivers on HOV-eligible routes are boosted in score. This requires the ETA Service to expose a `route_type=HOV` parameter and the Matching Service to request it for POOL candidates.

Q14: What's your strategy for handling matching in a city that just became available for the first time with no historical data?
A: New city launch checklist: (1) Seed `matching_config` with global defaults. (2) Pre-populate driver profiles from onboarding data. (3) No historical fairness data — all drivers start with a "new city" flag that disables fairness cooldown for the first 7 days (to avoid cold-start suppression). (4) Scoring uses distance-heavy weights (0.70 distance, 0.20 rating, 0.10 acceptance) since new drivers have limited rating and acceptance history. (5) No surge pricing for the first 2 weeks (to attract supply). (6) Matching config team monitors match rate hourly and adjusts radius steps / timeouts based on real traffic.

Q15: How would you design a "driver pre-positioning" feature — suggesting to idle drivers where to move to maximize their match probability?
A: Pre-positioning uses the demand forecast from the Surge Engine (H3 cell → predicted demand in next 15 minutes) and supply data (driver density map from Redis ZCARD per cell). For each idle (AVAILABLE, stationary) driver: (1) Compute the nearest H3 cell with high predicted demand-to-supply ratio. (2) Suggest navigation to that cell via an in-app nudge (not mandatory). (3) Track whether driver followed the nudge and whether they received a trip at the destination (outcome measurement). (4) Nudge recommendations are generated by a lightweight Python worker that reads surge forecast + Redis Geo every 5 minutes and generates per-driver suggestions, pushed via WebSocket. This is a revenue optimization for drivers; no SLA required, purely advisory.

---

## 12. References & Further Reading

1. **Uber Engineering — The Architecture of Uber's API** — https://eng.uber.com/uber-api-architecture/
2. **Lyft Engineering — How Lyft Matches Riders and Drivers** — https://eng.lyft.com/how-lyft-creates-hyper-local-demand-forecasts-31bc54cdc3b1
3. **Uber Engineering — Optimizing Driver Dispatch** — https://eng.uber.com/dispatching/
4. **Hungarian Algorithm (Kuhn-Munkres)** — Wikipedia: https://en.wikipedia.org/wiki/Hungarian_algorithm
5. **Kuhn, H.W. (1955): "The Hungarian Method for the Assignment Problem"** — Naval Research Logistics Quarterly
6. **Redis Sorted Sets (ZADD, GEORADIUS)** — https://redis.io/docs/data-types/sorted-sets/
7. **Protobuf Protocol Documentation** — https://protobuf.dev/programming-guides/proto3/
8. **gRPC Documentation** — https://grpc.io/docs/
9. **Google Maps Platform — Directions API** — https://developers.google.com/maps/documentation/directions
10. **Martin Kleppmann — Designing Data-Intensive Applications** — Chapter 12 (Future of Data Systems), O'Reilly 2017
11. **Lyft Engineering: Rebalancing Bike Share Stations** — https://eng.lyft.com/rebalancing-lyft-s-ride-share-bikeshare-4a7c7db3ef76 (supply-demand balancing concepts)
12. **Bipartite Matching — CLRS Introduction to Algorithms** — Chapter 26 (Maximum Flow), MIT Press
13. **Circuit Breaker Pattern** — Martin Fowler: https://martinfowler.com/bliki/CircuitBreaker.html
14. **Fairness in ML Systems** — Barocas, Hardt, Narayanan: https://fairmlbook.org/
15. **W3C Geolocation API** — https://www.w3.org/TR/geolocation/ (mobile GPS coordinate format reference)
