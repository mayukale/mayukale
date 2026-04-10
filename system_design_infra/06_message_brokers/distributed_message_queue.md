# System Design: Distributed Message Queue (Kafka from Scratch)

> **Relevance to role:** A cloud infrastructure platform engineer must deeply understand distributed messaging -- it underpins every async workflow: job scheduling events, bare-metal provisioning state changes, Kubernetes controller reconciliation signals, OpenStack service bus communication, and Agentic AI task orchestration. Kafka specifically is the backbone of most large-scale event-driven infrastructure platforms.

---

## 1. Requirement Clarifications

### Functional Requirements
| ID | Requirement |
|----|-------------|
| FR-1 | Producers publish messages to named **topics** |
| FR-2 | Topics are divided into ordered, append-only **partitions** |
| FR-3 | Consumers read messages via **consumer groups** with partition assignment |
| FR-4 | Messages are **persisted to disk** for configurable retention (time or size) |
| FR-5 | Support **replayability** -- consumers can seek to any offset |
| FR-6 | Provide **at-least-once** delivery by default; **exactly-once** via idempotent/transactional producers |
| FR-7 | Support **log compaction** for changelog topics |
| FR-8 | Admin operations: create/delete topics, reassign partitions, ACLs |

### Non-Functional Requirements
| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Throughput (write) | >= 600 MB/s per broker (sequential writes + page cache) |
| NFR-2 | Throughput (read) | >= 2 GB/s per broker (page cache + zero-copy) |
| NFR-3 | Latency (p99 produce) | < 5 ms (acks=1), < 15 ms (acks=all) |
| NFR-4 | Durability | No acknowledged message lost if >= 1 ISR replica survives |
| NFR-5 | Availability | 99.99% for produce path, 99.95% for consume path |
| NFR-6 | Scalability | Linear horizontal scaling via partitions |
| NFR-7 | Ordering | Per-partition total order guaranteed |

### Constraints & Assumptions
- Deployed on bare-metal servers with NVMe SSDs (4 TB each) and 10 Gbps NICs.
- Messages average 1 KB; max 10 MB (configurable via `message.max.bytes`).
- Retention default: 7 days time-based, 1 TB size-based per partition.
- Cluster size: 30-100 brokers. Topics: 5,000+. Partitions: 200,000+.
- JVM-based (Java 17+), runs on Linux with XFS filesystem.
- KRaft mode (no ZooKeeper dependency).

### Out of Scope
- Multi-datacenter replication (MirrorMaker 2 / Cluster Linking) -- covered in reliable_event_delivery_system.md.
- Kafka Connect and Kafka Streams (covered in event_streaming_platform.md).
- Schema management (covered in event_streaming_platform.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|-------------|--------|
| Peak produce rate | 10M msgs/sec x 1 KB avg | **10 GB/s** |
| Replication factor | 3 replicas per partition | 30 GB/s total disk write |
| Brokers needed (write) | 30 GB/s / 600 MB/s per broker | **50 brokers** (write-bound) |
| Brokers needed (read, 5 consumer groups) | 10 GB/s x 5 = 50 GB/s / 2 GB/s per broker | **25 brokers** (read-bound) |
| Broker count (max of above) | max(50, 25) | **50 brokers** (+ 20% headroom = 60) |
| Partitions per broker | 200,000 total / 60 brokers | ~3,333 partitions/broker |
| Single partition throughput | ~10 MB/s (sequentially) | -- |
| Partition count for 10 GB/s | 10,000 MB/s / 10 MB/s | **1,000 partitions** (minimum) |

### Latency Requirements

| Operation | Target | Mechanism |
|-----------|--------|-----------|
| Produce (acks=0) | < 1 ms | Fire-and-forget, no broker wait |
| Produce (acks=1) | < 5 ms | Leader writes to page cache |
| Produce (acks=all) | < 15 ms | All ISR replicas acknowledge |
| Consume (cache hit) | < 2 ms | Zero-copy sendfile() from page cache |
| Consume (cache miss) | < 20 ms | NVMe SSD random read |
| End-to-end (produce to consume) | < 50 ms p99 | Including batching (linger.ms=5) |

### Storage Estimates

| Parameter | Calculation | Result |
|-----------|-------------|--------|
| Daily ingest (pre-replication) | 10 GB/s x 86,400 | **864 TB/day** |
| Daily ingest (with RF=3) | 864 TB x 3 | **2,592 TB/day** |
| 7-day retention | 2,592 TB x 7 | **18,144 TB** |
| Per-broker storage | 18,144 TB / 60 brokers | **302 TB/broker** |
| Disks per broker | 302 TB / 4 TB NVMe | **76 disks** (unrealistic) |
| Revised: smaller cluster scope | Assume 500 GB/s total, RF=3, 7d | ~3 PB total, 50 TB/broker = 13 disks |

*Reality check:* At 10 GB/s pre-replication, this is a hyperscaler-grade deployment. Most production clusters handle 500 MB/s - 5 GB/s. The architecture scales the same way regardless.

### Bandwidth Estimates

| Path | Calculation | Result |
|------|-------------|--------|
| Producer -> Leader | 10 GB/s aggregate | 10 Gbps NICs saturated at ~1.25 GB/s per broker |
| Leader -> Follower (replication) | 10 GB/s x 2 followers = 20 GB/s | Each broker sends ~333 MB/s replication traffic |
| Leader -> Consumer | 50 GB/s (5 consumer groups) | ~833 MB/s per broker |
| Total per-broker NIC | 1.25 + 0.33 + 0.83 GB/s = ~2.4 GB/s | 25 Gbps NIC recommended |

---

## 3. High Level Architecture

```
 Producers (Java/Python services)
    |
    | TCP (Kafka binary protocol)
    v
+-----------------------------------------------------------------------+
|                        Kafka Cluster (KRaft mode)                     |
|                                                                       |
|  +-------------+  +-------------+  +-------------+  +-------------+  |
|  | Broker 0    |  | Broker 1    |  | Broker 2    |  | Broker N    |  |
|  | (Controller)|  |             |  |             |  |             |  |
|  |             |  |             |  |             |  |             |  |
|  | Partitions: |  | Partitions: |  | Partitions: |  | Partitions: |  |
|  | T0-P0 (L)   |  | T0-P0 (F)   |  | T0-P1 (L)   |  | T0-P2 (F)   |  |
|  | T1-P3 (F)   |  | T1-P3 (L)   |  | T0-P1 (F)   |  | T0-P2 (L)   |  |
|  +------+------+  +------+------+  +------+------+  +------+------+  |
|         |                |                |                |          |
|  +------v------+  +------v------+  +------v------+  +------v------+  |
|  | Log Dir     |  | Log Dir     |  | Log Dir     |  | Log Dir     |  |
|  | /kafka-logs |  | /kafka-logs |  | /kafka-logs |  | /kafka-logs |  |
|  | topic-part/ |  | topic-part/ |  | topic-part/ |  | topic-part/ |  |
|  |  .log       |  |  .log       |  |  .log       |  |  .log       |  |
|  |  .index     |  |  .index     |  |  .index     |  |  .index     |  |
|  |  .timeindex |  |  .timeindex |  |  .timeindex |  |  .timeindex |  |
|  +-------------+  +-------------+  +-------------+  +-------------+  |
+-----------------------------------------------------------------------+
    |
    | TCP (Kafka binary protocol)
    v
 Consumers (Consumer Groups)
    +-- Group A: [C0, C1, C2]  (each assigned subset of partitions)
    +-- Group B: [C0, C1]
```

### Component Roles

| Component | Role |
|-----------|------|
| **Broker** | Stores partitions, serves produce/fetch requests, replicates data |
| **Controller (KRaft)** | Manages cluster metadata, partition leadership, broker liveness via Raft consensus |
| **Topic** | Logical category for messages; purely a namespace |
| **Partition** | Unit of parallelism and ordering; append-only log on disk |
| **Segment** | Physical file for a range of offsets within a partition (.log + .index + .timeindex) |
| **Leader Replica** | Serves all reads and writes for a partition |
| **Follower Replica** | Fetches from leader, stays in ISR if caught up |
| **Producer** | Publishes records, selects partition via key hash or round-robin |
| **Consumer Group** | Coordinate consumers; each partition assigned to exactly one consumer in the group |

### Data Flows

**Produce Flow:**
1. Producer serializes key+value, computes partition = `murmur2(key) % numPartitions`.
2. Producer batches records per partition (controlled by `batch.size`, `linger.ms`).
3. Batch sent to partition leader broker.
4. Leader appends to active segment file (page cache write).
5. Followers fetch from leader; leader tracks each follower's LEO.
6. Once all ISR replicas acknowledge, leader advances High Watermark (HW).
7. Leader responds to producer (based on `acks` setting).

**Consume Flow:**
1. Consumer sends FetchRequest with (topic, partition, offset).
2. Leader reads from segment file; if data is in page cache, uses `sendfile()` zero-copy.
3. Consumer processes records, commits offset (auto or manual).
4. Committed offsets stored in `__consumer_offsets` internal topic.

---

## 4. Data Model

### Core Entities & Schema

**Record (ProducerRecord / ConsumerRecord)**
```
Record {
    topic:          string          // logical destination
    partition:      int32           // computed or explicit
    offset:         int64           // assigned by broker (monotonic per partition)
    timestamp:      int64           // CreateTime or LogAppendTime
    key:            byte[]          // nullable; used for partitioning and compaction
    value:          byte[]          // the payload
    headers:        Header[]        // key-value metadata pairs
}
```

**Segment Files (on-disk structure for partition `topic-0`):**
```
/kafka-logs/topic-0/
    00000000000000000000.log          # message data (append-only)
    00000000000000000000.index        # sparse offset -> file position mapping
    00000000000000000000.timeindex    # sparse timestamp -> offset mapping
    00000000000005242880.log          # next segment (rolls at segment.bytes=1GB)
    00000000000005242880.index
    00000000000005242880.timeindex
    leader-epoch-checkpoint           # leader epoch -> start offset
```

**.log file record batch format (on-disk):**
```
RecordBatch {
    baseOffset:          int64
    batchLength:         int32
    partitionLeaderEpoch: int32
    magic:               int8     // 2 for current version
    crc:                 uint32   // CRC of attributes through end
    attributes:          int16    // compression, timestamp type, transactional, control
    lastOffsetDelta:     int32
    firstTimestamp:      int64
    maxTimestamp:        int64
    producerId:          int64    // for idempotent/transactional producer
    producerEpoch:       int16
    baseSequence:        int32    // for idempotent dedup
    records:             Record[]
}
```

**__consumer_offsets topic (compacted):**
```
Key:   (group_id, topic, partition)
Value: (offset, metadata, commit_timestamp)
```

### Database Selection

| Data | Storage | Rationale |
|------|---------|-----------|
| Message data | Custom append-only log files on XFS | Sequential I/O maximizes throughput; page cache serves reads |
| Offset index | Memory-mapped sparse index files | O(1) lookup after binary search on sparse entries |
| Cluster metadata | KRaft (Raft log) | Replicated, consistent metadata without external dependency |
| Consumer offsets | Compacted internal topic (`__consumer_offsets`) | Reuses Kafka's own replication for offset durability |

### Indexing Strategy

| Index Type | File | Lookup Pattern | Entry Size |
|------------|------|---------------|------------|
| Offset index | `.index` | Binary search on sparse entries (every 4KB of .log by default) | 8 bytes (4B relative offset + 4B file position) |
| Time index | `.timeindex` | Binary search on timestamp to find offset | 12 bytes (8B timestamp + 4B relative offset) |
| Transaction index | `.txnindex` | Aborted transaction ranges for read_committed consumers | Variable |

**Offset lookup algorithm:**
1. Binary search `.index` files to find segment containing target offset.
2. Within segment, binary search sparse `.index` to find nearest entry <= target.
3. Sequential scan from that file position in `.log` to exact offset.

---

## 5. API Design

### Producer/Consumer APIs

**Produce API (Kafka Protocol - ProduceRequest v9)**
```
ProduceRequest {
    transactional_id:  string?        // null for non-transactional
    acks:              int16          // 0, 1, -1 (all)
    timeout_ms:        int32          // max wait for acks
    topic_data: [{
        topic:         string
        partition_data: [{
            partition:  int32
            records:    RecordBatch    // compressed batch
        }]
    }]
}

ProduceResponse {
    responses: [{
        topic:     string
        partitions: [{
            partition:     int32
            error_code:    int16
            base_offset:   int64       // first offset assigned
            log_append_time: int64
        }]
    }]
    throttle_time_ms: int32
}
```

**Fetch API (FetchRequest v13)**
```
FetchRequest {
    replica_id:    int32              // -1 for consumers, broker_id for followers
    max_wait_ms:   int32              // long-poll timeout
    min_bytes:     int32              // min data before responding
    max_bytes:     int32
    isolation_level: int8             // 0=read_uncommitted, 1=read_committed
    topics: [{
        topic:     string
        partitions: [{
            partition:      int32
            fetch_offset:   int64     // consumer's current position
            log_start_offset: int64
            partition_max_bytes: int32
        }]
    }]
}
```

**Java Producer Pattern:**
```java
Properties props = new Properties();
props.put(ProducerConfig.BOOTSTRAP_SERVERS_CONFIG, "broker1:9092,broker2:9092");
props.put(ProducerConfig.KEY_SERIALIZER_CLASS_CONFIG, StringSerializer.class);
props.put(ProducerConfig.VALUE_SERIALIZER_CLASS_CONFIG, ByteArraySerializer.class);
props.put(ProducerConfig.ACKS_CONFIG, "all");
props.put(ProducerConfig.ENABLE_IDEMPOTENCE_CONFIG, true);      // sequence numbers
props.put(ProducerConfig.MAX_IN_FLIGHT_REQUESTS_PER_CONNECTION, 5); // safe with idempotence
props.put(ProducerConfig.LINGER_MS_CONFIG, 5);                  // batch for 5ms
props.put(ProducerConfig.BATCH_SIZE_CONFIG, 65536);             // 64KB batches
props.put(ProducerConfig.COMPRESSION_TYPE_CONFIG, "lz4");

KafkaProducer<String, byte[]> producer = new KafkaProducer<>(props);

// Async send with callback
producer.send(new ProducerRecord<>("orders", orderId, payload), (metadata, exception) -> {
    if (exception != null) {
        logger.error("Produce failed for key={}", orderId, exception);
        // Retry or DLQ
    } else {
        logger.debug("Produced to {}:{}@{}", metadata.topic(), metadata.partition(), metadata.offset());
    }
});
```

**Java Consumer Pattern (poll loop):**
```java
Properties props = new Properties();
props.put(ConsumerConfig.BOOTSTRAP_SERVERS_CONFIG, "broker1:9092");
props.put(ConsumerConfig.GROUP_ID_CONFIG, "order-processor");
props.put(ConsumerConfig.AUTO_OFFSET_RESET_CONFIG, "earliest");
props.put(ConsumerConfig.ENABLE_AUTO_COMMIT_CONFIG, false);       // manual commit
props.put(ConsumerConfig.MAX_POLL_RECORDS_CONFIG, 500);
props.put(ConsumerConfig.SESSION_TIMEOUT_MS_CONFIG, 30000);
props.put(ConsumerConfig.HEARTBEAT_INTERVAL_MS_CONFIG, 10000);
props.put(ConsumerConfig.MAX_POLL_INTERVAL_MS_CONFIG, 300000);   // 5 min processing budget

KafkaConsumer<String, byte[]> consumer = new KafkaConsumer<>(props);
consumer.subscribe(List.of("orders"));

while (running) {
    ConsumerRecords<String, byte[]> records = consumer.poll(Duration.ofMillis(100));
    for (ConsumerRecord<String, byte[]> record : records) {
        processRecord(record);  // business logic
    }
    consumer.commitSync();  // commit after processing entire batch
}
```

**Spring Kafka Pattern:**
```java
@KafkaListener(topics = "orders", groupId = "order-processor",
               containerFactory = "kafkaListenerContainerFactory")
public void onMessage(ConsumerRecord<String, byte[]> record, Acknowledgment ack) {
    try {
        processRecord(record);
        ack.acknowledge();  // manual ack mode
    } catch (RetriableException e) {
        throw e;  // SeekToCurrentErrorHandler will retry
    }
}

@Bean
public ConcurrentKafkaListenerContainerFactory<String, byte[]> kafkaListenerContainerFactory() {
    var factory = new ConcurrentKafkaListenerContainerFactory<>();
    factory.setConsumerFactory(consumerFactory());
    factory.setConcurrency(12);  // 12 threads = 12 partitions max parallelism
    factory.getContainerProperties().setAckMode(AckMode.MANUAL_IMMEDIATE);
    factory.setCommonErrorHandler(new DefaultErrorHandler(
        new DeadLetterPublishingRecoverer(kafkaTemplate),
        new FixedBackOff(1000L, 3)  // 3 retries, 1s apart
    ));
    return factory;
}
```

### Admin APIs

```
// Kafka AdminClient (Java)
AdminClient admin = AdminClient.create(props);

// Create topic
admin.createTopics(List.of(
    new NewTopic("orders", 64, (short) 3)  // 64 partitions, RF=3
        .configs(Map.of(
            "retention.ms", "604800000",        // 7 days
            "min.insync.replicas", "2",
            "cleanup.policy", "delete"
        ))
));

// Reassign partitions (for rebalancing after adding brokers)
admin.alterPartitionReassignments(reassignmentMap);

// Describe cluster
admin.describeCluster().nodes().get();
```

### CLI

```bash
# Topic management
kafka-topics.sh --bootstrap-server broker1:9092 --create \
    --topic orders --partitions 64 --replication-factor 3 \
    --config retention.ms=604800000 --config min.insync.replicas=2

kafka-topics.sh --bootstrap-server broker1:9092 --describe --topic orders

# Consumer group management
kafka-consumer-groups.sh --bootstrap-server broker1:9092 \
    --group order-processor --describe
#   TOPIC    PARTITION  CURRENT-OFFSET  LOG-END-OFFSET  LAG
#   orders   0          12345678        12345700        22

# Reset offsets
kafka-consumer-groups.sh --bootstrap-server broker1:9092 \
    --group order-processor --reset-offsets --to-datetime 2026-04-01T00:00:00.000 \
    --topic orders --execute

# Performance test
kafka-producer-perf-test.sh --topic test --num-records 10000000 \
    --record-size 1024 --throughput -1 \
    --producer-props bootstrap.servers=broker1:9092 acks=all compression.type=lz4

# Log dump (inspect segment)
kafka-dump-log.sh --files /kafka-logs/orders-0/00000000000000000000.log \
    --print-data-log --deep-iteration
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Partition Replication & Leader Election (ISR / HW / LEO)

**Why it's hard:**
Replication must balance durability (don't lose data) against availability (keep serving even when replicas fail) and latency (don't block producers waiting for slow replicas). The ISR mechanism is Kafka's solution to the CAP theorem trade-off.

**Approaches:**

| Approach | Durability | Availability | Latency | Complexity |
|----------|-----------|--------------|---------|------------|
| Synchronous replication (all replicas) | Highest | Low (any slow replica blocks) | High | Low |
| Asynchronous replication | Low (data loss on leader failure) | Highest | Lowest | Low |
| **ISR-based (Kafka)** | **High (configurable)** | **High** | **Medium** | **Medium** |
| Quorum-based (Raft/Paxos) | High | Medium | Medium | High |

**Selected: ISR-based replication**

**Key concepts:**

```
  Partition P0 (RF=3):  Broker 0 (Leader), Broker 1 (Follower), Broker 2 (Follower)

  Leader log:      [0] [1] [2] [3] [4] [5] [6] [7]    LEO = 8
  Follower 1 log:  [0] [1] [2] [3] [4] [5] [6]        LEO = 7
  Follower 2 log:  [0] [1] [2] [3] [4] [5]             LEO = 6

  ISR = {Broker 0, Broker 1, Broker 2}  (all within replica.lag.time.max.ms=30s)
  High Watermark (HW) = min(LEO of all ISR) = 6

  Consumer with isolation=read_committed sees offsets 0..5 (below HW).
  Producer with acks=all gets response once HW advances past their record.
```

**LEO (Log End Offset):** The offset of the next message to be written on each replica. Each replica tracks its own LEO.

**HW (High Watermark):** The minimum LEO across all ISR replicas. Only messages below HW are visible to consumers. This prevents consumers from reading data that might be lost if the leader fails.

**ISR (In-Sync Replicas):** The set of replicas (including leader) that are "caught up." A follower is removed from ISR if it hasn't fetched within `replica.lag.time.max.ms` (default 30s).

**Leader election algorithm (KRaft):**
```
on_leader_failure(partition):
    isr = metadata_cache.get_isr(partition)
    isr.remove(failed_broker)
    
    if isr.is_empty():
        if unclean_leader_election_enabled:
            # Pick any live replica (may lose data!)
            new_leader = pick_any_live_replica(partition)
        else:
            # Partition goes offline until ISR replica recovers
            partition.state = OFFLINE
            return
    else:
        # Pick first broker in ISR that is alive
        new_leader = isr.first_alive()
    
    # Update metadata
    metadata.set_leader(partition, new_leader)
    metadata.increment_leader_epoch(partition)
    broadcast_metadata_update(all_brokers)
```

**acks semantics:**

| acks | Behavior | Durability | Latency |
|------|----------|-----------|---------|
| 0 | Don't wait for any broker response | None (fire-and-forget) | < 1ms |
| 1 | Wait for leader to write to page cache | Leader crash = data loss | ~2-5ms |
| all (-1) | Wait for all ISR replicas to acknowledge | No loss if 1+ ISR survives | ~5-15ms |

**Critical config: `min.insync.replicas`**
With `acks=all` and `min.insync.replicas=2` (RF=3): the producer gets an error if fewer than 2 replicas are in ISR. This prevents `acks=all` from degenerating to `acks=1` when followers fail.

**Failure Modes:**
- **Leader crashes with acks=all, min.insync.replicas=2:** No data loss. New leader elected from ISR. At most, in-flight batches not yet acknowledged are lost (producer retries).
- **Leader crashes with acks=1:** Messages written to leader page cache but not yet replicated are lost.
- **All ISR replicas crash:** Partition offline (unless `unclean.leader.election.enable=true`, which risks data loss).
- **Slow follower:** Removed from ISR after `replica.lag.time.max.ms`. Rejoins after catching up.

**Interviewer Q&As:**

**Q1: Why does Kafka use ISR instead of majority quorum like Raft?**
A: With RF=3 and quorum, you need 2/3 acks for durability. With ISR and `min.insync.replicas=2`, you also need 2 acks. The difference is in failure handling: ISR dynamically shrinks, so a partition with 1 surviving ISR replica can still serve reads (though not writes if min.insync.replicas=2). Quorum requires a static majority. ISR also avoids the latency penalty of always waiting for the slowest quorum member since slow replicas are removed from ISR.

**Q2: What happens if HW hasn't advanced but the leader crashes?**
A: The new leader's LEO becomes the new HW. Messages between the old HW and the new leader's LEO are now committed. Messages on the old leader that weren't replicated to the new leader are lost (they were never committed/visible to consumers). Producers with acks=all would not have received success for those messages.

**Q3: Can a consumer ever read a message that is later "un-committed"?**
A: No, consumers only see messages below HW. After leader failure, the new HW may be lower than the old leader's LEO, but consumers never saw those uncommitted messages.

**Q4: How does leader epoch prevent the "split brain" scenario?**
A: Each leader election increments the leader epoch. When an old leader comes back, it fetches the leader epoch checkpoint from the current leader. It truncates its log to the epoch boundary, preventing divergent data from being served.

**Q5: What is the impact of setting `unclean.leader.election.enable=true`?**
A: An out-of-sync replica can become leader, causing data loss (committed messages on the old leader that the new leader never received). Use only for topics where availability trumps durability (e.g., metrics).

**Q6: How does `replica.lag.time.max.ms` affect ISR?**
A: If a follower doesn't send a fetch request within this interval, or its fetch request offset lags behind the leader's LEO for this long, the leader removes it from ISR. Setting it too low causes ISR thrashing during GC pauses or network blips. Setting it too high means a slow follower delays acks=all produce latency.

---

### Deep Dive 2: Storage Engine (Segment Files, Page Cache, Zero-Copy)

**Why it's hard:**
A message broker must sustain extremely high write AND read throughput simultaneously, while keeping data durable. Traditional databases use B-trees with random I/O; Kafka achieves 10-100x better throughput by treating storage as a sequential append-only log and exploiting OS page cache.

**Approaches:**

| Approach | Write Throughput | Read Throughput | Complexity |
|----------|-----------------|-----------------|------------|
| B-tree database (MySQL, Postgres) | Low (random I/O) | Medium | Low |
| LSM-tree (RocksDB, LevelDB) | High (sequential write) | Medium (compaction overhead) | Medium |
| **Append-only log + page cache** | **Highest** | **Highest (zero-copy)** | **Medium** |
| Direct I/O with custom cache | High | High | Very High |

**Selected: Append-only log + OS page cache**

**Why sequential writes beat random writes:**
```
NVMe SSD Performance:
  Random 4KB write:  ~100,000 IOPS = 400 MB/s
  Sequential write:  ~3,000 MB/s (7.5x faster)
  
HDD Performance (for comparison):
  Random 4KB write:  ~200 IOPS = 0.8 MB/s
  Sequential write:  ~200 MB/s (250x faster!)
```

**Page cache exploitation:**
```
Write path:
  Producer batch -> Broker JVM heap (network buffer) 
    -> write() syscall -> OS page cache (DRAM) -> Async flush to disk
  
  The write() returns immediately after copying to page cache.
  Durability comes from replication, not fsync (flush.messages defaults to Long.MAX_VALUE).
  
Read path (hot data, consumer is near tail):
  Consumer FetchRequest -> Broker
    -> sendfile() syscall -> DMA from page cache -> NIC buffer -> Network
  
  Zero-copy: data never enters JVM heap. No user-space copy.
  Standard read() would: disk -> page cache -> kernel buffer -> user buffer -> kernel buffer -> NIC
  sendfile() shortcut: page cache -> NIC (2 copies eliminated)
```

**Why Kafka achieves 600 MB/s write, 2 GB/s read:**
```
Write (600 MB/s per broker):
  - Sequential append to log file (no seek overhead)
  - Batched writes (many records per write() syscall)
  - LZ4 compression (30-70% compression ratio, CPU < disk bottleneck)
  - OS page cache absorbs write bursts (64+ GB RAM per broker)
  - No fsync per message (replication provides durability)

Read (2 GB/s per broker):
  - Hot tail (last few minutes) always in page cache
  - sendfile() zero-copy: kernel DMA from page cache directly to NIC
  - Sequential read-ahead by OS (up to 128 pages prefetched)
  - Multiple consumers reading same data: single page cache read serves all
  - No JVM GC pressure: data never enters heap
```

**Segment file management:**
```
Segment lifecycle:
  1. Active segment: currently being written to
  2. Roll trigger: segment.bytes (1GB default) or segment.ms (7 days default)
  3. Rolled segment: immutable, eligible for retention cleanup
  4. Deleted segment: removed after retention.ms or retention.bytes exceeded

Segment file structure:
  .log file:
    [RecordBatch 0][RecordBatch 1]...[RecordBatch N]  (sequential, compressed)
    
  .index file (sparse, memory-mapped):
    [relative_offset=0, position=0]
    [relative_offset=512, position=32768]     # entry every 4KB of .log
    [relative_offset=1024, position=65536]
    ...
    
  .timeindex file (sparse, memory-mapped):
    [timestamp=1712000000000, relative_offset=0]
    [timestamp=1712000001000, relative_offset=512]
    ...
```

**Offset lookup pseudocode:**
```python
def fetch_records(topic, partition, target_offset, max_bytes):
    # 1. Find the right segment
    segments = sorted(partition.segments)  # sorted by base_offset
    segment = binary_search(segments, target_offset)  # largest base_offset <= target
    
    # 2. Find position in segment via sparse index
    index = mmap(segment.index_file)
    entry = binary_search(index, target_offset - segment.base_offset)
    file_position = entry.position
    
    # 3. Sequential scan from position to exact offset
    reader = open(segment.log_file)
    reader.seek(file_position)
    while reader.offset < target_offset:
        batch = reader.read_next_batch()
    
    # 4. Return records via sendfile (zero-copy)
    return sendfile(socket_fd, segment.log_file.fd, file_position, max_bytes)
```

**Failure Modes:**
- **Page cache data loss (power failure):** Mitigated by replication. At least `min.insync.replicas` have the data in their page cache (on different physical machines). All losing power simultaneously is extremely unlikely.
- **Corrupt segment file:** CRC per record batch detects corruption. Follower re-fetches from leader if CRC mismatch.
- **Disk full:** Broker goes into controlled shutdown. Retention policy triggers emergency cleanup. Alert on disk usage > 80%.

**Interviewer Q&As:**

**Q1: Why doesn't Kafka fsync every message?**
A: fsync forces page cache to disk, turning a 600 MB/s sequential write into a ~50 MB/s sync write. Kafka relies on replication for durability: even if one broker loses page cache data (kernel crash), the other ISR replicas have it. For max durability, set `flush.messages=1`, accepting massive throughput reduction.

**Q2: What happens when a consumer is reading old data (not in page cache)?**
A: The OS evicts other pages to load the segment from disk. This "pollutes" the page cache and can affect tail consumers reading recent data. Solution: use tiered storage (KIP-405) to move old data to object storage, or assign lagging consumers to follower replicas (`replica.selector.class=RackAwareReplicaSelector`).

**Q3: How does memory-mapping the .index file help?**
A: mmap makes the OS manage the index in page cache transparently. Binary search on the index only touches a few pages (log2(entries)), which are almost always cached. No JVM heap allocation required.

**Q4: Why XFS over ext4 for Kafka log directories?**
A: XFS handles concurrent file operations better (per-allocation-group locking), supports larger files, and has better metadata performance. ext4's journal can become a bottleneck with many concurrent appends.

**Q5: How does Kafka handle compaction without blocking writes?**
A: Log compaction runs on a background thread pool (`log.cleaner.threads`). It reads "dirty" (uncompacted) segments, deduplicates by key keeping only the latest value, and writes new compacted segments. The active segment is never compacted. A read lock on segments prevents deletion during reads.

**Q6: What is the relationship between batch.size, linger.ms, and throughput?**
A: `batch.size` (default 16KB) is the max bytes per partition batch. `linger.ms` (default 0) adds artificial delay to accumulate more records. With linger.ms=0, a batch sends immediately on send(). With linger.ms=5ms and batch.size=64KB, the producer waits up to 5ms or until 64KB accumulated, whichever comes first. Larger batches = better compression = fewer I/O operations = higher throughput, but higher latency.

---

### Deep Dive 3: Consumer Groups & Rebalancing

**Why it's hard:**
When consumers join, leave, or crash, partitions must be reassigned. During rebalancing, consumption stops, creating processing gaps. The challenge is minimizing rebalance duration and the "stop-the-world" effect.

**Approaches:**

| Approach | Stop-the-World | Incremental | Stickiness | Complexity |
|----------|---------------|-------------|------------|------------|
| Eager rebalance (original) | Yes (all revoked) | No | No | Low |
| Sticky assignor (eager) | Yes | No | Yes (minimizes movement) | Medium |
| **Cooperative sticky (incremental)** | **No** | **Yes** | **Yes** | **High** |
| Static group membership | Partial (no rebalance on restart) | No | Yes | Low |

**Selected: Cooperative Sticky Assignor + Static Group Membership**

**Consumer group protocol:**
```
Consumer Group Lifecycle:
  
  1. Consumer C1 starts, sends JoinGroup to Group Coordinator (broker)
  2. Coordinator assigns C1 as group leader
  3. C1 receives all member subscriptions, runs assignment algorithm
  4. C1 sends assignment back via SyncGroup
  5. C2 starts, sends JoinGroup -> triggers rebalance
  6. All consumers receive new assignment

Cooperative Rebalance (2-phase):
  Phase 1: All consumers join. Leader computes new assignment.
            Only REVOKED partitions are stopped. Owned partitions continue.
  Phase 2: Revoked partitions are assigned to new owners.
            No "stop-the-world" gap.
  
  Example:
    Before: C1=[P0,P1,P2,P3], C2=[P4,P5,P6,P7]
    C3 joins.
    Phase 1: Leader computes C1=[P0,P1,P2], C2=[P4,P5,P6], C3=[P3,P7]
             C1 revokes P3, C2 revokes P7. Other partitions keep processing.
    Phase 2: C3 receives P3, P7. Starts consuming.
```

**Static group membership:**
```java
props.put(ConsumerConfig.GROUP_INSTANCE_ID_CONFIG, "consumer-host-42");
// On restart, same consumer ID rejoins without triggering rebalance
// Rebalance only triggers after session.timeout.ms expires
```

**Offset management:**
```
Two types of offset per consumer:
  - Position (current): offset of next record to be returned by poll()
  - Committed: last offset acknowledged by consumer (stored in __consumer_offsets)

Auto-commit: every auto.commit.interval.ms (5s default), position is committed.
  Risk: crash after commit but before processing = messages skipped (at-most-once).
  
Manual commit (recommended for at-least-once):
  consumer.poll() -> process all records -> commitSync()
  Risk: crash after processing but before commit = messages reprocessed (at-least-once).
  
Exactly-once (with transactions):
  consumer.poll() -> 
  producer.beginTransaction() ->
  produce results + consumer.sendOffsetsToTransaction() ->
  producer.commitTransaction()
```

**Failure Modes:**
- **Consumer crash (no clean shutdown):** Coordinator detects via session timeout (session.timeout.ms=30s). Triggers rebalance. During timeout window, partitions are unprocessed.
- **Long processing time:** If poll() not called within `max.poll.interval.ms` (5min default), consumer is kicked from group. Solution: increase timeout or use pause()/resume().
- **Rebalance storm:** Frequent joins/leaves cause continuous rebalancing. Fix: static group membership, increase `session.timeout.ms`, use cooperative assignor.

**Interviewer Q&As:**

**Q1: What is the difference between `session.timeout.ms` and `max.poll.interval.ms`?**
A: `session.timeout.ms` (30s) is for heartbeat-based liveness detection: if the coordinator doesn't receive a heartbeat within this window, the consumer is considered dead. `max.poll.interval.ms` (5min) is for processing-based liveness: if the consumer doesn't call poll() within this window, it is considered stuck and gets kicked. The heartbeat runs on a separate thread, so a slow poll() processing loop won't trigger session timeout but will trigger max.poll.interval timeout.

**Q2: How does cooperative rebalancing reduce downtime?**
A: In eager rebalancing, ALL partitions are revoked from ALL consumers before reassignment. Processing stops entirely for the rebalance duration (seconds to minutes with many partitions). In cooperative, only partitions that change ownership are revoked. The rest continue processing. Total rebalance still takes the same time, but the impact is 10-100x smaller.

**Q3: What happens to uncommitted messages during rebalance?**
A: The new owner starts consuming from the last committed offset. Messages processed but not committed by the old owner are reprocessed. This is why at-least-once semantics require idempotent processing.

**Q4: How do you handle a consumer that processes slowly?**
A: (1) Increase `max.poll.records` to reduce poll() frequency. (2) Decrease `max.poll.records` to return fewer records per poll, keeping poll interval short. (3) Use `consumer.pause(partitions)` to stop fetching while processing. (4) Process in a thread pool, committing offsets for completed batches. (5) Increase `max.poll.interval.ms`.

**Q5: How many consumers can be in a group?**
A: At most one consumer per partition. Extra consumers are idle. For 64 partitions, max useful consumer count is 64. If you need more parallelism, increase partition count.

**Q6: What is the consumer group coordinator?**
A: A specific broker responsible for managing a consumer group. Determined by: `hash(group.id) % __consumer_offsets partition count` -> leader of that partition. Handles JoinGroup, SyncGroup, Heartbeat, LeaveGroup, OffsetCommit RPCs.

---

### Deep Dive 4: KRaft Mode (ZooKeeper Replacement)

**Why it's hard:**
ZooKeeper was a single point of failure and scaling bottleneck for metadata operations. The KRaft mode embeds a Raft-based metadata quorum directly in the Kafka brokers, eliminating the external dependency while maintaining strong consistency for metadata.

**Approaches:**

| Approach | Dependency | Metadata Latency | Scaling | Complexity |
|----------|-----------|-------------------|---------|------------|
| ZooKeeper (legacy) | External ZK cluster | ~10ms | Limited by ZK | High (two systems) |
| etcd (Raft) | External etcd cluster | ~5ms | Better than ZK | High (two systems) |
| **KRaft (embedded Raft)** | **None** | **~2ms** | **Millions of partitions** | **Medium** |

**Selected: KRaft**

**Architecture:**
```
KRaft Cluster:
  
  Controller Quorum (3 or 5 nodes, Raft):
    +-- Active Controller (Raft leader): processes metadata changes
    +-- Standby Controller: Raft followers, ready for failover
    +-- Standby Controller: Raft followers
  
  Broker Nodes (N nodes):
    +-- Metadata cache (in-memory copy of metadata log)
    +-- Fetch metadata updates from controller quorum
  
  Metadata topics stored in Raft log:
    - __cluster_metadata: broker registrations, topic configs, 
                          partition assignments, ISR changes,
                          ACLs, quotas, client configs
  
  Key advantage: 
    ZK watch bottleneck eliminated. Metadata propagation via 
    efficient pull-based replication (brokers fetch from controller).
    Supports millions of partitions (ZK struggled past 200K).
```

**Metadata propagation:**
```
1. Admin creates topic: CreateTopicsRequest -> Active Controller
2. Controller validates, appends to metadata log (Raft commit)
3. Raft followers replicate the log entry
4. Brokers fetch new metadata records (incremental pull)
5. Each broker updates its in-memory metadata cache
6. Broker starts accepting produce/fetch for new partitions
```

**Failure Modes:**
- **Active controller crashes:** Raft elects new leader in ~1-5 seconds. During election, metadata changes are blocked, but existing produce/consume continues.
- **Quorum loss (majority of controllers down):** Metadata changes blocked. Existing topics continue working with stale metadata. Critical: no partition reassignment, no new ISR updates.
- **Metadata log grows unbounded:** Periodic snapshots truncate the log. Brokers can bootstrap from snapshot + incremental changes.

**Interviewer Q&As:**

**Q1: Why not just use etcd or Consul instead of building KRaft?**
A: External systems add operational complexity (separate cluster to manage, monitor, upgrade). They also have metadata model mismatches: ZK/etcd store key-value data, but Kafka metadata is a log of changes. KRaft's log-based approach is a natural fit and enables efficient incremental propagation.

**Q2: How does KRaft handle partition reassignment?**
A: The active controller writes a reassignment record to the metadata log. The target broker fetches the record, starts replicating the partition from the current leader, and once caught up, the controller updates the assignment. This is similar to ZK-based reassignment but with faster propagation.

**Q3: What is the metadata log snapshot?**
A: Periodically, the controller writes a complete snapshot of all metadata at a given offset. Brokers joining or recovering can load the snapshot instead of replaying the entire log. Similar to Raft log compaction/snapshotting.

**Q4: Can combined mode (controller + broker on same node) be used in production?**
A: For small clusters (< 10 brokers), combined mode reduces operational overhead. For large clusters, dedicated controller nodes prevent metadata operations from competing with broker I/O. LinkedIn runs dedicated controllers.

**Q5: How does broker registration work in KRaft?**
A: Brokers send a BrokerRegistrationRequest to the active controller, which writes a registration record to the metadata log. Brokers must send periodic heartbeats; if missed, the controller fences the broker (marks it as unavailable) and reassigns its partition leaderships.

**Q6: What is the maximum partition count supported by KRaft vs ZooKeeper?**
A: ZooKeeper struggled beyond 200,000 partitions due to watch notification overhead and znode creation latency. KRaft scales to millions of partitions because metadata propagation is log-based (incremental) rather than watch-based (full state per notification).

---

## 7. Scheduling & Resource Management

### Broker Resource Management

| Resource | Management Strategy | Config |
|----------|-------------------|--------|
| CPU | Network threads (`num.network.threads=8`) + I/O threads (`num.io.threads=16`) | Separate thread pools prevent slow consumers from blocking producers |
| Memory | JVM heap (6-8 GB) + Page cache (remaining RAM, 50-200 GB) | Small heap prevents GC pauses; page cache serves reads |
| Disk I/O | Multiple log directories across disks (`log.dirs=/disk1,/disk2,...`) | Round-robin partition assignment across disks |
| Network | Per-client quotas (`quota.producer.default`, `quota.consumer.default`) | Prevent single client from saturating NIC |

### Partition Assignment Strategy

```python
def assign_partitions_to_brokers(topic, num_partitions, replication_factor, brokers):
    """Rack-aware partition assignment"""
    racks = group_by_rack(brokers)
    assignments = {}
    
    for partition_id in range(num_partitions):
        # Leader: round-robin across brokers
        leader_idx = partition_id % len(brokers)
        leader = brokers[leader_idx]
        
        # Followers: different racks, then different brokers in same rack
        replicas = [leader]
        used_racks = {leader.rack}
        
        for _ in range(replication_factor - 1):
            # Prefer broker in different rack
            candidate = next_broker_different_rack(brokers, used_racks, leader_idx)
            if candidate is None:
                candidate = next_broker_same_rack(brokers, replicas, leader_idx)
            replicas.append(candidate)
            used_racks.add(candidate.rack)
        
        assignments[partition_id] = replicas
    
    return assignments
```

### Integration with Kubernetes

```yaml
# Kafka on Kubernetes (Strimzi operator)
apiVersion: kafka.strimzi.io/v1beta2
kind: Kafka
metadata:
  name: production-cluster
spec:
  kafka:
    replicas: 60
    version: 3.7.0
    config:
      num.partitions: 12
      default.replication.factor: 3
      min.insync.replicas: 2
      log.retention.hours: 168
    storage:
      type: persistent-claim
      size: 4Ti
      class: local-nvme          # Local NVMe for performance
      deleteClaim: false
    resources:
      requests:
        memory: 64Gi
        cpu: "8"
      limits:
        memory: 64Gi             # No overcommit on memory
    rack:
      topologyKey: topology.kubernetes.io/zone
    template:
      pod:
        affinity:
          podAntiAffinity:
            requiredDuringSchedulingIgnoredDuringExecution:
              - labelSelector:
                  matchLabels:
                    strimzi.io/name: production-cluster-kafka
                topologyKey: kubernetes.io/hostname
```

### Bare-Metal Deployment Considerations

| Aspect | Recommendation | Why |
|--------|---------------|-----|
| CPU | 16-24 cores, prefer clock speed over core count | Kafka is not CPU-bound; higher clock helps compress/decompress |
| RAM | 64-256 GB | Maximize page cache. JVM heap only needs 6-8 GB |
| Disk | 12-24 NVMe SSDs per broker, JBOD (no RAID) | RAID wastes capacity for parity; Kafka replication provides redundancy |
| Network | 25 Gbps NIC minimum | Network often the bottleneck before disk |
| OS | Linux 5.x+, XFS, vm.swappiness=1, transparent huge pages OFF | Tuning critical for consistent latency |

---

## 8. Scaling Strategy

### Horizontal Scaling

| Dimension | Scaling Mechanism | Limit |
|-----------|------------------|-------|
| Write throughput | Add brokers + increase partitions | Limited by partition count (can't reduce partitions) |
| Read throughput | Add consumers to group (up to partition count) | 1 consumer per partition per group |
| Storage | Add brokers with new disks, reassign partitions | Online partition reassignment (throttled) |
| Topics | Unlimited (metadata overhead per topic) | ~1M topics with KRaft |
| Consumer groups | Unlimited (coordinator per group) | Memory for offset storage |

### Partition Count Sizing

```
Target throughput: 500 MB/s for a topic
Single partition throughput: ~10 MB/s (producer side, acks=all, lz4)
Partition count: 500 / 10 = 50 partitions

Rule of thumb:
  partition_count = max(
      target_throughput / per_partition_throughput,
      expected_consumer_count          # parallelism
  )
  
  # Round up to power of 2 for clean key distribution
  # Over-partition slightly (2x) for future growth -- can't reduce later
```

### Broker Addition Workflow

```
1. Add new broker to cluster (registers via KRaft)
2. New broker has 0 partitions (doesn't automatically get data)
3. Run kafka-reassign-partitions.sh to move partitions to new broker
4. Throttle replication: --throttle 100000000 (100 MB/s)
5. Monitor reassignment progress
6. Verify ISR includes new broker
7. Remove throttle
```

**Interviewer Q&As:**

**Q1: How do you scale a Kafka topic that needs more throughput than current partitions allow?**
A: Increase partition count with `kafka-topics.sh --alter --partitions N`. But: (1) key-based partitioning changes: `hash(key) % new_count != hash(key) % old_count`, breaking ordering for some keys. (2) You cannot decrease partitions. (3) Existing data stays in old partitions; only new messages go to new partitions. Best practice: over-provision partitions initially.

**Q2: What happens when you add brokers to a running cluster?**
A: New brokers get no partitions automatically. You must manually reassign partitions. Use `kafka-reassign-partitions.sh` with throttling to limit replication traffic. The process is online; producers and consumers are not affected. Cruise Control can automate this.

**Q3: How do you handle a topic with 1 million partitions?**
A: You don't. More partitions = more file descriptors, more memory for index/page cache, longer leader election on broker failure, more rebalance time for consumers. Guideline: < 4,000 partitions per broker, < 200,000 total per cluster (ZK) or ~1M (KRaft).

**Q4: How do you scale consumers beyond the partition count?**
A: You can't within a single consumer group. Options: (1) Increase partitions. (2) Use multiple consumer groups (each gets full copy). (3) Consumer-side thread pool: one consumer thread feeds a pool of worker threads (manual offset management required).

**Q5: How do you handle a hot partition?**
A: Caused by skewed key distribution. Options: (1) Add a random suffix to the key ("key-{0,1,2}" to spread across 3 partitions, aggregate downstream). (2) Custom partitioner that spreads hot keys. (3) If ordering not needed for hot key, use round-robin.

**Q6: What is the impact of increasing replication factor from 3 to 5?**
A: (1) 67% more replication network traffic per broker. (2) 67% more disk usage. (3) Higher produce latency with acks=all (must wait for more replicas). (4) Better durability: can survive 4 failures vs 2. (5) No read throughput improvement (only leader serves reads by default).

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Detection | Impact | Mitigation | RTO |
|---------|-----------|--------|------------|-----|
| Single broker crash | KRaft heartbeat timeout (9s default) | Partitions led by broker go offline briefly | Leader election for affected partitions; ISR replicas take over | 1-10s |
| Disk failure (one disk) | I/O error on segment write/read | Partitions on that disk go offline | Broker detects, logs error, admin replaces disk; JBOD limits blast radius | Minutes (manual) |
| Network partition (broker isolated) | Heartbeat timeout | Broker fenced; partitions reassigned | ISR shrinks; leader election if leader isolated | 10-30s |
| Full rack failure | Multiple broker heartbeat timeouts | Major partition leadership changes | Rack-aware replica placement ensures replicas survive | 10-30s |
| Controller quorum loss | Raft election timeout | No metadata changes (no new topics, no ISR updates) | Existing produce/consume continues. Restore quorum ASAP | Manual recovery |
| Consumer crash | Session timeout (30s) | Partitions unprocessed during timeout | Reduce session.timeout.ms; use static group membership | 30s default |
| Producer crash | Application-level | In-flight messages lost | Idempotent producer retries; transactional producer aborts | App restart time |
| Page cache loss (kernel panic) | Broker restart | Uncommitted data on that broker lost | ISR replicas have the data; follower catches up on restart | Broker restart time |
| Corrupt segment | CRC check on read | Consumer errors for affected offsets | Broker re-fetches from leader; unrecoverable if leader | Automatic (follower) |
| ZK/KRaft split brain | Leader epoch mismatch | Stale metadata served briefly | Leader epoch check on every produce/fetch; client refreshes metadata | < 1s |

### Delivery Semantics

| Semantic | Producer Config | Consumer Config | Trade-off |
|----------|----------------|-----------------|-----------|
| **At-most-once** | acks=0 or acks=1 (no retry) | auto.commit=true | Fastest, possible message loss |
| **At-least-once** | acks=all, retries=MAX, idempotence=true | auto.commit=false, commitSync() after processing | No loss, possible duplicates |
| **Exactly-once** | Transactional producer (beginTransaction, commitTransaction) | read_committed isolation, sendOffsetsToTransaction | No loss, no duplicates; highest latency |

**Exactly-once deep dive:**
```java
// Transactional producer
producer.initTransactions();

while (running) {
    ConsumerRecords<String, byte[]> records = consumer.poll(Duration.ofMillis(100));
    
    producer.beginTransaction();
    try {
        for (ConsumerRecord<String, byte[]> record : records) {
            // Process and produce result
            ProducerRecord<String, byte[]> result = process(record);
            producer.send(result);
        }
        
        // Atomically commit: produced records + consumer offsets
        producer.sendOffsetsToTransaction(
            currentOffsets(records),
            consumer.groupMetadata()
        );
        producer.commitTransaction();
    } catch (Exception e) {
        producer.abortTransaction();
    }
}
```

The transaction coordinator on the broker ensures:
1. All produced records are written atomically (visible only after commit).
2. Consumer offsets are committed atomically with produced records.
3. Aborted transactions: records are written but marked; `read_committed` consumers skip them using the `.txnindex`.

---

## 10. Observability

### Key Metrics

| Category | Metric | Alert Threshold | Why |
|----------|--------|----------------|-----|
| Throughput | `kafka.server:BytesInPerSec` | > 80% NIC capacity | Approaching network saturation |
| Throughput | `kafka.server:MessagesInPerSec` | Depends on baseline | Traffic spike or drop |
| Latency | `kafka.network:RequestMetrics:TotalTimeMs` (Produce) | p99 > 100ms | Slow produces indicate disk/replication issues |
| Latency | `kafka.network:RequestMetrics:TotalTimeMs` (Fetch) | p99 > 200ms | Slow fetches; page cache miss |
| Replication | `kafka.server:ReplicaManager:UnderReplicatedPartitions` | > 0 for > 5min | Follower can't keep up; data at risk |
| Replication | `kafka.server:ReplicaManager:UnderMinIsrPartitionCount` | > 0 | acks=all produces will fail |
| Replication | `kafka.controller:OfflinePartitionsCount` | > 0 | No leader available; data unavailable |
| Consumer | `kafka.consumer:ConsumerFetcherManager:MaxLag` | > 100,000 | Consumer falling behind |
| Consumer | `kafka.server:FetcherLagMetrics:ConsumerLag` per group | > 10min of data | Consumer stuck or slow |
| Disk | `kafka.log:LogFlushRateAndTimeMs` | p99 > 1000ms | Disk I/O issues |
| Disk | Disk usage % per log directory | > 80% | Retention not keeping up |
| JVM | GC pause time (G1GC) | > 200ms | GC affecting produce/consume latency |
| Network | `kafka.network:RequestChannel:RequestQueueSize` | > 100 | Network threads overwhelmed |
| Controller | `kafka.controller:ActiveControllerCount` | != 1 | No active controller or split brain |

### Distributed Tracing

```
Trace propagation via Kafka headers:
  Producer:
    record.headers().add("traceparent", "00-traceId-spanId-01".getBytes());
    record.headers().add("baggage", "env=prod".getBytes());
  
  Consumer:
    String traceparent = new String(record.headers().lastHeader("traceparent").value());
    // Continue trace context in consumer processing span

Integration with OpenTelemetry:
  - kafka-clients instrumentation: auto-instruments produce/consume
  - Traces: Producer span -> Broker span (optional) -> Consumer span
  - Link produce and consume spans via message offset
  
End-to-end latency tracing:
  - CreateTime in record timestamp -> consumer processing timestamp
  - Metric: consumer_processing_time - record_create_time = end-to-end latency
```

### Logging

```
Broker logging (log4j2):
  - kafka.controller: INFO (metadata changes)
  - kafka.server: WARN (operational issues)
  - kafka.network: WARN (request timeouts)
  - kafka.log: WARN (segment issues, retention)
  - state-change.log: all leadership/ISR changes (critical for debugging)
  
Application logging:
  - Log every failed produce with topic, partition, key, error
  - Log consumer rebalance events (partitions assigned/revoked)
  - Log commit failures with offset details
  - Log consumer lag every 60s per partition
  - Structured logging (JSON) for aggregation in Elasticsearch
  
Log aggregation pipeline:
  Application -> Filebeat -> Kafka (logs topic) -> Logstash -> Elasticsearch -> Kibana
  (Yes, Kafka logs its own issues to Kafka -- separate cluster recommended)
```

---

## 11. Security

### Authentication

| Method | Use Case | Config |
|--------|----------|--------|
| SASL/SCRAM-SHA-512 | Service accounts (Java clients) | `sasl.mechanism=SCRAM-SHA-512` |
| mTLS (SSL) | Service-to-service with cert-based identity | `ssl.client.auth=required` |
| SASL/OAUTHBEARER | Integration with OAuth2/OIDC (Keycloak, Okta) | Token-based, short-lived |
| SASL/GSSAPI (Kerberos) | Enterprise environments with Active Directory | `sasl.mechanism=GSSAPI` |

### Authorization

```
# ACL examples
kafka-acls.sh --bootstrap-server broker1:9092 \
    --add --allow-principal User:order-service \
    --operation Write --topic orders

kafka-acls.sh --bootstrap-server broker1:9092 \
    --add --allow-principal User:analytics-service \
    --operation Read --topic orders --group analytics-consumer

# Deny all by default
authorizer.class.name=kafka.security.authorizer.AclAuthorizer
allow.everyone.if.no.acl.found=false
```

### Encryption

| Layer | Mechanism | Impact |
|-------|-----------|--------|
| In-transit (client-broker) | TLS 1.3 | ~20% throughput reduction |
| In-transit (inter-broker) | TLS 1.3 | Same |
| At-rest | Filesystem encryption (LUKS/dm-crypt) or cloud KMS | ~5% overhead with AES-NI |
| Field-level | Application-level encryption of value bytes | No broker-side overhead |

### Network Security

```
Kafka listeners (multi-protocol):
  listeners=INTERNAL://0.0.0.0:9092,EXTERNAL://0.0.0.0:9093,REPLICATION://0.0.0.0:9094
  
  listener.security.protocol.map=
    INTERNAL:SASL_PLAINTEXT,    # Internal VPC (no TLS for perf)
    EXTERNAL:SASL_SSL,          # External clients (TLS required)
    REPLICATION:SASL_PLAINTEXT  # Inter-broker (dedicated VLAN)
  
  inter.broker.listener.name=REPLICATION
```

### Quotas & Rate Limiting

```
# Produce quota: 50 MB/s per client
kafka-configs.sh --bootstrap-server broker1:9092 \
    --alter --add-config 'producer_byte_rate=52428800' \
    --entity-type clients --entity-name order-service

# Consumer quota: 100 MB/s per client
kafka-configs.sh --bootstrap-server broker1:9092 \
    --alter --add-config 'consumer_byte_rate=104857600' \
    --entity-type clients --entity-name analytics-service

# Request rate quota: 200 requests/s
kafka-configs.sh --bootstrap-server broker1:9092 \
    --alter --add-config 'request_percentage=20' \
    --entity-type clients --entity-name batch-processor
```

---

## 12. Incremental Rollout Strategy

### Phase 1: Shadow Cluster (Week 1-2)
- Deploy 5-broker cluster alongside existing message queue (RabbitMQ/SQS).
- Dual-write from producers: existing MQ + Kafka.
- Validate message ordering, throughput, latency.
- No consumers on Kafka yet.

### Phase 2: Mirror + Validate (Week 3-4)
- Add Kafka consumers that validate against existing MQ consumers.
- Compare message counts, content hashes, processing latency.
- Run chaos tests: kill brokers, network partitions.

### Phase 3: Canary Migration (Week 5-6)
- Migrate one low-risk consumer group to Kafka.
- Monitor consumer lag, error rates, processing latency.
- Feature flag to switch back to old MQ within minutes.

### Phase 4: Gradual Cutover (Week 7-10)
- Migrate consumer groups one by one, ordered by criticality (least critical first).
- Switch producers to Kafka-only for migrated topics.
- Keep old MQ running as fallback.

### Phase 5: Full Migration (Week 11-12)
- All producers and consumers on Kafka.
- Decommission old MQ.
- Document runbooks, on-call procedures.

**Rollout Q&As:**

**Q1: How do you handle schema changes during migration?**
A: Use Schema Registry with backward-compatible schemas (Avro). Old consumers can read new messages and vice versa. Never make breaking schema changes during migration.

**Q2: How do you validate no messages are lost during dual-write?**
A: Each message gets a UUID. Both MQ and Kafka consumers log UUIDs. A reconciliation job compares sets: any UUID in MQ but not Kafka indicates a loss. Run for 48+ hours before trusting.

**Q3: What if Kafka latency is worse than expected?**
A: Profile: (1) Produce latency: check acks setting, batch size, linger.ms. (2) Replication latency: check replica.lag.time.max.ms, network bandwidth. (3) Consume latency: check page cache hit ratio, consumer fetch.min.bytes. (4) End-to-end: check consumer lag.

**Q4: How do you rollback if Kafka has issues?**
A: Feature flag per consumer group. Flipping the flag redirects consumers back to old MQ. Producers continue dual-writing until rollback is no longer needed. Data in Kafka is retained for replay if needed.

**Q5: How do you handle the operational learning curve?**
A: (1) Runbook for every alert (under-replicated partitions, offline partitions, consumer lag). (2) Chaos engineering drills: monthly broker kills, quarterly rack failure simulation. (3) On-call rotation with Kafka-specific training. (4) Dashboards in Grafana with clear thresholds.

**Q6: How do you size the initial cluster?**
A: (1) Measure current MQ throughput (MB/s, msgs/s). (2) Apply 3x headroom for replication and growth. (3) Start with fewer, larger brokers (easier to manage). (4) Add brokers as needed (partition reassignment). Example: 500 MB/s current -> 1.5 GB/s with RF=3 -> 1.5 GB/s / 600 MB/s per broker = 3 brokers minimum -> 6 brokers with headroom.

---

## 13. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale |
|----------|-------------------|--------|-----------|
| Metadata management | ZooKeeper, etcd, KRaft | KRaft | No external dependency; scales to millions of partitions; simpler operations |
| Storage engine | B-tree DB, LSM-tree, Append-only log | Append-only log + page cache | Maximizes sequential I/O; zero-copy reads; 10x throughput over alternatives |
| Replication model | Synchronous, Async, ISR, Quorum | ISR | Balances durability and availability; dynamic membership avoids blocking on slow replicas |
| Consumer coordination | External coordinator, Embedded in broker | Embedded (group coordinator) | No external dependency; offset storage in Kafka itself |
| Rebalance protocol | Eager, Cooperative, Static | Cooperative + Static group membership | Minimizes stop-the-world; static membership prevents restart-triggered rebalances |
| Serialization | JSON, Avro, Protobuf | Avro + Schema Registry | Compact binary format; schema evolution with compatibility checks |
| Compression | none, gzip, snappy, lz4, zstd | lz4 (default) / zstd (high compression) | lz4: best speed-to-ratio for real-time; zstd: best ratio for archival |
| Disk config | RAID-10, RAID-5, JBOD | JBOD | Kafka replication replaces RAID redundancy; JBOD maximizes usable capacity |
| Filesystem | ext4, XFS, btrfs | XFS | Better concurrent I/O, larger file support, superior metadata performance |
| JVM GC | G1GC, ZGC, Shenandoah | G1GC (Java 17) | Well-tested with Kafka; sub-200ms pauses with 6GB heap |

---

## 14. Agentic AI Integration

### AI-Driven Cluster Operations

| Capability | Implementation | Benefit |
|------------|---------------|---------|
| **Auto-partition rebalancing** | AI agent monitors partition skew metrics; triggers `kafka-reassign-partitions` when imbalance > 20% | Eliminates manual rebalancing; reduces hot-spot incidents |
| **Predictive scaling** | ML model forecasts traffic patterns (daily/weekly cycles); pre-scales consumers via HPA before peak | Zero-lag during predictable traffic spikes |
| **Anomaly detection on consumer lag** | Agent monitors lag per consumer group; distinguishes normal batch processing lag from genuine stalls | Reduces false alerts by 80%; faster incident detection |
| **Self-healing broker** | Agent detects under-replicated partitions, diagnoses root cause (disk, network, GC), applies remediation (restart, disk replacement, GC tuning) | MTTR reduction from 30min to 5min |
| **Intelligent topic tuning** | Agent analyzes produce/consume patterns; recommends partition count, retention, compression per topic | Optimal resource utilization without manual tuning |

### AI-Powered Message Routing

```python
class IntelligentRouter:
    """AI agent that routes messages to optimal partitions based on processing requirements."""
    
    def __init__(self, kafka_admin, model):
        self.admin = kafka_admin
        self.model = model  # LLM or ML model for routing decisions
        self.partition_load = {}  # real-time load per partition
    
    def route_message(self, topic, key, value, headers):
        """Route message considering partition load, consumer health, and content."""
        
        # Standard: hash-based routing for key ordering
        if key and not self.is_hot_key(key):
            return murmur2(key) % self.partition_count(topic)
        
        # Hot key detected: AI spreads across underloaded partitions
        if key and self.is_hot_key(key):
            underloaded = self.get_underloaded_partitions(topic, threshold=0.7)
            return random.choice(underloaded)
        
        # No key: route to partition with lowest consumer lag
        return self.get_lowest_lag_partition(topic)
    
    def auto_tune_topic(self, topic):
        """AI agent periodically tunes topic configuration."""
        metrics = self.collect_topic_metrics(topic)
        
        recommendation = self.model.analyze(
            prompt=f"""Analyze Kafka topic metrics and recommend configuration:
            Topic: {topic}
            Throughput: {metrics['bytes_in_per_sec']} bytes/s
            Consumer lag: {metrics['max_lag']} messages
            Partition count: {metrics['partition_count']}
            Avg message size: {metrics['avg_message_size']} bytes
            Consumer count: {metrics['consumer_count']}
            Peak hours: {metrics['peak_hours']}
            
            Recommend: partition_count, retention.ms, compression.type, 
                       max.message.bytes, min.insync.replicas
            """
        )
        
        if recommendation.confidence > 0.9:
            self.apply_config(topic, recommendation.config)
            self.notify_team(topic, recommendation)
```

### Agentic Infrastructure Orchestration

```
Agent Workflow: Kafka Cluster Scaling Decision

  1. Monitor: Agent continuously ingests metrics from Prometheus
     - Broker CPU, memory, disk, network utilization
     - Per-topic throughput trends (7-day window)
     - Consumer lag trends
     
  2. Analyze: LLM-based reasoning
     - "Broker 7 disk at 85%, growing 2%/day. Will hit 95% in 5 days."
     - "Topic 'events' throughput grew 40% this month. Current 48 partitions 
        insufficient in 2 weeks."
     - "Consumer group 'analytics' lag increasing despite scaling. 
        Likely partition bottleneck, not consumer throughput."
  
  3. Plan: Generate remediation plan
     - Short-term: Reduce retention on non-critical topics (free 20% disk)
     - Medium-term: Add 2 brokers, reassign partitions from broker 7
     - Long-term: Increase partition count for 'events' topic to 96
  
  4. Execute: With human approval (or auto for low-risk)
     - Auto: retention policy changes, consumer scaling
     - Approval required: broker addition, partition reassignment, partition count change
  
  5. Verify: Post-change monitoring
     - Agent verifies metrics improved within expected timeframe
     - Rolls back if degradation detected
```

---

## 15. Complete Interviewer Q&A Bank

### Architecture & Design

**Q1: Why is Kafka so fast compared to traditional message brokers?**
A: Four key factors: (1) Sequential I/O: append-only writes exploit disk throughput (3 GB/s NVMe sequential vs 400 MB/s random). (2) Page cache: OS manages caching transparently; hot data served from DRAM without JVM heap allocation. (3) Zero-copy: `sendfile()` transfers data directly from page cache to NIC, bypassing user-space. (4) Batching: producer batches amortize network overhead; compression applied per batch.

**Q2: How does Kafka guarantee ordering?**
A: Ordering is guaranteed per partition. All messages with the same key go to the same partition (via `murmur2(key) % numPartitions`). Within a partition, offsets are strictly monotonic. Cross-partition ordering is NOT guaranteed. If you need global ordering, use a single partition (sacrificing parallelism).

**Q3: What is the difference between a queue and a log?**
A: A queue (RabbitMQ, SQS) deletes messages after delivery. A log (Kafka) retains messages for a configurable period regardless of consumption. This enables: (1) multiple consumer groups reading independently, (2) replay/reprocessing from any offset, (3) event sourcing patterns. The trade-off is higher storage cost.

**Q4: How does Kafka handle back-pressure?**
A: Kafka doesn't have explicit back-pressure. Instead: (1) Producers: `max.block.ms` controls how long `send()` blocks when buffer is full. (2) Consumers: `fetch.max.bytes` and `max.poll.records` control consumption rate. (3) Broker: client quotas throttle individual clients. (4) The log abstraction naturally decouples producer and consumer rates.

**Q5: Why does Kafka need a separate controller?**
A: The controller manages cluster-level state: partition leadership, ISR membership, topic configuration. Without a central coordinator, each broker would need to independently detect failures and elect leaders, leading to split-brain scenarios. KRaft uses Raft consensus for controller election, ensuring exactly one active controller.

### Operations & Troubleshooting

**Q6: How do you diagnose a slow consumer?**
A: (1) Check consumer lag (`kafka-consumer-groups --describe`). (2) Check `max.poll.interval.ms` violations in consumer logs. (3) Profile consumer processing time per message. (4) Check if consumer is doing synchronous I/O (DB writes, HTTP calls). (5) Check GC pauses in consumer JVM. (6) Check partition count vs consumer count (under-parallelized?). (7) Check if consumer is reading old data (page cache miss).

**Q7: What happens when you run out of disk on a broker?**
A: (1) Broker logs errors for every partition write. (2) Partitions led by this broker stop accepting writes. (3) ISR shrinks for affected partitions. (4) Remediation: reduce retention, delete unused topics, add disks, move partitions to other brokers. Prevention: alert at 80% disk usage; auto-scaling in cloud.

**Q8: How do you perform a zero-downtime Kafka upgrade?**
A: Rolling restart strategy: (1) Set `inter.broker.protocol.version` to current version. (2) Upgrade brokers one at a time (controller last). Each restart: partitions fail over to other brokers, then back. (3) After all brokers upgraded, bump `inter.broker.protocol.version` to new version. (4) Another rolling restart to activate new protocol.

**Q9: How do you debug message ordering issues?**
A: Common causes: (1) Messages with the same logical key use different Kafka keys (partitioned differently). (2) `max.in.flight.requests.per.connection > 1` without idempotence (retry reorders batches). (3) Multiple producers without coordination. (4) Consumer processing in thread pool without partition affinity. Fix: enable idempotence (safe reordering), verify key consistency.

**Q10: A consumer group shows 0 lag but messages aren't being processed. Why?**
A: Possible causes: (1) Consumer is committing offsets but not processing (bug in commit logic). (2) Auto-commit is on and consumer crashes between commit and processing. (3) Consumer is assigned 0 partitions (more consumers than partitions). (4) Topic has no new messages. Debug: check `kafka-consumer-groups --describe` for assignment column.

### Advanced Internals

**Q11: Explain the idempotent producer mechanism.**
A: Each producer gets a unique `ProducerId` (PID) and maintains a `sequence number` per partition. The broker tracks the last 5 sequence numbers per (PID, partition). If a duplicate batch arrives (same PID + sequence), the broker returns success without re-appending. This handles exactly the case where the producer retries a batch that was actually written but whose ack was lost. Requires `enable.idempotence=true` and `max.in.flight.requests.per.connection <= 5`.

**Q12: How does log compaction work?**
A: For topics with `cleanup.policy=compact`, the log cleaner maintains only the latest value per key. Background compaction thread: (1) Reads "dirty" (uncompacted) segments. (2) Builds an offset map of key -> latest offset. (3) Copies records, skipping older duplicates. (4) Swaps old segments with compacted ones. Tombstone records (null value) are retained for `delete.retention.ms` then removed. Used for `__consumer_offsets`, changelog topics.

**Q13: How does Kafka Transactions work across multiple partitions?**
A: (1) Producer registers with a Transaction Coordinator (broker). (2) `beginTransaction()`: marks producer as in-transaction. (3) `send()` to multiple partitions: coordinator tracks which partitions are involved. (4) `commitTransaction()`: coordinator writes a PREPARE message, then COMMIT markers to all involved partitions. (5) `read_committed` consumers skip records in uncommitted transactions using the `.txnindex`. (6) If producer crashes, coordinator aborts after `transaction.timeout.ms`.

**Q14: What is the difference between `log.segment.bytes` and `log.retention.bytes`?**
A: `log.segment.bytes` (1GB default) controls when a new segment file is created (active segment rolls). `log.retention.bytes` (-1 default, unlimited) is the maximum total size of all segments in a partition before old segments are deleted. The retention policy only deletes INACTIVE (rolled) segments; the active segment is never deleted.

**Q15: How do follower replicas fetch data from the leader?**
A: Followers act as consumers of the leader. Each follower has a ReplicaFetcherThread that sends FetchRequests to the leader with its current LEO. The leader responds with records from that offset. The follower appends to its local log and advances its LEO. The leader updates its view of the follower's LEO, which affects HW calculation. Fetch requests use `replica.fetch.max.bytes` and `replica.fetch.wait.max.ms` (long-poll).

**Q16: What causes "NotLeaderForPartitionException"?**
A: The client's metadata cache is stale; it's sending a request to a broker that is no longer the leader for that partition. Causes: (1) Leader just changed (broker restart, failure). (2) Client's `metadata.max.age.ms` (5min default) hasn't triggered a refresh. The client automatically refreshes metadata and retries. Frequent occurrences indicate cluster instability.

**Q17: How does Kafka achieve exactly-once with `sendOffsetsToTransaction`?**
A: This is the consume-transform-produce pattern. The producer atomically commits: (1) all records produced in the transaction, and (2) the consumer group's offsets for the records consumed. Both are committed or aborted together. If the consumer restarts, it resumes from the last committed offset, and the corresponding produced records are either all visible or all invisible. No duplicates, no gaps.

---

## 16. References

| Resource | URL / Reference |
|----------|----------------|
| Apache Kafka Documentation | https://kafka.apache.org/documentation/ |
| KIP-500: KRaft mode | https://cwiki.apache.org/confluence/display/KAFKA/KIP-500 |
| KIP-848: Next-gen consumer rebalance | https://cwiki.apache.org/confluence/display/KAFKA/KIP-848 |
| Kafka: The Definitive Guide (2nd Ed.) | O'Reilly, Shapira et al. |
| Designing Data-Intensive Applications | Martin Kleppmann, Ch. 11 (Stream Processing) |
| LinkedIn Engineering Blog: Kafka at scale | https://engineering.linkedin.com/kafka |
| Confluent Developer Resources | https://developer.confluent.io/ |
| Jay Kreps: "The Log" | https://engineering.linkedin.com/distributed-systems/log-what-every-software-engineer-should-know-about-real-time-datas-unifying |
| Strimzi: Kafka on Kubernetes | https://strimzi.io/documentation/ |
| Kafka Improvement Proposals (KIPs) | https://cwiki.apache.org/confluence/display/KAFKA/Kafka+Improvement+Proposals |
