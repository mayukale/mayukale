# System Design: OpenStack Cloud Control Plane

> **Relevance to role:** The control plane is the nervous system of an OpenStack cloud. As a cloud infrastructure platform engineer, you own every component that makes API calls work: Keystone for identity, the RabbitMQ message bus, MySQL/Galera for state, Memcached for caching, HAProxy for load balancing, and the HA patterns (Pacemaker/Corosync) that keep everything running. Understanding how these components interact, fail, and scale is what separates operators who run stable clouds from those who fight fires.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Provide a unified REST API surface for all cloud services (compute, network, storage, identity, image, orchestration, secrets) |
| FR-2 | Authenticate and authorize all API requests via Keystone (Fernet tokens, RBAC) |
| FR-3 | Route RPC calls between API services, schedulers, conductors, and agents via message bus |
| FR-4 | Persist all service state in highly available relational databases |
| FR-5 | Cache frequently accessed data (tokens, catalog, API responses) for performance |
| FR-6 | Provide a VIP (Virtual IP) for each API endpoint with load balancing across multiple instances |
| FR-7 | Support rolling upgrades without service interruption |
| FR-8 | Orchestrate complex multi-resource deployments (Heat stacks) |
| FR-9 | Manage secrets and certificates (Barbican) |
| FR-10 | Rate-limit API requests to protect the control plane from abuse |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | API endpoint availability | 99.99% (< 52 min downtime/year) |
| NFR-2 | API response latency (P99) | < 500 ms for most operations |
| NFR-3 | Message bus throughput | 50,000 messages/second |
| NFR-4 | Database transaction throughput | 10,000 TPS |
| NFR-5 | Token validation latency | < 5 ms (local Fernet), < 50 ms (cache hit) |
| NFR-6 | Zero data loss on single controller failure | Synchronous DB replication |
| NFR-7 | Failover time for any single component | < 30 seconds |
| NFR-8 | Support 10,000 concurrent API connections | Across all services |

### Constraints & Assumptions
- 3 controller nodes (minimum for Galera quorum and RabbitMQ quorum)
- 5 controller nodes recommended for large deployments (survive 2 failures)
- Controller nodes: 64 vCPU, 256 GB RAM, NVMe SSDs
- All control plane services co-located on controller nodes (typical deployment)
- Alternatively: disaggregated deployment (DB nodes, MQ nodes, API nodes separately)
- Internal network: 25 Gbps between controllers, 10 Gbps to compute hosts
- OpenStack release: 2024.2 (Dalmatian) or later

### Out of Scope
- Data plane design (compute hosts, storage clusters)
- Individual service deep dives (covered in Nova, Neutron files)
- Physical hardware procurement and rack design
- Operating system and kernel tuning details

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Estimate | Calculation |
|--------|----------|-------------|
| Controller nodes | 5 | For 10K compute hosts |
| API endpoints (distinct services) | 10 | Keystone, Nova, Neutron, Cinder, Glance, Placement, Heat, Barbican, Octavia, Designate |
| Total API requests/second | 5,000 | All services combined |
| Keystone auth requests/second | 1,000 | Token issuance + validation |
| Nova API requests/second | 2,000 | List/show/create/delete VMs |
| Neutron API requests/second | 500 | Port/network CRUD |
| Other services API requests/second | 1,500 | Cinder, Glance, Heat, etc. |
| RabbitMQ messages/second | 10,000 | Nova + Neutron + Cinder RPC |
| MySQL transactions/second | 5,000 | Across all service databases |
| Concurrent API connections | 3,000 | HAProxy connection pool |

### Latency Requirements

| Operation | Target | Bottleneck |
|-----------|--------|------------|
| Keystone POST /v3/auth/tokens | < 100 ms | DB lookup + Fernet encryption |
| Keystone token validation (middleware) | < 5 ms | Local Fernet decryption |
| Nova GET /v2.1/servers (list 100) | < 200 ms | DB query + JSON serialization |
| Nova POST /v2.1/servers (acceptance) | < 500 ms | DB write + RabbitMQ publish |
| Neutron POST /v2.0/ports | < 500 ms | DB write + OVN NB DB write |
| RabbitMQ message delivery | < 10 ms | Queue → consumer |
| Galera write set certification | < 5 ms | Synchronous replication |
| Memcached get/set | < 1 ms | In-memory |
| HAProxy request routing | < 1 ms | L4/L7 proxy |

### Storage Estimates

| Component | Size | Notes |
|-----------|------|-------|
| Keystone DB | 500 MB | Users, projects, roles, assignments |
| Nova API DB | 1 GB | Flavors, request_specs, cell_mappings |
| Nova Cell DBs (3 cells) | 30 GB | Instances, migrations, compute_nodes |
| Placement DB | 500 MB | Resource providers, inventories, allocations |
| Neutron DB | 2 GB | Networks, ports, subnets, security groups |
| Cinder DB | 1 GB | Volumes, snapshots, backups |
| Glance DB | 200 MB | Image metadata (image data in Ceph) |
| Heat DB | 500 MB | Stacks, resources, events |
| Barbican DB | 200 MB | Secrets, containers, orders |
| Total MySQL storage | ~40 GB | Across all databases |
| RabbitMQ persistent messages | ~1 GB | Quorum queue journal |
| Memcached memory | 4 GB per node | Token cache, catalog cache |
| Controller disk (total) | 200 GB NVMe | OS + DB + logs |

### Bandwidth Estimates

| Flow | Bandwidth | Calculation |
|------|-----------|-------------|
| API traffic (HAProxy) | 500 Mbps | 5,000 req/s x 100 KB avg response |
| RabbitMQ replication | 200 Mbps | 10K msg/s x 10 KB x 2 replicas |
| Galera replication | 100 Mbps | 5K TPS x 2 KB avg write set x 2 replicas |
| Controller ↔ compute (RPC) | 2 Gbps | 10K compute hosts x 200 Kbps avg |
| Monitoring (Prometheus scrape) | 50 Mbps | 10K hosts x 5 KB metrics per scrape |
| Logging (rsyslog) | 100 Mbps | All services log to central ELK |

---

## 3. High Level Architecture

```
                          ┌───────────────────────────────────┐
                          │         External Clients           │
                          │  (Horizon, CLI, SDK, Terraform)    │
                          └──────────────┬────────────────────┘
                                         │ HTTPS
                                         ▼
                          ┌───────────────────────────────────┐
                          │       External Load Balancer       │
                          │  (F5 / MetalLB / DNS round-robin) │
                          └──────────────┬────────────────────┘
                                         │
         ┌───────────────────────────────┼───────────────────────────────┐
         │                               │                               │
         ▼                               ▼                               ▼
┌─────────────────┐           ┌─────────────────┐           ┌─────────────────┐
│ Controller-1    │           │ Controller-2    │           │ Controller-3    │
│                 │           │                 │           │                 │
│ ┌─────────────┐ │           │ ┌─────────────┐ │           │ ┌─────────────┐ │
│ │  HAProxy    │ │           │ │  HAProxy    │ │           │ │  HAProxy    │ │
│ │  + VIP      │ │           │ │             │ │           │ │             │ │
│ │ (keepalived)│ │           │ │ (keepalived)│ │           │ │ (keepalived)│ │
│ └──────┬──────┘ │           │ └──────┬──────┘ │           │ └──────┬──────┘ │
│        │        │           │        │        │           │        │        │
│ ┌──────▼──────┐ │           │ ┌──────▼──────┐ │           │ ┌──────▼──────┐ │
│ │  API Layer  │ │           │ │  API Layer  │ │           │ │  API Layer  │ │
│ │ ┌─────────┐ │ │           │ │ ┌─────────┐ │ │           │ │ ┌─────────┐ │ │
│ │ │Keystone │ │ │           │ │ │Keystone │ │ │           │ │ │Keystone │ │ │
│ │ │Nova-API │ │ │           │ │ │Nova-API │ │ │           │ │ │Nova-API │ │ │
│ │ │Neutron  │ │ │           │ │ │Neutron  │ │ │           │ │ │Neutron  │ │ │
│ │ │Cinder   │ │ │           │ │ │Cinder   │ │ │           │ │ │Cinder   │ │ │
│ │ │Glance   │ │ │           │ │ │Glance   │ │ │           │ │ │Glance   │ │ │
│ │ │Placement│ │ │           │ │ │Placement│ │ │           │ │ │Placement│ │ │
│ │ │Heat     │ │ │           │ │ │Heat     │ │ │           │ │ │Heat     │ │ │
│ │ │Barbican │ │ │           │ │ │Barbican │ │ │           │ │ │Barbican │ │ │
│ │ └─────────┘ │ │           │ │ └─────────┘ │ │           │ │ └─────────┘ │ │
│ └─────────────┘ │           │ └─────────────┘ │           │ └─────────────┘ │
│                 │           │                 │           │                 │
│ ┌─────────────┐ │           │ ┌─────────────┐ │           │ ┌─────────────┐ │
│ │ Workers     │ │           │ │ Workers     │ │           │ │ Workers     │ │
│ │ ┌─────────┐ │ │           │ │ ┌─────────┐ │ │           │ │ ┌─────────┐ │ │
│ │ │Scheduler│ │ │           │ │ │Scheduler│ │ │           │ │ │Scheduler│ │ │
│ │ │Conductor│ │ │           │ │ │Conductor│ │ │           │ │ │Conductor│ │ │
│ │ │Heat-Eng │ │ │           │ │ │Heat-Eng │ │ │           │ │ │Heat-Eng │ │ │
│ │ └─────────┘ │ │           │ │ └─────────┘ │ │           │ │ └─────────┘ │ │
│ └─────────────┘ │           │ └─────────────┘ │           │ └─────────────┘ │
│                 │           │                 │           │                 │
│ ┌─────────────┐ │           │ ┌─────────────┐ │           │ ┌─────────────┐ │
│ │ RabbitMQ    │ │           │ │ RabbitMQ    │ │           │ │ RabbitMQ    │ │
│ │ (cluster    │◄├───────────├►│ (cluster    │◄├───────────├►│ (cluster    │ │
│ │  member)    │ │           │ │  member)    │ │           │ │  member)    │ │
│ └─────────────┘ │           │ └─────────────┘ │           │ └─────────────┘ │
│                 │           │                 │           │                 │
│ ┌─────────────┐ │           │ ┌─────────────┐ │           │ ┌─────────────┐ │
│ │MySQL/Galera │ │           │ │MySQL/Galera │ │           │ │MySQL/Galera │ │
│ │ (cluster    │◄├───────────├►│ (cluster    │◄├───────────├►│ (cluster    │ │
│ │  member)    │ │           │ │  member)    │ │           │ │  member)    │ │
│ └─────────────┘ │           │ └─────────────┘ │           │ └─────────────┘ │
│                 │           │                 │           │                 │
│ ┌─────────────┐ │           │ ┌─────────────┐ │           │ ┌─────────────┐ │
│ │ Memcached   │ │           │ │ Memcached   │ │           │ │ Memcached   │ │
│ └─────────────┘ │           │ └─────────────┘ │           │ └─────────────┘ │
└─────────────────┘           └─────────────────┘           └─────────────────┘
         │                               │                               │
         │            ┌──────────────────┼──────────────────┐            │
         │            │                  │                  │            │
         └────────────┤    Pacemaker / Corosync Cluster     ├────────────┘
                      │    (manages VIPs, fencing)          │
                      └─────────────────────────────────────┘
```

### Component Roles

| Component | Role | HA Mode |
|-----------|------|---------|
| **HAProxy** | L7 load balancer for all API endpoints. Health checks backends. SSL termination. Connection pooling. Rate limiting. | Active-passive VIP via keepalived; all instances route traffic if external LB used |
| **keepalived** | VRRP-based VIP management. Provides floating IP for HAProxy. Automatic failover on HAProxy failure. | Active-passive; VRRP failover < 3s |
| **Keystone** | Identity service. Authenticates users, issues Fernet tokens, manages RBAC, serves service catalog. | Active-active (stateless); N instances behind HAProxy |
| **Nova-API** | Compute API front-end. Stateless. Multiple instances. | Active-active |
| **Neutron-server** | Networking API. Stateless. Multiple instances. | Active-active |
| **Cinder-API** | Block storage API. Stateless. | Active-active |
| **Glance-API** | Image service API. Stateless (image data in Ceph). | Active-active |
| **Placement API** | Resource tracking API. Stateless. | Active-active |
| **Heat API + Engine** | Orchestration. API is stateless; engine is a worker. | API: active-active; Engine: active-active (RPC consumers) |
| **Barbican** | Secret management. API is stateless. | Active-active |
| **RabbitMQ** | AMQP 0-9-1 message broker. All inter-service RPC. | Cluster mode with quorum queues; survives minority failure |
| **MySQL/Galera** | Relational database for all services. Synchronous multi-master replication. | Galera cluster (3 or 5 nodes); all nodes accept reads/writes; certification-based replication |
| **Memcached** | In-memory cache. Keystone token cache, API response cache. | Distributed hash ring (client-side sharding); each node independent; no replication |
| **Pacemaker/Corosync** | Cluster resource manager. Manages VIPs, fencing (STONITH), service restart. | Quorum-based; manages active-passive resources |

### Data Flows

```
API Request Flow (e.g., openstack server create):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. Client → External LB → HAProxy VIP → one of 3 HAProxy instances
2. HAProxy → nova-api (round-robin, health-checked)
3. nova-api:
   a. keystonemiddleware validates Fernet token (LOCAL, < 5ms)
   b. oslo.policy checks RBAC (in-memory rules)
   c. Request validated (flavor, image, network exist)
   d. Quota checked (DB query via oslo.db)
   e. Instance record created in nova_api DB (Galera write)
   f. RPC cast to nova-conductor via RabbitMQ

4. RabbitMQ routes message to nova-conductor consumer
5. nova-conductor:
   a. RPC call to nova-scheduler via RabbitMQ
   b. Scheduler queries Placement API (via HTTP)
   c. Placement queries placement DB (Galera read)
   d. Scheduler returns selected host
   e. Conductor writes to cell DB (Galera write)
   f. RPC cast to nova-compute.<host> via cell RabbitMQ

6. nova-compute (on compute host):
   a. Calls Glance API to download image (via HTTP)
   b. Calls Neutron API to create/bind port (via HTTP)
   c. Creates VM via libvirt
   d. Updates instance state via RPC to conductor → cell DB write

Every step involves: HAProxy → API → DB read/write → RabbitMQ → worker
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Control Plane Infrastructure Database (separate from OpenStack service DBs)
-- This tracks the control plane itself for operational management

CREATE TABLE controller_nodes (
    id              VARCHAR(36) PRIMARY KEY,
    hostname        VARCHAR(255) NOT NULL UNIQUE,
    ip_address      VARCHAR(45) NOT NULL,
    role            SET('api', 'db', 'mq', 'worker') NOT NULL,
    status          ENUM('active', 'maintenance', 'failed') DEFAULT 'active',
    galera_state    ENUM('synced', 'donor', 'joiner', 'desynced', 'unknown') DEFAULT 'unknown',
    rabbitmq_state  ENUM('running', 'stopped', 'partitioned', 'unknown') DEFAULT 'unknown',
    last_heartbeat  DATETIME,
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    INDEX idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE service_instances (
    id              VARCHAR(36) PRIMARY KEY,
    service_name    VARCHAR(255) NOT NULL,     -- 'nova-api', 'keystone', 'neutron-server'
    controller_id   VARCHAR(36) NOT NULL,
    pid             INT,
    port            INT NOT NULL,
    status          ENUM('running', 'stopped', 'error') DEFAULT 'running',
    version         VARCHAR(50),
    workers         INT,                        -- number of WSGI workers
    last_health_check DATETIME,
    created_at      DATETIME NOT NULL,
    FOREIGN KEY (controller_id) REFERENCES controller_nodes(id),
    INDEX idx_service (service_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE vip_addresses (
    id              VARCHAR(36) PRIMARY KEY,
    service_name    VARCHAR(255) NOT NULL,     -- 'keystone', 'nova', etc.
    vip_address     VARCHAR(45) NOT NULL,
    vip_port        INT NOT NULL,
    active_controller_id VARCHAR(36),
    protocol        ENUM('http', 'https') DEFAULT 'https',
    created_at      DATETIME NOT NULL,
    FOREIGN KEY (active_controller_id) REFERENCES controller_nodes(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE haproxy_backends (
    id              VARCHAR(36) PRIMARY KEY,
    service_name    VARCHAR(255) NOT NULL,
    backend_address VARCHAR(45) NOT NULL,
    backend_port    INT NOT NULL,
    weight          INT DEFAULT 100,
    health_status   ENUM('UP', 'DOWN', 'MAINT') DEFAULT 'UP',
    active_connections INT DEFAULT 0,
    total_requests  BIGINT DEFAULT 0,
    last_check_at   DATETIME,
    INDEX idx_service (service_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- RabbitMQ Queue Monitoring

CREATE TABLE rabbitmq_queue_stats (
    id              VARCHAR(36) PRIMARY KEY,
    queue_name      VARCHAR(255) NOT NULL,      -- 'conductor', 'scheduler', 'compute.host-001'
    vhost           VARCHAR(255) DEFAULT '/',
    messages_ready  INT DEFAULT 0,
    messages_unacked INT DEFAULT 0,
    consumers       INT DEFAULT 0,
    publish_rate    FLOAT DEFAULT 0,
    deliver_rate    FLOAT DEFAULT 0,
    sampled_at      DATETIME NOT NULL,
    INDEX idx_queue (queue_name),
    INDEX idx_sampled (sampled_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Galera Cluster Monitoring

CREATE TABLE galera_cluster_stats (
    id                  VARCHAR(36) PRIMARY KEY,
    node_hostname       VARCHAR(255) NOT NULL,
    wsrep_cluster_size  INT NOT NULL,
    wsrep_cluster_status VARCHAR(50),            -- 'Primary', 'Non-primary', 'Disconnected'
    wsrep_ready         BOOLEAN,
    wsrep_connected     BOOLEAN,
    wsrep_local_state   INT,                     -- 4=Synced, 2=Donor, 1=Joining
    wsrep_flow_control_paused FLOAT,             -- 0.0-1.0, fraction of time paused
    wsrep_cert_failures BIGINT,
    sampled_at          DATETIME NOT NULL,
    INDEX idx_node (node_hostname),
    INDEX idx_sampled (sampled_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Database Selection

| Database | Justification |
|----------|--------------|
| **MySQL 8.0 + Galera Cluster** | Industry standard for OpenStack. Galera provides synchronous multi-master replication. All services use SQLAlchemy + oslo.db. Galera's certification-based replication ensures consistency — a write on node 1 is visible on node 2 within milliseconds. |
| **Separate DB per service** | Each OpenStack service has its own database (keystone, nova_api, nova_cell1, neutron, cinder, glance, heat, barbican, placement). This isolates failures and allows independent scaling. All DBs on the same Galera cluster, or split across multiple clusters for large deployments. |

### Indexing Strategy

Note: each OpenStack service manages its own indexes (defined in SQLAlchemy models). The control plane infrastructure DB above uses minimal indexes for monitoring queries. For service-specific indexes, see the Nova and Neutron design files.

---

## 5. API Design

### REST Endpoints (OpenStack API Surface)

```
# Identity (Keystone) - port 5000
POST   /v3/auth/tokens                    # Authenticate, get token
GET    /v3/auth/catalog                    # Service catalog
GET    /v3/projects                        # List projects
POST   /v3/projects                        # Create project
GET    /v3/users                           # List users
POST   /v3/users                           # Create user
GET    /v3/roles                           # List roles
PUT    /v3/projects/{p}/users/{u}/roles/{r} # Assign role

# Compute (Nova) - port 8774
POST   /v2.1/servers                       # Create VM
GET    /v2.1/servers                       # List VMs
GET    /v2.1/flavors                       # List flavors
POST   /v2.1/servers/{id}/action           # VM actions (start, stop, migrate)

# Network (Neutron) - port 9696
POST   /v2.0/networks                      # Create network
POST   /v2.0/subnets                       # Create subnet
POST   /v2.0/ports                         # Create port
POST   /v2.0/routers                       # Create router
POST   /v2.0/floatingips                   # Create floating IP
POST   /v2.0/security-groups               # Create security group

# Block Storage (Cinder) - port 8776
POST   /v3/{project_id}/volumes            # Create volume
GET    /v3/{project_id}/volumes            # List volumes
POST   /v3/{project_id}/volumes/{id}/action # Volume actions (attach, snapshot)

# Image (Glance) - port 9292
POST   /v2/images                          # Create image metadata
PUT    /v2/images/{id}/file                # Upload image data
GET    /v2/images                          # List images

# Placement - port 8778
GET    /placement/resource_providers       # List resource providers
GET    /placement/allocation_candidates    # Get scheduling candidates

# Orchestration (Heat) - port 8004
POST   /v1/{project_id}/stacks            # Create stack
GET    /v1/{project_id}/stacks            # List stacks
DELETE /v1/{project_id}/stacks/{name}/{id} # Delete stack

# Secrets (Barbican) - port 9311
POST   /v1/secrets                         # Create secret
GET    /v1/secrets                         # List secrets
GET    /v1/secrets/{id}/payload            # Retrieve secret value

# HAProxy Stats (admin) - port 9000
GET    /stats                              # HAProxy statistics page
GET    /stats;csv                          # CSV format for monitoring
```

### CLI (openstack CLI commands)

```bash
# Control plane health checks
openstack endpoint list                            # Verify all endpoints registered
openstack compute service list                     # Nova service status
openstack network agent list                       # Neutron agent status
openstack volume service list                      # Cinder service status

# Token operations
openstack token issue                              # Get a new token
openstack token revoke <token-id>                  # Revoke a token
openstack catalog list                             # Service catalog

# Projects and users
openstack project create --domain default myproject
openstack user create --project myproject --password changeme myuser
openstack role add --project myproject --user myuser member

# Quotas
openstack quota show myproject
openstack quota set --instances 100 --cores 400 --ram 819200 myproject

# Heat orchestration
openstack stack create -t my-stack.yaml -e my-env.yaml my-stack
openstack stack list
openstack stack show my-stack
openstack stack delete my-stack

# Barbican secrets
openstack secret store --name db-password --payload "s3cr3t"
openstack secret get <secret-ref> --payload
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: RabbitMQ — Message Bus Architecture

**Why it's hard:**
RabbitMQ carries ALL inter-service communication in OpenStack. Every VM create, every migration, every network port binding triggers RPC messages. At 10,000 messages/second with strict delivery guarantees, RabbitMQ must be both fast and reliable. Message loss means orphaned operations; message duplication means duplicate resources. Network partitions between RabbitMQ nodes cause split-brain. Queue depth growth indicates a downstream bottleneck that can cascade into control plane failure.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **RabbitMQ with classic mirrored queues** | Proven; simple configuration | Deprecated in 3.8+; poor performance; split-brain prone; not recommended |
| **RabbitMQ with quorum queues (Raft)** | Raft consensus prevents split-brain; durable; automatic leader election | Higher memory usage; slightly higher latency; requires 3+ nodes |
| **Apache Kafka** | High throughput; native partitioning; log-based (replay) | Not AMQP compatible; oslo.messaging has limited Kafka support; operational complexity |
| **ZeroMQ (zmq)** | Zero-hop; no broker; low latency | No message persistence; no routing; harder to manage; limited community support |
| **Redis Streams** | Simple; fast; persistence | Not AMQP compatible; limited routing; not designed for RPC patterns |

**Selected approach:** RabbitMQ 3.13+ with quorum queues

**Justification:** Quorum queues use Raft consensus for replication, eliminating the split-brain issues of classic mirrored queues. oslo.messaging (OpenStack's messaging abstraction) has first-class RabbitMQ support. Quorum queues are the recommended configuration since RabbitMQ 3.8.

**Implementation detail:**

```
RabbitMQ Cluster Topology:
━━━━━━━━━━━━━━━━━━━━━━━━

  Controller-1          Controller-2          Controller-3
  ┌─────────────┐      ┌─────────────┐      ┌─────────────┐
  │ RabbitMQ    │      │ RabbitMQ    │      │ RabbitMQ    │
  │ Node        │◄────►│ Node        │◄────►│ Node        │
  │             │      │ (leader for │      │             │
  │             │      │  some queues)│      │             │
  └─────────────┘      └─────────────┘      └─────────────┘

  Quorum queue replication:
  - Each queue has a leader and 2 followers (Raft)
  - Writes go to the leader, replicated to followers before ack
  - If leader dies, a follower is elected leader (< 10s)
  - Survives 1 node failure (2 of 3 = quorum)

Exchange and Queue Layout:
━━━━━━━━━━━━━━━━━━━━━━━━━

Exchange: nova (topic exchange)
  Routing keys:
  ├── conductor → Queue: conductor (consumed by nova-conductor instances)
  ├── scheduler → Queue: scheduler (consumed by nova-scheduler instances)
  ├── compute.host-001 → Queue: compute.host-001 (consumed by nova-compute on host-001)
  ├── compute.host-002 → Queue: compute.host-002
  └── ... (one queue per compute host)

Exchange: neutron (topic exchange)
  Routing keys:
  ├── q-plugin → Queue: q-plugin (consumed by neutron-server ML2 plugin)
  ├── dhcp_agent.host-net-001 → Queue: dhcp_agent.host-net-001
  ├── l3_agent.host-net-001 → Queue: l3_agent.host-net-001
  └── q-agent-notifier-port-update_fanout → Fanout to all OVS agents

Exchange: cinder (topic exchange)
  Routing keys:
  ├── cinder-scheduler → Queue: cinder-scheduler
  ├── cinder-volume.host-storage-001 → Queue: per-volume-host
  └── cinder-backup → Queue: cinder-backup

RPC Patterns:
━━━━━━━━━━━━

1. RPC Call (synchronous):
   nova-conductor → Exchange: nova → Key: scheduler → Queue: scheduler
   nova-scheduler receives, processes, sends reply to reply queue
   nova-conductor waits for reply (timeout: 60s default)

2. RPC Cast (asynchronous, fire-and-forget):
   nova-api → Exchange: nova → Key: conductor → Queue: conductor
   nova-conductor receives and processes (no reply)

3. Fanout (broadcast):
   neutron-server → Fanout exchange → All OVS agents receive

oslo.messaging Configuration:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━

[oslo_messaging_rabbit]
rabbit_hosts = controller-1:5672,controller-2:5672,controller-3:5672
rabbit_ha_queues = true           # Use quorum queues
rabbit_retry_interval = 1
rabbit_retry_backoff = 2
rabbit_max_retries = 0            # Retry forever
heartbeat_timeout_threshold = 60
heartbeat_rate = 2

[DEFAULT]
transport_url = rabbit://openstack:password@controller-1:5672,controller-2:5672,controller-3:5672//
rpc_response_timeout = 60
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Single RabbitMQ node failure | Queue leaders on that node failover (< 10s); brief message delivery pause | Quorum queues auto-elect new leader; oslo.messaging retries automatically |
| RabbitMQ network partition | Split-brain: each partition has subset of queues; message loss possible | Quorum queues prevent split-brain (Raft requires majority); partition handling mode = pause_minority |
| Queue depth growing unbounded | Memory exhaustion on RabbitMQ node; message backlog causes latency | Monitor queue depth; set per-queue memory limits (x-max-in-memory-length); investigate slow consumers |
| RabbitMQ OOM kill | All message delivery stops; cascading failure | Set vm_memory_high_watermark = 0.4; monitor memory; add memory to controller nodes |
| Erlang cookie mismatch (after reinstall) | Node can't join cluster | Ensure cookie consistent across all nodes; ansible manages this |
| File descriptor exhaustion | New connections refused | Set ulimit -n 65536; monitor fd usage; tune connection limits |

**Interviewer Q&As:**

**Q1: Why does OpenStack use RabbitMQ instead of Kafka?**
A: OpenStack's messaging pattern is RPC (request-reply), not event streaming. oslo.messaging uses AMQP 0-9-1 topic exchanges for routing RPC calls to specific services and hosts. RabbitMQ natively supports this pattern with per-consumer queues and reply queues. Kafka is optimized for high-throughput append-only event logs and consumer groups, which is a different pattern. While oslo.messaging has experimental Kafka support (via confluent_kafka), it's not production-proven and doesn't support RPC call (synchronous reply) natively. Kafka would be better for event streaming (telemetry, audit logs) but not for RPC.

**Q2: How do quorum queues prevent split-brain?**
A: Quorum queues use the Raft consensus protocol. Each queue has a leader and N-1 followers (typically 2 followers with 3 nodes). Writes require acknowledgment from a majority (2 of 3). During a network partition, at most one partition has a majority — only that partition can accept writes. The minority partition's queue becomes unavailable (read-only or offline). This prevents both partitions from accepting writes and diverging. When the partition heals, the minority nodes rejoin and catch up from the leader's log.

**Q3: What happens when a compute host's RabbitMQ queue depth grows?**
A: Each nova-compute has a dedicated queue (e.g., `compute.host-001`). If the host is slow (e.g., busy spawning VMs), messages accumulate. Impact: new operations to that host are delayed. At extreme depth (> 10,000 messages), RabbitMQ memory pressure increases. Detection: monitor `messages_ready` per queue. Mitigation: (1) Rate-limit VM builds per host (scheduler can enforce max_io_ops_per_host). (2) Investigate slow host (disk I/O, CPU saturation). (3) If host is unresponsive, disable in scheduler and drain queue. (4) Set queue TTL (x-message-ttl) to expire stale messages.

**Q4: How do you upgrade RabbitMQ in a running cluster?**
A: Rolling upgrade: (1) Drain node 1 (`rabbitmqctl stop_app`). Queue leaders migrate to other nodes. (2) Upgrade RabbitMQ packages on node 1. (3) Start node 1 (`rabbitmqctl start_app`). It rejoins the cluster and syncs. (4) Repeat for nodes 2, 3. Throughout, the cluster maintains quorum (2 of 3 nodes up). oslo.messaging reconnects automatically to remaining nodes. Zero message loss with quorum queues. Best practice: upgrade during low-activity periods. Verify RabbitMQ version compatibility (all nodes must be within 1 minor version during rolling upgrade).

**Q5: How do you size RabbitMQ for 10,000 compute hosts?**
A: Each compute host creates ~3 queues (compute, compute.host-xxx, and potentially cell-specific). With 10,000 hosts across 3 cells, that's ~10,000 queues per cell's RabbitMQ cluster. Each queue consumes ~30 KB memory overhead. 10,000 queues x 30 KB = 300 MB — manageable. Message rate: ~3,000 msg/s per cell (resource updates + builds + migrations). RabbitMQ handles this easily with 3 nodes. Bottleneck: Erlang scheduler throughput. With 16 CPU cores per RabbitMQ node, expect ~50,000 msg/s. Rule of thumb: one RabbitMQ cluster per cell; 3-5 nodes per cluster; 64 GB RAM per node.

**Q6: How does oslo.messaging handle RabbitMQ connection failures?**
A: oslo.messaging's RabbitMQ driver uses kombu (Python AMQP library) with automatic reconnection. Configuration: `rabbit_retry_interval` (initial retry wait, default 1s), `rabbit_retry_backoff` (exponential backoff multiplier, default 2), `rabbit_max_retries` (0 = retry forever). When the connection drops: (1) kombu detects via heartbeat timeout (heartbeat_timeout_threshold). (2) Attempts reconnection to the same or another node in the rabbit_hosts list. (3) During reconnection, RPC calls queue locally and are sent after reconnection (for cast) or timeout with an error (for call). (4) Quorum queues preserve messages during the outage.

---

### Deep Dive 2: MySQL/Galera — Distributed Database

**Why it's hard:**
Every OpenStack API call ultimately reads from or writes to MySQL. At 5,000 TPS across all services, the database must handle high throughput, provide strict consistency (you don't want to double-schedule a VM due to stale reads), remain available during single-node failures, and support rolling upgrades. Galera's synchronous replication adds write latency and creates flow control pressure under high write loads.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **MySQL single instance + async replica** | Simple; fast writes; read scaling via replica | Single point of failure for writes; async replica has data loss risk; manual failover |
| **MySQL Group Replication** | MySQL-native HA; single-primary or multi-primary; Paxos-based | Less mature for OpenStack than Galera; certification conflict handling differs |
| **MySQL + Galera Cluster** | Synchronous multi-master; zero data loss; automatic failover; proven for OpenStack | Write amplification (all nodes certify every write); flow control under high write load; deadlock on high contention |
| **PostgreSQL + Patroni** | Strong consistency; advanced features (JSONB, CTEs); Patroni for HA | OpenStack support for PostgreSQL has declined; some services have MySQL-specific code; migration effort |
| **CockroachDB / TiDB** | Distributed SQL; automatic sharding; geo-replication | Not supported by oslo.db; significant migration effort; operational unfamiliarity |

**Selected approach:** MySQL 8.0 + Galera Cluster (3 or 5 nodes)

**Justification:** Galera is the industry standard for OpenStack. Every deployment guide, every operations team, every troubleshooting document assumes Galera. oslo.db and SQLAlchemy have been tested extensively with Galera. The synchronous replication guarantee (zero data loss on node failure) is critical for OpenStack's correctness.

**Implementation detail:**

```
Galera Cluster Architecture:
━━━━━━━━━━━━━━━━━━━━━━━━━━

  Controller-1         Controller-2         Controller-3
  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
  │ MySQL 8.0   │     │ MySQL 8.0   │     │ MySQL 8.0   │
  │  + Galera   │     │  + Galera   │     │  + Galera   │
  │             │     │             │     │             │
  │ Databases:  │     │ Databases:  │     │ Databases:  │
  │  keystone   │◄───►│  keystone   │◄───►│  keystone   │
  │  nova_api   │     │  nova_api   │     │  nova_api   │
  │  nova_cell0 │     │  nova_cell0 │     │  nova_cell0 │
  │  nova_cell1 │     │  nova_cell1 │     │  nova_cell1 │
  │  neutron    │     │  neutron    │     │  neutron    │
  │  cinder     │     │  cinder     │     │  cinder     │
  │  glance     │     │  glance     │     │  glance     │
  │  heat       │     │  heat       │     │  heat       │
  │  barbican   │     │  barbican   │     │  barbican   │
  │  placement  │     │  placement  │     │  placement  │
  └─────────────┘     └─────────────┘     └─────────────┘
        ▲                   ▲                   ▲
        │                   │                   │
        └───────── IST (Incremental State Transfer) ───────┘
                   or SST (full sync on rejoin)

Write Path (Galera certification-based replication):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. Client writes to Node-1:
   INSERT INTO instances (uuid, ...) VALUES ('abc-123', ...)

2. Node-1 executes locally (optimistic execution)

3. At COMMIT time, Node-1 generates a "write set":
   - Transaction's row modifications (primary key + changed columns)
   - Dependency information (rows read during transaction)

4. Write set broadcast to ALL nodes via group communication (gcomm)

5. Each node performs "certification":
   - Check if any OTHER transaction has modified the same rows
   - If no conflict → certified (apply the write set)
   - If conflict → abort (the conflicting transaction on that node fails)

6. All nodes have the same data after certification
   Total write latency: local execution + network broadcast + certification
   Typically: 5-20ms per write (depending on network latency between nodes)

Key Configuration:
━━━━━━━━━━━━━━━━━

[mysqld]
wsrep_cluster_name = openstack-galera
wsrep_cluster_address = gcomm://controller-1,controller-2,controller-3
wsrep_provider = /usr/lib/galera/libgalera_smm.so
wsrep_sst_method = mariabackup          # Full state transfer method
wsrep_slave_threads = 16                 # Parallel apply threads
wsrep_certify_nonPK = ON                 # Certify tables without PK (rare)
wsrep_retry_autocommit = 3               # Auto-retry on certification failure

# Performance tuning
innodb_buffer_pool_size = 64G            # 25% of total RAM
innodb_log_file_size = 1G
innodb_flush_log_at_trx_commit = 2       # Galera handles durability
innodb_autoinc_lock_mode = 2             # Interleaved (required for Galera)
max_connections = 2000
thread_pool_size = 32

# Galera flow control
wsrep_provider_options = "gcache.size=2G; gcs.fc_limit=256; gcs.fc_factor=0.8"
# gc_cache.size: amount of write sets kept for IST (incremental state transfer)
# gcs.fc_limit: queue depth before triggering flow control
# gcs.fc_factor: resume replication at this fraction of fc_limit

Connection Pooling (oslo.db):
━━━━━━━━━━━━━━━━━━━━━━━━━━━

[database]
connection = mysql+pymysql://nova:password@controller-vip/nova_cell1
max_pool_size = 20          # Connections per process
max_overflow = 50           # Burst connections
pool_timeout = 30           # Wait for connection from pool
connection_recycle_time = 3600  # Recycle connections hourly
# With 3 nova-api workers, each with 4 sub-processes:
# Max connections = 3 * 4 * (20 + 50) = 840 per service
# Total across all services: ~5000 connections to Galera
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Single Galera node failure | Cluster continues with 2 of 3 nodes; slight write latency increase | Auto-eviction; monitor wsrep_cluster_size; replace failed node |
| Galera quorum loss (2 of 3 fail) | All writes fail; remaining node read-only | Bootstrap from surviving node; this is a critical failure — page immediately |
| Certification conflict (deadlock) | Individual transaction fails with error | oslo.db retries (db_max_retries=20, db_retry_interval=0.1); application-level retry |
| Slow node (flow control) | ALL writes paused cluster-wide until slow node catches up | Monitor wsrep_flow_control_paused; investigate slow node (disk I/O, CPU); evict if persistent |
| SST (full state transfer) during rejoin | Donor node under high I/O load; donor may be slow for API requests | Use IST (incremental) when possible; schedule SST during maintenance; use dedicated donor |
| max_connections exceeded | New API connections refused; 503 errors | Monitor connections; tune pool sizes; add controller nodes; use ProxySQL for connection multiplexing |
| Large transaction (e.g., bulk delete) | Replication lag; certification timeout | Break large operations into smaller batches; `nova-manage db archive_deleted_rows --max_rows 1000` |

**Interviewer Q&As:**

**Q1: Why Galera over MySQL Group Replication?**
A: Galera (via MariaDB or Percona XtraDB Cluster) has been the standard for OpenStack since ~2013. Reasons: (1) Multi-master by default — any node accepts writes (Group Replication defaults to single-primary). (2) Certification-based replication is simpler to reason about than Paxos. (3) Extensive community experience — every OpenStack deployment guide uses Galera. (4) Battle-tested at scale by major OpenStack operators (Rackspace, OVHcloud, CERN). MySQL Group Replication is catching up and is a viable alternative for new deployments, but the operational tooling and knowledge base for Galera is much larger.

**Q2: How do you handle Galera certification conflicts?**
A: Certification conflicts occur when two nodes simultaneously modify the same row. The transaction that commits first wins; the other fails with ERROR 1213 (Deadlock). oslo.db handles this automatically: `db_max_retries = 20` with `db_retry_interval = 0.1s`. The retry picks up the new state and usually succeeds. To reduce conflicts: (1) Write to a single node per service (HAProxy "stick-table" for write routing). (2) Avoid large transactions. (3) Use optimistic locking (version columns) in application logic. In practice, certification failures are < 0.1% of transactions in a well-configured deployment.

**Q3: What happens during a Galera SST (State Snapshot Transfer)?**
A: SST is a full database copy from a running node (donor) to a rejoining node (joiner). This happens when a node has been down too long for IST (Incremental State Transfer — replaying cached write sets). SST methods: (1) `mariabackup` (recommended): takes a hot backup of the donor with minimal locking. (2) `rsync`: copies data files; donor is briefly read-only. During SST: the donor node has increased I/O load (it's copying the entire database). The joiner is unavailable until SST completes. For a 40 GB database, SST takes ~5 minutes with NVMe SSDs. Mitigation: increase `gcache.size` to keep more write sets in memory (allows IST instead of SST for longer outages).

**Q4: How do you monitor Galera cluster health?**
A: Key metrics: (1) `wsrep_cluster_size`: must equal expected node count. (2) `wsrep_cluster_status`: must be "Primary" (has quorum). (3) `wsrep_ready`: must be ON (node can accept queries). (4) `wsrep_local_state`: 4 = Synced (healthy), 2 = Donor, 1 = Joining. (5) `wsrep_flow_control_paused`: fraction of time writes are paused (should be < 0.01). (6) `wsrep_local_recv_queue_avg`: incoming replication queue (should be < 1.0). (7) `wsrep_cert_failures`: certification conflict rate. Export these via `mysqld_exporter` to Prometheus. Alert on: cluster_size < expected, flow_control_paused > 0.01, local_state != 4.

**Q5: How do you handle database schema migrations during rolling upgrades?**
A: OpenStack uses Alembic (via oslo.db) for migrations. Three phases: (1) **Expand**: add new columns, tables, indexes. These are backward-compatible — old code ignores new columns. Run before upgrading services: `nova-manage db sync`. (2) **Data migration**: backfill new columns from old data. Run online (no downtime): `nova-manage db online_data_migrations`. (3) **Contract**: drop old columns, remove deprecated tables. Run only AFTER all services are upgraded: `nova-manage db sync --contract`. Galera replicates DDL (schema changes) synchronously — all nodes get the new schema simultaneously. The expand-contract pattern ensures no service is broken during the upgrade window.

**Q6: At what point should you split services into separate Galera clusters?**
A: When a single Galera cluster can't handle the combined write load. Indicators: (1) wsrep_flow_control_paused > 0.05 (5% of time). (2) Query latency P99 > 100ms. (3) max_connections exhausted. Typical split: (1) Keystone + Nova API + Placement on cluster A. (2) Nova cell DBs on cluster B (or separate cluster per cell). (3) Neutron on cluster C. (4) Cinder + Glance + Heat + Barbican on cluster D. This reduces certification contention (different services rarely conflict) and allows independent scaling. The trade-off: more clusters to manage, more hardware, more monitoring.

---

### Deep Dive 3: HAProxy and API Load Balancing

**Why it's hard:**
HAProxy is the single entry point for ALL OpenStack API traffic. It must handle 10,000+ concurrent connections, route to healthy backends, provide SSL termination, enforce rate limits, and fail over seamlessly when a backend or HAProxy instance itself fails. Misconfiguration can take down the entire control plane.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **HAProxy + keepalived (VIP)** | Simple; proven; low latency; L4/L7 capable | Active-passive for VIP; single active HAProxy handles all traffic |
| **Multiple HAProxy with external LB** | All HAProxy instances active; external LB distributes | Additional component (external LB); dependency on external LB |
| **Envoy proxy** | Modern; gRPC support; dynamic configuration; xDS API | Overkill for OpenStack's REST APIs; less operational experience in OpenStack community |
| **Nginx** | High performance; widely used | Less feature-rich than HAProxy for health checking and stats; requires Nginx Plus for some features |
| **DNS round-robin** | No load balancer needed; simple | No health checking; slow failover (DNS TTL); no connection draining |

**Selected approach:** HAProxy + keepalived for VIP, with option for external LB in front

**Implementation detail:**

```
HAProxy Configuration (key sections):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

global
  maxconn 10000
  log /dev/log local0
  stats socket /var/run/haproxy.sock mode 660 level admin
  ssl-default-bind-ciphers ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384
  ssl-default-bind-options ssl-min-ver TLSv1.2

defaults
  mode http
  timeout connect 5s
  timeout client 30s
  timeout server 60s
  timeout queue 30s
  option httplog
  option dontlognull
  option http-server-close
  option forwardfor
  retries 3
  option redispatch           # Retry on different backend if first fails

# Keystone (Identity)
frontend keystone_public
  bind *:5000 ssl crt /etc/haproxy/certs/keystone.pem
  default_backend keystone_public_backend
  # Rate limiting: 100 requests/sec per source IP
  stick-table type ip size 100k expire 30s store http_req_rate(10s)
  http-request deny deny_status 429 if { sc0_http_req_rate gt 100 }

backend keystone_public_backend
  balance roundrobin
  option httpchk GET /v3
  http-check expect status 200
  server controller-1 10.0.0.1:5000 check inter 5s rise 2 fall 3
  server controller-2 10.0.0.2:5000 check inter 5s rise 2 fall 3
  server controller-3 10.0.0.3:5000 check inter 5s rise 2 fall 3

# Nova (Compute)
frontend nova_api
  bind *:8774 ssl crt /etc/haproxy/certs/nova.pem
  default_backend nova_api_backend

backend nova_api_backend
  balance leastconn           # Better for mixed fast/slow requests
  option httpchk GET /
  http-check expect status 200
  server controller-1 10.0.0.1:8774 check inter 5s rise 2 fall 3
  server controller-2 10.0.0.2:8774 check inter 5s rise 2 fall 3
  server controller-3 10.0.0.3:8774 check inter 5s rise 2 fall 3

# Neutron (Networking)
frontend neutron_api
  bind *:9696 ssl crt /etc/haproxy/certs/neutron.pem
  default_backend neutron_api_backend

backend neutron_api_backend
  balance leastconn
  option httpchk GET /v2.0
  http-check expect status 200
  server controller-1 10.0.0.1:9696 check inter 5s rise 2 fall 3
  server controller-2 10.0.0.2:9696 check inter 5s rise 2 fall 3
  server controller-3 10.0.0.3:9696 check inter 5s rise 2 fall 3

# Galera (MySQL) - L4 TCP mode
frontend galera_db
  bind *:3306
  mode tcp
  default_backend galera_db_backend

backend galera_db_backend
  mode tcp
  balance leastconn
  option mysql-check user haproxy_check
  server controller-1 10.0.0.1:3306 check inter 5s rise 2 fall 3
  server controller-2 10.0.0.2:3306 check inter 5s rise 2 fall 3 backup
  server controller-3 10.0.0.3:3306 check inter 5s rise 2 fall 3 backup
  # Only one active writer to reduce Galera certification conflicts

# RabbitMQ (AMQP) - L4 TCP mode
frontend rabbitmq
  bind *:5672
  mode tcp
  default_backend rabbitmq_backend

backend rabbitmq_backend
  mode tcp
  balance roundrobin
  server controller-1 10.0.0.1:5672 check inter 5s rise 2 fall 3
  server controller-2 10.0.0.2:5672 check inter 5s rise 2 fall 3
  server controller-3 10.0.0.3:5672 check inter 5s rise 2 fall 3

# Stats page
listen stats
  bind *:9000
  stats enable
  stats uri /stats
  stats refresh 10s
  stats admin if TRUE

keepalived Configuration:
━━━━━━━━━━━━━━━━━━━━━━━━

# Controller-1 (MASTER)
vrrp_instance VI_1 {
  interface eth0
  virtual_router_id 51
  priority 100
  advert_int 1
  authentication {
    auth_type PASS
    auth_pass secret123
  }
  virtual_ipaddress {
    10.0.0.100/24     # VIP for all API services
  }
  track_script {
    chk_haproxy
  }
}

vrrp_script chk_haproxy {
  script "pidof haproxy"
  interval 2
  weight -20
}
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Single backend failure (one nova-api) | HAProxy health check detects in 15s; routes to remaining backends | Health check: `fall 3` (3 consecutive failures) x `inter 5s` = 15s detection |
| Active HAProxy failure | VIP migrates to standby via keepalived VRRP (< 3s) | keepalived on all controllers; track_script monitors HAProxy process |
| SSL certificate expiry | All API calls fail with TLS error | Certificate monitoring alert at 30 days; automated renewal (certbot/Vault PKI) |
| Connection limit reached | New connections refused (503) | Monitor haproxy_current_sessions; increase maxconn; add controller nodes |
| Slow backend (high latency) | Requests queue; timeout errors | `timeout server 60s` prevents indefinite wait; `option redispatch` retries on other backends; circuit breaker pattern |
| DDoS on API endpoints | Control plane overwhelmed | Rate limiting (stick-table); external DDoS protection; API request size limits |

**Interviewer Q&As:**

**Q1: Why use roundrobin vs leastconn for load balancing?**
A: `roundrobin` distributes requests evenly by count — simple and works well when all requests take similar time. `leastconn` sends each new request to the backend with the fewest active connections — better when request duration varies significantly (e.g., a server list query takes 200ms but a server create takes 2s). For Nova and Neutron API, we use `leastconn` because API operations have widely varying latencies. For Keystone, `roundrobin` works well because token operations are uniformly fast.

**Q2: How does HAProxy health checking work for OpenStack services?**
A: HAProxy sends HTTP health checks to each backend. Configuration: `option httpchk GET /v3` for Keystone (expects 200). The check runs every `inter` seconds (5s). A backend is marked DOWN after `fall` consecutive failures (3 = 15s). It's marked UP after `rise` consecutive successes (2 = 10s). OpenStack services expose a health endpoint at their root URL that returns 200 when the WSGI server is running. More advanced: check DB connectivity by using a custom health endpoint that queries the database.

**Q3: How do you handle SSL termination at scale?**
A: SSL is terminated at HAProxy. Each service has its own certificate (or a wildcard cert for *.cloud.example.com). HAProxy's OpenSSL implementation handles ~10,000 new TLS handshakes/second per core (with ECDHE-RSA-AES256-GCM). For 5,000 API requests/sec, a single core suffices for the crypto. Session resumption (TLS tickets) reduces handshake overhead for persistent connections. Certificate management: use HashiCorp Vault PKI or Let's Encrypt with automation. Internal services (RPC between API and workers) also use TLS for defense-in-depth.

**Q4: How do you implement API rate limiting?**
A: HAProxy stick-tables track per-source-IP request rates. Example: `stick-table type ip size 100k expire 30s store http_req_rate(10s)` tracks 10-second request rate per IP. `http-request deny deny_status 429 if { sc0_http_req_rate gt 100 }` denies requests exceeding 100/10s (10 req/s). For per-project rate limiting, oslo.middleware's `rate_limit` can enforce limits based on the Keystone project_id in the token. More granular: deploy an API gateway (Kong, Envoy) in front of HAProxy for sophisticated rate limiting, request transformation, and observability.

**Q5: How do you handle the "thundering herd" problem when a controller comes back online?**
A: When a controller recovers after a failure, all services restart simultaneously. OpenStack services connect to RabbitMQ and MySQL, causing a connection spike. Mitigation: (1) Staggered service startup (systemd dependencies with delays). (2) Connection retry with jitter in oslo.messaging and oslo.db. (3) HAProxy `slowstart` option: gradually increase weight of a recovering backend over 30s. (4) MySQL connection pool warm-up: oslo.db creates connections lazily, spreading the load. (5) RabbitMQ connection limits per vhost/user prevent one service from monopolizing connections.

**Q6: What is the difference between the VIP (keepalived) approach and using an external load balancer?**
A: **VIP approach**: keepalived manages a floating IP (VIP) via VRRP. Only one HAProxy instance is active (holds the VIP). If it fails, the VIP moves to another controller (< 3s). Advantage: simple, no external dependency. Disadvantage: only one HAProxy handles all traffic (active-passive). **External LB**: a hardware or software LB (F5, MetalLB, cloud LB) distributes traffic to ALL HAProxy instances. All are active (active-active). Advantage: higher throughput, no single HAProxy bottleneck. Disadvantage: additional component to manage. Recommendation: VIP for small/medium deployments (< 5000 API req/s); external LB for large deployments.

---

### Deep Dive 4: Keystone — Identity and Token Management

**Why it's hard:**
Keystone is on the critical path of every single API request. Every request includes a token that must be validated. Token issuance must be fast (< 100ms). Token validation must be faster (< 5ms). The system must handle 1,000 auth requests/sec, support 50,000 users, manage 10,000 projects, and enforce RBAC policies — all without becoming a bottleneck. Fernet tokens are stateless but require key synchronization.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **UUID tokens (legacy)** | Simple random string | Every validation requires DB lookup (slow); token table grows large; revocation list grows |
| **PKI/PKIZ tokens (deprecated)** | Self-contained; cryptographically signed | Very large (2-5 KB); header size limits; CRL management complex |
| **Fernet tokens (current standard)** | Self-contained; small (~200 bytes); validated locally without DB; no revocation list | Must distribute encryption keys to all services; key rotation required; no explicit revocation |
| **JWT tokens (newer alternative)** | Standard format (RFC 7519); self-contained; widely supported tooling | Larger than Fernet; requires RSA/EC key management; OpenStack adoption still evolving |

**Selected approach:** Fernet tokens

**Justification:** Fernet tokens are validated locally by every service (keystonemiddleware decrypts the token using the Fernet key). No network call to Keystone, no database lookup. This eliminates Keystone as a runtime dependency for token validation. Token size is small (~200 bytes), fitting easily in HTTP headers.

**Implementation detail:**

```
Fernet Token Structure:
━━━━━━━━━━━━━━━━━━━━━

  Fernet token = Version || Timestamp || IV || Ciphertext || HMAC
  
  Ciphertext contains (after decryption):
    - Version: token format version
    - User ID: 16 bytes (UUID compressed)
    - Methods: authentication methods used (password, token, etc.)
    - Project ID: 16 bytes
    - Expires At: Unix timestamp
    - Audit IDs: for token chaining
    - Trust ID (optional): for trust-based delegation
    - Domain ID (optional): for domain-scoped tokens
    - Federated info (optional): IdP, protocol, groups

  Size: ~200 bytes (much smaller than UUID's DB storage)

Key Repository:
━━━━━━━━━━━━━━

  /etc/keystone/fernet-keys/
    0   ← Staged key (will become next primary)
    1   ← Secondary key (validates old tokens)
    2   ← Secondary key
    3   ← Primary key (signs new tokens)

  Key rotation (keystone-manage fernet_rotate):
    1. New staged key created (random 256-bit)
    2. Current staged (0) promoted to secondary
    3. Current primary remains primary
    4. Oldest secondary deleted (if > max_active_keys)

  Rotation schedule: every 24 hours (cron job)
  max_active_keys: default 3 (supports 72h token validity)
  Token expiry: 3600s (1 hour) default; configurable

Token Validation (keystonemiddleware):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Every OpenStack service runs keystonemiddleware in its WSGI pipeline.
  
  [filter:authtoken]
  paste.filter_factory = keystonemiddleware.auth_token:filter_factory
  auth_url = http://keystone-vip:5000/v3
  memcached_servers = controller-1:11211,controller-2:11211,controller-3:11211
  token_cache_time = 300
  
  Validation flow:
  1. Extract token from X-Auth-Token header
  2. Check Memcached cache: if hit, return cached token data (< 1ms)
  3. If cache miss: decrypt Fernet token locally using key repository
  4. Extract user_id, project_id, roles, expiry
  5. Verify: not expired, valid signature (HMAC)
  6. Cache result in Memcached (TTL = token_cache_time)
  7. Set request headers: X-User-Id, X-Project-Id, X-Roles
  8. Pass to service

  No network call to Keystone! Only cache lookup and local crypto.

RBAC Policy (oslo.policy):
━━━━━━━━━━━━━━━━━━━━━━━━━

  Each service has a policy file (or in-code defaults):
  
  # Nova policy examples:
  "os_compute_api:servers:create": "role:member"
  "os_compute_api:servers:create:forced_host": "role:admin"
  "os_compute_api:os-migrate-server:migrate_live": "role:admin"
  
  # Scoped token support (newer):
  "os_compute_api:servers:create": "role:member and project_id:%(project_id)s"
  "os_compute_api:os-services:update": "role:admin and system_scope:all"
  
  Policy enforcement:
  1. keystonemiddleware extracts roles from token
  2. Service checks policy: does user's roles match required roles?
  3. If match: proceed with request
  4. If no match: return HTTP 403 Forbidden
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Keystone service down | No new token issuance; existing tokens still valid locally | Multiple Keystone instances behind HAProxy; auto-restart |
| Fernet key not distributed | Remote services can't validate tokens signed with new key | Multiple key versions (3+ active); alert on key sync failure; manual key distribution fallback |
| Memcached failure | Token validation slower (Fernet decrypt instead of cache hit, still < 5ms) | Multiple Memcached nodes (client-side sharding); Fernet works without cache |
| Token expiry too short during incident | Users forced to re-authenticate frequently during outage | Increase token_expiration temporarily; use long-lived application credentials |
| RBAC policy misconfiguration | Users get unexpected 403 or overly broad access | Test policy changes in staging; use `oslopolicy-checker` to validate; audit API access |
| Keystone DB corrupted | Can't issue new tokens; can't look up projects/users | Galera cluster protects against this; backup and restore from replica |

**Interviewer Q&As:**

**Q1: How does Fernet token validation work without contacting Keystone?**
A: Fernet is a symmetric encryption scheme. The token is encrypted and signed using a shared key. Any service that has the key can decrypt and validate the token locally. The key repository (/etc/keystone/fernet-keys/) is distributed to all services (typically via configuration management like Ansible). When a service receives a token, keystonemiddleware decrypts it using the local keys, extracts the user/project/roles/expiry, and validates without any network call. The only time Keystone is contacted is for token issuance (authentication) and rare edge cases (checking if a project still exists).

**Q2: How do you handle token revocation with Fernet tokens?**
A: Fernet tokens are stateless — there's no revocation list. A valid Fernet token cannot be revoked before its expiry. Mitigation: (1) Short token lifetime (1 hour default) limits the window of a compromised token. (2) For immediate revocation needs (e.g., user terminated), rotate the Fernet keys — all outstanding tokens become invalid. (3) Keystone maintains a `revocation_events` table, and keystonemiddleware can be configured to check it (at the cost of a network call). (4) Application credentials can be deleted, preventing token refresh. In practice, the short TTL makes explicit revocation rarely needed.

**Q3: How does Keystone integrate with LDAP/Active Directory?**
A: Keystone supports multiple identity backends per domain. Configuration: the "corporate" domain uses LDAP, while the "service" domain uses SQL. `[identity] driver = ldap` for the corporate domain. Keystone maps LDAP attributes to users: `user_name_attribute = sAMAccountName`, `user_mail_attribute = mail`. Groups in LDAP map to Keystone groups. Roles are assigned to LDAP groups: `openstack role add --group ldap-engineers --project myproject member`. Passwords are validated against LDAP (Keystone never stores LDAP passwords). Limitation: LDAP users can't be created/modified via Keystone API — LDAP is read-only.

**Q4: What are application credentials, and why are they important?**
A: Application credentials are project-scoped authentication tokens that don't expire (unless manually deleted) and don't require a user password. Created by: `openstack application credential create --name my-app-cred`. They produce an ID and a secret, used for authentication: `OS_AUTH_TYPE=v3applicationcredential`. Use cases: (1) CI/CD pipelines that need long-lived auth. (2) Service-to-service authentication. (3) Heat stacks that need to make API calls. (4) Users who don't want to embed their password in scripts. Security: application credentials can be restricted to specific roles and can't create new application credentials (preventing privilege escalation).

**Q5: How do you scale Keystone for 1,000 auth requests/second?**
A: Keystone is stateless — scale by adding instances. Each instance runs as a WSGI application (under Apache httpd or uWSGI). (1) Workers: `keystone.conf [DEFAULT] public_workers = 8` (match CPU cores). (2) Instances: 3-5 Keystone instances behind HAProxy. (3) Memcached: cache validated tokens to avoid repeated Fernet decryption. (4) Database: Keystone's DB is small (~500MB); read replicas for list operations. (5) Fernet: token validation is CPU-bound (AES + HMAC) — ~100,000 validations/sec per core. At 1,000 auth/sec, a single Keystone instance with 8 workers easily handles the load. The real bottleneck is usually password hashing (bcrypt) during initial auth — tune `bcrypt_strength` (default 12, lower = faster but less secure).

**Q6: How does scoped token authorization work?**
A: Keystone supports three token scopes: (1) **Project-scoped**: token is valid for operations within a specific project. Most common for tenant operations (create VM, network). (2) **Domain-scoped**: token is valid for domain-level operations (manage projects, users within the domain). (3) **System-scoped**: token is valid for system-level operations (manage compute services, hypervisors, Galera status). Policies reference scope: `"role:admin and system_scope:all"` means the user needs admin role with a system-scoped token. This prevents a project-admin from accidentally managing system resources. Scoped tokens are the default since the Xena release.

---

## 7. Scheduling & Resource Management

### Control Plane Resource Management

```
Controller Node Sizing:
━━━━━━━━━━━━━━━━━━━━━━

Per controller (supporting ~3,300 compute hosts, 133K VMs):

  CPU:    64 cores (32 for services, 16 for MySQL, 8 for RabbitMQ, 8 for OS/overhead)
  RAM:    256 GB (64G for MySQL innodb_buffer_pool, 4G for Memcached, 
          128G for API workers, 32G for RabbitMQ, 28G for OS/overhead)
  Disk:   2x 2TB NVMe SSD (RAID1: OS + DB; separate partition for MySQL data)
  Network: 2x 25 GbE (bonded, LACP)

Service Worker Allocation (per controller):
  keystone:       8 WSGI workers
  nova-api:       16 WSGI workers
  neutron-server: 8 WSGI workers
  cinder-api:     4 WSGI workers
  glance-api:     4 WSGI workers
  placement-api:  8 WSGI workers
  heat-api:       4 WSGI workers
  barbican-api:   2 WSGI workers
  nova-scheduler: 4 processes
  nova-conductor: 8 processes
  heat-engine:    4 processes
  Total: ~70 worker processes, each consuming ~500MB-1GB RAM

Process Isolation:
  - Use systemd resource limits (MemoryMax, CPUQuota) per service
  - Consider containerization (Kolla-ansible deploys each service in Docker)
  - Monitor per-service resource usage in Prometheus
```

---

## 8. Scaling Strategy

### Scaling Dimensions

| Dimension | Strategy | Notes |
|-----------|----------|-------|
| API throughput | Add controller nodes; increase WSGI workers | Each nova-api worker handles ~100 req/s |
| DB write throughput | Split into multiple Galera clusters per service group | Reduces certification conflicts |
| DB read throughput | Add MySQL read replicas; route read queries to replicas | oslo.db supports read/write splitting (not built-in; use ProxySQL) |
| RabbitMQ throughput | Separate RabbitMQ cluster per cell | Isolates message traffic per cell |
| Memcached capacity | Add Memcached nodes (client-side consistent hashing distributes) | Each node is independent; no replication |
| Connection count | Use ProxySQL for MySQL connection multiplexing | Reduces real DB connections from 5,000 to 500 |

### Interviewer Q&As

**Q1: How do you scale the control plane from 10,000 to 50,000 compute hosts?**
A: (1) Add cells: partition 50K hosts into 10 cells of 5K each. Each cell has its own DB and RabbitMQ. (2) Scale the API layer: add controller nodes (from 5 to 10-15). (3) Separate infrastructure: dedicated DB cluster (5-node Galera), dedicated RabbitMQ cluster (5-node), dedicated API nodes. (4) Scale Placement: Placement handles queries for all cells — add instances and optionally read replicas. (5) Scale Keystone: add instances; Keystone DB load is minimal. (6) Monitoring: Thanos for long-term metric storage; dedicated monitoring infrastructure.

**Q2: What is the most common control plane bottleneck, and how do you address it?**
A: The most common bottleneck is the database — specifically, Galera certification conflicts under high concurrent write load. Symptoms: increased wsrep_flow_control_paused, oslo.db retry warnings in logs, elevated API latency. Mitigation: (1) Route writes to a single Galera node per service (reduces conflicts). (2) Split services into separate Galera clusters. (3) Add ProxySQL for connection pooling and query routing. (4) Optimize slow queries (enable slow_query_log, analyze with pt-query-digest). (5) Archive old data (nova-manage db archive_deleted_rows).

**Q3: How do you handle a sudden 10x spike in API requests?**
A: (1) HAProxy rate limiting kicks in, returning 429 Too Many Requests for abusive clients. (2) WSGI workers queue requests; queue depth increases but stays bounded by HAProxy maxconn. (3) RabbitMQ queues grow for async operations — this is fine as long as consumers are healthy. (4) Galera may experience flow control — writes slow down uniformly. (5) Short-term: increase WSGI workers on existing controllers. (6) Long-term: add controller nodes, implement per-project rate limiting, deploy API gateway with more sophisticated traffic management.

**Q4: How do you size Memcached for the control plane?**
A: Memcached is used primarily for: (1) Keystone token cache: each cached token ~1 KB, 10,000 active tokens = 10 MB. (2) Keystone catalog cache: ~50 KB per project, 5,000 projects = 250 MB. (3) API response cache (optional): variable. Total: typically 1-4 GB per Memcached instance is sufficient. Deploy on each controller node. Client-side consistent hashing distributes keys across all nodes. If a Memcached node fails, cache misses increase but functionality is unaffected (Fernet validates locally).

**Q5: How do you perform capacity planning for the control plane?**
A: (1) Baseline: measure current metrics — API req/s, DB TPS, RabbitMQ msg/s, CPU/RAM per controller. (2) Model growth: compute host count x average operations per host = projected load. Rule of thumb: 1 controller for every 2,000-3,000 compute hosts. (3) Stress test: use Rally to simulate 2x projected load; identify which component saturates first. (4) Plan for burst: size for 3x average load (handles peak events like region failover). (5) Alerting: alert at 60% utilization; add capacity at 80%.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Impact | Detection | Recovery | RTO |
|---|---------|--------|-----------|----------|-----|
| 1 | Single controller node failure | Reduced capacity (4 of 5 nodes); automatic failover for all components | Multiple monitoring checks | Galera: auto-evict; RabbitMQ: auto-evict; HAProxy: health check removes | < 30s |
| 2 | Two controller nodes fail simultaneously | Galera loses quorum (2 of 5 remaining, not majority); RabbitMQ may lose quorum | Monitoring alerts; API errors | Bootstrap Galera from remaining nodes; RabbitMQ recovers if 3+ alive | 5-15 min |
| 3 | HAProxy VIP failure + keepalived failure | No API access; running VMs unaffected | External health check; no response on VIP | Fix keepalived; manual VIP assignment as last resort | 1-5 min |
| 4 | RabbitMQ cluster partition | Message delivery failures; some operations fail | RabbitMQ partition detection; oslo.messaging errors | pause_minority auto-resolution; manual intervention if persistent | 1-5 min |
| 5 | Galera slow node (disk I/O issue) | Flow control pauses all writes cluster-wide | wsrep_flow_control_paused metric | Evict slow node (wsrep_desync); replace disk | < 5 min |
| 6 | Memcached node failure | Increased token validation latency (cache miss → Fernet decrypt, still < 5ms) | Memcached monitoring; cache hit rate drops | No recovery needed; clients hash to remaining nodes | Immediate |
| 7 | All controller nodes fail | Complete control plane outage; running VMs continue operating | External monitoring; no API response | Power restore; bootstrap Galera; start all services | 15-60 min |
| 8 | NVMe disk failure on controller | DB data loss on that node; Galera has copies on other nodes | SMART monitoring; disk error alerts | Replace disk; rejoin Galera (SST from surviving node) | 30-60 min |
| 9 | Control plane network partition (controllers can't reach compute hosts) | No new operations; running VMs continue; nova-compute heartbeats fail | Network monitoring; compute service status | Resolve network issue; VMs are fine; control plane operations resume | Varies |
| 10 | Runaway API client (infinite loop, broken automation) | Control plane saturated; other clients experience timeouts | HAProxy connection count per source IP; API latency spike | Rate limiting kicks in; identify and block abusive client | < 5 min |

### HA Patterns Summary

| Component | HA Strategy | Quorum Required | Nodes | Failover Time |
|-----------|-------------|-----------------|-------|---------------|
| HAProxy | Active-passive VIP (keepalived) | N/A | 3-5 | < 3s |
| Keystone | Active-active (stateless) | N/A | 3-5 | < 10s (health check) |
| Nova/Neutron API | Active-active (stateless) | N/A | 3-5 | < 10s |
| Nova scheduler/conductor | Active-active (RabbitMQ consumers) | N/A | 3-5 | < 10s |
| RabbitMQ | Raft quorum queues | Majority (2/3 or 3/5) | 3-5 | < 10s |
| MySQL/Galera | Multi-master replication | Majority (2/3 or 3/5) | 3-5 | < 5s |
| Memcached | Distributed (no replication) | N/A | 3-5 | Immediate (client failover) |
| Pacemaker/Corosync | Cluster manager | Majority | 3-5 | < 3s |

---

## 10. Observability

### Key Metrics

| Category | Metric | Source | Alert Threshold |
|----------|--------|--------|-----------------|
| **HAProxy** | haproxy_backend_up | HAProxy stats | Any backend DOWN |
| **HAProxy** | haproxy_frontend_current_sessions | HAProxy stats | > 80% maxconn |
| **HAProxy** | haproxy_backend_response_time_p99 | HAProxy stats | > 2s |
| **HAProxy** | haproxy_frontend_http_responses_total{code="5xx"} | HAProxy stats | > 1% of total |
| **RabbitMQ** | rabbitmq_queue_messages_ready | RabbitMQ exporter | > 1000 in any queue |
| **RabbitMQ** | rabbitmq_node_mem_used / rabbitmq_node_mem_limit | RabbitMQ exporter | > 70% |
| **RabbitMQ** | rabbitmq_queue_messages_unacknowledged | RabbitMQ exporter | > 500 |
| **Galera** | mysql_galera_wsrep_cluster_size | MySQL exporter | < expected nodes |
| **Galera** | mysql_galera_wsrep_flow_control_paused | MySQL exporter | > 0.01 (1%) |
| **Galera** | mysql_galera_wsrep_local_state | MySQL exporter | != 4 (Synced) |
| **Galera** | mysql_global_status_threads_connected | MySQL exporter | > 80% max_connections |
| **Galera** | mysql_global_status_slow_queries | MySQL exporter | Increasing trend |
| **Keystone** | keystone_token_issuance_latency_p99 | StatsD | > 500ms |
| **Memcached** | memcached_hit_ratio | Memcached exporter | < 80% (poor hit rate) |
| **OS** | node_cpu_utilization | Node exporter | > 80% on any controller |
| **OS** | node_memory_utilization | Node exporter | > 85% |
| **OS** | node_disk_io_time_weighted | Node exporter | Sustained high IOWAIT |
| **API** | openstack_api_error_rate (per service) | openstack-exporter | > 1% |

### OpenStack Ceilometer / Telemetry

```
Control plane telemetry is best handled by:
  1. Prometheus + exporters (primary operational monitoring):
     - mysqld_exporter: Galera and MySQL metrics
     - rabbitmq_exporter: queue depths, message rates, cluster state
     - haproxy_exporter: connection counts, response times, error rates
     - node_exporter: CPU, memory, disk, network on controllers
     - openstack-exporter: service health, resource counts
     - process-exporter: per-service process metrics

  2. ELK Stack (centralized logging):
     - All OpenStack services log via oslo.log → rsyslog → Logstash → Elasticsearch
     - Structured logging (JSON format) for machine parsing
     - Key fields: request_id (cross-service tracing), instance_uuid, project_id
     - Kibana dashboards: error rate by service, slow requests, RPC timeouts

  3. Distributed tracing (optional, newer):
     - OpenTelemetry integration via oslo.middleware
     - Trace a single API request through: HAProxy → API → DB → RabbitMQ → worker
     - Jaeger or Zipkin backend
     - Identifies latency bottlenecks in the request pipeline
```

---

## 11. Security

### Keystone: Auth, Tokens, Projects, Domains

(Covered in detail in Deep Dive 4 above. Summary here.)

```
Security Architecture:
━━━━━━━━━━━━━━━━━━━━

Authentication Chain:
  External → TLS → HAProxy → API service → keystonemiddleware → RBAC policy

Key Security Controls:
  1. TLS everywhere: all API endpoints, DB connections, RabbitMQ, Memcached
  2. Fernet tokens: local validation, short TTL (1 hour)
  3. RBAC: oslo.policy enforces role-based access per API operation
  4. Service accounts: each service has its own Keystone credentials (not shared)
  5. Secrets in Barbican: DB passwords, certificates, API keys stored in Barbican
  6. Network isolation: control plane on separate VLAN from data plane
  7. Rate limiting: HAProxy stick-tables + oslo.middleware rate limiter
  8. Audit logging: Keystone CADF events for all authentication/authorization
  9. STONITH fencing: Pacemaker ensures failed nodes are truly dead (no split-brain)
  10. Database encryption: TLS for Galera replication; Barbican for at-rest encryption of secrets
```

---

## 12. Incremental Rollout

### Upgrade Strategy: Rolling Upgrades

```
OpenStack Rolling Upgrade Process:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Phase 0: Preparation (1 day)
  - Review release notes for breaking changes
  - Test upgrade in staging environment
  - Backup all databases (mysqldump or mariabackup)
  - Verify Galera cluster health (all nodes synced)
  - Verify RabbitMQ cluster health (all nodes running)

Phase 1: Database Expand Migrations (1 hour)
  - Run expand migrations for each service:
    keystone-manage db_sync --expand
    nova-manage db sync
    neutron-db-manage upgrade heads
    cinder-manage db sync
  - These ADD columns/tables (backward compatible)
  - Old code ignores new columns

Phase 2: Upgrade Controller Services (1 controller at a time, 2-4 hours total)
  For each controller:
    1. Disable in HAProxy (drain connections):
       echo "disable server nova_api_backend/controller-1" | socat /var/run/haproxy.sock -
    2. Stop all OpenStack services on this controller
    3. Upgrade packages (apt upgrade / yum update / kolla-ansible upgrade)
    4. Start all services
    5. Verify health: service logs, API response, DB connectivity
    6. Re-enable in HAProxy:
       echo "enable server nova_api_backend/controller-1" | socat /var/run/haproxy.sock -
    7. Monitor for 30 minutes before proceeding to next controller

Phase 3: Upgrade Compute Agents (rolling, days to weeks)
  For each compute host:
    1. Disable nova-compute service (prevents new builds)
    2. Wait for in-flight operations to complete
    3. Upgrade nova-compute, neutron-ovs-agent, etc.
    4. Start services
    5. Verify: host reports to scheduler, existing VMs running
  Rate: 100-500 hosts per day (automated via Ansible)

Phase 4: Online Data Migrations (hours)
  - Backfill new columns from old data:
    nova-manage db online_data_migrations
  - Run until "0 rows matched" reported

Phase 5: Database Contract Migrations (1 hour)
  - Only after ALL services upgraded:
    keystone-manage db_sync --contract
    nova-manage db sync --contract
  - These DROP old columns/tables (breaking for old code)

Phase 6: Unpin RPC Versions (30 minutes)
  - Update nova.conf: remove upgrade_levels.compute
  - Restart nova-conductor (uses new RPC version)
  - Enables new features that require new RPC versions
```

### Rollout Q&As

**Q1: How do you handle a failed upgrade on one controller mid-rollout?**
A: The other controllers continue serving traffic (they're still on the old version or already upgraded). For the failed controller: (1) Check logs to identify the failure (missing dependency, config error, migration error). (2) If fixable: fix the issue and complete the upgrade. (3) If not fixable: roll back to old packages, start old services, re-enable in HAProxy. (4) The deployment is in a mixed-version state, which OpenStack supports (N and N-1 compatible). (5) Investigate the root cause before retrying.

**Q2: What is the RPC version pinning, and why is it important?**
A: During a rolling upgrade, controllers may run version N (upgraded) while compute hosts still run version N-1. Nova uses oslo.versionedobjects for RPC. Version N conductor might send an RPC message with a new field that version N-1 compute doesn't understand. Solution: pin the RPC version to N-1 in nova.conf (`upgrade_levels.compute = auto` detects and uses the minimum version). This ensures conductor sends messages compatible with the oldest compute. After all computes are upgraded, unpin to enable new features.

**Q3: How do you test an upgrade before running it in production?**
A: (1) **Staging environment**: mirror of production (same hardware, same configuration, same scale). Run the full upgrade procedure. Test all operations (create/delete VM, migrate, resize, etc.). (2) **Tempest tests**: OpenStack's integration test suite. Run post-upgrade to verify all APIs work. (3) **Rally benchmarks**: measure API latency and throughput pre- and post-upgrade. Alert on regressions. (4) **Grenade**: OpenStack CI tool that performs a full upgrade and runs tests. (5) **Canary**: upgrade one cell first, monitor for 1 week, then proceed.

**Q4: How do you handle database schema changes that are not backward compatible?**
A: OpenStack's expand/contract migration pattern handles this. Example: renaming a column `name` to `display_name`. (1) Expand: add `display_name` column (both exist). (2) Online data migration: copy `name` → `display_name` for all rows. (3) Code change: new version reads/writes `display_name`; old version reads/writes `name`. During mixed-version operation, both columns are maintained by triggers or application-level dual-write. (4) Contract: drop `name` column after all services use `display_name`. This ensures no version of the code ever references a column that doesn't exist.

**Q5: What is the maximum version skew supported during a rolling upgrade?**
A: OpenStack guarantees N-1 compatibility: services at version N can communicate with services at version N-1. This means: (1) You can have controllers at Dalmatian and compute hosts at Caracal — for the duration of the upgrade. (2) You should NOT have version N and N-2 running simultaneously (not supported). (3) This gives you one release cycle (~6 months) to complete the upgrade of all compute hosts. (4) If upgrading across multiple releases (e.g., Bobcat → Dalmatian), you must upgrade through each intermediate release (Bobcat → Caracal → Dalmatian).

---

## 13. Trade-offs & Decision Log

| # | Decision | Options Considered | Selected | Rationale |
|---|----------|--------------------|----------|-----------|
| 1 | Message broker | RabbitMQ, Kafka, ZeroMQ | RabbitMQ + quorum queues | oslo.messaging native support; quorum queues prevent split-brain; proven at scale |
| 2 | Database | MySQL/Galera, MySQL Group Replication, PostgreSQL/Patroni | MySQL/Galera | Industry standard for OpenStack; synchronous replication; extensive community experience |
| 3 | Load balancer | HAProxy, Nginx, Envoy | HAProxy + keepalived | Best health checking; proven with OpenStack; excellent stats/monitoring; active community |
| 4 | Token format | UUID, Fernet, JWT | Fernet | Local validation (no DB/network); small size; proven; standard since Ocata |
| 5 | Controller topology | Co-located (all-in-one), disaggregated | Co-located for small/medium; disaggregated for large | Co-located is simpler; disaggregated scales better but costs more |
| 6 | Cache | Memcached, Redis | Memcached | Standard for OpenStack; simple; no persistence needed for cache; keystonemiddleware native support |
| 7 | HA manager | Pacemaker/Corosync, systemd only, Kubernetes | Pacemaker/Corosync | Manages VIPs and fencing; proven for OpenStack HA; not overkill like Kubernetes for this use case |
| 8 | Deployment tool | Kolla-ansible, TripleO, OpenStack-Ansible, manual | Kolla-ansible | Docker-based (isolated services); simple Ansible playbooks; active development; good for upgrades |
| 9 | Controller count | 3, 5, 7 | 5 | Survives 2 simultaneous failures; odd number for quorum; 3 too risky for 10K hosts |
| 10 | SSL termination | At HAProxy, at each service, at external LB | HAProxy | Single certificate management point; reduces CPU on service processes; TLS offloading |

---

## 14. Agentic AI Integration

### AI-Enhanced Control Plane Operations

| Use Case | AI Approach | Implementation |
|----------|------------|----------------|
| **Predictive failure detection** | Anomaly detection on controller metrics | ML model trained on healthy controller metrics (CPU, memory, disk I/O, network, Galera flow control, RabbitMQ queue depth). Detects anomalies 30-60 minutes before failure. Example: gradual disk I/O increase predicting NVMe wear-out. |
| **Automated root cause analysis** | Causal inference from correlated events | When API error rate spikes: correlate with Galera flow control, RabbitMQ queue depth, compute host heartbeats, and network latency. AI ranks probable root causes. "90% probability: Galera flow control on controller-2 due to slow disk." |
| **Capacity planning** | Time-series forecasting | Predict when control plane components reach capacity: DB connections, RabbitMQ memory, HAProxy connections. Forecast based on compute host growth and API usage trends. Recommend: "Add 2 controller nodes within 60 days." |
| **Configuration optimization** | Bayesian optimization of parameters | Tune nova.conf, rabbitmq.conf, my.cnf parameters for optimal throughput/latency. Experiment in staging with different settings (pool_size, workers, buffer_pool). AI finds Pareto-optimal configuration. |
| **Intelligent alerting** | Alert grouping and deduplication | Reduce alert noise: when Galera flow control triggers, suppress related API latency and RabbitMQ queue alerts (they're symptoms, not root causes). Learn from operator responses to improve grouping. |
| **Upgrade risk assessment** | Release note analysis + historical data | AI reads release notes, analyzes past upgrade incidents, and scores upgrade risk. "This release changes Galera settings — high risk for clusters < 5 nodes. Recommend upgrading staging first." |

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through the full request path for `openstack server create`, from the client to the VM booting.**
A: CLI → HTTPS → External LB → HAProxy → nova-api (WSGI worker). nova-api: (1) keystonemiddleware validates Fernet token locally (< 5ms). (2) oslo.policy checks RBAC (role:member required). (3) Validates request: flavor exists, image exists, quota available. (4) Creates instance record in nova_api DB (Galera write, < 10ms). (5) Sends `build_instances` RPC cast to nova-conductor via RabbitMQ. (6) Returns 202 Accepted to client. nova-conductor receives RPC from RabbitMQ. Sends `select_destinations` RPC call to nova-scheduler. nova-scheduler queries Placement API (HTTP → HAProxy → placement-api → placement DB). Returns candidates. Scheduler filters/weights, selects host, claims allocations in Placement. Returns to conductor. Conductor writes instance to cell DB (Galera). Sends `build_and_run_instance` RPC cast to nova-compute via cell RabbitMQ. nova-compute: downloads image from Glance (HTTP), creates port in Neutron (HTTP), creates VM via libvirt, updates state via conductor.

**Q2: What happens to running VMs if the entire control plane goes down?**
A: Running VMs are completely unaffected. They continue running on their compute hosts. The hypervisor (libvirt/KVM) manages VMs independently of the control plane. Network connectivity continues because OVS flows are programmed in the kernel datapath. DHCP leases are valid until expiry (typically 24-48 hours). What doesn't work: no new VMs, no lifecycle operations (stop, migrate, resize), no metadata service, no new DHCP leases. Users can't access the API. This is by design: data plane independence from the control plane.

**Q3: How do you handle a RabbitMQ cluster partition?**
A: With `cluster_partition_handling = pause_minority` (recommended): when a partition occurs, the minority side pauses all queue operations. Clients on the minority side get connection errors and reconnect to the majority side (oslo.messaging's rabbit_hosts list). When the partition heals, the minority side automatically resumes. With quorum queues: Raft naturally handles partitions — only the partition with majority can elect leaders and process messages. Manual intervention: if the partition persists, restart RabbitMQ nodes on the minority side to force a clean rejoin.

**Q4: How does the control plane handle a "noisy" tenant making excessive API calls?**
A: (1) HAProxy rate limiting: stick-table tracks per-IP request rate. Exceeding the limit returns 429 Too Many Requests. (2) Per-project rate limiting: oslo.middleware or custom middleware checks project_id from the token and applies per-project limits. (3) Quota enforcement: even if API calls succeed, resource creation is capped by quotas. (4) Back-pressure: if downstream (conductor, scheduler) is slow, API requests queue but eventually timeout (500 error). (5) Monitoring: alert on per-project API call rate; contact the tenant.

**Q5: What is the difference between WSGI workers and sub-processes in OpenStack services?**
A: OpenStack API services run as WSGI applications. The WSGI server (Apache httpd / uWSGI / gunicorn) spawns multiple worker processes, each handling requests independently. `workers = 8` means 8 OS processes, each with its own Python interpreter, DB connection pool, and RabbitMQ connection. More workers = more concurrent requests but more memory and DB connections. Rule of thumb: workers = CPU cores (for CPU-bound); workers = 2x CPU cores (for I/O-bound, which most API services are). Non-API services (scheduler, conductor) spawn their own worker processes based on configuration.

**Q6: How do you debug a slow API response?**
A: (1) Check HAProxy stats: is the backend slow or is it HAProxy? (2) Check the request_id in API response headers (X-Openstack-Request-Id). (3) Search logs across all services for that request_id — it follows the request through nova-api, conductor, scheduler, compute. (4) Check DB slow query log — was there a slow SQL query? (5) Check RabbitMQ queue depth — was the RPC message queued? (6) If using distributed tracing (OpenTelemetry/Jaeger): view the trace to see exactly where time was spent. (7) Common causes: Galera flow control (writes paused), RabbitMQ consumer busy, slow image download, slow network (VXLAN tunnel issue).

**Q7: How does Pacemaker/Corosync fit into the HA picture?**
A: Pacemaker is a cluster resource manager; Corosync provides group communication (heartbeats). Together they manage: (1) VIP resources: if the active HAProxy node fails, Pacemaker moves the VIP to another node. (2) Fencing (STONITH): if a node becomes unresponsive, Pacemaker fences it (power-off via IPMI) to prevent split-brain. (3) Service restart: Pacemaker can restart failed services. However, in modern OpenStack deployments, many operators use simpler HA: systemd for service management, keepalived for VIP, and let Galera/RabbitMQ handle their own clustering. Pacemaker adds complexity but provides the STONITH guarantee that prevents split-brain.

**Q8: How do you handle certificate management for the control plane?**
A: (1) CA: either an internal CA (HashiCorp Vault PKI, FreeIPA) or public CA (Let's Encrypt). (2) Certificates: each API endpoint has a TLS certificate. HAProxy terminates TLS. (3) Internal TLS: Galera replication, RabbitMQ inter-node, and Memcached also use TLS. (4) Rotation: certificates have 90-day expiry. Automated renewal via certbot or Vault. (5) Monitoring: alert when any certificate expires within 30 days. (6) Storage: private keys stored in Barbican or Vault, never on disk in plaintext. (7) mTLS: optional for service-to-service communication (client cert validates the service identity).

**Q9: How does Heat (orchestration) interact with other control plane services?**
A: Heat is a template-based orchestration service. A Heat template defines a "stack" of resources (servers, networks, volumes, etc.). When a user creates a stack, Heat: (1) Parses the template and resolves dependencies (e.g., network before server). (2) For each resource, calls the appropriate OpenStack API: Nova for servers, Neutron for networks, Cinder for volumes. (3) Heat tracks resource state in its own DB. (4) If creation fails, Heat can rollback: delete created resources in reverse order. (5) Stack updates: Heat computes the diff between old and new template and applies minimal changes. Heat's engine runs as RPC workers consuming from RabbitMQ, like nova-conductor.

**Q10: What are the security implications of a compromised controller node?**
A: A compromised controller has access to: (1) All database credentials (nova, neutron, keystone, etc.) — can read/modify any data. (2) RabbitMQ credentials — can intercept/forge RPC messages. (3) Fernet keys — can forge tokens as any user. (4) Galera wsrep — can modify data on other nodes. Impact: total compromise of the cloud. Mitigation: (1) Harden controllers (minimal packages, SELinux, no SSH from external). (2) Network isolation (control plane VLAN). (3) Monitoring for unusual DB queries, RPC messages, token issuance. (4) If compromised: immediately rotate all credentials, Fernet keys, and certificates. Rebuild the compromised node from scratch.

**Q11: How do you handle log management for the control plane?**
A: Each service logs via oslo.log to local files and/or syslog. (1) Local: /var/log/<service>/<service>.log (for quick debugging). (2) Centralized: rsyslog forwards to a log aggregation pipeline (Logstash → Elasticsearch → Kibana, or Fluentd → S3/Elasticsearch). (3) Structured logging: oslo.log's JSON formatter enables machine parsing. (4) Key fields: timestamp, severity, request_id, instance_uuid, project_id. (5) Log rotation: logrotate with 7-day retention locally; 30-day in centralized store. (6) Alert on: ERROR/CRITICAL severity; specific patterns ("DBConnectionError", "MessagingTimeout", "NoValidHost"). (7) Privacy: logs may contain user IPs and project IDs — secure access to log store.

**Q12: How does the control plane handle a slow or unresponsive Glance (image service)?**
A: When nova-compute downloads an image from Glance during VM build, a slow Glance causes builds to hang. Impact: (1) VM remains in BUILD state. (2) Other VMs on the same host may be delayed (download blocks the build thread). Mitigation: (1) Glance caches images on compute hosts (image_cache_size in nova.conf). Second VM with the same image doesn't re-download. (2) Glance backend: use Ceph RBD, which enables copy-on-write (COW) cloning instead of full image download. (3) Timeout: nova.conf `image_download_timeout = 3600` (1 hour) prevents indefinite hangs. (4) Multiple Glance instances behind HAProxy for throughput. (5) Monitor Glance response time and backend (Ceph) health.

**Q13: How would you implement blue-green deployment for the control plane?**
A: Blue-green for the full control plane is impractical (shared state in DB), but it works for stateless API services. Approach: (1) Deploy new version of nova-api as "green" instances on separate ports. (2) HAProxy has both "blue" (current) and "green" (new) backends, with green initially at weight 0. (3) Gradually shift traffic: increase green weight, decrease blue weight (canary). (4) Monitor error rates. If green is healthy, set blue weight to 0. (5) Stop blue instances. For stateful components (DB, MQ), rolling upgrade is the only option. Alternative: use Cells v2 — upgrade one cell at a time (effectively blue-green at the cell level).

**Q14: What is ProxySQL, and when should you use it?**
A: ProxySQL is a MySQL protocol-aware proxy that sits between applications and Galera. Benefits: (1) Connection multiplexing: 5,000 application connections multiplexed into 500 real DB connections. (2) Read/write splitting: route SELECT to read replicas, writes to primary. (3) Query caching: cache frequently-read queries (e.g., flavor lookups). (4) Query firewall: block dangerous queries (DROP TABLE). (5) Failover: automatic backend detection and failover. Use ProxySQL when: (a) max_connections is exhausted on Galera nodes, or (b) you want read/write splitting, or (c) you need query-level monitoring. Deploy on each controller (local ProxySQL) or on dedicated proxy nodes.

**Q15: How do you ensure the control plane survives a power outage in one data center zone?**
A: Spread 5 controller nodes across 3 availability zones (different power domains): 2 in AZ-1, 2 in AZ-2, 1 in AZ-3. If AZ-1 loses power: 2 controllers down, 3 remaining (quorum for Galera and RabbitMQ). If AZ-1 and AZ-3 both lose power: 3 controllers down, only 2 remaining — quorum lost. For this reason, 5 controllers across 3 AZs is the minimum for surviving any single AZ failure. Alternative: use Galera arbitrator (garbd) in the third AZ — lightweight process that participates in quorum without storing data, allowing 2+2+garbd topology.

---

## 16. References

| # | Resource | URL |
|---|----------|-----|
| 1 | OpenStack HA Guide | https://docs.openstack.org/ha-guide/index.html |
| 2 | RabbitMQ Quorum Queues | https://www.rabbitmq.com/docs/quorum-queues |
| 3 | Galera Cluster Documentation | https://galeracluster.com/library/documentation/ |
| 4 | HAProxy Configuration Manual | https://www.haproxy.org/download/2.8/doc/configuration.txt |
| 5 | Keystone Architecture | https://docs.openstack.org/keystone/latest/getting-started/architecture.html |
| 6 | oslo.messaging Documentation | https://docs.openstack.org/oslo.messaging/latest/ |
| 7 | oslo.db Documentation | https://docs.openstack.org/oslo.db/latest/ |
| 8 | Kolla-Ansible | https://docs.openstack.org/kolla-ansible/latest/ |
| 9 | Pacemaker Documentation | https://clusterlabs.org/pacemaker/doc/ |
| 10 | ProxySQL Documentation | https://proxysql.com/documentation/ |
| 11 | OpenStack Upgrades | https://docs.openstack.org/nova/latest/admin/upgrades.html |
| 12 | Barbican (Secrets) | https://docs.openstack.org/barbican/latest/ |
