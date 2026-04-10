# System Design: MySQL at Scale

> **Relevance to role:** Cloud infrastructure platform engineers build and operate MySQL as the backbone of metadata stores, service catalogs, tenant registries, billing systems, and configuration databases for IaaS platforms. This role requires deep understanding of MySQL replication topologies, sharding with Vitess, connection pooling (HikariCP/ProxySQL), query optimization, InnoDB internals, and CQRS patterns — all running on bare metal with strict latency SLAs.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|-------------|--------|
| FR-1 | CRUD operations | Transactional read/write with ACID guarantees |
| FR-2 | Complex queries | JOINs, subqueries, aggregations across related tables |
| FR-3 | Read scaling | Route read traffic to replicas without sacrificing consistency guarantees |
| FR-4 | Horizontal sharding | Partition data across multiple MySQL instances for write scaling |
| FR-5 | Schema evolution | Online DDL without downtime (add columns, change types, add indexes) |
| FR-6 | Connection pooling | Efficient connection management for Java microservices |
| FR-7 | Backup and recovery | Point-in-time recovery with sub-minute RPO |
| FR-8 | Multi-tenancy | Per-tenant data isolation with shared infrastructure |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Write latency | p50 < 5 ms, p99 < 20 ms |
| NFR-2 | Read latency | p50 < 2 ms, p99 < 10 ms |
| NFR-3 | Write throughput | 50K writes/sec across cluster |
| NFR-4 | Read throughput | 200K reads/sec across cluster |
| NFR-5 | Availability | 99.99% (< 52 min downtime/year) |
| NFR-6 | Durability | Zero data loss (RPO = 0 with semi-sync replication) |
| NFR-7 | Max dataset size | 10 TB per shard, 100 TB total |
| NFR-8 | Max connections | 10K concurrent from Java services |

### Constraints & Assumptions
- Bare-metal servers: 64 cores, 512 GB RAM, NVMe SSDs (2× 4 TB RAID-1)
- MySQL 8.0 with InnoDB engine
- Java services using Spring Data JPA / Hibernate with HikariCP
- Python services using SQLAlchemy
- ProxySQL for connection routing and pooling
- Vitess for horizontal sharding
- Kubernetes for service deployment

### Out of Scope
- PostgreSQL migration analysis
- NewSQL databases (CockroachDB, TiDB)
- OLAP workloads (use ClickHouse/Trino)
- Full-text search (use Elasticsearch)

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Java microservices | 50 services × 20 pods | 1,000 pods |
| Connections per pod (HikariCP) | 10 connections per pool | 10,000 total connections |
| Read queries per second | 1,000 pods × 200 reads/sec per pod | 200K reads/sec |
| Write queries per second | 1,000 pods × 50 writes/sec per pod | 50K writes/sec |
| Peak multiplier | 2× average | 400K reads/sec, 100K writes/sec |
| Transactions per second | 30% of writes are multi-statement TXNs | ~15K TPS |

### Latency Requirements

| Operation | p50 | p99 |
|-----------|-----|-----|
| Point read (by PK) | 0.5 ms | 2 ms |
| Secondary index read | 1 ms | 5 ms |
| Range scan (100 rows) | 2 ms | 10 ms |
| Single-row insert | 1 ms | 5 ms |
| Multi-row insert (100) | 5 ms | 20 ms |
| Simple JOIN (2 tables) | 3 ms | 15 ms |
| Complex aggregation | 50 ms | 500 ms |
| Schema migration (online DDL) | N/A (background) | < 24h for 1 TB table |

### Storage Estimates

| Component | Calculation | Value |
|-----------|-------------|-------|
| Total dataset size | 100 tables, avg 1 TB per shard | ~10 TB per shard |
| Number of shards | 100 TB total / 10 TB per shard | 10 shards |
| Replication overhead | 10 shards × 3 replicas × 10 TB | 300 TB total |
| Binary log retention | 7 days × 50K writes/sec × 200 bytes avg | ~6 TB |
| Buffer pool size | 70% of 512 GB RAM | 358 GB per server |
| InnoDB log files | 2 × 2 GB (default; tuned to 4-8 GB) | 4-8 GB per server |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| Client → ProxySQL | 200K reads × 1 KB + 50K writes × 500 B | ~225 MB/s |
| ProxySQL → MySQL | Same as above + connection overhead | ~250 MB/s |
| Replication (binlog) | 50K writes/sec × 200 B per event | ~10 MB/s per replica |
| Backup (streaming) | 10 TB / 8 hours = nightly window | ~350 MB/s |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                     APPLICATION LAYER                                │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ Java Microservices (Spring Boot + Spring Data JPA)           │   │
│  │                                                              │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │   │
│  │  │ HikariCP     │  │ HikariCP     │  │ HikariCP     │      │   │
│  │  │ Pool (10 conn)│  │ Pool (10 conn)│  │ Pool (10 conn)│      │   │
│  │  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │   │
│  └─────────┼─────────────────┼─────────────────┼───────────────┘   │
│            │                 │                 │                     │
│  ┌─────────▼─────────────────▼─────────────────▼───────────────┐   │
│  │ Python Services (SQLAlchemy + connection pool)               │   │
│  └─────────┬───────────────────────────────────────────────────┘   │
└────────────┼───────────────────────────────────────────────────────┘
             │
┌────────────▼───────────────────────────────────────────────────────┐
│                     PROXY / ROUTING LAYER                            │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │              Vitess (VTGate Cluster)                          │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐                  │   │
│  │  │ VTGate   │  │ VTGate   │  │ VTGate   │  (stateless,     │   │
│  │  │          │  │          │  │          │   query routing)  │   │
│  │  └────┬─────┘  └────┬─────┘  └────┬─────┘                  │   │
│  └───────┼──────────────┼──────────────┼────────────────────────┘   │
│          │              │              │                             │
│  ┌───────┼──────────────┼──────────────┼────────────────────────┐   │
│  │       │    ProxySQL (per-shard read/write splitting)         │   │
│  │  ┌────▼────┐  ┌─────▼────┐  ┌──────▼───┐                   │   │
│  │  │ ProxySQL│  │ ProxySQL │  │ ProxySQL  │  (read/write      │   │
│  │  │ (shard1)│  │ (shard2) │  │ (shard3)  │   routing)        │   │
│  │  └────┬────┘  └─────┬────┘  └──────┬───┘                   │   │
│  └───────┼──────────────┼──────────────┼────────────────────────┘   │
└──────────┼──────────────┼──────────────┼────────────────────────────┘
           │              │              │
┌──────────▼──────────────▼──────────────▼────────────────────────────┐
│                     MYSQL SHARD LAYER                                 │
│                                                                      │
│  Shard 1                    Shard 2            ...     Shard N       │
│  ┌─────────────────────┐  ┌──────────────┐         ┌──────────┐    │
│  │ VTTablet (Primary)  │  │ VTTablet (P) │         │ VTTablet │    │
│  │ ┌────────────────┐  │  │ ┌──────────┐ │         │          │    │
│  │ │ MySQL Primary  │  │  │ │ MySQL P  │ │         │          │    │
│  │ │ (read + write) │  │  │ │          │ │         │          │    │
│  │ └────────────────┘  │  │ └──────────┘ │         │          │    │
│  ├─────────────────────┤  ├──────────────┤         ├──────────┤    │
│  │ VTTablet (Replica)  │  │ VTTablet (R) │         │ VTTablet │    │
│  │ ┌────────────────┐  │  │ ┌──────────┐ │         │          │    │
│  │ │ MySQL Replica  │──┤  │ │ MySQL R  │ │         │          │    │
│  │ │ (read-only)    │  │  │ │          │ │         │          │    │
│  │ └────────────────┘  │  │ └──────────┘ │         │          │    │
│  ├─────────────────────┤  ├──────────────┤         ├──────────┤    │
│  │ VTTablet (Replica)  │  │ VTTablet (R) │         │ VTTablet │    │
│  │ ┌────────────────┐  │  │ ┌──────────┐ │         │          │    │
│  │ │ MySQL Replica  │  │  │ │ MySQL R  │ │         │          │    │
│  │ │ (read-only)    │  │  │ │          │ │         │          │    │
│  │ └────────────────┘  │  │ └──────────┘ │         │          │    │
│  └─────────────────────┘  └──────────────┘         └──────────┘    │
│                                                                      │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ Vitess Topology (etcd): shard map, tablet health, vschema   │   │
│  └──────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **HikariCP** | JDBC connection pool in Java services; maintains pre-warmed connections; validates on borrow; configurable max pool size, connection timeout, idle timeout |
| **VTGate** | Vitess query router; parses SQL, consults vschema for shard routing, fans out queries to VTTablets; handles scatter-gather for cross-shard queries |
| **VTTablet** | Vitess shard agent; manages a single MySQL instance; handles query rewriting, connection pooling to MySQL, health checking, replication management |
| **ProxySQL** | MySQL-aware proxy; read/write splitting (writes → primary, reads → replicas); connection multiplexing; query caching; failover detection |
| **MySQL Primary** | Accepts all writes; source of binlog replication; InnoDB storage engine |
| **MySQL Replica** | Receives binlog from primary; serves read-only queries; semi-sync or async replication |
| **Vitess Topology (etcd)** | Stores cluster metadata: which shards exist, which tablet is primary/replica, vschema (sharding configuration) |

### Data Flows

**Write Path:**
1. Java service → HikariCP → VTGate
2. VTGate parses SQL, determines shard from WHERE clause (sharding key in vschema)
3. VTGate → VTTablet (primary) for the target shard
4. VTTablet → MySQL primary: execute SQL
5. MySQL writes to InnoDB buffer pool + redo log (committed)
6. Semi-sync replication: binlog sent to replica; at least 1 replica ACKs
7. MySQL returns OK to VTTablet → VTGate → HikariCP → Java service

**Read Path (CQRS):**
1. Java service marks query as read-only (Spring `@Transactional(readOnly = true)`)
2. HikariCP → VTGate (with read-only hint)
3. VTGate → VTTablet (replica) for the target shard
4. VTTablet → MySQL replica: execute SQL
5. Result returned through the chain

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Example: IaaS Platform Metadata Database
-- Sharded by tenant_id using Vitess

CREATE TABLE tenants (
    tenant_id       BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    name            VARCHAR(255) NOT NULL,
    plan            ENUM('free','standard','enterprise') NOT NULL DEFAULT 'standard',
    status          ENUM('active','suspended','deleted') NOT NULL DEFAULT 'active',
    created_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    PRIMARY KEY (tenant_id),
    UNIQUE KEY idx_name (name),
    KEY idx_status_created (status, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE instances (
    instance_id     BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    tenant_id       BIGINT UNSIGNED NOT NULL,
    name            VARCHAR(255) NOT NULL,
    flavor_id       INT UNSIGNED NOT NULL,
    image_id        BIGINT UNSIGNED NOT NULL,
    status          ENUM('building','active','shutoff','error','deleted') NOT NULL,
    host_id         BIGINT UNSIGNED,
    ip_address      VARBINARY(16),                 -- IPv4 or IPv6 binary
    created_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    deleted_at      DATETIME(6) DEFAULT NULL,
    PRIMARY KEY (instance_id),
    KEY idx_tenant_status (tenant_id, status),
    KEY idx_host (host_id),
    KEY idx_created (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE volumes (
    volume_id       BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    tenant_id       BIGINT UNSIGNED NOT NULL,
    instance_id     BIGINT UNSIGNED,               -- NULL if detached
    name            VARCHAR(255) NOT NULL,
    size_gb         INT UNSIGNED NOT NULL,
    volume_type     ENUM('ssd','hdd','nvme') NOT NULL DEFAULT 'ssd',
    status          ENUM('creating','available','in-use','deleting','error') NOT NULL,
    created_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (volume_id),
    KEY idx_tenant (tenant_id),
    KEY idx_instance (instance_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Audit log (append-only, time-partitioned)
CREATE TABLE audit_log (
    log_id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    tenant_id       BIGINT UNSIGNED NOT NULL,
    actor_id        BIGINT UNSIGNED NOT NULL,
    action          VARCHAR(64) NOT NULL,
    resource_type   VARCHAR(64) NOT NULL,
    resource_id     BIGINT UNSIGNED NOT NULL,
    details         JSON,
    created_at      DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (log_id),
    KEY idx_tenant_created (tenant_id, created_at),
    KEY idx_resource (resource_type, resource_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
PARTITION BY RANGE (UNIX_TIMESTAMP(created_at)) (
    PARTITION p202401 VALUES LESS THAN (UNIX_TIMESTAMP('2024-02-01')),
    PARTITION p202402 VALUES LESS THAN (UNIX_TIMESTAMP('2024-03-01')),
    -- ... monthly partitions
    PARTITION pmax VALUES LESS THAN MAXVALUE
);
```

**Vitess VSchema (sharding configuration):**
```json
{
  "sharded": true,
  "vindexes": {
    "hash_tenant": {
      "type": "hash"
    },
    "tenant_lookup": {
      "type": "consistent_lookup_unique",
      "params": {
        "table": "tenant_lookup",
        "from": "name",
        "to": "tenant_id"
      },
      "owner": "tenants"
    }
  },
  "tables": {
    "tenants": {
      "column_vindexes": [
        { "column": "tenant_id", "name": "hash_tenant" },
        { "column": "name", "name": "tenant_lookup" }
      ]
    },
    "instances": {
      "column_vindexes": [
        { "column": "tenant_id", "name": "hash_tenant" }
      ]
    },
    "volumes": {
      "column_vindexes": [
        { "column": "tenant_id", "name": "hash_tenant" }
      ]
    },
    "audit_log": {
      "column_vindexes": [
        { "column": "tenant_id", "name": "hash_tenant" }
      ]
    }
  }
}
```

### Database/Storage Selection

| Component | Technology | Rationale |
|-----------|-----------|-----------|
| Primary OLTP | MySQL 8.0 InnoDB | ACID, mature ecosystem, well-understood operations |
| Sharding middleware | Vitess | Online resharding, connection pooling, query routing; battle-tested at YouTube |
| Read routing | ProxySQL (within each shard) | Read/write splitting, connection multiplexing, query cache |
| Connection pool (Java) | HikariCP | Fastest JDBC pool; Spring Boot default |
| Connection pool (Python) | SQLAlchemy + pool | Built-in connection pooling |
| Topology store | etcd (Vitess topology) | Distributed, strongly consistent KV for shard metadata |

### Indexing Strategy

**Index design principles:**
1. **Primary key:** Always `BIGINT UNSIGNED AUTO_INCREMENT` — InnoDB clusters data by PK; sequential inserts avoid page splits
2. **Composite indexes:** Put high-selectivity columns first (e.g., `(tenant_id, status)` not `(status, tenant_id)`)
3. **Covering indexes:** Include all columns needed by a query to avoid table lookups
4. **Prefix indexes:** For VARCHAR columns: `KEY idx_name (name(50))` — enough selectivity without full index
5. **Invisible indexes (MySQL 8.0):** Test dropping an index by making it invisible first: `ALTER TABLE t ALTER INDEX idx INVISIBLE`

```sql
-- Covering index example: avoid table lookup for common query
-- Query: SELECT instance_id, name, status FROM instances WHERE tenant_id = ? AND status = 'active'
CREATE INDEX idx_tenant_status_covering
    ON instances (tenant_id, status, instance_id, name);
-- InnoDB stores PK in every secondary index, so instance_id is "free"
-- Adding 'name' makes it a covering index for this query

-- Explain plan verification
EXPLAIN SELECT instance_id, name, status
FROM instances
WHERE tenant_id = 12345 AND status = 'active';
-- Should show: Using index (covering index, no table access)
```

---

## 5. API Design

### Database Access Patterns (Java/Spring Data JPA)

```java
/**
 * Spring Data JPA Repository with optimized queries.
 * Demonstrates: N+1 prevention, pagination, read-only transactions,
 * and Vitess-compatible patterns.
 */
@Repository
public interface InstanceRepository extends JpaRepository<Instance, Long> {

    /**
     * Fetch instances with eager join to avoid N+1.
     * The @EntityGraph ensures volumes are loaded in the same query.
     */
    @EntityGraph(attributePaths = {"volumes"})
    @Query("SELECT i FROM Instance i WHERE i.tenantId = :tenantId AND i.status = :status")
    List<Instance> findByTenantAndStatus(
        @Param("tenantId") Long tenantId,
        @Param("status") InstanceStatus status);

    /**
     * Paginated query for dashboard.
     * Uses keyset pagination (search_after) instead of OFFSET for efficiency.
     */
    @Query("SELECT i FROM Instance i WHERE i.tenantId = :tenantId " +
           "AND i.createdAt < :cursor ORDER BY i.createdAt DESC")
    List<Instance> findByTenantPagedByCursor(
        @Param("tenantId") Long tenantId,
        @Param("cursor") Instant cursor,
        Pageable pageable);

    /**
     * Count query with index optimization.
     * Uses covering index idx_tenant_status.
     */
    @Query("SELECT COUNT(i) FROM Instance i WHERE i.tenantId = :tenantId AND i.status = :status")
    long countByTenantAndStatus(
        @Param("tenantId") Long tenantId,
        @Param("status") InstanceStatus status);
}

/**
 * Service layer demonstrating CQRS pattern, transaction management,
 * and read/write separation.
 */
@Service
public class InstanceService {

    private final InstanceRepository instanceRepo;
    private final VolumeRepository volumeRepo;
    private final AuditLogRepository auditRepo;
    private final ApplicationEventPublisher eventPublisher;

    /**
     * Write operation: create instance.
     * Transaction on primary with semi-sync replication.
     */
    @Transactional(propagation = Propagation.REQUIRED,
                   isolation = Isolation.READ_COMMITTED,
                   timeout = 5)  // 5 second timeout
    public Instance createInstance(CreateInstanceRequest req) {
        // Validate tenant exists and has quota
        Tenant tenant = tenantRepo.findById(req.getTenantId())
            .orElseThrow(() -> new TenantNotFoundException(req.getTenantId()));

        long currentCount = instanceRepo.countByTenantAndStatus(
            req.getTenantId(), InstanceStatus.ACTIVE);
        if (currentCount >= tenant.getQuota().getMaxInstances()) {
            throw new QuotaExceededException("Instance limit reached");
        }

        // Create instance
        Instance instance = Instance.builder()
            .tenantId(req.getTenantId())
            .name(req.getName())
            .flavorId(req.getFlavorId())
            .imageId(req.getImageId())
            .status(InstanceStatus.BUILDING)
            .build();

        instance = instanceRepo.save(instance);

        // Audit log (same transaction — same shard because same tenant_id)
        auditRepo.save(AuditLog.builder()
            .tenantId(req.getTenantId())
            .actorId(req.getActorId())
            .action("CREATE")
            .resourceType("instance")
            .resourceId(instance.getInstanceId())
            .details(Map.of("flavor", req.getFlavorId(), "image", req.getImageId()))
            .build());

        // Publish event for async processing (scheduling on host)
        eventPublisher.publishEvent(new InstanceCreatedEvent(instance));

        return instance;
    }

    /**
     * Read operation: uses replica via @Transactional(readOnly=true).
     * Spring/HikariCP/ProxySQL route this to a read replica.
     */
    @Transactional(readOnly = true)
    public Page<InstanceDTO> listInstances(Long tenantId, Instant cursor, int pageSize) {
        Pageable pageable = PageRequest.of(0, pageSize);

        List<Instance> instances;
        if (cursor == null) {
            instances = instanceRepo.findByTenantAndStatus(
                tenantId, InstanceStatus.ACTIVE);
        } else {
            instances = instanceRepo.findByTenantPagedByCursor(
                tenantId, cursor, pageable);
        }

        return new PageImpl<>(
            instances.stream().map(this::toDTO).collect(Collectors.toList()));
    }
}
```

**HikariCP Configuration:**
```yaml
# application.yml — HikariCP settings for MySQL via Vitess
spring:
  datasource:
    url: jdbc:mysql://vtgate-host:3306/keyspace
    username: app_user
    password: ${DB_PASSWORD}
    driver-class-name: com.mysql.cj.jdbc.Driver
    hikari:
      pool-name: instance-service-pool
      maximum-pool-size: 10         # Connections per pod
      minimum-idle: 5               # Keep 5 idle connections warm
      idle-timeout: 300000          # 5 min idle before close
      max-lifetime: 1800000         # 30 min max connection age
      connection-timeout: 3000      # 3s to get connection from pool
      validation-timeout: 1000      # 1s for connection validation
      connection-test-query: "SELECT 1"  # Validation query
      leak-detection-threshold: 30000  # Warn if connection held > 30s
      data-source-properties:
        cachePrepStmts: true
        prepStmtCacheSize: 250
        prepStmtCacheSqlLimit: 2048
        useServerPrepStmts: true
        useLocalSessionState: true
        rewriteBatchedStatements: true
        cacheResultSetMetadata: true
        cacheServerConfiguration: true
        elideSetAutoCommits: true
        maintainTimeStats: false
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Replication Topologies and Failover

**Why it's hard:** MySQL replication is the foundation for read scaling and high availability. But replication lag, split-brain during failover, and data loss during primary failure are all real risks. Choosing between async, semi-sync, and group replication involves trade-offs between latency, durability, and availability.

**Approaches:**

| Approach | Data Loss Risk | Write Latency | Failover Time | Complexity |
|----------|---------------|---------------|---------------|-----------|
| Async replication | High (uncommitted on replica) | Lowest | Manual or slow auto | Low |
| Semi-synchronous replication | Zero (at least 1 replica ACK) | +1-2 ms per write | Automated (Vitess) | Medium |
| MySQL Group Replication (MGR) | Zero (Paxos consensus) | +2-5 ms per write | Automatic (< 10s) | High |
| Galera Cluster (synchronous) | Zero (certification-based) | +5-10 ms per write | Automatic | High |
| GTID-based replication | Same as underlying mode | Same | Faster (GTID positioning) | Medium |

**Selected approach:** Semi-synchronous replication with GTID, managed by Vitess for automated failover.

**Justification:**
- Semi-sync guarantees at least 1 replica has the transaction before the primary ACKs the client (RPO=0)
- GTID simplifies failover: new primary knows exactly which transactions each replica has
- Vitess orchestrates failover: detects primary failure, promotes the most up-to-date replica, reconfigures routing
- Lower latency than Group Replication; simpler operations than Galera

**Implementation Detail:**

```sql
-- MySQL 8.0 Semi-Synchronous Replication Configuration

-- On PRIMARY:
SET GLOBAL rpl_semi_sync_master_enabled = 1;
SET GLOBAL rpl_semi_sync_master_timeout = 1000;  -- 1s timeout; fallback to async
SET GLOBAL rpl_semi_sync_master_wait_for_slave_count = 1;  -- Wait for 1 replica ACK
SET GLOBAL rpl_semi_sync_master_wait_point = 'AFTER_SYNC';
-- AFTER_SYNC: transaction committed to storage engine AND binlog synced
-- before waiting for replica ACK. This means on failover, the new primary
-- has the transaction (semi-sync guarantee).

-- GTID-based replication:
SET GLOBAL gtid_mode = ON;
SET GLOBAL enforce_gtid_consistency = ON;
-- GTID format: server_uuid:transaction_id (e.g., 3E11FA47-71CA-11E1-9E33-C80AA9429562:23)

-- On REPLICA:
CHANGE REPLICATION SOURCE TO
    SOURCE_HOST = 'primary-host',
    SOURCE_PORT = 3306,
    SOURCE_USER = 'repl_user',
    SOURCE_PASSWORD = 'repl_password',
    SOURCE_AUTO_POSITION = 1,  -- Use GTID for positioning
    GET_SOURCE_PUBLIC_KEY = 1;

SET GLOBAL rpl_semi_sync_slave_enabled = 1;
START REPLICA;
```

```java
/**
 * Vitess-managed failover: when primary fails, Vitess:
 * 1. Detects failure via health check (tablet_health_check_interval)
 * 2. Selects most up-to-date replica (by GTID)
 * 3. Promotes replica to primary (STOP REPLICA; RESET REPLICA ALL)
 * 4. Reconfigures other replicas to replicate from new primary
 * 5. Updates topology (etcd) so VTGate routes to new primary
 * 
 * This process takes < 30 seconds.
 */

/**
 * Application-side handling of failover:
 * HikariCP detects broken connections and acquires new ones from VTGate.
 * VTGate routes to the new primary transparently.
 * Application retries failed transactions.
 */
@Configuration
public class RetryConfiguration {

    @Bean
    public RetryTemplate mysqlRetryTemplate() {
        RetryTemplate template = new RetryTemplate();

        // Retry on transient MySQL errors (connection lost, deadlock)
        Map<Class<? extends Throwable>, Boolean> retryable = new HashMap<>();
        retryable.put(TransientDataAccessException.class, true);
        retryable.put(DeadlockLoserDataAccessException.class, true);
        retryable.put(CannotAcquireLockException.class, true);

        template.setRetryPolicy(new SimpleRetryPolicy(3, retryable));
        template.setBackOffPolicy(new ExponentialBackOffPolicy() {{
            setInitialInterval(100);
            setMultiplier(2.0);
            setMaxInterval(1000);
        }});

        return template;
    }
}

@Service
public class ResilientInstanceService {

    @Autowired private RetryTemplate retryTemplate;
    @Autowired private InstanceService instanceService;

    public Instance createInstanceWithRetry(CreateInstanceRequest req) {
        return retryTemplate.execute(ctx -> {
            if (ctx.getRetryCount() > 0) {
                log.warn("Retrying createInstance attempt #{}", ctx.getRetryCount());
            }
            return instanceService.createInstance(req);
        });
    }
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Primary crash | Writes blocked until failover | Vitess automated failover (< 30s); application retry |
| Semi-sync timeout (no replica ACK in 1s) | Falls back to async (data loss risk) | Monitor `Rpl_semi_sync_master_no_tx`; alert on fallback; multiple replicas |
| Replica lag > 5s | Stale reads from replicas | ProxySQL `max_replication_lag` check; route to primary if lag too high |
| Split brain (two primaries) | Data divergence | Vitess fencing: old primary is killed before new primary is promoted |
| GTID hole (missing transaction) | Replica cannot catch up | Restore replica from backup + binlog replay |
| Network partition (primary isolated) | Primary thinks it's still primary | Semi-sync timeout detection; Vitess health checks; fencing |

**Interviewer Q&As:**

**Q1: What's the difference between AFTER_SYNC and AFTER_COMMIT in semi-sync?**
A: `AFTER_SYNC` (default in MySQL 5.7.2+): primary waits for replica ACK after flushing binlog but before committing to storage engine. On primary crash, the replica has the transaction but the primary may not — no data loss on failover. `AFTER_COMMIT`: primary commits first, then waits. If primary crashes after commit but before replica ACK, the transaction is on the primary (lost if primary disk dies) but not on the replica — potential data loss.

**Q2: How does GTID simplify failover compared to binlog position?**
A: With binlog file:position, each replica is at a different position in a different binlog file. Promoting a replica requires calculating which binlog events the new primary has that other replicas don't. With GTID, each transaction has a globally unique ID. The new primary and replicas can simply compare GTID sets to determine which transactions are missing. `SOURCE_AUTO_POSITION = 1` handles this automatically.

**Q3: What happens when semi-sync falls back to async?**
A: If no replica ACKs within `rpl_semi_sync_master_timeout` (default 10s, we set 1s), the primary falls back to async replication — writes proceed without waiting. This is a safety valve to prevent writes from blocking indefinitely. However, writes during this window are vulnerable to data loss if the primary crashes. Monitor `Rpl_semi_sync_master_no_tx` and alert immediately.

**Q4: How does Vitess choose which replica to promote?**
A: Vitess (via `PlannedReparent` or `EmergencyReparent`) checks each replica's GTID set. It promotes the replica whose GTID set is the superset (most up-to-date). If multiple replicas are equally caught up, it prefers the one with lowest replication lag. Vitess also respects cell/rack preferences to keep the primary in a preferred location.

**Q5: How do you handle long-running transactions during failover?**
A: Long transactions on the old primary are lost on crash (uncommitted). On the new primary, they never existed. Applications must retry failed transactions. Best practice: keep transactions short (< 1 second). Break long operations into batches. Use idempotency keys to safely retry.

**Q6: What is replication lag and how do you manage it?**
A: Replication lag is the time difference between the primary committing a transaction and the replica applying it. Causes: (1) heavy write load, (2) single-threaded replication applier (pre-MySQL 5.7), (3) large transactions, (4) replica disk I/O. MySQL 8.0 parallel replication (`replica_parallel_workers=16`) applies transactions in parallel by writeset dependency tracking. ProxySQL monitors lag and routes reads to primary when lag exceeds threshold.

---

### Deep Dive 2: Sharding with Vitess

**Why it's hard:** A single MySQL instance can handle ~10 TB and ~20K writes/sec before performance degrades. Horizontal sharding distributes data across multiple instances, but introduces complexity: cross-shard queries, distributed transactions, resharding without downtime, and maintaining referential integrity.

**Approaches:**

| Approach | Cross-Shard Queries | Online Resharding | Application Changes | Maturity |
|----------|--------------------|--------------------|---------------------|----------|
| Application-level sharding | App must handle routing | Manual, complex | Extensive | N/A |
| Vitess | Yes (scatter-gather) | Built-in (VReplication) | Minimal (SQL-compatible) | High (YouTube-scale) |
| ShardingSphere | Yes | Supported | Moderate | Medium |
| ProxySQL alone | No built-in sharding | No | N/A | N/A (not a sharding solution) |
| Citus (PostgreSQL) | Yes | Yes | Minimal | High (but PostgreSQL only) |

**Selected approach:** Vitess.

**Justification:**
- Battle-tested at YouTube/Slack/GitHub/Square
- Online resharding via VReplication (no downtime)
- MySQL wire-protocol compatible — Java/Python apps connect as if to regular MySQL
- VTGate handles query routing, scatter-gather for cross-shard queries
- Schema management across shards

**Implementation Detail:**

```python
class VitessShardingDesign:
    """
    Vitess sharding architecture for our IaaS platform.
    
    Sharding key: tenant_id (hash vindex)
    
    Why tenant_id?
    1. All queries include tenant_id (multi-tenant platform)
    2. JOINs between tables within a tenant land on the same shard
    3. Tenant isolation: one tenant's load doesn't affect another's shard
    
    Shard layout:
    - Initial: 4 shards (-40, 40-80, 80-c0, c0-)
    - Capacity per shard: ~25 TB data, ~12.5K writes/sec
    - Resharding: split a hot shard (e.g., -40 → -20, 20-40) without downtime
    """

    INITIAL_SHARDS = ['-40', '40-80', '80-c0', 'c0-']

    @staticmethod
    def explain_vreplication_resharding():
        """
        Vitess resharding workflow (splitting shard -40 into -20 and 20-40):
        
        Phase 1: Create target shards (-20, 20-40) with empty MySQL instances
        
        Phase 2: VReplication starts copying data from source (-40) to targets
                  - Historical data: bulk copy (SwitchTraffic -10% at a time)
                  - Live changes: VStream (binlog-based CDC) replicates in real-time
        
        Phase 3: Verify data consistency (VDiff)
                  - Compare checksums between source and targets
        
        Phase 4: Switch reads to target shards
                  - VTGate routes read queries to target shards
                  - Source shard still handles writes
        
        Phase 5: Switch writes to target shards
                  - Brief (< 1s) write pause while VTGate switches routing
                  - VReplication catches up final transactions
                  - Writes resume on target shards
        
        Phase 6: Clean up
                  - Delete source shard data
                  - Update vschema
        """
        pass


class VitessQueryRouting:
    """
    How VTGate routes queries to shards.
    """

    @staticmethod
    def examples():
        """
        1. Single-shard query (most efficient):
           SELECT * FROM instances WHERE tenant_id = 123 AND status = 'active'
           → VTGate hashes tenant_id=123, routes to shard 40-80
        
        2. Scatter-gather query (cross-shard):
           SELECT COUNT(*) FROM instances WHERE status = 'error'
           → VTGate sends to ALL shards, collects counts, sums
        
        3. Cross-shard JOIN (avoided by design):
           SELECT t.name, i.name FROM tenants t JOIN instances i
             ON t.tenant_id = i.tenant_id WHERE t.tenant_id = 123
           → Same shard (both tables sharded by tenant_id) — single shard!
        
        4. Lookup vindex (name → tenant_id):
           SELECT * FROM tenants WHERE name = 'acme-corp'
           → VTGate checks lookup table: name='acme-corp' → tenant_id=123
           → Routes to shard for tenant_id=123
        
        5. Sequence (auto-increment across shards):
           INSERT INTO instances (tenant_id, name, ...) VALUES (123, 'web-1', ...)
           → VTGate gets next instance_id from sequence table (unsharded)
           → Inserts into shard for tenant_id=123 with the assigned instance_id
        """
        pass
```

```sql
-- Vitess resharding commands (vtctld CLI)

-- Step 1: Create target shards
vtctlclient CreateShard -- keyspace/shard
    commerce/-20
vtctlclient CreateShard -- keyspace/shard
    commerce/20-40

-- Step 2: Initialize tablets for new shards
vtctlclient InitShardPrimary commerce/-20 zone1-200
vtctlclient InitShardPrimary commerce/20-40 zone1-201

-- Step 3: Start VReplication (copy + stream)
vtctlclient Reshard -- source_shards='-40' -- target_shards='-20,20-40'
    commerce.reshard_workflow

-- Step 4: Monitor progress
vtctlclient VReplicationExec -- json zone1-200
    'select * from _vt.vreplication'

-- Step 5: Verify data consistency
vtctlclient VDiff commerce.reshard_workflow

-- Step 6: Switch reads
vtctlclient SwitchReads -- tablet_types=rdonly,replica
    commerce.reshard_workflow

-- Step 7: Switch writes (brief pause)
vtctlclient SwitchWrites commerce.reshard_workflow

-- Step 8: Clean up old shard
vtctlclient DropSources commerce.reshard_workflow
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| VTGate failure | Applications lose routing | Multiple VTGate instances behind LB; stateless |
| VTTablet failure | Shard unavailable until restart or failover | Vitess automatic tablet failover |
| VReplication lag during resharding | Longer switch-over pause | Monitor VReplication lag; wait for < 1s lag before switching |
| Cross-shard query timeout | Application error | Design queries to be single-shard; use scatter-gather only for aggregations |
| Lookup vindex inconsistency | Query routed to wrong shard | Lookup table is strongly consistent (sharded separately); VReplication maintains it |
| Schema mismatch across shards | Query failures on some shards | Vitess ApplySchema applies DDL to all shards atomically |

**Interviewer Q&As:**

**Q1: Why shard by tenant_id instead of by table?**
A: Tenant_id is the most common query filter (every API call includes it). Sharding by tenant_id means all of a tenant's data (instances, volumes, audit_log) lands on the same shard, enabling efficient single-shard JOINs. Sharding by table would require cross-shard JOINs for every query involving related tables.

**Q2: How does Vitess handle auto-increment IDs across shards?**
A: Vitess uses a sequence table (on an unsharded keyspace or a designated shard). VTGate fetches blocks of IDs from the sequence (e.g., 1000 at a time) and assigns them to inserts. This ensures globally unique IDs without cross-shard coordination on every insert.

**Q3: What happens during the write switch in resharding?**
A: VTGate briefly pauses writes (< 1s). During this pause: (1) VReplication catches up the remaining transactions. (2) VTGate atomically updates the routing table to point to the new shards. (3) Writes resume on the new shards. The pause is typically < 500ms. Applications experience a brief latency spike, not an error (if connection timeout > 1s).

**Q4: How do you handle a "large tenant" that outgrows a single shard?**
A: (1) If the tenant's data exceeds the shard capacity, reshard to give that tenant a dedicated shard (or multiple). (2) Vitess supports "custom sharding" where specific tenant_id ranges map to specific shards. (3) For extreme cases, shard within the tenant by a secondary key (e.g., `resource_type`).

**Q5: How does Vitess handle cross-shard transactions?**
A: Vitess supports two-phase commit (2PC) for cross-shard transactions, but it's disabled by default (performance cost). Instead, design the schema so transactions don't cross shards (all related tables sharded by the same key). For operations that must span shards (rare), use application-level sagas or eventual consistency.

**Q6: How does VDiff work to verify resharding correctness?**
A: VDiff runs a streaming comparison between source and target shards. It reads rows from both, computes checksums, and reports discrepancies. It handles the moving target (new writes) by using consistent snapshots (MVCC). VDiff can run while the source is actively receiving writes.

---

### Deep Dive 3: Query Optimization and InnoDB Internals

**Why it's hard:** MySQL query performance depends on understanding InnoDB's buffer pool, B-tree index structure, clustered indexes, and the query optimizer. A single missing index or a poorly written query can cause full table scans on billion-row tables, consuming minutes instead of milliseconds.

**Approaches to query optimization:**

| Technique | Impact | Effort |
|-----------|--------|--------|
| Index optimization (covering, composite) | 100-1000× | Medium |
| Query rewrite (avoid N+1, use JOINs) | 10-100× | Medium |
| Buffer pool tuning | 2-5× | Low |
| Partitioning (range on date) | 5-20× for time-range queries | Medium |
| Read/write splitting (CQRS) | 2-5× throughput | Medium |
| Denormalization | 2-10× read, worse write | High |
| Connection pooling (HikariCP) | 5-20× connection efficiency | Low |

**Implementation Detail:**

```java
/**
 * Query optimization patterns for Spring Data JPA with Hibernate.
 */
@Service
public class QueryOptimizationExamples {

    @PersistenceContext
    private EntityManager em;

    /**
     * PROBLEM: Hibernate N+1 query problem.
     * 
     * Without optimization:
     *   SELECT * FROM instances WHERE tenant_id = 123;  -- 1 query, 100 results
     *   SELECT * FROM volumes WHERE instance_id = 1;    -- N queries, 1 per instance
     *   SELECT * FROM volumes WHERE instance_id = 2;
     *   ... (100 more queries)
     * Total: 101 queries!
     * 
     * SOLUTION: Use JOIN FETCH or @EntityGraph
     */

    // Solution 1: JPQL JOIN FETCH
    public List<Instance> getInstancesWithVolumes(Long tenantId) {
        return em.createQuery(
            "SELECT DISTINCT i FROM Instance i " +
            "LEFT JOIN FETCH i.volumes " +
            "WHERE i.tenantId = :tenantId AND i.status = 'active'",
            Instance.class)
            .setParameter("tenantId", tenantId)
            .getResultList();
        // Generates: SELECT i.*, v.* FROM instances i
        //            LEFT JOIN volumes v ON i.instance_id = v.instance_id
        //            WHERE i.tenant_id = 123 AND i.status = 'active'
        // Total: 1 query!
    }

    // Solution 2: @BatchSize (N+1 → 1 + ceil(N/batchSize) queries)
    @Entity
    public static class Instance {
        @OneToMany(mappedBy = "instance", fetch = FetchType.LAZY)
        @BatchSize(size = 25)  // Load volumes for 25 instances per query
        private List<Volume> volumes;
    }

    /**
     * PROBLEM: OFFSET pagination on large result sets.
     * OFFSET 100000 forces MySQL to scan and discard 100000 rows.
     * 
     * SOLUTION: Keyset pagination (WHERE id > last_seen_id)
     */
    public List<AuditLog> getAuditLogPage(Long tenantId, Long lastLogId, int pageSize) {
        // BAD: SELECT * FROM audit_log WHERE tenant_id = 123
        //      ORDER BY log_id DESC LIMIT 20 OFFSET 100000
        // MySQL scans 100020 rows and discards 100000!

        // GOOD: Keyset pagination
        return em.createQuery(
            "SELECT a FROM AuditLog a " +
            "WHERE a.tenantId = :tenantId AND a.logId < :lastLogId " +
            "ORDER BY a.logId DESC",
            AuditLog.class)
            .setParameter("tenantId", tenantId)
            .setParameter("lastLogId", lastLogId)
            .setMaxResults(pageSize)
            .getResultList();
        // MySQL uses index: (tenant_id, log_id DESC)
        // Starts from last_log_id, reads exactly pageSize rows. O(pageSize), not O(offset+pageSize).
    }

    /**
     * EXPLAIN analysis for query optimization.
     */
    public void analyzeQuery(String sql) {
        /*
        EXPLAIN output columns and what to look for:
        
        | Column        | Good Value                    | Bad Value                     |
        |---------------|-------------------------------|-------------------------------|
        | type          | const, eq_ref, ref, range     | ALL (full table scan)         |
        | possible_keys | Index name                    | NULL                          |
        | key           | Index name (actually used)    | NULL                          |
        | key_len       | Shortest that covers query    | Very long (unnecessary cols)  |
        | rows          | Small number                  | Large number (millions)       |
        | Extra         | Using index (covering)        | Using filesort, Using temporary|
        
        Red flags:
        - type=ALL: full table scan — add an index
        - Using filesort: MySQL sorts in memory/disk — add ORDER BY columns to index
        - Using temporary: MySQL creates temp table — simplify query or add index
        - rows > 10% of table: query is not selective enough
        */
    }
}
```

```sql
-- InnoDB Buffer Pool Configuration
-- The buffer pool is InnoDB's most critical performance component.
-- It caches data pages and index pages in memory.

-- Size: 70-80% of total RAM (leaving 20-30% for OS, connections, sort buffers)
SET GLOBAL innodb_buffer_pool_size = 384 * 1024 * 1024 * 1024;  -- 384 GB

-- Multiple buffer pool instances (reduces contention on multi-core)
SET GLOBAL innodb_buffer_pool_instances = 16;  -- 1 per 1-2 GB of buffer pool

-- Buffer pool dump/load on restart (warm up cache)
SET GLOBAL innodb_buffer_pool_dump_at_shutdown = ON;
SET GLOBAL innodb_buffer_pool_load_at_startup = ON;
SET GLOBAL innodb_buffer_pool_dump_pct = 40;  -- Dump hottest 40% of pages

-- Redo log configuration (write-ahead log)
SET GLOBAL innodb_log_buffer_size = 64 * 1024 * 1024;  -- 64 MB
SET GLOBAL innodb_redo_log_capacity = 8 * 1024 * 1024 * 1024;  -- 8 GB (MySQL 8.0.30+)
-- Larger redo log: more writes buffered, less frequent checkpointing
-- But: longer crash recovery (must replay more redo log)

-- Change buffer (defers secondary index updates)
SET GLOBAL innodb_change_buffering = 'all';
SET GLOBAL innodb_change_buffer_max_size = 25;  -- 25% of buffer pool

-- I/O configuration for NVMe
SET GLOBAL innodb_io_capacity = 10000;           -- IOPS for background tasks
SET GLOBAL innodb_io_capacity_max = 20000;       -- Max burst IOPS
SET GLOBAL innodb_read_io_threads = 16;
SET GLOBAL innodb_write_io_threads = 16;
SET GLOBAL innodb_flush_method = 'O_DIRECT';     -- Bypass OS cache (buffer pool is our cache)
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Missing index → full table scan | Query takes minutes; locks block writes | Slow query log analysis; `EXPLAIN` in CI/CD; pt-query-digest |
| N+1 query pattern | 100x more queries than needed | @EntityGraph, JOIN FETCH, @BatchSize |
| Buffer pool thrashing (too small) | Constant disk I/O; high latency | Monitor `innodb_buffer_pool_read_requests` vs `innodb_buffer_pool_reads`; target > 99% hit rate |
| Deadlock | Transaction aborted; client retry | Consistent lock ordering; short transactions; `innodb_deadlock_detect = ON` |
| Long-running query | Holds row locks; blocks other transactions | `max_execution_time`; query timeout in application |
| InnoDB redo log full | Writes stall (checkpoint storm) | Increase `innodb_redo_log_capacity`; reduce write rate |

**Interviewer Q&As:**

**Q1: How does InnoDB's clustered index work?**
A: InnoDB stores table data in B-tree order of the primary key. The leaf nodes of the PK index contain the actual row data. This means PK lookups are extremely fast (single B-tree traversal to data). Secondary indexes store the PK value in their leaf nodes, so a secondary index lookup requires two B-tree traversals: secondary index → PK value → clustered index → row data. This is why covering indexes are valuable: they avoid the second lookup.

**Q2: What is a covering index and when do you use it?**
A: A covering index contains all columns needed by a query. MySQL can answer the query entirely from the index without accessing the table data (clustered index). In `EXPLAIN`, this shows as `Extra: Using index`. Example: for `SELECT tenant_id, status, name FROM instances WHERE tenant_id = 123 AND status = 'active'`, the covering index is `(tenant_id, status, name)`. The InnoDB PK (`instance_id`) is implicitly included in every secondary index.

**Q3: How does `innodb_change_buffer` improve write performance?**
A: When a row is modified, secondary indexes must be updated. If the secondary index page is not in the buffer pool, InnoDB buffers the change in the change buffer (in memory) and merges it later when the page is read. This converts random I/O (updating secondary index pages scattered on disk) into sequential I/O (merging changes when pages are eventually read). Beneficial for write-heavy workloads with many secondary indexes.

**Q4: What causes deadlocks in InnoDB and how do you prevent them?**
A: Deadlock: Transaction A holds lock X, waits for lock Y. Transaction B holds lock Y, waits for lock X. InnoDB detects deadlocks (FIFO wait-for graph) and rolls back one transaction. Prevention: (1) acquire locks in consistent order (always lock rows by ascending PK), (2) keep transactions short, (3) use `SELECT ... FOR UPDATE` with the smallest possible row set, (4) avoid mixing index ranges and individual row locks in the same transaction.

**Q5: How does MySQL 8.0 `instant ADD COLUMN` work?**
A: Before 8.0, `ADD COLUMN` required copying the entire table (millions of rows). MySQL 8.0 instant DDL adds the column to the metadata only — no data copy. The default value is stored in the data dictionary. Existing rows return the default when the column is read. Only works for: adding a column at the end of the table, with a default value, if the table doesn't use `ROW_FORMAT=COMPRESSED`.

**Q6: What is the Hibernate N+1 problem and how do you detect it in production?**
A: The N+1 problem: a query loads N entities, then Hibernate lazily loads each entity's relations one at a time (N more queries). Detection: (1) `hibernate.generate_statistics=true` logs query counts per session, (2) Hibernate Query Log shows individual SQL statements, (3) Application performance monitoring (APM) shows many short queries in a waterfall, (4) `p6spy` interceptor logs all JDBC calls. Fix: `JOIN FETCH`, `@EntityGraph`, `@BatchSize`, or DTO projections.

---

### Deep Dive 4: CQRS (Command Query Responsibility Segregation)

**Why it's hard:** Write-heavy and read-heavy workloads have different optimization requirements. Writes need ACID, low latency, and sequential I/O (redo log). Reads need fast index scans, potentially stale data is acceptable, and complex joins/aggregations. Running both on the same MySQL instance creates resource contention.

**Approaches:**

| Approach | Read/Write Isolation | Consistency | Complexity | Scalability |
|----------|---------------------|------------|-----------|------------|
| Single primary (no separation) | None | Strong | Low | Limited |
| Read replicas (ProxySQL routing) | Medium (replica lag) | Eventual (ms lag) | Low | Read scales linearly |
| Separate read model (materialized views) | High | Eventual (seconds) | High | Independent scaling |
| Event-sourced CQRS | Full | Eventual (event processing lag) | Very high | Full independence |

**Selected approach:** Read replicas with ProxySQL routing + selective materialized views for complex queries.

**Implementation Detail:**

```java
/**
 * CQRS pattern with Spring Data JPA.
 * 
 * Write path: Primary MySQL → synchronous response
 * Read path: Replica MySQL → eventually consistent (ms lag)
 * 
 * Routing is controlled by @Transactional(readOnly = true/false)
 * which HikariCP/ProxySQL uses to route to the appropriate server.
 */

@Configuration
public class CQRSDataSourceConfiguration {

    /**
     * Two-datasource approach for explicit CQRS.
     * Primary: for writes and read-your-writes scenarios.
     * Replica: for dashboard queries and reports.
     */

    @Bean("writeDataSource")
    @Primary
    public DataSource writeDataSource() {
        HikariConfig config = new HikariConfig();
        config.setJdbcUrl("jdbc:mysql://vtgate:3306/keyspace");
        config.setUsername("write_user");
        config.setMaximumPoolSize(10);
        config.setPoolName("write-pool");
        config.setReadOnly(false);
        // ProxySQL recognizes this user and routes to primary
        return new HikariDataSource(config);
    }

    @Bean("readDataSource")
    public DataSource readDataSource() {
        HikariConfig config = new HikariConfig();
        config.setJdbcUrl("jdbc:mysql://vtgate:3306/keyspace");
        config.setUsername("read_user");
        config.setMaximumPoolSize(20);  // More read connections
        config.setPoolName("read-pool");
        config.setReadOnly(true);
        // ProxySQL recognizes this user and routes to replica
        return new HikariDataSource(config);
    }

    @Bean
    public AbstractRoutingDataSource routingDataSource(
            @Qualifier("writeDataSource") DataSource writeDs,
            @Qualifier("readDataSource") DataSource readDs) {

        Map<Object, Object> dataSources = new HashMap<>();
        dataSources.put("write", writeDs);
        dataSources.put("read", readDs);

        AbstractRoutingDataSource routing = new AbstractRoutingDataSource() {
            @Override
            protected Object determineCurrentLookupKey() {
                return TransactionSynchronizationManager
                    .isCurrentTransactionReadOnly() ? "read" : "write";
            }
        };
        routing.setTargetDataSources(dataSources);
        routing.setDefaultTargetDataSource(writeDs);
        return routing;
    }
}

/**
 * ProxySQL configuration for read/write splitting.
 */
// proxysql.cnf
/*
mysql_servers =
(
    { address="mysql-primary", port=3306, hostgroup=10, max_connections=200 },
    { address="mysql-replica1", port=3306, hostgroup=20, max_connections=200 },
    { address="mysql-replica2", port=3306, hostgroup=20, max_connections=200 }
);

mysql_query_rules =
(
    {
        rule_id=1,
        match_pattern="^SELECT .* FOR UPDATE$",
        destination_hostgroup=10,  // Writes to primary
        apply=1
    },
    {
        rule_id=2,
        match_pattern="^SELECT",
        destination_hostgroup=20,  // Reads to replica
        apply=1
    },
    {
        rule_id=3,
        match_pattern=".*",
        destination_hostgroup=10,  // Everything else to primary
        apply=1
    }
);

mysql_replication_hostgroups =
(
    { writer_hostgroup=10, reader_hostgroup=20,
      max_replication_lag=5 }  // Route to primary if replica lag > 5s
);
*/

/**
 * Read-your-writes pattern: after a write, the next read
 * should go to the primary to see the just-written data.
 */
@Service
public class ReadYourWritesService {

    private final InstanceService instanceService;
    // Thread-local flag to force reads to primary after a write
    private static final ThreadLocal<Boolean> FORCE_PRIMARY = 
        ThreadLocal.withInitial(() -> false);

    @Transactional
    public Instance createAndReturn(CreateInstanceRequest req) {
        // Write to primary
        Instance created = instanceService.createInstance(req);

        // Force next read to primary (avoid replica lag)
        FORCE_PRIMARY.set(true);

        try {
            // This read goes to primary, guaranteed to see the write
            return instanceService.getInstance(created.getInstanceId());
        } finally {
            FORCE_PRIMARY.remove();
        }
    }

    // Used by routing DataSource:
    // if (FORCE_PRIMARY.get()) return "write"; else return "read";
}
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Replica lag > acceptable threshold | Stale reads | ProxySQL `max_replication_lag` routes to primary; alert on lag |
| All replicas down | All reads go to primary (overload) | Circuit breaker; degrade gracefully (cached responses) |
| ProxySQL misconfiguration | Writes routed to replica (rejected) | Testing; ProxySQL admin interface monitoring |
| Read-your-writes violation | User sees stale data after write | Thread-local flag to force primary reads after writes |
| Write datasource pool exhaustion | Write requests queue/timeout | Properly size pool; `connection-timeout` with backpressure |

**Interviewer Q&As:**

**Q1: When is replica lag acceptable and when is it not?**
A: Acceptable: dashboard queries, analytics, search results, recommendation feeds. These tolerate seconds of staleness. Not acceptable: (1) reading an entity immediately after creating/updating it (read-your-writes), (2) checking uniqueness constraints (must read primary), (3) financial transactions (account balance). Use the read-your-writes pattern for the non-acceptable cases.

**Q2: How does ProxySQL detect replica lag?**
A: ProxySQL monitors `Seconds_Behind_Master` on each replica via a health check query. If lag exceeds `max_replication_lag`, ProxySQL moves the replica from the reader hostgroup to a "lag" hostgroup (not used for reads). When lag drops below threshold, the replica is moved back. This is checked every `monitor_replication_lag_interval` (default 10s).

**Q3: Why use ProxySQL instead of just Vitess for read/write splitting?**
A: Vitess VTGate can do read/write splitting (using `@replica` tablet type), but ProxySQL adds: (1) connection multiplexing (many app connections → fewer MySQL connections), (2) query caching for frequent identical queries, (3) query rewriting and filtering, (4) detailed per-query analytics. In practice, both can be used together: Vitess for sharding, ProxySQL per-shard for read/write splitting.

**Q4: What is the @Transactional propagation behavior and how does it interact with CQRS?**
A: `REQUIRED` (default): join existing transaction or create new. `REQUIRES_NEW`: always create new (suspends current). `SUPPORTS`: join if exists, otherwise non-transactional. For CQRS: write methods use `REQUIRED` (routes to primary). Read methods use `readOnly=true` (routes to replica). A write method calling a read method in the same transaction: the read also goes to primary (correct behavior — avoids reading stale data during the transaction).

**Q5: How do you handle schema changes with CQRS (primary and replicas)?**
A: Online DDL (MySQL 8.0 instant or gh-ost) is applied on the primary. Replicas receive the DDL via replication. During DDL: (1) Instant DDL: metadata-only change, no impact on replicas. (2) Online DDL (gh-ost): creates a shadow table, copies data, atomic swap. Replicas replay the same process. During the swap, there's a brief lock (< 1s). ProxySQL connection draining handles this transparently.

**Q6: How do you test CQRS routing in integration tests?**
A: (1) Use `p6spy` to log all SQL queries with the target host. Verify writes go to primary and reads to replica. (2) In-memory test: use an embedded H2 with two schemas simulating primary/replica. (3) Testcontainers: spin up a MySQL primary+replica in Docker for integration tests. Verify read-after-write returns correct data within the same test.

---

## 7. Scaling Strategy

**Scaling dimensions:**

| Dimension | Approach |
|-----------|---------|
| Read throughput | Add replicas (up to 5 per shard); add ProxySQL for load balancing |
| Write throughput | Add Vitess shards (split by tenant_id hash range) |
| Connection count | ProxySQL connection multiplexing; HikariCP tuning |
| Storage | Add disk to existing servers; or reshard to distribute data |
| Query complexity | Denormalize; create materialized views; push to Elasticsearch for search |
| Schema changes | Online DDL (instant, gh-ost); Vitess ApplySchema |

**Interviewer Q&As:**

**Q1: How do you handle a 10× traffic increase?**
A: (1) Immediate: add read replicas (takes ~30 min to snapshot + catch up). (2) Short-term: increase HikariCP pool size; add ProxySQL instances. (3) Medium-term: reshard Vitess (split hot shards). (4) Long-term: evaluate whether to move heavy read workloads to a read-optimized store (Redis cache, Elasticsearch for search).

**Q2: What's the maximum write throughput for a single MySQL instance?**
A: On NVMe with 64 cores and 512 GB RAM: ~30-50K simple inserts/sec, ~10-20K complex transactions/sec. The bottleneck is usually the redo log sequential write speed and InnoDB page flushing. Beyond this, shard with Vitess.

**Q3: How do you handle connection exhaustion (too many microservices)?**
A: (1) HikariCP pool size per pod: 5-10 connections (not 100). (2) ProxySQL connection multiplexing: 10,000 app connections → 200 MySQL connections. (3) Vitess VTTablet also pools connections to MySQL. (4) If still exhausted: reduce idle connections (`idle-timeout`), use connection-on-demand instead of pre-warmed pools.

**Q4: When should you shard vs when should you scale up?**
A: Scale up when: (1) CPU/memory is the bottleneck (add cores/RAM). (2) Read throughput is the bottleneck (add replicas). (3) Data fits in a single instance (< 10 TB). Shard when: (1) Write throughput exceeds single-instance capacity. (2) Data exceeds single-instance storage. (3) Need per-tenant isolation for performance or compliance.

**Q5: How do you handle hot shards (one tenant generating disproportionate traffic)?**
A: (1) Monitor per-shard query rates via VTTablet metrics. (2) For read-hot tenants: add more replicas for that shard. (3) For write-hot tenants: reshard to give that tenant a dedicated shard. (4) Application-level caching (Redis) for frequently read data. (5) Rate limiting at the API gateway.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO | RPO |
|---|---------|-----------|--------|----------|-----|-----|
| 1 | MySQL primary crash | Vitess health check (10s) | Writes blocked for shard | Vitess promotes replica (semi-sync ensures no data loss) | < 30s | 0 |
| 2 | MySQL replica crash | ProxySQL health check | One fewer read endpoint | Replace or restart; catches up via replication | 0 (other replicas serve) | 0 |
| 3 | VTGate crash | LB health check | Affected connections dropped | Clients reconnect to other VTGate | 0 (LB routes to healthy) | 0 |
| 4 | ProxySQL crash | VTTablet direct connection | Reads not load-balanced | Restart ProxySQL; or VTGate routes directly to VTTablet | < 30s | 0 |
| 5 | Full rack failure | Multiple health check failures | One shard's primary + replicas on that rack | Vitess promotes replica on another rack; replicas catch up | < 60s | 0 (if semi-sync replica on other rack) |
| 6 | Network partition | Split-brain detection | Writes may fail on minority side | Vitess fencing; retry on majority side | < 60s | 0 |
| 7 | Disk failure (primary) | I/O errors | Primary crashes | Vitess failover; replace disk; rebuild primary from replica | < 30s (failover) | 0 |
| 8 | Data corruption (bit rot) | InnoDB checksum failure | Affected pages unreadable | Restore from replica (different physical disks) | Minutes | 0 |

### Data Durability and Replication

- **Redo log:** Every committed transaction is in the redo log (fsync'd with `innodb_flush_log_at_trx_commit=1`).
- **Semi-sync replication:** At least 1 replica ACKs before primary returns success. RPO = 0.
- **Binary log:** Retained for 7 days. Used for point-in-time recovery.
- **Backups:** Daily full backup (Percona XtraBackup — hot, non-blocking) + continuous binlog archival to S3.
- **Point-in-time recovery:** Restore latest backup + replay binlog to target timestamp. RPO = seconds.

---

## 9. Security

| Layer | Mechanism |
|-------|-----------|
| **Authentication** | MySQL native authentication + caching_sha2_password (MySQL 8.0 default); LDAP integration for operator access |
| **Authorization** | Principle of least privilege: app user has DML only (no DDL); read user has SELECT only; admin user for schema changes |
| **Encryption at rest** | InnoDB tablespace encryption (AES-256); keyring plugin with HashiCorp Vault for key management |
| **Encryption in transit** | TLS between app and ProxySQL/VTGate; TLS for replication channels |
| **Network isolation** | MySQL listens on private network only; ProxySQL/VTGate on service mesh; no direct app→MySQL connections |
| **Audit** | MySQL Enterprise Audit plugin (or Percona Audit Log); all DDL and privileged operations logged |
| **Connection security** | ProxySQL whitelist: only known service accounts can connect; max connections per user |
| **Data masking** | PII columns encrypted at application layer (AES-GCM); decrypted only in the service with the key |
| **SQL injection** | Parameterized queries (PreparedStatement) via JPA/Hibernate; ProxySQL query rules block suspicious patterns |

---

## 10. Incremental Rollout Strategy

| Phase | Scope | Duration | Validation |
|-------|-------|----------|-----------|
| 1 | Single MySQL instance (no sharding, no replicas) | 1 week | Schema validation, query performance baseline |
| 2 | Primary + 2 replicas + ProxySQL (read/write splitting) | 2 weeks | Replica lag monitoring; read routing validation |
| 3 | Vitess setup (2 shards, unsharded → sharded migration) | 3 weeks | VReplication migration; query routing correctness |
| 4 | Java services migrate to VTGate (HikariCP reconfiguration) | 2 weeks | Connection pool tuning; latency comparison |
| 5 | CQRS implementation (readOnly routing) | 2 weeks | Verify reads go to replicas; read-your-writes testing |
| 6 | Production traffic migration (10% → 50% → 100%) | 3 weeks | SLA monitoring; error rate comparison |
| 7 | Resharding test (split shard) | 1 week | Online resharding with production data; VDiff verification |

**Rollout Q&As:**

**Q1: How do you migrate from a single MySQL to Vitess without downtime?**
A: (1) Deploy Vitess (VTGate, VTTablet) alongside existing MySQL. VTTablet manages the existing MySQL as an "unsharded" keyspace. (2) Point application at VTGate instead of MySQL directly. (3) Validate all queries work through Vitess. (4) Create new sharded keyspace with vschema. (5) Use MoveTables (VReplication) to copy data from unsharded to sharded keyspace. (6) Switch traffic. Zero downtime throughout.

**Q2: How do you validate query compatibility with Vitess?**
A: (1) Run `vtexplain` on all production queries — it shows which shards each query would hit. (2) Shadow traffic: VTGate can log queries it can't route (unsupported syntax). (3) Integration tests with VTGate in Docker. (4) Canary: route 1% of traffic through Vitess, compare latency and results with direct MySQL.

**Q3: How do you handle schema migrations during the rollout?**
A: Use Vitess `ApplySchema` which executes DDL on all shards. For backwards-compatible changes (add column, add index), apply DDL first, then deploy application code. For breaking changes, use expand-contract: add new column → deploy code that writes to both old and new → backfill → deploy code that reads from new → drop old column.

**Q4: What's your rollback strategy if Vitess introduces query regressions?**
A: Keep direct MySQL connectivity as fallback. VTGate is a TCP proxy — switching the application JDBC URL from VTGate back to MySQL (via ProxySQL) is a config change with a rolling restart. All data is still in the same MySQL instances; Vitess adds a routing layer but doesn't move data until resharding.

**Q5: How do you test failover before going to production?**
A: (1) Simulate primary failure: `vtctlclient EmergencyReparentShard`. Verify automatic promotion, replication resumption, and application recovery. (2) Simulate network partition: iptables block between primary and replicas. Verify semi-sync fallback and Vitess fencing. (3) Chaos testing: use Litmus or Chaos Monkey to randomly kill VTTablet/VTGate pods. Measure RTO.

---

## 11. Trade-offs & Decision Log

| # | Decision | Alternatives | Rationale |
|---|----------|-------------|-----------|
| 1 | MySQL 8.0 over PostgreSQL | PostgreSQL (richer features), MariaDB (fork) | Team expertise; Vitess maturity with MySQL; InnoDB performance for OLTP; MySQL 8.0 closes many PostgreSQL advantages (CTEs, window functions, instant DDL) |
| 2 | Vitess over ShardingSphere | ShardingSphere (Java ecosystem), CockroachDB (NewSQL), PlanetScale (managed Vitess) | Vitess is battle-tested at YouTube scale; online resharding; MySQL wire-compatible; open-source; self-hosted on bare metal |
| 3 | Semi-sync over Group Replication | Async (faster, data loss risk), MGR (Paxos, higher latency), Galera (certification-based) | Semi-sync: zero data loss with < 2ms latency overhead; simpler operations than MGR; Vitess handles failover orchestration |
| 4 | HikariCP over c3p0/DBCP2 | c3p0 (legacy), Apache DBCP2, Vibur DBCP | HikariCP is fastest, most reliable, Spring Boot default; extensive monitoring via Micrometer |
| 5 | ProxySQL + Vitess over Vitess alone | Vitess-only (simpler), ProxySQL-only (no sharding), HAProxy (no MySQL awareness) | ProxySQL adds: connection multiplexing, query caching, fine-grained read/write rules. Vitess adds: sharding, resharding. Together they cover all needs |
| 6 | Tenant_id sharding key over auto-increment ID | UUID (random), timestamp, composite key | Tenant_id: all tenant data co-located (efficient JOINs); natural query filter; even distribution (hash vindex) |
| 7 | Keyset pagination over OFFSET pagination | OFFSET (simpler), cursor-based (opaque) | OFFSET is O(offset+limit) in MySQL; keyset is O(limit) regardless of page depth; critical for large result sets |
| 8 | CQRS with read replicas over single primary | Single primary (simpler), event-sourced CQRS (complex) | Read replicas provide 2-4× read throughput with minimal complexity; full CQRS unnecessary for our consistency needs |

---

## 12. Agentic AI Integration

| Use Case | Agentic AI Application | Implementation |
|----------|----------------------|----------------|
| **Query optimization** | Agent analyzes slow query log and suggests index improvements | Parse `pt-query-digest` output; for each slow query, agent runs EXPLAIN, identifies missing indexes, and generates ALTER TABLE statements |
| **Capacity planning** | Agent predicts storage and connection growth; auto-provisions | Time-series model on table sizes, connection counts, QPS; agent submits Vitess resharding requests when threshold approached |
| **Anomaly detection** | Agent detects unusual query patterns (SQL injection, runaway queries) | Real-time analysis of ProxySQL query digest; alert on new query patterns, unusual query rates, or queries returning abnormally large result sets |
| **Deadlock resolution** | Agent analyzes deadlock graphs and recommends lock ordering changes | Parse InnoDB deadlock log (`SHOW ENGINE INNODB STATUS`); identify involved tables and lock types; recommend transaction restructuring |
| **Schema evolution** | Agent suggests schema optimizations based on query patterns | Analyze query log for: unused indexes (no reads), missing indexes (filesort/temp), over-indexed tables (write overhead) |
| **Replication health** | Agent monitors replication lag and auto-remedies | If lag > threshold: agent checks for long-running queries on replica, pauses non-critical queries, increases parallel replication workers |

**Example: Automated Index Advisor Agent**

```python
class MySQLIndexAdvisorAgent:
    """
    Analyzes slow queries and recommends index changes.
    Runs as a scheduled job (daily).
    """

    def __init__(self, mysql_client, alert_client):
        self.mysql = mysql_client
        self.alerts = alert_client

    async def analyze_and_recommend(self):
        """Parse slow query log and generate index recommendations."""

        # Get slow queries from performance_schema
        slow_queries = await self.mysql.query("""
            SELECT digest_text, count_star, avg_timer_wait/1e12 as avg_sec,
                   sum_rows_examined, sum_rows_sent
            FROM performance_schema.events_statements_summary_by_digest
            WHERE avg_timer_wait > 1e11  -- > 100ms average
            ORDER BY count_star * avg_timer_wait DESC
            LIMIT 50
        """)

        recommendations = []
        for query in slow_queries:
            # Run EXPLAIN
            explain = await self.mysql.query(f"EXPLAIN {query['digest_text']}")

            for row in explain:
                if row['type'] == 'ALL':  # Full table scan
                    suggestion = self.suggest_index(
                        query['digest_text'], row['table'])
                    if suggestion:
                        recommendations.append({
                            'query': query['digest_text'],
                            'avg_time_sec': query['avg_sec'],
                            'executions': query['count_star'],
                            'issue': 'full_table_scan',
                            'suggestion': suggestion,
                            'estimated_improvement': '10-100x'
                        })

                elif 'Using filesort' in str(row.get('Extra', '')):
                    suggestion = self.suggest_sort_index(
                        query['digest_text'], row['table'])
                    recommendations.append({
                        'query': query['digest_text'],
                        'issue': 'filesort',
                        'suggestion': suggestion,
                        'estimated_improvement': '5-20x'
                    })

        # Also find unused indexes
        unused = await self.find_unused_indexes()
        for idx in unused:
            recommendations.append({
                'table': idx['table'],
                'index': idx['index_name'],
                'issue': 'unused_index',
                'suggestion': f"ALTER TABLE {idx['table']} DROP INDEX {idx['index_name']}",
                'estimated_improvement': 'Reduces write overhead'
            })

        if recommendations:
            await self.alerts.info(
                f"MySQL Index Advisor: {len(recommendations)} recommendations. "
                f"Top: {recommendations[0]['suggestion']}")

        return recommendations

    async def find_unused_indexes(self):
        """Find indexes with zero reads in the last 30 days."""
        return await self.mysql.query("""
            SELECT s.table_name as `table`, s.index_name,
                   s.seq_in_index, s.column_name
            FROM information_schema.statistics s
            LEFT JOIN sys.schema_index_statistics sis
                ON s.table_schema = sis.table_schema
                AND s.table_name = sis.table_name
                AND s.index_name = sis.index_name
            WHERE s.table_schema = DATABASE()
                AND s.index_name != 'PRIMARY'
                AND (sis.rows_selected IS NULL OR sis.rows_selected = 0)
            ORDER BY s.table_name, s.index_name
        """)

    def suggest_index(self, query_text, table):
        """Parse WHERE clause to suggest composite index."""
        # Simplified: extract WHERE columns and ORDER BY columns
        where_cols = self.extract_where_columns(query_text)
        order_cols = self.extract_order_columns(query_text)
        select_cols = self.extract_select_columns(query_text)

        # Index columns: equality WHERE first, range WHERE second, ORDER BY third
        # Covering: add SELECT columns at the end
        index_cols = []
        for col in where_cols.get('equality', []):
            index_cols.append(col)
        for col in where_cols.get('range', []):
            index_cols.append(col)
        for col in order_cols:
            if col not in index_cols:
                index_cols.append(col)

        if index_cols:
            cols = ', '.join(index_cols)
            return f"CREATE INDEX idx_{table}_{'_'.join(index_cols[:3])} ON {table} ({cols})"
        return None
```

---

## 13. Complete Interviewer Q&A Bank

**Architecture:**

**Q1: When would you choose MySQL over PostgreSQL for a new project?**
A: MySQL when: (1) Vitess-based sharding is planned (Vitess only supports MySQL), (2) team has MySQL expertise, (3) high-throughput OLTP with simple queries (InnoDB is highly optimized), (4) replication is a first-class requirement. PostgreSQL when: (1) complex queries (better optimizer, more SQL features), (2) JSON/JSONB as primary data type, (3) geographic data (PostGIS), (4) no sharding needed (PostgreSQL vertical scalability is excellent).

**Q2: Explain the InnoDB architecture.**
A: InnoDB has: (1) **Buffer pool**: in-memory cache of data and index pages (most critical for performance). (2) **Redo log**: write-ahead log for crash recovery; sequential writes. (3) **Undo log**: stores old row versions for MVCC (multi-version concurrency control). (4) **Change buffer**: defers secondary index updates for non-unique indexes. (5) **Adaptive hash index**: automatically builds hash indexes for frequently accessed pages. (6) **Doublewrite buffer**: prevents partial page writes by writing to doublewrite buffer before the actual page location.

**Q3: How does MVCC work in InnoDB?**
A: Each row has hidden columns: `DB_TRX_ID` (transaction that last modified it) and `DB_ROLL_PTR` (pointer to undo log for the previous version). READ COMMITTED: each statement sees a snapshot as of statement start. REPEATABLE READ (default): each statement sees a snapshot as of transaction start. Readers never block writers; writers never block readers. This is achieved by constructing the appropriate row version from the undo log chain.

**Q4: What is the doublewrite buffer and why is it needed?**
A: InnoDB pages are 16 KB but disk sector writes are typically 512 bytes or 4 KB. If power fails during a 16 KB page write, the page is partially written (torn page). The doublewrite buffer writes pages to a sequential area first, then to the actual location. On recovery, if a page is torn, InnoDB restores it from the doublewrite buffer. NVMe with power-loss protection can disable this for performance.

**Replication & HA:**

**Q5: Compare async, semi-sync, and group replication.**
A: Async: primary doesn't wait for replicas; fastest writes; risk of data loss on primary crash. Semi-sync: primary waits for 1+ replica ACK; +1-2ms latency; RPO=0 for acknowledged writes. Group Replication: Paxos consensus; all members agree on transaction order; +2-5ms latency; auto-failover; multi-primary possible but complex. For most use cases, semi-sync with Vitess orchestration is the sweet spot.

**Q6: How does Vitess compare to Amazon RDS/Aurora?**
A: Vitess: open-source, self-managed, bare-metal compatible, supports horizontal sharding, schema management. RDS: managed service, vertical scaling only, no sharding (must do manually). Aurora: MySQL-compatible, shared storage architecture, up to 15 read replicas, auto-failover, but no horizontal sharding. Vitess is better for: bare-metal, multi-cloud, extreme scale. Aurora is better for: operational simplicity, cloud-native workloads.

**Query Optimization:**

**Q7: How do you use EXPLAIN to diagnose a slow query?**
A: Look for: (1) `type=ALL` → full table scan → add index. (2) `key=NULL` → no index used → check WHERE clause alignment with indexes. (3) `rows` large → scanning too many rows → tighten WHERE, add more selective index. (4) `Extra: Using filesort` → sorting not using index → add ORDER BY columns to index. (5) `Extra: Using temporary` → temp table for GROUP BY/DISTINCT → add index covering GROUP BY columns.

**Q8: What is index selectivity and why does it matter?**
A: Selectivity = number of distinct values / total rows. High selectivity (close to 1.0) means each value appears rarely → index is very effective. Low selectivity (e.g., `status` with 5 values in 1M rows = 0.000005) → index is nearly useless for filtering. Put high-selectivity columns first in composite indexes. Exception: even low-selectivity columns are useful in composite indexes if they're used for range scans after equality matches.

**Q9: What are invisible indexes in MySQL 8.0?**
A: Invisible indexes exist on disk but are ignored by the optimizer. Use case: test the impact of dropping an index without actually dropping it. `ALTER TABLE t ALTER INDEX idx INVISIBLE;` → monitor query performance. If no degradation, safely drop. `ALTER TABLE t ALTER INDEX idx VISIBLE;` to reverse. This avoids the costly rebuild of accidentally dropped indexes.

**Connection Management:**

**Q10: How does HikariCP handle connection validation?**
A: HikariCP validates connections before returning them to the application. Three strategies: (1) `connection-test-query: "SELECT 1"` — runs a lightweight query. (2) `validationTimeout: 1000` — max time for validation. (3) `maxLifetime: 1800000` — retire connections after 30 min (MySQL's `wait_timeout` default is 8h). HikariCP also detects leaked connections via `leak-detection-threshold`.

**Q11: How does ProxySQL connection multiplexing work?**
A: ProxySQL maintains two connection pools: frontend (app → ProxySQL) and backend (ProxySQL → MySQL). Many frontend connections share fewer backend connections. When an app sends a query, ProxySQL: (1) assigns a backend connection from the pool, (2) forwards the query, (3) returns the result, (4) releases the backend connection. This reduces MySQL's connection overhead (each MySQL connection uses ~10 MB of memory).

**Operations:**

**Q12: How do you perform online schema changes on a 1 TB table?**
A: (1) MySQL 8.0 instant DDL for simple adds (< 1s, metadata only). (2) For complex changes (add index, change type): use `gh-ost` (GitHub Online Schema Migrations). gh-ost creates a shadow table with the new schema, copies data row-by-row, applies binlog changes, and does an atomic swap. No locks on the original table. Takes hours for 1 TB but has zero downtime. (3) Vitess `ApplySchema` runs DDL on all shards in parallel using online DDL.

**Q13: How do you handle MySQL upgrades (e.g., 5.7 → 8.0)?**
A: (1) Test on staging with production data snapshot. (2) Run `mysql_upgrade_check` to identify incompatibilities. (3) Upgrade replicas first: stop replication, upgrade binary, restart, verify. (4) Promote upgraded replica to primary (Vitess PlannedReparentShard). (5) Upgrade old primary (now replica). (6) Verify all functionality. MySQL 8.0 supports in-place upgrade from 5.7.

**Q14: What backup strategy do you use for a 10 TB MySQL?**
A: (1) Physical backup: Percona XtraBackup (hot, non-blocking, incremental). Full backup weekly (~10 TB, takes 4-6h at 500 MB/s). Incremental daily (delta, typically < 500 GB). (2) Binlog archival: continuous streaming to S3. (3) Point-in-time recovery: restore latest full + incrementals + binlog replay to target timestamp. (4) Test restores monthly on a separate server.

**Q15: How do you monitor MySQL health?**
A: Key metrics: (1) `Threads_running` (active queries; alert > 50). (2) `Slow_queries` (count since restart). (3) `Innodb_buffer_pool_hit_rate` (should be > 99%). (4) `Seconds_Behind_Master` (replication lag). (5) `Com_select/Com_insert/Com_update` (query rate). (6) `Innodb_row_lock_waits` (contention). (7) `Innodb_deadlocks` (deadlock count). (8) `Connections` vs `Max_connections`. Tools: Prometheus + mysqld_exporter, PMM (Percona Monitoring and Management), pt-query-digest.

**Q16: How do you handle a "runaway query" that consumes all CPU/memory?**
A: (1) Identify: `SHOW PROCESSLIST` or `performance_schema.threads`. (2) Kill: `KILL <thread_id>`. (3) Prevent: `max_execution_time = 30000` (30s query timeout in MySQL 8.0). (4) ProxySQL query rules: block queries matching known bad patterns. (5) Post-mortem: analyze the query, add missing index, or rewrite.

---

## 14. References

| # | Resource | Relevance |
|---|----------|-----------|
| 1 | [High Performance MySQL, 4th Ed (Schwartz et al.)](https://www.oreilly.com/library/view/high-performance-mysql/9781492080503/) | Comprehensive MySQL performance guide |
| 2 | [Vitess Documentation](https://vitess.io/docs/) | Sharding, VReplication, vschema |
| 3 | [MySQL 8.0 Reference Manual](https://dev.mysql.com/doc/refman/8.0/en/) | Official MySQL documentation |
| 4 | [HikariCP](https://github.com/brettwooldridge/HikariCP) | JDBC connection pooling |
| 5 | [ProxySQL Documentation](https://proxysql.com/documentation/) | MySQL proxy, read/write splitting |
| 6 | [InnoDB Internals (Jeremy Cole)](https://blog.jcole.us/innodb/) | Deep dive into InnoDB page structure, buffer pool |
| 7 | [gh-ost: GitHub's Online Schema Migration](https://github.com/github/gh-ost) | Online DDL tool |
| 8 | [Percona XtraBackup](https://docs.percona.com/percona-xtrabackup/8.0/) | Hot backup for InnoDB |
| 9 | [Spring Data JPA Reference](https://docs.spring.io/spring-data/jpa/reference/) | JPA with Spring Boot |
| 10 | [Hibernate Performance Tuning](https://vladmihalcea.com/hibernate-performance-tuning/) | N+1, batch fetching, caching |
