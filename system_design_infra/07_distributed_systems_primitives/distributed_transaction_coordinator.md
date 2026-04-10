# System Design: Distributed Transaction Coordinator

> **Relevance to role:** Distributed transactions are unavoidable in cloud infrastructure platforms. Provisioning a bare-metal server requires atomic updates across inventory (MySQL), network configuration (Neutron/SDN), billing system, and IPMI controller -- if any step fails, all must be rolled back. Kubernetes admission controllers enforce multi-resource transactions. Job scheduling involves atomic reservation of compute + network + storage. OpenStack Nova coordinates VM creation across Nova, Neutron, Cinder, and Glance. Understanding 2PC, Saga, TCC, and the outbox pattern is essential for building correct multi-service workflows.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Atomic multi-service operations | All participating services commit or all abort |
| FR-2 | Saga orchestration | Long-running transactions with compensating actions |
| FR-3 | Saga choreography | Event-driven multi-service coordination (no central coordinator) |
| FR-4 | Two-Phase Commit (2PC) | Synchronous prepare-commit protocol for tightly coupled services |
| FR-5 | Compensating transactions | Undo semantics for each step in a saga |
| FR-6 | Idempotent operations | Every step can be safely retried without side effects |
| FR-7 | Transaction status tracking | Query the status of any in-flight or completed transaction |
| FR-8 | Timeout and dead-letter handling | Transactions that exceed timeout are aborted and compensated |
| FR-9 | Outbox pattern | Atomic local DB write + event publish |
| FR-10 | Transaction log | Persistent, queryable log of all transaction steps for debugging |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Transaction completion latency | < 5 seconds for typical 3-step saga |
| NFR-2 | Throughput | 10,000 transactions/sec |
| NFR-3 | Availability | 99.99% (coordinator is on the critical path) |
| NFR-4 | Consistency | Eventual consistency for sagas; strong consistency for 2PC |
| NFR-5 | Durability | Transaction log survives node failures |
| NFR-6 | Idempotency guarantee | Exactly-once effect per step (at-least-once delivery + idempotency) |
| NFR-7 | Compensation success rate | 99.99% (compensating actions must succeed) |

### Constraints & Assumptions
- Participants are Java/Python microservices communicating via gRPC and Kafka.
- Each service owns its own database (database-per-service pattern).
- No distributed database (no global transactions at the DB level).
- Eventual consistency is acceptable for most workflows (saga).
- 2PC is used only for tightly coupled, latency-insensitive operations.
- Message broker: Kafka (durable, ordered, exactly-once semantics available).

### Out of Scope
- Distributed SQL transactions (Spanner-style global serializability).
- XA transactions (JTA/XA resource manager protocol).
- Financial double-entry accounting (specialized domain).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Distributed transactions per second | 1,000 | Provisioning, scheduling, config changes |
| Steps per transaction (avg) | 4 | e.g., reserve + configure + bill + notify |
| Total step operations per second | 4,000 | 1,000 x 4 |
| Compensation rate | 5% | 50 compensations/sec (steps that need undo) |
| Peak multiplier | 3x | Batch deployments, fleet operations |
| Peak transactions/sec | 3,000 | 1,000 x 3 |
| In-flight transactions (avg) | 5,000 | 1,000 tps x 5s avg duration |
| Transaction log entries per day | 345.6M | 4,000 steps/sec x 86,400 |

### Latency Requirements

| Operation | p50 | p99 | Notes |
|-----------|-----|-----|-------|
| Saga (3-step, happy path) | 500 ms | 2 s | Sequential steps |
| Saga (3-step, with compensation) | 1 s | 5 s | Forward + compensation steps |
| 2PC (2-participant) | 50 ms | 200 ms | Synchronous prepare + commit |
| Transaction status query | 5 ms | 20 ms | Read from transaction log |
| Outbox event publish | 10 ms | 50 ms | Async from DB to Kafka |

### Storage Estimates

| Item | Size | Total |
|------|------|-------|
| Transaction record | 1 KB | |
| Step record (per step) | 512 B | |
| Per transaction (4 steps) | 3 KB | |
| Daily transactions | 86.4M | 259 GB/day |
| Transaction log (30 day retention) | | 7.8 TB |
| In-flight transaction state | 5,000 x 3 KB | 15 MB |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Coordinator -> participants (commands) | 4K ops/sec x 512 B | 2 MB/s |
| Participants -> coordinator (responses) | 4K ops/sec x 256 B | 1 MB/s |
| Kafka events (outbox) | 4K events/sec x 1 KB | 4 MB/s |
| Transaction log writes | 4K entries/sec x 512 B | 2 MB/s |

---

## 3. High-Level Architecture

```
  ┌──────────────────────────────────────────────────────────────┐
  │                     Client / API Layer                        │
  │  "Provision a bare-metal server for tenant X"                │
  └──────────────────────┬───────────────────────────────────────┘
                         │
  ┌──────────────────────▼───────────────────────────────────────┐
  │              Transaction Coordinator (Orchestrator)           │
  │                                                               │
  │  ┌────────────────────────────────────────────────────────┐  │
  │  │                  Saga Engine                            │  │
  │  │  ┌──────────┐  ┌──────────┐  ┌───────────────────┐    │  │
  │  │  │Saga      │  │Step      │  │Compensation       │    │  │
  │  │  │Definition│  │Executor  │  │Manager            │    │  │
  │  │  │Registry  │  │          │  │                   │    │  │
  │  │  └──────────┘  └──────────┘  └───────────────────┘    │  │
  │  └────────────────────────┬───────────────────────────────┘  │
  │                           │                                   │
  │  ┌────────────────────────▼───────────────────────────────┐  │
  │  │              Transaction Log (MySQL)                    │  │
  │  │  Saga state, step status, compensation records          │  │
  │  └────────────────────────────────────────────────────────┘  │
  │                                                               │
  │  ┌────────────────────────────────────────────────────────┐  │
  │  │              2PC Engine (for tight coupling)             │  │
  │  │  Prepare → Commit/Abort                                 │  │
  │  └────────────────────────────────────────────────────────┘  │
  └──────────────┬──────────────┬──────────────┬─────────────────┘
                 │              │              │
      gRPC/Kafka │   gRPC/Kafka │   gRPC/Kafka │
                 ▼              ▼              ▼
  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐
  │Inventory │  │ Network  │  │ Billing  │  │  IPMI    │
  │Service   │  │ Service  │  │ Service  │  │Controller│
  │(MySQL)   │  │(Neutron) │  │(MySQL)   │  │(bare-    │
  │          │  │          │  │          │  │ metal)   │
  │ Reserve  │  │ Config   │  │ Charge   │  │ Power On │
  │ server   │  │ VLAN     │  │ tenant   │  │ server   │
  │          │  │          │  │          │  │          │
  │ Compen-  │  │ Compen-  │  │ Compen-  │  │ Compen-  │
  │ sation:  │  │ sation:  │  │ sation:  │  │ sation:  │
  │ Unre-    │  │ Remove   │  │ Refund   │  │ Power    │
  │ serve    │  │ VLAN     │  │ charge   │  │ Off      │
  └──────────┘  └──────────┘  └──────────┘  └──────────┘
                         │
  ┌──────────────────────▼───────────────────────────────────────┐
  │                    Message Broker (Kafka)                      │
  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
  │  │ saga.commands │  │ saga.events  │  │ saga.dlq     │       │
  │  │ (per-service) │  │ (completion) │  │ (dead letter)│       │
  │  └──────────────┘  └──────────────┘  └──────────────┘       │
  └──────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Saga Engine** | Executes saga definitions step-by-step; handles compensation on failure |
| **Saga Definition Registry** | Stores saga templates (step sequence, compensation for each step) |
| **Step Executor** | Sends commands to participants and awaits responses |
| **Compensation Manager** | On failure, executes compensating transactions in reverse order |
| **Transaction Log** | Persistent record of every saga, step, and compensation for recovery and debugging |
| **2PC Engine** | For synchronous, tightly-coupled operations (rare, used when eventual consistency is not acceptable) |
| **Kafka** | Message broker for asynchronous step commands and completion events |
| **Participant Services** | Each service implements forward action + compensating action for each transaction type |

### Data Flows

**Saga (Happy Path -- Bare-Metal Provisioning):**

```
1. Client: POST /v1/provision {server_id, tenant_id, network_config}
2. Coordinator creates Saga instance in Transaction Log (status=STARTED)
3. Step 1: Inventory.ReserveServer(server_id, tenant_id)
   → Coordinator publishes command to Kafka topic "inventory.commands"
   → Inventory service processes, writes to MySQL, publishes completion
   → Coordinator records step 1 = COMPLETED
4. Step 2: Network.ConfigureVLAN(server_id, network_config)
   → Same Kafka flow
   → Coordinator records step 2 = COMPLETED
5. Step 3: Billing.CreateCharge(tenant_id, server_id, rate)
   → Same Kafka flow
   → Coordinator records step 3 = COMPLETED
6. Step 4: IPMI.PowerOn(server_id)
   → Same Kafka flow
   → Coordinator records step 4 = COMPLETED
7. Coordinator marks Saga = COMPLETED
8. Client notified (webhook or polling)
```

**Saga (Compensation Path -- Network Config Fails):**

```
1-3. Steps 1-2 succeed (Reserve + VLAN config)
4. Step 3: Network.ConfigureVLAN fails
   → Coordinator records step 3 = FAILED
   → Saga enters COMPENSATING state
5. Compensation Step 2: Network.RemoveVLAN (undo step 2)
   → But step 2 was actually the last successful step
   → This compensates the network config that was set up
   (Note: in our example, step 3 is Network and step 2 is
   Inventory, so let's fix the numbering)

Corrected with our actual steps:
1. Inventory.ReserveServer → COMPLETED
2. Network.ConfigureVLAN → FAILED (network error)
3. Saga enters COMPENSATING
4. Compensate Step 1: Inventory.UnreserveServer
   → Coordinator records compensation 1 = COMPLETED
5. Saga = COMPENSATED
6. Client notified of failure
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Transaction Log (MySQL)

CREATE TABLE saga_instance (
    saga_id         VARCHAR(64) PRIMARY KEY,
    saga_type       VARCHAR(128) NOT NULL,    -- e.g., "bare_metal_provision"
    status          ENUM('STARTED', 'RUNNING', 'COMPENSATING',
                         'COMPLETED', 'COMPENSATED', 'FAILED') NOT NULL,
    input_data      JSON NOT NULL,            -- request payload
    output_data     JSON,                     -- result (if completed)
    error_data      JSON,                     -- error (if failed)
    current_step    INT DEFAULT 0,            -- index of current step
    created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    completed_at    TIMESTAMP NULL,
    timeout_at      TIMESTAMP NOT NULL,       -- saga-level timeout
    idempotency_key VARCHAR(128) UNIQUE,      -- prevent duplicate sagas
    INDEX idx_status (status),
    INDEX idx_timeout (timeout_at),
    INDEX idx_type_created (saga_type, created_at)
);

CREATE TABLE saga_step (
    step_id         VARCHAR(64) PRIMARY KEY,
    saga_id         VARCHAR(64) NOT NULL,
    step_index      INT NOT NULL,             -- order in saga
    step_name       VARCHAR(128) NOT NULL,    -- e.g., "reserve_server"
    participant     VARCHAR(128) NOT NULL,    -- e.g., "inventory-service"
    status          ENUM('PENDING', 'EXECUTING', 'COMPLETED',
                         'FAILED', 'COMPENSATING', 'COMPENSATED',
                         'COMPENSATION_FAILED') NOT NULL,
    command_data    JSON NOT NULL,            -- input to the step
    response_data   JSON,                    -- output from the step
    error_data      JSON,                    -- error if failed
    started_at      TIMESTAMP NULL,
    completed_at    TIMESTAMP NULL,
    retry_count     INT DEFAULT 0,
    max_retries     INT DEFAULT 3,
    FOREIGN KEY (saga_id) REFERENCES saga_instance(saga_id),
    INDEX idx_saga_step (saga_id, step_index)
);

CREATE TABLE outbox_event (
    event_id        BIGINT AUTO_INCREMENT PRIMARY KEY,
    aggregate_type  VARCHAR(128) NOT NULL,   -- e.g., "server"
    aggregate_id    VARCHAR(128) NOT NULL,   -- e.g., server_id
    event_type      VARCHAR(128) NOT NULL,   -- e.g., "ServerReserved"
    payload         JSON NOT NULL,
    created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    published       BOOLEAN DEFAULT FALSE,
    published_at    TIMESTAMP NULL,
    INDEX idx_unpublished (published, created_at)
);
```

### Database Selection

| Store | Use | Rationale |
|-------|-----|-----------|
| **MySQL** | Transaction log, saga state, outbox | ACID transactions for atomic state updates; existing infrastructure |
| **Kafka** | Async command/event messaging between coordinator and participants | Durable, ordered, exactly-once delivery; decouples services |
| **Redis** | Idempotency key cache | Fast dedup check for retried operations |

### Indexing Strategy

- `saga_instance`: Index on `status` (find running/compensating sagas), `timeout_at` (find timed-out sagas), `idempotency_key` (dedup).
- `saga_step`: Composite index on `(saga_id, step_index)` for step-by-step execution.
- `outbox_event`: Index on `(published, created_at)` for the outbox poller.

---

## 5. API Design

### gRPC Service

```protobuf
service TransactionCoordinator {
  // Start a new saga
  rpc StartSaga(StartSagaRequest) returns (StartSagaResponse);

  // Get saga status
  rpc GetSagaStatus(GetSagaStatusRequest) returns (SagaStatus);

  // Cancel a running saga (triggers compensation)
  rpc CancelSaga(CancelSagaRequest) returns (CancelSagaResponse);

  // List sagas (admin, paginated)
  rpc ListSagas(ListSagasRequest) returns (ListSagasResponse);

  // Retry a failed saga step
  rpc RetryStep(RetryStepRequest) returns (RetryStepResponse);

  // Register a saga definition
  rpc RegisterSagaDefinition(SagaDefinition)
      returns (RegisterResponse);
}

message StartSagaRequest {
  string saga_type = 1;           // e.g., "bare_metal_provision"
  string idempotency_key = 2;     // prevent duplicate sagas
  bytes input_data = 3;           // JSON payload
  int32 timeout_seconds = 4;      // saga-level timeout
}

message SagaStatus {
  string saga_id = 1;
  string saga_type = 2;
  string status = 3;              // STARTED, RUNNING, etc.
  repeated StepStatus steps = 4;
  string error = 5;
  string created_at = 6;
  string completed_at = 7;
  int32 progress_pct = 8;         // 0-100
}

message StepStatus {
  int32 step_index = 1;
  string step_name = 2;
  string participant = 3;
  string status = 4;
  int32 retry_count = 5;
  string error = 6;
}

message SagaDefinition {
  string saga_type = 1;
  repeated StepDefinition steps = 2;
  int32 default_timeout_seconds = 3;
}

message StepDefinition {
  string step_name = 1;
  string participant = 2;         // service name
  string forward_action = 3;     // action to execute
  string compensate_action = 4;  // action to undo
  int32 timeout_seconds = 5;
  int32 max_retries = 6;
  bool is_compensatable = 7;     // some steps can't be undone
}
```

### REST Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/v1/sagas` | Start a new saga |
| GET | `/v1/sagas/{saga_id}` | Get saga status |
| POST | `/v1/sagas/{saga_id}/cancel` | Cancel and compensate |
| GET | `/v1/sagas?status={s}&type={t}` | List sagas |
| POST | `/v1/sagas/{saga_id}/steps/{idx}/retry` | Retry a failed step |
| POST | `/v1/saga-definitions` | Register saga definition |

### CLI

```bash
# Start a provisioning saga
txnctl saga start bare_metal_provision \
  --input '{"server_id":"srv-42","tenant_id":"t-7","network":"vlan-100"}' \
  --timeout 300s
# Output: Saga started. ID: saga-abc123

# Check status
txnctl saga status saga-abc123
# Output:
# Saga:     saga-abc123
# Type:     bare_metal_provision
# Status:   RUNNING
# Progress: 50% (2/4 steps)
#
# STEP  NAME               PARTICIPANT        STATUS      DURATION
# 1     reserve_server     inventory-service   COMPLETED   120ms
# 2     configure_vlan     network-service     COMPLETED   340ms
# 3     create_charge      billing-service     EXECUTING   ...
# 4     power_on           ipmi-controller     PENDING     -

# Cancel (triggers compensation)
txnctl saga cancel saga-abc123 --reason "Tenant request"

# List failed sagas
txnctl saga list --status FAILED --last 24h
# Output:
# SAGA ID        TYPE                    STATUS    FAILED AT           ERROR
# saga-def456    bare_metal_provision    FAILED    2026-04-09T10:30Z   IPMI timeout
# saga-ghi789    vm_provision            FAILED    2026-04-09T10:15Z   Billing error
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Saga Pattern (Orchestration)

**Why it's hard:**
In a microservices architecture, each service has its own database. There's no global transaction manager. If step 3 of a 5-step process fails, steps 1 and 2 must be "undone" -- but there's no ROLLBACK across databases. Compensating transactions are semantic inverses, not true rollbacks. They must handle edge cases: what if compensation also fails? What if the forward step partially completed? What if the step is not compensatable (e.g., sending an email)?

**Approaches:**

| Pattern | Coordination | Consistency | Complexity | Coupling |
|---------|-------------|-------------|------------|----------|
| **Orchestration Saga** | **Central coordinator drives steps** | **Eventual** | **Medium** | **Low (coordinator knows the flow)** |
| Choreography Saga | Each service emits events; next service reacts | Eventual | High (distributed logic) | Very low (no coordinator) |
| Two-Phase Commit (2PC) | Coordinator: prepare → commit/abort | Strong (atomic) | Medium | High (all participants locked) |
| TCC (Try-Confirm-Cancel) | Business-level 2PC: try → confirm/cancel | Strong | High | Medium |

**Selected: Orchestration Saga (primary) + 2PC (for tightly-coupled cases)**

**Saga Orchestrator Implementation:**

```python
class SagaOrchestrator:
    """
    Executes sagas step-by-step with compensation on failure.
    Uses the Transaction Log (MySQL) for state persistence.
    """

    def __init__(self, db, kafka_producer, kafka_consumer,
                 definition_registry):
        self.db = db
        self.producer = kafka_producer
        self.consumer = kafka_consumer
        self.registry = definition_registry

    def start_saga(self, saga_type, input_data, idempotency_key,
                    timeout_seconds=300):
        """
        Start a new saga instance.
        """
        # Idempotency check
        existing = self.db.find_saga_by_idempotency_key(idempotency_key)
        if existing:
            return existing  # Return existing saga (idempotent)

        # Get saga definition
        definition = self.registry.get(saga_type)
        if not definition:
            raise UnknownSagaTypeError(saga_type)

        # Create saga instance in DB
        saga = SagaInstance(
            saga_id=generate_id(),
            saga_type=saga_type,
            status=SagaStatus.STARTED,
            input_data=input_data,
            timeout_at=datetime.utcnow() + timedelta(seconds=timeout_seconds),
            idempotency_key=idempotency_key,
            steps=[
                SagaStep(
                    step_id=generate_id(),
                    step_index=i,
                    step_name=step_def.step_name,
                    participant=step_def.participant,
                    status=StepStatus.PENDING,
                    command_data=self._build_command(step_def, input_data)
                )
                for i, step_def in enumerate(definition.steps)
            ]
        )
        self.db.save_saga(saga)

        # Execute first step
        self._execute_next_step(saga)
        return saga

    def _execute_next_step(self, saga):
        """Execute the next pending step in the saga."""
        step = saga.next_pending_step()
        if step is None:
            # All steps completed — saga is done
            saga.status = SagaStatus.COMPLETED
            saga.completed_at = datetime.utcnow()
            self.db.update_saga(saga)
            self._emit_saga_completed(saga)
            return

        # Mark step as executing
        step.status = StepStatus.EXECUTING
        step.started_at = datetime.utcnow()
        self.db.update_step(step)

        # Send command to participant
        command = SagaCommand(
            saga_id=saga.saga_id,
            step_id=step.step_id,
            step_name=step.step_name,
            action=step.forward_action,
            data=step.command_data,
            idempotency_key=f"{saga.saga_id}-{step.step_index}"
        )

        topic = f"{step.participant}.commands"
        self.producer.send(topic, command.to_json())

    def handle_step_response(self, response):
        """
        Called when a participant reports step completion/failure.
        """
        saga = self.db.get_saga(response.saga_id)
        step = saga.get_step(response.step_id)

        if response.success:
            step.status = StepStatus.COMPLETED
            step.response_data = response.data
            step.completed_at = datetime.utcnow()
            self.db.update_step(step)

            # Execute next step
            self._execute_next_step(saga)

        else:
            # Step failed
            step.status = StepStatus.FAILED
            step.error_data = response.error
            step.retry_count += 1
            self.db.update_step(step)

            # Retry or compensate?
            if step.retry_count < step.max_retries:
                # Retry the step
                log.warning(
                    f"Saga {saga.saga_id} step {step.step_name} failed. "
                    f"Retry {step.retry_count}/{step.max_retries}"
                )
                self._retry_step(saga, step)
            else:
                # Max retries exceeded — start compensation
                log.error(
                    f"Saga {saga.saga_id} step {step.step_name} "
                    f"permanently failed. Starting compensation."
                )
                self._start_compensation(saga, step)

    def _start_compensation(self, saga, failed_step):
        """
        Compensate all completed steps in reverse order.
        """
        saga.status = SagaStatus.COMPENSATING
        self.db.update_saga(saga)

        # Get completed steps in reverse order
        completed_steps = [
            s for s in saga.steps
            if s.status == StepStatus.COMPLETED
            and s.step_index < failed_step.step_index
        ]
        completed_steps.sort(key=lambda s: -s.step_index)  # Reverse

        for step in completed_steps:
            self._compensate_step(saga, step)

    def _compensate_step(self, saga, step):
        """Execute the compensating action for a step."""
        definition = self.registry.get(saga.saga_type)
        step_def = definition.steps[step.step_index]

        if not step_def.compensate_action:
            log.warning(
                f"Step {step.step_name} has no compensation. Skipping."
            )
            step.status = StepStatus.COMPENSATED
            self.db.update_step(step)
            return

        step.status = StepStatus.COMPENSATING
        self.db.update_step(step)

        command = SagaCommand(
            saga_id=saga.saga_id,
            step_id=step.step_id,
            step_name=f"compensate_{step.step_name}",
            action=step_def.compensate_action,
            data={
                "original_command": step.command_data,
                "original_response": step.response_data
            },
            idempotency_key=f"{saga.saga_id}-{step.step_index}-compensate"
        )

        topic = f"{step.participant}.commands"
        self.producer.send(topic, command.to_json())


# Example: Bare-Metal Provisioning Saga Definition
BARE_METAL_PROVISION_SAGA = SagaDefinition(
    saga_type="bare_metal_provision",
    default_timeout_seconds=600,
    steps=[
        StepDefinition(
            step_name="reserve_server",
            participant="inventory-service",
            forward_action="ReserveServer",
            compensate_action="UnreserveServer",
            timeout_seconds=30,
            max_retries=3,
            is_compensatable=True
        ),
        StepDefinition(
            step_name="configure_network",
            participant="network-service",
            forward_action="ConfigureVLAN",
            compensate_action="RemoveVLAN",
            timeout_seconds=60,
            max_retries=2,
            is_compensatable=True
        ),
        StepDefinition(
            step_name="create_billing_charge",
            participant="billing-service",
            forward_action="CreateCharge",
            compensate_action="RefundCharge",
            timeout_seconds=30,
            max_retries=3,
            is_compensatable=True
        ),
        StepDefinition(
            step_name="power_on_server",
            participant="ipmi-controller",
            forward_action="PowerOn",
            compensate_action="PowerOff",
            timeout_seconds=120,
            max_retries=2,
            is_compensatable=True  # Can power off, but may leave
                                   # hardware in inconsistent state
        )
    ]
)
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Coordinator crashes mid-saga | Saga stuck | On restart, coordinator loads incomplete sagas from DB and resumes |
| Participant crashes after receiving command | Step never completes | Timeout triggers retry; idempotency key prevents double-execution |
| Kafka loses a message | Step not delivered | Kafka replication factor = 3; acks=all; at-least-once delivery |
| Compensation fails | Saga in inconsistent state | Retry compensation with exponential backoff; dead-letter after max retries; manual intervention |
| Concurrent saga instances for same resource | Race condition | Idempotency key prevents duplicates; distributed locks for resource-level serialization |
| Network partition between coordinator and participant | Commands/responses delayed | Timeouts + retries; eventually consistent |

**Interviewer Q&A:**

**Q1: What is the fundamental difference between a saga and a 2PC?**
A: 2PC provides atomicity (all or nothing) with locking — participants hold locks during the prepare phase, blocking other transactions. A saga provides eventual consistency — each step commits locally, and compensation is used on failure. Sagas don't hold locks, so they have better availability and throughput, but they expose intermediate states (a server may be temporarily reserved but not yet billed).

**Q2: Why use orchestration over choreography?**
A: Orchestration: central coordinator manages the flow. Easier to understand, debug, and modify. Supports complex flows (branching, parallel steps). Single point of failure (coordinator). Choreography: each service reacts to events. No SPOF. But: the flow is distributed across services; hard to understand the full picture; adding a step requires changing multiple services. For infrastructure workflows (provisioning, scheduling), orchestration is preferred because the workflows are well-defined and debuggability is critical.

**Q3: What happens if a compensating transaction fails?**
A: Retry with exponential backoff. If retries exhaust, the step goes to COMPENSATION_FAILED. The saga enters FAILED state. An alert is raised for manual intervention. The admin reviews the transaction log and either retries manually or applies a manual fix. This is why compensating actions should be idempotent and simple.

**Q4: How do you handle non-compensatable steps (e.g., sending an email)?**
A: Place non-compensatable steps at the end of the saga. If a previous step fails, the non-compensatable step hasn't executed yet, so no compensation needed. If the non-compensatable step itself fails, there's nothing to undo (it didn't happen). If you must have a non-compensatable step in the middle, accept that the saga cannot fully undo — the system enters a "partially compensated" state requiring manual intervention.

**Q5: How does this relate to Axon Framework (Java)?**
A: Axon Framework provides saga orchestration as a first-class concept. `@Saga` annotated classes define steps via `@SagaEventHandler`. Axon manages saga state persistence, event routing, and compensation. It uses event sourcing internally. For Java services, Axon significantly reduces the boilerplate of saga implementation. We can use Axon for Java participants and a custom Python coordinator.

**Q6: What is the "semantic lock" pattern in sagas?**
A: Since sagas don't use database locks, concurrent sagas can interfere. A semantic lock uses a status field in the database (e.g., `server.status = RESERVED`) as a soft lock. Other sagas check this status before proceeding. If the server is RESERVED, the second saga fails (or waits). This provides isolation without distributed locks, but requires careful status management.

---

### Deep Dive 2: Two-Phase Commit (2PC)

**Why it's hard:**
2PC guarantees atomicity (all participants commit or all abort), but it has significant drawbacks: it's blocking (participants hold locks during prepare), the coordinator is a single point of failure, and if the coordinator crashes after sending "prepare" but before sending "commit," participants are stuck in a prepared (locked) state indefinitely. Despite these drawbacks, 2PC is sometimes the only option when eventual consistency is not acceptable.

**2PC Protocol:**

```
Phase 1: Prepare
                        Coordinator
                       /     |     \
                      /      |      \
               Prepare    Prepare    Prepare
                    /        |         \
                   v         v          v
             Participant A  Participant B  Participant C
             "Yes, I can    "Yes, I can    "No, I
              commit"        commit"        cannot"
                    \        |         /
                     \       |        /
                  Vote Yes  Vote Yes  Vote No
                        \    |    /
                         v   v   v
                        Coordinator
                    (Any "No" → ABORT)
                    (All "Yes" → COMMIT)

Phase 2: Commit (or Abort)
                        Coordinator
                       /     |     \
                      /      |      \
                Abort     Abort     Abort
                    /        |         \
                   v         v          v
             Participant A  Participant B  Participant C
             "Roll back"    "Roll back"    "Already
                                            aborted"
```

**Implementation:**

```python
class TwoPhaseCommitCoordinator:
    """
    Two-Phase Commit coordinator.
    Used for tightly-coupled, synchronous operations where
    eventual consistency is NOT acceptable.
    """

    def __init__(self, db, participants):
        self.db = db
        self.participants = participants

    def execute(self, transaction_id, operations):
        """
        Execute a 2PC transaction.

        operations: dict of {participant_id: operation_data}

        Returns: COMMITTED or ABORTED
        """
        # Persist transaction as PREPARING
        self.db.save_transaction(
            transaction_id, status="PREPARING", operations=operations)

        # Phase 1: Prepare
        votes = {}
        for participant_id, op_data in operations.items():
            participant = self.participants[participant_id]
            try:
                vote = participant.prepare(transaction_id, op_data)
                votes[participant_id] = vote
                log.info(
                    f"2PC {transaction_id}: {participant_id} voted "
                    f"{'YES' if vote else 'NO'}"
                )
            except Exception as e:
                votes[participant_id] = False
                log.error(
                    f"2PC {transaction_id}: {participant_id} "
                    f"prepare failed: {e}"
                )

        # Decision
        all_voted_yes = all(votes.values())

        if all_voted_yes:
            # Phase 2: Commit
            self.db.update_transaction(transaction_id, status="COMMITTING")
            self._commit_all(transaction_id, operations)
            self.db.update_transaction(transaction_id, status="COMMITTED")
            return "COMMITTED"
        else:
            # Phase 2: Abort
            self.db.update_transaction(transaction_id, status="ABORTING")
            self._abort_all(transaction_id, operations)
            self.db.update_transaction(transaction_id, status="ABORTED")
            return "ABORTED"

    def _commit_all(self, transaction_id, operations):
        """Send commit to all participants."""
        for participant_id in operations:
            participant = self.participants[participant_id]
            # Retry commit indefinitely — once we decide to commit,
            # we MUST commit (crash recovery will retry)
            self._retry_until_success(
                lambda: participant.commit(transaction_id),
                f"commit {participant_id}"
            )

    def _abort_all(self, transaction_id, operations):
        """Send abort to all participants."""
        for participant_id in operations:
            participant = self.participants[participant_id]
            try:
                participant.abort(transaction_id)
            except Exception as e:
                log.warning(
                    f"2PC {transaction_id}: abort {participant_id} "
                    f"failed: {e}. Will retry on recovery."
                )

    def recover(self):
        """
        On coordinator startup, recover in-doubt transactions.
        This is the critical recovery path.
        """
        in_doubt = self.db.find_transactions(status="COMMITTING")
        for txn in in_doubt:
            log.info(
                f"Recovering in-doubt transaction {txn.transaction_id}. "
                f"Decision was COMMIT. Retrying commit."
            )
            self._commit_all(txn.transaction_id, txn.operations)
            self.db.update_transaction(txn.transaction_id,
                                        status="COMMITTED")

        preparing = self.db.find_transactions(status="PREPARING")
        for txn in preparing:
            log.info(
                f"Recovering preparing transaction {txn.transaction_id}. "
                f"Decision unknown. Aborting."
            )
            self._abort_all(txn.transaction_id, txn.operations)
            self.db.update_transaction(txn.transaction_id,
                                        status="ABORTED")
```

**2PC vs 3PC:**

```
2PC Problem: Coordinator crashes after PREPARE, before COMMIT/ABORT.
Participants are in "prepared" state — holding locks.
They cannot commit (don't know coordinator's decision).
They cannot abort (coordinator might have decided to commit).
→ Blocked until coordinator recovers.

3PC Adds: PRE-COMMIT phase between PREPARE and COMMIT.
  Phase 1: CAN-COMMIT? → Participants respond YES/NO
  Phase 2: PRE-COMMIT → Participants acknowledge (no locks yet)
  Phase 3: DO-COMMIT → Participants commit (locks held briefly)

3PC reduces blocking but does NOT prevent inconsistency during
network partitions. The FLP impossibility result shows that no
deterministic protocol can achieve both safety and liveness in
an asynchronous system with failures.

In practice: 2PC with coordinator HA (replicated log) is preferred
over 3PC. Google Spanner uses 2PC + Paxos for coordinator HA.
```

**Interviewer Q&A:**

**Q1: When would you use 2PC in an infrastructure platform?**
A: Rarely. 2PC is justified when: (1) The operation MUST be atomic (no intermediate states allowed). (2) All participants are tightly coupled (same team, same datacenter, low latency). (3) The transaction is fast (< 1 second). Example: reserving a server in inventory + creating a network port in SDN simultaneously, where having a reserved server with no network port (or vice versa) is dangerous. For most infrastructure workflows, sagas with compensation are preferred.

**Q2: What is the coordinator failure problem in 2PC?**
A: If the coordinator crashes after sending PREPARE but before sending COMMIT/ABORT, participants are "in doubt." They've prepared (locked resources) but don't know the decision. They remain blocked until the coordinator recovers and tells them. This is why the coordinator must persist its decision before sending COMMIT/ABORT, and why coordinator HA (replicated transaction log) is critical.

**Q3: How does Google Spanner solve the 2PC coordinator problem?**
A: Spanner replicates the coordinator's transaction log using Paxos. If the coordinator node crashes, a Paxos replica takes over and knows the decision. Participants query the Paxos group for the decision. This eliminates the single-point-of-failure. The cost: Paxos adds latency (cross-datacenter replication). Spanner accepts this because it provides globally consistent transactions.

**Q4: What is the difference between 2PC and XA?**
A: XA is a standard interface (specification) for distributed transactions. 2PC is the protocol. XA defines how a transaction manager (TM) communicates with resource managers (RMs) — the `xa_prepare`, `xa_commit`, `xa_rollback` calls. In Java, JTA (Java Transaction API) implements XA. An application server (e.g., WildFly, Tomcat) acts as the TM, and JDBC drivers act as RMs. XA/2PC across microservices is impractical because it requires all databases to support XA and all to be accessible from a single TM.

**Q5: What is the blocking nature of 2PC and why does it matter?**
A: During Phase 1 (prepare), each participant acquires locks on the resources it will modify. These locks are held until Phase 2 (commit or abort). If the coordinator is slow or crashes, locks are held indefinitely, blocking all other transactions on those resources. In a high-throughput system, this can cause cascading timeouts and outages. Sagas don't have this problem because each step commits independently.

**Q6: Can 2PC achieve exactly-once semantics?**
A: 2PC achieves atomicity: all participants commit or all abort. This is stronger than exactly-once — it's all-or-nothing. But it doesn't prevent duplicate delivery at the network level. Participants must be idempotent: receiving a duplicate COMMIT for an already-committed transaction should be a no-op.

---

### Deep Dive 3: Outbox Pattern

**Why it's hard:**
A service often needs to update its database AND publish an event to Kafka atomically. If the DB write succeeds but the Kafka publish fails, the system is inconsistent. If you publish first and then write to DB, and the DB write fails, you've published a false event. The outbox pattern solves this by writing the event to a local outbox table in the same database transaction as the business data, then asynchronously publishing from the outbox.

**Implementation:**

```python
class OutboxPattern:
    """
    Ensures atomic DB write + event publish.
    The event is written to the outbox table in the same DB transaction.
    A background process polls the outbox and publishes to Kafka.
    """

    def reserve_server(self, server_id, tenant_id, saga_id):
        """
        Business operation: reserve a server.
        Both the state change and the event are in the same transaction.
        """
        with self.db.transaction() as txn:
            # Business logic: update server state
            txn.execute(
                "UPDATE servers SET status = 'RESERVED', "
                "tenant_id = %s WHERE server_id = %s AND status = 'AVAILABLE'",
                (tenant_id, server_id)
            )

            # Check if update succeeded (optimistic lock)
            if txn.rowcount == 0:
                raise ServerNotAvailableError(server_id)

            # Write event to outbox (SAME transaction)
            event = {
                "event_type": "ServerReserved",
                "aggregate_type": "server",
                "aggregate_id": server_id,
                "payload": {
                    "server_id": server_id,
                    "tenant_id": tenant_id,
                    "saga_id": saga_id,
                    "reserved_at": datetime.utcnow().isoformat()
                }
            }
            txn.execute(
                "INSERT INTO outbox_event "
                "(aggregate_type, aggregate_id, event_type, payload) "
                "VALUES (%s, %s, %s, %s)",
                (event["aggregate_type"], event["aggregate_id"],
                 event["event_type"], json.dumps(event["payload"]))
            )

            # Transaction commits both the reservation AND the event
            # atomically. If either fails, both roll back.


class OutboxPoller:
    """
    Background process that polls the outbox table and publishes
    events to Kafka. Uses Debezium CDC as an alternative.
    """

    def __init__(self, db, kafka_producer, poll_interval_ms=100):
        self.db = db
        self.producer = kafka_producer
        self.poll_interval = poll_interval_ms / 1000.0

    def run(self):
        """Main polling loop."""
        while True:
            events = self.db.query(
                "SELECT * FROM outbox_event "
                "WHERE published = FALSE "
                "ORDER BY event_id "
                "LIMIT 100 "
                "FOR UPDATE SKIP LOCKED"  # Concurrency safe
            )

            for event in events:
                try:
                    # Publish to Kafka
                    topic = f"{event.aggregate_type}.events"
                    self.producer.send(
                        topic,
                        key=event.aggregate_id,
                        value=json.dumps({
                            "event_id": event.event_id,
                            "event_type": event.event_type,
                            "aggregate_type": event.aggregate_type,
                            "aggregate_id": event.aggregate_id,
                            "payload": event.payload,
                            "timestamp": event.created_at.isoformat()
                        })
                    )

                    # Mark as published
                    self.db.execute(
                        "UPDATE outbox_event SET published = TRUE, "
                        "published_at = NOW() WHERE event_id = %s",
                        (event.event_id,)
                    )
                except Exception as e:
                    log.error(f"Failed to publish event {event.event_id}: {e}")
                    # Will retry on next poll

            time.sleep(self.poll_interval)


class DebeziumCDCAlternative:
    """
    Alternative to polling: use Debezium CDC (Change Data Capture)
    to stream outbox table changes to Kafka automatically.

    Debezium reads the MySQL binlog and publishes row changes.
    With the "outbox event router" SMT (Single Message Transform),
    Debezium can route outbox events to the correct Kafka topic.

    Advantages over polling:
    - Lower latency (real-time from binlog)
    - No polling overhead
    - Handles high throughput better

    Disadvantages:
    - Additional infrastructure (Debezium Connect cluster)
    - Binlog dependency (must be enabled on MySQL)
    """
    pass
```

**Interviewer Q&A:**

**Q1: Why not just publish to Kafka first, then write to DB?**
A: If Kafka publish succeeds but DB write fails, you've published a false event. Downstream consumers react to an event that never actually happened. This is worse than the reverse (DB succeeds, Kafka fails), because the outbox pattern can retry the Kafka publish, but you can't "un-publish" from Kafka.

**Q2: What is the latency of the outbox pattern?**
A: Polling-based: poll_interval + Kafka publish time = ~100ms-500ms. CDC-based (Debezium): binlog read latency + Kafka publish = ~50-200ms. The latency is acceptable for most infrastructure workflows (provisioning takes seconds to minutes).

**Q3: How do you ensure events are published in order?**
A: The outbox table has an auto-increment event_id. The poller processes events in event_id order. Kafka preserves order within a partition. Route events for the same aggregate to the same partition (key = aggregate_id). This ensures per-aggregate ordering.

**Q4: What about Debezium vs polling for the outbox?**
A: Debezium (CDC): lower latency, real-time, no polling overhead. But: adds operational complexity (Kafka Connect cluster, Debezium connectors). Polling: simpler, no additional infrastructure. But: higher latency, wasted CPU on empty polls. For small-medium scale, polling is fine. For high-throughput (> 10K events/sec), Debezium is preferred.

**Q5: How do you handle duplicate events (at-least-once delivery)?**
A: The outbox poller may publish an event and crash before marking it as published. On restart, it re-publishes. Consumers must be idempotent: processing the same event twice must produce the same result. Use the event_id as a deduplication key on the consumer side (check: "have I processed event_id X already?").

**Q6: How does the outbox pattern relate to Spring's @Transactional?**
A: In Java/Spring, the outbox write is inside the same `@Transactional` method as the business logic. Spring's transaction manager ensures both the business write and the outbox insert commit or rollback together. The outbox poller runs separately (not in the same transaction) and reads committed events.

---

### Deep Dive 4: TCC (Try-Confirm-Cancel)

**Why it's hard:**
TCC is a business-level 2PC. Instead of database-level locks (2PC/XA), services implement `try` (reserve resources), `confirm` (commit reservation), and `cancel` (release reservation). This gives the flexibility of sagas with the atomicity of 2PC, but requires every participant to implement three operations correctly.

**TCC Protocol:**

```
Phase 1 (Try): Reserve resources — soft lock, no commitment
  Inventory: mark server as "TENTATIVELY_RESERVED"
  Billing: create a pending charge (not yet billed)
  Network: pre-allocate VLAN (not yet configured)

Phase 2a (Confirm): Commit all reservations — hard commit
  Inventory: change status to "RESERVED"
  Billing: confirm the charge (billing starts)
  Network: apply VLAN configuration

Phase 2b (Cancel): Release all reservations — undo try
  Inventory: change status back to "AVAILABLE"
  Billing: delete the pending charge
  Network: release the pre-allocated VLAN
```

```python
class TCCCoordinator:
    """
    Try-Confirm-Cancel coordinator.
    Business-level 2PC with explicit resource reservation.
    """

    def execute(self, transaction_id, participants):
        """
        Phase 1: Try all participants.
        Phase 2: If all succeed, Confirm all. If any fail, Cancel all.
        """
        # Phase 1: Try
        try_results = {}
        for name, participant in participants.items():
            try:
                result = participant.try_action(transaction_id)
                try_results[name] = result
                if not result.success:
                    raise TryFailedError(name, result.error)
            except Exception as e:
                # Cancel all already-tried participants
                self._cancel_all(transaction_id,
                                  {n: p for n, p in participants.items()
                                   if n in try_results})
                raise

        # Phase 2: Confirm all (must succeed — retried indefinitely)
        for name, participant in participants.items():
            self._retry_until_success(
                lambda p=participant: p.confirm(transaction_id),
                f"confirm {name}"
            )

        return TransactionResult(status="CONFIRMED")

    def _cancel_all(self, transaction_id, tried_participants):
        """Cancel all participants that were successfully tried."""
        for name, participant in tried_participants.items():
            try:
                participant.cancel(transaction_id)
            except Exception as e:
                log.error(f"Cancel failed for {name}: {e}. "
                          "Manual cleanup required.")


class InventoryTCCParticipant:
    """
    Example TCC participant: Inventory service.
    """

    def try_action(self, transaction_id):
        """Reserve server tentatively."""
        result = self.db.execute(
            "UPDATE servers SET status = 'TENTATIVELY_RESERVED', "
            "transaction_id = %s "
            "WHERE server_id = %s AND status = 'AVAILABLE'",
            (transaction_id, self.server_id)
        )
        if result.rowcount == 0:
            return TryResult(success=False, error="Server not available")
        return TryResult(success=True)

    def confirm(self, transaction_id):
        """Confirm reservation — idempotent."""
        self.db.execute(
            "UPDATE servers SET status = 'RESERVED' "
            "WHERE server_id = %s AND transaction_id = %s",
            (self.server_id, transaction_id)
        )

    def cancel(self, transaction_id):
        """Cancel reservation — idempotent."""
        self.db.execute(
            "UPDATE servers SET status = 'AVAILABLE', "
            "transaction_id = NULL "
            "WHERE server_id = %s AND transaction_id = %s "
            "AND status = 'TENTATIVELY_RESERVED'",
            (self.server_id, transaction_id)
        )
```

**Interviewer Q&A:**

**Q1: When would you choose TCC over a saga?**
A: TCC when: (1) You need stronger isolation than a saga (resources are soft-locked during try, preventing other transactions from using them). (2) The confirm/cancel decision is made quickly (< 5 seconds). (3) All participants can implement the three operations. Saga when: long-running workflows, loose coupling, not all steps can reserve resources.

**Q2: What is the main risk of TCC?**
A: The "try" phase holds soft locks on resources. If the coordinator crashes between try and confirm/cancel, resources are stuck in "tentatively reserved" state. A timeout mechanism must cancel stale tentative reservations (e.g., if no confirm/cancel within 30 seconds, automatically cancel).

**Q3: How does TCC compare to 2PC?**
A: TCC is 2PC at the business level instead of the database level. 2PC holds database locks; TCC holds "semantic locks" (status fields). TCC is more flexible (business logic controls the lock), more available (no database-level blocking), but requires more implementation effort (every participant must implement try/confirm/cancel).

**Q4: Can TCC and saga be combined?**
A: Yes. Use TCC for the first few critical steps (where isolation is needed) and saga for the remaining steps (where eventual consistency is fine). Example: TCC for reserve_server + allocate_network (must be atomic), then saga for billing + notification.

**Q5: How do you handle network timeout during the confirm phase?**
A: The confirm phase MUST eventually succeed (the resources are already reserved). If a confirm times out, retry indefinitely with exponential backoff. If a participant is permanently down, escalate to manual intervention. The key insight: once all tries succeed, the decision is to confirm, and this decision must be honored.

**Q6: What is the "heuristic exception" in TCC/2PC?**
A: When a participant unilaterally decides to commit or abort without the coordinator's instruction (e.g., after a timeout). This violates the protocol and can lead to inconsistency. It's a last resort to avoid infinite blocking. The system must detect heuristic exceptions, alert operators, and trigger manual reconciliation.

---

## 7. Scheduling & Resource Management

### Distributed Transactions in Job Scheduling

```python
class JobSchedulingSaga:
    """
    Saga for scheduling a job that requires:
    1. Resource reservation (bare-metal server)
    2. Network configuration (VLAN assignment)
    3. Storage allocation (NFS mount)
    4. Job queue entry creation
    """

    DEFINITION = SagaDefinition(
        saga_type="schedule_job",
        steps=[
            StepDefinition(
                step_name="reserve_compute",
                participant="resource-manager",
                forward_action="ReserveCompute",
                compensate_action="ReleaseCompute",
                timeout_seconds=30,
                max_retries=3
            ),
            StepDefinition(
                step_name="configure_network",
                participant="network-manager",
                forward_action="AssignNetwork",
                compensate_action="ReleaseNetwork",
                timeout_seconds=60,
                max_retries=2
            ),
            StepDefinition(
                step_name="allocate_storage",
                participant="storage-manager",
                forward_action="AllocateStorage",
                compensate_action="DeallocateStorage",
                timeout_seconds=60,
                max_retries=2
            ),
            StepDefinition(
                step_name="enqueue_job",
                participant="job-queue",
                forward_action="EnqueueJob",
                compensate_action="DequeueJob",
                timeout_seconds=10,
                max_retries=3
            ),
        ]
    )
```

**Example: VM Provisioning with OpenStack-Style Workflow**

```
Saga: provision_vm
Step 1: Nova → ReserveInstance (compute capacity)
Step 2: Neutron → CreatePort (network port)
Step 3: Cinder → CreateVolume (block storage)
Step 4: Glance → VerifyImage (image exists)
Step 5: Nova → SpawnVM (actually create the VM)

If Step 3 (CreateVolume) fails:
  Compensate Step 2: Neutron → DeletePort
  Compensate Step 1: Nova → ReleaseInstance
  (Steps 4 and 5 were never executed — no compensation needed)
```

---

## 8. Scaling Strategy

| Dimension | Strategy | Detail |
|-----------|----------|--------|
| Transaction throughput | Shard coordinator by saga_type or key range | Each coordinator shard handles a subset of sagas |
| Kafka throughput | Increase partitions per topic | Parallel consumption |
| MySQL (transaction log) | Read replicas for status queries; primary for writes | Write volume is manageable (4K writes/sec) |
| In-flight transactions | Limit per coordinator (backpressure) | Each coordinator handles max 10K in-flight |
| Multi-datacenter | Per-DC coordinator + cross-DC saga for global ops | Local sagas are fast; cross-DC sagas use 2PC at DC level |

**Interviewer Q&A:**

**Q1: How do you scale the saga coordinator?**
A: (1) Partition by saga type: different coordinators handle different workflows. (2) Partition by key: sagas for tenant A go to coordinator 1, tenant B to coordinator 2 (consistent hashing). (3) Multiple coordinator instances with saga ownership: each saga is owned by one coordinator (via consistent hash); on coordinator failure, another takes over.

**Q2: What's the bottleneck in a saga system?**
A: The slowest participant. A saga's latency is the sum of all step latencies (sequential saga). Mitigation: (1) Parallelize independent steps (e.g., reserve compute and allocate storage can run in parallel). (2) Set aggressive per-step timeouts. (3) Optimize the slowest participant (often the network or IPMI step).

**Q3: How do you handle saga coordinator failover?**
A: The coordinator persists saga state to MySQL. On startup, it loads all incomplete sagas (status = RUNNING or COMPENSATING) and resumes them. Idempotency keys ensure resumed steps don't duplicate work. Multiple coordinator instances can share the workload (saga ownership via leader election or consistent hashing).

**Q4: How do you handle saga ordering (two sagas for the same resource)?**
A: (1) Idempotency key prevents duplicate sagas. (2) For sequential operations on the same resource, use the semantic lock pattern: the resource's status field acts as a guard. (3) For true ordering, route all sagas for the same resource to the same coordinator partition (serialized execution).

**Q5: What is the performance impact of the outbox pattern?**
A: One additional INSERT per business operation (into the outbox table). At 4K ops/sec, that's 4K inserts/sec into the outbox — well within MySQL capabilities. The poller adds ~100ms latency to event publishing. With Debezium CDC, the latency drops to ~50ms and there's no polling overhead.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO |
|---|---------|-----------|--------|----------|-----|
| 1 | Coordinator crash | Health check / k8s restart | In-flight sagas paused | New coordinator loads sagas from DB and resumes | 10-30s |
| 2 | Participant crash | Step timeout | Step fails; retry or compensate | Participant restarts; coordinator retries step | Step timeout (30-120s) |
| 3 | Kafka unavailable | Producer send fails | Commands not delivered | Retry with backoff; sagas stalled until Kafka recovers | Kafka recovery time |
| 4 | MySQL (txn log) unavailable | DB connection fails | Cannot start new sagas; cannot update state | Existing sagas continue in-memory; new sagas rejected | MySQL recovery time |
| 5 | Compensation failure | Compensation step returns error | Saga stuck in COMPENSATING | Retry compensation; dead-letter after max retries; manual fix | Minutes to hours |
| 6 | Poison message (unprocessable command) | Participant throws unretryable error | Step cannot complete | Move to DLQ; alert; manual investigation | Manual |
| 7 | Network partition between coordinator and participant | Command/response timeout | Step treated as failed | Timeout + retry; eventually consistent | Step timeout |
| 8 | Duplicate saga (idempotency failure) | Two identical sagas created | Double execution of business logic | Idempotency key deduplication; if missed, semantic locks | Prevented by design |

### Consensus & Coordination

The saga coordinator itself needs HA. Options:
1. **Active-passive with leader election:** One coordinator processes sagas; standby takes over on failure. Uses the leader election service (covered in leader_election_service.md).
2. **Partitioned active-active:** Multiple coordinators, each owning a partition of sagas. No SPOF for any single saga, but each coordinator is a SPOF for its partition.
3. **Selected: Partitioned active-active with failover.** Each saga is assigned to a coordinator via consistent hashing. If a coordinator fails, its sagas are reassigned to survivors.

---

## 10. Observability

### Key Metrics

| # | Metric | Type | Alert Threshold | Why |
|---|--------|------|-----------------|-----|
| 1 | `saga.start.rate` | Counter | > 5K/sec | Approaching coordinator capacity |
| 2 | `saga.completion.rate` | Counter | N/A | Business throughput |
| 3 | `saga.compensation.rate` | Counter | > 10% of starts | High failure rate |
| 4 | `saga.duration.p99` | Histogram per type | > 30s | Slow transactions |
| 5 | `saga.in_flight` | Gauge | > 20K | Coordinator overloaded |
| 6 | `saga.step.duration.p99` | Histogram per step | > 10s | Slow participant |
| 7 | `saga.step.failure.rate` | Counter per step | > 5% | Participant reliability issue |
| 8 | `saga.compensation.failure.rate` | Counter | > 0.1% | Compensation not working; inconsistency risk |
| 9 | `saga.timeout.rate` | Counter | > 1% | Steps too slow or deadlocked |
| 10 | `saga.dlq.depth` | Gauge | > 0 | Unprocessable messages; manual intervention needed |
| 11 | `outbox.lag` | Gauge (seconds) | > 5s | Events not being published; downstream stale |
| 12 | `outbox.unpublished.count` | Gauge | > 1000 | Kafka or poller issue |
| 13 | `2pc.in_doubt.count` | Gauge | > 0 | Coordinator crashed during 2PC; participants locked |
| 14 | `2pc.latency.p99` | Histogram | > 500ms | 2PC taking too long; lock contention |

---

## 11. Security

| Threat | Mitigation |
|--------|------------|
| Unauthorized saga initiation | mTLS authentication; RBAC per saga type |
| Transaction log tampering | Append-only log; checksums; audit trail |
| Compensation abuse (trigger refund without purchase) | Compensation only executes as part of saga flow; no standalone compensation API |
| Participant impersonation | mTLS between coordinator and participants; verify participant identity |
| Sensitive data in transaction log | Encrypt PII in saga payloads; restrict transaction log access |
| DLQ data exposure | DLQ messages may contain sensitive data; encrypt at rest; restrict access |

---

## 12. Incremental Rollout Strategy

### Phase 1: Saga Framework (Week 1-4)
- Build saga orchestrator with MySQL transaction log.
- Implement outbox pattern.
- Unit and integration tests.
- First saga: simple 2-step workflow (reserve + unreserve).

### Phase 2: Core Provisioning Saga (Week 5-8)
- Implement bare-metal provisioning saga (4 steps).
- Integration test each participant's forward + compensating action.
- Chaos test: kill coordinator mid-saga; verify recovery.
- Shadow mode: run new saga alongside existing provisioning; compare results.

### Phase 3: Migration (Week 9-12)
- Migrate existing provisioning workflow to saga-based.
- Monitor compensation rates, latency, success rates.
- Add VM provisioning saga.
- Add job scheduling saga.

### Phase 4: Advanced Features (Week 13-16)
- Add TCC for latency-sensitive workflows.
- Add parallel step execution.
- Add Debezium CDC for outbox.
- Add saga visualization dashboard.

**Rollout Q&A:**

**Q1: How do you test compensating transactions?**
A: (1) For each step, write a test that executes the forward action then the compensating action, and verifies the system is back to its original state. (2) Chaos test: inject failures at each step of the saga; verify compensation runs correctly. (3) Property-based testing: randomly inject failures at any step; verify the system is always either fully committed or fully compensated.

**Q2: What if the compensation logic has a bug?**
A: (1) Detect via monitoring (COMPENSATION_FAILED status). (2) Fix the bug, deploy the fix. (3) Retry failed compensations via admin API (`txnctl saga retry-compensation saga-abc123`). (4) For data already in inconsistent state, run a reconciliation job that compares the states of all participants and fixes discrepancies.

**Q3: How do you handle the transition from the old provisioning system to the new saga-based system?**
A: (1) Dual-run: both old and new systems process provisioning. (2) Compare results. (3) Gradually route traffic from old to new (10% -> 50% -> 100%). (4) The old system is the fallback. (5) Both systems are idempotent, so running both for the same request is safe.

**Q4: What if a saga takes longer than expected during rollout?**
A: (1) Monitor saga duration (p50, p99). (2) If p99 exceeds the SLO, identify the slow step (per-step latency metrics). (3) Optimize the slow participant. (4) If the slow step is a third-party dependency (e.g., IPMI), increase the step timeout and the saga timeout. (5) Consider making the slow step async (return early, check status later).

**Q5: How do you handle the case where a saga succeeds but the client never receives the confirmation?**
A: The client uses the idempotency key to retry. The coordinator returns the existing completed saga (not a new one). The client gets the result. Alternatively, use a webhook: the coordinator calls the client's callback URL when the saga completes.

---

## 13. Trade-offs & Decision Log

| # | Decision | Options | Selected | Rationale |
|---|----------|---------|----------|-----------|
| 1 | Primary coordination pattern | Saga vs 2PC vs TCC | Saga (orchestration) | Best fit for long-running, loosely-coupled infrastructure workflows; 2PC for rare tight-coupling cases |
| 2 | Saga style | Orchestration vs choreography | Orchestration | Central visibility; easier debugging; well-defined workflow |
| 3 | Messaging | Kafka vs RabbitMQ vs gRPC | Kafka | Durable, ordered, exactly-once available; existing infrastructure |
| 4 | Transaction log | MySQL vs Cassandra vs event store | MySQL | ACID transactions; familiar; existing infrastructure |
| 5 | Outbox implementation | Polling vs CDC (Debezium) | Polling (initial); CDC (future) | Polling is simpler to start; migrate to CDC when scale demands |
| 6 | Idempotency | Client-provided key vs server-generated | Client-provided | Client controls deduplication; server-generated doesn't prevent duplicate requests |
| 7 | Step execution | Sequential vs parallel | Sequential (default); parallel (opt-in) | Sequential is simpler to reason about; parallel for independent steps |
| 8 | Compensation order | Reverse order vs any order | Reverse order | Matches undo semantics; avoids dependency issues |
| 9 | Timeout handling | Per-step + per-saga | Both | Per-step prevents one slow step from blocking; per-saga provides overall deadline |
| 10 | Dead letter handling | Auto-retry vs manual | Auto-retry 3x, then manual (DLQ) | Most failures are transient; persistent failures need human review |

---

## 14. Agentic AI Integration

**1. Saga Failure Root Cause Analysis:**
```python
class SagaFailureAnalyzer:
    """
    AI agent that analyzes saga failures and recommends fixes.
    """

    def analyze_failure(self, saga_id):
        saga = self.coordinator.get_saga(saga_id)
        failed_step = next(s for s in saga.steps
                          if s.status == "FAILED")

        # Correlate with infrastructure metrics
        participant_health = self.metrics.get_health(
            failed_step.participant,
            window=f"{failed_step.started_at}..{failed_step.completed_at}")

        analysis = {
            "saga_id": saga_id,
            "failed_step": failed_step.step_name,
            "error": failed_step.error_data,
            "participant_health": participant_health,
            "root_cause": self._classify_error(failed_step, participant_health),
            "recommendation": self._recommend_fix(failed_step, participant_health)
        }

        return analysis

    def _classify_error(self, step, health):
        if "timeout" in str(step.error_data).lower():
            if health.latency_p99 > 10000:
                return "PARTICIPANT_OVERLOADED"
            else:
                return "NETWORK_ISSUE"
        elif "not found" in str(step.error_data).lower():
            return "RESOURCE_NOT_AVAILABLE"
        elif "permission" in str(step.error_data).lower():
            return "AUTH_FAILURE"
        return "UNKNOWN"
```

**2. Automatic Saga Optimization:**
```
Agent analyzes completed sagas:
  - Average duration per step
  - Which steps can run in parallel (no dependencies)
  - Retry rates per step (which participants are unreliable)

Recommendations:
  - "Steps 'reserve_compute' and 'allocate_storage' are independent.
    Running in parallel would reduce saga duration from 2.1s to 1.3s."
  - "Step 'configure_network' has 12% retry rate. Network service
    investigation recommended."
  - "IPMI PowerOn step has p99 of 45s. Consider async execution
    with status polling."
```

**3. Compensation Verification:**
```
Agent periodically verifies that compensated sagas left the system
in a consistent state:

For each compensated saga:
  1. Check each participant's final state
  2. Verify resources are released (server available, VLAN freed, etc.)
  3. Verify billing was refunded

Report:
  - "5 compensated sagas in last 24h. 4 fully consistent.
    1 has orphaned network port (vlan-100 on server-42 not released).
    Creating cleanup ticket."
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Explain the Saga pattern in 30 seconds.**
A: A saga is a sequence of local transactions. Each service commits its own transaction and publishes an event. If any step fails, previously completed steps are undone by compensating transactions (semantic reversal). Unlike 2PC, sagas don't hold distributed locks, providing better availability at the cost of temporary inconsistency.

**Q2: What is the difference between orchestration and choreography sagas?**
A: Orchestration: a central coordinator tells each participant what to do. The coordinator knows the full workflow and manages state. Choreography: each participant reacts to events and decides what to do next. No central coordinator. Orchestration is easier to understand and debug; choreography has no SPOF but is harder to trace.

**Q3: What are compensating transactions and why are they hard?**
A: Compensating transactions are the "undo" for each saga step. They're hard because: (1) Not every action has a perfect inverse (you can't unsend an email). (2) The compensation must be idempotent (may be called multiple times). (3) The compensation may need data from the forward step's response. (4) Compensation itself can fail, requiring retries or manual intervention.

**Q4: When would you use 2PC over a saga in an infrastructure platform?**
A: When intermediate states are dangerous. Example: if inventory shows a server as "available" after it's been partially network-configured but before the reservation is complete, another provisioner could grab it, leading to a conflict. 2PC ensures the reservation and network config are atomic. However, this is rare — most infrastructure workflows tolerate brief intermediate states.

**Q5: What is the outbox pattern and why is it needed?**
A: The outbox pattern ensures that a database write and a message publish are atomic. Without it, you might write to the DB but fail to publish the event (or vice versa). The solution: write the event to an "outbox" table in the same DB transaction as the business data. A separate process reads the outbox and publishes to Kafka. This guarantees: if the DB write committed, the event will eventually be published.

**Q6: How does the Saga pattern handle concurrent modifications to the same resource?**
A: Semantic locking: the saga's first step changes the resource's status to a "reserved" or "in-progress" state. Concurrent sagas check this status and fail (or wait). This is optimistic: the check-and-update is done atomically in the participant's database (e.g., `UPDATE ... WHERE status = 'AVAILABLE'`).

**Q7: What is the TCC pattern and when would you use it?**
A: Try-Confirm-Cancel. Phase 1 (Try): each participant reserves resources (soft lock). Phase 2 (Confirm or Cancel): if all tries succeed, confirm all; if any fails, cancel all. It's a business-level 2PC. Use when: you need isolation (prevent other transactions from using reserved resources) with faster release than 2PC (business locks instead of DB locks).

**Q8: How does Kafka's exactly-once semantics help with sagas?**
A: Kafka's exactly-once (idempotent producer + transactional consumer) ensures that saga commands and events are delivered exactly once. Without it, a command might be duplicated (at-least-once), causing a step to execute twice. With exactly-once, the coordinator doesn't need to deduplicate on the consumer side. However, participants should still be idempotent as defense-in-depth.

**Q9: What is the "dual-write problem"?**
A: Writing to two different systems (e.g., MySQL + Kafka) non-atomically. If one write succeeds and the other fails, the systems are inconsistent. The outbox pattern solves this by writing to only one system (MySQL, which includes the outbox table) and asynchronously propagating to the other (Kafka).

**Q10: How do you debug a failed saga in production?**
A: (1) Query the transaction log: `txnctl saga status <saga_id>` shows all steps, their statuses, timestamps, and errors. (2) Check per-step error data for the root cause. (3) Check the participant's logs using the saga_id as a correlation ID. (4) If a step is stuck, check Kafka (is the command waiting in the topic?). (5) If compensation failed, check why (resource lock, network issue, bug).

**Q11: What is the relationship between event sourcing and sagas?**
A: Event sourcing stores state as a sequence of events. Sagas coordinate multi-service transactions via events. They complement each other: event sourcing provides a natural audit trail for each saga step, and saga events can be replayed for debugging or reconciliation. Axon Framework combines both: sagas are driven by events, and saga state is event-sourced.

**Q12: How does Spring's @Transactional relate to distributed transactions?**
A: `@Transactional` provides local ACID transactions within a single database. It does NOT provide distributed transactions across multiple services/databases. For distributed transactions, you need a saga coordinator, 2PC, or TCC. `@Transactional` is used within each saga step to ensure the local database operations are atomic.

**Q13: What is an idempotency key and how do you implement it?**
A: A unique identifier for an operation that allows safe retries. The client generates the key (e.g., UUID) and sends it with every request. The server checks: "Have I seen this key before?" If yes, return the cached result. If no, process the request and store the key + result. Implementation: Redis or MySQL with `ON DUPLICATE KEY` for the key.

**Q14: How do you handle saga timeouts?**
A: Two levels: (1) Per-step timeout: if a step doesn't complete within N seconds, it's treated as failed (trigger retry or compensation). (2) Per-saga timeout: if the entire saga doesn't complete within M seconds, abort and compensate. A background "timeout checker" periodically scans for timed-out sagas and triggers compensation.

**Q15: What is the "semantic lock" pattern in sagas?**
A: A compensatable saga step changes a resource's status to an intermediate state (e.g., "PENDING_RESERVATION") that acts as a soft lock. Other transactions check this status and fail or wait. When the saga completes (commit or compensate), the status is updated to its final state ("RESERVED" or "AVAILABLE"). This provides isolation without distributed locks.

**Q16: How would you implement a "saga of sagas" (hierarchical saga)?**
A: A parent saga coordinates child sagas. Each step of the parent saga starts a child saga. The parent waits for the child to complete before proceeding. If the child fails, the parent compensates completed children. This is useful for complex workflows like "provision a cluster" (parent) which involves "provision each node" (children). The coordinator supports nesting by treating a child saga as a single step.

---

## 16. References

1. Garcia-Molina, H. & Salem, K. (1987). *Sagas*. ACM SIGMOD. (Original saga paper)
2. Gray, J. & Reuter, A. (1993). *Transaction Processing: Concepts and Techniques*. Morgan Kaufmann.
3. Richardson, C. (2018). *Microservices Patterns*. Manning. Chapter 4: Managing transactions with sagas.
4. Kleppmann, M. (2017). *Designing Data-Intensive Applications*. O'Reilly. Chapter 9.
5. Axon Framework. *Saga implementation*. https://docs.axoniq.io/reference-guide/axon-framework/sagas
6. Debezium. *Outbox Event Router*. https://debezium.io/documentation/reference/transformations/outbox-event-router.html
7. Bernstein, P. & Newcomer, E. (2009). *Principles of Transaction Processing*. Morgan Kaufmann.
8. Corbett, J. et al. (2012). *Spanner: Google's Globally-Distributed Database*. OSDI. (2PC + Paxos)
9. Netflix. *Conductor: Workflow Orchestration Engine*. https://github.com/Netflix/conductor
10. Uber. *Cadence: Fault-tolerant orchestration engine*. https://cadenceworkflow.io/
11. Temporal. *Temporal: Durable execution platform*. https://temporal.io/ (successor to Cadence)
