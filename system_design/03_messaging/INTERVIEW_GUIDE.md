# Pattern 3: Messaging — Interview Study Guide

Reading Pattern 3: Messaging — 4 problems, 11 shared components

---

## STEP 1 — PATTERN OVERVIEW

This pattern covers four canonical messaging design problems: **WhatsApp**, **Slack**, **Discord**, and **Live Comments** (YouTube/Twitch-style live chat). Every single one of these has appeared in real FAANG and NVIDIA system design interviews within the last two years. They look different on the surface, but they are all variations on the same underlying problem: get a message from one producer to N consumers in near-real-time, reliably, at scale.

**The 4 problems at a glance:**

| Problem | Scale (DAU/concurrent) | Key Differentiator | Hardest Part |
|---|---|---|---|
| WhatsApp | 1.5 B DAU, 450 M concurrent conns | End-to-end encryption (Signal Protocol) | E2EE with async key exchange, group fan-out at 3.5 M msg/s |
| Slack | 20 M DAU, 10 M concurrent conns | Workspace model, full-text search, integrations | MySQL-based message store, per-workspace search isolation, webhook ecosystem |
| Discord | 150 M MAU, 10 M concurrent conns | Guild permission system, mega-servers (500K members), WebRTC voice | Permission bitmask evaluation at fan-out time, lazy member loading |
| Live Comments | 30 M concurrent viewers | Stream-centric fan-out (not user-centric), ML hate-speech filtering | 500K-viewer fan-out per stream, async AutoMod pipeline |

**Shared components (11 total):** WebSocket Gateway, Fan-Out Service, Redis Cluster, Kafka Event Bus, Connection Registry, Offline Queue, Push Notification Service (APNs/FCM), Object Storage + CDN, gRPC for internal calls, Idempotency/Deduplication, Snowflake IDs (or equivalent).

---

## STEP 2 — MENTAL MODEL

### The Core Idea

Every messaging system is a **durable, ordered, fan-out delivery machine**. A message enters the system at one point, the system durably persists it, and then delivers it to a set of recipients as fast as possible. The core tension is: **you need writes to be fast** (message accepted quickly, sender gets an ack), **you need fan-out to be parallel** (all recipients notified concurrently), and **you need the system to be reliable** (no message is lost even if servers crash mid-delivery).

Think of it as a postal service that has to deliver 3.5 million letters per second, where most letters go to a handful of recipients (1:1 chat), but some go to thousands of people simultaneously (group chats, live streams), and you have to handle the case where a recipient is asleep and can't take delivery right now.

### The Real-World Analogy

Imagine a newsroom's wire service at peak breaking-news time. Reporters (senders) file stories. Editors (gateways) accept and route. The wire service (Kafka) durably records every story and distributes it to all subscriber newsrooms. Each newsroom (gateway server) handles its own local delivery to journalists (clients) who are online. Journalists who are on deadline elsewhere get a message slip (offline queue) to catch up when they return. An editor on a very large story (group message to 1,000 people) doesn't personally hand the story to each journalist — they post it on the main board and let each newsroom handle its own distribution (fan-out service).

### Why This Category is Hard

Three things make messaging disproportionately hard compared to other system design categories:

**1. Stateful real-time connections at massive scale.** You cannot just throw stateless HTTP behind a load balancer. WebSocket connections are persistent and stateful — each server "owns" a set of connected users. When you want to send a message to a user, you have to find which server they're connected to (connection registry), and route through it. This creates a routing problem that compounds at every level of scale.

**2. The fan-out amplification problem.** A single message in a 1,000-member group doesn't generate 1 write — it generates 1,000 delivery events. A single comment in a 500,000-viewer live stream generates half a million pushes. The "small object, many recipients" pattern amplifies write load by orders of magnitude and is the root cause of most production incidents in messaging systems.

**3. Ordering, deduplication, and delivery guarantees all at once.** Users expect messages to appear in the same order everyone else sees them. The system has to guarantee no message is lost even if the server crashes mid-delivery. And the retry logic that enables durability also risks delivering the same message twice — so you need idempotency at every layer. Getting all three right simultaneously, under heavy load, with competing failure modes, is genuinely difficult.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these four to five questions at the start. Every one of them changes the architecture in a meaningful way.

**Q1: "What's the delivery model — is this 1:1, group, or broadcast?"**
This determines your fan-out strategy. 1:1 is simple routing. Small groups (WhatsApp, Slack channels) use member-list-based fan-out. Large channels and live streams require tiered fan-out with Redis Pub/Sub or viewer-set routing. If they say "up to 500,000 simultaneous recipients for one message" (live stream), that's a completely different architecture than "up to 1,000 recipients in a group."

**Q2: "Is message delivery persistent or ephemeral? Do messages survive after delivery?"**
WhatsApp stores only undelivered messages on the server (delivered ones are on device). Slack retains messages for months to years (search, compliance). Live Comments retains for VOD replay. Persistence requirements determine your database choice and storage budget — the difference between Cassandra with 30-day TTL vs. MySQL with 7-year WORM compliance exports.

**Q3: "Is end-to-end encryption required?"**
This single requirement eliminates server-side search, server-side backup, server-side spam filtering, and forces a completely different key distribution architecture (Signal Protocol pre-key bundles). If they say yes, immediately scope out search, server-side moderation, and most analytics features.

**Q4: "What are the scale numbers — DAU, messages per day, and group/channel size?"**
Anchor your capacity estimation. WhatsApp: 100 B msg/day, 450 M concurrent connections. Slack: 800 M msg/day, 10 M concurrent connections. Discord: 4 B msg/day, 10 M concurrent connections. Live Comments: 50 K comment writes/sec, 30 M concurrent viewers. If the interviewer says "think of it as a WhatsApp competitor," those are the numbers you use.

**Q5: "Is there a content moderation requirement?"**
If yes, ask whether it needs to be synchronous (blocking delivery) or asynchronous. Synchronous moderation at 50 K/sec requires hundreds of GPU inference servers and adds 200-500 ms latency to every message — that's a product-defining trade-off. Async post-delivery moderation is the standard live-chat choice but briefly exposes bad content to viewers.

**Red flags to watch for:**
- If you skip clarifying questions and immediately jump to "I'd use WebSocket and Kafka," the interviewer will ask "but what if it's E2EE?" and you'll have to restart. Spend two minutes on clarification — it saves twenty.
- If you don't ask about scale, you can't differentiate between a 50 M user system and a 2 B user system, and the numbers will be off by orders of magnitude.
- If you don't ask about persistence, you might design a durable message store for a system that only needs a 30-minute ephemeral buffer.

---

### 3b. Functional Requirements

State your functional requirements explicitly before drawing anything. This is how you prove to the interviewer you understand the problem's scope.

**For a WhatsApp-style system:**
- 1:1 and group messaging (groups up to 1,024 members)
- Delivery receipts (sent, delivered, read — three tiers)
- Offline message queuing (messages queued for 30 days)
- End-to-end encryption (Signal Protocol; server never sees plaintext)
- Media sharing (images, video, audio, documents) — encrypted before upload
- Presence (online/offline, typing indicators)
- Push notifications (APNs/FCM for offline users)
- Message deletion ("delete for everyone" within time window)
- Explicitly out of scope: voice/video calls, Payments, Status stories

**For a Slack-style system:**
- Workspace/channel model (org → channels → messages)
- Real-time delivery to all online channel members
- Message threading (replies under a parent message)
- Full-text search across workspace history
- Slash commands and third-party integrations
- Message retention policies (configurable by admin)
- File sharing up to 1 GB with preview generation
- Reactions, mentions, presence
- Explicitly out of scope: voice/video (Huddles), AI summarization, SCIM/SSO details

**Scope decision that matters most:** For WhatsApp, scope out search upfront if E2EE is in scope — the interviewer should acknowledge this trade-off. For Slack, the message search requirement alone is a major architectural component (Elasticsearch cluster per workspace). For Live Comments, scope out video stream delivery — that's a separate CDN problem.

**How to state requirements clearly in the interview:** "I'll focus on the core messaging flow — sending, delivering, queuing for offline users, and delivery receipts. I'll note where E2EE changes things but won't deep-dive the Signal Protocol unless you want to. Search is a major addition; let me note it here and revisit if we have time." This signals scope control, which is a senior-level behavior.

---

### 3c. Non-Functional Requirements

Don't just list NFRs — derive them from the problem and state the trade-off each one implies.

**Availability: 99.99% (≤ 52 min/year downtime)**
Implies: multi-region active-active deployment, no single points of failure, circuit breakers between every service pair. The trade-off: active-active at 99.99% means you're running at least 2x the infrastructure needed for peak load, paying for that headroom.

**Message delivery latency: p99 < 100-500 ms (online recipients)**
- WhatsApp: p99 < 500 ms (consumer mobile, global reach)
- Slack: p99 < 200 ms (desktop enterprise, lower global spread)
- Discord: p99 < 100 ms (gaming, users expect very fast response)
- Live Comments: p99 < 2 s (can tolerate more due to stream context)

Implies: you cannot use polling (too slow). You need WebSocket/SSE for push delivery. You cannot afford synchronous DB reads in the delivery path — hot data must be in Redis.

**Durability: zero message loss, at-least-once delivery**
Implies: write-ahead logging on gateways, Cassandra writes with LOCAL_QUORUM before acking sender, Kafka at-least-once semantics, idempotent consumers. The trade-off: at-least-once means duplicates are possible — every consumer must be idempotent, which adds complexity throughout.

**Ordering: strong per-conversation/channel, eventual across conversations**
Implies: server-assigned timestamps (Snowflake IDs or microsecond epoch), Cassandra clustering key for ordered reads. You never trust the client clock for ordering. The trade-off: strong ordering per conversation is achievable with a single writer per shard; total global ordering is too expensive and not required.

**Consistency: eventual for presence and reactions, strong for message store**
Implies: presence can be stale by 30-60 seconds (Redis TTL-based). Reaction counts can be approximate. Message delivery to the store must be consistent (LOCAL_QUORUM). This gives you the performance wins without compromising core functionality.

**Scale-specific NFRs to call out explicitly:**
- Number of concurrent WebSocket connections (drives gateway server count)
- Fan-out amplification factor (group size × messages/sec = delivery events/sec)
- Storage growth rate (to justify database choice)

---

### 3d. Capacity Estimation

The formula to always use in your head first:

```
messages/sec = DAU × messages_per_user_per_day / 86,400
delivery_events/sec = messages/sec × avg_recipients
peak = avg × peak_multiplier (3x WhatsApp, 4x Slack, 5x Discord/LiveComments)
```

**Anchor numbers to memorize:**

| System | Messages/day | Peak msg/sec | Peak delivery events/sec | Concurrent WS connections |
|---|---|---|---|---|
| WhatsApp | 100 B | 3.5 M | 17 M (group fan-out) | 450 M |
| Slack | 800 M | 37 K | 926 K | 10 M |
| Discord | 4 B | 231 K | 2.3 M | 10 M |
| Live Comments | 4.3 B (comments) | 250 K | 125 M | 30 M viewers |

**What the math tells you:**

When you see 450 M concurrent WebSocket connections (WhatsApp), that tells you immediately you need ~10,000 gateway servers (at 45K connections/server on 8-core, 32 GB RAM). When you see 17 M delivery events/sec for WhatsApp groups, you know the Fan-Out Service needs to be massively parallel with batched gRPC — you cannot afford 17 M individual gRPC calls/sec.

For storage: 100 B messages/day × 1 KB = 100 TB/day write volume for WhatsApp messages alone. At Cassandra RF=3 with 50% compression, you're growing your cluster by about 5 nodes/day (10 TB SSD/node). 90-day retention requires ~1,350 physical Cassandra nodes. This number tells you the cluster is enterprise-scale and forces operational considerations around rolling upgrades and compaction.

**Time to spend on estimation:** 4-5 minutes. Hit the key numbers, do the gateway server count math, and call out one insight from the numbers (like "this fan-out rate means we need a dedicated fan-out service, not inline delivery"). Don't calculate every sub-metric — show you can extract meaning from numbers, not just compute them.

---

### 3e. High-Level Design (HLD)

Draw these components in this order on the whiteboard. The order matters because each component justifies the next.

**Whiteboard order:**

1. **Client Layer** → WebSocket gateway (why: clients need persistent connections for low-latency push delivery; HTTP polling doesn't work at this scale)
2. **WebSocket Gateway Cluster** → Message Router/Fan-Out Service (why: gateways are stateful but need to route to other gateways; a separate stateless router handles this cleanly)
3. **Connection Registry (Redis)** alongside the router (why: the router needs to know which gateway holds each user; Redis GET is O(1) and sub-ms)
4. **Kafka Event Bus** between ingest and consumers (why: decouples message acceptance from delivery, indexing, and notifications; provides durability buffer)
5. **Message Store (Cassandra/MySQL)** as a Kafka consumer (why: persistent message storage for history, offline drain, and receipts)
6. **Offline Queue (Redis Sorted Set)** for offline users (why: users reconnect after hours; you need durable per-user queuing without a per-user Kafka partition)
7. **Push Notification Service** (why: offline mobile users who haven't reconnected need a wake-up call via APNs/FCM)
8. **Supporting services** — add media storage, search, presence, moderation based on requirements

**Key architectural decisions to call out explicitly:**

"I'm using WebSocket instead of HTTP polling because polling at 450 M clients generates 450 M req/sec for presence alone — that's untenable. WebSocket gives me persistent connections that reduce this to heartbeats every 30 seconds."

"I'm choosing Cassandra for message storage because the partition key maps perfectly to the access pattern — 'give me the last N messages in conversation X'. The wide-row model with time-ordered clustering keys makes this O(1). MySQL would need a compound index and can't handle 3.5 M writes/sec without extreme sharding."

"I'm separating the Fan-Out Service from the Gateway because fan-out is stateless and I need to scale it independently. Gateways scale based on connection count; fan-out workers scale based on message rate and fan-out amplification."

**Data flow to describe verbally (1:1 message):**
1. Sender encrypts and sends message via WebSocket
2. Gateway accepts, writes to Kafka and returns SENT ack to sender
3. Fan-Out Service reads from Kafka, looks up recipient in connection registry
4. If online: sends batch gRPC to recipient's gateway → gateway pushes to recipient's WebSocket → DELIVERED receipt flows back
5. If offline: writes to Redis offline queue → triggers push notification
6. On recipient reconnect: drains offline queue → DELIVERED receipts flow back to sender

---

### 3f. Deep Dive Areas

The interviewer will probe one or two of these. Go deep on at least two unprompted.

#### Deep Dive 1: Fan-Out at Scale (the most-probed area)

**The problem:** A group message to 1,024 members generates 1,023 delivery events. At WhatsApp's scale, 30% of 3.5 M msg/sec (peak) = 1.05 M group messages/sec × average 49 recipients = 51 M delivery events/sec. You cannot handle this with 51 M individual gRPC calls/second.

**The solution:** A dedicated Fan-Out Service with a goroutine pool. When a group message arrives:
1. Batch-query the connection registry with Redis MGET for all member user_ids in one round trip (~1 ms for 1,024 keys)
2. Group members by which gateway server they're connected to (expect 50-100 unique gateways for a 1,024-member group in practice, since members cluster by region/ISP)
3. Send one batched gRPC call per unique gateway (collapses 1,024 potential calls to ~50-100)
4. Pipeline all offline ZADD operations in one Redis round trip

**Trade-offs to volunteer:**
- ✅ Batching to gateways reduces gRPC call count by 10-20x
- ❌ Adding a Fan-Out Service hop adds ~5-10 ms latency
- ✅ Stateless — scales horizontally as a Kafka consumer group
- ❌ For truly massive groups (100K+ members), MGET is 100K keys ≈ 3 MB request; need partitioned fan-out workers

**Tiered strategy for very large groups:**
- Small groups (< 1K members): direct fan-out via MGET + batched gRPC
- Large channels (1K-100K members, Slack/Discord): Redis Pub/Sub — publish once to `ws_channel:{channel_id}`; all gateway servers subscribed to that channel receive one message and fan out to their local connections
- Mega-servers (Discord, 500K+ members): push the event to ALL gateway servers; each gateway filters against its own in-memory connection map. Yes, this sends to gateways that don't have any members of this server — but it avoids the O(N members) lookup problem entirely. Only valid because the per-gateway message is small and there are ~500 gateways.

#### Deep Dive 2: Offline Queuing and Reconnect

**The problem:** Users disconnect and reconnect constantly (battery dies, subway tunnel, midnight in a different timezone). Messages sent during disconnect must be queued and delivered in order when the user reconnects, without loss, with correct deduplication.

**The solution:** Redis Sorted Set as an offline queue per user.
- Key: `offline_queue:{user_id}`, Type: Sorted Set, Score: enqueue_unix_timestamp, Value: serialized MessageEnvelope
- TTL: 30 days (WhatsApp) — after which undelivered messages expire
- On reconnect: `ZRANGEBYSCORE offline_queue:{user_id} -inf +inf` → delivers all queued messages in timestamp order
- Client ACKs each envelope_id → gateway issues `ZREM` to remove from queue
- If client disconnects mid-drain: messages remain in queue (ZREM not issued) and are re-delivered on next reconnect; client deduplicates by envelope_id

**Trade-offs to volunteer:**
- ✅ Redis Sorted Set gives you ordering for free (score = timestamp)
- ✅ TTL means no manual cleanup jobs
- ❌ Redis memory: 5% of 1.5 B DAU offline at peak × 10 messages avg × 316 bytes/entry ≈ 237 GB — needs a 10-node Redis cluster with 32 GB each
- ❌ Redis is in-memory; AOF persistence adds latency. Mitigate with AOF `everysec` + Redis Cluster replication (1 primary + 2 replicas)
- ✅ Fallback: Cassandra-backed offline store for Redis failure — slower but zero message loss

#### Deep Dive 3: Message Ordering (slightly less common but asked at Staff+)

**The problem:** Two users send messages nearly simultaneously in the same conversation. What order do other recipients see them in? What happens with clock skew between gateway servers?

**The solution:** Server assigns the ordering key — never trust the client clock.
- WhatsApp: TIMEUUID (version-1 UUID) at the gateway. Encodes generation timestamp + random bits. Globally unique, approximately time-ordered. Clock skew between gateways is bounded by NTP to < 10 ms — below chat ordering granularity.
- Slack: `ts` field — Unix microseconds assigned by the MySQL shard writer for that workspace. Because all writes for a workspace go through one shard's single writer, the `ts` is guaranteed monotonically increasing within the workspace.
- Discord: Snowflake ID — 41-bit epoch ms since Discord epoch (Jan 1, 2015) + worker ID + process ID + per-process increment. Globally unique and time-ordered without coordination between workers.
- Live Comments: Snowflake ID at Comment Ingest Service.

**The key insight to state:** "I never use the client timestamp for ordering. The server assigns the ordering key. For per-conversation ordering, a single writer path (Cassandra partition, MySQL shard) provides a natural total order. For cross-conversation, we only guarantee eventual consistency."

---

### 3g. Failure Scenarios

Senior candidates frame failure scenarios proactively. Don't wait to be asked — mention the most important two or three failures when you finish your HLD.

**Failure 1: Gateway server crash**
- Impact: ~45,000 users on that server lose their WebSocket connection
- Detection: TCP RST immediately hits clients; load balancer health check marks server unhealthy within 10 seconds
- Recovery: clients reconnect with exponential backoff + jitter (1s → 2s → 4s, ±30% jitter, max 32s). On reconnect, client goes to a healthy gateway. The new gateway reads the connection registry, sees no pending connection, and drains the offline queue.
- Key insight: **messages sent to users on the crashed gateway while they're reconnecting go into their offline queue** — zero message loss because offline queue is in Redis, not on the gateway. This is why separating the offline queue from the gateway is critical.

**Failure 2: Redis shard failure**
- Impact: connection registry and offline queue for ~1/N_shards of users inaccessible
- Detection: Redis Cluster failure detection in < 5 seconds; replica promotion in < 30 seconds
- Recovery: AOF `everysec` persistence means at most 1 second of data loss. Replica promoted automatically. For additional durability: fallback to Cassandra-backed offline store for the affected key range
- Key insight: you need a Redis failure fallback. "We'd serve stale presence" is acceptable for presence. "We'd lose messages in the offline queue" is not acceptable. Always have a Cassandra fallback for the offline queue.

**Failure 3: Kafka consumer group lag (Fan-Out Service falls behind)**
- Impact: messages accepted by gateways (SENT ack) but not yet delivered to recipients; latency degrades
- Detection: Kafka consumer lag metric; alert if lag > 10,000 messages or latency > SLA
- Recovery: auto-scale Fan-Out Service consumer group instances (each instance gets a subset of partitions). Kafka's durable log means no messages are lost — they're just delayed. For sustained overload: shed load on large channels by switching to a pull model (clients poll history API).
- Key insight: **Kafka is the buffer that saves you during traffic spikes**. Without Kafka between ingest and fan-out, a traffic spike means the Fan-Out Service is a blocking dependency and the entire write path degrades. With Kafka, the ingest path (accepting messages, returning SENT ack) remains healthy; only delivery is delayed.

**Framing tip:** Don't just describe what breaks — describe how the system degrades gracefully. "During this failure, messages are not lost, delivery is delayed by X, and users see a brief reconnection." That's the senior-level framing.

---

## STEP 4 — COMMON COMPONENTS

This section covers every component that appears across multiple problems in this pattern. Know each one cold.

---

### WebSocket Gateway Servers

**Why used:** Real-time push delivery to clients requires persistent connections. HTTP/1.1 polling generates massive unnecessary traffic (30 M clients × 1 poll/sec = 30 M req/sec just for idle presence). WebSocket is full-duplex and uses a single persistent TCP connection per client.

**Key config:**
- Each server handles 15,000-45,000 connections (varies by server size: 8-core, 32 GB RAM → ~45 K)
- Uses epoll-based async event loop (Go goroutines, Node.js event loop, or custom C++ like Discord's Gateway)
- Each server maintains an in-memory map of `{user_id → websocket_conn}` for local connections
- Heartbeat interval: client sends heartbeat every 30 seconds; server renews presence TTL
- Load balancer uses sticky routing (consistent hash on user_id or IP) to minimize reconnect storms

**Counts at scale:**
- WhatsApp: ~10,000 gateway servers (450 M conns / 45 K per server)
- Slack: ~500 gateway servers (10 M conns / 20 K per server)
- Discord: ~500 gateway servers (10 M conns / 20 K per server)
- Live Comments: ~2,000 Chat Receive Service servers (30 M viewers / 15 K per server)

**What happens without it:** You're stuck with HTTP long-polling or Server-Sent Events. Long-polling creates thundering-herd on reconnects; SSE is unidirectional (can't send typing indicators or receipts). Both are significantly less efficient and have worse browser/mobile support at very high concurrency.

**Server-Sent Events (SSE):** Used as a fallback in Slack and Live Comments for clients that can't maintain WebSocket (smart TVs, some corporate firewalls). SSE is unidirectional (server → client only); for posting messages, client uses a separate HTTP endpoint. Good enough for display-only use cases.

---

### Fan-Out Service

**Why used:** The gateway that receives a message cannot afford to perform the full fan-out inline — blocking a gateway's event loop on 1,023 gRPC calls for a group message would spike latency for all other users on that gateway. A separate stateless service handles fan-out asynchronously.

**Key config:**
- Stateless, horizontally scalable Kafka consumer group
- Goroutine/thread pool to parallelize per-gateway batch gRPC calls
- Reads channel/group member list from cache (Redis, 5s TTL) — NOT from DB on the hot path
- Groups recipients by their gateway server, sends one batched gRPC per gateway
- For offline members: pipelines ZADD to offline queues in one Redis round trip

**Tiered strategy:**
- Small groups (< 1K): MGET connection registry → group by gateway → batched gRPC
- Large channels (1K-100K, Slack): Redis Pub/Sub — publish once, each subscribed gateway delivers locally
- Mega-servers (Discord 500K+): push event to ALL gateways, each filters by its in-memory map

**What happens without it:** Either you do fan-out inline in the gateway (adds latency spikes, head-of-line blocking for all other users on that gateway) or you do fan-out in the Message Router (couples routing and delivery, harder to scale independently). The Fan-Out Service is the decoupling layer that lets each component scale to its own bottleneck.

---

### Redis Cluster

**Why used:** Sub-millisecond latency for hot-path lookups that cannot go to a relational database. Redis is the operational backbone of all four messaging systems.

**Key use cases across all four problems:**

| Use Case | Key Pattern | Data Type | TTL |
|---|---|---|---|
| Connection Registry | `conn:{user_id}:{device_id}` → gateway_id | String | 90s (renewed by heartbeat) |
| Offline Queue | `offline_queue:{user_id}` | Sorted Set (score=ts) | 30 days |
| Presence | `presence:{user_id}` → {status, last_seen} | Hash | 60s |
| Group/Channel member cache | `channel_members:{channel_id}` | Set | 5-60s |
| Slow Mode (Live Comments) | `slow:{stream_id}:{user_id}` | String (SET NX) | slow_mode_seconds |
| Rate limiting | `ratelimit:{user_id}:{endpoint}` | Sorted Set or String | Window duration |
| Deduplication | `dedup:{envelope_id}` | String | 24h |
| Viewer Set (Live Comments) | `stream_servers:{stream_id}` | Set of CRS server IDs | 120s |
| Recent Comments cache | `recent_comments:{stream_id}` | List (LPUSH + LTRIM) | Until stream ends |
| Permission bitmask (Discord) | `perm:{channel_id}:{user_id}:{role_version}` | String (8-byte bigint) | 5 min |
| Ban cache | `ban_cache:{channel_id}` | Set | 300s |

**Key config:** RF=1 primary + 2 replicas per shard. AOF persistence with `appendfsync everysec` (at most 1 second data loss on hard failure). Redis Cluster mode for horizontal scaling. Sentinel or cluster-native HA.

**What happens without it:** You'd make DB calls on every message delivery (connection registry lookup: +10-50 ms latency per message), every presence update (+10 ms), and every slow-mode check (+10 ms). At 3.5 M msg/sec, even 10 ms per message in DB calls is physically impossible. Redis is not an optimization — it's a requirement.

---

### Kafka (Event Bus)

**Why used:** Decouples message ingest from fan-out, search indexing, notification dispatch, and webhook delivery. Provides a durable buffer that absorbs traffic spikes. Enables multiple independent consumers (fan-out, search indexer, notification service, analytics, webhooks) to all consume the same message stream without coupling.

**Key config:**
- Replication factor = 3 across 3 AZs (durability)
- Partition strategy: Slack partitions by `workspace_id` (ensures per-workspace ordering). Discord partitions by `channel_id`. Live Comments partitions by `stream_id` (one stream's comments stay in order).
- At-least-once delivery semantics — consumers must handle idempotency
- Consumer groups: Fan-Out Service, Search Indexer, Notification Service, Webhook Service each have their own consumer group

**Topics used (Slack example):** `messages`, `reactions`, `presence`, `thread_updates`, `notifications`, `webhooks`, `raw_messages`

**Topics used (Live Comments):** `raw_comments`, `approved_comments`, `moderation_actions`, `super_chats`, `stream_events`

**What happens without it:** Either you couple the gateway directly to the fan-out service (tight coupling, no buffer, cascading failures during spikes), or you route everything through the DB (too slow for fan-out). Kafka is what lets you say "the gateway just needs to write to Kafka and return SENT to the sender — everything else is async." This is how you achieve sub-100 ms sender acknowledgment regardless of how long fan-out takes.

**Important nuance:** WhatsApp's architecture doesn't lean on Kafka as heavily as Slack/Discord/LiveComments. WhatsApp uses more direct routing (Message Router → connection registry → gateway gRPC) with Kafka as an overflow/analytics buffer. Slack, Discord, and Live Comments use Kafka as the primary nervous system.

---

### Connection Registry in Redis

**Why used:** When the Fan-Out Service needs to deliver a message to user X, it has to know which of the 10,000 gateway servers user X is connected to. This lookup must be O(1) and sub-millisecond.

**Key pattern:** `conn:{user_id}:{device_id}` → `gateway_server_id` (e.g., "gw-use1-042")

**Key config:**
- TTL = 1.5x heartbeat interval (heartbeat every 30s → TTL 90s). If the client disconnects without sending an explicit disconnect message (e.g., phone dies), the TTL auto-expires the registry entry.
- Renewed by the gateway on every client heartbeat
- On reconnect, the gateway SETS the entry pointing to the new gateway server

**What happens without it:** The Fan-Out Service doesn't know where to send messages. You'd have to broadcast to all gateway servers and have each one check locally — O(num_gateways) network calls for every message delivery. That's 10,000 unnecessary gRPC calls for every message. Completely untenable at scale.

---

### Offline Queue (per-user)

**Why used:** Users are offline for hours (they sleep, commute, have dead batteries). Messages sent during offline periods must be queued, delivered in order on reconnect, and expired after a reasonable period.

**Key config:**
- Redis Sorted Set: `offline_queue:{user_id}`, score = enqueue_unix_timestamp
- TTL: 30 days (WhatsApp), effectively unbounded (Slack), not applicable (Live Comments uses recent-150 List)
- Drain on reconnect: `ZRANGEBYSCORE offline_queue:{user_id} -inf +inf`
- ACK per envelope_id: `ZREM offline_queue:{user_id} {value}` after client acknowledges
- Batch drain: up to 50 envelopes per WebSocket frame to avoid overwhelming the client on reconnect

**Memory estimate:** At WhatsApp scale — 5% of 1.5 B DAU offline × 10 messages avg × 316 bytes/entry ≈ 237 GB. A 10-node Redis cluster with 32 GB RAM each handles this comfortably.

**What happens without it:** Messages sent to offline users are lost unless you fall back to only APNs/FCM push (which notifies but doesn't deliver message content). You'd need the user to explicitly fetch history on every app open, which breaks the "all messages appear automatically" user expectation.

---

### Push Notification Service (APNs / FCM)

**Why used:** When a user is offline (no WebSocket connection), the only way to alert them is through Apple Push Notification Service (APNs) for iOS or Firebase Cloud Messaging (FCM) for Android. These are the OS-level notification channels that wake up backgrounded apps.

**Key config:**
- Notification payload is intentionally minimal — no message content in the push payload. WhatsApp sends only `{user_id, unread_count}`. This is a privacy requirement (APNs/FCM notifications can be intercepted at the OS level, so you don't want message content visible there).
- APNs: `content-available: 1` for silent background delivery (app wakes up to fetch messages). Display notification shown only if app doesn't process in background.
- Evaluates user preferences before sending: Do Not Disturb, muted channels, notification keywords
- Retry with exponential backoff (APNs allows retry up to 24h for persistent notifications)

**What happens without it:** Offline users never know they have messages unless they manually open the app. For a communication tool, this is a fatal usability flaw.

---

### Object Storage (S3 / Equivalent) + CDN

**Why used:** Media files (images, videos, documents) are too large for database storage and require exabyte-scale capacity with 11 nines durability. CDN fronting ensures media downloads don't all hit the origin.

**Key config:**
- Presigned URLs (15-minute expiry) for access control — only authorized users can download
- Content-addressed by SHA-256 of the file (or ciphertext for WhatsApp) — enables deduplication without decryption
- CDN caches by URL; content-addressed means URL is permanent → CDN cache hit rate is very high for popular media
- Lifecycle policies for expiry (WhatsApp: 30-day TTL on media after upload)
- WhatsApp: SHA-256 of *ciphertext* as the S3 key — deduplication without decryption. Same encrypted blob (forwarded media) deduplicates. Different encryption of the same plaintext does not — intentional, preserves privacy.

**What happens without it:** Storing media in the DB creates enormous performance problems (BLOB columns in MySQL/PostgreSQL don't scale past terabytes). Serving media from application servers bottlenecks on bandwidth and CPU.

---

### gRPC (Internal Service Communication)

**Why used:** Inter-service calls (Fan-Out Service → Gateway servers, Message Router → Fan-Out Service) need to be fast, typed, and support streaming. gRPC runs over HTTP/2 with multiplexed connections, has Protocol Buffer serialization (more compact than JSON), and supports bidirectional streaming.

**Key advantage:** Persistent HTTP/2 connections between service pairs mean no TCP handshake overhead for each call. At 3.5 M msg/sec, eliminating TCP handshake overhead per call is significant.

**What happens without it:** REST over HTTP/1.1 for internal calls adds TCP connection overhead, has JSON serialization overhead, and lacks built-in streaming. Not a dealbreaker for low-throughput paths, but critical for the Fan-Out → Gateway path at millions of calls/second.

---

### Idempotency / Deduplication

**Why used:** Every reliable system uses retry logic. Retries mean the same message can arrive twice. The consumer must be able to handle duplicates gracefully.

**Key pattern:**
```
SET dedup:{envelope_id} "1" NX EX 86400
```
If the key already exists (NX = only set if not exists), the message is a duplicate — silently drop. TTL of 24 hours means dedup state doesn't grow indefinitely.

**DB-level idempotency:** `client_msg_id` (client-generated UUID) with a UNIQUE constraint in the DB. `ON CONFLICT DO NOTHING` ensures duplicate DB inserts are safe. Slack uses `(workspace_id, client_msg_id)` as a unique index.

**At-least-once vs. exactly-once:** Kafka consumer groups commit offsets only after successful processing. If the consumer crashes after processing but before committing, the message is reprocessed. This is intentional — at-least-once delivery is the guarantee. Exactly-once adds too much overhead (two-phase commits) for a chat system. Idempotent consumers handle the rare duplicate gracefully.

**What happens without it:** Retry logic (which you absolutely need for reliability) causes duplicate messages in the chat. At WhatsApp's scale (3.5 M msg/sec), even 0.01% duplicate rate = 350 duplicate messages/second visible to users. That's unacceptable.

---

### Snowflake IDs (or Equivalent Time-Ordered IDs)

**Why used:** You need globally unique message IDs that are also time-ordered, without a centralized ID generator (which would be a single point of failure at millions of IDs/second).

**Snowflake ID structure (Discord epoch Jan 1, 2015):**
```
[63..22] = milliseconds since epoch (41 bits = ~69 years of IDs)
[21..17] = internal worker ID (5 bits = 32 workers)
[16..12] = internal process ID (5 bits = 32 processes)
[11..0]  = per-process increment (12 bits = 4096 IDs/ms/process)
```

This gives you time-ordered IDs that are globally unique without coordination between workers. At 4096 IDs/ms/process with 32 workers × 32 processes = 4 billion IDs/ms — more than enough.

**Alternatives:**
- TIMEUUID (WhatsApp): version-1 UUID encodes timestamp + MAC address-based node ID + clock sequence
- `ts` string (Slack): server-assigned Unix microseconds at write time; not strictly a Snowflake but serves the same purpose

**What happens without it:** Either a centralized sequence generator (single point of failure, network bottleneck at millions of IDs/sec) or client-assigned timestamps (ordering not trustworthy due to clock skew, no global uniqueness guarantee).

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### WhatsApp

**Unique Thing 1: End-to-End Encryption with Signal Protocol (X3DH + Double Ratchet)**

WhatsApp is the only problem in this folder where the server deliberately cannot read messages. This forces a Key Service architecture — each user pre-generates and uploads 100+ "pre-key bundles" (X3DH keys) to the server. When Alice wants to message Bob for the first time, she fetches one of Bob's one-time pre-keys, performs an X3DH Diffie-Hellman handshake offline, derives a shared secret without Bob needing to be online, and starts a Double Ratchet session. The server stores only ciphertext. This makes server-side search impossible, server-side spam filtering impossible, and server-side backup only possible through an optional user-controlled key vault.

**Unique Thing 2: Group E2EE via Sender Keys (O(1) encryption per message)**

Naive group E2EE would require encrypting the same message N times — once for each member. WhatsApp avoids this with Signal's Sender Keys protocol: each user generates a Sender Key chain and distributes it once to all group members via pairwise-encrypted sessions. After that, each group message is encrypted once using the sender's Sender Key. The server fans out the same ciphertext blob to all members. This is O(1) encryption cost regardless of group size.

**Different technical decision:** WhatsApp uses **Cassandra** for message storage because it needs to sustain 3.5 M writes/sec across 2 B users with acceptable eventual consistency. Slack uses **MySQL** for the same layer because it needs ACID transactions for threading and complex queries within a workspace. WhatsApp's design explicitly accepts eventual consistency in exchange for write throughput; Slack accepts sharding complexity in exchange for strong consistency.

**How WhatsApp differs from Slack in two sentences:** WhatsApp's defining constraint is E2EE — the server cannot see message content, making search, moderation, and analytics impossible, and requiring a dedicated Key Service for Signal Protocol pre-key distribution that Slack doesn't need. Slack's defining constraint is the workspace model — all data is isolated by workspace_id as the shard unit, enabling full SQL within a workspace (threading, reactions, file references, search indexing) at the cost of MySQL sharding complexity via Vitess.

---

### Slack

**Unique Thing 1: Workspace-as-Shard with MySQL (not Cassandra)**

Slack chose MySQL sharded by workspace_id as their primary message store. This is intentionally different from WhatsApp's Cassandra choice. The reason: Slack's access patterns require ACID transactions (message edit must atomically update text + search signal + thread reply count) and complex SQL (unread counts joining `channel_members` and `messages`, thread reply queries joining parent and replies). MySQL handles these naturally. At Slack's scale (37 K msg/sec peak vs. WhatsApp's 3.5 M), MySQL is achievable with ~200 shards. Vitess handles online resharding.

**Unique Thing 2: Per-workspace Elasticsearch Index with Index Lifecycle Management (ILM)**

Message search is a first-class feature in Slack. Elasticsearch runs per-workspace (or shared with per-workspace index isolation for small workspaces). Access control enforcement is baked into the index itself — each indexed document includes a `member_ids` array of who can see the channel. Search results are filtered at query time using this array, avoiding a DB lookup per search result. Elasticsearch ILM manages storage cost: messages move from hot (SSD, 0-30 days) → warm (HDD, 30-90 days) → cold (searchable snapshots on S3, 90+ days) → delete. No other problem in this folder has this level of search infrastructure.

**Different technical decision:** Slack uses **Redis Pub/Sub for large channels** (> 10,000 members) instead of explicit member-list fan-out. Fan-Out publishes one message to `ws_channel:{channel_id}` in Redis; all gateway servers subscribed to that channel receive it and deliver to their local connections. This changes fan-out cost from O(num_members) to O(num_gateway_servers), which is a 20-100x reduction for large channels.

**How Slack differs from Discord in two sentences:** Slack's complexity centers on the workspace abstraction — organizations, compliance, integrations, retention policies, and full-text search make it an enterprise product with strong consistency requirements; MySQL sharding is the right call. Discord's complexity centers on the guild permission system and voice infrastructure — granular per-channel permission bitmasks that must be evaluated for every message delivery at mega-server scale, plus a WebRTC SFU voice layer that Slack's architecture doesn't need.

---

### Discord

**Unique Thing 1: The Guild Permission System with Bitmask Cache**

Every message delivery in Discord requires a permission check: can this user even see this channel? The permission algorithm computes a 64-bit bitmask from: @everyone role permissions + OR of all member roles + ADMINISTRATOR bypass check + channel-level permission overwrites (role-specific and member-specific, with deny bits evaluated before allow bits). Computing this from DB on every message is too slow. Discord caches the final computed bitmask per `(user_id, channel_id)` in Redis with a 5-minute TTL. Version-based invalidation: instead of invalidating specific keys on role changes, increment a `role_version` counter per guild — all existing cache keys that don't have the current version are automatically stale. No explicit DEL storms needed.

**Unique Thing 2: ScyllaDB Instead of Cassandra (and the engineering blog to back it up)**

Discord migrated from Cassandra to ScyllaDB in 2023 ("How Discord Stores Trillions of Messages" engineering blog). ScyllaDB is a C++ re-implementation of Cassandra's CQL-compatible protocol. The key win: shard-per-core architecture eliminates JVM garbage collection pauses that caused Cassandra's p99 latency to spike to 5-10 seconds under heavy compaction. Discord processes 4 B messages/day; at that scale, p99 latency from GC pauses was causing noticeable user-facing degradation during gaming events (new game launches, esports finals). ScyllaDB achieved 10x better tail latency at the same throughput.

**Different technical decision:** Discord uses **push-to-all-gateways** for mega-servers (500K+ members) instead of resolving the member list. When a message arrives for a mega-server channel, the event is pushed to all 500 gateway servers; each gateway filters against its own in-memory connection map to find which of its users are members of this guild and have VIEW_CHANNEL permission. This is the only scalable approach when the member list itself is 500K rows.

**How Discord differs from Live Comments in two sentences:** Discord is user-centric — messages are delivered to specific users who have joined specific servers, requiring per-user permission checks and individual connection routing; the hard problem is the permission system at mega-server scale. Live Comments is stream-centric — every viewer of a stream receives the same comment with no personalization or permission filtering; the hard problem is fan-out to 500K identical recipients, which is why the viewer-set-based CRS architecture is fundamentally different from Discord's guild fan-out.

---

### Live Comments

**Unique Thing 1: Viewer-Set-Based Fan-Out (Not User-Based)**

Every other problem routes messages to specific users. Live Comments routes to the stream — everyone watching stream S gets every comment for stream S. The fan-out architecture exploits this by tracking which Chat Receive Service (CRS) servers have at least one viewer of stream S (not which individual users). Redis Set `stream_servers:{stream_id}` contains CRS server IDs. The Fan-Out Service reads this set (~34 server IDs for a 500K-viewer stream), issues one gRPC broadcast call to each CRS server, and each CRS delivers to all its local viewers for that stream. This makes fan-out cost O(num_CRS_servers) — approximately O(num_viewers / 15,000) — instead of O(num_viewers). For a 500K-viewer stream: 34 gRPC calls instead of 500,000.

**Unique Thing 2: Async ML AutoMod Pipeline with Trust-Tier Fast-Tracking**

No other problem in this folder has automated hate speech filtering. The live chat AutoMod pipeline is three stages:
1. Trust tier: 70% of comments from verified/trusted accounts bypass ML entirely
2. Bloom filter + l33tspeak-normalized regex: catches 80% of violations in < 5 ms
3. ML classifier (DeBERTa/RoBERTa on Triton Inference Server): handles nuanced hate speech in 50-500 ms

Comments are delivered to viewers first (optimistic display), then the async ML pipeline runs. If a violation is detected, a `COMMENT_DELETE` event is published to Kafka → fans out to all viewers who remove the message from their chat. This brief visibility window (< 2 seconds) is the accepted trade-off for maintaining real-time UX.

**Different technical decision:** Live Comments uses **Redis SET NX with TTL** for slow mode enforcement — a single atomic Redis command that sets a key only if it doesn't exist, with a TTL equal to the slow mode interval. This is O(1) and requires no cleanup job. If the key already exists, the user is rate-limited; if it doesn't exist, the key is set and the comment proceeds. WhatsApp and Slack don't have slow mode because their chat model doesn't require per-user-per-stream rate limiting.

**How Live Comments differs from WhatsApp in two sentences:** WhatsApp delivers messages to specific individual users through a routing graph (connection registry, per-user offline queues), making per-user state (E2EE keys, receipts, presence) the architectural center of gravity. Live Comments delivers the same message to all viewers of a stream using viewer-set routing where fan-out cost is independent of viewer count, making stream-centric fan-out and ML content moderation — not user routing — the architectural center of gravity.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (2-4 sentences each)

**Q: Why use WebSocket instead of HTTP long-polling for real-time messaging?**
A: WebSocket gives you a **persistent full-duplex connection** where both the client and server can send messages at any time. HTTP long-polling requires the client to maintain a new HTTP request every time it receives a message, creating constant reconnection overhead and making it effectively half-duplex. At 450 million concurrent connections (WhatsApp), WebSocket's single persistent TCP connection per user is dramatically more efficient than long-polling's repeated request-response cycle. Long-polling also caps delivery latency at the poll interval, which is too slow for real-time chat.

**Q: Why Cassandra over MySQL for WhatsApp's message store?**
A: The access pattern is "give me the last N messages in conversation X" — which maps perfectly to Cassandra's **partition key = conversation_id + clustering key = (bucket, message_id DESC)**. All messages for a conversation window sit on one node; reads are single-partition scans. MySQL needs a compound index on (conversation_id, message_id) and suffers page-level locking under WhatsApp's 3.5 million writes per second. Cassandra's LSM-tree storage engine has no read-before-write semantics on inserts, making it dramatically faster for pure write workloads. WhatsApp also accepts eventual consistency per conversation, which Cassandra handles natively with tunable consistency levels.

**Q: What is a Snowflake ID and why does Discord use it?**
A: A **Snowflake ID** is a 64-bit integer whose high bits encode a timestamp (milliseconds since Discord's epoch, January 1 2015) and whose low bits encode a worker ID, process ID, and per-process increment. This gives you globally unique IDs that sort chronologically without a centralized coordinator. Discord uses them because at 4 billion messages per day, a centralized ID generator would be a single point of failure and a throughput bottleneck. With Snowflake, any server can generate a globally unique, time-ordered ID independently.

**Q: How do you handle the case where a user is offline when a message is sent?**
A: The **offline queue** — a Redis Sorted Set per user, keyed by user_id, with messages scored by their enqueue timestamp. When the fan-out service determines a user has no connection registry entry (they're offline), it writes the message envelope to their Sorted Set and triggers a push notification via APNs or FCM to wake up their device. When the user reconnects, the gateway performs a ZRANGEBYSCORE to fetch all queued messages in timestamp order and delivers them over the new WebSocket connection. The client acknowledges each message (via ZREM), ensuring nothing is lost if the client disconnects mid-drain.

**Q: What does "at-least-once delivery" mean and why not exactly-once?**
A: At-least-once means every message is guaranteed to be delivered, but may occasionally be delivered more than once in retry scenarios. **Exactly-once requires two-phase commits** between the message broker and the consumer, which adds significant latency and complexity. For a chat system where users care more about messages arriving than about occasionally seeing a duplicate (which idempotent consumers handle by deduplicating on envelope_id), at-least-once is the right trade-off. Kafka, Cassandra, and Redis all operate on at-least-once semantics natively; building exactly-once on top would require distributed transactions across all of them.

**Q: How does presence work and how stale can it be?**
A: Each client sends a heartbeat to the server every 30 seconds. The server updates a **Redis Hash per user** (`presence:{user_id}`) with the latest timestamp and renews a 60-second TTL. If the user disconnects cleanly or their device dies without sending an explicit disconnect, the TTL expires after 60 seconds — no explicit disconnect event required. This means presence can be up to 60 seconds stale, which is acceptable for "last seen" and online/offline indicators. Typing indicators are separate: sent as ephemeral WebSocket events with a 5-second client-side TTL, suppressed for large channels.

**Q: Why is Slack's message store MySQL instead of Cassandra?**
A: Slack's access patterns require **ACID transactions** and complex SQL queries that Cassandra doesn't support naturally — message edits must atomically update text and invalidate search index signals, thread reply counts need atomic increments, and unread counts require joins between `channel_members` and `messages`. The workspace-as-shard model means all data for a workspace lives on one shard, making single-shard transactions simple. Slack's peak volume (37K messages/sec) is achievable with approximately 200 MySQL shards. Slack's own engineering blog confirms MySQL as their production choice.

---

### Tier 2 — Deep Dive Questions (with why + trade-offs)

**Q: How do you handle group message fan-out for a 1,024-member WhatsApp group at scale?**
A: The **Fan-Out Service uses a goroutine pool** that batch-queries the connection registry with a single Redis MGET for all 1,024 member user_ids — one round trip, ~1 ms. It then groups the results by gateway server. In practice, 1,024 members of a group cluster by region and ISP, so you get 50-100 unique gateways rather than 1,024. The service sends one batched gRPC call per unique gateway (collapses 1,024 potential calls to 50-100). Offline members are pipelined into Redis in a single ZADD batch command. The key trade-off is that a membership snapshot is taken at fan-out time — if someone joins the group between when the message was sent and when the snapshot was taken, they miss it; if someone leaves after the snapshot, they briefly receive the message. This is acceptable chat semantics. Scaling this to 100K-member "communities" would require partitioned fan-out workers and product constraints (admin-only posting) to reduce message frequency.

**Q: How does Discord's permission system work at scale for a 500,000-member server?**
A: Discord computes a **final 64-bit permission bitmask per (user, channel)** from: @everyone role → OR of all member roles → ADMINISTRATOR bypass → channel-level overwrites with deny-before-allow precedence. This is cached in Redis at key `perm:{channel_id}:{user_id}:{role_version}`. When a role changes, rather than issuing 500,000 cache DEL operations, Discord increments a `role_version` counter for the guild. All existing cached permission keys now have an outdated version in their key — they're automatically stale and recomputed on next access. For the fan-out of a single message to a 500K-member server, Discord pushes the event to all ~500 Gateway servers and lets each Gateway filter against its local connection map. This avoids the unsolvable problem of resolving 500K member connections in real time. The trade-off is that gateways receive events for guilds where they have zero connected members — acceptable overhead since the message is small.

**Q: Describe the AutoMod pipeline for live chat hate speech detection.**
A: It's a **three-stage async pipeline** that runs post-delivery to maintain real-time UX. Stage 1 (< 0.1 ms): Trust tier fast-track — 70% of comments from verified/trusted accounts skip to Stage 3 (ML) or are approved directly, based on account age and violation history. Stage 2 (< 5 ms): Bloom filter + l33tspeak-normalized regex — catches 80% of violations using per-channel and platform-wide word blocklists stored as RedisBloom filters; text normalization handles "h4t3" → "hate" type evasions. Stage 3 (50-500 ms): Fine-tuned DeBERTa/RoBERTa running on NVIDIA Triton Inference Server with dynamic batching; batches of 32 comments processed together at ~50 ms per batch. If Stage 3 flags a violation, it publishes a deletion event to Kafka → fans out to all viewers as a `COMMENT_DELETE` event. The core trade-off: comments are visible for up to 2 seconds before deletion (fail-open for real-time UX). Synchronous blocking would add 200-500 ms to every message and require ~78 A100 GPU instances at full load — cost-prohibitive and latency-destroying.

**Q: How do you scale message search in Slack across different-sized workspaces?**
A: Slack uses a **per-workspace Elasticsearch index** (`messages_{workspace_id}`) with Index Lifecycle Management (ILM) for storage tiering. Access control is baked into the index — each document includes a `member_ids` array of users with access to that channel, so search queries filter by `user_id in member_ids` at search time, avoiding a DB lookup per result. Large workspaces (enterprise, hundreds of thousands of messages/day) get dedicated Elasticsearch clusters; small workspaces share a cluster with index-level isolation. ILM manages cost: hot tier (SSD, 0-30 days) → warm tier (HDD, 30-90 days) → cold tier (searchable snapshots on S3, 90+ days). The trade-off: Elasticsearch is not the source of truth (MySQL is); there's a ~5-second indexing lag from message write to searchable. Consistency is eventual for search — acceptable for a "find old messages" use case.

**Q: How does WhatsApp E2EE work when Bob is offline when Alice sends a message?**
A: This is exactly what **X3DH (Extended Triple Diffie-Hellman)** solves. Bob pre-uploads key bundles to WhatsApp's Key Service: an identity key, a signed pre-key, and up to 100 one-time pre-keys. When Alice wants to start a session with offline Bob, she fetches Bob's key bundle, generates an ephemeral key, and performs four DH operations — she derives the same shared root key that Bob will derive when he comes online and reads the bundle metadata. There's no interactive handshake — Alice can encrypt and send immediately. Bob reconstructs the session using the key bundle metadata Alice used when he reconnects. The Double Ratchet ensures forward secrecy after session establishment: each message advances the chain key, and neither party stores the old key after use. The trade-off: if Bob's one-time pre-keys are exhausted, Alice falls back to using only the signed pre-key (slightly less forward secrecy), and Bob's device is notified to upload more pre-keys.

**Q: What happens to message ordering when two users send messages simultaneously in the same Slack channel?**
A: Slack's MySQL shard for that workspace serializes all writes through **InnoDB row-level locking on INSERT** with a server-assigned `ts = NOW(6)` (microseconds). Because all writes for a workspace go through a single shard, no two messages can receive the same microsecond timestamp. AUTO_INCREMENT primary key + `ts` index ensures a total order. If the shard is under heavy write load and two INSERTs arrive in the same microsecond, InnoDB's lock ordering resolves the tie consistently. The client receives messages ordered by `ts` and displays them in that order. Cross-shard (cross-workspace) ordering doesn't exist by design — Slack doesn't need it. The trade-off is that one very large workspace can saturate its shard's write throughput, which is mitigated by Vitess-based online resharding (splitting the workspace's shard when it exceeds a write threshold).

**Q: How do you handle the thundering herd when 10 million WhatsApp users simultaneously reconnect (e.g., New Year at midnight)?**
A: **Exponential backoff with jitter** is the primary defense — clients use `min(max_delay, base × 2^n) × random(0.7, 1.3)` on reconnect, spreading out the connection attempts across ~30 seconds instead of all hitting simultaneously. Consistent hash routing on user_id means reconnecting users tend to go back to the same gateway server they were on before (warm cache for connection registry lookups). The gateway auto-scaling group pre-scales before known events. New connection processing is rate-limited at 50K new connections/second across the gateway fleet to prevent any single gateway from being overwhelmed. During the reconnect storm, the offline queue is the safety net — messages continue to accumulate there during the brief period before reconnect succeeds, and are drained in correct order once the connection is established.

---

### Tier 3 — Staff+ Stress Tests (reason aloud)

**Q: If WhatsApp wanted to add server-side message search while maintaining E2EE, how would you design it?**
A: This is a genuine fundamental tension — you can't index ciphertext for full-text search. Let me reason through the options.

Option 1: **Client-side search only** — index on the device, search locally. WhatsApp actually does this. The limitation is you can't search on a new device (no history there), and it's slow for large conversation histories.

Option 2: **Trusted third-party compute enclave** — Intel SGX or AWS Nitro Enclaves. The client sends the decryption key securely to a hardware-attested enclave, the enclave decrypts, indexes, and returns search results. The key is ephemeral inside the enclave and never persists. The trade-off: you're now trusting the hardware manufacturer's attestation, and any enclave vulnerability breaks the E2EE promise. It's also operationally complex and the key is in-memory on a server, even briefly.

Option 3: **Homomorphic encryption search (HE)** — theoretically allows searching ciphertext without decryption. In practice, fully homomorphic encryption is 1000-10000x slower than plaintext operations. Not viable for a real-time search product at WhatsApp's scale today.

Option 4: **Optional user-controlled search index** — user opts in; their client derives a search key from their backup key, encrypts the search index with that key, and stores it server-side. Server can't read it; only the user's device with the search key can. This is where I'd land as the practical answer — it preserves E2EE semantics (server can't read the index), gives users search capability across devices, and the trade-off is explicit and user-controlled. The key insight I'd call out unprompted: **any server-side search fundamentally weakens the E2EE promise**; the honest answer is that you pick your trade-off and make it explicit to users.

**Q: Discord has 19 million active servers daily. How do you handle the permission cache warming cold-start problem when a new server with 100,000 members starts getting activity?**
A: When a server has been quiet and suddenly gets a message, the permission cache for all 100K members is cold. Here's how I'd reason through this:

The first message causes a permission cache miss for every potential recipient. The Fan-Out Service queries the `channel_viewers:{channel_id}` index (a Redis Set of user_ids with VIEW_CHANNEL), then for each online member, computes the permission bitmask from the PostgreSQL shard for this guild and caches the result. If 50% of the 100K members are online, that's 50,000 cache misses on first message. At 1 ms per permission computation, that's 50 seconds of serial compute — completely unacceptable.

The solution is **lazy warming with a warm-up trigger**: when a guild transitions from idle to active (first message after > 5 minutes of silence), the Fan-Out Service triggers an async permission pre-warming job. This job computes and caches permission bitmasks for the top N most active channels and their online members in parallel, using bulk PostgreSQL reads (one query per role + one query per channel for overwrites, not per-member). At 100K members with avg 5 roles each, you need 100K role lookups — batched as a JOIN, not individual queries. Pre-warming completes in ~5-10 seconds. The first 1-2 messages during this window are served from the DB (slow but not catastrophic — they go out of order; clients handle it). After warming, all subsequent messages hit the cache.

Additionally: version-based invalidation means you don't need to warm the full 100K × N_channels set — only the online members' active channels, which is typically much smaller.

**Q: You're the on-call engineer and the Fan-Out Service is falling 30 seconds behind Kafka at peak load. Walk me through your response.**
A: Here's how I'd triage this live.

First, I'd **assess impact without assuming**. Is this affecting delivery latency (messages delayed but not lost) or delivery failures (messages dropped)? Kafka consumer lag means delayed delivery — messages are queued in Kafka, durable, not lost. The severity is high but not a data loss incident.

Second, **immediate mitigation**: Fan-Out Service instances auto-scale on consumer lag metric (should already be in the runbook). If not already scaling, I'd manually add instances to the consumer group. Each new instance takes ownership of a subset of Kafka partitions, immediately reducing per-instance lag.

Third, **identify the bottleneck in the fan-out path**: is the lag caused by (a) Redis connection registry lookups being slow (Redis latency metric), (b) downstream gateway gRPC calls being slow (gRPC latency metric), or (c) pure CPU saturation in the Fan-Out Service? Each has a different fix. If Redis is slow: check Redis memory (is eviction happening?), add read replicas. If gRPC to gateways is slow: check if specific gateway servers are unhealthy (one slow backend can block a batch). If CPU saturation: the instance count scaling should help; also check if a specific large channel is causing disproportionate work.

Fourth, **graceful degradation for large channels**: if the lag is caused by a specific 100K-member channel, temporarily switch that channel's fan-out to the Redis Pub/Sub path (publish one message; gateways deliver locally) to bypass the MGET → batched-gRPC path. This reduces Fan-Out Service load by ~100x for that channel.

Fifth, **communicate and document**: open an incident channel, post impact assessment (N seconds delivery delay for X% of users), track recovery metrics. Once lag drops below SLA threshold, close the incident and write a post-mortem on the scaling trigger thresholds.

The senior framing I'd call out: "I treat Kafka lag as a delivery delay, not message loss. The system degrades gracefully because Kafka is the buffer. My priority is reducing the lag without losing a single message."

**Q: How would you design cross-region message delivery for WhatsApp with < 500 ms p99 latency between a user in Tokyo and a user in São Paulo?**
A: Tokyo → São Paulo RTT is approximately 280 ms. This is a genuine constraint — you can't violate the speed of light.

The architecture: both users connect to their nearest regional cluster (sender → AP-South DC in Tokyo; recipient → SA-East DC in São Paulo). The Message Router in Tokyo needs to know that the recipient is connected to the SA-East DC. This requires a **global connection registry** — a cross-region Redis layer (or a routing service backed by a globally replicated data store) that maps user_id → DC region.

For the routing: Tokyo's Message Router sees "recipient is in SA-East," forwards the message payload via a dedicated inter-DC backbone (not public internet — direct fiber peering between AWS APs, ~200 ms cross-continent RTT, significantly better than public internet). The SA-East Message Router delivers to the recipient's local gateway.

Latency breakdown: Tokyo write to Cassandra LOCAL_QUORUM (~20 ms) + cross-DC gRPC (~200 ms) + SA-East delivery to recipient gateway (~10 ms) ≈ 230 ms. This fits within the 500 ms p99 SLA.

The trade-off I'd call out: the **cross-DC connection registry adds complexity**. If the cross-DC link is partitioned, the Tokyo DC can't find the recipient's gateway. During partition, you fall back to writing to the recipient's DC-local offline queue (via Cassandra async cross-DC replication) and send an APNs/FCM push — message is delivered on reconnect, not in real-time. This is the correct fail-safe: availability and durability over real-time delivery during partitions.

The critical insight: **don't put the global registry on the hot message path unless you need it**. For most users, sender and recipient are in the same region. Regional routing handles 80%+ of traffic locally; cross-region routing is a minority case that justifies the additional complexity.

---

## STEP 7 — MNEMONICS

### The "CORD-PK" Framework

When you sit down for any messaging interview, sketch these 7 boxes in order before drawing anything else:

**C** — **Connections** (WebSocket Gateway — who holds the socket?)
**O** — **Ordering** (Snowflake IDs / server-assigned timestamps — who assigns the ID?)
**R** — **Registry** (Connection Registry in Redis — where is each user connected?)
**D** — **Durability** (Kafka + Message Store — what survives a crash?)
**P** — **Push** (Fan-Out Service + APNs/FCM — how does the message get to all recipients?)
**K** — **Queue** (Offline Queue in Redis — what happens when the recipient is offline?)

Every messaging system you'll ever design is a specific implementation of CORD-PK. Fill in the boxes with the problem-specific technology choices.

### The 5-Second Fan-Out Triage

When asked about fan-out for groups/channels of any size, follow this decision tree:

- **< 1,000 members**: Direct MGET + batched gRPC. Simple.
- **1,000 - 100,000 members**: Redis Pub/Sub per channel. Gateways subscribe, deliver locally.
- **> 100,000 members**: Push to ALL gateways, filter locally. Accept broadcast overhead.
- **Stream (unlimited viewers)**: Viewer-set routing. Track CRS servers per stream, not users per stream.

### Opening One-Liner

"Messaging systems are fundamentally a **durable ordered fan-out problem** — I'll design the ingest path to be fast and durable, the fan-out to be parallel and tiered by group size, and the offline path to guarantee no message loss across reconnects. Let me start with some clarifying questions on delivery model and scale."

This one-liner tells the interviewer you understand the problem, you have a framework, and you're not going to jump to "I'd use WebSocket" without understanding the requirements first. It's calm and confident.

---

## STEP 8 — CRITIQUE

### What's Well-Covered in the Source Material

The source files are comprehensive and accurate. The WhatsApp E2EE section (X3DH + Double Ratchet + Sender Keys) is unusually detailed and correct — most study materials get this wrong. The Discord Snowflake ID structure and the actual permission bitmask algorithm are drawn from Discord's official documentation. The ScyllaDB choice is backed by Discord's real engineering blog post ("How Discord Stores Trillions of Messages," 2023). The Live Comments ML pipeline (DeBERTa, Triton Inference Server, trust-tier fast-tracking) is industry-realistic. The capacity estimates use real reported numbers (WhatsApp's 100B messages/day, Discord's 4B/day, Slack's 20M DAU).

The connection between all four problems — that they share 11 core components and differ only in how they configure and combine them — is the right mental model and will help you transfer knowledge between problems in an interview.

### What's Missing or Shallow

**Multi-region active-active is underspecified.** The WhatsApp file mentions cross-DC Cassandra replication and per-region LOCAL_QUORUM, but doesn't address the hard problem: what happens during a cross-region network partition? How does the global connection registry behave? Which DC "wins" for writes? A real interview at Staff+ level will probe this.

**Leader election and consensus are not covered.** When a gateway server crashes, something has to detect this and trigger client reconnects. The source material says "load balancer health check" but doesn't address: what if the health check falsely marks a healthy server as dead? What if two servers think they're the primary for a given gateway cluster? These are nuanced failure modes that come up in real interviews.

**The Kafka partition scheme for ordering at massive scale is worth more depth.** The source material says "partition by workspace_id" for Slack but doesn't address the case where one very large, very active workspace generates enough traffic to saturate a single Kafka partition. This is a real operational problem in high-traffic Slack instances.

**Cost analysis is mentioned but not synthesized.** The Live Comments ML section estimates ~$420K/year for GPU inference after trust-tier optimization. The media storage section mentions egress costs. But there's no top-down cost comparison between the four systems or guidance on what to do when cost becomes a design constraint (which happens in real interviews at scale-up companies).

**GDPR and data deletion are not covered.** For all four systems, when a user deletes their account or requests data deletion under GDPR, you need to: delete their messages from Cassandra (tombstones, not actual deletes initially), delete their keys from the Key Service (WhatsApp), delete their Elasticsearch documents, and propagate deletion to all CDN caches. This is a non-trivial system design question that often comes up for messaging systems in European markets.

### Senior Probes That Will Trip Up Under-Prepared Candidates

1. "If WhatsApp's Key Service goes down, what happens to new conversations?" (Answer: Alice can't fetch Bob's pre-key bundle; she can't start E2EE sessions with new contacts; existing sessions are unaffected; Key Service must be multi-region active-active with a read-heavy replica fleet)

2. "Slack has a workspace with 500,000 members and all of them are in #general. How do you handle a message to #general?" (Answer: This is the mega-channel edge case for Slack — the same tiered fan-out applies; for channels > 100K members, push to all gateways and filter locally; this is an admitted scaling challenge in Slack's architecture)

3. "A Discord bot has ADMINISTRATOR permission. What are the security implications?" (Answer: ADMINISTRATOR bypasses all channel-level permission denies; if the bot is compromised, attacker has owner-level access to the entire guild; Discord recommends minimum-scope permissions; this is a product-security trade-off, not a system design trade-off)

4. "How do you prevent the Fan-Out Service from writing to a user's offline queue multiple times for the same message during retries?" (Answer: Idempotency — the ZADD to the offline queue uses the envelope_id as the value; Sorted Set ZADDs with the same member (value) update the score but don't create duplicates; client-side dedup on envelope_id handles any that slip through)

5. "Live Comments has a 500K-viewer stream. One of the Chat Receive Servers crashes. Walk me through every affected user's reconnect experience." (Answer: ~2,500 viewers (~500K/200 CRS) lose their WebSocket connection; client reconnects to a new CRS; sends `?since={last_comment_id}`; new CRS reads `LRANGE recent_comments:{stream_id} 0 149` from Redis and delivers missed comments in initial batch; no comment loss for gaps < 150 comments in the Redis List)

### Common Traps to Avoid

**Trap 1: Proposing Kafka for per-user queuing.** Kafka doesn't scale to a partition per user (2 billion users × 1 partition = 2 billion Kafka partitions — not feasible). Use Redis Sorted Sets for per-user offline queues. Kafka is for per-workspace or per-stream event streams with bounded partition counts.

**Trap 2: Putting fan-out on the critical path of the sender ack.** If the sender has to wait for all 1,023 group members to receive the message before getting their SENT ack, any slow gateway introduces unacceptable latency for the sender. Sender gets the SENT ack when the message hits the server (after Cassandra write + Kafka publish). Fan-out is async.

**Trap 3: Using SQL for message storage without calling out the sharding strategy.** If you say "I'd use MySQL for WhatsApp's message store," the follow-up is immediate: "How do you handle 3.5 million writes per second across 2 billion users?" If you can't answer that with "sharded by conversation_id with N shards, using Vitess" or explain why you'd actually use Cassandra instead, you've lost the thread.

**Trap 4: Forgetting that WebSocket gateways are stateful.** Candidates sometimes say "I'd scale the gateway layer horizontally like a stateless service." WebSocket gateways are stateful — they hold open socket file descriptors. You can add new servers behind a consistent-hash load balancer, but existing connections don't migrate; they live until the client reconnects. Rolling deploys require connection draining (send close frame → client reconnects → deploy). Failing to call this out signals you haven't actually operated a WebSocket-at-scale system.

**Trap 5: Designing E2EE but not addressing key distribution.** Saying "I'd use E2EE" without explaining how keys are distributed asynchronously (X3DH pre-key bundles) tells the interviewer you know the term but not the mechanism. The hard part of E2EE for messaging is not the symmetric encryption — it's the asynchronous key exchange that allows Alice to send to an offline Bob without a real-time interactive handshake.

---

*This guide is self-contained. All concepts above are derived from the WhatsApp, Slack, Discord, and Live Comments design documents, and the common patterns and problem-specific differentiator files in this folder.*
