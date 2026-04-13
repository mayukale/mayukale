# Infra-06: Message Brokers — Interview Study Guide

Reading Infra Pattern 6: Message Brokers — 5 problems, 9 shared components

---

## STEP 1 — OVERVIEW

This guide covers five interconnected message broker system design problems. Every single one of them is built on Apache Kafka as the backbone. The five problems are:

1. **Dead Letter Queue System** — routing failed messages, poison pill detection, replay UI
2. **Distributed Message Queue** — Kafka from scratch, internals, ISR, segment files, zero-copy
3. **Event Streaming Platform** — Schema Registry, CDC with Debezium, Flink stream processing
4. **Pub/Sub Notification System** — fan-out at scale, push/pull delivery, 1M subscriber broadcast
5. **Reliable Event Delivery System** — transactional outbox, idempotent consumers, saga orchestration

**Why this cluster exists:** In FAANG and infrastructure companies, async messaging underpins everything: job scheduling, provisioning state transitions, billing events, monitoring fan-out, and distributed saga coordination. An interviewer asking you to "design a message broker" or "design a pub/sub system" or "design a reliable event delivery system" is testing whether you understand the full stack — not just "use Kafka" but why Kafka is structured the way it is, and how to build reliable systems on top of it.

**The nine shared components** that recur across all five problems: Apache Kafka (core log), ISR-based replication, consumer groups, partition-based ordering via message key, producer batching (batch.size + linger.ms), idempotent producer / exactly-once semantics, message headers for metadata, retry with exponential backoff + DLQ, and Redis for idempotency/caching.

---

## STEP 2 — MENTAL MODEL

### The Core Idea

**A message broker is a durable, ordered, replayable mailbox for bytes — where the only rule is: sequential writes to an append-only log.**

Everything else that seems complicated — ISR replication, consumer groups, exactly-once delivery, DLQs, fan-out — is just carefully managing who reads from which position in which mailbox, and what happens when something goes wrong.

### The Real-World Analogy

Think of Kafka like a newspaper printing press. The press prints today's edition and stores every edition ever printed in a giant warehouse (the log). Any subscriber (consumer group) can pick up the paper at any time — today's edition, last month's edition, doesn't matter. Different subscriber clubs (consumer groups) each have their own bookmark. The press doesn't care how many clubs are reading or how far behind they are. It just keeps printing new editions and doesn't throw away old ones until a retention clock runs out.

A "topic" is a newspaper name (The Orders Gazette). A "partition" is a print run that goes to a specific geographic region — all orders for the US East region go to partition 3. Within that partition, the order of articles is guaranteed. But you can't compare the ordering of The Orders Gazette partition 3 with partition 7 — they're independent.

Dead letters are the equivalent of newspapers that got delivered to the wrong address and came back unread — you need a separate shelf (DLQ topic) to store them so you can figure out what went wrong.

### Why This Is Hard

Three things make message broker systems genuinely hard in interviews:

**1. The dual-write problem.** If your service writes to a database AND to Kafka, what happens if one succeeds and the other fails? You can't use 2PC across DB and Kafka without performance consequences. The transactional outbox pattern is the answer, but most candidates don't know it.

**2. At-least-once vs. exactly-once.** Kafka guarantees at-least-once delivery by default. Getting to exactly-once requires understanding where duplicates enter the picture (producer retries, consumer crashes before offset commit) and how to eliminate them at each layer (idempotent producers with sequence numbers, idempotent consumers with Redis SET NX).

**3. Ordering vs. parallelism tradeoff.** Kafka guarantees ordering within a partition but not globally. The only way to get more parallelism is more partitions. But more partitions mean you can't get a total order across them. Every design decision about partition key selection, partition count, and consumer group assignment is a choice between ordering guarantees and throughput.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before drawing a single box, ask these. What they reveal is in brackets.

1. **"What delivery guarantee do you need — at-least-once, at-most-once, or exactly-once?"** [Exactly-once requires transactional outbox + idempotent consumers. At-most-once allows acks=0 but loses messages. This one question changes 30% of the design.]

2. **"Do you need strict ordering guarantees, and if so, at what granularity — per entity, per topic, or globally?"** [Global ordering is impossible at scale; per-entity ordering via partition key is the practical answer. If they say "global," push back gently.]

3. **"What is the expected message throughput and average payload size?"** [This drives broker count, partition count, replication strategy, and whether zero-copy sendfile() matters at all. 1K/s vs. 10M/s are radically different designs.]

4. **"What happens to a message that can't be processed after N retries — do we need to inspect it, fix it, and replay it, or just drop it?"** [This determines whether a DLQ + operator UI is in scope.]

5. **"Are there multiple independent consumers of the same message stream, and do they each need their own delivery cursor?"** [Yes = Kafka consumer groups; No = maybe a simpler queue. If they say 1M+ subscribers per topic, that triggers tiered fan-out architecture.]

**What changes based on answers:**
- Ordering required per entity → partition key = entity ID, single partition per consumer
- Exactly-once → outbox pattern on write side + Redis dedup on consumer side
- 1M+ subscribers → fan-out service with sharding, not naive consumer groups
- Need inspection/replay → Elasticsearch-backed DLQ dashboard

**Red flags to watch for in the interview:**
- Jumping straight to "use Kafka" without asking about throughput, ordering, or delivery guarantees
- Saying "use a database queue table" for 1M+ messages/sec without acknowledging the I/O model problem
- Proposing global ordering without a plan for how to scale past one partition
- Forgetting that Kafka acks=1 (default) loses messages when the leader crashes before followers sync

---

### 3b. Functional Requirements

**Core must-haves regardless of variant:**
- Producers publish messages to named topics
- Consumers read messages with independent position tracking (consumer groups)
- At-least-once delivery by default; exactly-once optionally
- Messages persisted for configurable retention (not dropped on delivery)

**Dead Letter Queue specifically adds:** routing failed messages after retry exhaustion, search/browse/replay UI for operators, poison pill detection

**Distributed Message Queue adds:** partition replication with ISR, leader election, configurable acks, log compaction

**Event Streaming Platform adds:** Schema Registry with compatibility enforcement, CDC via Debezium, stream processing (Flink/Kafka Streams)

**Pub/Sub Notification adds:** push (webhook) and pull delivery modes, subscription-level filter expressions, ordered delivery per ordering key, 1M+ subscriber fan-out

**Reliable Event Delivery adds:** transactional outbox pattern, idempotent consumer with Redis dedup, saga orchestration for multi-service transactions

**Clear scope statement for your whiteboard:** "I'll design a system that reliably delivers messages from producers to N independent consumer groups, guaranteeing [at-least / exactly]-once processing, with ordering guaranteed per [entity / topic], retaining messages for [7 / 30] days."

---

### 3c. Non-Functional Requirements

**Do not just list NFRs — derive them from the use case and call out trade-offs.**

| NFR | Typical Target | How to Derive |
|-----|---------------|---------------|
| Write latency P99 | 5 ms (acks=1), 15 ms (acks=all) | acks setting is a direct durability/latency trade-off |
| Read latency P99 | 2 ms (cache hit), 20 ms (miss) | Consumer lag determines cache hit rate |
| Throughput per broker | 600 MB/s write, 2 GB/s read | Sequential I/O + page cache + sendfile() |
| Availability | 99.99% | ISR replication RF=3, min.insync.replicas=2 |
| Durability | RPO=0 if ISR >= 2 | Losing all ISR simultaneously is the only unrecoverable failure |
| End-to-end latency | < 50 ms produce→consume | Includes batching (linger.ms), not just Kafka internals |
| DLQ write latency | < 10 ms | Must not slow down main consumer processing |
| Fan-out (1M subs) | < 30 seconds | Requires sharded fan-out, not serial HTTP calls |

**The two biggest trade-offs to name explicitly:**

**Durability vs. Throughput:** `acks=all` with `min.insync.replicas=2` gives you RPO=0 but adds ~10ms of latency. `acks=1` is fast but risks losing data if the leader crashes before replication. **acks=0** is fire-and-forget, useful only for non-critical metrics. For financial or provisioning events, always say acks=all.

**Ordering vs. Parallelism:** Stronger ordering (single partition) means no horizontal scale on the consumer side. Weaker ordering (many partitions, key-based) means massive parallelism but only within a partition. Say this explicitly in the interview: "Ordering is per-partition, not global. If two events must be ordered relative to each other, they must share a partition key."

---

### 3d. Capacity Estimation

**The formula pattern every interviewer expects:**

```
Messages/sec  x  Bytes/message  =  Bytes/sec (raw throughput)
Bytes/sec  x  Replication factor (3)  =  Total disk write rate
Total disk write rate  /  Throughput per broker (600 MB/s)  =  Broker count
Bytes/sec  x  86,400  x  Retention days  x  RF  =  Total storage
```

**Anchor numbers to memorize:**
- NVMe sequential write: ~3,000 MB/s. Random 4KB: ~400 MB/s. Sequential is **7.5x faster**.
- HDD sequential: ~200 MB/s. Random: ~0.8 MB/s. Sequential is **250x faster** — this is why append-only log design exists.
- Kafka broker throughput: **600 MB/s write, 2 GB/s read** per broker.
- Kafka acks=all P99 produce latency: **~15 ms**.
- Zero-copy sendfile() read latency (cache hit): **~2 ms**.

**Worked example (10M msgs/sec, 1 KB each):**
- Raw throughput: 10 GB/s
- With RF=3: 30 GB/s total disk writes
- Brokers needed: 30 GB/s / 600 MB/s = **50 brokers** (write-bound)
- Consumer side (5 consumer groups): 5 x 10 GB/s = 50 GB/s / 2 GB/s = 25 brokers → write-bound dominates → **60 brokers with headroom**
- Storage (7 days): 10 GB/s x 86,400 x 7 x 3 = ~18 PB (hyperscaler grade; for interviews use 500 MB/s as realistic starting point → ~900 TB)

**Architecture implications to name after the numbers:**
- If broker count > 30: need a dedicated controller quorum (KRaft) separate from broker nodes
- If partitions > 100K: be careful about partition count impact on leader election latency during cluster restarts
- If message size > 1 MB: compression ratio improves dramatically; consider LZ4 or ZSTD

**Time budget in interview:** 5-7 minutes. Don't get bogged down. Show the formula, plug in the numbers, draw the broker count conclusion, state the dominant bottleneck.

---

### 3e. High-Level Design

**The 5 canonical Kafka components to draw first:**

```
Producers → [Kafka Cluster] → Consumer Groups
                ↑
         [Schema Registry]    [Monitoring / Alerting]
                ↓
         [DLQ / Retry Topics] (failure path)
```

**4-6 components and their roles:**

1. **Producer** — serializes and batches messages; picks partition via `murmur2(key) % numPartitions`; configured with acks=all + idempotent producer enabled
2. **Kafka Broker cluster** — each broker holds a subset of partition replicas; one replica per partition is the leader (serves all reads/writes); followers replicate from leader
3. **Controller (KRaft)** — manages cluster metadata via Raft consensus; handles leader election on broker failure; replaced ZooKeeper in modern Kafka
4. **Consumer Group** — each consumer in a group owns a disjoint subset of partitions; group coordinator (a broker) manages offset commits and rebalancing
5. **Retry/DLQ Topics** — parallel failure path; failed messages written here instead of blocking main consumer
6. **Monitoring** — Prometheus scrapes broker metrics (consumer lag, ISR shrink, disk bytes); PagerDuty alerts on lag > threshold

**Data flow — describe it in order:**

*Produce path:* Producer batches records per partition (linger.ms=5, batch.size=64KB) → sends to partition leader → leader writes to active segment file (page cache) → followers fetch and acknowledge → leader advances High Watermark → producer gets ack.

*Consume path:* Consumer sends FetchRequest(topic, partition, offset) → leader checks if data is in page cache → if yes: sendfile() zero-copy DMA to NIC → consumer processes records → commits offset to `__consumer_offsets` internal topic.

*Failure path:* Consumer catches exception → checks error type (retryable/non-retryable) → if retryable: re-publishes to retry topic with exponential delay → if non-retryable or retries exhausted: publishes to DLQ topic with error headers attached.

**Key decisions to call out explicitly (don't wait to be asked):**

- "I'm using acks=all with min.insync.replicas=2 and RF=3. This gives RPO=0 as long as at least 2 brokers survive. The trade-off is ~10ms added latency over acks=1."
- "I'm partitioning by entity ID (order ID, server ID) so all events for a given entity land on the same partition and are processed in order."
- "I'm using non-blocking retry topics instead of in-memory retries to avoid freezing the consumer while waiting for backoff."

**Whiteboard drawing order:** (1) Draw the producer and Kafka box. (2) Add consumer groups. (3) Add the failure path (retry topics → DLQ). (4) Add Schema Registry if relevant. (5) Add monitoring. Explain as you draw — don't draw in silence.

---

### 3f. Deep Dive Areas

Interviewers will probe these three areas most aggressively. Know each cold.

**Deep Dive 1: ISR, HW, and the durability/availability balance**

The problem: if you wait for all replicas to acknowledge, a single slow follower can add 30-second+ latency spikes. If you don't wait for any replica, you lose data on leader crash.

Kafka's solution is the **In-Sync Replica (ISR) set** — a dynamic subset of replicas that are "caught up" with the leader, defined as: any follower that hasn't sent a FetchRequest within `replica.lag.time.max.ms=30s` gets removed from ISR. The **High Watermark (HW)** is the minimum LEO (Log End Offset) across all ISR replicas. Only messages at or below HW are visible to consumers.

The key config combination: `acks=all` + `min.insync.replicas=2` + `RF=3`. This means: the leader waits for all ISR replicas to ack; but if ISR shrinks below 2, the produce fails with `NOT_ENOUGH_REPLICAS` error. This prevents `acks=all` from silently degrading to `acks=1` when followers fall behind.

✅ If ISR >= 2 replicas: RPO = 0 (no acknowledged data lost)
❌ If all ISR crash simultaneously (very unlikely): partition goes offline until a replica recovers
❌ Setting `unclean.leader.election.enable=true` allows an out-of-sync replica to become leader — recovers availability but may lose committed messages. Only acceptable for non-critical topics like metrics.

**The interviewer follow-up you must expect:** "What happens if the leader crashes and the new leader's HW is lower than the old leader's LEO?" Answer: The new leader starts from its own LEO. Messages that were on the old leader but not yet replicated were never committed (producer hadn't received ack), so no consumer ever saw them. Producers for those messages will retry (idempotent producer deduplicates). No committed data is lost.

**Deep Dive 2: Transactional Outbox — eliminating the dual-write race condition**

The problem: a service wants to write to MySQL AND produce to Kafka atomically. The naive approach — write to DB then write to Kafka — has a fatal gap: if the service crashes between the two writes, the Kafka event is never published. The business state says "order created" but no downstream service ever heard about it.

Two-phase commit (2PC) across MySQL and Kafka is theoretically possible but adds coordinator complexity, reduces throughput, and is almost never done in practice.

The **transactional outbox pattern** solves this with a single local transaction:

```
BEGIN;
  INSERT INTO orders (...);
  INSERT INTO outbox (event_id, topic, payload, status='PENDING');
COMMIT;
```

The outbox is in the same database as the business data. A separate **polling publisher** reads `SELECT WHERE status='PENDING' LIMIT 1000` every 100ms and produces to Kafka. On Kafka ACK, it marks rows as PUBLISHED. If the service crashes between polling and marking, the same rows get published again on the next poll — at-least-once to Kafka. Consumers deduplicate via Redis `SET NX event_id` with 24h TTL.

The **CDC variant** using Debezium instead of polling publisher: Debezium reads the MySQL binlog (which already captured the outbox INSERT as part of the committed transaction) and produces it to Kafka. Latency drops from <500ms to <200ms. But it adds operational complexity: you need Debezium running and a Kafka Connect cluster.

✅ Outbox polling: simple to implement, <500ms latency, works with any DB
✅ CDC (Debezium): <200ms latency, no polling load on the database
❌ Polling adds database load (mitigate with an index on status,created_at)
❌ CDC requires Debezium + Kafka Connect operational overhead

**Deep Dive 3: Fan-out at scale — the 1M subscriber problem**

The problem: a single Kafka consumer group can efficiently handle maybe 10,000 consumer groups before metadata overhead becomes significant. For topics with 1M subscribers, you can't create 1M consumer groups.

The answer is **tiered fan-out:**

- **Pull subscribers (≤ 1,000/topic):** Direct Kafka consumer group per subscription. Lowest latency (<50ms). Scales to roughly 10K total consumer groups per cluster.
- **Push subscribers (application-level fan-out):** One Kafka consumer group per topic (`fanout-{topic_id}`). A fan-out service reads each message once and fans it out to N subscriber endpoints via parallel HTTP calls.
- **Broadcast fan-out (1M+ push subscribers):** Shard the subscriber list. Each shard (10K subscribers) gets its own fan-out worker. 100 shards x 10K HTTP calls each = 1M deliveries/sec. Each shard maintains its own HTTP connection pool.

The fan-out service must evaluate **filter expressions** per subscription before delivering — if a subscriber's filter doesn't match the message attributes, skip the HTTP call. At 1M subs with 95% filtering, you might do 50K actual deliveries instead of 1M.

✅ Pull: lowest latency, simplest, uses native Kafka
✅ Sharded push fan-out: scales to arbitrary subscriber counts
❌ Pull subscriber count is bounded by Kafka consumer group scaling limits
❌ Push fan-out service becomes a bottleneck if not horizontally sharded

---

### 3g. Failure Scenarios

Walk through these when the interviewer asks "what could go wrong" — or proactively, once you finish the HLD.

**Scenario 1: Leader broker crashes mid-write**
- With `acks=all` and `min.insync.replicas=2`: the producer's inflight request gets an error (or times out). Producer retries (idempotent producer prevents duplicate). ISR elects new leader within seconds. RPO = 0.
- With `acks=1`: messages written to leader page cache but not yet replicated to followers may be lost. The new leader never had them. This is why acks=1 is dangerous for financial events.

**Scenario 2: Consumer crashes after processing but before offset commit**
- Consumer group coordinator detects missing heartbeat within `session.timeout.ms=30s`. Partition is reassigned to another consumer.
- New consumer re-reads from the last committed offset — the message is reprocessed. This is at-least-once. You handle it on the consumer side with Redis `SET NX event_id` dedup.

**Scenario 3: Poison pill message causes consumer crash loop**
- Consumer crashes on a specific message, restarts, reads same message (offset not committed), crashes again. Repeat forever.
- Detection: track `{group, topic, partition, offset}` → failure count in Redis. After 3 failures at the same offset, classify as poison pill.
- Response: isolate the message (route to DLQ, advance offset), alert the on-call team, quarantine the message key.

**Scenario 4: Downstream service is down during push delivery**
- Fan-out service HTTP call gets 503.
- Retry with exponential backoff (1s, 5s, 30s).
- After max retries: route to subscription's DLQ (not Kafka DLT — this is a delivery-level DLQ, different from processing DLT).
- When downstream recovers: bulk replay from DLQ filtered by error class and time window.

**Scenario 5: Outbox poller falls behind during a traffic spike**
- Outbox table accumulates PENDING rows. Alert on outbox table size > threshold.
- Scale horizontally: multiple polling publisher instances, each claiming a range of rows with optimistic locking.
- Alternatively: switch to CDC mode (Debezium) which has no polling lag — it reads binlog events as they're committed.

**Senior framing for failure scenarios:** Don't just say "use a DLQ." Walk through the failure mode precisely: what state is corrupted, how does the system detect it, what is the automated recovery path, and what requires human intervention. Interviewers at L6+ want to see you distinguish between self-healing failures and page-worthy incidents.

---

## STEP 4 — COMMON COMPONENTS

These nine components appear across all five problems. Know each cold.

---

### 1. Apache Kafka — Core Log Storage and Transport

**Why used:** Every problem uses Kafka as the backbone because it provides three properties simultaneously: durability (data persisted to disk with replication), ordering (guaranteed per partition), and replayability (consumers can seek to any offset at any time). No other system provides all three at this throughput level.

**Key config:**
- `replication.factor=3` — three copies of every partition across three brokers
- `acks=all` + `min.insync.replicas=2` — RPO=0 as long as 2 brokers survive
- `retention.ms` — how long messages live (7 days standard; 30 days for DLQ topics)
- `compression.type=lz4` — 30-70% compression, negligible CPU cost
- KRaft mode — no ZooKeeper dependency; Raft-based controller consensus

**What breaks without it:** Without Kafka, you have no durable ordered log. You'd need a database queue table, which maxes out at ~10K inserts/sec before I/O becomes the bottleneck. You lose replayability entirely, and multiple consumers reading the same data requires application-level fan-out.

---

### 2. ISR-Based Replication (In-Sync Replicas)

**Why used:** ISR is Kafka's answer to the CAP theorem tension between consistency and availability. Unlike quorum-based replication (Raft/Paxos where you always need majority), ISR dynamically shrinks — a slow follower is removed from ISR rather than blocking the entire partition.

**Key config:**
- `replica.lag.time.max.ms=30s` — follower is removed from ISR if it hasn't fetched within this window
- `unclean.leader.election.enable=false` — (default) only ISR members can become leader; prevents data loss
- High Watermark advances only when all ISR replicas have acknowledged

**What breaks without it:** Without ISR, you choose between "wait for all replicas" (any slow follower blocks all writes) or "don't wait" (leader crash loses data). ISR gives you a dynamic middle ground.

---

### 3. Consumer Groups for Independent Subscription

**Why used:** Multiple independent downstream services need to consume the same event stream without coordinating with each other. Each consumer group maintains its own offset in `__consumer_offsets`. Adding a new consumer group never affects other groups.

**Key config:**
- One consumer per partition maximum (within a group) — partition count caps parallelism
- `enable.auto.commit=false` + manual `commitSync()` — for at-least-once processing
- `max.poll.interval.ms=300000` — processing budget before consumer is kicked from group
- `group.instance.id` — static membership avoids rebalance on controlled restarts

**What breaks without it:** Without consumer groups, you'd need one physical copy of the data per consumer — massive storage waste. Or you'd need a centralized dispatcher that becomes a bottleneck and a single point of failure.

---

### 4. Partition-Based Ordering via Message Key

**Why used:** Kafka only guarantees ordering within a partition. To guarantee that all events for a given order/server/user arrive in the order they were produced, you must route them to the same partition using a consistent key.

**Key formula:** `partition = murmur2(key) % numPartitions`

**What breaks without it:** Without a partition key, records are distributed round-robin across partitions. Two events for the same order might be processed by two different consumers in any order. For state machine transitions (CREATED → PROCESSING → COMPLETED), wrong order produces invalid state.

---

### 5. Producer Batching (batch.size + linger.ms)

**Why used:** Each network round-trip and each disk write has fixed overhead. Batching amortizes that overhead across many records. A 64KB batch with LZ4 compression achieves 5-10x the throughput of single-record produces.

**Key config:**
- `linger.ms=5` — wait up to 5ms to accumulate a batch before sending
- `batch.size=65536` — 64KB max per partition per batch
- `compression.type=lz4` — fast compression; the CPU savings from fewer bytes exceed the CPU cost of compression

**What breaks without it:** Without batching, at 10M msgs/sec you'd be making 10M individual Kafka produce calls/sec. The network and disk I/O overhead alone would require 50x more brokers.

---

### 6. Idempotent Producer + Exactly-Once Semantics

**Why used:** Kafka's TCP connection can drop between a successful broker write and the producer's ACK. Without idempotency, the producer retries and creates a duplicate. The idempotent producer assigns a monotonically increasing sequence number per partition. The broker deduplicates by tracking `(producerId, partition, sequenceNumber)`.

**Key config:**
- `enable.idempotence=true` — enables sequence numbers; safe to use with `max.in.flight.requests.per.connection=5`
- Transactional producer — for atomic multi-partition writes + consumer offset commits as a single atomic operation

**What breaks without it:** Without idempotency, retrying a failed produce creates duplicate messages. At exactly-once in stream processing (Flink), without transactional producers you get duplicate output records on job recovery.

---

### 7. Message Headers for Metadata

**Why used:** Headers carry metadata (schema ID, trace ID, original topic, error class) without inflating the message payload or affecting partitioning. Every system in this cluster uses headers differently.

**Examples:**
- DLQ: `kafka_dlt-original-topic`, `kafka_dlt-exception-fqcn`, `kafka_dlt-retry-count`
- Event streaming (Confluent wire format): first 5 bytes of value = `0x00` (magic) + 4-byte schema ID
- Reliable delivery: `event_id`, `event_type`, `aggregate_type`, `aggregate_id` as headers
- Pub/Sub: `message_id` (UUIDv7), `severity`, `region` as filter-evaluation attributes

**What breaks without it:** Without headers, metadata must be embedded in the payload. This means every consumer must parse the payload to route, filter, or classify the message. Deserialization cost, schema coupling.

---

### 8. Retry with Exponential Backoff + Dead Letter Queue

**Why used:** Transient failures (network timeout, downstream 503) should be retried. But retrying must not block the consumer from processing other messages on the same partition. Non-blocking retry topics (orders.retry.0, orders.retry.1...) decouple the retry wait from the main partition consumer.

**Retry progression (5 levels):**
```
Attempt 1 → orders.retry.0  (delay: 1s)
Attempt 2 → orders.retry.1  (delay: 5s)
Attempt 3 → orders.retry.2  (delay: 30s)
Attempt 4 → orders.retry.3  (delay: 60s)
Attempt 5 → orders.retry.4  (delay: 300s)
Final     → orders.DLT       (30-day retention)
```

The retry consumer checks `now < message.timestamp + retryDelay`. If too early, it pauses the partition and schedules a resume — no `Thread.sleep`, no blocking.

**Error classification matters:**
- `RETRYABLE_TRANSIENT` (ConnectException, SocketTimeout) → retry with backoff
- `RETRYABLE_RATE_LIMITED` (HTTP 429) → retry with longer backoff
- `NON_RETRYABLE_VALIDATION` (NullPointerException, invalid enum) → immediate DLQ, no retries
- `NON_RETRYABLE_SCHEMA` (DeserializationException) → immediate DLQ
- `POISON_PILL` → quarantine, alert

**What breaks without it:** Without non-blocking retry topics, a message that needs 30 seconds of backoff freezes the entire partition for 30 seconds. At 100 msgs/sec per partition, that's 3,000 messages of lag accumulated per transient failure.

---

### 9. Redis for Idempotency and Subscription Caching

**Why used:** Consumer-side deduplication requires a fast, TTL-expiring key-value store. Redis `SET NX` (set if not exists) is the perfect primitive: O(1), sub-millisecond, atomic. TTL ensures the dedup window is bounded (24h by default — must exceed Kafka retention period for replay safety).

**Key pattern:** `idempotency:{consumer_group}:{event_id}` → `PROCESSING | COMPLETED | FAILED` with TTL=24h

**Also used for:** Subscription metadata cache in pub/sub (avoids MySQL round-trip per message for filter evaluation), fan-out service's subscriber list per topic.

**What breaks without it:** Without Redis dedup, at-least-once becomes many-times-once during outages. A 2-minute downstream outage causes all consumers to retry, and every service that consumed the same message twice might double-charge customers or provision duplicate servers.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Dead Letter Queue System

**What makes this unique:** This is the only problem focused entirely on the *failure path*. The main Kafka pipeline is assumed to exist. You're designing the operator-facing system for inspecting, understanding, and recovering from processing failures. The novel components are: the Elasticsearch-backed inspection UI, the replay API, and poison pill detection.

**Unique decisions not in other problems:**
- Non-retryable exceptions bypass retry topics entirely and go straight to DLT (saves retry latency for unfixable messages)
- Poison pill detection tracks failure count per `{group, topic, partition, offset}` in Redis — after 3 failures at the same offset, auto-quarantine
- DLQ messages have a richer data model: original payload, error class, stack trace, retry count, and a `replay_status` field (PENDING / REPLAYED / SKIPPED)
- Replay is a deliberate operator action: filter by error class + time window, optionally edit payload, produce back to source topic
- Circuit breaker integration: if downstream is persistently unavailable (many ConnectExceptions), stop retrying and DLQ immediately

**Two-sentence differentiator:** The DLQ system is the failure observability and recovery plane — it transforms message failures from a black hole into an actionable operator workflow. Every other system uses Kafka's normal happy path; this one is specifically designed around what happens when that happy path fails permanently.

---

### Distributed Message Queue

**What makes this unique:** This is the only problem asking you to design Kafka *itself from scratch* — the internal mechanics, not just how to use it. The interview is testing whether you understand ISR/HW/LEO, segment files, zero-copy sendfile(), KRaft, and consumer group rebalancing at an implementation level.

**Unique decisions not in other problems:**
- KRaft mode: the controller is a Raft quorum of brokers managing cluster metadata; no ZooKeeper; faster failover
- Segment file structure: `.log` (raw data) + `.index` (sparse offset→position, memory-mapped) + `.timeindex` (timestamp→offset); binary search on index then sequential scan in .log
- Zero-copy sendfile(): kernel DMA from page cache directly to NIC buffer — data never enters JVM heap; 2 GB/s read throughput vs ~400 MB/s with heap copies
- Leader epoch: each election increments epoch; stale leaders check epoch and truncate divergent log entries on rejoin (prevents split-brain)
- Log compaction: background cleaner thread deduplicates by key, keeps only latest value per key; enables changelog topics for stream processing state

**Two-sentence differentiator:** Unlike the other four problems which use Kafka as an external dependency, this problem is Kafka — you need to explain the internal I/O model (why sequential append + page cache + sendfile() achieves 600 MB/s write and 2 GB/s read), the replication protocol (LEO/HW/ISR), and the coordination layer (KRaft). If an interviewer asks you to "design a distributed message queue," this is the answer.

---

### Event Streaming Platform

**What makes this unique:** This problem adds three layers that don't exist in raw Kafka: schema management (Schema Registry), database change capture (Debezium CDC), and stateful stream processing (Flink with exactly-once guarantees). The complexity is in the pipeline — not just getting bytes from A to B, but transforming and joining them in real time.

**Unique decisions not in other problems:**
- Schema Registry backed by the `_schemas` Kafka topic (compacted, RF=3) — stateless service, no external database needed
- Confluent wire format: 5-byte header (`0x00` magic byte + 4-byte schema ID) prepended to every Avro message; allows compact serialization without embedding the full schema
- Schema compatibility: BACKWARD (default, add fields with defaults), FORWARD (remove fields), FULL (both). Breaking changes (rename a field, remove a required field) require a new subject or schema migration.
- Flink checkpoint barriers: JobManager injects a barrier into source streams; when an operator receives barriers from ALL inputs, it snapshots state to S3 and forwards the barrier downstream; on failure, restore state from checkpoint + reset Kafka offsets to checkpoint position — exactly-once with no manual dedup
- Debezium CDC vs. polling publisher: Debezium reads MySQL binlog (<200ms latency); polling publisher hits the DB every 100ms (<500ms latency); CDC is lower latency but operationally heavier

**Two-sentence differentiator:** The event streaming platform builds a real-time operational intelligence layer on top of Kafka — CDC pipelines make database state changes immediately observable as events, the Schema Registry enforces compatibility contracts across 5,000 event types and 100,000 schema versions, and Flink provides exactly-once stateful transformations with sub-second latency. This is the design you draw when the requirement is "I need to know what's happening in my system in real time."

---

### Pub/Sub Notification System

**What makes this unique:** This is the only problem where a single message must be delivered to potentially 1 million different endpoints. The core challenge is fan-out architecture — Kafka's native consumer group model doesn't scale to 1M consumer groups. You need an application-level fan-out tier.

**Unique decisions not in other problems:**
- Two delivery modes per subscription: PULL (Kafka consumer group, lowest latency) and PUSH (HTTP webhook, fan-out service delivers)
- Tiered fan-out: ≤1,000 pull subscribers → native Kafka consumer groups; push subscribers → one consumer group per topic feeds a fan-out service; broadcast (1M+) → sharded fan-out (1 shard per 10K subscribers, 100 shards for 1M)
- Filter expressions evaluated per subscription per message: `"severity='CRITICAL' AND region='us-east-1'"` — only deliver to subscribers whose filter matches the message attributes
- Ordering keys: messages with the same ordering key on a subscription are delivered in the order they were published — enforced by sticky partition assignment within the fan-out service
- Message TTL: undelivered messages expire after configurable duration; prevents stale notifications from accumulating when a subscriber is down

**Two-sentence differentiator:** Pub/Sub is Kafka with a subscription management layer on top, where the hard problem is not storage or throughput but fan-out — routing one message to N heterogeneous endpoints (some polling, some webhooks) with filter semantics, at scales where 1 billion deliveries per second during a broadcast is a real number. The tiered fan-out architecture (pull→CG, push→fan-out service, broadcast→sharded fan-out) is the key design insight.

---

### Reliable Event Delivery System

**What makes this unique:** This is the only problem that starts before Kafka — with the dual-write problem at the database layer. The question isn't how to design Kafka, it's how to atomically write to a relational database AND guarantee that a Kafka event is published, without 2PC, even across service crashes.

**Unique decisions not in other problems:**
- Transactional outbox: outbox table in same database as business entities; `BEGIN; INSERT business; INSERT outbox; COMMIT;` — single local transaction, no distributed coordination
- Dual publishing strategies: polling publisher (simple, <500ms) vs. Debezium CDC (lower latency, <200ms, but requires Debezium)
- Idempotent consumer pattern: Redis `SET NX "idempotency:{group}:{event_id}"` with 24h TTL — converts Kafka's at-least-once into exactly-once at the application layer
- Saga orchestration: multi-service transactions modeled as a series of command/reply events; on step failure, execute compensating transactions in reverse order; `saga_instance` + `saga_step` tables in MySQL track state
- Outbox cleanup: hourly job deletes published rows (PUBLISHED and created_at < now - 1h); keeps outbox table small to maintain polling performance

**Two-sentence differentiator:** Reliable event delivery is the problem of crossing the DB-to-broker boundary atomically without 2PC — the transactional outbox pattern is the canonical answer, and this design shows both publishing strategies (polling vs CDC) plus the full consumer-side deduplication chain. The saga orchestration layer extends this to multi-service transactions where partial failures require coordinated compensation across multiple services.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (what interviewers ask first)

**Q: What is the difference between a topic and a partition in Kafka?**

A: A **topic** is a logical namespace — the name you produce to and consume from, like "orders" or "provisioning-events." It doesn't have a physical existence of its own. A **partition** is the actual physical unit: an append-only log file on a broker's disk, with a strictly ordered sequence of records. A topic is divided into N partitions, each stored on a different broker (for the leader replica). Producers write to the leader partition. Consumers in a group each own a subset of partitions. Ordering is guaranteed within a partition but not across partitions.

**Q: What does "at-least-once delivery" mean, and how does Kafka achieve it?**

A: **At-least-once delivery** means every message will be delivered to the consumer at least one time, but possibly more. In Kafka, this works because: consumers commit their offset (position) after processing, not before. If a consumer processes a message and then crashes before committing the offset, the coordinator reassigns the partition and the next consumer starts from the last committed offset — which is before the just-processed message. That message gets processed again. To convert at-least-once into exactly-once at the application layer, consumers use Redis `SET NX event_id` to skip duplicate processing.

**Q: What is the High Watermark (HW) in Kafka, and why does it matter?**

A: The **High Watermark** is the minimum Log End Offset (LEO) across all In-Sync Replicas (ISR) for a partition. Consumers can only read messages at or below the HW. This prevents a consumer from reading data that might be lost if the current leader fails and a new leader (with fewer records) takes over. When you use `acks=all`, the producer's write is acknowledged only after the HW advances past the new record — guaranteeing the record is on all ISR replicas before the producer is told it succeeded.

**Q: Why does Kafka use an append-only log instead of a traditional database?**

A: The append-only design allows Kafka to exploit sequential I/O. On NVMe SSDs, sequential writes achieve ~3 GB/s; random 4KB writes achieve ~400 MB/s — a 7.5x difference. On HDDs the difference is 250x. The OS page cache means that writes go to DRAM first, not disk synchronously (replication provides durability instead). For reads, because consumers almost always read near the tail of the log, the data is already in page cache. Kafka uses `sendfile()` (zero-copy) to DMA from page cache directly to the NIC buffer, never touching the JVM heap — achieving 2 GB/s read throughput per broker.

**Q: What is a consumer group, and how does it compare to competing consumers on a queue?**

A: A **consumer group** is a set of consumers that collaboratively consume a topic, with each partition assigned to exactly one consumer in the group. Adding consumers to a group up to the partition count increases parallelism. Multiple groups can consume the same topic independently — each group has its own offset cursor and processes every message. This is fundamentally different from a traditional queue: a queue gives each message to exactly one consumer across all subscribers. Kafka's model lets you have a "billing" group and an "analytics" group both consuming the same "orders" topic without any coordination between them.

**Q: What is the transactional outbox pattern and why is it needed?**

A: The **transactional outbox** solves the dual-write problem: you can't atomically write to a database and produce to Kafka (they're different systems). If you write to DB first and then produce to Kafka, a crash in between means the Kafka event is never published. The solution: add an `outbox` table to your service's own database. In the same business transaction, write your business record and an outbox row. A separate publisher reads the outbox and produces to Kafka. On successful Kafka ACK, it marks the outbox row as published. Because the outbox is in the same database as the business data, the write is atomic. Duplicate publication is handled by idempotent consumers.

**Q: What is the difference between push and pull delivery in pub/sub?**

A: In **pull delivery**, the subscriber polls the broker at its own pace (Kafka consumer calling `poll()`). The subscriber controls the rate, can pause, and handles backpressure naturally. In **push delivery**, the broker (or a fan-out service) actively calls the subscriber's endpoint (HTTP POST to a webhook URL). Push is lower effort for the subscriber but requires the subscriber's endpoint to be reachable, handle backpressure, and respond with 2xx. For exactly-once with push: the delivery service tracks acks, retries on failure, and maintains a per-subscription DLQ.

---

### Tier 2 — Deep Dive Questions (15-30 minutes into the interview)

**Q: Why does Kafka use ISR instead of a quorum-based approach like Raft?**

A: With Raft (RF=3), you need 2/3 acks — you always wait for the second replica to respond. **ISR is more adaptive**: slow replicas are removed from ISR, so `acks=all` doesn't wait for them. If only the leader is in ISR (degenerated case), `acks=all` effectively becomes `acks=1`. You prevent this with `min.insync.replicas=2`: if ISR shrinks below 2, produce fails rather than silently degrading. The practical difference: ISR handles GC pauses gracefully (the JVM replica is temporarily removed from ISR, not a quorum member that must be replaced). The trade-off: ISR can have data loss if `unclean.leader.election.enable=true` and an out-of-sync replica becomes leader. Raft's majority quorum prevents that but requires a larger quorum for the same fault tolerance (RF=5 for 2 failures vs RF=3 with ISR).

**Q: Walk me through exactly what happens — step by step — when a producer sends a message with acks=all.**

A: (1) Producer serializes the record, computes `partition = murmur2(key) % numPartitions`. (2) Producer adds the record to a per-partition batch buffer. (3) If `linger.ms` expires or `batch.size` fills, the batch is sent to the partition leader broker. (4) Leader receives the ProduceRequest, appends the record batch to the active `.log` segment in the OS page cache. (5) Leader updates its own LEO. (6) Follower brokers are continuously sending FetchRequests to the leader; they fetch the new records and append to their own segment files, updating their LEO. (7) Leader tracks each follower's LEO. When all ISR followers have fetched the record (their LEO >= record offset), the leader advances the High Watermark. (8) Leader sends ProduceResponse back to the producer with the assigned base offset and success code. (9) Producer's callback fires with the metadata. **The critical point**: the producer is blocked (or its callback is pending) until step 8. If any ISR follower is unreachable, the leader waits until `request.timeout.ms` expires, then returns an error.

**Q: How do you achieve exactly-once semantics across a database write and Kafka — without 2PC?**

A: The canonical answer is **transactional outbox + idempotent consumer**. On the write side: in a single local DB transaction, write the business entity and an outbox row. A polling publisher or CDC connector reads the outbox and produces to Kafka. This is at-least-once to Kafka (the same row may be published twice if the publisher crashes between Kafka ACK and marking the row published). On the consumer side: Redis `SET NX "idempotency:{group}:{event_id}"` with 24h TTL. If SET NX returns false (key already exists), the message was already processed — skip it. The result is exactly-once at the application layer. The TTL must be longer than the Kafka retention period to be safe during replay. This avoids any distributed transaction protocol.

**Q: What happens during a Kafka consumer group rebalance, and how does cooperative rebalancing help?**

A: In **eager rebalancing** (original behavior): when any consumer joins or leaves, ALL partitions are revoked from ALL consumers simultaneously. Processing stops entirely. The group coordinator waits for all consumers to rejoin, runs partition assignment, and all consumers receive their new assignments. This creates a "stop-the-world" pause — in a large group with thousands of partitions, this can last seconds. **Cooperative sticky rebalancing** (preferred): only partitions that need to change ownership are revoked. The rest keep processing. It's a two-phase protocol: Phase 1, revoke partitions that are moving. Phase 2, assign them to new owners. The net effect is that a consumer joining the group only causes 1/N of the partitions to briefly stop, not all of them. **Static group membership** adds another optimization: if a consumer restarts quickly with the same `group.instance.id`, it rejoins without triggering a rebalance at all (the coordinator assumes it's the same instance returning).

**Q: How does Flink achieve exactly-once processing with Kafka as both source and sink?**

A: Flink uses **checkpoint barriers** injected by the JobManager into the source streams. A barrier flows downstream through the operator DAG like a regular event. When an operator receives a barrier from ALL of its input channels simultaneously, it: (1) snapshots its local state (counters, aggregation accumulators, running joins) to S3 via asynchronous checkpointing, (2) forwards the barrier to downstream operators. When all sink operators have snapshotted their state AND committed their Kafka offsets, the checkpoint is complete. On job failure: Flink restores all operator state from the latest complete checkpoint AND resets all Kafka consumer offsets to the position recorded in that checkpoint. Then it replays events from that offset. Because the state and offsets are checkpointed atomically, there are no duplicates — the state already reflects exactly the events up to the checkpoint boundary.

**Q: How do you handle a downstream service being down — how does it affect your message processing pipeline, and what is your recovery strategy?**

A: Three mechanisms work together. **Circuit breaker**: if the downstream starts returning sustained 5xx, stop attempting delivery immediately and route all affected messages to the DLQ. This prevents retry amplification (hundreds of consumers all retrying simultaneously against a down service). **Non-blocking retry topics**: transient errors (not circuit-broken) are re-published to retry topics with exponential backoff (1s, 5s, 30s...) — the main consumer keeps processing other messages without waiting. **DLQ + bulk replay**: once the downstream recovers, run a bulk replay job: filter DLQ messages by error class (ConnectException, 503) and time window (during the outage), reproduce them to the source topic at a controlled rate (below downstream's steady-state capacity, e.g., 50% of normal throughput) to avoid immediately overwhelming the recovering service.

**Q: How would you debug a situation where consumer lag is growing on a production Kafka topic?**

A: Structured investigation: (1) **Is it a throughput problem or a processing problem?** Check `kafka-consumer-groups.sh --describe` for the affected group. Is the LAG growing uniformly across all partitions, or only some? Uniform growth usually means consumer throughput is below producer throughput (scale consumers or reduce processing cost). Per-partition growth suggests a specific consumer instance is stuck. (2) **Is a consumer stuck?** Check consumer logs for exceptions, long GC pauses, or deadlocks. Check `max.poll.interval.ms` — if processing takes longer than this, the consumer is kicked from the group and partitions are unassigned, causing lag. (3) **Is there a poison pill?** If one partition's lag is spiking while others are fine, check for repeated exceptions at the same offset. (4) **Is the DLQ filling up?** If retry topics are growing, the downstream is overloaded or down. Check the downstream service's health. (5) **Alert thresholds**: set Prometheus alert on consumer lag > X seconds of data (lag_bytes / bytes_per_second), not just lag count (message count doesn't account for variable message sizes).

---

### Tier 3 — Staff+ Stress Test Questions (reason aloud)

**Q: A critical payment processing consumer has been in a crash loop for 20 minutes. Consumer lag is at 50 million messages. How do you recover, and what are the risks of each recovery path?**

A: Think through this in layers. First, **stop the bleeding**: deploy the poison pill detector (if not already in place) — it tracks failure count per offset and auto-quarantines after 3 crashes. This unblocks the partition immediately. Second, **assess the root cause**: is this a bad deployment (fix the code and redeploy) or bad data (a specific message is malformed)? Pull the message at the stalled offset using `kafka-dump-log.sh` or the DLQ Dashboard. Third, **decide on recovery path**. Option A (safest): fix the code, redeploy, let consumers catch up naturally from the 50M message backlog. This is safe but slow — if processing rate is 1M/sec, that's 50 seconds of recovery, but during that 50 seconds you have ordering guarantees and no message is skipped. Option B (faster): skip the bad messages into the DLQ using the DLQ replay service (advance offset past the bad message, manually quarantine it), then let consumers process normally. This unblocks immediately but you've deferred the bad message handling. Option C (dangerous): reset consumer offset to now (discard all 50M messages). This is only acceptable if you have another source of truth (event sourcing, database) that can reconcile the skipped messages. For payment processing, option C is almost never acceptable. **The risk the interviewer is looking for you to name**: if you bulk-advance the offset past "bad" messages without inspecting them, you might also skip valid payment events in the middle of the backlog. Never skip ranges — skip specific offsets after confirming they're the bad ones.

**Q: Your event streaming platform has 5,000 distinct event types, each with up to 20 schema versions in production simultaneously. A team deploys a breaking schema change (removes a required field without a default). What happens, and how do you prevent this at the infrastructure level?**

A: Without guardrails, this is catastrophic. Consumers with the old schema try to deserialize events produced with the new schema. The field is missing. `DeserializationException` fires. Messages flood the DLQ. Downstream services start failing silently because they can't process events. **What should have prevented this**: the Schema Registry has a compatibility check API. Before registering a new schema version, the CI/CD pipeline must call `POST /compatibility/subjects/{subject}/versions/latest`. If the subject is configured as `BACKWARD` (the default), the registry checks: can a consumer using the old schema still deserialize a message produced with the new schema? Removing a required field fails this check. The CI pipeline blocks the deployment. **Enforcing this**: the producer's Avro serializer can be configured with `auto.register.schemas=false` (forces explicit pre-registration) and `use.latest.version=true`. The registry's global compatibility mode is set to `BACKWARD`. Sensitive topics (payment events, provisioning commands) use `FULL_TRANSITIVE` compatibility — every version must be compatible with every previous version, not just the latest. **If it gets through anyway** (someone bypassed the check, perhaps in a hotfix): the DLQ receives a burst of deserialization failures. The DLQ alert fires. On-call engineer sees all failures are `NON_RETRYABLE_SCHEMA` class from one topic. They identify the bad schema version in the registry, delete it (soft delete), and force producers to roll back. DLQ messages with bad payloads are inspectable but not replayable — they were encoded with a schema that no longer exists in the registry.

**Q: You're asked to design a Kafka-based system that needs global ordering — all events from all partitions must be consumed in the exact order they were produced, across all producers. How do you approach this, and what are the hidden costs?**

A: First, **challenge the requirement**: global ordering in a distributed system at any meaningful throughput is essentially incompatible with availability and horizontal scale. This is the CAP theorem applied to messaging. Make the interviewer articulate what "global ordering" actually means for their use case. Most of the time, the real requirement is: "all events for the same entity (same order ID, same server ID) must be in order relative to each other." That's **per-key ordering**, which Kafka handles naturally via partition routing. If they truly need global ordering: **Option A (single partition)**: put all traffic on one topic with one partition. Single writer, single leader broker. You get global total order but zero horizontal scale — throughput is capped at ~100-300 MB/s for that partition. No fault tolerance during leader election gaps. This might be acceptable for a control plane with very low throughput (100 events/sec). **Option B (sequence number + sorter layer)**: assign a monotonically increasing global sequence number at the producer using a distributed counter (Redis INCR, or a Zookeeper-based sequencer). Produce to multiple partitions with the sequence number in the header. A downstream sorter service buffers records and emits them in sequence order. This achieves ordering but adds significant latency (the sorter must wait for all sequences to fill in before it can emit), and a gap in the sequence (producer crash) can stall the sorter indefinitely. **Hidden costs the interviewer wants you to name**: global ordering removes all parallelism, introduces a single sequencer as a SPOF, dramatically increases latency (must buffer until the sequence is complete), and makes horizontal scaling impossible. The right answer in almost every real system is to redesign the requirement to be per-entity ordered instead of globally ordered.

**Q: You need to replay 30 days of events for a new downstream service that was added to the system. The topic has 10 TB of data, 1,000 partitions, and you need replay to complete in under 4 hours. Walk through how you'd execute this and what could go wrong.**

A: This is a resource planning and operational execution problem. **The math first**: 10 TB in 4 hours = 2.5 TB/hour = ~694 MB/s required read throughput. Kafka's read throughput per broker is ~2 GB/s (with page cache hit). For historical data (not in cache), it's I/O bound at disk read speed (~3 GB/s NVMe sequential). So the read side can handle this — it's the consumer processing side and the destination that need to be sized. **Execution plan**: (1) Create a new consumer group for the replaying service with `auto.offset.reset=earliest`. (2) Start with high consumer parallelism: 1,000 partitions = 1,000 consumer threads maximum. If each consumer processes at 1 MB/s, 1,000 consumers = 1 GB/s throughput, completing in ~2.8 hours with headroom. (3) Monitor consumer lag in real time: `kafka-consumer-groups.sh --describe` shows per-partition lag. (4) Adjust `max.poll.records` upward (e.g., 5000) to maximize batching throughput during catch-up. **What could go wrong**: The new service might not be able to process 1 GB/s — it has downstream database writes, API calls, etc. Saturating the destination DB during replay will slow replay and potentially cause production issues if the destination DB is shared. Mitigation: quota the replay consumer using Kafka's client-side quota (`--throttle`), or use a separate consumer group with a lower `fetch.max.bytes` setting. Also: the 30-day data is likely NOT in the OS page cache on the brokers (only recent data is hot). Reading cold segments from disk for all 1,000 partitions simultaneously creates read amplification that can impact tail latencies for production consumers. Monitor broker disk I/O and production consumer latency during replay. If production is impacted, route the replay consumer group to follower replicas (`client.rack` + `replica.selector.class=RackAwareReplicaSelector`) to separate replay I/O from production I/O.

**Q: Design the idempotency system for a consumer that processes payment events. It receives the same event_id twice — what happens at each layer, and what is the worst-case failure mode your design cannot recover from?**

A: Walk through the layers. **Layer 1 — Kafka idempotent producer**: the producer assigns a sequence number `(producerId, partition, seqNum)`. The broker rejects duplicate batches with the same sequence number from the same producer epoch. This prevents producer-retry duplicates from ever being stored in Kafka. **Layer 2 — Consumer-side Redis dedup**: when the consumer polls a message, it does `SET NX "idempotency:{group}:{event_id}" "PROCESSING" EX 86400`. If SET NX returns false, the key exists — message already seen. If the value is COMPLETED, skip and ack. If PROCESSING (previous consumer crashed mid-processing), you have a decision: retry or alert. Retrying is safe if the downstream payment processor also has idempotency. After successful processing: `SET "idempotency:{group}:{event_id}" "COMPLETED" EX 86400`. **Layer 3 — Payment processor idempotency**: the downstream payment system must also be idempotent on the same `event_id`. If it charges a card and then receives the same event_id again, it should return "already processed" rather than charging twice. **The worst-case failure the design cannot recover from**: what if the Redis cluster loses data (AOF corruption, entire cluster restart) AND Kafka offsets have advanced past the affected messages AND the messages are beyond Kafka retention? In that case: the idempotency window is gone, Kafka can't replay the events, and you have no way to know if a specific event_id was processed or not. This is why the Redis TTL must be longer than Kafka retention — if Kafka still has the events, you can replay and the Redis SET NX will prevent double-processing. If Redis data loss is a concern, use Redis with AOF + AOF rewrite disabled at peak load, or replicate idempotency keys to MySQL as a fallback store for the critical payment consumer group.

---

## STEP 7 — MNEMONICS

### The PICKS Framework for Kafka Design

When the interviewer says "design a message broker / pub-sub system / event delivery system," open with this mental checklist:

**P — Partition key** (what determines ordering? entity ID? topic-wide? none?)
**I — ISR and acks** (what's the durability guarantee? acks=all? acks=1? min.insync.replicas?)
**C — Consumer groups** (how many independent consumers? pull or push? fan-out needed?)
**K — Key for idempotency** (how do you prevent duplicates? Redis SET NX? transactional outbox?)
**S — Schema and serialization** (raw bytes? Avro? who manages compatibility?)

### The Failure Path Checklist

When a message fails, walk through: **Classify → Retry → Quarantine → Replay**

- **Classify**: is it retryable (transient) or non-retryable (schema, validation, poison pill)?
- **Retry**: non-blocking retry topics with exponential backoff (1s, 5s, 30s, 60s, 300s)
- **Quarantine**: after max retries, route to DLT (Dead Letter Topic) with full error metadata
- **Replay**: after root cause fixed, bulk replay from DLT filtered by error class + time window

### Opening One-Liner for the Interview

When the interviewer asks you to start: **"The core design tension in any message broker is the triangle of durability, throughput, and ordering. Before I draw anything, I want to understand which corners of that triangle you're willing to compromise on — because acks=all gives you durability at the cost of 10ms latency, partition ordering gives you ordering at the cost of parallelism, and fan-out gives you scale at the cost of coordination complexity."**

This one statement signals to the interviewer that you understand the design space at a senior level, and it forces a clarifying question answer that will shape the rest of your design.

---

## STEP 8 — CRITIQUE OF THE SOURCE MATERIAL

### What Is Well-Covered

The five source documents are exceptionally thorough on Kafka internals. The ISR/HW/LEO explanation in `distributed_message_queue.md` is production-quality — it includes the exact failure mode for each acks setting, the leader epoch mechanism, and pseudocode for leader election. The transactional outbox in `reliable_event_delivery_system.md` covers both polling publisher and CDC variants with actual Java + Spring code examples. The non-blocking retry topic implementation in `dead_letter_queue_system.md` includes the `KafkaBackoffAwareMessageListenerAdapter` implementation detail that most candidates at L5 don't know exists. Schema compatibility rules (BACKWARD/FORWARD/FULL) in the event streaming material are complete.

### What Is Missing or Shallow

**Kafka Streams vs. Flink decision criteria** is mentioned but not deeply analyzed. The material says "use Flink for complex, use Kafka Streams for lightweight" but doesn't explain the operational boundary: Kafka Streams is a library (runs inside your service, no separate cluster), Flink is a cluster (separate operational footprint). For 200 Flink jobs, that's a significant operational investment. The material doesn't discuss when Kafka Streams is preferable.

**Multi-region / disaster recovery** is explicitly called out-of-scope in all five documents. But in a FAANG interview, "what happens if the entire data center goes down" is a likely follow-up. MirrorMaker 2 and Confluent Cluster Linking are the answer — they replicate topics across clusters with offset translation. You should know these exist even if you don't design them in detail.

**Kafka quotas and rate limiting** are not covered. Interviewers at senior levels ask: "how do you prevent a noisy producer from saturating the cluster?" Answer: Kafka's built-in quota system (`kafka-configs.sh --alter --add-config 'producer_byte_rate=10485760'`) throttles producers/consumers at the broker level. This is relevant for the replay scenario (Step 6 Tier 3 Q4).

**Kafka Connect operational details** are covered at the config level but not the failure recovery level. A common interview follow-up: "what happens if a Debezium connector task fails?" Answer: the task enters ERROR state; you must manually restart it (`POST /connectors/{name}/tasks/{id}/restart`); Debezium stores its binlog position in the Kafka Connect internal topic, so on restart it resumes from where it left off without data loss.

### Senior Probes to Expect

These are questions that distinguish an L5 from an L6 answer:

- "You said acks=all gives RPO=0. What if a follower is in ISR but hasn't persisted the data to disk yet — just page cache — and all three machines lose power simultaneously?" (Answer: RF=3 on separate physical machines makes this essentially impossible; Kafka's durability model relies on replication across machines, not fsync within a machine. For absolute on-disk durability, use `flush.messages=1`, accepting a 10x throughput reduction.)

- "How does Kafka handle the case where a consumer processes messages, calls commitSync(), and then the network request carrying the offset commit is lost?" (Answer: the consumer retries the commit. The `__consumer_offsets` topic uses Kafka's own at-least-once semantics; a duplicate commit for the same offset is idempotent — the broker stores the max committed offset per group/partition, so replaying a commit for an already-committed offset is harmless.)

- "Your outbox polling publisher runs every 100ms. Under what conditions does this create a problem?" (Answer: if the business transaction produces many events quickly, PENDING rows accumulate. The polling interval introduces up to 100ms of additional latency. If the polling DB query is not covered by an index on `(status, created_at)`, the query does a full table scan as rows accumulate — this is the index you must always create on the outbox table. Also: the polling publisher is a single point of failure; in HA mode, two instances might pick up the same row simultaneously — protect with `SELECT ... FOR UPDATE SKIP LOCKED` to get row-level locking without contention.)

- "What is leader epoch, and why does it exist?" (Answer: leader epoch is an incrementing counter per partition that is bumped on every leader election. When an old leader comes back from a network partition, it checks the current leader's epoch. If its epoch is stale, it knows it's no longer the leader, truncates its log to the epoch boundary, and becomes a follower. Without leader epoch, the rejoining old leader might serve reads from diverged log entries that were never committed by the new cluster topology — a subtle but catastrophic data consistency bug.)

### Common Traps Candidates Fall Into

1. **Saying "Kafka guarantees exactly-once" without qualification.** Kafka's transactional API provides exactly-once within Kafka (produce + offset commit atomically). It does NOT prevent your application from processing a message twice if you call external services inside the consumer before committing. True end-to-end exactly-once always requires idempotent application logic.

2. **Designing without specifying partition count or how it's calculated.** "I'll use Kafka" without saying "I need at least X partitions to achieve Y throughput" is a red flag. Show the math: `partitions_needed = total_throughput / per_partition_throughput` (roughly 10 MB/s per partition).

3. **Using a global ordering requirement to justify a single-partition design at high throughput.** A single partition at 1 GB/s is physically impossible (Kafka's per-partition write throughput limit is ~100-300 MB/s). Call out the requirement-level conflict.

4. **Forgetting that consumer groups create read amplification.** If you have 20 consumer groups on a 500 MB/s topic, the total read throughput requirement is 10 GB/s — 5x the write bandwidth. Size your broker read capacity accordingly (2 GB/s read per broker = 5 brokers minimum for this case).

5. **Treating DLQ as a dump.** Candidates often design a DLQ and stop there. Interviewers want to hear: how do operators find out about DLQ messages, how do they search/filter them, how do they replay them, and how do you prevent DLQ messages from silently accumulating for months? The answer is: alert on DLQ rate + Elasticsearch-backed inspection UI + replay API.

---

*End of Infra-06 Message Brokers Interview Guide*
