# System Design: Service Discovery System

> **Relevance to role:** Service discovery is the nervous system of any cloud infrastructure platform. Kubernetes relies on CoreDNS + kube-proxy for service routing. OpenStack services (Nova, Neutron, Keystone) register in a service catalog. Bare-metal provisioners must discover IPMI controllers, DHCP servers, and PXE boot servers. Java/Python microservices need to find database endpoints, message brokers, and peer services. Without reliable service discovery, no component can find any other component.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Service registration | Services register themselves (self-registration) or are registered by a third party (orchestrator) |
| FR-2 | Service deregistration | Explicit removal or automatic on failure |
| FR-3 | Service lookup | Query by service name, tags, metadata; return healthy endpoints |
| FR-4 | Health checking | HTTP, TCP, gRPC, script-based health checks with configurable intervals |
| FR-5 | Real-time notifications | Watch/subscribe for service changes (new instance, instance down) |
| FR-6 | Load balancing metadata | Return weights, zones, canary tags for client-side load balancing |
| FR-7 | Multi-datacenter support | Services discoverable across datacenters with locality awareness |
| FR-8 | DNS interface | Standard DNS resolution (A/AAAA/SRV records) for legacy compatibility |
| FR-9 | Key-value store | Auxiliary KV store for configuration (Consul-style) |
| FR-10 | Service mesh integration | Feed service catalog to Envoy/Istio control plane |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Lookup latency | < 1 ms (cached), < 5 ms (uncached) |
| NFR-2 | Registration latency | < 10 ms |
| NFR-3 | Health check propagation | < 5 seconds from failure to deregistration |
| NFR-4 | Availability | 99.99% (infrastructure-critical) |
| NFR-5 | Scalability | 100,000 service instances, 10,000 unique services |
| NFR-6 | Consistency model | AP preferred (availability > consistency for lookups); CP for registration |
| NFR-7 | Throughput | 500,000 lookups/sec cluster-wide |

### Constraints & Assumptions
- Platform runs Kubernetes (primary), with some bare-metal services outside k8s.
- Java services use Spring Cloud, Python services use custom clients.
- DNS is the universal interface; gRPC/HTTP for richer queries.
- Multi-datacenter within a single region (3 AZs); cross-region is out of scope.
- Existing infrastructure: CoreDNS in k8s, but need a unified discovery for k8s + non-k8s services.

### Out of Scope
- Service mesh data plane (Envoy sidecar proxy traffic routing).
- Application-level routing (A/B testing, feature flags).
- Certificate management (handled by PKI / Vault).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Unique services | 2,000 | Platform microservices + infrastructure services |
| Instances per service (avg) | 10 | Some have 3 (control plane), some have 100 (stateless API) |
| Total service instances | 20,000 | 2,000 x 10 |
| Bare-metal services (non-k8s) | 500 | BMC controllers, PXE, DHCP, legacy |
| DNS lookups per second | 200,000 | Every inter-service call requires resolution |
| gRPC/HTTP lookups per second | 50,000 | Rich queries with metadata filtering |
| Health checks per second | 20,000 | 20K instances x 1 check/sec (avg) |
| Watch subscriptions | 5,000 | Services watching their dependencies |
| Registration/deregistration events per sec | 100 | Deploys, scaling, failures |

### Latency Requirements

| Operation | p50 | p99 | Notes |
|-----------|-----|-----|-------|
| DNS lookup (cached by CoreDNS) | 0.1 ms | 0.5 ms | Local CoreDNS cache |
| DNS lookup (cache miss) | 1 ms | 5 ms | Query service registry |
| gRPC lookup by name | 0.5 ms | 2 ms | Direct registry query |
| gRPC lookup with tag filter | 1 ms | 5 ms | Server-side filtering |
| Registration | 5 ms | 20 ms | Requires consensus (Raft) |
| Health check (HTTP) | 10 ms | 100 ms | Network + app response |
| Watch notification delivery | 50 ms | 200 ms | Event propagation |

### Storage Estimates

| Item | Size | Total |
|------|------|-------|
| Service instance record | 1 KB (name, address, port, tags, metadata, health) | 20 MB (20K instances) |
| Health check history (last 10 per instance) | 200 B per check | 40 MB |
| KV store entries | 10K entries x 4 KB avg | 40 MB |
| **Total** | | **~100 MB** |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| DNS queries | 200K/sec x 256 B | 50 MB/s |
| gRPC lookups | 50K/sec x 512 B | 25 MB/s |
| Health check traffic | 20K/sec x 256 B | 5 MB/s |
| Watch notifications | 100 events/sec x 5K subscribers x 512 B | 25 MB/s |
| Raft replication | 100 writes/sec x 1 KB x 2 followers | 0.2 MB/s |

---

## 3. High-Level Architecture

```
   ┌────────────────────────────────────────────────────────────────┐
   │                     Client Applications                        │
   │  ┌──────────┐  ┌──────────────┐  ┌──────────────────────────┐ │
   │  │Java/Spring│  │Python Client │  │ k8s CoreDNS (DNS-based) │ │
   │  │Cloud      │  │(custom SDK)  │  │                          │ │
   │  └─────┬─────┘  └──────┬───────┘  └────────────┬─────────────┘ │
   └────────┼───────────────┼────────────────────────┼──────────────┘
            │ gRPC          │ gRPC                   │ DNS
            │               │                        │
   ┌────────▼───────────────▼────────────────────────▼──────────────┐
   │                    Service Discovery Cluster                    │
   │                                                                 │
   │  ┌─────────────────────────────────────────────────────────┐   │
   │  │                   API Gateway Layer                      │   │
   │  │  ┌──────────┐  ┌───────────┐  ┌──────────────────────┐ │   │
   │  │  │ DNS Server│  │gRPC Server│  │ HTTP REST Server     │ │   │
   │  │  │ (RFC-     │  │           │  │                      │ │   │
   │  │  │  compliant│  │           │  │                      │ │   │
   │  │  └──────────┘  └───────────┘  └──────────────────────┘ │   │
   │  └────────────────────────┬────────────────────────────────┘   │
   │                           │                                     │
   │  ┌────────────────────────▼────────────────────────────────┐   │
   │  │                   Core Services                          │   │
   │  │  ┌──────────┐  ┌───────────┐  ┌──────────────────────┐ │   │
   │  │  │Service   │  │Health     │  │Watch/Notification    │ │   │
   │  │  │Registry  │  │Checker    │  │Engine                │ │   │
   │  │  └──────────┘  └───────────┘  └──────────────────────┘ │   │
   │  │  ┌──────────┐  ┌───────────┐  ┌──────────────────────┐ │   │
   │  │  │KV Store  │  │ACL Engine │  │Query Engine          │ │   │
   │  │  │          │  │           │  │(tag filter, locality)│ │   │
   │  │  └──────────┘  └───────────┘  └──────────────────────┘ │   │
   │  └────────────────────────┬────────────────────────────────┘   │
   │                           │                                     │
   │  ┌────────────────────────▼────────────────────────────────┐   │
   │  │              Consensus Layer (Raft)                       │   │
   │  │                                                          │   │
   │  │   Node 1 (Leader)  ◄──► Node 2      ◄──►  Node 3       │   │
   │  │   AZ-1                  AZ-2                AZ-3        │   │
   │  └──────────────────────────────────────────────────────────┘   │
   └─────────────────────────────────────────────────────────────────┘
                           │
   ┌───────────────────────▼──────────────────────────────────────┐
   │              External Integrations                            │
   │  ┌──────────┐  ┌──────────┐  ┌──────────────────────────┐   │
   │  │Kubernetes │  │Envoy     │  │Prometheus                │   │
   │  │API Server │  │Control   │  │(service discovery)       │   │
   │  │(sync)     │  │Plane     │  │                          │   │
   │  └──────────┘  └──────────┘  └──────────────────────────┘   │
   └──────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **DNS Server** | Serves A/AAAA/SRV records for service names; compatible with standard DNS clients; caches responses with short TTL |
| **gRPC Server** | Rich query API: filter by tags, metadata, health status; streaming watch for real-time updates |
| **HTTP REST Server** | Management API for registration, health check configuration, policy management |
| **Service Registry** | Core catalog: stores all registered services and their instances; Raft-replicated |
| **Health Checker** | Runs periodic health checks (HTTP, TCP, gRPC, script) against registered instances; updates registry on state change |
| **Watch/Notification Engine** | Maintains watch subscriptions; pushes events to subscribers on registry changes |
| **KV Store** | Auxiliary key-value store for configuration and metadata (Consul KV equivalent) |
| **ACL Engine** | Enforces access control: which services can discover which other services |
| **Query Engine** | Processes complex queries with tag filters, locality preferences, weight-based selection |
| **Consensus Layer (Raft)** | Replicates registry state across 3-5 nodes; ensures no data loss on node failure |

### Data Flows

**Service Registration (self-registration):**
1. Service starts, calls `Register(name, address, port, tags, health_check_config)`.
2. API server receives request; proposes to Raft.
3. Raft commits to majority.
4. Health Checker begins periodic checks on the new instance.
5. Watch Engine notifies all subscribers watching this service name.
6. DNS cache is invalidated for this service's records.

**Service Lookup (DNS path):**
1. Client resolves `my-service.service.dc1.consul` (or `my-service.namespace.svc.cluster.local` in k8s).
2. CoreDNS forwards to service discovery's DNS server.
3. DNS server queries the registry (in-memory).
4. Returns only healthy instances as A records (or SRV with port).
5. Client connects to one of the returned addresses.

**Health Check Failure:**
1. Health Checker detects 3 consecutive failures for instance X.
2. Proposes state change to Raft: instance X -> `critical` status.
3. On commit, registry marks instance X as unhealthy.
4. Watch Engine pushes `ServiceChanged` event to subscribers.
5. DNS server excludes instance X from responses.
6. Dependent services stop routing to instance X.

---

## 4. Data Model

### Core Entities & Schema

```
┌─────────────────────────────────────────────────────────┐
│ Service                                                  │
├─────────────────────────────────────────────────────────┤
│ name            VARCHAR(256)    PK    -- e.g. "nova-api" │
│ namespace       VARCHAR(128)          -- e.g. "openstack"│
│ tags            SET<STRING>           -- e.g. {"v2",     │
│                                          "canary"}       │
│ metadata        MAP<STRING,STRING>    -- arbitrary KV    │
│ created_at      TIMESTAMP                                │
│ updated_at      TIMESTAMP                                │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ ServiceInstance                                          │
├─────────────────────────────────────────────────────────┤
│ instance_id     VARCHAR(128)    PK    -- UUID or         │
│                                         host:port        │
│ service_name    VARCHAR(256)    FK                       │
│ address         VARCHAR(64)           -- IP or hostname  │
│ port            INT                                      │
│ protocol        ENUM(HTTP,gRPC,TCP)                      │
│ weight          INT DEFAULT 100       -- load balancing  │
│ zone            VARCHAR(64)           -- e.g. "az-1"     │
│ tags            SET<STRING>                               │
│ metadata        MAP<STRING,STRING>                        │
│ health_status   ENUM(PASSING, WARNING, CRITICAL)         │
│ health_check    HealthCheckConfig                        │
│ last_heartbeat  TIMESTAMP                                │
│ registered_at   TIMESTAMP                                │
│ deregistered_at TIMESTAMP NULLABLE                       │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ HealthCheckConfig                                        │
├─────────────────────────────────────────────────────────┤
│ type            ENUM(HTTP,TCP,gRPC,SCRIPT,TTL)           │
│ endpoint        VARCHAR(256)    -- e.g. "/health"        │
│ interval        DURATION        -- e.g. "10s"            │
│ timeout         DURATION        -- e.g. "5s"             │
│ deregister_after DURATION       -- e.g. "30m" (auto-     │
│                                    deregister if critical)│
│ success_threshold INT DEFAULT 1 -- consecutive passes    │
│                                    to mark healthy       │
│ failure_threshold INT DEFAULT 3 -- consecutive failures  │
│                                    to mark critical      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ HealthCheckResult                                        │
├─────────────────────────────────────────────────────────┤
│ instance_id     VARCHAR(128)    FK                       │
│ check_time      TIMESTAMP                                │
│ status          ENUM(PASSING, WARNING, CRITICAL)         │
│ output          TEXT            -- response body / error  │
│ latency_ms      INT                                      │
└─────────────────────────────────────────────────────────┘
```

### Database Selection

| Option | Verdict | Rationale |
|--------|---------|-----------|
| **Raft-replicated in-memory store** | **Selected** | Service catalog is small (< 100 MB); must be fast; Raft provides durability + consistency; in-memory provides sub-ms lookups |
| etcd (external) | Rejected for primary | Adds external dependency; service discovery should be self-contained (bootstrap problem: you need service discovery to find etcd, but etcd IS service discovery) |
| MySQL | Rejected | Too slow for DNS-speed lookups; transactional overhead unnecessary |
| Redis | Rejected for primary | No consensus; data loss on failover would lose service catalog |

### Indexing Strategy

- **Primary index:** Service name -> list of instances (hash map).
- **Secondary indices:**
  - Tag -> set of service instances (inverted index for tag-based queries).
  - Zone -> set of instances (for locality-aware routing).
  - Health status -> set of instances (for fast "give me only healthy" queries).
- **DNS cache:** Service name -> pre-computed DNS response (invalidated on registry change).

---

## 5. API Design

### gRPC Service

```protobuf
service ServiceDiscovery {
  // Registration
  rpc Register(RegisterRequest) returns (RegisterResponse);
  rpc Deregister(DeregisterRequest) returns (DeregisterResponse);
  rpc Heartbeat(HeartbeatRequest) returns (HeartbeatResponse);

  // Lookup
  rpc Resolve(ResolveRequest) returns (ResolveResponse);
  rpc List(ListServicesRequest) returns (ListServicesResponse);

  // Watch
  rpc Watch(WatchRequest) returns (stream WatchEvent);

  // Health
  rpc GetHealth(GetHealthRequest) returns (HealthResponse);
  rpc UpdateHealth(UpdateHealthRequest) returns (HealthResponse);
}

message RegisterRequest {
  string service_name = 1;
  string namespace = 2;
  ServiceInstance instance = 3;
  HealthCheckConfig health_check = 4;
  bool replace_existing = 5;     // re-register if exists
}

message ResolveRequest {
  string service_name = 1;
  string namespace = 2;
  repeated string tags = 3;       // filter by tags
  bool healthy_only = 4;          // default true
  string preferred_zone = 5;      // locality preference
  int32 limit = 6;                // max results
}

message ResolveResponse {
  repeated ServiceInstance instances = 1;
  int64 index = 2;                // Raft index for watch
  bool is_cached = 3;
}

message WatchRequest {
  string service_name = 1;
  string namespace = 2;
  int64 after_index = 3;          // block until changes after this index
}

message WatchEvent {
  enum EventType {
    REGISTERED = 0;
    DEREGISTERED = 1;
    HEALTH_CHANGED = 2;
    METADATA_UPDATED = 3;
  }
  EventType type = 1;
  ServiceInstance instance = 2;
  int64 index = 3;
}
```

### REST Endpoints

| Method | Path | Description |
|--------|------|-------------|
| PUT | `/v1/agent/service/register` | Register a service instance |
| PUT | `/v1/agent/service/deregister/{id}` | Deregister |
| PUT | `/v1/agent/check/pass/{check_id}` | TTL-based health check pass |
| GET | `/v1/catalog/services` | List all services |
| GET | `/v1/catalog/service/{name}` | Get instances of a service |
| GET | `/v1/health/service/{name}` | Get healthy instances |
| GET | `/v1/health/service/{name}?passing=true` | Only passing instances |
| GET | `/v1/health/checks/{name}` | Get health check results |

### DNS Interface

```
# A record lookup (returns healthy instance IPs)
dig my-service.service.consul
;; ANSWER SECTION:
my-service.service.consul.  0  IN  A  10.0.1.5
my-service.service.consul.  0  IN  A  10.0.2.3
my-service.service.consul.  0  IN  A  10.0.3.7

# SRV record (returns port + weight)
dig my-service.service.consul SRV
;; ANSWER SECTION:
my-service.service.consul.  0  IN  SRV  1 100 8080 instance-1.node.consul.
my-service.service.consul.  0  IN  SRV  1 100 8080 instance-2.node.consul.
my-service.service.consul.  0  IN  SRV  1  50 8080 instance-3.node.consul.

# Tag-based filtering
dig canary.my-service.service.consul
;; ANSWER SECTION:
canary.my-service.service.consul.  0  IN  A  10.0.1.5

# Datacenter-specific
dig my-service.service.dc2.consul
```

### CLI

```bash
# Register a service
svcctl register \
  --name nova-api \
  --namespace openstack \
  --address 10.0.1.5 \
  --port 8774 \
  --tags "v2,production" \
  --health-check-http "/healthcheck" \
  --health-check-interval 10s

# Lookup
svcctl resolve nova-api
# Output:
# NAME       ADDRESS     PORT   HEALTH    ZONE    WEIGHT  TAGS
# nova-api   10.0.1.5    8774   passing   az-1    100     v2,production
# nova-api   10.0.2.3    8774   passing   az-2    100     v2,production
# nova-api   10.0.3.7    8774   warning   az-3    50      v2,canary

# List all services
svcctl list
# Output:
# NAME              INSTANCES   HEALTHY   NAMESPACES
# nova-api          3           2         openstack
# nova-compute      48          48        openstack
# mysql-primary     3           3         database
# kafka-broker      9           9         messaging

# Watch for changes
svcctl watch nova-api
# [2026-04-09T10:15:30Z] HEALTH_CHANGED nova-api@10.0.3.7 passing -> critical
# [2026-04-09T10:15:35Z] DEREGISTERED   nova-api@10.0.3.7
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Health Checking System

**Why it's hard:**
Health checking 20,000+ instances every few seconds generates significant load on both the checker and the checked services. False positives (marking a healthy instance as down) cause traffic shifts and potential cascading failures. False negatives (keeping a failed instance in rotation) cause client errors. The system must be fast, accurate, and resilient.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| Server-side polling (Consul model) | Central authority; consistent view | Checker is bottleneck; network perspective only |
| Client-side heartbeat (Eureka model) | Service knows its own health best | Heartbeat doesn't verify actual serving capability |
| **Hybrid: server-side polling + client TTL heartbeat** | **Best accuracy** | **More complex** |
| Peer-to-peer health checking | Distributed load | Inconsistent views; gossip convergence delay |
| Passive (monitor actual traffic errors) | No extra traffic | Requires production traffic; new instances have no signal |

**Selected: Hybrid (server-side active checks + client TTL heartbeat)**

```python
class HealthCheckScheduler:
    """
    Schedules and executes health checks for all registered instances.
    Distributed across cluster nodes to spread the load.
    """

    def __init__(self, registry, raft, node_id, cluster_nodes):
        self.registry = registry
        self.raft = raft
        self.node_id = node_id
        self.cluster_nodes = cluster_nodes
        self.executor = ThreadPoolExecutor(max_workers=200)
        self.check_results = {}  # instance_id -> deque of results

    def assign_checks(self):
        """
        Distribute health check responsibility across cluster nodes.
        Each instance is checked by exactly one node to avoid duplicate
        checks. Uses consistent hashing for stable assignment.
        """
        ring = ConsistentHashRing(self.cluster_nodes)
        my_instances = []

        for instance in self.registry.all_instances():
            responsible_node = ring.get_node(instance.instance_id)
            if responsible_node == self.node_id:
                my_instances.append(instance)

        return my_instances

    def run_check(self, instance):
        """Execute a single health check."""
        config = instance.health_check
        start = time.monotonic()

        try:
            if config.type == HealthCheckType.HTTP:
                result = self._check_http(instance, config)
            elif config.type == HealthCheckType.TCP:
                result = self._check_tcp(instance, config)
            elif config.type == HealthCheckType.GRPC:
                result = self._check_grpc(instance, config)
            elif config.type == HealthCheckType.SCRIPT:
                result = self._check_script(instance, config)
            elif config.type == HealthCheckType.TTL:
                result = self._check_ttl(instance, config)
            else:
                result = HealthCheckResult(status=CRITICAL,
                                           output="Unknown check type")

            result.latency_ms = (time.monotonic() - start) * 1000

        except Timeout:
            result = HealthCheckResult(
                status=CRITICAL,
                output=f"Timeout after {config.timeout}",
                latency_ms=(time.monotonic() - start) * 1000
            )
        except Exception as e:
            result = HealthCheckResult(
                status=CRITICAL,
                output=str(e),
                latency_ms=(time.monotonic() - start) * 1000
            )

        self._process_result(instance, result)

    def _check_http(self, instance, config):
        """
        HTTP health check.
        Passing: 2xx status code
        Warning: 429 (overloaded but alive)
        Critical: 5xx, timeout, connection refused
        """
        url = f"http://{instance.address}:{instance.port}{config.endpoint}"
        response = requests.get(url, timeout=config.timeout.total_seconds())

        if 200 <= response.status_code < 300:
            return HealthCheckResult(status=PASSING,
                                     output=f"HTTP {response.status_code}")
        elif response.status_code == 429:
            return HealthCheckResult(status=WARNING,
                                     output="HTTP 429 (rate limited)")
        else:
            return HealthCheckResult(status=CRITICAL,
                                     output=f"HTTP {response.status_code}")

    def _check_grpc(self, instance, config):
        """
        gRPC health check per the gRPC Health Checking Protocol.
        https://github.com/grpc/grpc/blob/master/doc/health-checking.md
        """
        channel = grpc.insecure_channel(
            f"{instance.address}:{instance.port}")
        stub = health_pb2_grpc.HealthStub(channel)

        try:
            response = stub.Check(
                health_pb2.HealthCheckRequest(
                    service=instance.service_name
                ),
                timeout=config.timeout.total_seconds()
            )
            if response.status == health_pb2.HealthCheckResponse.SERVING:
                return HealthCheckResult(status=PASSING,
                                         output="gRPC SERVING")
            else:
                return HealthCheckResult(status=CRITICAL,
                                         output=f"gRPC {response.status}")
        finally:
            channel.close()

    def _check_ttl(self, instance, config):
        """
        TTL-based check: service must call PUT /v1/agent/check/pass/{id}
        within the TTL interval. If it doesn't, mark as critical.
        """
        last_heartbeat = instance.last_heartbeat
        if last_heartbeat is None:
            return HealthCheckResult(status=CRITICAL,
                                     output="No heartbeat received")

        elapsed = time.time() - last_heartbeat.timestamp()
        if elapsed > config.interval.total_seconds():
            return HealthCheckResult(
                status=CRITICAL,
                output=f"TTL expired ({elapsed:.1f}s > "
                       f"{config.interval.total_seconds()}s)"
            )
        return HealthCheckResult(status=PASSING,
                                 output=f"TTL OK ({elapsed:.1f}s)")

    def _process_result(self, instance, result):
        """
        Apply threshold logic: only change health status after
        consecutive threshold checks to prevent flapping.
        """
        history = self.check_results.setdefault(
            instance.instance_id, deque(maxlen=10))
        history.append(result)

        current_status = instance.health_status
        config = instance.health_check

        if result.status == CRITICAL:
            # Count consecutive failures
            consecutive_failures = 0
            for r in reversed(history):
                if r.status == CRITICAL:
                    consecutive_failures += 1
                else:
                    break

            if consecutive_failures >= config.failure_threshold:
                if current_status != CRITICAL:
                    self._update_health(instance, CRITICAL,
                                        f"{consecutive_failures} consecutive "
                                        f"failures")

        elif result.status == PASSING:
            consecutive_passes = 0
            for r in reversed(history):
                if r.status == PASSING:
                    consecutive_passes += 1
                else:
                    break

            if consecutive_passes >= config.success_threshold:
                if current_status != PASSING:
                    self._update_health(instance, PASSING,
                                        f"{consecutive_passes} consecutive "
                                        f"passes")

    def _update_health(self, instance, new_status, reason):
        """
        Propose health status change through Raft.
        This ensures all nodes have consistent view.
        """
        entry = RaftEntry(
            op="HEALTH_UPDATE",
            instance_id=instance.instance_id,
            service_name=instance.service_name,
            old_status=instance.health_status,
            new_status=new_status,
            reason=reason,
            timestamp=time.time()
        )
        self.raft.propose(entry)
```

**Eureka Self-Preservation Mode (important concept):**

```
Problem: During a network partition between Eureka server and services,
Eureka might mark ALL instances as unhealthy (mass expiry).
This would cause Eureka to return an empty service list —
worse than returning stale data.

Eureka's solution: Self-Preservation Mode
- If the renewal rate drops below 85% of expected in 15 minutes,
  Eureka enters self-preservation.
- In this mode, it does NOT expire any instances.
- It returns stale data (some instances may be down) rather than
  no data (all instances removed).
- The assumption: it's better to route to a possibly-dead instance
  (client retries) than to have no instances at all.

Our implementation:
```

```python
class SelfPreservation:
    """
    Prevents mass deregistration during network partitions.
    """

    def __init__(self, registry, threshold=0.85, window_minutes=15):
        self.registry = registry
        self.threshold = threshold
        self.window = window_minutes * 60
        self.enabled = False

    def evaluate(self):
        """Called every minute."""
        total_instances = self.registry.instance_count()
        expected_renewals = total_instances  # 1 per instance per interval
        actual_renewals = self.registry.renewal_count_in_window(self.window)

        renewal_rate = actual_renewals / max(expected_renewals, 1)

        if renewal_rate < self.threshold:
            if not self.enabled:
                log.warning(
                    f"Self-preservation ENABLED: renewal rate "
                    f"{renewal_rate:.2%} < {self.threshold:.2%}"
                )
                self.enabled = True
        else:
            if self.enabled:
                log.info(
                    f"Self-preservation DISABLED: renewal rate "
                    f"{renewal_rate:.2%} recovered"
                )
                self.enabled = False

    def should_evict(self, instance):
        """
        If self-preservation is active, don't evict instances
        even if their health check fails.
        """
        if self.enabled:
            return False  # Don't evict — stale data > no data
        return True
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Health checker node goes down | Its assigned instances stop being checked | Reassign via consistent hash; remaining nodes pick up orphaned checks |
| Network partition between checker and instance | False positive (healthy marked down) | Threshold (3 consecutive failures before marking critical); multi-checker from different AZs for critical services |
| Thundering herd after mass health check failure | Sudden traffic shift to remaining healthy instances | Self-preservation mode; gradual traffic shift (weight ramping); circuit breakers on clients |
| Health check endpoint lies (returns 200 but service is broken) | Instance stays in rotation despite being unhealthy | Deep health checks: verify database connectivity, cache reachability, not just HTTP 200 |

**Interviewer Q&A:**

**Q1: What's the difference between a liveness check and a readiness check?**
A: Liveness: "Is the process alive?" Failure means restart the process (k8s kills the pod). Readiness: "Can the process serve traffic?" Failure means remove from load balancing (k8s removes from Service endpoints). A service can be alive but not ready (e.g., still loading a large cache). Service discovery health checks are closest to readiness checks — they control traffic routing, not process lifecycle.

**Q2: How does Consul distribute health checks across its cluster?**
A: Consul uses a gossip protocol (Serf) for health check distribution. Each Consul agent runs on the same machine as the service and performs local health checks. Results are gossiped to the Consul servers. This is extremely efficient: no remote health checking, no bottleneck on the server. Our design uses a centralized approach for non-k8s services and delegates to kubelet for k8s services.

**Q3: How do you handle health check for a database service?**
A: Multi-level: (1) TCP check — can we connect? (2) Query check — `SELECT 1` succeeds? (3) Replication check — is the replica lag < threshold? (4) Capacity check — is disk usage < 90%? For MySQL primary/replica discovery, the health check must distinguish roles: the primary reports as primary, replicas report their lag. This is a "semantic health check."

**Q4: What is the "thundering herd" problem when an instance fails?**
A: When a health check marks an instance as down, all clients shift traffic to the remaining instances simultaneously. If there were N instances handling traffic and one fails, each remaining instance sees a (N/(N-1)) traffic increase instantaneously. Mitigation: (1) Client-side load balancing with gradual weight reduction. (2) Connection draining (stop sending new requests but finish in-flight). (3) Over-provision to handle N-1 load.

**Q5: How do you handle health check flooding (too many checks overwhelming a service)?**
A: (1) Each instance is checked by exactly one checker node (consistent hashing). (2) Minimum interval of 5 seconds. (3) Health check endpoints should be lightweight (no database queries, no external calls). (4) Rate limit health check traffic. (5) For k8s, use the kubelet's built-in check mechanism (1 checker per pod, local).

**Q6: Should health checks use the same network path as production traffic?**
A: Ideally yes, because you want to verify the full path. But in practice, health checks often bypass load balancers and go directly to the instance. This misses load-balancer-level failures. A compromise: health checks at two levels — direct (instance health) and through the LB (end-to-end path health).

---

### Deep Dive 2: Service Registration Models (Self-Registration vs Third-Party)

**Why it's hard:**
Who is responsible for registering a service instance? If the service registers itself (self-registration), it must know about the registry and handle registration failures. If a third party registers it (e.g., k8s, a sidecar, a deployment system), the service is simpler but the third party becomes a critical dependency.

**Approaches:**

| Model | How | Pros | Cons | Example |
|-------|-----|------|------|---------|
| **Self-registration** | Service calls registry API on startup | Simple; service knows its own metadata | Couples service to registry; every service needs registration code | Eureka, Spring Cloud |
| **Third-party registration** | External agent watches for new instances and registers them | Service decoupled from registry; language-agnostic | Requires registration agent; agent is SPOF | Kubernetes, Registrator |
| **Sidecar registration** | Sidecar container in same pod registers on behalf of service | Language-agnostic; lifecycle tied to service | Sidecar overhead; complexity | Consul Connect |
| **Platform-native** | Platform (k8s) automatically registers based on deployment spec | Zero service-side code; automatic | Only works within the platform; non-k8s services excluded | Kubernetes Service |

**Selected: Hybrid — platform-native for k8s + sidecar for non-k8s**

```python
class KubernetesServiceSync:
    """
    Syncs Kubernetes Service/Endpoints into our service registry.
    Third-party registration model: k8s is the authoritative source.
    """

    def __init__(self, k8s_client, registry):
        self.k8s = k8s_client
        self.registry = registry

    def sync_loop(self):
        """
        Watch k8s Endpoints objects and sync to our registry.
        Uses k8s watch API for real-time updates.
        """
        resource_version = "0"

        while True:
            try:
                watcher = watch.Watch()
                stream = watcher.stream(
                    self.k8s.list_endpoints_for_all_namespaces,
                    resource_version=resource_version,
                    timeout_seconds=300
                )

                for event in stream:
                    event_type = event['type']     # ADDED, MODIFIED, DELETED
                    endpoints = event['object']
                    resource_version = endpoints.metadata.resource_version

                    self._process_endpoints(event_type, endpoints)

            except ApiException as e:
                if e.status == 410:  # Gone — resource version too old
                    resource_version = "0"  # Full resync
                else:
                    log.error(f"k8s watch error: {e}")
                    time.sleep(5)

    def _process_endpoints(self, event_type, endpoints):
        """Convert k8s Endpoints to our service registry format."""
        service_name = endpoints.metadata.name
        namespace = endpoints.metadata.namespace

        if event_type == "DELETED":
            self.registry.deregister_all(service_name, namespace)
            return

        # Extract addresses from subsets
        instances = []
        for subset in (endpoints.subsets or []):
            ports = {p.name: p.port for p in (subset.ports or [])}

            # Ready addresses (healthy)
            for addr in (subset.addresses or []):
                instances.append(ServiceInstance(
                    instance_id=f"{addr.ip}:{ports.get('', 0)}",
                    service_name=service_name,
                    namespace=namespace,
                    address=addr.ip,
                    port=ports.get('http', ports.get('grpc', 0)),
                    health_status=PASSING,
                    zone=addr.node_name,  # k8s node = zone proxy
                    metadata={
                        "pod_name": addr.target_ref.name
                            if addr.target_ref else "",
                        "node_name": addr.node_name or ""
                    }
                ))

            # Not-ready addresses (unhealthy but registered)
            for addr in (subset.not_ready_addresses or []):
                instances.append(ServiceInstance(
                    instance_id=f"{addr.ip}:{ports.get('', 0)}",
                    service_name=service_name,
                    namespace=namespace,
                    address=addr.ip,
                    port=ports.get('http', 0),
                    health_status=CRITICAL,
                    zone=addr.node_name
                ))

        self.registry.sync_instances(service_name, namespace, instances)


class BareMetalServiceRegistrar:
    """
    Sidecar for bare-metal services that don't run in Kubernetes.
    Runs on the same host; registers the co-located service.
    """

    def __init__(self, registry_client, service_config):
        self.client = registry_client
        self.config = service_config
        self.registered = False

    def run(self):
        """Main loop: register, heartbeat, deregister on shutdown."""
        signal.signal(signal.SIGTERM, self._shutdown)

        # Register
        self.client.register(
            service_name=self.config.name,
            address=self.config.address,
            port=self.config.port,
            tags=self.config.tags,
            health_check=HealthCheckConfig(
                type=HealthCheckType.HTTP,
                endpoint=self.config.health_endpoint,
                interval="10s",
                timeout="5s",
                deregister_after="30m"
            )
        )
        self.registered = True
        log.info(f"Registered {self.config.name} at "
                 f"{self.config.address}:{self.config.port}")

        # Heartbeat loop
        while self.registered:
            try:
                self.client.heartbeat()
            except Exception as e:
                log.error(f"Heartbeat failed: {e}")
            time.sleep(10)

    def _shutdown(self, signum, frame):
        """Graceful deregistration on SIGTERM."""
        log.info(f"Deregistering {self.config.name}")
        try:
            self.client.deregister()
        except Exception:
            pass  # Best effort
        self.registered = False
```

**Interviewer Q&A:**

**Q1: How does Kubernetes service discovery work internally?**
A: (1) Pod is created with labels (e.g., `app=nova-api`). (2) A Service object has a selector (e.g., `app=nova-api`). (3) The Endpoints controller watches Pods and Services; when a Pod matches a Service selector and is Ready, it adds the Pod IP to the Endpoints object. (4) kube-proxy watches Endpoints and programs iptables/IPVS rules on every node. (5) CoreDNS watches Services and serves DNS records. (6) A client resolves `nova-api.openstack.svc.cluster.local` via CoreDNS, gets the ClusterIP, and kube-proxy forwards to a healthy Pod IP.

**Q2: What is the bootstrap problem in service discovery?**
A: How do you discover the service discovery system itself? Solutions: (1) Static configuration: discovery servers at well-known IPs (e.g., `/etc/resolv.conf` for DNS). (2) Multicast/anycast: discovery service on a well-known anycast IP. (3) Kubernetes: kube-apiserver is configured by kubelet at a known address; everything else is discovered from there. For our system: deploy on known IPs; client SDKs are configured with initial seed list.

**Q3: Compare Consul and Eureka for service discovery.**
A: Consul: CP system (Raft consensus), supports health checks (active polling), multi-datacenter, DNS interface, KV store, service mesh. Eureka: AP system (peer-to-peer replication, no consensus), self-preservation mode, simpler, Netflix-ecosystem. For infrastructure: Consul is preferred because CP guarantees prevent split-brain routing. Eureka is fine for application-level discovery where stale data is tolerable.

**Q4: How do you handle service discovery for services that span k8s and non-k8s?**
A: Unified registry: (1) k8s services are synced via our KubernetesServiceSync watcher. (2) Non-k8s services register via sidecar or self-registration. (3) Both end up in the same registry. (4) Clients query one API regardless of where the service runs. The DNS interface makes this transparent: `nova-api.service.consul` works whether Nova is in k8s or on bare-metal.

**Q5: What is the difference between client-side and server-side load balancing in service discovery?**
A: Server-side (e.g., k8s ClusterIP + kube-proxy): DNS returns a single VIP; the proxy routes to backend. Client doesn't know about individual instances. Client-side (e.g., Ribbon, Envoy sidecar): DNS/API returns all healthy instances; client picks one using a load-balancing algorithm (round-robin, least-connections, weighted). Client-side is more flexible (can use locality, weights, circuit breaking) but requires smarter clients.

**Q6: How does CoreDNS handle service discovery in Kubernetes?**
A: CoreDNS runs as a Deployment in `kube-system`. It watches the k8s API for Services and Endpoints. For each Service, it creates DNS records: `{service}.{namespace}.svc.cluster.local` -> ClusterIP (A record). For headless services (no ClusterIP), it returns individual Pod IPs. SRV records include port information. CoreDNS caches responses and serves from memory. TTL is typically 30s.

---

### Deep Dive 3: DNS-Based vs Registry-Based Discovery

**Why it's hard:**
DNS is universally supported (every language, every OS, every library can resolve DNS). But DNS has inherent limitations: caching causes stale results, no health information in responses, limited metadata, and TTL-based invalidation is slow. Rich registry APIs solve these problems but require client-side integration.

**Comparison:**

| Feature | DNS-Based | Registry-Based (gRPC/HTTP) |
|---------|-----------|---------------------------|
| Client compatibility | Universal | Requires SDK integration |
| Staleness | TTL-dependent (seconds to minutes) | Real-time (watch/subscribe) |
| Health information | Not in standard DNS | Full health status per instance |
| Metadata (tags, weights) | Limited (TXT records, SRV weights) | Rich metadata support |
| Load balancing | Client picks randomly from A records | Client can use weights, zones, least-connections |
| Multi-datacenter | DNS supports DC-specific queries | Full DC-aware routing |
| Performance | Cached locally by OS/resolver | Requires network call (can be cached by SDK) |
| Complexity | Zero client-side code | SDK integration per language |

**Selected: Both — DNS for universal access + gRPC for rich queries**

```
Strategy: DNS as the universal fallback + gRPC for smart clients

┌─────────────────────────────────────────────────────┐
│ Smart Clients (Java Spring Cloud, Python SDK)       │
│  → Use gRPC Watch API                              │
│  → Client-side load balancing with locality         │
│  → Real-time health updates                         │
│  → Full metadata and tag filtering                  │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│ Legacy / Third-Party Clients (curl, any language)   │
│  → Use DNS (A/SRV records)                          │
│  → Basic round-robin load balancing                 │
│  → Health reflected in DNS (only healthy instances)  │
│  → 5-second TTL for reasonable freshness             │
└─────────────────────────────────────────────────────┘
```

**DNS Implementation Details:**

```python
class ServiceDiscoveryDNSServer:
    """
    DNS server that translates service names to instance addresses.
    Integrates with the service registry.
    """

    # DNS naming convention:
    # {service}.service.{datacenter}.{domain}
    # {tag}.{service}.service.{datacenter}.{domain}
    #
    # Examples:
    # nova-api.service.dc1.infra       → A records for nova-api in dc1
    # canary.nova-api.service.dc1.infra → A records for canary-tagged
    # nova-api.service.dc1.infra SRV   → SRV records with ports

    def __init__(self, registry, default_ttl=5):
        self.registry = registry
        self.default_ttl = default_ttl  # 5 second TTL
        self.cache = LRUCache(max_size=10000)

    def handle_query(self, query):
        """Process a DNS query."""
        name = query.name.lower()
        qtype = query.type  # A, AAAA, SRV, TXT

        # Parse the service name from DNS query
        parsed = self._parse_service_name(name)
        if not parsed:
            return DNSResponse(rcode=NXDOMAIN)

        service_name = parsed.service
        tag_filter = parsed.tag
        datacenter = parsed.datacenter

        # Check cache
        cache_key = f"{name}:{qtype}"
        cached = self.cache.get(cache_key)
        if cached and not cached.expired():
            return cached.response

        # Query registry for healthy instances
        instances = self.registry.resolve(
            service_name=service_name,
            healthy_only=True,
            tags=[tag_filter] if tag_filter else [],
            datacenter=datacenter
        )

        if not instances:
            return DNSResponse(rcode=NXDOMAIN)

        # Build DNS response
        if qtype == "A":
            records = [ARecord(ip=i.address, ttl=self.default_ttl)
                       for i in instances]
            # Shuffle for basic load balancing
            random.shuffle(records)

        elif qtype == "SRV":
            records = [SRVRecord(
                priority=1,
                weight=i.weight,
                port=i.port,
                target=f"{i.instance_id}.node.{datacenter}.infra",
                ttl=self.default_ttl
            ) for i in instances]

        else:
            return DNSResponse(rcode=NOTIMP)

        response = DNSResponse(records=records, rcode=NOERROR)
        self.cache.put(cache_key, CacheEntry(response, ttl=self.default_ttl))
        return response
```

**Interviewer Q&A:**

**Q1: What DNS TTL should you use for service discovery?**
A: 0-5 seconds. Long TTLs (30s, 60s) cause stale routing to failed instances. Very short TTLs (0) cause excessive DNS queries. 5 seconds is a good balance: instances that fail are removed from DNS within 5 seconds, and DNS query load is manageable. Note: some clients ignore TTL and cache longer (Java's default DNS cache is 30s; set `networkaddress.cache.ttl=5` in `java.security`).

**Q2: What are the limitations of DNS for service discovery?**
A: (1) No health status — DNS returns records or not; no way to indicate "warning" vs "critical." (2) Caching — clients and intermediate resolvers cache records, causing staleness. (3) No metadata — you can't return tags, weights, or zones in a standard A record (SRV helps partially). (4) Connection-level — DNS resolves at connection time; long-lived connections don't re-resolve. (5) No subscription — clients must poll (re-resolve periodically), not react to changes.

**Q3: How does Consul handle DNS queries?**
A: Consul embeds a DNS server (port 8600). It translates DNS queries to catalog lookups. Format: `{service}.service.{datacenter}.consul`. Supports A, SRV, and prepared queries (complex routing logic via TXT records). Consul's DNS interface is a compatibility layer on top of its HTTP API. TTL is configurable per service. For k8s integration, CoreDNS can forward specific zones to Consul's DNS.

**Q4: How do you handle DNS-based discovery for gRPC services?**
A: gRPC uses long-lived HTTP/2 connections. DNS-based discovery has a problem: the connection is made once, and the client doesn't re-resolve DNS unless the connection drops. Solutions: (1) gRPC client-side load balancing with `dns:///` scheme and periodic re-resolution (`grpc.dns_min_time_between_resolutions`). (2) Use xDS protocol (Envoy-compatible) instead of DNS for gRPC. (3) Headless service in k8s + gRPC pick-first with round-robin.

**Q5: What is the difference between headless and ClusterIP services in k8s?**
A: ClusterIP: k8s assigns a virtual IP; kube-proxy load-balances to pods; DNS returns the VIP. Headless (`clusterIP: None`): no VIP; DNS returns individual Pod IPs; client does its own load balancing. Use headless for: stateful services (each instance is unique), gRPC (need all endpoints for client-side LB), or when you need to control routing logic.

**Q6: How does service discovery integrate with a service mesh (Istio/Envoy)?**
A: Istiod (control plane) watches k8s Services/Endpoints and pushes them to Envoy sidecars via xDS API (Endpoint Discovery Service / EDS). Envoy then load-balances traffic locally, without DNS. This is more dynamic than DNS: changes propagate in seconds, with health checking, circuit breaking, and weighted routing built into Envoy. Our system can integrate by implementing the xDS EDS API.

---

## 7. Scheduling & Resource Management

### Service Discovery for Infrastructure Scheduling

**Job Scheduler discovers compute resources:**
```python
class SchedulerServiceDiscovery:
    """
    The job scheduler uses service discovery to find:
    1. Available compute nodes (bare-metal, VM hosts)
    2. Supporting services (MySQL, Kafka, Elasticsearch)
    3. Peer schedulers (for coordination)
    """

    def discover_compute_nodes(self, resource_requirements):
        """Find compute nodes matching job requirements."""
        instances = self.discovery.resolve(
            service_name="compute-agent",
            tags=self._requirement_to_tags(resource_requirements),
            healthy_only=True,
            preferred_zone=resource_requirements.preferred_zone
        )

        # Filter by resource availability (from instance metadata)
        available = [
            i for i in instances
            if self._has_capacity(i.metadata, resource_requirements)
        ]

        # Sort by locality (same zone first) then by available resources
        available.sort(key=lambda i: (
            0 if i.zone == resource_requirements.preferred_zone else 1,
            -int(i.metadata.get("available_cpu", 0))
        ))

        return available

    def _requirement_to_tags(self, req):
        """Convert job requirements to service discovery tags."""
        tags = [req.instance_type]  # e.g., "gpu", "high-memory"
        if req.requires_ssd:
            tags.append("ssd")
        if req.requires_gpu:
            tags.append(f"gpu-{req.gpu_type}")
        return tags
```

**Bare-Metal IPMI Discovery:**
```
Service: "bmc-controller"
Tags: ["rack-42", "dell-r750", "gpu-a100"]
Metadata:
  ipmi_address: "10.10.42.100"
  serial_number: "SN12345"
  firmware_version: "2.1.3"
  available_cpu: "128"
  available_memory_gb: "512"

The provisioner discovers BMCs via service discovery rather than
a static inventory file. This enables:
- Dynamic rack addition without config changes
- Automatic removal of failed BMCs
- Tag-based filtering for specific hardware types
```

---

## 8. Scaling Strategy

| Dimension | Strategy | Detail |
|-----------|----------|--------|
| Lookup throughput | DNS caching + read replicas | CoreDNS caches locally; gRPC clients cache with watch-based invalidation |
| Registry size | In-memory; Raft group handles 100K instances easily | 100K instances x 1KB = 100MB in memory |
| Health check load | Distributed across cluster nodes via consistent hash | 20K instances / 5 nodes = 4K checks per node |
| Watch subscribers | Event fan-out with coalescing | Batch notifications; coalesce rapid changes (debounce 100ms) |
| Multi-datacenter | Per-DC clusters with WAN gossip (Consul model) | Each DC has independent cluster; cross-DC queries forwarded |

**Interviewer Q&A:**

**Q1: How does service discovery scale to 100K+ instances?**
A: The registry itself is small (100MB for 100K instances). The bottleneck is health checking (100K checks at 10s interval = 10K checks/sec) and event fan-out (a deploy rolling 1K instances generates 1K events/sec). Health checking: distribute across 10+ checker nodes. Fan-out: coalesce events, batch notifications, and use hierarchical pub-sub.

**Q2: What is the performance impact of DNS TTL on service discovery freshness?**
A: With a 5-second TTL, a client using a newly failed instance will continue for up to 5 seconds. In practice, the client's connection attempt will fail immediately (connection refused), triggering a re-resolve. The real problem is clients that cache DNS responses beyond TTL (Java's default is 30s for positive results). Ensure all JVM clients set `networkaddress.cache.ttl=5`.

**Q3: How do you handle service discovery during a mass deployment (1000 pods restarting)?**
A: (1) Rolling deployment: only update 25% of instances at a time (maxSurge/maxUnavailable in k8s). (2) Event coalescing: batch health-change events within 100ms window. (3) Pre-warm new instances before registering (readiness gate). (4) Connection draining: deregister before stopping (preStop hook in k8s). This prevents a thundering herd of registration/deregistration events.

**Q4: How does Consul handle multi-datacenter service discovery?**
A: Each DC has its own Consul cluster (separate Raft group). Consul servers in different DCs communicate via WAN gossip (Serf). Cross-DC queries are forwarded to the remote DC's servers. DNS queries can specify DC: `nova-api.service.dc2.consul`. Response latency increases by cross-DC RTT. Data is not replicated; queries are forwarded on demand.

**Q5: What is the "thundering herd" problem in service discovery?**
A: When a service's DNS TTL expires, all clients re-resolve simultaneously, hitting the DNS server with a spike. Mitigation: (1) Jitter the TTL (return TTL +/- 20%). (2) Pre-fetch: resolve before TTL expires. (3) Stale-while-revalidate: serve stale DNS while refreshing in background. CoreDNS's `autopath` and `prefetch` plugins help.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO |
|---|---------|-----------|--------|----------|-----|
| 1 | Service discovery node crash | Raft heartbeat timeout | Cluster continues (if quorum maintained) | Node restarts; catches up from peers | 0 |
| 2 | Leader crash | Raft election | Brief inability to register/deregister (2-5s) | New leader elected; writes resume | 2-5s |
| 3 | All discovery nodes down | DNS resolution fails | All inter-service communication fails | Emergency restart; clients use last-known cache | 5-15 min (critical) |
| 4 | Health checker network partition | Checks fail for partitioned instances | False positive: healthy instances marked down | Self-preservation mode activates; stale data served | Self-preservation: 0 |
| 5 | Kubernetes API server down | k8s sync stops | k8s service changes not reflected in registry | Stale data served; resume sync when API server recovers | Duration of outage (stale) |
| 6 | DNS cache poisoning | Monitoring (unexpected IP in DNS) | Clients routed to wrong instances | Flush caches; investigate source | Minutes (manual) |
| 7 | Split-brain (two Raft leaders) | Impossible with correct Raft | N/A | N/A | N/A |
| 8 | Massive service registration storm | CPU/memory spike on leader | Registration latency increases | Back-pressure; rate limit registration API | Auto-recovers |

### Consensus & Coordination

**Why Raft for Service Registry:**
- Service registrations are writes; must be consistent (no duplicate registrations, no lost deregistrations).
- Read path (lookups) can be served from any node (follower reads with stale tolerance for DNS, linearizable reads for API).
- Small dataset (< 100 MB) fits easily in Raft state machine.

**Client-Side Resilience:**
```python
class ResilientServiceDiscoveryClient:
    """
    Client SDK with fallback mechanisms.
    """

    def __init__(self, discovery_endpoints, local_cache_path):
        self.endpoints = discovery_endpoints  # multiple nodes
        self.local_cache = DiskCache(local_cache_path)
        self.in_memory_cache = {}

    def resolve(self, service_name):
        """
        Three-level fallback:
        1. In-memory cache (populated by watch)
        2. Any discovery node (try all endpoints)
        3. On-disk cache (last known good)
        """
        # Level 1: in-memory cache
        cached = self.in_memory_cache.get(service_name)
        if cached and not cached.expired():
            return cached.instances

        # Level 2: query discovery cluster
        for endpoint in self.endpoints:
            try:
                result = self._query(endpoint, service_name)
                self.in_memory_cache[service_name] = result
                self.local_cache.put(service_name, result)  # persist
                return result.instances
            except Exception:
                continue

        # Level 3: disk cache (may be stale)
        stale = self.local_cache.get(service_name)
        if stale:
            log.warning(f"Using stale cache for {service_name}")
            return stale.instances

        raise ServiceDiscoveryUnavailableError(service_name)
```

---

## 10. Observability

### Key Metrics

| # | Metric | Type | Alert Threshold | Why |
|---|--------|------|-----------------|-----|
| 1 | `discovery.dns.queries.rate` | Counter | > 500K/sec | DNS query volume; capacity planning |
| 2 | `discovery.dns.latency.p99` | Histogram | > 5ms | DNS resolution slow |
| 3 | `discovery.registration.rate` | Counter | > 500/sec sustained | Registration storm (mass deploy or bug) |
| 4 | `discovery.deregistration.rate` | Counter | > 100/sec sustained | Mass failure or aggressive eviction |
| 5 | `discovery.health_check.failure_rate` | Gauge per service | > 20% | Service degradation |
| 6 | `discovery.instances.total` | Gauge | deviation > 10% from expected | Instances missing or duplicated |
| 7 | `discovery.instances.healthy_pct` | Gauge per service | < 70% | Service under capacity |
| 8 | `discovery.watch.subscribers` | Gauge | > 10K | Watch fan-out may become bottleneck |
| 9 | `discovery.self_preservation.active` | Boolean gauge | true | Network partition likely |
| 10 | `discovery.stale_cache.usage` | Counter | > 0 | Clients falling back to stale data |
| 11 | `discovery.raft.commit.latency` | Histogram | > 20ms | Raft cluster health |
| 12 | `discovery.k8s_sync.lag` | Gauge | > 30s | k8s sync falling behind |

---

## 11. Security

| Threat | Mitigation |
|--------|------------|
| Unauthorized service registration (impersonation) | mTLS: only services with valid certificates can register; certificate CN must match service name |
| Service discovery enumeration | ACL policies: service A can only discover services it's authorized to call |
| DNS spoofing | DNSSEC within infrastructure; DNS over TLS to discovery servers |
| Health check endpoint exploitation | Health check endpoints should be internal-only (not exposed via ingress) |
| Man-in-the-middle between client and registry | mTLS for all gRPC/HTTP communication; TLS for DNS (DoT) |
| Rogue node joins discovery cluster | Pre-shared bootstrap token; mutual TLS for Raft peers |

### Consul-Style ACL Policy

```hcl
# Service "nova-api" can register itself and discover MySQL + Kafka
service "nova-api" {
  policy = "write"      # Can register/deregister itself
}

service "mysql-primary" {
  policy = "read"       # Can discover MySQL
}

service "kafka-broker" {
  policy = "read"       # Can discover Kafka
}

# Deny all other services
service_prefix "" {
  policy = "deny"
}
```

---

## 12. Incremental Rollout Strategy

### Phase 1: Parallel Operation (Week 1-4)
- Deploy service discovery alongside existing mechanism (e.g., k8s-native + static configs).
- Register all services in both old and new systems.
- All lookups use old system; log what new system would return.
- Compare results; fix discrepancies.

### Phase 2: Read from New, Write to Both (Week 5-8)
- Lookups go to new system (with fallback to old on error).
- Registrations go to both systems.
- Canary: 2-3 non-critical services use new system exclusively.
- Monitor for correctness and latency.

### Phase 3: Migration (Week 9-16)
- Migrate services in dependency order (leaf services first, core services last).
- Each service team validates their service discovery is correct.
- Remove old-system registration for migrated services.

### Phase 4: Decommission Old System (Week 17-20)
- Remove old system.
- Update documentation and runbooks.

**Rollout Q&A:**

**Q1: What is the biggest risk during service discovery migration?**
A: The "discovery gap" — a service is deregistered from the old system before it's fully registered in the new system. During this gap, no one can find it. Mitigation: always register in the new system first, validate it's reachable, then deregister from the old system.

**Q2: How do you handle the chicken-and-egg problem during migration?**
A: If service A uses the new discovery and service B is only in the old discovery, A can't find B. Solution: during migration, the new discovery system imports entries from the old system (sync bridge). All services in the old system appear in the new system. Once migration is complete, remove the bridge.

**Q3: How do you validate service discovery correctness during rollout?**
A: (1) Automated test: for every service, resolve via old and new system; compare results. (2) Traffic mirror: route 1% of real DNS queries to the new system; compare responses. (3) End-to-end test: deploy a test service, register it, resolve it, call it. Run continuously.

**Q4: What if a service discovery failure causes a production outage during rollout?**
A: Instant rollback via feature flag (client SDK falls back to old system). DNS clients get TTL-expired old results. The rollback is per-service, not global — only the affected service reverts.

**Q5: How do you handle multi-language support during rollout?**
A: DNS interface is language-agnostic (works immediately). For rich features (watch, metadata), provide SDKs in Java (Spring Cloud integration) and Python. Other languages use the REST API directly. Prioritize SDK development for the most common languages first.

---

## 13. Trade-offs & Decision Log

| # | Decision | Options | Selected | Rationale |
|---|----------|---------|----------|-----------|
| 1 | Consistency model | CP (Consul) vs AP (Eureka) | CP for writes, AP-tolerant for reads | Registration must be consistent; lookups can tolerate brief staleness |
| 2 | Health check model | Server-side polling vs client heartbeat | Hybrid | Server-side for active verification; client TTL for services that can't be polled |
| 3 | DNS integration | DNS-only vs API-only vs both | Both | DNS for universal access; API for rich features |
| 4 | Registration model | Self-registration vs third-party | Platform-native (k8s) + sidecar (non-k8s) | Minimizes service-side code; leverages existing platform |
| 5 | Multi-DC | WAN replication vs federated | Federated (independent clusters + forwarding) | Lower latency within DC; cross-DC queries are explicit |
| 6 | Self-preservation | Enable vs disable | Enable with configurable threshold | Prevents mass eviction during partitions |
| 7 | DNS TTL | 0s vs 5s vs 30s | 5s | Balance between freshness and DNS load |
| 8 | Watch mechanism | Long-polling vs streaming | Streaming (gRPC server-sent) | Lower latency; more efficient for frequent changes |

---

## 14. Agentic AI Integration

**1. Intelligent Service Placement:**
```
Agent analyzes:
  - Service dependency graph (from discovery data)
  - Latency between zones (from health check data)
  - Traffic patterns (from DNS query logs)

Recommendations:
  - "nova-api has 80% of traffic from az-1 but only 30% of instances
    there. Recommend rebalancing to 50% az-1, 25% az-2, 25% az-3."
  - "mysql-primary and nova-api are in different zones, adding 2ms
    per query. Recommend co-locating in az-1."
```

**2. Automated Health Check Tuning:**
```python
class HealthCheckTuningAgent:
    def recommend_intervals(self, service_name):
        # Analyze health check history
        history = self.metrics.get_health_transitions(
            service_name, window="30d")

        flapping_rate = history.transitions_per_hour
        avg_failure_duration = history.avg_critical_duration

        if flapping_rate > 10:
            return {
                "recommendation": "Increase failure_threshold from "
                                  f"{current.failure_threshold} to "
                                  f"{current.failure_threshold + 2}",
                "reason": f"High flapping rate ({flapping_rate}/hr); "
                          "current threshold too sensitive"
            }

        if avg_failure_duration > 300:  # 5 minutes
            return {
                "recommendation": "Enable deregister_after=10m",
                "reason": f"Avg failure duration is {avg_failure_duration}s; "
                          "long-dead instances should be deregistered"
            }
```

**3. Dependency Graph Anomaly Detection:**
```
Agent builds service dependency graph from discovery queries:
  nova-api → mysql-primary, nova-compute, keystone, kafka

Detects anomalies:
  - "New dependency detected: nova-api → elasticsearch (first seen today).
    Was this intentional? No matching change ticket found."
  - "Circular dependency detected: service-a → service-b → service-c
    → service-a. This creates a deployment ordering problem."
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through what happens when a Kubernetes pod starts and becomes discoverable.**
A: (1) kubelet starts the pod containers. (2) Containers run startup probes (if defined). (3) Once startup probe passes, kubelet starts running readiness probe. (4) When readiness probe passes, kubelet reports the pod as Ready. (5) Endpoints controller sees the Ready pod, updates the Endpoints object to include the pod's IP. (6) kube-proxy watches Endpoints, updates iptables/IPVS rules. (7) CoreDNS watches Services, serves updated DNS. (8) Clients resolve DNS and can now reach the new pod.

**Q2: What is the difference between iptables and IPVS mode in kube-proxy?**
A: iptables mode: creates one iptables rule per service endpoint. O(N) rules. Probability-based load balancing (1/N, 1/(N-1), ...). Works well up to ~5K services. IPVS mode: uses Linux Virtual Server in kernel. O(1) lookup using hash tables. Supports multiple LB algorithms (round-robin, least-connections, weighted). Better for large clusters (> 5K services). Both intercept traffic to ClusterIPs and forward to pod IPs.

**Q3: How do you handle graceful shutdown in service discovery?**
A: (1) Pod receives SIGTERM. (2) k8s preStop hook runs (e.g., deregister from service discovery). (3) k8s removes pod from Endpoints (parallel with preStop). (4) Application stops accepting new connections. (5) Application drains in-flight requests (terminationGracePeriodSeconds). (6) Application exits. The key race: kube-proxy may still route traffic for a few seconds after the pod starts shutting down. Use a preStop sleep (2-5s) to allow iptables updates to propagate.

**Q4: What is the "stale endpoint" problem?**
A: After a pod is terminated, some clients still have the old IP cached (DNS TTL, connection pool). They send requests to the old IP, which either fails (connection refused) or hits a completely different pod (IP reused). Mitigation: long enough TTL for IP reuse (k8s default: 5 min CIDR rotation), short DNS TTL (5s), and client-side connection health checks.

**Q5: How does Netflix Eureka's peer-to-peer replication work?**
A: Eureka servers replicate asynchronously: each server forwards registrations/heartbeats to all peers. No consensus protocol. This means two servers can temporarily disagree about the service catalog. Eureka is AP: during a partition, both sides continue operating with potentially stale data. Consistency is eventual (30-second replication batch). This is fine for Netflix's use case (client-side load balancing with retries) but not for infrastructure control-plane where routing to a non-existent service causes errors.

**Q6: How do you implement locality-aware routing with service discovery?**
A: (1) Each instance is tagged with its zone/rack/DC in the registry. (2) Client SDK knows its own zone. (3) On resolve, prefer instances in the same zone (lowest latency). (4) If no healthy instances in the same zone, fall back to other zones. (5) Envoy/Istio implement this as "zone-aware routing" with configurable thresholds (e.g., only use local zone if > 80% healthy).

**Q7: What is the problem with long-lived gRPC connections and service discovery?**
A: gRPC uses HTTP/2 persistent connections. Once connected, the client doesn't re-resolve DNS. If the server scales or instances change, the client is still connected to the original set. Solution: (1) gRPC built-in round-robin policy with periodic re-resolution. (2) Envoy sidecar with xDS-based endpoint updates. (3) Periodic connection recycling (max connection age).

**Q8: How do you handle service discovery for external services (third-party APIs)?**
A: Register external services as "external" with static addresses and no health checking (or HTTP health check against the third-party's status page). This unifies internal and external service discovery. Example: register `stripe-api` with address `api.stripe.com:443` and a health check against `https://status.stripe.com/api/v2/status.json`.

**Q9: What is a "prepared query" in Consul?**
A: A saved query template with complex routing logic: failover to another DC if local instances are unhealthy, nearest-DC routing based on network coordinates, tag-based filtering. Clients reference the prepared query by name. This abstracts complex routing decisions from client code. Example: "Find the nearest healthy MySQL replica, preferring the local DC, failing over to dc2."

**Q10: How does service discovery interact with canary deployments?**
A: (1) Deploy canary instances with a `canary` tag. (2) Service discovery returns both production and canary instances. (3) Client-side or mesh-level routing splits traffic: 95% to production, 5% to canary. (4) Monitor canary metrics. (5) If healthy, promote canary (remove tag, update weight). (6) If unhealthy, deregister canary instances. The service discovery system provides the data; the routing decision is in the client/mesh.

**Q11: What is service mesh control plane vs data plane?**
A: Control plane (Istiod, Consul Connect server): maintains service catalog, distributes routing rules, manages certificates. Data plane (Envoy sidecar): intercepts traffic, applies routing rules, enforces mTLS, reports telemetry. Service discovery is primarily a control-plane function. Our system serves as part of the control plane, feeding endpoint data to the data plane (Envoy) via xDS APIs.

**Q12: How do you handle DNS resolution for services with multiple ports?**
A: SRV records. An SRV record contains: priority, weight, port, and target hostname. A service with HTTP (8080) and gRPC (9090) ports returns two SRV records with different port values. Client resolves the SRV record and connects to the appropriate port. Alternatively, use named ports in the A record query (`http.nova-api.service.consul`, `grpc.nova-api.service.consul`).

**Q13: What is the performance overhead of service discovery on the request path?**
A: With caching: ~0. DNS results are cached locally for TTL seconds. gRPC SDK caches resolved endpoints and watches for changes. Without caching: ~1-5ms per DNS resolution. The overhead is amortized across all requests to the same service (resolve once, use for all requests during TTL).

**Q14: How do you handle the "split-brain" scenario in service discovery?**
A: Raft prevents split-brain: the minority partition cannot write (register/deregister). But reads (lookups) can be served from followers. During a partition: majority side has authoritative data; minority side serves stale but valid data. Clients on the minority side can still route to services they already know about. New services registered during the partition are only available on the majority side until partition heals.

**Q15: Compare Kubernetes service discovery with Consul for a hybrid infrastructure.**
A: k8s: tightly integrated with k8s, zero configuration for in-cluster services, CoreDNS, kube-proxy/IPVS, limited to k8s pods. Consul: works across k8s and non-k8s, richer feature set (multi-DC, ACL, KV store, service mesh), requires separate deployment. For hybrid infra (k8s + bare-metal), Consul (or our system) is necessary. k8s-native discovery only covers k8s pods. Our system bridges both worlds.

---

## 16. References

1. Netflix. *Eureka: AWS Service Discovery*. https://github.com/Netflix/eureka
2. HashiCorp. *Consul Documentation*. https://www.consul.io/docs
3. Kubernetes. *Service and Endpoints*. https://kubernetes.io/docs/concepts/services-networking/service/
4. Kubernetes. *CoreDNS*. https://coredns.io/plugins/kubernetes/
5. Google. *The Chubby Lock Service* (includes service discovery aspects). OSDI 2006.
6. Burns, B. et al. (2016). *Borg, Omega, and Kubernetes*. ACM Queue.
7. Envoy. *xDS Protocol*. https://www.envoyproxy.io/docs/envoy/latest/api-docs/xds_protocol
8. gRPC. *Health Checking Protocol*. https://github.com/grpc/grpc/blob/master/doc/health-checking.md
9. RFC 2782. *A DNS RR for specifying the location of services (DNS SRV)*.
10. Lamport, L. (2001). *Paxos Made Simple*. ACM SIGACT News.
