# System Design: Multiplayer Game Backend (Authoritative Server Architecture)

---

## 1. Requirement Clarifications

### Functional Requirements
1. Support real-time competitive multiplayer games with 2–100 players per session (primary design target: 16-player team shooter).
2. Authoritative game server: the server computes and validates all game state; clients send inputs, server applies logic.
3. Game state synchronization: all players in a session see a consistent (within lag compensation tolerance) view of the world.
4. Matchmaking: group players into sessions by skill level (MMR-based), game mode, region, and latency.
5. Session lifecycle: lobby creation, player join/ready, game start, mid-game, game end, results recording.
6. Leaderboards: real-time ranked leaderboard per game mode; all-time and seasonal rankings.
7. Player statistics: win/loss, K/D ratio, rank, match history.
8. Anti-cheat: server-side validation of all movement and action inputs; detection of speed hacks, aimbots, teleportation.
9. Friends system: see which friends are online/in-game; invite to session.
10. Chat: global lobby chat and in-game voice/text chat per session.

### Non-Functional Requirements
1. Latency: authoritative game state updates must reach clients within 50 ms P99 within a region (intra-region RTT target: < 30 ms).
2. Tick rate: 64 Hz server tick rate (15.6 ms per tick) for competitive play; configurable to 128 Hz for premium servers.
3. Scalability: support 10 M concurrent players; 625 k simultaneous sessions (at 16 players/session).
4. Reliability: a game session that starts must complete; no mid-game server crashes that lose session data.
5. Availability: matchmaking and social systems: 99.99%; game servers: 99.95% (brief disruptions during active sessions are catastrophic to players).
6. Fairness: no player can gain competitive advantage through packet manipulation or speed hacks.
7. Regional deployment: game servers in NA, EU, Asia-Pacific, South America with players always matched to their closest region (RTT < 50 ms to server).
8. Persistence: match results recorded durably within 5 s of match end.

### Out of Scope
- Game engine client implementation (Unity / Unreal)
- Monetization / item shop / loot boxes
- Voice chat infrastructure (use third-party like Vivox/Agora)
- Anti-cheat kernel driver (use third-party like BattlEye/EAC)
- Game content updates / patch delivery / CDN for assets
- Replay system (mentioned as follow-up)

---

## 2. Users & Scale

### User Types
| Role | Description |
|---|---|
| Player | Active game participant; sends inputs, receives state updates |
| Spectator | Observes a match; receives state updates with delay (anti-cheat) |
| Coach / Analyst | Reviews past match data; no real-time connection to game server |
| Admin / GM | Game master; can observe any session; kick/ban players |

### Traffic Estimates

**Assumptions:**
- 10 M concurrent players at peak
- Average session: 16 players, one 30-minute match
- Sessions = 10 M / 16 = 625 k simultaneous sessions
- Tick rate: 64 Hz = 1 tick every 15.6 ms
- Per-player input packet: 60 bytes (movement delta, aim angles, actions bitmask, client tick number)
- Per-player state update from server: 200 bytes per player visible in state (16 players × 50 bytes/entity + 100 bytes overhead)
- Sessions run on dedicated game server pods

| Metric | Calculation | Result |
|---|---|---|
| Simultaneous sessions | 10 M players / 16 players/session | 625 k sessions |
| Server tick events/s | 625 k sessions × 64 ticks/s | 40 M ticks/s |
| Client → server packets/s | 10 M players × 64 Hz | 640 M packets/s |
| Client input bandwidth | 640 M × 60 bytes × 8 bits | ~307 Gbps inbound |
| Server → client packets/s | 10 M players × 64 Hz | 640 M packets/s |
| State update bandwidth | 640 M × 200 bytes × 8 bits | ~1 Tbps outbound |
| Matchmaking requests | 10 M/16 sessions × 30 min → 20,833 new sessions/min | ~347 sessions/s |
| Leaderboard writes | 625 k sessions end/hr × 16 players | 2.78 M rank updates/hr = 778/s |
| Match result storage | 347 sessions/s × 5 KB/match | ~1.7 MB/s (trivial) |
| Player stats DB queries | 10 M players × 5 queries/session start | ~867 k queries/session start burst |

### Latency Requirements
| Operation | Target (P50) | Target (P99) | Reason |
|---|---|---|---|
| Client input → server processing | < 5 ms | < 15 ms | Intra-DC; server-authoritative state must update quickly |
| Server state → client delivery | < 15 ms | < 30 ms | Network RTT within region |
| Total round-trip (client fires → sees result) | < 30 ms | < 60 ms | Player perception threshold |
| Matchmaking (find match) | < 5 s | < 30 s | User tolerance for waiting |
| Session start (lobby ready → first game tick) | < 3 s | < 5 s | |
| Leaderboard query | < 100 ms | < 500 ms | Non-critical path |
| Match result recording | < 5 s | < 10 s | Durability after match end |

### Storage Estimates
| Data | Size per Record | Volume | Total |
|---|---|---|---|
| Player profile | 2 KB | 100 M players | 200 GB |
| Match history (metadata only) | 5 KB | 347/s × 86400 × 365 = 10.9 B matches/year | 54.7 TB/year |
| Match replay data | 2 MB/match (full state snapshots + input log) | 10.9 B matches/year × opt-in 10% | ~2.2 PB/year (opt-in only) |
| Leaderboard entries | 200 bytes | 100 M players × 10 game modes | 200 GB |
| Session state (in-memory only, not persisted) | 500 KB per session | 625 k sessions | ~313 GB RAM across game servers |

### Bandwidth Estimates
| Flow | Calculation | Result |
|---|---|---|
| Player → game server | 640 M packets/s × 60 bytes | ~307 Gbps |
| Game server → players | 640 M packets/s × 200 bytes | ~1 Tbps |
| Matchmaking service internal | 347 sessions/s × 32 players × 2 KB metadata | ~22 MB/s (negligible) |
| Leaderboard writes | 778 updates/s × 1 KB | < 1 MB/s |
| Player stats API (social overlay) | 10 M players × 10 req/session × 500 bytes | ~50 GB/session start burst |

---

## 3. High-Level Architecture

```
                      ┌─────────────────────────────────────────────────────┐
                      │               CLIENT (Game Binary)                  │
                      │  Input sampling 64 Hz (movement, aim, actions)      │
                      │  Client-side prediction + reconciliation             │
                      │  Entity interpolation for remote players            │
                      └────────────┬────────────────────┬────────────────────┘
                                   │ UDP (game data)     │ HTTPS (REST/WS)
                                   │                     │
            ┌──────────────────────▼─────┐  ┌───────────▼────────────────────┐
            │  GAME SERVER CLUSTER       │  │  SOCIAL / PLATFORM API         │
            │  (Regional, per-DC)        │  │                                │
            │  ┌──────────────────────┐  │  │  ┌──────────────────────────┐ │
            │  │  Game Server Manager │  │  │  │  Matchmaking Service     │ │
            │  │  - Pod lifecycle      │  │  │  │  - MMR-based grouping    │ │
            │  │  - Session assignment │  │  │  │  - Region selection      │ │
            │  │  - Health monitoring  │  │  │  │  - Lobby management      │ │
            │  └─────────┬────────────┘  │  │  └──────────────────────────┘ │
            │            │               │  │                                │
            │  ┌─────────▼────────────┐  │  │  ┌──────────────────────────┐ │
            │  │  Game Server Pods    │  │  │  │  Player Profile Service  │ │
            │  │  (one per session)   │  │  │  │  - Stats, rank, history  │ │
            │  │  ┌────────────────┐  │  │  │  └──────────────────────────┘ │
            │  │  │ Tick Loop      │  │  │  │                                │
            │  │  │ 64 Hz          │  │  │  │  ┌──────────────────────────┐ │
            │  │  │ Input Queue    │  │  │  │  │  Friends Service         │ │
            │  │  │ State Machine  │  │  │  │  │  - Presence, invites     │ │
            │  │  │ Anti-cheat     │  │  │  │  └──────────────────────────┘ │
            │  │  │ Physics Engine │  │  │  │                                │
            │  │  └────────────────┘  │  │  │  ┌──────────────────────────┐ │
            │  └──────────┬───────────┘  │  │  │  Leaderboard Service     │ │
            │             │              │  │  │  - Redis sorted sets      │ │
            │  ┌──────────▼───────────┐  │  │  └──────────────────────────┘ │
            │  │  Session State Store │  │  │                                │
            │  │  - Redis (hot state) │  │  │  ┌──────────────────────────┐ │
            │  │  - Postgres (results)│  │  │  │  Notification Service    │ │
            │  └──────────────────────┘  │  │  │  - Push to mobile/web    │ │
            └────────────────────────────┘  │  └──────────────────────────┘ │
                                            └────────────────────────────────┘

    ┌──────────────────────────────────────────────────────────────────────────┐
    │  SHARED INFRASTRUCTURE                                                   │
    │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌─────────────┐ │
    │  │  Auth Service│  │  Kafka       │  │  PostgreSQL  │  │  Redis      │ │
    │  │  (JWT / OAuth│  │  (game events│  │  Cluster     │  │  Cluster    │ │
    │  │  + session   │  │  for async   │  │  (player     │  │  (leaderbd  │ │
    │  │  tokens)     │  │  processing) │  │  stats, match│  │  presence   │ │
    │  │              │  │              │  │  history)    │  │  sessions)  │ │
    │  └──────────────┘  └──────────────┘  └──────────────┘  └─────────────┘ │
    └──────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **Game Server Pod**: The authoritative game instance for one session. Runs the tick loop at 64 Hz, processes player inputs, updates game state, applies physics/collision, and broadcasts delta updates. One pod per session, isolated by design.
- **Game Server Manager**: Allocates game server pods from a pool (using Agones on Kubernetes). Receives matchmaking requests, assigns pods, monitors health, gracefully handles match end and pod recycling.
- **Matchmaking Service**: Accepts players into a queue, groups them into balanced teams using MMR (Matchmaking Rating). Enforces latency constraints (all players in a match must have < 80 ms RTT to the selected server region).
- **Session State Store**: Redis for hot in-flight session state (player positions, scores, ephemeral). PostgreSQL for durable results written at match end.
- **Leaderboard Service**: Maintains real-time global and per-mode rankings using Redis sorted sets. Updated asynchronously after match completion.
- **Anti-cheat layer**: Runs inside the game server pod, validating each player input against physical constraints (max speed, max turn rate, reachable positions).

**Primary Data Flow (player shoots another player):**
1. Client samples input at 64 Hz: `{ tick: 15023, pos: {x,y,z}, aim: {pitch, yaw}, actions: SHOOT }`.
2. Client sends input via UDP to game server pod. Client also predicts the shot locally (client-side prediction).
3. Game server receives input, queues it for the current tick's processing.
4. Tick loop executes: applies all pending inputs (in input timestamp order), runs collision detection, validates the shot with lag compensation (rewinds world state to the shooter's perceived time), records the hit.
5. Server computes delta state (only changed entities) and sends UDP packets to all players in the session.
6. Receiving clients apply the authoritative delta, reconciling with their prediction.
7. Game server publishes `player_hit` event to Kafka for stats/achievement processing.

---

## 4. Data Model

### Entities & Schema

```sql
-- Players
CREATE TABLE players (
    player_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    username        VARCHAR(32) NOT NULL UNIQUE,
    display_name    VARCHAR(50),
    email           VARCHAR(254) UNIQUE,
    password_hash   TEXT,
    avatar_url      TEXT,
    country_code    CHAR(2),
    preferred_region VARCHAR(20),    -- 'us-east', 'eu-west', 'ap-southeast'
    is_banned       BOOLEAN DEFAULT FALSE,
    ban_reason      TEXT,
    ban_expires_at  TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login_at   TIMESTAMPTZ
);

-- Player MMR and statistics per game mode
CREATE TABLE player_stats (
    player_id       UUID NOT NULL REFERENCES players(player_id),
    game_mode       VARCHAR(32) NOT NULL,   -- 'ranked_tdm', 'casual', 'ranked_br'
    mmr             INTEGER NOT NULL DEFAULT 1500,   -- Elo/TrueSkill rating
    rank_tier       VARCHAR(20),            -- 'bronze', 'silver', 'gold', 'diamond'
    peak_mmr        INTEGER NOT NULL DEFAULT 1500,
    wins            INTEGER NOT NULL DEFAULT 0,
    losses          INTEGER NOT NULL DEFAULT 0,
    kills           BIGINT NOT NULL DEFAULT 0,
    deaths          BIGINT NOT NULL DEFAULT 0,
    assists         BIGINT NOT NULL DEFAULT 0,
    total_damage    BIGINT NOT NULL DEFAULT 0,
    matches_played  INTEGER NOT NULL DEFAULT 0,
    season_id       INTEGER NOT NULL DEFAULT 1,
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (player_id, game_mode, season_id)
);
CREATE INDEX idx_player_stats_mmr ON player_stats(game_mode, season_id, mmr DESC);

-- Sessions / Matches
CREATE TABLE matches (
    match_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    game_mode       VARCHAR(32) NOT NULL,
    region          VARCHAR(20) NOT NULL,
    server_pod_id   VARCHAR(64),
    status          VARCHAR(20) NOT NULL DEFAULT 'lobby',  -- lobby|active|completed|aborted
    started_at      TIMESTAMPTZ,
    ended_at        TIMESTAMPTZ,
    duration_s      INTEGER,
    winner_team     SMALLINT,               -- 1 or 2 for team games; NULL for BR
    map_id          VARCHAR(32),
    game_version    VARCHAR(20),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX idx_matches_status ON matches(status);
CREATE INDEX idx_matches_created ON matches(created_at DESC);

-- Match participants (one row per player per match)
CREATE TABLE match_participants (
    match_id        UUID NOT NULL REFERENCES matches(match_id),
    player_id       UUID NOT NULL REFERENCES players(player_id),
    team            SMALLINT NOT NULL,
    final_position  SMALLINT,              -- placement in BR modes
    kills           INTEGER DEFAULT 0,
    deaths          INTEGER DEFAULT 0,
    assists         INTEGER DEFAULT 0,
    damage_dealt    INTEGER DEFAULT 0,
    damage_taken    INTEGER DEFAULT 0,
    headshots       INTEGER DEFAULT 0,
    mmr_before      INTEGER NOT NULL,
    mmr_after       INTEGER,               -- NULL until match ends
    mmr_delta       INTEGER,
    performance_score DECIMAL(8,2),        -- composite performance metric
    PRIMARY KEY (match_id, player_id)
);
CREATE INDEX idx_match_participants_player ON match_participants(player_id, match_id DESC);

-- Leaderboard seasons
CREATE TABLE seasons (
    season_id       SERIAL PRIMARY KEY,
    name            VARCHAR(50) NOT NULL,
    start_date      DATE NOT NULL,
    end_date        DATE NOT NULL,
    is_active       BOOLEAN DEFAULT TRUE
);

-- Friends
CREATE TABLE friends (
    player_id       UUID NOT NULL REFERENCES players(player_id),
    friend_id       UUID NOT NULL REFERENCES players(player_id),
    status          VARCHAR(20) NOT NULL DEFAULT 'pending',  -- pending|accepted|blocked
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (player_id, friend_id)
);
CREATE INDEX idx_friends_friend ON friends(friend_id, status);

-- Bans / Reports
CREATE TABLE player_reports (
    report_id       UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    reporter_id     UUID NOT NULL REFERENCES players(player_id),
    reported_id     UUID NOT NULL REFERENCES players(player_id),
    match_id        UUID REFERENCES matches(match_id),
    reason          VARCHAR(50) NOT NULL,   -- 'cheating', 'toxicity', 'griefing'
    description     TEXT,
    status          VARCHAR(20) DEFAULT 'open',  -- open|reviewed|actioned|dismissed
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Game server pods registry (operational; lives in etcd/k8s; SQL for documentation)
-- Actual registration via Agones GameServer CRD in Kubernetes
/*
{
  "pod_id": "game-server-us-east-a-7f3bc",
  "region": "us-east",
  "session_id": "match-uuid",
  "status": "running",    // available|allocated|running|terminating
  "player_count": 16,
  "ip": "10.0.1.47",
  "port": 7777,
  "node_ip": "203.0.113.47",
  "node_port": 7777,
  "allocated_at": "2024-04-06T14:30:00Z"
}
*/
```

**In-memory game state (per session, in game server pod RAM):**
```
# Game state structure (in Go/C++ on game server)
GameState {
    tick:          uint64
    timestamp_ns:  int64
    entities: {
        player_id -> PlayerEntity {
            position:     Vec3
            velocity:     Vec3
            health:       int16
            armor:        int16
            ammo:         map[weapon] -> int16
            is_alive:     bool
            last_shot_tick: uint64
        }
        object_id -> StaticEntity { ... }   # environmental objects
        projectile_id -> Projectile { ... }  # in-flight bullets
    }
    events: []GameEvent  # events this tick (kills, damage, etc.)
    state_hash: uint64   # rolling hash for state verification
}

# Input buffer per player (for lag compensation)
InputHistory {
    player_id: uuid
    inputs: RingBuffer[64 ticks] {
        tick_number:  uint64
        position:     Vec3          # position claimed by client at this tick
        aim:          Vec2          # pitch, yaw
        actions:      uint32 bitmask
        client_ts_ms: uint64
    }
}
```

### Database Choice

| Database | Use Case | Pros | Cons | Decision |
|---|---|---|---|---|
| PostgreSQL | Player stats, match history, friends | ACID, complex queries (MMR ranking, match history), relational integrity | Horizontal scaling requires Citus; not suited for real-time session state | **Selected** for persistent game data |
| Redis | Leaderboards (sorted sets), player presence/online status, session state | ZADD/ZRANK for leaderboards O(log N), sub-millisecond, pub/sub for presence | In-memory cost; not durable for critical data | **Selected** for leaderboards and presence |
| etcd / Agones | Game server pod registry and allocation | Strong consistency for pod allocation, Kubernetes-native, watch API for reactive allocation | Not a general database; limited query capability | **Selected** for pod lifecycle |
| Kafka | Game events stream (kills, stats, anti-cheat events) | Durable, ordered, fan-out to multiple consumers (stats, anti-cheat, replay) | Not a query engine | **Selected** as event bus |
| In-process memory | Game session state during play | Zero-latency, cache-friendly, no serialization overhead | Not persistent; lost on crash | **Selected** for hot game state |
| DynamoDB | — | Serverless scaling | Higher P99 latency than Redis; no sorted sets | Not selected |
| ScyllaDB | — | Higher throughput than Cassandra | Adds operational complexity; PostgreSQL sufficient | Not selected |

---

## 5. API Design

### Game Network Protocol (UDP — in-game)

```
# Client → Server (input packet, UDP, 60-70 bytes)
struct InputPacket:
    magic:          uint16 = 0x4742  # "GB"
    version:        uint8
    message_type:   uint8 = 0x01     # INPUT
    session_id:     uint64
    player_id:      uint32           # compact session-local player index
    client_tick:    uint32           # client's local tick counter
    server_ack:     uint32           # last server tick client received
    pos_x:          int32            # fixed-point × 100, centimeters
    pos_y:          int32
    pos_z:          int32
    aim_pitch:      int16            # fixed-point × 100, degrees
    aim_yaw:        int16
    actions:        uint32 bitmask   # MOVE_FWD, MOVE_BACK, JUMP, SHOOT, RELOAD, ...
    sequence:       uint16           # for packet loss detection
    checksum:       uint32           # CRC32 of packet body

# Server → Client (state delta, UDP, variable size ~200-600 bytes for 16 players)
struct StateDeltaPacket:
    magic:          uint16 = 0x4742
    version:        uint8
    message_type:   uint8 = 0x02     # STATE_DELTA
    server_tick:    uint32
    ack_client_tick: uint32          # last client input processed
    entity_count:   uint8
    entities[]:     EntityDelta {
        entity_id:  uint16
        flags:      uint8            # bitmask: HAS_POS|HAS_VEL|HAS_HEALTH|HAS_ANIM
        pos_x:      int32            # if HAS_POS
        pos_y:      int32
        pos_z:      int32
        vel_x:      int16            # if HAS_VEL (quantized)
        vel_y:      int16
        vel_z:      int16
        health:     int16            # if HAS_HEALTH
        anim_state: uint8            # if HAS_ANIM
    }
    events_count:   uint8
    events[]:       GameEvent {
        type:       uint8            # KILL, HIT, SPAWN, PICKUP, ROUND_END
        params:     bytes[8]         # event-specific data
    }
    checksum:       uint32
```

### Platform REST API (HTTPS — WebSocket for real-time social features)

```
# Authentication
POST /api/v1/auth/login
  Body: { "username": "str", "password": "str" }
  Response: 200 { "access_token": "jwt", "refresh_token": "jwt", "player_id": "uuid" }
  Rate limit: 5 req/min per IP

POST /api/v1/auth/refresh
  Auth: Bearer <refresh_token>
  Response: 200 { "access_token": "jwt" }

# Matchmaking
POST /api/v1/matchmaking/queue
  Auth: Bearer <jwt>
  Body: { "game_mode": "ranked_tdm", "party_id": "optional_uuid" }
  Response: 202 { "queue_ticket": "uuid", "estimated_wait_s": 15 }
  Rate limit: 10 req/min per player

DELETE /api/v1/matchmaking/queue/{queue_ticket}
  Auth: Bearer <jwt>
  Response: 204  # cancel queue

# WebSocket for matchmaking events
WS: wss://api.example.com/v1/matchmaking/ws?ticket=<queue_ticket>
  Server → Client events:
  { "type": "MATCH_FOUND", "session_id": "uuid", "server_ip": "203.0.113.47",
    "server_port": 7777, "team": 1, "connect_token": "signed_token_for_UDP_auth" }
  { "type": "MATCH_CANCELLED", "reason": "not_enough_players" }
  { "type": "QUEUE_POSITION", "position": 142, "estimated_wait_s": 20 }

# Player Profile
GET /api/v1/players/{player_id}
  Auth: Bearer <jwt>
  Response: 200 { player_id, username, avatar_url, stats: { mmr, rank, wins, kd_ratio }, ... }
  Cache: 30 s (CDN, public profile)

GET /api/v1/players/{player_id}/matches?limit=20&page=1&mode=ranked_tdm
  Auth: Bearer <jwt>
  Response: 200 { matches: [...], total: 1234, page: 1 }

# Leaderboards
GET /api/v1/leaderboards/{game_mode}?season=current&limit=100&offset=0
  Auth: Bearer <jwt>
  Response: 200 {
    "entries": [{"rank": 1, "player_id": "...", "username": "...", "mmr": 3200}],
    "my_rank": { "rank": 15024, "mmr": 2100 }
  }
  Cache: 60 s (CDN)
  Rate limit: 30 req/min per player

# Friends
GET /api/v1/friends
  Auth: Bearer <jwt>
  Response: 200 { friends: [{ player_id, username, status: "online|in-game|offline",
                               current_match_id: "uuid|null" }] }
  Cache: none (real-time presence required)

POST /api/v1/friends/requests
  Auth: Bearer <jwt>
  Body: { "target_player_id": "uuid" }
  Response: 201

# Game server connection token (signed, time-limited, prevents impersonation)
POST /api/v1/sessions/{session_id}/connect_token
  Auth: Bearer <jwt>
  Response: 200 { "token": "<hmac_signed_token>", "expires_at": "ISO8601", "server_ip": "...", "port": 7777 }
  # Client presents this token on UDP connection to game server
```

---

## 6. Deep Dive: Core Components

### 6.1 Authoritative Server Tick Loop with Lag Compensation

**Problem it solves:**
Players connect over internet with varying latency (10–150 ms RTT). Without compensation, a player shooting an opponent whose position is 100 ms old would miss even if their aim was perfect at the moment they fired. Lag compensation rewinds the game world to the shooter's perceived time (when they pulled the trigger) to determine if the shot was valid, while preventing players with high latency from gaining unfair advantages.

**Approaches Comparison:**

| Approach | Fairness | Complexity | Cheat Resistance |
|---|---|---|---|
| Client-authoritative | Low (clients lie about positions) | Low | None (trivially cheated) |
| Server-authoritative, no lag comp | High | Medium | High — but feels unfair (shots miss even when aimed correctly) |
| Server-authoritative + client prediction + lag compensation | High | High | High |
| Peer-to-peer with relay server | Medium | Medium | Low (peer states unverified) |
| Lock-step (wait for all inputs each tick) | High | Low | High — but terrible latency at high player count |

**Selected: Server-authoritative with client prediction, input lag compensation (rewind), and delta compression.**

**Algorithm with pseudocode:**

```python
# Game server tick loop (runs at 64 Hz)
class GameServer:
    def __init__(self, session_id, players):
        self.tick = 0
        self.state = GameState(players)
        self.input_queues = {pid: deque() for pid in players}  # player_id -> input queue
        self.state_history = RingBuffer(size=128)  # 2 seconds of history at 64 Hz
        self.tick_interval_ms = 1000.0 / 64  # 15.625 ms

    def run(self):
        next_tick_time = monotonic_ns()
        while self.state.status == ACTIVE:
            now = monotonic_ns()
            if now >= next_tick_time:
                self.process_tick()
                next_tick_time += self.tick_interval_ms * 1_000_000  # ns
            else:
                sleep_until(next_tick_time)  # high-resolution sleep

    def process_tick(self):
        self.tick += 1
        tick_start_ns = monotonic_ns()

        # 1. Drain network receive buffers (UDP socket)
        raw_packets = self.socket.recv_all_nonblocking()
        for pkt in raw_packets:
            if verify_checksum(pkt) and verify_connect_token(pkt):
                input = parse_input(pkt)
                anti_cheat_validate(input, self.state, pkt.player_id)
                self.input_queues[pkt.player_id].append(input)

        # 2. Apply inputs for this tick
        for player_id in self.state.players:
            # Use latest input if available; repeat last input if missing (extrapolation)
            input = self.input_queues[player_id].popleft() if self.input_queues[player_id] else self.last_inputs[player_id]
            self.state.apply_movement(player_id, input)

        # 3. Process shoot actions with lag compensation
        for player_id, input in self.current_inputs.items():
            if input.actions & ACTION_SHOOT:
                self.process_shot(player_id, input)

        # 4. Physics update (projectile movement, collision)
        self.state.physics_tick(dt=self.tick_interval_ms / 1000.0)

        # 5. Collect game events (kills, flag captures, round events)
        events = self.state.collect_events()

        # 6. Save state snapshot for future lag compensation
        self.state_history.append((self.tick, deepcopy(self.state)))

        # 7. Compute delta state and send to each player
        for player_id in self.state.players:
            delta = self.compute_delta(player_id, self.state, self.last_sent[player_id])
            pkt = build_state_packet(self.tick, input_ack=self.last_inputs[player_id].tick, delta, events)
            self.send_udp(player_id, pkt)  # unreliable, unordered

        # 8. Update last sent state and tick timing
        self.last_sent = self.state.snapshot()
        tick_duration_µs = (monotonic_ns() - tick_start_ns) / 1000
        metrics.histogram("tick_duration_µs", tick_duration_µs)

    def process_shot(self, shooter_id, input):
        # Lag compensation: rewind state to the time the shooter's client fired
        shooter_latency_ms = self.estimate_latency(shooter_id)  # based on recent acks
        rewind_ticks = min(int(shooter_latency_ms / self.tick_interval_ms), 64)  # max 2s rewind
        rewind_tick = self.tick - rewind_ticks

        # Get historical state at rewind_tick
        historical_state = self.state_history.get(rewind_tick)
        if historical_state is None:
            return  # can't verify; drop the shot

        # In the rewound world, check if shooter's aim intersects any target
        hit = raycast(
            origin=historical_state.entities[shooter_id].position,
            direction=input.aim_direction,
            entities=historical_state.entities,
            exclude={shooter_id},
            max_distance=WEAPON_RANGE
        )

        # Additional validation: is the rewound target position plausible?
        # (Prevent shooting through walls using current-state geometry)
        if hit and is_valid_shot(hit, historical_state, self.state):
            damage = calculate_damage(input.weapon, hit.hit_box)
            self.state.apply_damage(hit.entity_id, damage, shooter_id)

def anti_cheat_validate(input, state, player_id):
    player = state.entities[player_id]

    # Speed check: position delta cannot exceed max_speed × elapsed_time × tolerance
    claimed_pos = input.position
    max_allowed_move = MAX_SPEED * TICK_INTERVAL_S * 1.2  # 20% tolerance for packet timing jitter
    actual_move = distance(player.position, claimed_pos)
    if actual_move > max_allowed_move:
        flag_suspicious(player_id, "SPEED_HACK", actual_move, max_allowed_move)
        # Use server's extrapolated position instead of client's claimed position
        input.position = extrapolate(player.position, player.velocity, TICK_INTERVAL_S)

    # Aim validation: turn rate cannot exceed max_turn_rate_per_tick
    turn_delta = angle_difference(player.last_aim, input.aim)
    if turn_delta > MAX_TURN_RATE_PER_TICK * 1.5:
        flag_suspicious(player_id, "AIM_HACK", turn_delta)
        input.aim = clamp_aim(player.last_aim, MAX_TURN_RATE_PER_TICK)
```

**Interviewer Q&A:**

Q1: How do you handle a player with 200 ms latency in a session where others have 10 ms?
A1: The lag compensation window is capped at 128 ticks (2 seconds at 64 Hz). A 200 ms player is rewound ~13 ticks. This means when the 200 ms player shoots, the server rewinds the other players' positions by 200 ms — giving them an advantage where enemies appear to be behind cover from the server's current perspective. This is the classic "peeker's advantage." Industry solutions: (1) Cap the rewind window to 100 ms maximum (accept that 200 ms+ players experience reduced hit registration); (2) Regional matchmaking enforces all players in a session have < 80 ms RTT to the server, limiting the rewind gap to < 6 ticks; (3) Session-level latency score: if a player's latency jumps to 300 ms mid-match, they're flagged and the session may substitute a bot or warn other players.

Q2: What happens when a client stops sending inputs (connection drop)?
A2: The server detects missing inputs (no INPUT packet received within 3 ticks = ~47 ms). The server: (1) Extrapolates the player's position using their last known velocity (linear extrapolation); (2) After 5 ticks (78 ms), marks the player as "disconnected" (health stops regenerating, flag is not advanced); (3) After 30 ticks (0.47 s), freezes the player entity in place; (4) After 300 ticks (4.7 s), removes the player entity and notifies teammates; (5) If the player reconnects within 60 seconds (same session_id + connect_token), they rejoin with their state preserved (respawn at a safe spawn point). This prevents griefing via intentional disconnect.

Q3: How does the client know how much to predict ahead?
A3: The client measures its round-trip time (RTT) by comparing its sent tick number with the `ack_client_tick` in server responses: RTT ≈ (current_client_tick - ack_client_tick) × tick_interval_ms. Client-side prediction runs exactly RTT/2 / tick_interval_ms ticks ahead of the last confirmed server state. If RTT = 60 ms and tick_interval = 15.6 ms, the client predicts ~4 ticks ahead. The client also adds a configurable interpolation buffer (1–3 ticks = 15–47 ms) to smooth out jitter, so the client's rendered state is actually RTT/2 + interpolation_buffer behind the server's state.

Q4: How does delta compression work and why is it important?
A4: A full game state snapshot for 16 players at 64 bytes/player = 1,024 bytes. Sending this 64 times/second to 16 players = 1,024 × 64 × 16 = 1,048 KB/s/session. At 625 k sessions: 625 k × 1,048 KB/s = 655 GB/s. Delta compression only sends changed fields: in a typical tick, a player moves (3 floats = 12 bytes), so the delta for 16 players averages ~200 bytes, not 1,024 bytes — a 5× reduction. Implementation: each field has a "dirty bit"; the server tracks the last state sent to each client and only includes fields that changed since then. The EntityDelta struct uses a `flags` bitmask to indicate which optional fields are present.

Q5: What is "tick rate" and why does 128 Hz matter for competitive play?
A5: Tick rate is how many times per second the server processes inputs and sends state updates. At 64 Hz, one tick = 15.6 ms. An enemy player's position is sampled 64 times per second. If a player moves between two tick samples, their position is interpolated. At 128 Hz, one tick = 7.8 ms, halving positional error. The practical benefit: at 64 Hz, a player moving at 300 units/s moves 4.7 units between ticks — close-range gunfights can feel "snappy" or "laggy" at this resolution. At 128 Hz, movement is 2.3 units/tick — sub-pixel for most monitor resolutions. CS:GO competitive servers and Valorant use 128 Hz for this reason. The compute cost is 2× at 128 Hz but the servers are CPU-bound, so a 128 Hz pod handles fewer concurrent sessions (or requires a faster CPU).

---

### 6.2 Matchmaking System

**Problem it solves:**
Group players into balanced, fair matches quickly. Balanced means teams have similar total MMR. Quick means wait time < 30 s for most players. The constraints are conflicting: strict MMR matching = long waits; loose MMR matching = unfair games. Additionally, all players in a match must have acceptable latency to the game server.

**Approaches Comparison:**

| Approach | Fairness | Wait Time | Complexity |
|---|---|---|---|
| FIFO queue (first come, first served) | Poor | Low | Very low |
| Strict MMR bracket (± 50 points) | High | High (for rare MMR values) | Low |
| Expanding search window (widen MMR range over time) | Adaptive | Good | Medium |
| Elo/TrueSkill with priority queue | High | Good | Medium |
| Machine learning (predicted outcome balance) | Highest | Depends on model latency | High |

**Selected: TrueSkill2-inspired skill rating with expanding search window and regional pre-grouping.**

**Pseudocode:**

```python
# Matchmaking queue entry
class QueueEntry:
    player_id: UUID
    mmr: int
    mmr_uncertainty: float      # TrueSkill sigma: higher = less confident in rating
    game_mode: str
    preferred_region: str
    latency_ms: dict            # { region: measured_latency_ms }
    queued_at: float            # timestamp
    party: Optional[List[UUID]] # party members queued together

# Matchmaking worker (runs per game mode per region)
class Matchmaker:
    def __init__(self, game_mode, region, players_per_match=16, teams=2):
        self.queue = SortedList(key=lambda e: e.mmr)  # sorted by MMR for efficient range queries
        self.game_mode = game_mode
        self.region = region
        self.players_per_match = players_per_match
        self.teams = teams

    def add_to_queue(self, entry: QueueEntry):
        # Validate latency to this region
        if entry.latency_ms.get(self.region, 999) > MAX_ACCEPTABLE_LATENCY_MS:
            return REGION_REJECTED
        self.queue.add(entry)

    def run_matching_cycle(self):
        # Run every 1 second
        for anchor in self.queue:
            wait_s = time.now() - anchor.queued_at
            # Expanding MMR window: ±50 at t=0, expanding by +25/s up to ±200
            mmr_window = min(50 + int(wait_s * 25), 200)
            # Collect candidates within MMR window
            candidates = self.queue.irange(
                minimum=anchor.mmr - mmr_window,
                maximum=anchor.mmr + mmr_window,
                inclusive=(True, True)
            )
            if len(candidates) >= self.players_per_match:
                # Try to form a balanced match
                match = self.form_balanced_match(candidates, self.players_per_match)
                if match:
                    for entry in match:
                        self.queue.remove(entry)
                    self.create_session(match)

    def form_balanced_match(self, candidates, count):
        # Goal: split into 2 teams where |avg_mmr(team1) - avg_mmr(team2)| < BALANCE_THRESHOLD
        best_split = None
        best_imbalance = float('inf')

        # Sort candidates by MMR; try snake-draft team assignment
        sorted_candidates = sorted(candidates[:count*2], key=lambda e: e.mmr, reverse=True)
        # Snake draft: alternately assign to team 1 and team 2
        team1, team2 = [], []
        for i, player in enumerate(sorted_candidates[:count]):
            if i % 2 == 0:
                team1.append(player)
            else:
                team2.append(player)

        imbalance = abs(mean_mmr(team1) - mean_mmr(team2))
        if imbalance < BALANCE_THRESHOLD:  # e.g., 100 MMR points
            return team1 + team2
        return None

    def create_session(self, players):
        # Request game server allocation from Agones
        session = agones.allocate_game_server(region=self.region, game_mode=self.game_mode)
        match_id = db.create_match(players, session.pod_id, self.game_mode, self.region)
        # Notify players via WebSocket
        for player in players:
            team = 1 if player in team1 else 2
            notify_player(player.player_id, {
                type: "MATCH_FOUND",
                session_id: match_id,
                server_ip: session.node_ip,
                server_port: session.node_port,
                team: team,
                connect_token: generate_connect_token(player.player_id, match_id)
            })
```

**Interviewer Q&A:**

Q1: How do you prevent players from exploiting the expanding MMR window to smurfs in low-rank games?
A1: MMR uncertainty (TrueSkill sigma) is tracked separately from MMR. A smurf account (new account, high skill) has high uncertainty. High-uncertainty players are matched first within their observed performance bracket, not their stated MMR. Additionally: (1) New accounts have accelerated MMR adjustment (higher K-factor); (2) Placement matches prevent players from choosing their starting rank; (3) Behavioral analysis flags accounts that consistently perform well above their stated MMR; (4) The expanding window has a hard cap at ±200 MMR — we'd rather have a player wait 4 minutes for a fair match than put a Diamond-skill player in a Bronze match.

Q2: How do you handle parties of mixed MMR? (e.g., two friends, one Diamond, one Bronze)
A2: The party's "effective MMR" is computed as `max(individual_MMRs) + party_bonus` where party_bonus = 0 for < 50 MMR gap, scaling to 200 bonus MMR for a 500+ MMR gap. This inflates the effective MMR to match the party against higher-skilled players (fair for the opposing team). The individual MMR adjustments after the match are scaled: the Bronze player's MMR changes less than the Diamond player's (since being carried is factored out using a performance score derived from actual stats vs. expected stats).

Q3: What happens to matchmaking if one region has very few players online (e.g., 4 AM)?
A3: Regional fallback: if a player's preferred region queue does not form a match within 60 seconds, the matchmaker tries adjacent regions (e.g., from Asia-Pacific, try US-West). The player is only moved to a farther region if their measured latency to that region is still below 120 ms. If no region forms a match within 120 seconds: (1) Offer the player a practice match vs. AI bots; (2) Put them in "cross-region" matching where SBMM is relaxed (±500 MMR) to maximize fill rate; (3) For ranked modes, cross-region matching is disabled with a "no matches available" message (ranked MMR integrity > wait time).

Q4: How do you maintain a fair leaderboard when MMR adjustments happen in batch at match end?
A4: Leaderboards are updated atomically after match result processing. The sequence: (1) Match ends → game server publishes `match_end` event to Kafka; (2) Stats service consumes the event, computes MMR delta for each player using the TrueSkill update formula, and writes to `player_stats` table in PostgreSQL with a single transaction per match; (3) A Kafka consumer updates Redis sorted set leaderboard: `ZADD leaderboard:{game_mode}:{season_id} {new_mmr} {player_id}`; (4) Redis sorted sets are atomically consistent — the leaderboard update is atomic per player, and all 16 players are updated within a few hundred milliseconds of each other. Mid-match "live" leaderboard updates are not supported (only final results count).

Q5: How do you scale matchmaking to 10 M concurrent players and 347 new sessions/second?
A5: The matchmaking workers are stateless (queue state is in Redis) and horizontally scalable. Architecture: (1) Redis Sorted Set per `(game_mode, region)` stores the active queue, keyed by player_id, scored by mmr; (2) Matchmaking worker pods consume the queue using Redis transactions (WATCH/MULTI/EXEC to prevent two workers from picking the same candidate); (3) At 347 sessions/s and one matching cycle per second, each worker can process its entire regional queue in < 1 s (a Redis ZRANGEBYSCORE scan of 10 k entries takes < 10 ms); (4) With 5 game modes × 5 regions = 25 matchmaking worker pods, each handling ~14 sessions/s — trivially scalable.

---

### 6.3 Leaderboard Design

**Problem it solves:**
Support a global leaderboard with 100 M players, real-time rank queries for any player ("what is my rank out of 100 M?"), and top-N queries for display. This must work at 778 rank updates/s and serve thousands of rank queries/s with < 100 ms latency.

**Approaches Comparison:**

| Approach | Write Cost | Rank Query | Top-N | Memory |
|---|---|---|---|---|
| Sort entire table on query | O(1) write | O(N log N) — terrible | O(N log N) — terrible | Low |
| Periodic batch rank precomputation | O(1) write | O(1) (precomputed) | O(1) | Medium — stale |
| Redis Sorted Set | O(log N) write | O(log N) | O(log N + K) | O(N) in RAM |
| B-tree DB index (PostgreSQL ORDER BY + RANK()) | O(log N) write | O(log N) | O(log N + K) | Disk |
| Skip list with rank counting | O(log N) write | O(log N) | O(K) | O(N) |

**Selected: Redis Sorted Set as primary leaderboard with PostgreSQL as durable backup.**

Redis `ZADD leaderboard:{mode}:{season} {score} {player_id}` is O(log N). `ZREVRANK` gives a player's rank in O(log N). `ZREVRANGE start stop WITHSCORES` gives top-N in O(log N + K). A Redis Cluster with 16 shards: each shard holds `100 M / 16 = 6.25 M` entries. `ZREVRANK` on 6.25 M entries: log2(6.25 M) ≈ 23 operations in Redis's skip list. At sub-microsecond per operation: < 100 µs for a rank query.

**Pseudocode:**

```python
# Leaderboard service

LEADERBOARD_KEY = "leaderboard:{game_mode}:{season_id}"  # Redis sorted set
LEADERBOARD_SHARD_COUNT = 16  # Redis cluster shards by key hash

# Update player rank after match
def update_player_rank(player_id, game_mode, new_mmr, season_id):
    key = LEADERBOARD_KEY.format(game_mode=game_mode, season_id=season_id)
    redis.zadd(key, {player_id: new_mmr}, ch=True)  # ch=True: return count of changed elements
    # Also persist to PostgreSQL asynchronously
    kafka.produce("leaderboard-updates", key=player_id, value={
        "player_id": player_id, "game_mode": game_mode, "season_id": season_id,
        "new_mmr": new_mmr, "ts": now()
    })

# Get player's rank
def get_my_rank(player_id, game_mode, season_id):
    key = LEADERBOARD_KEY.format(game_mode=game_mode, season_id=season_id)
    rank = redis.zrevrank(key, player_id)  # 0-indexed, None if not in set
    if rank is None:
        return {"rank": None, "mmr": None, "total_players": redis.zcard(key)}
    mmr = redis.zscore(key, player_id)
    return {"rank": rank + 1, "mmr": int(mmr), "total_players": redis.zcard(key)}

# Get top N leaderboard
def get_top_n(game_mode, season_id, n=100, offset=0):
    key = LEADERBOARD_KEY.format(game_mode=game_mode, season_id=season_id)
    entries = redis.zrevrange(key, offset, offset + n - 1, withscores=True)
    player_ids = [pid for pid, score in entries]
    # Batch fetch usernames from a separate hash (or PostgreSQL)
    profiles = redis.hmget("player:usernames", *player_ids)  # pre-built username lookup
    return [
        {"rank": offset + i + 1, "player_id": pid, "username": profiles[i], "mmr": int(score)}
        for i, (pid, score) in enumerate(entries)
    ]

# For a percentile display: "Top X% of players"
def get_percentile(player_id, game_mode, season_id):
    rank = get_my_rank(player_id, game_mode, season_id)["rank"]
    total = redis.zcard(LEADERBOARD_KEY.format(game_mode=game_mode, season_id=season_id))
    if rank is None: return None
    percentile = 100.0 * (1 - (rank - 1) / total)
    return round(percentile, 1)
```

**Interviewer Q&A:**

Q1: How do you handle ties in the leaderboard (multiple players with the same MMR)?
A1: Redis sorted sets handle ties by sorting by lexicographic order of the member (player_id) when scores are equal. This is deterministic but arbitrary. Better approach: use a composite score: `score = mmr × 10^9 + (timestamp_of_last_mmr_update reversed)`. This means players with the same MMR are ranked by who reached that MMR first (earlier = higher rank), which is a reasonable tiebreaker. In practice, TrueSkill scores are floating-point, making exact ties extremely rare.

Q2: How do you handle the season reset at the start of each season?
A2: Season resets require: (1) The old season's leaderboard is persisted to PostgreSQL (already done in real-time); (2) A new Redis sorted set is created for the new season key (e.g., `leaderboard:ranked_tdm:season_5`); (3) Player stats are seeded for the new season: `player_stats` gets a new row per `(player_id, game_mode, season_id)` with `mmr = 1500` (reset to default) or `mmr = prev_season_peak × 0.7 + 1500 × 0.3` (soft reset — partially retains skill); (4) The UI switches to showing the new season leaderboard. The reset is a batch operation at midnight ET on season start day: ~100 M players × ZADD = 100 M Redis operations. At 1 M ops/s on Redis Cluster: ~100 s. This is done in a maintenance window (typically during off-peak hours).

Q3: How do you prevent leaderboard score manipulation (e.g., boosting via intentional losses)?
A3: (1) Minimum games threshold: rank is only shown after 10 placement matches, preventing 1-game MMR manipulation; (2) Decay: MMR decays by 25 points per week of inactivity above a threshold (Diamond+), pushing inactive smurfs off the top; (3) Loss forgiven for documented disconnects: if a player disconnects within 2 minutes of match start, the match is "abandoned" and MMR is not changed; (4) Win trading detection: if two accounts consistently play against each other and alternate wins (suspicious for artificial boosting), the accounts are flagged for review; (5) Hardware fingerprinting: banned accounts attempting to create new accounts are blocked via HWID hash matching.

---

## 7. Scaling

### Horizontal Scaling

| Component | Scaling Strategy | Bottleneck |
|---|---|---|
| Game server pods | One pod per session; scale pod count with active sessions | GPU/CPU for physics; network bandwidth |
| Game Server Manager (Agones) | k8s-native scaling; warm pool of pre-allocated pods | Pod allocation latency (pre-warm 10% extra) |
| Matchmaking workers | One worker per game_mode×region; horizontal within mode | Redis queue contention (use SRANDMEMBER + MULTI/EXEC) |
| WebSocket (social/matchmaking) | Stateless; scale behind L7 LB | Connection count per pod (target 50 k) |
| Leaderboard Redis | Redis Cluster; shard by key hash | Memory per shard (each sorted set: 100 M × 40 bytes = 4 GB) |
| PostgreSQL (player stats) | Primary + 5 read replicas; shard by player_id with Citus | Write throughput at match end burst |
| Kafka | Add brokers; scale partition count | Throughput; 778 updates/s is trivial |

### DB Sharding
- **PostgreSQL `player_stats`**: Shard by `player_id % 16` using Citus. All reads/writes for a player go to one shard; aggregate queries (global stats) run across all shards in parallel. With 100 M players, each shard holds 6.25 M rows — perfectly manageable.
- **Match history**: Partition by `created_at` (time-based) with monthly partitions using native PostgreSQL partitioning. Recent matches are on fast NVMe storage; older partitions are on cheaper HDD. Monthly maintenance job archives partitions > 1 year to S3-backed Parquet.

### Replication
- Game server pods: no replication (single authoritative instance). Resilience via checkpoint-and-resume.
- PostgreSQL: synchronous replication to 1 standby; 5 async read replicas.
- Redis Cluster: 1 replica per primary in separate AZ.
- Kafka: RF=3, acks=all for match result events (critical); RF=2 for ephemeral events.

### Caching
| Layer | Data | TTL | Strategy |
|---|---|---|---|
| REST API in-process | Player profile | 60 s | LRU cache; invalidate on MMR update |
| CDN | Leaderboard top 100 | 60 s | Public, cacheable |
| Redis | Player MMR lookup | No TTL | Always current; updated after each match |
| Game server pod RAM | Entity state | No TTL (in-process) | Single source of truth for the session |

**Interviewer Q&A — Scaling:**

Q1: How do you manage 625 k game server pods on Kubernetes?
A1: At 625 k pods, standard Kubernetes struggles (etcd has ~10 k node limit in practice). Solutions: (1) Agones on top of Kubernetes manages game server pods specifically; (2) Use multiple Kubernetes clusters (one per region, ~100 k pods/cluster), federated by a global Game Server Manager; (3) Bare-metal approach: at Riot/Valve scale, game servers run on bare metal with a custom orchestrator (not k8s) for lower overhead. Each bare metal server runs 100–500 game server processes (not containers) for maximum density; (4) For 625 k sessions, at 200 players/physical server (50 sessions × 16 players × assuming one session per core at 64 Hz), we need 625 k / 50 = 12,500 physical servers. At 500 W/server: 6.25 MW of compute globally.

Q2: How do you handle a game server pod crash mid-match?
A2: The Game Server Manager monitors pod health via heartbeat (game server pod sends a heartbeat every 250 ms). On missed heartbeat (> 750 ms, 3 consecutive misses): (1) All players in the session receive a TCP notification "server reconnect in progress"; (2) The last persisted state snapshot (written every 30 s to Redis) is used to restore the session on a new pod; (3) New pod is allocated, state loaded from Redis, and players are given a new connect_token for the replacement server; (4) Players reconnect automatically (the game client handles "server migration" transparently); (5) Total interruption: 2–5 seconds. Mid-match recovery is a major UX feature — players accept brief interruptions but not full match loss.

Q3: How do you scale anti-cheat processing?
A3: Anti-cheat runs inline in the game server pod (synchronous validation per input). The computational cost is O(players × inputs_per_tick): 16 players × 64 Hz × 5 µs/validation = ~5 ms/tick of anti-cheat overhead out of 15.6 ms tick budget — acceptable. For deeper analysis (replay-based cheat detection), a Kafka consumer group processes game event streams asynchronously: (1) Each match's inputs are logged to Kafka topic `match-inputs`; (2) Anti-cheat analysis pods consume and replay the match inputs, running more expensive checks (ML-based aimbot detection on aim trajectory); (3) This is done post-match and doesn't affect real-time performance; (4) Scaling: add anti-cheat analysis pods proportionally to match volume.

Q4: How do you handle geographic routing so players always hit the nearest game server?
A4: Anycast + latency-based routing: (1) Players' game clients run a pre-match "ping test" (ICMP or HTTPS) to 5–6 regional endpoints at queue entry time; measured latency per region is stored in the matchmaking queue entry; (2) The matchmaking worker selects the region where all players in the proposed match have latency < 80 ms; (3) Game Server Manager in each region has an Agones warm pool of pre-allocated pods; (4) The selected region's Game Server Manager allocates a pod and returns the pod's Anycast IP + port to players; (5) Anycast ensures packets take the shortest BGP path to the data center's network edge; (6) Players in the same city as a data center (e.g., Singapore) get < 5 ms RTT; players at the edge of a region (e.g., Sydney for the Singapore DC) may get 30–50 ms.

Q5: How do you estimate and plan hardware for the game server cluster?
A5: A single game server pod at 64 Hz with 16 players running physics, lag compensation, and delta compression uses approximately: 1 CPU core (at 70% utilization per our benchmarks), 100 MB RAM (500 KB state × 200 pods per node), 5 Mbps network bandwidth (bidirectional: 640 players/node × 200 bytes × 64 Hz). A modern 64-core server with 256 GB RAM and 25 Gbps NIC can host: min(256 GB / 100 MB, 64 cores, 25 Gbps / 5 Mbps) = min(2,560, 64, 5,000) = 64 sessions/server. For 625 k sessions: 625 k / 64 = ~9,766 physical servers globally. At 3U per server in a 40U rack: ~975 racks. Globally distributed across 5 regions.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---|---|---|---|---|
| Game server pod crash | Session drops; 16 players lose match | Heartbeat miss (3 × 250 ms = 750 ms) | State restore to new pod; players auto-reconnect | 2–5 s interruption |
| Matchmaking service crash | Players can't queue for new matches | k8s liveness probe | k8s restarts pod; queue state in Redis persists | < 30 s |
| Redis Cluster node failure | Leaderboard reads/writes fail; presence stale | Cluster gossip; Sentinel | Replica promotion; reconnect | < 10 s |
| PostgreSQL primary failure | Match result writes fail | Patroni health check | Patroni promotes standby; buffer results in Kafka | < 30 s |
| Kafka broker failure | Event delivery delay | Broker health monitor | RF=3; consumers reconnect to replicas | < 10 s |
| Network partition between game server and players | Players experience lag/disconnect | Server-side input timeout | Clients reconnect; session continues with bot substitution | < 30 s |
| DDoS on game server IP | Session disrupted | DDoS detection (traffic rate anomaly) | Anycast IP black-holing; migrate session to new IP | 5–10 min (session may abort) |
| Full AZ failure | Regional service degradation | Cloud provider health | k8s pods rescheduled to other AZs; Anycast routes around | 2–5 min |

### Retries & Idempotency
- **Match result writes to PostgreSQL**: Kafka consumer with at-least-once delivery; `INSERT INTO match_participants ... ON CONFLICT DO NOTHING` (idempotent on `(match_id, player_id)` primary key).
- **Connect token for game server UDP auth**: tokens have a 5-minute expiry; lost tokens require a new `/connect_token` REST call (client handles this automatically on reconnect).
- **Game server pod allocation**: if Agones allocation fails (pod not ready), retry 3 times with 500 ms backoff; fall back to a pod in an adjacent region.

### Circuit Breaker
- **Game server → Redis** (session checkpoint): if Redis is slow (> 50 ms P99 for SET), circuit opens; checkpoint intervals increase from 30 s to 120 s (less frequent writes reduce load). If Redis is down completely, game session continues without checkpointing (risk: full state loss if pod crashes; acceptable for short-duration games).
- **Stats service → PostgreSQL**: if write latency > 100 ms or error rate > 5%, circuit opens; match results are buffered in Kafka (7-day retention). Stats are applied when the circuit recovers. Player MMR may be temporarily stale in the UI, but match history is not lost.

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Purpose |
|---|---|---|---|
| `game_server_tick_duration_µs` | Histogram | P99 > 12,000 µs (12 ms, leaving < 3.6 ms margin) | Tick overrun detection |
| `game_server_active_sessions` | Gauge | — | Capacity planning |
| `input_queue_depth{player_id}` | Gauge | > 5 (lag piling up) | Network lag indicator |
| `anti_cheat_flags_per_session` | Counter | > 10/session | Cheat detection alert |
| `matchmaking_queue_depth{mode,region}` | Gauge | > 1000 (long waits) | Matchmaking capacity |
| `matchmaking_wait_time_s` | Histogram | P95 > 60 s | Matchmaking SLA |
| `session_crash_rate` | Counter | > 0.01% | Reliability SLA |
| `player_connection_errors` | Counter | Spike > 2× baseline | Network issues |
| `leaderboard_write_latency_ms` | Histogram | P99 > 50 ms | Redis health |
| `lag_compensation_rewind_ticks` | Histogram | P95 > 32 ticks (> 500 ms rewind) | Player latency health |
| `postgres_write_queue_depth` | Gauge | > 500 | Match result processing backlog |
| `pod_allocation_latency_ms` | Histogram | P99 > 5000 ms | Agones pool exhaustion |

### Distributed Tracing
- OpenTelemetry spans across: `player_input_received` → `tick_processed` → `lag_compensation_computed` → `delta_sent`. This trace shows the per-tick processing latency broken down by component.
- Matchmaking trace: `player_queued` → `match_found` → `session_allocated` → `connect_token_issued` → `player_connected_to_game_server`. Target: < 5 s end-to-end.
- Sampling: 100% for error paths (failed sessions, crashes, cheat flags); 1% for normal game ticks (high volume).

### Logging
- Game server: log session_id, tick number, and all game events (kills, flag captures) but NOT per-tick player positions (too voluminous). Use structured JSON; emit to Kafka `game-events` topic.
- Anti-cheat: log all flag events with player_id, flag_type, magnitude, and the specific input that triggered the flag. Store in PostgreSQL `player_reports` table for review.
- Matchmaking: log queue entry and exit with wait_time_s, final MMR spread of the match, and region selection reason.
- Session checkpoints: log checkpoint_ts and state_hash to verify integrity on pod restore.
- Retention: game events: 90 days hot, 3 years cold. Anti-cheat logs: 7 years (legal).

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Alternative | Why Chosen |
|---|---|---|---|
| Authoritative server vs. P2P | Authoritative server | Peer-to-peer (lockstep) | P2P has no anti-cheat; one peer's cheat ruins all others. Authoritative server validates all inputs. |
| UDP vs. TCP for game data | UDP | TCP | TCP's retransmission/ordering causes variable latency; UDP with app-level sequencing gives consistent low latency |
| 64 Hz tick rate vs. lower | 64 Hz (128 Hz for premium) | 20 Hz (standard for many games) | 64 Hz is the industry standard for competitive play; lower tick rates create unfair hitboxes |
| Lag compensation: rewind vs. none | Rewind to shooter's time | No lag compensation | Rewind is the only fair approach; without it, high-latency players can't land shots |
| Leaderboard: Redis vs. PostgreSQL | Redis (primary) + PostgreSQL (backup) | PostgreSQL only | Redis ZREVRANK is O(log N) and < 1 ms; PostgreSQL rank query on 100 M rows takes seconds without precomputation |
| Matchmaking: SBMM vs. FBMM | TrueSkill SBMM (skill-based) | Faster-fill FBMM | Skill-based matchmaking is essential for game health and player retention even if it increases wait time |
| Anti-cheat: server-side only vs. kernel driver | Server-side + 3rd party kernel driver | Server-side only | Server-side alone can't detect client memory tampering; kernel driver (BattlEye/EAC) detects process injection |
| Session state: in-memory vs. Redis-backed | In-memory (primary) + Redis checkpoint | Redis-only state | Redis adds 0.2 ms latency per operation; at 64 Hz, Redis-backed state adds 12 ms/tick overhead — 76% of tick budget. In-memory is 100× faster |
| Match result durability | Kafka buffer → PostgreSQL | Write directly to DB at match end | Kafka provides durability buffer if DB is slow; direct writes risk data loss on DB overload at match end burst |

---

## 11. Follow-up Interview Questions

Q1: How would you implement a replay system?
A1: Log all player inputs and the initial game state snapshot to Kafka. On replay request: (1) Fetch the session's initial state from the match archive in S3; (2) Stream the input log from the Kafka archive (or S3 Parquet); (3) A deterministic replay service runs the game simulation at any speed (10× for analysis) by feeding the logged inputs into the same game logic — since the physics engine is deterministic, the replay produces identical results to the original match; (4) The replay is streamed to the viewer as a normal state delta stream (same WebSocket protocol as live viewing). Gotcha: non-determinism must be eliminated (random seeds must be stored with the initial state; floating-point determinism across CPU architectures requires fixed-point math or IEEE 754 reproducible mode).

Q2: How do you implement a spectator mode with delay?
A2: Spectators connect to the game server like players but receive state updates with a configurable delay (30–120 s to prevent real-time strategic information leakage). Implementation: the game server maintains a state history ring buffer of the last 128 seconds (at 1 snapshot/s = 128 snapshots × 500 KB = 64 MB per session). Spectators request `delay=60s`, and the server sends them snapshots from 60 s ago. Professional broadcasts use 120 s delay. The spectator state stream is compressed separately (no lag compensation needed) and delivered via a separate lower-priority packet queue to prevent spectator processing from affecting the live game tick latency.

Q3: How would you design a ranked season with promotion/demotion between tiers?
A3: Tiers (Bronze, Silver, Gold, Platinum, Diamond) each have an MMR range. Promotion occurs when a player's MMR exceeds the tier ceiling; demotion when it falls below the tier floor. Anti-yo-yo mechanism: a 50-point "demotion shield" prevents demotion within the first 5 games after promotion. Implementation: the MMR computation service checks for tier changes after each match and emits a `rank_change` event to Kafka; the notification service delivers "You've been promoted to Gold!" push notifications; the UI subscribes to player events via WebSocket. Soft reset at season start: `new_season_mmr = 1500 + (prev_peak_mmr - 1500) × 0.5` compresses the distribution toward the center.

Q4: How do you handle DDoS attacks on game servers?
A4: Game servers have publicly exposed UDP ports (necessary for low latency). DDoS mitigation: (1) Anycast with DDoS scrubbing at network edge (Cloudflare Magic Transit or AWS Shield Advanced); (2) UDP traffic is rate-limited per source IP at the firewall level (max 1,000 packets/s per IP; game clients send 64 Hz = well under this); (3) Connect tokens (HMAC-signed, time-limited) prevent connection from unauthorized IPs — valid game clients have a token; (4) If a specific game server IP is targeted, Game Server Manager migrates the session to a new IP (allocates new pod, sends "server migration" message to clients with their new connect_token for the new address); (5) Jitter-based amplification: game server does not send packets to an IP unless it has received a valid connect token from that IP (prevents reflection attacks).

Q5: What is "netcode" and what makes it good or bad?
A5: Netcode refers to the collection of algorithms that handle the discrepancy between a player's local game state (with prediction applied) and the server's authoritative state. Good netcode: (1) client-side prediction so inputs feel instantaneous; (2) lag compensation so shots feel fair at any reasonable latency; (3) smooth reconciliation when prediction diverges from server state (smooth correction rather than jarring snap); (4) Entity interpolation so remote players move smoothly between received position updates rather than teleporting. Bad netcode causes "rubber banding" (player snaps back when prediction is wrong), "hit registration issues" (shots that clearly hit don't register, due to missing lag compensation), and "teleporting" (remote players jump between positions instead of interpolating).

Q6: How does the game differentiate between packet loss (expected) and cheating?
A6: Packet loss: a sequence of missing input packets followed by resumed normal packets. The server extrapolates position during gaps and has a "jitter buffer" that accepts inputs up to 200 ms late. Cheating patterns: (1) Speed hack: player claims positions impossible to reach in the time elapsed — detected by distance check against max_speed × elapsed_time; (2) No-clip/wall hack: player moves through solid geometry — server checks collision against the authoritative world geometry (the client cannot modify server-side geometry); (3) Teleportation: discontinuous position jumps not explained by respawn events — flagged as teleport hack; (4) Aimbot: inhuman aim trajectories (instantaneous 180° turns to track targets through walls) detected by analyzing the input history stream; all flagged inputs are logged for anti-cheat review.

Q7: How would you implement a "kill cam" feature?
A7: Kill cam shows the killer's perspective for the last 3 seconds before the kill. Implementation: each game server maintains the last 20 ticks (3.1 s at 64 Hz) of state history per player (this is already needed for lag compensation). On a kill event: (1) Game server packages the 20-tick snapshot of the killer's state (position, aim direction, shooting inputs) and the victim's state; (2) Packages this as a `kill_cam_data` payload and sends it to the killed player via the reliable channel (a small TCP connection maintained alongside the UDP game connection for reliable events); (3) Client renders the kill cam using the killer's inputs applied to their client-side simulation, showing the killer's perspective; (4) Storage: no additional storage required since kill cam data is computed on-demand from the existing state history; only sent to the affected player.

Q8: How do you handle "desync" between the server and a client that has been lagging heavily for 5 seconds?
A8: After 5 seconds of heavy lag, the client's predicted state may be wildly wrong. When the client reconnects (TCP still alive but UDP packets were dropping): (1) The server sends a full state snapshot (not delta) on the next packet after detecting the client was lagging (client's last acked tick is > 50 ticks behind); (2) The client applies the full snapshot, discarding all local predictions; (3) The client resumes normal delta + prediction mode; (4) Visually, the player may see a "snap" (objects jump to correct positions) — this is unavoidable. The server's "reliable event" channel (TCP) ensures all kill events, score changes, and round events during the lag window are replayed to the client so nothing is missed.

Q9: How would you design cross-platform play between PC and console players?
A9: (1) The game server is platform-agnostic — it receives inputs in the same format regardless of source platform; (2) Per-platform matchmaking pools: by default, PC players are in a separate pool from console players (controller vs. mouse/keyboard creates unfair matchups); (3) Opt-in crossplay: players can choose to enable crossplay; when enabled, they're added to a combined pool with a "input device" tag; (4) Aim assist balance: console players with controllers receive server-side aim assist validation (small magnetic radius around targets that doesn't snap but reduces the friction of controller input); PC players get no aim assist; this is validated server-side to prevent PC players from spoofing console identity to gain aim assist; (5) Platform API integration: social features (friend lists, party invites) require OAuth integration with PlayStation Network, Xbox Live, and Nintendo Online.

Q10: How would you run a game tournament with 10,000 simultaneous games?
A10: A tournament requires 10,000 sessions × 16 players = 160,000 players. At the scheduled start time (thundering herd): (1) Pre-allocate all 10,000 game server pods 30 minutes before the tournament (warm them up with health checks); (2) Bracket management service generates match assignments (which 16 players play each other) based on tournament seeding; (3) At T-5 minutes, distribute connect_tokens to all players (they can connect to their assigned server early for lobby banter); (4) Tournament controller fires a synchronized `TOURNAMENT_START` event via Kafka at T+0; game server pods start their tick loop simultaneously (within 100 ms of each other, using NTP); (5) Between rounds, the tournament controller waits for all match results (Kafka consumer listening for `match_end` events), seeds the next round, and allocates new pods for the next round of games.

Q11: How do you store and serve player profile icons/avatars at scale?
A11: Player avatars are static images (JPEG/WebP, 128×128 to 512×512 pixels). Storage: S3 with path `avatars/{player_id}/{version}.webp`. Serving: CDN with long TTL (86400 s since avatars change rarely). Upload: signed S3 URL (generated by platform API, short-lived 10-minute expiry) for client-to-S3 direct upload, bypassing platform API servers entirely. On successful upload, client notifies the API which updates `players.avatar_url` and invalidates the CDN cache for that URL. Image processing (resize to multiple resolutions: 64×64, 128×128, 256×256): triggered by an S3 event notification → Lambda/Cloud Function → store multiple sizes back to S3.

Q12: How would you design the friends presence system?
A12: Presence requires knowing which friends are online/in-game in near-real-time. Architecture: (1) When a player opens the game client, it connects to a WebSocket presence endpoint and registers as "online" (SADD `presence:online` player_id, HSET `presence:status:{player_id}` status "online" with 60 s TTL); (2) Game server pods publish `player_in_game` events to Kafka on session join; presence service updates `presence:status:{player_id}` to "in-game:{match_id}"; (3) Friends list: when a player's friend list is loaded, the UI subscribes to presence updates for all friend_ids via a dedicated WebSocket channel; (4) Fan-out: when a player's status changes, the presence service looks up all players who have this player in their friend list (PostgreSQL reverse lookup on `friends` table, cached in Redis) and sends a push update via WebSocket to their connections; (5) Heartbeat: clients send a ping every 30 s to maintain the 60 s TTL on `presence:status`; if TTL expires without a ping (client crash), the player is automatically marked offline.

Q13: How do you handle the "party queue" where a group of friends want to play together?
A13: Party management: (1) The party leader creates a party (generates a party_id) and invites friends via the friends system; (2) Friends accept, and their matchmaking queue entry includes `party_id`; (3) The matchmaking worker treats party members as a unit — either all are placed in the same match or none are; (4) Party MMR: effective MMR = average of party members' MMRs + party_size_penalty (larger parties get a higher effective MMR to match against coordinated opponents); (5) Mixed-size matching: a party of 4 and a party of 3 can be combined with solo players to fill a 16-player match; the matchmaking algorithm respects party cohesion (all party members on the same team).

Q14: How do you prevent one player with high latency from ruining the experience for everyone else?
A14: Per-player latency isolation: (1) The game server measures each player's latency per tick (from ack_client_tick lag); (2) If a player's latency exceeds 200 ms for 30 consecutive seconds, they receive a warning: "Your connection is degrading other players' experience"; (3) Lag compensation is capped: rewind window never exceeds 100 ms regardless of the high-latency player's actual latency — their shots may not register perfectly, but other players are not disadvantaged; (4) High-latency player's movement uses server-side extrapolation (their actual position is computed server-side, not trusted from their lagged inputs) — they appear smooth to other players; (5) After 60 s of sustained > 200 ms latency, the game server kicks the player with code "CONNECTION_UNSTABLE"; they can rejoin if their connection improves.

Q15: What metrics would you use to measure if your matchmaking is "good"?
A15: North star metric: **match outcome unpredictability** — in a perfectly balanced match, each team should have ~50% win probability. Key metrics: (1) Win rate distribution: should be centered at 50% per player (MMR-adjusted); a healthy distribution has 40–60% of players in the 45–55% win rate range; (2) Mean match MMR spread: difference between highest and lowest MMR player in a session; target < 300 points for ranked, < 500 for casual; (3) Average wait time by percentile: P50 < 10 s, P95 < 45 s, P99 < 120 s; (4) Abandonment rate: percentage of players who cancel queue after > 30 s (high abandonment = queue times are unacceptably long); (5) Post-match satisfaction score (optional survey): "Was this match fair?" — target > 75% "Yes"; (6) Top 1% MMR concentration: how many players are within 100 MMR of each other at the top — too concentrated = everyone knows each other = unhealthy competitive ecosystem.

---

## 12. References & Further Reading

1. Valve's GabeN on Networking: https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking
2. Valve's Lag Compensation in Source Engine: https://developer.valvesoftware.com/wiki/Lag_compensation
3. Riot Games: The Peeker's Advantage in VALORANT: https://playvalorant.com/en-us/news/dev/valorant-128-tick-servers/
4. "Fast-Paced Multiplayer" by Gabriel Gambetta (multi-part series): https://www.gabrielgambetta.com/client-server-game-architecture.html
5. Agones: Open-Source Game Server Orchestration on Kubernetes: https://agones.dev/site/docs/
6. Microsoft TrueSkill Ranking System: https://www.microsoft.com/en-us/research/project/trueskill-ranking-system/
7. TrueSkill2 Paper (Microsoft Research): https://www.microsoft.com/en-us/research/publication/trueskill-2-improved-bayesian-skill-rating-system/
8. Epic Games' Fortnite Server Architecture Blog: https://www.unrealengine.com/en-US/tech-blog/
9. LMAX Disruptor for Low-Latency Tick Processing: https://lmax-exchange.github.io/disruptor/
10. Netcode Explained (Overwatch / Riot GDC Talks): https://www.gdcvault.com/play/1024001/
11. "Game Programming Patterns" — Robert Nystrom: https://gameprogrammingpatterns.com/game-loop.html
12. AWS Architecture: Multiplayer Game Backend on AWS: https://aws.amazon.com/gaming/solutions/
13. BattlEye Anti-Cheat Overview: https://www.battleye.com/
14. Easy Anti-Cheat (EAC): https://www.easy.ac/en-us/
15. "Networked Physics in Virtual Reality" — GDC 2017, Glenn Fiedler: https://www.gdcvault.com/play/1024035/
