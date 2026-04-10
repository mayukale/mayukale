# System Design: Real-Time Leaderboard

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Score Submission**: Accept score updates for any player at any time. A score update is either absolute (new total score) or relative (delta, e.g., +100 points).
2. **Global Leaderboard**: Maintain a ranked list of all players globally, sorted by score descending. Return a player's global rank and the top-N players at any time.
3. **Regional Leaderboards**: Maintain separate ranked lists per region (e.g., North America, Europe, Asia), with the same query capabilities.
4. **Friend/Social Leaderboard**: Return a player's rank among their friends (subset leaderboard) — show N players above and below a given player among their social graph.
5. **Historical Leaderboards**: Store end-of-period leaderboard snapshots (end of day, end of week, end of season). Allow querying historical ranks for any player.
6. **Player Score Profile**: Return a player's current score, current global rank, current regional rank, and historical rank progression.
7. **Rank Change Notification**: Push real-time notifications to players when their rank changes by a configurable threshold (e.g., rank moves from 5,001 to 4,999 — they entered the top 5,000).
8. **Anti-Cheat**: Detect and reject suspicious score submissions (implausible velocity, abnormal score jumps) before they affect the leaderboard.

### Non-Functional Requirements

1. **Scale**: Support 100 million registered players. 10 million concurrent active players during peak events (esports tournaments, game launches).
2. **Score Update Throughput**: Handle 100,000 score updates per second at peak.
3. **Rank Query Latency**: Return a player's rank in < 10 ms (p99). Return top-100 leaderboard in < 20 ms (p99).
4. **Score Update Propagation**: Score updates are reflected in the leaderboard within 1 second.
5. **Durability**: Score updates must be durable — a confirmed score update is never lost.
6. **Eventual Consistency**: Leaderboard reads may lag score updates by up to 1 second (acceptable).
7. **Fairness**: All valid score updates must be reflected in the leaderboard. No score update is silently ignored.
8. **Availability**: 99.99% uptime for score submission. 99.9% uptime for leaderboard reads.

### Out of Scope

- Matchmaking and game server logic.
- Score computation (how game events translate to score — the game server handles this).
- Player authentication and account management.
- In-game purchases and virtual economies.
- Cross-game leaderboards (each game has its own leaderboard instance).
- Real-time spectator feeds (separate streaming system).

---

## 2. Users & Scale

### User Types

| Actor | Description |
|---|---|
| Game Server | Submits score updates on behalf of players after game events |
| Game Client | Queries leaderboard for display in-game |
| Player | Passive recipient of rank-change notifications |
| Anti-Cheat Service | Validates score submissions before they affect the leaderboard |
| Historical Snapshot Job | Periodic job capturing leaderboard state at period end |
| Admin Dashboard | Operator view of leaderboard health, suspicious players |

### Traffic Estimates

**Assumptions**:
- 100M total registered players.
- Peak concurrent active players: 10M (10% of total — typical for a major game during a live event).
- Average score update frequency: 1 per 6 seconds per active player (roughly every completed game action).
- Peak score update rate: 10M active players / 6 s = **1.67M updates/s**. Design target: 100K/s (assuming 6% of active players score simultaneously per second — shorter bursts).
- Leaderboard queries: each active player checks leaderboard every 30 seconds in-game. 10M / 30 = 333K queries/s. With caching, reduce to 50K uncached queries/s.
- Friend leaderboard: queried less frequently — 50K/s.
- Rank notifications: assume 1% of score updates cause a notable rank change → 1,000 notifications/s.

| Metric | Calculation | Result |
|---|---|---|
| Peak score updates/s | 10M active × 1/60s average | ~167K/s |
| Design headroom | 167K × 1.5 burst | **~100K/s design target (with burst headroom)** |
| Redis ZADD throughput needed | 100K/s × 3 leaderboards (global + 5 regions, 2 ZADD each) | 600K ops/s |
| Leaderboard query throughput | 10M active / 30s × 10% uncached | 33K ZRANK+ZSCORE ops/s |
| Notification throughput | 100K updates/s × 1% rank threshold crossed | 1,000 push notifications/s |
| Score storage (Cassandra) | 100M players × 200 B | 20 GB |
| Historical snapshots | 100M players × 50 B × 365 days | 1.83 TB/year |
| Score update size | player_id(8B) + score(8B) + timestamp(8B) + game_id(4B) + metadata(50B) | ~80 B |
| Kafka message rate | 100K/s × 80 B | 8 MB/s |

### Latency Requirements

| Operation | Target |
|---|---|
| Score update ack (after durability guarantee) | < 50 ms (p99) |
| Score reflected in Redis leaderboard | < 1 s |
| Player rank query (global) | < 10 ms (p99) |
| Top-100 leaderboard query | < 20 ms (p99) |
| Friend leaderboard query | < 100 ms (p95) |
| Historical rank query | < 200 ms (p95) |
| Rank change notification delivery | < 5 s from score update |

### Storage Estimates

| Data | Size | Volume |
|---|---|---|
| Redis global leaderboard ZSET | 16 B/member × 100M players | 1.6 GB |
| Redis per-region ZSETs (5 regions) | 16 B/member × 20M avg members | 1.6 GB total |
| Redis player metadata | 50 B/player × 10M active | 500 MB |
| Cassandra score records | 200 B × 100M players | 20 GB |
| Cassandra score history | 200 B × 100M × avg 1000 updates | 20 TB |
| Historical snapshot store (Cassandra) | 50 B × 100M × 365 snapshots/year | 1.83 TB/year |
| Kafka (score updates) | 80 B × 100K/s × 86400 × 7 days retention | 484 GB |

### Bandwidth Estimates

| Flow | Rate |
|---|---|
| Ingest (game server → API) | 100K × 80 B = 8 MB/s |
| API → Kafka | 8 MB/s |
| Kafka → Redis updaters | 8 MB/s × 3 consumer groups | 24 MB/s |
| Leaderboard query responses | 50K QPS × 500 B avg | 25 MB/s |
| WebSocket push notifications | 1K notifications/s × 200 B | 200 KB/s |

---

## 3. High-Level Architecture

```
                ┌──────────────────────────────────────────────────────────────────┐
                │                  GAME TIER                                      │
                │  Game Servers (submit score updates after game events)           │
                │  Game Clients (query leaderboard, receive notifications)         │
                └──────────┬──────────────────────────────────────┬───────────────┘
                           │ Score Updates (HTTP POST)             │ Rank Queries (HTTP GET)
                           │                                       │ WebSocket (notifications)
                           ▼                                       ▼
                ┌──────────────────────────┐      ┌───────────────────────────────────┐
                │   SCORE INGESTION API    │      │      LEADERBOARD QUERY API        │
                │   (Stateless, ×N pods)   │      │      (Stateless, ×N pods)         │
                │                          │      │                                   │
                │  1. Auth: verify         │      │  1. Auth: API key                 │
                │     game server token    │      │  2. Rank query:                   │
                │  2. Validate score       │      │     - Global: ZRANK + ZSCORE      │
                │  3. Anti-cheat check     │      │     - Regional: region ZSET       │
                │  4. Write to Kafka       │      │     - Top-N: ZRANGE 0 N-1         │
                │  5. Ack to caller        │      │  3. Return from Redis (< 10 ms)   │
                └──────────┬───────────────┘      │  4. Cache response 5s             │
                           │                       └───────────────────────────────────┘
                           ▼                                     ▲
                ┌──────────────────────┐                        │ (Redis reads)
                │   Apache Kafka       │                        │
                │   Topic:             │                        │
                │   score_updates      │                        │
                │   (32 partitions,    │                        │
                │    by player_id)     │                        │
                └──────────┬───────────┘                        │
                           │                                     │
              ┌────────────┼───────────────────┐               │
              │            │                   │               │
              ▼            ▼                   ▼               │
   ┌─────────────────┐ ┌────────────────┐ ┌────────────┐       │
   │ Redis Updater   │ │ Score Persister│ │ Notif.     │       │
   │ (×N workers)    │ │ (×N workers)   │ │ Engine     │       │
   │                 │ │                │ │            │       │
   │ Consumes from   │ │ Writes score   │ │ Detects    │       │
   │ Kafka:          │ │ updates to     │ │ rank       │       │
   │ ZADD global_lb  │ │ Cassandra      │ │ threshold  │       │
   │ ZADD region_lb  │ │ (durable)      │ │ crossings  │       │
   │ HSET player_meta│ │                │ │ Pushes via │       │
   └────────┬────────┘ └────────────────┘ │ WebSocket  │       │
            │                              └────────────┘       │
            ▼                                                    │
   ┌────────────────────────────────────────────────────────────┴───┐
   │                    REDIS CLUSTER                               │
   │                                                                │
   │  global_leaderboard           ZSET (score → player_id)        │
   │  region:NA_leaderboard        ZSET (score → player_id)        │
   │  region:EU_leaderboard        ZSET (score → player_id)        │
   │  region:ASIA_leaderboard      ZSET (score → player_id)        │
   │  player:score:{player_id}     HASH (score, rank, region, ...)  │
   │  player:rank_history:{id}     LIST (last 100 rank snapshots)   │
   └────────────────────────────────────────────────────────────────┘
                      ▲
                      │ (historical queries, score history)
                      │
         ┌────────────────────────┐
         │  Cassandra Cluster     │
         │  - player_scores table │
         │  - score_history table │
         │  - leaderboard_        │
         │    snapshots table     │
         └────────────────────────┘
```

**Component Roles**:

- **Score Ingestion API**: Accepts score updates from game servers. Performs synchronous anti-cheat validation (fast rules), writes to Kafka for durability, returns `202 Accepted` to the game server. This is the latency-critical path — p99 < 50 ms.
- **Apache Kafka**: Durable ordered log for score updates. Partitioning by `player_id % 32` ensures all updates for a player are ordered within a partition, preventing race conditions in the Redis ZADD sequence.
- **Redis Updater Workers**: Kafka consumers that apply score updates to Redis sorted sets. These are the "hot path" for leaderboard updates. Use Redis pipelining to batch multiple ZADD calls, reducing round-trips.
- **Score Persister Workers**: Separate Kafka consumer group. Writes score updates to Cassandra for durability and score history. Runs asynchronously from Redis updates — Cassandra persistence does not block the leaderboard update path.
- **Notification Engine**: Kafka consumer that monitors rank changes. After each ZADD, queries the new rank and compares to the player's previous rank (stored in Redis HASH). If the delta exceeds the player's notification threshold, enqueues a push notification.
- **Redis Cluster**: The core in-memory data store. Global leaderboard, regional leaderboards, and player metadata all live here. Redis's ZADD, ZRANK, and ZRANGE are the primitives that make sub-10ms rank queries possible.
- **Cassandra**: Durable, persistent store for score data and historical snapshots. Serves queries for historical rank data and score audit trails. Not on the hot read path — only queried when Redis doesn't have the data.
- **Leaderboard Query API**: Serves all leaderboard read requests. For global and regional ranks, reads directly from Redis (< 1 ms data retrieval). For friend leaderboards and historical data, performs more complex operations. Adds a short TTL cache (5 seconds) for top-N queries.

**Primary Use-Case Data Flow (score update → rank visible in leaderboard)**:

1. Game server sends `POST /v1/scores` with `{player_id: "p123", game_id: "g1", score_delta: +500, event_type: "kill"}`.
2. Ingestion API authenticates game server token. Looks up player's current score from Redis HASH (`HGET player:score:p123 current_score`).
3. Anti-cheat: Check if +500 delta is valid for `event_type = kill` in game `g1`. If score > max_per_event → reject with 422.
4. Compute new absolute score: `new_score = current_score + delta`.
5. Publish to Kafka `score_updates` topic: `{player_id: "p123", new_score: 12500, delta: 500, timestamp: T, game_id: "g1"}`.
6. Kafka acknowledges with `acks=all`. Ingestion API returns `202 Accepted` to game server.
7. Redis Updater consumes from Kafka. Executes:
   ```redis
   ZADD global_leaderboard 12500 p123
   ZADD region:NA_leaderboard 12500 p123
   HSET player:score:p123 current_score 12500 last_updated T
   ```
8. Total Redis write latency: ~1 ms. End-to-end from API receipt to Redis update: < 200 ms.
9. Next time a client queries `GET /v1/ranks?player_id=p123`:
   ```redis
   ZRANK global_leaderboard p123  → rank 48291 (0-indexed)
   ZSCORE global_leaderboard p123 → 12500
   ```
   Returns `{"rank": 48292, "score": 12500}` in < 5 ms.
10. Notification Engine checks: rank changed from 50,000 to 48,292 (delta > 1,000 threshold) → sends push notification.

---

## 4. Data Model

### Entities & Schema

**Redis Data Structures**:

```
# Global leaderboard: ZSET with score as float, player_id as member
# ZADD overwrites score if player already exists — natural idempotency
ZADD global_leaderboard:game:g1 12500 "player:p123"
ZADD global_leaderboard:game:g1 15000 "player:p456"

# Rank query: O(log N) — returns 0-indexed rank, lower index = higher rank
ZREVRANK global_leaderboard:game:g1 "player:p123"  → 48291  (48292th place)

# Top-N: O(log N + N) — returns top N players with scores
ZREVRANGE global_leaderboard:game:g1 0 99 WITHSCORES

# Regional leaderboard: separate ZSET per region
ZADD region:NA:game:g1   12500 "player:p123"
ZADD region:EU:game:g1   15000 "player:p456"
ZADD region:ASIA:game:g1  8000 "player:p789"

# Player metadata hash: current score, region, last rank
HSET player:meta:p123 
  score        12500
  region       NA
  global_rank  48292
  region_rank  22041
  last_updated 1712649600000
  season       "S12"

# Rank history (for trend display): circular list of last 100 ranks
LPUSH player:rank_history:p123 48292
LTRIM player:rank_history:p123 0 99   # Keep only last 100 entries

# Friend leaderboard: not stored as ZSET — computed on query
# (See Core Component: Friend Leaderboard)
```

**Cassandra — player_scores** (durable score store):
```sql
CREATE TABLE player_scores (
    game_id         TEXT,
    player_id       UUID,
    current_score   BIGINT,
    season          TEXT,
    region          TEXT,
    last_updated    TIMESTAMP,
    update_count    COUNTER,  -- total score events this season
    PRIMARY KEY ((game_id, season), player_id)
) WITH CLUSTERING ORDER BY (player_id ASC)
  AND default_time_to_live = 7776000;  -- 90-day TTL (end of season + buffer)
```

**Cassandra — score_events** (audit trail):
```sql
CREATE TABLE score_events (
    game_id         TEXT,
    player_id       UUID,
    event_time      TIMESTAMP,
    score_delta     INT,
    new_score       BIGINT,
    event_type      TEXT,    -- kill, win, objective, bonus
    game_session_id UUID,
    fraud_score     FLOAT,   -- anti-cheat score
    PRIMARY KEY ((game_id, player_id), event_time)
) WITH CLUSTERING ORDER BY (event_time DESC)
  AND default_time_to_live = 2592000;  -- 30-day retention
```

**Cassandra — leaderboard_snapshots** (historical end-of-day/week/season):
```sql
CREATE TABLE leaderboard_snapshots (
    game_id         TEXT,
    snapshot_type   TEXT,        -- daily | weekly | seasonal
    snapshot_date   DATE,
    rank            INT,
    player_id       UUID,
    score           BIGINT,
    region          TEXT,
    PRIMARY KEY ((game_id, snapshot_type, snapshot_date), rank)
) WITH CLUSTERING ORDER BY (rank ASC)
  AND default_time_to_live = 63072000;  -- 2-year retention
```

**Cassandra — player_rank_history** (per-player rank over time):
```sql
CREATE TABLE player_rank_history (
    game_id         TEXT,
    player_id       UUID,
    recorded_at     TIMESTAMP,
    global_rank     INT,
    regional_rank   INT,
    score           BIGINT,
    PRIMARY KEY ((game_id, player_id), recorded_at)
) WITH CLUSTERING ORDER BY (recorded_at DESC)
  AND default_time_to_live = 7776000;
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| Redis (Sorted Sets) | Real-time global/regional leaderboard | Sub-ms ZADD/ZRANK/ZRANGE; sorted set is precisely the right data structure; atomic operations; 100M ZSET members < 2 GB RAM | Volatile (requires AOF/RDB); RAM cost at scale; not suitable for complex ad-hoc queries | **Selected for all real-time rank operations** |
| Cassandra | Score persistence, history, snapshots | Linear horizontal scale; time-series model fits score history; HIGH write throughput; multi-AZ native | Eventual consistency; no sorted queries (ZRANK equivalent); no aggregation queries | **Selected for durable storage and historical data** |
| PostgreSQL | Score data | ACID, ORDER BY with index | Single node limit; doesn't scale to 100M players' real-time rank updates | Rejected |
| DynamoDB | Score persistence | Managed, low ops overhead, on-demand scaling | ZADD-equivalent requires custom secondary index + sort; sort key range scan for top-N is expensive | Considered; Cassandra selected for predictable latency under load |
| ClickHouse | Historical leaderboard analytics | Excellent aggregate queries, percentile calculations | Not a real-time rank store; insert latency too high for 100K/s updates | Good for offline analytics; not the primary store |
| MySQL | Friends leaderboard via JOIN | Familiar, JOIN for friend list | JOINs of 100-500 friends × millions of active players → high load | Rejected for hot path |

**Selection Rationale**:
- Redis Sorted Set is the exact implementation of a leaderboard data structure. `ZADD O(log N)` inserts and updates scores. `ZREVRANK O(log N)` returns the rank of a member. `ZREVRANGE O(log N + M)` returns the top-M players. With 100M players, N = 1e8; log2(1e8) = 27 operations — sub-microsecond in Redis terms. No other data structure provides these semantics at this performance level.
- Cassandra is the best write-heavy, key-value store for durable persistence. Score events are a time-series pattern (player_id, event_time → score_delta), which fits Cassandra's clustering column model perfectly.

---

## 5. API Design

### Score Submission

```
POST /v1/games/{game_id}/scores
Authorization: Bearer <game_server_token>
X-RateLimit-Policy: 500K updates/min per game_id

Request:
{
  "player_id":        "550e8400-e29b-41d4-a716-446655440000",
  "score_delta":       500,          // positive or negative; or use "score_absolute"
  "score_absolute":   null,          // set absolute score (mutually exclusive with delta)
  "event_type":       "kill",        // for anti-cheat validation
  "game_session_id":  "sess-abc123",
  "region":           "NA",
  "timestamp_ms":     1712649600000  // game server timestamp
}

Response 202 Accepted:
{
  "player_id":     "550e8400-...",
  "new_score":     12500,
  "previous_score": 12000,
  "status":        "accepted"
}

Response 422 Unprocessable Entity (anti-cheat rejection):
{
  "error": "invalid_score",
  "reason": "score_delta exceeds maximum for event_type:kill",
  "player_id": "550e8400-..."
}
```

### Leaderboard Queries

```
# Global rank for a specific player
GET /v1/games/{game_id}/leaderboard/rank?player_id={uuid}
Authorization: Bearer <client_token>
X-RateLimit-Policy: 100 requests/s per player_id

Response 200:
{
  "player_id":    "550e8400-...",
  "global_rank":  48292,         // 1-indexed
  "score":        12500,
  "regional_rank": 22041,
  "region":       "NA",
  "percentile":   51.71           // 100 * (1 - rank/total_players)
}

# Top-N global leaderboard
GET /v1/games/{game_id}/leaderboard/top?limit=100&offset=0
Authorization: Bearer <client_token>
Cache-Control: max-age=5

Response 200:
{
  "game_id": "g1",
  "total_players": 8324591,
  "entries": [
    { "rank": 1, "player_id": "...", "score": 998500, "display_name": "ProGamer123", "region": "EU" },
    { "rank": 2, "player_id": "...", "score": 997100, "display_name": "xXSlayerXx",  "region": "NA" }
  ],
  "generated_at": "2026-04-09T14:32:05Z",
  "cache_ttl_seconds": 5
}

# Rank neighborhood (players around a given rank)
GET /v1/games/{game_id}/leaderboard/around?player_id={uuid}&window=5
Authorization: Bearer <client_token>

Response 200:
{
  "target_rank": 48292,
  "entries": [
    { "rank": 48287, "player_id": "...", "score": 12600 },
    ...
    { "rank": 48292, "player_id": "550e8400-...", "score": 12500, "is_self": true },
    ...
    { "rank": 48297, "player_id": "...", "score": 12400 }
  ]
}

# Friend/social leaderboard
GET /v1/games/{game_id}/leaderboard/friends?player_id={uuid}
Authorization: Bearer <client_token>

Response 200:
{
  "player_rank_among_friends": 12,
  "total_friends_with_scores": 47,
  "entries": [
    { "rank": 10, "player_id": "...", "score": 14200, "display_name": "Alice" },
    { "rank": 11, "player_id": "...", "score": 13800, "display_name": "Bob" },
    { "rank": 12, "player_id": "550e8400-...", "score": 12500, "display_name": "You", "is_self": true }
  ]
}

# Historical leaderboard snapshot
GET /v1/games/{game_id}/leaderboard/history?date=2026-04-07&type=daily&limit=100
Authorization: Bearer <client_token>

Response 200:
{
  "snapshot_type": "daily",
  "snapshot_date": "2026-04-07",
  "total_players":  8200000,
  "entries": [
    { "rank": 1, "player_id": "...", "score": 995000 }
  ]
}
```

### WebSocket Notification Stream

```
WS /v1/games/{game_id}/notifications
Authorization: Bearer <player_token>

Subscribe (client → server):
{ "action": "subscribe", "events": ["rank_change", "milestone"] }

Rank change event (server → client):
{
  "type": "rank_change",
  "player_id": "550e8400-...",
  "previous_rank": 50001,
  "new_rank": 48292,
  "score": 12500,
  "milestone": "top_50000",
  "timestamp": "2026-04-09T14:32:05Z"
}
```

---

## 6. Deep Dive: Core Components

### Core Component 1: Redis Sorted Set at Scale

**Problem it solves**: The leaderboard must support 100M players, with ZADD at 100K updates/second and ZRANK at 50K queries/second, all with < 10 ms response time. A single Redis instance cannot hold 100M entries in a ZSET and serve this throughput. A Redis ZSET stores members as a doubly-linked list of sorted sets (skip list internally), providing O(log N) for ZADD and ZRANK. With N=100M: log2(100M) ≈ 27 hops per operation. Redis does ~1M simple ops/second, so 1M/27 ≈ 37K ZADD ops/second per instance — insufficient for 100K/s requirement. The problem: how to scale the sorted set beyond a single Redis instance while maintaining correct global rank.

**Approach Comparison**:

| Approach | ZADD Throughput | ZRANK Correctness | Complexity | Memory |
|---|---|---|---|---|
| Single Redis instance | ~37K/s | Perfect | Low | 1.6 GB |
| Redis Cluster (hash sharding by player_id) | High | WRONG — ZRANK across shards is undefined | Medium | Distributed |
| Shard ZSET by score range (bucketing) | High | Correct (sum ranks across buckets) | Medium | Distributed |
| Pre-computed rank with periodic refresh | High | Approx (stale by refresh interval) | Low | Small |
| Single Redis Primary + read replicas | Read: High; Write: ~37K/s | Read: eventual | Medium | Replicated |
| Approximate rank (percentile estimation) | Very high | Approximate (±1%) | Low | Very small |
| Redis Cluster with ZADD to all + ZRANK on dedicated primary | High | Correct on primary | High | 3× memory |

**Selected Approach: Score-Range Bucketed ZSET (Hierarchical)**

Divide the score range into B buckets (e.g., B=100). Each bucket covers a score range (e.g., [0–1000), [1000–2000), ...). Each bucket is a separate Redis ZSET on a different Redis node. ZADD goes to the correct bucket's ZSET. ZRANK is computed as: rank = sum of sizes of all buckets above + ZRANK within the bucket.

```
Bucket structure (example with 100 buckets, max_score = 10M):
  bucket_0:  scores [0,      100,000)    → Redis node 1
  bucket_1:  scores [100,000, 200,000)  → Redis node 2
  ...
  bucket_99: scores [9,900,000, ∞)      → Redis node 100

Player with score 12,500 → bucket_0
Player with score 500,000 → bucket_4
```

For exact rank computation, we also maintain a `bucket_sizes` HASH:
```redis
HSET bucket_sizes game:g1  bucket_0 4250000  bucket_1 2100000  ...
```

ZRANK(player_id, score=12500):
1. Determine bucket: `bucket = score // bucket_size`
2. `ZREVRANK bucket_0:game:g1 "player:p123"` → local_rank = 1,800,000
3. Sum sizes of buckets with score ranges above bucket_0 (all buckets for scores > 100K):
   `rank = sum(bucket_1_size, ..., bucket_99_size) + local_rank`
   `rank = (100M - 4.25M) + 1.8M = 97.55M ... ` — this is incorrect direction.

**Corrected Direction** (rank = number of players with HIGHER score):
1. `ZREVRANK bucket_0:game:g1 "player:p123"` → position within bucket_0 (0-indexed, higher score = lower index in ZREVRANK) = 2,449,999 (player is 2,450,000th within bucket_0 from the top of bucket_0)
2. Rank = (number of players in buckets with score > bucket_0's max) + local_rank_in_bucket
3. `rank = sum(bucket_1_size + ... + bucket_99_size) + 2,449,999`

This is correct but requires summing bucket sizes — O(B) Redis operations. Optimization: store cumulative bucket sizes pre-computed:
```redis
HSET bucket_cumulative_above game:g1  
  bucket_0 95750000   # sum of sizes of buckets 1..99
  bucket_1 93000000   # sum of sizes of buckets 2..99
  ...
```
Update these whenever a player moves between buckets (score crosses a bucket boundary). ZRANK becomes O(log N / B) (ZREVRANK within bucket) + O(1) (Redis HGET for cumulative above).

**Full Implementation Pseudocode**:

```python
class BucketedLeaderboard:
    BUCKET_SIZE = 100_000     # score range per bucket
    NUM_BUCKETS = 101         # 0 to 10M+
    
    def __init__(self, redis_cluster, game_id: str):
        self.redis = redis_cluster
        self.game_id = game_id

    def _get_bucket(self, score: int) -> int:
        return min(score // self.BUCKET_SIZE, self.NUM_BUCKETS - 1)

    def _bucket_key(self, bucket: int) -> str:
        return f"lb:{self.game_id}:bucket:{bucket}"

    def _cumulative_key(self) -> str:
        return f"lb:{self.game_id}:cumulative_above"

    def add_or_update(self, player_id: str, new_score: int, old_score: int = None):
        """
        Update player score. Handles bucket transitions atomically.
        """
        new_bucket = self._get_bucket(new_score)
        
        if old_score is not None:
            old_bucket = self._get_bucket(old_score)
        else:
            old_bucket = None
        
        # Lua script for atomic bucket update
        lua_script = """
        local new_bucket_key = KEYS[1]
        local old_bucket_key = KEYS[2]
        local cumulative_key = KEYS[3]
        local player_id = ARGV[1]
        local new_score = tonumber(ARGV[2])
        local old_bucket = tonumber(ARGV[3])
        local new_bucket = tonumber(ARGV[4])
        
        -- Add to new bucket (or update score if already there)
        redis.call('ZADD', new_bucket_key, new_score, player_id)
        
        if old_bucket_key ~= new_bucket_key then
            -- Player moved to different bucket: remove from old bucket
            if old_bucket_key ~= '' then
                redis.call('ZREM', old_bucket_key, player_id)
            end
            
            -- Update cumulative counts for all affected buckets
            -- All buckets below new_bucket gain 1 player above them
            -- All buckets below old_bucket lose 1 player above them
            -- This is tracked via HINCRBY on cumulative_above hash
            -- Buckets between min(old,new) and max(old,new)-1 are affected
            local lo = math.min(old_bucket, new_bucket)
            local hi = math.max(old_bucket, new_bucket)
            local direction = (new_bucket > old_bucket) and 1 or -1
            
            for b = lo, hi - 1 do
                -- Buckets below lo: their "above count" changes
                redis.call('HINCRBY', cumulative_key, 'b' .. b, -direction)
            end
        end
        
        return 1
        """
        
        old_bucket_key = self._bucket_key(old_bucket) if old_bucket is not None else ''
        
        self.redis.eval(
            lua_script, 3,
            self._bucket_key(new_bucket),
            old_bucket_key,
            self._cumulative_key(),
            player_id, new_score, 
            old_bucket if old_bucket is not None else new_bucket,
            new_bucket
        )

    def get_rank(self, player_id: str, score: int) -> int:
        """
        Returns 1-indexed global rank (1 = highest score).
        """
        bucket = self._get_bucket(score)
        bucket_key = self._bucket_key(bucket)
        cumulative_key = self._cumulative_key()
        
        # Local rank within bucket (0-indexed, ZREVRANK = 0 means highest in bucket)
        local_rank = self.redis.zrevrank(bucket_key, player_id)
        if local_rank is None:
            return -1  # Player not found
        
        # Number of players with score in higher buckets (cumulative above)
        cumulative_above = self.redis.hget(cumulative_key, f"b{bucket}")
        higher_count = int(cumulative_above or 0)
        
        # Global rank = players above (in higher buckets) + local rank + 1 (1-indexed)
        return higher_count + local_rank + 1

    def get_top_n(self, n: int = 100) -> list:
        """
        Returns top N players globally. Starts from the highest bucket.
        """
        results = []
        remaining = n
        
        # Iterate buckets from highest score to lowest
        for bucket in range(self.NUM_BUCKETS - 1, -1, -1):
            if remaining <= 0:
                break
            
            bucket_key = self._bucket_key(bucket)
            # Get top `remaining` entries from this bucket
            entries = self.redis.zrevrange(
                bucket_key, 0, remaining - 1, withscores=True
            )
            
            for player_id, score in entries:
                results.append({'player_id': player_id, 'score': score})
            
            remaining -= len(entries)
        
        # Assign global ranks (1-indexed)
        return [{'rank': i+1, **r} for i, r in enumerate(results)]
```

**Memory Analysis**:
- 100M players × 16 bytes/entry (8-byte score + 8-byte player_id pointer) = 1.6 GB for all ZSET data.
- 100 bucket ZSETs, each with up to 1M entries average = 100 × 1M × 16 B = 1.6 GB total (same).
- Cumulative hash: 100 buckets × 8 bytes = negligible.
- Conclusion: bucketed approach uses same memory but distributes across 100 Redis nodes, each holding ~16 MB. ZREVRANK on a 1M-entry ZSET: log2(1M) = 20 hops = ~0.2 µs — a huge improvement over log2(100M) = 27 hops on a single node, but more importantly, each node handles only 1/100 of the write load.

**Interviewer Q&As**:

Q1: Why not use Redis Cluster with hash-based sharding for the leaderboard? It would naturally distribute the 100M entries.
A: Redis Cluster shards data by hash slot, distributing keys across nodes. A ZADD for player_id="p123" goes to whichever node holds the slot for that key. But a ZADD to `global_leaderboard` must go to the SAME key on ONE node — you can't shard a single ZSET across multiple nodes in Redis Cluster. ZRANK on a single ZSET requires the entire set to be on one node. Redis Cluster helps when you have MANY separate keys (like per-player hashes), but a single ZSET for 100M players is a single key — Cluster can't help. The bucketing approach we selected effectively implements manual sharding of the conceptual single ZSET into 100 real ZSETs, each on a separate node, while preserving the ability to compute exact global rank via the cumulative_above index.

Q2: Your bucketing approach assumes a known score range. What if scores can grow unboundedly (e.g., competitive game where top players have 100M+ points)?
A: We handle this with two strategies: (1) Dynamic bucket boundaries: instead of fixed ranges (0–100K, 100K–200K), we use percentile-based buckets. The top 1% of scores are in bucket_99, the next 1% in bucket_98, etc. These boundaries are recomputed hourly by scanning the current score distribution and stored in a separate metadata key. ZADD needs to look up the current bucket boundaries (O(1) with a sorted boundaries array). (2) Separate "overflow" bucket: bucket_99 handles all scores above the 99th percentile. If this bucket exceeds 2M entries, we split it by adding bucket_100 with a higher boundary. Bucket splitting is an O(N) operation for the affected bucket but happens infrequently. (3) Score normalization: if absolute scores grow unboundedly, normalize to a relative score (rank within current period) — players' lifetime ranks become the score. This keeps the value range bounded.

Q3: ZADD is O(log N). At 100K updates/second on a 1M-entry bucket, the theoretical throughput is limited. How do you handle bursts?
A: Redis handles ~200K ZADD ops/second on typical hardware (O(log N) at N=1M is very fast). At 100K updates/second split across 100 buckets, each bucket receives ~1K updates/second — well within single-instance Redis throughput. Burst handling: Redis pipeline batching. The Redis Updater worker accumulates 100ms of score updates and sends them as a Redis pipeline (one round-trip for many commands). At 100K updates/s × 100ms = 10K updates per pipeline batch × 0.2 µs/ZADD = 2 ms processing time. The pipeline command overhead is amortized across 10K operations. Burst capacity: Kafka buffers 5–10 seconds of burst (100K × 10s = 1M messages) if Redis momentarily falls behind.

Q4: How does your leaderboard handle a tie — two players with the same score?
A: Redis ZSET handles ties by secondary sorting on the member string (lexicographic order of player_id). If player "p100" and "p200" both have score 12500, Redis will consistently return "p100" as having a higher rank than "p200" (because "p100" < "p200" lexicographically). This is deterministic but not necessarily fair. For a fairer tie-break: modify the score stored in Redis to encode both the score and a timestamp: `float_score = score + (1 / timestamp)`. A player who achieved the score earlier gets a marginally higher float value, giving them the tie-breaking edge. Since timestamps are in milliseconds (13 digits) and scores are at most 10 digits, `score + 1/timestamp` still fits in IEEE 754 double precision without collision. Alternative: use separate columns for score and tie-break time (Cassandra can store both; Redis ZSET always needs a single float).

Q5: Your cumulative_above hash is updated on every cross-bucket score update. How do you handle concurrent updates that cause cumulative_above to become incorrect?
A: The Lua script executes atomically on Redis (Redis is single-threaded for command execution). Within the Lua script, the HINCRBY for cumulative_above happens atomically with the ZADD and ZREM. There is no concurrency issue for operations on the same Redis node. However, our architecture uses different Redis nodes for different buckets. Cross-bucket transitions require atomic updates to cumulative_above (which may be on a different node) AND the bucket ZSETs. This is a distributed transaction problem. Our solution: accept a brief inconsistency. The cumulative_above hash is on a separate "meta" Redis node. Cross-bucket transitions update the bucket ZSET first, then update cumulative_above within 1ms (separate command). During this 1ms window, ranks may be off by 1. This is acceptable for a leaderboard (brief rank inconsistency is invisible to users). For guaranteed consistency, we'd use a Lua MULTI-EXEC across nodes — but Redis Cluster doesn't support MULTI/EXEC across slots. Two-phase commit is too expensive for 100K/s updates.

---

### Core Component 2: Friend/Social Leaderboard

**Problem it solves**: A friend leaderboard shows a player's rank among only their friends, not the global population. This is a fundamentally different problem from the global leaderboard: there is no single ZSET containing all friends (each player has a different friend list). Computing "rank among 200 friends" on-the-fly requires fetching scores for all friends and sorting them. The challenge: do this in < 100 ms for a player with up to 1,000 friends, with 50K concurrent queries/second.

**Approach Comparison**:

| Approach | Latency | Write Overhead | Correctness | Scale |
|---|---|---|---|---|
| Per-query friend score lookup + sort | O(F log F) where F=friends count | None | Perfect | Poor at high QPS |
| Pre-computed friend ZSET per player | O(log F) for ZRANK | O(F) ZADD per score update | Perfect | Very poor (each update touches F ZSETs) |
| Score aggregation at query time (Redis ZUNIONSTORE) | O(F log F) | None | Perfect | Medium |
| Approximate rank (percentile of known friend scores) | O(F) | None | Approx | Good |
| Batch pipeline: fetch F scores, sort client-side | O(F) Redis HGET + sort | None | Perfect | Good |

**Selected Approach: Batch Pipeline — Fetch All Friend Scores + In-Memory Sort**

Instead of trying to maintain a friend-specific ZSET (which requires O(F) writes per score update and creates O(N×F) total ZADD operations — 100M players × 200 avg friends = 20B additional ZADD per score update — impossible), we compute the friend leaderboard at query time.

**Algorithm**:
1. Fetch friend list for player X from the social graph service (returns list of friend player_ids, up to 1,000).
2. Redis pipeline: `HGET player:meta:{friend_id} score` for all friends → returns 1,000 scores in one round-trip.
3. Sort the scores in memory: O(F log F) = O(1,000 × 10) = 10,000 comparisons — negligible CPU.
4. Find player X's rank in the sorted list.
5. Return the N players around X.

**Performance Analysis**:
- Friend list fetch (social graph service, cached in Redis): 1 ms
- Redis pipeline of 1,000 HGET: 1 round-trip + 1 ms processing = ~2 ms
- In-memory sort of 1,000 elements: < 0.1 ms
- Total: ~3–5 ms — well within the 100 ms SLA.

**Scaling to 50K QPS**:
- Each request requires ~1,000 HGET operations → 50K × 1,000 = 50M HGET ops/second.
- Redis HGET: ~1M ops/second per node. Need 50 Redis nodes just for friend leaderboard queries.
- This is the key bottleneck. Mitigations:
  1. **Cache the friend leaderboard result**: TTL=10 seconds. Most users view their friend leaderboard but don't open it every second. At 10s TTL, cache hit rate > 90%, reducing Redis load to 5M HGET/s (5 nodes).
  2. **Compress the friend score vector**: Store friend scores as a Redis HASH per player: `player:friends:scores:{player_id}` → `{friend_id: score, ...}`. Updated asynchronously when a friend's score changes. This single HGETALL replaces 1,000 individual HGETs, reducing to 1 Redis command per friend leaderboard query. **Trade-off**: requires updating this hash on every friend's score change — O(friends_count) writes per score update. For 100 avg friends: 100K score updates/s × 100 friends = 10M HSET ops/s. Feasible but requires 10+ Redis nodes dedicated to friend score caches.

**Friend Leaderboard with Cached Score Vector**:

```python
class FriendLeaderboard:
    FRIEND_SCORES_TTL = 30      # seconds
    FRIEND_LIST_TTL = 300       # seconds (friend lists change rarely)

    def __init__(self, redis, social_graph_service):
        self.redis = redis
        self.social = social_graph_service

    def get_friend_leaderboard(self, player_id: str, 
                                game_id: str,
                                context_size: int = 5) -> dict:
        """
        Returns friend leaderboard centered on player_id.
        """
        # Step 1: Get friend list (cached)
        friend_list_key = f"friends:{player_id}"
        friends = self.redis.smembers(friend_list_key)
        
        if not friends:
            friends = self.social.get_friends(player_id)  # API call: ~10 ms
            if friends:
                pipe = self.redis.pipeline()
                pipe.sadd(friend_list_key, *friends)
                pipe.expire(friend_list_key, self.FRIEND_LIST_TTL)
                pipe.execute()

        # Add the player themselves
        all_players = set(friends) | {player_id}
        
        # Step 2: Fetch all scores in one pipeline
        pipe = self.redis.pipeline()
        for pid in all_players:
            pipe.hget(f"player:meta:{pid}", "score")
        scores_raw = pipe.execute()
        
        # Step 3: Build (player_id, score) pairs, filter out players with no score
        player_scores = [
            (pid, int(s)) for pid, s in zip(all_players, scores_raw) if s is not None
        ]
        
        # Step 4: Sort descending by score
        player_scores.sort(key=lambda x: -x[1])
        
        # Step 5: Find player's position
        rank = next((i for i, (pid, _) in enumerate(player_scores) 
                     if pid == player_id), None)
        
        if rank is None:
            return {'error': 'player_not_found'}
        
        # Step 6: Return context window around player
        start = max(0, rank - context_size)
        end = min(len(player_scores), rank + context_size + 1)
        
        return {
            'player_rank_among_friends': rank + 1,  # 1-indexed
            'total_friends_with_scores': len(player_scores),
            'entries': [
                {
                    'rank': i + 1,
                    'player_id': pid,
                    'score': score,
                    'is_self': pid == player_id
                }
                for i, (pid, score) in enumerate(player_scores[start:end], start=start)
            ]
        }
```

**Interviewer Q&As**:

Q1: What's the maximum number of friends the friend leaderboard can support before latency exceeds 100 ms?
A: The latency scales with F (friends count). Redis pipeline for F keys: ~2 ms for F=1,000 (one round-trip + network). In-memory sort: negligible. At F=10,000 friends: pipeline still ~2 ms (same round-trip, slightly more data), sort takes O(10K × 14) ≈ 140K comparisons at ~100M comparisons/second = 1.4 ms. Total: ~4 ms — still well within 100 ms SLA. The bottleneck at large F is Redis aggregate throughput (10K HGET per request). With 50K QPS and F=10,000: 500M HGET/s — requires 500 Redis nodes. At this scale, switch to the "cached score vector" approach: one HGETALL per request regardless of F, with O(F) background update on score change.

Q2: How do you keep friend scores up to date in the cached score vector without lagging by the cache TTL?
A: When a player's score is updated, the Notification Engine (a Kafka consumer) also updates all of that player's friends' `player:friends:scores:{friend_id}` caches via a `HSET player:friends:scores:{friend_id} {player_id} {new_score}` command. This is a fan-out: for each score update, we look up the scoring player's friend list and push the score to each friend's cache key. With 100 avg friends: 100K score updates/s × 100 fan-outs = 10M HSET ops/s. Pros: friend leaderboard is always fresh (< 1s lag). Cons: 10M ops/s requires significant Redis capacity. Trade-off: for casual games (friend count 20–30), this is fine. For social games with up to 5,000 friends, fan-out is expensive — we cap fan-out at 500 friends (showing "mutual friends" subset) for performance.

Q3: What if a player has 50,000 friends (a social media influencer)?
A: At 50,000 friends, the "fetch all scores pipeline" approach still technically works (2 ms for the pipeline), but the fan-out write path (50,000 HSET per score update) is prohibitive. Two strategies: (1) **Lazy computation**: don't maintain a cached score vector. At query time, batch-fetch all 50,000 friend scores via pipeline (2 ms), sort in memory (O(50K × 16) = 800K comparisons = 8 ms), return top-100 and player's rank. Total: ~15 ms — still within 100 ms SLA. Cache the result for 30 seconds. (2) **Tiered friend leaderboard**: For players with > 1,000 friends, show a sampled leaderboard: randomly sample 200 friends, display rank within that sample with a disclaimer "Among 200 of your top active friends." Cache the sample for 60 seconds. This is the approach Instagram uses for their activity feeds (sampling at scale).

---

### Core Component 3: Anti-Cheat Score Validation

**Problem it solves**: Leaderboards are high-stakes (prizes, status, bragging rights). Without anti-cheat, bots and cheaters can inject arbitrary scores, poisoning the leaderboard and destroying the experience for legitimate players. At 100K score updates/second, validation must be fast (< 5 ms synchronous overhead) and cover common cheating patterns: impossible scores, velocity cheating (accumulating points faster than humanly possible), botting (automated play detected by pattern), and external memory manipulation (game client exploits sending falsified scores).

**Approach Comparison**:

| Validation Approach | Latency | Cheat Coverage | False Positive Risk | Notes |
|---|---|---|---|---|
| Server-side rule-based | < 1 ms | Catches obvious cases | Low | Max score per event, max delta per period |
| Statistical anomaly (z-score) | < 5 ms | Catches velocity cheating | Medium (new legitimate players look like cheats) | Compare to population baseline |
| Replay verification | 100–500 ms | Catches most client manipulation | Low | Game server re-executes game events |
| Client-side anti-cheat (trusted client) | N/A | Unreliable (client can be modified) | N/A | Defense in depth only |
| ML behavior model | 10–50 ms (async) | Catches sophisticated bots | Tunable | Best coverage, higher latency |

**Selected Approach: Synchronous Rule-Based + Async ML**

Synchronous validation (< 2 ms) catches obvious fraud before the score enters Kafka. Async ML scoring runs after the score is accepted and can retroactively flag scores for investigation.

**Anti-Cheat Pseudocode**:

```python
class AntiCheatValidator:
    # Game-specific rules: max score delta per event type
    MAX_DELTA_PER_EVENT = {
        "kill":       1000,
        "win":        5000,
        "objective":  2000,
        "bonus":      500,
        "default":    3000
    }
    
    # Max score per hour per player (velocity limit)
    MAX_SCORE_PER_HOUR = 100_000
    
    # Max score any player has ever achieved (global maximum sanity check)
    ABSOLUTE_MAX_SCORE = 10_000_000
    
    def __init__(self, redis):
        self.redis = redis

    def validate_synchronous(self, update: ScoreUpdate) -> ValidationResult:
        """
        Fast synchronous checks. Run before Kafka publish.
        Returns (is_valid, reason).
        """
        # Rule 1: Event-type delta limit
        max_delta = self.MAX_DELTA_PER_EVENT.get(
            update.event_type, self.MAX_DELTA_PER_EVENT["default"]
        )
        if abs(update.score_delta) > max_delta:
            return ValidationResult(
                valid=False,
                reason=f"delta_{update.score_delta}_exceeds_max_{max_delta}_for_{update.event_type}"
            )
        
        # Rule 2: Absolute score sanity check
        current_score = self._get_current_score(update.player_id, update.game_id)
        new_score = current_score + update.score_delta
        if new_score > self.ABSOLUTE_MAX_SCORE or new_score < 0:
            return ValidationResult(
                valid=False,
                reason=f"absolute_score_{new_score}_out_of_valid_range"
            )
        
        # Rule 3: Velocity check (score accumulation rate)
        velocity_key = f"velocity:{update.game_id}:{update.player_id}:{int(time.time() // 3600)}"
        current_hour_score = self.redis.incr(velocity_key, update.score_delta)
        self.redis.expire(velocity_key, 7200)  # 2-hour TTL
        
        if current_hour_score > self.MAX_SCORE_PER_HOUR:
            return ValidationResult(
                valid=False,
                reason=f"velocity_limit_exceeded_{current_hour_score}_in_current_hour"
            )
        
        # Rule 4: Session context (game server must provide session_id)
        if not update.game_session_id:
            return ValidationResult(valid=False, reason="missing_game_session_id")
        
        # Rule 5: Check if game_session_id is a known active session
        # (Game server registered this session at session start)
        session_key = f"session:{update.game_id}:{update.game_session_id}"
        session_exists = self.redis.exists(session_key)
        if not session_exists:
            return ValidationResult(valid=False, reason="invalid_or_expired_session_id")
        
        return ValidationResult(valid=True, fraud_score=0.0)

    def validate_async_ml(self, update: ScoreUpdate, current_score: int):
        """
        Async ML validation. Runs after Kafka publish.
        Flags suspicious scores retroactively.
        """
        features = {
            'score_delta': update.score_delta,
            'new_score': current_score,
            'event_type_encoded': self._encode_event_type(update.event_type),
            'time_of_day': datetime.utcnow().hour,
            'day_of_week': datetime.utcnow().weekday(),
            'player_session_count_today': self._get_session_count(update.player_id),
            'player_avg_score_per_session': self._get_avg_score(update.player_id),
            'score_delta_z_score': self._compute_z_score(update),
        }
        
        # ML model inference (ONNX runtime, < 10 ms)
        fraud_score = self.ml_model.predict(features)
        
        if fraud_score > 0.8:
            # High confidence fraud: retroactively flag
            self._flag_player(update.player_id, update.game_id, fraud_score, features)
            # Optionally: remove player from leaderboard pending review
            if fraud_score > 0.95:
                self._quarantine_player(update.player_id, update.game_id)
        
        return fraud_score
```

**Interviewer Q&As**:

Q1: Your synchronous anti-cheat adds 2 ms to the score update path. At 100K/s, how much does this impact the API?
A: The 2 ms synchronous validation is the dominant latency on the Ingestion API (Redis HGET for current score = 0.5 ms, rule checks = 0.1 ms, Redis INCR for velocity = 0.5 ms, session check = 0.5 ms = ~1.6 ms total). This is within our < 50 ms p99 target for the full API call (which includes network + Kafka publish overhead). The Redis operations are the bottleneck: at 100K requests/s × 4 Redis ops each = 400K Redis ops/s. With 5 Redis nodes: 80K ops/s per node — well within Redis's 1M ops/s capacity.

Q2: How do you handle a legitimate player who suddenly improves dramatically (a professional player joins, a player has a breakthrough)?
A: Legitimate improvement looks different from cheating: (1) Gradual increase vs. step-change: a legitimate player improves over sessions, while a cheater sets their score directly to maximum. (2) Session context: legitimate improvement happens within valid game sessions. (3) Z-score over time: if a player's score delta is consistently 4σ above mean (every game, not just one), that's a red flag. A one-time 4σ game is likely legitimate (exceptional performance). The ML model is trained on labeled data from both legitimate players and confirmed cheaters. Features include session-level patterns (first 5 minutes of a session vs. later), rank progression history (a player who jumps from rank 10M to rank 1 in one session is suspect), and time-of-day patterns (bot activity often peaks during off-hours in the player's region).

Q3: How do you retroactively handle a case where a cheater's score was in the leaderboard for 3 hours before detection?
A: Retroactive correction is a separate process from real-time validation. On ML detection: (1) The cheater's player_id is added to a quarantine list (Redis SET: `quarantine:{game_id}`). (2) The Quarantine Worker executes: `ZREM global_leaderboard p_cheater`, `ZREM region:NA_leaderboard p_cheater`. (3) All players with ranks below the cheater's former position effectively move up by 1. (4) The cheater's score in Cassandra is marked `is_fraud=true`. (5) Affected players receive rank-change notifications (they moved up). (6) If the cheater was in the top 10 and the leaderboard was publicly displayed (tournament), human review is required before removal — the system flags for ops team review with a 30-minute SLA.

---

## 7. Scaling

### Horizontal Scaling

- **Score Ingestion API**: Stateless, scale to 100 pods. At 100K updates/s and < 50 ms p99, each pod handles 1K updates/s with comfortable headroom. Anti-cheat validation adds 2 ms per request (Redis ops) — Redis cluster handles the load.
- **Redis Updater Workers**: Kafka consumer group. Scale to 32 workers (matching 32 Kafka partitions). Each worker processes ~3,125 score updates/s. Uses pipelining: batches 100ms of updates into a single Redis pipeline (312 ZADDs per pipeline). At 312 ZADDs × 0.2 µs each = 0.0624 ms processing time per batch — extremely fast. Workers are bound by Kafka consumption throughput, not Redis write speed.
- **Redis Cluster**: 10-node cluster (5 primary + 5 replica). Global leaderboard bucket ZSETs distributed across primaries. Each primary handles: 10K ZADD/s (from Redis Updaters) + 50K ZRANK queries/s = 60K ops/s. Redis handles 200K+ ops/s per node — comfortable.
- **Cassandra**: 5-node cluster for score persistence. 100K writes/s distributed: 20K writes/s per node — well within Cassandra's 100K writes/s per node capacity.

### DB Sharding

Redis: Bucket-based ZSET sharding (described in Core Component 1). Each of the 100 score-range buckets is a separate key. Redis Cluster assigns keys to slots via CRC16 hash. Using hash tags: `lb:{game_id}:bucket:{n}` — the `game_id` tag ensures all buckets for a game stay on related slots (optional, for same-node locality of cumulative_above access).

Cassandra: `player_scores` partitioned by `(game_id, season)` — all season data for a game is co-located. This optimizes snapshot queries (reading all players for a season end). For score events: partitioned by `(game_id, player_id)` — all events for a player in a game are co-located, enabling efficient player score history retrieval.

### Replication

- Redis: 1 replica per primary (cross-AZ). Async replication, typical lag < 10 ms. For leaderboard correctness: reads go to primaries (Redis Cluster default). Replica serves as hot standby — no eventual consistency risk for rank reads.
- Cassandra: RF=3, LOCAL_QUORUM. Tolerates 1 node failure without data loss.
- Kafka: RF=3, acks=all. Zero data loss guarantee.

### Caching

- **API response cache**: Top-100 global leaderboard cached in Redis with 5-second TTL. At 50K queries/s for top-100 with 5s TTL and 10K active game sessions: cache hit rate ~99.9% (same N players see the same top-100 within 5s).
- **Player metadata cache**: `player:meta:{player_id}` Redis HASH serves as the L1 cache for player current score and rank. Updated on every ZADD. No separate caching needed — Redis IS the cache.
- **Friend list cache**: Redis SET per player with 5-minute TTL. Friend lists change rarely compared to score updates.
- **Regional leaderboard cache**: Same 5-second TTL. Regional queries share cached results across all players in that region.

### CDN

- The Leaderboard Query API serves leaderboard results that can be edge-cached for top-N queries: `Cache-Control: public, max-age=5`. CDN (CloudFront) caches top-100 leaderboard responses globally, serving 99%+ of top-100 queries from edge (< 10 ms vs. < 20 ms from origin). Player-specific rank queries (contain player_id) are private (`Cache-Control: private, no-store`) — not CDN-cacheable.
- Tournament-mode leaderboards with millions of viewers can be served entirely from CDN for the top-N display.

### Interviewer Q&As (Scaling)

Q1: How do you handle a "score event storm" during a tournament where 1M concurrent players all score simultaneously (end-of-round awards)?
A: A tournament end-of-round event triggers 1M simultaneous score updates — 10× our design target. Mitigation: (1) Game servers spread the updates: instead of all servers sending at T=0, they're configured to jitter updates by up to 5 seconds (T+0 to T+5). This spreads the burst across 5 seconds: 1M / 5s = 200K/s — 2× peak, manageable. (2) Kafka absorbs the instantaneous burst (Kafka can accept millions of messages/second on a properly partitioned topic). The Redis Updater workers process at their maximum rate (100K/s effective) and drain the Kafka backlog within 10 seconds. During this 10-second window, leaderboard reads show slightly stale ranks — acceptable. (3) We auto-scale the Redis Updater worker pool via Kubernetes HPA on the Kafka consumer lag metric: if lag > 1M messages, scale from 32 to 64 workers.

Q2: Your Redis stores 100M player scores. How do you handle a Redis cluster failure during peak load?
A: Redis Cluster with cross-AZ replicas: a single node failure triggers automatic failover to the replica within 10 seconds. During failover, writes to the failed shard's slots are rejected. We handle this: (1) Kafka buffers score updates during the 10-second failover window — at 100K/s × 10s = 1M buffered updates, which is replayed automatically once the replica is promoted. (2) The Leaderboard Query API serves stale reads from the replica (now promoted to primary) immediately. (3) If an entire AZ fails (3 nodes down simultaneously): the cluster is degraded but operational if RF=3 and the other 2 AZs have copies. In a 3-AZ setup with 10 nodes: nodes 1,2,3 in AZ-A; nodes 4,5,6 in AZ-B; nodes 7,8,9,10 in AZ-C. AZ-A failure: AZ-B and AZ-C promote their replicas, cluster remains fully operational.

Q3: How do you prevent leaderboard enumeration — an attacker scraping the entire leaderboard to harvest all player IDs?
A: (1) Rate limiting per IP: maximum 100 leaderboard API requests per minute per IP. This limits scraping speed significantly (100 pages × 100 results = 10K players/minute, scraping 100M players would take 167 hours). (2) Pagination with opaque cursors: the cursor for the next page is encrypted (AES-256 of `{rank, timestamp, request_token}`). Cursors expire after 60 seconds, preventing precomputed enumeration. (3) The leaderboard API requires authentication (game client token). Anonymous access is not allowed. (4) Player IDs in the leaderboard are display names (not UUIDs) — actual player UUIDs are only returned to authenticated requests for the player's own data. (5) Shadow enumeration detection: if an IP requests more than 10 sequential leaderboard pages in one session, flag it for review.

Q4: How does your system handle concurrent score updates for the same player (game server sends two score events simultaneously)?
A: Two simultaneous score updates for the same player on different goroutines of the same game server, both going to Kafka. Kafka partitions by player_id, so both events land on the same partition in FIFO order. The Redis Updater processes this partition serially — the second ZADD is always processed after the first. Since ZADD is `ZADD key score member` (sets score absolutely or compares for NX/GT/LT options), we use `ZADD GT` (greater than) option for score-only-increasing metrics: `ZADD global_leaderboard GT 12500 p123`. This ensures even if events arrive out of order, the final score is the maximum. For games where scores can decrease (penalties), we use the timestamp-ordered Kafka consumption guarantee: the Kafka consumer processes updates in order, so the final ZADD is always the most recent event's score.

Q5: How does the leaderboard scale to support 100 different games simultaneously on the same infrastructure?
A: Game isolation is via naming convention: `lb:{game_id}:bucket:{n}` for ZSETs. Each game has its own set of 100 bucket ZSETs. With 100 games × 100 buckets = 10,000 ZSET keys. At 100M players per game × 100 games = 10B total ZSET entries — this would require 10× more Redis memory (16 TB). Realistically, players are distributed across games: 100M unique players × avg 3 games played = 300M player-game pairs. With 100M players in the largest game and 100K in the smallest: total ZSET entries ~ 300M × 16 B = 4.8 GB. Kafka: separate topic per game (`scores:g1`, `scores:g2`) or single topic with game_id partition key. Cassandra: single table with `(game_id, player_id)` partition key — naturally scales. The system architecture scales to N games without redesign — it's purely additive.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component | Failure Mode | Detection | Mitigation | Data Loss Risk |
|---|---|---|---|---|
| Score Ingestion API pod crash | Pod dies mid-request | K8s liveness probe | K8s restart; in-flight requests retry (idempotent: same score_delta + session_id) | None (game server retries with same score_delta) |
| Kafka broker failure | Leader election | Consumer lag spike | KRaft auto-elects new leader < 10s; Kafka buffers all pending updates | None (acks=all) |
| Redis node failure | ZADD failures for affected slots | Cluster bus FAIL state | Cross-AZ replica promoted < 10s; rank reads temporarily stale | Possible 10s of score updates not in Redis (replayed from Kafka after failover) |
| Redis Updater worker crash | Consumer offset not committed | K8s restart + Kafka lag metric | K8s restarts pod; Kafka redelivers from last committed offset (at-least-once ZADD) | None (ZADD is idempotent for same score) |
| Cassandra node failure | Write timeout | Cassandra health check | LOCAL_QUORUM writes to remaining 2 nodes; no downtime | None (RF=3) |
| Anti-cheat service slow | High validation latency | p99 latency alert | Circuit breaker opens: accept updates without validation; async ML still runs | Temporary increase in fraudulent scores (resolved by async ML) |
| Notification service failure | WebSocket disconnect | Client reconnect | Client reconnects; missed rank change notifications are recomputed on reconnect | Notifications for changes during outage may be missed |
| Historical snapshot job failure | No snapshot at period end | Alert on snapshot absence | Job retries 3×; manual retrigger if needed | Possible gap in historical data for one period |

### Failover

- **Redis**: Cross-AZ replica promotion via Redis Cluster's CLUSTER FAILOVER mechanism. Automatic within 10 seconds.
- **Cassandra**: Automatic coordinator failover. RF=3 with LOCAL_QUORUM means any 2 of 3 nodes can serve requests.
- **Multi-Region**: Active-active deployment in 3 regions. Each region serves its own leaderboard (eventual consistency across regions). Global leaderboard synchronization via Kafka cross-region replication for score updates. A regional leaderboard during cross-region partition is slightly stale (scores from other regions not reflected) — acceptable for most use cases.

### Retries & Idempotency

- **Score submissions**: Game servers use exponential backoff retry with same `{player_id, score_delta, game_session_id, timestamp}`. The Anti-Cheat service deduplicates via `session_id + timestamp` pair — same event seen twice returns `202 Accepted` without re-processing.
- **Redis ZADD idempotency**: ZADD with `GT` (greater-than) flag: if the same score update is replayed from Kafka (at-least-once delivery), the second ZADD will only update if the new score is greater than the current. For score deltas, we write the cumulative new_score (not the delta) to Redis — computing `new_score = old_score + delta` at Kafka consumption time, then `ZADD GT new_score player_id`. If replayed, `ZADD GT` is a no-op (same score is not "greater than" itself). This achieves idempotent Redis writes.
- **Historical snapshot idempotency**: Snapshot job writes to Cassandra with `IF NOT EXISTS` on the snapshot_date + rank composite key. Re-running the job for the same period inserts only missing rows.

### Circuit Breaker

The score ingestion API has a circuit breaker around the anti-cheat Redis velocity check. If Redis latency exceeds 10 ms for 5 consecutive calls, the circuit opens: velocity check is bypassed, score is accepted, and a `cheat_check_bypassed: true` flag is added to the Kafka message. The async ML scoring still runs. The circuit half-opens after 30 seconds. During the open period, we accept a small increase in fraudulent scores passing the velocity check — acceptable as a trade-off for availability.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric | Type | Alert Threshold | Interpretation |
|---|---|---|---|
| `score.ingest.rate` | Counter | < 10K/s (dead man) or > 200K/s | Score submission pipeline health |
| `score.anti_cheat.rejection_rate` | Gauge | > 5% (sudden spike) | Attack in progress or anti-cheat misconfiguration |
| `redis.zadd.p99_latency_ms` | Histogram | > 5 ms | Redis under pressure; rank updates slowing |
| `kafka.consumer_lag.redis_updater` | Gauge | > 100K messages | Redis updater falling behind; stale leaderboard |
| `leaderboard.rank_query.p99_ms` | Histogram | > 50 ms | Redis query latency degraded |
| `friend_lb.query.p95_ms` | Histogram | > 200 ms | Friend list fetch or score pipeline slow |
| `notifications.queued` | Gauge | > 100K | Notification delivery falling behind |
| `anti_cheat.circuit_breaker.state` | Gauge | != 0 (CLOSED) | Anti-cheat Redis unavailable |
| `leaderboard.total_active_players` | Gauge | Anomalous drop (> 30% in 5 min) | Server-side issue reducing score submissions |
| `redis.memory.used_gb` | Gauge | > 80% capacity | Redis approaching memory limit |
| `cassandra.write.p99_ms` | Histogram | > 100 ms | Cassandra under pressure |
| `snapshot.completion_delay_minutes` | Gauge | > 10 min past period end | Snapshot job delayed |

### Distributed Tracing

Each score submission carries a `trace_id` through: Ingestion API → Kafka message header → Redis Updater → Cassandra Writer → Notification Engine. OpenTelemetry spans show the complete journey. A trace for a score update looks like: `[API validation: 2ms] → [Kafka publish: 5ms] → [Redis ZADD: 1ms] → [Cassandra write: 15ms] → [Notification check: 3ms]`. Total: ~26 ms from receipt to fully persistent.

### Logging

Structured JSON logs per score event: `{player_id, game_id, score_delta, new_score, event_type, fraud_score, processing_time_ms, worker_id, trace_id}`. Sampled at 1% for accepted updates (INFO), 100% for anti-cheat rejections (WARN) and errors (ERROR). Shipped to Elasticsearch for searching suspicious patterns (e.g., all rejections from a specific IP range). Score update audit trail written to Cassandra `score_events` table for billing and dispute resolution.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Choice | Alternative | Reason | Accepted Trade-off |
|---|---|---|---|---|
| Leaderboard store | Redis Sorted Set | Database with ORDER BY | Sub-ms ZADD/ZRANK; sorted set is precisely the right data structure; no query is faster | RAM cost; volatility requires AOF + Cassandra backup |
| Scaling approach | Score-range bucketed ZSETs | Redis Cluster hash sharding | Global ZSET can't be hash-sharded; bucket approach provides correct global rank via cumulative index | Complexity of cumulative_above index management |
| Score update durability | Kafka + Cassandra async | Synchronous DB write | Synchronous Cassandra write on hot path would add 15 ms to ingest latency; Kafka provides durability without blocking | Score persisted to Cassandra with ~200 ms lag vs. Redis |
| Friend leaderboard | Query-time computation | Per-player friend ZSET | Fan-out writes (O(F) ZADDs per score update) impractical at scale; query-time with Redis pipeline is 3–5 ms | Slightly higher read latency vs. maintained friend ZSET |
| Anti-cheat | Sync rules + async ML | Sync-only rules | ML catches sophisticated bot patterns; sync rules block obvious fraud without adding latency | ML scores arrive 200 ms after event (async) |
| Rank update confirmation | Eventually consistent (< 1s lag) | Synchronous (wait for Redis ZADD) | Synchronous Redis write before API 202 adds 1–5 ms; not worth it for leaderboard use case | Rank visible in leaderboard < 1s after score update |
| Historical snapshots | Cassandra point-in-time writes | Rebuild from score history | Snapshot is O(1) read vs. O(N) rebuild; 100M-player snapshot computed instantly | Requires scheduled snapshot job at period end |
| Tie-breaking | Score + (1/timestamp) float trick | Redis lexicographic tie-break | Timestamp-based tie-break is semantically correct (earlier achiever gets precedence) | Minor float precision loss at extreme scores |
| Multi-region | Active-active with eventual consistency | Active-passive | Active-active serves queries with < 10 ms latency in any region; passive would add cross-region round-trip | Cross-region leaderboard may lag by up to 500 ms |

---

## 11. Follow-up Interview Questions

Q1: How would you design a "season reset" — resetting all scores to 0 at the start of a new season?
A: Season reset is a high-traffic operation that must be atomic and fast. Strategy: (1) Do NOT delete and recreate the Redis ZSETs (FLUSHDB would block Redis for seconds on 100M-entry ZSET). Instead, use key rotation: rename `global_leaderboard:game:g1:season_current` → `global_leaderboard:game:g1:season_S11` (atomic Redis RENAME, O(1)). Create a new empty ZSET for `season_current`. (2) The old season ZSET becomes the final season snapshot and is eventually expired. (3) In Cassandra: the new season is a new partition `(game_id, season="S12")` — old season data is untouched. (4) Notify players: "Season 11 has ended. Season 12 begins now!" via the notification system. (5) Cache invalidation: all leaderboard query caches are invalidated immediately after the RENAME. (6) The entire season reset operation takes < 100 ms and serves thousands of queries during the transition without downtime.

Q2: How do you design the leaderboard for a game with multiple score dimensions (e.g., rank by kills, rank by wins, rank by playtime)?
A: Each score dimension is a separate Redis ZSET: `lb:kills:game:g1`, `lb:wins:game:g1`, `lb:playtime:game:g1`. Each gets its own bucket structure. Score updates are fan-out: when a game event increments kills, the Score Ingestion API receives `{score_dimensions: {kills: +1, playtime: +30}}`. The Redis Updater writes to all affected dimension ZSETs in a single Redis pipeline. Multi-dimension leaderboards are independent — a player can be rank 500 in kills and rank 5,000 in wins. The API supports `dimension` query parameter: `GET /v1/games/{game_id}/leaderboard/rank?player_id=X&dimension=kills`. Composite ranking (weighted sum of dimensions) is supported via a computed dimension updated by a periodic job: `lb:composite:game:g1` = `ZADD composite_score = 0.5*kills_rank + 0.3*wins_rank + 0.2*playtime_rank`.

Q3: During a tournament broadcast, millions of viewers watch the top-10 leaderboard in real time. How do you serve this at scale?
A: Tournament broadcast has a fundamentally different access pattern: millions of readers, very high cache hit potential. Strategy: (1) Pre-push the top-10 leaderboard to CDN every 1 second (server-push pattern). A dedicated "tournament snapshot" service reads the live top-10 from Redis every second and pushes it to CloudFront using CloudFront's origin shield + edge cache invalidation API. (2) All viewer clients fetch the cached CDN response (< 10 ms, globally). Zero load on origin Redis. (3) WebSocket for "live feed": instead of polling, viewers subscribe to a WebSocket server that pushes the top-10 update whenever a change occurs (at most 1 update/second). This reduces polling load by 100×. (4) Server-Sent Events (SSE) as a simpler alternative to WebSocket for read-only streams. (5) Scale WebSocket servers to 100 pods, each handling 50K concurrent connections (5M total capacity for tournament broadcast).

Q4: How do you handle the "cold start" problem when a new player's first game results in a very high score?
A: A new player with score 0 in the ZSET has rank = total_players (last place). After their first game, they jump to their actual rank — this looks like a huge rank jump to the notification system (from 100M to potentially rank 50K). The notification system must handle this: first-game score updates are flagged with `is_first_game: true` and suppress rank-change notifications (the player hasn't "lost rank" anywhere — they just appear on the leaderboard for the first time). The notification sent is instead: "Welcome to the leaderboard! Your first rank is #50,234." The anti-cheat system also has a "new player" grace period: rule thresholds are relaxed for the first 3 game sessions (legitimate new players may score unusually high if they're skilled players from other games).

Q5: How would you support "clan/team leaderboards" where clans compete based on the sum of their members' scores?
A: Clan leaderboards aggregate individual scores by clan. Two approaches: (1) **Real-time aggregation**: Maintain a separate `clan_total_score` ZSET. When a player scores, also update their clan's total. This requires: knowing each player's clan membership (stored in Redis HASH `player:clan:{player_id} = clan_id`), then `ZINCRBY clan_leaderboard score clan_id`. This adds 1 more ZADD equivalent per score update — acceptable. (2) **Pre-computed clan scores (batch)**: A job runs every 5 minutes that sums all members' scores per clan and updates the clan ZSET. This is simpler but introduces 5-minute staleness. For competitive tournaments, real-time is required. Clan membership changes (player joins/leaves clan) require retroactive score transfer: add or subtract the joining player's score from the new/old clan's total. This is handled by the clan management service sending an event to a `clan_membership_change` Kafka topic, consumed by a worker that updates clan scores.

Q6: How does the leaderboard change when the game developer wants to remove all scores from a specific game session (e.g., a server bug gave everyone extra points)?
A: Bug-related score invalidation requires: (1) Identify all score events from the affected `game_session_id` in Cassandra: `SELECT player_id, score_delta FROM score_events WHERE game_id = 'g1' AND game_session_id = 'bug-session-xyz'`. (2) For each affected player, compute the corrected score: `corrected = current_score - sum(score_deltas from bug session)`. (3) Batch update Redis: for each affected player, `ZADD global_leaderboard corrected_score player_id`. (4) Update Cassandra: mark the bug session events as `is_void=true`. (5) The correction is atomic per player (single ZADD). For 10,000 affected players, this takes ~10 ms in Redis pipeline. (6) Notify affected players: "Scores from session X have been adjusted due to a technical issue." (7) Update leaderboard snapshots if the bug period crossed a snapshot boundary.

Q7: How do you protect against "rank sniping" in the last 10 seconds of a tournament — players timing their score submissions to jump just above others?
A: Last-second rank changes are a feature for competitive players, but the system must handle the rush correctly. Mitigation for fairness: (1) Tournament "closing freeze" period: in the final 30 seconds, scores are accepted but not applied to the competitive leaderboard until the tournament officially ends. Scores are buffered in Kafka and applied in batch after the deadline, ensuring no timing advantage from network latency. (2) Timestamp precision: use the game server's `timestamp_ms` (not the received time) as the official score time. Score events with timestamp_ms > tournament_end_ms are rejected regardless of when received. (3) For the "rank display in final seconds" — we show the leaderboard as of T-60 seconds during the final minute to avoid visual chaos, with a "Final Ranks: TBD in 60 seconds" banner. The true final ranks are computed from the batch flush at tournament end.

Q8: How would you implement "soft delete" — hiding a player from the leaderboard without removing their score (e.g., account banned, awaiting appeal)?
A: Soft delete without ZREM: (1) Maintain a Redis SET `banned_players:game:g1` of banned player IDs. (2) Leaderboard Query API, after fetching top-N from ZSET, filters out any player_id that's in the banned set. (3) For rank queries of specific players, if the player is in the banned set, return `{"rank": null, "status": "suspended"}`. (4) The score remains in the ZSET — if the ban is reversed, the player immediately reappears at their correct rank (no recompute needed). (5) For pagination of the top-N leaderboard, banned players count toward rank but are hidden: a viewer scrolling through ranks 1–100 may see "rank 47" jump to "rank 49" if rank 48 is banned. For a cleaner display, we can use a "display rank" that counts only non-banned players — this requires a counter of active players above each banned player, which is expensive to maintain. Simpler: label hidden entries with a generic "Hidden Player" display name to maintain contiguous ranks.

Q9: How do you implement leaderboard privacy — a player opts out of appearing on public leaderboards but still wants to see their own rank?
A: Privacy-aware leaderboard: (1) Player's score remains in the ZSET (needed to compute their rank correctly). (2) A `private_players:game:g1` Redis SET contains opted-out player IDs. (3) Public leaderboard API (top-N, neighbor view) filters out private players from results (same mechanism as soft delete). (4) The player can query their own rank via the authenticated rank API: `GET /v1/games/{game_id}/leaderboard/rank?player_id=my_own_id` — this returns their rank even if they're private. (5) Friend leaderboard: private players are shown to their friends (they've implicitly consented to friend visibility by having the friendship). (6) GDPR consideration: a player who requests data deletion must be removed from the ZSET entirely (not just the private set), as their presence in the leaderboard (even unlabeled) constitutes processing of personal data.

Q10: How would you add a "daily challenge" leaderboard that resets at midnight UTC each day but keeps history?
A: Daily challenge leaderboard is a time-scoped leaderboard: (1) Use dated ZSET keys: `lb:daily:{game_id}:2026-04-09` for today's leaderboard. Score updates to the daily leaderboard use `ZADD lb:daily:{game_id}:{today}` in parallel with the global ZSET update. (2) At midnight UTC, a scheduled job: `RENAME lb:daily:{game_id}:{yesterday}` to `lb:history:daily:{game_id}:{yesterday}` (make it immutable). Then the new `lb:daily:{game_id}:{today}` starts empty. (3) Redis TTL on old daily ZSETs: `EXPIRE lb:history:daily:{game_id}:{yesterday} 604800` (7 days in Redis for fast queries). Older historical data is backed up to Cassandra leaderboard_snapshots table. (4) Daily challenge scores are separate from career scores — a player's daily challenge ZADD only goes to the daily key, not the global key (unless the game design conflates them). (5) Players get `daily_challenge_rank` in their profile, updated from the daily ZSET.

Q11: How do you handle the leaderboard during a Redis upgrade (e.g., upgrading Redis from 6.x to 7.x with a restart)?
A: Redis cluster rolling upgrades: (1) Upgrade one replica node at a time (no downtime — Redis Cluster continues serving with the other nodes). (2) Promote each upgraded replica to primary using CLUSTER FAILOVER on the original primary. (3) The original primary (now a replica) is then upgraded. (4) Total downtime: zero for the cluster; individual shard failovers take < 10 seconds each. During the 10-second failover per shard: ZADD writes to the failing shard are buffered in Kafka (the Redis Updater consumer gets a Redis error, pauses, and retries after the failover completes). (5) For major version upgrades that require AOF/RDB format changes: use a side-by-side migration. Stand up a new Redis 7 cluster, replicate all data from Redis 6 (rdbfile import + live replication sync), then switch traffic. Blue-green style.

Q12: How would you design the notification system to handle 1 billion push notifications per day (for 100M players receiving notifications)?
A: 1B notifications/day = 11,574/second. The bottleneck is push notification delivery infrastructure (APNs for iOS, FCM for Android). APNs and FCM support batch sending. Strategy: (1) Rank change events are published to a `rank_notifications` Kafka topic. (2) Notification workers batch notifications by device type (APNs vs. FCM) and game. (3) Send bulk to APNs HTTP/2 (Apple supports concurrent HTTP/2 connections, each supporting 500 concurrent requests). At 11,574/s: need 23 APNs connections × 500 req each = 11,500 concurrent requests. (4) Notification workers are a 20-pod deployment, each maintaining 5 APNs connections: 100 total connections × 500 req = 50,000 concurrent deliveries. (5) Delivery deduplication: if a player gained 100 ranks in the last 5 minutes, only 1 notification is sent (the cumulative change). (6) Notification priority: only rank changes crossing milestones (top 10K, top 1K, top 100, top 10) trigger notifications for most players. High-frequency minor rank changes (rank 50,000 → 49,999) are silently skipped for the 90% of players with default settings.

Q13: How do you implement a "leaderboard replay" feature — showing how the leaderboard looked at any point in the past 30 days?
A: Real-time replay requires storing the complete leaderboard state at every point in time — impractical (100M entries × 1 day × 86,400 snapshots = petabytes). Instead, we store delta events and reconstruct: (1) The Cassandra `score_events` table is a time-ordered ledger of all score changes for 30 days. (2) For any target timestamp T: start from the daily snapshot at the beginning of T's day, then apply all score events from midnight to T in order. (3) Reconstruction is done by a Spark job on demand (not in real-time). For the top-1,000 leaderboard at time T: a query of `score_events` filtered to events before T, grouped by player, max aggregate = each player's score at time T. Sort → return top 1,000. (4) Pre-compute hourly snapshots for popular time points (e.g., top of each hour) stored in Cassandra leaderboard_snapshots. Queries within a known snapshot time return instantly.

Q14: How would you extend this system to support a game that has both a competitive season leaderboard AND lifetime achievement rankings?
A: Two parallel leaderboard instances: (1) Season leaderboard: `lb:{game_id}:season:S12:{dimension}` ZSETs. Reset between seasons. Score = current season score. Season-specific historical snapshots. (2) Lifetime leaderboard: `lb:{game_id}:lifetime:{dimension}` ZSETs. Never reset. Score = sum of all season scores. Score updates to the season leaderboard ALSO update the lifetime leaderboard. A single Redis Updater pipeline: `ZADD lb:g1:season:S12 season_score player_id` + `ZINCRBY lb:g1:lifetime delta player_id`. The `ZINCRBY` atomically adds the delta to the lifetime score. Note: `ZINCRBY` instead of `ZADD` is critical here — it adds to the existing score rather than replacing it. Season boundaries: at season end, the season ZSET is snapshotted and renamed; the lifetime ZSET is untouched. Lifetime ranks are displayed in the player's career profile, separate from competitive season rankings.

Q15: How do you handle cheater detection for a player who slowly accumulates an impossible score over 30 days (1M points per day above their peer group's average)?
A: Long-term velocity cheating that stays within hourly limits requires historical baseline comparison. Our async ML model uses features including: (1) Rolling 7-day score accumulation vs. peer percentile (players with similar playtime and session count). A player consistently at 99.9th percentile of their cohort triggers investigation. (2) Score-to-playtime ratio: if a player accumulates 10× the score per hour vs. their cohort, it's suspicious even if each hourly bucket is within limits. (3) Progression curve: legitimate players show a learning curve (improving over days/weeks). Cheaters often show a flat, consistently above-average progression from day 1. (4) The ML model is retrained weekly on labeled fraud data (confirmed cheaters from manual review + confirmed legitimate players). Features are computed over 30-day windows, so slow cheaters are caught during the weekly retraining cycle. (5) Score anomaly score is stored with each player: `player:fraud_risk:{player_id}` in Redis. Players above 0.7 risk score are flagged for human review. Players above 0.95 are automatically quarantined.

---

## 12. References & Further Reading

1. **Redis Sorted Set Documentation** — Redis Ltd. https://redis.io/docs/data-types/sorted-sets/ — ZADD, ZRANK, ZRANGE command reference with complexity analysis. The foundational reference for the core data structure.

2. **"Redis in Action"** — Josiah Carlson, Manning Publications, 2013. Chapter 6 describes sorted set-based leaderboard implementations. Available at: https://redis.com/ebook/redis-in-action/

3. **"Scaling Real-Time Leaderboards at King"** — Mattias Forssel, *InfoQ*, 2019. How King (Candy Crush developer) scales leaderboards for hundreds of millions of players. https://www.infoq.com/presentations/leaderboard-redis/

4. **"Building a Real-Time Leaderboard with Redis"** — AWS Architecture Blog. https://aws.amazon.com/blogs/database/building-a-real-time-gaming-leaderboard-with-amazon-elasticache-for-redis/ — Practical implementation guide for Redis-based leaderboards on AWS.

5. **"Skip Lists: A Probabilistic Alternative to Balanced Trees"** — William Pugh, *Communications of the ACM*, 1990. The data structure underlying Redis Sorted Sets.

6. **"Anti-Cheat in Online Games"** — VAC (Valve Anti-Cheat) Technical Overview. Valve Corporation. https://www.valvesoftware.com/en/publications — Overview of behavioral anti-cheat detection methods used in production gaming systems.

7. **"HyperLogLog in Practice: Algorithmic Engineering of a State of the Art Cardinality Estimation Algorithm"** — Heule et al., *ACM SIGMOD 2013*. Relevant for approximate rank estimation and unique player counting.

8. **"Designing Data-Intensive Applications"** — Martin Kleppmann, O'Reilly Media, 2017. Chapter 11 (Stream Processing) covers Kafka-based event sourcing patterns used in this design.

9. **"Probabilistic Data Structures and Algorithms for Big Data Applications"** — Andrii Gakhov, 2019. Covers Count-Min Sketch and HyperLogLog for approximate counting at scale.

10. **Cassandra Data Modeling Guide** — Apache Software Foundation. https://cassandra.apache.org/doc/latest/cassandra/data_modeling/intro.html — Official guide on modeling time-series data, particularly relevant for the score_events and leaderboard_snapshots table designs.
