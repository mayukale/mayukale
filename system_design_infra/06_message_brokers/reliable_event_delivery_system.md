# System Design: Reliable Event Delivery System

> **Relevance to role:** A cloud infrastructure platform engineer must guarantee that critical events -- bare-metal provisioning state transitions, Kubernetes resource mutations, billing charges, job scheduler completions, OpenStack service orchestration commands -- are delivered exactly once even across distributed service boundaries. This design addresses the fundamental problem: how do you atomically write to a database AND publish to a message broker without 2PC, while achieving at-least-once or exactly-once semantics end-to-end?

---

## 1. Requirement Clarifications

### Functional Requirements
| ID | Requirement |
|----|-------------|
| FR-1 | Guarantee **at-least-once** event delivery from producer service to all consumers |
| FR-2 | Support **exactly-once** processing semantics via idempotent consumers |
| FR-3 | Implement **transactional outbox pattern**: atomically persist business state + outbox event |
| FR-4 | Support two outbox publishing strategies: **polling publisher** and **CDC-based** (Debezium) |
| FR-5 | Implement **idempotency keys** for consumer-side deduplication |
| FR-6 | Support **saga orchestration and choreography** for distributed transactions |
| FR-7 | Track delivery status per message: produced, delivered, processed, failed |
| FR-8 | Support event replay from any point in time |
| FR-9 | Handle partial failures: service A succeeds, service B fails |

### Non-Functional Requirements
| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Outbox -> Kafka publish latency | < 500 ms p99 (polling), < 200 ms p99 (CDC) |
| NFR-2 | End-to-end delivery latency (DB write -> consumer processing) | < 2 seconds p99 |
| NFR-3 | Exactly-once processing guarantee | 100% (with idempotent consumers) |
| NFR-4 | Throughput | 100,000 events/sec |
| NFR-5 | No event loss | Even during service crashes, network partitions, DB failovers |
| NFR-6 | Event ordering | Per aggregate (same entity key) |
| NFR-7 | Availability | 99.99% for event publishing path |

### Constraints & Assumptions
- Source databases: MySQL 8.0 (InnoDB) and PostgreSQL 14.
- Message broker: Apache Kafka (existing cluster).
- Services: Java 17 + Spring Boot (primary), Python FastAPI (secondary).
- Idempotency store: Redis Cluster.
- Saga orchestrator: custom implementation on Kafka.
- Business domains: provisioning, billing, inventory, scheduling.
- Average event payload: 1 KB. Max: 100 KB.

### Out of Scope
- Multi-region event replication (Kafka MirrorMaker).
- Schema management (covered in event_streaming_platform.md).
- DLQ management (covered in dead_letter_queue_system.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|-------------|--------|
| Business transactions/sec | 100,000 | -- |
| Events per transaction (avg) | 1.5 (some transactions produce multiple events) | -- |
| Total events/sec | 100,000 x 1.5 | **150,000 events/sec** |
| Outbox rows/sec (write) | 150,000 | -- |
| Outbox rows/sec (read + delete after publish) | 150,000 | -- |
| Kafka produce/sec | 150,000 | -- |
| Consumer groups | 20 (downstream services) | -- |
| Total consume/sec | 150,000 x 20 | **3M consume operations/sec** |
| Idempotency checks (Redis) | 3M checks/sec | -- |
| Saga instances active | 10,000 concurrent | -- |

### Latency Requirements

| Path | Target | Mechanism |
|------|--------|-----------|
| Business write + outbox insert | < 10 ms | Same DB transaction |
| Outbox -> Kafka (polling) | < 500 ms | Poll interval (100ms) + Kafka produce |
| Outbox -> Kafka (CDC) | < 200 ms | Debezium binlog latency |
| Kafka -> Consumer | < 50 ms | Consumer poll |
| Consumer idempotency check (Redis) | < 1 ms | Redis GET |
| End-to-end (DB write -> consumer processed) | < 2 s | Sum of above |
| Saga completion (3-step) | < 10 s | Sequential service calls |

### Storage Estimates

| Parameter | Calculation | Result |
|-----------|-------------|--------|
| Outbox table (MySQL) | 150K rows/sec x 1 KB x avg 2 sec retention | **300 MB** (constantly rotating) |
| Kafka events (7-day retention) | 150K/sec x 1 KB x 86,400 x 7 x RF=3 | **27.2 TB** |
| Idempotency keys (Redis) | 3M unique message IDs x 100 bytes x 24h retention | **25.9 GB** |
| Saga state (MySQL) | 10K active x 5 KB | **50 MB** |
| Delivery tracking (MySQL) | 150K events/sec x 200 bytes x 24h | **2.59 TB/day** (track only failures) |

### Bandwidth Estimates

| Path | Calculation | Result |
|------|-------------|--------|
| Outbox -> Kafka (produce) | 150K/sec x 1 KB | 150 MB/s |
| Kafka replication | 150 MB/s x 2 followers | 300 MB/s |
| Kafka -> Consumers (20 groups) | 150 MB/s x 20 | 3 GB/s |
| Redis idempotency checks | 3M/sec x 100 bytes | 300 MB/s |

---

## 3. High Level Architecture

```
+----------------------------------+
| Producer Service                  |
| (e.g., Provisioning Service)     |
|                                  |
| BEGIN TRANSACTION                |
|   INSERT INTO orders (...)       |
|   INSERT INTO outbox (...)       |
| COMMIT                           |
+--+-------------------------------+
   |
   | (Two outbox publishing strategies)
   |
   +----> Strategy A: Polling Publisher
   |      +-----------------------------+
   |      | Outbox Poller Service        |
   |      | (scheduled, every 100ms)     |
   |      |                              |
   |      | SELECT * FROM outbox         |
   |      | WHERE status = 'PENDING'     |
   |      | ORDER BY created_at          |
   |      | LIMIT 1000                   |
   |      |                              |
   |      | -> Kafka produce (batch)     |
   |      | -> UPDATE outbox             |
   |      |    SET status = 'PUBLISHED'  |
   |      +-----------------------------+
   |
   +----> Strategy B: CDC (Debezium)
          +-----------------------------+
          | Debezium Connector           |
          | (reads MySQL binlog)         |
          |                              |
          | binlog event: INSERT outbox  |
          | -> Kafka produce             |
          | (topic = outbox.events)      |
          +-----------------------------+
   
   Both strategies produce to:
   +-----------------------------+
   | Kafka                       |
   | Topic: provisioning.events  |
   +---+-----+-----+-----+------+
       |     |     |     |
       v     v     v     v
+------+--+ ++----++ +---+----+ +--------+
|Consumer | |Consumer| |Consumer | |Consumer |
|Service A| |Service B| |Service C| |Service D|
|         | |         | |         | |         |
|Idempotent| |Idempotent| |Idempotent| |Idempotent|
|Consumer  | |Consumer  | |Consumer  | |Consumer  |
|         | |         | |         | |         |
|Redis    | |Redis    | |Redis    | |Redis    |
|dedup    | |dedup    | |dedup    | |dedup    |
+----------+ +---------+ +---------+ +---------+

Saga Orchestration (for multi-service transactions):
+---------------------------------------------------+
| Saga Orchestrator                                  |
|                                                   |
| Step 1: Provision Server  -> provisioning.commands |
| Step 2: Allocate Network  -> network.commands      |
| Step 3: Configure DNS     -> dns.commands          |
|                                                   |
| Each step: command event -> service processes ->   |
|            reply event -> orchestrator continues   |
|                                                   |
| On failure: compensating transactions              |
|   Step 3 failed -> undo Step 2 -> undo Step 1     |
+---------------------------------------------------+
```

### Component Roles

| Component | Role |
|-----------|------|
| **Producer Service** | Executes business logic; writes to DB + outbox in single transaction |
| **Outbox Table** | Staging area for events; part of the service's DB; transactionally consistent |
| **Polling Publisher** | Reads outbox table, publishes to Kafka, marks as published |
| **Debezium (CDC Publisher)** | Reads DB binlog, publishes outbox inserts to Kafka |
| **Kafka** | Durable, ordered event store; decouples producers from consumers |
| **Idempotent Consumer** | Processes events with dedup (Redis SET NX on message ID) |
| **Saga Orchestrator** | Coordinates multi-service transactions via command/reply events |
| **Redis Cluster** | Stores idempotency keys for consumer dedup; fast SET NX operations |
| **Delivery Tracker** | Records per-event delivery status for observability |

### Data Flows

**Transactional Outbox Flow (Polling):**
1. Producer service: `BEGIN; INSERT INTO orders(...); INSERT INTO outbox(...); COMMIT;`
2. Polling publisher (every 100ms): `SELECT FROM outbox WHERE status='PENDING' LIMIT 1000`
3. Publisher: batch Kafka produce for all pending events.
4. On Kafka ack: `UPDATE outbox SET status='PUBLISHED' WHERE id IN (...)`
5. Cleanup job (hourly): `DELETE FROM outbox WHERE status='PUBLISHED' AND created_at < NOW() - INTERVAL 1 HOUR`
6. Consumer: poll Kafka, check idempotency, process, commit offset.

**Transactional Outbox Flow (CDC):**
1. Producer service: `BEGIN; INSERT INTO orders(...); INSERT INTO outbox(...); COMMIT;`
2. MySQL writes binlog entry for the outbox INSERT.
3. Debezium reads binlog, produces to `outbox.events` Kafka topic.
4. Router service: reads `outbox.events`, routes to appropriate domain topic based on event type header.
5. Consumer: poll domain topic, check idempotency, process, commit offset.

---

## 4. Data Model

### Core Entities & Schema

**Outbox Table (MySQL):**
```sql
CREATE TABLE outbox (
    id                  BIGINT AUTO_INCREMENT PRIMARY KEY,
    aggregate_type      VARCHAR(100) NOT NULL,         -- e.g., "Order", "Server"
    aggregate_id        VARCHAR(255) NOT NULL,         -- e.g., "order-12345", "srv-42"
    event_type          VARCHAR(100) NOT NULL,         -- e.g., "OrderCreated", "ServerProvisioned"
    event_id            CHAR(36) NOT NULL UNIQUE,      -- UUIDv7 (idempotency key)
    payload             JSON NOT NULL,                 -- serialized event data
    metadata            JSON NULL,                     -- correlation_id, causation_id, user_id
    topic               VARCHAR(255) NOT NULL,         -- target Kafka topic
    partition_key       VARCHAR(255) NULL,             -- Kafka partition key (default: aggregate_id)
    status              ENUM('PENDING', 'PUBLISHED', 'FAILED') DEFAULT 'PENDING',
    retry_count         INT DEFAULT 0,
    created_at          TIMESTAMP(6) DEFAULT CURRENT_TIMESTAMP(6),  -- microsecond precision
    published_at        TIMESTAMP(6) NULL,
    
    INDEX idx_status_created (status, created_at),     -- Polling publisher query
    INDEX idx_aggregate (aggregate_type, aggregate_id), -- Debug: find events by aggregate
    INDEX idx_event_id (event_id)                      -- Dedup check
) ENGINE=InnoDB;
```

**Idempotency Record (Redis):**
```
Key:    idempotency:{consumer_group}:{event_id}
Value:  {
            "status": "PROCESSING|COMPLETED|FAILED",
            "processed_at": "2026-04-09T10:00:00Z",
            "result_hash": "sha256..."
        }
TTL:    24 hours (configurable, must exceed max Kafka retention for replay safety)
```

**Saga State (MySQL):**
```sql
CREATE TABLE saga_instances (
    saga_id             CHAR(36) PRIMARY KEY,
    saga_type           VARCHAR(100) NOT NULL,          -- e.g., "ServerProvisioningSaga"
    current_step        INT NOT NULL DEFAULT 0,
    status              ENUM('RUNNING', 'COMPLETED', 'COMPENSATING', 'FAILED') DEFAULT 'RUNNING',
    payload             JSON NOT NULL,                  -- saga input data
    step_results        JSON NULL,                      -- results from each step
    created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    expires_at          TIMESTAMP NOT NULL,             -- saga timeout
    
    INDEX idx_status (status),
    INDEX idx_expires (expires_at)
);

CREATE TABLE saga_steps (
    id                  BIGINT AUTO_INCREMENT PRIMARY KEY,
    saga_id             CHAR(36) NOT NULL,
    step_index          INT NOT NULL,
    step_name           VARCHAR(100) NOT NULL,          -- e.g., "ProvisionServer"
    command_event_id    CHAR(36) NULL,                  -- event_id of command sent
    reply_event_id      CHAR(36) NULL,                  -- event_id of reply received
    status              ENUM('PENDING', 'SENT', 'COMPLETED', 'FAILED', 'COMPENSATED') DEFAULT 'PENDING',
    error_message       TEXT NULL,
    created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    completed_at        TIMESTAMP NULL,
    
    UNIQUE INDEX idx_saga_step (saga_id, step_index),
    FOREIGN KEY (saga_id) REFERENCES saga_instances(saga_id)
);
```

**Delivery Tracking (for observability):**
```sql
CREATE TABLE delivery_log (
    id                  BIGINT AUTO_INCREMENT PRIMARY KEY,
    event_id            CHAR(36) NOT NULL,
    consumer_group      VARCHAR(100) NOT NULL,
    status              ENUM('DELIVERED', 'PROCESSED', 'FAILED', 'DUPLICATE') NOT NULL,
    processed_at        TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    processing_time_ms  INT NULL,
    error_message       TEXT NULL,
    
    INDEX idx_event_consumer (event_id, consumer_group),
    INDEX idx_status_time (status, processed_at)
) ENGINE=InnoDB
PARTITION BY RANGE (TO_DAYS(processed_at)) (
    PARTITION p_today VALUES LESS THAN (TO_DAYS(CURRENT_DATE + INTERVAL 1 DAY)),
    PARTITION p_yesterday VALUES LESS THAN (TO_DAYS(CURRENT_DATE))
    -- rolling 7-day partitions, oldest dropped daily
);
```

### Database Selection

| Data | Storage | Rationale |
|------|---------|-----------|
| Outbox events | Same MySQL as business data | Single-DB transaction guarantees atomicity |
| Published events | Kafka | Durable, ordered, high-throughput delivery |
| Idempotency keys | Redis Cluster | Sub-ms SET NX for dedup; auto-expiring TTL |
| Saga state | MySQL (dedicated DB or same as orchestrator) | Transactional state management |
| Delivery log | MySQL (partitioned, time-series) | Queryable delivery status; auto-partition pruning |

### Indexing Strategy

| Store | Index | Purpose |
|-------|-------|---------|
| Outbox | `(status, created_at)` | Polling publisher: find pending events in order |
| Outbox | `(aggregate_type, aggregate_id)` | Debug: find all events for an entity |
| Saga | `(status)` | Find running/failed sagas |
| Saga | `(expires_at)` | Timeout expired sagas |
| Redis | Key: `idempotency:{group}:{event_id}` | O(1) dedup lookup |
| Delivery log | `(event_id, consumer_group)` | Query delivery status per event |

---

## 5. API Design

### Producer/Consumer APIs

**Producer API (Transactional Outbox - Java/Spring):**
```java
@Service
public class OrderService {
    
    private final OrderRepository orderRepo;
    private final OutboxRepository outboxRepo;
    
    @Transactional  // Single DB transaction
    public Order createOrder(CreateOrderRequest request) {
        // 1. Business logic
        Order order = new Order(request);
        order.validate();
        orderRepo.save(order);
        
        // 2. Outbox event (same transaction)
        OutboxEvent event = OutboxEvent.builder()
            .eventId(UUIDv7.generate().toString())
            .aggregateType("Order")
            .aggregateId(order.getId())
            .eventType("OrderCreated")
            .topic("provisioning.events")
            .partitionKey(order.getId())
            .payload(serialize(new OrderCreatedEvent(
                order.getId(), order.getCustomerId(), 
                order.getItems(), order.getTotal())))
            .metadata(Map.of(
                "correlation_id", MDC.get("correlationId"),
                "causation_id", MDC.get("causationId"),
                "user_id", SecurityContext.getUserId()
            ))
            .build();
        
        outboxRepo.save(event);
        
        return order;
    }
}

// OutboxRepository
@Repository
public interface OutboxRepository extends JpaRepository<OutboxEvent, Long> {
    
    @Query("SELECT e FROM OutboxEvent e WHERE e.status = 'PENDING' ORDER BY e.createdAt LIMIT :limit")
    List<OutboxEvent> findPendingEvents(@Param("limit") int limit);
    
    @Modifying
    @Query("UPDATE OutboxEvent e SET e.status = 'PUBLISHED', e.publishedAt = CURRENT_TIMESTAMP WHERE e.id IN :ids")
    void markPublished(@Param("ids") List<Long> ids);
    
    @Modifying
    @Query("DELETE FROM OutboxEvent e WHERE e.status = 'PUBLISHED' AND e.createdAt < :cutoff")
    void cleanupPublished(@Param("cutoff") Instant cutoff);
}
```

**Polling Publisher (Java):**
```java
@Service
public class OutboxPollingPublisher {
    
    private final OutboxRepository outboxRepo;
    private final KafkaTemplate<String, byte[]> kafkaTemplate;
    
    @Scheduled(fixedDelay = 100)  // Every 100ms
    @Transactional
    public void publishPendingEvents() {
        List<OutboxEvent> events = outboxRepo.findPendingEvents(1000);
        
        if (events.isEmpty()) return;
        
        List<CompletableFuture<SendResult<String, byte[]>>> futures = new ArrayList<>();
        
        for (OutboxEvent event : events) {
            ProducerRecord<String, byte[]> record = new ProducerRecord<>(
                event.getTopic(),
                null,                                       // partition (auto from key)
                event.getPartitionKey(),                    // key
                event.getPayload().getBytes()               // value
            );
            
            // Add metadata headers
            record.headers().add("event_id", event.getEventId().getBytes());
            record.headers().add("event_type", event.getEventType().getBytes());
            record.headers().add("aggregate_type", event.getAggregateType().getBytes());
            record.headers().add("aggregate_id", event.getAggregateId().getBytes());
            record.headers().add("timestamp", String.valueOf(
                event.getCreatedAt().toEpochMilli()).getBytes());
            
            if (event.getMetadata() != null) {
                event.getMetadata().forEach((k, v) -> 
                    record.headers().add("meta." + k, v.getBytes()));
            }
            
            futures.add(kafkaTemplate.send(record).toCompletableFuture());
        }
        
        // Wait for all Kafka acks
        try {
            CompletableFuture.allOf(futures.toArray(new CompletableFuture[0]))
                .get(10, TimeUnit.SECONDS);
            
            // All published successfully: mark in DB
            List<Long> ids = events.stream().map(OutboxEvent::getId).toList();
            outboxRepo.markPublished(ids);
            
        } catch (Exception e) {
            logger.error("Failed to publish outbox events", e);
            // Events remain PENDING; will be retried on next poll
        }
    }
    
    @Scheduled(cron = "0 0 * * * *")  // Every hour
    public void cleanupPublishedEvents() {
        outboxRepo.cleanupPublished(Instant.now().minus(Duration.ofHours(1)));
    }
}
```

**Idempotent Consumer (Java):**
```java
@Service
public class IdempotentEventConsumer {
    
    private final RedisTemplate<String, String> redis;
    private final EventHandler eventHandler;
    
    @KafkaListener(topics = "provisioning.events", groupId = "billing-service")
    public void consume(ConsumerRecord<String, byte[]> record, Acknowledgment ack) {
        String eventId = extractHeader(record, "event_id");
        String eventType = extractHeader(record, "event_type");
        String consumerGroup = "billing-service";
        
        // 1. Idempotency check: SET NX (set if not exists)
        String idempotencyKey = String.format("idempotency:%s:%s", consumerGroup, eventId);
        Boolean isNew = redis.opsForValue().setIfAbsent(
            idempotencyKey, 
            "PROCESSING",
            Duration.ofHours(24)  // TTL
        );
        
        if (Boolean.FALSE.equals(isNew)) {
            // Already processed or being processed
            String status = redis.opsForValue().get(idempotencyKey);
            if ("COMPLETED".equals(status)) {
                logger.info("Duplicate event skipped: eventId={}", eventId);
                ack.acknowledge();
                return;
            }
            if ("PROCESSING".equals(status)) {
                throw new RetryableException("Event being processed by another instance");
            }
        }
        
        // 2. Process the event
        try {
            eventHandler.handle(eventType, record.value());
            
            // 3. Mark as completed
            redis.opsForValue().set(idempotencyKey, "COMPLETED", Duration.ofHours(24));
            ack.acknowledge();
            
        } catch (Exception e) {
            // 4. Mark as failed (allow retry)
            redis.delete(idempotencyKey);
            throw e;  // Error handler will retry or DLQ
        }
    }
}
```

**Saga Orchestrator API:**
```java
@Service
public class ServerProvisioningSagaOrchestrator {
    
    private final SagaRepository sagaRepo;
    private final OutboxRepository outboxRepo;
    
    @Transactional
    public String startSaga(ProvisionServerRequest request) {
        SagaInstance saga = SagaInstance.builder()
            .sagaId(UUID.randomUUID().toString())
            .sagaType("ServerProvisioningSaga")
            .status(SagaStatus.RUNNING)
            .payload(serialize(request))
            .expiresAt(Instant.now().plus(Duration.ofMinutes(30)))
            .build();
        sagaRepo.save(saga);
        
        executeStep(saga, 0, "ProvisionServer", 
            "provisioning.commands",
            new ProvisionServerCommand(request.getServerId(), request.getSpec()));
        
        return saga.getSagaId();
    }
    
    @KafkaListener(topics = "saga.replies", groupId = "saga-orchestrator")
    @Transactional
    public void handleReply(ConsumerRecord<String, byte[]> record, Acknowledgment ack) {
        String sagaId = extractHeader(record, "saga_id");
        String stepName = extractHeader(record, "step_name");
        boolean success = Boolean.parseBoolean(extractHeader(record, "success"));
        
        SagaInstance saga = sagaRepo.findById(sagaId).orElseThrow();
        
        if (success) {
            saga.addStepResult(stepName, record.value());
            int nextStep = saga.getCurrentStep() + 1;
            switch (nextStep) {
                case 1 -> executeStep(saga, 1, "AllocateNetwork",
                    "network.commands", new AllocateNetworkCommand(saga.getPayload()));
                case 2 -> executeStep(saga, 2, "ConfigureDNS",
                    "dns.commands", new ConfigureDNSCommand(saga.getPayload()));
                case 3 -> {
                    saga.setStatus(SagaStatus.COMPLETED);
                    sagaRepo.save(saga);
                    publishSagaCompleted(saga);
                }
            }
        } else {
            saga.setStatus(SagaStatus.COMPENSATING);
            sagaRepo.save(saga);
            compensate(saga);
        }
        ack.acknowledge();
    }
    
    private void compensate(SagaInstance saga) {
        int currentStep = saga.getCurrentStep();
        for (int i = currentStep - 1; i >= 0; i--) {
            SagaStep step = saga.getStep(i);
            if (step.getStatus() == StepStatus.COMPLETED) {
                String compensationTopic = step.getStepName() + ".compensate";
                executeCompensation(saga, i, compensationTopic, step.getResult());
            }
        }
    }
    
    private void executeStep(SagaInstance saga, int stepIndex, String stepName,
                            String commandTopic, Object command) {
        String eventId = UUID.randomUUID().toString();
        SagaStep step = new SagaStep(saga.getSagaId(), stepIndex, stepName);
        step.setCommandEventId(eventId);
        step.setStatus(StepStatus.SENT);
        sagaRepo.saveStep(step);
        saga.setCurrentStep(stepIndex);
        sagaRepo.save(saga);
        
        outboxRepo.save(OutboxEvent.builder()
            .eventId(eventId)
            .aggregateType("Saga")
            .aggregateId(saga.getSagaId())
            .eventType(stepName + "Command")
            .topic(commandTopic)
            .partitionKey(saga.getSagaId())
            .payload(serialize(command))
            .metadata(Map.of("saga_id", saga.getSagaId(), "step_name", stepName))
            .build());
    }
}
```

### Admin APIs

```
# Event delivery status
GET /api/v1/events/{event_id}/delivery
Response: {
  "event_id": "uuid-123",
  "event_type": "OrderCreated",
  "published_at": "2026-04-09T10:00:00.100Z",
  "deliveries": [
    {"consumer_group": "billing-service", "status": "PROCESSED", "at": "2026-04-09T10:00:00.300Z"},
    {"consumer_group": "inventory-service", "status": "PROCESSED", "at": "2026-04-09T10:00:00.250Z"},
    {"consumer_group": "notification-service", "status": "FAILED", "at": "2026-04-09T10:00:00.500Z",
     "error": "HTTP 503 from downstream"}
  ]
}

# Saga status
GET /api/v1/sagas/{saga_id}
Response: {
  "saga_id": "saga-abc",
  "saga_type": "ServerProvisioningSaga",
  "status": "RUNNING",
  "current_step": 2,
  "steps": [
    {"name": "ProvisionServer", "status": "COMPLETED", "duration_ms": 5000},
    {"name": "AllocateNetwork", "status": "COMPLETED", "duration_ms": 2000},
    {"name": "ConfigureDNS", "status": "SENT", "waiting_since": "2026-04-09T10:00:07Z"}
  ]
}

# Retry failed event delivery
POST /api/v1/events/{event_id}/retry
{
  "consumer_group": "notification-service"
}

# Force-complete stuck saga
POST /api/v1/sagas/{saga_id}/force-complete
{
  "reason": "Manual intervention: DNS configured manually"
}
```

### CLI

```bash
# Check outbox health
outbox-cli status
#   DATABASE     PENDING   PUBLISHED   FAILED   OLDEST_PENDING
#   orders_db    23        0           2        2026-04-09T10:00:00Z
#   billing_db   0         0           0        -

# Retry failed outbox events
outbox-cli retry --database orders_db --event-id uuid-123

# Check event delivery across all consumers
delivery-cli status --event-id uuid-123

# List active sagas
saga-cli list --status RUNNING --type ServerProvisioningSaga

# Check saga health
saga-cli health
#   SAGA_TYPE                    RUNNING   COMPENSATING   FAILED   AVG_DURATION
#   ServerProvisioningSaga       42        3              1        12.5s
#   NetworkSetupSaga             15        0              0        5.2s
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Transactional Outbox Pattern (The Core Problem)

**Why it's hard:**
The fundamental problem: a service needs to (1) update its database AND (2) publish an event to Kafka atomically. Without 2PC (which is impractical at scale), these are two independent operations that can fail independently.

```
The dual-write problem:

  Option A: Write DB first, then publish to Kafka
    1. INSERT INTO orders (...) -- SUCCEEDS
    2. kafka.send("order.created") -- FAILS (Kafka down, network issue)
    Result: DB updated but event never published. Downstream services never know.
    
  Option B: Publish to Kafka first, then write DB
    1. kafka.send("order.created") -- SUCCEEDS
    2. INSERT INTO orders (...) -- FAILS (constraint violation)
    Result: Event published but DB not updated. Downstream sees phantom event.
    
  Option C: 2PC (Two-Phase Commit)
    1. PREPARE on both DB and Kafka
    2. COMMIT on both
    Problem: Kafka doesn't natively support 2PC. Even if it did, 2PC is slow,
    has blocking failure modes, and doesn't scale.
```

**Approaches:**

| Approach | Atomicity | Performance | Complexity | Kafka Dependency |
|----------|----------|-------------|------------|-----------------|
| Dual-write (hope for the best) | No | Fast | Low | Direct |
| 2PC (XA transactions) | Yes | Slow (100ms+) | Very High | Direct |
| **Transactional outbox (polling)** | **Yes** | **Good** | **Medium** | **Indirect (poller)** |
| **Transactional outbox (CDC)** | **Yes** | **Best** | **Medium-High** | **Indirect (Debezium)** |
| Event-first (Kafka as source of truth) | Yes | Good | High | Direct (but different model) |

**Selected: Transactional Outbox (both polling and CDC options)**

**Why the outbox pattern works:**
```
  1. Single DB transaction:
     BEGIN;
       INSERT INTO orders (id, customer_id, ...) VALUES (...);
       INSERT INTO outbox (event_id, event_type, payload, ...) VALUES (...);
     COMMIT;
     
  If the transaction commits: both the business data and the event are persisted.
  If the transaction rolls back: neither is persisted. Consistent.
  
  2. A separate process reads the outbox and publishes to Kafka:
     - Polling: SELECT from outbox WHERE status='PENDING'
     - CDC: Debezium reads the binlog INSERT into outbox
     
  This decouples Kafka availability from the business transaction.
  If Kafka is down: outbox accumulates events; they're published when Kafka recovers.
```

**Polling publisher vs CDC comparison:**

| Aspect | Polling Publisher | CDC (Debezium) |
|--------|------------------|----------------|
| Latency | 100-500ms (poll interval) | 50-200ms (binlog lag) |
| Throughput | Limited by DB query rate | Binlog throughput (high) |
| DB load | SELECT + UPDATE per batch | Binlog read only (minimal) |
| Outbox cleanup | Explicit DELETE required | Row can be DELETE'd immediately |
| Ordering | ORDER BY created_at (may have clock skew) | Binlog order (guaranteed) |
| Complexity | Simple (SQL queries) | Medium (Debezium connector management) |

**Polling publisher optimization with SKIP LOCKED:**
```java
@Query(nativeQuery = true, value = """
    SELECT * FROM outbox 
    WHERE status = 'PENDING' 
    ORDER BY created_at 
    LIMIT :limit 
    FOR UPDATE SKIP LOCKED
""")
List<OutboxEvent> findPendingForUpdate(@Param("limit") int limit);

// SKIP LOCKED allows multiple poller instances to process different events
// without blocking each other. Each poller locks a different set of rows.
```

**CDC outbox with Debezium (Outbox Event Router SMT):**
```json
{
  "name": "outbox-connector",
  "config": {
    "connector.class": "io.debezium.connector.mysql.MySqlConnector",
    "database.hostname": "mysql-primary",
    "database.port": "3306",
    "database.server.name": "orders-db",
    "table.include.list": "orders.outbox",
    "transforms": "outbox",
    "transforms.outbox.type": "io.debezium.transforms.outbox.EventRouter",
    "transforms.outbox.route.by.field": "topic",
    "transforms.outbox.table.field.event.id": "event_id",
    "transforms.outbox.table.field.event.key": "aggregate_id",
    "transforms.outbox.table.field.event.type": "event_type",
    "transforms.outbox.table.field.event.payload": "payload",
    "transforms.outbox.table.fields.additional.placement": "event_type:header,aggregate_type:header",
    "transforms.outbox.route.topic.replacement": "${routedByValue}",
    "tombstones.on.delete": "false"
  }
}
```

**Failure Modes:**
- **Polling publisher crashes between Kafka ack and DB update:** Event re-published on next poll. Consumers must be idempotent.
- **Debezium connector crashes:** Resumes from last committed binlog position. May re-publish some events. Consumers dedup.
- **Outbox table grows unbounded (publisher down for hours):** Alert on outbox table size. Emergency: increase poll frequency, add poller instances.
- **Kafka unavailable for extended period:** Outbox accumulates events. When Kafka recovers, burst published. Consumers handle via Kafka backpressure.

**Interviewer Q&As:**

**Q1: Why is the transactional outbox pattern preferred over 2PC?**
A: 2PC has four problems: (1) Kafka doesn't natively support XA transactions. (2) 2PC is slow (100ms+ per transaction). (3) 2PC has blocking failure modes (coordinator crash blocks all participants). (4) 2PC doesn't scale horizontally. The outbox pattern uses a single DB transaction (fast, well-understood) and decouples Kafka publishing.

**Q2: What are the guarantees of the outbox pattern?**
A: At-least-once publishing. The event is guaranteed to be published if the DB transaction commits. May be published more than once (publisher crash after Kafka ack but before DB update). Consumer-side idempotency is required for exactly-once processing.

**Q3: How do you handle ordering with the polling publisher?**
A: `ORDER BY created_at` ensures approximate order. With multiple pollers using `SKIP LOCKED`, events from the same aggregate may be processed out of order. Solution: partition pollers by aggregate_id. Or use CDC: binlog preserves exact transaction order.

**Q4: Why is CDC better than polling for the outbox pattern?**
A: (1) Lower latency (binlog streaming vs poll interval). (2) Lower DB load (binlog read vs SELECT+UPDATE). (3) Guaranteed ordering (binlog order vs clock-based). (4) No table growth (Debezium reads INSERT from binlog; row can be deleted immediately).

**Q5: What happens if the outbox table DELETE fails after publishing?**
A: Event is already in Kafka. The row remains with status=PUBLISHED. Cleanup job deletes it later. Cosmetic issue, not a correctness issue.

**Q6: How do you scale the polling publisher for 150K events/sec?**
A: (1) Multiple pollers with `SKIP LOCKED`. (2) Batched SELECT (1000 rows). (3) Batched Kafka produce. (4) Shard outbox tables by aggregate_type or hash(aggregate_id). Or use CDC: scales with binlog throughput.

---

### Deep Dive 2: Exactly-Once Processing (Idempotent Consumers)

**Why it's hard:**
True exactly-once delivery between two independent systems is theoretically impossible in an asynchronous distributed system (Two Generals' Problem). What we achieve is "effectively exactly-once" through idempotent processing: at-least-once delivery + consumer-side deduplication.

**The delivery semantics spectrum:**

```
At-most-once:
  Producer: acks=0 | Consumer: auto-commit before processing
  Risk: message loss

At-least-once:
  Producer: acks=all, retries=MAX, idempotence=true
  Consumer: manual commit after processing
  Risk: duplicates

Exactly-once (effectively):
  At-least-once delivery + consumer idempotency (dedup on event_id)
  Effect applied exactly once even if message delivered multiple times.
```

**Approaches to exactly-once:**

| Approach | Scope | Overhead | Reliability | Complexity |
|----------|-------|----------|-------------|------------|
| Kafka idempotent producer | Producer -> Broker | ~1-2% latency | High | Low |
| Kafka transactions | Producer -> Broker -> Consumer offset | ~10% latency | Very High | Medium |
| **Consumer idempotency (Redis dedup)** | **Consumer processing** | **~1ms per message** | **Very High** | **Medium** |
| **Database-level idempotency** | **Consumer processing** | **~5ms per message** | **Highest** | **Medium** |

**Selected: Kafka idempotent producer + Consumer idempotency (Redis + DB upserts)**

**Kafka idempotent producer:**
```
Producer (enable.idempotence=true):
  - Gets unique ProducerId (PID) on init
  - Maintains sequence number per (topic, partition)
  - Each batch: PID + sequence sent

Broker:
  - Tracks last 5 sequences per (PID, partition)
  - Duplicate batch (same PID + sequence) -> return success, don't re-append
  - Sequential batch (seq = last + 1) -> append
  
This handles: producer sends batch, broker appends, ack lost,
              producer retries -> broker deduplicates.
```

**Kafka Transactions (atomic produce + consume offset):**
```java
producer.initTransactions();

while (running) {
    ConsumerRecords<String, byte[]> records = consumer.poll(Duration.ofMillis(100));
    
    producer.beginTransaction();
    try {
        for (ConsumerRecord<String, byte[]> record : records) {
            byte[] result = transform(record);
            producer.send(new ProducerRecord<>("billing.events", record.key(), result));
        }
        producer.sendOffsetsToTransaction(currentOffsets(records), consumer.groupMetadata());
        producer.commitTransaction();
    } catch (ProducerFencedException | OutOfOrderSequenceException e) {
        producer.close();
        throw e;
    } catch (KafkaException e) {
        producer.abortTransaction();
    }
}
```

**Redis-based consumer idempotency:**
```java
public class RedisIdempotencyService {
    
    public IdempotencyResult checkAndClaim(String consumerGroup, String eventId) {
        String key = "idempotency:" + consumerGroup + ":" + eventId;
        
        Boolean claimed = redis.opsForValue().setIfAbsent(
            key, "PROCESSING:" + System.currentTimeMillis(), Duration.ofHours(24));
        
        if (Boolean.TRUE.equals(claimed)) return IdempotencyResult.NEW;
        
        String value = redis.opsForValue().get(key);
        if (value != null && value.startsWith("COMPLETED")) return IdempotencyResult.DUPLICATE;
        
        if (value != null && value.startsWith("PROCESSING")) {
            long startTime = Long.parseLong(value.split(":")[1]);
            if (System.currentTimeMillis() - startTime > 300_000) {
                // Stale lock (> 5min): override
                redis.opsForValue().set(key, "PROCESSING:" + System.currentTimeMillis(),
                    Duration.ofHours(24));
                return IdempotencyResult.NEW;
            }
            return IdempotencyResult.IN_PROGRESS;
        }
        return IdempotencyResult.NEW;
    }
    
    public void markCompleted(String consumerGroup, String eventId) {
        String key = "idempotency:" + consumerGroup + ":" + eventId;
        redis.opsForValue().set(key, "COMPLETED:" + System.currentTimeMillis(),
            Duration.ofHours(24));
    }
    
    public void markFailed(String consumerGroup, String eventId) {
        redis.delete("idempotency:" + consumerGroup + ":" + eventId);
    }
}
```

**Database-level idempotency (strongest):**
```java
@Transactional
public void handleOrderCreated(OrderCreatedEvent event) {
    jdbcTemplate.update("""
        INSERT INTO billing_orders (order_id, customer_id, amount, status, event_id)
        VALUES (?, ?, ?, 'PENDING', ?)
        ON DUPLICATE KEY UPDATE 
            amount = IF(VALUES(event_id) > event_id, VALUES(amount), amount),
            event_id = IF(VALUES(event_id) > event_id, VALUES(event_id), event_id)
    """, event.getOrderId(), event.getCustomerId(), event.getAmount(), event.getEventId());
}
```

**Failure Modes:**
- **Redis unavailable:** Circuit breaker activates. Fallback to DB-level idempotency. Fail closed for financial ops.
- **Consumer crashes between processing and markCompleted:** Redis key stuck at PROCESSING. Stale lock detection (5min timeout) handles this.
- **Consumer processes but fails to commit Kafka offset:** On restart, re-reads message. Redis dedup catches it.

**Interviewer Q&As:**

**Q1: Is true exactly-once delivery possible?**
A: No (Two Generals' Problem). We achieve "effectively exactly-once" through idempotency: the message may be delivered multiple times, but the consumer's effect is applied once.

**Q2: Why use Redis for idempotency instead of the consumer's database?**
A: Redis SET NX: 0.1ms. DB INSERT unique check: 5-10ms. At 3M dedup checks/sec, Redis handles it; a DB would be overwhelmed. For critical operations: both (Redis fast-path + DB safety net).

**Q3: How long should idempotency keys be retained?**
A: At least as long as Kafka retention. If Kafka retains 7 days, keys should live 7+ days. Ensures replay from any retention point is deduplicated.

**Q4: How does exactly-once work when the consumer calls an external HTTP API?**
A: If the API doesn't support idempotency keys: you can't guarantee exactly-once externally. Options: (1) Pass event_id as idempotency key to the API. (2) Buffer in local DB, forward reliably. (3) Accept at-least-once.

**Q5: What is the performance overhead of exactly-once?**
A: Kafka transactions: ~10-15% latency. Redis dedup: ~0.1ms/message. DB upsert: ~5ms/message. Combined: 10-15% latency + ~1% throughput. Acceptable for most infrastructure workloads.

**Q6: What happens if idempotency check passes but then the consumer crashes before processing completes?**
A: Redis key is at PROCESSING. After 5-minute stale lock timeout, another consumer instance claims and retries. The partial processing from the crashed instance may leave side effects, so processing itself should be idempotent (DB upserts, not inserts).

---

### Deep Dive 3: Saga Pattern (Distributed Transactions)

**Why it's hard:**
Infrastructure operations span multiple services: provisioning hardware, configuring network, setting up DNS, updating billing. If any step fails, previous steps must be compensated. Traditional 2PC doesn't work across microservices.

**Approaches:**

| Approach | Coordination | Coupling | Visibility | Complexity |
|----------|-------------|---------|------------|------------|
| 2PC | Centralized | Tight | High | Very High |
| **Orchestration** | **Central orchestrator** | **Medium** | **High** | **Medium** |
| Choreography | Decentralized (events) | Low | Low | Medium |

**Orchestration vs Choreography:**
```
Choreography: Each service reacts to events independently.
  + Loose coupling, simple services
  - Hard to track progress, complex failure handling, implicit workflow

Orchestration: Central coordinator directs each step.
  + Clear workflow, easy debugging, centralized failure handling
  - Orchestrator is a single point of logic (not failure)
```

**Selected: Orchestration for 3+ step operations; Choreography for simple 2-step flows**

**Saga definition (declarative):**
```java
public static SagaDefinition<ProvisionServerRequest> build() {
    return SagaDefinition.<ProvisionServerRequest>builder()
        .name("ServerProvisioningSaga")
        .timeout(Duration.ofMinutes(30))
        
        .step("ProvisionServer")
            .command(req -> new ProvisionServerCommand(req.getServerId(), req.getSpec()))
            .commandTopic("provisioning.commands")
            .compensatingCommand(req -> new DecommissionServerCommand(req.getServerId()))
            .timeout(Duration.ofMinutes(10))
        
        .step("AllocateNetwork")
            .command((req, prev) -> new AllocateNetworkCommand(
                req.getServerId(), prev.get("ProvisionServer").getIpAddress()))
            .commandTopic("network.commands")
            .compensatingCommand(req -> new DeallocateNetworkCommand(req.getServerId()))
            .timeout(Duration.ofMinutes(5))
        
        .step("ConfigureDNS")
            .command((req, prev) -> new ConfigureDNSCommand(
                req.getServerId(), prev.get("AllocateNetwork").getDnsName()))
            .commandTopic("dns.commands")
            .compensatingCommand(req -> new RemoveDNSCommand(req.getServerId()))
            .timeout(Duration.ofMinutes(2))
        
        .step("ChargeBilling")
            .command(req -> new ChargeBillingCommand(req.getCustomerId(), req.getBillingPlan()))
            .commandTopic("billing.commands")
            .compensatingCommand(req -> new RefundBillingCommand(
                req.getCustomerId(), req.getBillingPlan()))
            .timeout(Duration.ofMinutes(1))
        
        .onCompleted(saga -> publishEvent("ServerFullyProvisioned", saga))
        .onFailed(saga -> publishEvent("ServerProvisioningFailed", saga))
        .build();
}
```

**Participant service (handles commands via outbox):**
```java
@KafkaListener(topics = "provisioning.commands", groupId = "provisioning-service")
@Transactional
public void handleCommand(ConsumerRecord<String, byte[]> record, Acknowledgment ack) {
    String sagaId = extractHeader(record, "saga_id");
    String eventType = extractHeader(record, "event_type");
    int stepIndex = Integer.parseInt(extractHeader(record, "step_index"));
    
    try {
        Object result = switch (eventType) {
            case "ProvisionServerCommand" -> provisionServer(deserialize(record.value()));
            case "DecommissionServerCommand" -> decommissionServer(deserialize(record.value()));
            default -> throw new IllegalArgumentException("Unknown: " + eventType);
        };
        
        // Reply success via outbox (atomic with business state)
        outboxRepo.save(OutboxEvent.builder()
            .eventId(UUID.randomUUID().toString())
            .aggregateType("Saga").aggregateId(sagaId)
            .eventType("SagaStepReply").topic("saga.replies")
            .partitionKey(sagaId).payload(serialize(result))
            .metadata(Map.of("saga_id", sagaId, "step_index", 
                String.valueOf(stepIndex), "success", "true"))
            .build());
    } catch (Exception e) {
        // Reply failure via outbox
        outboxRepo.save(OutboxEvent.builder()
            .eventId(UUID.randomUUID().toString())
            .aggregateType("Saga").aggregateId(sagaId)
            .eventType("SagaStepReply").topic("saga.replies")
            .partitionKey(sagaId)
            .payload(serialize(Map.of("error", e.getMessage())))
            .metadata(Map.of("saga_id", sagaId, "step_index",
                String.valueOf(stepIndex), "success", "false",
                "error_message", e.getMessage()))
            .build());
    }
    ack.acknowledge();
}
```

**Failure Modes:**
- **Orchestrator crashes:** Restarts from DB state. Pending steps re-checked. Commands are idempotent.
- **Participant crashes:** Command redelivered from Kafka. Participant must be idempotent.
- **Compensation fails:** Retry with backoff. If permanent: FAILED state, human intervention. Background reconciliation detects inconsistencies.
- **Saga timeout:** Background job triggers compensation. Design timeouts > 2x expected duration.

**Interviewer Q&As:**

**Q1: When orchestration vs choreography?**
A: Orchestration: 3+ steps, complex failure handling, need visibility. Choreography: 2-3 simple steps, loose coupling priority.

**Q2: How do you ensure participant commands are idempotent?**
A: Each command has a unique command_event_id. Participant checks if already processed (Redis SET NX or DB unique constraint). If yes: returns cached result.

**Q3: What if compensation fails?**
A: Retry with backoff. If permanent failure: FAILED state + human alert. Design compensations to always succeed (handle "nothing to compensate" case).

**Q4: How do you handle long-running sagas (30 minutes)?**
A: (1) Timeout 2x expected. (2) Steps send heartbeat events. (3) Step-level timeouts separate from saga timeout.

**Q5: How do you test sagas?**
A: (1) Unit test each handler. (2) Integration test full saga (testcontainers). (3) Failure injection at each step. (4) Timeout simulation. (5) Idempotency replay test.

**Q6: Can you combine outbox + saga?**
A: Yes, and you should. Each saga command published via outbox. Saga state update and command publication are in the same DB transaction. Atomic.

---

## 7. Scheduling & Resource Management

### Outbox Publisher Resources

| Component | Resource | Sizing |
|-----------|----------|--------|
| Polling publisher | 4 pods x 2 CPU x 4 GB | Each pod polls with SKIP LOCKED |
| CDC publisher (Debezium) | 1 Kafka Connect worker per DB | 4 CPU x 8 GB per worker |
| Event router (CDC) | 4 pods x 2 CPU x 4 GB | Routes outbox events to domain topics |

### Saga Orchestrator Resources

| Component | Resource | Sizing |
|-----------|----------|--------|
| Saga engine | 4 pods x 4 CPU x 8 GB | 10K concurrent sagas |
| Saga timeout checker | 1 pod x 1 CPU x 2 GB | Runs every minute |
| Saga state DB | MySQL replica set (3 nodes) | Low volume |

### Consumer Idempotency Resources

| Component | Resource | Sizing |
|-----------|----------|--------|
| Redis Cluster | 6 nodes x 4 GB x 4 CPU | 3M SET NX/sec, 25 GB data |
| Processed events table | MySQL (partitioned by day) | 7-day retention |

### Kubernetes CronJobs

```yaml
apiVersion: batch/v1
kind: CronJob
metadata:
  name: outbox-cleanup
spec:
  schedule: "0 * * * *"
  jobTemplate:
    spec:
      template:
        spec:
          containers:
            - name: cleanup
              image: infra/outbox-cleanup:1.0
              env:
                - name: CLEANUP_OLDER_THAN_HOURS
                  value: "1"
---
apiVersion: batch/v1
kind: CronJob
metadata:
  name: saga-timeout-checker
spec:
  schedule: "*/1 * * * *"
  jobTemplate:
    spec:
      template:
        spec:
          containers:
            - name: timeout-checker
              image: infra/saga-engine:1.0
              command: ["java", "-jar", "saga.jar", "--mode=timeout-check"]
```

---

## 8. Scaling Strategy

| Dimension | Mechanism | Limit |
|-----------|----------|-------|
| Outbox write throughput | Shard outbox tables by aggregate_type | ~50K inserts/sec per table |
| Outbox publish throughput | Multiple pollers with SKIP LOCKED | DB query rate limited |
| CDC throughput | Parallel Debezium tasks per DB shard | Binlog throughput |
| Consumer throughput | Add consumers up to partition count | 1 per partition per group |
| Idempotency throughput | Redis cluster scaling | ~100K ops/sec per node |
| Saga throughput | Shard saga state by saga_id hash | ~10K sagas/sec per shard |

**Interviewer Q&As:**

**Q1: How do you scale the outbox for 150K events/sec?**
A: (1) Shard outbox table by hash(aggregate_id). (2) One poller per shard with SKIP LOCKED. (3) Or CDC: scales with binlog throughput. (4) Batched Kafka produce.

**Q2: What happens when outbox table grows too large?**
A: Publisher should DELETE rows after publishing. Alert on > 100K pending rows. Emergency: increase poll frequency, add pollers.

**Q3: How do you handle adding new consumer groups?**
A: New group starts from `auto.offset.reset`. `earliest` with idempotency = safe replay of all retained events.

**Q4: What is max saga throughput?**
A: MySQL write bottleneck: ~10K txns/sec. Each saga step: 2 DB writes. 4-step saga: ~1,250 new sagas/sec. Shard for more.

**Q5: How do you handle Redis failure for idempotency?**
A: Circuit breaker -> DB fallback (processed_events with unique constraint). Higher latency but correct. Auto-failover via Redis Cluster.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Detection | Impact | Mitigation | RTO |
|---------|-----------|--------|------------|-----|
| Producer crash before outbox write | N/A | Consistent: neither business data nor event | Transaction rollback | N/A |
| Producer crash after outbox write | Heartbeat | Event published by poller/CDC | Automatic | < 500ms |
| Polling publisher crash | Pod restart | Events accumulate | K8s restart; other pollers via SKIP LOCKED | < 30s |
| Debezium crash | Connect monitoring | CDC stops | Auto-restart; resume from binlog | < 30s |
| Kafka unavailable | Producer timeout | Outbox accumulates | Publish when recovered; no loss | Kafka recovery |
| Consumer crash | Group rebalance | Events reprocessed | Idempotent processing | 10-30s |
| Redis failure | Health check | Dedup degrades to DB | Circuit breaker + DB fallback | < 5s |
| Saga orchestrator crash | Pod restart | Sagas paused | Restore from DB; re-send commands | < 30s |
| Saga participant timeout | Timeout checker | Compensation triggered | Retry or compensate | 1-60 min |
| MySQL primary failure | Replication lag | Outbox writes fail | MySQL failover; poller resumes | 30s-5min |
| Duplicate event | Idempotency check | Consumer sees duplicate | Redis/DB dedup | Automatic |

### Delivery Semantics Summary

| Layer | Semantic | Mechanism |
|-------|----------|-----------|
| Producer -> Outbox | Exactly-once | Single DB transaction |
| Outbox -> Kafka | At-least-once | Poller retries; CDC restarts from binlog |
| Kafka -> Consumer | At-least-once | Commit after processing; rebalance may replay |
| Consumer processing | Exactly-once (effectively) | Redis SET NX + DB upsert |
| **End-to-end** | **Exactly-once (effectively)** | **Composition of above** |

---

## 10. Observability

### Key Metrics

| Category | Metric | Alert Threshold | Why |
|----------|--------|----------------|-----|
| Outbox | `outbox_pending_count` | > 10,000 | Publisher falling behind |
| Outbox | `outbox_oldest_pending_age_sec` | > 30 | Events delayed |
| Outbox | `outbox_publish_rate` | Drop > 50% | Publisher may be down |
| Consumer | `idempotency_duplicate_rate` | > 5% | Excessive redelivery |
| Consumer | `processing_latency_p99` | > 2000 ms | Consumer slow |
| Redis | `idempotency_ops_per_sec` | > 80% capacity | Redis saturation |
| Saga | `saga_active_count` | > 10x baseline | Saga storm |
| Saga | `saga_completion_time_p99` | > 2x expected | Steps slow |
| Saga | `saga_compensation_count` | > 10% of started | High failure rate |
| E2E | `event_e2e_latency_p99` | > 5000 ms | Delivery slow |
| E2E | `event_loss_detected` | > 0 | Reconciliation gap |

### Distributed Tracing

```
[Producer Service]
  |--- Span: "create-order" (business + outbox write)
  v
[Polling Publisher]
  |--- Span: "outbox-publish" (linked via correlation_id)
  v
[Kafka]
  |
[Consumer Service]
  |--- Span: "consume-event" (linked via event_id header)
  |    |--- Span: "idempotency-check"
  |    |--- Span: "process-event"
  |    |--- Span: "commit-offset"

Saga trace:
  [Saga Orchestrator]
    |--- Span: "saga-start"
    |    |--- step-1-command -> step-1-reply
    |    |--- step-2-command -> step-2-reply
    |    |--- step-3-command -> step-3-reply
    |    |--- saga-complete
```

### Logging

```json
{"timestamp":"2026-04-09T10:00:00.123Z","level":"INFO","component":"outbox-publisher",
 "event":"batch_published","batch_size":1000,"oldest_age_ms":150,"kafka_time_ms":8}

{"timestamp":"2026-04-09T10:00:00.456Z","level":"INFO","component":"billing-consumer",
 "event":"event_processed","event_id":"uuid-123","idempotency":"NEW","time_ms":15}

{"timestamp":"2026-04-09T10:00:05.789Z","level":"INFO","component":"saga-engine",
 "event":"step_completed","saga_id":"saga-abc","step":"ProvisionServer","duration_ms":5000}
```

---

## 11. Security

### Event Data Security

| Layer | Mechanism |
|-------|-----------|
| Outbox at rest | MySQL TDE (AES-256) |
| Kafka at rest | LUKS filesystem encryption |
| In transit | TLS 1.3 (all paths) |
| PII in events | Field-level encryption before outbox write |
| Redis | TLS + AUTH |

### Event Integrity

```java
// Sign events to prevent tampering
public class EventSigner {
    public String sign(OutboxEvent event) {
        String data = event.getEventId() + event.getPayload() + event.getCreatedAt();
        Signature sig = Signature.getInstance("SHA256withRSA");
        sig.initSign(signingKey);
        sig.update(data.getBytes());
        return Base64.getEncoder().encodeToString(sig.sign());
    }
}

// Consumer verifies before processing
public class EventVerifier {
    public boolean verify(ConsumerRecord<String, byte[]> record) {
        String signature = extractHeader(record, "event_signature");
        String data = extractHeader(record, "event_id") + new String(record.value()) 
            + extractHeader(record, "timestamp");
        Signature sig = Signature.getInstance("SHA256withRSA");
        sig.initVerify(verifyKey);
        sig.update(data.getBytes());
        return sig.verify(Base64.getDecoder().decode(signature));
    }
}
```

### Saga Security
- Command authentication: signed token from orchestrator
- Participant authorization: verify orchestrator identity
- Compensation authorization: original saga_id + signature
- Audit trail: every state transition logged
- Data isolation: saga payloads encrypted at rest

---

## 12. Incremental Rollout Strategy

### Phase 1: Outbox Pattern (Week 1-3)
- Implement outbox table in one service. Deploy polling publisher. Validate every write produces a Kafka event.

### Phase 2: Consumer Idempotency (Week 4-5)
- Deploy Redis dedup for 3 consumers. Inject duplicates to validate. Measure dedup rate.

### Phase 3: CDC Publisher (Week 6-7)
- Deploy Debezium. Run parallel with poller. Compare output. Switch to CDC.

### Phase 4: Saga Pattern (Week 8-10)
- Implement orchestrator for one workflow. Test all failure modes. Load test 1000 concurrent sagas.

### Phase 5: Full Rollout (Week 11-14)
- Outbox for all producers. Idempotent consumers everywhere. Sagas for all multi-service ops.

**Rollout Q&As:**

**Q1: How to migrate from direct Kafka produce to outbox?**
A: Add outbox table. Write to outbox in same transaction. Keep both direct+outbox temporarily. Reconcile. Remove direct produce.

**Q2: How to handle transition with mixed approaches?**
A: Consumer idempotency works regardless of produce method. Deploy consumer dedup first, then migrate producers incrementally.

**Q3: How to validate exactly-once in production?**
A: Reconciliation: compare producer DB counts with consumer DB counts. Alert if divergence > 0.01%.

**Q4: How to load test sagas?**
A: Mock participants with configurable delays. Start 10K sagas. Inject 10% failures. Verify all complete or compensate.

**Q5: How to rollback outbox if issues?**
A: Feature flag for direct produce. Flip flag on issues. Duplicates during transition handled by consumer dedup.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Chosen | Rationale |
|----------|---------|--------|-----------|
| Atomicity | Dual-write, 2PC, Outbox, Event-first | Transactional Outbox | Single-DB atomicity; decouples Kafka |
| Outbox publisher | Polling, CDC, JDBC Connect | CDC primary, Polling fallback | CDC: lower latency, less DB load |
| Consumer dedup | None, Redis, DB, Kafka transactions | Redis + DB upsert | Redis: fast; DB: safety net |
| Saga pattern | Choreography, Orchestration, 2PC | Orchestration | Visibility, centralized failure handling |
| Saga state | Kafka, MySQL, Redis | MySQL | Transactional; queryable; durable |
| Idempotency TTL | 1h, 24h, 7d | 24h Redis, 7d DB | 24h covers retries; 7d covers replay |
| Event ordering | Global, Per-partition, Per-aggregate | Per-aggregate | Correct for entity state; parallel across entities |
| Compensation | Automatic, Manual | Auto with human fallback | Auto for known failures; escalate unknown |

---

## 14. Agentic AI Integration

### AI-Powered Event Delivery Monitoring

```python
class EventDeliveryAgent:
    """AI agent ensuring reliable event delivery."""
    
    def detect_delivery_anomalies(self):
        outbox_events = self.query_outbox_published(last_hours=1)
        consumer_events = self.query_consumer_processed(last_hours=1)
        missing = outbox_events - consumer_events
        
        if missing:
            analysis = self.model.reason(
                prompt=f"""{len(missing)} events published but not consumed.
                Missing: {self.summarize(missing)}
                Consumer status: {self.get_consumer_status()}
                Topic lag: {self.get_lag()}
                Diagnose and recommend action."""
            )
            if analysis.severity == "CRITICAL":
                self.alert_oncall(analysis)
    
    def optimize_outbox_config(self):
        metrics = self.collect_outbox_metrics(days=7)
        return self.model.analyze(
            prompt=f"""Outbox metrics: {metrics}
            Recommend: poll_interval, batch_size, publisher_count,
                       cleanup_frequency, should_switch_to_cdc"""
        )
```

### AI-Powered Saga Recovery

```python
class SagaRecoveryAgent:
    """AI agent for stuck/failed saga recovery."""
    
    def recover_stuck_saga(self, saga_id):
        saga = self.get_saga_state(saga_id)
        participant_health = self.get_participant_health(saga.current_step)
        
        plan = self.model.reason(
            prompt=f"""Saga {saga_id} stuck at step {saga.current_step}.
            State: {saga}
            Participant health: {participant_health}
            Time stuck: {saga.time_stuck}
            
            Options: RETRY, SKIP, COMPENSATE, MANUAL.
            Which and why?"""
        )
        
        if plan.action == "RETRY" and plan.confidence > 0.9:
            self.retry_step(saga_id, saga.current_step)
        elif plan.action == "COMPENSATE":
            self.compensate(saga_id)
        else:
            self.escalate(saga_id, plan)
    
    def predict_saga_failures(self):
        for saga in self.get_active_sagas():
            risk = self.model.predict(
                prompt=f"""Failure probability for saga {saga.saga_id}:
                Type: {saga.saga_type}, Step: {saga.current_step}/{saga.total_steps},
                Elapsed: {saga.elapsed}, Historical failure rate: {saga.historical_rate}"""
            )
            if risk > 0.7:
                self.take_preemptive_action(saga)
```

### Intelligent Reconciliation

```
Agent workflow:
  1. Hourly: compare producer events vs consumer effects per aggregate
  2. Detect: missing events, duplicate effects, ordering violations
  3. Diagnose: check consumer lag, DLQ, idempotency cache, errors
  4. Remediate: replay missing, clean duplicates, flag ordering issues
  5. Prevent: identify patterns, recommend config/code changes
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: What is the transactional outbox pattern?**
A: Solves dual-write: atomically update DB and publish event by writing both in a single DB transaction. Separate process (poller/CDC) publishes outbox events to Kafka. Guarantees at-least-once publishing. Consumer idempotency provides exactly-once processing.

**Q2: Difference between at-least-once and exactly-once?**
A: At-least-once: delivered 1+ times, possible duplicates. Exactly-once: effect applied once via idempotent consumer (dedup on event_id). True exactly-once delivery is impossible (Two Generals); we achieve it through idempotency.

**Q3: Why can't we achieve true exactly-once delivery?**
A: Two Generals' Problem: no guarantee two systems agree on delivery status in async network. We use at-least-once + consumer dedup for "effectively exactly-once."

**Q4: Polling vs CDC for outbox?**
A: Polling: simpler, higher latency (poll interval), more DB load. CDC: lower latency (binlog), minimal DB load, guaranteed ordering, more complex ops. Start with polling; migrate to CDC for latency/load.

**Q5: What is the Saga pattern?**
A: Manages distributed transactions without 2PC. Each step: local transaction + event. Failure: compensating transactions undo previous steps. Orchestration (central coordinator) or Choreography (event-driven).

**Q6: How to handle saga compensation failure?**
A: Retry with backoff. Permanent failure: FAILED state + human alert. Design compensations to always succeed (handle "nothing to undo" gracefully). Background reconciliation detects inconsistencies.

**Q7: How to implement consumer idempotency with MySQL?**
A: (1) Upsert: `INSERT ... ON DUPLICATE KEY UPDATE`. (2) Processed events table: unique constraint on event_id, checked in same transaction as business write.

**Q8: What if Redis goes down?**
A: Circuit breaker -> DB fallback (processed_events table). Higher latency but correct. Fail closed for financial ops, fail open for notifications.

**Q9: How to test outbox end-to-end?**
A: Testcontainers (MySQL + Kafka). Create entity, verify Kafka event, verify consumer processes it. Crash tests: kill poller mid-publish, verify retry + dedup.

**Q10: Ordering with multiple outbox pollers?**
A: SKIP LOCKED can interleave events from same aggregate across pollers. Fix: shard pollers by aggregate_id, or use CDC (binlog preserves order).

**Q11: Max outbox latency?**
A: Polling: ~115ms worst case (100ms poll + 15ms produce). CDC: ~55ms worst case (50ms binlog + 5ms produce).

**Q12: Outbox for different databases?**
A: Same pattern. MySQL: Debezium MySQL (binlog). PostgreSQL: Debezium PostgreSQL (WAL). Polling works identically on both.

**Q13: Can Kafka be the outbox?**
A: Event-first: write Kafka first, consumer updates DB. Challenge: if DB update fails, inconsistency. Outbox pattern is safer (DB transaction is source of truth).

**Q14: How to monitor end-to-end delivery?**
A: Outbox metrics (pending count, age, rate). Kafka lag per group. Consumer dedup rate. Periodic reconciliation (producer DB vs consumer DB counts).

**Q15: Saga orchestration vs choreography?**
A: Orchestration: 3+ steps, complex failures, visibility needed. Choreography: 2-3 steps, loose coupling priority, independently owned services.

**Q16: Long-running sagas (30 min)?**
A: Timeout 2x expected (60min). Heartbeat events from steps. Step-level timeouts separate from saga timeout. Participant reports progress asynchronously.

**Q17: Outbox impact on production database?**
A: Polling: SELECT + UPDATE queries add load. SKIP LOCKED minimizes contention. CDC: reads binlog only (minimal). Monitor DB CPU/IOPS; switch to CDC if > 10% overhead.

---

## 16. References

| Resource | URL / Reference |
|----------|----------------|
| Transactional Outbox Pattern | https://microservices.io/patterns/data/transactional-outbox.html |
| Saga Pattern | https://microservices.io/patterns/data/saga.html |
| Debezium Outbox Event Router | https://debezium.io/documentation/reference/transformations/outbox-event-router.html |
| Kafka Exactly-Once Semantics | https://www.confluent.io/blog/exactly-once-semantics-are-possible-heres-how-apache-kafka-does-it/ |
| Designing Data-Intensive Applications | Martin Kleppmann, Chapters 9, 11, 12 |
| Enterprise Integration Patterns | Hohpe & Woolf |
| Pat Helland: "Life Beyond Distributed Transactions" | https://queue.acm.org/detail.cfm?id=3025012 |
| Eventuate Tram (Saga Framework) | https://eventuate.io/abouteventuatetram.html |
| Chris Richardson: "Microservices Patterns" | Manning, Chapters 3-4 |
| Two Generals' Problem | https://en.wikipedia.org/wiki/Two_Generals%27_Problem |
| Kafka Transactions (KIP-98) | https://cwiki.apache.org/confluence/display/KAFKA/KIP-98 |
