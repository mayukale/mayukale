# Problem-Specific Design — Messaging (03_messaging)

## WhatsApp

### Unique Functional Requirements
- End-to-end encrypted (E2EE) messages: server never has access to plaintext
- Asynchronous key exchange using pre-key bundles (X3DH protocol) — keys must be available even when recipient is offline
- Three-state delivery receipts: sent (✓), delivered (✓✓), read (✓✓ blue)
- Typing indicators per conversation
- "Delete for everyone" within a configurable time window
- Media encrypted before upload; server stores only ciphertext
- Group limit: 1,024 members
- Offline message queue retained for 30 days

### Unique Components / Services
- **Key Service**: stores Signal Protocol pre-key bundles (one-time pre-keys for X3DH); atomically consumed and replenished; HTTP REST access
- **Signal Protocol stack**: X3DH for async key agreement; Double Ratchet Algorithm for per-message key derivation; Group E2EE via Sender Keys (O(1) encryption per group message regardless of group size)
- **Media Gateway**: chunked encrypted media upload; content-addressed by SHA-256(ciphertext) for server-side deduplication without decryption
- **Write-Ahead Log (WAL) on gateway SSD**: gateway persists message to local SSD before async Cassandra write; retry on crash

### Unique Data Models
- **messages** (Cassandra): partition key = `conversation_id`; clustering key = `(bucket, message_id DESC)` where bucket = `floor(unix_ts / 86400)` (daily); stores message metadata and ciphertext (not plaintext)
- **message_receipts** (Cassandra): partition key = `conversation_id`; clustering key = `(message_id, user_id)`; tracks SENT/DELIVERED/READ per recipient
- **one_time_pre_keys** (Cassandra): pre-key bundles for Signal Protocol; atomically consumed
- **users** (PostgreSQL): user_id (UUID), phone_number (E.164 format), identity_key, created_at
- **conversation_participants** (PostgreSQL): conversation_id → user_id mapping
- **Offline queue**: Redis Sorted Set `offline_queue:{user_id}`, score = enqueue_unix_timestamp, value = base64(envelope), 30-day TTL; drained on reconnect via ZRANGEBYSCORE

### Unique Scalability / Design Decisions
- **Content-addressed media deduplication without decryption**: SHA-256(ciphertext) as S3 key; if two users send the same encrypted blob, server deduplicates by hash — works for forwarded media but not the same plaintext encrypted differently
- **Sender Keys for group E2EE**: each group sender generates a Sender Key shared with all members; encrypts message once (O(1)) instead of once per member (O(N)); compromise of Sender Key exposes past messages until re-keyed (known trade-off)
- **Cassandra cluster size**: 1,350 physical nodes across 3 DCs (RF=3) to handle 3.5M writes/sec
- **PostgreSQL sharded by user_id % 64**: Patroni-managed HA with 30s failover; PgBouncer for connection pooling
- **Consistent hashing on user_id for gateway assignment**: minimizes WebSocket reconnections during gateway scale-out events
- **"Safety numbers"**: 60-digit security codes allowing users to verify key authenticity out-of-band; guards against MITM at Key Service

### Key Differentiator
WhatsApp's defining constraint is **end-to-end encryption**: the server stores only ciphertext, making traditional server-side features (search, moderation, analytics) impossible and requiring a dedicated key distribution infrastructure (Signal Protocol pre-key bundles, Sender Keys) that no other problem in this folder needs.

---

## Slack

### Unique Functional Requirements
- Workspace/organization model: all data isolated by workspace_id; users belong to multiple workspaces
- Channel-based messaging: public and private channels within a workspace
- Message threading: replies form a sub-conversation under any message; thread participants notified separately
- Full-text message search across all workspace history
- Slash commands and app integrations (OAuth 2.0 app installation, incoming/outgoing webhooks)
- Message retention policies configurable by workspace admin (30 days, 90 days, 1 year, forever)
- File uploads up to 1 GB with preview generation (images, PDFs, code files)
- Enterprise Key Management (EKM): customer-managed KMS keys for at-rest encryption
- Compliance export: immutable WORM S3 Object Lock with 7-year retention for regulated industries

### Unique Components / Services
- **MySQL (InnoDB) sharded by workspace_id**: primary message store (not Cassandra); chosen for ACID transactions and complex queries within a workspace; Vitess for online resharding (64 → 256 shards)
- **PostgreSQL (global, unsharded)**: global workspaces directory; shard routing table; app registrations
- **Elasticsearch per-workspace index** (`messages_{workspace_id}`): BM25 relevance + recency decay; access control enforced via `member_ids` embedded in each indexed document; Index Lifecycle Management (hot → warm → cold → delete)
- **Redis Pub/Sub for large channels**: for channels > 10,000 members, Fan-Out publishes to Redis channel `ws_channel:{channel_id}`; each Gateway subscribes and fans out to its own connections — O(num_gateways) instead of O(members)
- **Retention/Compliance Service**: applies workspace retention policies; exports to WORM S3 Object Lock
- **Integration/Webhook Service**: outgoing webhooks, slash command dispatch (3-second timeout, async response_url up to 30 min)

### Unique Data Models
- **messages** (MySQL): message_id, workspace_id, channel_id, user_id, `client_msg_id` (UNIQUE idempotency key), `ts` (BIGINT microseconds — Slack's own timestamp format e.g., "1712620800.123456"), text, thread_ts (nullable, links reply to parent), reply_count, file_ids (JSON), edited_at, deleted_at (soft delete)
- **thread_summaries** (MySQL): (workspace_id, channel_id, thread_ts) → reply_count, latest_reply_ts, participant_ids (JSON), follower_ids (JSON)
- **Elasticsearch document**: includes `member_ids` (array) for in-index access control filtering; avoids DB lookup per search result
- **files** (MySQL): file_id, workspace_id, uploader_id, filename, content_type, size_bytes, storage_key (S3), thumbnail_key, sha256, expires_at
- **workspace_members**: role, notification_preferences_json; shard-local to workspace shard
- **Shard routing table** (PostgreSQL): workspace_id → shard_id mapping for all queries
- **Offline queue**: Redis Sorted Set `offline:{user_id}:{workspace_id}` (separate per workspace for isolation)

### Unique Scalability / Design Decisions
- **Workspace-as-shard-key**: co-locates all workspace data on one MySQL shard; cross-workspace queries do not exist by design; enables strong ACID consistency within a workspace
- **Thread write coalescing**: Redis HINCRBY for thread reply counts + flush every 5 seconds; reduces MySQL write frequency by ~500× for hot threads
- **Per-workspace Elasticsearch index**: isolation between workspaces; simplifies access control; large workspaces get dedicated Elasticsearch clusters
- **Elasticsearch ILM**: hot (0–30 days, SSD) → warm (30–90 days, HDD) → cold (90+ days, searchable snapshots on S3) → delete; balances search performance with storage cost
- **Slack's `ts` field**: string-encoded Unix timestamp with microsecond precision; serves as both unique ID and chronological sort key; avoids separate ID generator
- **EKM + compliance export**: Slack encrypts at-rest with workspace-specific KMS keys; compliance export to gzipped, WORM S3 with auditor read access — no other problem in this folder has this requirement

### Key Differentiator
Slack's defining architectural choice is **workspace-as-shard-unit with MySQL**: all messages, channels, and members within a workspace live on one shard, enabling full ACID transactions and complex SQL queries (threading, reactions, file references) without cross-shard joins — at the cost of requiring Vitess for online resharding and accepting that one very large workspace can saturate a shard.

---

## Discord

### Unique Functional Requirements
- Server (Guild) model with hierarchical channels, roles, and a granular permission system (allow/deny bitmasks per role and per channel override)
- Voice channels: users join and talk in real-time using WebRTC (UDP/DTLS-SRTP)
- Video and screen sharing ("Go Live") for Nitro subscribers
- Mega-server support: up to 500,000–1,000,000 members per server
- Bot ecosystem: bots connect via Gateway API with shard-based guild assignment; slash commands via Interactions Endpoint URL
- Stage channels: "speaker + audience" model with cascaded SFU architecture for up to 25,000 viewers
- Forum channels: channel type where messages are organized as posts with replies
- No E2EE: server reads all messages to enable moderation and search

### Unique Components / Services
- **ScyllaDB** (Cassandra-compatible): chosen specifically over Cassandra for 10× lower tail latency (C++ shard-per-core architecture, no JVM GC pauses); TimeWindowCompactionStrategy (TWCS) for immutable old buckets
- **Voice Server (WebRTC SFU)**: regional media relay; Mediasoup/Janus-based; Opus codec (64 Kbps, 20ms frames, FEC, DTX); ICE + STUN + TURN for NAT traversal
- **Cascaded SFU**: Root SFU + Leaf SFUs for Stage channels; scales to 25K viewers; root forwards to leaf, leaves forward to viewers
- **Permissions Cache**: Redis-backed per-user-per-channel bitmask; version-based invalidation using `role_version` counter instead of explicit DEL storms
- **Lazy Member Loading**: mega-guild member list not sent on READY payload; client requests ranges progressively via OP 14; prevents MB-sized READY payloads

### Unique Data Models
- **messages** (ScyllaDB CQL): PRIMARY KEY `((channel_id, bucket), message_id)`; bucket = `floor(message_id_epoch_ms / (1000×60×60×24×7))` (weekly); `referenced_msg_id` for replies; `mention_roles`, `embeds`, `attachments` as JSON
- **reactions** (ScyllaDB CQL): PRIMARY KEY `(channel_id, message_id, emoji_id, emoji_name, user_id)`
- **permission_overwrites** (PostgreSQL): `(channel_id, overwrite_id)` → `overwrite_type` (0=role, 1=member), `allow_bits`, `deny_bits`
- **roles** (PostgreSQL): role_id, guild_id, name, color, permissions (bitmask), position, hoist
- **voice_states** (Redis Hash): key `voice_state:{guild_id}:{user_id}` → channel_id, session_id, self_mute, self_deaf, server_mute, self_stream; TTL 90s
- **guild_online** (Redis Sorted Set): `guild_online:{guild_id}` for online member count and fan-out targeting
- **channel_subscribers** (Redis Set): `channel_subscribers:{channel_id}` for mega-guild permission-aware fan-out
- **Discord Snowflake epoch**: January 1, 2015 00:00:00 UTC (1420070400000 ms)

### Unique Scalability / Design Decisions
- **Permission bitmask evaluation algorithm**: @everyone role → OR all member roles → admin bypass → channel-level overwrites; O(num_roles) computation, cached as final bitmask per (user, channel) pair in Redis; version-based invalidation avoids cache stampede on role changes
- **Mega-guild fan-out (> 100K members)**: instead of resolving which gateways hold members (impossible at 500K members), push event to ALL gateway servers and let each gateway filter against its in-memory connection map; effectively broadcasts to 500 gateways
- **Bot gateway sharding**: bot divides guild_id % num_shards == shard_id to partition guild event processing across bot instances; Discord enforces this at the gateway level
- **Weekly bucket for messages**: old buckets become immutable → TWCS efficiently compacts them; no tombstone buildup; new writes land in current week's bucket only
- **ScyllaDB QUORUM writes + LOCAL_ONE reads**: 2/3 replicas acked for writes; single replica suffices for reads (acceptable staleness for message history)
- **Voice server selection**: 20+ regional voice server pools; selected by user's geographic region + real-time load; users in same server may be on different voice servers → SFU mixes streams

### Key Differentiator
Discord's defining complexity is **the guild permission system at mega-server scale**: a single event in a 500K-member server requires computing per-channel read permissions for every recipient before delivery, driving the version-based permission bitmask cache, the lazy member loading protocol, and the push-to-all-gateways fan-out strategy — combined with a unique WebRTC voice layer that no other problem in this folder has.

---

## Live Comments

### Unique Functional Requirements
- Stream-based model: comments belong to a live stream, not a persistent conversation; stream ends and becomes a VOD
- Fan-out to up to 500,000 simultaneous viewers of a single stream
- Slow mode: per-stream minimum interval between comments from a single user (Redis SET NX)
- Subscriber-only and emote-only modes
- Paid highlights (Super Chats): color-coded, pinned-duration messages tied to payment transaction
- Automated hate speech filtering (ML-based) with trust-tier fast-tracking
- Comment history: ~150 most recent comments shown on join; synchronized with VOD replay timestamps
- Moderation actions: delete comment, ban user, timeout user — must propagate to all viewers within 500ms
- Channel-specific emotes and badges
- User mentions (@username)

### Unique Components / Services
- **ML AutoMod Pipeline**: Layered — Trust Tier fast-track (trusted users bypass ML, ~70% of traffic) → Bloom filter + regex (blocklist, l33tspeak normalization) → ML classifier (fine-tuned DeBERTa/RoBERTa, 125M parameters) on **Triton Inference Server** (NVIDIA, dynamic batching)
- **Chat Receive Service (CRS)**: specialized WebSocket tier that holds viewer connections per stream; differs from general gateway because it routes by stream_id, not user_id
- **Viewer Set Registry** (Redis Set): stores the set of CRS server IDs currently handling viewers of a given stream (not individual user IDs); Fan-Out reads this to know which CRS servers to notify — O(num_CRS_servers) fan-out, not O(viewers)
- **Comment Ingest Service**: HTTP POST endpoint for comment submission; validates slow mode, ban, rate limits; publishes to Kafka `raw_comments`
- **Stream Registry** (Redis): single source of truth for stream state metadata (is_live, config: slow_mode_seconds, subscriber_only, emote_only)
- **Gift Service**: processes Super Chat gift events during Live streams; accumulates counts in Redis; triggers real-time visual effects

### Unique Data Models
- **streams** (PostgreSQL/MySQL): stream_id, channel_id, title, started_at, ended_at, is_live, game_id, language
- **stream_chat_config** (PostgreSQL/MySQL): stream_id → slow_mode_seconds, subscriber_only, emote_only, follower_only_minutes, unique_chat_mode, pinned_comment_id
- **comments** (Cassandra): PRIMARY KEY `((stream_id, bucket), comment_id)`; bucket = `floor(comment_id_epoch_ms / (1000×3600))` (hourly); includes user_badges, user_color, emotes, is_deleted, comment_type
- **super_chats** (PostgreSQL/MySQL): super_chat_id, comment_id, stream_id, user_id, amount_cents, currency, display_tier, display_duration_s, payment_reference (idempotency key from payment processor)
- **user_bans** (PostgreSQL/MySQL): stream_id, channel_id, user_id, ban_type, timeout_until, reason
- **recent_comments** (Redis List): `recent_comments:{stream_id}` → last 150 comment JSON blobs; 18 GB total for 60K concurrent streams; served immediately on viewer join without Cassandra query
- **slow_mode lock** (Redis): `slowmode:{stream_id}:{user_id}` → SET NX with TTL = slow_mode_seconds; O(1) atomic check-and-set
- **ban_cache** (Redis): Bloom filter (false positives check Redis SET for confirmation) + Redis SET for confirmed bans

### Unique Scalability / Design Decisions
- **Viewer-set-based fan-out (not user-based)**: Redis Set holds CRS server IDs (not viewer user IDs); Fan-Out sends one gRPC per CRS server containing the full comment payload; each CRS fans out to its own WebSocket connections for that stream_id — decouples fan-out cost from viewer count
- **Trust-tier fast-tracking**: ~70% of comments from verified/trusted accounts bypass the ML classifier entirely; reduces GPU inference cost by ~10×
- **Fail-open circuit breakers**: if Redis slow mode check fails, allow the comment through; if ML inference times out, allow the comment through (availability > correctness for real-time chat)
- **Ring buffer per CRS connection**: slow-reading clients drop oldest comments instead of blocking the entire stream; prevents head-of-line blocking
- **Async post-delivery moderation**: comments are shown to viewers first, then scanned; moderation action (delete/ban) propagates within 2 seconds — accepted as sufficient for live streams
- **Kafka `raw_comments` → `approved_comments` pipeline**: raw comments published immediately; ML classifier consumes raw_comments and publishes to approved_comments only after passing; Fan-Out consumes approved_comments so viewers only see approved content
- **Hourly Cassandra buckets**: live streams generate very high write rates; hourly buckets prevent single partitions from growing unboundedly
- **Separate Kafka topics per event type**: `raw_comments`, `moderation_actions`, `super_chats` are separate topics; moderation actions consumed by a dedicated service that invalidates fan-out for deleted comments
- **VOD replay comment synchronization**: comments stored with Snowflake IDs (timestamp-embedded); during VOD playback, client fetches comments for a time range from Cassandra via `(stream_id, bucket, comment_id)` range scan synchronized to video seek position

### Key Differentiator
Live Comments is fundamentally different from the other three because the fan-out is **stream-centric, not user-centric**: a single stream has 500K viewers who are all receiving the same comment (no personalization, no routing by user), requiring the viewer-set-based architecture where fan-out cost is O(CRS servers) rather than O(viewers) — and adding a unique ML moderation layer (Triton + DeBERTa) that the other messaging systems do not have.
