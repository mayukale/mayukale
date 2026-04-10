# System Design: Distributed Message Queue

---

## 1. Requirement Clarifications

### Functional Requirements
- Producers publish messages to named topics
- Consumers subscribe to topics and read messages
- Messages are durably stored on disk and replicated across brokers
- Consumer groups: multiple consumers within a group share partitions; each message delivered to exactly one consumer in the group
- Offset management: consumers commit offsets; can replay from any offset
- Retention: messages retained by time (e.g., 7 days) or size (e.g., 100 GB per topic) — configurable per topic
- Log compaction: retain only the latest value per key (for event-sourcing / changelog topics)
- Producer delivery guarantees: at-most-once, at-least-once, exactly-once (idempotent producer + transactions)
- Admin API: create/delete topics, describe consumer group lag, alter configurations
- Schema registry integration: producers/consumers use Avro/Protobuf schemas with compatibility checks

### Non-Functional Requirements
- Throughput: sustain 10 GB/sec write throughput cluster-wide
- End-to-end latency: P99 < 10 ms (producer to consumer) for non-batch mode
- Durability: no message loss once broker acknowledges (`acks=all`)
- Availability: 99.99% uptime; tolerate loss of any single broker without message loss
- Horizontal scalability: scale by adding brokers and partitions
- Ordered delivery: messages within a partition are strictly ordered by offset
- Replay capability: consumers can re-read historical messages within retention window
- Fan-out: a single message can be consumed by unlimited independent consumer groups
- Partition rebalancing: when consumer group membership changes, partitions are reassigned automatically

### Out of Scope
- Exactly-once delivery across external systems (e.g., Kafka → database without transactional outbox)
- Built-in dead letter queue (DLQ) UI — DLQ is a regular topic, UI is external
- Message priority queues within a partition (Kafka-model: FIFO per partition)
- Real-time stream processing (Kafka Streams / Flink — separate system)
- Message filtering at broker level (filtering happens in consumer application)

---

## 2. Users & Scale

### User Types
| Actor | Behavior |
|---|---|
| Producer service | Publishes events (user clicks, orders, sensor data) to topics |
| Consumer service | Reads and processes messages from topics |
| Stream processor | Reads, transforms, and writes back to topics (e.g., Kafka Streams) |
| Admin operator | Creates topics, monitors consumer lag, adjusts retention |
| Schema registry | Validates message schemas on produce/consume |

### Traffic Estimates (calculations shown)

**Assumptions:**
- 500 M events/day across all topics (user-facing events, internal microservice events)
- Average message size: 1 KB
- Peak-to-average ratio: 5× (handles flash sales, viral events)
- Replication factor: 3 (each message written to 3 brokers)

Average events/sec = 500 M / 86,400 ≈ 5,787 events/sec  
Peak events/sec = 5,787 × 5 = **28,935 events/sec ≈ 30,000 events/sec**  
Peak write throughput (before replication) = 30,000 × 1 KB = **30 MB/sec**  
Peak write throughput with replication (3×) = 30 × 3 = **90 MB/sec total disk writes across cluster**  
With 10 partitions per topic and 50 topics = 500 partitions  
Partitions per broker (20 brokers): 500 / 20 = 25 partitions/broker

**Broker sizing:** Each broker needs to handle ~4.5 MB/sec writes (90 MB / 20 brokers) + replication traffic. Modern NVMe disks sustain 1–3 GB/sec sequential writes — well within limits. Bottleneck is typically network (10 GbE = 1,250 MB/sec per broker NIC).

### Latency Requirements
| Operation | P50 | P99 | P999 |
|---|---|---|---|
| Producer send (acks=1) | 1 ms | 5 ms | 15 ms |
| Producer send (acks=all, sync) | 2 ms | 10 ms | 30 ms |
| Consumer poll (records available) | 0.5 ms | 5 ms | 10 ms |
| End-to-end (produce → consume) | 3 ms | 10 ms | 40 ms |
| Consumer group rebalance | — | < 30 s | — |

### Storage Estimates

**Assumptions:**
- Retention period: 7 days
- 500 M events/day × 1 KB = 500 GB/day raw
- With 3× replication: 1.5 TB/day across cluster
- 7 days retention: 7 × 1.5 TB = **10.5 TB total cluster storage**
- 20 brokers → 525 GB per broker (NVMe SSDs recommended; HDDs work for sequential writes)
- Add 30% headroom: each broker needs ~700 GB–1 TB storage

| Component | Daily Volume | 7-Day Retention | With Replication |
|---|---|---|---|
| User events topic (20 partitions) | 100 GB | 700 GB | 2.1 TB |
| Order events topic (10 partitions) | 50 GB | 350 GB | 1.05 TB |
| Audit log topic (5 partitions) | 10 GB | 70 GB | 210 GB |
| All other topics | 340 GB | 2.38 TB | 7.14 TB |
| **Total** | **500 GB** | **3.5 TB raw** | **10.5 TB cluster** |

### Bandwidth Estimates

Peak write (producers → brokers): 30 MB/sec inbound  
Replication traffic (leader → follower): 30 MB/sec × 2 replicas = 60 MB/sec per partition leader  
Consumer reads: assume 3 consumer groups × 30 MB/sec = 90 MB/sec outbound  
Total per-broker bandwidth ≈ (90 + 60 + 90) / 20 ≈ **12 MB/sec per broker** — well within 10 GbE (1,250 MB/sec)  
Peak scenario (5× traffic, full consumer catch-up): up to 300 MB/sec per broker — provision 25 Gbps NICs for top-tier brokers

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                        Producer Services                          │
│  (Order Service, Click Tracker, Sensor Gateway, Audit Logger)    │
│                                                                   │
│  KafkaProducer client:                                            │
│  - Partitioner: hash(key) % num_partitions  (or round-robin)    │
│  - Record accumulator (batch.size, linger.ms)                    │
│  - Idempotent producer (PID + sequence numbers)                  │
└──────────────┬───────────────────────────────────────────────────┘
               │  TCP (binary protocol)
               ▼
┌──────────────────────────────────────────────────────────────────┐
│                    Broker Cluster (N brokers)                     │
│                                                                   │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐  │
│  │   Broker 1      │  │   Broker 2      │  │   Broker 3      │  │
│  │                 │  │                 │  │                 │  │
│  │ Partition 0 [L] │  │ Partition 0 [F] │  │ Partition 0 [F] │  │
│  │ Partition 3 [F] │  │ Partition 1 [L] │  │ Partition 2 [L] │  │
│  │ Partition 6 [L] │  │ Partition 3 [F] │  │ Partition 3 [L] │  │
│  │                 │  │                 │  │                 │  │
│  │ ┌─────────────┐ │  │ ┌─────────────┐ │  │ ┌─────────────┐ │  │
│  │ │ Log Segment │ │  │ │ Log Segment │ │  │ │ Log Segment │ │  │
│  │ │ .log  .idx  │ │  │ │ .log  .idx  │ │  │ │ .log  .idx  │ │  │
│  │ │ .timeindex  │ │  │ │ .timeindex  │ │  │ │ .timeindex  │ │  │
│  │ └─────────────┘ │  │ └─────────────┘ │  │ └─────────────┘ │  │
│  └─────────────────┘  └─────────────────┘  └─────────────────┘  │
│                                                                   │
│  [L] = Leader partition  [F] = Follower (replica) partition      │
└──────────────┬──────────────────────┬────────────────────────────┘
               │                      │
               │ Leader election,     │ Metadata cache
               │ ISR tracking         │ (broker endpoints,
               ▼                      │  partition leaders)
┌─────────────────────────┐          │
│  Coordination Layer      │◄─────────┘
│  (KRaft / ZooKeeper)     │
│  - Controller election   │
│  - Topic metadata        │
│  - ISR management        │
│  - Broker registration   │
└─────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────────────────────────────┐
│                       Consumer Services                           │
│                                                                   │
│  Consumer Group A (order-processor):                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │
│  │ Consumer A1  │  │ Consumer A2  │  │ Consumer A3  │           │
│  │ → Part 0,1   │  │ → Part 2,3   │  │ → Part 4,5   │           │
│  └──────────────┘  └──────────────┘  └──────────────┘           │
│                                                                   │
│  Consumer Group B (analytics-aggregator): [independent offsets]  │
│  ┌───────────────────────────────────────┐                       │
│  │ Consumer B1  → all 6 partitions       │                       │
│  └───────────────────────────────────────┘                       │
│                                                                   │
│  Offsets stored in: __consumer_offsets topic (internal)          │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                  Schema Registry (Confluent)                       │
│  - Stores Avro/Protobuf/JSON Schema per subject (topic+type)     │
│  - Compatibility modes: BACKWARD, FORWARD, FULL, NONE            │
│  - Producer fetches schema ID → embeds 5-byte header in message  │
│  - Consumer fetches schema by ID → deserializes message          │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                   Monitoring / Ops                                 │
│  - Prometheus JMX exporter → Grafana dashboards                  │
│  - Burrow (consumer lag monitoring)                              │
│  - Kafka Manager / AKHQ / Conduktor (topic/group management)     │
└──────────────────────────────────────────────────────────────────┘
```

**Component Roles:**
- **Broker:** Receives, stores, and serves messages. Each broker hosts a set of partition leaders and followers. The partition leader handles all reads and writes for that partition; followers replicate.
- **Partition:** The unit of parallelism and ordering. Messages within a partition are totally ordered by offset. Partitions are distributed across brokers for load balancing.
- **KRaft (formerly ZooKeeper):** Stores cluster metadata: broker list, topic configurations, partition-to-broker assignment, ISR (in-sync replicas) set. KRaft (Kafka 2.8+) eliminates ZooKeeper dependency, using Raft consensus internally.
- **Controller:** One broker is elected as controller (via KRaft). Manages partition leader elections, broker registrations, and ISR updates.
- **Consumer Group Coordinator:** A broker designated as coordinator for a consumer group (by hashing `group.id`). Manages group membership, triggers rebalances, stores committed offsets.
- **Schema Registry:** Centralized schema storage and validation. Producers/consumers fetch schema IDs on first use and cache locally.

**Primary Use-Case Data Flow (Produce → Consume):**
1. Producer serializes record, applies partitioner: `partition = hash(record.key) % num_partitions`
2. Record placed in batch accumulator (up to `batch.size=16KB` or `linger.ms=5ms`)
3. Sender thread flushes batch to partition leader broker via `ProduceRequest`
4. Leader writes to leader log (sequential append to segment `.log` file)
5. Leader waits for ISR followers to acknowledge (if `acks=all`)
6. Leader returns `ProduceResponse` with assigned offset
7. ISR followers fetch from leader and write to their local log
8. Consumer polls partition leader (or follower if `fetch.min.bytes` and `replica.selector.class` configured): `FetchRequest(partition, offset, max_bytes)`
9. Leader returns `FetchResponse` with records batch starting at requested offset
10. Consumer processes records, commits offset: `OffsetCommitRequest` to coordinator

---

## 4. Data Model

### Entities & Schema

```
-- Physical on-disk format --

Partition Log Directory: /data/kafka/topics/orders-0/
  ├── 00000000000000000000.log      -- segment file (base_offset=0)
  ├── 00000000000000000000.index    -- sparse offset index (offset→file position)
  ├── 00000000000000000000.timeindex -- time index (timestamp→offset)
  ├── 00000000000000012345.log      -- next segment (base_offset=12345)
  ├── 00000000000000012345.index
  └── leader-epoch-checkpoint       -- leader epoch history

-- Message (Record) Format (Kafka RecordBatch v2) --

RecordBatch:
  base_offset          : int64    -- first offset in batch
  batch_length         : int32    -- bytes from attributes to end
  partition_leader_epoch : int32  -- monotonic leader epoch
  magic                : int8     -- version (currently 2)
  crc                  : int32    -- CRC32C of data after this field
  attributes           : int16    -- compression (NONE/GZIP/SNAPPY/LZ4/ZSTD), timestamp type, transactional, control
  last_offset_delta    : int32    -- (last offset in batch) - base_offset
  first_timestamp      : int64    -- ms since epoch (first record)
  max_timestamp        : int64    -- ms since epoch (last record)
  producer_id          : int64    -- idempotent producer ID (PID)
  producer_epoch       : int16    -- producer epoch
  base_sequence        : int32    -- first sequence number in batch
  records_count        : int32
  records              : Record[]

Record:
  length               : varint
  attributes           : int8     -- reserved
  timestamp_delta      : varint   -- relative to batch first_timestamp
  offset_delta         : varint   -- relative to base_offset
  key_length           : varint   -- -1 if null
  key                  : bytes
  value_length         : varint
  value                : bytes
  headers              : Header[] -- key-value pairs for metadata (correlation-id, trace-id)

-- Offset Index (.index) --
Entry: (relative_offset: int32, file_position: int32) -- every ~4KB of log data
Purpose: binary search to find file position for a given offset

-- Consumer Offset Storage (internal topic __consumer_offsets) --
Key: (group_id, topic, partition)    -- message key
Value: OffsetAndMetadata {
  offset    : int64    -- next offset to fetch
  metadata  : string   -- optional consumer metadata
  timestamp : int64    -- when committed
  leader_epoch : int32 -- for fencing stale commits
}

-- Topic Configuration (stored in KRaft metadata log) --
TopicRecord {
  topic_id   : UUID
  name       : string
  partitions : PartitionRecord[] {
    partition_id     : int32
    replicas         : int32[]   -- broker IDs
    isr              : int32[]   -- in-sync replica broker IDs
    leader           : int32     -- broker ID of current leader
    leader_epoch     : int32
  }
}
```

### Database Choice

| Option | Pros | Cons | Fit |
|---|---|---|---|
| **Sequential log on local disk (Kafka model)** | Sequential I/O saturates disk bandwidth; OS page cache acts as read cache; simple append-only structure | Relies on local disk; node failure requires replica promotion | **Selected** |
| RDBMS (PostgreSQL) | ACID, complex queries | Random I/O for writes, not designed for high-throughput append | Not suitable |
| Distributed KV store (RocksDB, like Pulsar) | Random reads by key, compaction | Pulsar uses ledger abstraction; adds BookKeeper dependency | Valid alternative |
| Apache BookKeeper (Pulsar model) | Decoupled storage; brokers are stateless; easy scaling | More complex architecture; higher op overhead | Valid for elastic scaling |
| Cloud object storage (S3 tiered storage) | Infinite retention at low cost | High latency for hot reads (use as tiered cold tier) | Tiered storage add-on |

**Selected: Sequential log files on local NVMe SSDs (Kafka's native storage)**  
Justification: Sequential disk writes achieve 1–3 GB/sec on NVMe vs ~100–200 MB/sec for random writes. The OS page cache (filesystem buffer cache) transparently caches recently written and recently read segments, making consumer reads that are close to the write head nearly memory-speed. The immutable, append-only structure simplifies replication (followers just need to fetch bytes from a known offset) and compaction (delete old segments atomically). The tradeoff is that brokers are stateful — but this is managed by having replicas on separate brokers.

---

## 5. API Design

**Protocol:** Kafka Binary Protocol over TCP (persistent connections). Not REST by default; REST Proxy (Confluent REST Proxy) provides HTTP interface for non-JVM clients.

**Authentication:** SASL/SCRAM-SHA-512 or mTLS for broker-to-client. ACLs: `AclBindings` allow/deny per principal, per resource (topic/group/cluster), per operation (READ/WRITE/CREATE/DELETE/ALTER/DESCRIBE).

**Rate Limiting:** Kafka client quotas: `producer_byte_rate` per client-id (e.g., 10 MB/sec), `consumer_byte_rate` per group (e.g., 50 MB/sec), `request_percentage` CPU quota per client. Enforced by broker with throttle delay responses.

```
-- Producer API --

ProduceRequest (API Key 0):
  transactional_id  : nullable string  -- for transactional producers
  acks              : int16  -- 0=no ack, 1=leader only, -1=all ISR (acks=all)
  timeout_ms        : int32
  topic_data:
    topic           : string
    partition_data:
      partition     : int32
      record_set    : RecordBatch  -- compressed batch of records

ProduceResponse:
  topic_responses:
    topic           : string
    partition_responses:
      partition     : int32
      base_offset   : int64   -- offset of first record in batch
      log_append_time_ms : int64
      log_start_offset : int64
      error_code    : int16   -- 0=success, 6=NOT_LEADER, 5=LEADER_NOT_AVAILABLE

-- Consumer API --

FetchRequest (API Key 1):
  max_wait_ms     : int32  -- long-poll timeout
  min_bytes       : int32  -- min data to return (0 = return immediately)
  max_bytes       : int32  -- max total response size
  topics:
    topic         : string
    partitions:
      partition          : int32
      fetch_offset       : int64  -- consumer's current offset
      log_start_offset   : int64  -- for follower fetch validation
      max_partition_bytes : int32

FetchResponse:
  throttle_time_ms : int32
  topics:
    topic         : string
    partitions:
      partition   : int32
      high_watermark : int64  -- latest committed offset
      last_stable_offset : int64  -- for transactional reads
      records     : RecordBatch[]
      error_code  : int16

OffsetCommitRequest (API Key 8):
  group_id        : string
  generation_id   : int32  -- current generation (increments on rebalance)
  member_id       : string  -- assigned by coordinator
  group_instance_id : nullable string  -- for static membership
  topics:
    topic         : string
    partitions:
      partition       : int32
      committed_offset : int64
      committed_metadata : nullable string

-- Admin API --

CreateTopicsRequest (API Key 19):
  topics:
    name              : string
    num_partitions    : int32    -- -1 to use broker default
    replication_factor : int16   -- typically 3
    assignments       : []       -- explicit partition→broker assignments (usually empty)
    configs:
      retention.ms          : string  -- e.g., "604800000" (7 days)
      retention.bytes       : string  -- e.g., "107374182400" (100 GB)
      cleanup.policy        : string  -- "delete" or "compact"
      min.insync.replicas   : string  -- "2" (require 2 ISR for acks=all)
      compression.type      : string  -- "producer" / "gzip" / "lz4" / "zstd"
      segment.bytes         : string  -- "1073741824" (1 GB per segment)

DescribeGroupsRequest (API Key 15):
  groups : string[]

DescribeGroupsResponse:
  groups:
    group_id       : string
    state          : string  -- "Empty", "PreparingRebalance", "CompletingRebalance", "Stable", "Dead"
    protocol_type  : string  -- "consumer"
    protocol       : string  -- assignment strategy: "range", "roundrobin", "sticky"
    members:
      member_id    : string
      client_id    : string
      client_host  : string
      member_metadata : bytes  -- subscribed topics
      member_assignment : bytes  -- assigned partitions

-- REST Proxy (HTTP) --

POST /topics/{topic_name}
  Headers: Content-Type: application/vnd.kafka.avro.v2+json
           X-Auth-Token: <service-token>
  Body: {
    "value_schema": "{\"type\":\"record\",\"name\":\"Order\",...}",
    "records": [
      {"key": "order_12345", "value": {"order_id": 12345, "user_id": 67890, "amount": 99.99}}
    ]
  }
  Response: 200 {"offsets": [{"partition": 3, "offset": 10021}]}
  Rate limit: 1000 req/min per service token

GET /consumers/{group_name}/instances/{instance}/records
  Headers: Accept: application/vnd.kafka.json.v2+json
  Query: ?timeout=3000&max_bytes=1000000
  Response: 200 [{"topic": "orders", "key": "order_12345", "value": {...}, "partition": 3, "offset": 10021}]
```

---

## 6. Deep Dive: Core Components

### 6.1 Partition & Replication: In-Sync Replicas (ISR)

**Problem it solves:** Durability requires that writes survive broker failures. Replication copies each partition's data to multiple brokers. The ISR mechanism tracks which replicas are "caught up" to the leader and eligible to take over as new leader without data loss.

**Approaches Comparison:**

| Approach | Mechanism | Pros | Cons |
|---|---|---|---|
| **ISR with `acks=all`** | Leader waits for all ISR members to ack | No data loss if any ISR member survives | If ISR shrinks to 1 (leader only), durability degrades |
| Majority quorum (Raft style) | Write acknowledged when majority writes | ISR always has majority; mathematically safe | Requires 2f+1 nodes for f failures; less efficient (5 nodes for 2 failures) |
| Chain replication | Primary → secondary → tertiary; ack from tail | Strong consistency, simple recovery | Higher write latency (must traverse full chain) |
| Semi-sync (MySQL-style) | Ack from at least 1 replica | Simple | One replica behind can cause data loss |

**Selected: ISR with `min.insync.replicas=2` and `acks=all`**

Reasoning: The ISR model is space-efficient — with 3 replicas, you can sustain any 1 failure while still accepting writes. Majority quorum would require 5 brokers to survive 2 failures. The `min.insync.replicas=2` config ensures that if the ISR shrinks to 1 (only the leader), producers get a `NotEnoughReplicasException` rather than silently losing durability. This forces a trade-off: favor consistency (reject write) over availability (accept write with degraded durability).

**ISR Maintenance Algorithm:**
```
-- Each follower fetches from leader in a tight loop --
-- Leader tracks last fetch time and offset for each follower --

# Leader-side ISR management:
function leader_check_isr():
    for follower in all_replicas:
        lag = leader_log_end_offset - follower.last_fetched_offset
        time_since_fetch = now() - follower.last_fetch_time
        
        if time_since_fetch > replica_lag_time_max_ms (default 30s):
            # Follower is not fetching → remove from ISR
            remove_from_isr(follower)
            notify_controller(isr_shrink_event)
        
        if lag > replica_lag_max_messages:
            # Follower is too far behind (old config, deprecated in Kafka 0.9)
            # Now only time-based ISR is used
            pass

function follower_can_rejoin_isr(follower):
    # Follower can re-join ISR only when:
    # 1. It has caught up to leader's log end offset (fully caught up)
    # 2. It has been in-sync for at least replica_fetch_backoff_ms
    if follower.last_fetched_offset >= leader.high_watermark:
        add_to_isr(follower)
        notify_controller(isr_expand_event)

# High Watermark (HW): 
# The maximum offset that all ISR members have fetched and acknowledged.
# Consumers can only read up to the High Watermark (ensures consistent reads
# — a consumer cannot read a message that a new leader might not have).

# Log End Offset (LEO): the next offset to be written (latest offset + 1)
# HW <= LEO always
# After all ISR members have LEO >= X, HW advances to X
```

**Leader Election on Broker Failure:**
```
function handle_broker_failure(failed_broker_id):
    controller = get_current_controller()
    
    for each partition where failed_broker is leader:
        isr = get_current_isr(partition)
        isr.remove(failed_broker_id)
        
        if len(isr) == 0:
            if unclean_leader_election_enable:
                # Last resort: elect any replica, risking data loss
                new_leader = choose_any_replica(partition)
            else:
                # Partition is offline until ISR member comes back
                raise PartitionOfflineException
        else:
            # Prefer replica with highest LEO (most up-to-date)
            new_leader = isr[0]  # typically first in preferred replica list
        
        # Increment leader epoch (used for fencing stale produce requests)
        new_epoch = current_leader_epoch + 1
        update_metadata(partition, new_leader, new_epoch, isr)
        broadcast_metadata_to_all_brokers()
```

**Interviewer Q&As:**

Q: What is the High Watermark and why can't consumers read beyond it?  
A: The High Watermark (HW) is the highest offset that all ISR replicas have confirmed writing. If a consumer could read beyond the HW and the leader crashed before those messages were replicated, the new leader (which doesn't have those messages) would serve a "gap" — the same offset would return different data or nothing. By restricting reads to HW, Kafka guarantees that any message a consumer reads has been durably replicated to all ISR members and will be served consistently by any future leader.

Q: What happens when ISR shrinks to just the leader — is this safe?  
A: With `acks=all` and `min.insync.replicas=2`, if ISR shrinks to size 1 (only leader), producers receive `NotEnoughReplicasException` and writes fail. This is the correct behavior for durability-critical systems. The operator must alert on ISR shrink events (`kafka.server:type=ReplicaManager,name=IsrShrinksPerSec`) and investigate the lagging replica. If `min.insync.replicas=1`, the leader accepts writes even alone, but the partition has no durability protection — acceptable for low-criticality topics (e.g., metrics) where availability > durability.

Q: What is `unclean.leader.election.enable` and when would you turn it on?  
A: With `unclean.leader.election.enable=false` (default for durability), Kafka will wait indefinitely for an ISR member to become leader. If all ISR members are dead, the partition is offline (unavailable for reads and writes). With `unclean.leader.election.enable=true`, Kafka elects any available replica as leader, even if it's behind the old leader — at the cost of message loss and potential reordering. Turn it on for topics where availability is strictly more important than correctness, such as clickstream data or application logs.

Q: How does leader epoch help prevent data loss during leader failover?  
A: Before Kafka 0.11, followers determined their truncation point after a leader change by comparing offsets. This could cause divergence: a follower with higher LEO but belonging to a partitioned-off segment could become leader and be ahead of the new majority. Leader epoch (an integer incremented every time a new leader is elected) lets followers know whether the data they have came from the current leader's tenure. On follower restart, it fetches the leader's epoch and offset history, truncates its log to the point where epochs diverge, and then re-fetches from the current leader's data. This eliminates data divergence without sacrificing throughput.

Q: How does Kafka handle a network partition where the controller and some brokers are separated?  
A: With KRaft, the controller is a Raft quorum (typically 3–5 broker-controllers). A network partition affecting the minority of the quorum does not affect leadership — the majority continues. Brokers that can't reach the active controller will eventually fail to renew their registration and stop accepting producer requests after `connections.max.idle.ms`. New leader elections for affected partitions happen only once the controller quorum has quorum. This is a deliberate CP trade-off for metadata operations.

---

### 6.2 Delivery Semantics: Exactly-Once

**Problem it solves:** Network failures mean a producer might retransmit a message that was already committed by the broker (duplicate) or a consumer might reprocess a message it already committed (duplicate consumption). Exactly-once semantics (EOS) guarantees that each record is produced and consumed exactly once, even in the presence of failures.

**Approaches Comparison:**

| Semantic | Producer Config | Consumer Behavior | Risk |
|---|---|---|---|
| At-most-once | `acks=0`, no retries | Auto-commit before processing | Message loss on broker failure or consumer crash |
| At-least-once | `acks=all`, `retries=MAX` | Commit after processing | Duplicate messages on retry; consumer must be idempotent |
| **Exactly-once** | Idempotent producer + transactions | Transactional read-process-write with `isolation.level=read_committed` | Higher latency; requires transactional API |

**Selected: At-least-once as default; exactly-once for financial/order processing streams.**

Reasoning: Exactly-once adds ~5–10 ms latency per transaction (2-phase commit between producer and broker), which is acceptable for order processing but overkill for clickstream. Most systems achieve effective exactly-once at the application layer using at-least-once delivery + idempotent consumers (check-before-process with a deduplication key in the database). True EOS is required when the consumer side-effect is itself non-idempotent and cannot be made idempotent (rare).

**Idempotent Producer Implementation:**
```
# Producer initialization:
# bootstrap.servers → connect → receive PID (Producer ID) from broker
# PID is unique per producer session; monotonically increasing epoch per PID

class IdempotentProducer:
    def __init__(self):
        self.pid = request_pid_from_broker()  # ProducerID, unique 64-bit int
        self.epoch = 0                         # increments on producer restart
        self.sequence_numbers = {}             # per-partition sequence number

    def send(self, topic, partition, record):
        seq = self.sequence_numbers.get((topic, partition), 0)
        record.producer_id = self.pid
        record.producer_epoch = self.epoch
        record.base_sequence = seq
        
        # Broker-side deduplication:
        # Broker tracks (PID, epoch, partition) → last 5 sequence numbers
        # If incoming seq <= last_seq: duplicate → drop silently
        # If incoming seq == last_seq + 1: in-order → accept
        # If incoming seq > last_seq + 1: out-of-order → error
        
        response = send_to_broker(record)
        if response.error == SUCCESS:
            self.sequence_numbers[(topic, partition)] = seq + batch_size
        elif response.error == DUPLICATE_SEQUENCE:
            pass  # already committed, treat as success
        else:
            raise response.error

# Transactional Producer (exactly-once read-process-write):
class TransactionalProducer(IdempotentProducer):
    def __init__(self, transactional_id):
        super().__init__()
        self.transactional_id = transactional_id  # stable across restarts
        # broker uses transactional_id to fence zombie producers (old epochs)
        self.transaction_coordinator = find_coordinator(transactional_id)
        self.epoch = initialize_transactions()  # bumps epoch, fencing old producers

    def begin_transaction(self):
        self.txn_state = ACTIVE

    def send_in_txn(self, topic, partition, record):
        # Register this partition with transaction coordinator (AddPartitionsToTxn)
        register_partition_in_txn(topic, partition)
        super().send(topic, partition, record)

    def send_offsets_to_transaction(self, offsets, consumer_group):
        # Atomically commit consumer offsets as part of the transaction
        # TxnOffsetCommitRequest → coordinator persists in __consumer_offsets
        # as a pending transaction commit
        txn_coordinator.commit_offsets(offsets, consumer_group, self.transactional_id)

    def commit_transaction(self):
        # 2-phase commit:
        # Phase 1: EndTxnRequest(COMMIT) → coordinator writes PREPARE_COMMIT to __transaction_state
        # Coordinator sends WriteTxnMarkersRequest to all partition leaders
        # Partition leaders write COMMIT marker record
        # Phase 2: Coordinator writes COMPLETE_COMMIT to __transaction_state
        txn_coordinator.end_transaction(COMMIT)
        self.txn_state = COMMITTED

    def abort_transaction(self):
        txn_coordinator.end_transaction(ABORT)
        # Brokers write ABORT marker; consumers with read_committed skip these records
```

**Consumer isolation:**
```
# read_committed: consumer only receives records from committed transactions
# AND non-transactional records up to the Last Stable Offset (LSO)
# LSO = highest offset where all preceding transactions are committed/aborted
# read_uncommitted (default): consumer receives all records including in-flight transactions

consumer.config("isolation.level", "read_committed")

# LSO is always <= HW. During a long-running transaction, LSO stays behind HW,
# causing consumer lag to appear artificially high. Monitor both HW lag and LSO lag.
```

**Interviewer Q&As:**

Q: What is the difference between idempotent producer and transactional producer?  
A: An idempotent producer deduplicates retries within a single producer session: if a producer retries a batch that was already committed, the broker detects the duplicate sequence number and drops it silently. This ensures exactly-once delivery to a single partition within a session. A transactional producer extends this to atomic writes across multiple partitions and atomically commits consumer offsets — an entire read-process-write cycle either commits atomically or aborts. Idempotency is necessary but not sufficient for exactly-once; transactions complete the guarantee.

Q: What is a "zombie fencing" and how does the transactional_id help?  
A: A zombie producer is a stale producer instance (e.g., a process that was assumed dead by the orchestrator but is still running) trying to commit transactions that have already been taken over by a new instance. The `transactional_id` is a stable, user-provided string tied to a logical producer role (e.g., "order-service-instance-1"). When a new producer initializes with the same `transactional_id`, the broker bumps the epoch. Any ProduceRequest from the old producer with the old epoch is rejected with `ProducerFencedException`. This prevents double-commits.

Q: Exactly-once in Kafka — does it guarantee exactly-once delivery end-to-end, including the consumer's side effects (e.g., a database write)?  
A: No. Kafka EOS guarantees exactly-once within the Kafka system: each message is written to the Kafka log exactly once, and consumer offset commits are atomic with message production (for stream processing pipelines). If the consumer writes to an external database, that write is outside Kafka's transaction scope. To achieve end-to-end EOS with an external DB, use the transactional outbox pattern: write to the DB and produce to Kafka in the same DB transaction (Debezium CDC captures the DB transaction and publishes to Kafka exactly once).

Q: How does `isolation.level=read_committed` affect consumer throughput?  
A: Consumers with `read_committed` can only read up to the Last Stable Offset (LSO), which is the offset below which all transactions have been committed or aborted. If a producer has a long-running transaction (e.g., several seconds), LSO falls behind the HW by the duration of the transaction, causing consumers to appear lagged even though data is available. This is a real throughput concern for mixed transactional/non-transactional workloads. Mitigation: keep transactions short; use `transaction.timeout.ms` (default 60s, should be set to 10–15s); separate transactional and non-transactional topics.

Q: How would you implement exactly-once processing without Kafka transactions?  
A: Use at-least-once delivery + idempotent consumer. The consumer includes a unique message key (or `partition:offset`) in every side-effect write: `INSERT INTO processed_events (partition, offset, data) VALUES (?, ?, ?) ON CONFLICT DO NOTHING`. Before processing, check if the event was already handled. This achieves effectively-exactly-once processing at the application layer. Works for any consumer side effect that can be made idempotent. Kafka transactions add value mainly for Kafka-to-Kafka stream processing pipelines where the "DB check" approach adds a round trip.

---

### 6.3 Log Compaction

**Problem it solves:** For changelog topics (e.g., user profile updates, database CDC), consumers only need the latest value per key — historical intermediate values are irrelevant. Log compaction retains the most recent record for each key and removes older duplicates, bounding topic size by cardinality (number of unique keys) rather than time or volume.

**Approaches Comparison:**

| Approach | Description | Pros | Cons |
|---|---|---|---|
| **Log compaction (cleanup.policy=compact)** | Background thread retains latest value per key, removes older | Bounded by unique key count; consumer can reconstruct current state | Tombstone (null value) required for key deletion; compaction is async (not instant) |
| Time-based retention only | Delete segments older than N days | Simple, predictable | Consumer replaying from beginning may see stale state for keys not updated recently |
| Size-based retention | Delete oldest segments when size exceeds limit | Predictable storage | Same issue as time-based |
| Hybrid (compact + delete) | compact and delete oldest compacted data | Both bounded size and latest state | More complex; tombstones expire after `delete.retention.ms` |

**Selected: `cleanup.policy=compact` for changelog/event-sourcing topics; `cleanup.policy=delete` for event stream topics.**

**Compaction Algorithm (Cleaner Thread):**
```
# Kafka log has two logical sections:
# 1. "Dirty" section: newer, not yet compacted, may have duplicates
# 2. "Clean" section: compacted, guaranteed latest value per key

function compact_partition(partition):
    # Build offset map: key → highest offset seen
    offset_map = {}
    dirty_start = last_compacted_offset
    
    # Phase 1: Build key→latest_offset map from dirty section
    for record in partition.records[dirty_start:]:
        if record.key is not None:
            offset_map[record.key] = record.offset
    
    # Phase 2: Write compacted log (copy, skip obsolete records)
    new_segment = open_temp_segment()
    for record in partition.records[dirty_start:]:
        if record.key is None:
            # Keyless records always kept
            new_segment.write(record)
        elif offset_map[record.key] == record.offset:
            # Latest record for this key → keep
            new_segment.write(record)
        elif record.value is None and record.offset < now() - delete_retention_ms:
            # Tombstone older than delete.retention.ms → purge tombstone
            pass  # skip (deletes the key permanently from log)
        elif record.value is None:
            # Tombstone within retention window → keep (signals deletion to consumers)
            new_segment.write(record)
        else:
            # Older duplicate value → skip
            pass
    
    # Phase 3: Atomically swap segments
    atomic_replace(partition.dirty_segments, new_segment)
    
    # Log cleaner is throttled: max.compaction.lag.ms, min.compaction.lag.ms
    # to avoid impacting producer/consumer throughput
    # io.threads (default 1 per broker) run compaction in background

# Tombstone: a record with key=K and value=null
# Signals to consumers: delete key K from your local state
# Tombstone is retained for delete.retention.ms (default 24h) 
# so slow consumers can process the deletion before the tombstone disappears
```

**Interviewer Q&As:**

Q: Can a consumer replay a compacted topic and reconstruct the full current state?  
A: Yes — this is the primary use case. After compaction, the topic contains exactly one record per key (the latest). A new consumer group starting at `earliest` offset will read every key's latest value in offset order, arriving at a complete snapshot of current state. This is how Kafka Streams builds local state stores (RocksDB) from changelog topics and how database connectors bootstrap new downstream databases from CDC topics.

Q: What is a compaction lag and why does it matter?  
A: Compaction is asynchronous — the dirty segment grows before the cleaner runs. `min.compaction.lag.ms` (default 0) prevents the cleaner from compacting records newer than this threshold (ensuring producers see their recent records during debugging). `max.compaction.lag.ms` sets the maximum time a message can remain un-compacted. If the cleaner falls behind (e.g., high write rate, insufficient I/O), consumers see duplicate keys in the dirty section. Monitor with `kafka.log:type=LogCleaner,name=max-dirty-percent`.

Q3: How do you delete a key from a compacted topic?  
A: Produce a tombstone: a record with the same key and `value=null`. The compaction cleaner retains the tombstone for `delete.retention.ms` (default 86,400,000 ms = 24 hours) to allow consumers to process the deletion. After that period, the tombstone itself is removed from the log. Consumers must handle null values in their state stores (e.g., delete the key from RocksDB).

---

## 7. Scaling

### Horizontal Scaling

**Add Brokers:** New brokers join the cluster and register with KRaft. Existing partitions are NOT automatically rebalanced — use `kafka-reassign-partitions.sh` or Cruise Control to move partition leadership and replicas to new brokers. Moving a partition involves sending all its data to the new broker (full log copy), which is throttled (`replica.fetch.max.bytes`, `throttled.replica.list.rate`) to avoid impacting live traffic.

**Add Partitions:** Increase `num.partitions` for a topic. New partitions are immediately available for new produces. Existing consumers will receive new partition assignments on the next rebalance. Note: adding partitions breaks key-to-partition mapping (hash(key) % num_partitions changes) — do not add partitions to topics where key-based ordering across all data matters (e.g., user profile changelog). Instead, use a fixed large partition count from the start.

**Partition Sizing Rule of Thumb:**
- Partitions = max(target_throughput / throughput_per_partition, target_consumers_in_group)
- throughput_per_partition ≈ 10 MB/sec for writes (limited by single-partition sequential write)
- For 1 GB/sec target: 1000 / 10 = 100 partitions minimum
- Consumer count determines maximum parallelism: partitions ≥ max expected consumer count

### DB Sharding / Partition Distribution
Partitions are the unit of distribution. Use Kafka's rack-awareness (`broker.rack`) to spread replicas across availability zones: rack-aware assignment ensures that the leader and followers are in different AZs, so a single AZ failure loses no data and no more than 1/3 of leaders (for 3 AZs + RF=3).

### Replication
- RF=3: survives 2 broker failures before data loss risk
- `min.insync.replicas=2`: requires 2 replicas to be in-sync for writes to succeed
- `unclean.leader.election.enable=false`: never elect a non-ISR replica as leader

### Consumer Group Scaling
- Increase consumer count up to `num_partitions` (beyond that, extra consumers are idle)
- Use `partition.assignment.strategy=StickyAssignor` to minimize partition movement on rebalance
- Static group membership (`group.instance.id`): consumer keeps its partition assignment across restarts (no rebalance on rolling restart), reducing rebalance storms in large groups

### Tiered Storage (Kafka 3.6+)
- Offload log segments older than `remote.storage.enable=true` + `local.retention.ms` to S3/GCS
- Brokers keep only recent hot data locally; consumers reading cold data fetch from object storage
- Reduces broker storage requirement from 10.5 TB to ~200 GB (last 2 hours of data locally)

### Interviewer Q&As (Scaling)

Q: How does consumer group rebalancing work and why can it cause latency spikes?  
A: When a consumer joins or leaves a group (or a new partition is added), the group coordinator triggers a rebalance. All consumers stop processing, submit their current committed offset, and rejoin the group. The group leader (first consumer to join) computes a partition assignment and distributes it. During this rebalance, all consumers in the group are paused — no messages are processed. With 100+ consumers and eager rebalancing, this pause can last 30 seconds. Mitigation: Incremental Cooperative Rebalancing (ICRA, enabled by `CooperativeStickyAssignor`) — consumers only revoke partitions that need to move, allowing non-affected consumers to keep processing during rebalance.

Q: What is Cruise Control and when do you need it?  
A: Cruise Control (LinkedIn's open-source tool) is an automated Kafka cluster balancer. It monitors broker-level resource utilization (network I/O, disk, CPU) and automatically proposes partition reassignments to balance load. Without it, after adding new brokers, partition leaders remain on old brokers until manually rebalanced. Cruise Control is essential for large clusters (50+ brokers) where manual balancing is impractical. It also handles broker decommissioning, replica imbalance, and goal-based optimization (e.g., "keep replica leaders evenly distributed across AZs").

Q: How do you handle a slow consumer that's falling behind?  
A: Monitor consumer lag with `kafka-consumer-groups.sh --describe` or Burrow. If lag is growing: (1) Scale consumer instances (up to partition count). (2) Optimize consumer processing (async processing, batching). (3) If the consumer is a stream processor, check for data skew — one partition with disproportionate data causes one consumer to fall behind. (4) Increase `fetch.max.bytes` and `max.partition.fetch.bytes` to fetch larger batches per poll. (5) If consumer is irreversibly behind (e.g., data within retention), reset offset to latest and accept data loss, or re-process from an external snapshot.

Q: What is the difference between log retention and log compaction, and can they coexist?  
A: Log retention deletes entire log segments when they age beyond `retention.ms` or exceed `retention.bytes`. Log compaction retains only the latest value per key, regardless of age. They coexist with `cleanup.policy=compact,delete`: the log is compacted (deduplicating keys), but compacted segments older than `retention.ms` are still deleted. This bounds both storage (no indefinite growth) and correctness (latest value per key).

Q: How would you design a Kafka-based event sourcing system for 1 billion events/day?  
A: Partition count: 1B events/day / 86,400 sec = ~11,600 events/sec. With 1KB avg size = 11.6 MB/sec. At 10 MB/sec per partition capacity: need ~2 partitions minimum; use 20–50 for future scale and parallelism. Each aggregate type (Order, User, Product) gets its own topic, with entity ID as partition key (all events for entity X in same partition = ordered log). Consumers reconstruct current state by replaying from offset 0. Use compacted topics for snapshot topics (current state cache) and delete-policy topics for event log. Retain events for as long as the business needs audit trail (potentially forever for compliance — use tiered storage to S3 at low cost).

---

## 8. Reliability & Fault Tolerance

| Failure Scenario | Impact | Detection | Recovery |
|---|---|---|---|
| Broker crash (leader of some partitions) | Partitions go offline until failover | Controller detects missing heartbeat (session.timeout default 18s) | Controller elects ISR member as new leader; ~15–30s |
| Broker crash (follower only) | ISR shrinks; durability reduced | `IsrShrinksPerSec` metric | Replace broker; follower re-syncs automatically |
| Network partition (broker isolated) | Isolated broker stops being leader (controller fences it) | Controller loses broker heartbeat | Partitions reassigned to remaining ISR; isolated broker catches up when reconnected |
| Controller failure | Metadata updates stall | KRaft quorum detects missing leader | New controller elected via Raft in <5s |
| Disk full | Broker stops accepting writes for partitions on that disk | `kafka.log:type=Log,name=LogSize` | Expand disk or delete old segments; `delete` retention fires automatically |
| Consumer crash mid-processing | Uncommitted offsets re-processed by new consumer | Group coordinator detects missed heartbeat (session.timeout.ms) | Rebalance assigns partition to another consumer; re-processes since last commit |
| Duplicate produce (network timeout, producer retries) | Duplicate message in log without idempotent producer | Application-level deduplication or idempotent producer | Enable `enable.idempotence=true` |
| Long GC pause on consumer | Max.poll.interval exceeded → consumer kicked from group | `max.poll.interval.ms` (default 300s) | Reduce batch size; use G1GC; tune JVM; increase interval |
| Message too large | Broker rejects with `RecordTooLargeException` | Producer exception | Set `message.max.bytes` on broker; `max.request.size` on producer; compress messages |
| ZooKeeper / KRaft quorum loss | Metadata operations fail; new leader elections impossible | ZK/KRaft health checks | Restore quorum; existing partitions continue serving reads/writes with current leader |

**Retries & Idempotency:**
- Producer: `retries=Integer.MAX_VALUE`, `delivery.timeout.ms=120000` (2 min total), `enable.idempotence=true`
- Consumer: at-least-once by default; for idempotent processing, use `partition:offset` as idempotency key
- Circuit breaker: if downstream processing (DB write) fails consistently, pause consumer via `consumer.pause(partitions)` and retry after backoff; prevents commit-offset advancement while processing is broken

**Consumer Offset Management Strategy:**
```
# Manual offset commit (recommended for at-least-once):
while True:
    records = consumer.poll(timeout_ms=1000)
    for record in records:
        try:
            process(record)  # must be idempotent
        except Exception as e:
            # Dead Letter Queue: produce to errors-{topic} topic
            dlq_producer.send(f"errors-{record.topic}", record)
    # Commit only after all records processed:
    consumer.commitSync()  # blocks; use commitAsync for throughput

# Exactly-once with transactions:
producer.begin_transaction()
for record in consumer.poll():
    result = transform(record)
    producer.send(output_topic, result)
producer.send_offsets_to_transaction(consumer.assignment_offsets(), group_id)
producer.commit_transaction()
```

---

## 9. Monitoring & Observability

| Metric | Source | Alert Threshold | Meaning |
|---|---|---|---|
| `kafka.server:type=BrokerTopicMetrics,name=BytesInPerSec` | JMX | > 80% of NIC capacity | Network saturation |
| `kafka.server:type=ReplicaManager,name=UnderReplicatedPartitions` | JMX | > 0 for > 1 min | Replicas not in sync; durability risk |
| `kafka.server:type=ReplicaManager,name=IsrShrinksPerSec` | JMX | Any spike | Replica falling behind; investigate GC/disk/network |
| `kafka.controller:type=KafkaController,name=ActiveControllerCount` | JMX | != 1 | Multiple or no controllers; split-brain |
| `consumer_lag` (per group, per partition) | Burrow / consumer_offset metric | > 10,000 records for > 5 min | Consumer falling behind; scale or investigate |
| `kafka.server:type=FetcherStats,name=RequestsPerSec` | JMX | Sudden drop | Follower fetch stopped; ISR shrink imminent |
| `kafka.log:type=LogFlushStats,name=LogFlushRateAndTimeMs` | JMX | P99 > 500ms | Disk latency; I/O contention |
| `kafka.network:type=RequestMetrics,name=TotalTimeMs` (ProduceRequest) | JMX | P99 > 100ms | Slow produce; check ISR, disk, GC |
| `kafka.server:type=KafkaRequestHandlerPool,name=RequestHandlerAvgIdlePercent` | JMX | < 20% | Handler threads saturated; add brokers |
| `__consumer_offsets` partition leader | Broker log | — | If coordinator is degraded, offset commits fail; consumers hang |

**Distributed Tracing:**
- Inject `W3C Trace-Context` headers into Kafka record headers: `traceparent`, `tracestate`
- Consumer framework (Micrometer, Spring Kafka) extracts trace context and continues span
- Measure end-to-end trace: `http_request → kafka_produce_span → kafka_consume_span → db_write_span`
- Alert on spans with kafka_consume_span duration > 100ms (consumer processing too slow)

**Logging:**
- Broker: `server.log` (INFO level), `state-change.log` (leader elections, ISR changes), `log-cleaner.log` (compaction progress), `kafka-authorizer.log` (ACL denials)
- Producer: log `RecordTooLargeException`, `TimeoutException`, `ProducerFencedException` at ERROR level with topic+key context
- Consumer: log rebalance events (onPartitionsAssigned/Revoked), offset commit failures, and DLQ sends at WARN level

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Option Chosen | Alternative | Reason |
|---|---|---|---|
| Storage | Sequential log on local NVMe | BookKeeper (Pulsar) | Max throughput; OS page cache; simpler ops |
| Replication | ISR with acks=all | Majority quorum (Raft) | Space-efficient (3 brokers for 2f+1 instead of 5) |
| Coordination | KRaft (Raft-based, embedded) | ZooKeeper | Eliminates external dependency; faster failover |
| Consumer offset | Internal topic (`__consumer_offsets`) | External DB | Atomic offset commit with message consumption |
| Exactly-once | Idempotent producer + transactions | Application-level deduplication | Kafka EOS for stream processing pipelines; app dedup for external sinks |
| Partition assignment | Cooperative Sticky Assignor | Eager Assignor | Minimizes rebalance downtime |
| Log retention | Delete + optional compaction | Compaction only | Most topics need time-bounded retention; compaction for changelog topics |
| Delivery default | At-least-once | At-most-once | Safer default; consumers designed to be idempotent |
| Fan-out | Consumer groups (each reads independently) | Single subscription with copy | Infinite fan-out without message duplication |
| Backpressure | Producer `linger.ms` + `batch.size` | No batching | Batching improves throughput 10–100× vs unbatched |

---

## 11. Follow-up Interview Questions

Q1: How does Kafka differ from RabbitMQ?  
A: Kafka is a durable, ordered, replayable log. Messages are retained after consumption (based on time/size). Multiple consumer groups can read the same topic independently from different offsets. RabbitMQ is a traditional message broker: messages are deleted after acknowledgment, routing is flexible (exchanges, bindings, routing keys), and per-message TTL and priority are supported. Kafka is better for high-throughput event streaming, replay, and fan-out. RabbitMQ is better for task queues with complex routing, per-message TTL, and exactly-once delivery semantics with transactional semantics to a single consumer.

Q2: How do you size the number of partitions for a new topic?  
A: Rule: `partitions = max(throughput_target_MB/s / 10, max_consumer_count)`. Err on the side of more partitions (50–200 for important topics) since adding partitions later is disruptive. Too few: bottleneck on throughput and consumer parallelism. Too many: more file handles, more ZooKeeper/KRaft entries, longer rebalance time. LinkedIn operates with thousands of partitions per broker. Each partition requires ~1 file handle and ~1 MB RAM on the broker for index files.

Q3: What is the role of the `__consumer_offsets` topic?  
A: It is an internal Kafka topic (50 partitions by default, compaction enabled) that stores the committed offsets for all consumer groups. When a consumer commits `(group_id, topic, partition) → offset`, it sends an `OffsetCommitRequest` to the group coordinator broker. The coordinator appends a record to `__consumer_offsets`. On consumer restart, it sends `OffsetFetchRequest` to find where to resume reading. Compaction ensures only the latest offset per (group, topic, partition) is retained.

Q4: How would you implement a dead letter queue (DLQ) in Kafka?  
A: Create a separate topic: `dlq-{original_topic}`. When a consumer fails to process a message after N retries, produce it to the DLQ topic with enriched headers (original topic, partition, offset, error message, timestamp, attempt count). A separate DLQ consumer processes or alerts on these messages. Spring Kafka's `SeekToCurrentErrorHandler` does this automatically. Use `DeadLetterPublishingRecoverer` for structured DLQ records. The DLQ topic should have a longer retention than the source topic to allow investigation.

Q5: How does Kafka guarantee ordering?  
A: Ordering is guaranteed only within a single partition. All records with the same key (using the default hash partitioner) go to the same partition, so all events for a specific user or order are strictly ordered. Across partitions, there is no ordering guarantee. If global ordering is required (rare), use a single partition — but this limits throughput to one partition's capacity. Alternatively, use timestamps and accept that the consumer does time-based ordering in-memory within a short time window.

Q6: What is Log End Offset vs Committed Offset vs High Watermark?  
A: **LEO (Log End Offset):** Next offset to be written; the highest offset currently in the leader's log. **High Watermark (HW):** Highest offset replicated to all ISR members; consumers cannot read beyond this. **Committed Offset:** Consumer's position — the offset the consumer has told the coordinator it has processed. Consumer lag = HW - committed_offset. Note: "committed offset" in consumer terms is different from "committed to ISR" (broker terms); the word "committed" is overloaded.

Q7: How would you handle a producer that needs to write to multiple topics atomically?  
A: Use the Kafka Transactions API. Initialize with a `transactional.id`, call `beginTransaction()`, produce to all topics within the transaction, then call `commitTransaction()`. The 2PC protocol ensures all-or-nothing: either all messages across all topics are committed (visible to `read_committed` consumers) or all are aborted. Consumers must use `isolation.level=read_committed` to see only committed records.

Q8: What is consumer group lag and how do you alert on it without false positives?  
A: Consumer group lag = sum over all partitions of (HW offset - consumer committed offset). Simple lag alerting causes false positives: if consumers are idle (no messages to process), lag is 0 but a spike in production will show instant lag. Better alerting: alert on lag rate of change (lag increasing over time) AND alert when lag exceeds a threshold AND time elapsed since last message is < X (producer is actively sending). Burrow implements sophisticated lag analysis with "ok / warning / error / stall" consumer states.

Q9: How do you perform a schema migration for a topic used by many consumers?  
A: Use the Confluent Schema Registry with `BACKWARD` compatibility: new schema must be able to read data written with old schema. Add optional fields with defaults (never remove or change field types in BACKWARD mode). Producers upgrade to new schema first; consumers continue reading with old schema (Schema Registry handles deserialization with old schema). Once all consumers upgraded, remove old fields in a FULL_TRANSITIVE compatible change. Never use NONE compatibility in production topics with multiple consumers.

Q10: What is the risk of consumer auto-commit and how do you mitigate it?  
A: `enable.auto.commit=true` (default) commits offsets every `auto.commit.interval.ms` (default 5000ms) automatically, regardless of whether the consumer has successfully processed the record. If the consumer crashes between auto-commit and finishing processing, the records are lost (at-most-once). Mitigation: disable auto-commit (`enable.auto.commit=false`), process records, then call `commitSync()` (exactly after processing). This gives at-least-once — records may be re-processed on crash-before-commit, so processing must be idempotent.

Q11: How do you implement backpressure in a Kafka consumer pipeline?  
A: Kafka consumers apply implicit backpressure via `max.poll.records` and `max.poll.interval.ms`. If a consumer fetches 500 records but takes 5 minutes to process them, it exceeds `max.poll.interval.ms` and is kicked from the group. Control: reduce `max.poll.records`, process in parallel with a thread pool, pause consumption with `consumer.pause(partitions)` when downstream is saturated, and resume after downstream recovers. For Reactor/RxJava consumers, use bounded queues and `onBackpressureBuffer`.

Q12: What is a "hot partition" in Kafka and how do you fix it?  
A: A hot partition occurs when a specific partition key is far more frequent than others (e.g., all events for a major brand go to the same partition). That partition's leader broker becomes a bottleneck. Detection: monitor `BytesInPerSec` per partition. Fix: (1) Add a random suffix to the hot key before hashing (redistributes to multiple partitions, but breaks ordering for that entity). (2) Use a custom partitioner that routes hot keys to a dedicated partition range. (3) Pre-aggregate high-frequency events before publishing. (4) Use `null` key for events that don't need ordering (round-robin partitioning).

Q13: How would you design Kafka for multi-tenancy (multiple teams sharing one cluster)?  
A: Use topic naming conventions: `<team>.<domain>.<topic>`. Enforce ACLs per team: team A can only produce/consume `teamA.*`. Apply per-client quotas to prevent noisy neighbors: `kafka-configs.sh --add-config 'producer_byte_rate=10485760'` per service. Use separate consumer group IDs per team. Consider separate clusters for teams with very different SLAs (real-time vs batch). Monitor per-topic and per-group metrics with team labels.

Q14: How does Kafka handle back-compatibility in its wire protocol?  
A: Kafka's binary protocol is versioned. Each request/response type has an API version. Brokers support multiple API versions simultaneously; clients advertise the highest version they support in `ApiVersionsRequest`. The broker responds with its supported version range. Clients use the minimum of their max and broker's max. This allows rolling upgrades: old clients talk to new brokers (old API version), new clients talk to old brokers (old API version). New API features are only used when both sides support the new version.

Q15: What would you change if you needed Kafka to support 1 ms end-to-end latency?  
A: Standard Kafka isn't optimized for sub-10ms latency due to batching (`linger.ms`, `batch.size`). For 1 ms: (1) Set `linger.ms=0` and `batch.size=1` on producers (no batching — higher CPU per message). (2) Set `acks=1` (only leader ack — sacrifices durability). (3) Increase `num.replica.fetchers` on brokers. (4) Use SSD-backed brokers with `log.flush.interval.messages=1` (flush to disk immediately — trades throughput for latency). (5) Pin broker threads to CPU cores (`KAFKA_OPTS=-Dkafka.network.requestTimeoutMs=5000`). (6) Use RDMA networking. At 1ms, NATS or Redis Streams may be more appropriate architecturally.

---

## 12. References & Further Reading

- **Kafka Documentation — Design:** https://kafka.apache.org/documentation/#design
- **Kafka Documentation — Implementation:** https://kafka.apache.org/documentation/#implementation
- **Confluent — Kafka: The Definitive Guide (2nd ed., 2021):** Narkhede, Shapira, Palino — O'Reilly
- **Confluent — Exactly-once Semantics in Kafka:** https://www.confluent.io/blog/exactly-once-semantics-are-possible-heres-how-apache-kafka-does-it/
- **Jay Kreps — "The Log: What every software engineer should know about real-time data's unifying abstraction":** https://engineering.linkedin.com/distributed-systems/log-what-every-software-engineer-should-know-about-real-time-datas-unifying
- **Confluent — Kafka Streams Architecture:** https://kafka.apache.org/documentation/streams/architecture
- **Burrow — Consumer Lag Monitoring:** https://github.com/linkedin/Burrow
- **Confluent Schema Registry:** https://docs.confluent.io/platform/current/schema-registry/index.html
- **LinkedIn Engineering — Apache Kafka at LinkedIn:** https://engineering.linkedin.com/kafka/apache-kafka-linkedin-current-and-future
- **Uber Engineering — Kafka at Uber:** https://eng.uber.com/reliable-reprocessing/
- **Cloudflare — Kafka at Cloudflare:** https://blog.cloudflare.com/using-apache-kafka-to-process-1-trillion-messages/
- **KIP-500 — Kafka without ZooKeeper (KRaft):** https://cwiki.apache.org/confluence/display/KAFKA/KIP-500%3A+Replace+ZooKeeper+with+a+Self-Managed+Metadata+Quorum
- **Cruise Control — Kafka Cluster Balancer:** https://github.com/linkedin/cruise-control
- **AWS MSK — Managed Streaming for Apache Kafka:** https://docs.aws.amazon.com/msk/latest/developerguide/what-is-msk.html
