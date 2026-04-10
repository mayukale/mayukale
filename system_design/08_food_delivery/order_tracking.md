# System Design: Real-Time Order Tracking

---

## 1. Requirement Clarifications

### Functional Requirements

- **Live Dasher location**: consumers see the Dasher's position on a map, updated every 4 seconds while the Dasher is en route to the restaurant or en route to the consumer
- **Order status state machine**: consumer and merchant see the current order status (placed → merchant accepted → preparing → ready for pickup → dasher assigned → dasher en route to merchant → dasher at merchant → picked up → en route to consumer → delivered) and receive an update within 5 seconds of any state transition
- **ETA recalculation**: estimated delivery time is recomputed on each Dasher location ping and pushed to the consumer; accuracy target ±5 minutes at p80
- **Push notification triggers**: each major status transition fires a push notification to the appropriate parties (consumer via APNs/FCM; merchant via FCM/tablet; Dasher via FCM)
- **Map rendering**: consumer-facing map shows the Dasher's location pin, route polyline, consumer address pin, and restaurant pin
- **Historical order status**: consumer can view the status timeline (when each transition occurred) in the order detail screen after delivery
- **Multi-device support**: consumer may be tracking on both iPhone and iPad simultaneously; both receive updates

### Non-Functional Requirements

- **Update latency**: Dasher location visible to consumer within 5 seconds of the GPS ping (end-to-end)
- **Scalability**: support 1 million simultaneously tracked orders at peak
- **Availability**: 99.99% for the notification path; 99.9% for the real-time map stream (brief gaps tolerable if state eventually catches up)
- **Reliability**: zero status transitions silently dropped — every state change must be durably recorded and eventually delivered to all subscribers even if they are temporarily offline
- **Push notification deliverability**: ≥ 99.5% of critical notifications delivered within 10 seconds
- **Battery efficiency**: Dasher GPS ping interval of 4 s is a business requirement; consumer polling (if fallback to polling) must not drain battery (minimum 15-second polling intervals)
- **Security**: consumers can only track Dashers assigned to their own order; Dasher location data not exposed to third parties

### Out of Scope

- Turn-by-turn navigation for the Dasher (handled by Google Maps / Waze SDK within the Dasher app)
- Consumer-to-Dasher or consumer-to-merchant chat (separate messaging feature)
- Analytics pipeline for Dasher efficiency metrics
- International telecom integrations for SMS in non-US markets
- Dasher location data sold to advertisers (explicitly prohibited)

---

## 2. Users & Scale

### User Types

| Actor    | Description                                               |
|----------|-----------------------------------------------------------|
| Consumer | Views live map; receives push notifications on status changes |
| Dasher   | Sends GPS pings every 4 s; receives dispatch and milestone confirmations |
| Merchant | Receives status notifications (order placed, Dasher en route, etc.) |
| Internal | Ops dashboard for monitoring stuck orders; ML pipeline for ETA training |

### Traffic Estimates

**Assumptions**
- 4.93 M orders/day → average 57 orders/s, peak 171 orders/s
- Average delivery duration: 35 minutes
- Active orders at any time (steady state): 57 orders/s × (35 min × 60 s) = 119 700 active orders
- Peak active orders: 171 orders/s × 2 100 s = ~360 000 active orders
- Assumption: 70% of consumers actively watch the tracking map during delivery = 252 000 concurrent tracking sessions at peak
- Dashers pinging at 4 s intervals: 500 K active Dashers at peak dinner hour

| Metric                                      | Calculation                                                                       | Result               |
|---------------------------------------------|-----------------------------------------------------------------------------------|----------------------|
| Dasher location writes per second (peak)    | 500 K Dashers × (1/4) pings/s                                                    | 125 000 writes/s     |
| Dasher location fan-out reads per second    | 252 000 consumer tracking sessions × (1 update/4 s) push-based                   | 63 000 pushes/s      |
| Status change events per second             | 171 peak orders/s × avg 6 status transitions per order / 35 min × 60             | ~2.9 events/s avg; burst 30/s at peak |
| Push notifications per second               | 30 status events/s × 3 actors (consumer + merchant + dasher) × 3 recipients avg  | ~270 pushes/s        |
| WebSocket connections (peak)                | 252 000 consumers + 500 K Dashers + 50 K merchants                               | ~800 K WS connections |
| ETA recalculation events                    | 125 000 location pings/s → rate-limited to 1/15 s per active order               | ~360 000/15 s = ~24 000 ETA calcs/s |

### Latency Requirements

| Operation                                     | p50 target | p99 target | Notes                                      |
|-----------------------------------------------|------------|------------|--------------------------------------------|
| Dasher GPS write ACK                          | 20 ms      | 100 ms     | Ingest path; Dasher app waits for ACK      |
| Consumer receives location update (E2E)       | 1 s        | 5 s        | From Dasher GPS pin to consumer map update |
| Status change → consumer push notification    | 200 ms     | 5 s        | APNs/FCM delivery adds tail latency        |
| ETA update delivery to consumer              | 2 s        | 10 s       | Recomputed after each location update      |
| Order status history fetch                   | 50 ms      | 200 ms     | Read from DB; usually cached               |

### Storage Estimates

| Data Type                               | Calculation                                                                       | Result         |
|-----------------------------------------|-----------------------------------------------------------------------------------|----------------|
| Location pings (30-day retention)       | 125 000/s × 86 400 s/day × 30 days × 50 bytes/ping                               | ~16.2 TB       |
| Order status history                    | 4.93 M orders/day × 6 transitions × 200 bytes × 365 days × 3 years               | ~6.5 TB        |
| Active order tracking state (in-memory) | 360 000 peak active orders × 500 bytes (Dasher position + order metadata)         | ~180 MB (Redis) |
| Push notification log (90 days)         | 270 pushes/s × 86 400 s/day × 90 days × 500 bytes                                | ~1.05 TB       |
| WebSocket session metadata              | 800 K sessions × 200 bytes                                                        | ~160 MB        |

### Bandwidth Estimates

| Flow                                         | Calculation                                                              | Result         |
|----------------------------------------------|--------------------------------------------------------------------------|----------------|
| Dasher GPS ingest (ingress)                  | 125 000 writes/s × 200 bytes/payload                                     | 25 MB/s        |
| Consumer location stream (egress, WebSocket) | 63 000 pushes/s × 150 bytes/update                                       | 9.45 MB/s      |
| ETA updates pushed to consumers              | 24 000 ETA updates/s × 100 bytes                                         | 2.4 MB/s       |
| Status event notifications (egress)          | 270 pushes/s × 500 bytes                                                 | 135 KB/s       |
| Total estimated peak                         | —                                                                        | ~40 MB/s       |

---

## 3. High-Level Architecture

```
  ┌──────────────────────────────────────────────────────────────────────────────────┐
  │                                  CLIENTS                                         │
  │  Dasher App (iOS/Android): GPS ping every 4 s, milestone taps                   │
  │  Consumer App (iOS/Android/Web): Map view, push notification receiver           │
  │  Merchant App (tablet): Order status board, push notification receiver          │
  └──────────────────┬──────────────────────────────────────┬────────────────────────┘
                     │  HTTPS + WSS                         │  HTTPS + WSS
           ┌─────────▼──────────────────┐         ┌────────▼──────────────────┐
           │   API Gateway / CDN Edge   │         │   WebSocket Gateway        │
           │ (REST auth, rate limit,    │         │ (connection mgmt,          │
           │  routing for REST APIs)    │         │  JWT validation,           │
           └─────────┬──────────────────┘         │  session affinity)         │
                     │                            └────────┬──────────────────┘
                     │                                     │
         ┌───────────▼──────────────┐         ┌──────────▼───────────────────────┐
         │   Location Service        │         │   Location Fan-out Service        │
         │  (GPS ingest, validation, │         │  (subscriptions, push to         │
         │   write to Redis +        │         │   WebSocket sessions)            │
         │   Cassandra, publish to   │─────────►                                  │
         │   Kafka location-events)  │         │  Redis Pub/Sub:                  │
         └───────────────────────────┘         │  channel: tracking:{order_id}   │
                                               └──────────────────────────────────┘
                     │
         ┌───────────▼──────────────┐
         │   ETA Recalc Service     │
         │  (consumes location      │
         │   events, calls OSRM,    │
         │   updates Redis,         │
         │   publishes eta-updates) │
         └───────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────────────┐
  │              Apache Kafka                                                    │
  │  Topics: location-events (partitioned by order_id)                          │
  │           order-status-events (partitioned by order_id)                     │
  │           notification-triggers (partitioned by recipient_id)               │
  │           eta-update-events (partitioned by order_id)                       │
  └──────────────────────────────────────────────────────────────────────────────┘
            ▲                                    │
            │ publishes                          │ consumes
  ┌─────────┴──────────────┐        ┌────────────▼───────────────┐
  │   Order Service        │        │   Notification Service      │
  │  (state machine,       │        │  (APNs, FCM, SMS dispatch,  │
  │   publishes status     │        │   deduplication, delivery   │
  │   transition events)   │        │   retry, preference filter) │
  └────────────────────────┘        └────────────────────────────┘

  ┌─────────────────────────┐  ┌────────────────────────────┐  ┌──────────────────────┐
  │  Redis Cluster          │  │  Cassandra Cluster         │  │  PostgreSQL (Aurora) │
  │  - Dasher current pos   │  │  - Location history        │  │  - Order state       │
  │  - Active order state   │  │    (TTL 30 days)           │  │  - Status history    │
  │  - ETA per order        │  │  - Notification log        │  │  - Subscriptions     │
  │  - WS session map       │  └────────────────────────────┘  └──────────────────────┘
  │  - Pub/Sub channels     │
  └─────────────────────────┘
```

**Component roles:**

- **WebSocket Gateway**: Accepts and maintains long-lived WebSocket connections from consumer, Dasher, and merchant apps. Validates JWT on connection establishment. Maintains a mapping `{ connection_id → order_id, user_id }` in local memory (or Redis for cross-node routing). Forwards messages from internal services to the right connection. Does not contain business logic.
- **Location Service**: Receives Dasher GPS pings via REST (`POST /dasher/location`). Validates GPS data (speed sanity check, accuracy threshold). Writes current position to Redis (`dasher:pos:{dasher_id}`) and appends to Cassandra. Publishes `location.updated` event to Kafka `location-events` topic.
- **Location Fan-out Service**: Consumes `location-events` from Kafka. For each event, looks up the active order for that Dasher, then publishes to Redis Pub/Sub channel `tracking:{order_id}`. All WebSocket Gateway nodes subscribed to that channel receive the message and push to connected consumers.
- **ETA Recalc Service**: Consumes `location-events` from Kafka. Rate-limits recalculation to once per 15 seconds per order. Calls OSRM for the remaining route legs. Computes new ETA. Stores in Redis (`eta:{order_id}`) and publishes `eta.updated` event. Fan-out Service picks this up and pushes to consumer WebSocket.
- **Order Service**: Owns the order state machine. On every status transition, publishes to `order-status-events` Kafka topic. Also writes `order_status_history` to PostgreSQL.
- **Notification Service**: Consumes `notification-triggers` Kafka topic. Builds the notification payload (title, body, deep-link URL). Calls APNs for iOS; FCM for Android/tablets. Logs notification in Cassandra. Respects consumer notification preferences. Deduplicates using Redis idempotency check.

**Primary use-case data flow (Dasher GPS ping → consumer sees map update):**

1. Dasher app (background process) sends `POST /dasher/location` with `{lat, lng, heading, speed, timestamp_ms}` every 4 seconds.
2. API Gateway authenticates JWT, routes to Location Service.
3. Location Service:
   - Validates: lat/lng bounds, speed < 200 km/h, timestamp within 10 s of server time
   - Writes `SETEX dasher:pos:{dasher_id} 10 {lat,lng,heading,ts}` to Redis
   - Appends row to Cassandra `dasher_location_history`
   - Publishes to Kafka `location-events` topic (key = `order_id` of active order for this Dasher)
   - Returns 204 No Content to Dasher app
4. Location Fan-out Service (Kafka consumer, <100 ms lag):
   - Looks up `order_id` for `dasher_id` from Redis hash `dasher:active_order:{dasher_id}`
   - Publishes message `{dasher_id, lat, lng, heading, ts}` to Redis Pub/Sub `tracking:{order_id}`
   - Publishes same message to `tracking:{order_id}:{consumer_id}` for targeted routing
5. WebSocket Gateway nodes subscribed to `tracking:{order_id}`:
   - Receive message from Redis Pub/Sub
   - Look up WebSocket connection(s) for the consumer of `order_id`
   - Push JSON message to all connected sessions: `{"type":"location_update","lat":37.77,"lng":-122.41,"heading":180,"eta_min":12}`
6. Consumer map updates: pin moves; route polyline recalculates client-side using new position.

---

## 4. Data Model

### Entities & Schema

```sql
-- ─────────────────────────────────────────────
-- ORDER STATUS HISTORY (PostgreSQL)
-- (Source of truth for the timeline displayed in order detail)
-- ─────────────────────────────────────────────
CREATE TABLE order_status_history (
    history_id      UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    order_id        UUID NOT NULL REFERENCES orders(order_id),
    from_status     TEXT,               -- NULL for initial transition
    to_status       TEXT NOT NULL,
    actor_id        UUID,               -- user who triggered the transition
    actor_role      TEXT,               -- 'consumer', 'merchant', 'dasher', 'system'
    notes           TEXT,
    occurred_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE INDEX idx_status_history_order_id ON order_status_history(order_id, occurred_at ASC);

-- ─────────────────────────────────────────────
-- NOTIFICATION LOG (Cassandra — high write volume)
-- ─────────────────────────────────────────────
-- Cassandra CQL schema:
CREATE TABLE notifications (
    notification_id  UUID,
    recipient_id     UUID,
    order_id         UUID,
    notification_type TEXT,   -- 'order_placed', 'dasher_assigned', 'delivered', etc.
    channel          TEXT,    -- 'apns', 'fcm', 'sms', 'websocket'
    title            TEXT,
    body             TEXT,
    deep_link_url    TEXT,
    delivery_status  TEXT,    -- 'sent', 'delivered', 'failed'
    sent_at          TIMESTAMP,
    delivered_at     TIMESTAMP,
    PRIMARY KEY ((recipient_id), sent_at, notification_id)
) WITH CLUSTERING ORDER BY (sent_at DESC)
  AND default_time_to_live = 7776000;  -- 90 days TTL

-- ─────────────────────────────────────────────
-- DASHER LOCATION HISTORY (Cassandra)
-- ─────────────────────────────────────────────
CREATE TABLE dasher_location_history (
    dasher_id       UUID,
    recorded_at     TIMESTAMP,
    order_id        UUID,
    lat             DOUBLE,
    lng             DOUBLE,
    heading_deg     FLOAT,
    speed_mps       FLOAT,
    accuracy_m      FLOAT,
    PRIMARY KEY ((dasher_id), recorded_at)
) WITH CLUSTERING ORDER BY (recorded_at DESC)
  AND default_time_to_live = 2592000;  -- 30 days TTL

-- ─────────────────────────────────────────────
-- WEBSOCKET SESSION REGISTRY (Redis — ephemeral)
-- ─────────────────────────────────────────────
-- Redis keys:
-- ws:session:{session_id}    HASH  { user_id, order_id, role, node_id, connected_at }  TTL 2hr
-- ws:user:{user_id}          SET   { session_id_1, session_id_2 }                       TTL 2hr
-- ws:order:{order_id}        SET   { session_id_1 (consumer), session_id_2 (dasher) }   TTL 2hr
-- ws:node:{node_id}          SET   { session_id_1, ..., session_id_N }                  TTL 30s (heartbeat refreshed)

-- ─────────────────────────────────────────────
-- ACTIVE ORDER STATE (Redis — ephemeral, fast reads)
-- ─────────────────────────────────────────────
-- Redis keys:
-- order:state:{order_id}     HASH  {
--     status, merchant_id, consumer_id, dasher_id,
--     merchant_lat, merchant_lng,
--     delivery_lat, delivery_lng,
--     dasher_lat, dasher_lng, dasher_heading,
--     eta_min, eta_updated_at,
--     status_updated_at
-- }  TTL 4hr (covers max delivery duration + buffer)

-- dasher:active_order:{dasher_id}   STRING  { order_id }   TTL 2hr

-- ─────────────────────────────────────────────
-- PUSH NOTIFICATION DEVICE TOKENS (PostgreSQL)
-- ─────────────────────────────────────────────
CREATE TABLE device_tokens (
    token_id        UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(user_id),
    platform        TEXT NOT NULL CHECK (platform IN ('ios', 'android', 'web')),
    device_token    TEXT NOT NULL,
    is_active       BOOLEAN NOT NULL DEFAULT true,
    registered_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_used_at    TIMESTAMPTZ,
    UNIQUE (user_id, device_token)
);

CREATE INDEX idx_device_tokens_user_id ON device_tokens(user_id) WHERE is_active = true;

-- ─────────────────────────────────────────────
-- NOTIFICATION PREFERENCES (PostgreSQL)
-- ─────────────────────────────────────────────
CREATE TABLE notification_preferences (
    user_id                 UUID PRIMARY KEY REFERENCES users(user_id),
    push_enabled            BOOLEAN NOT NULL DEFAULT true,
    sms_enabled             BOOLEAN NOT NULL DEFAULT false,
    email_enabled           BOOLEAN NOT NULL DEFAULT true,
    -- Per-event overrides
    notify_on_placed        BOOLEAN NOT NULL DEFAULT true,
    notify_on_dasher_assign BOOLEAN NOT NULL DEFAULT true,
    notify_on_picked_up     BOOLEAN NOT NULL DEFAULT true,
    notify_on_delivered     BOOLEAN NOT NULL DEFAULT true,
    notify_on_promotions    BOOLEAN NOT NULL DEFAULT false,
    updated_at              TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

### Database Choice

**Four distinct data stores, each chosen for a specific access pattern:**

| Store            | Data Stored                                  | Access Pattern                              | Why This Choice                                                                               |
|------------------|----------------------------------------------|---------------------------------------------|-----------------------------------------------------------------------------------------------|
| Redis Cluster    | Current Dasher position, active order state, WS session registry, ETA, Pub/Sub | Sub-ms reads; Pub/Sub fan-out; ephemeral TTL data | In-memory speed mandatory for 125K writes/s ingest and sub-second fan-out; native Pub/Sub; TTL-based cleanup of ephemeral data |
| Cassandra        | Dasher location history, notification log    | High-throughput time-series writes; TTL-based expiry; no multi-row transactions needed | Linear write scalability; TTL-native; wide-row model matches time-series; RF=3 survives 1-node failure |
| PostgreSQL (Aurora) | Order status history, device tokens, notification preferences, order canonical state | ACID transactions for state transitions; strong consistency for preferences | State transitions require atomicity; preferences rarely change; small data volume suits PostgreSQL |
| Kafka            | location-events, order-status-events, notification-triggers, eta-update-events | Durable event log; at-least-once delivery; replay; multi-consumer | Decouples producers (Location Service, Order Service) from consumers (Fan-out, Notification, ETA); provides durability and replay for reliability |

**Why not use a single database for everything?**

A relational database like PostgreSQL could theoretically handle all of this, but it would fail on write throughput: 125 000 Dasher GPS writes/s far exceeds what a single PostgreSQL cluster can handle (practical limit is ~50 K simple inserts/s on modern hardware, and our schema has indexing overhead). Cassandra handles this via horizontal partitioning with eventual consistency — perfectly appropriate for location history where we never need strong consistency. Redis is chosen specifically because the Pub/Sub fan-out must happen within milliseconds; a database-polling approach would add unacceptable latency.

---

## 5. API Design

```
Base URL:    https://api.doordash.com/v1
Auth:        Bearer <JWT>
WebSocket:   wss://ws.doordash.com/v1/tracking
```

### Dasher Location & Milestone Endpoints

```
POST /dasher/location
     Auth: Dasher JWT (role=dasher required)
     Rate limit: 1 accepted write per 3 s per Dasher (server enforces; more frequent = 429)
     Body: {
       "lat": 37.7749,
       "lng": -122.4194,
       "heading_deg": 180.0,
       "speed_mps": 8.3,
       "accuracy_m": 5.0,
       "timestamp_ms": 1700000000000
     }
     Response 204: No Content (success)
     Response 400: Invalid coordinates or timestamp too old (> 30 s)
     Response 429: Too Many Requests

PATCH /dasher/orders/{order_id}/milestone
     Auth: Dasher JWT
     Body: { "milestone": "at_merchant" | "picked_up" | "delivered",
             "lat": float, "lng": float }
     Response 200: { "status": "dasher_at_merchant", "next_milestone": "picked_up" }
     Response 409: Invalid state transition
     Response 403: Dasher not assigned to this order
```

### Consumer Tracking Endpoints

```
GET /orders/{order_id}/tracking
     Auth: Consumer JWT (must be the order's consumer_id)
     Description: HTTP polling fallback; returns current state if WebSocket unavailable
     Response 200: {
       "order_id": "uuid",
       "status": "dasher_en_route_to_consumer",
       "dasher": {
         "first_name": "Alex",
         "photo_url": "https://...",
         "rating": 4.9,
         "vehicle_type": "car",
         "current_lat": 37.7749,
         "current_lng": -122.4194,
         "current_heading_deg": 180
       },
       "eta_minutes": 12,
       "eta_updated_at": "2024-11-15T18:32:00Z",
       "route_polyline": "encoded_polyline_string",
       "merchant": {
         "name": "Taqueria El Sol",
         "lat": 37.771,
         "lng": -122.425
       },
       "delivery_address": {
         "street": "123 Main St",
         "lat": 37.780,
         "lng": -122.418
       },
       "status_history": [
         { "status": "placed", "occurred_at": "2024-11-15T18:00:00Z" },
         { "status": "merchant_accepted", "occurred_at": "2024-11-15T18:02:00Z" }
       ]
     }
     Cache: no-store (real-time data)
     Rate limit: 60 req/min per consumer (polling mode fallback)

GET /orders/{order_id}/status-history
     Auth: Consumer JWT
     Response 200: { "history": [ { "status", "occurred_at", "actor_role" } ] }
     Cache: no-store for in-progress orders; CDN 1 hr for delivered/cancelled
```

### WebSocket Protocol

```
Connection:
  wss://ws.doordash.com/v1/tracking?order_id={uuid}&token={jwt}

  Server validates JWT; verifies consumer owns the order_id.
  On success: sends initial state message (same payload as GET /tracking).
  On failure: closes connection with code 4001 (unauthorized).

Client → Server messages:
  { "type": "ping" }                         -- keepalive every 30 s
  { "type": "subscribe", "order_id": "uuid" }-- subscribe to additional order (multi-order case)
  { "type": "unsubscribe", "order_id": "uuid" }

Server → Client messages:
  {
    "type": "location_update",
    "order_id": "uuid",
    "dasher_lat": 37.7749,
    "dasher_lng": -122.4194,
    "dasher_heading_deg": 180,
    "eta_minutes": 12,
    "route_polyline": "encoded_polyline",
    "timestamp_ms": 1700000000000
  }

  {
    "type": "status_update",
    "order_id": "uuid",
    "new_status": "dasher_picked_up",
    "occurred_at": "2024-11-15T18:25:00Z",
    "eta_minutes": 15,
    "message": "Alex has picked up your order!"
  }

  {
    "type": "eta_update",
    "order_id": "uuid",
    "eta_minutes": 10,
    "updated_at": "2024-11-15T18:32:00Z"
  }

  {
    "type": "pong"
  }

  {
    "type": "order_complete",
    "order_id": "uuid",
    "occurred_at": "2024-11-15T18:45:00Z"
  }

Connection teardown:
  Server closes WS connection with code 1000 (normal) when order status = 'delivered' or 'cancelled'.
  Client should reconnect if connection drops before terminal status.
```

### Notification Endpoints

```
POST /notifications/tokens
     Auth: Any user JWT
     Body: { "platform": "ios"|"android"|"web",
             "device_token": "hex_or_b64_string" }
     Response 201: { "token_id": "uuid" }
     Description: Register/update device push token

DELETE /notifications/tokens/{token_id}
     Auth: Owner JWT only
     Response 204: Token deactivated

GET /notifications/preferences
     Auth: Consumer JWT
     Response 200: { notification_preferences object }

PUT /notifications/preferences
     Auth: Consumer JWT
     Body: { "push_enabled": true, "notify_on_promotions": false, ... }
     Response 200: { updated preferences }
```

---

## 6. Deep Dive: Core Components

### 6.1 WebSocket vs. Polling for Real-Time Location Updates

**Problem it solves:**
Consumers need to see the Dasher's position updated every 4 seconds on a map. This requires a low-latency, bidirectional or server-push communication channel between the backend and the consumer's app. The choice of protocol directly determines battery usage, server resource consumption, latency, and implementation complexity.

**Approaches comparison:**

| Approach                       | Description                                                     | Pros                                                              | Cons                                                                  |
|--------------------------------|-----------------------------------------------------------------|-------------------------------------------------------------------|-----------------------------------------------------------------------|
| Short polling (HTTP GET every N s) | Client polls REST endpoint every 4–15 s                     | Simple; no persistent connection; works everywhere                | High server load; high latency (up to N s); many empty responses     |
| Long polling                   | Client makes HTTP request; server holds it open until update    | Lower latency than short polling; no persistent socket            | Server holds threads open; complex timeout handling; 1 update per connection per cycle |
| Server-Sent Events (SSE)       | HTTP/1.1 one-way stream from server to client                   | Simpler than WebSocket; auto-reconnect built in; works over HTTP/2 | Unidirectional only; connection limit issues on HTTP/1.1              |
| WebSocket                      | Full-duplex TCP-based persistent connection                     | Lowest latency; bidirectional; efficient framing; binary support  | Connection state requires sticky routing or session registry; slightly complex upgrade handshake |
| gRPC streaming                 | HTTP/2 bidirectional streaming                                  | Efficient; strongly typed (Protobuf); good for mobile             | Less universal browser support; requires gRPC client lib on mobile    |

**Selected: WebSocket as primary; HTTP polling as fallback**

**Justification by specific technical properties:**

1. **Bidirectionality**: The consumer app needs to send keepalive pings and subscription changes; WebSocket handles both directions over a single TCP connection, reducing connection overhead compared to separate POST + GET pairs.

2. **Frame overhead**: A WebSocket data frame for a location update has a 2–14 byte header vs. ~700 bytes of HTTP headers per polling request. At 63 000 fan-out pushes/s, this saves substantial bandwidth: 63 000 × 686 bytes = 43 MB/s of avoided header overhead.

3. **Latency**: A WebSocket push from server to consumer takes ~5–50 ms (network RTT). Polling at 4-second intervals has worst-case latency of 4 s. With the 5-second E2E SLA, only WebSocket or SSE are viable.

4. **Connection count feasibility**: 800 K concurrent WebSocket connections. Modern load balancers (nginx, HAProxy, AWS ALB) and servers (Node.js via uWebSockets.js, or Go net/http with goroutines) handle 100 K–500 K connections per server with ~500 KB RAM per connection. A fleet of 8–10 WebSocket Gateway nodes handles this load.

5. **Fallback**: WebSocket may fail in restricted corporate networks (proxies stripping upgrade headers), old mobile OS versions, or brief network transitions. The polling endpoint `GET /orders/{id}/tracking` is the fallback, rate-limited to 60 req/min (once per second effective; acceptable for consumers who can't get WebSocket).

**Implementation detail — cross-node fan-out:**

WebSocket Gateway is a stateful service (connections are pinned to a node). When the Location Fan-out Service receives a location event, it needs to push to the consumer's WebSocket connection, which may be on any node.

Solution: Redis Pub/Sub as the fan-out bus.

```
                 ┌──────────────────────────────────┐
                 │  Location Fan-out Service         │
                 │  PUBLISH tracking:{order_id}      │
                 │  "{lat, lng, heading, eta_min}"   │
                 └──────────────┬───────────────────┘
                                │ Redis PUBLISH
          ┌─────────────────────┼──────────────────────┐
          ▼                     ▼                       ▼
  ┌───────────────┐   ┌─────────────────┐   ┌──────────────────┐
  │ WS Node 1     │   │ WS Node 2       │   │ WS Node N        │
  │ SUBSCRIBE     │   │ SUBSCRIBE       │   │ SUBSCRIBE        │
  │ tracking:*    │   │ tracking:*      │   │ tracking:*       │
  │               │   │                 │   │                  │
  │ Has consumer  │   │ Has consumer    │   │ Has no session   │
  │ of order_id:  │   │ of different    │   │ for this order:  │
  │ PUSH to WS    │   │ order: ignores  │   │ ignores silently │
  └───────────────┘   └─────────────────┘   └──────────────────┘
```

Each WS node subscribes to `tracking:*` using Redis Pub/Sub pattern subscription. When a message arrives, the node checks its local in-memory session map for any connections subscribed to `tracking:{order_id}`. If found, it pushes. If not found, it discards. This avoids the need for a central routing table while still delivering to the right node(s).

**Redis Pub/Sub throughput**: Redis handles ~1 M messages/s per cluster on modern hardware. Our 63 000 fan-out pushes/s (each published once to Redis, consumed by N nodes) generates 63 000 × 10 nodes = 630 000 Redis Pub/Sub message deliveries/s. This is within Redis's capacity on a 3-node cluster.

**Interviewer Q&As:**

Q1: What happens when a consumer's phone switches from WiFi to LTE mid-delivery?
A: The WebSocket connection drops when the network interface changes. The consumer app detects the disconnect event and immediately attempts to reconnect (exponential backoff: 1 s, 2 s, 4 s up to 30 s max). On reconnect, the server sends the current order state (including the latest Dasher position and ETA) as the initial message, so the consumer immediately sees up-to-date information. Any status transitions that occurred during the brief offline window are reflected in the initial state snapshot.

Q2: How do you handle 1 million WebSocket connections across the gateway fleet?
A: We provision WebSocket Gateway nodes using horizontally scalable Go or Node.js servers. Each node handles ~100 K–150 K connections using epoll-based async I/O (Go goroutines or Node.js event loop). At 1 M peak connections: 8 nodes × 125 K connections/node with 25% headroom. Kubernetes HPA scales on a custom metric: `websocket_connection_count / target_connections_per_pod`. Memory per connection: ~500 bytes (session metadata in-process) + Redis Pub/Sub subscription overhead. At 125 K connections per node × 500 bytes = 62.5 MB per node for session state — trivial.

Q3: Why not use Server-Sent Events instead of WebSocket?
A: SSE is unidirectional (server → client only). While the tracking use case is mostly server-push, the client also needs to send subscription changes and keepalives. With SSE, we would need a separate HTTP connection for client-to-server messages, effectively having two connections. SSE also suffers from browser connection limits (6 per domain on HTTP/1.1) though HTTP/2 multiplexing mitigates this. WebSocket provides a cleaner abstraction for this use case at the cost of slightly more complex server implementation.

Q4: How do you prevent a consumer from subscribing to another consumer's order tracking?
A: At WebSocket connection time, the server validates the JWT and extracts `consumer_id`. It queries Redis `order:state:{order_id}` and checks that `consumer_id` field matches the JWT's `consumer_id` claim. If they don't match, the connection is rejected with WebSocket close code 4003 (Forbidden). This check happens before the connection is fully established, so no location data is ever transmitted to unauthorized clients. The JWT itself is validated for signature integrity and expiry via the JWT library.

Q5: How would you handle a consumer who leaves the tracking screen but the app stays running in the background?
A: The consumer app should send `{ "type": "unsubscribe", "order_id": "uuid" }` when the tracking view is backgrounded. This reduces server-side push volume. However, the server cannot rely on client cooperation; if the consumer forgets (app killed) or the unsubscribe message is lost, the server's WebSocket connection still exists until the TCP keepalive timeout (usually 60–120 s on mobile). Server-side: each WS session has an `is_active_viewer` flag toggled by subscribe/unsubscribe events; location pushes are skipped for sessions with `is_active_viewer=false`, reducing unnecessary network traffic to backgrounded apps while maintaining the connection for status change notifications.

---

### 6.2 ETA Recalculation Pipeline

**Problem it solves:**
The ETA displayed on the consumer's tracking screen must update as the Dasher moves. A static ETA computed at order placement quickly becomes inaccurate as traffic conditions change, the Dasher takes a detour, or the merchant is slower/faster than predicted. Consumers heavily judge the platform on ETA accuracy. At the same time, recomputing ETA for every single location ping (125 000/s across all Dashers) would generate 125 000 OSRM API calls/s — far beyond what a self-hosted OSRM cluster can serve. The challenge is achieving frequent-enough recalculation to appear real-time while staying within compute budget.

**Approaches comparison:**

| Approach                                     | Description                                                        | Pros                                                    | Cons                                                           |
|----------------------------------------------|--------------------------------------------------------------------|---------------------------------------------------------|----------------------------------------------------------------|
| Recalculate on every GPS ping                | ETA recomputed per ping for every active order                     | Maximum freshness                                       | 125 000 OSRM calls/s at peak; requires ~1 000 OSRM instances  |
| Fixed interval (every 60 s)                  | Batch recalculation every minute                                   | Low compute cost                                        | Up to 60 s stale ETA; poor consumer experience near end of delivery |
| Distance-threshold trigger                   | Recompute only when Dasher moves > 200 m from last ETA calculation | Proportional to movement; no recalc when stationary    | Complex threshold logic; stale during heavy traffic slowdowns  |
| Rate-limited per active order (every 15 s)   | One recalc per active order per 15 seconds max                     | Balanced frequency; predictable OSRM load; fresh enough | 15 s maximum staleness (acceptable vs. 5 s SLA)               |
| Client-side dead reckoning between recalcs   | Server sends route; client animates Dasher movement interpolated from last known position + heading + speed | Smooth animation; reduced server push frequency | Client must handle route deviation; complex fallback when dead reckoning diverges |

**Selected: Rate-limited server recalculation (every 15 s per active order) + client-side dead reckoning**

This hybrid approach provides the best trade-off: the server recomputes a fresh OSRM-based ETA every 15 seconds, while the client smoothly animates the Dasher's pin between updates using the last known heading and speed for visual smoothness.

**Implementation detail:**

```
ETA Recalc Service (Kafka consumer of location-events):

for each location_event in location-events partition:
    order_id = event.order_id
    last_recalc_key = "eta_recalc_ts:{order_id}"

    # Rate limit: at most one recalc per 15 s per order
    last_recalc_ts = redis.GET(last_recalc_key)
    if last_recalc_ts and (now - last_recalc_ts) < 15:
        continue  # skip; too recent

    # Fetch current order state
    order = redis.HGETALL("order:state:{order_id}")
    dasher_lat, dasher_lng = event.lat, event.lng

    if order.status in ['dasher_en_route_to_merchant', 'dasher_at_merchant']:
        # Leg 1: Dasher → Merchant
        leg1_seconds = osrm.route(dasher_lat, dasher_lng,
                                   order.merchant_lat, order.merchant_lng).duration
        # Leg 2: Merchant → Consumer (estimate; Dasher not yet departed)
        leg2_seconds = osrm.route(order.merchant_lat, order.merchant_lng,
                                   order.delivery_lat, order.delivery_lng).duration
        remaining_prep_min = max(0, order.estimated_ready_at - now).minutes
        total_eta_min = (leg1_seconds / 60) + remaining_prep_min + (leg2_seconds / 60)

    elif order.status in ['dasher_picked_up', 'dasher_en_route_to_consumer']:
        # Only leg remaining: Dasher → Consumer
        leg_seconds = osrm.route(dasher_lat, dasher_lng,
                                  order.delivery_lat, order.delivery_lng).duration
        total_eta_min = leg_seconds / 60

    else:
        continue  # not a trackable status

    # Store updated ETA
    redis.HSET("order:state:{order_id}", "eta_min", ceil(total_eta_min),
                                          "eta_updated_at", now_iso)
    redis.SETEX(last_recalc_key, 15, now_ts)  # rate limit lock

    # Publish ETA update event for fan-out
    kafka.publish("eta-update-events", order_id, {
        "order_id": order_id,
        "eta_minutes": ceil(total_eta_min),
        "updated_at": now_iso
    })
```

**OSRM call budget**: 360 000 active orders at peak ÷ 15 s recalc interval = 24 000 OSRM calls/s. Each order requires 1–2 OSRM route calls. Budget: ~48 000 OSRM calls/s. A self-hosted OSRM cluster can handle ~2 000 calls/s per node. We need ~24 OSRM nodes. At `t2.medium` cost (~$35/month), this is ~$840/month — trivial vs. Google Maps API cost at this volume (~$240K/month at $0.005/call).

**Client-side dead reckoning:** The consumer app receives `{dasher_lat, dasher_lng, dasher_heading_deg, dasher_speed_mps}` on each push. Between server updates (up to 4 s), the app predicts the Dasher's position:
```
predicted_lat = last_lat + (speed * cos(heading) * elapsed_time) / EARTH_RADIUS_IN_METERS
predicted_lng = last_lng + (speed * sin(heading) * elapsed_time) / EARTH_RADIUS_IN_METERS
```
This creates smooth map animation at 30+ FPS rather than a pin that jumps every 4 seconds. When the next server update arrives, the pin snaps to the actual GPS position if dead reckoning diverged (> 50 m).

**Interviewer Q&As:**

Q1: How does the system handle a Dasher who is stationary (waiting at a red light or parked)?
A: The rate limit key `eta_recalc_ts:{order_id}` prevents recalculation more than once per 15 s regardless of movement. If the Dasher is stationary, OSRM will return the same ETA (route unchanged; duration increases by 15 s of waiting). The ETA update is still published, so the consumer sees the time counting down correctly. Client-side dead reckoning with speed=0 keeps the pin stationary without jitter.

Q2: What is the end-to-end latency budget for an ETA update to reach the consumer?
A: Dasher GPS ping received by Location Service (10 ms) → Kafka publish (5 ms) → ETA Recalc Service consumes (30 ms Kafka lag) → OSRM call (10 ms) → Redis write + Kafka publish (10 ms) → Location Fan-out Service consumes (30 ms) → Redis Pub/Sub (5 ms) → WebSocket Gateway push (5 ms) → consumer WebSocket receive (10–100 ms network). Total: ~105–195 ms for a fresh ETA to reach the consumer. Well within the 10-second p99 target.

Q3: How do you handle OSRM returning an error for a specific route (e.g., a new road not in the map data)?
A: OSRM falls back to a "straight-line distance" estimation if no route is found. The ETA Service catches OSRM errors and computes: `fallback_eta_min = straight_line_distance_km / avg_speed_kmh + service_time`. Average speed is determined by `vehicle_type` (car: 30 km/h urban; bike: 15 km/h). The ETA update message includes `"confidence": "low"` in this case, and the consumer's app shows "approximately N minutes" rather than "N minutes" to signal reduced precision.

Q4: How would you improve ETA accuracy using historical traffic patterns?
A: OSRM supports custom speed profiles. We build a `time_of_week_speed` profile from Dasher telemetry: for each road segment in our OSRM map, we compute the 4-week rolling median speed by time-of-day and day-of-week bin (168 one-hour bins). This profile is rebuilt weekly from Cassandra location history data in an offline Spark job, then the OSRM routing graph is recomputed with the new speed data and hot-swapped (zero-downtime via a blue/green instance swap). This makes OSRM inherently aware that Friday evening is slower than Tuesday morning on Market Street.

Q5: What would cause ETA to jump suddenly (e.g., from 10 minutes to 25 minutes)?
A: Causes: (1) Dasher took a significantly wrong turn (app navigation failed); route recalculation finds a much longer path to the destination. (2) OSRM picked up a major road closure in the updated speed profile. (3) Dasher is picking up a stacked order (second order added to their batch) and is going to a different pickup location first. For user experience, sudden large ETA changes are smoothed: if the new ETA exceeds the old by more than 5 minutes, we send a `status_update` push notification ("Your delivery is taking a bit longer — we're sorry!") rather than silently updating the number.

---

### 6.3 Push Notification Delivery

**Problem it solves:**
Each order status transition must trigger timely, reliable notifications to the right actors. Consumers need to know when the Dasher is assigned, when their food is picked up, and when it's delivered. Merchants need to know when a Dasher is approaching for pickup coordination. Dashers need milestone confirmations. Notifications must be deduplicated (no double-vibrates), respect user preferences, be reliable even if the device is temporarily offline (APNs/FCM handle offline delivery), and not spam the consumer with too many notifications during a single order.

**Approaches comparison:**

| Approach                               | Description                                                     | Pros                                                     | Cons                                                             |
|----------------------------------------|-----------------------------------------------------------------|----------------------------------------------------------|------------------------------------------------------------------|
| Direct APNs/FCM call from Order Service | Order Service calls push provider directly on each state transition | Simple; immediate                                   | Tight coupling; Order Service must know about notification providers; no retry logic |
| Notification Service (Kafka consumer)  | Dedicated service consumes status events; calls APNs/FCM        | Separation of concerns; retry logic centralized; preference filtering in one place | Eventual consistency (Kafka lag adds 50–500 ms) |
| WebSocket-only (no push)               | All updates via WebSocket; no APNs/FCM                          | No push provider dependency; simpler                    | App must be foregrounded; terrible experience when backgrounded  |
| Fan-out service with priority queues   | Critical notifications (delivered) on high-priority queue; promo on low-priority | Better delivery guarantee for critical events  | More complex queue management                                   |

**Selected: Dedicated Notification Service consuming Kafka, with priority-tiered delivery**

**Implementation detail:**

**Step 1 — Event consumption and classification:**
The Notification Service consumes `notification-triggers` Kafka topic. Events are classified by urgency:
```
CRITICAL: order_placed, dasher_assigned, dasher_picked_up, delivered, cancelled
HIGH:     dasher_en_route_to_merchant, merchant_accepted, ready_for_pickup
INFO:     promotions, weekly_recap
```
CRITICAL events are processed from a high-priority partition (higher consumer throughput). INFO events are batched and processed lazily.

**Step 2 — Preference and device token lookup:**
```
For recipient_id, order notification_type:
1. Fetch notification_preferences from Redis (TTL 5 min, backed by PostgreSQL)
2. If push_enabled = false → skip push; check SMS/email flags
3. Fetch device_tokens for recipient_id from PostgreSQL (cached in Redis, TTL 1 hr)
4. If no active tokens → skip push; attempt email
```

**Step 3 — Deduplication:**
```
dedup_key = "notif_dedup:{recipient_id}:{order_id}:{notification_type}"
if redis.SET(dedup_key, 1, EX=300, NX=True):
    proceed with notification
else:
    skip (duplicate within 5-minute window)
```

**Step 4 — APNs/FCM dispatch:**
- **iOS (APNs)**: Use APNs HTTP/2 API. Send with `apns-priority: 10` (critical) or `apns-priority: 5` (normal). Include `apns-push-type: alert` for user-facing notifications.
- **Android (FCM)**: Use FCM HTTP v1 API. Set `priority: "high"` for CRITICAL events.
- Both are called via an internal connection pool (HTTP/2 persistent connections to APNs/FCM endpoints). Connection establishment is amortized over many calls.
- Multi-device: if a user has 2 active tokens (iPhone + iPad), the notification is sent to both.

**Step 5 — Failure handling:**
```
APNs/FCM error codes:
  - "Unregistered" → deactivate that device_token in PostgreSQL
  - "BadDeviceToken" → deactivate token
  - "TooManyRequests" → back off exponentially; requeue message
  - Network timeout → retry 3× with exponential backoff (1 s, 2 s, 4 s)
  - After 3 failures → publish to notification-dlq Kafka topic; alert
  - Log all outcomes to Cassandra notification log
```

**Step 6 — Notification payload construction:**
```json
// Consumer notification: Dasher picked up order
{
  "notification": {
    "title": "Your order is on the way! 🚗",
    "body": "Alex picked up your Taqueria El Sol order. ETA: 12 min",
    "sound": "default"
  },
  "data": {
    "type": "order_status_update",
    "order_id": "uuid",
    "new_status": "dasher_picked_up",
    "deep_link": "doordash://orders/{order_id}/tracking"
  },
  "apns": {
    "headers": { "apns-priority": "10", "apns-push-type": "alert" },
    "payload": { "aps": { "content-available": 1 } }
  }
}
```

The `deep_link` opens the tracking screen directly when the consumer taps the notification.

**Interviewer Q&As:**

Q1: What is the deliverability rate target, and how is it measured?
A: Target is 99.5% of CRITICAL notifications delivered within 10 seconds. Measurement: every notification sent is logged in Cassandra with `sent_at`. APNs and FCM provide delivery receipts (APNs sends a feedback service response for failed tokens; FCM provides delivery reports via the v1 API). A Spark job daily computes delivery rates from the notification log, cross-referenced with FCM delivery receipt data. Tokens with consistent delivery failures (> 3 in 7 days) are automatically deactivated.

Q2: What happens if APNs is down for 5 minutes?
A: APNs has SLA of 99.9% (about 8 hours downtime/year). If APNs returns 5xx errors: (1) Circuit breaker opens for APNs. (2) CRITICAL notifications are routed to SMS fallback (Twilio) if the user has a phone number on file and `sms_enabled=true`. (3) Non-CRITICAL notifications are queued in a Kafka dead-letter topic for retry when APNs recovers. (4) WebSocket pushes continue to work for consumers with active app sessions. (5) On-call alert fires for monitoring.

Q3: How do you prevent notification spam if a Dasher rapidly transitions through multiple states in 30 seconds?
A: The deduplication key includes `notification_type`, so "dasher_at_merchant" and "dasher_picked_up" are distinct events and both fire. However, we apply an event debounce: if the same `(order_id, notification_type)` pair fires within 60 seconds, only the first notification is sent. This prevents duplicate "Dasher picked up your order" notifications if the Dasher's app sends the milestone twice due to a retry.

Q4: How would you design the notification system to support multiple languages?
A: The notification payload template is keyed by `(notification_type, locale)`. A `notification_templates` table in PostgreSQL stores translated `title_template` and `body_template` per locale (e.g., `{locale: 'es-US', notification_type: 'dasher_picked_up', title: "¡Tu pedido está en camino!", body: "{dasher_name} recogió tu pedido de {merchant_name}. ETA: {eta_min} min"}`). The Notification Service fetches the consumer's locale from their profile (stored in the JWT claims) and renders the appropriate template. New locales are added by uploading translations to the `notification_templates` table without code changes.

Q5: How would you handle a consumer who has no internet connection when a critical notification is sent (e.g., they are in a tunnel)?
A: APNs and FCM are designed for this exact scenario. When the Notification Service sends a push to APNs/FCM, the provider stores the notification for delivery when the device reconnects. APNs stores the most recent notification per app per device (last-write-wins for the same `apns-collapse-id` key). For our tracking notifications, we set `apns-collapse-id: order-{order_id}-status` so that if multiple status updates were queued while the consumer was offline, only the latest status is shown when they reconnect — not a flood of stale "Dasher at merchant," "Dasher picked up," etc. FCM has an equivalent `collapse_key` parameter.

---

## 7. Scaling

### Horizontal Scaling

**Location Service**: Stateless write handlers. At 125 000 writes/s: each handler can process ~2 000 writes/s (Redis + Cassandra writes are async; main overhead is JSON parsing and validation). Requires ~65 pods. In practice, co-locate with NUMA-aware hardware for Redis latency. Kubernetes HPA on RPS metric.

**WebSocket Gateway**: Stateful (each node owns a set of connections). Scale out by adding nodes; new connections are distributed via a round-robin or least-connections load balancer rule. Connections to existing orders stay on their node until reconnect. Target: 125 K connections per node = 8 nodes at 1 M peak connections.

**Location Fan-out Service**: Kafka consumer group. Scale up to the number of `location-events` partitions (50 partitions). Each consumer publishes to Redis Pub/Sub and optionally to the `eta-update-events` topic. Scale horizontally to reduce per-consumer message lag.

**ETA Recalc Service**: Kafka consumer group with rate-limiter per order (stored in Redis). Stateless compute; call OSRM. Scale to maintain < 1-second Kafka consumer lag at 24 000 recalculations/s. Each pod handles ~500 OSRM calls/s (bounded by OSRM cluster throughput). Requires ~48 OSRM-calling pods; OSRM cluster needs 24 nodes.

**Notification Service**: Kafka consumer group on `notification-triggers`. At 270 pushes/s, this is a very low-throughput service. 2 pods with auto-scale on queue depth. APNs/FCM calls are the bottleneck; both providers support batching (FCM: up to 500 messages per batch request).

### Database Sharding

**Cassandra (location history)**: Natural horizontal scaling. Partition key `dasher_id` distributes writes across all nodes. At 125 K writes/s sustained, a 12-node cluster (each node handles ~10 K writes/s at 50% utilization) provides sufficient headroom. Adding nodes is transparent via the Cassandra token ring.

**Redis (Pub/Sub + active order state)**: Redis Cluster handles sharding automatically. The Pub/Sub channel `tracking:{order_id}` is hash-slotted to a specific node, and all WebSocket Gateway nodes subscribe via the cluster's Pub/Sub proxy. Redis Cluster scales to 1 000 nodes (3 master + 1 replica per shard practical limit for most deployments). At 800 K WebSocket connections' metadata, Redis memory usage is ~800 K × 200 bytes = ~160 MB — trivial.

### Replication

- **Redis**: Each Pub/Sub primary has 1 replica. Replica promotes automatically on primary failure via Redis Sentinel or Cluster's internal FAIL detection.
- **Cassandra**: RF=3, LOCAL_QUORUM for writes and reads. Survives 1 node failure per DC.
- **PostgreSQL (Aurora)**: 1 writer, 2 read replicas. Status history writes go to writer; status history reads (order detail timeline) go to read replicas.
- **Kafka**: RF=3 per topic partition, ISR=2. Ensures no data loss with 1-broker failure.

### Caching Strategy

| Data                           | Cache         | TTL         | Invalidation                                          |
|--------------------------------|---------------|-------------|-------------------------------------------------------|
| Active order state             | Redis HASH    | 4 hr        | Overwritten on each status update                    |
| Current Dasher position        | Redis STRING  | 10 s        | Overwritten on each GPS ping                         |
| ETA per order                  | Redis HASH    | 4 hr        | Overwritten on each ETA recalculation                |
| Notification preferences       | Redis HASH    | 5 min       | Invalidated by `PUT /notifications/preferences`      |
| Device tokens per user         | Redis LIST    | 1 hr        | Invalidated by token registration/deactivation       |
| WS session registry            | Redis HASH    | 2 hr        | Updated on connect/disconnect; heartbeat refresh     |
| Dasher active order            | Redis STRING  | 2 hr        | Set on dispatch; cleared on delivery/cancellation    |
| Order status history (past)    | CDN/Redis     | 1 hr        | Only for terminal orders (delivered/cancelled)       |

**Interviewer Q&As:**

Q1: How do you handle Redis Pub/Sub "lost messages" if a subscriber (WS Gateway node) restarts?
A: Redis Pub/Sub is fire-and-forget — if a subscriber is offline when a message is published, it misses it. This is acceptable for location update messages because the next GPS ping arrives within 4 seconds and the consumer's map will refresh. For status change messages (which must not be missed), we use a different delivery path: the Order Service publishes to Kafka `order-status-events`, and the Notification Service delivers via APNs/FCM (durable, store-and-forward). Status changes are also written to the `order:state:{order_id}` Redis HASH, so a consumer who reconnects immediately gets the current status in the connection handshake response.

Q2: How does the system handle 3× peak load during major holidays (e.g., Super Bowl Sunday)?
A: Pre-scaling: based on historical traffic patterns, we scale the WebSocket Gateway, Location Service, and ETA Recalc Service to 3× capacity the morning before the event. Auto-scaling lag (typically 2–5 min for Kubernetes HPA) is insufficient for sudden spikes. Cassandra and Redis clusters are pre-warmed; OSRM fleet is doubled. Post-event, pods are scaled back via scheduled scaling jobs. We also increase Kafka partition count for `location-events` topic 24 hours before the event (requires partition reassignment, done during off-peak the night before).

Q3: How do you scale the OSRM cluster? It seems like a bottleneck.
A: OSRM is horizontally scalable — each instance is stateless (the routing graph is loaded into memory from a file). We run OSRM behind a load balancer (nginx round-robin). Each OSRM instance loads the full US routing graph (~8 GB in memory). Adding nodes is simply launching new EC2 instances, downloading the routing graph from S3, and adding them to the load balancer target group. Autoscaling on CPU ensures capacity is proportional to ETA recalculation load. Each `r5.2xlarge` instance (64 GB RAM, 8 vCPUs) handles ~2 000 route calls/s.

Q4: How would you handle the WebSocket Gateway at 10× scale (8 M concurrent connections)?
A: Each WebSocket Gateway node can handle ~150 K connections. At 8 M connections: 54 nodes. The Redis Pub/Sub fan-out becomes a bottleneck at this scale: 630 000 messages/s published × 54 subscribers = ~34 M Redis Pub/Sub deliveries/s, approaching the limit of a Redis Cluster. Alternative: move from Redis Pub/Sub to a dedicated message bus (Apache Pulsar, which provides per-topic consumer subscriptions without full fan-out, or NATS JetStream for higher-throughput pub/sub). Alternatively, instead of all nodes subscribing to all channels, use a routing table: `ws_session_to_node:{order_id} → node_id`, and the Fan-out Service directly calls the relevant node's internal API, bypassing Pub/Sub fan-out overhead.

Q5: How would you reduce battery drain on the consumer's phone from WebSocket keep-alives?
A: The WebSocket client sends a `{ "type": "ping" }` every 30 seconds. This is a single small frame and has negligible battery impact vs. the 30-second cell radio activity it triggers. To optimize: (1) Use `content-available: 1` APNs silent pushes to wake the app only for significant status changes; (2) Once the Dasher is > 10 minutes away, reduce the consumer's WebSocket update frequency to once per 15 seconds (the server checks `eta_min > 10` and skips pushing on intermediate pings, only pushing ETA updates); (3) When the consumer backgrounds the app (detected via `app_backgrounded` WebSocket message from client), pause location pushes and rely solely on APNs/FCM for status transitions. This mirrors what DoorDash and Uber Eats actually do.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario                        | Impact                                             | Detection                                    | Mitigation                                                                       |
|-----------------------------------------|----------------------------------------------------|----------------------------------------------|----------------------------------------------------------------------------------|
| WebSocket Gateway node failure          | All connected sessions on that node disconnect     | Kubernetes pod health check; connection metric drops | Clients auto-reconnect to remaining nodes; exponential backoff reconnect; state restored from Redis on reconnect |
| Redis Cluster node failure              | Pub/Sub messages lost for ~1–5 s (failover window)| Redis Sentinel / Cluster heartbeat            | 1-4 s gap in location pushes; clients see pin freeze; next ping recovers. Status events use Kafka path (durable) |
| Kafka consumer lag (Fan-out Service)    | Location updates delayed beyond 5 s               | Consumer lag metric > 500 messages            | Auto-scale Kafka consumer group; alert on-call; location data is not lost, just delayed |
| OSRM cluster down                       | ETA cannot be recomputed                           | Health check; circuit breaker                | Fall back to speed-based estimate: `remaining_distance / avg_speed`. ETA shows "approximately" qualifier |
| APNs/FCM down                           | Push notifications not delivered                  | Error rate > 1% on push calls                | Circuit breaker; SMS fallback for critical events; queue for retry when provider recovers |
| Cassandra node failure (during write)   | Some location writes fail to one replica          | Write quorum failure triggers exception       | LOCAL_QUORUM: write to 2 of 3 still succeeds; data remains durable; failed replica catches up via hinted handoff |
| Network partition: Consumer app ↔ server | WebSocket disconnects; polling falls back         | Client detects TCP close or timeout           | Client uses polling fallback; server state is consistent; reconnect delivers current state |
| Dasher app crash mid-delivery           | No GPS pings for extended period                  | `dasher:pos:{id}` TTL expires (10 s)         | ETA falls back to last known position + estimated progress; consumer sees "location temporarily unavailable"; Ops alert if > 5 min gap |
| Order Service fails to publish status event | Consumer/merchant miss a status update        | Kafka producer error; transactional outbox    | Outbox pattern ensures eventual delivery; status is always consistent in PostgreSQL |
| Duplicate Dasher location writes        | Duplicate location updates may cause ETA recalc jitter | Processing same event twice               | Idempotent Redis SETEX (overwrites with same or close value); Cassandra upsert by primary key |

### Retries & Idempotency

- **Location writes**: Idempotent. Redis `SETEX` overwrites regardless; Cassandra upsert by `(dasher_id, recorded_at)`. Duplicate pings are harmless.
- **Status transitions**: Protected by `SELECT ... FOR UPDATE` + version check in PostgreSQL. Each transition is idempotent if the same transition is re-attempted — the system checks `current_status == expected_status` before applying.
- **Push notifications**: Deduplicated via Redis `SET NX` with 5-minute TTL. APNs/FCM retried 3× with exponential backoff.
- **ETA recalculation**: Rate-limited by Redis key; idempotent OSRM calls; Redis HSET is idempotent.
- **Kafka publishing**: Producer idempotence enabled (`enable.idempotence=true`). Combined with `acks=all` and `retries=MAX_INT`, guarantees at-least-once delivery with deduplication.

### Circuit Breaker Configuration

| Dependency          | Failure Threshold      | Open Duration | Fallback                                    |
|---------------------|------------------------|---------------|---------------------------------------------|
| OSRM routing        | 5 failures in 10 s     | 30 s          | Speed-based ETA estimate                   |
| APNs                | 10 failures in 30 s    | 60 s          | SMS (Twilio) for critical events            |
| FCM                 | 10 failures in 30 s    | 60 s          | WebSocket push if session active            |
| Redis (Pub/Sub)     | Write error rate > 20% | 30 s          | Direct WS Gateway HTTP call via service mesh|
| Cassandra           | Write latency > 1 s p95| 10 s          | Location data write to Redis only (temporary) |

---

## 9. Monitoring & Observability

### Key Metrics

| Metric                                       | Type        | Alert Threshold               | Purpose                                              |
|----------------------------------------------|-------------|-------------------------------|------------------------------------------------------|
| location_e2e_latency_p99_ms                  | Histogram   | > 5 000 ms                    | End-to-end tracking SLA violation                   |
| websocket_active_connections                 | Gauge       | > 900 K (capacity warning)    | Gateway capacity planning                           |
| websocket_reconnect_rate_per_min             | Counter     | > 10% of sessions/min         | Network instability or gateway issue                |
| kafka_consumer_lag_location_fanout           | Gauge       | > 1 000 messages              | Fan-out service falling behind                      |
| kafka_consumer_lag_eta_recalc                | Gauge       | > 2 000 messages              | ETA recalculation falling behind                    |
| push_notification_delivery_rate              | Counter     | < 99.5% for CRITICAL events   | Notification SLA violation                          |
| push_notification_latency_p95_ms            | Histogram   | > 10 000 ms                   | Slow APNs/FCM delivery                              |
| dasher_location_write_success_rate           | Counter     | < 99.9%                       | Location ingest failures                            |
| eta_absolute_error_p80_minutes               | Histogram   | > 8 min                       | ETA model degradation                               |
| osrm_latency_p99_ms                         | Histogram   | > 200 ms                      | OSRM slowdown; ETA pipeline impact                  |
| active_orders_without_location_update_5min  | Gauge       | > 100 orders                  | Stuck tracking sessions                             |
| redis_pubsub_channel_backlog                | Gauge       | > 500 messages per channel    | Fan-out bottleneck                                  |
| circuit_breaker_open_events                 | Event       | Any occurrence                | Dependency failure                                  |
| apns_invalid_token_rate                     | Counter     | > 5% of sends                 | Stale device token database                         |

### Distributed Tracing

- OTEL SDK instrumented across: Dasher App (client-side trace start) → API Gateway → Location Service → Kafka → Fan-out Service → Redis Pub/Sub → WebSocket Gateway → Consumer App (client-side trace end with `received_at` timestamp).
- `traceparent` header propagated in Kafka message headers.
- Trace spans: `gps_ingest`, `redis_write`, `cassandra_write`, `kafka_publish`, `fanout_consume`, `pubsub_publish`, `ws_push`. Each span records its individual latency contribution.
- End-to-end trace enables identification of the slowest step in the pipeline. Example: if `cassandra_write` is consistently > 50 ms, it's the Cassandra cluster that needs attention, not OSRM.
- Sampling: 1% of all location traces; 100% of traces where E2E latency > 2 s (tail-based sampling via OpenTelemetry Collector with tail sampling processor).

### Logging

- Structured JSON: `{ service, trace_id, span_id, order_id, dasher_id, consumer_id, event_type, latency_ms, status, level, timestamp }`.
- Location writes logged at DEBUG level per ping (too verbose for WARN); aggregated statistics logged at INFO every 60 s.
- Status transitions logged at INFO with full context: `{ order_id, from_status, to_status, actor_role, latency_ms }`.
- Push notification outcomes logged at INFO (success) and ERROR (failure) in the Notification Service.
- Logs stream to ELK/OpenSearch. Retention: 7 days hot, 90 days warm, 1 year cold.
- PII handling: `consumer_id` logged as UUID only; no names or addresses in logs; `dasher_id` as UUID only; GPS coordinates logged at H3 cell granularity (not exact position) for privacy compliance.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                                  | Choice Made                                  | Alternative                              | Rationale                                                              | Trade-off Accepted                                                   |
|-------------------------------------------|----------------------------------------------|------------------------------------------|------------------------------------------------------------------------|----------------------------------------------------------------------|
| Real-time protocol                        | WebSocket primary + HTTP polling fallback    | SSE / long-polling                       | Bidirectional; lowest latency; efficient framing                       | Stateful servers; connection registry complexity                     |
| Cross-node WS fan-out                     | Redis Pub/Sub                                | Dedicated message bus (NATS, Pulsar)     | Redis already in stack; simple pattern subscription; sufficient at current scale | At 10M connections, Redis Pub/Sub fan-out becomes a bottleneck      |
| ETA recalculation frequency               | Rate-limited to 1/15 s per active order      | Every ping / every 60 s                  | Balanced freshness vs. OSRM cost; 15 s staleness acceptable            | ETA may lag reality by up to 15 s during route deviations            |
| ETA routing engine                        | Self-hosted OSRM                             | Google Maps API                          | Cost ($840/month vs. $240K/month); latency control; traffic feedback loop | OSRM cluster operational burden; map data freshness management       |
| Client-side dead reckoning                | Implemented in consumer app                  | Higher server push frequency             | Smooth map animation without 4× higher server push cost               | Dead-reckoned pin position is approximate; snaps on server update    |
| Push notification deduplication           | Redis SET NX with 5-min TTL                  | DB-level unique constraint               | Sub-millisecond check vs. DB query; TTL auto-expires                  | Redis failure during dedup window could allow duplicates (rare)      |
| Notification fallback (APNs down)         | SMS via Twilio for CRITICAL events           | Email / in-app only                      | SMS is the most reliable channel when app is backgrounded              | Per-message Twilio cost (~$0.0075/SMS); only triggered on APNs failure |
| Location data retention                   | 30 days in Cassandra, then deleted           | Indefinite retention                     | Privacy (GDPR Art. 5 data minimization); cost reduction                | Cannot investigate delivery incidents from > 30 days ago            |
| Status history storage                    | PostgreSQL (durable, ACID)                   | Cassandra / event store                  | Low write rate (30/s); strong consistency for dispute resolution       | PostgreSQL scaling limit (mitigated by sharding)                     |

---

## 11. Follow-up Interview Questions

**Q1: How would you design the "Dasher is approaching your address" geofence notification?**
A: Define a geofence: a circle with radius 500 m centered on the consumer's delivery address. After the Dasher picks up the food, every location update checks: `haversine(dasher_lat, dasher_lng, delivery_lat, delivery_lng) < 500 m`. The check runs in the Fan-out Service (< 0.1 ms arithmetic). When first triggered, publish a `geofence_enter` event to `notification-triggers` Kafka topic with `notification_type = dasher_approaching`. Deduplicated so it fires only once per order. This notification is shown even if the consumer has the app foregrounded (as a banner) and is particularly useful when backgrounded. The same mechanism supports "Dasher arriving at restaurant" geofence for merchant coordination.

**Q2: How would you design the "live order status" view for the merchant tablet that shows all active orders simultaneously?**
A: The merchant app subscribes to a WebSocket channel per restaurant: `wss://ws.doordash.com/v1/merchant/{merchant_id}/orders`. The WebSocket Gateway maintains a connection per merchant session. The Order Service publishes all order status changes for a given merchant to `order-status-events` Kafka, and the Fan-out Service additionally publishes to a Redis Pub/Sub channel `merchant:{merchant_id}:orders`. The merchant WebSocket session receives all order updates in one stream and renders a list sorted by `estimated_ready_at`. The merchant tablet app thus shows a real-time dashboard of all in-flight orders without polling.

**Q3: How would you implement the "rate your Dasher" prompt that appears exactly at delivery?**
A: The Notification Service handles the `delivered` status event. After a configurable delay (2 minutes, to let the consumer settle and check their food), the Notification Service sends a push notification: "How was your delivery? Rate Alex." This delay is implemented via a delayed Kafka message: the Notification Service publishes to `notification-triggers` with `deliver_at = now + 2min`. A scheduled processor in the Notification Service (running a sorted-set delay queue in Redis, keyed by `deliver_at` timestamp) pops expired entries and calls APNs/FCM. This avoids blocking Kafka consumers for 2 minutes and keeps the notification delivery architecture stateless.

**Q4: How would you handle "order bundling" — one Dasher picking up two orders from the same restaurant?**
A: In this scenario, one Dasher is linked to two `order_id`s. The `dasher:active_orders:{dasher_id}` becomes a SET of order IDs rather than a single string. The Fan-out Service, on each location ping, iterates through all active orders for the Dasher and publishes to all `tracking:{order_id}` channels. Both consumers track the same Dasher's position. ETA Recalc Service handles both orders independently (same Dasher route, different delivery addresses). The second consumer's ETA is correctly longer since the Dasher delivers in sequence. The order detail screen shows: "Alex is also delivering another order and will come to you second."

**Q5: How would you design "contactless delivery" with photo proof?**
A: The Dasher app prompts the Dasher to take a photo at the drop-off point. The app uploads the photo directly to S3 using a presigned URL (obtained from `GET /dasher/delivery-photo-upload-url/{order_id}`). The presigned URL expires in 5 minutes. The upload triggers an S3 event notification → Lambda → writes the `photo_url` to the order record in PostgreSQL and publishes a `delivery_proof_uploaded` event. The consumer receives a push notification with a deep link to the photo. The photo is retained for 90 days (dispute window) then deleted via S3 lifecycle policy. The Dasher cannot mark the order as "delivered" in the app until the photo upload ACK is received (enforced at the API layer: `PATCH /dasher/orders/{id}/milestone` with `milestone=delivered` requires `photo_s3_key` in the body).

**Q6: How does the system handle time zones for multi-city deployments?**
A: All timestamps in the system are stored in UTC. The consumer app converts to local time for display. The API returns `occurred_at` as ISO 8601 with `Z` (UTC) suffix. The ETA recalculation uses local time only for the OSRM speed profile lookup (time-of-day traffic patterns are inherently local). The OSRM speed profile is parameterized by `(road_segment_id, local_hour_of_week)` using the road segment's city timezone. The Location Service determines the timezone from the lat/lng using a timezone lookup library (timezone-boundary-builder) compiled into the service.

**Q7: How would you design a "friend tracking" feature — a consumer can share a live tracking link with someone who doesn't have the app?**
A: Generate a shareable short URL `https://drd.sh/track/{short_token}` where `short_token` is a signed JWT embedding `{order_id, share_expiry, role: viewer}`. The token is signed with a share-specific key with expiry = `estimated_delivery_at + 30 min`. The recipient opens the URL in a browser; a web app loads and establishes a WebSocket connection using the token as auth. The WebSocket Gateway validates the share token (signature + expiry) and subscribes the viewer to the same `tracking:{order_id}` channel as the consumer. No account creation required. The consumer can revoke sharing by calling `DELETE /orders/{order_id}/share`.

**Q8: How would you prevent a malicious party from flooding the Dasher location ingest endpoint?**
A: Multiple layers: (1) JWT authentication is required for `POST /dasher/location`. A stolen token is rate-limited to 1 write/3 s per `dasher_id`. (2) API Gateway rate limits to 1 req/3 s per source IP + per `dasher_id` claim. (3) GPS validation rejects coordinates outside a valid bounding box (US + Canada), implausible speeds, or timestamps more than 30 s old. (4) An anomaly detection job monitors `location writes per dasher per hour` against a baseline; a sudden 100× spike alerts on-call. (5) Device fingerprinting — the Dasher app is published with certificate pinning; requests from unknown TLS clients are rejected at the API Gateway.

**Q9: How would you design an "operations dashboard" for internal teams to manually intervene in stuck deliveries?**
A: An internal `ops-portal` web application with its own JWT-based auth (SAML-integrated with corporate SSO). The dashboard reads from: (1) PostgreSQL orders with filters by status, region, age. (2) A "stuck order" index: orders where `updated_at < now - 15 min AND status NOT IN ('delivered', 'cancelled')` — computed as a Redis ZSET sorted by `updated_at`, populated by the Order Service. Ops agents can: force a status transition (with a mandatory reason field logged in `order_status_history`), manually assign/reassign a Dasher, trigger a consumer refund, and see the full Dasher location history for the order. All ops actions require a second-factor approval from a senior agent (two-person rule) for irreversible actions (refund, forced cancellation).

**Q10: How would you measure the quality of the live tracking experience?**
A: Four key product metrics: (1) **Tracking engagement rate**: % of consumers who open the tracking screen during a delivery (benchmarks against internal goal). (2) **Tracking-to-complaint rate**: correlation between long ETA error and post-delivery "food took too long" ratings. (3) **Location update freshness**: median time between the consumer's last received location update and the actual Dasher position (measured by cross-referencing Consumer WebSocket receive timestamp vs. Dasher GPS timestamp). (4) **App crash/disconnect rate on tracking screen**: measured via Firebase Crashlytics and WebSocket `close_code` distribution. A `close_code` of 1006 (abnormal close) indicates a crash or network failure.

**Q11: What happens if a consumer submits the "Delivered" confirmation themselves before the Dasher taps the delivered button?**
A: This scenario is prevented by the state machine. Only the Dasher (role=dasher, the specific `dasher_id` on the order) can trigger the `delivered` milestone via `PATCH /dasher/orders/{id}/milestone`. The consumer cannot trigger a state transition to `delivered`. The consumer's `order_complete` signal is a separate concept — for "leave at door" orders, the Dasher taps delivered, and the consumer is not required to confirm. There is no race condition because the Order Service enforces role-based transition permissions in addition to valid-state checks.

**Q12: How would you support offline mode for the Dasher app — what happens if the Dasher loses connectivity for 60 seconds?**
A: The Dasher app buffers GPS pings locally when network is unavailable (using a local SQLite queue). On reconnect, the app sends the buffered pings in order. The Location Service accepts backfilled pings if `timestamp_ms < now - 300 s` (5-minute grace window), writes them to Cassandra, but does NOT fan them out to consumers (stale data would confuse the map). Only the most recent ping position is written to Redis for dispatch/ETA purposes. If the Dasher was supposed to tap a milestone during the offline window, the app queues the milestone action locally and sends it on reconnect. The Order Service accepts milestone taps with timestamps up to 60 s in the past (configurable grace window for network latency).

**Q13: How would you design the system to support tracking a grocery order that takes 45 minutes to shop (vs. 15-minute food delivery)?**
A: The ETA model and tracking behavior need to accommodate a "shopping" phase (Dasher browsing store aisles). The status machine gains a new state: `dasher_shopping` between `dasher_at_merchant` and `dasher_picked_up`. During shopping, GPS pings continue at 4 s intervals, but the consumer map shows "Your shopper is picking items" rather than a moving pin (the Dasher is moving slowly inside a store — showing the exact aisle position is neither accurate nor useful). ETA recalculation during shopping uses a different model: `remaining_shopping_items * avg_time_per_item + checkout_wait + drive_time`. The shopping item count is provided by the merchant's inventory system. This requires a new `shopping_items_remaining` field in the active order state and a new Kafka event type `item_picked` published by the Dasher as they mark each item.

**Q14: How would you implement server-sent confirmation of delivery to the payment system?**
A: The Order Service, on transitioning to `delivered` status, publishes an `order.delivered` event to Kafka. The Payment Service consumes this event and releases the Stripe payment capture hold (payments are authorized but not captured at order placement; captured on delivery). This ensures consumers are only charged for orders that are actually delivered. If payment capture fails (expired authorization), the Payment Service issues a re-authorization or falls back to alerting the consumer. The delivery event also triggers the tip release (tip is held separately until delivery confirmation, protecting the consumer in case of non-delivery).

**Q15: How would you design the tracking system to scale to 10M simultaneous orders (vs. 360K today)?**
A: Three main bottlenecks to address at 10M scale: (1) **WebSocket Gateway**: From 8 nodes to ~70 nodes at 150 K connections each. Add a routing tier: instead of all nodes subscribing to all Pub/Sub channels, a connection registry service (`ws-registry`) maps `order_id → node_id`. The Fan-out Service queries `ws-registry` and calls the specific node's internal API directly. This eliminates broadcast fan-out overhead. (2) **Redis Pub/Sub**: At 10M active orders with 125K location pings/s, the Pub/Sub load grows 28×. Replace Redis Pub/Sub with Apache Pulsar (supports per-topic subscriptions with horizontal partition scaling) or NATS JetStream. (3) **Cassandra location writes**: At 1.25 M writes/s (assuming 10× more Dashers), scale Cassandra cluster from 12 to 120 nodes — purely linear scaling by adding nodes. OSRM cluster scales to 240 nodes accordingly.

---

## 12. References & Further Reading

- DoorDash Engineering Blog — "Rebuilding Our Dasher Dispatch System": https://doordash.engineering/2020/06/18/dispatching-millions-of-tasks/
- Uber Engineering Blog — "How Uber Handles Millions of Ride/Delivery Requests Efficiently" (Real-time data processing): https://eng.uber.com/real-time-exactly-once-ad-event-processing/
- Lyft Engineering Blog — "Building a Real-time Map" (WebSocket fleet tracking): https://eng.lyft.com/building-a-real-time-map-f33b08b1f194
- WebSocket RFC 6455: https://www.rfc-editor.org/rfc/rfc6455
- Apple Push Notification Service (APNs) Documentation: https://developer.apple.com/documentation/usernotifications/sending-notification-requests-to-apns
- Firebase Cloud Messaging (FCM) HTTP v1 API: https://firebase.google.com/docs/cloud-messaging/send-message
- OSRM (Open Source Routing Machine) Documentation: http://project-osrm.org/docs/v5.24.0/api/
- Redis Pub/Sub Documentation: https://redis.io/docs/manual/pubsub/
- Apache Cassandra — Time Series Data Modeling: https://cassandra.apache.org/doc/latest/cassandra/data_modeling/data_modeling_rdbms.html
- Kleppmann, Martin — "Designing Data-Intensive Applications" (O'Reilly, 2017) — Chapter 11 (Stream Processing), Chapter 9 (Consistency and Consensus)
- OpenTelemetry Documentation — Distributed Tracing: https://opentelemetry.io/docs/concepts/signals/traces/
- H3 Geospatial Indexing System: https://h3geo.org/docs/
- Apache Kafka — Idempotent Producer: https://kafka.apache.org/documentation/#producerconfigs_enable.idempotence
- AWS Aurora Multi-AZ Documentation: https://docs.aws.amazon.com/AmazonRDS/latest/AuroraUserGuide/Concepts.AuroraHighAvailability.html
- Google Maps Platform — Routes API (for comparison with OSRM): https://developers.google.com/maps/documentation/routes
