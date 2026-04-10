# System Design: Pub/Sub Notification System

> **Relevance to role:** A cloud infrastructure platform engineer builds notification systems that underpin every operational workflow: bare-metal server provisioning status updates pushed to thousands of engineers, Kubernetes pod event fan-out to monitoring dashboards, OpenStack service alerts distributed to on-call teams, job scheduling completion notifications, and Agentic AI task status broadcasts. The core challenge is reliable fan-out at scale -- delivering one event to many subscribers with ordering, filtering, and delivery guarantees.

---

## 1. Requirement Clarifications

### Functional Requirements
| ID | Requirement |
|----|-------------|
| FR-1 | Publishers send messages to named **topics** (logical channels) |
| FR-2 | Subscribers register interest in topics via **subscriptions** |
| FR-3 | Each subscription independently tracks delivery (offset/ack); all subscriptions receive all messages |
| FR-4 | Support **push** (HTTP/gRPC webhook) and **pull** (long-poll) delivery models |
| FR-5 | Support **filter expressions** on subscriptions (attribute-based routing) |
| FR-6 | Support **message ordering** per ordering key within a subscription |
| FR-7 | Support **message TTL**: undelivered messages expire after configurable duration |
| FR-8 | Support **at-least-once** delivery with idempotency keys for consumer-side dedup |
| FR-9 | Large fan-out: single message delivered to up to **1 million subscribers** |
| FR-10 | Admin: create/delete topics, manage subscriptions, view delivery status |

### Non-Functional Requirements
| ID | Requirement | Target |
|----|-------------|--------|
| NFR-1 | Publish latency | < 10 ms p99 |
| NFR-2 | Push delivery latency (publish -> subscriber webhook) | < 200 ms p99 |
| NFR-3 | Pull delivery latency (publish -> consumer poll) | < 50 ms p99 |
| NFR-4 | Fan-out throughput (single message -> N subscribers) | 1M deliveries in < 30 seconds |
| NFR-5 | Message throughput | 500,000 messages/sec published |
| NFR-6 | Subscriber count | 1M+ per topic (for broadcast topics) |
| NFR-7 | Availability | 99.99% |
| NFR-8 | Durability | No acknowledged message lost |
| NFR-9 | Message size | Up to 10 MB per message |

### Constraints & Assumptions
- Core message store: Kafka (leveraging existing Kafka cluster).
- Push delivery: outbound HTTP/gRPC webhooks to subscriber endpoints.
- Pull delivery: Kafka consumer groups per subscription.
- Subscription metadata stored in MySQL.
- Message attributes (for filtering) stored as Kafka headers.
- Infrastructure notification use cases: 50-100 subscribers per topic (common), 1M subscribers (broadcast, rare).
- Average message size: 1 KB.

### Out of Scope
- Email/SMS delivery channels (handled by downstream notification services).
- User preference management (handled by notification settings service).
- Mobile push notifications (APNs/FCM integration).
- Message templating and rendering.

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|-------------|--------|
| Published messages | 500,000 msgs/sec | -- |
| Average subscribers per topic | 50 (weighted average) | -- |
| Fan-out messages | 500,000 x 50 | **25M deliveries/sec** |
| Push subscribers (webhook) | 60% of subscriptions | 15M deliveries/sec |
| Pull subscribers (consumer) | 40% of subscriptions | 10M deliveries/sec |
| Topics | 10,000 | -- |
| Total subscriptions | 10,000 topics x 50 avg subscribers | **500,000 subscriptions** |
| Broadcast topics (1M+ subscribers) | 10 topics | -- |
| Broadcast fan-out | 10 topics x 100 msgs/sec x 1M subscribers | **1B deliveries/sec peak** |
| Messages per day | 500K msgs/sec x 86,400 | **43.2B messages/day** |

### Latency Requirements

| Path | Target | Mechanism |
|------|--------|-----------|
| Publisher -> Kafka | < 10 ms (acks=1) | Kafka produce |
| Kafka -> Pull subscriber | < 50 ms | Kafka consumer poll |
| Kafka -> Push delivery start | < 100 ms | Fan-out service consumes from Kafka |
| Push delivery (HTTP webhook) | < 200 ms | HTTP POST to subscriber endpoint |
| Filter evaluation | < 1 ms | In-memory attribute matching |
| Subscription creation/deletion | < 100 ms | MySQL write + cache invalidation |

### Storage Estimates

| Parameter | Calculation | Result |
|-----------|-------------|--------|
| Message retention | 7 days | -- |
| Daily volume (Kafka) | 500K msgs/sec x 1 KB x 86,400 | **43.2 TB/day** |
| 7-day retention (RF=3) | 43.2 TB x 7 x 3 | **907 TB** |
| Subscription metadata (MySQL) | 500K subscriptions x 2 KB | **1 GB** |
| Delivery tracking (for push) | 25M deliveries/sec x 200 bytes x 86,400 | **432 TB/day** (track only failures) |
| Delivery tracking (failures only, 0.1%) | 25K failures/sec x 200 bytes x 86,400 | **432 GB/day** |

### Bandwidth Estimates

| Path | Calculation | Result |
|------|-------------|--------|
| Publishers -> Kafka | 500 MB/s | 4 Gbps |
| Kafka -> Fan-out service (pull) | 500 MB/s (one consumer group) | 4 Gbps |
| Fan-out service -> Subscriber endpoints (push) | 15M x 1 KB = 15 GB/s | 120 Gbps (distributed) |
| Fan-out service fleet NIC requirement | 120 Gbps / 25 Gbps per server | **5+ servers** (NIC-bound) |

---

## 3. High Level Architecture

```
Publishers (services)
    |
    | Publish API (gRPC / REST)
    v
+---+-------------------+
| Publish Service       |
| (stateless, HA)       |
|                       |
| - Validate message    |
| - Add message ID      |
| - Route to Kafka      |
+---+-------------------+
    |
    | Kafka produce (topic partition)
    v
+---+-------------------+     +------------------------+
| Kafka Cluster         |     | Subscription           |
| (message store)       |     | Metadata Store         |
|                       |     | (MySQL + Redis cache)  |
| Topics:               |     |                        |
|  infra.provisioning   |     | topic -> [subscription]|
|  k8s.pod.events       |     | subscription -> config |
|  alerts.critical      |     |  - delivery_mode       |
|                       |     |  - endpoint_url        |
+---+-------+-----------+     |  - filter_expression   |
    |       |                 |  - ordering_key        |
    |       |                 +--------+---------------+
    |       |                          |
    v       v                          |
+---+---+ +-+----------+              |
| Pull  | | Fan-out    |<-------------+
| Subs  | | Service    |
| (Kafka| | (push      |
| CG)   | |  delivery) |
|       | |            |
|       | | For each   |
|       | | subscription|
|       | |  per message:|
|       | |  1. Evaluate |
|       | |     filter  |
|       | |  2. HTTP POST|
|       | |     to sub  |
|       | |     endpoint|
|       | |  3. Track   |
|       | |     delivery|
+-------+ +------+-----+
                  |
          +-------+--------+
          |                |
          v                v
  +-------+------+  +-----+--------+
  | Push         |  | Delivery     |
  | Subscriber   |  | Tracker      |
  | Endpoints    |  | (acks, retries)|
  | (webhooks)   |  |              |
  +--------------+  +--------------+

Broadcast Fan-out (1M subscribers):
+-------------------+     +--------------------+
| Kafka Topic       |     | Fan-out Service    |
| alerts.broadcast  +---->| (partitioned by    |
|                   |     |  subscriber range) |
+-------------------+     |                    |
                          | Shard 1: sub 0-9999|
                          | Shard 2: sub 10000-|
                          |          19999     |
                          | ...                |
                          | Shard N: sub 990000|
                          |          -999999   |
                          +----+--------+------+
                               |        |
                          HTTP POST  HTTP POST
                               |        |
                          Sub endpoint  Sub endpoint
```

### Component Roles

| Component | Role |
|-----------|------|
| **Publish Service** | Receives publish requests; validates; assigns message ID; produces to Kafka |
| **Kafka Cluster** | Durable message store; handles ordering and retention |
| **Subscription Metadata Store** | MySQL + Redis: stores subscription configs (endpoint, filter, ordering) |
| **Fan-out Service** | Consumes from Kafka; for each message, evaluates filters and delivers to matching push subscribers |
| **Pull Subscribers** | Standard Kafka consumers (one consumer group per subscription) |
| **Delivery Tracker** | Records delivery status per (message, subscription); manages retries for failed deliveries |
| **Push Subscriber Endpoints** | External HTTP/gRPC endpoints that receive webhook deliveries |

### Data Flows

**Publish Flow:**
1. Publisher sends `PublishRequest(topic, message, attributes)`.
2. Publish Service validates message, generates `message_id` (UUIDv7).
3. Publish Service produces to Kafka topic with attributes as headers.
4. Kafka returns offset; Publish Service returns `message_id` to publisher.

**Push Delivery Flow:**
1. Fan-out Service consumes message from Kafka.
2. Loads subscriptions for this topic (from Redis cache).
3. For each subscription with `delivery_mode=PUSH`:
   a. Evaluates filter expression against message attributes.
   b. If match: enqueues HTTP delivery task.
4. Delivery worker sends HTTP POST to subscriber endpoint.
5. Subscriber returns 2xx -> delivery success; commit.
6. Subscriber returns 4xx/5xx or timeout -> retry with backoff.
7. After max retries -> deliver to subscription's DLQ.

**Pull Delivery Flow:**
1. Pull subscriber creates a consumer group named `sub-{subscription_id}`.
2. Consumer polls Kafka topic.
3. Client-side filter: evaluates filter expression, skips non-matching messages.
4. Processes message and commits offset.

---

## 4. Data Model

### Core Entities & Schema

**Topic:**
```sql
CREATE TABLE topics (
    topic_id            VARCHAR(255) PRIMARY KEY,      -- e.g., "infra.provisioning"
    display_name        VARCHAR(255) NOT NULL,
    description         TEXT,
    message_retention   INT DEFAULT 604800,            -- seconds (7 days)
    max_message_size    INT DEFAULT 1048576,           -- bytes (1 MB)
    ordering_enabled    BOOLEAN DEFAULT FALSE,
    schema_id           INT NULL,                      -- optional Schema Registry reference
    owner_team          VARCHAR(100) NOT NULL,
    created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_owner_team (owner_team)
);
```

**Subscription:**
```sql
CREATE TABLE subscriptions (
    subscription_id     VARCHAR(255) PRIMARY KEY,      -- e.g., "sub-monitoring-dashboard"
    topic_id            VARCHAR(255) NOT NULL,
    display_name        VARCHAR(255) NOT NULL,
    delivery_mode       ENUM('PUSH', 'PULL') NOT NULL,
    
    -- Push delivery config
    push_endpoint       VARCHAR(2048) NULL,            -- webhook URL
    push_auth_type      ENUM('NONE', 'BEARER', 'BASIC', 'HMAC') DEFAULT 'NONE',
    push_auth_secret    VARCHAR(512) NULL,             -- encrypted
    push_timeout_ms     INT DEFAULT 10000,
    push_batch_size     INT DEFAULT 1,                 -- batch deliveries
    
    -- Filter config
    filter_expression   TEXT NULL,                     -- e.g., "attributes.severity = 'CRITICAL'"
    
    -- Ordering config
    ordering_key        VARCHAR(255) NULL,             -- message attribute key for ordering
    
    -- Retry config
    max_retries         INT DEFAULT 5,
    retry_backoff_ms    VARCHAR(255) DEFAULT '1000,5000,30000,60000,300000',
    
    -- DLQ config
    dlq_topic_id        VARCHAR(255) NULL,
    
    -- TTL
    ack_deadline_sec    INT DEFAULT 60,                -- max time to ack before redeliver
    message_ttl_sec     INT DEFAULT 604800,            -- messages older than this are skipped
    
    -- Status
    status              ENUM('ACTIVE', 'PAUSED', 'DELETED') DEFAULT 'ACTIVE',
    created_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at          TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    
    FOREIGN KEY (topic_id) REFERENCES topics(topic_id),
    INDEX idx_topic_status (topic_id, status)
);
```

**Published Message (Kafka Record):**
```
Key:    ordering_key (if specified) or null
Value:  message payload (bytes)
Headers:
    message_id:        "01912345-abcd-7000-8000-000000000001"  (UUIDv7)
    publish_timestamp: "1712620800000"
    publisher_id:      "provisioning-service"
    attr.severity:     "CRITICAL"
    attr.region:       "us-east-1"
    attr.resource_type: "bare-metal"
    attr.action:       "provision_failed"
    content_type:      "application/json"
```

**Delivery Status (for push subscriptions, stored in Redis):**
```
Key:    delivery:{subscription_id}:{message_id}
Value:  {
            "status": "PENDING|DELIVERED|FAILED|EXPIRED",
            "attempts": 2,
            "last_attempt_at": "2026-04-09T10:00:05Z",
            "last_status_code": 503,
            "last_error": "Connection timeout",
            "next_retry_at": "2026-04-09T10:00:35Z"
        }
TTL:    7 days (auto-expire)
```

### Database Selection

| Data | Storage | Rationale |
|------|---------|-----------|
| Messages | Kafka topics | Durable, ordered, high throughput |
| Topic metadata | MySQL | Relational; admin queries; low volume |
| Subscription metadata | MySQL + Redis cache | MySQL for durability; Redis for hot path lookups |
| Delivery status (push) | Redis (with Kafka DLQ for failures) | Low-latency tracking; auto-expiring; DLQ for permanent failures |
| Pull subscriber offsets | Kafka `__consumer_offsets` | Built-in offset management |

### Indexing Strategy

| Store | Index | Purpose |
|-------|-------|---------|
| MySQL | `subscriptions(topic_id, status)` | Fast lookup: all active subscriptions for a topic |
| MySQL | `topics(owner_team)` | Admin: list topics by team |
| Redis | `topic:{topic_id}:subscriptions` (SET) | Fan-out service: get all subscription IDs for a topic |
| Redis | `sub:{subscription_id}:config` (HASH) | Fan-out service: get subscription config (endpoint, filter) |

---

## 5. API Design

### Producer/Consumer APIs

**Publish API (gRPC):**
```protobuf
service PubSubService {
    // Publish a message to a topic
    rpc Publish(PublishRequest) returns (PublishResponse);
    
    // Pull messages from a subscription
    rpc Pull(PullRequest) returns (PullResponse);
    
    // Acknowledge messages (for pull subscribers)
    rpc Acknowledge(AcknowledgeRequest) returns (AcknowledgeResponse);
    
    // Modify ack deadline (extend processing time)
    rpc ModifyAckDeadline(ModifyAckDeadlineRequest) returns (ModifyAckDeadlineResponse);
    
    // Streaming pull (bidirectional streaming)
    rpc StreamingPull(stream StreamingPullRequest) returns (stream StreamingPullResponse);
}

message PublishRequest {
    string topic_id = 1;
    repeated PubSubMessage messages = 2;
}

message PubSubMessage {
    bytes data = 1;
    map<string, string> attributes = 2;   // for filtering
    string ordering_key = 3;               // for ordered delivery
}

message PublishResponse {
    repeated string message_ids = 1;       // assigned by server
}

message PullRequest {
    string subscription_id = 1;
    int32 max_messages = 2;                // max messages to return
}

message PullResponse {
    repeated ReceivedMessage received_messages = 1;
}

message ReceivedMessage {
    string ack_id = 1;                     // used for acknowledgment
    PubSubMessage message = 2;
    string message_id = 3;
    string publish_time = 4;
    int32 delivery_attempt = 5;
}

message AcknowledgeRequest {
    string subscription_id = 1;
    repeated string ack_ids = 2;
}
```

**Publish (REST equivalent):**
```
POST /v1/topics/{topic_id}/publish
Content-Type: application/json
{
  "messages": [
    {
      "data": "base64-encoded-payload",
      "attributes": {
        "severity": "CRITICAL",
        "region": "us-east-1",
        "resource_type": "bare-metal"
      },
      "ordering_key": "server-42"
    }
  ]
}
Response: {
  "message_ids": ["01912345-abcd-7000-8000-000000000001"]
}
```

**Pull (REST):**
```
POST /v1/subscriptions/{subscription_id}/pull
{
  "max_messages": 100
}
Response: {
  "received_messages": [
    {
      "ack_id": "ack-uuid-1",
      "message": {
        "data": "base64-encoded-payload",
        "attributes": {"severity": "CRITICAL"},
        "message_id": "01912345-abcd-7000-8000-000000000001",
        "publish_time": "2026-04-09T10:00:00Z"
      },
      "delivery_attempt": 1
    }
  ]
}

POST /v1/subscriptions/{subscription_id}/acknowledge
{
  "ack_ids": ["ack-uuid-1", "ack-uuid-2"]
}
Response: {}
```

**Java Publisher:**
```java
@Service
public class InfraNotificationPublisher {
    
    private final KafkaTemplate<String, byte[]> kafkaTemplate;
    
    public String publish(String topicId, byte[] data, Map<String, String> attributes,
                          String orderingKey) {
        String messageId = UUIDv7.generate().toString();
        
        ProducerRecord<String, byte[]> record = new ProducerRecord<>(
            topicId,
            null,                           // partition (auto from key)
            orderingKey,                    // key for ordering
            data                            // payload
        );
        
        // Add attributes as headers
        record.headers().add("message_id", messageId.getBytes());
        record.headers().add("publish_timestamp", 
            String.valueOf(System.currentTimeMillis()).getBytes());
        attributes.forEach((k, v) -> 
            record.headers().add("attr." + k, v.getBytes()));
        
        kafkaTemplate.send(record).get(5, TimeUnit.SECONDS);
        return messageId;
    }
}
```

**Java Pull Subscriber:**
```java
@Service
public class InfraEventSubscriber {
    
    private final KafkaConsumer<String, byte[]> consumer;
    private final SubscriptionFilter filter;
    
    public InfraEventSubscriber(String subscriptionId, String topicId, 
                                 String filterExpression) {
        Properties props = new Properties();
        props.put(ConsumerConfig.GROUP_ID_CONFIG, "sub-" + subscriptionId);
        props.put(ConsumerConfig.ENABLE_AUTO_COMMIT_CONFIG, false);
        // ... other consumer configs
        
        this.consumer = new KafkaConsumer<>(props);
        this.consumer.subscribe(List.of(topicId));
        this.filter = new SubscriptionFilter(filterExpression);
    }
    
    public void pollAndProcess() {
        ConsumerRecords<String, byte[]> records = consumer.poll(Duration.ofMillis(100));
        
        for (ConsumerRecord<String, byte[]> record : records) {
            Map<String, String> attributes = extractAttributes(record.headers());
            
            // Client-side filter evaluation
            if (!filter.matches(attributes)) {
                continue;  // Skip non-matching messages
            }
            
            processNotification(record);
        }
        
        consumer.commitSync();
    }
}
```

### Admin APIs

```
# Topic management
POST   /v1/topics
GET    /v1/topics
GET    /v1/topics/{topic_id}
PATCH  /v1/topics/{topic_id}
DELETE /v1/topics/{topic_id}

# Subscription management
POST   /v1/topics/{topic_id}/subscriptions
GET    /v1/topics/{topic_id}/subscriptions
GET    /v1/subscriptions/{subscription_id}
PATCH  /v1/subscriptions/{subscription_id}
DELETE /v1/subscriptions/{subscription_id}

# Subscription operations
POST   /v1/subscriptions/{subscription_id}/pause
POST   /v1/subscriptions/{subscription_id}/resume
POST   /v1/subscriptions/{subscription_id}/seek     // reset offset

# Metrics
GET    /v1/subscriptions/{subscription_id}/metrics
Response: {
  "subscription_id": "sub-monitoring-dashboard",
  "backlog_messages": 1234,
  "oldest_unacked_message_age_sec": 45,
  "delivery_rate_per_sec": 500,
  "push_success_rate": 0.997,
  "push_avg_latency_ms": 45
}
```

### CLI

```bash
# Publish
pubsub publish infra.provisioning \
    --data '{"server_id":"srv-42","status":"provision_failed"}' \
    --attributes severity=CRITICAL,region=us-east-1

# Subscribe (pull)
pubsub subscribe infra.provisioning \
    --name sub-oncall-dashboard \
    --delivery-mode pull \
    --filter "attributes.severity = 'CRITICAL'"

# Subscribe (push)
pubsub subscribe infra.provisioning \
    --name sub-slack-alerts \
    --delivery-mode push \
    --endpoint https://hooks.slack.com/services/T00/B00/xxxx \
    --auth-type BEARER --auth-secret $SLACK_TOKEN \
    --filter "attributes.severity IN ('CRITICAL', 'HIGH')"

# List subscriptions
pubsub subscriptions list --topic infra.provisioning

# Check backlog
pubsub subscriptions metrics sub-oncall-dashboard

# Seek to timestamp (replay)
pubsub subscriptions seek sub-oncall-dashboard --timestamp "2026-04-09T00:00:00Z"
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Fan-out Architecture (Message Broker Fan-out vs Application-Level Fan-out)

**Why it's hard:**
Delivering one message to 1 million subscribers requires 1 million HTTP calls. At 10ms per call (sequential), that's 2.7 hours. The fan-out must be massively parallel while handling subscriber failures, respecting rate limits, maintaining ordering per ordering key, and not overwhelming the network.

**Approaches:**

| Approach | Latency | Scalability | Complexity | Resource Cost |
|----------|---------|-------------|------------|--------------|
| Kafka consumer group per subscription | Low (Kafka native) | Limited by partition count | Low | High (1M consumer groups impossible) |
| Application-level fan-out (single consumer -> HTTP workers) | Medium | Very high | High | Medium |
| **Tiered fan-out (broker + application)** | **Low for small, Medium for large** | **Very high** | **High** | **Optimized** |
| Dedicated push broker (custom) | Lowest | High | Very high | High |

**Selected: Tiered fan-out**

**Architecture:**
```
Tier 1: Kafka-level fan-out (for pull subscribers, <= 1000 per topic)
  Each pull subscription = one Kafka consumer group
  Direct Kafka pull, lowest latency, no fan-out service needed
  
  Limit: Kafka manages ~10,000 consumer groups efficiently
  For 10,000 topics x 50 pull subscriptions = 500,000 consumer groups (feasible)

Tier 2: Application-level fan-out (for push subscribers)
  One Kafka consumer group per topic: "fanout-{topic_id}"
  Fan-out service reads message, loads subscription list, dispatches HTTP calls
  
  For small fan-out (50 push subscribers per topic):
    Single fan-out consumer per topic, 50 HTTP calls per message
    At 1000 msgs/sec: 50,000 HTTP calls/sec (easily parallelized)
  
  For large fan-out (1M push subscribers):
    Dedicated fan-out service with sharded subscriber lists
    See "Broadcast Fan-out" below
```

**Small fan-out implementation (50 subscribers):**
```java
@Service
public class FanoutService {
    
    private final SubscriptionCache subscriptionCache;  // Redis-backed
    private final HttpDeliveryPool deliveryPool;        // async HTTP client pool
    private final DeliveryTracker deliveryTracker;
    
    @KafkaListener(topicPattern = "infra\\..*", groupId = "fanout-service",
                   concurrency = "32")
    public void fanout(ConsumerRecord<String, byte[]> record, Acknowledgment ack) {
        String topicId = record.topic();
        Map<String, String> attributes = extractAttributes(record.headers());
        String messageId = extractHeader(record, "message_id");
        
        // Load active push subscriptions for this topic
        List<Subscription> pushSubs = subscriptionCache.getPushSubscriptions(topicId);
        
        // Fan-out: evaluate filter and deliver
        List<CompletableFuture<DeliveryResult>> futures = new ArrayList<>();
        
        for (Subscription sub : pushSubs) {
            // Evaluate filter
            if (sub.getFilter() != null && !evaluateFilter(sub.getFilter(), attributes)) {
                continue;  // Skip: message doesn't match filter
            }
            
            // Async HTTP delivery
            CompletableFuture<DeliveryResult> future = deliveryPool.deliver(
                sub.getEndpoint(),
                buildWebhookPayload(record, messageId, sub),
                sub.getAuthConfig(),
                sub.getTimeoutMs()
            );
            
            futures.add(future.whenComplete((result, error) -> {
                if (error != null || !result.isSuccess()) {
                    // Schedule retry
                    deliveryTracker.scheduleRetry(sub.getId(), messageId, record);
                } else {
                    deliveryTracker.markDelivered(sub.getId(), messageId);
                }
            }));
        }
        
        // Wait for all deliveries to complete (or timeout)
        CompletableFuture.allOf(futures.toArray(new CompletableFuture[0]))
            .orTimeout(30, TimeUnit.SECONDS)
            .whenComplete((v, e) -> ack.acknowledge());
    }
}
```

**Broadcast fan-out (1M subscribers):**
```python
class BroadcastFanoutService:
    """
    Handles fan-out to 1M+ subscribers using sharded worker pool.
    
    Architecture:
      1. Primary consumer reads message from broadcast topic
      2. Writes (message_id, message) to a "fanout task" topic with N partitions
      3. Each partition handles a range of subscriber IDs
      4. Fan-out workers consume from their partition, deliver to their subscriber shard
    
    Why sharding:
      1M HTTP calls / 50ms avg = 50,000 concurrent connections needed
      Single machine: ~10,000 concurrent connections (practical limit)
      Need 5+ fan-out workers minimum
      Shard by subscriber_id range for locality (same worker always handles same subscribers)
    """
    
    def __init__(self, num_shards=100):
        self.num_shards = num_shards
        # Each shard handles 1M / 100 = 10,000 subscribers
    
    def dispatch_fanout(self, message_id, topic_id, message):
        """Primary consumer: dispatch fan-out tasks to shards."""
        
        for shard_id in range(self.num_shards):
            # Produce a fan-out task for each shard
            task = FanoutTask(
                message_id=message_id,
                topic_id=topic_id,
                message=message,
                shard_id=shard_id
            )
            self.kafka_producer.send(
                topic="fanout-tasks",
                key=str(shard_id),  # partition by shard
                value=serialize(task)
            )
    
    def process_shard(self, task):
        """Shard worker: deliver to all subscribers in this shard."""
        
        subscribers = self.load_shard_subscribers(task.topic_id, task.shard_id)
        # subscribers: list of 10,000 (endpoint, auth, filter)
        
        # Batch HTTP delivery using async HTTP client
        batch_size = 100
        for batch in chunked(subscribers, batch_size):
            futures = []
            for sub in batch:
                if sub.filter and not evaluate_filter(sub.filter, task.message.attributes):
                    continue
                future = self.http_client.post_async(
                    url=sub.endpoint,
                    body=build_webhook_payload(task.message),
                    headers=build_auth_headers(sub.auth),
                    timeout_ms=sub.timeout_ms
                )
                futures.append((sub.id, future))
            
            # Collect results
            for sub_id, future in futures:
                try:
                    response = future.result(timeout=15)
                    if response.status_code >= 400:
                        self.schedule_retry(sub_id, task.message_id)
                except Exception:
                    self.schedule_retry(sub_id, task.message_id)

# Performance math for 1M subscriber broadcast:
#   100 shards x 10,000 subscribers each
#   Per shard: 10,000 HTTP calls / 100 batch = 100 batches
#   Per batch: 100 concurrent HTTP calls x 50ms = 50ms
#   Total per shard: 100 batches x 50ms = 5 seconds
#   With 100 parallel shard workers: 5 seconds wall-clock time
#   Total: 1M deliveries in ~5 seconds (well within 30s target)
```

**Failure Modes:**
- **Subscriber endpoint down:** Retry with exponential backoff. After max retries, DLQ. Track per-subscriber health score; auto-pause consistently failing subscriptions.
- **Fan-out service crash:** Kafka consumer group rebalances; other instances pick up partitions. At-least-once delivery (some messages may be re-delivered).
- **Network saturation:** 1M HTTP calls generate 1 GB of outbound traffic per message. Solution: subscriber batching (deliver 10 messages per HTTP call), compression.
- **Subscriber rate limiting:** Subscriber returns 429. Respect Retry-After header. Adaptive rate limiting per subscriber.

**Interviewer Q&As:**

**Q1: How does Google Cloud Pub/Sub handle 1M subscribers?**
A: Google Cloud Pub/Sub uses a multi-layer fan-out architecture: (1) Forwarders distribute messages to multiple delivery zones. (2) Each zone has a pool of delivery workers assigned to subscriber ranges. (3) Push delivery is separate from the core message store. (4) Subscriber endpoints are health-checked; consistently failing endpoints are throttled. They published that they handle 10M messages/sec with sub-100ms latency.

**Q2: Why not use one Kafka consumer group per push subscription?**
A: With 500,000 push subscriptions, you'd need 500,000 consumer groups, each consuming every partition of every topic they subscribe to. Kafka can handle ~10,000-50,000 consumer groups before coordinator memory and rebalancing overhead becomes problematic. Application-level fan-out decouples the number of subscriptions from Kafka consumer group count.

**Q3: How do you handle ordering with fan-out?**
A: For pull subscriptions: Kafka's per-partition ordering is preserved (ordering key = partition key). For push subscriptions: the fan-out service delivers sequentially per ordering key (same key always routed to same delivery worker). Concurrent delivery for different ordering keys; sequential for the same key.

**Q4: How do you handle a subscriber that is much slower than others?**
A: Per-subscriber delivery is independent. A slow subscriber doesn't block other subscribers. The fan-out service tracks delivery state per (message_id, subscription_id) in Redis. Slow subscribers accumulate a backlog; their delivery runs behind. Alert if backlog exceeds threshold.

**Q5: How do you handle large messages (10 MB) with fan-out?**
A: For push delivery of large messages: (1) Message stored in Kafka (supports up to 10 MB with config). (2) Push webhook includes a reference URL instead of the full payload: `{"message_id": "...", "data_url": "https://pubsub.internal/messages/..."}`. (3) Subscriber downloads the full payload via the URL. This avoids sending 10 MB x 1M subscribers = 10 TB per message.

**Q6: What is the cost of push vs pull delivery?**
A: Push: infrastructure cost is on the publisher side (fan-out service fleet, outbound bandwidth). Better for real-time notifications; subscriber is passive. Pull: infrastructure cost is on the subscriber side (consumer instances, inbound bandwidth). Better for batch processing; subscriber controls throughput. Push is more complex to operate (retry logic, health checking, rate limiting).

---

### Deep Dive 2: Subscription Filtering

**Why it's hard:**
Evaluating filter expressions for every message against every subscription must be extremely fast. With 500K msgs/sec and 50 subscriptions per topic, that's 25M filter evaluations/sec. Filters must support rich expressions (equality, comparison, IN, regex) while being evaluated in < 1 ms.

**Approaches:**

| Approach | Flexibility | Performance | Complexity |
|----------|------------|-------------|------------|
| Separate Kafka topic per filter criteria | Low (static filters) | Highest (no runtime eval) | Low |
| Client-side filtering (pull subscribers skip non-matching) | Full | Poor (wastes bandwidth) | Low |
| **Server-side attribute filtering** | **High** | **Good (indexed)** | **Medium** |
| CEL (Common Expression Language) evaluation | Very High | Good | High |

**Selected: Server-side attribute filtering with compiled filter expressions**

**Filter expression syntax:**
```
# Simple equality
attributes.severity = "CRITICAL"

# Multiple conditions (AND)
attributes.severity = "CRITICAL" AND attributes.region = "us-east-1"

# IN operator
attributes.severity IN ("CRITICAL", "HIGH")

# NOT
NOT attributes.resource_type = "vm"

# Prefix match
hasPrefix(attributes.server_id, "prod-")

# EXISTS
attributes.maintenance_window EXISTS
```

**Filter evaluation engine:**
```java
public class FilterEngine {
    
    // Compiled filter for fast evaluation
    private final Map<String, CompiledFilter> compiledFilters = new ConcurrentHashMap<>();
    
    public CompiledFilter compile(String filterExpression) {
        // Parse filter expression into AST
        FilterAST ast = FilterParser.parse(filterExpression);
        
        // Optimize: extract attribute keys for fast pre-filtering
        Set<String> requiredAttributes = ast.getReferencedAttributes();
        
        // Compile to a Predicate for fast evaluation
        Predicate<Map<String, String>> predicate = ast.compileToPredicate();
        
        return new CompiledFilter(requiredAttributes, predicate);
    }
    
    public boolean matches(String subscriptionId, Map<String, String> messageAttributes) {
        CompiledFilter filter = compiledFilters.get(subscriptionId);
        if (filter == null) return true;  // No filter = match all
        
        // Quick check: do required attributes exist?
        for (String attr : filter.getRequiredAttributes()) {
            if (!messageAttributes.containsKey(attr)) {
                return false;  // Fast rejection
            }
        }
        
        // Full evaluation
        return filter.getPredicate().test(messageAttributes);
    }
}

// Performance benchmark:
//   Simple equality filter: ~50 ns per evaluation
//   Complex AND/OR filter (3 conditions): ~150 ns per evaluation
//   25M evaluations/sec at 150 ns = 3.75 seconds CPU time / sec
//   On 8-core machine: 3.75 / 8 = 47% CPU utilization (acceptable)
```

**Inverted index optimization for large fan-out:**
```python
class FilterIndex:
    """
    Inverted index: attribute value -> set of matching subscription IDs.
    
    Instead of evaluating each subscription's filter against the message,
    look up which subscriptions match based on message attributes.
    
    Trades memory for speed.
    """
    
    def __init__(self):
        # attribute_name -> value -> set of subscription_ids
        self.index = defaultdict(lambda: defaultdict(set))
        # subscription_id -> filter (for non-indexable filters)
        self.complex_filters = {}
    
    def register_subscription(self, sub_id, filter_expression):
        ast = parse(filter_expression)
        
        if ast.is_simple_equality():
            # Indexable: attributes.severity = "CRITICAL"
            attr, value = ast.get_equality()
            self.index[attr][value].add(sub_id)
        else:
            # Non-indexable: complex expression
            self.complex_filters[sub_id] = compile(filter_expression)
    
    def find_matching_subscriptions(self, message_attributes):
        """Find all subscriptions matching this message's attributes."""
        
        # Phase 1: Index lookup (O(1) per attribute)
        candidates = set()
        for attr, value in message_attributes.items():
            matching = self.index.get(attr, {}).get(value, set())
            candidates.update(matching)
        
        # Also include subscriptions with no filter (match all)
        candidates.update(self.no_filter_subscriptions)
        
        # Phase 2: Evaluate complex filters
        for sub_id, compiled_filter in self.complex_filters.items():
            if compiled_filter.evaluate(message_attributes):
                candidates.add(sub_id)
        
        return candidates

# Performance with inverted index:
#   1M subscriptions, 80% simple equality filters, 20% complex
#   Index lookup: O(1) -> finds 800K matching subs instantly
#   Complex evaluation: 200K x 150ns = 30ms
#   Total: ~30ms to find all matching subscriptions among 1M
#   vs brute force: 1M x 150ns = 150ms
```

**Failure Modes:**
- **Invalid filter expression:** Rejected at subscription creation time. Filter is parsed and compiled; errors are returned immediately.
- **Filter evaluation crash (regex timeout):** Timeout per evaluation (1ms). Subscription flagged as having a problematic filter; operator notified.
- **Stale filter cache:** Redis cache invalidated on subscription update. TTL-based refresh as backup (60s).

**Interviewer Q&As:**

**Q1: Why not use separate Kafka topics for each filter criteria?**
A: This approach (topic-per-filter) works for a small, fixed set of filters (e.g., one topic per severity level). But: (1) Filter criteria are arbitrary (region, resource type, action, etc.), leading to topic explosion. (2) Multi-attribute filters require cross-topic consumption. (3) Adding a new filter requires creating a new topic and re-routing. (4) Doesn't scale for dynamic, user-defined filters.

**Q2: How does server-side filtering affect Kafka consumer throughput?**
A: The fan-out service consumes ALL messages from the topic (no Kafka-level filtering). Filter evaluation happens after consumption. This means Kafka throughput is based on total message volume, not filtered volume. For topics where 99% of messages are filtered out for most subscribers, this wastes bandwidth. Solution: if a filter is very common (>50% of subscribers filter on same attribute), consider separate topics.

**Q3: How do you handle filter changes on an existing subscription?**
A: (1) Update filter in MySQL. (2) Invalidate Redis cache for the subscription. (3) Fan-out service loads new filter on next message. (4) For pull subscriptions: filter change takes effect on next poll. (5) No message replay needed: filter applies only to new messages.

**Q4: Can you support SQL-like filters?**
A: Yes, with a CEL (Common Expression Language) or SQL-WHERE parser. Example: `attributes.severity = 'CRITICAL' AND (attributes.region = 'us-east-1' OR attributes.region = 'us-west-2') AND attributes.amount > 1000`. Compile to a predicate once; evaluate per message. The overhead is higher than simple equality but still < 1 ms per evaluation.

**Q5: How do you test filter correctness?**
A: (1) Unit tests: filter expression -> expected match/no-match for sample attributes. (2) Integration test: publish message with known attributes, verify only matching subscriptions receive it. (3) Filter playground in admin UI: enter a filter, see which of the last 100 messages would match.

**Q6: What is the memory cost of the inverted index?**
A: For 1M subscriptions with simple equality filters: ~1M entries in the index. Each entry: ~100 bytes (attribute + value + subscription ID). Total: ~100 MB. For complex filters: ~200K compiled predicates at ~1 KB each = ~200 MB. Total: ~300 MB. Fits easily in a single fan-out service instance's memory.

---

### Deep Dive 3: Push Delivery Reliability

**Why it's hard:**
HTTP push delivery has many failure modes: subscriber endpoint down, slow, rate-limited, returning errors, network issues, DNS failures, TLS certificate issues. The system must handle all of these gracefully, retry appropriately, and guarantee at-least-once delivery while not overwhelming subscribers.

**Approaches:**

| Approach | Reliability | Complexity | Subscriber Effort |
|----------|------------|------------|------------------|
| Fire-and-forget HTTP POST | Low (no retry) | Low | Low |
| **Retry with exponential backoff + DLQ** | **High** | **Medium** | **Low** |
| Acknowledgment-based (subscriber confirms) | Highest | High | Medium |
| Webhook + verification callback | Highest | High | High |

**Selected: Retry with exponential backoff + subscriber health scoring**

**Push delivery with idempotency:**
```java
@Service
public class PushDeliveryService {
    
    private final AsyncHttpClient httpClient;
    private final SubscriberHealthTracker healthTracker;
    private final RetryScheduler retryScheduler;
    
    public CompletableFuture<DeliveryResult> deliver(
            Subscription sub, PubSubMessage message, String messageId) {
        
        // Build webhook payload
        WebhookPayload payload = WebhookPayload.builder()
            .messageId(messageId)            // Idempotency key for subscriber
            .subscriptionId(sub.getId())
            .publishTime(message.getPublishTime())
            .data(message.getData())
            .attributes(message.getAttributes())
            .deliveryAttempt(1)
            .build();
        
        // Build request with auth
        BoundRequestBuilder request = httpClient.preparePost(sub.getEndpoint())
            .setHeader("Content-Type", "application/json")
            .setHeader("X-PubSub-Message-Id", messageId)
            .setHeader("X-PubSub-Subscription", sub.getId())
            .setHeader("X-PubSub-Delivery-Attempt", "1")
            .setBody(serialize(payload))
            .setRequestTimeout(sub.getTimeoutMs());
        
        // Add authentication
        applyAuth(request, sub.getAuthConfig());
        
        // HMAC signature for webhook verification
        String signature = hmacSha256(sub.getSigningKey(), serialize(payload));
        request.setHeader("X-PubSub-Signature", signature);
        
        return request.execute()
            .toCompletableFuture()
            .thenApply(response -> {
                int statusCode = response.getStatusCode();
                
                if (statusCode >= 200 && statusCode < 300) {
                    healthTracker.recordSuccess(sub.getId());
                    return DeliveryResult.success(messageId, sub.getId());
                } else if (statusCode == 429) {
                    // Rate limited: respect Retry-After header
                    long retryAfter = parseRetryAfter(response, 60_000);
                    retryScheduler.scheduleRetry(sub, message, messageId, 
                        1, retryAfter);
                    return DeliveryResult.retryScheduled(messageId, sub.getId());
                } else {
                    // Server error or client error
                    healthTracker.recordFailure(sub.getId(), statusCode);
                    retryScheduler.scheduleRetry(sub, message, messageId, 
                        1, calculateBackoff(1, sub.getRetryBackoffMs()));
                    return DeliveryResult.failed(messageId, sub.getId(), statusCode);
                }
            })
            .exceptionally(throwable -> {
                // Connection error, timeout, DNS failure
                healthTracker.recordFailure(sub.getId(), -1);
                retryScheduler.scheduleRetry(sub, message, messageId, 
                    1, calculateBackoff(1, sub.getRetryBackoffMs()));
                return DeliveryResult.failed(messageId, sub.getId(), -1);
            });
    }
    
    private long calculateBackoff(int attempt, List<Long> backoffSchedule) {
        int index = Math.min(attempt - 1, backoffSchedule.size() - 1);
        long baseDelay = backoffSchedule.get(index);
        // Add jitter: +/- 20%
        long jitter = (long) (baseDelay * 0.2 * (Math.random() * 2 - 1));
        return baseDelay + jitter;
    }
}
```

**Subscriber health scoring:**
```java
public class SubscriberHealthTracker {
    
    // Sliding window: last 100 deliveries per subscriber
    private final Map<String, CircularBuffer<Boolean>> deliveryHistory;
    
    public void recordSuccess(String subscriptionId) {
        deliveryHistory.computeIfAbsent(subscriptionId, 
            k -> new CircularBuffer<>(100)).add(true);
    }
    
    public void recordFailure(String subscriptionId, int statusCode) {
        deliveryHistory.computeIfAbsent(subscriptionId, 
            k -> new CircularBuffer<>(100)).add(false);
        
        double successRate = getSuccessRate(subscriptionId);
        
        if (successRate < 0.1) {
            // Less than 10% success rate: auto-pause subscription
            pauseSubscription(subscriptionId);
            alertOps("Subscription " + subscriptionId + 
                     " auto-paused: success rate " + successRate);
        } else if (successRate < 0.5) {
            // Less than 50%: reduce delivery rate
            applyBackpressure(subscriptionId);
        }
    }
    
    public double getSuccessRate(String subscriptionId) {
        CircularBuffer<Boolean> history = deliveryHistory.get(subscriptionId);
        if (history == null || history.size() < 10) return 1.0;
        long successes = history.stream().filter(b -> b).count();
        return (double) successes / history.size();
    }
}
```

**Subscriber-side idempotency:**
```java
// Subscriber webhook handler (receiver side)
@PostMapping("/webhook/notifications")
public ResponseEntity<Void> receiveNotification(
        @RequestBody WebhookPayload payload,
        @RequestHeader("X-PubSub-Message-Id") String messageId,
        @RequestHeader("X-PubSub-Signature") String signature) {
    
    // 1. Verify HMAC signature
    if (!verifySignature(signingKey, payload, signature)) {
        return ResponseEntity.status(403).build();
    }
    
    // 2. Idempotency check: have we processed this message?
    boolean isNew = redis.opsForValue().setIfAbsent(
        "processed:" + messageId, "1", Duration.ofDays(7));
    
    if (!isNew) {
        // Already processed: return 200 (don't trigger retry)
        return ResponseEntity.ok().build();
    }
    
    // 3. Process notification
    try {
        processNotification(payload);
        return ResponseEntity.ok().build();
    } catch (Exception e) {
        // Return 500 to trigger retry
        redis.delete("processed:" + messageId);  // Allow reprocessing
        return ResponseEntity.status(500).build();
    }
}
```

**Failure Modes:**
- **Subscriber permanently down:** Health score drops to 0. Subscription auto-paused. Alert to subscription owner. Messages accumulate in backlog (Kafka retains). When subscriber recovers, unpause and drain backlog.
- **Subscriber returns 200 but doesn't process:** The pub/sub system considers delivery successful. This is a subscriber-side bug. Mitigation: subscriber-side acknowledgment model (subscriber must call back to confirm processing). More complex but more reliable.
- **DNS failure for subscriber endpoint:** HTTP client cannot resolve hostname. Retry with backoff. If persistent (> 1 hour), auto-pause and alert.

**Interviewer Q&As:**

**Q1: How does the subscriber verify that a webhook is authentic?**
A: HMAC-SHA256 signature. The pub/sub system signs the payload with a shared secret (configured per subscription). The subscriber verifies the signature before processing. This prevents spoofed webhook calls. Alternative: mTLS between pub/sub and subscriber.

**Q2: What is the difference between "at-least-once" and "exactly-once" push delivery?**
A: At-least-once: the pub/sub system may deliver the same message multiple times (on retry after timeout, on consumer restart). Exactly-once: each message is delivered exactly once. True exactly-once push delivery is extremely hard (requires 2PC with the subscriber). Practical approach: at-least-once delivery with idempotency keys. The subscriber uses the message_id to deduplicate.

**Q3: How do you handle a subscriber that is slow (takes 5 seconds per request)?**
A: (1) Increase `push_timeout_ms` for that subscription. (2) Use batched delivery: send 10 messages per webhook call. (3) Use a separate delivery worker pool for slow subscribers (prevent blocking fast subscribers). (4) If persistently slow: alert the subscriber owner to optimize their endpoint.

**Q4: How do you prevent the fan-out service from overwhelming a subscriber?**
A: Per-subscriber rate limiting. Default: 100 requests/sec per subscriber. Configurable per subscription. If the subscriber returns 429 with Retry-After, respect it. If the delivery rate exceeds the rate limit, buffer messages and deliver at the subscriber's pace.

**Q5: What happens if the fan-out service crashes during delivery?**
A: Kafka consumer group rebalances. Another fan-out instance picks up the partitions. The new instance re-reads messages from the last committed offset. Messages that were delivered but not committed are re-delivered (at-least-once). Messages that were not yet delivered are delivered normally.

**Q6: How do you handle webhook endpoint URL changes?**
A: Subscription update API allows changing the endpoint URL. The fan-out service loads subscription config from Redis cache, which is invalidated on update. No message loss: messages in-flight are delivered to the old endpoint; new messages go to the new endpoint. For zero-downtime migration: subscriber should support both old and new endpoints briefly.

---

## 7. Scheduling & Resource Management

### Fan-out Service Resource Planning

| Component | Resource | Sizing |
|-----------|----------|--------|
| Fan-out service (small fan-out) | 16 pods x 4 CPU x 8 GB | 32 concurrent Kafka consumers, 5000 outbound HTTP/sec per pod |
| Fan-out service (broadcast) | 100 pods x 4 CPU x 8 GB | One pod per shard, 10,000 HTTP/sec per pod |
| HTTP connection pool | 10,000 connections per pod | Reuse connections to same endpoint |
| Retry scheduler | 4 pods x 2 CPU x 4 GB | Redis-backed delayed retry queue |
| Subscription cache (Redis) | 3 nodes x 4 GB | All subscription configs cached |

### Fan-out Worker Assignment

```
Per-topic fan-out worker assignment:
  
  Low-volume topics (< 100 msgs/sec, < 50 subscribers):
    Shared fan-out workers (multi-topic consumer)
    1 worker handles 100+ topics
  
  High-volume topics (> 1000 msgs/sec or > 100 subscribers):
    Dedicated fan-out workers
    Kafka partitions = subscriber count / batch parallelism
  
  Broadcast topics (> 10K subscribers):
    Dedicated broadcast fan-out cluster
    Subscriber list sharded across workers
    Each worker handles ~10K subscribers
```

### Kubernetes Deployment

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: fanout-service
spec:
  replicas: 16
  template:
    spec:
      containers:
        - name: fanout
          image: infra/fanout-service:1.0
          resources:
            requests:
              cpu: "4"
              memory: "8Gi"
            limits:
              cpu: "4"
              memory: "8Gi"
          env:
            - name: KAFKA_BOOTSTRAP_SERVERS
              value: "broker1:9092,broker2:9092"
            - name: REDIS_URL
              value: "redis-cluster:6379"
            - name: HTTP_CONNECTION_POOL_SIZE
              value: "10000"
            - name: MAX_CONCURRENT_DELIVERIES
              value: "5000"
      affinity:
        podAntiAffinity:
          preferredDuringSchedulingIgnoredDuringExecution:
            - weight: 100
              podAffinityTerm:
                labelSelector:
                  matchLabels:
                    app: fanout-service
                topologyKey: kubernetes.io/hostname
```

---

## 8. Scaling Strategy

### Scaling Dimensions

| Dimension | Mechanism | Limit |
|-----------|----------|-------|
| Published message throughput | Add Kafka partitions + brokers | Linear with partitions |
| Subscriber count per topic | Add fan-out service pods | Linear with pods |
| Topic count | Add fan-out consumers (shared) | ~100,000 topics |
| Push delivery throughput | Add fan-out workers + scale HTTP pool | Linear with workers |
| Pull subscriber throughput | Standard Kafka consumer scaling | 1 consumer per partition |
| Subscription metadata | MySQL read replicas + Redis cache | Millions of subscriptions |

### Auto-Scaling Configuration

```yaml
# HPA for fan-out service based on outbound HTTP queue depth
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: fanout-service-hpa
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: fanout-service
  minReplicas: 8
  maxReplicas: 64
  metrics:
    - type: Pods
      pods:
        metric:
          name: fanout_pending_deliveries
        target:
          type: AverageValue
          averageValue: "5000"   # Scale up if > 5000 pending per pod
    - type: Pods
      pods:
        metric:
          name: fanout_delivery_latency_p99_ms
        target:
          type: AverageValue
          averageValue: "100"   # Scale up if p99 > 100ms
```

**Interviewer Q&As:**

**Q1: How do you scale from 50 to 1 million subscribers for a topic?**
A: (1) At 50 subscribers: single fan-out consumer per topic, direct HTTP delivery. (2) At 1000 subscribers: increase fan-out consumer concurrency; parallel HTTP calls. (3) At 10,000 subscribers: shard subscriber list across multiple fan-out workers. (4) At 1M subscribers: dedicated broadcast fan-out cluster with 100+ sharded workers, batched delivery, subscriber health scoring.

**Q2: What is the bottleneck for push delivery at scale?**
A: Outbound network bandwidth. At 1M deliveries x 1 KB = 1 GB per fan-out message. With 100 messages/sec on a broadcast topic: 100 GB/s outbound. A single 25 Gbps NIC handles 3.1 GB/s. Need 32+ fan-out servers just for network. Solution: batched delivery (10 messages per HTTP call reduces connection overhead 10x), compression (gzip reduces payload 60-80%).

**Q3: How do you handle thundering herd on subscriber endpoints?**
A: When a broadcast message is published, all 1M subscribers receive it nearly simultaneously. Solution: (1) Jitter: random delay (0-5s) per delivery to spread the load. (2) Per-subscriber rate limiting. (3) Batching: accumulate messages and deliver in batches at regular intervals.

**Q4: How do you scale the subscription metadata store?**
A: MySQL handles subscription CRUD (low volume). Redis caches subscription configs for hot-path lookups (every message delivery). At 500K subscriptions: Redis uses ~500 MB (1 KB per subscription). Redis cluster scales horizontally. For topic-to-subscription lookups: Redis SET per topic, efficient for topics with up to 10K subscriptions. For broadcast topics (1M subscribers): shard the subscriber list across Redis keys.

**Q5: How do you handle a topic with 100,000 messages/sec and 100 push subscribers?**
A: Total deliveries: 10M/sec. Per fan-out pod (5000 deliveries/sec): need 2000 pods? No -- batch! If batch_size=10: 1M HTTP calls/sec. Per pod: 50,000 HTTP calls/sec with async HTTP client. Need 20 pods. Plus: most messages are filtered out (subscribers only match 10% of messages) -> 2 pods suffice.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Detection | Impact | Mitigation | RTO |
|---------|-----------|--------|------------|-----|
| Publish service crash | Load balancer health check | Publish requests fail | Multiple instances behind LB; publisher retries | < 5s |
| Kafka broker failure | ISR mechanism | Some partitions temporarily unavailable | RF=3; leader election; producer retries | 1-10s |
| Fan-out service crash | Consumer group rebalance | Push delivery paused for affected partitions | Rebalance assigns partitions to surviving instances | 10-30s |
| Redis cache failure | Health check timeout | Subscription lookups fallback to MySQL | Redis sentinel/cluster; MySQL fallback | < 5s |
| MySQL failure | Health check timeout | Subscription management unavailable | MySQL replication; read replicas; subscription cache | Manual |
| Subscriber endpoint down | HTTP error/timeout | Deliveries fail; retry with backoff | Exponential backoff; DLQ after max retries; auto-pause | Subscriber-dependent |
| Network partition (fan-out <-> subscriber) | HTTP timeout | Deliveries fail | Retry; fan-out service in multiple AZs | Network recovery |
| Broadcast message to 1M subscribers | Normal (high load) | Potential fan-out service resource exhaustion | Dedicated broadcast cluster; rate limiting; jitter | N/A |
| Subscription metadata corruption | Data validation | Wrong endpoint/filter | MySQL backups; Redis cache invalidation; audit log | Manual |

### Delivery Semantics

| Delivery Mode | Semantic | Mechanism |
|---------------|----------|-----------|
| Pull (Kafka consumer) | At-least-once | Consumer commits offset after processing |
| Push (webhook) | At-least-once | Fan-out service retries on failure; subscriber uses idempotency key |
| Push (with subscriber ack) | At-least-once (stronger) | Subscriber must explicitly acknowledge; unacked messages redelivered |

### Idempotency Guarantee

```
Publisher -> Pub/Sub:
  Publisher includes idempotency_key in PublishRequest.
  Pub/Sub deduplicates: if same idempotency_key seen within dedup window (10 min),
  returns the existing message_id without re-publishing.
  Implementation: Redis SET NX with TTL.

Pub/Sub -> Subscriber (push):
  Each webhook includes X-PubSub-Message-Id header.
  Subscriber uses message_id as idempotency key.
  Subscriber maintains: Redis SET NX "processed:{message_id}" with 7-day TTL.
  
Pub/Sub -> Subscriber (pull):
  Message contains message_id.
  Same idempotency pattern as push.
```

---

## 10. Observability

### Key Metrics

| Category | Metric | Alert Threshold | Why |
|----------|--------|----------------|-----|
| Publish | `publish_requests_per_sec` | > 120% of expected peak | Traffic spike |
| Publish | `publish_latency_p99` | > 50 ms | Kafka slow or overloaded |
| Delivery | `push_delivery_success_rate` (per subscription) | < 95% | Subscriber issues |
| Delivery | `push_delivery_latency_p99` | > 500 ms | Subscriber slow |
| Delivery | `push_pending_deliveries` | > 100,000 | Delivery backlog growing |
| Delivery | `pull_consumer_lag` (per subscription) | > 10,000 messages | Pull subscriber falling behind |
| Fan-out | `fanout_messages_per_sec` | > 80% capacity | Approaching fan-out limit |
| Fan-out | `fanout_filter_evaluation_time` | > 5 ms | Filter complexity too high |
| Subscription | `subscriber_health_score` | < 0.5 | Subscriber consistently failing |
| Subscription | `auto_paused_subscriptions` | > 0 | Subscribers auto-paused |
| Infrastructure | Fan-out pod CPU usage | > 80% | Need more fan-out capacity |
| Infrastructure | Outbound bandwidth utilization | > 70% NIC capacity | Network bottleneck |

### Distributed Tracing

```
Trace flow:
  [Publisher] --(publish)-> [Publish Service] --(produce)-> [Kafka]
      |                                                        |
      |--- traceId: abc123 --->                               |
                                                               |
  [Fan-out Service] --(consume)-< [Kafka]
      |
      |--- traceId: abc123 (continued) --->
      |
      +---(deliver)--> [Subscriber A endpoint]
      |                    |--- traceId: abc123, spanId: sub-a
      |
      +---(deliver)--> [Subscriber B endpoint]
                           |--- traceId: abc123, spanId: sub-b

Each delivery creates a child span linked to the publish span.
Span attributes: subscription_id, delivery_attempt, status_code, latency.
```

### Logging

```json
{
  "timestamp": "2026-04-09T10:00:00.123Z",
  "level": "INFO",
  "component": "fanout-service",
  "event": "push_delivery",
  "topic_id": "infra.provisioning",
  "message_id": "01912345-abcd-7000-8000-000000000001",
  "subscription_id": "sub-monitoring-dashboard",
  "endpoint": "https://dashboard.internal/webhook",
  "status_code": 200,
  "latency_ms": 45,
  "delivery_attempt": 1,
  "filter_matched": true,
  "trace_id": "abc123"
}

{
  "timestamp": "2026-04-09T10:00:00.456Z",
  "level": "WARN",
  "component": "fanout-service",
  "event": "push_delivery_failed",
  "topic_id": "infra.provisioning",
  "message_id": "01912345-abcd-7000-8000-000000000002",
  "subscription_id": "sub-slack-alerts",
  "endpoint": "https://hooks.slack.com/services/T00/B00/xxxx",
  "status_code": 429,
  "retry_after_ms": 60000,
  "delivery_attempt": 3,
  "next_retry_at": "2026-04-09T10:01:00.456Z",
  "trace_id": "def456"
}
```

---

## 11. Security

### Authentication & Authorization

| Actor | Authentication | Authorization |
|-------|---------------|---------------|
| Publisher | mTLS or API key (per service) | ACL: `topic:publish` on specific topics |
| Pull subscriber | mTLS or API key (per service) | ACL: `subscription:pull` on specific subscriptions |
| Push subscriber (receiving webhook) | HMAC signature verification | N/A (passive receiver) |
| Admin | OAuth2 / OIDC | RBAC: `topic:admin`, `subscription:admin` |

### Webhook Security

```
Outbound webhook security:
  1. HMAC-SHA256 signature: sign payload with per-subscription secret
     Header: X-PubSub-Signature: sha256=<hex(hmac(secret, payload))>
  
  2. Mutual TLS: fan-out service presents client certificate to subscriber
     subscriber validates certificate chain
  
  3. Bearer token: fan-out service includes subscriber's API token in Authorization header
     Header: Authorization: Bearer <token>
  
  4. IP allowlisting: subscriber allows only fan-out service IP ranges
     Fan-out service uses NAT gateway with known IP
  
  5. Webhook verification handshake (on subscription creation):
     POST subscriber_endpoint with challenge
     Subscriber must return the challenge in response body
     Prevents registering arbitrary URLs
```

### Data Protection

```
PII in messages:
  - Message payload is opaque bytes to the pub/sub system (no inspection)
  - Message attributes are plain text (used for filtering)
  - PII should NOT be in attributes (attributes are logged, cached, indexed)
  - Encryption: publishers encrypt PII in payload; subscribers decrypt
  
Transport security:
  - Publisher -> Publish Service: TLS 1.3 (gRPC/HTTPS)
  - Publish Service -> Kafka: SASL_SSL
  - Fan-out Service -> Kafka: SASL_SSL
  - Fan-out Service -> Subscriber: HTTPS (required for push)
```

---

## 12. Incremental Rollout Strategy

### Phase 1: Core Pub/Sub (Week 1-3)
- Deploy Publish Service and Subscription Metadata Store.
- Support pull subscriptions only (Kafka consumer groups).
- 3 pilot topics with 5 subscriptions each.

### Phase 2: Push Delivery (Week 4-6)
- Deploy Fan-out Service with basic push delivery.
- Support simple filter expressions (equality only).
- 10 topics with push and pull subscriptions.
- Retry + DLQ for failed deliveries.

### Phase 3: Advanced Features (Week 7-9)
- Complex filter expressions (AND, OR, IN, prefix).
- Ordering keys for push delivery.
- Subscriber health scoring and auto-pause.
- DLQ replay for failed push deliveries.

### Phase 4: Broadcast Support (Week 10-12)
- Broadcast fan-out service for topics with 10K+ subscribers.
- Sharded subscriber lists.
- Batched push delivery.
- Load test with 1M subscribers.

### Phase 5: Full Production (Week 13-16)
- Migrate all infrastructure notifications to Pub/Sub.
- Self-service topic/subscription management.
- Full observability and alerting.
- Operational runbooks and on-call training.

**Rollout Q&As:**

**Q1: How do you migrate from an existing notification system?**
A: (1) Dual-write: existing system publishes to both old and new pub/sub. (2) Subscribers register on new pub/sub. (3) Validate: compare messages received via old and new paths. (4) Cutover: redirect publishers to new pub/sub only. (5) Decommission old system after 2-week soak period.

**Q2: How do you handle backward compatibility during rollout?**
A: (1) Publish API is additive: new fields don't break old publishers. (2) Subscription API has sensible defaults for all config fields. (3) Filter expressions are optional (default: match all). (4) Push delivery format is stable: header names and payload structure are versioned.

**Q3: How do you test push delivery at scale before production?**
A: (1) Deploy mock subscriber endpoints (echo servers that return 200). (2) Simulate 1M subscriber fan-out with mock endpoints. (3) Measure: fan-out latency, delivery success rate, resource consumption. (4) Inject failures: random 500s, slow responses, timeouts. (5) Validate retry and DLQ behavior.

**Q4: How do you handle the operational learning curve?**
A: (1) Self-service admin UI for topic/subscription management. (2) Built-in health dashboard per topic and subscription. (3) Runbook for every alert (subscriber health drop, delivery backlog, DLQ spike). (4) On-call rotation with pub/sub-specific training.

**Q5: How do you ensure no messages are lost during migration?**
A: (1) Message IDs are globally unique (UUIDv7). (2) Both old and new systems log all message IDs. (3) Reconciliation job compares message sets. (4) Gap detection: any message in old system but not new triggers investigation. (5) Kafka retention ensures messages are available for replay during migration.

---

## 13. Trade-offs & Decision Log

| Decision | Options | Chosen | Rationale |
|----------|---------|--------|-----------|
| Message store | Custom queue, Redis Streams, Kafka | Kafka | Existing infrastructure; durable; replayable; high throughput |
| Pull delivery | Custom protocol, gRPC streaming, Kafka consumer | Kafka consumer groups | Native Kafka; no custom protocol; proven at scale |
| Push delivery | Fire-and-forget, Retry+DLQ, ACK-based | Retry with backoff + DLQ | At-least-once guarantee; manageable complexity |
| Filter evaluation | Kafka-level, Client-side, Server-side | Server-side (fan-out service) | Best balance of performance and flexibility |
| Subscription storage | In Kafka, MySQL, PostgreSQL | MySQL + Redis cache | MySQL for CRUD; Redis for hot-path lookups |
| Fan-out architecture | One CG per subscription, Application fan-out, Tiered | Tiered (Kafka CG for pull, App fan-out for push) | Scalable for both small and large fan-out |
| Ordering guarantee | Global order, Per-partition, Per-ordering-key | Per-ordering-key | Most flexible; supports parallel delivery for independent keys |
| Idempotency | None, Publisher dedup, Subscriber dedup | Both (publisher dedup + subscriber idempotency key) | Defense in depth |
| Subscriber auth | None, API key, HMAC, mTLS | HMAC (default) + mTLS (option) | HMAC is simple; mTLS for high-security subscribers |

---

## 14. Agentic AI Integration

### AI-Powered Notification Routing

```python
class IntelligentNotificationRouter:
    """AI agent that optimizes notification routing and delivery."""
    
    def optimize_delivery_schedule(self, topic_id):
        """Analyze delivery patterns and optimize."""
        
        metrics = self.get_delivery_metrics(topic_id, days=7)
        
        optimization = self.model.analyze(
            prompt=f"""Analyze notification delivery for topic '{topic_id}':
            
            Delivery metrics (7 days):
            - Total messages: {metrics['total_messages']}
            - Push subscribers: {metrics['push_count']}
            - Avg delivery latency: {metrics['avg_latency_ms']}ms
            - Failure rate: {metrics['failure_rate']}%
            - Peak hours: {metrics['peak_hours']}
            - Subscriber health scores: {metrics['health_scores']}
            
            Recommend:
            1. Which subscribers should be batched (high volume, tolerant of latency)?
            2. Which subscribers need real-time delivery?
            3. Optimal batch size and interval per subscriber category.
            4. Subscribers that should be auto-paused (consistently unhealthy).
            """
        )
        
        return optimization
    
    def smart_filter_suggestion(self, subscription_id):
        """Suggest filter optimization based on processing patterns."""
        
        stats = self.get_subscription_stats(subscription_id)
        
        suggestion = self.model.analyze(
            prompt=f"""Subscription '{subscription_id}' stats:
            - Messages received: {stats['received']}/day
            - Messages actually processed: {stats['processed']}/day
            - Messages discarded after receive: {stats['discarded']}/day
            - Common discard reasons: {stats['discard_reasons']}
            
            The subscriber is receiving {stats['received']} messages but only 
            processing {stats['processed']}. Suggest a filter expression that 
            would reduce unnecessary deliveries.
            
            Current filter: {stats['current_filter']}
            """
        )
        
        return suggestion
```

### AI-Driven Subscriber Health Management

```python
class SubscriberHealthAgent:
    """AI agent that manages subscriber health and takes proactive action."""
    
    def predict_subscriber_failure(self):
        """Predict which subscribers will fail in the next hour."""
        
        health_trends = self.get_all_subscriber_health_trends(hours=24)
        
        predictions = self.model.analyze(
            prompt=f"""Based on 24-hour health trends for all subscribers,
            predict which subscribers will likely fail in the next hour:
            
            {health_trends}
            
            Consider:
            - Declining success rate trends
            - Increasing latency trends  
            - Time-of-day patterns (deployments, maintenance windows)
            - Correlation between subscribers on same infrastructure
            
            For each at-risk subscriber:
            1. Probability of failure
            2. Recommended preemptive action
            3. Impact if subscriber goes down
            """
        )
        
        for prediction in predictions:
            if prediction.probability > 0.8:
                self.take_preemptive_action(prediction)
    
    def diagnose_delivery_failure_spike(self, topic_id):
        """AI diagnoses why delivery failures spiked for a topic."""
        
        context = {
            'failed_subscriptions': self.get_failed_subscriptions(topic_id),
            'recent_changes': self.get_recent_infra_changes(),
            'network_health': self.get_network_health(),
            'subscriber_deployment_events': self.get_deployment_events()
        }
        
        diagnosis = self.model.reason(
            prompt=f"""Delivery failures spiked for topic '{topic_id}'.
            
            Context: {context}
            
            Is this:
            1. A publisher issue (bad messages)?
            2. A subscriber issue (endpoint down/slow)?
            3. An infrastructure issue (network, DNS)?
            4. A scaling issue (throughput exceeded capacity)?
            
            Provide root cause and recommended action.
            """
        )
        
        return diagnosis
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: How would you design a pub/sub notification system?**
A: Core components: (1) Publish Service: validates and stores messages in Kafka. (2) Subscription metadata store (MySQL + Redis). (3) Pull delivery: Kafka consumer groups per subscription. (4) Push delivery: Fan-out Service consumes from Kafka, dispatches HTTP webhooks per subscription. (5) Filter evaluation: server-side attribute matching. (6) Retry + DLQ for failed deliveries.

**Q2: What is the difference between pub/sub and a message queue?**
A: Message queue: one message consumed by one consumer (competing consumers). Pub/sub: one message delivered to all subscribers (fan-out). Kafka supports both: single consumer group = queue; multiple consumer groups = pub/sub. This design adds push delivery, filtering, and subscription management on top of Kafka's pub/sub foundation.

**Q3: How do you handle fan-out to 1 million subscribers?**
A: Sharded fan-out: (1) Partition subscriber list into 100 shards of 10K each. (2) Each shard handled by a dedicated fan-out worker. (3) Workers deliver in parallel with async HTTP client. (4) Batched delivery: 10 messages per HTTP call. (5) Jitter to prevent thundering herd. (6) Total delivery time for 1M subscribers: ~5 seconds.

**Q4: Push vs pull -- when do you use each?**
A: Push (webhook): when subscriber needs real-time notification with minimal latency; when subscriber is a service that reacts to events. Pull (consumer): when subscriber controls processing rate; when subscriber does batch processing; when ordering within partition is critical.

**Q5: How do you implement message filtering?**
A: Messages carry attributes as Kafka headers. Subscriptions define filter expressions (e.g., `attributes.severity = 'CRITICAL'`). The fan-out service evaluates filters before delivery. For large subscriber counts: inverted index on common attribute values for O(1) lookup. Complex filters: compiled to predicates, evaluated at ~150ns each.

**Q6: How do you guarantee at-least-once delivery for push notifications?**
A: (1) Fan-out service retries on HTTP 4xx/5xx/timeout with exponential backoff. (2) After max retries, message goes to DLQ. (3) Fan-out consumer commits Kafka offset only after all deliveries are initiated. (4) On crash: consumer restarts from last committed offset, re-delivers. (5) Subscriber uses message_id for idempotent processing.

**Q7: How do you handle ordering in a pub/sub system?**
A: Messages with the same ordering key go to the same Kafka partition. For pull subscribers: Kafka guarantees per-partition order. For push subscribers: fan-out service delivers messages with the same ordering key sequentially (waits for ack before sending next). Different ordering keys are delivered in parallel.

**Q8: How do you handle a subscriber that is down for 24 hours?**
A: Push: fan-out service retries with increasing backoff. After max retries (e.g., 1h), messages go to DLQ. Subscription auto-paused after health score drops. When subscriber recovers: (1) Resume subscription. (2) Pull subscriber: consumes from last committed offset (Kafka retains 7 days). (3) Push subscriber: replay DLQ messages for the outage window.

**Q9: What is the cost model for this system?**
A: Per message: one Kafka write + one Kafka read per push subscription group. Per delivery: one HTTP call (batching amortizes). Storage: Kafka retention (7 days). Compute: fan-out service fleet. Network: outbound bandwidth for push delivery. At 500K msgs/sec with 50 avg subscribers: ~25M deliveries/sec, requiring ~30 fan-out pods and ~40 Gbps outbound bandwidth.

**Q10: How do you handle message TTL?**
A: Messages carry a `publish_timestamp`. Fan-out service checks: `if (now - publish_timestamp > subscription.message_ttl_sec): skip()`. Expired messages are not delivered but the Kafka offset is committed. Pull subscribers perform the same TTL check client-side.

**Q11: How do you handle duplicate messages in the pub/sub system?**
A: Publisher-side: idempotency key in PublishRequest; Publish Service deduplicates via Redis SET NX with 10-minute TTL. Subscriber-side: each webhook includes message_id; subscriber deduplicates via Redis SET NX with 7-day TTL. Both layers are needed for defense-in-depth.

**Q12: How do you monitor the health of push subscribers?**
A: Sliding window of last 100 deliveries per subscriber. Metrics: success rate, average latency, error distribution (4xx vs 5xx vs timeout). Auto-pause if success rate < 10%. Alert if success rate < 50%. Dashboard shows per-subscriber health scores with trend.

**Q13: How do you handle schema changes in published messages?**
A: Use Schema Registry with Avro. Publishers register schemas before publishing. Subscribers deserialize using schema ID in the message header. Schema compatibility mode (BACKWARD) ensures subscribers can read new message formats without code changes.

**Q14: What happens if the subscription metadata store (MySQL + Redis) goes down?**
A: Redis down: fan-out service falls back to MySQL queries. Slower (10ms vs 0.5ms) but functional. MySQL down: new subscriptions cannot be created; existing subscriptions work from Redis cache. Both down: fan-out service uses in-memory cache (stale but functional for existing subscriptions).

**Q15: How do you implement webhook verification to prevent abuse?**
A: On subscription creation with push endpoint: (1) Generate a random challenge token. (2) POST to the endpoint with the challenge. (3) Endpoint must return the challenge in the response body. (4) Only if verification succeeds, activate the subscription. This proves the endpoint owner consents to receiving webhooks. Re-verify periodically.

**Q16: How would you extend this to support exactly-once delivery?**
A: True exactly-once requires subscriber acknowledgment tracked atomically with the delivery state. Implementation: (1) Assign a unique delivery_id per (message, subscription). (2) Fan-out delivers with delivery_id. (3) Subscriber calls Acknowledge API with delivery_id after processing. (4) Fan-out marks delivery as complete only on explicit ack. (5) Unacked messages are redelivered after ack_deadline. This is essentially Google Cloud Pub/Sub's model.

---

## 16. References

| Resource | URL / Reference |
|----------|----------------|
| Google Cloud Pub/Sub Documentation | https://cloud.google.com/pubsub/docs |
| AWS SNS Documentation | https://docs.aws.amazon.com/sns/ |
| Azure Event Grid Documentation | https://learn.microsoft.com/en-us/azure/event-grid/ |
| Designing Data-Intensive Applications | Martin Kleppmann, Chapter 11 |
| Enterprise Integration Patterns: Publish-Subscribe Channel | Hohpe & Woolf |
| Kafka Consumer Groups | https://kafka.apache.org/documentation/#consumerconfigs |
| Webhook Best Practices (Stripe) | https://stripe.com/docs/webhooks/best-practices |
| Google Pub/Sub: Filtering | https://cloud.google.com/pubsub/docs/filtering |
| LinkedIn Pub/Sub at Scale | Engineering blog |
| CEL (Common Expression Language) | https://github.com/google/cel-spec |
