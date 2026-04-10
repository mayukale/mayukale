# System Design: Kubernetes Cluster API Platform

> **Relevance to role:** A cloud infrastructure platform engineer must automate the provisioning, lifecycle management, and upgrading of hundreds of Kubernetes clusters across bare-metal, vSphere, and cloud providers. Cluster API (CAPI) is the de facto standard for declarative cluster management — understanding its architecture is essential for building a self-service platform that treats clusters as cattle, not pets.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Declaratively provision Kubernetes clusters across multiple infrastructure providers (AWS, GCP, vSphere, bare-metal) |
| FR-2 | Support cluster lifecycle operations: create, upgrade, scale, repair, delete |
| FR-3 | Provide standardized cluster templates (ClusterClass) for consistent configurations |
| FR-4 | Enable fleet-wide operations: rolling upgrades, policy enforcement, configuration drift detection |
| FR-5 | Integrate with GitOps workflows for cluster provisioning (Flux, ArgoCD) |
| FR-6 | Support multi-tenancy: teams can self-service provision clusters within guardrails |
| FR-7 | Handle bootstrap: kubeadm, Talos, k3s, or custom bootstrap providers |
| FR-8 | Manage machine lifecycle: cordon, drain, replace unhealthy machines |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Cluster provisioning time | < 15 min (cloud), < 30 min (bare-metal) |
| NFR-2 | Cluster upgrade time (rolling) | < 1 hour for a 100-node cluster |
| NFR-3 | Management cluster availability | 99.99% |
| NFR-4 | Cluster count | 500+ workload clusters per management cluster |
| NFR-5 | Reconciliation latency | Desired state achieved within 5 min of change |
| NFR-6 | Machine replacement on failure | < 10 min (cloud), < 30 min (bare-metal) |
| NFR-7 | API response time | < 2s for cluster CRUD operations |

### Constraints & Assumptions
- Management cluster runs Cluster API controllers + provider controllers.
- Workload clusters are independent — management cluster failure does not affect running workloads.
- Bare-metal provisioning uses Metal3 provider with Ironic for BMC control.
- Bootstrap provider is kubeadm (most common) unless Talos is specified.
- All cluster configurations stored in Git (GitOps source of truth).
- Network infrastructure (VPCs, subnets, VIPs) is pre-provisioned or managed by the infrastructure provider.

### Out of Scope
- Application deployment to workload clusters (handled by ArgoCD/Flux).
- Control plane internals (covered in kubernetes_control_plane.md).
- Service mesh (covered in service_mesh_design.md).
- Cost management and billing (mentioned but not primary focus).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|------------|--------|
| Workload clusters | 500 clusters under management | 500 |
| Machines (total) | 500 clusters x 50 nodes avg | 25,000 |
| Machine objects | 25,000 (1 per node) | 25,000 |
| MachineSet objects | 500 clusters x 5 node pools avg | 2,500 |
| Cluster objects | 500 | 500 |
| CAPI controller QPS | ~50 reconciliations/s (steady state) | 50 QPS |
| Provisioning burst | 10 clusters simultaneously x 50 machines | 500 machines in parallel |
| User operations | 50 platform engineers x 10 ops/day | ~0.006 QPS (negligible) |

### Latency Requirements

| Operation | Target | Notes |
|-----------|--------|-------|
| Cluster creation API response | < 2s | Returns Cluster object with status=Provisioning |
| Control plane ready | < 10 min (cloud), < 20 min (bare-metal) | Infrastructure provisioning + bootstrap |
| Worker node join | < 5 min per node (cloud) | After control plane is ready |
| Cluster upgrade start | < 30s from spec change | Controller detects and begins reconciliation |
| Machine health check → replacement | < 5 min detection + < 10 min replacement | Total < 15 min (cloud) |
| GitOps sync (desired → actual) | < 3 min | Flux/ArgoCD reconciliation interval |

### Storage Estimates

| Component | Calculation | Result |
|-----------|------------|--------|
| Management cluster etcd | 500 clusters x 55 objects x 5 KB avg | ~137 MB |
| Cluster secrets (kubeconfigs, certs) | 500 clusters x 10 secrets x 10 KB | ~50 MB |
| Git repository (cluster manifests) | 500 clusters x 5 KB manifest | ~2.5 MB |
| Backup storage (etcd snapshots) | 500 clusters x 100 MB snapshot x 10 retention | ~500 GB |

### Bandwidth Estimates

| Flow | Calculation | Result |
|------|------------|--------|
| CAPI controller → cloud API | 50 reconcile/s x 2 KB API call | ~100 KB/s |
| Management → workload cluster API | 500 clusters x 1 health check/30s x 1 KB | ~17 KB/s |
| GitOps sync | 500 clusters x 5 KB manifest / 3 min | ~14 KB/s |
| BMC/IPMI (bare-metal) | 100 machines x 5 KB command / provisioning burst | ~500 KB/s burst |

---

## 3. High Level Architecture

```
                    ┌─────────────────────────────────┐
                    │         Platform Team            │
                    │  (Git commits, kubectl, UI)      │
                    └──────────┬──────────────────────┘
                               │
                    ┌──────────▼──────────────────────┐
                    │      GitOps Controller           │
                    │   (Flux / ArgoCD)                │
                    │   Syncs cluster manifests        │
                    └──────────┬──────────────────────┘
                               │ applies CRDs
                    ┌──────────▼──────────────────────┐
                    │    Management Cluster            │
                    │                                  │
                    │  ┌──────────────────────────┐   │
                    │  │  CAPI Core Controllers   │   │
                    │  │  - Cluster controller     │   │
                    │  │  - Machine controller     │   │
                    │  │  - MachineSet controller  │   │
                    │  │  - MachineDeployment ctrl │   │
                    │  │  - MachineHealthCheck     │   │
                    │  └────────────┬─────────────┘   │
                    │               │                  │
                    │  ┌────────────▼─────────────┐   │
                    │  │  Infrastructure Providers │   │
                    │  │  ┌──────┐ ┌──────┐       │   │
                    │  │  │ AWS  │ │ GCP  │       │   │
                    │  │  │ CAPA │ │ CAPG │       │   │
                    │  │  └──┬───┘ └──┬───┘       │   │
                    │  │  ┌──┴───┐ ┌──┴───┐       │   │
                    │  │  │vSphere│ │Metal3│       │   │
                    │  │  │ CAPV  │ │ CAPM3│       │   │
                    │  │  └──────┘ └──────┘       │   │
                    │  └──────────────────────────┘   │
                    │               │                  │
                    │  ┌────────────▼─────────────┐   │
                    │  │  Bootstrap Providers      │   │
                    │  │  kubeadm / Talos / k3s    │   │
                    │  └──────────────────────────┘   │
                    └──────────┬──────────────────────┘
                               │ provisions
              ┌────────────────┼─────────────────────┐
              │                │                      │
    ┌─────────▼──────┐ ┌──────▼────────┐ ┌──────────▼───────┐
    │ Workload       │ │ Workload      │ │ Workload         │
    │ Cluster A      │ │ Cluster B     │ │ Cluster C        │
    │ (AWS, 50 nodes)│ │ (vSphere, 20) │ │ (Bare-metal, 100)│
    └────────────────┘ └───────────────┘ └──────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Management Cluster** | Runs CAPI controllers; stores cluster state; does not run user workloads |
| **CAPI Core Controllers** | Reconcile Cluster, Machine, MachineSet, MachineDeployment, MachineHealthCheck CRDs |
| **Infrastructure Provider** | Provider-specific controllers that provision VMs/bare-metal (CAPA for AWS, CAPV for vSphere, CAPM3 for Metal3) |
| **Bootstrap Provider** | Generates cloud-init/ignition data to bootstrap kubelet and join the cluster (kubeadm, Talos) |
| **Control Plane Provider** | Manages control plane machines (KubeadmControlPlane — handles etcd, API server, etc.) |
| **GitOps Controller** | Syncs cluster manifests from Git to management cluster; provides audit trail |
| **Workload Clusters** | Independent k8s clusters running user applications; managed by CAPI but autonomous |

### Data Flows

1. **Cluster creation:** Engineer commits Cluster + InfraCluster + MachineDeployment manifests to Git → Flux syncs to management cluster → CAPI Cluster controller creates InfraCluster → Infrastructure provider provisions VMs/networks → Bootstrap provider generates join tokens → kubeadm bootstraps control plane → Workers join → Cluster status = Ready.

2. **Machine replacement:** MachineHealthCheck detects unhealthy machine (failed health probe) → Sets Machine condition to Unhealthy → MachineSet creates replacement Machine → Infrastructure provider provisions new VM → Bootstrap provider generates join config → New node joins cluster → Old machine cordoned, drained, deleted.

3. **Cluster upgrade:** Engineer updates `spec.version` in KubeadmControlPlane → Control plane provider performs rolling upgrade (one control plane node at a time: new machine → wait ready → remove old) → MachineDeployment `spec.version` updated → MachineSet rolling update (surge → drain → delete) → All nodes on new version.

---

## 4. Data Model

### Core Entities & Schema

```yaml
# Cluster (core CAPI object)
apiVersion: cluster.x-k8s.io/v1beta1
kind: Cluster
metadata:
  name: production-us-east
  namespace: clusters
spec:
  clusterNetwork:
    pods:
      cidrBlocks: ["10.244.0.0/16"]
    services:
      cidrBlocks: ["10.96.0.0/12"]
  controlPlaneRef:
    apiVersion: controlplane.cluster.x-k8s.io/v1beta1
    kind: KubeadmControlPlane
    name: production-us-east-cp
  infrastructureRef:
    apiVersion: infrastructure.cluster.x-k8s.io/v1beta1
    kind: AWSCluster           # or VSpherCluster, Metal3Cluster
    name: production-us-east
status:
  phase: Provisioned
  controlPlaneReady: true
  infrastructureReady: true
  conditions:
    - type: Ready
      status: "True"

---
# Machine (mirrors a single node)
apiVersion: cluster.x-k8s.io/v1beta1
kind: Machine
metadata:
  name: prod-worker-abc123
  labels:
    cluster.x-k8s.io/cluster-name: production-us-east
    node-pool: general
  ownerReferences:
    - apiVersion: cluster.x-k8s.io/v1beta1
      kind: MachineSet
      name: prod-worker-pool
spec:
  clusterName: production-us-east
  version: "v1.30.2"
  bootstrap:
    configRef:
      apiVersion: bootstrap.cluster.x-k8s.io/v1beta1
      kind: KubeadmConfig
      name: prod-worker-abc123-bootstrap
  infrastructureRef:
    apiVersion: infrastructure.cluster.x-k8s.io/v1beta1
    kind: AWSMachine
    name: prod-worker-abc123
status:
  phase: Running
  nodeRef:
    name: ip-10-0-1-42
  addresses:
    - type: InternalIP
      address: 10.0.1.42

---
# MachineDeployment (mirrors Deployment — manages MachineSets)
apiVersion: cluster.x-k8s.io/v1beta1
kind: MachineDeployment
metadata:
  name: prod-worker-pool
spec:
  clusterName: production-us-east
  replicas: 50
  selector:
    matchLabels:
      node-pool: general
  template:
    metadata:
      labels:
        node-pool: general
    spec:
      clusterName: production-us-east
      version: "v1.30.2"
      bootstrap:
        configRef:
          kind: KubeadmConfigTemplate
          name: prod-worker-bootstrap
      infrastructureRef:
        kind: AWSMachineTemplate
        name: prod-worker-machine
  strategy:
    type: RollingUpdate
    rollingUpdate:
      maxSurge: 1
      maxUnavailable: 0

---
# ClusterClass (standardized template)
apiVersion: cluster.x-k8s.io/v1beta1
kind: ClusterClass
metadata:
  name: standard-production
spec:
  controlPlane:
    ref:
      apiVersion: controlplane.cluster.x-k8s.io/v1beta1
      kind: KubeadmControlPlaneTemplate
      name: standard-cp-template
    machineInfrastructure:
      ref:
        kind: AWSMachineTemplate
        name: standard-cp-machine
  workers:
    machineDeployments:
      - class: general-purpose
        template:
          bootstrap:
            ref:
              kind: KubeadmConfigTemplate
              name: standard-worker-bootstrap
          infrastructure:
            ref:
              kind: AWSMachineTemplate
              name: standard-worker-machine
  variables:
    - name: region
      required: true
      schema:
        openAPIV3Schema:
          type: string
          enum: ["us-east-1", "us-west-2", "eu-west-1"]
    - name: workerCount
      required: true
      schema:
        openAPIV3Schema:
          type: integer
          minimum: 3
          maximum: 200
```

**Object Hierarchy (mirrors Deployment/ReplicaSet/Pod):**

```
Cluster
├── KubeadmControlPlane (manages control plane machines)
│   └── Machine (one per control plane node)
│       ├── AWSMachine (infrastructure)
│       └── KubeadmConfig (bootstrap)
├── MachineDeployment (manages worker node pools)
│   └── MachineSet (manages a set of machines with same template)
│       └── Machine (one per worker node)
│           ├── AWSMachine (infrastructure)
│           └── KubeadmConfig (bootstrap)
└── MachineHealthCheck (monitors machine health)
```

### Database Selection

| Store | Usage | Justification |
|-------|-------|---------------|
| etcd (management cluster) | All CAPI CRDs, Secrets (kubeconfigs, certs) | Native k8s storage; CAPI objects are k8s CRDs |
| Git (Flux/ArgoCD) | Desired state for all clusters | Audit trail, PR-based approval, rollback capability |
| Object storage (S3/GCS) | etcd snapshots for all workload clusters | Durable backup; cross-region replication |
| Vault / Sealed Secrets | Sensitive credentials (cloud API keys, BMC passwords) | Encryption at rest; RBAC on secrets |

### Indexing Strategy

| Index | Purpose |
|-------|---------|
| `cluster.x-k8s.io/cluster-name` label | All objects for a specific cluster |
| `cluster.x-k8s.io/provider` label | All objects for a specific infrastructure provider |
| `topology.cluster.x-k8s.io/owned` label | Objects managed by ClusterClass topology |
| ownerReferences | Garbage collection chain (Cluster → MachineDeployment → MachineSet → Machine) |
| namespace per team/environment | Isolation of cluster objects by tenant |

---

## 5. API Design

### REST/gRPC/kubectl Endpoints

**Cluster Lifecycle:**

```bash
# Create a cluster from ClusterClass
kubectl apply -f - <<EOF
apiVersion: cluster.x-k8s.io/v1beta1
kind: Cluster
metadata:
  name: dev-team-alpha
  namespace: team-alpha
  labels:
    environment: development
spec:
  topology:
    class: standard-production
    version: v1.30.2
    controlPlane:
      replicas: 3
    workers:
      machineDeployments:
        - class: general-purpose
          name: worker-pool
          replicas: 10
    variables:
      - name: region
        value: "us-east-1"
      - name: workerCount
        value: 10
EOF

# Get cluster status
kubectl get clusters -n team-alpha
kubectl describe cluster dev-team-alpha -n team-alpha

# Scale worker pool
kubectl patch machinedeployment dev-team-alpha-worker-pool -n team-alpha \
  --type merge -p '{"spec":{"replicas": 20}}'

# Upgrade cluster version
kubectl patch cluster dev-team-alpha -n team-alpha \
  --type merge -p '{"spec":{"topology":{"version":"v1.31.0"}}}'

# Get workload cluster kubeconfig
clusterctl get kubeconfig dev-team-alpha -n team-alpha > dev-team-alpha.kubeconfig

# Delete cluster (cascading delete of all machines)
kubectl delete cluster dev-team-alpha -n team-alpha
```

**Fleet Management:**

```bash
# List all clusters across namespaces
kubectl get clusters -A -o wide

# Get machine health across fleet
kubectl get machinehealthcheck -A

# Initialize CAPI with providers
clusterctl init --infrastructure aws --bootstrap kubeadm --control-plane kubeadm

# Move CAPI management to another cluster (pivot)
clusterctl move --to-kubeconfig=new-mgmt.kubeconfig

# Upgrade CAPI components
clusterctl upgrade plan
clusterctl upgrade apply --contract v1beta1
```

### CLI Design (clusterctl)

```bash
# Provider management
clusterctl init --infrastructure aws,vsphere,metal3
clusterctl upgrade plan                    # Show available upgrades
clusterctl upgrade apply --contract v1beta1

# Cluster generation from templates
clusterctl generate cluster my-cluster \
  --kubernetes-version v1.30.2 \
  --control-plane-machine-count 3 \
  --worker-machine-count 10 \
  --infrastructure aws \
  > my-cluster.yaml

# Cluster introspection
clusterctl describe cluster my-cluster     # Tree view of all objects
clusterctl get kubeconfig my-cluster       # Export kubeconfig

# Migration
clusterctl move --to-kubeconfig=target.kubeconfig  # Pivot management
clusterctl backup --directory=/backup/              # Backup CAPI objects
clusterctl restore --directory=/backup/             # Restore CAPI objects
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Infrastructure Provider Architecture (Multi-Provider Support)

**Why it's hard:**
Each infrastructure provider (AWS, vSphere, bare-metal) has fundamentally different APIs, provisioning semantics, and failure modes. CAPI must provide a uniform abstraction layer that handles: (1) different provisioning latencies (seconds for cloud VMs vs. minutes for bare-metal PXE boot), (2) different networking models (VPC vs. VLAN vs. flat network), (3) different failure domains (AZ vs. rack vs. chassis), and (4) different machine lifecycle operations (API terminate vs. IPMI power-off).

**Approaches Compared:**

| Approach | Provider Flexibility | Complexity | Consistency | Use Case |
|----------|---------------------|-----------|-------------|----------|
| CAPI provider interface | High (pluggable) | Medium | Contract-enforced | Production multi-cloud |
| Terraform + custom controller | Very high | High | Manual enforcement | Existing Terraform users |
| Crossplane + CAPI | High (composition) | High | API-level | Platform engineering |
| Custom provisioning scripts | Unlimited | Very high | None | Legacy environments |
| Managed k8s APIs (EKS/GKE/AKS) | Low (vendor lock-in) | Low | Provider-specific | Cloud-only shops |

**Selected: CAPI provider interface**

**Justification:** CAPI defines a contract (set of CRDs + expected behaviors) that each provider implements. The contract ensures: (1) Infrastructure objects must report `Ready` when the resource is provisioned. (2) Machines must have `providerID` linking to the cloud resource. (3) Bootstrap data must be delivered to the machine (cloud-init, ignition). This allows the core controllers to be provider-agnostic.

**Provider-Specific Implementation Details:**

```
AWS (CAPA):
  Cluster → creates VPC, subnets, NAT gateway, ELB for API server
  Machine → creates EC2 instance with userdata (bootstrap)
  Provisioning time: ~3-5 min per machine
  Failure domain: AWS AZ (mapped to CAPI failure domain)

vSphere (CAPV):
  Cluster → creates resource pool, folder
  Machine → clones VM template, configures network
  Provisioning time: ~2-4 min per machine
  Failure domain: vSphere cluster / datastore

Bare-metal (CAPM3 / Metal3):
  Cluster → configures VIP (keepalived/kube-vip)
  Machine → claims BareMetalHost → Ironic provisions via PXE/IPMI
  Provisioning time: ~10-20 min per machine (PXE boot + OS install)
  Failure domain: Rack / power circuit

  BareMetalHost lifecycle:
  1. Registering  → BMC credentials validated
  2. Inspecting   → Hardware inventory collected via Ironic
  3. Available     → Ready to be claimed by a Machine
  4. Provisioning  → OS being installed via PXE
  5. Provisioned   → Machine running, joined to cluster
  6. Deprovisioning → Machine being wiped
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Cloud API rate limiting | Machine provisioning slowed/stalled | Exponential backoff in provider; request quota increase |
| Cloud API outage | New machines cannot be provisioned | Retry with backoff; running clusters unaffected |
| BMC unreachable (bare-metal) | Cannot power on/off machine | Alert; manual intervention; redundant BMC network |
| Bootstrap failure (kubeadm join fails) | Machine provisioned but not joined to cluster | MachineHealthCheck detects NodeReady=False; replaces machine |
| Template drift (VM template outdated) | New machines have wrong OS/packages | Pin template versions in MachineTemplate; automate template builds |
| Network misconfiguration | Machine cannot reach API server | Pre-validate network; health checks detect and replace |

**Interviewer Q&As:**

**Q1: How does CAPI handle the chicken-and-egg problem of the management cluster?**
A: The initial management cluster is bootstrapped using a "bootstrap cluster" (typically a local kind cluster). Steps: (1) Create kind cluster on a workstation. (2) Initialize CAPI controllers on kind. (3) Create the real management cluster as a workload cluster. (4) `clusterctl move` pivots CAPI objects from kind to the real management cluster. (5) Delete kind cluster. After this, the management cluster manages itself (self-hosted).

**Q2: How do you handle bare-metal provisioning with CAPI?**
A: Metal3 (CAPM3) integrates CAPI with OpenStack Ironic. Process: (1) Register physical servers as BareMetalHost CRDs with BMC credentials (IPMI/Redfish). (2) Ironic inspects hardware (CPU, RAM, disk, NIC). (3) When CAPI creates a Machine, CAPM3 claims an available BareMetalHost matching the requirements. (4) Ironic provisions the host via PXE: boots the machine, writes the OS image, configures networking. (5) Cloud-init/ignition bootstraps kubeadm. (6) Machine joins the cluster. Key difference from cloud: there's a finite pool of pre-registered hardware, so capacity planning is critical.

**Q3: What happens when the management cluster goes down?**
A: Running workload clusters are completely unaffected — they are independent Kubernetes clusters with their own etcd, API server, and controllers. However: (1) No new clusters can be created. (2) No machine replacements occur (MachineHealthCheck is paused). (3) No upgrades can proceed. (4) Mitigation: run the management cluster as HA (3 control plane nodes); keep etcd backups; have a documented recovery procedure. Some organizations run a standby management cluster that can be activated.

**Q4: How does ClusterClass enable self-service provisioning?**
A: ClusterClass defines a standardized cluster template with variables. Teams can create clusters by specifying only the variables (region, size, version) without understanding the underlying infrastructure details. Platform team controls the template (machine types, security settings, networking), while application teams customize within allowed boundaries. Variables have validation schemas (OpenAPI v3) that enforce constraints (e.g., minimum 3 workers, maximum 200).

**Q5: How do you handle multi-provider clusters (e.g., control plane on bare-metal, workers on cloud)?**
A: This is not natively supported by CAPI — a cluster is associated with one infrastructure provider. Workarounds: (1) Use virtual kubelet to attach cloud workers to a bare-metal cluster. (2) Use Karmada or multi-cluster federation to distribute workloads across clusters on different providers. (3) Build a custom infrastructure provider that wraps multiple providers. (4) Use Cluster API's failure domain concept within a single provider (e.g., different vSphere clusters acting as failure domains).

**Q6: How does CAPI handle cluster upgrades?**
A: CAPI treats upgrades as immutable infrastructure replacement. When `spec.version` changes on KubeadmControlPlane: (1) Create a new control plane machine with the new version. (2) Wait for it to join and become Ready. (3) Remove one old control plane machine. (4) Repeat until all control plane nodes are upgraded. (5) Then MachineDeployment rolls out new worker machines similarly (surge and drain). This is safer than in-place upgrades because a failed new machine can be destroyed without affecting the old one.

---

### Deep Dive 2: Machine Health Check and Auto-Remediation

**Why it's hard:**
Automatically detecting and replacing unhealthy machines must balance: (1) Speed (replace quickly to restore capacity) vs. safety (don't replace too many at once and cause an outage). (2) Accuracy (distinguish transient issues from permanent failures). (3) Provider-specific behavior (cloud machines can be replaced instantly; bare-metal requires PXE provisioning).

**Approaches Compared:**

| Approach | Detection Speed | False Positive Rate | Recovery Automation | Complexity |
|----------|----------------|--------------------|--------------------|-----------|
| CAPI MachineHealthCheck | Configurable (node conditions) | Medium (tunable timeouts) | Full (machine replacement) | Low |
| Node Problem Detector + custom controller | Fast (OS-level checks) | Low (granular) | Must build | High |
| Cloud provider auto-healing (ASG) | Fast | Medium | Full (instance replacement) | Low but provider-specific |
| Manual monitoring + runbook | Slow (human response) | Low | None | Low (operationally expensive) |

**Selected: CAPI MachineHealthCheck + Node Problem Detector**

```yaml
apiVersion: cluster.x-k8s.io/v1beta1
kind: MachineHealthCheck
metadata:
  name: worker-health
  namespace: clusters
spec:
  clusterName: production-us-east
  selector:
    matchLabels:
      node-pool: general
  unhealthyConditions:
    - type: Ready
      status: Unknown    # Kubelet stopped reporting
      timeout: 300s      # 5 min grace period
    - type: Ready
      status: "False"    # Node explicitly unhealthy
      timeout: 300s
    - type: MemoryPressure
      status: "True"
      timeout: 120s
    - type: DiskPressure
      status: "True"
      timeout: 120s
  maxUnhealthy: "40%"    # Safety: don't remediate if > 40% are unhealthy
  nodeStartupTimeout: 600s  # 10 min for new machine to become Ready
  remediationTemplate:
    kind: Metal3Remediation  # or AWSRemediation (provider-specific)
    name: reboot-then-replace
```

**Remediation Flow:**

```
1. MachineHealthCheck watches Nodes matching selector
2. Detects node with Ready=Unknown for > 5 min
3. Checks maxUnhealthy threshold:
   - If > 40% of machines are unhealthy → DO NOT remediate (likely cluster-wide issue)
   - If < 40% → proceed with remediation
4. Sets Machine.status.conditions[HealthCheckSucceeded] = False
5. Remediation controller (provider-specific):
   a. First attempt: Reboot machine (bare-metal: IPMI reset / cloud: reboot API)
   b. Wait for nodeStartupTimeout
   c. If still unhealthy: Delete Machine → MachineSet creates replacement
6. New Machine provisioned → joins cluster → marked Ready
7. Old machine cleaned up (deprovisioned, returned to pool for bare-metal)
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| False positive (transient network glitch) | Unnecessary machine replacement | Increase timeout; use Node Problem Detector for more granular health signals |
| Cascade failure (> 40% nodes unhealthy) | Remediation paused; cluster degraded | maxUnhealthy threshold; alert ops team for manual investigation |
| Replacement machine also fails | Continuous churn; capacity not restored | Exponential backoff on remediation; alert on repeated failures |
| Bare-metal pool exhausted | Cannot replace failed machine; no spare hardware | Monitor BareMetalHost availability; maintain spare capacity (10-20%) |
| Management cluster unhealthy | No health checks or remediation | HA management cluster; separate monitoring for management cluster itself |

**Interviewer Q&As:**

**Q1: Why does MachineHealthCheck have a maxUnhealthy threshold?**
A: To prevent cascade failures. If a network partition causes 60% of nodes to appear unhealthy, blindly replacing them would: (1) Terminate actually-healthy machines. (2) Overwhelm the infrastructure provider with provisioning requests. (3) Cause massive pod eviction. The maxUnhealthy threshold acts as a circuit breaker — if too many machines are unhealthy, it's likely a cluster-wide issue (network, control plane) not individual machine failure.

**Q2: How do you handle machine remediation during a cluster upgrade?**
A: During upgrades, machines are already being replaced. MachineHealthCheck should be aware of this: (1) Machines being deleted as part of rolling update should not trigger remediation. (2) CAPI sets `cluster.x-k8s.io/delete-machine` annotation during planned deletions. (3) MachineHealthCheck skips machines with this annotation. (4) Additionally, set `maxUnhealthy` high enough to accommodate the upgrade surge (e.g., 60% if upgrading 50% of machines at a time).

**Q3: How do you test MachineHealthCheck without causing production outages?**
A: (1) Use a staging cluster with the same MHC configuration. (2) Inject failures: cordon a node and stop the kubelet to simulate Ready=Unknown. (3) Observe MHC behavior: detection time, remediation trigger, replacement provisioning. (4) Verify maxUnhealthy threshold by cordoning multiple nodes simultaneously. (5) Monitor `capi_machine_health_check_remediation_total` metric.

**Q4: How do you handle the case where a replaced machine gets the same IP address?**
A: In cloud environments, IPs are typically dynamic. In bare-metal, IPs may be statically assigned to the BMC/host. Solutions: (1) Use DHCP with MAC-based reservations for bare-metal. (2) Ensure the old Node object is deleted from the k8s API before the replacement joins (to avoid conflicting node objects). (3) CAPI handles this: it deletes the Node object as part of Machine deletion. (4) For StatefulSets with node affinity, the new node gets a different name — PVs may need to be reattached.

**Q5: What is the difference between MachineHealthCheck and the node lifecycle controller in kube-controller-manager?**
A: The node lifecycle controller (NLC) adds taints to unhealthy nodes and evicts pods, but it does not replace the underlying machine. MachineHealthCheck goes one step further: it detects unhealthy nodes and triggers machine replacement via CAPI. They work together: NLC evicts pods for immediate workload recovery, while MHC replaces the machine for capacity recovery. Think of NLC as reactive (move pods away) and MHC as curative (fix the infrastructure).

**Q6: How do you handle a bare-metal machine that is stuck in Provisioning state?**
A: (1) Check BareMetalHost status: is Ironic able to communicate with the BMC? (2) Check Ironic logs for PXE boot failures (DHCP, TFTP, HTTP issues). (3) Common causes: incorrect BMC credentials, network VLAN misconfiguration, firmware incompatibility. (4) Set a provisioning timeout in the infrastructure provider (Metal3 uses `spec.automatedCleaningMode`). (5) If stuck, manually power-cycle via IPMI and inspect the console output. (6) CAPM3 supports remediation strategies: reboot → reprovision → mark as failed.

---

### Deep Dive 3: Cluster Upgrades at Scale

**Why it's hard:**
Upgrading a Kubernetes cluster involves upgrading the control plane (etcd, API server, scheduler, controller-manager) and all worker nodes while maintaining service availability. At scale (100+ nodes), this must be automated, rollback-safe, and completion-tracked across the entire fleet.

**Implementation Detail:**

```
Control Plane Upgrade (KubeadmControlPlane):
  1. Create new control plane machine with target k8s version
  2. Wait for new machine to join and pass health checks
  3. Wait for etcd member to be healthy
  4. Remove one old control plane machine:
     a. Cordon the node
     b. Remove etcd member
     c. Drain pods (with PDB respect)
     d. Delete Machine
  5. Repeat until all control plane nodes are on new version
  
  Safety: only one control plane node replaced at a time
  Rollback: if new machine fails health check, delete it and keep old one
  Duration: ~5-10 min per control plane node

Worker Upgrade (MachineDeployment):
  1. MachineDeployment.spec.version updated
  2. New MachineSet created with new version
  3. Rolling update (like Deployment):
     - Scale up new MachineSet by maxSurge
     - Wait for new machines to be Ready
     - Scale down old MachineSet by maxUnavailable
     - Repeat
  4. Old MachineSet scaled to 0 and eventually deleted
  
  Safety: PodDisruptionBudgets respected during drain
  Rollback: update spec.version back to old version → new MachineSet created with old version
  Duration: ~3-5 min per worker node (cloud), ~15-25 min per node (bare-metal)

Fleet-wide upgrade strategy:
  1. Upgrade management cluster first (CAPI components)
  2. Upgrade dev/staging clusters (canary)
  3. Monitor for 24-48 hours
  4. Upgrade production clusters (region by region)
  5. Each cluster upgrade is an independent operation
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| New control plane node fails to join | Upgrade stalled; old nodes still serving | Automatic rollback (delete failed new machine); alert for investigation |
| Worker node fails to drain (PDB blocking) | Upgrade slowed; old node not removed | Set drain timeout; investigate PDB; manual override if needed |
| Infrastructure provider out of capacity | Cannot provision new machines | Retry with backoff; alert for capacity planning |
| Version skew violation | Kubelet too old for new API server features | CAPI enforces: upgrade control plane first, then workers |
| etcd data migration failure | Control plane unhealthy after upgrade | etcd backup before upgrade; restore from backup if needed |

**Interviewer Q&As:**

**Q1: How do you upgrade 500 clusters without overwhelming the infrastructure provider?**
A: (1) Use a fleet upgrade controller that limits concurrency (e.g., 10 clusters upgrading simultaneously). (2) Stagger by priority: dev → staging → production. (3) Within each priority tier, stagger by region to limit blast radius. (4) Use ClusterClass topology reconciliation — update the ClusterClass template version, and all clusters using that class get upgraded automatically (with rate limiting). (5) Monitor `capi_cluster_phase` metric to track progress.

**Q2: How do you handle a cluster upgrade that fails halfway through?**
A: CAPI upgrade is designed to be resumable: (1) Each Machine replacement is an independent operation. (2) If the upgrade is paused/failed, the cluster has a mix of old and new version nodes — this is fine within k8s version skew policy (kubelet can be N-2 of API server). (3) To resume: fix the root cause and the controller will continue from where it left off. (4) To rollback: update spec.version back to old version — new machines will be created with old version, and new-version machines will be replaced.

**Q3: How do you validate that a cluster upgrade was successful?**
A: (1) All nodes report the correct version: `kubectl get nodes -o custom-columns=NAME:.metadata.name,VERSION:.status.nodeInfo.kubeletVersion`. (2) All system pods are running: `kubectl get pods -n kube-system`. (3) E2e conformance tests pass (Sonobuoy). (4) Application health checks pass. (5) Custom SLO validation: API server latency, scheduling throughput, etc. (6) CAPI reports Cluster.status.phase=Provisioned and all MachineHealthChecks pass.

---

## 7. Scheduling & Resource Management

### Machine Scheduling in CAPI

Unlike pod scheduling (which assigns pods to existing nodes), CAPI "scheduling" involves:

1. **Machine placement**: Selecting which failure domain (AZ, rack) to place a new machine in.
2. **Resource selection**: For bare-metal, selecting which available BareMetalHost matches the machine requirements.
3. **Capacity management**: Ensuring sufficient infrastructure capacity for machine provisioning.

**Failure Domain Distribution:**

```yaml
# CAPI distributes machines across failure domains automatically
# InfraCluster reports available failure domains:
status:
  failureDomains:
    us-east-1a:
      controlPlane: true
      attributes:
        type: availability-zone
    us-east-1b:
      controlPlane: true
    us-east-1c:
      controlPlane: true

# KubeadmControlPlane distributes control plane nodes evenly:
# 3 CP nodes → 1 per AZ
# 5 CP nodes → round-robin across AZs

# MachineDeployment uses topology-aware placement:
spec:
  template:
    spec:
      failureDomain: "us-east-1a"  # Pin to specific AZ
# Or let CAPI distribute automatically (omit failureDomain)
```

**Bare-Metal Machine Selection:**

```
BareMetalHost selection for Machine:
1. MachineSet creates Machine with infrastructure spec (CPU, RAM, disk requirements)
2. CAPM3 controller looks for available BareMetalHosts matching:
   - State: Available
   - Hardware meets minimum requirements (from inspection data)
   - Labels match selector (optional: specific rack, hardware generation)
3. Claims the BareMetalHost (sets ownerReference)
4. Triggers provisioning via Ironic

Challenge: finite pool of hardware
Mitigation: monitor available BareMetalHosts; alert when pool < 20% capacity
```

---

## 8. Scaling Strategy

### Scaling the CAPI Platform

| Dimension | Strategy | Limit |
|-----------|---------|-------|
| Number of workload clusters | Single management cluster | Tested to ~500 clusters |
| Machines per cluster | MachineDeployment replicas | Limited by k8s node limit (~5,000) |
| Total machines under management | Management cluster etcd size | ~25,000 machines (before etcd becomes a bottleneck) |
| Provisioning throughput | Provider API rate limits | ~50 VMs/min (AWS), ~10 machines/min (bare-metal) |
| Geographic distribution | One management cluster per region or global with VPN | < 100ms latency to provider APIs |

**Multi-Management-Cluster Architecture (for > 500 clusters):**

```
                   ┌────────────────────┐
                   │  Fleet Manager     │
                   │  (GitOps + UI)     │
                   └────────┬───────────┘
                            │
              ┌─────────────┼─────────────┐
              │             │             │
    ┌─────────▼────┐ ┌─────▼────────┐ ┌──▼──────────┐
    │ Mgmt Cluster │ │ Mgmt Cluster │ │ Mgmt Cluster│
    │ US-East      │ │ US-West      │ │ EU-West     │
    │ (200 WC)     │ │ (150 WC)     │ │ (150 WC)    │
    └──────────────┘ └──────────────┘ └─────────────┘
```

### Interviewer Q&As

**Q1: How do you handle the management cluster being a single point of failure?**
A: (1) Run the management cluster as HA (3 CP nodes across AZs). (2) Workload clusters are autonomous — management cluster failure doesn't affect them. (3) Keep etcd backups every 30 min. (4) Maintain a documented recovery procedure (< 1 hour RTO). (5) For extreme HA requirements, run a standby management cluster with replicated Git state — `clusterctl restore` to activate. (6) Self-hosted pattern: the management cluster can manage itself, but you need an escape hatch (a separate small cluster or local kind cluster) for disaster recovery.

**Q2: How do you handle provider API rate limits when scaling to hundreds of clusters?**
A: (1) CAPI providers implement exponential backoff. (2) Stagger cluster creation (don't create 100 clusters simultaneously). (3) Request quota increases from the cloud provider. (4) Use dedicated cloud accounts per team/environment to isolate rate limits. (5) For bare-metal, the bottleneck is PXE provisioning bandwidth — limit concurrent provisions to avoid DHCP/TFTP saturation.

**Q3: How do you migrate CAPI management from one cluster to another?**
A: `clusterctl move` performs a live migration: (1) Pauses CAPI controllers on the source cluster. (2) Exports all CAPI objects (Clusters, Machines, Secrets). (3) Applies them to the target cluster. (4) Resumes CAPI controllers on the target. (5) Verifies all workload clusters are still healthy. The source cluster can then be decommissioned. This is used during management cluster upgrades and disaster recovery.

**Q4: How do you ensure consistency between Git (desired state) and the management cluster (actual state)?**
A: (1) Flux/ArgoCD continuously reconciles Git → management cluster. (2) Configure drift detection — alert when management cluster state differs from Git. (3) Use `prune: true` in Flux to delete objects that are removed from Git. (4) Use admission webhooks on the management cluster to prevent direct kubectl modifications (force all changes through Git). (5) Periodic audit: compare Git manifests against live cluster state and reconcile.

**Q5: How do you implement cluster templates that work across multiple providers?**
A: ClusterClass with variables: (1) Define a ClusterClass per provider (standard-aws, standard-vsphere, standard-baremetal). (2) Use common variables (version, workerCount, region) across all classes. (3) Platform tooling (CLI/UI) presents a unified interface that maps user intent to the correct ClusterClass. (4) Alternatively, use Crossplane Compositions to abstract away provider differences at a higher level. (5) The key is: provider-specific details (instance types, VM templates, BareMetalHost selectors) are encapsulated in the ClusterClass, not exposed to users.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure Scenario | Impact | Detection | Recovery | RTO |
|---|-----------------|--------|-----------|----------|-----|
| 1 | Management cluster API server down | No new operations; workload clusters unaffected | External health check | HA API server auto-recovery | < 30s |
| 2 | Management cluster etcd failure | All CAPI operations frozen | etcd health metrics | Restore from backup; workload clusters unaffected | 15-60 min |
| 3 | Infrastructure provider API outage | Cannot provision/delete machines | Provider status page; API error rates | Wait for provider recovery; retry queue | Provider-dependent |
| 4 | Workload cluster control plane failure | Workload cluster degraded | CAPI cluster health check | MachineHealthCheck replaces CP node | < 15 min |
| 5 | Worker node hardware failure | Capacity reduced in workload cluster | MachineHealthCheck (NodeReady=False) | Auto-replacement via MachineSet | < 15 min (cloud), < 30 min (bare-metal) |
| 6 | GitOps controller failure (Flux/ArgoCD) | No new changes applied; existing state maintained | Controller health metrics | Restart controller; state eventually converges | < 5 min |
| 7 | Certificate expiry (management ↔ workload) | Cannot access workload cluster from management | cert-manager renewal alerts | Rotate certificates; CAPI handles rotation for kubeadm certs | < 30 min |
| 8 | Network partition (management ↔ workload) | Health checks fail; false positive remediation risk | Network monitoring | maxUnhealthy prevents cascade; heal network | Variable |
| 9 | CAPI controller bug | Incorrect reconciliation; potential cluster corruption | Monitoring reconciliation errors | Roll back CAPI version; manual intervention | < 1 hour |
| 10 | Bare-metal Ironic failure | Cannot provision new bare-metal machines | Ironic health metrics | Restart Ironic; backlog clears | < 15 min |

### Automated Recovery

| Mechanism | Implementation |
|-----------|---------------|
| MachineHealthCheck | Auto-replaces unhealthy machines (with safety thresholds) |
| MachineSet controller | Maintains desired replica count (auto-creates machines on deletion) |
| KubeadmControlPlane | Self-healing control plane (etcd member management, cert rotation) |
| Flux/ArgoCD | Self-healing cluster configuration (drift detection + reconciliation) |
| etcd backup CronJob | Automated snapshots every 30 min for all clusters |
| Certificate rotation | cert-manager or kubeadm auto-rotation for all TLS certs |

---

## 10. Observability

### Key Metrics

| Metric | Source | Alert Threshold | Meaning |
|--------|--------|----------------|---------|
| `capi_cluster_phase` | CAPI controller | != Provisioned for > 30 min | Cluster stuck in provisioning |
| `capi_machine_phase` | CAPI controller | == Failed for > 5 min | Machine provisioning failed |
| `capi_machine_health_check_remediation_total` | CAPI controller | > 5/hour for a cluster | Excessive machine replacements (flapping) |
| `capi_machine_health_check_short_circuited` | CAPI controller | > 0 | maxUnhealthy threshold hit; remediation paused |
| Infrastructure provider API errors | Provider controller | > 10% error rate | Provider API issues |
| Machine provisioning duration | Provider controller | p99 > 2x baseline | Provisioning slowdown |
| Cluster upgrade duration | Custom metric | > 2 hours for 100-node cluster | Upgrade stalled |
| Management cluster etcd size | etcd | > 4 GB | Approaching etcd limits |
| BareMetalHost availability | CAPM3 | < 20% of pool | Running low on spare hardware |
| Flux reconciliation lag | Flux | > 5 min | GitOps sync delayed |

### Distributed Tracing

```
Cluster lifecycle tracing:
1. Git commit → Flux reconciliation → API apply → CAPI controller reconciliation
2. Machine creation → Provider API call → VM/BMH provisioning → Bootstrap → Node join
3. Trace ID propagated through all steps for end-to-end visibility

Implementation:
- CAPI controllers instrumented with OpenTelemetry
- Provider controllers add spans for infrastructure API calls
- Export to Jaeger/Tempo for visualization
- Useful for debugging: "Why did this cluster take 45 min to provision?"
```

### Logging

```
Key log sources:
1. CAPI core controllers (cluster, machine, machineset, machinedeployment)
2. Infrastructure provider controllers (CAPA, CAPV, CAPM3)
3. Bootstrap provider (kubeadm config generation)
4. Flux/ArgoCD (GitOps sync status)
5. Ironic (bare-metal provisioning details)

Structured logging fields:
- cluster: cluster name
- machine: machine name
- provider: infrastructure provider
- operation: create/update/delete/upgrade
- phase: current lifecycle phase
- duration: operation duration

Ship to centralized logging (Elasticsearch/Loki) with cluster-name index.
```

---

## 11. Security

### Auth & AuthZ

**Management Cluster Access Control:**

```yaml
# Platform team: full access to CAPI objects
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRole
metadata:
  name: capi-platform-admin
rules:
- apiGroups: ["cluster.x-k8s.io"]
  resources: ["*"]
  verbs: ["*"]
- apiGroups: ["infrastructure.cluster.x-k8s.io"]
  resources: ["*"]
  verbs: ["*"]
- apiGroups: ["bootstrap.cluster.x-k8s.io"]
  resources: ["*"]
  verbs: ["*"]
---
# Application team: view-only for their namespace + scale operations
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: capi-tenant-operator
  namespace: team-alpha
rules:
- apiGroups: ["cluster.x-k8s.io"]
  resources: ["clusters"]
  verbs: ["get", "list", "watch"]
- apiGroups: ["cluster.x-k8s.io"]
  resources: ["machinedeployments/scale"]
  verbs: ["update", "patch"]
- apiGroups: ["cluster.x-k8s.io"]
  resources: ["clusters"]
  verbs: ["create"]
  # Only via ClusterClass — enforced by admission webhook
```

**Workload Cluster Credential Management:**

```
Challenge: management cluster stores kubeconfigs for all workload clusters
  
Solution:
1. Store kubeconfigs as Kubernetes Secrets in the management cluster
2. Encrypt secrets at rest (--encryption-provider-config)
3. Use short-lived kubeconfigs with automatic rotation
4. Scope access: only CAPI controllers have access to workload cluster kubeconfigs
5. For human access: use OIDC on workload clusters (not shared kubeconfigs)
6. For GitOps: ArgoCD uses its own service account tokens per workload cluster
```

### Multi-tenancy Isolation

| Layer | Mechanism |
|-------|-----------|
| Namespace | One namespace per team in management cluster |
| RBAC | Team-scoped roles; cannot see other teams' clusters |
| ClusterClass | Enforced templates prevent teams from creating non-compliant clusters |
| Admission webhook | Validate cluster spec against organizational policies |
| Network policy | Management cluster network policies isolate controller traffic |
| Resource quotas | Limit clusters/machines per team namespace |
| Audit logging | Track who created/modified/deleted which clusters |

---

## 12. Incremental Rollout Strategy

### Phase 1: Bootstrap (Week 1-2)
- Deploy management cluster (3 CP nodes, HA).
- Install CAPI core + one infrastructure provider (e.g., AWS).
- Create first workload cluster manually with clusterctl.
- Validate end-to-end lifecycle: create, scale, upgrade, delete.

### Phase 2: GitOps Integration (Week 3-4)
- Set up Flux/ArgoCD on management cluster.
- Define ClusterClass templates for standardized provisioning.
- Create second workload cluster via Git commit.
- Implement PR-based approval for cluster changes.

### Phase 3: Multi-Provider (Week 5-6)
- Add vSphere and/or Metal3 infrastructure providers.
- Create workload clusters on each provider.
- Validate MachineHealthCheck and auto-remediation per provider.
- Document provider-specific operational procedures.

### Phase 4: Self-Service (Week 7-8)
- Build platform CLI/UI for team self-service.
- Implement RBAC and namespace isolation.
- Define admission policies (max cluster size, approved regions, required labels).
- Onboard first application team.

### Phase 5: Fleet Management (Week 9-10)
- Implement fleet-wide upgrade strategy.
- Set up monitoring and alerting for all workload clusters.
- Implement cost attribution per team/cluster.
- Document disaster recovery procedures.

### Rollout Interviewer Q&As

**Q1: How do you handle the transition from manually provisioned clusters to CAPI-managed clusters?**
A: (1) Cannot directly import existing clusters into CAPI (no adoption mechanism). (2) Strategy: create new CAPI-managed clusters alongside existing ones. (3) Migrate workloads using blue-green or canary approach. (4) Decommission old clusters once migration is complete. (5) For clusters that cannot be migrated (legacy, special requirements), keep them as unmanaged but monitor them separately.

**Q2: How do you test CAPI upgrades before applying to production?**
A: (1) Run `clusterctl upgrade plan` to see available upgrades. (2) Apply upgrade to a staging management cluster first. (3) Verify all managed workload clusters still healthy. (4) Test critical operations (create, upgrade, delete cluster) on staging. (5) Apply to production with monitoring. (6) CAPI follows Kubernetes version skew policy — management cluster components should not be more than 1 minor version apart.

**Q3: How do you handle CAPI in an air-gapped environment?**
A: (1) Mirror container images to internal registry. (2) Configure CAPI providers to use internal registry (`clusterctl config`). (3) For bare-metal: mirror OS images for Ironic to internal HTTP server. (4) For kubeadm bootstrap: configure internal package mirror for k8s components. (5) Mirror clusterctl binary and provider metadata. (6) Pre-download all required artifacts as part of the deployment pipeline.

**Q4: How do you implement cost tracking for clusters provisioned via CAPI?**
A: (1) Tag all infrastructure resources with cluster-name, team, environment labels. (2) CAPI providers propagate labels to cloud resources (EC2 tags, vSphere custom attributes). (3) Use cloud cost allocation tools (AWS Cost Explorer, GCP Billing) grouped by tags. (4) For bare-metal: track BareMetalHost allocation per team. (5) Build a cost dashboard aggregating cloud + bare-metal costs per team/cluster.

**Q5: How do you ensure cluster configurations don't drift from the template?**
A: (1) ClusterClass topology controller continuously reconciles cluster topology against the class definition. (2) Changes outside the class variables are overwritten. (3) Flux/ArgoCD detects drift between Git and management cluster. (4) Admission webhooks prevent direct modifications that bypass Git. (5) Regular audit: compare running cluster configurations against ClusterClass templates and report deviations.

---

## 13. Trade-offs & Decision Log

| # | Decision | Alternative Considered | Trade-off | Rationale |
|---|----------|----------------------|-----------|-----------|
| 1 | CAPI for cluster lifecycle | Terraform + custom controller | Less flexible but standardized k8s-native workflow | CAPI provides reconciliation loops, health checks, and upgrade orchestration out of the box |
| 2 | ClusterClass templates | Helm charts for cluster manifests | Less flexible variable system but stronger type safety | ClusterClass enforces consistency and enables fleet-wide operations |
| 3 | GitOps for cluster state | Direct API access | Slower feedback but auditable and reversible | Git provides audit trail, PR-based approval, rollback capability |
| 4 | kubeadm bootstrap | Talos, k3s | Less secure by default but most widely supported | kubeadm is CAPI's default; Talos adds immutable OS benefits but is less mature |
| 5 | Metal3 for bare-metal | MAAS, custom PXE | More complex but integrated with CAPI lifecycle | Metal3 plugs directly into CAPI's Machine abstraction |
| 6 | MachineHealthCheck auto-remediation | Manual intervention only | Risk of false positive replacement but faster recovery | Automated healing critical for large fleets; maxUnhealthy threshold provides safety |
| 7 | One management cluster per region | Single global management cluster | Operational overhead but lower latency to providers | Avoids cross-region API latency; isolates regional failures |
| 8 | Namespace-per-team isolation | Cluster-per-team management | Less isolated but simpler operations | Sufficient isolation with RBAC; cluster-per-team is only needed for regulated environments |
| 9 | Immutable machine replacement | In-place upgrades | More infrastructure churn but safer rollback | Immutable replacement eliminates state accumulation and ensures consistency |

---

## 14. Agentic AI Integration

### AI-Powered Fleet Management

| Use Case | AI Agent Capability | Implementation |
|----------|-------------------|---------------|
| **Intelligent cluster sizing** | Recommend initial cluster size based on workload requirements | Agent analyzes workload manifests (resource requests, replica counts) and recommends node count + instance type |
| **Predictive capacity planning** | Forecast when clusters will need more capacity | Agent analyzes growth trends in pod count, resource utilization; triggers pre-emptive scaling |
| **Upgrade risk assessment** | Assess risk of upgrading a specific cluster | Agent checks: deprecated APIs in use, known CVEs in target version, cluster-specific configurations that may break |
| **Anomaly-based health checks** | Detect unhealthy machines before node conditions trigger | Agent monitors OS-level metrics (disk I/O latency, kernel panics, memory errors) and flags machines before k8s detects the problem |
| **Cost optimization** | Recommend right-sizing across the fleet | Agent analyzes resource utilization across all clusters and recommends consolidation, right-sizing, or workload migration |
| **Incident correlation** | Correlate machine failures with infrastructure patterns | Agent identifies: "3 machines in rack-12 failed this week — possible hardware batch issue" |

### Example: AI-Driven Upgrade Planner

```python
class ClusterUpgradeAgent:
    """
    Assesses risk and plans fleet-wide upgrades.
    """
    
    def assess_upgrade_risk(self, cluster_name: str, target_version: str) -> UpgradeRisk:
        # Check deprecated APIs
        deprecated_apis = self.api_audit.find_deprecated_apis(
            cluster=cluster_name,
            target_version=target_version
        )
        
        # Check known issues
        known_issues = self.cve_db.check_version(target_version)
        
        # Check workload compatibility
        incompatible_workloads = self.workload_scanner.check_compatibility(
            cluster=cluster_name,
            target_version=target_version
        )
        
        # Analyze historical upgrade success rate
        historical_success = self.metrics.query(
            f'capi_cluster_upgrade_success_rate{{target_version="{target_version}"}}'
        )
        
        risk_score = self.calculate_risk(
            deprecated_apis, known_issues, incompatible_workloads, historical_success
        )
        
        return UpgradeRisk(
            cluster=cluster_name,
            target_version=target_version,
            score=risk_score,
            deprecated_apis=deprecated_apis,
            blockers=incompatible_workloads,
            recommendation="Proceed" if risk_score < 0.3 else "Review manually",
            suggested_order=self.calculate_upgrade_order()  # dev → staging → prod
        )
    
    def plan_fleet_upgrade(self, target_version: str) -> FleetUpgradePlan:
        clusters = self.capi_client.list_clusters()
        
        # Assess each cluster
        assessments = [self.assess_upgrade_risk(c.name, target_version) for c in clusters]
        
        # Group by risk level and suggest upgrade waves
        waves = self.group_into_waves(assessments)
        
        return FleetUpgradePlan(
            target_version=target_version,
            total_clusters=len(clusters),
            waves=waves,  # [{wave: 1, clusters: [dev-1, dev-2], risk: low}, ...]
            estimated_duration=self.estimate_duration(waves),
            blockers=[a for a in assessments if a.score > 0.7]
        )
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Explain the relationship between Cluster, Machine, MachineSet, and MachineDeployment in CAPI.**
A: It mirrors the Kubernetes workload hierarchy: (1) **MachineDeployment** is like a Deployment — it manages MachineSet revisions for rolling updates. (2) **MachineSet** is like a ReplicaSet — it maintains a desired count of Machines with the same template. (3) **Machine** is like a Pod — it represents a single node with references to infrastructure (AWSMachine) and bootstrap (KubeadmConfig). (4) **Cluster** is the top-level object that ties everything together, including control plane and worker references. Changing the Machine template in MachineDeployment triggers a new MachineSet creation and rolling replacement, just like updating a Deployment's pod template.

**Q2: How does CAPI differ from Terraform for cluster provisioning?**
A: (1) CAPI is declarative with continuous reconciliation — if a machine dies, CAPI replaces it automatically. Terraform only runs when invoked. (2) CAPI understands Kubernetes semantics — it can drain nodes before deletion, manage etcd membership, and orchestrate control plane upgrades. Terraform manages VMs but doesn't understand Kubernetes. (3) CAPI uses CRDs — cluster state lives in the management cluster's etcd, visible via kubectl. Terraform state is a separate state file. (4) CAPI is less flexible per-resource but provides a complete cluster lifecycle out of the box.

**Q3: What is `clusterctl move` and when would you use it?**
A: `clusterctl move` migrates all CAPI objects from one management cluster to another. Use cases: (1) **Pivot from bootstrap**: initial setup uses kind as temporary management cluster, then moves to the real management cluster. (2) **Management cluster upgrade**: create a new management cluster, move state, decommission old one. (3) **Disaster recovery**: move management to a healthy cluster. Implementation: pauses controllers on source, exports all objects (preserving UIDs), applies to target, resumes. Workload clusters are unaffected during the move.

**Q4: How do you implement day-2 operations (cert rotation, etcd backup) for workload clusters managed by CAPI?**
A: (1) **Cert rotation**: KubeadmControlPlane automatically rotates certificates before expiry (configurable). For custom certs, use cert-manager on the workload cluster. (2) **etcd backup**: Deploy a CronJob on each workload cluster (or a centralized backup controller on the management cluster) that takes etcd snapshots and uploads to object storage. (3) **Config drift**: Deploy policy controllers (OPA, Kyverno) on workload clusters. (4) **OS patching**: Update the Machine template with new OS image → MachineDeployment triggers rolling replacement.

**Q5: How does CAPI handle the upgrade of a 3-node etcd cluster as part of a control plane upgrade?**
A: KubeadmControlPlane manages etcd as part of the control plane Machine. During upgrade: (1) Create new Machine with new k8s version → etcd member joins as learner, promoted to voter. (2) Verify etcd cluster is healthy (4 members). (3) Remove old Machine → its etcd member is removed. (4) Back to 3 members with one upgraded. (5) Repeat for remaining 2 nodes. At no point does the etcd cluster lose quorum (always has 3+ healthy members). The KubeadmControlPlane controller orchestrates this sequence and verifies etcd health at each step.

**Q6: How do you handle heterogeneous worker node pools in CAPI?**
A: Use multiple MachineDeployments per cluster, each with a different machine template. Example: (1) `general-purpose` pool: m5.2xlarge, 50 nodes, for stateless web services. (2) `memory-optimized` pool: r5.4xlarge, 10 nodes, for Elasticsearch. (3) `gpu` pool: p3.2xlarge, 5 nodes, for ML inference. Each pool has its own MachineDeployment with appropriate instance type, labels, and taints. Workloads use node affinity/tolerations to target the right pool.

**Q7: What is the CAPI contract and why does it matter?**
A: The CAPI contract defines the expected behavior of infrastructure and bootstrap providers. Key contract requirements: (1) Infrastructure provider must set `status.ready=true` when the resource is provisioned. (2) Infrastructure must report `spec.providerID` matching the node's provider ID. (3) Bootstrap provider must set `status.ready=true` and `status.dataSecretName` when bootstrap data is generated. (4) Providers must handle paused clusters (stop reconciliation when `spec.paused=true`). The contract enables CAPI core to work with any compliant provider without knowledge of provider internals.

**Q8: How do you debug a Machine that is stuck in Provisioning state?**
A: (1) Check Machine status: `kubectl describe machine <name>` — look at conditions and events. (2) Check the infrastructure object (AWSMachine, VSphereMachine): is it reporting errors from the cloud API? (3) Check the bootstrap object (KubeadmConfig): is bootstrap data generated? (4) For cloud: check the cloud console — is the VM running? Can it reach the API server? (5) For bare-metal: check BareMetalHost status and Ironic logs — is PXE boot succeeding? (6) Common issues: incorrect AMI, security group blocking API server access, insufficient IAM permissions, kubeadm join token expired.

**Q9: How do you implement a cluster "pause" for maintenance windows?**
A: Set `spec.paused: true` on the Cluster object. This: (1) Pauses all CAPI controllers for this cluster — no machine creation, deletion, or upgrade. (2) MachineHealthCheck stops remediating. (3) Useful for planned maintenance where you don't want CAPI interfering. (4) The cluster itself continues to run — only CAPI management is paused. (5) Resume by setting `spec.paused: false`. (6) Individual MachineDeployments can also be paused independently for more granular control.

**Q10: How do you handle secrets management for CAPI in a multi-tenant environment?**
A: (1) Each team's namespace contains only their cluster secrets. (2) RBAC prevents cross-namespace access. (3) Use Sealed Secrets or External Secrets Operator to encrypt secrets in Git. (4) Cloud provider credentials: use IAM instance profiles (AWS) or Workload Identity (GCP) instead of static credentials where possible. (5) For bare-metal BMC credentials: store in Vault, reference via External Secrets. (6) Audit log all secret access on the management cluster.

**Q11: How does CAPI handle the case where a workload cluster's API server is unreachable from the management cluster?**
A: (1) CAPI periodically checks workload cluster health by connecting to its API server. (2) If unreachable, Cluster.status conditions show connectivity failure. (3) MachineHealthCheck cannot query node conditions — it falls back to Machine-level infrastructure health (cloud VM status). (4) If the workload cluster API server is down due to control plane node failure, KubeadmControlPlane will attempt to replace the machine (if it can detect the infrastructure is unhealthy via the provider). (5) If it's a network partition, maxUnhealthy prevents cascade remediation.

**Q12: How do you implement cluster decommissioning safely?**
A: (1) Drain all workloads from the cluster (migrate to another cluster). (2) Verify no persistent data remains (PVs, external storage). (3) Remove GitOps entries (Flux/ArgoCD stops syncing applications). (4) Delete the Cluster object from the management cluster. (5) CAPI cascading deletion: Cluster → MachineDeployments → MachineSets → Machines → InfrastructureMachines → actual VMs/BareMetalHosts. (6) Verify infrastructure resources are cleaned up (check cloud console). (7) For bare-metal: BareMetalHosts return to Available state in the pool.

**Q13: How do you handle version skew between CAPI components and workload cluster versions?**
A: CAPI supports a range of Kubernetes versions for workload clusters (typically current + 2-3 prior minor versions). The contract defines: (1) Bootstrap provider must generate valid bootstrap data for the target version. (2) Infrastructure provider is version-agnostic (it provisions VMs, not k8s). (3) Management cluster k8s version is independent of workload cluster versions. (4) CAPI core tracks which k8s versions it supports via the `spec.version` field validation.

**Q14: How does CAPI handle provider-specific features that don't fit the generic abstraction?**
A: Through the infrastructure-specific CRDs. The generic Machine references an AWSMachine (or VSphereMachine, etc.) that contains provider-specific fields (instance type, security groups, placement groups, GPU configuration). This separation allows CAPI core to be provider-agnostic while still exposing full provider capabilities. ClusterClass variables can include provider-specific variables that map to these fields.

**Q15: How do you implement a self-service portal on top of CAPI?**
A: (1) Build a web UI/CLI that abstracts CAPI CRDs into user-friendly forms. (2) Backend creates Cluster manifests from ClusterClass templates, filling in user-provided variables. (3) Submit to Git (PR-based) or directly to management cluster API. (4) Expose cluster status, kubeconfig download, and scaling operations. (5) Implement approval workflows for production clusters (Slack/PagerDuty integration). (6) Add cost estimation before provisioning (query cloud pricing APIs). (7) Backstage.io with a CAPI plugin is a popular choice.

---

## 16. References

| # | Reference | URL |
|---|-----------|-----|
| 1 | Cluster API Book | https://cluster-api.sigs.k8s.io/ |
| 2 | Cluster API GitHub | https://github.com/kubernetes-sigs/cluster-api |
| 3 | CAPA (AWS Provider) | https://github.com/kubernetes-sigs/cluster-api-provider-aws |
| 4 | CAPV (vSphere Provider) | https://github.com/kubernetes-sigs/cluster-api-provider-vsphere |
| 5 | Metal3 (Bare-metal Provider) | https://metal3.io/ |
| 6 | ClusterClass KEP | https://github.com/kubernetes-sigs/cluster-api/blob/main/docs/proposals/20210526-cluster-class-and-managed-topologies.md |
| 7 | Flux CD | https://fluxcd.io/ |
| 8 | ArgoCD | https://argo-cd.readthedocs.io/ |
| 9 | clusterctl CLI | https://cluster-api.sigs.k8s.io/clusterctl/overview |
| 10 | Kubernetes Version Skew Policy | https://kubernetes.io/releases/version-skew-policy/ |
