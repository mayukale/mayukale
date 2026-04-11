# Common Patterns — Live Realtime (18_live_realtime)

## Common Components

### WebSocket / Persistent Connection for Real-Time Push
- All four systems use persistent bidirectional connections (WebSocket or UDP) to push state updates to clients without polling
- live_streaming: WebSocket Gateway pods for chat; HLS polling replaces WebSocket for video (HTTP/1.1 manifest polling every 1–2 s)
- stock_ticker: WebSocket (`wss://`) with subscribe/unsubscribe messages per symbol; 500 K concurrent connections across 50 pods
- multiplayer_game: UDP (unreliable, unordered) for game state at 64 Hz; UDP preferred over TCP to avoid head-of-line blocking from packet loss
- collaborative_doc: WebSocket for operation streaming; each document routes all clients to the same session server pod via consistent hash on doc_id

### Kafka as Central Event / Operation Bus
- All four use Kafka to decouple real-time event production from persistence and downstream processing
- live_streaming: `chat-persist` topic (Kafka → Cassandra persistence consumer); stream lifecycle events (Kafka → notification fan-out)
- stock_ticker: `market-data` topic partitioned by symbol hash (1024 partitions, 10 K symbols); enables multiple consumers: tick archive, WebSocket engine, OHLCV candle builder
- multiplayer_game: game events stream (kills, stats, anti-cheat signals); partitioned by match_id; consumed by stats aggregator, achievement processor, replay service
- collaborative_doc: `doc-ops-{shard}` topic keyed by doc_id; operation log service persists to Cassandra; background compaction service reads from Kafka

### Redis for Hot In-Memory State and Pub/Sub
- All four use Redis for sub-millisecond state operations on the critical real-time path
- live_streaming: Redis pub/sub channels `chat:stream:{stream_id}` for chat fan-out; HyperLogLog `viewers:{stream_id}` for viewer count (PFADD/PFCOUNT, 12 KB/stream, < 1% error); segment tracking sorted set
- stock_ticker: `HSET market:quote:{symbol}` with bid/ask/last/volume/ts/seq; `ZADD market:book:{symbol}:bid -price "size|count|exchange"` (negative prices for reverse sort); `SADD ws:symbol:{symbol}:pods` for cross-pod routing
- multiplayer_game: `ZADD leaderboard:{game_mode}:{season_id} score player_id`; player presence / online status; session routing
- collaborative_doc: `HSET doc:session:{doc_id}` session metadata (TTL 300 s); `HSET doc:presence:{doc_id}:{user_id}` cursor position + color (TTL 30 s); `SET doc:perm:{doc_id}:{user_id}` permission cache (TTL 60 s); pub/sub for presence fan-out

### Cassandra for High-Write Time-Series / Append-Only Storage
- live_streaming and collaborative_doc use Cassandra for append-only time-series data
- live_streaming: `chat_messages(stream_id UUID, bucket INT = unix_ts/3600, message_id TIMEUUID ...)` PK = ((stream_id, bucket), message_id); 7-day TTL; 100 K messages/s platform-wide
- collaborative_doc: `document_operations(doc_id UUID, shard INT = unix_ts/3600 % 64, seq_num BIGINT ...)` PK = ((doc_id, shard), seq_num); 90-day TTL; older ops in S3 snapshots

### S3 Object Store for Durable Media and State
- live_streaming: `.ts` HLS segments and `.m3u8` manifests; segment TTL 24h (immutable); DVR 4-hour window
- stock_ticker: Apache Iceberg tables (Parquet) for full tick archive; predicate pushdown for range scans; time-travel queries
- collaborative_doc: document snapshots (`docs/{doc_id}/snapshots/{seq_num}.bin`) compressed with zstd; every 5 minutes of editing activity; `docs/{doc_id}/current.bin` → latest snapshot

### PostgreSQL for Operational Metadata
- All four use PostgreSQL for relational metadata: streams/users/follows (live_streaming), instruments/api_keys/subscriptions (stock_ticker), players/matches/stats/friends (multiplayer_game), documents/permissions/revisions/comments (collaborative_doc)
- All use ACID transactions and relational integrity for low-frequency management operations; reads served from replicas; real-time hot path uses Redis / in-memory state

### Stateless Serving Layer + External State
- All four isolate stateful session data from the serving tier:
  - live_streaming: stateless Playlist Service (reads S3); stateful Chat Gateway (but pub/sub state in Redis, not in-process)
  - stock_ticker: stateless REST Snapshot API reads Redis; stateful Market Data Processor is single-writer per symbol (partitioned by consistent hash)
  - multiplayer_game: each Game Server Pod holds full in-memory session state for exactly one match (intentional isolation, not shared state)
  - collaborative_doc: each Session Server pod holds CRDT state for one document; consistent hash routing ensures all clients connect to the same pod

## Common Databases

### Redis
- All four; hot-path state, pub/sub, sorted sets for leaderboards/order books, HyperLogLog for cardinality, TTL-based ephemeral state; cluster mode for horizontal scaling

### Kafka
- All four; ordered, durable event bus; partitioned by key (symbol, doc_id, stream_id, match_id); RF=3; multiple consumer groups for fan-out without coupling

### Cassandra
- live_streaming, collaborative_doc; append-only time-series (chat messages, document operations); wide-column with time-bucketed partition keys; TTL-based retention; multi-DC replication

### S3
- live_streaming, stock_ticker, collaborative_doc; durable, cost-effective object storage for media, archives, and snapshots; CDN-friendly (immutable segment URLs)

### PostgreSQL
- All four; operational metadata and relational integrity; not on the latency-critical real-time path

## Common Communication Patterns

### UDP for Latency-Critical Bidirectional Real-Time
- multiplayer_game: UDP with magic bytes (0x4742), tick-numbered packets, client-side prediction + server reconciliation; unreliable is intentional — packet loss causes a stale input rather than head-of-line block
- stock_ticker: exchange feed protocols (NASDAQ ITCH, NYSE XDP, CBOE PITCH) delivered over UDP multicast or TCP binary; DPDK kernel bypass eliminates OS scheduling overhead

### Async Fan-Out via Redis Pub/Sub
- live_streaming: Redis channel `chat:stream:{stream_id}` for message fan-out across WebSocket gateway pods; all subscribed pods receive and forward to local connections
- collaborative_doc: Redis pub/sub for cursor/presence fan-out (decoupled from CRDT operation path)
- stock_ticker: per-pod inverted index (HashMap<symbol, Set<connection_id>>) for O(1) local fan-out + 5 ms batching window for last-write-wins deduplication

## Common Scalability Techniques

### Single-Writer Semantics per Entity
- stock_ticker: Market Data Processor partitioned by symbol hash → one writer per symbol → no locking on order book state; LMAX Disruptor / lock-free ring buffer for inter-thread queue
- multiplayer_game: one Game Server Pod per match → no distributed game state during match
- collaborative_doc: all clients for a document route to the same Session Server pod → single-writer CRDT state

### Pre-Computation and Snapshot + Delta Pattern
- collaborative_doc: periodic CRDT snapshots every 5 min to S3; on document load, load latest snapshot + replay only recent ops from Cassandra
- stock_ticker: REST Snapshot API reads from Redis (last price, order book) for O(1) response; only cache misses hit the processing layer
- live_streaming: Playlist Service regenerates manifests every 2 s; CDN caches segments with 24h TTL (immutable); segment pre-warming for streams > 10 K viewers

## Common Deep Dive Questions

### How do you fan out real-time events to hundreds of thousands of subscribers without a single bottleneck?
Answer: The pattern is always: (1) partition subscribers by entity (stream_id, symbol, doc_id) across multiple gateway pods using consistent hashing or pub/sub channels; (2) within each pod, maintain an in-memory inverted index for O(1) lookup of local subscribers; (3) apply batching/last-write-wins for high-frequency updates to prevent write amplification. For stock_ticker: per-pod inverted index + 5 ms batch window reduces 50 updates/s to 1 flush per symbol per cycle. For live_streaming: Redis pub/sub distributes chat across pods; mega-streams use 10 Redis channels per stream to spread pub/sub load.
Present in: live_streaming, stock_ticker, multiplayer_game, collaborative_doc

### How do you handle state recovery after a server crash?
Answer: All four externalize their durable state: live_streaming → segments in S3 (manifest recovery from S3); stock_ticker → exchange reconnects and replays from market snapshot + buffered messages; multiplayer_game → player reconnects within 60 s and rejoins the same match pod (session_id + connect_token); collaborative_doc → CRDT state rebuilt from latest S3 snapshot + Cassandra op replay. In-memory state is always reconstructible from the durable log; this is why Kafka and S3/Cassandra are authoritative, not Redis.
Present in: live_streaming, stock_ticker, multiplayer_game, collaborative_doc

## Common NFRs

- **Latency**: real-time push < 50–200 ms P99 delivery after event occurrence
- **Throughput**: 100 K–1 M events/s ingested; 500 K–10 M concurrent connections across the platform
- **Availability**: 99.99% for real-time delivery; graceful degradation on component failure
- **Durability**: media segments, tick data, and document operations must not be lost once acknowledged
- **Ordering**: per-entity ordering preserved (per-symbol for ticks, per-document for ops, per-session for game inputs)
