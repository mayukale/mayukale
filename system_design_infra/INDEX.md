# Infrastructure System Design Interview Library

**80 systems · 15 categories · Staff/Principal level · Cloud Infrastructure / IaaS Platform Engineer**

> Tailored to: bare-metal IaaS reservation, job scheduling, resource management, Kubernetes/container orchestration, OpenStack backends, message brokers, MySQL + Elasticsearch, Java/Python services, Agentic AI in infrastructure.

---

## How to Use This Library

Each file follows a 16-section format built specifically for infrastructure roles:
1. Requirement Clarifications
2. Scale & Capacity Estimates (with calculations)
3. High-Level Architecture (ASCII diagram)
4. Data Model (full SQL schema)
5. API Design (REST/gRPC + CLI)
6. Core Component Deep Dives (algorithms + 6 Q&As each)
7. Scheduling & Resource Management
8. Scaling Strategy
9. Reliability & Fault Tolerance
10. Observability
11. Security
12. **Incremental Rollout Strategy** (critical for 1000s-of-servers deployments)
13. Trade-offs & Decision Log
14. **Agentic AI Integration** (role differentiator)
15. Complete Interviewer Q&A Bank (15+ per system)
16. References

---

## Most Likely to Be Asked — Priority Ranking

> Ranked by probability of appearing in a Staff/Principal interview for this specific role.

### Tier 1 — Almost Certain (prepare these first)

| Rank | File | Why It Will Be Asked |
|------|------|---------------------|
| 1 | [bare_metal_reservation_platform.md](01_bare_metal_iaas/bare_metal_reservation_platform.md) | **The actual product** — this IS the job |
| 2 | [kubernetes_control_plane.md](05_kubernetes_containers/kubernetes_control_plane.md) | K8s is core to the role; control plane depth separates Staff from Senior |
| 3 | [distributed_job_scheduler.md](02_job_scheduling/distributed_job_scheduler.md) | Job scheduling is explicitly listed in the job description |
| 4 | [cluster_resource_manager.md](03_resource_management/cluster_resource_manager.md) | Resource management is core; YARN/Mesos/k8s scheduler internals |
| 5 | [distributed_message_queue.md](06_message_brokers/distributed_message_queue.md) | Kafka is explicitly listed; expect deep ISR/exactly-once questions |
| 6 | [self_healing_infrastructure.md](04_automated_recovery/self_healing_infrastructure.md) | Automated recovery is explicitly listed |
| 7 | [mysql_at_scale.md](09_storage_systems/mysql_at_scale.md) | MySQL explicitly listed; Vitess sharding, replication, HikariCP |
| 8 | [elasticsearch_at_scale.md](09_storage_systems/elasticsearch_at_scale.md) | Elasticsearch explicitly listed; ILM, shard sizing, indexing pipeline |

### Tier 2 — Very Likely (prepare before interview)

| Rank | File | Why |
|------|------|-----|
| 9 | [agentic_auto_remediation.md](13_agentic_ai_in_infrastructure/agentic_auto_remediation.md) | Agentic AI explicitly in role description — differentiator |
| 10 | [openstack_nova_compute_design.md](14_openstack_cloud/openstack_nova_compute_design.md) | OpenStack explicitly listed; Nova scheduler architecture |
| 11 | [compute_resource_allocator.md](03_resource_management/compute_resource_allocator.md) | Bin packing, placement, NUMA awareness |
| 12 | [rollout_controller.md](11_cicd_and_deployment/rollout_controller.md) | Deploying to thousands of servers without downtime |
| 13 | [distributed_lock_service.md](07_distributed_systems_primitives/distributed_lock_service.md) | Locks underpin reservation system and scheduler |
| 14 | [metrics_and_monitoring_system.md](08_observability/metrics_and_monitoring_system.md) | Prometheus/Grafana for infra monitoring |
| 15 | [pod_autoscaler_system.md](05_kubernetes_containers/pod_autoscaler_system.md) | HPA/VPA/KEDA explicitly mentioned |

### Tier 3 — Prepare If Time Allows

| File | Why |
|------|-----|
| [kubernetes_operator_framework.md](05_kubernetes_containers/kubernetes_operator_framework.md) | Operators are how infra teams extend k8s |
| [iaas_platform_design.md](01_bare_metal_iaas/iaas_platform_design.md) | Broader IaaS context |
| [vm_live_migration_system.md](14_openstack_cloud/vm_live_migration_system.md) | OpenStack Nova migration — ops challenge |
| [reliable_event_delivery_system.md](06_message_brokers/reliable_event_delivery_system.md) | Exactly-once delivery in infra pipelines |
| [secrets_management_system.md](12_security_and_access/secrets_management_system.md) | Vault is ubiquitous in infra |
| [distributed_tracing_system.md](08_observability/distributed_tracing_system.md) | OpenTelemetry + Jaeger |

---

## Full Category Index

### 01 · Bare Metal IaaS ← THE ANCHOR CATEGORY
| File | Key Insight |
|------|-------------|
| [bare_metal_reservation_platform.md](01_bare_metal_iaas/bare_metal_reservation_platform.md) | Interval tree O(log n) conflict detection; `SELECT FOR UPDATE` prevents double-reservation; machine state machine (available→reserved→provisioning→in-use→draining→maintenance→failed) |
| [iaas_platform_design.md](01_bare_metal_iaas/iaas_platform_design.md) | Resource hierarchy Region→AZ→Rack→Server; control plane must handle partial failures with compensating transactions |
| [self_service_developer_portal.md](01_bare_metal_iaas/self_service_developer_portal.md) | Temporal.io for multi-step provisioning workflows; approval gates before expensive GPU resources; cost estimation before commit |
| [machine_pool_manager.md](01_bare_metal_iaas/machine_pool_manager.md) | Warm vs cold pool: warm absorbs burst in seconds, cold in minutes; health scoring drives automatic ejection; firmware lifecycle across pool |

---

### 02 · Job Scheduling
| File | Key Insight |
|------|-------------|
| [distributed_job_scheduler.md](02_job_scheduling/distributed_job_scheduler.md) | etcd leader election with fencing tokens prevents duplicate execution; two-phase SUSPECT/DEAD heartbeat protocol; Quartz JDBC JobStore for clustering |
| [task_queue_system.md](02_job_scheduling/task_queue_system.md) | RabbitMQ quorum queues for durability; HikariCP pool = (workers × DB connections); Python GIL requires prefork for CPU-bound tasks |
| [priority_based_job_scheduler.md](02_job_scheduling/priority_based_job_scheduler.md) | Three-band weighted fair dequeue (70/25/5%); priority aging prevents starvation; priority inversion propagates through DAG dependencies (Mars Pathfinder lesson) |
| [cron_service_at_scale.md](02_job_scheduling/cron_service_at_scale.md) | Redis ZSET scored by next_execution_time_ms for O(log N) trigger scan across 10M jobs; DST spring-forward gap and fall-back first-occurrence policies |
| [deadline_aware_scheduler.md](02_job_scheduling/deadline_aware_scheduler.md) | EDF (Earliest Deadline First); admission control rejects infeasible jobs at submission rather than missing deadline silently |

---

### 03 · Resource Management
| File | Key Insight |
|------|-------------|
| [compute_resource_allocator.md](03_resource_management/compute_resource_allocator.md) | Multi-dimensional bin packing (NP-hard): FFD heuristic + dot-product fragmentation scoring; GPU NVLink topology awareness; CPU 5:1 overcommit, RAM 1.2:1 |
| [cluster_resource_manager.md](03_resource_management/cluster_resource_manager.md) | Dominant Resource Fairness (DRF) for fair multi-tenant sharing; gang scheduling for ML training (all-or-nothing); two-level scheduling (Mesos) vs monolithic (k8s) |
| [quota_and_limit_enforcement_system.md](03_resource_management/quota_and_limit_enforcement_system.md) | Hierarchical quota: org→team→project→user; admission webhook must respond < 10ms p99; burst quota enables temporary overages with billing implications |
| [resource_reservation_system.md](03_resource_management/resource_reservation_system.md) | Interval tree O(log n + k) overlap detection; overbooking with guaranteed/best-effort tiers and adaptive ratios |
| [capacity_planning_system.md](03_resource_management/capacity_planning_system.md) | USE method; Prophet ensemble forecasting; 3-6 month hardware procurement lead time → must order at 70% utilization |

---

### 04 · Automated Recovery
| File | Key Insight |
|------|-------------|
| [self_healing_infrastructure.md](04_automated_recovery/self_healing_infrastructure.md) | k8s reconciliation loop is the canonical model; flap detection prevents oscillation; bare-metal recovery via IPMI watchdog + PXE reimaging |
| [automated_failover_system.md](04_automated_recovery/automated_failover_system.md) | Phi Accrual failure detector adapts to network jitter; STONITH prevents split-brain at bare-metal level; MySQL Orchestrator for GTID-based automatic failover |
| [chaos_engineering_platform.md](04_automated_recovery/chaos_engineering_platform.md) | Steady-state hypothesis validated BEFORE injecting chaos; dead man's switch auto-aborts; blast radius control is non-negotiable |
| [health_check_and_remediation_service.md](04_automated_recovery/health_check_and_remediation_service.md) | Deep health checks (DB connectivity, downstream services); SLO-aware gating: don't remediate if it would break SLO for others |
| [auto_remediation_runbook_system.md](04_automated_recovery/auto_remediation_runbook_system.md) | Runbook-as-YAML with preconditions/postconditions; dry-run mode mandatory; blast radius limit per runbook (max 10% of fleet) |

---

### 05 · Kubernetes & Containers
| File | Key Insight |
|------|-------------|
| [kubernetes_control_plane.md](05_kubernetes_containers/kubernetes_control_plane.md) | etcd is single source of truth (linearizable reads, watch API); scheduler: filter O(N nodes) then score O(M candidates); preemption algorithm |
| [kubernetes_cluster_api_platform.md](05_kubernetes_containers/kubernetes_cluster_api_platform.md) | CAPI mirrors Deployment/ReplicaSet/Pod at cluster level (Machine/MachineSet/MachineDeployment); Metal3 provider for bare-metal via Ironic |
| [container_orchestration_system.md](05_kubernetes_containers/container_orchestration_system.md) | cgroups v2 for QoS isolation; Cilium eBPF eliminates iptables overhead; CSI lifecycle (provision→attach→stage→publish) |
| [kubernetes_operator_framework.md](05_kubernetes_containers/kubernetes_operator_framework.md) | Level-triggered reconciliation: every reconcile handles ANY state, not just transitions; finalizers block deletion until cleanup |
| [multi_tenant_kubernetes_platform.md](05_kubernetes_containers/multi_tenant_kubernetes_platform.md) | Defense-in-depth: RBAC + NetworkPolicy + ResourceQuota + PSA + OPA — each layer independent; vCluster for stronger isolation |
| [pod_autoscaler_system.md](05_kubernetes_containers/pod_autoscaler_system.md) | HPA → unschedulable pods → Cluster Autoscaler (chained); KEDA enables scale-to-zero; VPA cannot coexist with HPA on CPU/memory |
| [service_mesh_design.md](05_kubernetes_containers/service_mesh_design.md) | xDS decouples control/data plane; ambient mesh reduces resource overhead 97% vs sidecars; SPIFFE cryptographic workload identity |

---

### 06 · Message Brokers
| File | Key Insight |
|------|-------------|
| [distributed_message_queue.md](06_message_brokers/distributed_message_queue.md) | Kafka's 600MB/s from sequential I/O + page cache + zero-copy sendfile(); ISR + HW ensures consumers only read committed; KRaft eliminates ZooKeeper |
| [event_streaming_platform.md](06_message_brokers/event_streaming_platform.md) | Schema registry compatibility modes prevent consumer breakage; Flink exactly-once via checkpoint barriers + Kafka transactions; Debezium CDC from binlog/WAL |
| [dead_letter_queue_system.md](06_message_brokers/dead_letter_queue_system.md) | Non-blocking retry topics (retry-1/2/3/DLQ) prevent poison pill from blocking main topic; circuit breaker routes to DLQ when downstream is down |
| [pub_sub_notification_system.md](06_message_brokers/pub_sub_notification_system.md) | Fan-out to 1M subscribers requires sharded worker pool; inverted index for subscription filter avoids full scan |
| [reliable_event_delivery_system.md](06_message_brokers/reliable_event_delivery_system.md) | Transactional outbox + Debezium CDC is most reliable pattern for atomic DB write + publish; consumer idempotency via Redis SET NX |

---

### 07 · Distributed Systems Primitives
| File | Key Insight |
|------|-------------|
| [distributed_lock_service.md](07_distributed_systems_primitives/distributed_lock_service.md) | etcd lease-based locks with fencing tokens are safer than Redlock (clock drift + GC pause); fencing token passed to protected resource |
| [distributed_rate_limiter.md](07_distributed_systems_primitives/distributed_rate_limiter.md) | Sliding Window Counter (O(1), <1% error) via Redis Lua; token bucket for burst; centralized vs sidecar changes failure mode |
| [service_discovery_system.md](07_distributed_systems_primitives/service_discovery_system.md) | ndots=5 causes 4x DNS query amplification; NodeLocal DNSCache at 169.254.25.10 eliminates conntrack race at high request rates |
| [distributed_configuration_management.md](07_distributed_systems_primitives/distributed_configuration_management.md) | etcd watch API sub-second propagation; Vault dynamic secrets with TTL eliminate long-lived credentials; `vault://` references |
| [consistent_hashing_system.md](07_distributed_systems_primitives/consistent_hashing_system.md) | 150 vnodes per physical node reduces load std deviation; Jump Consistent Hash O(ln N) stateless but no arbitrary removal |
| [leader_election_service.md](07_distributed_systems_primitives/leader_election_service.md) | Raft requires N/2+1 quorum — 2 nodes cannot elect safely; STONITH for bare-metal HA; leader lease bounds clock skew |
| [distributed_transaction_coordinator.md](07_distributed_systems_primitives/distributed_transaction_coordinator.md) | 2PC coordinator failure leaves cohorts blocked; Saga with compensating transactions is the production choice for long-running infra workflows |

---

### 08 · Observability
| File | Key Insight |
|------|-------------|
| [distributed_logging_system.md](08_observability/distributed_logging_system.md) | Kafka as log buffer decouples collector from ES; ILM hot-warm-cold-delete; 30-50 GB per shard; Java MDC propagates trace_id |
| [metrics_and_monitoring_system.md](08_observability/metrics_and_monitoring_system.md) | Gorilla compression (delta-of-delta + XOR) = 1.37 bytes/sample; Thanos Sidecar uploads 2h blocks to S3; recording rules pre-compute expensive queries |
| [distributed_tracing_system.md](08_observability/distributed_tracing_system.md) | Tail-based sampling decides after full trace completes — samples all errors regardless of head rate; W3C TraceContext across HTTP/gRPC/Kafka |
| [alerting_and_on_call_system.md](08_observability/alerting_and_on_call_system.md) | Multi-window multi-burn-rate SLO alerts (fast 1h + slow 6h windows); dead man's switch alerts when monitoring is down |
| [anomaly_detection_system.md](08_observability/anomaly_detection_system.md) | STL seasonal decomposition handles daily/weekly patterns; ensemble scoring reduces false positives; confirmation window prevents single-point alerts |
| [audit_log_system.md](08_observability/audit_log_system.md) | SHA-256 hash chain + RFC 3161 timestamps for tamper evidence; S3 Object Lock (WORM); k8s audit policy levels |

---

### 09 · Storage Systems
| File | Key Insight |
|------|-------------|
| [distributed_object_storage.md](09_storage_systems/distributed_object_storage.md) | Reed-Solomon 6+3 uses 1.5x storage vs 3x replication; prefix-based sharding avoids hot partitions; strong read-after-write consistency |
| [distributed_file_system.md](09_storage_systems/distributed_file_system.md) | Ceph CRUSH algorithm with fault-domain awareness; BlueStore eliminates double-write penalty; HDFS NameNode memory limits require Federation |
| [time_series_database.md](09_storage_systems/time_series_database.md) | Prometheus TSDB 2h in-memory head + WAL; Gorilla compression; cardinality explosion is the #1 operational problem; Thanos for global queries |
| [elasticsearch_at_scale.md](09_storage_systems/elasticsearch_at_scale.md) | Custom routing by tenant_id prevents hot shards; ILM force-merge + shrink on warm tier; deep pagination via search_after + PIT |
| [mysql_at_scale.md](09_storage_systems/mysql_at_scale.md) | Vitess vschema + VReplication for online resharding; HikariCP maximumPoolSize = (cores × 2) + disk spindles; AbstractRoutingDataSource |

---

### 10 · Networking & Traffic
| File | Key Insight |
|------|-------------|
| [api_gateway_design.md](10_networking_and_traffic/api_gateway_design.md) | Radix trie O(k) route matching; Envoy/Nginx at 100K+ RPS per core vs Kong ~30K (Lua overhead); Wasm plugins for portable middleware |
| [load_balancer_design.md](10_networking_and_traffic/load_balancer_design.md) | IPVS (kernel-space) handles 10M connections; Maglev consistent hashing minimizes disruption on backend change; eBPF/XDP line-rate |
| [service_proxy_and_sidecar.md](10_networking_and_traffic/service_proxy_and_sidecar.md) | Envoy intercepts via iptables REDIRECT (15001/15006, UID 1337 exclusion); ~0.5ms latency + 50MB RAM overhead per sidecar |
| [dns_at_scale.md](10_networking_and_traffic/dns_at_scale.md) | ndots=5 causes 4 DNS lookups; NodeLocal DNSCache at link-local 169.254.25.10 eliminates conntrack race condition |
| [network_policy_enforcement_system.md](10_networking_and_traffic/network_policy_enforcement_system.md) | eBPF identity-based enforcement O(1) vs iptables O(n rules); default deny-all then explicit allow |

---

### 11 · CI/CD & Deployment
| File | Key Insight |
|------|-------------|
| [cicd_pipeline_system.md](11_cicd_and_deployment/cicd_pipeline_system.md) | Content-addressed build cache (S3+Redis) achieves 80%+ hit rate; Tekton is k8s-native; GitOps with ArgoCD makes deployment state auditable |
| [blue_green_deployment_system.md](11_cicd_and_deployment/blue_green_deployment_system.md) | DB expand-contract migration required — schema compatible with both blue and green simultaneously; k8s Service selector swap is instantaneous |
| [canary_deployment_system.md](11_cicd_and_deployment/canary_deployment_system.md) | Argo Rollouts AnalysisTemplate uses Prometheus metrics; compare canary vs baseline (not fixed threshold) for statistical validity |
| [feature_flag_service.md](11_cicd_and_deployment/feature_flag_service.md) | In-process SDK (zero network hops); MurmurHash3 for deterministic bucketing; SSE for real-time updates; automated flag debt cleanup |
| [rollout_controller.md](11_cicd_and_deployment/rollout_controller.md) | Topology-aware wave planning (AZ-balanced); two-phase health gate; max 5% of fleet per wave; auto-pause on error rate threshold |
| [artifact_registry.md](11_cicd_and_deployment/artifact_registry.md) | Content-addressed (SHA256) layer deduplication = 60-80% storage savings; Trivy + Kyverno blocks vulnerable images at deploy time |

---

### 12 · Security & Access
| File | Key Insight |
|------|-------------|
| [rbac_authorization_system.md](12_security_and_access/rbac_authorization_system.md) | k8s TokenRequest API issues audience-bound short-lived tokens (not old mounted secrets); OPA/Rego sub-millisecond in-process evaluation |
| [secrets_management_system.md](12_security_and_access/secrets_management_system.md) | Vault auto-unseal via KMS; dynamic DB credentials with TTL eliminate long-lived passwords; Raft integrated storage for HA |
| [certificate_lifecycle_management.md](12_security_and_access/certificate_lifecycle_management.md) | SPIFFE/SPIRE cryptographic workload identity; short-lived certs (24h) make revocation less critical; cert-manager automates rotation |
| [api_key_management_system.md](12_security_and_access/api_key_management_system.md) | Store SHA-256 hash (never plaintext); canary/honeypot keys detect DB exfiltration; GitHub Secret Scanning partner integration |
| [zero_trust_network_design.md](12_security_and_access/zero_trust_network_design.md) | Never trust network location; Cilium eBPF micro-segmentation O(1); Teleport identity-aware proxy replaces VPN |

---

### 13 · Agentic AI in Infrastructure ← ROLE DIFFERENTIATOR
| File | Key Insight |
|------|-------------|
| [ai_ops_platform.md](13_agentic_ai_in_infrastructure/ai_ops_platform.md) | RAG with hybrid BM25+dense retrieval over runbooks+incidents; confidence threshold gates auto-remediate vs escalate |
| [agentic_auto_remediation.md](13_agentic_ai_in_infrastructure/agentic_auto_remediation.md) | ReAct pattern; safety gate checks preconditions before every action; blast radius calculator; undo stack for rollback |
| [predictive_autoscaling_system.md](13_agentic_ai_in_infrastructure/predictive_autoscaling_system.md) | ML prediction sets minimum floor + HPA as safety net; bare-metal provision time (15 min) requires 20-min prediction horizon |
| [intelligent_capacity_planner.md](13_agentic_ai_in_infrastructure/intelligent_capacity_planner.md) | MIP for spot/reserved/on-demand cost optimization; LLM summaries include number validation to prevent hallucinated figures |
| [llm_assisted_runbook_executor.md](13_agentic_ai_in_infrastructure/llm_assisted_runbook_executor.md) | Human approval gate before execution is non-negotiable; LLM fills dynamic parameters from alert context; generates runbooks from post-mortems |
| [infrastructure_copilot.md](13_agentic_ai_in_infrastructure/infrastructure_copilot.md) | RBAC enforced before every action — NL doesn't bypass authorization; confirmation step before every write |

---

### 14 · OpenStack Cloud
| File | Key Insight |
|------|-------------|
| [openstack_nova_compute_design.md](14_openstack_cloud/openstack_nova_compute_design.md) | Nova Placement API with CUSTOM_RESOURCES for GPUs/FPGAs; nova-conductor prevents nova-compute from direct DB access |
| [openstack_neutron_network_design.md](14_openstack_cloud/openstack_neutron_network_design.md) | OVN replaces ML2+OVS with distributed control plane; DVR moves L3 forwarding to compute nodes; ML2 mechanism drivers are pluggable |
| [multi_region_cloud_platform.md](14_openstack_cloud/multi_region_cloud_platform.md) | Keystone federation shares identity across regions; RabbitMQ must NOT span regions; eventual consistency acceptable for catalog, not reservations |
| [cloud_control_plane_design.md](14_openstack_cloud/cloud_control_plane_design.md) | RabbitMQ AMQP topic exchanges are the backbone; upgrade requires N/N+1 API compatibility (both versions must coexist) |
| [vm_live_migration_system.md](14_openstack_cloud/vm_live_migration_system.md) | Pre-copy converges when dirty rate < network bandwidth; post-copy page-faults remaining memory; downtime target <100ms; 1TB RAM challenges convergence |

---

### 15 · CLI & Portal Design
| File | Key Insight |
|------|-------------|
| [developer_self_service_portal.md](15_cli_and_portal_design/developer_self_service_portal.md) | Temporal.io durable workflows; quota enforcement at submission time; cost estimation before provisioning |
| [cli_client_for_infra_platform.md](15_cli_and_portal_design/cli_client_for_infra_platform.md) | OAuth2 device flow for interactive CLI login; service account tokens for CI/CD; OS keyring for token storage (not ~/.config plaintext) |
| [infrastructure_as_code_platform.md](15_cli_and_portal_design/infrastructure_as_code_platform.md) | Kahn's topological sort for dependency ordering; DynamoDB state lock prevents concurrent applies; gRPC provider plugins |
| [web_portal_for_iaas.md](15_cli_and_portal_design/web_portal_for_iaas.md) | BFF aggregates service calls via CompletableFuture; STOMP WebSocket + Redis pub/sub for real-time status; server-side sessions for admin portals |

---

## Cross-Reference: Shared Algorithms & Patterns

### Interval Trees / Conflict Detection
- [bare_metal_reservation_platform.md](01_bare_metal_iaas/bare_metal_reservation_platform.md) — primary
- [resource_reservation_system.md](03_resource_management/resource_reservation_system.md)
- [cron_service_at_scale.md](02_job_scheduling/cron_service_at_scale.md)
- [distributed_job_scheduler.md](02_job_scheduling/distributed_job_scheduler.md)

### Leader Election (etcd/Raft)
- [distributed_lock_service.md](07_distributed_systems_primitives/distributed_lock_service.md) — deep dive
- [leader_election_service.md](07_distributed_systems_primitives/leader_election_service.md) — deep dive
- [distributed_job_scheduler.md](02_job_scheduling/distributed_job_scheduler.md), [cron_service_at_scale.md](02_job_scheduling/cron_service_at_scale.md), [kubernetes_control_plane.md](05_kubernetes_containers/kubernetes_control_plane.md)

### Fencing Tokens (stale lock protection)
- [distributed_lock_service.md](07_distributed_systems_primitives/distributed_lock_service.md), [distributed_job_scheduler.md](02_job_scheduling/distributed_job_scheduler.md), [leader_election_service.md](07_distributed_systems_primitives/leader_election_service.md), [bare_metal_reservation_platform.md](01_bare_metal_iaas/bare_metal_reservation_platform.md)

### Bin Packing / Placement
- [compute_resource_allocator.md](03_resource_management/compute_resource_allocator.md) — NP-hard approximations
- [cluster_resource_manager.md](03_resource_management/cluster_resource_manager.md) — DRF + gang scheduling
- [kubernetes_control_plane.md](05_kubernetes_containers/kubernetes_control_plane.md) — filter/score
- [openstack_nova_compute_design.md](14_openstack_cloud/openstack_nova_compute_design.md) — Nova scheduler

### Kafka Exactly-Once
- [distributed_message_queue.md](06_message_brokers/distributed_message_queue.md) — ISR, acks, transactions
- [reliable_event_delivery_system.md](06_message_brokers/reliable_event_delivery_system.md) — outbox pattern
- [event_streaming_platform.md](06_message_brokers/event_streaming_platform.md) — Flink checkpointing
- [audit_log_system.md](08_observability/audit_log_system.md)

### Kubernetes Reconciliation Loop / Operator Pattern
- [kubernetes_control_plane.md](05_kubernetes_containers/kubernetes_control_plane.md), [kubernetes_operator_framework.md](05_kubernetes_containers/kubernetes_operator_framework.md)
- [self_healing_infrastructure.md](04_automated_recovery/self_healing_infrastructure.md), [rollout_controller.md](11_cicd_and_deployment/rollout_controller.md), [pod_autoscaler_system.md](05_kubernetes_containers/pod_autoscaler_system.md)

### Transactional Outbox Pattern
- [reliable_event_delivery_system.md](06_message_brokers/reliable_event_delivery_system.md) — canonical
- [bare_metal_reservation_platform.md](01_bare_metal_iaas/bare_metal_reservation_platform.md), [distributed_transaction_coordinator.md](07_distributed_systems_primitives/distributed_transaction_coordinator.md)

### eBPF in Infrastructure
- [network_policy_enforcement_system.md](10_networking_and_traffic/network_policy_enforcement_system.md), [load_balancer_design.md](10_networking_and_traffic/load_balancer_design.md), [service_proxy_and_sidecar.md](10_networking_and_traffic/service_proxy_and_sidecar.md), [service_mesh_design.md](05_kubernetes_containers/service_mesh_design.md), [zero_trust_network_design.md](12_security_and_access/zero_trust_network_design.md)

### AI Agent / ReAct Pattern
- [agentic_auto_remediation.md](13_agentic_ai_in_infrastructure/agentic_auto_remediation.md) — canonical
- [ai_ops_platform.md](13_agentic_ai_in_infrastructure/ai_ops_platform.md), [llm_assisted_runbook_executor.md](13_agentic_ai_in_infrastructure/llm_assisted_runbook_executor.md), [infrastructure_copilot.md](13_agentic_ai_in_infrastructure/infrastructure_copilot.md), [auto_remediation_runbook_system.md](04_automated_recovery/auto_remediation_runbook_system.md)

---

## Top 10 Must-Know Concepts for This Exact Role

### 1. Bare-Metal Reservation & Conflict Prevention
Interval tree O(log n) overlap detection, `SELECT FOR UPDATE` vs optimistic locking, machine state machine, IPMI/BMC provisioning, idempotency keys.
→ [bare_metal_reservation_platform.md](01_bare_metal_iaas/bare_metal_reservation_platform.md)

### 2. Distributed Scheduler — Exactly-Once Job Execution
etcd leader election with fencing tokens, worker heartbeat + timeout detection, job state machine, JDBC job store (Quartz), at-least-once vs exactly-once semantics.
→ [distributed_job_scheduler.md](02_job_scheduling/distributed_job_scheduler.md)

### 3. Kubernetes Scheduler Internals
Filter (predicates) → score (priorities) → bind; preemption algorithm; etcd watch API; API server admission webhook chain; controller-manager reconciliation loop.
→ [kubernetes_control_plane.md](05_kubernetes_containers/kubernetes_control_plane.md)

### 4. Kafka Delivery Semantics
ISR, High Watermark vs LEO, acks=all vs acks=1, idempotent producer, transactional producer, consumer group rebalancing (eager vs cooperative), exactly-once stream processing.
→ [distributed_message_queue.md](06_message_brokers/distributed_message_queue.md)

### 5. Resource Placement & Bin Packing
First Fit Decreasing heuristic, multi-dimensional bin packing (CPU + RAM + GPU), Dominant Resource Fairness, gang scheduling for ML, NUMA topology, overcommit ratios.
→ [compute_resource_allocator.md](03_resource_management/compute_resource_allocator.md), [cluster_resource_manager.md](03_resource_management/cluster_resource_manager.md)

### 6. Rolling Rollout Across Thousands of Servers
Wave-based deployment (max 5% per wave), topology-aware batching (AZ-balanced), health gate (local + Prometheus), automated pause/rollback, database expand-contract migration.
→ [rollout_controller.md](11_cicd_and_deployment/rollout_controller.md), [canary_deployment_system.md](11_cicd_and_deployment/canary_deployment_system.md)

### 7. MySQL at Scale
Vitess sharding (vschema, VReplication), GTID-based replication and failover, HikariCP pool sizing formula, ProxySQL read/write splitting, InnoDB buffer pool, deadlock prevention.
→ [mysql_at_scale.md](09_storage_systems/mysql_at_scale.md)

### 8. Elasticsearch Operational Excellence
Shard sizing (30-50 GB), ILM hot-warm-cold, custom routing to avoid hot shards, Java BulkProcessor, mapping (keyword vs text, doc_values), deep pagination (search_after + PIT).
→ [elasticsearch_at_scale.md](09_storage_systems/elasticsearch_at_scale.md)

### 9. Agentic AI Safety in Infrastructure
ReAct pattern, precondition checks before every action, blast radius calculation, dry-run mode, audit trail for every AI action, confidence thresholds, rollback capability.
→ [agentic_auto_remediation.md](13_agentic_ai_in_infrastructure/agentic_auto_remediation.md)

### 10. OpenStack Nova + Neutron
Nova components (api/scheduler/conductor/compute/placement), Placement API resource classes, ML2 plugin architecture, OVN vs ML2+OVS, live migration pre-copy convergence math.
→ [openstack_nova_compute_design.md](14_openstack_cloud/openstack_nova_compute_design.md), [vm_live_migration_system.md](14_openstack_cloud/vm_live_migration_system.md)

---

## Technology Quick Reference

| Technology | Primary Files | Secondary Files |
|------------|--------------|-----------------|
| **etcd** | distributed_lock_service, leader_election_service | kubernetes_control_plane, distributed_configuration_management |
| **Kafka** | distributed_message_queue, event_streaming_platform | reliable_event_delivery_system, audit_log_system |
| **MySQL/Vitess** | mysql_at_scale | bare_metal_reservation_platform, distributed_job_scheduler |
| **Elasticsearch** | elasticsearch_at_scale | distributed_logging_system, auto_remediation_runbook_system |
| **Kubernetes** | kubernetes_control_plane, kubernetes_operator_framework | referenced throughout all categories |
| **OpenStack** | openstack_nova_compute_design, openstack_neutron_network_design | cloud_control_plane_design |
| **Redis** | distributed_rate_limiter, cron_service_at_scale | bare_metal_reservation_platform, feature_flag_service |
| **Prometheus/Grafana** | metrics_and_monitoring_system | alerting_and_on_call_system, rollout_controller |
| **HashiCorp Vault** | secrets_management_system | certificate_lifecycle_management, distributed_configuration_management |
| **eBPF/Cilium** | network_policy_enforcement_system | load_balancer_design, service_mesh_design |
| **HikariCP** | mysql_at_scale | task_queue_system, distributed_job_scheduler |
| **Spring Boot/Java** | mysql_at_scale, distributed_job_scheduler | web_portal_for_iaas, all Java service designs |
| **Envoy/Istio** | service_proxy_and_sidecar, service_mesh_design | api_gateway_design |
| **LLM/RAG** | ai_ops_platform, agentic_auto_remediation | infrastructure_copilot, intelligent_capacity_planner |
| **Temporal.io** | developer_self_service_portal | bare_metal_reservation_platform (workflow engine) |
| **IPMI/BMC** | bare_metal_reservation_platform, machine_pool_manager | self_healing_infrastructure, vm_live_migration_system |
| **SPIFFE/SPIRE** | certificate_lifecycle_management | service_mesh_design, zero_trust_network_design |

---

*80 files · 15 categories · Opus-generated · Staff/Principal level · Infrastructure Platform Engineer*
