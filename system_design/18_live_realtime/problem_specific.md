# Problem-Specific Design — Live Realtime (18_live_realtime)

## Live Streaming

### Unique Functional Requirements
- RTMP / RTMPS / WebRTC ingest from streamers; HLS/DASH delivery to viewers with ABR playback
- Ultra-low-latency mode (< 2 s glass-to-glass) via WebRTC re-streaming
- 100 K concurrent live streams; up to 10 M concurrent viewers per stream; 50 M total concurrent viewers
- DVR / time-shift up to 4 hours; clips (up to 60 s from last 90 s); VOD immediately after stream ends

### Unique Components / Services
- **RTMP Ingest Edge**: Anycast IP for lowest RTT; terminates TCP; relays to transcoder cluster via internal RTMP or SRT
- **FFmpeg NVENC GPU Transcoder**: decodes ingest stream; encodes 4 renditions (360p, 480p, 720p, 1080p) concurrently using NVENC hardware encoder; forces keyframe every 2 s; muxes into 2-second HLS .ts segments; uploads to S3. Capacity: one NVIDIA A10G GPU = ~50 streams at 4-rendition 1080p60 → 100 K streams / 50 = 2,000 GPU instances at peak
- **Playlist Service**: stateless; reads segment availability from S3; generates and updates `.m3u8` variant manifests; maintains sliding DVR window; pushes manifests to CDN PoPs every 2 s via CDN API (proactive push to avoid cache stampede)
- **CDN Caching**: segment cache key `/live/{stream_id}/{rendition}/seg{N:06d}.ts` TTL 86400s (immutable content); manifest TTL 1 s with stale-while-revalidate 1 s; for streams > 10 K viewers, Playlist Service pre-warms all CDN PoPs immediately after segment upload; multi-CDN strategy (Fastly for real-time purge, Akamai/CloudFront for AP capacity); Origin Shield collapses PoP-to-origin requests
- **Chat Fan-out**: Redis pub/sub `chat:stream:{stream_id}`; WebSocket Gateway pods subscribe; each pod maintains local connections for stream; for mega-streams (> 500 K viewers): 10 Redis channels `chat:stream:{stream_id}:{shard_id}` (shards 0–9) spreads pub/sub across 10 Redis nodes; message persisted async via Kafka → Cassandra consumer
- **Viewer Count**: HyperLogLog per stream in Redis `PFADD/PFCOUNT viewers:{stream_id}`; 12 KB/stream regardless of viewer count; < 1% error; published via SSE to streamer dashboard every 5 s
- **Chat Rate Limiter**: in-process token bucket (20 msgs/30 s per user); account age gate (< 5 min → dropped); SHA-256 hash of last 5 messages per user (TTL 10 s) for duplicate detection; moderator `/ban` → Redis blocklist

### Unique Data Model
- **streams**: stream_id UUID, user_id, stream_key VARCHAR(64) UNIQUE (hashed in DB), status (offline/live/ended), ingest_region, ingest_pod (assigned transcoder)
- **stream_renditions**: rendition_name (360p/480p/720p/1080p), width, height, fps, video_bitrate kbps, audio_bitrate kbps, codec
- **stream_segments**: segment_number, duration_ms, storage_path S3 key; partitioned by HASH(stream_id) for write throughput; also tracked in Redis sorted set `stream:{id}:rendition:{name}` score=segment_number
- **chat_messages (Cassandra)**: PK = ((stream_id, bucket=unix_ts/3600), message_id TIMEUUID); 7-day TTL
- **viewer_count_snapshots (TimescaleDB)**: stream_id, recorded_at, viewer_count; hypertable for historical analytics

### Key Differentiator
Live Streaming's uniqueness is its **GPU NVENC transcoder fleet + CDN pre-warming for 50 M concurrent viewers**: FFmpeg NVENC encodes 4 renditions from a single ingest at 2-second segment granularity (keyframe forced every 2 s); Playlist Service proactively pushes manifests to CDN PoPs (1 s TTL, stale-while-revalidate) preventing cache stampede; HyperLogLog (12 KB/stream) enables viewer counting across 100 K streams without storing per-viewer IDs; Redis pub/sub chat fan-out sharded to 10 channels for mega-streams (> 500 K viewers); DVR = immutable segments in S3 with long TTL enables 4-hour time-shift without server state.

---

## Stock Ticker

### Unique Functional Requirements
- Sub-millisecond NBBO (National Best Bid and Offer) from multiple exchanges
- Binary market data protocols: NASDAQ ITCH, NYSE XDP, CBOE PITCH; gap detection + retransmission
- Order book maintenance: ADD/MODIFY/CANCEL/TRADE messages; per-symbol, per-exchange order book
- 10,000 symbols, 500 K concurrent WebSocket clients, 5 M market data updates/s during peak
- Apache Iceberg tick archive on S3; PB-scale historical data with time-travel queries

### Unique Components / Services
- **Feed Handler (DPDK)**: kernel bypass via DPDK (Data Plane Development Kit) for microsecond-level processing; eliminates OS scheduler overhead; one process per exchange; gap detection via sequence numbers; retransmission request on gap → buffer incoming → apply when filled → fall back to exchange snapshot if retransmission > 200 ms
- **Market Data Processor**: single-writer per symbol (consistent hash by symbol); maintains full order book state (price-level sorted map: TreeMap descending for bids, ascending for asks); NormalizedTick struct with `int64 exchange_ts_ns` (PTP nanosecond timestamps); NBBO computed after each book update by iterating all exchange books for the symbol (O(E) ≈ 5–8 exchanges); LMAX Disruptor / lock-free ring buffer for inter-thread queue
- **Order Book Redis Schema**: `ZADD market:book:{symbol}:bid -price "size|order_count|exchange"` (negative price for reverse sort, so ZRANGE returns best bid first); `ZADD market:book:{symbol}:ask price "size|order_count|exchange"`; `HSET market:quote:{symbol}` with bid, ask, last, volume, ts_microseconds, seq_num
- **WebSocket Fan-out**: per-pod inverted index `HashMap<symbol, Set<connection_id>>`; 5 ms batch window → last-write-wins per symbol; 50 pods × 10 K connections = 500 K total; each pod sees full Kafka topic (5 M/s) but 5 ms batching makes it tractable; slow client TCP buffer full → drop oldest pending updates
- **Tick Archive (Apache Iceberg on S3)**: Parquet columnar format; Iceberg for time-travel, predicate pushdown, schema evolution; Trino/Spark for ad-hoc queries; Kafka `feed-audit` topic retains all raw binary messages for regulatory compliance
- **REST Snapshot API**: stateless; reads from Redis `market:quote:{symbol}` in < 1 ms; falls back to Processor state for fresh order book depth; not on the critical sub-millisecond path

### Unique Data Model
- **instruments**: symbol PK, exchange, asset_type, tick_size DECIMAL(12,6), lot_size, isin, cusip, sector
- **ohlcv_candles (TimescaleDB)**: symbol, interval (1m/5m/1h/1d), open_ts, open/high/low/close DECIMAL(18,6), volume BIGINT, vwap; hypertable partitioned by open_ts; compress after 7 days; drop after 2 years
- **order_book_snapshots (TimescaleDB)**: symbol, snapshot_ts, exchange, side (B/A), level, price, size; 30-day retention
- **client_subscriptions**: client_id, symbol, subscription_type (quote/trade/orderbook); `idx_subs_symbol ON client_subscriptions(symbol)` for reverse lookup
- **Redis live state**: `market:quote:{symbol}` HSET; `market:book:{symbol}:bid` / `market:book:{symbol}:ask` ZSET; `ws:symbol:{symbol}:pods` SADD for cross-pod subscription routing; `ws:conn:{connection_id}` HSET pod_id + user_id

### Algorithms

**Order Book Update (pseudocode):**
```python
case ADD_ORDER:
    level = PriceLevel(price_int=msg.price_int)
    (book.bids if msg.side=='B' else book.asks).put(msg.price_int, level)
    level.total_size += msg.size; level.order_count += 1

case MODIFY_ORDER:
    order = find_order(book, msg.order_id)   # O(1) with order_id index
    delta = msg.new_size - order.size
    level.total_size += delta; order.size = msg.new_size

case CANCEL_ORDER:
    level.total_size -= order.size
    if level.total_size == 0: remove price level
```

### Key Differentiator
Stock Ticker's uniqueness is its **DPDK kernel-bypass binary protocol parser + single-writer order book per symbol**: DPDK eliminates OS scheduler jitter enabling microsecond feed processing; single-writer semantics (consistent hash by symbol) ensures lock-free order book updates; negative-score ZADD Redis pattern enables O(log N) top-of-book retrieval for bids; LMAX Disruptor SPSC ring buffer between feed handler and processor eliminates contention; per-pod inverted index + 5 ms last-write-wins batching reduces 50 AAPL updates/s to 1 flush/pod/cycle; Apache Iceberg enables PB-scale tick archive with time-travel for regulatory replay.

---

## Multiplayer Game Backend

### Unique Functional Requirements
- 64 Hz server tick rate (15.625 ms/tick); UDP binary packets; client-side prediction + server reconciliation
- Lag compensation: rewind world state to shooter's perceived time; `rewind_ticks = min(latency_ms / tick_interval_ms, 64)` (max 2 s rewind)
- Authoritative anti-cheat: speed check (MAX_SPEED × 1.2 tolerance), aim turn-rate validation per tick
- TrueSkill MMR matchmaking: expanding search window ±50 base + 25 MMR/s up to ±200 max; all players in match must have < 80 ms RTT to selected server region
- 10 M concurrent players; Agones on Kubernetes for game server pod lifecycle

### Unique Components / Services
- **Game Server Pod (Authoritative)**: runs 64 Hz tick loop; processes inputs in arrival order; applies physics/collision; broadcasts delta state; one pod per match session (intentional isolation); input buffer `RingBuffer[64 ticks]` per player for lag compensation history
- **Tick Loop Pseudocode**: receive UDP packets → validate inputs → apply movement → process shots with lag compensation → physics_tick → collect events → compute delta state → send_udp to each player → save state snapshot
- **Lag Compensation**: `rewind_tick = current_tick - min(latency_ms / 15.625, 64)`; raycast against historical entity positions; additional validation: rewound target position must be geometrically plausible (no shooting through walls using current-state geometry)
- **Anti-Cheat (In-Pod)**: speed hack: `distance(last_pos, claimed_pos) > MAX_SPEED × TICK_INTERVAL_S × 1.2` → flag + use server-extrapolated position; aim hack: `turn_delta > MAX_TURN_RATE × 1.5` → flag + clamp aim
- **Delta Compression**: dirty-bit per field; only changed fields included in state packet; full snapshot = 16 players × 64 B = 1,024 B; typical delta = 200 B (5× reduction); `flags` bitmask indicates which optional fields present
- **Game Server Manager (Agones)**: GameServer CRD in Kubernetes; statuses: available → allocated → running → terminating; on match end, pod recycled; pod registry stored in etcd/K8s, not in PostgreSQL
- **TrueSkill Matchmaking**: `QueueEntry {player_id, mmr, mmr_uncertainty, preferred_region, latency_ms per region, queued_at}`; SortedList by MMR; cycle every 1 s; `mmr_window = min(50 + int(wait_s × 25), 200)`; latency constraint: all players in match < 80 ms RTT to region
- **Disconnection Handling**: extrapolate position for 5 ticks (78 ms); freeze at 30 ticks (0.47 s); remove entity at 300 ticks (4.7 s); if reconnects within 60 s with same session_id + connect_token → rejoin with preserved state

### Unique Data Model
- **player_stats**: (player_id, game_mode, season_id) PK; mmr, rank_tier, wins, losses, kills, deaths, assists, total_damage, matches_played; `idx_player_stats_mmr ON player_stats(game_mode, season_id, mmr DESC)` for leaderboard queries
- **matches**: match_id, game_mode, region, server_pod_id, status (lobby/active/completed/aborted), winner_team, map_id, game_version
- **match_participants**: (match_id, player_id) PK; team, kills, deaths, damage_dealt/taken, headshots, mmr_before, mmr_after, mmr_delta, performance_score
- **In-memory game state (pod RAM)**: `GameState {tick uint64, entities {player_id → PlayerEntity {position Vec3, velocity Vec3, health int16, armor int16, ammo map, is_alive bool}}}` + `InputHistory {RingBuffer[64 ticks]}` per player
- **Redis leaderboards**: `ZADD leaderboard:{game_mode}:{season_id} score player_id`; player presence/online status HASH

### Key Differentiator
Multiplayer Game Backend's uniqueness is its **64 Hz authoritative tick loop with lag compensation up to 64-tick (2 s) rewind**: UDP with client-side prediction + reconciliation eliminates head-of-line blocking; lag compensation rewound raycast against historical state history at `min(latency_ms/15.625, 64)` ticks ensures fair hit registration even at 200 ms latency; anti-cheat runs inside the authoritative pod per-tick (no external call); Agones Kubernetes-native pod allocation provides declarative game server lifecycle; TrueSkill expanding window (±50 + 25/s) balances fairness vs. wait time; delta compression achieves 5× bandwidth reduction per tick per player.

---

## Collaborative Document (CRDT)

### Unique Functional Requirements
- Multiple users editing the same document simultaneously with zero-latency local application (offline-first)
- CRDT-based consistency (no central coordination for conflict resolution): any order of operation application converges to identical final state
- 10 M concurrent editing sessions; each document's clients route to the same session server via consistent hash on doc_id
- 30-day operation log (Cassandra); document snapshots every 5 minutes of editing activity (S3); CRDT GC (rebasing) when tombstones > 10× live content

### Unique Components / Services
- **Yjs CRDT (YATA Algorithm)**: each character insertion tagged with unique ID `(client_id: int, clock: int)` — Lamport clock per client; deletions are tombstones (is_deleted=True, item stays in doubly-linked list); deterministic ordering: tiebreak by client_id when items share left_origin; offline edits accumulate locally; merge on reconnect via SYNC_REQUEST/SYNC_RESPONSE
- **Item struct**: `{id: (client_id, clock), content: str|None, left_origin: Item.id|None, right_origin: Item.id|None, is_deleted: bool}`; `item_index: HashMap[id, Item]` for O(1) lookup
- **integrate() algorithm**: scan rightward from left_origin; if concurrent item has same left neighbor → tiebreak by client_id (lower wins); insert before the first item whose left_origin differs or whose client_id > new_item.client_id; deterministic + commutative across all clients
- **Session Server (Document Owner)**: consistent hash on doc_id routes all WebSocket connections to same pod; pod holds in-memory YDoc; broadcasts ops to all connected clients; assigns monotonically increasing seq_num; writes to Kafka `doc-ops-{shard}` keyed by doc_id; Kafka consumer persists to Cassandra
- **Snapshot Service**: background worker; every 5 min of editing activity → serialize CRDT state → zstd compress → S3 `docs/{doc_id}/snapshots/{seq_num}.bin`; updates `docs/{doc_id}/current.bin` pointer atomically; document load = latest snapshot + replay Cassandra ops since snapshot seq_num
- **CRDT Garbage Collection (Rebasing)**: when tombstone_count / live_count > 10 → create new CRDT with only live items re-IDed from scratch → new S3 snapshot → old clients migrate on next reconnect
- **Presence Service**: cursor positions in `HSET doc:presence:{doc_id}:{user_id}` (TTL 30 s, refreshed per cursor move); batched in 100 ms windows; decoupled from CRDT operation path to avoid cursor thrashing slowing text operations

### Unique Data Model
- **document_operations (Cassandra)**: PK = ((doc_id, shard=unix_ts/3600%64), seq_num); op_type (insert/delete/format/comment), author_id, vector_clock TEXT, op_data BLOB (protobuf), client_ts, server_ts; 90-day TTL; `default_time_to_live = 7776000`
- **document_revisions**: doc_id, revision_id BIGSERIAL, op_seq_start, op_seq_end, snapshot_s3_key, author_id, summary; pointer into Cassandra op ranges + S3 snapshots
- **S3 snapshots**: `docs/{doc_id}/snapshots/{seq_num}.bin` zstd-compressed serialized CRDT state; rebuilt when tombstones > 10× live items
- **documents**: doc_id, owner_id, title VARCHAR(500), last_snapshot_seq BIGINT, word_count, storage_bytes
- **document_permissions**: (doc_id, principal_type, principal_id) PK; permission (view/comment/edit/owner); link_token VARCHAR(64) for shareable link; expires_at; ACL cache in Redis TTL 60 s

### Algorithms

**Offline sync on reconnect:**
1. Client sends `{type: "SYNC_REQUEST", since_seq: last_server_seq_received}`
2. Server fetches all ops since last_server_seq from Cassandra → sends `SYNC_RESPONSE` batch
3. Client feeds received ops into local CRDT via `apply_op()` — CRDT guarantees convergence in any order
4. Client sends its pending ops to server → server integrates → final state identical on all clients; no data lost

### Key Differentiator
Collaborative Document's uniqueness is its **Yjs YATA CRDT with tombstone-based deletion + consistent-hash session routing**: CRDT eliminates central OT transform server (offline-first, multi-server safe); YATA tiebreaker (client_id comparison) ensures deterministic ordering of concurrent insertions at the same position; tombstones accumulate until rebasing (GC when tombstones > 10× live) → periodic snapshot reset; consistent hash on doc_id collocates all editors on the same Session Server pod for in-process CRDT integration without distributed coordination; Cassandra op log (90-day TTL, shard bucketing) + S3 snapshots (5-min intervals) provide fast load = snapshot + incremental replay.
