# Common Patterns — Messaging (03_messaging)

## Common Components

### WebSocket Gateway Servers
- Used in all four problems as the real-time delivery layer; each gateway server holds thousands of persistent connections
- In whatsapp: ~10,000 WebSocket servers, 45K connections each (450M peak concurrent); consistent-hashed on user_id to reduce reconnect storms
- In slack: ~500 gateway servers, 20K connections each (10M concurrent); sticky load balancing
- In discord: ~500 gateway servers, 20K connections each (10M concurrent); sticky routing via Cloudflare Anycast + NLB
- In live_comments: Chat Receive Service (CRS) tier; viewers assigned to CRS via `hash(user_id) % num_CRS_servers`; SSE provided as fallback

### Fan-Out Service
- All four problems have a dedicated service that reads from Kafka, resolves which gateway servers hold the target connections, and delivers messages in batch gRPC calls
- In whatsapp: Fan-Out Service with Go goroutine worker pool (10K goroutines); batches 1,024 group member deliveries into ~50-100 gRPC calls
- In slack: Channel Fan-out Service; for channels > 10,000 members uses Redis Pub/Sub so each Gateway subscribes and delivers locally
- In discord: Guild Fan-Out Service; tiered strategy (direct for small guilds, 16 sharded workers for large, push-to-all-gateways for mega-guilds)
- In live_comments: Fan-Out Service reads CRS server IDs from Redis viewer set, sends one batched gRPC per CRS (O(num_CRS_servers), not O(viewers))

### Redis Cluster
- Used in all four for ephemeral/hot-path state: presence, connection registry, offline queues, rate limiting
- In whatsapp: offline queue (Sorted Sets, 30-day TTL), presence (Hashes, source of truth), connection registry (`conn:{user_id}:{device_id}` → gateway_id, 90s TTL), group membership cache (5s TTL), deduplication (24h TTL)
- In slack: channel member cache (60s TTL), user profile (1h TTL), recent messages (5m TTL), unread counts (HASH), offline queue (Sorted Set), presence, rate limiting
- In discord: permissions bitmask cache (versioned key), guild metadata (5m TTL), online member Sorted Sets per guild, voice state (Hash, 90s TTL), presence (60s TTL)
- In live_comments: slow mode SET NX, viewer set (Set of CRS server IDs), recent comments List (last 150), ban cache, Bloom filter for word blocklists

### Kafka (Event Bus)
- Used in all four as the durable async backbone decoupling message ingestion from fan-out
- In whatsapp: overflow buffer during Fan-out overload; log pipeline; Kafka considered then rejected for per-group topics (doesn't scale)
- In slack: topics `messages`, `reactions`, `presence`, `thread_updates`, `notifications`, `webhooks`, `raw_messages`; partitioned by workspace_id
- In discord: topics `messages`, `reactions`, `presence`, `member_updates`, `voice_state_updates`, `thread_events`; plus retry topic for circuit breaker fallback
- In live_comments: topics `raw_comments`, `moderation_actions`, `super_chats`, `stream_events`, `approved_comments`; per-stream partition for ordering

### Connection Registry in Redis
- All three persistent-messaging systems (WhatsApp, Slack, Discord) store which gateway server each user is connected to
- Key format: `conn:{user_id}:{device_id}` → `gateway_server_id`
- TTL renewed via heartbeat (30s heartbeat → 90s TTL in WhatsApp; similar in Slack and Discord)
- Used by Fan-out and Message Router to locate the correct gateway for delivery

### Offline Queue (per-user)
- WhatsApp and Slack both use Redis Sorted Sets keyed by user_id to queue messages for offline recipients
- Score = enqueue timestamp; drain on reconnect via ZRANGEBYSCORE
- TTL: 30 days (WhatsApp), implicitly bounded (Slack)
- Live Comments uses Redis List of recent 150 comments for reconnecting viewers; Discord uses push notification to mobile

### Push Notification Service (APNs / FCM)
- All four fall back to APNs (iOS) or FCM (Android) when the target user is not connected via WebSocket
- Notification Service evaluates user preferences (DND, mute settings) before sending
- Present in: whatsapp, slack, discord, live_comments

### Object Storage (S3 / Equivalent) + CDN
- All four store media blobs in S3 or equivalent; CDN-fronted for downloads
- WhatsApp: S3/GCS; content-addressed by SHA-256(ciphertext); 30-day lifecycle expiry; presigned URLs
- Slack: Amazon S3; CloudFront-fronted with presigned download URLs; thumbnail cache 1h TTL
- Discord: Cloudflare R2 / S3; Cloudflare CDN for media, avatars, emojis, stickers
- Live Comments: S3 for deleted content archive (DMCA, 90-day retention); CDN for emote images and avatars

### gRPC (Internal Service Communication)
- WhatsApp: Message Router → Gateway delivery; Fan-Out → Gateway batch delivery
- Slack: Gateway → Fan-Out Service
- Discord: Guild Fan-Out → Gateway servers (batched pushes)
- Live Comments: Fan-Out Service → Chat Receive Services

### Idempotency / Deduplication
- All four assign a unique ID per message and deduplicate in Redis with a short TTL
- WhatsApp: 16-byte random `envelope_id`; Redis SET NX with 24h TTL
- Slack: client-generated `client_msg_id` (UUID); UNIQUE DB constraint `(workspace_id, client_msg_id)`
- Discord: nonce-based dedup in Redis (60s TTL)
- Live Comments: `client_nonce` (UUID) with 60s TTL dedup

### Snowflake IDs (or Equivalent Time-Ordered IDs)
- Discord and Live Comments use Snowflake IDs (64-bit, timestamp-embedded) for globally unique, time-ordered message IDs
- Slack uses server-assigned microsecond Unix timestamp (`ts` string) for ordering
- WhatsApp uses random 16-byte `envelope_id` (not time-ordered)

### OpenTelemetry (Distributed Tracing)
- All four mention OpenTelemetry with trace context propagated via gRPC metadata and HTTP headers
- 100% error sampling; low-rate success sampling (0.01–1%)
- Jaeger or Datadog APM as backend
- Structured JSON logging with trace_id, user identifiers (often hashed for privacy)

## Common Databases

### Message Store (Cassandra / ScyllaDB)
- WhatsApp uses Cassandra; Discord uses ScyllaDB (Cassandra-compatible); Live Comments uses Cassandra
- Common pattern: partition key is the conversation/channel/stream ID; clustering key is (bucket, message_id DESC) with time-based buckets to bound partition growth
- RF=3 across 3 AZs; LOCAL_QUORUM writes; LOCAL_ONE reads
- TimeWindowCompactionStrategy (TWCS) for time-series append-only workloads
- Slack is the exception: uses MySQL sharded by workspace_id instead of Cassandra

### Redis (see Common Components above)

## Common Queues / Event Streams

### Kafka
- See Common Components above; RF=3 universally
- At-least-once delivery semantics; consumers must handle idempotency
- Consumer lag monitored; Fan-Out Service auto-scales on Kafka consumer lag

## Common Communication Patterns

### WebSocket for Real-Time Delivery + REST for CRUD
- All four use WebSocket for real-time delivery and REST (HTTPS) for create/update/delete operations
- Server-Sent Events (SSE) offered as a read-only fallback in Slack and Live Comments

### Heartbeat-Based Presence
- All four track user presence via a heartbeat from the client, updating a Redis entry with a TTL
- If the heartbeat stops (user disconnects), Redis TTL auto-expires the presence record (no explicit disconnect event needed)
- Presence allowed to be up to 30–60 seconds stale

### Per-Channel/Conversation Ordered Delivery
- All four guarantee monotonic message ordering within a conversation/channel/stream
- Implemented via server-assigned IDs (Snowflake or microsecond timestamps) that encode time in high bits
- Cassandra/ScyllaDB clustering key (bucket, message_id DESC) provides efficient time-ordered reads

### Tiered Fan-Out Strategy
- All four implement some form of tiered fan-out to handle rooms/groups of varying sizes
- Small groups: direct gRPC to each gateway holding a member connection
- Large groups/channels: switch to Redis Pub/Sub broadcast (Slack) or sharded parallel workers (Discord) or viewer-set-based routing (Live Comments)

### Fail-Open Circuit Breakers
- Live Comments explicitly uses fail-open patterns (allow messages through when Redis or ML inference fails)
- WhatsApp uses fail-safe (write-ahead log on gateway SSD for async retry)
- All four have circuit breakers between key service pairs with defined fallback behaviors

## Common Scalability Techniques

### Horizontal Stateless Fan-Out and Router Services
- Message Router and Fan-Out Service are stateless in all four; scale horizontally behind load balancer or via Kafka consumer groups
- Only the Gateway servers are stateful (hold WebSocket connections)

### Batching Deliveries per Gateway
- All four batch multiple messages into a single gRPC call to each gateway server
- WhatsApp: Redis pipeline of 300 ZADD commands in one round trip; gateway batches up to 50 WebSocket envelopes per frame
- Slack: batched gRPC per gateway grouped by channel members
- Discord: gRPC batch delivery per gateway with role/permission filter applied before dispatch
- Live Comments: one batched gRPC per CRS server

### Consistent Hashing for Gateway Affinity
- WhatsApp: consistent-hash on user_id for load balancer → gateway assignment (minimizes reconnection on scale-out)
- Discord: consistent-hash sticky routing; bot sharding uses guild_id % shard_count
- Live Comments: `hash(user_id) % num_CRS_servers` assignment

### Time-Bucketed Message Partitions
- WhatsApp: daily bucket `floor(unix_ts / 86400)` in Cassandra
- Discord: weekly bucket `floor(message_id_epoch_ms / (1000×60×60×24×7))` in ScyllaDB
- Live Comments: hourly bucket in Cassandra
- Prevents unbounded partition growth; old buckets become immutable and compact efficiently

## Common Deep Dive Questions

### How do you deliver a message to a user who is offline?
Answer: Enqueue the message in a Redis Sorted Set per user (score = enqueue timestamp) with a TTL (24h–30 days). On reconnect, drain the queue with ZRANGEBYSCORE and deliver in bulk. Simultaneously send an APNs/FCM push notification so the user opens the app. WhatsApp queues up to 30 days; Slack queues until next session.
Present in: whatsapp, slack, discord (push notification path), live_comments (last-150 Redis List)

### How do you route a message to the correct WebSocket gateway server?
Answer: Maintain a connection registry in Redis: key `conn:{user_id}:{device_id}` → gateway_server_id, TTL = 1.5× heartbeat interval. Fan-Out Service reads the registry, groups recipients by gateway, and sends one batched gRPC per gateway. If no entry exists, user is offline → enqueue.
Present in: whatsapp, slack, discord, live_comments

### How do you handle fan-out to very large groups or channels?
Answer: Tiered strategy. For small groups: look up member list, query registry, batch gRPC to each gateway. For large groups (> ~10K): publish to a Redis Pub/Sub channel keyed by channel_id; all gateway servers that have any subscriber locally handle delivery themselves. For mega-groups: push event to all gateways and let each filter locally against their in-memory connection map.
Present in: whatsapp, slack, discord, live_comments

### How do you prevent duplicate message delivery on retry?
Answer: Client generates a unique dedup key (envelope_id, client_msg_id, nonce). Server checks `SET dedup:{id} 1 EX <TTL> NX`; if key exists, silently drop. DB insert uses UNIQUE constraint or `ON CONFLICT DO NOTHING`. Kafka offset committed only after all downstream writes succeed so replay is safe.
Present in: whatsapp, slack, discord, live_comments

### How do you ensure per-channel message ordering?
Answer: Server assigns monotonically increasing IDs (Snowflake or microsecond timestamps); client clock is never trusted for ordering. Cassandra/ScyllaDB clustering key ensures DESC time order on reads. Kafka partition per conversation/channel ensures ordered ingest.
Present in: whatsapp, slack, discord, live_comments

### How do you track user presence efficiently at scale?
Answer: Each client sends a heartbeat every ~30 seconds. Server updates a Redis Hash per user with TTL = ~60–90 seconds. If the heartbeat stops, Redis auto-expires the key — no explicit disconnect event required. Presence staleness of 30–60 seconds is accepted. Typing indicators are transient WebSocket pushes, not persisted.
Present in: whatsapp, slack, discord, live_comments

## Common NFRs

- **Availability**: 99.99% across all four problems
- **Real-time delivery**: p99 latency 100ms (Discord) to 2s (Live Comments) for online recipients
- **At-least-once delivery**: no message loss; idempotent consumers handle duplicates
- **Per-channel/conversation ordering**: strong ordering within a conversation; eventual across conversations
- **Offline message queuing**: messages held for offline recipients and delivered on reconnect
- **Eventual consistency for presence and reactions**: stale presence (up to 60s) and approximate reaction counts are acceptable
- **Horizontal scalability**: all services except gateway are stateless; gateway scales by adding servers behind consistent-hash LB
