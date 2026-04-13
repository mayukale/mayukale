# Pattern 18: Live & Realtime — Interview Study Guide

Reading Pattern 18: Live & Realtime — 4 problems, 7 shared components

The four problems in this pattern are: Stock Ticker (market data platform), Collaborative Document Editor (Google Docs scale), Multiplayer Game Backend (authoritative server), and Live Streaming Platform (Twitch/YouTube Live scale). Every single one of these is a live system design interview staple. If you can deeply internalize the 7 shared architectural components and understand the 4 specific differentiators, you will be prepared for any variant of this problem class at any FAANG/NVIDIA-level interview.

---

## STEP 1 — PATTERN OVERVIEW

**What is "Live & Realtime"?**

A system is in this category if it has to push state changes to connected clients continuously, typically within milliseconds to low seconds, without the client explicitly asking for each update. The client subscribes once, and the server drives the conversation from that point forward. This is fundamentally different from request-response systems because the server initiates the data transfer.

**The 4 problems and their sizes:**

| Problem | Connections | Update Rate | Latency Target |
|---|---|---|---|
| **Stock Ticker** | 500 K concurrent WebSocket clients | 5 M feed messages/s peak | 50 ms P99 exchange to client |
| **Collaborative Doc** | 30 M WebSocket connections (10 M sessions × 3 users) | 20 M operations/s theoretical peak | 500 ms P99 same-region propagation |
| **Multiplayer Game** | 10 M concurrent players (UDP) | 640 M packets/s (64 Hz × 10 M players) | 50 ms P99 in-region |
| **Live Streaming** | 50 M concurrent viewers (HLS + WebSocket for chat) | 100 K chat messages/s; 25 M HLS segment requests/s | 8 s P99 glass-to-glass |

**The 7 shared components** (covered fully in Step 4):
1. WebSocket / Persistent connections for real-time push
2. Kafka as central event bus
3. Redis for hot in-memory state and pub/sub
4. Cassandra for high-write append-only time-series
5. S3 object store for durable media and state
6. PostgreSQL for operational metadata
7. Stateless serving layer backed by external state

---

## STEP 2 — MENTAL MODEL

**The core idea: publish-subscribe with fan-out.**

Every real-time system, regardless of domain, reduces to this: something produces a state change event, and many connected clients need to see that change very quickly. The central engineering challenge is not producing the event — it is delivering that event to every interested subscriber before the world changes again.

Think of it like a scoreboard at a stadium. The game is constantly happening. Tens of thousands of fans in the stadium (clients) need to see the current score on the big screen (display) without asking for it every second. The system watches the game (producer), routes events through infrastructure (the scoreboard wiring), and updates every display simultaneously. Now imagine 50 million fans watching from home via TV — same game, same events, but the fan-out problem is orders of magnitude harder.

**The real-world analogy that sticks: Bloomberg Terminal + the stadium scoreboard at internet scale.**

A Bloomberg terminal shows 10,000 stocks updating simultaneously. Each trader sees their watchlist change in real time. The engineering behind it is the same as Twitch chat, a multiplayer shooter, or Google Docs — an event is produced, it fans out to subscribers, and consistency/ordering requirements differ by domain.

**Why is this hard?**

Three things make real-time systems genuinely difficult, and they all interact:

1. **Fan-out amplification.** A single event (one stock price update, one keystroke, one game tick) needs to reach potentially millions of subscribers. If AAPL updates 50 times per second and 200,000 people subscribe to AAPL, that is 10 million deliveries per second from one stock. Naive approaches collapse under this load.

2. **State management under concurrent writes.** Multiple clients write state simultaneously (multiple users typing in the same doc, multiple players shooting at each other). You need conflict resolution. Wrong answer: last-write-wins. Right answers: CRDTs, authoritative servers, or single-writer semantics per entity.

3. **Persistent connections at scale.** HTTP is stateless and cheap. WebSocket connections are stateful and expensive — each one holds memory, a file descriptor, and a kernel socket. At 500K concurrent connections, a single server cannot handle them all. You need a connection management strategy from day one.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

These 4-5 questions change the architecture significantly. Ask them before drawing anything.

**1. "What is the latency target for receiving an update after it is produced?"**

This is the single most important question. The answer drives your entire transport and infrastructure choice.
- < 1 ms: you are in HFT territory, requires kernel-bypass (DPDK), co-location, hardware timestamps
- < 50 ms: requires WebSocket or UDP, in-memory state, Kafka with careful tuning
- < 500 ms: WebSocket with small batching acceptable, Redis pub/sub
- < 8 s: HLS polling is acceptable (Live Streaming video delivery)

**What changes:** If the answer is sub-millisecond, you drop Kafka from the hot path and use kernel-bypass directly. If the answer is 8 seconds, HLS polling over CDN beats WebSocket for scalability at 50M viewers.

**2. "Is the update flow one-to-many (broadcast), many-to-many (collaboration), or one-to-one?"**

- One-to-many (stock ticker, live streaming): server pushes to subscribers; clients mostly read
- Many-to-many (collaborative doc, multiplayer game): clients write, everyone sees the writes
- This changes where you put conflict resolution logic and how expensive fan-out is

**What changes:** Many-to-many systems require conflict resolution (CRDT or authoritative server). One-to-many systems can use simpler last-write-wins with batching.

**3. "What are the read-to-write ratios and how skewed is popularity? (Hotspot/celebrity problem)"**

A stock like AAPL has 200K subscribers. An obscure penny stock has 3. A Twitch streamer with 1M viewers has a fundamentally different fan-out problem than one with 10 viewers.

**What changes:** Heavily skewed systems need special handling for hot entities — Redis channel sharding, special CDN pre-warming, partitioned fan-out pods. Systems with uniform distribution can use simpler consistent hashing.

**4. "What happens when a client disconnects and reconnects? What is the recovery contract?"**

- Does the client need to catch up on missed updates? (Collaborative doc: yes, must replay ops)
- Can it just resume from current state? (Stock ticker: yes, just get latest price)
- Does the session need to survive? (Game: yes, player reconnects within 60s must rejoin same match)

**What changes:** Catch-up on reconnect requires a durable ordered log (Kafka or Cassandra). Current-state-only recovery can use Redis snapshots. Session survival requires connection state externalized to Redis.

**5. "Is ordering guaranteed, and at what granularity?"**

- Per-entity ordering: every AAPL update arrives in sequence number order
- Total ordering: all events across all entities in global time order (almost never needed, extremely expensive)
- Eventual consistency: convergence guaranteed but delivery order unconstrained (CRDT)

**What changes:** Per-entity ordering is achievable with Kafka partitioned by entity key. Total ordering requires a single partition (bottleneck). CRDT removes the ordering requirement entirely for conflict resolution.

**Red flags to watch for:**
- Interviewer says "it should be real-time" without specifying a latency number — always clarify
- "We need strong consistency on every update" — push back, distinguish between consistency models
- "Just use WebSockets everywhere" — ask about the 50M viewer case; WebSocket does not scale to HLS delivery costs

---

### 3b. Functional Requirements

**Core (must-have for any realtime system):**
- Producers can publish state-change events (symbol price updates, keystrokes, game inputs, video segments)
- Clients can subscribe to a stream of events for entities they care about
- Events are delivered to subscribers within the agreed latency SLA
- Clients can reconnect and recover missed state

**Scope-defining requirements (clarify these):**
- Does the client read historical data, or only live? (Stock ticker: both; streaming: DVR up to 4h)
- Does the client write as well as read? (Collab doc: yes; live streaming: mostly no)
- Is ordering semantically important? (Game: yes per-tick; stock: yes per-symbol seq)

**Clear statement for the interview:**

"I'll design a system that ingests [events] from [producers], maintains [state] for [entities], and pushes updates to [N] concurrent subscribers within [X ms] P99. The system must handle [Y events/s] peak ingest and recover subscriber state on reconnect."

---

### 3c. Non-Functional Requirements (NFRs)

**Latency** — derive from use case, not from an abstract target. Market data: traders lose money after 50ms. Collaborative doc: human perception of "real-time" collaboration is ~200-500ms. Game: 50ms RTT matches human reaction time. Live video: 8s is acceptable because viewers expect buffering.

**Throughput** — calculate it, do not guess. Fan-out is the multiplier: [events/s ingest] × [avg subscribers per entity] = [deliveries/s]. This number will shock you and is the anchor for infrastructure sizing.

**Availability** — 99.99% (52 min/year) is the typical target. **Key trade-off:** ✅ Highly available systems tolerate brief inconsistency (show slightly stale price) vs. ❌ strongly consistent systems that block on failure (never acceptable in realtime).

**Durability** — "once acknowledged, never lost." This forces you to have Kafka/Cassandra/S3 as the durable backbone. Redis is not durable (data can be lost on crash). **Key trade-off:** ✅ Redis gives you sub-millisecond state reads but ❌ you cannot treat it as your source of truth.

**Ordering** — decide granularity early. Per-entity ordering via Kafka partition key is essentially free. Cross-entity global ordering is extremely expensive (single partition = single throughput bottleneck). **Key trade-off:** ✅ Per-entity ordering satisfies almost every real use case; ❌ global ordering is almost never worth the cost.

**Scalability** — horizontal scaling of stateless workers is easy. The hard part is stateful components: WebSocket gateway pods hold connection state, session servers hold CRDT state, game servers hold match state. Design these as intentionally isolated single-writers per entity, externalizing recovery state to Kafka/Redis.

---

### 3d. Capacity Estimation

**The master formula for realtime systems:**

```
Fan-out delivery rate = [peak events per second] × [avg subscribers per entity]
WebSocket servers needed = [concurrent connections] / [connections per server]
Kafka partitions = max([peak events/s] / [consumer throughput per partition], [consumers])
Storage = [events/s] × [bytes/event] × [seconds/day × retention days]
```

**Anchor numbers (memorize these):**

| Metric | Typical Value | Notes |
|---|---|---|
| WebSocket connections per server | 10,000–50,000 | 10K is conservative/safe; 50K with tuned kernel |
| Redis ops/s single node | ~100,000 | Use cluster for more |
| Kafka throughput per partition | ~10 MB/s | Partition by entity key |
| Kafka consumer lag acceptable | < 1s | Otherwise consumers are falling behind |
| HLS segment size (2s, 720p) | ~750 KB | For bandwidth calc |
| CRDT operation size | ~50 bytes | For doc collaboration bandwidth |
| Game input packet | ~60 bytes UDP | At 64 Hz per player |
| Stock tick normalized size | ~100 bytes | After normalization from binary protocol |

**Walk through one example (Stock Ticker):**
- 5M feed messages/s × 100 bytes = 500 MB/s ingest (4 Gbps)
- 1000 actively traded symbols × 5 updates/s × 200 avg subscribers = 1M WebSocket deliveries/s
- 500K connections / 10K per pod = 50 WebSocket pods
- Storage: 5M msgs/s × 6.5h × 3600s × 100 bytes = 11.7 TB/day raw ticks

**Architecture implications from the math:**
- 1M deliveries/s → Kafka partition count must exceed your WebSocket pod count (50 pods × fan-out each)
- 11.7 TB/day → columnar compression (Parquet/Iceberg) is mandatory, not optional
- 50 WebSocket pods → need pub/sub or distributed routing so each pod only holds its subset of connections

**Time yourself:** a good capacity estimate takes 5-7 minutes. Interviewers want to see you derive numbers, not memorize them. Show the formula, plug in assumptions, get a number, then draw the architecture implication.

---

### 3e. High-Level Design (HLD)

**The universal 4-layer realtime architecture:**

```
[Producers / Ingest] → [Event Bus (Kafka)] → [Fan-out / Push Layer] → [Clients]
                                   ↓
                          [State Store (Redis)]
                          [Archive (S3/Cassandra)]
                          [Metadata (PostgreSQL)]
```

**4-6 components you must draw, in whiteboard order:**

1. **Ingest / Producer Layer** — where raw events enter your system. Could be exchange feed handlers, RTMP ingest servers, WebSocket receive endpoints, or game UDP listeners. Key design: single-writer semantics per entity (one process owns one symbol, one session server owns one document, one game pod owns one match).

2. **Event Bus (Kafka)** — decouples producers from consumers. Partitioned by entity key. Producers write here; multiple consumer groups read independently (archive consumer, push consumer, analytics consumer). This is why you can add new downstream consumers without touching the producer.

3. **State Cache (Redis)** — fast read path for current state. The Push Layer writes current state here; the REST API reads from here for snapshot queries. TTL-based for ephemeral state (presence, permissions cache). Redis is NOT your source of truth — Kafka/Cassandra is.

4. **Fan-out / Push Layer** — WebSocket gateway pods or game server pods. Each pod holds a subset of connections and has an in-memory inverted index (entity → list of local connections). Receives from Kafka, fans out locally. Consistent hash routing ensures the right connections land on the right pod.

5. **Durable Archive (Cassandra / S3)** — append-only time-series writes (Cassandra for queryable history with TTL, S3 for bulk immutable blobs). This is where you rebuild state after a crash.

6. **Metadata (PostgreSQL)** — users, permissions, entities reference data. Low frequency reads/writes. ACID guaranteed. Never on the hot realtime path.

**Key decisions to articulate while drawing:**
- "I'm partitioning Kafka by symbol/doc_id/match_id to ensure per-entity ordering"
- "All WebSocket connections for a document route to the same Session Server via consistent hash — that's how I avoid distributed CRDT coordination"
- "Redis is for read performance, Kafka is for durability — I never treat Redis as source of truth"

**Whiteboard order:** Start with the client on the left, the event producer on the right, draw Kafka in the middle, then fill in State Cache above and Archive below, then draw the Fan-out layer between Kafka and clients. This takes 3-4 minutes and gives the interviewer a complete mental map before you go deep.

---

### 3f. Deep Dive Areas

**The 2-3 areas interviewers probe hardest in this pattern:**

**Deep Dive 1: Fan-out at scale (asked in every single interview)**

Problem: A single entity (hot stock, mega-streamer, popular game) has 200K subscribers. A naive loop over all subscribers from one server takes too long and saturates network cards.

Solution: Three-layer fan-out.
- Layer 1: Partition subscribers across N pods (consistent hash by entity). Each pod holds ~10K connections for that entity.
- Layer 2: Within each pod, maintain an in-memory inverted index: `HashMap<entity_id, Set<connection_id>>`. O(1) lookup, no DB hit.
- Layer 3: Apply batching/last-write-wins for high-frequency updates. Stock tickers use a 5ms batch window — instead of fanning out each of AAPL's 50 updates/second, you flush once per 5ms with only the latest state. This reduces 50 deliveries/pod-cycle to 1.

Trade-offs: ✅ Batching reduces write amplification dramatically. ❌ Batching adds up to 5ms latency. ✅ Per-pod inverted index eliminates all DB lookup on hot path. ❌ Pod crash loses connection routing — but connection routing is rebuilt on reconnect, not from a DB.

**Deep Dive 2: Conflict resolution for concurrent writers (asked in collaborative doc and game interviews)**

Problem: User A inserts "H" at position 5. User B simultaneously deletes the character at position 5. Both operations reach the server with the same base state. What is the correct merge?

Two approaches:

Operational Transformation (OT): Transform B's delete by the effect of A's insert. The server must apply a transform function for every pair of concurrent operation types. Works but requires a centralization point — all operations must be ordered by the server before clients can commit. Google Docs originally used this (Jupiter algorithm). ❌ Difficult to implement correctly for all operation types. ❌ Requires server round-trip before client can show the result.

CRDT (specifically Yjs/YATA): Every character insertion is tagged with a unique ID `(client_id, logical_clock)`. Deletions are tombstones — the character is marked deleted but its ID stays in the linked list. Concurrent insertions at the same position are ordered deterministically by client_id as a tiebreaker. This means the final state is identical regardless of the order operations are applied. ✅ Offline-first — clients apply operations locally before sending to server. ✅ No server round-trip for conflict resolution. ❌ Tombstones accumulate over time — need periodic GC (rebasing when tombstones > 10× live content).

For game backends: use authoritative server model instead. The server is the single source of truth. Clients predict locally (client-side prediction) but the server's state always wins. Conflict resolution is just "the server is right." ✅ Simple conflict semantics. ❌ Players with high latency see prediction errors corrected visibly (rubber-banding).

**Unprompted trade-off to mention:** CRDTs are fundamentally better for offline-first scenarios but have space overhead from tombstones. OT requires server-serialization but has no space overhead. For a mobile app that needs offline editing, CRDT wins. For a web-only tool where you can enforce connectivity, OT is simpler.

**Deep Dive 3: Recovery after server crash**

Problem: The WebSocket gateway pod dies. All 10,000 connections drop. How do clients recover?

The key insight: **in-memory state must always be reconstructible from the durable log**. This is why Kafka and Cassandra/S3 are authoritative, not Redis.

Recovery patterns by problem:
- **Stock ticker**: Exchange reconnects and replays from market snapshot + retransmission; client re-subscribes and gets latest state from Redis cache; no data was truly lost because Kafka has the full feed
- **Collaborative doc**: Client sends `SYNC_REQUEST {since_seq: last_seq_received}`; session server fetches ops since that seq from Cassandra and sends `SYNC_RESPONSE`; CRDT semantics guarantee convergence after replay
- **Game**: Player has 60 seconds to reconnect with same session_id + connect_token; server extrapolates player position for 5 ticks (78ms), freezes at 30 ticks, removes entity at 300 ticks; if reconnects in time → state preserved
- **Live streaming**: CDN caches segments independently; if transcoder pod dies, stream router reassigns to new pod; RTMP ingest re-establishes; viewers experience brief buffering at most

**Unprompted trade-off:** Kafka consumer lag is your early warning system. If consumers are falling behind, you are not recovering — you are accumulating debt. Monitor `consumer_lag` per partition. If lag > 1 second, either your consumers are too slow or your producers are bursting.

---

### 3g. Failure Scenarios

**Mode 1: Fan-out pod crash**

What happens: 10,000 WebSocket connections drop. Clients see disconnection events and begin reconnecting. New connections re-establish to any healthy pod (load balancer distributes). Clients send their reconnect requests; state is rebuilt from Redis/Cassandra.

Senior framing: "The key is that connection state is ephemeral and rebuild-able. I never rely on the pod having memory of previous connections. The pod is purely a message forwarder. If it dies, clients reconnect and the new pod reads current state from Redis. No data loss because Redis and Kafka are still alive."

**Mode 2: Kafka partition leader failure**

What happens: Kafka's ISR (in-sync replica) protocol promotes a follower to leader within milliseconds. Producers see a brief error and retry with the new leader (configured via `retries` and `retry.backoff.ms`). Consumers transparently reconnect to the new leader.

Senior framing: "I set `acks=all` on producers (wait for all ISR replicas) and `min.insync.replicas=2` on the topic. This means no message is acknowledged until at least 2 replicas have it. Combined with RF=3, I can lose 1 broker and never lose a message. The trade-off is ~2ms extra latency per write, which is acceptable for everything except the sub-millisecond stock tick ingest path — for that I use `acks=1` and accept the tail-risk of losing in-flight messages during a broker failure, trading durability for latency."

**Mode 3: Redis failure**

What happens: Redis is the hot-path read cache. If it fails, REST snapshot APIs have no data to serve. WebSocket fan-out routing loses subscription routing tables.

Senior framing: "Redis is a cache, not a database. My system degrades gracefully on Redis failure: REST APIs fall back to the processing layer (slower, but correct). WebSocket routing breaks until the pod rebuilds its in-memory inverted index from a Kafka replay of subscription events. The critical insight is that Redis failure does not cause data loss — it causes a latency spike. I monitor Redis with a 99th percentile latency alert and have automated failover to the replica via Redis Sentinel or Cluster."

**Mode 4: Consumer falling behind (backpressure)**

What happens: A WebSocket push consumer cannot deliver messages fast enough. Its Kafka offset falls behind. Clients start receiving stale updates.

Senior framing: "I implement **last-write-wins semantics in the consumer**. When a consumer fetches a batch of 1000 events from Kafka and sees 50 updates for AAPL, it delivers only the most recent one to subscribers. This is called 'coalescing' or 'compaction' in the consumer. Combined with the 5ms batch window, a falling-behind consumer catches up quickly because it skips stale intermediate states. The client never needs every intermediate price — just the current one. This is fundamentally different from an order-processing system where you cannot skip events."

---

## STEP 4 — COMMON COMPONENTS

Every one of these components appears in at least 3 of the 4 problems. Know them cold.

---

### Component 1: WebSocket / Persistent Connection

**Why used:** HTTP request-response requires the client to ask for updates. Real-time systems need the server to push. WebSocket provides a persistent bidirectional channel over a single TCP connection. Established via HTTP upgrade handshake; after upgrade, both sides can send frames at any time.

**Key config:**
- Heartbeat ping/pong every 30s, client must respond within 10s. Detects dead connections without waiting for TCP timeout (which can take minutes).
- Max frame size: 64KB default. Large payloads (CRDT sync, game state) should be split or compressed.
- Connection capacity: a single WebSocket server handles 10K–50K concurrent connections with tuned Linux kernel (increase `net.core.somaxconn`, `fs.file-max`, `net.ipv4.tcp_tw_reuse`).
- TLS termination: terminate TLS at the load balancer or at the pod — terminating at LB reduces CPU load on pods but prevents end-to-end encryption without re-encryption.

**What without it:** You poll. Polling at 1 req/s for 500K clients = 500K HTTP requests/s just for "no update" responses. This is 100× more server load for worse latency. Long-polling (hold request open until update) is better but still creates TCP overhead for each new request.

**Note on UDP (Game Backend):** Games use UDP instead of WebSocket because TCP's head-of-line blocking is fatal at 64 Hz. If one game state packet is lost and TCP retransmits, the entire stream stalls while waiting for the retransmit. With UDP, the next packet arrives normally (with the latest state) and the old one is simply discarded. For real-time systems where the latest state supersedes all previous states, UDP is correct.

---

### Component 2: Kafka as Central Event Bus

**Why used:** Kafka is the spine of all four systems because it solves three problems simultaneously: (1) durability — once an event is written to Kafka with `acks=all`, it survives broker failures; (2) fan-out — multiple independent consumer groups can all read the same topic without the producer knowing or caring; (3) replay — consumers can seek to any offset and replay history, enabling crash recovery.

**Key config:**
- Partition by entity key: `symbol`, `doc_id`, `stream_id`, `match_id`. This guarantees per-entity ordering at O(log P) cost.
- Replication factor 3, `min.insync.replicas=2`. Never lose a message if 1 broker dies.
- Retention: set based on the longest recovery window needed, not business logic. If consumers can fall 24 hours behind and still catch up, set retention to 48 hours minimum.
- `linger.ms=5`, `batch.size=65536` for throughput. `linger.ms=0`, `acks=1` for ultra-low latency.
- Consumer group IDs: one per downstream use case. Archive consumer group reads everything. Push consumer group reads everything. They advance independently.

**What without it:** You either couple producers to consumers (producer calls WebSocket service directly — now they must both be up simultaneously) or you build your own queue. The producer dies if the consumer is slow. Fan-out requires the producer to know about every downstream system. Kafka absorbs all of this.

---

### Component 3: Redis for Hot In-Memory State and Pub/Sub

**Why used:** Redis gives sub-millisecond reads and writes for the hot path. Four specific Redis features dominate this pattern:

- **Hash (HSET/HGET):** Store current stock quote `market:quote:AAPL` or session metadata. O(1) field access.
- **Sorted Set (ZADD/ZRANGE):** Order book (`market:book:AAPL:bid` with negative price scores for reverse sort), leaderboards (`leaderboard:ranked_tdm:season_1` with MMR as score). O(log N) add, O(log N + K) range query.
- **HyperLogLog (PFADD/PFCOUNT):** Approximate unique viewer count per stream. 12 KB per counter regardless of cardinality, < 1% error. Never store a `SET` of viewer IDs — that is O(N) memory.
- **Pub/Sub channels:** Fan-out chat messages across WebSocket gateway pods. Publish once to `chat:stream:{stream_id}`, all subscribed pods receive and forward to their local connections. For mega-streams, shard across 10 channels to avoid single-node bottleneck.

**Key config:**
- TTL on ephemeral state: cursor positions (30s TTL, refreshed on movement), permission cache (60s TTL), session metadata (300s TTL). Automatic cleanup without a garbage collector.
- Redis Cluster mode: 16,384 hash slots distributed across nodes. Client libraries handle routing automatically.
- AOF persistence for anything you care about recovering. RDB snapshots for faster recovery if brief data loss on crash is acceptable.

**What without it:** Every hot-path read hits PostgreSQL or Cassandra. PostgreSQL can handle ~10K QPS on a well-tuned instance. At 83K REST requests/s (Stock Ticker) or 500K permission checks/s (Collaborative Doc), you need Redis in front.

---

### Component 4: Cassandra for High-Write Append-Only Time-Series

**Why used:** Cassandra's LSM-tree storage engine converts random writes into sequential disk writes, giving it 100K+ sustained writes/s on commodity hardware with predictable latency. Two use cases in this pattern: chat messages (Live Streaming, 100K messages/s platform-wide) and document operations (Collaborative Doc, millions of ops/s theoretical peak).

**Key config:**
- Partition key design is critical. Use a composite partition key with a time bucket to bound partition size: `(stream_id, bucket INT = unix_ts / 3600)`. Without bucketing, a single stream_id partition grows forever and becomes a "large partition" — Cassandra's #1 performance anti-pattern (single partition cannot be distributed across nodes).
- Clustering column: `message_id TIMEUUID` for chat (gives natural time ordering within partition). `seq_num BIGINT` for document operations (logical ordering by operation sequence).
- `default_time_to_live`: 7 days for chat (604,800 seconds), 90 days for document operations (7,776,000 seconds). Automatic expiry without explicit deletes.
- Replication factor 3, `LOCAL_QUORUM` for writes and reads. Balances consistency with latency.

**What without it:** PostgreSQL will not sustain 100K writes/s for time-series append-only workloads without extreme sharding and connection pooling. MySQL is worse. At these write rates, you need an LSM-based store.

---

### Component 5: S3 Object Store for Durable Media and State

**Why used:** S3 is where you put large, immutable blobs that need 11-nines durability and cost-effective long-term storage. Three use cases:

- **Live Streaming:** 2-second HLS `.ts` segments (immutable once written, cache forever) and `.m3u8` manifests (TTL 1s, frequently updated). The entire 150 Tbps delivery problem is solved by CDN caching immutable segments.
- **Stock Ticker:** Apache Iceberg tables in Parquet columnar format for PB-scale tick archive. Parquet predicate pushdown means a query for AAPL trades on 2024-04-01 reads only the relevant row groups, not the full file.
- **Collaborative Doc:** Periodic CRDT snapshots (`docs/{doc_id}/snapshots/{seq_num}.bin`, zstd-compressed) so document load = latest snapshot + replay only recent ops, not full history.

**Key config:**
- For immutable content (HLS segments, Parquet files): `Cache-Control: max-age=86400, immutable`. CDN caches forever.
- For mutable pointers (HLS manifest, current snapshot pointer): short TTL or explicit cache invalidation via CDN API.
- S3 conditional writes (`If-None-Match`, `If-Match`) to prevent race conditions when multiple writers update the same object.
- Apache Iceberg metadata: enables time-travel queries, atomic schema evolution, and partition pruning on S3 — you get a queryable data warehouse without paying for one.

**What without it:** Live streaming cannot scale past a few thousand viewers without CDN-served immutable segments. The origin cannot serve 50M viewers. Tick data at PB scale cannot be stored in a relational database.

---

### Component 6: PostgreSQL for Operational Metadata

**Why used:** Instruments reference data, user accounts, document permissions, match history, stream metadata — all of these are relational, low-frequency, and benefit from ACID guarantees and referential integrity. Foreign keys prevent orphaned permissions. Transactions ensure that creating a document and its initial permissions happen atomically.

**Key config:**
- Primary reads from replicas; only writes go to primary. This gives you read scale-out for free.
- Connection pooling (PgBouncer): PostgreSQL handles ~300 concurrent connections well. With thousands of application pods each wanting 10 connections, use PgBouncer in transaction mode to pool to ~100 DB connections.
- Indexes matter: `idx_subs_symbol ON client_subscriptions(symbol)` — reverse lookup "which clients subscribe to AAPL" for subscription management. `idx_player_stats_mmr ON player_stats(game_mode, season_id, mmr DESC)` for MMR range queries in matchmaking.
- Never put real-time data here. Tick prices, document operations, chat messages, game inputs — none of these belong in PostgreSQL.

**What without it:** Every permission check and user lookup hits Cassandra (wrong data model for joins) or Redis (no persistence guarantees). At low volume, yes you could use DynamoDB or Firestore, but at this scale the relational model for permissions (ACLs, joins with users table, group membership) is genuinely valuable.

---

### Component 7: Stateless Serving Layer + External State

**Why used:** This is an architectural principle, not a single technology. All four systems separate "where connections live" from "where state lives." The serving layer (WebSocket pods, REST pods, Playlist Service) holds no state that cannot be reconstructed. All durable state is in Kafka, Redis, Cassandra, S3, or PostgreSQL.

**Why this matters:** When a serving pod dies, the only impact is connection drops. No data is lost. Clients reconnect to any healthy pod and rebuild state from the external stores. This enables horizontal scaling: add more pods without data migration.

**Specific implementations:**
- Stock Ticker: stateless REST Snapshot API reads Redis for current prices. WebSocket pods hold connections but not subscription state — subscription state is in a Redis `SADD ws:symbol:{symbol}:pods` set.
- Live Streaming: Playlist Service is fully stateless — it just reads segment availability from S3 and renders a manifest. No local state.
- Collaborative Doc: Session Server holds in-memory CRDT state — this is intentionally stateful. But the CRDT is always rebuild-able from the latest S3 snapshot + Cassandra op replay.
- Game Backend: Game Server Pod holds match state — intentionally isolated, one pod per match. If pod dies, the match is irrecoverable (accepted trade-off at 99.95% availability target). Match results are written to Kafka/PostgreSQL every few seconds for partial recovery.

**What without it:** Serving pods become pets, not cattle. You cannot scale them out, you cannot restart them, and deployment becomes a coordination exercise.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

These are the things that make each problem unique. If you blur these in an interview, you look like you have memorized a template rather than understood the domain.

---

### Stock Ticker: DPDK Kernel-Bypass + Single-Writer Order Book

**What makes it unique:** Every other system in this pattern starts with data already inside your network. Stock ticker is unique because the raw data comes from exchanges in binary wire protocols (NASDAQ ITCH, NYSE XDP, CBOE PITCH) over UDP multicast, and the ingestion must happen at microsecond granularity. A 50ms end-to-end SLA with 5M messages/s ingest means you cannot afford OS kernel interrupt latency. This requires DPDK (Data Plane Development Kit) kernel bypass — the NIC writes packets directly to a memory ring buffer that your application polls, eliminating kernel scheduling jitter entirely.

The second unique aspect is the order book. An order book is a data structure that tracks every outstanding limit order at every price level. Messages come as ADD_ORDER, MODIFY_ORDER, CANCEL_ORDER, TRADE events at up to 500K/s per symbol. You maintain this as a `HashMap<price_as_integer, PriceLevel>` plus a `TreeMap<price>` for sorted iteration. Price is stored as a fixed-point integer (price × 10,000) to eliminate floating-point rounding errors. Single-writer semantics per symbol via consistent hash means no locks anywhere on the hot path.

**Two-sentence differentiator:** Stock Ticker is the only problem that lives below the OS kernel — DPDK kernel bypass for microsecond feed parsing, PTP nanosecond timestamps, and lock-free LMAX Disruptor ring buffers between threads. The order book data structure (HashMap + TreeMap, fixed-point prices, negative-score ZADD in Redis for best-bid-first ordering) and Apache Iceberg PB-scale tick archive with regulatory compliance are completely unique to this domain.

---

### Collaborative Document: CRDT + Consistent-Hash Session Routing

**What makes it unique:** Every other problem in this pattern has a natural authority — the exchange publishes prices, the game server validates inputs, the streamer pushes video. Collaborative editing has no natural authority: two users can simultaneously edit the same character position while offline, and both edits are equally valid. This requires a conflict resolution algorithm that converges without requiring a central coordinator.

Yjs (YATA algorithm) solves this: every insertion is tagged `(client_id, logical_clock)`. Deletions are tombstones. Concurrent insertions at the same position are deterministically ordered by client_id as a tiebreaker. This is commutative — any order of application produces the same final state. The consequence: offline editing just works. A user edits while flying and reconnects at landing; their operations merge perfectly with others' operations from the same period.

The second unique aspect: all clients editing a document must route to the same Session Server pod (consistent hash by doc_id). The pod holds the CRDT state in RAM. If you did not do this, you would need distributed CRDT merging across pods on every operation — very expensive.

**Two-sentence differentiator:** Collaborative Doc is the only problem that requires true conflict-free distributed data structures (Yjs CRDT) instead of authority-based conflict resolution, enabling genuinely offline-first editing that converges automatically on reconnect. Consistent-hash routing of all editors to the same Session Server pod eliminates distributed CRDT coordination, while Cassandra's 90-day operation log + S3 snapshots every 5 minutes of editing enables fast document load as snapshot-plus-incremental-replay.

---

### Multiplayer Game Backend: 64Hz Authoritative Tick Loop + Lag Compensation

**What makes it unique:** Games are the only problem in this pattern where the server runs a physics simulation. The game server is not a router or a storage system — it is a computational engine running at 64 frames per second, processing physics, collision detection, hit registration, and anti-cheat validation all within a 15.6ms window. This requires completely different infrastructure thinking: UDP (not TCP) to avoid head-of-line blocking, binary packed packets (not JSON), and hardware-aware code (cache-friendly data structures, no garbage collection pauses during match).

Lag compensation is a uniquely game-specific problem. Player A on a 100ms connection shoots at Player B. When Player A pulled the trigger, they saw Player B at position X. But by the time the input reaches the server, Player B has moved. To make the game feel fair, the server must rewind its world state 100ms into the past (finding the tick when Player A fired), perform the raycast against Player B's historical position, and decide if the hit was valid. This is the `rewind_ticks = min(latency_ms / tick_interval_ms, 64)` formula.

**Two-sentence differentiator:** Multiplayer Game Backend is the only problem running a real-time physics simulation — a 64Hz authoritative tick loop with per-tick input validation, lag compensation (rewinding world state up to 64 ticks/2 seconds to validate shots at the shooter's perceived time), and delta compression (dirty-bit bitmask reducing 1024-byte full state to ~200-byte delta per tick). The Agones/Kubernetes game server pod lifecycle, TrueSkill expanding-window matchmaking (±50 base + 25 MMR/second), and UDP binary packet format with client-side prediction + server reconciliation are completely domain-specific.

---

### Live Streaming: GPU NVENC Transcoder Fleet + CDN at 150 Tbps

**What makes it unique:** Live streaming is the only problem in this pattern where the bottleneck is compute-heavy media processing, not message routing. Transcoding a 1080p60 stream into four renditions in real time requires GPU hardware (NVIDIA NVENC). At 100K concurrent streams, that is ~2,000 GPU instances continuously running FFmpeg. No other problem in this pattern requires GPU-scale infrastructure.

The delivery side is equally unique. 50M concurrent viewers watching at 3 Mbps average = 150 Tbps of egress. No origin infrastructure can serve this directly. The entire delivery model depends on CDN — immutable 2-second .ts segments cached at edge PoPs globally, with manifest TTL of 1 second (just enough to ensure freshness). The Playlist Service's job is specifically to keep manifests updated frequently enough that the CDN TTL ensures freshness without cache stampedes.

**Two-sentence differentiator:** Live Streaming is the only problem in this pattern driven by GPU compute (NVENC hardware encoding, ~50 streams per A10G GPU, 2,000 GPU instances at 100K stream peak) and 150 Tbps CDN delivery via immutable HLS segment caching (86400s TTL) with 1-second manifest TTL and proactive CDN pre-warming for streams exceeding 10K viewers. The HyperLogLog viewer count (12 KB per stream regardless of scale), DVR via long-lived S3 segment retention, and Redis-sharded chat pub/sub across 10 channels for mega-streams are all live-streaming-specific patterns.

---

## STEP 6 — Q&A BANK

### Tier 1: Surface Questions (5-7 questions, 2-4 sentences each)

**Q1: "Why use WebSockets over HTTP polling for real-time updates?"**

**WebSocket eliminates the overhead of establishing a new TCP connection and HTTP handshake for each update.** With polling at 1 request/second for 500K clients, you generate 500K HTTP requests/second of "no update" traffic — wasted compute, wasted bandwidth, and worse latency (up to 1 second stale). WebSocket holds one persistent TCP connection per client; the server pushes updates at the moment they occur. For situations where WebSocket is not feasible (legacy proxies, one-way fire-and-forget), Server-Sent Events (SSE) is a reasonable middle ground for server-to-client streams.

**Q2: "Why partition Kafka by entity key rather than randomly?"**

**Random partitioning distributes load evenly but destroys per-entity ordering.** If AAPL updates are on partitions 3, 7, and 12, consumers cannot determine the correct order of AAPL price changes. Partitioning by symbol hash means all AAPL events land on partition (hash("AAPL") % N), so a single consumer processes them in sequence number order. The trade-off is that hot symbols (AAPL, MSFT) may overload their partition — mitigated by choosing a large partition count (1024+) and monitoring per-partition lag.

**Q3: "What is a HyperLogLog and why use it for viewer counts?"**

**HyperLogLog is a probabilistic data structure that estimates the cardinality of a set (count of unique items) using a fixed 12 KB of memory regardless of how large the set is, with less than 1% error.** The alternative — maintaining a Redis SET of all viewer IDs — consumes 50 bytes × N viewers; at 10M viewers per stream, that is 500 MB just for one popular stream. With 100K concurrent streams, the SET approach is untenable. HyperLogLog's `PFADD/PFCOUNT` operations are exactly what you need: add a viewer when they join, count unique viewers whenever the dashboard needs a number.

**Q4: "What is client-side prediction in game networking?"**

**Client-side prediction means the game client applies the player's own inputs immediately, without waiting for server confirmation, to eliminate the perception of input lag.** At 60ms RTT to the server, a player would feel 60ms of delay between pressing forward and seeing their character move — completely unplayable. With prediction, the client moves the character instantly. When the server's authoritative state arrives, the client compares it to its prediction and applies a correction if they diverge (server reconciliation). Well-implemented, this correction is invisible to the player. The risk is misprediction — the character briefly snaps back — which is why the server's state always wins.

**Q5: "What is the difference between CRDT and Operational Transformation (OT)?"**

**Both solve the same problem — concurrent edits to shared state — but with different mechanisms.** OT requires a central server to sequence all operations and apply transformation functions for each pair of concurrent operation types; it works but requires server ordering before clients can commit, making offline editing difficult. CRDT (specifically Yjs/YATA for text) encodes enough information in each operation (unique ID, logical clock, neighbor references) that operations can be applied in any order and converge to the same result on all clients, enabling true offline-first editing. The trade-off: CRDT has space overhead from tombstones (deleted-but-retained items) that requires periodic garbage collection.

**Q6: "Why is Redis not a suitable primary database for this system?"**

**Redis holds all data in RAM, is single-threaded for command execution, and its persistence mechanisms (RDB/AOF) have edge cases that can lose recent writes.** More fundamentally: Redis has no built-in replication with strong consistency guarantees for reads (Redis Sentinel/Cluster use asynchronous replication). If Redis crashes before an AOF fsync, you lose data. The systems in this pattern use Redis as an extremely fast cache in front of Kafka (durable event log) and PostgreSQL/Cassandra (durable databases). Redis is authoritative for ephemeral state (presence, permission cache with TTL), but never for data that cannot be reconstructed from somewhere else.

**Q7: "Why use UDP instead of TCP for the game server?"**

**TCP's reliability guarantee is the wrong guarantee for real-time game state.** TCP retransmits lost packets and holds back later packets until the retransmit arrives (head-of-line blocking). For game state at 64Hz, if one packet is lost, you do not want to wait for retransmission — you want the next packet, which already contains more current state. With UDP, lost packets are simply discarded. The client uses client-side prediction to cover the gap, and the next server update reconciles everything. TCP's head-of-line blocking would cause visible stuttering during any packet loss. Reliability is implemented at the application layer (sequence numbers, state resend if no ack) rather than transport layer.

---

### Tier 2: Deep Dive Questions (5-7 questions, with why + trade-offs)

**Q1: "How do you handle a 'hot' stock symbol with 500,000 subscribers receiving 50 updates per second?"**

**The core problem is that 500K × 50 = 25M deliveries per second from a single entity, which would overload any single server.**

The solution is three layers. First, partition subscribers across N WebSocket gateway pods via consistent hash on subscriber-id; the symbol update fan-out is distributed across all pods. Second, each pod maintains an in-memory inverted index `HashMap<symbol, Set<connection_id>>` for O(1) local lookup — no database hit per delivery. Third, apply a 5ms batch window with last-write-wins: within each 5ms window, only the latest AAPL update is delivered to each subscriber, collapsing 50 updates/s to 1 flush per 5ms per pod.

Trade-off: ✅ This reduces the effective fan-out from 25M/s to ~200K/s (1 update per 5ms × 500K subscribers, but at pod granularity much less). ❌ The 5ms window adds latency. ✅ For most use cases (retail trader watching a watchlist), sub-5ms is imperceptible. ❌ For algorithmic traders who need every tick, offer a "full feed" tier that bypasses batching and sends directly to a lower-fanout FIX/binary connection.

For mega-popular symbols, you can also use Redis pub/sub where the symbol is the channel name — every pod subscribed to `market:updates:AAPL` receives the update and fans out to its local connections.

**Q2: "How does the CRDT handle a user who was offline for 2 hours and then reconnects?"**

**The client sends a `SYNC_REQUEST {since_seq: last_server_seq_received}` immediately on reconnect.**

The Session Server fetches all operations since that sequence number from Cassandra (or from the nearest S3 snapshot + Cassandra for the incremental ops). It sends a `SYNC_RESPONSE` with the full batch of missed operations. The client's CRDT engine feeds each received operation through `apply_op()` — and this is where CRDT shines. The CRDT guarantees that applying operations in any order produces the same final state. So even if the client had local operations that the server did not receive, and the server had operations the client did not receive, after both sides exchange their pending operations via the sync protocol, every client converges to the same document state.

Trade-off: ✅ No data is lost, no edits are silently discarded. ✅ Works offline for any duration. ❌ If the user was offline for 3 months, the Cassandra ops for that period may have expired (90-day TTL). In this case, the server sends a full document snapshot instead, and the client's local operations from 3 months ago are lost — an acceptable trade-off for a TTL-based retention policy.

**Q3: "Your Kafka consumer for WebSocket fan-out is falling behind. What do you do?"**

**First, diagnose: is this a throughput problem (consumer too slow) or a burst problem (temporary spike)?**

Check the consumer lag metric by partition. If lag is growing uniformly across partitions, the consumer is computationally bottlenecked. If lag is only on the hot partitions (AAPL, MSFT, GOOG), it is a fan-out hotspot problem.

For throughput: scale out consumers horizontally (add more consumer group instances). Since partitions are the unit of parallelism, the max consumers = number of partitions. If you need more consumers than partitions, increase partition count (requires rebalance — do this proactively, not during an incident).

For fan-out hotspot: add **coalescing** in the consumer. When processing a batch, group events by symbol and keep only the latest per symbol before delivering. This means a consumer processing 10,000 events where 5,000 are AAPL updates will only deliver 1 AAPL update to subscribers — eliminating the hotspot at the cost of losing intermediate updates (acceptable for stock prices, not for order book depth changes where all intermediate states may matter).

Trade-off: ✅ Coalescing dramatically reduces fan-out cost. ❌ Consumers of order book data need every intermediate state to maintain an accurate local order book — coalescing breaks order book accuracy. Solution: separate consumer groups for "last quote" (coalescing OK) and "order book" (no coalescing, needs every message).

**Q4: "How do you prevent a rogue game client from cheating by sending false position data?"**

**The authoritative server model means the server validates every input before updating game state.** Clients send inputs (movement direction, aim angles, actions), not positions. The server maintains the canonical world state and moves entities according to physics.

Specific checks per tick:
- Speed hack: compute `distance(last_pos, claimed_pos)`. If > `MAX_SPEED × TICK_INTERVAL_S × 1.2` (20% tolerance for floating-point precision), flag the client, discard the claimed position, and use the server-extrapolated position instead.
- Aim hack: compute `turn_delta` between consecutive aim vectors. If > `MAX_TURN_RATE × 1.5`, flag and clamp.
- Teleportation: `distance > 5 meters per tick` at normal speed → impossible → flag.

Trade-off: ✅ Server validation makes position spoofing non-functional (server ignores it). ❌ False positives from legitimate lag spikes — hence the 1.2× tolerance factor. ✅ Anti-cheat runs in-process in the game pod, zero network latency for each check. ❌ Cannot detect subtle aim bots that stay within physical limits — these require ML-based behavioral analysis on recorded match data (offline, out-of-scope for real-time system design).

**Q5: "Explain the HLS delivery pipeline and why the CDN is critical."**

**HLS is a pull-based protocol: the client periodically polls a manifest (.m3u8 file) listing available video segments, then downloads each segment.** Segments are immutable once uploaded — a 2-second segment for stream X, rendition 720p, segment number 12345 never changes. This immutability is the key to CDN efficiency.

The pipeline: FFmpeg transcoder uploads `seg012345.ts` to S3 with `Cache-Control: max-age=86400, immutable`. Playlist Service updates the `.m3u8` manifest (TTL 1 second, `stale-while-revalidate: 1`). CDN PoPs cache segments with 24-hour TTL. When the first viewer in a region requests `seg012345.ts`, the PoP fetches from origin and caches. All subsequent viewers in that region get it from CDN cache.

At 10M viewers watching the same stream: 25M segment requests every 2 seconds. Without CDN, origin would need 25M/2s = 12.5M requests/second. With CDN at 99.9% cache hit rate, origin only sees 12,500 requests/second. ✅ CDN is not an optimization — it is the only reason this architecture works at scale. ❌ DVR requires keeping old segments alive in CDN cache; for a 4-hour DVR window at 100K streams, you need ~810 TB of edge cache capacity across all PoPs.

**Q6: "How does lag compensation work and what are its limits?"**

**Lag compensation rewounds the server's view of the world to the time when the shooter saw the target, to determine if the shot was valid from the shooter's perspective.**

When Player A fires at tick 1000 with a 100ms RTT, the input arrives at the server at approximately tick 1006. Player B has moved since tick 1000. Without compensation, the shot misses even though Player A's aim was perfect. With compensation: the server finds tick 994 in its history buffer (`rewind_ticks = 100ms / 15.625ms = 6.4 ≈ 6 ticks`), gets Player B's position at that tick, performs the raycast, and determines a hit.

Limits: ✅ Fairness for all latency levels up to the max rewind window (64 ticks = 1 second). ❌ At 200ms latency, the server must rewind 12 ticks — meaning Player B's client has already shown them moving away, but they still die from a shot that looks like it missed. This is the fundamental tension of lag compensation: what is fair for the shooter feels unfair for the target. ❌ Rewind cannot exceed the history buffer size (64 ticks). Players with > 1 second latency cannot benefit from compensation and should not be in competitive play. ❌ Cannot rewind through solid walls using current geometry — the server must validate that the rewound shot path does not pass through current-state geometry (prevents shooting through walls that were closed at current time but open in history).

**Q7: "What happens to a document editing session when the Session Server pod crashes?"**

**All clients connected to that pod experience a WebSocket disconnection. The pod held the in-memory CRDT state for that document.**

Recovery: The API Gateway detects the pod is down (health check fails, Kubernetes pod status changes). A new pod is allocated for the document (consistent hash routing still points to the same pod slot; a new pod is started). The new pod loads state from the latest S3 snapshot + replays Cassandra ops since that snapshot's sequence number. This takes 1-10 seconds depending on how much catching up is needed.

Clients reconnect (WebSocket reconnect with exponential backoff) and send `SYNC_REQUEST {since_seq: last_seq_received}`. The rebuilt pod responds with any operations they missed. Clients with local pending operations send those as well; CRDT merge handles any conflicts.

Trade-off: ✅ No data is lost (Kafka and Cassandra were receiving writes; S3 has the last snapshot). ❌ 1-10 seconds of downtime for that document's session (all users see "reconnecting"). ❌ Any operations sent to the crashed pod but not yet persisted to Kafka could be lost. To mitigate: the session server writes to Kafka synchronously before broadcasting to clients — if the Kafka write fails, the operation is rejected. This ensures operations visible to clients are always in the durable log.

---

### Tier 3: Staff+ Stress Tests (3-5 questions, reason aloud)

**Q1: "We have a mega-streamer with 10 million concurrent viewers in a single stream. Chat is completely non-functional. What happened and how do you fix it?"**

Reason aloud:

"Let me think through the chat architecture. Chat messages are published to a Redis pub/sub channel `chat:stream:{stream_id}`. All WebSocket gateway pods subscribe to that channel and fan out to their local connections. At 10M viewers, say we have 10K connections per pod and 1,000 pods. Every chat message published to that single Redis channel goes to all 1,000 pods. At 5 messages/second, Redis is publishing 5,000 fan-out events per second on a single pub/sub channel. That is a single Redis node bottleneck.

The fix: shard the Redis channel into 10 channels `chat:stream:{stream_id}:{shard_0..9}`. Each pod subscribes to exactly one shard (pod_id % 10 → shard). When publishing a chat message, publish to shard `hash(message_id) % 10`. Now the 5 messages/second load is distributed across 10 Redis nodes, and each pod only receives 1/10th of the messages. Each viewer sees all messages because the pod they are connected to receives its shard's messages, but wait — viewers on different pods would see different subsets of messages.

Actually, I need to reconsider. For chat, viewers want to see all messages, not a subset. So the sharding needs to happen at the gateway pod level: each pod subscribes to all 10 shards but processes them independently. The load reduction is on the Redis side (10 channels across 10 nodes instead of 1), not on the pod side.

But there is a deeper problem: 10M viewers receiving 5 messages/second = 50M message deliveries per second across all pods. Each pod handles 10K viewers and receives 5 messages/second, delivering 50K WebSocket frames/second. At a conservative 10 microseconds per frame, that is 500ms worth of CPU per second — 50% CPU utilization just on chat delivery, before any other work. Fix: rate-limit chat for large streams (slow-mode: 1 message per 30 seconds per user), and consider dropping to a sampling model (show a representative sample of messages for streams > 100K viewers — Twitch does this). The key insight is that at 10M viewers, chat is no longer individually meaningful — it is ambient scrolling."

**Q2: "An interviewer asks: 'I want to add a leaderboard to the Stock Ticker — show the top 10 most actively traded symbols in real time.' How do you design it?"**

Reason aloud:

"A leaderboard needs a ranked, dynamically updating count. Redis sorted set is the natural fit: `ZADD leaderboard:top_symbols {trade_count} {symbol}` and `ZREVRANGE leaderboard:top_symbols 0 9 WITHSCORES` for top 10.

The update mechanism: every time a trade occurs for a symbol (which I already have in the Kafka market data stream), a consumer increments that symbol's count in the sorted set. At 5M messages/s peak, a large fraction are trades. If 10% are trades (500K trades/s), and I update Redis on every trade, that is 500K ZINCRBY operations per second on a single sorted set. Redis is single-threaded; 500K ops/second on a single key is at Redis's throughput limit.

Optimization: batch the updates. Instead of one ZINCRBY per trade, maintain an in-memory counter map `symbol → trade_count_since_last_flush` and flush to Redis every 1 second with a Lua script that does multiple ZINGRBYs atomically. This reduces Redis ops from 500K/s to 10K/s (1 flush per second × 10K symbols).

The leaderboard does not need sub-second accuracy. Refresh every 5 seconds. So a Redis read of the top 10 sorted set every 5 seconds on the REST API side is trivial. WebSocket push for the leaderboard updates: publish the new top 10 to Redis pub/sub every 5 seconds; WebSocket servers forward to subscribed clients.

One edge case: at market open, every symbol trades simultaneously. The sorted set hot key could be overwhelmed. Mitigation: partition by first letter (`leaderboard:A-F`, `leaderboard:G-M`, `leaderboard:N-Z`) across multiple Redis nodes, merge the top-N from each shard at read time."

**Q3: "The head of engineering wants to add a 'replay any match' feature to the game backend. What breaks, what is already there, and what do you add?"**

Reason aloud:

"What is already there: Kafka has all game events (kills, damage, shots) partitioned by match_id. Match metadata is in PostgreSQL. Player inputs are... not retained. The game server receives inputs, processes them, and discards them. The in-memory game state snapshots are not persisted — they live only in the pod during the match.

What breaks: you cannot replay a match from inputs alone (deterministic simulation depends on the exact physics engine state at tick 0, which varies with server hardware, floating-point rounding, and game version). You cannot replay from the Kafka events alone (events are outcomes, not inputs — you cannot reconstruct moment-to-moment positions from kill events).

What you need to add: a **replay persistence system**. During a match, the game server writes full state snapshots to S3 every 1-second interval (64 ticks × snapshot = ~500 KB per second × 30-minute match = ~900 MB per match) plus the input log (60 bytes/input × 16 players × 64 Hz × 30 min = ~170 MB per match). That is ~1 GB per match. At 347 sessions/s, opt-in replay storage would be ~100 GB/s if all matches are stored — clearly need opt-in (estimate 10% of matches).

The replay architecture: at match end, game server ships the state snapshot stream and input log to S3. A replay service plays back state snapshots at any speed, using the input log to interpolate between snapshots. Clients connect to the replay service the same way they connect to a live match — same UDP protocol, same state delta packets, just driven by recorded data instead of live inputs.

This is clean: the rest of the system is untouched, replay is a pure add-on consuming from S3. The one complexity is game version compatibility — a match played on v1.0 may not replay correctly on v1.3 if physics changed. You need to ship the game server binary alongside the replay data, or maintain a game server version matrix."

**Q4: "The collaborative document editor is used by a legal firm. They require a full immutable audit trail of every character change, with the user who made it and the exact timestamp, for regulatory compliance. Your current Cassandra op log has a 90-day TTL. What changes?"**

Reason aloud:

"The current design uses Cassandra for the 90-day active operation log and S3 for periodic snapshots. Neither satisfies 'immutable forever with legal hold' requirements.

The change: add a separate compliance pipeline. Kafka already receives all operations. Add a new consumer group `compliance-archive` that reads from `doc-ops-{shard}` and writes to S3 in an immutable, WORM-compliant bucket (S3 Object Lock in Compliance Mode — cannot be deleted or modified even by bucket admins for the retention period). Every operation is written as an Avro record: `{op_id, doc_id, user_id, op_type, op_data_bytes, client_ts, server_ts, ip_address, session_id}`.

The immutability requirement: S3 Object Lock Compliance Mode with `governance-mode=compliance` and `retention-period=7 years`. Objects cannot be deleted even by the root account during the retention period. This satisfies SEC Rule 17a-4 equivalent requirements (same pattern used in the Stock Ticker for the feed audit log).

The query requirement: compliance teams need to search the audit trail by user or document. Raw S3 files are not queryable. Add Apache Iceberg metadata over the S3 files — allows Athena/Trino queries like `SELECT * FROM ops WHERE doc_id = X AND user_id = Y AND server_ts BETWEEN T1 AND T2`. Partition by (year, month) for efficient pruning.

Performance impact: the compliance consumer is entirely off the critical path. It reads from Kafka asynchronously. The only change to the live path is that Kafka retention for `doc-ops-{shard}` must be extended from the current setting to at least 7 days (to give the compliance consumer time to drain without pressure)."

**Q5: "If you had to re-architect the stock ticker to support sub-100 microsecond P99 end-to-end latency instead of 50ms, what changes?"**

Reason aloud:

"Current P99 is 50ms. Target is sub-100 microseconds. That is a 500× improvement. Let me think about where latency lives today.

Current critical path: feed handler (DPDK, ~10 µs) → Kafka publish (~2 ms) → WebSocket consumer processes Kafka → fan-out to client (~10 ms). The bottleneck is Kafka at ~2ms, and then network to client at 10+ ms.

To get to 100µs end-to-end, I have to eliminate Kafka from the hot path and co-locate the entire pipeline with the exchange feed.

Architecture changes:
1. **Remove Kafka from the hot path.** Use a lock-free in-process queue (LMAX Disruptor / Aeron) between feed handler and the WebSocket push engine. Aeron IPC is ~300 nanoseconds per message. Kafka is out.
2. **Co-location with kernel bypass.** The WebSocket push engine must also run at the same co-location facility as the exchange, not in a cloud region 30ms away. Client connection is a dedicated leased-line, not the public internet.
3. **Protocol change for delivery.** WebSocket has TCP overhead (ack, congestion control). Switch to UDP multicast with application-layer sequencing. The client receives UDP multicast — like the exchange delivers to the feed handler. This eliminates per-connection overhead.
4. **Client is co-located too.** Sub-100 µs end-to-end is only achievable if the client (an algorithmic trading system) is also in the same data center. A retail user on a home internet connection will never see < 1ms.
5. **Remove all thread context switches.** The single DPDK thread that receives from the exchange directly calls the push function. No OS scheduler involvement.

What I keep: the CRDT/Cassandra/S3 durability pipeline remains, but as an async consumer on a separate thread. The sub-100µs path is a separate data path: exchange → DPDK ring buffer → Aeron IPC → multicast to co-located clients. The durable archive path reads from Aeron in parallel without blocking the hot path."

---

## STEP 7 — MNEMONICS

### The WAKES mnemonic for the 5 critical realtime design decisions:

**W — Write path vs. Read path separation:** Define each separately. Write path is producer → Kafka → state update. Read path is Redis cache → REST API or WebSocket push. Never conflate them.

**A — Authority:** Who is the single source of truth for each entity's state? For stocks: the exchange is authoritative for prices, your system is authoritative for normalized NBBO. For games: the game server pod is authoritative for all match state. For docs: the session server holds authoritative CRDT state. For streaming: S3 is authoritative for segment content, Playlist Service is authoritative for manifest state.

**K — Kafka (durable backbone):** Kafka is always present. It decouples producers from consumers, enables fan-out to multiple consumers, provides durability and replay. Everything downstream can be rebuilt from Kafka. This is what makes Redis a cache instead of a database.

**E — Entity-level isolation (single-writer semantics):** One process owns one entity. One market data processor per symbol. One session server per document. One game pod per match. This eliminates distributed locking and enables lock-free, cache-local computation.

**S — State externalized for recovery:** Any in-memory state must be reconstructible from durable storage. Connection state in Redis, match state in S3+Postgres, CRDT state in S3+Cassandra. Pod death must be survivable without data loss.

### Opening one-liner for any realtime interview:

"Before I start drawing, I want to frame this as a fan-out problem: every state change event has a producer and N subscribers, and the core engineering challenge is delivering to all N subscribers before the world changes again. My architecture will always have four pieces: an event bus for durability and decoupling (Kafka), in-memory state for the hot read path (Redis), persistent connections for push delivery (WebSocket or UDP), and a durable log for recovery (Cassandra or S3). Everything else is domain-specific. Can I confirm the latency target first?"

---

## STEP 8 — CRITIQUE OF THE SOURCE MATERIAL

### Well-covered areas:

The source material is exceptionally thorough on the technical mechanics of each system. The order book data structure (HashMap + TreeMap, fixed-point prices), the CRDT algorithm (YATA with tombstones, integrate() pseudocode), the lag compensation formula (`rewind_ticks = min(latency_ms / tick_interval_ms, 64)`), and the HLS pipeline (FFmpeg command, segment upload hook) are all at the level of depth needed to impress at Staff+ interviews. The capacity estimation tables are complete and derive correctly from first principles. The database choice rationale tables (comparing Redis vs. TimescaleDB vs. Iceberg vs. DynamoDB) are excellent for showing trade-off reasoning.

### Missing or shallow areas:

**1. Multi-region / global failover** is mentioned but not designed. How does a stock ticker fail over to a backup data center when NYSE co-location loses power? The source mentions "multi-DC replication" for Cassandra but does not address: how do clients get routed to the backup region? How does the Kafka cluster handle cross-region replication (MirrorMaker 2)? How do you handle the case where both regions are up but disagree on sequence numbers?

**2. Security and data entitlements** are mentioned (API key, data licensing) but not deeply designed. A real market data platform has tiered data entitlements: delayed vs. real-time data, Level 1 vs. Level 2 (order book), and regulatory restrictions (Bloomberg cannot be re-distributed). The WebSocket subscribe action should check entitlement before adding the connection to the fan-out list. This middleware layer is described but not shown architecturally.

**3. Cost** is never discussed. At 2,000 GPU instances for live streaming or 14.7 PB for the tick archive, the cost implications are enormous. Senior engineers are expected to know that GPU instances are ~$5/hr each (2,000 × $5 = $10K/hour at peak), that S3 at $0.023/GB/month means 14.7 PB = ~$340K/month in storage alone, and to discuss cost optimization strategies (spot instances for transcoders, S3 lifecycle to Glacier for old ticks).

**4. Testing and observability** is entirely absent. How do you load test a WebSocket server handling 500K connections? How do you monitor that your Kafka consumer lag is not growing? What SLI/SLO metrics do you define? Interviewers at senior level increasingly ask "how would you know if this is working in production?"

**5. Gradual rollout / dark launch** for a live system change is not covered. If you change the CRDT algorithm, how do you roll it out to 10M concurrent editing sessions without data loss?

### Senior probes that would catch a memorizer:

1. "You said Redis pub/sub for chat fan-out. What is the maximum throughput of a single Redis pub/sub channel, and how do you measure when you've hit it?"
2. "Your game server pod has 313 GB of RAM across 625K sessions. What is your memory per session and what does it contain?"
3. "You're using Iceberg on S3 for tick data. A regulator asks for all AAPL trades between 9:30:00.000 and 9:30:00.050 on a specific date. Walk me through the query execution path."
4. "A user reports that their edits sometimes appear on their screen, disappear for a second, then reappear slightly differently. What is happening and how do you debug it?"
5. "Your live streaming platform needs to support ad insertion. A SCTE-35 splice point marker arrives in the ingest stream. How does this change your transcoder, Playlist Service, and CDN configuration?"

### Common traps interviewers set:

**Trap 1:** "Just put everything in Kafka and it'll be fine." Kafka is not a database. It is not queryable. REST APIs that need current price cannot run a Kafka range scan in < 50ms. Always have Redis as the read cache in front of Kafka.

**Trap 2:** "Use consistent hashing so every request hits the right server." Consistent hashing solves routing but not failover. When a session server pod crashes, consistent hashing still routes to its slot — you need a handoff protocol to rebuild state on the new pod at that slot. Know how you rebuild.

**Trap 3:** "The CRDT guarantees convergence, so there are no conflicts." CRDTs guarantee convergence to the same state, but that state might not be what any user intended. If User A types "cat" and User B types "dog" at the same position simultaneously, the CRDT might converge to "catdog" or "dogcat" — technically consistent but confusing to both users. This is intentional and correct — the CRDT's job is convergence, not semantic intent preservation. Know this limitation when asked.

**Trap 4:** "Just scale horizontally." You cannot horizontally scale a stateful component without understanding the state partitioning strategy. A second game server pod does not help if the match is pinned to the first pod. A second CRDT session server does not help if all clients for a document must connect to the same pod. State partitioning is the hard problem.

---

*This guide covers all 4 problems in Pattern 18, all 7 shared components, 3 deep-dive areas, a 15-question Q&A bank across 3 tiers, and a critique identifying gaps and senior-level traps. It is designed to be fully self-contained — read this once before your interview and you will be prepared for any variant of this pattern class.*
