# Common Patterns — Message Brokers (06_message_brokers)

## Common Components

### Apache Kafka as Core Storage + Transport
- All 5 systems use Kafka as the backbone; append-only partitioned log; ISR replication RF=3; KRaft mode (no ZooKeeper)
- dead_letter_queue: DLQ topic `orders.DLT` + retry topics `orders.retry.0–4`; 30-day retention; RF=3
- distributed_message_queue: 50 brokers; 200K+ partitions; 7-day retention; acks=all with ISR
- event_streaming_platform: 5,000+ event types; RF=3; 7-day retention; Avro wire format with 5-byte schema header
- pubsub_notification_system: 10K topics; RF=3; 7-day retention; one Kafka consumer group per pull subscription
- reliable_event_delivery: outbox → Kafka → 20 consumer groups; partition key = `aggregate_id`

### Idempotent Producer + Exactly-Once Semantics
- All 5 systems achieve exactly-once or at-least-once with idempotency at the consumer side
- distributed_message_queue: idempotent producer (sequence numbers per partition); transactional producer for atomic multi-partition writes
- reliable_event_delivery: Redis `SET NX "idempotency:{consumer_group}:{event_id}"` with 24h TTL; consumer skips if already COMPLETED
- event_streaming_platform: Flink checkpoint barriers — state + Kafka offsets atomically snapshotted; replay from checkpoint on failure
- pubsub_notification_system: message_id (UUIDv7) deduplication at push delivery layer
- dead_letter_queue: idempotency preserved via message key passthrough to DLQ topic

### Consumer Groups for Independent Subscription
- All 5 systems use Kafka consumer groups; each group independently tracks offsets; multiple groups can consume the same topic
- distributed_message_queue: one group per logical consumer application; each partition assigned to exactly one consumer in a group
- pubsub_notification_system: one consumer group per pull subscription; fan-out service uses one group per topic (`fanout-{topic_id}`)
- reliable_event_delivery: 20 downstream service consumer groups on same outbox Kafka topic
- event_streaming_platform: Flink job cluster + Kafka Streams app each have separate consumer groups

### Partition-Based Ordering via Message Key
- All 5 systems route related messages to the same partition using a consistent key; ordering guaranteed per partition, not globally
- distributed_message_queue: `partition = murmur2(key) % numPartitions`
- reliable_event_delivery: `partition_key = aggregate_id` — all events for same entity go to same partition
- pubsub_notification_system: `ordering_key` field on subscription; messages with same key delivered in order
- event_streaming_platform: `aggregate_id` as Kafka key for CDC events (MySQL binlog ordering per PK)
- dead_letter_queue: original message key preserved on DLQ topic entry

### Retry with Exponential Backoff + Dead Letter Queue
- 3 of 5 systems implement multi-stage retry with explicit DLQ escalation
- dead_letter_queue: retry topics `orders.retry.0` (1s) → `orders.retry.1` (5s) → `orders.retry.2` (30s) → `orders.retry.3` (60s) → `orders.retry.4` (300s) → `orders.DLT`
- reliable_event_delivery: saga orchestrator retries failed steps; compensating transactions if step permanently fails
- event_streaming_platform: Kafka consumer auto-retry; Flink restarts from checkpoint on failure

### Message Headers for Metadata (Schema ID, Trace, Routing)
- All 5 systems attach metadata as Kafka message headers; headers separate from payload; do not affect partitioning
- dead_letter_queue: `kafka_dlt-original-topic`, `kafka_dlt-original-partition`, `kafka_dlt-original-offset`, `kafka_dlt-exception-fqcn`, `kafka_dlt-exception-message`
- event_streaming_platform: 5-byte Confluent wire format header (`0x00` + 4-byte `schema_id`) prepended to Avro payload
- reliable_event_delivery: `event_id`, `event_type`, `aggregate_type`, `aggregate_id`, `timestamp` as Kafka headers
- pubsub_notification_system: `message_id` (UUIDv7), `publish_timestamp`, attribute headers for filter evaluation
- distributed_message_queue: arbitrary `Header[]` key-value pairs; passed through transparently

## Common Databases

### Apache Kafka (Append-Only Log)
- All 5; `.log` segment files + `.index` (sparse offset→position) + `.timeindex` (timestamp→offset); sequential I/O; page cache + zero-copy sendfile()

### Redis Cluster (Idempotency + Cache)
- 3 of 5 (reliable_event_delivery, pubsub_notification_system, event_streaming_platform); `SET NX` for dedup; subscription metadata cache < 1 ms; TTL-based auto-expiry

### MySQL (Outbox + Metadata)
- 3 of 5 (reliable_event_delivery outbox, pubsub_notification_system subscription config, event_streaming_platform CDC source); `outbox` table with `idx_status_created` for polling; `saga_instance` + `saga_step` for orchestration state

### Elasticsearch (Search + Audit)
- 2 of 5 (dead_letter_queue DLQ inspection index, event_streaming_platform sink connector); full-text search on error messages; time-ranged DLQ browsing

## Common Communication Patterns

### Transactional Outbox (Atomic Business State + Event)
- reliable_event_delivery: `BEGIN; INSERT INTO orders; INSERT INTO outbox; COMMIT;` — no 2PC, no distributed transaction; outbox polling every 100 ms OR Debezium CDC (< 200 ms)
- event_streaming_platform: Debezium reads MySQL binlog → outbox changes → Kafka; < 200 ms CDC latency
- Key guarantee: business state and event are committed atomically; no event lost if service crashes between write and Kafka produce

### Saga Orchestration via Command/Reply Topics
- reliable_event_delivery: orchestrator sends `ProvisionServer` command to `provisioning.commands`; receives reply on `provisioning.replies`; advances or compensates saga; `saga_instance` table tracks state
- event_streaming_platform: multi-step CDC pipelines model a form of saga via Kafka Connect source → transform → sink

### Fan-Out (Tiered by Subscriber Count)
- pubsub_notification_system: pull subscribers ≤ 1,000/topic → native Kafka consumer groups; push subscribers → dedicated fan-out service with sharded subscriber lists (shard per 10,000 subscribers)
- dead_letter_queue: batch replay fans out individual DLQ messages back to original topic at ≥ 10,000 msgs/sec

## Common Scalability Techniques

### Sequential Append-Only I/O (3 GB/s vs 400 MB/s random)
- distributed_message_queue: NVMe sequential write 3 GB/s vs random 100K IOPS = 400 MB/s; HDD sequential 200 MB/s vs random 0.8 MB/s (250× faster)
- All systems benefit: Kafka segments are append-only; reads use page cache (OS caches hot segments); zero-copy `sendfile()` bypasses JVM heap → 2 GB/s read throughput per broker

### Producer Batching (batch.size + linger.ms)
- distributed_message_queue: `linger.ms = 5`; batches records per partition before network send; reduces syscalls; compresses batch (LZ4/Snappy/ZSTD)
- reliable_event_delivery: outbox polling LIMIT 1,000 per batch; all 1,000 produced in one Kafka batch call
- dead_letter_queue: batch replay 1,000 messages at once

### ISR-Based Replication (Availability vs Durability Tradeoff)
- distributed_message_queue: ISR = replicas with LEO within `replica.lag.time.max.ms = 30s` of leader; HW = min(LEO of all ISR); consumers only see messages below HW
- `acks=all`: leader waits for all ISR to acknowledge before advancing HW; RPO = 0 if ISR ≥ 2
- `acks=1`: leader only; higher throughput, potential data loss if leader crashes before ISR sync

### Checkpoint Barriers for Stateful Exactly-Once (Flink)
- event_streaming_platform: JobManager injects barriers into source streams; when operator receives barriers from ALL inputs → snapshot state to S3; on failure → restore from checkpoint + reset Kafka offsets; exactly-once with no duplicates
- Checkpoint interval typically 30–60 s; recovery rewrites only events after last checkpoint

## Common Deep Dive Questions

### How do you prevent a slow/failing message from blocking all messages behind it on the same partition?
Answer: Non-blocking retry topics: instead of blocking the partition consumer and waiting for exponential backoff in-memory, the failed message is re-produced to a separate retry topic (`orders.retry.N`) with the intended processing time encoded as the message timestamp. A retry consumer reads from that topic and checks `now < record.timestamp + retryDelay`; if too early, it pauses that partition and resumes after the delay. Original partition continues processing immediately. At final retry failure, message goes to DLT. This isolates slow messages without blocking the happy path.
Present in: dead_letter_queue_system

### How do you achieve exactly-once delivery across a DB write and a Kafka produce without a distributed transaction?
Answer: Transactional outbox: (1) Write business entity + outbox row in a single local DB transaction — atomic, no 2PC; (2) Separate polling publisher reads outbox rows and produces to Kafka; (3) On Kafka ACK, mark outbox row PUBLISHED. Consumer uses Redis `SET NX event_id` to deduplicate. If service crashes between polling and marking, the same row is re-published on next poll — at-least-once to Kafka, but exactly-once at consumer via dedup. CDC variant (Debezium reading binlog) achieves < 200 ms latency vs < 500 ms for polling.
Present in: reliable_event_delivery_system

## Common NFRs

| NFR | Values Across Systems |
|-----|----------------------|
| **Availability** | 99.99% (all 5); 99.95% stream processing (Flink) |
| **Write Latency P99** | 5–15 ms (Kafka acks=all), < 10 ms (DLQ write), < 10 ms (publish), < 200 ms (outbox CDC) |
| **End-to-End Latency P99** | < 50 ms (Kafka produce→consume), < 200 ms (push delivery), < 2 s (outbox end-to-end), < 500 ms (CDC) |
| **Throughput** | 600 MB/s write per broker, 2 GB/s read per broker, 10M msgs/sec peak, 500K msgs/sec pub/sub |
| **Durability** | RF=3 all systems; ISR ≥ 2 for RPO=0; exactly-once via checkpoints or idempotency |
| **Retention** | 7 days standard; 30 days DLQ; 24h idempotency keys (Redis) |
| **Ordering** | Per partition (key-based routing); not global |
