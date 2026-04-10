# System Design: Event Streaming Platform

> **Relevance to role:** A cloud infrastructure platform engineer needs event streaming to power real-time operational intelligence: bare-metal provisioning events flowing through CDC pipelines, Kubernetes controller state changes materialized into queryable views, OpenStack service events processed with stream analytics, and Agentic AI systems reacting to infrastructure events in real-time. This design covers the full event streaming stack beyond raw Kafka.

---

## 1. Requirement Clarifications

### Functional Requirements
| ID | Requirement |
|----|-------------|
| FR-1 | **Schema Registry**: manage and enforce schemas (Avro, Protobuf, JSON Schema) with compatibility checks |
| FR-2 | **Event Sourcing**: store all state changes as immutable events; reconstruct state via replay |
| FR-3 | **Stream Processing**: real-time transformations, aggregations, joins, windowing |
| FR-4 | **Change Data Capture (CDC)**: capture database row-level changes and stream to Kafka |
| FR-5 | **Event Replay**: reprocess historical events from any point in time |
| FR-6 | **Multi-format support**: handle structured, semi-structured, and binary events |
| FR-7 | **Connectors**: source/sink connectors for databases, object stores, search indices, cloud services |

### Non-Functional Requirements
| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | End-to-end latency (CDC to consumer) | < 500 ms p99 |
| NFR-2 | Stream processing latency | < 100 ms for stateless; < 1s for windowed aggregations |
| NFR-3 | Schema registry latency | < 10 ms for schema lookup (cached) |
| NFR-4 | Throughput | 5 GB/s aggregate across all streaming pipelines |
| NFR-5 | Exactly-once processing | End-to-end for critical pipelines |
| NFR-6 | Event replay | Replay 30 days of events within 4 hours |
| NFR-7 | Availability | 99.99% for schema registry; 99.95% for stream processing |

### Constraints & Assumptions
- Kafka cluster already deployed (see distributed_message_queue.md).
- Schema Registry runs as a stateless service backed by Kafka's `_schemas` topic.
- Stream processing via Apache Flink (primary) and Kafka Streams (lightweight).
- CDC via Debezium on MySQL and PostgreSQL.
- Events stored in Avro format (compact, schema evolution, wide ecosystem support).
- Infrastructure events: ~50,000 distinct event types across all services.

### Out of Scope
- Batch processing (Spark batch, Hadoop MapReduce).
- Data lake/warehouse ingestion (covered by sink connectors but architecture not detailed).
- Machine learning model training on event streams.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|-------------|--------|
| Event sources (microservices) | 500 services x avg 10 event types | **5,000 event types** |
| Events produced | 500 services x 2,000 events/sec avg | **1M events/sec** |
| Avg event size (Avro-encoded) | 500 bytes | -- |
| Raw throughput | 1M x 500 bytes | **500 MB/s** |
| CDC sources | 200 MySQL tables, 50 PostgreSQL tables | **250 CDC streams** |
| CDC throughput | 250 tables x 500 changes/sec x 1 KB | **125 MB/s** |
| Total event throughput | 500 + 125 MB/s | **625 MB/s** |
| Schema Registry lookups | 1M events/sec (each needs schema) | **1M lookups/sec** (99.9% cached) |
| Stream processing jobs | 200 Flink jobs, 100 Kafka Streams apps | **300 streaming pipelines** |
| Unique schemas | 5,000 types x 20 versions avg | **100,000 schema versions** |

### Latency Requirements

| Path | Target | Mechanism |
|------|--------|-----------|
| Producer -> Schema Registry lookup | < 1 ms (cached) | Local LRU cache in serializer |
| CDC (MySQL binlog -> Kafka) | < 200 ms | Debezium reads binlog in near-real-time |
| Stream processing (stateless filter) | < 10 ms | Flink operator chain, in-memory |
| Stream processing (windowed aggregation) | < 1 s | Flink window trigger + state backend |
| End-to-end (DB change -> materialized view) | < 2 s | CDC + stream processing + sink |
| Event replay (1 hour of data) | < 5 min | Parallel consumer, high fetch throughput |

### Storage Estimates

| Component | Calculation | Result |
|-----------|-------------|--------|
| Schema Registry | 100,000 schemas x 5 KB avg | **500 MB** (trivial) |
| Event data (Kafka, 30-day retention) | 625 MB/s x 86,400 x 30 x 3 (RF) | **4.86 PB** |
| Flink state (RocksDB checkpoints) | 300 jobs x 10 GB avg state | **3 TB** |
| Flink checkpoints (HDFS/S3) | 3 TB x 3 retained checkpoints | **9 TB** |

### Bandwidth Estimates

| Path | Calculation | Result |
|------|-------------|--------|
| CDC sources -> Kafka | 125 MB/s | 1 Gbps |
| Application producers -> Kafka | 500 MB/s | 4 Gbps |
| Kafka -> Flink consumers | 625 MB/s x 2 (some topics consumed by multiple jobs) | ~10 Gbps |
| Flink -> Kafka (processed output) | 300 MB/s (not all jobs produce output) | 2.4 Gbps |
| Kafka -> Sink connectors | 200 MB/s (Elasticsearch, S3, MySQL) | 1.6 Gbps |

---

## 3. High Level Architecture

```
+------------------+     +------------------+     +------------------+
| MySQL / Postgres |     | Java/Python      |     | Infrastructure   |
| (source DBs)     |     | Microservices    |     | Controllers      |
+--------+---------+     +--------+---------+     +--------+---------+
         |                        |                        |
    Debezium CDC            Avro Serializer           Avro Serializer
    (binlog/WAL)            + Schema Registry         + Schema Registry
         |                        |                        |
         v                        v                        v
+--------+------------------------+------------------------+--------+
|                          Kafka Cluster                            |
|                                                                   |
|  Topics:  db.inventory.orders   service.orders.created            |
|           db.inventory.products service.payments.completed         |
|           _schemas              __consumer_offsets                 |
+--------+------------+------------+------------+-------------------+
         |            |            |            |
         v            v            v            v
+--------+--+  +------+-----+  +--+--------+  +-----+---------+
| Schema     |  | Flink      |  | Kafka     |  | Kafka Connect |
| Registry   |  | Cluster    |  | Streams   |  | (Sinks)       |
| (lookup)   |  | (complex   |  | (simple   |  |               |
|            |  |  processing)|  |  transforms)| | ES Sink      |
+------------+  +------+-----+  +--+--------+  | S3 Sink      |
                       |            |           | JDBC Sink    |
                       v            v           +------+-------+
                +------+------------+--+               |
                | Kafka (output topics)|               v
                +----------+-----------+    +----------+----------+
                           |                | Elasticsearch / S3  |
                           v                | MySQL (materialized)|
                    +------+------+         +---------------------+
                    | Consumers   |
                    | (downstream)|
                    +-------------+

Schema Registry Detail:
+---------------------------------------------------+
| Schema Registry (Stateless, HA behind LB)         |
|                                                   |
|  POST /subjects/{subject}/versions  -> register   |
|  GET  /schemas/ids/{id}             -> lookup     |
|  POST /compatibility/subjects/{s}/versions/{v}    |
|                                     -> check      |
|                                                   |
|  Backend: _schemas topic (compacted, RF=3)        |
|  Cache: in-memory (all schemas, ~500 MB)          |
+---------------------------------------------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **Schema Registry** | Central schema store; enforces compatibility; provides schema ID for compact serialization |
| **Avro Serializer** | Client-side: serializes payload with schema, prepends 5-byte header (magic byte + 4-byte schema ID) |
| **Debezium** | CDC connector: reads MySQL binlog / Postgres WAL, produces change events to Kafka |
| **Kafka Connect** | Framework for source/sink connectors; manages offset tracking, scalability, fault tolerance |
| **Flink Cluster** | Distributed stream processing: complex event processing, windowed aggregations, stateful joins |
| **Kafka Streams** | Library-based stream processing: lightweight transformations within Java applications |
| **Sink Connectors** | Write processed events to downstream stores (Elasticsearch, S3, JDBC databases) |

### Data Flows

**CDC Flow (MySQL -> Elasticsearch):**
1. Application writes to MySQL `orders` table.
2. MySQL writes to binlog (row-based replication format).
3. Debezium reads binlog, produces change event to `db.inventory.orders` topic.
4. Event contains: `{before: {...}, after: {...}, op: "u", ts_ms: ...}`.
5. Flink job consumes, enriches (joins with customer data), transforms.
6. Flink produces to `enriched.orders` topic.
7. Elasticsearch Sink Connector consumes, indexes into ES.
8. Dashboard queries ES for real-time order analytics.

---

## 4. Data Model

### Core Entities & Schema

**Schema Registry Subject:**
```json
{
  "subject": "orders-value",
  "version": 3,
  "id": 42,
  "schemaType": "AVRO",
  "schema": "{\"type\":\"record\",\"name\":\"Order\",\"fields\":[...]}"
}
```

**Avro Schema Example (with evolution):**
```json
// Version 1
{
  "type": "record",
  "name": "Order",
  "namespace": "com.infra.events",
  "fields": [
    {"name": "order_id", "type": "string"},
    {"name": "customer_id", "type": "string"},
    {"name": "amount", "type": "double"},
    {"name": "status", "type": {"type": "enum", "name": "Status", 
                                 "symbols": ["CREATED", "PAID", "SHIPPED"]}}
  ]
}

// Version 2 (BACKWARD compatible: new field has default)
{
  "type": "record",
  "name": "Order",
  "namespace": "com.infra.events",
  "fields": [
    {"name": "order_id", "type": "string"},
    {"name": "customer_id", "type": "string"},
    {"name": "amount", "type": "double"},
    {"name": "status", "type": {"type": "enum", "name": "Status",
                                 "symbols": ["CREATED", "PAID", "SHIPPED", "CANCELLED"]}},
    {"name": "currency", "type": "string", "default": "USD"}
  ]
}
```

**Debezium CDC Event (MySQL):**
```json
{
  "schema": { ... },
  "payload": {
    "before": {"id": 1001, "name": "Widget", "price": 9.99},
    "after":  {"id": 1001, "name": "Widget Pro", "price": 14.99},
    "source": {
      "version": "2.5.0",
      "connector": "mysql",
      "name": "db.inventory",
      "ts_ms": 1712620800000,
      "db": "inventory",
      "table": "products",
      "server_id": 1,
      "file": "mysql-bin.000003",
      "pos": 12345,
      "row": 0
    },
    "op": "u",           // c=create, u=update, d=delete, r=read (snapshot)
    "ts_ms": 1712620800123,
    "transaction": null
  }
}
```

**Event Sourcing Event:**
```json
{
  "event_id": "uuid-v7-sortable",
  "aggregate_type": "Order",
  "aggregate_id": "order-12345",
  "event_type": "OrderShipped",
  "version": 5,
  "timestamp": "2026-04-09T10:30:00Z",
  "data": {
    "tracking_number": "1Z999AA10123456784",
    "carrier": "UPS"
  },
  "metadata": {
    "user_id": "user-789",
    "correlation_id": "req-abc",
    "causation_id": "event-uuid-4"
  }
}
```

### Database Selection

| Data | Storage | Rationale |
|------|---------|-----------|
| Schemas | Kafka `_schemas` topic (compacted) | HA, durable, no external DB dependency |
| Event stream (raw) | Kafka topics (append-only log) | Immutable, replayable, high throughput |
| Event store (sourcing) | Kafka or PostgreSQL (with event table) | Kafka for streaming; Postgres for querying by aggregate |
| Flink state | RocksDB (local) + S3/HDFS (checkpoints) | Fast local state with durable checkpoints |
| Materialized views | Elasticsearch, MySQL, Redis | Optimized for read patterns |
| Connect offsets | Kafka `connect-offsets` topic | Built-in connector offset management |

### Indexing Strategy

| Store | Index | Pattern |
|-------|-------|---------|
| Event store (Postgres) | `(aggregate_type, aggregate_id, version)` unique | Reconstruct aggregate state |
| Event store (Postgres) | `(event_type, timestamp)` | Query events by type and time range |
| Elasticsearch (materialized) | Full-text on event payload fields | Search and analytics |
| Schema Registry cache | `(subject, version) -> schema_id` | Fast serializer lookup |
| Schema Registry cache | `schema_id -> schema` | Fast deserializer lookup |

---

## 5. API Design

### Producer/Consumer APIs

**Schema Registry REST API:**
```
# Register a new schema version
POST /subjects/{subject}/versions
Content-Type: application/json
{
  "schemaType": "AVRO",
  "schema": "{\"type\":\"record\",\"name\":\"Order\",...}"
}
Response: {"id": 42}

# Get schema by ID (used by deserializer)
GET /schemas/ids/{id}
Response: {"schema": "{...}"}

# Check compatibility before registering
POST /compatibility/subjects/{subject}/versions/latest
{
  "schemaType": "AVRO",
  "schema": "{...}"
}
Response: {"is_compatible": true}

# List all subjects
GET /subjects
Response: ["orders-value", "payments-value", ...]

# Get all versions for a subject
GET /subjects/{subject}/versions
Response: [1, 2, 3]

# Delete a schema version (soft delete)
DELETE /subjects/{subject}/versions/{version}

# Set compatibility level
PUT /config/{subject}
{"compatibility": "BACKWARD"}
```

**Kafka Connect REST API:**
```
# Deploy a connector
POST /connectors
{
  "name": "mysql-cdc-orders",
  "config": {
    "connector.class": "io.debezium.connector.mysql.MySqlConnector",
    "database.hostname": "mysql-primary.internal",
    "database.port": "3306",
    "database.user": "debezium",
    "database.password": "${vault:mysql/debezium}",
    "database.server.id": "1001",
    "database.server.name": "db.inventory",
    "database.include.list": "inventory",
    "table.include.list": "inventory.orders,inventory.products",
    "database.history.kafka.topic": "schema-changes.inventory",
    "transforms": "unwrap",
    "transforms.unwrap.type": "io.debezium.transforms.ExtractNewRecordState",
    "key.converter": "io.confluent.connect.avro.AvroConverter",
    "value.converter": "io.confluent.connect.avro.AvroConverter",
    "key.converter.schema.registry.url": "http://schema-registry:8081",
    "value.converter.schema.registry.url": "http://schema-registry:8081"
  }
}

# Check connector status
GET /connectors/mysql-cdc-orders/status
Response: {
  "name": "mysql-cdc-orders",
  "connector": {"state": "RUNNING", "worker_id": "connect-1:8083"},
  "tasks": [
    {"id": 0, "state": "RUNNING", "worker_id": "connect-1:8083"}
  ]
}

# Restart a failed task
POST /connectors/mysql-cdc-orders/tasks/0/restart

# Pause/Resume
PUT /connectors/mysql-cdc-orders/pause
PUT /connectors/mysql-cdc-orders/resume
```

**Flink SQL (stream processing):**
```sql
-- Define Kafka source table
CREATE TABLE orders (
    order_id STRING,
    customer_id STRING,
    amount DOUBLE,
    status STRING,
    event_time TIMESTAMP(3),
    WATERMARK FOR event_time AS event_time - INTERVAL '5' SECOND
) WITH (
    'connector' = 'kafka',
    'topic' = 'db.inventory.orders',
    'properties.bootstrap.servers' = 'broker1:9092',
    'properties.group.id' = 'flink-orders-aggregation',
    'format' = 'avro-confluent',
    'avro-confluent.url' = 'http://schema-registry:8081',
    'scan.startup.mode' = 'latest-offset'
);

-- Windowed aggregation
CREATE TABLE order_hourly_stats (
    window_start TIMESTAMP(3),
    window_end TIMESTAMP(3),
    order_count BIGINT,
    total_amount DOUBLE,
    avg_amount DOUBLE
) WITH (
    'connector' = 'kafka',
    'topic' = 'analytics.orders.hourly',
    'format' = 'avro-confluent',
    'avro-confluent.url' = 'http://schema-registry:8081'
);

INSERT INTO order_hourly_stats
SELECT
    TUMBLE_START(event_time, INTERVAL '1' HOUR) AS window_start,
    TUMBLE_END(event_time, INTERVAL '1' HOUR) AS window_end,
    COUNT(*) AS order_count,
    SUM(amount) AS total_amount,
    AVG(amount) AS avg_amount
FROM orders
GROUP BY TUMBLE(event_time, INTERVAL '1' HOUR);
```

### Admin APIs

```python
# Flink REST API for job management
# Submit a job
POST /jars/{jar_id}/run
{
    "entryClass": "com.infra.streaming.OrderAggregation",
    "parallelism": 16,
    "programArgs": "--bootstrap.servers broker1:9092 --checkpoint.interval 60000"
}

# Get job status
GET /jobs/{job_id}
Response: {
    "jid": "abc123",
    "name": "OrderAggregation",
    "state": "RUNNING",
    "start-time": 1712620800000,
    "vertices": [
        {"name": "Source: orders", "parallelism": 16, "status": "RUNNING"},
        {"name": "Window Aggregate", "parallelism": 16, "status": "RUNNING"},
        {"name": "Sink: kafka", "parallelism": 16, "status": "RUNNING"}
    ]
}

# Trigger savepoint (for upgrades/migration)
POST /jobs/{job_id}/savepoints
{"target-directory": "s3://flink-savepoints/order-aggregation/"}

# Cancel with savepoint
POST /jobs/{job_id}/stop
{"targetDirectory": "s3://flink-savepoints/order-aggregation/"}
```

### CLI

```bash
# Schema Registry CLI
kafka-avro-console-producer --broker-list broker1:9092 \
    --topic orders \
    --property schema.registry.url=http://schema-registry:8081 \
    --property value.schema='{"type":"record","name":"Order","fields":[...]}'

kafka-avro-console-consumer --bootstrap-server broker1:9092 \
    --topic orders \
    --from-beginning \
    --property schema.registry.url=http://schema-registry:8081

# Kafka Connect CLI (via curl)
curl -X GET http://connect-cluster:8083/connectors | jq .
curl -X GET http://connect-cluster:8083/connectors/mysql-cdc-orders/status | jq .

# Flink CLI
flink run -d -p 16 -c com.infra.streaming.OrderAggregation streaming-jobs.jar
flink list  # list running jobs
flink savepoint <job_id> s3://flink-savepoints/
flink cancel <job_id>

# Debezium snapshot trigger
curl -X POST http://connect-cluster:8083/connectors/mysql-cdc-orders/restart \
    -H "Content-Type: application/json" \
    -d '{"includeTasks": "true", "onlyFailed": "false"}'
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Schema Registry & Schema Evolution

**Why it's hard:**
In a distributed system with hundreds of producers and consumers, schema changes must be coordinated. A producer deploying a new schema version before consumers are updated will cause deserialization failures. The Schema Registry must enforce compatibility rules to prevent breaking changes.

**Approaches:**

| Approach | Schema Management | Evolution Support | Performance | Adoption |
|----------|------------------|-------------------|-------------|----------|
| No schema (JSON strings) | None | None (hope for the best) | Good (no overhead) | Easy |
| Schema-per-message (embedded) | In message | Full | Poor (bloated messages) | Easy |
| **Schema Registry (external)** | **Centralized** | **Compatibility-enforced** | **Excellent (5-byte overhead)** | **Medium** |
| Schema-in-header | In Kafka header | Manual | Good | Easy |

**Selected: Confluent Schema Registry with Avro**

**Compatibility modes:**

| Mode | Rule | Use Case |
|------|------|----------|
| BACKWARD (default) | New schema can read old data | Consumer upgrades first |
| FORWARD | Old schema can read new data | Producer upgrades first |
| FULL | Both BACKWARD and FORWARD | Most restrictive, safest |
| BACKWARD_TRANSITIVE | BACKWARD against ALL previous versions | Strict long-term compatibility |
| FORWARD_TRANSITIVE | FORWARD against ALL previous versions | Strict long-term compatibility |
| FULL_TRANSITIVE | FULL against ALL previous versions | Maximum safety |
| NONE | No compatibility check | Schema-less evolution (dangerous) |

**What's allowed per mode:**

| Change | BACKWARD | FORWARD | FULL |
|--------|----------|---------|------|
| Add field with default | Yes | Yes | Yes |
| Add field without default | No | Yes | No |
| Remove field with default | Yes | No | No |
| Remove field without default | No | No | No |
| Rename field | No | No | No |

**Serialization flow (wire format):**
```
Avro wire format (Confluent):
  [0x00] [schema_id: 4 bytes big-endian] [avro_payload: N bytes]
  
  Total overhead: 5 bytes per message
  
Producer serialization:
  1. Check local cache for (subject, schema) -> schema_id
  2. Cache miss: POST to Schema Registry, get schema_id
  3. Serialize: magic_byte + schema_id + avro_encode(record, schema)
  
Consumer deserialization:
  1. Read schema_id from first 5 bytes
  2. Check local cache for schema_id -> (writer_schema)
  3. Cache miss: GET /schemas/ids/{id} from Schema Registry
  4. Deserialize: avro_decode(payload, writer_schema, reader_schema)
     (Avro resolution rules handle schema differences)
```

**Implementation detail:**
```java
// Java: Schema-aware producer
public class AvroProducerConfig {
    public static Properties build() {
        Properties props = new Properties();
        props.put(ProducerConfig.BOOTSTRAP_SERVERS_CONFIG, "broker1:9092");
        props.put(ProducerConfig.KEY_SERIALIZER_CLASS_CONFIG, StringSerializer.class);
        props.put(ProducerConfig.VALUE_SERIALIZER_CLASS_CONFIG, 
                  KafkaAvroSerializer.class);
        props.put("schema.registry.url", "http://schema-registry:8081");
        props.put("auto.register.schemas", false);  // CI/CD registers schemas
        props.put("use.latest.version", true);
        return props;
    }
}

// CI/CD pipeline registers schema before deployment:
// maven-plugin: schema-registry:register
// or: curl -X POST http://schema-registry:8081/subjects/orders-value/versions \
//       -H "Content-Type: application/vnd.schemaregistry.v1+json" \
//       -d @order-v2.avsc
```

**Failure Modes:**
- **Schema Registry down:** Producers/consumers use cached schema IDs. New schemas cannot be registered. Impact depends on how long the outage lasts and whether new schema versions are being deployed.
- **Incompatible schema registered (NONE mode):** Consumers fail to deserialize. Mitigation: always use at least BACKWARD compatibility.
- **Schema ID overflow:** IDs are auto-incrementing integers. At 1M schemas/day, it takes 5,800 years to overflow int32. Not a concern.
- **Cache poisoning:** If Schema Registry returns wrong schema for an ID, all consumers produce corrupt data. Mitigation: Schema Registry is backed by compacted Kafka topic; data is immutable once written.

**Interviewer Q&As:**

**Q1: Why Avro over Protobuf or JSON Schema for Kafka?**
A: Avro has the best schema evolution story for streaming: (1) Writer schema is not embedded in the message (only 4-byte ID), unlike Protobuf which needs field tags. (2) Avro's schema resolution allows readers with a different schema version to deserialize data automatically. (3) Avro is the native format for Confluent's ecosystem. Protobuf is a valid choice too, especially if you already use gRPC; it has better cross-language support but slightly less compact for Kafka-specific workloads.

**Q2: How do you handle a breaking schema change?**
A: You cannot make breaking changes under BACKWARD or FORWARD compatibility. Options: (1) Create a new topic with the new schema; migrate consumers. (2) Use a "dual-write" period: produce to both old and new topics. (3) Use a Flink job to transform old-format events to new-format. (4) If absolutely necessary: set compatibility to NONE temporarily (with team-wide coordination).

**Q3: What happens if the Schema Registry is unavailable during producer startup?**
A: The producer cannot serialize the first message (schema ID lookup fails). The producer will block or throw an exception depending on configuration. Mitigation: (1) Schema Registry is stateless and horizontally scalable behind a load balancer. (2) Warm the cache on startup. (3) Fallback to embedded schema ID if known.

**Q4: How do you manage schemas in a CI/CD pipeline?**
A: (1) Schema files (.avsc, .proto) live in the service's git repo. (2) CI pipeline registers the schema with Schema Registry (compatibility check happens automatically). (3) If compatibility check fails, CI fails -> developer must fix the schema. (4) Schema ID is not hardcoded; the serializer discovers it at runtime.

**Q5: What is the subject naming strategy?**
A: Default: `{topic}-key` and `{topic}-value`. Alternative: `{record.name}` (TopicRecordNameStrategy) allows multiple schemas per topic. Choose based on whether topics are homogeneous (one event type per topic) or heterogeneous (multiple event types).

**Q6: How do you handle schema evolution for Debezium CDC events?**
A: Debezium generates Avro schemas based on the MySQL/Postgres table DDL. When a column is added, Debezium auto-registers a new schema version. If the DDL change is backward-compatible (add nullable column), the schema evolution is seamless. If it's breaking (drop column), you need to coordinate with consumers.

---

### Deep Dive 2: Stream Processing (Flink vs Kafka Streams)

**Why it's hard:**
Stream processing must handle infinite data streams with bounded resources, maintain consistent state across failures, handle out-of-order events (late arrivals), and provide exactly-once semantics. The choice between Flink and Kafka Streams depends on the use case.

**Approaches:**

| Feature | Kafka Streams | Apache Flink | Spark Structured Streaming |
|---------|--------------|--------------|---------------------------|
| Deployment | Library (embedded in app) | Cluster (JobManager/TaskManagers) | Cluster (Driver/Executors) |
| State management | RocksDB + changelog topic | RocksDB + checkpoints (S3/HDFS) | In-memory + WAL |
| Exactly-once | Kafka transactions | Checkpoint barriers | Micro-batch (inherent) |
| Latency | < 10 ms | < 10 ms (event-time) | 100 ms - seconds (micro-batch) |
| Windowing | Tumbling, sliding, session | Tumbling, sliding, session, global | Tumbling, sliding |
| SQL support | No (KSQL is separate) | Yes (Flink SQL) | Yes (Spark SQL) |
| Event-time processing | Yes | Yes (best-in-class) | Yes |
| Scale | Scales with Kafka partitions | Independent scaling | Independent scaling |
| Best for | Simple transforms in microservices | Complex event processing, joins | Batch+streaming unified |

**Selected: Flink for complex processing; Kafka Streams for lightweight transforms**

**Flink Exactly-Once via Checkpoint Barriers:**
```
How Flink achieves exactly-once:

1. JobManager periodically injects checkpoint barriers into source streams.
2. Barrier flows through the operator graph like a regular event.
3. When an operator receives barriers from ALL input channels:
   a. Snapshots its state to persistent storage (S3/HDFS)
   b. Forwards the barrier downstream
4. When all sinks acknowledge the barrier:
   a. Checkpoint is complete
   b. Committed offsets are saved
5. On failure:
   a. Restore from latest completed checkpoint
   b. Reset Kafka consumer offsets to checkpoint offsets
   c. Replay events from checkpoint offset
   d. All state is restored to checkpoint snapshot
   e. No duplicates because state + offsets are atomically restored

Barrier alignment (exactly-once):
  Stream A: [e1] [e2] [BARRIER] [e3] [e4]
  Stream B: [e5] [e6] [e7] [BARRIER] [e8]
  
  Operator waits for barriers from BOTH streams before snapshotting.
  Events after barrier from Stream A are buffered until Stream B's barrier arrives.
  
  At-least-once (no alignment):
  Operator doesn't wait; snapshots immediately on first barrier.
  Faster but may include events past the barrier (replayed on recovery = duplicates).
```

**Windowing semantics:**
```
Event stream with event-time timestamps:

Tumbling window (1 hour):
  [00:00 - 01:00) [01:00 - 02:00) [02:00 - 03:00)
  Events: |e1(00:05) e2(00:30) e3(00:55)| |e4(01:10) e5(01:45)| |e6(02:20)|
  Output: {count:3, sum:...}              {count:2, sum:...}    {count:1}
  
  No overlap. Each event belongs to exactly one window.

Sliding window (1 hour, slide 15 minutes):
  [00:00 - 01:00) [00:15 - 01:15) [00:30 - 01:30) ...
  An event at 00:30 belongs to windows: [00:00-01:00), [00:15-01:15), [00:30-01:30)
  
  Overlapping. Each event may belong to multiple windows.
  Memory intensive: more windows to maintain.

Session window (gap = 30 minutes):
  Events: e1(00:05) e2(00:20) e3(00:45)    e4(02:00) e5(02:10)
  Sessions: [e1, e2, e3] (gap < 30min)     [e4, e5] (gap < 30min)
  Gap between e3 and e4 is > 30min -> new session.
  
  Dynamic boundaries. Useful for user session analysis.
```

**Watermarks and late events:**
```python
# Watermark: "No events with timestamp < W will arrive"
# Allows Flink to close windows and emit results

# Event-time processing with allowed lateness:
watermark_strategy = WatermarkStrategy \
    .for_bounded_out_of_orderness(Duration.of_seconds(5)) \
    .with_timestamp_assigner(lambda event, _: event.timestamp)

# What happens to late events (arriving after watermark)?
# Option 1: Drop (default after window closes)
# Option 2: Side output (collect in separate stream for manual processing)
# Option 3: Allowed lateness (window stays open for additional time, emits updates)

# Flink Java:
DataStream<Order> orders = env
    .addSource(new FlinkKafkaConsumer<>("orders", schema, props))
    .assignTimestampsAndWatermarks(
        WatermarkStrategy.<Order>forBoundedOutOfOrderness(Duration.ofSeconds(5))
            .withTimestampAssigner((order, ts) -> order.getEventTime())
    );

SingleOutputStreamOperator<OrderStats> result = orders
    .keyBy(Order::getRegion)
    .window(TumblingEventTimeWindows.of(Time.hours(1)))
    .allowedLateness(Time.minutes(10))      // window stays open 10 min after watermark
    .sideOutputLateData(lateOutputTag)       // truly late events go to side output
    .aggregate(new OrderAggregator());

// Side output for late events
DataStream<Order> lateOrders = result.getSideOutput(lateOutputTag);
lateOrders.addSink(new KafkaSink<>("late-orders", ...));
```

**Failure Modes:**
- **Flink TaskManager crash:** JobManager detects (heartbeat timeout). Restarts task on another TaskManager. State restored from latest checkpoint.
- **Flink JobManager crash:** HA mode (ZooKeeper or Kubernetes): standby JobManager takes over. Jobs restored from checkpoint.
- **Checkpoint timeout:** If checkpoint takes longer than `checkpoint.timeout`, it's aborted. Causes: large state, slow S3 upload, back-pressure. Tune: `state.backend.rocksdb.checkpoint.transfer.thread.num`.
- **Back-pressure:** Downstream operator slower than upstream. Symptoms: checkpoint barriers delayed, increasing latency. Fix: increase parallelism of bottleneck operator.

**Interviewer Q&As:**

**Q1: When would you choose Kafka Streams over Flink?**
A: Kafka Streams when: (1) Simple transformations (filter, map, enrich). (2) You want to embed processing in a microservice (no separate cluster). (3) Source and sink are both Kafka. (4) State size is moderate (fits in local RocksDB). Flink when: (1) Complex event processing (multi-stream joins, complex windowing). (2) Source/sink diversity (JDBC, S3, Elasticsearch). (3) SQL-based processing preferred. (4) Very large state. (5) Need for fine-grained checkpointing control.

**Q2: How does Flink handle back-pressure?**
A: Flink uses credit-based flow control. Each receiving task sends credits (available buffer space) to the sending task. When credits are exhausted, the sender pauses. Back-pressure propagates from sink to source. The Kafka source pauses fetching when downstream is slow. Monitoring: Flink dashboard shows back-pressure per operator.

**Q3: What is the impact of event-time vs processing-time windows?**
A: Event-time windows produce correct results regardless of processing delays (events are placed in windows based on when they occurred, not when they arrived). Processing-time windows are simpler and lower-latency but produce inconsistent results if events are delayed or replayed. Always use event-time for business logic; processing-time only for operational metrics where approximation is acceptable.

**Q4: How do you upgrade a Flink job without losing state?**
A: (1) Take a savepoint: `flink savepoint <job_id> s3://path`. (2) Cancel the job. (3) Deploy new JAR. (4) Start from savepoint: `flink run -s s3://path/savepoint-xxx new-job.jar`. State is restored if operator UIDs match. Always assign explicit UIDs to operators: `.uid("order-aggregator")`.

**Q5: What happens if a Kafka partition has no events for a long time (idle source)?**
A: Watermark advancement stalls because Flink takes the minimum watermark across all partitions. If partition 5 has no events, its watermark stays at the last event's time, preventing windows from closing. Solution: `withIdleness(Duration.ofMinutes(5))` - marks the partition as idle, and its watermark is excluded from the minimum calculation.

**Q6: How do you handle exactly-once end-to-end (Kafka source -> Flink -> Kafka sink)?**
A: Flink's `TwoPhaseCommitSinkFunction` for Kafka: (1) On checkpoint barrier, Flink pre-commits the Kafka transaction. (2) On checkpoint completion, Flink commits the transaction. (3) Consumer must read with `isolation.level=read_committed`. Combined with Kafka consumer offset management in the checkpoint, this achieves end-to-end exactly-once.

---

### Deep Dive 3: Change Data Capture (CDC) with Debezium

**Why it's hard:**
CDC must capture every database change without missing any, maintain the exact order of operations, handle schema evolution (DDL changes), and do all this without impacting the source database's performance.

**Approaches:**

| Approach | Completeness | Ordering | DB Impact | Complexity |
|----------|-------------|----------|-----------|------------|
| Polling (timestamp column) | Misses deletes; misses rapid updates | Approximate | High (polling queries) | Low |
| Triggers | Complete | Per-table | Medium (trigger overhead) | Medium |
| **Log-based CDC (Debezium)** | **Complete** | **Exact** | **Minimal** | **Medium** |
| Application-level dual-write | Depends on app correctness | No guarantee | None | High (consistency issues) |

**Selected: Debezium log-based CDC**

**MySQL CDC mechanism:**
```
MySQL binlog (row-based format):
  1. Every INSERT/UPDATE/DELETE writes a binlog event.
  2. Debezium connects as a MySQL replica (SHOW MASTER STATUS, CHANGE MASTER TO).
  3. Debezium reads binlog events in real-time via MySQL replication protocol.
  4. Debezium converts binlog events to Kafka records.
  5. One Kafka topic per table: "server_name.database.table"

Initial snapshot:
  1. Debezium takes a global read lock (optional, can use snapshot.locking.mode=none)
  2. Reads current binlog position
  3. SELECT * from each table (consistent snapshot at binlog position)
  4. Produces snapshot records with op="r" (read)
  5. Releases lock
  6. Switches to streaming mode from the captured binlog position
  7. No data missed between snapshot and streaming

PostgreSQL CDC mechanism:
  1. Uses logical replication slots (wal2json or pgoutput plugin)
  2. PostgreSQL streams WAL changes to Debezium
  3. Replication slot ensures no WAL is recycled before Debezium reads it
  4. Caution: if Debezium is down, WAL accumulates on disk
```

**Debezium architecture in Kafka Connect:**
```
+-------------------+
| Kafka Connect     |
| Cluster           |
|                   |
| +---------------+ |     +------------------+
| | Debezium      | |<--->| MySQL Primary    |
| | MySQL         | |     | (binlog)         |
| | Connector     | |     +------------------+
| | (1 task)      | |
| +-------+-------+ |
|         |         |
| +-------v-------+ |
| | Kafka Producer | |---> Kafka Topics
| | (internal)     | |     db.inventory.orders
| +---------------+ |     db.inventory.products
+-------------------+
```

**Failure Modes:**
- **Debezium connector crash:** Kafka Connect restarts the task. Debezium resumes from the last committed binlog position (stored in Kafka connect-offsets topic). No data loss.
- **MySQL binlog purged before Debezium reads it:** Debezium loses its position. Must re-snapshot. Prevention: set `expire_logs_days` > Debezium max downtime.
- **PostgreSQL replication slot not consumed:** WAL accumulates, disk fills. Alert on `pg_replication_slots.confirmed_flush_lsn` lag.
- **Schema change (DDL):** Debezium reads DDL from binlog, updates internal schema history. If DDL is not backward-compatible, downstream consumers may break.

**Interviewer Q&As:**

**Q1: Why log-based CDC over polling?**
A: (1) Captures all changes including deletes (polling can't detect deletes without soft-delete). (2) Exact ordering (polling may miss rapid updates between polls). (3) Lower database load (no polling queries; binlog reading is lightweight). (4) Lower latency (real-time vs polling interval).

**Q2: How does Debezium handle schema changes?**
A: Debezium maintains a schema history topic in Kafka. When it encounters a DDL event in the binlog, it records the DDL statement and the resulting schema. On restart, it replays the schema history to reconstruct the current schema. If a schema change is not backward-compatible (e.g., column type change), downstream consumers need to be updated.

**Q3: What is the impact of Debezium on MySQL performance?**
A: Minimal. Debezium reads the binlog as a replication slave. The binlog is already being written by MySQL for its own replication. Additional impact: one more replication connection, slight increase in binlog read I/O. For very high-throughput databases, the binlog network transfer may be noticeable.

**Q4: How do you handle the initial snapshot for a large table (100M+ rows)?**
A: (1) Use `snapshot.mode=initial` (default): reads entire table, then switches to streaming. For large tables, this can take hours. (2) Use `snapshot.fetch.size=10000` to control memory usage. (3) For very large tables: `snapshot.mode=schema_only` (skip data, only capture new changes) and backfill via a separate batch process.

**Q5: Can Debezium guarantee exactly-once delivery?**
A: Debezium provides at-least-once delivery. On restart, it may re-read some binlog events (between the last committed offset and the crash point). Consumers must be idempotent. For exactly-once: use Debezium's transaction metadata to detect and deduplicate at the consumer.

**Q6: How do you monitor Debezium CDC lag?**
A: (1) `debezium_metrics_MilliSecondsBehindSource`: time lag between current binlog position and Debezium's read position. (2) Compare MySQL `SHOW MASTER STATUS` position with Debezium's last processed position. (3) Kafka consumer lag on the CDC output topics. Alert if lag exceeds 30 seconds.

---

### Deep Dive 4: Event Sourcing & CQRS

**Why it's hard:**
Event sourcing stores all state changes as immutable events. Reconstructing current state requires replaying all events for an aggregate, which becomes slow as event count grows. CQRS (Command Query Responsibility Segregation) separates the write model (events) from read models (materialized views), adding complexity.

**Approaches:**

| Approach | Write Model | Read Model | Consistency | Complexity |
|----------|------------|------------|-------------|------------|
| Traditional CRUD | Mutable DB rows | Same DB | Strong | Low |
| Event sourcing (single store) | Event log | Replay from log | Strong (within aggregate) | Medium |
| **Event sourcing + CQRS** | **Event log (Kafka)** | **Materialized views** | **Eventual** | **High** |
| Event sourcing + snapshots | Event log + periodic snapshots | Snapshot + tail events | Strong (within aggregate) | High |

**Selected: Event Sourcing + CQRS with Kafka as event store**

**Event store design:**
```python
class EventStore:
    """Kafka-backed event store for event sourcing."""
    
    def __init__(self, kafka_producer, kafka_consumer):
        self.producer = kafka_producer
        self.consumer = kafka_consumer
    
    def append_event(self, aggregate_type, aggregate_id, event):
        """
        Append event to Kafka topic.
        Topic: events.{aggregate_type}
        Key: aggregate_id (ensures ordering per aggregate)
        """
        topic = f"events.{aggregate_type}"
        record = ProducerRecord(
            topic=topic,
            key=aggregate_id,
            value=serialize(event),
            headers={
                "event_type": event.type,
                "event_version": str(event.version),
                "correlation_id": event.metadata.correlation_id
            }
        )
        # Synchronous send for guaranteed ordering
        self.producer.send(record).get(timeout=5)
    
    def load_events(self, aggregate_type, aggregate_id):
        """Load all events for an aggregate (replay)."""
        topic = f"events.{aggregate_type}"
        # Use compacted topic with aggregate_id as key
        # Or: query a projection store indexed by aggregate_id
        return self.projection_store.get_events(aggregate_type, aggregate_id)

class OrderAggregate:
    """Reconstruct order state from events."""
    
    def __init__(self):
        self.order_id = None
        self.status = None
        self.items = []
        self.total = 0.0
        self.version = 0
    
    def apply(self, event):
        """Apply an event to update state."""
        if event.type == "OrderCreated":
            self.order_id = event.data["order_id"]
            self.status = "CREATED"
            self.items = event.data["items"]
            self.total = sum(i["price"] * i["qty"] for i in self.items)
        elif event.type == "OrderPaid":
            self.status = "PAID"
        elif event.type == "OrderShipped":
            self.status = "SHIPPED"
            self.tracking = event.data["tracking_number"]
        elif event.type == "OrderCancelled":
            self.status = "CANCELLED"
        self.version += 1
    
    @classmethod
    def from_events(cls, events):
        """Reconstruct aggregate from event history."""
        aggregate = cls()
        for event in events:
            aggregate.apply(event)
        return aggregate
```

**Snapshot optimization:**
```python
class SnapshotStore:
    """Periodic snapshots to avoid replaying entire event history."""
    
    def load_aggregate(self, aggregate_type, aggregate_id):
        # 1. Load latest snapshot
        snapshot = self.get_latest_snapshot(aggregate_type, aggregate_id)
        
        if snapshot:
            aggregate = deserialize(snapshot.data)
            start_version = snapshot.version + 1
        else:
            aggregate = OrderAggregate()
            start_version = 0
        
        # 2. Load events after snapshot
        events = self.event_store.load_events_after_version(
            aggregate_type, aggregate_id, start_version
        )
        
        # 3. Apply remaining events
        for event in events:
            aggregate.apply(event)
        
        # 4. Create new snapshot if too many events since last snapshot
        if aggregate.version - (snapshot.version if snapshot else 0) > 100:
            self.save_snapshot(aggregate_type, aggregate_id, 
                            aggregate.version, serialize(aggregate))
        
        return aggregate
```

**Failure Modes:**
- **Projection lag:** Materialized views are eventually consistent. During high load, projections may lag behind the event store. Consumers see stale data. Mitigation: monitor projection lag; increase projection consumer parallelism.
- **Event schema evolution:** Old events must remain deserializable. Never delete event schema versions. Use Avro schema evolution.
- **Snapshot corruption:** If a snapshot is corrupt, fall back to full replay. Slow but correct.

**Interviewer Q&As:**

**Q1: Why Kafka as an event store instead of a traditional database?**
A: Kafka provides: (1) Immutable, append-only log (natural fit for events). (2) Ordered per partition (per aggregate). (3) Built-in replication and durability. (4) Consumer groups for building multiple projections. (5) Retention policies for archival. Limitations: no query by aggregate_id (need a separate projection). For aggregate-level queries, a PostgreSQL event store with a Kafka outbox is often better.

**Q2: How do you handle CQRS consistency for a user who writes and immediately reads?**
A: Options: (1) Read-your-own-writes: after writing an event, the API returns the updated aggregate (computed locally, not from the projection). (2) Causal consistency: include a version number; if the projection is behind, wait or return stale data with a warning. (3) Synchronous projection: update the read model in the same request (defeats the purpose of async CQRS but acceptable for specific queries).

**Q3: What is the maximum number of events per aggregate before performance degrades?**
A: Without snapshots: replaying 10,000+ events per aggregate is slow (seconds). With snapshots every 100 events: replay at most 100 events from the last snapshot. Guideline: if aggregate lifetime exceeds 1,000 events, implement snapshots.

**Q4: How do you handle event versioning (upcasting)?**
A: When event schema changes: (1) Store events in their original version. (2) When loading, apply upcasters that transform old event formats to the current format. (3) Upcasters are pure functions: `upcast_v1_to_v2(event_v1) -> event_v2`. (4) Chain upcasters for multi-version jumps.

**Q5: What is the difference between event sourcing and CDC?**
A: Event sourcing: the application produces domain events intentionally (OrderCreated, OrderShipped). Events represent business intent. CDC: captures database-level changes (row inserted, column updated). CDC events are structural, not semantic. Event sourcing is a design pattern; CDC is an integration technique. They complement each other: event-sourced services produce rich events; legacy CRUD services can be integrated via CDC.

**Q6: How do you delete data in an event-sourced system (GDPR)?**
A: (1) Crypto-shredding: encrypt PII in events with a per-user key. To "delete," destroy the key. Events remain but PII is unreadable. (2) Tombstone events: produce a "UserDataDeleted" event; projections remove PII from read models. (3) Event rewriting: produce a new event stream with PII redacted (complex, breaks immutability guarantee).

---

## 7. Scheduling & Resource Management

### Flink Resource Management

| Resource | Management | Config |
|----------|-----------|--------|
| TaskManager slots | Each TaskManager has N slots; each slot runs one task (operator) | `taskmanager.numberOfTaskSlots: 4` |
| Memory | Managed memory (off-heap) for state + network buffers + JVM overhead | `taskmanager.memory.process.size: 8g` |
| CPU | One slot ≈ one CPU core (guideline) | Match slot count to CPU cores |
| State | RocksDB state backend; incremental checkpoints | `state.backend: rocksdb` |
| Checkpoints | Periodic state snapshots to S3/HDFS | `execution.checkpointing.interval: 60s` |

### Flink on Kubernetes

```yaml
apiVersion: flink.apache.org/v1beta1
kind: FlinkDeployment
metadata:
  name: order-streaming
spec:
  image: flink:1.18
  flinkVersion: v1_18
  flinkConfiguration:
    state.backend: rocksdb
    state.checkpoints.dir: s3://flink-checkpoints/order-streaming
    state.savepoints.dir: s3://flink-savepoints/order-streaming
    execution.checkpointing.interval: "60000"
    execution.checkpointing.min-pause: "30000"
    restart-strategy: fixed-delay
    restart-strategy.fixed-delay.attempts: "10"
    restart-strategy.fixed-delay.delay: "30s"
  jobManager:
    resource:
      memory: "4096m"
      cpu: 2
    replicas: 1
  taskManager:
    resource:
      memory: "8192m"
      cpu: 4
    replicas: 8
  job:
    jarURI: s3://flink-jars/order-streaming-1.0.jar
    entryClass: com.infra.streaming.OrderStreamingJob
    parallelism: 32
    upgradeMode: savepoint
```

### Kafka Connect Resource Management

```
Kafka Connect distributed mode:
  - Workers form a group (similar to consumer groups)
  - Connectors and tasks are distributed across workers
  - Rebalance on worker join/leave
  
Resource sizing:
  - Each connector task uses ~1 thread + memory for buffering
  - MySQL CDC: 1 task per connector (single binlog reader)
  - JDBC Sink: N tasks per connector (parallelism = N)
  - Elasticsearch Sink: N tasks (one per partition subset)
  
Worker sizing:
  - 4 CPU cores, 8 GB heap per worker
  - 20-30 tasks per worker (depends on task type)
  - 5-10 workers for a medium deployment
```

### Schema Registry Resource Management

```
Schema Registry sizing:
  - Stateless service (all state in _schemas Kafka topic)
  - Memory: all schemas loaded in-memory (~500 MB for 100K schemas)
  - CPU: minimal (schema lookup is an in-memory hash map)
  - 2-3 instances behind load balancer for HA
  - Primary-secondary mode: one leader handles writes, all handle reads
  
Deployment:
  - Kubernetes Deployment with 3 replicas
  - CPU: 1 core, Memory: 2 GB per instance
  - Health check: GET /subjects (returns 200 if operational)
  - Leader election via Kafka (kafkastore.topic = _schemas)
```

---

## 8. Scaling Strategy

### Stream Processing Scaling

| Dimension | Mechanism | Limit |
|-----------|----------|-------|
| Flink parallelism | Increase task slots (parallelism = number of subtasks) | Max = number of Kafka partitions for source |
| Flink state size | Incremental checkpoints, tiered state backend | Terabytes with RocksDB |
| Kafka Connect tasks | Increase task count per connector | Depends on connector type |
| Schema Registry | Add read replicas (stateless) | Practically unlimited reads |
| CDC throughput | One Debezium connector per database; shard databases | Limited by binlog throughput |

### Auto-Scaling Flink on Kubernetes

```yaml
# Flink reactive mode: auto-scales based on available TaskManagers
flinkConfiguration:
  scheduler-mode: reactive
  
# Kubernetes HPA for TaskManagers
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: flink-taskmanager-hpa
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: order-streaming-taskmanager
  minReplicas: 4
  maxReplicas: 32
  metrics:
    - type: Pods
      pods:
        metric:
          name: flink_taskmanager_job_task_busyTimeMsPerSecond
        target:
          type: AverageValue
          averageValue: "700"   # Scale up if tasks are busy > 70% of time
```

**Interviewer Q&As:**

**Q1: How do you scale a Flink job that is bottlenecked on state access?**
A: (1) Increase parallelism (spread state across more tasks). (2) Use incremental checkpoints (only checkpoint changed state). (3) Tune RocksDB: increase write buffer size, enable compression. (4) Use SSD for local state directory. (5) Consider rekeying to distribute hot keys.

**Q2: How do you handle a sudden 10x traffic spike in the streaming pipeline?**
A: (1) Kafka absorbs the spike (producers write faster than consumers can process). (2) Consumer lag increases. (3) Flink reactive mode auto-scales TaskManagers. (4) If auto-scaling is too slow: manually increase Flink parallelism via savepoint-restart. (5) For CDC: MySQL binlog continues; Debezium catches up when traffic subsides.

**Q3: What happens when you add partitions to a Kafka topic that Flink is consuming?**
A: Flink's Kafka source periodically discovers new partitions (`flink.partition-discovery.interval-millis`). New partitions are assigned to Flink subtasks. Existing state is not redistributed; new partitions start from the configured offset. If the topic change requires a parallelism increase, a savepoint-restart is needed.

**Q4: How do you scale Schema Registry writes?**
A: Schema Registry writes go through the leader only. The leader serializes writes to the `_schemas` topic. At 100K schemas, write throughput is not a concern (schemas are registered rarely, during deployments). If needed, shard by subject namespace (different Schema Registry clusters per team).

**Q5: How do you handle a Flink job with terabytes of state?**
A: (1) Incremental checkpoints (only changed SST files are uploaded). (2) Local recovery (TaskManager restarts on same node, uses local state). (3) Unaligned checkpoints (reduce back-pressure impact on checkpoint duration). (4) Tune `state.backend.rocksdb.memory.managed` and `state.backend.rocksdb.compaction.level.target-file-size-base`.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Detection | Impact | Mitigation | RTO |
|---------|-----------|--------|------------|-----|
| Schema Registry leader crash | Kafka leader election for `_schemas` | Write unavailable; reads from cache | Multiple instances; Kafka-based leader election | < 5s |
| Debezium connector crash | Kafka Connect task monitoring | CDC data stops flowing | Auto-restart by Connect framework; resume from stored offset | < 30s |
| Flink TaskManager crash | Heartbeat timeout | Job tasks fail; state lost locally | Restart task on another TM; restore from checkpoint | 30-120s |
| Flink JobManager crash | Kubernetes restart / HA failover | Job management unavailable | HA with ZK/Kubernetes; standby JM | 15-60s |
| Kafka Connect worker crash | Connect group rebalance | Tasks redistributed to surviving workers | Automatic rebalance | 10-30s |
| MySQL binlog purged | Debezium connector error | Cannot resume CDC; data gap | Trigger re-snapshot; set binlog retention > max downtime | Minutes-hours |
| Schema incompatibility deployed | Schema Registry rejects registration | Producer deployment fails (cannot serialize) | CI/CD compatibility check; BACKWARD compatibility mode | N/A (prevented) |
| Flink checkpoint failure | Checkpoint timeout/failure metric | If sustained: no recovery point; data loss risk on failure | Alert; increase checkpoint timeout; fix back-pressure | Ongoing |
| Network partition (Flink <-> Kafka) | Task errors, checkpoint failures | Job fails or degrades | Job restarts from checkpoint when network recovers | Network recovery time |

### Delivery Semantics

| Pipeline | Semantic | Mechanism |
|----------|----------|-----------|
| CDC (Debezium -> Kafka) | At-least-once | Debezium commits offset after Kafka ack; restart replays from last offset |
| Flink (Kafka -> Kafka) | Exactly-once | Checkpoint barriers + Kafka transactions (TwoPhaseCommitSinkFunction) |
| Flink (Kafka -> External DB) | At-least-once (with idempotent sink) | Checkpoint + idempotent upsert (ON DUPLICATE KEY UPDATE) |
| Kafka Streams (Kafka -> Kafka) | Exactly-once | `processing.guarantee=exactly_once_v2` (Kafka transactions) |
| Kafka Connect Sink (Kafka -> ES) | At-least-once | Connect offset commit after successful sink write |

---

## 10. Observability

### Key Metrics

| Category | Metric | Alert Threshold | Why |
|----------|--------|----------------|-----|
| CDC | `debezium.metrics.MilliSecondsBehindSource` | > 30,000 ms | CDC falling behind database changes |
| CDC | `debezium.metrics.NumberOfEventsFiltered` | Sudden spike | Possible misconfigured filter |
| Schema Registry | Request latency p99 | > 50 ms | Schema lookup slow, affecting all producers |
| Schema Registry | Schema count growth rate | > 100/day | Possible schema registration loop |
| Flink | `flink.job.uptime` | < expected | Job restarting frequently |
| Flink | `flink.taskmanager.job.task.busyTimeMsPerSecond` | > 900 (90%) | Task overloaded |
| Flink | `flink.job.lastCheckpointDuration` | > 120,000 ms | Slow checkpoints; risk of data loss |
| Flink | `flink.job.lastCheckpointSize` | > 50 GB | State growing unboundedly |
| Flink | Watermark lag | > 60,000 ms | Event-time falling behind; windows delayed |
| Kafka Connect | Connector/task status | != RUNNING | Connector/task failed |
| Kafka Connect | `kafka.connect.worker.connector-count` | < expected | Connectors lost during rebalance |
| End-to-end | Event latency (produce -> consume) | > 5,000 ms | Pipeline bottleneck |

### Distributed Tracing

```
Trace context propagation through the streaming pipeline:

Producer:
  headers["traceparent"] = "00-{traceId}-{spanId}-01"
  
Flink:
  Extract trace context from consumed record headers
  Create child span for processing
  Inject trace context into produced record headers
  
  // OpenTelemetry Flink integration
  ProcessFunction.processElement(record, ctx, out):
    span = tracer.startSpan("process-order", parent=extractContext(record))
    result = transform(record)
    injectContext(result, span.context())
    out.collect(result)
    span.end()

Sink Connector:
  Extract trace context from record headers
  Create child span for sink write
  
End-to-end trace:
  [Producer] -> [Kafka] -> [Flink Source] -> [Flink Process] -> [Flink Sink] -> [Kafka] -> [ES Connector] -> [ES]
  |--- traceId: abc123 ---|--- same traceId ---|--- same traceId ---|--- same traceId ---|
```

### Logging

```
Structured logging for all streaming components:

{
  "timestamp": "2026-04-09T10:30:00.123Z",
  "level": "WARN",
  "component": "flink-job:order-aggregation",
  "operator": "window-aggregate",
  "subtask": 7,
  "message": "Late event dropped",
  "event_time": "2026-04-09T09:15:00.000Z",
  "watermark": "2026-04-09T10:25:00.000Z",
  "lateness": "PT1H15M",
  "key": "region-us-east",
  "trace_id": "abc123"
}

Log aggregation: Application -> Flink log4j -> Filebeat -> Kafka -> Elasticsearch
Flink dashboard: real-time job graph with throughput/latency per operator
```

---

## 11. Security

### Schema Registry Security

| Layer | Mechanism | Config |
|-------|-----------|--------|
| Authentication | HTTPS + RBAC (Confluent) or API keys | `authentication.method=BEARER` |
| Authorization | Per-subject ACLs: who can register, read, delete schemas | Role-based: `DeveloperRead`, `DeveloperWrite`, `Admin` |
| Transport | TLS 1.3 | Schema Registry behind TLS-terminating LB |
| Audit | All schema registrations logged | `access.control.allow.methods=GET,POST` |

### CDC Security

```
Debezium database credentials:
  - Stored in Kubernetes Secret or HashiCorp Vault
  - Minimal privileges: REPLICATION SLAVE, REPLICATION CLIENT, SELECT on CDC tables
  - Separate database user for Debezium (auditable, revocable)
  
Debezium -> Kafka:
  - SASL/SCRAM authentication to Kafka
  - TLS for in-transit encryption
  
PII handling in CDC:
  - Debezium SMTs (Single Message Transforms) to mask/hash PII fields
  - transforms=mask
  - transforms.mask.type=org.apache.kafka.connect.transforms.MaskField$Value
  - transforms.mask.fields=ssn,credit_card
  - transforms.mask.replacement=***MASKED***
```

### Flink Security

```
Flink job isolation:
  - Each job runs in its own Kubernetes namespace
  - Network policies: Flink pods can only reach Kafka brokers and state backend
  - RBAC: only CI/CD can deploy jobs; operators can view status
  
Flink secrets:
  - Kafka credentials, S3 credentials mounted as Kubernetes Secrets
  - Never in Flink configuration files (stored in HDFS/S3 alongside jars)
```

---

## 12. Incremental Rollout Strategy

### Phase 1: Schema Registry (Week 1-2)
- Deploy Schema Registry cluster.
- Register existing schemas (reverse-engineer from JSON/Protobuf).
- Enable Schema Registry for one non-critical topic (producer + consumer).
- Validate serialization/deserialization roundtrip.

### Phase 2: CDC Pipeline (Week 3-5)
- Deploy Debezium for one non-critical MySQL table.
- Validate: every DB change appears in Kafka topic.
- Measure CDC lag (< 500 ms target).
- Add tables incrementally.

### Phase 3: Stream Processing (Week 6-8)
- Deploy Flink cluster.
- Implement one simple streaming job (filter + transform).
- Validate exactly-once semantics with chaos testing.
- Add windowed aggregation jobs.

### Phase 4: Full Pipeline (Week 9-12)
- Connect CDC -> Flink -> materialized views.
- Deploy sink connectors (Elasticsearch, S3).
- Enable all production CDC tables.
- Performance test at 2x expected load.

### Phase 5: Migration (Week 13-16)
- Migrate existing batch ETL to streaming.
- Decommission polling-based integrations.
- Document operational runbooks.

**Rollout Q&As:**

**Q1: How do you roll back a Flink job that produces incorrect results?**
A: (1) Stop the job with savepoint. (2) Identify the last correct offset. (3) Reset the output topic consumer offsets. (4) If the output is a database: restore from backup or use compensating transactions. (5) Fix the bug, deploy from savepoint with corrected logic.

**Q2: How do you validate CDC correctness?**
A: (1) Count comparison: `SELECT COUNT(*) FROM table` vs Kafka topic message count. (2) Hash comparison: hash of all rows vs hash of latest values in compacted topic. (3) Spot check: random sample of records compared field-by-field. (4) Run validation continuously for 48 hours before declaring CDC production-ready.

**Q3: How do you handle the Debezium initial snapshot for a 1TB database?**
A: (1) Schedule during low-traffic window. (2) Use `snapshot.fetch.size=10000` to limit memory. (3) Monitor MySQL replication lag (snapshot queries compete with production traffic). (4) Estimated time: 1TB / 100 MB/s read throughput = ~3 hours. (5) After snapshot, streaming mode catches up from the captured binlog position.

**Q4: How do you test schema compatibility in CI/CD?**
A: (1) Schema files in git (version-controlled). (2) CI pipeline: `POST /compatibility/subjects/{subject}/versions/latest` with new schema. (3) If incompatible, CI fails with clear error message. (4) PR review includes schema diff. (5) Schema changes require sign-off from data platform team.

**Q5: How do you handle a corrupt Flink checkpoint?**
A: (1) Flink automatically retries from the previous checkpoint if the latest is corrupt. (2) Keep multiple checkpoints: `state.checkpoints.num-retained: 3`. (3) If all checkpoints are corrupt: start from savepoint (if available) or restart job from Kafka offsets (losing Flink state; stateful operations recompute from scratch).

---

## 13. Trade-offs & Decision Log

| Decision | Options | Chosen | Rationale |
|----------|---------|--------|-----------|
| Schema format | JSON, Avro, Protobuf | Avro | Best schema evolution, compact binary, native Kafka ecosystem support |
| Schema management | Embedded, Header, Registry | Schema Registry | Centralized compatibility enforcement; 5-byte message overhead |
| Compatibility mode | BACKWARD, FORWARD, FULL, NONE | BACKWARD (default), FULL for critical topics | BACKWARD allows adding fields safely; FULL for payment/billing |
| CDC approach | Polling, Triggers, Log-based | Log-based (Debezium) | Complete, ordered, minimal DB impact |
| Stream processor | Kafka Streams, Flink, Spark | Flink (primary) + Kafka Streams (lightweight) | Flink for complex processing; Kafka Streams for in-app transforms |
| Flink state backend | Memory, FileSystem, RocksDB | RocksDB | Supports state larger than memory; incremental checkpoints |
| Event store | Kafka, PostgreSQL, EventStoreDB | Kafka (streaming) + PostgreSQL (querying) | Kafka for streaming consumers; PostgreSQL for aggregate queries |
| Checkpoint storage | HDFS, S3, GCS | S3 | Durable, cost-effective, no HDFS cluster to manage |
| Flink deployment | Standalone, YARN, Kubernetes | Kubernetes (Flink Operator) | Consistent with platform strategy; auto-scaling, self-healing |

---

## 14. Agentic AI Integration

### AI-Powered Schema Evolution

```python
class SchemaEvolutionAgent:
    """AI agent that manages schema evolution across the streaming platform."""
    
    def analyze_schema_change(self, subject, proposed_schema):
        """Analyze impact of schema change across all consumers."""
        
        # 1. Get all consumers of this subject's topic
        consumers = self.get_topic_consumers(subject)
        
        # 2. Analyze compatibility with each consumer's reader schema
        impact = self.model.analyze(
            prompt=f"""Analyze the impact of this schema change:
            Subject: {subject}
            Current schema: {self.get_current_schema(subject)}
            Proposed schema: {proposed_schema}
            Consumers: {consumers}
            
            For each consumer, determine:
            1. Will deserialization break?
            2. Will any field semantics change?
            3. Is migration needed?
            4. Recommended rollout order (producers first or consumers first)?
            """
        )
        
        return impact
    
    def auto_generate_migration(self, subject, old_schema, new_schema):
        """Generate Flink migration job to transform old events to new schema."""
        migration_code = self.model.generate(
            prompt=f"""Generate a Flink SQL migration job that:
            1. Reads from topic with old schema: {old_schema}
            2. Transforms records to new schema: {new_schema}
            3. Writes to a new topic with the new schema
            4. Handles all edge cases (null fields, type conversions)
            """
        )
        return migration_code
```

### AI-Driven Pipeline Monitoring

```python
class StreamingPipelineAgent:
    """AI agent that monitors and auto-tunes streaming pipelines."""
    
    def diagnose_latency_spike(self, pipeline_id):
        """Diagnose root cause of latency increase."""
        
        metrics = self.collect_metrics(pipeline_id)
        # Collect: Flink task busyness, checkpoint duration, 
        # Kafka consumer lag, CDC lag, back-pressure indicators
        
        diagnosis = self.model.reason(
            prompt=f"""Diagnose latency spike in streaming pipeline:
            Pipeline: {pipeline_id}
            Metrics: {metrics}
            
            Possible causes:
            1. Kafka partition skew (hot partition)
            2. Flink back-pressure (slow operator)
            3. State backend bottleneck (large state)
            4. GC pauses
            5. Network saturation
            6. Upstream traffic spike
            
            Provide root cause analysis and recommended fix.
            """
        )
        
        if diagnosis.confidence > 0.85:
            self.apply_fix(pipeline_id, diagnosis.recommended_fix)
        else:
            self.escalate_to_oncall(pipeline_id, diagnosis)
    
    def auto_tune_flink_job(self, job_id):
        """AI-driven auto-tuning of Flink job parameters."""
        
        history = self.get_job_metrics_history(job_id, days=7)
        
        recommendations = self.model.analyze(
            prompt=f"""Recommend Flink job configuration changes:
            Job: {job_id}
            Current config: {self.get_job_config(job_id)}
            7-day metrics: {history}
            
            Tune: parallelism, checkpoint interval, RocksDB settings,
                   watermark strategy, window parameters.
            Explain trade-offs for each recommendation.
            """
        )
        
        return recommendations
```

### Intelligent Event Routing and Processing

```
Agentic AI for streaming pipeline orchestration:

1. Auto-detect event patterns:
   - Agent analyzes event streams for patterns, anomalies, correlations
   - Suggests new stream processing jobs based on discovered patterns
   - Example: "Order cancellation rate spiked 3x for region=EU. 
     Recommend: create alert pipeline for cancellation anomaly detection."

2. Self-healing CDC pipelines:
   - Agent detects Debezium connector failure
   - Diagnoses cause: binlog position lost, schema change, network issue
   - Applies fix: re-snapshot, update schema history, restart connector
   - Verifies data integrity post-recovery

3. Dynamic stream routing:
   - Based on event content and system load, route events to different
     processing pipelines
   - High-priority events: low-latency pipeline with minimal windowing
   - Bulk events: batched pipeline with heavy aggregation
   - Agent learns routing rules from historical patterns
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: How would you design an event streaming platform from scratch?**
A: Start with the core components: (1) Kafka for the event backbone (durable, replayable log). (2) Schema Registry for contract enforcement between producers and consumers. (3) CDC (Debezium) for integrating existing databases. (4) Stream processor (Flink) for transformations and aggregations. (5) Sink connectors for materialized views. Key design principles: schema-first development, at-least-once by default with exactly-once where needed, event-time processing, and full observability.

**Q2: What is the difference between event streaming and message queuing?**
A: Message queuing (RabbitMQ, SQS): messages are consumed and deleted; one consumer per message; transient. Event streaming (Kafka): events are retained; multiple consumers read independently; replayable; ordered within partition. Streaming is a superset: you can implement queue semantics with streaming (single consumer group), but not vice versa.

**Q3: How do you ensure exactly-once processing across the entire pipeline?**
A: End-to-end exactly-once requires: (1) Idempotent producer (Kafka dedup via sequence numbers). (2) Transactional writes (Kafka transactions for produce + offset commit atomicity). (3) Exactly-once stream processing (Flink checkpoint barriers). (4) Idempotent sink (upsert to database, or dedup in consumer). Any break in this chain degrades to at-least-once.

**Q4: How do you handle schema evolution when 200 services depend on a topic?**
A: (1) Use BACKWARD_TRANSITIVE compatibility: any new schema must be readable by ALL previous consumer versions. (2) CI/CD enforces compatibility check before schema registration. (3) For breaking changes: create a new topic, run a Flink migration job, migrate consumers incrementally, then decommission the old topic.

**Q5: What is Change Data Capture and when would you use it?**
A: CDC captures row-level changes from a database's transaction log (binlog/WAL) and streams them to Kafka. Use cases: (1) Sync data between services without coupling (service A writes to its DB; service B consumes CDC events). (2) Build search indices (MySQL CDC -> Kafka -> Elasticsearch). (3) Event-driven architecture for legacy CRUD applications. (4) Transactional outbox pattern (write to outbox table; Debezium publishes to Kafka).

**Q6: How would you debug a Flink job that produces incorrect aggregation results?**
A: (1) Check watermark progression: if watermark is too aggressive, late events are dropped. (2) Check window definition: tumbling vs sliding, event-time vs processing-time. (3) Check allowed lateness: events arriving after window close + allowed lateness are discarded. (4) Check state: are state snapshots consistent? (5) Replay events through a test Flink job with detailed logging per record.

**Q7: What is the transactional outbox pattern?**
A: Instead of writing to DB and Kafka separately (which can fail atomically), write to the DB within a transaction: business table + outbox table. Then: (1) Polling publisher reads outbox, publishes to Kafka, marks as published. OR (2) Debezium CDC reads the outbox table's binlog entries and publishes to Kafka automatically. This guarantees at-least-once delivery without 2PC.

**Q8: How do you handle event replay for a bug fix?**
A: (1) Fix the Flink job code. (2) Deploy with a savepoint that resets to the desired Kafka offset. (3) Clear the output (truncate output topic or drop/recreate materialized view). (4) Replay from the reset offset. (5) Monitor replay progress (consumer lag). Concern: replay produces duplicate outputs to external sinks. Solution: idempotent sinks.

**Q9: What are the trade-offs of event sourcing?**
A: Pros: (1) Complete audit trail. (2) Temporal queries (state at any point in time). (3) Event replay for debugging and recovery. (4) Natural fit for event-driven architectures. Cons: (1) Complexity (CQRS adds a read model layer). (2) Eventual consistency (read model lags behind write model). (3) Event schema evolution is harder than table migration. (4) Storage growth (all events retained).

**Q10: How do you size a Flink cluster for a given workload?**
A: (1) Estimate input throughput (MB/s). (2) Benchmark single-task throughput for your specific operators. (3) Parallelism = input throughput / single-task throughput. (4) TaskManagers = parallelism / slots per TM. (5) Memory per TM: state size / parallelism + network buffers + JVM overhead. (6) Add 50% headroom for traffic spikes. Example: 500 MB/s input, 50 MB/s per task = 10 parallelism. 4 slots per TM = 3 TMs (rounded up).

**Q11: What is the difference between Kafka Streams and KSQL/ksqlDB?**
A: Kafka Streams is a Java library embedded in your application. ksqlDB is a SQL engine built on Kafka Streams that provides a SQL interface for stream processing. ksqlDB is easier for simple queries (CREATE STREAM, SELECT ... GROUP BY), but Kafka Streams offers more flexibility for complex logic. ksqlDB is good for prototyping and simple use cases; Kafka Streams for production-grade custom processing.

**Q12: How do you handle a partitioned Flink job where one partition has much more data?**
A: Data skew means one Flink subtask processes disproportionately more data. Solutions: (1) Repartition with a different key. (2) Pre-aggregate locally before global aggregation (like MapReduce combiner). (3) Split hot keys: append a random suffix, aggregate per-suffix, then merge. (4) Flink's `rebalance()` or `rescale()` for round-robin distribution (loses key-based ordering).

**Q13: What is a watermark in stream processing?**
A: A watermark is a timestamp assertion: "No events with timestamp <= W will arrive." It allows the stream processor to close windows and emit results. Generated at the source (e.g., `currentTimestamp - maxOutOfOrderness`). Advances monotonically. If watermark is too aggressive (small out-of-orderness), late events are dropped. If too conservative, windows close late, increasing latency.

**Q14: How do you implement join between two Kafka topics in Flink?**
A: (1) Key both streams by the join key. (2) Use Flink's interval join for bounded time difference: `stream1.keyBy(key).intervalJoin(stream2.keyBy(key)).between(Time.minutes(-5), Time.minutes(5))`. (3) For unbounded join (latest value from each side): use Flink's CoProcessFunction with state. (4) Window join: both streams windowed, join within each window.

**Q15: How do you prevent a Debezium initial snapshot from impacting production database performance?**
A: (1) Run snapshot against a read replica, not the primary. (2) Use `snapshot.locking.mode=none` to avoid global locks (risk: slightly inconsistent snapshot boundaries). (3) Set `snapshot.fetch.size=10000` to limit row fetch batch size. (4) Schedule during off-peak hours. (5) Monitor MySQL replication lag during snapshot.

**Q16: What happens if a Flink checkpoint takes longer than the checkpoint interval?**
A: Flink will not start a new checkpoint until the previous one completes (or times out). If checkpoints consistently take longer than the interval, the effective checkpoint interval grows, increasing the amount of data that needs to be replayed on failure. Solution: increase checkpoint interval, reduce state size, use incremental checkpoints, or increase checkpoint parallelism.

---

## 16. References

| Resource | URL / Reference |
|----------|----------------|
| Confluent Schema Registry | https://docs.confluent.io/platform/current/schema-registry/ |
| Apache Flink Documentation | https://flink.apache.org/docs/ |
| Debezium Documentation | https://debezium.io/documentation/ |
| Kafka Streams Documentation | https://kafka.apache.org/documentation/streams/ |
| Martin Kleppmann - "Designing Data-Intensive Applications" | O'Reilly, Chapters 11-12 |
| Event Sourcing Pattern | https://martinfowler.com/eaaDev/EventSourcing.html |
| CQRS Pattern | https://martinfowler.com/bliki/CQRS.html |
| Flink Exactly-Once (Ververica) | https://www.ververica.com/blog/high-throughput-low-latency-and-exactly-once-stream-processing-with-apache-flink |
| CDC with Debezium (Red Hat) | https://debezium.io/blog/ |
| Kafka Connect Documentation | https://kafka.apache.org/documentation/#connect |
| Avro Specification | https://avro.apache.org/docs/current/spec.html |
