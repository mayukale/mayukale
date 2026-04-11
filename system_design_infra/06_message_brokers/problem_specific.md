# Problem-Specific Notes — Message Brokers (06_message_brokers)

## dead_letter_queue_system

### Unique Purpose
Route permanently failed messages to a Dead Letter Topic (DLT) after exhausting retry policy. Preserve original metadata. Enable search, edit, and replay of failed messages. Detect poison-pill messages. 0.01% DLQ rate at 1M msgs/sec = 100 msgs/sec to DLQ; 25.9 GB/day; 777 GB / 30-day retention.

### Unique Architecture
- **Non-blocking retry topics**: `orders.retry.0–4` with delays 1s/5s/30s/60s/300s; failed messages re-produced to retry topic instead of blocking partition consumer; final failure goes to `orders.DLT`
- **DLQ Indexer**: consumes `orders.DLT`, indexes to Elasticsearch for search by error_type / source_topic / timestamp
- **Poison-pill detection**: same message failing 47+ times → classify as `POISON_PILL`; auto-isolate
- **Circuit breaker**: if downstream unavailable, stop retrying immediately → route to DLT

### Unique Data Structures
```
# DLQ message headers (added by DeadLetterPublishingRecoverer)
kafka_dlt-original-topic:          "orders"
kafka_dlt-original-partition:      "7"
kafka_dlt-original-offset:         "1234567"
kafka_dlt-original-timestamp:      "1712620800000"
kafka_dlt-original-consumer-group: "order-processor"
kafka_dlt-exception-fqcn:          "java.lang.NullPointerException"
kafka_dlt-exception-message:       "Order.customerId is null"

# Elasticsearch DLQ document
{
  "dlq_id": "orders.DLT:3:456",
  "message_key": "order-12345",
  "message_value": {"order_id": "12345", "customer_id": null},
  "error_class": "java.lang.NullPointerException",
  "error_message": "Order.customerId is null",
  "failure_timestamp": "2026-04-09T10:00:05.000Z",
  "replay_status": "PENDING"  -- "PENDING" | "REPLAYED" | "SKIPPED"
}

# Error classification
RETRYABLE_TRANSIENT     → Network timeout, service unavailable
RETRYABLE_RATE_LIMITED  → 429 Too Many Requests
NON_RETRYABLE_VALIDATION → Null required field, invalid enum
NON_RETRYABLE_SCHEMA    → Deserialization failure
NON_RETRYABLE_LOGIC     → Business rule violation
POISON_PILL             → Causes consumer crash (repeated)
```

### Unique Algorithm: Non-Blocking Retry Delay
```java
// Partition paused until retry window opens; original partition unblocked immediately
void onMessage(ConsumerRecord<String, byte[]> record) {
    long targetTime = record.timestamp() + retryDelay;
    long now = System.currentTimeMillis();
    if (now < targetTime) {
        consumer.pause(Set.of(new TopicPartition(record.topic(), record.partition())));
        scheduler.schedule(() ->
            consumer.resume(Set.of(new TopicPartition(record.topic(), record.partition()))),
            Duration.ofMillis(targetTime - now));
        throw new KafkaBackoffException("Waiting for retry delay");
    }
    processMessage(record);
}
```

### Retry Progression
```
Attempt 1 → orders.retry.0  (delay: 1s)
Attempt 2 → orders.retry.1  (delay: 5s)
Attempt 3 → orders.retry.2  (delay: 30s)
Attempt 4 → orders.retry.3  (delay: 60s)
Attempt 5 → orders.retry.4  (delay: 300s = 5 min)
Final     → orders.DLT       (30-day retention)
```

### Unique NFRs
- DLQ write latency: < 10 ms (must not slow consumer)
- DLQ replay throughput: ≥ 10,000 msgs/sec
- Inspection search latency: < 2 s
- DLQ message retention: 30 days; 777 GB total at 0.01% failure rate
- Availability: 99.99%

---

## distributed_message_queue

### Unique Purpose
Kafka from scratch: 50-broker cluster, 200K+ partitions, 10M msgs/sec = 10 GB/s raw → 30 GB/s with RF=3 disk writes. 600 MB/s write per broker, 2 GB/s read per broker. P99 produce (acks=all) < 15 ms; P99 consume (cache hit) < 2 ms.

### Unique Architecture
- **KRaft mode**: Controller manages cluster metadata via Raft consensus; no ZooKeeper; partition leadership, broker liveness
- **Segment files**: `.log` (append-only data) + `.index` (sparse offset→fileposition) + `.timeindex` (timestamp→offset); rolls at `segment.bytes = 1 GB`
- **ISR**: replicas with LEO within `replica.lag.time.max.ms = 30s`; HW = min(LEO of all ISR); only messages ≤ HW visible to consumers
- **Zero-copy sendfile()**: kernel transfers segment file → NIC without entering JVM heap; 2 GB/s read throughput

### Unique Data Structures
```
# On-disk segment (partition topic-0)
/kafka-logs/topic-0/
    00000000000000000000.log          # message data (append-only)
    00000000000000000000.index        # sparse: offset → file_position
    00000000000000000000.timeindex    # sparse: timestamp → offset
    00000000000005242880.log          # next segment after roll
    leader-epoch-checkpoint           # leader_epoch → start_offset

# Key offsets tracked per replica
LEO (Log End Offset): next offset to be written (per replica)
HW  (High Watermark): min(ISR LEO values) — consumer-visible boundary
ISR (In-Sync Replicas): replicas within lag.time.max.ms of leader
```

### Unique Algorithm: Partition Assignment + Batching
```python
# Partition selection (murmur2)
partition = murmur2(key) % numPartitions

# Producer batching → leader append → ISR sync → HW advance
1. Serialize key+value
2. partition = murmur2(key) % numPartitions
3. Batch records per partition (batch.size, linger.ms=5)
4. Send batch to partition leader
5. Leader appends to active .log segment (page cache write)
6. Followers fetch from leader; leader tracks follower LEO
7. Once all ISR replicas ACK → leader advances HW
8. Leader responds to producer (based on acks setting)

# Read path (binary search + sendfile)
segment = binary_search(segments_sorted_by_base_offset, target_offset)
position = binary_search(segment.index, target_offset - segment.base_offset)
sendfile(socket_fd, segment.log_fd, position, max_bytes)
# Zero copy: kernel DMA → socket buffer, no JVM heap copy
```

### Leader Election (KRaft)
```python
def on_leader_failure(partition):
    isr = metadata_cache.get_isr(partition)
    isr.remove(failed_broker)
    if isr.is_empty():
        if unclean_leader_election_enabled:
            new_leader = pick_any_live_replica(partition)
        else:
            partition.state = OFFLINE; return
    else:
        new_leader = isr.first_alive()
    metadata.set_leader(partition, new_leader)
    metadata.increment_leader_epoch(partition)  # fencing against stale leaders
    broadcast_metadata_update(all_brokers)
```

### I/O Performance
| Medium | Random 4KB Write | Sequential Write | Speedup |
|--------|-----------------|-----------------|---------|
| NVMe SSD | ~400 MB/s | ~3,000 MB/s | 7.5× |
| HDD | ~0.8 MB/s | ~200 MB/s | 250× |

### Unique NFRs
- Write P99 (acks=1): < 5 ms; (acks=all): < 15 ms
- Read P99 (cache hit): < 2 ms; (cache miss): < 20 ms
- End-to-end (produce → consume): < 50 ms P99
- Brokers needed: 50 (write-bound at 10 GB/s, 600 MB/s capacity each)
- Cluster: 30–100 brokers, 5,000+ topics, 200,000+ partitions

---

## event_streaming_platform

### Unique Purpose
Schema Registry + CDC + Stream Processing. 500 services × 2,000 events/sec = 1M events/sec; 500 MB/s raw; 250 CDC streams (200 MySQL + 50 PostgreSQL); 5,000 event types × 20 versions = 100K schema versions. Replay 30 days in < 4 hours.

### Unique Architecture
- **Schema Registry**: stateless HA behind LB; backed by `_schemas` Kafka topic (compacted, RF=3); in-memory cache 500 MB; returns `schema_id` (4 bytes)
- **Avro wire format**: `0x00` (magic) + 4-byte big-endian `schema_id` + Avro payload → 5 bytes overhead
- **Debezium**: reads MySQL binlog (row-level changes) → Kafka; < 200 ms CDC latency; tracks per-table offset in Kafka Connect internal topics
- **Flink**: exactly-once via checkpoint barriers; event-time windowing; parallelism = 16 per job; state snapshotted to S3

### Unique Data Structures
```json
// Schema Registry subject
{
  "subject": "orders-value",
  "version": 3,
  "id": 42,
  "schemaType": "AVRO",
  "schema": "{\"type\":\"record\",\"name\":\"Order\",\"fields\":[...]}"
}

// Avro wire format (Confluent encoding)
[0x00] [schema_id: 4 bytes BE] [avro_payload: N bytes]

// Avro schema
{
  "type": "record", "name": "Order", "namespace": "com.infra.events",
  "fields": [
    {"name": "order_id", "type": "string"},
    {"name": "customer_id", "type": "string"},
    {"name": "amount", "type": "double"},
    {"name": "status", "type": {"type": "enum", "name": "Status", ...}}
  ]
}
```

### Schema Compatibility Rules
| Change | BACKWARD | FORWARD | FULL |
|--------|----------|---------|------|
| Add field with default | Yes | Yes | Yes |
| Add field without default | No | Yes | No |
| Remove field with default | Yes | No | No |
| Remove field without default | No | No | No |
| Rename field | No | No | No |

Modes: `BACKWARD` (default), `FORWARD`, `FULL`, `BACKWARD_TRANSITIVE`, `FORWARD_TRANSITIVE`, `FULL_TRANSITIVE`

### Unique Algorithm: Flink Exactly-Once via Checkpoint Barriers
```
1. JobManager injects checkpoint barriers into source streams
2. Barrier flows downstream like a regular event
3. When operator receives barriers from ALL input channels:
   a. Snapshot operator state to S3/HDFS
   b. Forward barrier downstream
4. When all sinks ACK barrier → checkpoint complete; commit Kafka offsets
5. On failure:
   a. Restore state from latest checkpoint
   b. Reset Kafka offsets to checkpoint offsets
   c. Replay events after checkpoint offset
   d. No duplicates (state + offsets atomically consistent)
```

### Flink SQL (Tumbling Window)
```sql
SELECT
    TUMBLE_START(event_time, INTERVAL '1' HOUR) AS window_start,
    COUNT(*) AS order_count, SUM(amount) AS total_amount
FROM orders
GROUP BY TUMBLE(event_time, INTERVAL '1' HOUR);
```

### Producer/Consumer Serialization
```
Producer:
  1. Check local LRU cache: (subject, schema) → schema_id
  2. Cache miss: POST /subjects/{subject}/versions → get schema_id
  3. Serialize: 0x00 + schema_id(4B BE) + avro_encode(record)

Consumer:
  1. Read schema_id from bytes 1–4
  2. Check LRU cache: schema_id → writer_schema
  3. Cache miss: GET /schemas/ids/{id}
  4. Deserialize: avro_decode(payload, writer_schema, reader_schema)
```

### Unique NFRs
- Schema registry latency: < 10 ms (cached < 1 ms)
- Stream processing latency (stateless): < 100 ms; (windowed): < 1 s
- CDC latency (DB → consumer): < 500 ms P99
- Throughput: 5 GB/s aggregate; 1M events/sec
- Replay: 30 days in < 4 hours
- Availability: 99.99% schema registry; 99.95% stream processing

---

## pubsub_notification_system

### Unique Purpose
Fan-out pub/sub: 500K msgs/sec published, 50 avg subscribers/topic = 25M deliveries/sec. Broadcast topics: 10 topics × 1M+ subscribers = 1B deliveries/sec peak. Push (60%) and pull (40%) delivery. 1M+ subscribers per topic. Message TTL, ordering keys, filter expressions.

### Unique Architecture
- **Pull subscribers**: native Kafka consumer groups (one per subscription); lowest latency; limited to ~10K consumer groups efficiently
- **Push subscribers**: fan-out service consumes one Kafka group per topic (`fanout-{topic_id}`); shards subscriber list for broadcast (shard per 10,000 subscribers); HTTP/gRPC webhook delivery
- **Subscription metadata**: MySQL + Redis cache; `delivery_mode`, `endpoint_url`, `filter_expression`, `ordering_key` per subscription
- **Message ID**: UUIDv7 (timestamp-sortable); used for deduplication and tracing

### Unique Data Structures
```
# Subscription config (MySQL)
subscription_id: UUID
topic_id: string
delivery_mode: "PUSH" | "PULL"
endpoint_url: string              -- for push webhooks
filter_expression: string         -- attribute-based: "severity = 'CRITICAL'"
ordering_key: string              -- messages with same key delivered in order
status: "ACTIVE" | "PAUSED"

# Message headers (Kafka)
message_id:          UUIDv7  -- timestamp-sortable
publish_timestamp:   milliseconds
severity:            "CRITICAL" | "HIGH" | "LOW"
region:              "us-east-1"
resource_type:       "bare-metal"

# Delivery status (tracked per message × subscription)
status:      "PENDING" | "DELIVERED" | "FAILED"
attempt:     int (1–5)
last_attempt_time: timestamp
error:       string
```

### Unique Algorithm: Tiered Fan-Out
```
Tier 1 (Pull, ≤ 1,000 subs/topic):
  Each pull subscription = one Kafka consumer group
  Direct native Kafka pull → lowest latency (< 50 ms)
  Limit: Kafka manages ~10,000 consumer groups efficiently
  10,000 topics × 50 pull subs = 500,000 consumer groups (at limit)

Tier 2 (Push, application-level fan-out):
  One Kafka consumer group per topic: "fanout-{topic_id}"
  Fan-out service reads message → parallel HTTP calls to N subscribers
  At 1,000 msgs/sec × 50 subs = 50,000 HTTP calls/sec (parallelized)

Broadcast Fan-Out (1M+ push subscribers):
  Dedicated shard-per-10K fan-out service
  Shard 0: subscribers 0–9,999
  Shard K: subscribers K×10K – (K+1)×10K - 1
  100 shards × 10,000 HTTP calls/sec each = 1M deliveries/sec
```

### Filter Evaluation
```java
// Per-message, per-subscription filter evaluation (< 1 ms)
for (Subscription sub : pushSubs) {
    if (sub.getFilter() != null && !evaluateFilter(sub.getFilter(), msgAttributes))
        continue;  // Skip non-matching
    deliverAsync(sub.getEndpointUrl(), message);
}
// Example filter: "severity = 'CRITICAL' AND region = 'us-east-1'"
```

### Unique NFRs
- Publish latency: < 10 ms P99
- Push delivery latency: < 200 ms P99; pull: < 50 ms P99
- Fan-out: 1M deliveries in < 30 s
- Throughput: 500K msgs/sec published; 25M deliveries/sec average; 1B/sec peak broadcast
- Scale: 10K topics, 500K subscriptions, 1M+ subscribers per broadcast topic
- Availability: 99.99%

---

## reliable_event_delivery_system

### Unique Purpose
Exactly-once event delivery across distributed services via transactional outbox + idempotent consumers + saga orchestration. 100K business transactions/sec → 150K events/sec; 20 consumer groups = 3M consume ops/sec; 10K concurrent saga instances; business write + outbox insert < 10 ms.

### Unique Architecture
- **Transactional outbox**: `BEGIN; INSERT INTO orders; INSERT INTO outbox; COMMIT;` — single local DB transaction; no 2PC; polling publisher reads outbox every 100 ms (< 500 ms latency) OR Debezium CDC (< 200 ms)
- **Idempotent consumer**: Redis `SET NX "idempotency:{group}:{event_id}"` TTL=24h; skip if already COMPLETED; prevents duplicate processing even with Kafka at-least-once
- **Saga orchestrator**: command/reply topics; step-by-step with compensating transactions on failure; `saga_instance` + `saga_step` in MySQL

### Key Schema
```sql
CREATE TABLE outbox (
  id             BIGINT PRIMARY KEY AUTO_INCREMENT,
  aggregate_type VARCHAR(255) NOT NULL,    -- "Order", "Saga"
  aggregate_id   VARCHAR(255) NOT NULL,
  event_type     VARCHAR(255) NOT NULL,
  topic          VARCHAR(255) NOT NULL,    -- target Kafka topic
  partition_key  VARCHAR(255) NOT NULL,    -- for murmur2 routing
  payload        MEDIUMBLOB NOT NULL,
  metadata       JSON,
  status         VARCHAR(50) NOT NULL,     -- "PENDING", "PUBLISHED"
  created_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  published_at   TIMESTAMP NULL,
  INDEX idx_status_created (status, created_at),
  INDEX idx_aggregate (aggregate_type, aggregate_id)
);
CREATE TABLE saga_instance (
  saga_id      VARCHAR(255) PRIMARY KEY,
  saga_type    VARCHAR(255) NOT NULL,
  status       VARCHAR(50) NOT NULL,   -- "RUNNING","COMPLETED","COMPENSATING","FAILED"
  current_step INT,
  payload      MEDIUMBLOB NOT NULL,
  expires_at   TIMESTAMP NOT NULL,
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE saga_step (
  saga_id          VARCHAR(255) NOT NULL,
  step_index       INT NOT NULL,
  step_name        VARCHAR(255) NOT NULL,
  command_event_id VARCHAR(255),
  status           VARCHAR(50) NOT NULL,  -- "SENT","COMPLETED","FAILED"
  result           MEDIUMBLOB,
  PRIMARY KEY (saga_id, step_index)
);
```

### Unique Algorithm: Outbox Polling Publisher
```java
// Runs every 100 ms
List<OutboxEvent> events = outboxRepo.findPending(1000);
List<CompletableFuture<?>> futures = new ArrayList<>();
for (OutboxEvent event : events) {
    ProducerRecord<String, byte[]> record =
        new ProducerRecord<>(event.getTopic(), event.getPartitionKey(), event.getPayload());
    record.headers().add("event_id", event.getEventId().getBytes());
    record.headers().add("event_type", event.getEventType().getBytes());
    record.headers().add("aggregate_id", event.getAggregateId().getBytes());
    futures.add(kafkaTemplate.send(record).toCompletableFuture());
}
CompletableFuture.allOf(futures.toArray(new CompletableFuture[0])).get(10, SECONDS);
outboxRepo.markPublished(events.stream().map(OutboxEvent::getId).toList());
// Cleanup hourly: DELETE FROM outbox WHERE status='PUBLISHED' AND created_at < NOW()-1h
```

### Unique Algorithm: Idempotent Consumer
```java
String key = "idempotency:" + consumerGroup + ":" + eventId;
Boolean isNew = redis.opsForValue().setIfAbsent(key, "PROCESSING", Duration.ofHours(24));
if (Boolean.FALSE.equals(isNew)) {
    if ("COMPLETED".equals(redis.opsForValue().get(key))) { ack.acknowledge(); return; }
}
// process event...
redis.opsForValue().set(key, "COMPLETED", Duration.ofHours(24));
```

### Saga Orchestration (3-Step with Compensation)
```
Step 0: → provisioning.commands (ProvisionServer)
  ← provisioning.replies → advance
Step 1: → network.commands (AllocateNetwork)
  ← network.replies → advance
Step 2: → dns.commands (ConfigureDNS)
  ← dns.replies → COMPLETED

On Step 2 failure:
  saga.setStatus(COMPENSATING)
  for i in (currentStep-1 downTo 0):
    executeCompensation(saga, i, step[i].result)
  # Reverse-order compensating transactions
```

### Dual Publishing Strategies
| Strategy | Latency | Complexity | When to Use |
|----------|---------|------------|-------------|
| Polling Publisher (100ms) | < 500 ms P99 | Simple; extra polling load | Default; most cases |
| Debezium CDC (binlog) | < 200 ms P99 | Requires Debezium setup | Lower latency required |

### Unique NFRs
- Outbox → Kafka (polling): < 500 ms P99; (CDC): < 200 ms P99
- End-to-end (DB write → consumer): < 2 s P99
- Exactly-once guarantee: 100% with idempotent consumers
- Throughput: 100K events/sec; 3M idempotency checks/sec (Redis)
- Saga completion (3-step): < 10 s
- Active saga instances: 10K concurrent
