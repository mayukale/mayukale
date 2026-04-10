# System Design: Calendar Service (Google Calendar-style)

---

## 1. Requirement Clarifications

### Functional Requirements

1. Users can create, edit, and delete calendar events with title, description, start/end time, and location.
2. Support for recurring events following RFC 5545 (iCalendar) recurrence rules (RRULE): daily, weekly, monthly, yearly, with exceptions (EXDATE) and modifications to single instances (THIS and FUTURE scope).
3. Full timezone support — events are stored with timezone information; display is adjusted to the viewer's current timezone.
4. Users can invite other users to events; invitees receive invite notifications and can RSVP (Accept / Decline / Maybe).
5. Calendar sharing: users can share their calendar with specific users at different permission levels (view free/busy only, view details, edit).
6. Conflict detection: warn users when a new event overlaps with an existing event.
7. Reminders: configurable per-event reminders via email, push notification, or SMS (e.g., 15 minutes before event).
8. Multiple calendars per user (Work, Personal, Holidays) with color coding. Events belong to a calendar.
9. External calendar sync: import/export iCalendar (.ics) files; subscribe to external iCal feeds (read-only).
10. Search: full-text search over event titles and descriptions within a user's accessible calendars.
11. Free/busy query: return a user's busy time slots for a given range (for scheduling assistants).

### Non-Functional Requirements

1. High availability: 99.99% uptime — calendar is a productivity-critical service.
2. Strong consistency within a user's own events (reads must reflect writes immediately for the same user).
3. Eventual consistency acceptable for cross-user features (invite RSVP status visible to organizer within seconds).
4. Low latency: calendar view load (fetching events for a month) p99 < 200 ms.
5. Scale: 2 billion users globally (Google scale assumption); 500 million DAU.
6. Write throughput: ~50,000 events created/minute (≈ 833 writes/second at normal operation).
7. Recurring event expansion must not generate excessive storage — use on-the-fly expansion.
8. Reminder delivery: guaranteed delivery within ±30 seconds of scheduled time.
9. Multi-device sync: event changes on one device must appear on other devices within 3 seconds.
10. GDPR compliance: users can export all their data and request deletion.

### Out of Scope

- Video/audio conferencing integration (Google Meet / Zoom links stored as metadata, not built here).
- Resource booking (meeting rooms, equipment) — mentioned as an extension.
- Calendar analytics / productivity insights.
- Task management (Google Tasks).
- Group/team calendar management with complex permissions (enterprise feature).
- Push notification delivery infrastructure (assume FCM/APNs).
- Email infrastructure (assume SendGrid/SES for reminder emails).

---

## 2. Users & Scale

### User Types

| Role                 | Description                                                                         |
|----------------------|-------------------------------------------------------------------------------------|
| Owner                | Full control over a calendar and its events; can share with others                 |
| Editor               | Can create/edit/delete events on a shared calendar                                  |
| Viewer               | Can see event details on a shared calendar                                          |
| Free/Busy Viewer     | Can only see time slots as busy/free, not event details                             |
| Invitee              | Invited to a specific event; can RSVP; sees event on their calendar                |
| External/Subscriber  | Subscribes to an iCal feed; read-only; no account required                         |

### Traffic Estimates

**Assumptions:**
- 2 billion registered users; 500 million DAU (25% DAU ratio — high for a default mobile app).
- Average user opens calendar 3 times/day (view), creates 1 event/week.
- 500M DAU * 3 opens = 1.5B calendar views/day.
- Event creation: 500M * (1/7) = ~71M events created/day.
- Recurring events: ~40% of all events; average 52 occurrences/year per recurring event.
- Invites sent: avg 2.5 invitees per created event = 71M * 2.5 = 177M invites/day.
- Reminders: avg 1.5 reminders per event = 71M * 1.5 = 106M reminders/day.

| Metric                           | Calculation                                              | Result            |
|----------------------------------|----------------------------------------------------------|-------------------|
| Calendar view reads/day          | 500M * 3 views                                           | 1,500,000,000     |
| Calendar view RPS (normal)       | 1.5B / 86,400                                            | ~17,361 RPS       |
| Peak calendar RPS (Mon 9am, 3x)  | 17,361 * 3                                               | ~52,000 RPS       |
| Events created/day               | 500M * 1/7                                               | ~71,000,000       |
| Event write RPS (normal)         | 71M / 86,400                                             | ~822 RPS          |
| Invite notifications/day         | 71M * 2.5                                                | ~177,000,000      |
| Invite RPS                       | 177M / 86,400                                            | ~2,049 RPS        |
| Reminder deliveries/day          | 71M * 1.5                                                | ~106,000,000      |
| Peak reminder/second (minute-bdy)| 106M / 86,400 * 60 (burst at minute boundary)            | ~7,361 RPS        |
| Sync events (multi-device)       | 822 writes * avg 2.5 devices                             | ~2,055 device sync RPS |

### Latency Requirements

| Operation                        | Target p50 | Target p99 |
|----------------------------------|------------|------------|
| Calendar view (monthly)          | 30 ms      | 200 ms     |
| Event create / update            | 50 ms      | 300 ms     |
| Recurring event expansion        | 20 ms      | 100 ms     |
| Conflict detection               | 10 ms      | 50 ms      |
| Invite / RSVP                    | 50 ms      | 300 ms     |
| Free/busy query                  | 40 ms      | 200 ms     |
| Reminder delivery (vs scheduled) | ±5 s       | ±30 s      |
| Multi-device sync propagation    | 500 ms     | 3,000 ms   |
| Full-text search                 | 100 ms     | 500 ms     |

### Storage Estimates

**Assumptions:**
- Average event record (no recurring, no attendees): 500 bytes.
- Recurring event master record: 800 bytes (includes RRULE string).
- Event exception (EXDATE / modified instance): 600 bytes.
- Attendee record: 100 bytes.
- Calendar record: 200 bytes.
- Reminder record: 100 bytes.
- Users: 2B * 300 bytes profile = 600 GB.
- Active events per user (12 months rolling): avg 200 events.

| Data Type                    | Calculation                                                   | Size         |
|------------------------------|---------------------------------------------------------------|--------------|
| Event records                | 2B users * 200 events * 500 B                                 | 200 TB       |
| Attendee records             | 71M events/day * 365 * 2 years * 2.5 attendees * 100 B        | ~13 TB       |
| Calendar records             | 2B users * 3 calendars * 200 B                                | ~1.2 GB      |
| Reminder records             | 71M/day * 365 * 2 years * 1.5 reminders * 100 B               | ~7.8 TB      |
| iCal feed subscriptions      | 100M subscribers * 5 feeds * 200 B                            | ~100 GB      |
| Search index (Elasticsearch) | 200 TB events * 30% index overhead                            | ~60 TB       |
| Total storage                | ~280 TB (events + attendees + reminders + index)              | ~280 TB      |

### Bandwidth Estimates

| Traffic Type                | Calculation                                            | Bandwidth      |
|-----------------------------|--------------------------------------------------------|----------------|
| Calendar view responses     | 52,000 RPS peak * 10 KB (monthly view = ~100 events)   | ~520 MB/s      |
| Event write payloads        | 822 RPS * 2 KB                                         | ~1.6 MB/s      |
| Sync push to devices        | 2,055 device RPS * 1 KB delta                          | ~2 MB/s        |
| Total outbound (CDN-assisted)| ~600 MB/s peak                                        | ~600 MB/s      |

---

## 3. High-Level Architecture

```
                    ┌────────────────────────────────────────────────────────────┐
                    │                    CLIENT LAYER                             │
                    │    Web App  /  iOS  /  Android  /  Third-party CalDAV      │
                    └──────────────────────────┬─────────────────────────────────┘
                                               │ HTTPS / CalDAV / WebSocket
                    ┌──────────────────────────▼─────────────────────────────────┐
                    │                  API Gateway + Load Balancer                │
                    │     JWT auth, rate limiting, CalDAV protocol translation    │
                    └──┬───────────────┬───────────────┬───────────────┬─────────┘
                       │               │               │               │
        ┌──────────────▼──┐  ┌─────────▼────────┐  ┌──▼────────────┐  ┌▼──────────────┐
        │  Calendar &     │  │  Invite / RSVP   │  │  Sharing &    │  │  Reminder     │
        │  Event Service  │  │  Service         │  │  Permissions  │  │  Service      │
        │  (CRUD, recur,  │  │  (attendees,     │  │  Service      │  │  (scheduler,  │
        │   conflict det.)│  │   RSVP status)   │  │  (ACL, share) │  │  delivery)    │
        └──────────┬──────┘  └─────────┬────────┘  └──┬────────────┘  └──┬────────────┘
                   │                   │              │                   │
        ┌──────────▼──────┐  ┌─────────▼────────┐    │          ┌────────▼───────────┐
        │  Sync Service   │  │  Notification    │    │          │  Reminder Queue    │
        │  (delta sync,   │  │  Service         │    │          │  (Redis Sorted Set │
        │   CRDTs,        │  │  (email, push,   │    │          │  + Kafka)          │
        │   WebSocket)    │  │   SMS)           │    │          └────────────────────┘
        └──────────┬──────┘  └──────────────────┘    │
                   │                                  │
        ┌──────────▼──────────────────────────────────▼────────────────────────────┐
        │                     Data Layer                                             │
        │  ┌─────────────────────────┐   ┌──────────────────┐  ┌──────────────────┐│
        │  │  Cassandra Cluster       │   │  Redis Cluster   │  │  Elasticsearch   ││
        │  │  (events, attendees,    │   │  (free/busy      │  │  (event search   ││
        │  │   reminders, calendars) │   │  cache, session, │  │   index)         ││
        │  └─────────────────────────┘   │  conflict cache, │  └──────────────────┘│
        │  ┌─────────────────────────┐   │  sync cursors)   │                      │
        │  │  PostgreSQL             │   └──────────────────┘                      │
        │  │  (users, ACLs,          │                                              │
        │  │   calendar metadata)   │                                              │
        │  └─────────────────────────┘                                             │
        └─────────────────────────────────────────────────────────────────────────┘
                                              │
                              ┌───────────────▼──────────────┐
                              │  Kafka (event change stream,  │
                              │  invite events, reminder      │
                              │  trigger events, sync events) │
                              └──────────────────────────────┘
```

**Component Roles:**

- **API Gateway**: JWT/OAuth2 validation, rate limiting. Also translates CalDAV/iCal protocol requests to internal REST calls, enabling native iOS/macOS Calendar.app and Outlook integration.
- **Calendar & Event Service**: Core CRUD for calendars and events. Handles recurring event logic (RRULE parsing, expansion, exception management). Performs real-time conflict detection.
- **Invite / RSVP Service**: Manages event attendees, invite delivery, and RSVP state. Updates the organizer's view of attendee status.
- **Sharing & Permissions Service**: Manages ACL (Access Control List) for calendar sharing. Answers "can user X perform action Y on calendar Z?" Authorizes every cross-user data access.
- **Reminder Service**: Manages reminder schedules. Uses a Redis Sorted Set (score = reminder fire time as Unix timestamp) for efficient next-due-reminder queries. Delivers via email/push/SMS.
- **Sync Service**: Provides incremental sync for multi-device support. Maintains a per-user change log; clients send a `sync_token` to receive only changes since last sync. WebSocket for real-time push.
- **Cassandra Cluster**: Primary event store. Wide column storage is ideal for the calendar access pattern: `SELECT events WHERE user_id = X AND calendar_id = Y AND start_time >= A AND start_time < B`. High write throughput matches Cassandra's strengths.
- **PostgreSQL**: Stores user accounts, calendar metadata, and ACL rules. Transactional — ACL changes must be ACID.
- **Redis**: Free/busy cache (precomputed busy time blocks per user), conflict detection cache, and session tokens.
- **Elasticsearch**: Full-text search on event titles, descriptions, and attendee names.
- **Kafka**: Decouples event changes from downstream consumers (sync fanout, invite notifications, search index updates, reminder scheduler).

**Primary Data Flow (Create Event with Invites):**

1. User creates event with attendees → POST /events → Calendar & Event Service.
2. Service validates, checks conflicts (Redis free/busy cache), writes event to Cassandra.
3. Service publishes `event.created` to Kafka.
4. Kafka fanout: (a) Sync Service updates change log, pushes delta to user's other devices via WebSocket; (b) Invite Service creates attendee records, publishes invite notifications; (c) Reminder Scheduler adds reminder entries to Redis Sorted Set; (d) Elasticsearch sink updates search index; (e) Free/busy cache updater invalidates/updates cached busy blocks.
5. Invite notification → Notification Service → email/push to each invitee.
6. Invitee RSVPs → PATCH /events/{id}/attendees/{user_id} → RSVP status updated in Cassandra → organizer receives push notification of RSVP status.

---

## 4. Data Model

### Entities & Schema

```sql
-- PostgreSQL: Users, Calendars, ACLs (transactional metadata)

CREATE TABLE users (
    user_id         UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    email           VARCHAR(255) UNIQUE NOT NULL,
    display_name    VARCHAR(200) NOT NULL,
    timezone        VARCHAR(50) NOT NULL DEFAULT 'UTC',  -- IANA tz: 'America/New_York'
    locale          VARCHAR(10) NOT NULL DEFAULT 'en-US',
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE calendars (
    calendar_id     UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID NOT NULL REFERENCES users(user_id),
    name            VARCHAR(200) NOT NULL,
    description     TEXT,
    color           VARCHAR(7) NOT NULL DEFAULT '#4285F4',  -- hex color
    timezone        VARCHAR(50) NOT NULL,                   -- calendar default tz
    is_primary      BOOLEAN NOT NULL DEFAULT FALSE,
    is_shared       BOOLEAN NOT NULL DEFAULT FALSE,
    ical_uid        VARCHAR(255) UNIQUE,    -- for iCal sync
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    INDEX idx_calendars_user (user_id)
);

-- Calendar ACL (sharing permissions)
CREATE TABLE calendar_acl (
    acl_id          UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    calendar_id     UUID NOT NULL REFERENCES calendars(calendar_id) ON DELETE CASCADE,
    grantee_user_id UUID NOT NULL REFERENCES users(user_id),
    role            VARCHAR(20) NOT NULL,
                    -- FREE_BUSY_READER, READER, WRITER, OWNER
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (calendar_id, grantee_user_id),
    INDEX idx_acl_grantee (grantee_user_id)
);

-- iCal external feed subscriptions
CREATE TABLE ical_subscriptions (
    subscription_id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
    calendar_id     UUID NOT NULL REFERENCES calendars(calendar_id),
    feed_url        VARCHAR(2000) NOT NULL,
    last_synced_at  TIMESTAMPTZ,
    sync_interval_mins INT NOT NULL DEFAULT 60,
    etag            VARCHAR(255),    -- HTTP ETag for conditional GET
    last_modified   TIMESTAMPTZ,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
```

```
-- Cassandra: Events, Attendees, Reminders (high-throughput wide-column)

-- Primary event table: query by user + calendar + date range
CREATE TABLE events (
    user_id         UUID,
    calendar_id     UUID,
    start_time      TIMESTAMP,     -- stored in UTC
    event_id        UUID,
    title           TEXT,
    description     TEXT,
    location        TEXT,
    end_time        TIMESTAMP,     -- stored in UTC
    all_day         BOOLEAN,
    timezone        TEXT,          -- original event timezone (IANA)
    status          TEXT,          -- CONFIRMED, TENTATIVE, CANCELLED
    visibility      TEXT,          -- PUBLIC, PRIVATE (affects free/busy viewers)
    is_recurring    BOOLEAN,
    rrule           TEXT,          -- RFC 5545 RRULE string, e.g. 'FREQ=WEEKLY;BYDAY=MO'
    recurrence_id   UUID,          -- parent event_id for exceptions
    this_and_future BOOLEAN,       -- TRUE if this exception applies to all future instances
    exdates         LIST<TIMESTAMP>,  -- excluded recurrence dates
    organizer_id    UUID,
    sequence        INT,           -- iCal SEQUENCE number for update ordering
    created_at      TIMESTAMP,
    updated_at      TIMESTAMP,
    PRIMARY KEY ((user_id, calendar_id), start_time, event_id)
) WITH CLUSTERING ORDER BY (start_time ASC, event_id ASC)
  AND default_time_to_live = 0;  -- no auto-expiry; managed explicitly

-- Event lookup by event_id (for direct access, update, delete)
CREATE TABLE events_by_id (
    event_id        UUID PRIMARY KEY,
    user_id         UUID,
    calendar_id     UUID,
    -- full denormalized copy of event data
    title           TEXT,
    description     TEXT,
    location        TEXT,
    start_time      TIMESTAMP,
    end_time        TIMESTAMP,
    all_day         BOOLEAN,
    timezone        TEXT,
    status          TEXT,
    is_recurring    BOOLEAN,
    rrule           TEXT,
    recurrence_id   UUID,
    exdates         LIST<TIMESTAMP>,
    organizer_id    UUID,
    sequence        INT,
    created_at      TIMESTAMP,
    updated_at      TIMESTAMP
);

-- Attendees for an event
CREATE TABLE event_attendees (
    event_id        UUID,
    attendee_id     UUID,     -- user_id of invitee
    email           TEXT,     -- email (for external invites without account)
    display_name    TEXT,
    rsvp_status     TEXT,     -- NEEDS_ACTION, ACCEPTED, DECLINED, TENTATIVE
    role            TEXT,     -- REQ_PARTICIPANT, OPT_PARTICIPANT, CHAIR
    responded_at    TIMESTAMP,
    PRIMARY KEY (event_id, attendee_id)
);

-- Reminders
CREATE TABLE event_reminders (
    user_id         UUID,
    fire_at         TIMESTAMP,   -- exact time to deliver reminder (UTC)
    event_id        UUID,
    method          TEXT,        -- EMAIL, PUSH, SMS
    minutes_before  INT,
    delivered       BOOLEAN,
    PRIMARY KEY ((user_id), fire_at, event_id, method)
) WITH CLUSTERING ORDER BY (fire_at ASC);

-- Sync change log (per-user incremental sync)
CREATE TABLE sync_changelog (
    user_id         UUID,
    change_id       TIMEUUID,    -- Cassandra TIMEUUID: time-ordered UUID
    event_id        UUID,
    operation       TEXT,        -- CREATED, UPDATED, DELETED
    calendar_id     UUID,
    PRIMARY KEY ((user_id), change_id)
) WITH CLUSTERING ORDER BY (change_id DESC)
  AND default_time_to_live = 2592000;  -- 30-day TTL on changelog entries
```

### Database Choice

**Options Considered:**

| Database     | Pros                                                                          | Cons                                                                     |
|--------------|-------------------------------------------------------------------------------|--------------------------------------------------------------------------|
| Cassandra    | Massive write throughput; excellent range queries by partition key; multi-DC  | No ACID transactions; limited secondary indexes; complex schema design   |
| PostgreSQL   | ACID, flexible queries, full-text search                                      | Single-node write ceiling; date-range queries need careful indexing      |
| DynamoDB     | Managed, elastic scale, millisecond reads                                     | Expensive for large datasets; range queries limited to sort key          |
| MongoDB      | Flexible schema; TTL indexes; 2dsphere geo index                              | Not ideal for columnar access patterns; join-heavy queries cost more     |
| HBase        | Strong consistency; excellent range scans                                     | Heavy operational overhead (ZooKeeper, HDFS); complex ops                |
| BigTable     | Google's internal choice; petabyte scale; excellent range scans               | Google Cloud only; expensive; operational complexity                     |

**Selected: Cassandra for events + PostgreSQL for metadata/ACLs**

Justification:
1. The dominant calendar query is: "give me all events for user X, calendar Y, between start_time A and end_time B." This maps perfectly to Cassandra's partition key `(user_id, calendar_id)` with a clustering key `start_time` — a range scan on a single partition, which is Cassandra's highest-performance operation.
2. At 200 TB of event data across 2B users, PostgreSQL would require complex sharding. Cassandra provides linear horizontal scalability with consistent low-latency writes (< 5 ms) at this scale.
3. Each user's events are isolated to their own Cassandra partition — writes for one user don't contend with another user's writes.
4. PostgreSQL is retained for user accounts and ACLs because these require ACID transactions (granting/revoking permissions must be atomic and consistent) and involve complex relational queries (e.g., "find all calendars user X has access to"). The ACL dataset is small (~100 GB) and fits comfortably in PostgreSQL.
5. The `sync_changelog` table uses Cassandra's native TIMEUUID as a clustering key — this provides time-ordered, de-duplicated change tracking without a separate sequence number generator.

---

## 5. API Design

```
BASE URL: https://api.calendar.example.com/v1
```

### Calendars

```
GET /calendars
  Auth: Required
  Response 200: { "items": [{ "calendar_id", "name", "color", "timezone", "is_primary", "role" }] }
  Note: returns owned calendars + shared calendars the user has access to

POST /calendars
  Auth: Required
  Body: { "name", "description", "color", "timezone" }
  Response 201: { "calendar_id", "name", "color", "timezone", "is_primary": false }

PATCH /calendars/{calendar_id}
  Auth: Required (owner or editor)
  Body: any subset of { "name", "description", "color", "timezone" }
  Response 200: { ...updated calendar }

DELETE /calendars/{calendar_id}
  Auth: Required (owner only)
  Response 204
  Note: deletes all events in the calendar; non-primary only

GET /calendars/{calendar_id}/acl
  Auth: Required (owner only)
  Response 200: { "items": [{ "acl_id", "user_email", "role" }] }

POST /calendars/{calendar_id}/acl
  Auth: Required (owner only)
  Body: { "user_email": "bob@example.com", "role": "READER" }
  Response 201: { "acl_id", "user_email", "role" }

PATCH /calendars/{calendar_id}/acl/{acl_id}
  Auth: Required (owner only)
  Body: { "role": "WRITER" }
  Response 200: { "acl_id", "role" }

DELETE /calendars/{calendar_id}/acl/{acl_id}
  Auth: Required (owner only)
  Response 204
```

### Events

```
GET /calendars/{calendar_id}/events
  Auth: Required (must have at least FREE_BUSY_READER role)
  Rate limit: 1,000 req/min/user
  Query:
    time_min*: ISO8601 (inclusive)
    time_max*: ISO8601 (exclusive)
    timezone: IANA timezone for response timestamps (default: user's timezone)
    max_results: int (default 250, max 2500)
    single_events: bool (default true — expand recurring events into instances)
    sync_token: string (for incremental sync; mutually exclusive with time_min/time_max)
    page_token: string (pagination cursor)
  Response 200:
  {
    "items": [{
      "event_id", "calendar_id", "title", "description", "location",
      "start": { "dateTime": "ISO8601", "timeZone": "America/New_York" },
      "end":   { "dateTime": "ISO8601", "timeZone": "America/New_York" },
      "all_day": false,
      "status": "CONFIRMED",
      "recurrence": ["RRULE:FREQ=WEEKLY;BYDAY=MO"],  -- present only on master events
      "recurring_event_id": "uuid",  -- present only on instances
      "organizer": { "email", "display_name" },
      "attendees": [{ "email", "display_name", "rsvp_status" }],
      "reminders": [{ "method": "PUSH", "minutes_before": 15 }],
      "sequence": 0,
      "created_at", "updated_at"
    }],
    "next_page_token": "...",
    "next_sync_token": "...",   -- present on final page; use for next incremental sync
    "time_zone": "America/New_York"
  }

POST /calendars/{calendar_id}/events
  Auth: Required (must have WRITER or OWNER role)
  Rate limit: 100 req/min/user
  Body:
  {
    "title": "Team Standup",
    "description": "Daily sync",
    "location": "Zoom link",
    "start": { "dateTime": "2026-06-01T09:00:00", "timeZone": "America/New_York" },
    "end":   { "dateTime": "2026-06-01T09:30:00", "timeZone": "America/New_York" },
    "recurrence": ["RRULE:FREQ=DAILY;BYDAY=MO,TU,WE,TH,FR;COUNT=52"],
    "attendees": [{ "email": "alice@example.com" }, { "email": "bob@example.com" }],
    "reminders": [{ "method": "PUSH", "minutes_before": 10 }, { "method": "EMAIL", "minutes_before": 60 }]
  }
  Response 201: { ...full event object }

PATCH /calendars/{calendar_id}/events/{event_id}
  Auth: Required (organizer or calendar WRITER/OWNER)
  Query: scope: THIS_ONLY | THIS_AND_FUTURE | ALL (default: THIS_ONLY for recurring events)
  Body: any subset of event fields
  Response 200: { ...updated event }
  Note: For recurring events, scope determines which instances are affected.
        THIS_AND_FUTURE creates a new master event for the series.

DELETE /calendars/{calendar_id}/events/{event_id}
  Auth: Required (organizer or calendar WRITER/OWNER)
  Query: scope: THIS_ONLY | THIS_AND_FUTURE | ALL
  Response 204

-- RSVP to an event invite
PATCH /calendars/{calendar_id}/events/{event_id}/attendees/me
  Auth: Required
  Body: { "rsvp_status": "ACCEPTED" }  -- ACCEPTED | DECLINED | TENTATIVE
  Response 200: { "event_id", "rsvp_status" }
```

### Free/Busy

```
POST /freebusy
  Auth: Required
  Rate limit: 100 req/min/user
  Body:
  {
    "time_min": "2026-06-01T00:00:00Z",
    "time_max": "2026-06-08T00:00:00Z",
    "timezone": "America/New_York",
    "items": [{ "id": "user_id_or_email" }]
  }
  Response 200:
  {
    "time_min", "time_max",
    "calendars": {
      "alice@example.com": {
        "busy": [
          { "start": "2026-06-01T09:00:00-04:00", "end": "2026-06-01T09:30:00-04:00" },
          ...
        ],
        "errors": []
      }
    }
  }
  Note: only returns free/busy, not event titles, for users who granted FREE_BUSY_READER role.
        Returns full busy details for users who granted READER or higher.
```

### Sync

```
GET /calendars/{calendar_id}/events?sync_token={token}
  (same endpoint as GET /events, but with sync_token instead of time_min/time_max)
  Response 200: incremental changes since sync_token was issued
  Response 410 GONE: sync_token expired (> 30 days); client must do full sync
```

---

## 6. Deep Dive: Core Components

### 6.1 Recurring Events (RFC 5545)

**Problem it solves:**
Recurring events (e.g., "every weekday at 9am for 1 year") have potentially hundreds of instances. Storing each instance as a separate row would consume 52x the storage and make bulk modifications (changing the time of "all future" meetings) extremely expensive. The system must efficiently store, query, expand, and modify recurring events while supporting complex RRULE patterns and per-instance exceptions.

**Approaches Comparison:**

| Approach                           | Mechanism                                               | Pros                                      | Cons                                                            |
|------------------------------------|---------------------------------------------------------|-------------------------------------------|-----------------------------------------------------------------|
| Store all instances                | One row per recurrence instance                         | Simple query; no expansion logic          | Massive storage (52x); bulk edits touch thousands of rows      |
| Store only master + RRULE          | One row; expand on read                                 | Minimal storage; simple write             | Expensive to query across many RRULE patterns simultaneously   |
| Master + exception rows            | Master row with RRULE; additional rows for exceptions   | Minimal storage; handles edits cleanly    | Expansion logic required at read time; complex                 |
| Pre-expanded with lazy materialization | Expand on demand; cache expanded instances         | Fast reads after warm; controls storage  | Cache invalidation on master update; hot events may consume memory |

**Selected: Master record + RRULE + exception rows (expand on read)**

This is the standard iCalendar (RFC 5545) approach, used by Google Calendar, Apple Calendar, and Outlook.

**Data model:**
- **Master event**: The template. Contains RRULE string and is identified by `is_recurring = true` and no `recurrence_id`.
- **Exception event**: An individual instance that has been modified or cancelled. Has a `recurrence_id` pointing to the master, and a `start_time` matching the original occurrence's start time (the "recurrence date" per RFC 5545).
- **EXDATE**: Dates excluded entirely from the series (stored as a list on the master record).

**RFC 5545 RRULE examples:**
```
FREQ=WEEKLY;BYDAY=MO,WE,FR;COUNT=10         → 10 occurrences, Mon/Wed/Fri
FREQ=MONTHLY;BYDAY=2MO;UNTIL=20261231T000000Z → 2nd Monday of each month until year end
FREQ=YEARLY;BYMONTH=1;BYMONTHDAY=1          → Every Jan 1st (New Year's)
FREQ=DAILY;INTERVAL=2;COUNT=5               → Every 2 days, 5 times
```

**Expansion algorithm:**
```python
from dateutil.rrule import rrulestr
from datetime import datetime, timezone

def expand_recurring_event(master: Event, time_min: datetime, time_max: datetime,
                           exceptions: list[Event]) -> list[Event]:
    """
    Expand a recurring event's master record into individual instances
    within the requested time window.
    """
    # Parse the RRULE using dateutil (RFC 5545 compliant library)
    rule = rrulestr(
        master.rrule,
        dtstart=master.start_time,
        ignoretz=False
    )

    # Get all occurrence start times in the requested window
    occurrences = rule.between(
        time_min - (master.end_time - master.start_time),  # expand slightly left for events that started before time_min
        time_max,
        inc=True
    )

    # Build a map of exception events keyed by their original occurrence time
    exception_map: dict[datetime, Event] = {}
    cancelled_dates: set[datetime] = set()
    for exc in exceptions:
        if exc.status == 'CANCELLED':
            cancelled_dates.add(exc.start_time)  # recurrence date = original occurrence time
        else:
            exception_map[exc.start_time] = exc  # recurrence date → modified instance

    # Also exclude EXDATE entries from master
    excluded_dates = set(master.exdates or [])

    result = []
    duration = master.end_time - master.start_time

    for occ_start in occurrences:
        # Skip explicitly excluded dates
        if occ_start in excluded_dates or occ_start in cancelled_dates:
            continue

        # Check if this occurrence has a user-modified exception
        if occ_start in exception_map:
            instance = exception_map[occ_start]
        else:
            # Synthesize instance from master template
            instance = Event(
                event_id=generate_instance_id(master.event_id, occ_start),
                title=master.title,
                description=master.description,
                location=master.location,
                start_time=occ_start,
                end_time=occ_start + duration,
                timezone=master.timezone,
                organizer_id=master.organizer_id,
                recurring_event_id=master.event_id,
                is_recurring=False,  # instances are not themselves recurring
                calendar_id=master.calendar_id
            )

        # Only include if the instance falls within the requested window
        if instance.start_time < time_max and instance.end_time > time_min:
            result.append(instance)

    return result

def get_events_for_calendar(user_id: str, calendar_id: str,
                            time_min: datetime, time_max: datetime) -> list[Event]:
    # Fetch non-recurring events in range (direct Cassandra query)
    regular_events = cassandra.execute(
        """SELECT * FROM events
           WHERE user_id = %s AND calendar_id = %s
           AND start_time >= %s AND start_time < %s
           AND is_recurring = false""",
        [user_id, calendar_id, time_min, time_max]
    )

    # Fetch recurring master events whose series could intersect the range
    # (masters with start_time <= time_max; filtering by RRULE end would require
    # parsing, so we over-fetch slightly and filter in code)
    master_events = cassandra.execute(
        """SELECT * FROM events
           WHERE user_id = %s AND calendar_id = %s
           AND start_time <= %s AND is_recurring = true""",
        [user_id, calendar_id, time_max]
    )

    # Fetch exception events (instances with recurrence_id set)
    exception_events = cassandra.execute(
        """SELECT * FROM events
           WHERE user_id = %s AND calendar_id = %s
           AND start_time >= %s AND start_time < %s
           AND recurrence_id IS NOT NULL""",
        [user_id, calendar_id, time_min, time_max]
    )
    exceptions_by_master = defaultdict(list)
    for exc in exception_events:
        exceptions_by_master[exc.recurrence_id].append(exc)

    # Expand each master
    expanded = []
    for master in master_events:
        instances = expand_recurring_event(
            master, time_min, time_max,
            exceptions_by_master.get(master.event_id, [])
        )
        expanded.extend(instances)

    return regular_events + expanded
```

**Handling "Edit This and Future":**
```python
def edit_this_and_future(original_master_id: str, split_date: datetime,
                         new_event_data: dict, user_id: str, calendar_id: str):
    original = cassandra.get_event_by_id(original_master_id)

    # Truncate the original series: add UNTIL to its RRULE
    new_rrule = append_until_to_rrule(original.rrule, split_date - timedelta(seconds=1))
    cassandra.execute(
        "UPDATE events SET rrule = %s WHERE user_id = %s AND calendar_id = %s AND event_id = %s",
        [new_rrule, user_id, calendar_id, original_master_id]
    )

    # Create a new master event for the split-forward series
    new_master_id = uuid4()
    new_rrule_forward = modify_rrule_dtstart(original.rrule, split_date)
    new_event = Event(
        event_id=new_master_id,
        recurrence_id=None,  # new independent master
        start_time=split_date,
        rrule=new_rrule_forward,
        **merge(original, new_event_data)
    )
    cassandra.insert_event(new_event)

    # Re-assign exceptions that belong to the new series
    for exc in get_future_exceptions(original_master_id, split_date):
        update_exception_parent(exc.event_id, new_master_id)
```

**Interviewer Q&A:**

Q1: How do you store an event that recurs "every 2nd Tuesday of the month, for the next 5 years"? How many rows does that create?
A: Exactly 1 row — the master event with RRULE=`FREQ=MONTHLY;BYDAY=2TU`. The 5-year limit is encoded as `UNTIL=20310101T000000Z` or `COUNT=60` in the RRULE string. When a user views their calendar for any month in that 5-year range, the expand_recurring_event function computes which Tuesdays fall in that month and returns them as synthesized instances. No additional rows.

Q2: A user modifies one instance of a weekly recurring meeting to be an hour earlier. How is this stored?
A: An exception event row is inserted into Cassandra with `recurrence_id = master_event_id` and `start_time` set to the ORIGINAL scheduled time of that occurrence (not the new time). The new start_time for display is stored in the exception's modified fields. The master's EXDATE list is updated to exclude the original occurrence date. When expanding, the expansion loop sees the original date in EXDATE, skips it, then finds the exception in `exception_map` and returns the modified version.

Q3: What is the performance impact of expanding a 5-year weekly event for a user scrolling through a full year view?
A: The rrulestr expansion with dateutil runs in memory in O(N) time where N = number of occurrences in the range. For a 5-year weekly event queried for a 1-year range = ~52 occurrences. Expansion takes < 1 ms in Python. The bottleneck is the Cassandra query to fetch the master events, not the expansion logic. For users with many recurring events (outliers with 500+ series), we apply a limit and paginate.

Q4: How do you handle timezone changes for a recurring event (e.g., user moves from NY to LA)?
A: The event's RRULE is defined relative to the event's timezone (stored in `master.timezone`). If the event is "9am EST every Monday" and the user moves to LA, the event is stored with `timezone=America/New_York`. Expansion still generates 9am EST occurrences. The client converts those times to PT for display. The event's semantic meaning ("9am New York time") is preserved. If the user wants to change the event to always be 9am in their local timezone, they must edit the event and update the timezone to `America/Los_Angeles`.

Q5: How do you handle events that span DST transitions for recurring events?
A: The start time is stored in the event's specific IANA timezone (e.g., `America/New_York`), not as a fixed UTC offset. The expansion library (dateutil with `tz` support) correctly handles DST transitions: a weekly meeting at "9am America/New_York" will be stored as 14:00 UTC in winter and 13:00 UTC in summer. When expanding, the library computes the correct UTC time for each occurrence based on the local timezone rules for that specific date.

---

### 6.2 Conflict Detection

**Problem it solves:**
When a user creates or edits an event, the system should warn them if the new event overlaps with an existing event on their calendar. Conflict detection must be fast (< 50 ms), handle recurring events, and respect permission boundaries (only check calendars the user controls or has accepted invites to).

**Approaches Comparison:**

| Approach                          | Mechanism                                                  | Pros                               | Cons                                                          |
|-----------------------------------|------------------------------------------------------------|------------------------------------|---------------------------------------------------------------|
| DB range query on write           | Query all events overlapping [start, end] at creation time | Always accurate                    | Expensive for users with many recurring events                |
| Redis free/busy bitmap            | Per-user, per-day bitmaps at 15-min granularity            | O(1) check; very fast              | Only 15-min precision; RAM cost 2B users * 365 * 96 bits      |
| Redis sorted set (interval store) | Intervals stored as (start, end) pairs; range check        | Accurate; fast in Redis            | Sorted sets don't natively support overlap queries            |
| In-memory interval tree           | Process-local interval tree per user                       | O(log N) overlap detection         | Not distributed; invalidation on event changes needed         |
| Segment-based Redis cache         | Pre-compute busy blocks (contiguous busy segments)         | Fast to query; compact             | Segment merging complex; must be updated on every event change |

**Selected: Redis free/busy cache + on-demand DB verification**

For speed (< 50 ms p99), maintain a Redis key per user per date: `freebusy:{user_id}:{date}` → a sorted set of intervals `(start_unix, end_unix)`. This is updated on every event create/update/delete.

**Conflict detection pseudocode:**
```python
def check_conflict(user_id: str, new_start: datetime, new_end: datetime,
                   calendar_ids: list[str]) -> list[ConflictingEvent]:
    """
    Returns list of events that conflict with [new_start, new_end].
    new_start and new_end are in UTC.
    """
    conflicting = []

    # Determine all dates the new event spans
    dates = get_dates_for_range(new_start, new_end)  # e.g. ["2026-06-01", "2026-06-02"]

    for date in dates:
        key = f"freebusy:{user_id}:{date}"
        # Redis sorted set stores intervals as: score=start_unix, member="start_unix:end_unix:event_id"
        # To find overlapping intervals: an interval [a,b] overlaps [s,e] if a < e AND b > s
        # Equivalent query: find members with start < new_end AND end > new_start
        candidates = redis.zrangebyscore(key, 0, new_end.timestamp())
        for candidate in candidates:
            c_start, c_end, event_id = parse_freebusy_entry(candidate)
            if c_end > new_start.timestamp():  # overlap confirmed
                conflicting.append(ConflictingEvent(event_id=event_id,
                                                    start=c_start, end=c_end))

    return conflicting

def update_freebusy_cache(user_id: str, event: Event, operation: str):
    """Called on every event create/update/delete."""
    dates = get_dates_for_range(event.start_time, event.end_time)
    for date in dates:
        key = f"freebusy:{user_id}:{date}"
        entry = f"{event.start_time.timestamp()}:{event.end_time.timestamp()}:{event.event_id}"
        if operation in ('CREATED', 'UPDATED'):
            redis.zadd(key, {entry: event.start_time.timestamp()})
            redis.expire(key, 90 * 86400)  # 90-day TTL
        elif operation == 'DELETED':
            # Remove any entries for this event_id from this date's sorted set
            # Use a Lua script for atomicity
            redis.eval(DELETE_BY_EVENT_ID_LUA, 1, key, event.event_id)
```

For recurring events, the free/busy cache is populated with expanded instances up to 90 days forward. A background job maintains this expansion for the next 90 days, running nightly.

**Interviewer Q&A:**

Q1: The Redis free/busy cache for 2B users × 365 days = 730B keys. How do you handle memory?
A: We do NOT pre-create keys for all users × all days. Keys are created lazily when an event is created for that (user, date), and expire after 90 days. In practice, only active users with events in the near future have warm keys. A user who creates 200 events/year has ~200-400 date keys at any time. At 500 bytes/key average, 500M active users * 200 keys * 500 bytes = ~50 TB of Redis. This requires a large Redis cluster (500+ nodes at 100 GB/node), but is manageable and cost-effective vs querying Cassandra for every conflict check.

Q2: How do you handle conflict detection for events invited to the user by others (not on their own calendar)?
A: When a user accepts an invite (RSVP = ACCEPTED), the Invite Service publishes an `invite_accepted` event to Kafka. The free/busy cache updater consumes this and adds the event to the user's free/busy cache. Thus, accepted invites are treated identically to owned events for conflict detection purposes. Tentative (MAYBE) invites are also added to the cache but tagged separately so the UI can distinguish "hard conflict" vs "tentative conflict."

Q3: How would conflict detection work for multi-day all-day events?
A: All-day events are stored as date-only (not datetime). When checking conflicts, all-day events are treated as spanning 00:00:00 to 23:59:59 of each covered day. The free/busy cache uses the same (user, date) key structure, so a 3-day all-day event creates 3 entries in the cache, one per day. The conflict check queries all dates in the new event's range.

---

### 6.3 Reminder Delivery

**Problem it solves:**
Users configure per-event reminders (e.g., "notify me 15 minutes before"). With 106 million reminders/day, the reminder system must: (1) store reminder schedules durably, (2) fire reminders within ±30 seconds of the scheduled time at scale, (3) survive crashes without missing reminders, and (4) handle reminder updates/cancellations (e.g., user reschedules the event).

**Approaches Comparison:**

| Approach                           | Mechanism                                          | Pros                                | Cons                                                     |
|------------------------------------|----------------------------------------------------|-------------------------------------|----------------------------------------------------------|
| Cron job polling DB                | Every minute: SELECT reminders due                 | Simple                              | 106M reminders/day requires full table scan every minute |
| Redis Sorted Set (zrangebyscore)   | Score = fire_at timestamp; poll ZRANGEBYSCORE       | O(log N) per poll; sub-minute       | In-memory only; loss on Redis restart                   |
| Kafka with delayed messages        | Kafka Streams / ksqlDB time-window queries          | Durable; scalable                   | Kafka doesn't natively support delayed delivery          |
| Kafka + Redis two-tier             | Redis Sorted Set for scheduling; Kafka for delivery | Durability + speed                  | Two systems to maintain                                  |
| AWS EventBridge / Google Cloud Scheduler | Managed scheduling                         | No infrastructure to build          | Per-event cost at scale; limited rate                    |

**Selected: Redis Sorted Set for scheduling + Kafka for durable delivery + Cassandra for persistence**

Architecture:
1. On event creation/update, reminder records are written to Cassandra (durable) and added to Redis Sorted Set `reminders:pending` with score = fire_at Unix timestamp.
2. Reminder Poller service runs every 5 seconds: `ZRANGEBYSCORE reminders:pending 0 {now+5s}` to fetch due reminders. Uses `ZREM` after fetching (atomic pop with Lua script).
3. Fetched reminder records are published to Kafka topic `reminders.due`.
4. Reminder Delivery workers consume from Kafka and deliver via email/push/SMS.
5. On delivery success: mark `delivered = true` in Cassandra.
6. On Redis restart/failure: a recovery job reads undelivered reminders from Cassandra for the next 10 minutes and repopulates Redis Sorted Set.

```python
# Reminder Poller (runs every 5 seconds)
def poll_and_dispatch_reminders():
    now_ts = time.time()
    window_end = now_ts + 5  # look 5 seconds ahead

    # Atomic fetch-and-remove using Lua script
    # Lua ensures atomicity: no reminder is processed twice
    due_entries = redis.eval(FETCH_AND_REMOVE_DUE_LUA, 1,
                             "reminders:pending", 0, window_end)

    for entry in due_entries:
        user_id, event_id, method, fire_at = parse_reminder_entry(entry)
        # Publish to Kafka for durable delivery
        kafka.produce("reminders.due", key=user_id, value={
            "user_id": user_id,
            "event_id": event_id,
            "method": method,
            "fire_at": fire_at
        })

FETCH_AND_REMOVE_DUE_LUA = """
local due = redis.call('ZRANGEBYSCORE', KEYS[1], ARGV[1], ARGV[2])
if #due > 0 then
    redis.call('ZREM', KEYS[1], unpack(due))
end
return due
"""

# Schedule reminder on event creation
def schedule_reminders(event: Event, reminders: list[ReminderConfig]):
    for reminder in reminders:
        fire_at = event.start_time - timedelta(minutes=reminder.minutes_before)
        fire_ts = fire_at.timestamp()
        entry = f"{event.user_id}:{event.event_id}:{reminder.method}:{fire_ts}"

        # Write to Cassandra for durability
        cassandra.execute(
            "INSERT INTO event_reminders (user_id, fire_at, event_id, method, minutes_before) VALUES (%s,%s,%s,%s,%s)",
            [event.user_id, fire_at, event.event_id, reminder.method, reminder.minutes_before]
        )

        # Add to Redis Sorted Set (score = fire timestamp)
        redis.zadd("reminders:pending", {entry: fire_ts})

# Cancel reminders when event is deleted/updated
def cancel_reminders(user_id: str, event_id: str):
    # Remove from Redis (pattern match not efficient; use a secondary index)
    # Maintain a secondary set: reminder_keys:{user_id}:{event_id} → list of Sorted Set entries
    entries = redis.smembers(f"reminder_keys:{user_id}:{event_id}")
    if entries:
        redis.zrem("reminders:pending", *entries)
        redis.delete(f"reminder_keys:{user_id}:{event_id}")

    # Mark as cancelled in Cassandra
    cassandra.execute(
        "DELETE FROM event_reminders WHERE user_id = %s AND event_id = %s",
        [user_id, event_id]
    )
```

**Interviewer Q&A:**

Q1: With 106M reminders/day, how many reminders fire per second at peak? Can Redis handle it?
A: 106M / 86,400 seconds ≈ 1,227 reminders/second average. At peak (e.g., 9am Monday when many meetings start), bursts reach ~10,000/second. Redis ZADD/ZRANGEBYSCORE handles ~100,000 operations/second on a single node. With a Redis cluster (N shards), we partition the `reminders:pending` set by `{user_id modulo N}` → N sorted sets, each handled by one shard. At 10 shards: each shard handles 1,000 reminders/second — well within capacity.

Q2: What happens if the Reminder Poller pod crashes? Are reminders lost?
A: No. Poller crash scenarios: (1) Pod crashes before fetching: Redis still has the entries; next poller instance (Kubernetes restarts within 30s) picks them up. (2) Pod crashes after ZREM but before Kafka publish: the entries are gone from Redis but NOT marked as delivered in Cassandra. The recovery job (runs every 10 minutes) reads unddelivered reminders from Cassandra with `fire_at BETWEEN now()-10m AND now()+10m AND delivered = false` and republishes to Kafka. (3) Pod crashes after Kafka publish: the Kafka consumer handles delivery idempotently.

Q3: How do you handle reminder delivery for recurring events without scheduling 52 individual reminders upfront?
A: We maintain a rolling 14-day scheduling horizon. A background job runs every hour and looks at recurring events with instances in the next 14 days. It calls `schedule_reminders` for each instance within the horizon that doesn't already have a scheduled reminder in Redis. This horizon approach avoids creating thousands of reminder entries for a 5-year recurring event upfront, while still ensuring reminders are scheduled with enough lead time.

Q4: How do you handle a user who changes an event's time 5 minutes before the reminder fires?
A: The old reminders are cancelled (ZREM from Redis + Cassandra soft-delete) and new reminders are scheduled for the updated time. If the new event time is < 15 minutes away and the reminder was set for 15 minutes before, the reminder fires immediately (fire_at = now) rather than being skipped. This is handled by `max(fire_at, now + 10 seconds)` in the schedule_reminders function.

Q5: How do you guarantee exactly-once reminder delivery vs at-least-once?
A: The system provides at-least-once delivery — this is the pragmatic choice for reminders. Exactly-once would require distributed consensus which adds latency. The `delivered` flag in Cassandra prevents the delivery worker from re-delivering an already-confirmed reminder if Kafka redelivers the message. The delivery worker checks `delivered` before sending: if already true, it acknowledges and skips. This achieves idempotent delivery (effectively exactly-once from the user's perspective) without the overhead of two-phase commit.

---

## 7. Scaling

### Horizontal Scaling

- **Calendar & Event Service**: Stateless; scales to 500+ pods. Partitioned by `user_id` modulo at the API Gateway level to improve cache locality (same user's requests routed to same pod shard, sharing L1 process-local cache).
- **Sync Service**: WebSocket connections maintained per device. Supports 500M DAU with 1.2 devices each = 600M concurrent WebSocket connections. Implemented with an event-loop architecture (Node.js/Go), 10,000 connections/pod, 60,000 pods needed at peak.
- **Reminder Poller**: Single-leader election (via Redis SETNX with TTL) for each reminder shard to prevent duplicate polling.
- **Notification Service**: Scales based on Kafka lag; auto-scales from 10 to 500 consumer pods.

### Database Scaling

- **Cassandra**: 100-node cluster. Events are partitioned by `(user_id, calendar_id)` — each user's data is on 1-3 nodes (with replication factor 3). Linear scale: add nodes to handle 3x more data. Each Cassandra node handles ~2 TB of data, 100 nodes = 200 TB (matching our storage estimate with 1 replica; 3x replication = 600 TB raw storage).
- **PostgreSQL (ACLs/users)**: 1 primary + 3 read replicas. Total data ~100 GB — single instance easily handles this. Sharding only needed beyond 10M active shared calendars.
- **Elasticsearch**: 20-node cluster, 10 shards per region. Event titles and descriptions indexed; ~60 TB index size.

### Caching

| Cache                          | Key Pattern                                    | TTL        | Update Trigger            |
|--------------------------------|------------------------------------------------|------------|---------------------------|
| Free/busy blocks               | `freebusy:{user_id}:{date}`                    | 90 days    | Event create/update/delete |
| Calendar metadata              | `calendar:{calendar_id}`                       | 10 min     | Calendar update           |
| ACL lookup                     | `acl:{calendar_id}:{user_id}`                  | 5 min      | ACL change                |
| Expanded recurring instances   | `expanded:{event_id}:{month_key}`              | 1 hr       | Master event update       |
| User settings/timezone         | `user:{user_id}:prefs`                         | 30 min     | Profile update            |

### CDN

Static assets (web app JS/CSS, calendar UI framework): served from CloudFront with long cache headers (1-year cache with content-hashed filenames). Calendar data is not CDN-cached (too personalized), but API responses include `Cache-Control: private, max-age=30` for browser-side caching of recent reads.

**Interviewer Q&A:**

Q1: With 600M concurrent WebSocket connections, how do you route a sync push to the correct connection?
A: Each WebSocket connection is registered in a Redis hash: `ws_connections:{user_id}` → `{device_id: pod_address}`. When the Sync Service needs to push to a user, it looks up their pod addresses in Redis and sends an internal HTTP call to the specific pod managing that connection. This is the "connection registry" pattern used by Slack, Discord, and similar real-time systems.

Q2: How does Cassandra handle hot partitions? (e.g., user with 10,000 events in a single calendar)
A: A Cassandra partition is bounded to a single node. An extreme user with 10,000 events in one calendar = ~5 MB of partition data — trivially small. Cassandra handles millions of rows per partition comfortably. True hot partitions arise from very high write rates to a single key, not data size. Our partition key `(user_id, calendar_id)` means writes for different users never contend. The only concern would be a user who creates events at 1,000 writes/second, which is not a real calendar usage pattern.

Q3: How do you handle cross-datacenter replication for global users?
A: Cassandra's built-in multi-DC replication is used. Two DCs: us-east and eu-west. Keyspace configured with `NetworkTopologyStrategy` and replication factor 3 in each DC. Write consistency level `LOCAL_QUORUM` ensures writes are acknowledged by the local DC without waiting for the remote DC, providing low write latency. Read consistency level `LOCAL_QUORUM` serves reads from the nearest DC. A user's events are always readable from their nearest DC within < 10 ms. Cross-DC sync lag is < 1 second for normal operation.

Q4: How does the sync_token mechanism work for multi-device sync?
A: When a client completes an initial sync, it receives a `next_sync_token` (derived from the current TIMEUUID timestamp in the sync_changelog). On subsequent requests, the client sends this token. The server queries: `SELECT * FROM sync_changelog WHERE user_id = X AND change_id > {sync_token_timeuuid} LIMIT 500`. The client receives only the changes since its last sync. If the sync_token is older than 30 days (TTL on the changelog), the server returns 410 Gone and the client must perform a full sync.

Q5: How would you scale to 10x current traffic (5B DAU) without redesigning the core architecture?
A: (1) Expand Cassandra cluster from 100 to 1,000 nodes — linear scale, no schema changes. (2) Expand WebSocket tier proportionally. (3) Expand Redis cluster for free/busy cache (from 500 to 5,000 nodes or use a more memory-efficient data structure). (4) Add a third geographic region (ap-northeast for Asia). (5) Introduce read-only follower Cassandra clusters per region to offload cross-region read traffic. The core architecture remains unchanged; only fleet size scales.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Component                | Failure Mode                              | Impact                                        | Mitigation                                                                  |
|--------------------------|-------------------------------------------|-----------------------------------------------|-----------------------------------------------------------------------------|
| Cassandra node failure   | 1-2 nodes down                            | Reads/writes continue (quorum = 2/3 replicas) | RF=3; LOCAL_QUORUM still satisfied with 2 nodes                            |
| Cassandra DC failure     | Entire US-east DC down                   | Failover to EU-west DC with small latency bump | Multi-DC replication; Route 53 failover to EU-west API endpoint            |
| Redis failure            | Free/busy cache lost                     | Conflict detection falls to Cassandra query   | Cassandra fallback in conflict detection; cache rebuilt on demand           |
| Reminder Poller crash    | Poller unavailable for < 30 s            | Reminders delayed by < 30 s                   | Kubernetes auto-restart; recovery job for any delayed reminders             |
| Kafka broker failure     | 1 broker down                            | No message loss (RF=3)                        | Kafka replication; producer acks = all; ISR maintained                      |
| Sync WebSocket pod crash | Active connections dropped               | Clients reconnect within 3 s; catch up via sync_token | Client reconnect logic; sync_changelog stores all changes               |
| Elasticsearch failure    | Search unavailable                       | Search returns error; calendar views unaffected | Search is non-critical; graceful degradation                              |

### Idempotency

- Event creation: client provides a client-generated `event_id` (UUID v4). Server uses `INSERT IF NOT EXISTS` in Cassandra (LWT — Lightweight Transaction) to prevent duplicate creation on retry.
- Reminder delivery: `delivered` flag in Cassandra prevents double-sending.
- Invite processing: Cassandra `event_attendees` primary key `(event_id, attendee_id)` — duplicate invite inserts are no-ops (upsert semantics).

---

## 9. Monitoring & Observability

### Metrics

| Metric                                  | Type      | Alert Threshold                   |
|-----------------------------------------|-----------|-----------------------------------|
| `event.create.latency_p99`              | Histogram | > 300 ms                          |
| `calendar.view.latency_p99`             | Histogram | > 200 ms                          |
| `reminder.delivery.delay_seconds`       | Histogram | > 60 s                            |
| `reminder.missed.count`                 | Counter   | > 0 (fire_at passed, not delivered) |
| `recurring.expansion.latency_p99`       | Histogram | > 100 ms                          |
| `cassandra.read.latency_p99`            | Histogram | > 50 ms                           |
| `sync.device.lag_seconds`              | Gauge     | > 10 s                            |
| `freebusy.cache_hit_rate`               | Gauge     | < 80%                             |
| `websocket.active_connections`          | Gauge     | Informational (capacity planning) |
| `ical.feed.sync_failure_rate`           | Gauge     | > 5%                              |

### Distributed Tracing

OpenTelemetry traces propagated through all services and Kafka message headers. A calendar view trace spans: API Gateway → Calendar Service → Cassandra (list events) → RRULE expansion (inline span) → response serialization. A reminder delivery trace: Redis Sorted Set poll → Kafka publish → Delivery Worker → SendGrid/FCM call → Cassandra update.

### Logging

Structured JSON logs. Important fields: `trace_id`, `user_id`, `calendar_id`, `event_id`, `operation`, `duration_ms`. RRULE parse errors logged at WARN with the raw RRULE string for debugging. Audit log of all ACL changes (who shared what calendar with whom) retained in PostgreSQL + S3.

---

## 10. Trade-offs & Design Decisions Summary

| Decision                           | Chosen                                         | Alternative                         | Trade-off                                                                  |
|------------------------------------|------------------------------------------------|-------------------------------------|----------------------------------------------------------------------------|
| Event storage                      | Cassandra wide-column                          | PostgreSQL with partitioning        | Cassandra gives linear scale and ideal access pattern; loses ACID           |
| Recurring events                   | Expand on read (RFC 5545 master + exceptions)  | Pre-materialized instances          | Minimal storage; requires expansion logic; complex exception handling       |
| Conflict detection                 | Redis free/busy sorted sets                    | DB range query on each write        | Redis is 100x faster; requires cache maintenance                           |
| Reminder scheduling                | Redis Sorted Set + Kafka                       | DB polling job                      | Redis sorted set enables sub-second precision; Kafka ensures durability     |
| Sync mechanism                     | Delta sync via sync_token (changelog)           | Full sync on each device open       | Delta sync saves bandwidth for active users; requires 30-day changelog TTL |
| Multi-DC consistency               | LOCAL_QUORUM (eventual cross-DC)               | EACH_QUORUM (strong global)         | LOCAL_QUORUM gives lower write latency; accepts < 1s cross-DC sync lag     |
| Timezone storage                   | UTC in DB + IANA timezone name on event        | Local time in DB                    | UTC + IANA handles DST correctly; local time creates bugs at DST boundary  |

---

## 11. Follow-up Interview Questions

Q1: How do you handle the "find a time" feature where a scheduling assistant suggests available meeting times for a group of people?
A: Call `POST /freebusy` with all attendees. The response returns each user's busy blocks. The scheduling assistant computes the intersection of free time: for each 30-minute slot in the requested range, check if ALL attendees' free/busy calendars show the slot as free. The intersection algorithm runs in O(N * T) where N = attendees and T = time slots. Cache the result for the initiating user for 5 minutes.

Q2: How do you implement the iCalendar (.ics) export for GDPR data portability?
A: A GDPR export job reads all of a user's events from Cassandra (full scan of their partition), formats them as RFC 5545 iCal objects using a compliant serialization library, and streams the output to an S3 file. The user receives a download link. The export is generated asynchronously (can take minutes for users with years of events). A `POST /export` request creates an export job; `GET /export/{job_id}` polls status; the download URL is provided when ready.

Q3: How would you design the resource booking feature (meeting rooms)?
A: Resources (rooms, projectors) are modeled as calendar owners with a `resource_type` flag. Booking a room = creating an event on the room's calendar (requires WRITER role on that calendar). Conflict detection applies to the room's calendar — if the room is already booked for the time slot, the booking fails. The IT admin manages room calendars and grants users permission to book. Room availability is visible via the free/busy API.

Q4: How do you handle external iCal feed subscriptions (read-only)?
A: A subscription record stores the feed URL, last-synced ETag, and sync interval. A background job runs every `sync_interval_mins` (default 60): fetches the feed URL with `If-None-Match: {etag}` header. If HTTP 304, no changes. If 200, parse the ICS body, diff against previously imported events (by UID), and update Cassandra accordingly (upsert events with `X-CALENDAR-SOURCE: external`). External events are read-only — users cannot edit them via the UI.

Q5: How would you implement calendar search (finding an event by keyword)?
A: Elasticsearch index maintains event titles and descriptions (updated via Kafka → Elasticsearch sink connector, < 2 seconds behind writes). Search query: `GET /search?q=standup&calendar_ids[]=X&time_min=2026-01-01`. The Calendar Service queries Elasticsearch with a `bool` query filtering by `calendar_id` (from calendars the user can access) and full-text match on `title` and `description`. Results are returned as event_ids, then hydrated from Cassandra or the application's event cache. Access control is enforced: results only include events from calendars the user can read.

Q6: How do you handle meeting cancellation when the organizer deletes the event?
A: Event deletion publishes a `event.deleted` Kafka event. The Invite Service consumes this and sends cancellation notifications to all attendees. The event is not physically deleted from Cassandra immediately — it is marked `status = CANCELLED` with a `cancelled_at` timestamp. Attendees' calendar views filter out cancelled events (or show them with strikethrough styling). Physical deletion occurs after 30 days (Cassandra TTL). This approach ensures all attendees' Sync Service receives the deletion and can remove the event from their local device cache.

Q7: How do you prevent a malicious user from querying free/busy to infer meeting content (e.g., figuring out when their boss has meetings with HR)?
A: Free/busy access is controlled by ACL. By default, a calendar is not shared — external users cannot see ANY information. When a user shares their calendar, they explicitly choose `FREE_BUSY_READER` (shows only busy/free, no titles or details) vs `READER` (shows full event titles and descriptions). The `POST /freebusy` API only returns data for users who have granted at least `FREE_BUSY_READER` access. For users who haven't granted access, the API returns an error for that specific user (not "not found" which would leak user existence).

Q8: How would you design the CalDAV interface for compatibility with native calendar apps?
A: CalDAV is a standard protocol (RFC 4791) for calendar synchronization. The API Gateway has a CalDAV protocol translator that maps CalDAV verbs (REPORT, PROPFIND, PUT, DELETE) to internal REST API calls. Key mappings: `REPORT calendar-query` → `GET /calendars/{id}/events?time_min=...`, `PUT /calendars/{cal-id}/{event-uid}.ics` → `POST /events` or `PATCH /events/{id}`, `DELETE /calendars/{cal-id}/{event-uid}.ics` → `DELETE /events/{id}`. The translator handles iCal serialization/deserialization. This enables Apple Calendar, Outlook, and Thunderbird to connect natively without any client-side modification.

Q9: What is the consistency model when two people edit the same event simultaneously?
A: Last-write-wins (LWW) using the iCalendar `SEQUENCE` field. Each edit increments the `sequence` number. When the Cassandra write arrives, if the incoming `sequence` < stored `sequence`, the write is rejected (out-of-order update). If equal, it is a conflict (detected via Cassandra's LWT: `UPDATE IF sequence = expected`). On conflict, the client receives a 409 with the current event state and the user must re-apply their changes. This is the same mechanism used by Google Calendar and Outlook.

Q10: How do you handle time zones for all-day events that display differently in different time zones?
A: All-day events have no time component — only a date. They are stored as `DATE` type (YYYY-MM-DD), not TIMESTAMP. When displaying, the client shows the event on that calendar date regardless of the viewer's timezone. This is the correct behavior: "Board Meeting on June 1" should appear on June 1 for a user in Tokyo and a user in New York. The only exception is cross-timezone shared events where the organizer's timezone matters — in that case, the event is stored as a timed event in the organizer's timezone, not as an all-day event.

Q11: How would you implement reminder snooze functionality?
A: When a user snoozes a reminder (e.g., "snooze 10 minutes"), the mobile client calls `POST /events/{event_id}/reminders/snooze` with `{ "snooze_minutes": 10 }`. This creates a new one-time reminder in the event_reminders table with `fire_at = now() + 10 minutes` and adds it to the Redis Sorted Set. The original reminder is marked delivered. The snooze reminder fires 10 minutes later and is not renewable beyond the event's end time.

Q12: How do you handle event spam (a malicious user creating 1M events on their calendar)?
A: Per-user storage quotas enforced at the application level: maximum 25,000 events per calendar, maximum 25 calendars per user (= 625,000 total events, well above any legitimate use). Rate limiting on event creation: 100 events/minute per user. Storage quota check: count events in calendar before insert using a counter maintained in Redis (`INCR event_count:{calendar_id}`). If the counter exceeds the limit, return 429.

Q13: What happens to shared calendar events if the sharer revokes access?
A: When `DELETE /calendars/{cal_id}/acl/{acl_id}` is called: (1) ACL record is deleted from PostgreSQL. (2) Kafka publishes `calendar_access_revoked` event. (3) The grantee's Sync Service receives this and pushes a `calendar_deleted` delta to all their devices. (4) The grantee's client removes all events from that calendar from local storage. From that point forward, any API calls from the grantee referencing that calendar_id return 403. Previously downloaded event data (offline copies) may persist on device until the app next syncs.

Q14: How would you implement "smart suggestions" (invite suggestions based on frequent collaborators)?
A: Build a `collaboration_graph` service that consumes the Kafka `invite.sent` stream and maintains a weighted graph: `(organizer_id, attendee_id) → invite_count`. Stored in Redis as a sorted set per user: `collaborators:{user_id}` with score = invite_count. The event creation UI calls `GET /suggestions/attendees?q={partial_email}` which blends Elasticsearch results (email match) with the collaboration graph (most frequent collaborators) ranked by relevance. This is a soft recommendation — purely additive, no core system dependency.

Q15: How do you ensure that reminder delivery is compliant with user notification preferences and local laws (e.g., GDPR right to erasure)?
A: On deletion request (`DELETE /users/{user_id}`): (1) Cancel all pending reminders (ZREM from Redis, mark delivered in Cassandra). (2) Delete all event data from Cassandra (delete by partition key `user_id`). (3) Delete user profile from PostgreSQL. (4) Revoke all OAuth tokens. (5) Publish Kafka event to delete user data from Elasticsearch index. (6) Export a final data archive to user-specified location before deletion. All steps are tracked in a GDPR deletion job with idempotent steps for auditing. Kafka `user.deleted` event propagates to all downstream services.

---

## 12. References & Further Reading

1. RFC 5545 — Internet Calendaring and Scheduling Core Object Specification (iCalendar): https://tools.ietf.org/html/rfc5545
2. RFC 4791 — Calendaring Extensions to WebDAV (CalDAV): https://tools.ietf.org/html/rfc4791
3. Google Calendar API Documentation — Events resource: https://developers.google.com/calendar/api/v3/reference/events
4. dateutil Python library — rrule module (RFC 5545 RRULE implementation): https://dateutil.readthedocs.io/en/stable/rrule.html
5. Apache Cassandra Documentation — Data Modeling: https://cassandra.apache.org/doc/latest/cassandra/data_modeling/
6. Martin Fowler — "Event Sourcing": https://martinfowler.com/eaaDev/EventSourcing.html
7. Designing Data-Intensive Applications — Martin Kleppmann — Chapter 5: Replication
8. Google Engineering Blog — "Google Calendar Infrastructure": https://cloud.google.com/blog
9. Redis Documentation — Sorted Sets: https://redis.io/docs/data-types/sorted-sets/
10. IANA Time Zone Database: https://www.iana.org/time-zones
