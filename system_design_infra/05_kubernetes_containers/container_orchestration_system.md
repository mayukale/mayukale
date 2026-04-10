# System Design: Container Orchestration System

> **Relevance to role:** A cloud infrastructure platform engineer operates the full container lifecycle stack — from image pull through scheduling, networking, storage, and runtime security. This document covers the Kubernetes data plane: how containers actually run, how they communicate, how they access storage, and how they are secured. This is the layer where abstractions meet reality and where most production incidents surface.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Manage container lifecycle: create, start, stop, restart, delete |
| FR-2 | Provide resource isolation (CPU, memory, disk I/O) via cgroups and namespaces |
| FR-3 | Support persistent storage via PersistentVolumes, PersistentVolumeClaims, and CSI drivers |
| FR-4 | Provide pod-to-pod networking with a flat network model (every pod gets a unique IP) |
| FR-5 | Implement service discovery and load balancing (ClusterIP, NodePort, LoadBalancer) |
| FR-6 | Enforce pod security standards (Restricted, Baseline, Privileged) |
| FR-7 | Support namespace isolation with resource quotas and limit ranges |
| FR-8 | Manage container images: pull, cache, garbage collect |
| FR-9 | Support init containers, sidecar containers, and ephemeral containers |
| FR-10 | Provide health checking: liveness, readiness, and startup probes |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Container startup time (from scheduled to running) | < 30s (cached image), < 120s (cold pull) |
| NFR-2 | Pod-to-pod network latency (same node) | < 0.1 ms |
| NFR-3 | Pod-to-pod network latency (cross-node) | < 1 ms (same AZ) |
| NFR-4 | Service discovery latency (endpoint update) | < 5s from pod Ready to endpoint available |
| NFR-5 | Storage attach time (EBS/iSCSI) | < 30s |
| NFR-6 | Node pod density | Up to 110 pods/node (default), 250 with tuning |
| NFR-7 | Image pull throughput | ≥ 100 MB/s from local registry |
| NFR-8 | Container restart on crash | < 10s (with exponential backoff: 10s, 20s, 40s, ... 5 min max) |

### Constraints & Assumptions
- Container runtime: containerd (CRI-compliant, Docker removed in k8s 1.24).
- CNI plugin: Calico (network policy support) or Cilium (eBPF-based).
- CSI drivers for cloud (EBS, GCE PD) and bare-metal (Ceph, NFS, local volumes).
- OCI-compliant container images.
- Linux kernel 5.10+ for modern cgroup v2 and eBPF features.
- Nodes run Ubuntu 22.04 or Flatcar Container Linux.

### Out of Scope
- Control plane internals (covered in kubernetes_control_plane.md).
- Service mesh (covered in service_mesh_design.md).
- Autoscaling (covered in pod_autoscaler_system.md).
- Multi-cluster orchestration (covered in kubernetes_cluster_api_platform.md).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Result |
|--------|------------|--------|
| Nodes | Large production cluster | 5,000 |
| Pods per node | Average 30 (max 110) | 150,000 total |
| Containers per pod | Average 1.5 (main + sidecars) | 225,000 containers |
| Image pulls per hour | ~500 new pods/hour x 1.5 images | ~750 pulls/hour |
| Network connections (pod-to-pod) | 150,000 pods x 10 active connections avg | 1,500,000 connections |
| Services | 10,000 services x 50 endpoints avg | 500,000 endpoints |
| PersistentVolumeClaims | 20% of pods use PVCs → 30,000 | 30,000 |
| Container restarts per hour | 0.5% failure rate x 225,000 containers | ~1,125 restarts/hour |

### Latency Requirements

| Operation | Target | Notes |
|-----------|--------|-------|
| Container start (cached image) | < 5s | containerd creates and starts container |
| Container start (image pull) | < 60s | Depends on image size and registry proximity |
| Liveness probe failure → restart | < 30s | failureThreshold x periodSeconds + restart time |
| Readiness probe → endpoint update | < 5s | kube-proxy/Cilium updates iptables/eBPF maps |
| Volume attach (cloud block) | < 30s | Cloud API + kernel device discovery |
| Volume attach (local) | < 1s | Already on the node |
| DNS resolution (CoreDNS) | < 5 ms | In-cluster caching DNS |
| Service ClusterIP → pod | < 1 ms | iptables/IPVS/eBPF in-kernel forwarding |

### Storage Estimates

| Component | Calculation | Result |
|-----------|------------|--------|
| Container images on node | 30 pods x 500 MB avg image (layer shared) | ~5 GB per node |
| Container writable layer | 30 pods x 100 MB avg | ~3 GB per node |
| Container logs | 30 pods x 10 MB/day x 7 days retention | ~2.1 GB per node |
| PV storage (cluster total) | 30,000 PVCs x 50 GB avg | ~1.5 PB |
| etcd (Endpoints/EndpointSlices) | 10,000 services x 5 KB | ~50 MB |

### Bandwidth Estimates

| Flow | Calculation | Result |
|------|------------|--------|
| Image pull traffic | 750 pulls/hour x 200 MB avg | ~42 GB/hour = ~11.7 MB/s |
| Pod-to-pod east-west traffic | 150,000 pods x 100 KB/s avg | ~15 GB/s cluster-wide |
| Service traffic (north-south) | 10,000 services x 10 MB/s avg | ~100 GB/s ingress |
| Log shipping | 5,000 nodes x 50 KB/s | ~250 MB/s |
| Kubelet → API server | 5,000 x 1 KB/10s heartbeat | ~500 KB/s |

---

## 3. High Level Architecture

```
                    ┌──────────────────────────────────────────────┐
                    │                  Node                        │
                    │                                              │
                    │  ┌──────────────────────────────────────┐   │
                    │  │              kubelet                  │   │
                    │  │  - Watches API server for pod specs  │   │
                    │  │  - Manages container lifecycle       │   │
                    │  │  - Reports node/pod status           │   │
                    │  │  - Runs probes (liveness/readiness)  │   │
                    │  │  - Manages volumes (CSI attach/mount)│   │
                    │  │  - Garbage collects images/containers│   │
                    │  └───────────┬──────────────────────────┘   │
                    │              │ CRI (gRPC)                    │
                    │  ┌───────────▼──────────────────────────┐   │
                    │  │           containerd                  │   │
                    │  │  - Image pull/push/store              │   │
                    │  │  - Container create/start/stop        │   │
                    │  │  - Snapshot management (overlayfs)    │   │
                    │  └───────────┬──────────────────────────┘   │
                    │              │ OCI runtime spec              │
                    │  ┌───────────▼──────────────────────────┐   │
                    │  │           runc                        │   │
                    │  │  - Create Linux namespaces            │   │
                    │  │  - Configure cgroups (v2)             │   │
                    │  │  - Set up rootfs (pivot_root)         │   │
                    │  │  - Execute container entrypoint       │   │
                    │  └──────────────────────────────────────┘   │
                    │                                              │
                    │  ┌──────────────────┐  ┌─────────────────┐  │
                    │  │  CNI Plugin       │  │  CSI Node Plugin│  │
                    │  │  (Calico/Cilium)  │  │  (EBS/Ceph/NFS)│  │
                    │  │  - veth pair      │  │  - Volume mount │  │
                    │  │  - IP allocation  │  │  - Device attach│  │
                    │  │  - Network policy │  │  - Filesystem   │  │
                    │  └──────────────────┘  └─────────────────┘  │
                    │                                              │
                    │  ┌──────────────────┐  ┌─────────────────┐  │
                    │  │  kube-proxy       │  │  CoreDNS (pod)  │  │
                    │  │  (iptables/IPVS/  │  │  - Service DNS  │  │
                    │  │   eBPF via Cilium)│  │  - Pod DNS      │  │
                    │  └──────────────────┘  └─────────────────┘  │
                    └──────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **kubelet** | Primary node agent; manages pod lifecycle, probes, volumes, status reporting |
| **containerd** | Container runtime implementing CRI; manages images, snapshots, containers |
| **runc** | Low-level OCI runtime; creates Linux namespaces/cgroups, starts container process |
| **CNI plugin** | Configures pod networking: IP address, veth pair, routes, network policies |
| **CSI plugin** | Manages persistent volume lifecycle: provision, attach, mount, snapshot |
| **kube-proxy** | Implements Service abstraction via iptables/IPVS/eBPF rules |
| **CoreDNS** | In-cluster DNS server for service discovery (`svc.cluster.local`) |

### Data Flows

1. **Pod startup flow:**
   ```
   API server assigns pod to node (spec.nodeName set)
   → kubelet watches pod spec via informer
   → kubelet calls CNI to set up networking (allocate IP, create veth pair)
   → kubelet calls CSI to attach and mount volumes
   → kubelet calls containerd (CRI) to pull image
   → containerd pulls image layers, creates snapshot (overlayfs)
   → kubelet calls containerd to create container (passes OCI spec)
   → containerd calls runc to create namespaces, cgroups, start process
   → kubelet runs startup probe → readiness probe → pod Ready
   → Endpoint controller adds pod IP to Service endpoints
   → kube-proxy/Cilium updates forwarding rules
   ```

2. **Service traffic flow (ClusterIP):**
   ```
   Pod A sends request to my-svc.default.svc.cluster.local:80
   → CoreDNS resolves to ClusterIP 10.96.0.42
   → Pod A's TCP connect to 10.96.0.42:80
   → kube-proxy iptables DNAT rule rewrites to pod IP 10.244.3.17:8080
   → Packet routed via CNI to destination node
   → Destination pod receives on port 8080
   ```

3. **Volume mount flow (PVC with CSI):**
   ```
   Pod spec references PVC → PVC bound to PV → PV has CSI driver reference
   → kubelet calls CSI ControllerPublishVolume (attach to node)
   → CSI driver attaches block device (e.g., EBS volume to EC2 instance)
   → kubelet calls CSI NodeStageVolume (format if needed, mount to staging dir)
   → kubelet calls CSI NodePublishVolume (bind mount to pod's volume path)
   → Container sees mounted filesystem at /data (or configured mountPath)
   ```

---

## 4. Data Model

### Core Entities & Schema

```yaml
# Pod — the atomic scheduling and execution unit
apiVersion: v1
kind: Pod
metadata:
  name: web-server
  namespace: production
spec:
  initContainers:
    - name: init-db
      image: busybox:1.36
      command: ['sh', '-c', 'until nslookup db-service; do sleep 2; done']
  containers:
    - name: web
      image: nginx:1.27
      ports:
        - containerPort: 80
      resources:
        requests:
          cpu: "250m"        # 0.25 CPU cores reserved
          memory: "256Mi"    # 256 MiB reserved
        limits:
          cpu: "500m"        # Throttled beyond this (CFS bandwidth)
          memory: "512Mi"    # OOM killed beyond this
      livenessProbe:
        httpGet:
          path: /healthz
          port: 80
        initialDelaySeconds: 10
        periodSeconds: 10
        failureThreshold: 3
      readinessProbe:
        httpGet:
          path: /ready
          port: 80
        periodSeconds: 5
        failureThreshold: 2
      startupProbe:
        httpGet:
          path: /healthz
          port: 80
        failureThreshold: 30
        periodSeconds: 10     # 30 x 10 = 300s max startup time
      volumeMounts:
        - name: data
          mountPath: /data
        - name: config
          mountPath: /etc/config
          readOnly: true
  volumes:
    - name: data
      persistentVolumeClaim:
        claimName: web-data-pvc
    - name: config
      configMap:
        name: web-config
  securityContext:
    runAsNonRoot: true
    runAsUser: 1000
    fsGroup: 2000
    seccompProfile:
      type: RuntimeDefault
  serviceAccountName: web-sa
  topologySpreadConstraints:
    - maxSkew: 1
      topologyKey: topology.kubernetes.io/zone
      whenUnsatisfiable: DoNotSchedule
      labelSelector:
        matchLabels:
          app: web

---
# PersistentVolumeClaim
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: web-data-pvc
  namespace: production
spec:
  accessModes:
    - ReadWriteOnce
  storageClassName: fast-ssd
  resources:
    requests:
      storage: 50Gi

---
# StorageClass
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: fast-ssd
provisioner: ebs.csi.aws.com    # or ceph-rbd.csi.ceph.com
parameters:
  type: gp3
  iops: "3000"
  throughput: "125"
  encrypted: "true"
reclaimPolicy: Retain
allowVolumeExpansion: true
volumeBindingMode: WaitForFirstConsumer

---
# Service
apiVersion: v1
kind: Service
metadata:
  name: web-service
  namespace: production
spec:
  type: ClusterIP
  selector:
    app: web
  ports:
    - port: 80
      targetPort: 80
      protocol: TCP

---
# NetworkPolicy
apiVersion: networking.k8s.io/v1
kind: NetworkPolicy
metadata:
  name: web-policy
  namespace: production
spec:
  podSelector:
    matchLabels:
      app: web
  policyTypes:
    - Ingress
    - Egress
  ingress:
    - from:
        - namespaceSelector:
            matchLabels:
              name: production
        - podSelector:
            matchLabels:
              app: frontend
      ports:
        - protocol: TCP
          port: 80
  egress:
    - to:
        - podSelector:
            matchLabels:
              app: database
      ports:
        - protocol: TCP
          port: 5432
    - to:    # Allow DNS
        - namespaceSelector: {}
      ports:
        - protocol: UDP
          port: 53

---
# ResourceQuota
apiVersion: v1
kind: ResourceQuota
metadata:
  name: production-quota
  namespace: production
spec:
  hard:
    requests.cpu: "100"
    requests.memory: "200Gi"
    limits.cpu: "200"
    limits.memory: "400Gi"
    pods: "500"
    persistentvolumeclaims: "100"
    services.loadbalancers: "5"

---
# LimitRange
apiVersion: v1
kind: LimitRange
metadata:
  name: default-limits
  namespace: production
spec:
  limits:
    - type: Container
      default:
        cpu: "500m"
        memory: "256Mi"
      defaultRequest:
        cpu: "100m"
        memory: "128Mi"
      max:
        cpu: "8"
        memory: "16Gi"
      min:
        cpu: "50m"
        memory: "64Mi"
    - type: PersistentVolumeClaim
      max:
        storage: "500Gi"
      min:
        storage: "1Gi"
```

### Database Selection

| Data Type | Storage | Justification |
|-----------|---------|---------------|
| Pod/Service/PVC specs | etcd (via API server) | Kubernetes native; watched by controllers |
| Container images | containerd content store (local per node) | OCI layers with deduplication |
| Container filesystem | overlayfs (local per node) | Copy-on-write for efficiency |
| Persistent data | PV backends (EBS, Ceph, NFS, local) | CSI-standardized interface |
| Container logs | Node local + shipped to centralized store | Rotated by kubelet; indexed in Elasticsearch/Loki |
| Metrics | Prometheus (time-series) | Native k8s integration; PromQL |

### Indexing Strategy

| Index | Purpose |
|-------|---------|
| Label selectors on pods | Service endpoint selection, NetworkPolicy matching |
| `spec.nodeName` field selector | Kubelet watches only its own pods |
| `status.phase` field selector | List pods in specific phase (Pending, Running, etc.) |
| EndpointSlice by service name | Fast endpoint lookup for kube-proxy |
| PV by StorageClass | Volume provisioner selects available PVs |

---

## 5. API Design

### REST/gRPC/kubectl Endpoints

**Pod Lifecycle:**

```bash
# Create pod
kubectl apply -f pod.yaml
# Equivalent: POST /api/v1/namespaces/production/pods

# Get pod with resource usage
kubectl top pod web-server -n production
# Requires metrics-server

# Exec into container
kubectl exec -it web-server -n production -c web -- /bin/sh
# WebSocket upgrade → kubelet proxy → containerd exec

# Stream logs
kubectl logs web-server -n production -c web --follow --since=1h
# HTTP/2 stream → kubelet → containerd log

# Port forward
kubectl port-forward pod/web-server 8080:80 -n production
# Local SOCKS → API server → kubelet proxy → pod

# Copy files
kubectl cp production/web-server:/data/dump.sql ./dump.sql

# Ephemeral debug container (k8s 1.25+)
kubectl debug -it web-server --image=busybox --target=web
# Adds ephemeral container to running pod — shares pid namespace
```

**Storage Operations:**

```bash
# Create storage class
kubectl apply -f storageclass.yaml

# Create PVC
kubectl apply -f pvc.yaml

# Check PV/PVC binding
kubectl get pv,pvc -n production

# Expand PVC (if allowVolumeExpansion: true)
kubectl patch pvc web-data-pvc -n production -p '{"spec":{"resources":{"requests":{"storage":"100Gi"}}}}'

# Check CSI driver status
kubectl get csidrivers
kubectl get csinodes
```

**Network Operations:**

```bash
# Check service endpoints
kubectl get endpoints web-service -n production
kubectl get endpointslices -l kubernetes.io/service-name=web-service -n production

# Check network policies
kubectl get networkpolicies -n production

# DNS debugging
kubectl run dnstest --image=busybox --rm -it -- nslookup web-service.production.svc.cluster.local

# Check kube-proxy mode
kubectl get configmap kube-proxy -n kube-system -o yaml | grep mode
```

### CRI (Container Runtime Interface)

```protobuf
// Key CRI RPCs (gRPC between kubelet and containerd)
service RuntimeService {
  rpc RunPodSandbox(RunPodSandboxRequest)       returns (RunPodSandboxResponse);
  rpc StopPodSandbox(StopPodSandboxRequest)     returns (StopPodSandboxResponse);
  rpc RemovePodSandbox(RemovePodSandboxRequest)  returns (RemovePodSandboxResponse);
  rpc CreateContainer(CreateContainerRequest)     returns (CreateContainerResponse);
  rpc StartContainer(StartContainerRequest)       returns (StartContainerResponse);
  rpc StopContainer(StopContainerRequest)         returns (StopContainerResponse);
  rpc RemoveContainer(RemoveContainerRequest)     returns (RemoveContainerResponse);
  rpc ListContainers(ListContainersRequest)       returns (ListContainersResponse);
  rpc ContainerStatus(ContainerStatusRequest)     returns (ContainerStatusResponse);
  rpc ExecSync(ExecSyncRequest)                   returns (ExecSyncResponse);
}

service ImageService {
  rpc PullImage(PullImageRequest)     returns (PullImageResponse);
  rpc RemoveImage(RemoveImageRequest) returns (RemoveImageResponse);
  rpc ListImages(ListImagesRequest)   returns (ListImagesResponse);
  rpc ImageStatus(ImageStatusRequest) returns (ImageStatusResponse);
}
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Container Resource Management (cgroups v2)

**Why it's hard:**
Kubernetes must provide resource isolation between containers sharing the same kernel. Over-commitment (limits > node capacity) is normal and desired for efficiency, but requires sophisticated management to prevent noisy neighbors while maximizing utilization. The mapping from Kubernetes resource model (requests/limits) to Linux cgroups is nuanced and frequently misunderstood.

**Approaches Compared:**

| Approach | Isolation | Efficiency | Complexity | Overhead |
|----------|----------|-----------|-----------|---------|
| cgroups v2 (unified hierarchy) | Good | High | Medium | < 1% CPU |
| cgroups v1 (legacy) | Good (but inconsistent across controllers) | Medium | High (split hierarchy) | < 1% CPU |
| VMs (kata containers/gVisor) | Strong (kernel-level) | Lower (duplicate kernel) | High | 5-15% CPU, ~100 MB RAM |
| No isolation (trusted workloads) | None | Highest | None | Zero |

**Selected: cgroups v2 with Kubernetes QoS classes**

**Implementation Detail:**

```
Kubernetes QoS Classes (determined by requests/limits configuration):

1. Guaranteed (highest OOM score adjustment = -997)
   - Every container has CPU and memory requests == limits
   - Gets dedicated CPU bandwidth (CFS quota)
   - OOM killed only if entire node is out of memory
   
   cgroup settings:
     cpu.max: "500000 100000"    # 500ms of CPU per 100ms period = 5 cores
     memory.max: 536870912       # 512 Mi hard limit
     memory.min: 536870912       # 512 Mi guaranteed (not reclaimable)

2. Burstable (OOM score adjustment = calculated from requests)
   - At least one container has requests != limits
   - Can burst above requests up to limits
   - OOM killed before Guaranteed but after BestEffort
   
   cgroup settings:
     cpu.max: "500000 100000"    # limits
     cpu.weight: 25              # Proportional to requests (250m)
     memory.max: 536870912       # limits
     memory.min: 0               # Not guaranteed (can be reclaimed)
     memory.low: 268435456       # Soft guarantee (requests)

3. BestEffort (OOM score adjustment = 1000, first to be killed)
   - No requests or limits set
   - Uses whatever resources are available
   - First to be evicted under memory pressure
   
   cgroup settings:
     cpu.max: "max"              # Unlimited
     cpu.weight: 1               # Lowest priority
     memory.max: max             # Unlimited (but OOM killed first)

CPU Management:
  - requests → cpu.weight (proportional sharing when CPU is contended)
  - limits → cpu.max (CFS bandwidth control: throttled beyond limit)
  - No limits → cpu.max = "max" (can use all available CPU)
  - Static CPU manager policy: pins Guaranteed QoS pods to specific CPU cores
    (for latency-sensitive workloads — avoids context switching)

Memory Management:
  - requests → memory.low (soft guarantee; kernel tries to protect this)
  - limits → memory.max (hard limit; OOM kill on exceed)
  - No limits → memory.max = node allocatable (risk of OOM)
  
  OOM Kill Priority:
  1. BestEffort pods (OOM score 1000)
  2. Burstable pods exceeding requests (OOM score proportional to overage)
  3. Guaranteed pods (OOM score -997, almost never killed)
  
  Eviction (kubelet-level, before OOM):
  - memory.available < 100Mi → evict BestEffort first
  - memory.available < 50Mi  → evict Burstable exceeding requests
  - Configurable via --eviction-hard, --eviction-soft
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| CPU throttling (CFS) | Latency spikes in latency-sensitive pods | Use Guaranteed QoS; set limits = requests; or remove CPU limits |
| Memory OOM kill | Container killed, pod may restart (RestartPolicy) | Right-size limits using VPA recommendations; monitor container_memory_working_set_bytes |
| Noisy neighbor (BestEffort pods) | Guaranteed/Burstable pods experience degradation | Use node affinity to isolate critical workloads; enforce LimitRange |
| Eviction storm | Too many pods evicted simultaneously | Set PodDisruptionBudgets; tune eviction thresholds |
| PID exhaustion | New processes cannot be created | Set pids-limit in containerd config (default 4096/container) |
| Disk pressure | New pods cannot be scheduled to the node | Image garbage collection (--image-gc-high-threshold=85%); log rotation |

**Interviewer Q&As:**

**Q1: Should you set CPU limits on your containers?**
A: This is one of the most debated topics in Kubernetes. The argument against limits: CPU is compressible — when not contended, pods can burst above requests without harming other pods. CPU limits force CFS bandwidth throttling even when the CPU is idle, increasing latency unnecessarily. The argument for limits: without limits, a single runaway container can monopolize CPU, degrading all co-located pods. My recommendation: set CPU requests (for scheduling) but omit CPU limits for latency-sensitive services. For batch workloads, set limits to prevent resource hogging. Always set memory limits (memory is not compressible — OOM is catastrophic).

**Q2: Explain the difference between memory requests and limits in terms of kernel behavior.**
A: Requests map to `memory.low` in cgroups v2 — this is a soft guarantee. The kernel will try to keep this much memory available for the cgroup but can reclaim it under extreme pressure. Limits map to `memory.max` — this is a hard limit. Any allocation beyond this triggers an OOM kill within the cgroup. The gap between requests and limits is the "burstable" region — the container can use it when available but has no guarantee. Setting requests == limits (Guaranteed QoS) sets `memory.min` instead of `memory.low`, providing a hard guarantee that the kernel will never reclaim.

**Q3: What is the static CPU manager and when would you use it?**
A: The static CPU manager (kubelet `--cpu-manager-policy=static`) pins Guaranteed QoS pods' containers to specific CPU cores. This eliminates context-switch overhead and improves cache locality, reducing tail latency by 10-50% for latency-sensitive workloads. Use it for: real-time systems, high-frequency trading, database engines, networking appliances. Do not use it for: general-purpose web services (wastes CPU when idle), batch jobs (reduces scheduling flexibility).

**Q4: How does kubelet decide which pods to evict under memory pressure?**
A: Eviction order: (1) BestEffort pods (no requests/limits). (2) Burstable pods using more memory than their requests. (3) Burstable pods using less than requests (unlikely to be evicted). (4) Guaranteed pods (only if the node is completely out of memory). Within each QoS class, pods are sorted by memory usage relative to their requests — the most "over-quota" pod is evicted first. Kubelet uses two thresholds: soft eviction (grace period before action) and hard eviction (immediate action).

**Q5: How do you handle a container that has a memory leak?**
A: (1) Set memory limits to cap the damage — the container will be OOM-killed when it hits the limit. (2) Configure RestartPolicy: Always (for Deployments) or OnFailure (for Jobs). (3) Monitor `container_memory_working_set_bytes` over time — a steady increase indicates a leak. (4) Use profiling tools (pprof for Go, jmap for Java) via kubectl exec or ephemeral debug containers. (5) Implement readiness probe that checks memory usage — remove the pod from service before it becomes unresponsive. (6) Use VPA to dynamically adjust limits based on observed usage patterns.

**Q6: How does Kubernetes handle disk resource management?**
A: (1) **Ephemeral storage**: container writable layer + logs. Tracked per pod, configurable via `resources.requests.ephemeral-storage` and `resources.limits.ephemeral-storage`. Kubelet evicts pods exceeding their limit. (2) **Image storage**: kubelet garbage collects unused images when disk usage exceeds `--image-gc-high-threshold` (85% default). (3) **Volume storage**: managed by CSI drivers; PVC enforces capacity. (4) **emptyDir with sizeLimit**: kubelet enforces the limit. (5) Node-level: kubelet reports `allocatable.ephemeral-storage` and scheduler considers it during placement.

---

### Deep Dive 2: Container Networking (CNI)

**Why it's hard:**
Kubernetes requires a flat network model where every pod gets a unique, routable IP address — no NAT between pods. Implementing this across thousands of nodes requires efficient IP address management, routing (overlay or BGP), network policy enforcement, and service load balancing, all with minimal latency overhead.

**Approaches Compared:**

| CNI Plugin | Datapath | Network Policy | Performance | Complexity | Scalability |
|-----------|---------|---------------|-------------|-----------|------------|
| Calico (iptables) | BGP + iptables | Yes (k8s native) | Good | Medium | Good (< 5k nodes) |
| Calico (eBPF) | eBPF | Yes (k8s native) | Excellent | Medium | Excellent |
| Cilium | eBPF | Yes (extended, L7) | Excellent | Medium-High | Excellent |
| Flannel | VXLAN overlay | No | Good | Low | Good |
| AWS VPC CNI | Native VPC | Yes (via NP) | Excellent (no overlay) | Low | Cloud-only |
| Weave Net | VXLAN + encryption | Yes | Medium | Low | Medium |

**Selected: Cilium (eBPF-based) for production, Calico as alternative**

**Justification:** Cilium uses eBPF to implement networking entirely in the kernel, bypassing iptables. This provides: (1) O(1) service load balancing (vs. O(n) iptables chains), (2) native network policy enforcement at L3/L4/L7, (3) transparent encryption (WireGuard), and (4) observability (Hubble). For bare-metal environments without cloud VPC integration, Cilium with BGP peering is the optimal choice.

**Implementation Detail:**

```
Pod Network Setup (CNI flow):

1. kubelet calls CNI plugin (Cilium) via exec:
   CNI_COMMAND=ADD /opt/cni/bin/cilium-cni < config.json

2. Cilium agent:
   a. Allocates pod IP from CIDR pool (e.g., 10.244.3.0/24 for this node)
   b. Creates veth pair: eth0 (in pod) ↔ lxc-xxx (on host)
   c. Configures eBPF programs on the veth:
      - tc ingress/egress for network policy
      - Socket-level redirection for service load balancing
   d. Installs routes for pod IP on the host
   e. For cross-node traffic: direct routing (BGP) or VXLAN/Geneve tunnel

Cross-Node Routing:

  Option A: BGP (bare-metal, no overlay)
    - Cilium BGP speaker advertises node's pod CIDR to ToR switches
    - No encapsulation overhead
    - Requires BGP-capable network infrastructure
    
  Option B: VXLAN/Geneve overlay
    - Pods' packets encapsulated in UDP (VXLAN port 8472)
    - Works on any L3 network (no special switch requirements)
    - ~5-10% throughput overhead, ~0.1ms latency overhead
    
  Option C: Native routing (cloud VPC)
    - AWS VPC CNI assigns pod IPs from VPC subnet
    - No overlay; native VPC routing
    - Pod IP is directly reachable from outside the cluster

Service Load Balancing (Cilium eBPF vs. kube-proxy iptables):

  kube-proxy iptables:
    - Installs DNAT rules for each Service → Endpoint mapping
    - Chain length proportional to endpoint count: O(n)
    - 10,000 services x 50 endpoints = 500,000 rules
    - Rule update time: ~5s for full iptables restore
    - Latency: ~0.1ms per connection setup

  Cilium eBPF (replacing kube-proxy):
    - Service → Endpoint mapping in eBPF maps (hash table)
    - O(1) lookup regardless of service count
    - Update time: microseconds (single map entry)
    - Latency: ~0.05ms per connection setup
    - Supports DSR (Direct Server Return) for LoadBalancer services
    - Supports Maglev hashing for consistent load balancing

Network Policy Enforcement:

  L3/L4 (standard Kubernetes NetworkPolicy):
    - Ingress: allow from specific pods/namespaces on specific ports
    - Egress: allow to specific pods/namespaces/CIDRs on specific ports
    - Default: if no policy exists, all traffic is allowed
    - Once any policy selects a pod, only explicitly allowed traffic passes

  L7 (Cilium-specific CiliumNetworkPolicy):
    - HTTP: allow GET /api/v1/users but deny DELETE
    - gRPC: allow specific methods
    - Kafka: allow produce to specific topics
    - DNS: allow resolution of specific domains
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| CNI plugin crash | New pods cannot get networking; existing pods unaffected | DaemonSet auto-restart; health monitoring |
| IP address exhaustion | Pods stuck in ContainerCreating | Larger pod CIDR; IPAM monitoring; IP reclamation |
| BGP peer down | Cross-node pod traffic blackholed | Multiple BGP peers; BFD for fast failure detection |
| VXLAN tunnel failure | Cross-node communication broken | Tunnel health checks; fallback to direct routing |
| Network policy misconfiguration | Legitimate traffic blocked or sensitive traffic exposed | Default-deny policies; CI testing of policies; Hubble visibility |
| iptables rule explosion (kube-proxy) | High CPU, slow rule updates, packet loss during updates | Migrate to Cilium eBPF or IPVS mode |

**Interviewer Q&As:**

**Q1: Why does Kubernetes require a flat network model (no NAT between pods)?**
A: NAT creates two problems: (1) Applications need to know their real IP for health checking, logging, and service discovery. Behind NAT, a pod sees its private IP but others see the NAT'd IP — causing confusion. (2) Performance — NAT adds connection tracking overhead and complicates protocols that embed IP addresses (SIP, FTP). The flat model means every pod has a cluster-wide unique IP, and any pod can reach any other pod at its IP without translation. This simplifies application design and debugging.

**Q2: How does service discovery work end-to-end in Kubernetes?**
A: (1) Deployment creates pods with labels `app: web`. (2) Service `web-svc` has selector `app: web`. (3) Endpoint controller watches pods matching the selector and creates EndpointSlice objects with pod IPs. (4) CoreDNS watches Services and creates DNS records: `web-svc.default.svc.cluster.local → ClusterIP 10.96.0.42`. (5) kube-proxy (or Cilium) watches EndpointSlices and installs forwarding rules: `10.96.0.42:80 → {10.244.1.5:8080, 10.244.2.7:8080, ...}`. (6) Client pod resolves DNS → gets ClusterIP → kernel DNAT → packet reaches backend pod.

**Q3: How do you debug network connectivity issues between pods?**
A: Systematic approach: (1) Verify pods are Running and have IPs: `kubectl get pods -o wide`. (2) Test connectivity from within a pod: `kubectl exec -it client -- curl http://<pod-ip>:8080`. (3) If same-node works but cross-node fails: check CNI plugin status, node routes, tunnel interfaces. (4) If DNS fails: `kubectl exec -it client -- nslookup web-svc` — check CoreDNS logs. (5) If NetworkPolicy might be blocking: temporarily delete all NetworkPolicies to test, or use Cilium Hubble to see dropped flows. (6) Check kube-proxy logs for endpoint sync issues. (7) On the node: `conntrack -L`, `iptables -L -n -t nat`, `ip route`.

**Q4: What is the difference between IPVS and iptables mode for kube-proxy?**
A: iptables mode uses DNAT rules in the nat table. For each Service, it creates a chain with one rule per endpoint using probability-based load balancing (statistic mode). With 10,000 services x 50 endpoints, this creates ~500,000 rules. Rule insertion is O(n) — a full sync can take seconds. IPVS mode uses the kernel's IP Virtual Server, which uses hash tables for service→endpoint mapping. Lookup is O(1), updates are faster, and it supports more load balancing algorithms (round-robin, least-connections, source-hash). IPVS mode is recommended for > 1,000 services.

**Q5: How does Kubernetes handle multi-homed pods (pods with multiple network interfaces)?**
A: The standard CNI spec creates one interface (eth0) per pod. For multi-homing: use **Multus CNI** — a meta-CNI that delegates to multiple CNI plugins. Example: eth0 via Cilium (primary, for k8s services), net1 via SR-IOV (for high-performance data plane). Use cases: (1) NFV (network function virtualization), (2) separating control plane and data plane traffic, (3) high-performance storage networks. Configured via `k8s.v1.cni.cncf.io/networks` annotation on the pod.

**Q6: How does DNS scaling work in large clusters?**
A: CoreDNS runs as a Deployment with HPA (scales based on pod count or QPS). For very large clusters: (1) **NodeLocal DNSCache** — runs a DNS cache on every node (DaemonSet). Pods resolve against the local cache, which forwards cache misses to CoreDNS. Reduces CoreDNS load by ~80%. (2) **Autoscaler addon** — automatically scales CoreDNS replicas based on node count and pod count. (3) Configuration: reduce CoreDNS cache TTL for services (default 30s) or increase for external domains. (4) Monitor: `coredns_dns_request_count_total`, `coredns_dns_request_duration_seconds`.

---

### Deep Dive 3: Persistent Storage (CSI)

**Why it's hard:**
Stateful workloads need durable storage that survives pod restarts and node failures. Storage must be provisioned, attached to the correct node, formatted, mounted, and eventually reclaimed — all orchestrated across distributed systems with different backends (cloud block, network filesystems, local disks) and different access patterns (ReadWriteOnce, ReadWriteMany, ReadOnlyMany).

**Approaches Compared:**

| Storage Backend | Access Mode | Performance | Durability | Portability | Use Case |
|----------------|------------|-------------|-----------|-------------|----------|
| Cloud block (EBS, GCE PD) | RWO | High (SSD/NVMe) | Very high (replicated) | Cloud-only | Databases, stateful services |
| Ceph RBD | RWO | High | High (triple-replicated) | Any | Bare-metal databases |
| CephFS / NFS | RWX | Medium | High | Any | Shared data, CMS |
| Local volumes | RWO | Highest (NVMe direct) | Low (no replication) | Node-local | High-perf databases (Cassandra, Elasticsearch) |
| MinIO / S3 (via CSI FUSE) | RWX | Medium | High | Any | Object storage workloads |

**Selected: CSI architecture with multiple backends**

**Implementation Detail:**

```
CSI Architecture:
                                          
  ┌───────────┐    ┌──────────────────┐    ┌─────────────────┐
  │  kubelet   │    │  CSI Controller  │    │  Storage Backend│
  │  (per node)│    │  (Deployment)    │    │  (EBS/Ceph/NFS) │
  └─────┬─────┘    └────────┬─────────┘    └────────┬────────┘
        │                   │                        │
        │ CSI Node RPCs     │ CSI Controller RPCs    │ Cloud API
        │                   │                        │
  ┌─────▼─────┐    ┌───────▼──────────┐            │
  │ CSI Node  │    │ CSI Controller   │            │
  │ Plugin    │    │ Plugin           ├────────────┘
  │ (DaemonSet)│   │ (Deployment)     │
  └───────────┘    └──────────────────┘

Volume Lifecycle:

1. Dynamic Provisioning:
   User creates PVC → PV controller sees unbound PVC with StorageClass
   → External provisioner calls CSI CreateVolume
   → Storage backend creates volume (EBS: aws ec2 create-volume)
   → PV created and bound to PVC

2. Attach (ControllerPublishVolume):
   Pod scheduled to node → AD controller (attach-detach)
   → External attacher calls CSI ControllerPublishVolume
   → Storage backend attaches volume to node (EBS: aws ec2 attach-volume)
   → VolumeAttachment object created

3. Stage (NodeStageVolume):
   kubelet detects volume attached to this node
   → Calls CSI NodeStageVolume
   → Formats filesystem if needed (mkfs.ext4)
   → Mounts to staging directory (/var/lib/kubelet/plugins/csi/...)

4. Publish (NodePublishVolume):
   → Calls CSI NodePublishVolume
   → Bind mounts from staging to pod volume path
   → Container sees mounted filesystem

5. Teardown (reverse order):
   NodeUnpublishVolume → NodeUnstageVolume → ControllerUnpublishVolume
   → If reclaimPolicy=Delete: DeleteVolume
   → If reclaimPolicy=Retain: PV preserved for manual reclaim

Volume Expansion:
   User patches PVC with larger size → CSI ControllerExpandVolume
   → Backend grows volume (EBS: aws ec2 modify-volume)
   → CSI NodeExpandVolume → resize filesystem (resize2fs)
   → Online expansion — no pod restart needed
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Volume attach timeout | Pod stuck in ContainerCreating | Attach timeout (default 2 min); retry; check cloud API quotas |
| Volume detach stuck (force delete node) | Volume stuck in Attached state; new pod cannot mount | `--force-detach` in CSI; VolumeAttachment cleanup controller |
| Storage backend failure (EBS outage) | Reads/writes fail; pods may crash | Multi-AZ volumes (EBS io2, GP3 multi-attach); application-level retries |
| Filesystem corruption | Data loss or read errors | fsck on mount; snapshots for backup; checksumming at application layer |
| PV full | Application crashes (ENOSPC) | Volume expansion; monitor `kubelet_volume_stats_used_bytes`; PVC auto-expansion (alpha) |
| CSI driver crash | No new volume operations; existing mounts continue working | DaemonSet auto-restart; CSI liveness probe |

**Interviewer Q&As:**

**Q1: What is the difference between volumeBindingMode WaitForFirstConsumer vs Immediate?**
A: **Immediate** (default): PV is provisioned and bound to PVC as soon as the PVC is created, before any pod uses it. Problem: the PV might be in the wrong AZ — if the pod gets scheduled to a different AZ, it cannot mount the volume. **WaitForFirstConsumer**: PV provisioning is delayed until a pod using the PVC is scheduled. The scheduler considers volume topology (AZ, region) when placing the pod, ensuring the PV is created in the same zone. This is the recommended setting for all cloud storage classes.

**Q2: How do you handle storage for StatefulSets?**
A: StatefulSets use `volumeClaimTemplates` — each replica gets its own PVC with a predictable name (`data-mysql-0`, `data-mysql-1`). On pod deletion and recreation, the same PVC is reattached to the new pod (matched by ordinal index). This provides: (1) Stable storage identity across restarts. (2) Data persists even if the node fails (PV is re-attached to the new node). (3) Scale-down preserves PVCs (not deleted) so scale-up reattaches them. Use `persistentVolumeClaimRetentionPolicy` (beta) to control PVC cleanup on StatefulSet delete/scale-down.

**Q3: How do you implement storage on bare-metal without cloud block storage?**
A: Options: (1) **Ceph RBD** — distributed block storage; replicated across nodes; excellent for databases. Requires Ceph cluster (3+ OSDs). (2) **Rook** — Kubernetes operator for Ceph; automates deployment and management. (3) **OpenEBS** — container-attached storage; uses node local storage with replication. (4) **Local PV** — uses node's local NVMe/SSD; highest performance but no replication (application must handle replication, e.g., Cassandra). (5) **NFS** — simple shared storage; good for read-heavy workloads. (6) **Longhorn** — distributed block storage designed for k8s; simpler than Ceph.

**Q4: How does Kubernetes handle volume snapshots?**
A: CSI snapshot support (GA in v1.20): (1) Create VolumeSnapshotClass (like StorageClass for snapshots). (2) Create VolumeSnapshot pointing to a PVC. (3) CSI driver creates a snapshot in the storage backend (e.g., EBS snapshot). (4) Restore: create a new PVC with `dataSource: VolumeSnapshot`. (5) Use cases: backup before upgrades, creating dev environments from production data, disaster recovery. (6) Snapshot scheduling: use external tools (Velero, custom CronJob) to create periodic snapshots.

**Q5: What is the maximum number of volumes that can be attached to a single node?**
A: Cloud provider limits: AWS EBS = 28 volumes/instance (varies by type), GCE PD = 128/instance, Azure = 64/VM. Local volumes: limited by disk count. Kubernetes tracks this via the `CSINodeInfo` object and the `AttachVolumeLimit` scheduler plugin. The scheduler will not place a pod on a node if it would exceed the volume attachment limit. For high-density storage workloads, use fewer larger volumes or local volumes (no attach limit).

**Q6: How do you handle data migration when moving a stateful pod to a different node?**
A: (1) For cloud block storage (EBS): the volume is detached from the old node and attached to the new node — data moves with the volume. Kubernetes handles this automatically during rescheduling. (2) For local volumes: data does not migrate — the pod must be scheduled to the same node. Use node affinity or StatefulSet with local PV provisioner. (3) For true data portability: use Ceph RBD or similar network storage that can be mounted from any node. (4) For disaster recovery: use volume snapshots + restore to move data across AZs.

---

## 7. Scheduling & Resource Management

### Resource Management at the Node Level

**Kubelet Resource Accounting:**

```
Node Capacity (total hardware resources)
  - System Reserved (OS processes, kernel: --system-reserved=cpu=500m,memory=1Gi)
  - Kube Reserved (kubelet, containerd: --kube-reserved=cpu=500m,memory=1Gi)
  - Eviction Threshold (--eviction-hard=memory.available<100Mi)
  = Allocatable (what's available for pods)

Example for a 16 CPU, 64 GB node:
  Capacity:    16 CPU, 64 Gi
  - System:     0.5 CPU, 1 Gi
  - Kube:       0.5 CPU, 1 Gi
  - Eviction:   0 CPU, 0.1 Gi
  = Allocatable: 15 CPU, 61.9 Gi
```

**Over-Commitment:**

```
Pod requests are guaranteed; limits allow over-commitment.

Example:
  Node Allocatable: 15 CPU, 61.9 Gi
  
  Pod A: requests=2 CPU, 4 Gi  |  limits=4 CPU, 8 Gi
  Pod B: requests=2 CPU, 4 Gi  |  limits=4 CPU, 8 Gi
  Pod C: requests=2 CPU, 4 Gi  |  limits=4 CPU, 8 Gi
  
  Total Requests: 6 CPU, 12 Gi    ← What scheduler considers (fits)
  Total Limits:   12 CPU, 24 Gi   ← What could be consumed if all burst
  
  This is safe because:
  - CPU: bursting beyond limits = throttled (CFS), not killed
  - Memory: bursting beyond limits = OOM killed, so set limits carefully
  - Not all pods burst simultaneously in practice
```

**Topology Manager:**

```
For NUMA-aware workloads (databases, packet processing):

kubelet --topology-manager-policy=single-numa-node
  → Ensures all resources (CPU, memory, GPU, NIC) for a container
    are allocated from the same NUMA node
  → Prevents cross-NUMA memory access (2-3x latency penalty)
  → Works with static CPU manager and device plugins (GPU, SRIOV)
```

---

## 8. Scaling Strategy

### Container Orchestration Scaling

| Dimension | Strategy | Limit |
|-----------|---------|-------|
| Pods per node | Kubelet `--max-pods` | Default 110; tunable to 250 with CIDR adjustment |
| Nodes per cluster | k8s tested limit | 5,000 nodes |
| Services per cluster | kube-proxy / Cilium | 10,000 (kube-proxy iptables); 100,000+ (Cilium eBPF) |
| PVs per cluster | etcd size limit | 50,000+ (with EndpointSlice-like sharding) |
| Image pull throughput | Registry bandwidth | Mirror registry per-AZ; pre-pull DaemonSets |

### Interviewer Q&As

**Q1: How do you handle image pull latency for large images (> 1 GB)?**
A: (1) **Registry mirror**: run a pull-through cache in each AZ (Harbor, zot). (2) **Image pre-pulling**: DaemonSet that pulls commonly used images to all nodes. (3) **Layer sharing**: use common base images; layers are cached and shared across images. (4) **Lazy pulling (Stargz)**: download only the layers/files needed at startup; rest downloaded in background. (5) **Image streaming (EROFS)**: Google's approach — mount image directly from registry without full download. (6) **Smaller images**: use distroless/Alpine base images; multi-stage builds.

**Q2: How does Kubernetes handle 10,000 services with kube-proxy?**
A: With iptables mode, 10,000 services create ~500,000+ rules. Full iptables sync takes 5-10 seconds, during which connection setup is delayed. Solutions: (1) Switch to IPVS mode (O(1) lookup). (2) Switch to Cilium eBPF (replaces kube-proxy entirely). (3) Use EndpointSlices to limit the update scope (only affected slice is synced). (4) In practice, most clusters don't need 10,000 services — use headless services where possible.

**Q3: How do you scale CoreDNS for a 5,000-node cluster?**
A: (1) Enable NodeLocal DNSCache (DaemonSet on every node, caches DNS locally). This reduces CoreDNS load by 80-90%. (2) Scale CoreDNS replicas proportionally (dns-autoscaler addon: 1 replica per 256 pods). (3) Use autopath plugin to reduce FQDN query chains. (4) Monitor `coredns_dns_request_count_total` and `coredns_cache_hits_total`. (5) Separate external DNS resolution from internal (split-horizon DNS).

**Q4: What happens when a node runs out of ephemeral storage?**
A: Kubelet monitors disk usage and triggers eviction: (1) `nodefs.available < 10%` → evict pods by ephemeral storage usage (BestEffort first, then highest usage). (2) `imagefs.available < 15%` → garbage collect unused container images. (3) If still low, kubelet marks node as DiskPressure → no new pods scheduled. (4) Prevention: set `resources.limits.ephemeral-storage` on containers; configure log rotation; use separate disks for images and pod data.

**Q5: How do you handle Windows containers in a mixed Linux/Windows cluster?**
A: (1) Windows nodes run alongside Linux nodes in the same cluster. (2) Use node selectors or node affinity to schedule Windows pods on Windows nodes (`kubernetes.io/os: windows`). (3) Windows uses HCS (Host Compute Service) instead of Linux namespaces/cgroups. (4) Limited CNI support (Flannel overlay, Azure CNI). (5) No privileged containers, no DaemonSets natively on Windows. (6) This is relevant for legacy .NET applications in enterprise environments.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure Scenario | Impact | Detection | Recovery | RTO |
|---|-----------------|--------|-----------|----------|-----|
| 1 | Container crash | Single container restarts with backoff | Pod status CrashLoopBackOff | kubelet restarts (backoff: 10s, 20s, ... 300s) | 10s - 5 min |
| 2 | Node failure (hardware) | All pods on node lost | Node controller detects missed heartbeats (40s) | Pods rescheduled after eviction (5 min) | 5-10 min |
| 3 | Network partition (node ↔ CP) | Node marked NotReady | Kubelet heartbeat missed | Pods evicted after timeout; rescheduled | 5-10 min |
| 4 | CNI plugin failure | New pods cannot get networking | Pod stuck in ContainerCreating | DaemonSet restarts CNI; existing pods unaffected | < 1 min |
| 5 | CSI driver failure | Volume operations fail | Volume attach/mount timeout | CSI DaemonSet restart; retry attach | < 5 min |
| 6 | Storage backend outage | Reads/writes fail for PV-backed pods | I/O errors in pod logs | Application retry; storage backend recovery | Variable |
| 7 | Image registry down | New pods cannot pull images | ImagePullBackOff | Fallback to cached images; mirror registry | < 1 min (cached), variable (new) |
| 8 | DNS failure (CoreDNS down) | Service discovery broken | DNS resolution timeouts | HPA/replica restart; NodeLocal DNSCache continues serving cached entries | < 1 min |
| 9 | Kubelet crash | Pods continue running but no health checks, no new pods | Node status Unknown | systemd restarts kubelet; pods continue running | < 30s |
| 10 | IP address exhaustion | New pods cannot be created | CNI returns error | Expand pod CIDR; reclaim leaked IPs | 5-30 min |

### Automated Recovery

| Mechanism | Implementation | Protects Against |
|-----------|---------------|-----------------|
| RestartPolicy: Always | kubelet restarts crashed containers with exponential backoff | Container crashes |
| Liveness probe | kubelet kills and restarts unresponsive containers | Application hangs, deadlocks |
| Readiness probe | kubelet removes pod from service endpoints | Slow starts, temporary unavailability |
| PodDisruptionBudget | Limits voluntary evictions | Reckless drains, upgrades, spot eviction |
| ReplicaSet | Maintains desired pod count | Pod failures, node failures |
| Pod anti-affinity | Spreads replicas across nodes/zones | Single node/zone failure |
| PV reclaim | Retains or deletes volumes on PVC deletion | Accidental data deletion |

---

## 10. Observability

### Key Metrics

| Metric | Source | Alert Threshold | Meaning |
|--------|--------|----------------|---------|
| `container_cpu_usage_seconds_total` | cAdvisor | > 90% of limit sustained | Container approaching CPU throttle |
| `container_memory_working_set_bytes` | cAdvisor | > 80% of limit | Container approaching OOM |
| `container_cpu_cfs_throttled_seconds_total` | cAdvisor | Rate > 0 sustained | CPU throttling occurring |
| `kubelet_running_pods` | kubelet | > max-pods * 90% | Node approaching pod limit |
| `kubelet_volume_stats_used_bytes` | kubelet | > 80% of capacity | Volume filling up |
| `container_network_receive_bytes_total` | cAdvisor | > NIC bandwidth * 80% | Network saturation |
| `kube_pod_status_phase{phase="Pending"}` | kube-state-metrics | Count > 0 for > 5 min | Pods cannot be scheduled |
| `kube_pod_container_status_restarts_total` | kube-state-metrics | Rate > 5/hour per pod | Crash loop |
| `coredns_dns_request_count_total` | CoreDNS | Rate > 50K QPS | DNS overload |
| `node_disk_io_time_seconds_total` | node-exporter | > 80% utilization | Disk I/O bottleneck |

### Distributed Tracing

```
Container-level tracing:
1. Application injects trace headers (W3C Trace Context)
2. Service mesh sidecar (if present) creates spans for network calls
3. CNI (Cilium Hubble) provides L3/L4 flow visibility
4. CSI driver traces volume operation latency

Integration:
  Application → OpenTelemetry SDK → OTel Collector → Jaeger/Tempo
  CNI → Hubble → Hubble UI / Grafana
  kubelet → cAdvisor → Prometheus → Grafana
```

### Logging

```
Container logging architecture:

1. Container stdout/stderr → captured by containerd log driver
   → Written to /var/log/pods/<namespace>_<pod>_<uid>/<container>/0.log
   → Rotated by kubelet (--container-log-max-size=10Mi, --container-log-max-files=5)

2. Log shipping (DaemonSet):
   Fluentd/Fluent Bit/Vector reads log files → ships to:
   - Elasticsearch (full-text search, structured queries)
   - Loki (labels-based, cheaper, less flexible)
   - S3 (long-term archive)

3. Application logs vs. system logs:
   - Application: /var/log/pods/ (per-pod)
   - kubelet: journald or /var/log/kubelet.log
   - containerd: journald or /var/log/containerd.log
   - CNI: journald or /var/log/calico/ or /var/log/cilium/
```

---

## 11. Security

### Auth & AuthZ

**Pod Security Standards (PSS, replacing PSP in k8s 1.25):**

```yaml
# Namespace-level enforcement
apiVersion: v1
kind: Namespace
metadata:
  name: production
  labels:
    pod-security.kubernetes.io/enforce: restricted
    pod-security.kubernetes.io/audit: restricted
    pod-security.kubernetes.io/warn: restricted

# Restricted profile prohibits:
#  - Privileged containers
#  - Host networking/PID/IPC
#  - Writable root filesystem (must set readOnlyRootFilesystem: true)
#  - Running as root (must set runAsNonRoot: true)
#  - Privilege escalation (must set allowPrivilegeEscalation: false)
#  - All capabilities dropped (must set drop: ["ALL"])
#  - Non-default seccomp profile (must set RuntimeDefault or Localhost)
#  - hostPath volumes
```

**Container Image Security:**

```
Image Pipeline:
1. Build: Multi-stage builds with distroless base images
2. Scan: Trivy/Grype scans for CVEs at CI time
3. Sign: cosign + Sigstore for supply chain integrity
4. Admit: Admission webhook (Kyverno/OPA) verifies image signature + scan results
5. Pull: ImagePullPolicy: Always (for mutable tags); prefer immutable digests
6. Runtime: Falco monitors for unexpected process execution, file access, syscalls
```

### Multi-tenancy Isolation

| Layer | Mechanism | Isolation Level |
|-------|-----------|----------------|
| Process | Linux namespaces (pid, net, mnt, uts, ipc, user) | Container-level |
| Resource | cgroups v2 (CPU, memory, I/O) | Container-level |
| Network | NetworkPolicy (default deny per namespace) | Namespace-level |
| API | RBAC (namespace-scoped roles) | Namespace-level |
| Storage | StorageClass per tenant; PVC quotas | Namespace-level |
| Security | Pod Security Standards per namespace | Namespace-level |
| Admission | OPA/Gatekeeper constraints per namespace | Namespace-level |
| Stronger | gVisor/Kata Containers (VM-level isolation) | Pod-level |

---

## 12. Incremental Rollout Strategy

### Phase 1: Runtime Foundation (Week 1-2)
- Deploy containerd on all nodes (verify CRI compatibility).
- Configure cgroups v2, kubelet resource settings.
- Establish baseline metrics (cAdvisor, node-exporter).

### Phase 2: Networking (Week 3-4)
- Deploy Cilium CNI with default-allow policies.
- Test pod-to-pod, pod-to-service, and external connectivity.
- Implement default-deny NetworkPolicies for production namespace.
- Enable Hubble for network observability.

### Phase 3: Storage (Week 5-6)
- Deploy CSI drivers for required backends (EBS, Ceph, NFS).
- Create StorageClasses with appropriate reclaim policies.
- Test volume lifecycle: create, attach, expand, snapshot, delete.
- Implement monitoring for volume health.

### Phase 4: Security Hardening (Week 7-8)
- Enable Pod Security Standards (warn → audit → enforce).
- Deploy image scanning (Trivy Operator).
- Implement admission policies (Kyverno/OPA).
- Enable audit logging.

### Phase 5: Production Hardening (Week 9-10)
- Implement ResourceQuotas and LimitRanges.
- Deploy NodeLocal DNSCache.
- Tune kubelet parameters (max-pods, eviction thresholds).
- Establish SLOs and alerting.

### Rollout Interviewer Q&As

**Q1: How do you migrate from Docker to containerd as the container runtime?**
A: (1) Kubernetes 1.24 removed dockershim — containerd is the standard CRI runtime. (2) Docker images are OCI-compliant — they work unchanged with containerd. (3) Migration: install containerd alongside Docker, configure kubelet `--container-runtime-endpoint=unix:///run/containerd/containerd.sock`, drain the node, restart kubelet, uncordon. (4) Gotcha: Docker-only features (docker exec via Docker socket) no longer work — use `crictl` or `kubectl exec` instead.

**Q2: How do you implement a gradual migration to Cilium from Calico?**
A: (1) Both CNI plugins cannot run simultaneously on the same cluster (they fight over IP allocation). (2) Strategy: create a new node pool with Cilium, cordon old nodes, drain workloads to new nodes, decommission old nodes. (3) Or use Cilium's migration tools that can take over from Calico without pod restarts (Cilium 1.14+ supports in-place migration). (4) Test thoroughly in staging first — network policies have different semantics between Calico and Cilium.

**Q3: How do you handle the transition from iptables-based kube-proxy to Cilium eBPF?**
A: (1) Deploy Cilium with `kube-proxy-replacement: strict` (replaces kube-proxy entirely). (2) Verify all services are accessible with Cilium handling load balancing. (3) Delete kube-proxy DaemonSet. (4) Clean up iptables rules left by kube-proxy: `iptables -F -t nat`. (5) Caveat: Cilium requires kernel 4.9+ for eBPF, 5.4+ for full features.

**Q4: How do you roll out Pod Security Standards without breaking existing workloads?**
A: (1) Start with `warn` mode — users see warnings but pods are not rejected. (2) Audit existing pods against the target profile: `kubectl label ns production pod-security.kubernetes.io/audit=restricted`. (3) Fix violations (add security context, drop capabilities, etc.). (4) Escalate to `enforce` mode once all pods pass. (5) Exemptions: certain system pods (CNI, CSI) need elevated privileges — use `exemptions` in the PodSecurity admission configuration.

**Q5: How do you handle the rollout of a new CSI driver?**
A: (1) Deploy the CSI driver (controller + node DaemonSet). (2) Create a new StorageClass pointing to the new driver. (3) Test with a non-critical workload: create PVC, verify attach/mount/read/write. (4) Gradually migrate workloads: create new PVCs with the new StorageClass, copy data, switch. (5) Existing PVs with the old driver continue to work — CSI migration (built into k8s) handles the translation.

---

## 13. Trade-offs & Decision Log

| # | Decision | Alternative Considered | Trade-off | Rationale |
|---|----------|----------------------|-----------|-----------|
| 1 | containerd over Docker | Docker, CRI-O | Less tooling (no docker CLI) but lighter, CRI-native | Docker removed from k8s 1.24; containerd is the standard |
| 2 | Cilium over Calico | Calico, Flannel, AWS VPC CNI | More complex but eBPF eliminates iptables bottleneck | O(1) service routing; L7 network policies; Hubble observability |
| 3 | CSI over in-tree volumes | In-tree volume plugins | External dependency but standardized interface | In-tree plugins deprecated; CSI is the future |
| 4 | cgroups v2 over v1 | cgroups v1 | Requires newer kernel but unified hierarchy | Better memory management, PSI support, unified accounting |
| 5 | Pod Security Standards over PSP | PodSecurityPolicy | Less granular but simpler and maintained | PSP deprecated in 1.21, removed in 1.25 |
| 6 | eBPF service mesh over iptables | kube-proxy iptables, IPVS | Kernel version requirement but O(1) performance | Scales to 100K+ services; lower latency |
| 7 | NodeLocal DNSCache over CoreDNS alone | CoreDNS scaling | Additional DaemonSet but 80% load reduction | Critical for DNS reliability at scale |
| 8 | WaitForFirstConsumer volume binding | Immediate binding | Delayed PV creation but topology-aware | Prevents cross-AZ volume/pod mismatch |
| 9 | ReadWriteOnce over ReadWriteMany | RWX everywhere | Limits sharing but much higher performance | Most workloads only need single-writer; RWX adds complexity |
| 10 | Default-deny network policies | Default-allow | More initial configuration but defense-in-depth | Zero-trust model; explicitly allow required traffic |

---

## 14. Agentic AI Integration

### AI-Powered Container Operations

| Use Case | AI Agent Capability | Implementation |
|----------|-------------------|---------------|
| **Resource right-sizing** | Analyze container resource usage and recommend requests/limits | Agent queries Prometheus for `container_cpu_usage`, `container_memory_working_set_bytes`; generates VPA-like recommendations |
| **Anomaly detection** | Detect unusual container behavior (CPU spikes, memory leaks, network patterns) | Agent monitors time-series metrics; uses statistical models to flag deviations from baseline |
| **Root cause analysis** | Correlate container crashes with system events | Agent traces: CrashLoopBackOff → OOM killed → memory leak → recent deployment → specific commit |
| **Network policy generation** | Observe traffic patterns and generate NetworkPolicies | Agent monitors Cilium Hubble flows for 7 days; generates least-privilege NetworkPolicies matching observed patterns |
| **Image vulnerability prioritization** | Triage CVEs based on actual exploitability | Agent correlates Trivy scan results with: (1) Is the vulnerable package actually used? (2) Is the port exposed? (3) Is the container internet-facing? |
| **Capacity forecasting** | Predict node resource exhaustion | Agent analyzes resource usage trends; predicts when nodes will be full; recommends scaling before it happens |

### Example: AI-Driven Network Policy Generator

```python
class NetworkPolicyAgent:
    """
    Observes actual network traffic and generates NetworkPolicies.
    """
    
    def generate_policies(self, namespace: str, observation_days: int = 7) -> list:
        # Query Hubble for observed flows
        flows = self.hubble.query_flows(
            namespace=namespace,
            since=f'{observation_days}d',
            verdict='FORWARDED'
        )
        
        # Group by source pod label → destination pod label + port
        policy_rules = defaultdict(set)
        for flow in flows:
            src = self.resolve_pod_labels(flow.source)
            dst = self.resolve_pod_labels(flow.destination)
            policy_rules[(src, dst)].add(flow.destination_port)
        
        # Generate NetworkPolicy per unique source selector
        policies = []
        for (src, dst), ports in policy_rules.items():
            policy = NetworkPolicy(
                name=f'{src.app}-to-{dst.app}',
                namespace=namespace,
                pod_selector=src,
                ingress_from=dst,
                ports=ports
            )
            policies.append(policy)
        
        # Add default deny
        policies.append(NetworkPolicy(
            name='default-deny-all',
            namespace=namespace,
            pod_selector={},  # All pods
            policy_types=['Ingress', 'Egress'],
            ingress=[],  # Deny all
            egress=[
                {'to': [], 'ports': [{'port': 53, 'protocol': 'UDP'}]}  # Allow DNS
            ]
        ))
        
        return policies
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: Walk me through the complete lifecycle of a container from image pull to process execution.**
A: (1) kubelet sees unstarted container in pod spec. (2) kubelet calls containerd PullImage (CRI). (3) containerd resolves image tag to digest via registry API. (4) containerd downloads image manifest + config + layers (OCI distribution spec). (5) Layers are stored as content-addressable blobs. (6) containerd creates a snapshot (overlayfs) with layers as lower dirs and a writable upper dir. (7) kubelet calls containerd CreateContainer with OCI runtime spec (env vars, mounts, cgroups, namespaces). (8) containerd calls runc to: create Linux namespaces (pid, net, mnt, uts, ipc), configure cgroups, pivot_root to container rootfs, apply seccomp profile, drop capabilities, exec the entrypoint. (9) Container process runs as PID 1 inside the namespace.

**Q2: Explain how Kubernetes probes work and when to use each type.**
A: Three probe types: (1) **Startup probe**: runs during container startup. While it fails, liveness/readiness probes are disabled. Use for slow-starting containers (Java apps with 60s+ startup). (2) **Liveness probe**: runs continuously. Failure → kubelet kills and restarts the container. Use to detect deadlocks, unrecoverable states. Do NOT check dependencies (or you'll cascade-fail). (3) **Readiness probe**: runs continuously. Failure → pod removed from Service endpoints. Use to indicate when the pod can serve traffic (database connection established, cache warmed). All probes support HTTP GET, TCP socket, gRPC, or exec command.

**Q3: What is the difference between a DaemonSet and a Deployment with node affinity?**
A: DaemonSet ensures exactly one pod per node (or per matching node). Key differences: (1) DaemonSet is scheduled by the DaemonSet controller (bypasses default scheduler), ensuring one pod per node. (2) DaemonSet automatically adds pods when new nodes join and removes when nodes leave. (3) DaemonSet pods are not evicted by the kubelet during node-pressure eviction (they have a higher priority). (4) DaemonSet supports rolling updates with maxUnavailable. Use for: node-level infrastructure (CNI, CSI, log shipper, monitoring agent). Use Deployment for: application workloads where you want N replicas spread across nodes.

**Q4: How does Kubernetes garbage collection work for container images?**
A: kubelet periodically checks disk usage: (1) If `imagefs.available` < `--image-gc-high-threshold` (85%), start garbage collecting. (2) Remove unused images (no running container references) starting with the least recently used. (3) Stop when disk usage drops below `--image-gc-low-threshold` (80%). (4) Never remove images used by running containers. (5) containerd uses reference counting — an image is unused if no snapshot references its layers. (6) For pre-pulled images (DaemonSet pull), set `imagePullPolicy: IfNotPresent` to avoid re-pulling after GC.

**Q5: How do init containers differ from sidecar containers?**
A: Init containers: run sequentially before any regular container starts. Each must complete successfully before the next starts. Use for: database migration, config file generation, waiting for dependencies. They share the pod's volumes but not the network namespace (until k8s 1.28 sidecar support). Sidecar containers (native in k8s 1.28+): defined as init containers with `restartPolicy: Always`. They start before regular containers and run alongside them for the pod's entire lifetime. Use for: log shipping, proxy, monitoring agent. Before 1.28, sidecars were implemented as regular containers — but they didn't have guaranteed startup ordering.

**Q6: How does ephemeral container debugging work?**
A: `kubectl debug -it pod/myapp --image=busybox --target=myapp` adds an ephemeral container to a running pod. The ephemeral container: (1) Shares the target container's PID namespace (can see its processes). (2) Shares the pod's network namespace (can access localhost services). (3) Can mount the pod's volumes. (4) Does not affect the existing containers. (5) Cannot be removed once added (pod must be deleted). (6) Use for: debugging containers without a shell (distroless images), inspecting network traffic, profiling.

**Q7: How do you implement zero-downtime deployments in Kubernetes?**
A: (1) Set `readinessProbe` — pod only receives traffic when ready. (2) Set `terminationGracePeriodSeconds` (default 30s) — pod has time to finish in-flight requests. (3) Add `preStop` lifecycle hook — sleep 5s to allow endpoints to propagate before shutdown. (4) Configure `maxSurge: 1, maxUnavailable: 0` in Deployment strategy — always have at least N healthy pods. (5) Set PodDisruptionBudget — prevent voluntary evictions from removing too many pods. (6) Handle SIGTERM in your application — stop accepting new connections, finish existing ones, exit.

**Q8: What happens when you delete a pod?**
A: (1) API server sets `deletionTimestamp` on the pod. (2) Endpoint controller removes pod from Service endpoints. (3) kubelet receives the pod update via watch. (4) kubelet runs `preStop` hook (if defined). (5) kubelet sends SIGTERM to PID 1 in each container. (6) kubelet waits up to `terminationGracePeriodSeconds` (default 30s). (7) If process still running, kubelet sends SIGKILL. (8) kubelet calls CNI DEL to clean up networking. (9) kubelet calls CSI NodeUnpublishVolume to unmount volumes. (10) kubelet reports pod terminated; API server deletes the object. Race condition: step 2 and 3 happen in parallel — connections may still arrive after SIGTERM. The preStop sleep mitigates this.

**Q9: How does Kubernetes handle pod DNS resolution?**
A: (1) Every pod gets `/etc/resolv.conf` configured by kubelet. (2) Default: `nameserver <CoreDNS ClusterIP>`, `search <ns>.svc.cluster.local svc.cluster.local cluster.local`. (3) For `my-svc`, the resolver tries: `my-svc.<ns>.svc.cluster.local`, `my-svc.svc.cluster.local`, `my-svc.cluster.local`, then FQDN. (4) DNS policy options: ClusterFirst (default, use CoreDNS), Default (use node's DNS), None (provide custom resolv.conf). (5) Headless service (ClusterIP: None) returns pod IPs directly (no virtual IP). (6) Pod DNS record: `<pod-ip-dashes>.<ns>.pod.cluster.local`.

**Q10: How do you handle secrets in containers securely?**
A: Layered approach: (1) **Kubernetes Secrets**: base64-encoded in etcd; enable encryption at rest. Mount as files (not env vars — env vars appear in process listing, crash dumps). (2) **External secrets**: use External Secrets Operator to sync from Vault/AWS Secrets Manager. (3) **CSI secrets store**: mount secrets directly from Vault via CSI driver (SecretProviderClass). (4) **Projected volumes with service account token**: use bound SA tokens for workload identity. (5) **Never embed secrets in images**. (6) Rotate secrets regularly; use short-lived tokens where possible.

**Q11: What is the Container Storage Interface (CSI) and why was it created?**
A: CSI is a standard interface between container orchestrators and storage systems. Before CSI, each storage vendor had to add their driver code directly into the Kubernetes codebase (in-tree plugins). Problems: (1) Vendor release cycle tied to k8s release cycle. (2) Storage bugs could crash kubelet. (3) Storage vendor code needed to be open-source. CSI solves all three: vendor-maintained external plugins, running as separate processes (gRPC), with independent release cycles. CSI defines three RPC services: Identity (plugin info), Controller (provision/attach/snapshot), Node (stage/publish/expand).

**Q12: How do you implement pod disruption budgets and why are they important?**
A: PDB specifies the minimum available or maximum unavailable pods during voluntary disruptions (drain, upgrade, autoscaling). Example: `minAvailable: 2` for a 3-replica service ensures at least 2 pods are always running. PDBs are respected by: `kubectl drain`, cluster autoscaler scale-down, voluntary eviction API. They are NOT respected by: node crash (involuntary), kubelet eviction (resource pressure), pod deletion. Without PDBs, a rolling upgrade could temporarily remove all replicas. PDBs can also be a problem — misconfigured PDB can block upgrades indefinitely (e.g., `minAvailable: 3` on a 3-replica Deployment means no pod can be evicted).

**Q13: How does the kubelet decide the order of operations when starting a pod?**
A: (1) Create pod sandbox (CNI: allocate IP, create network namespace). (2) Pull images for init containers. (3) Run init containers sequentially (each must exit 0 before next starts). (4) Pull images for regular containers. (5) Start regular containers in spec order (but they run in parallel). (6) Start sidecar containers (restartPolicy: Always init containers, k8s 1.28+) — these start before regular containers. (7) Run startup probes. (8) Once startup probes pass, start liveness and readiness probes. (9) When readiness passes, add to Service endpoints.

**Q14: How do you handle the case where a container needs to run as root?**
A: First question: does it really need root? Common alternatives: (1) Use Linux capabilities instead of full root (e.g., `NET_BIND_SERVICE` for port 80). (2) Use init container as root, then main container as non-root. (3) Use `fsGroup` to set file ownership. If root is truly required: (1) Use `Baseline` Pod Security Standard (allows root but restricts host access). (2) Never use `Privileged` PSS unless the container needs host namespace access (CNI, CSI). (3) Isolate root containers on dedicated nodes. (4) Use gVisor or Kata Containers for stronger isolation.

**Q15: How does Kubernetes handle multi-architecture images (amd64/arm64)?**
A: (1) OCI image index (manifest list) contains references to platform-specific manifests. (2) containerd pulls the manifest list, selects the matching platform (based on node architecture). (3) `kubectl get nodes -o wide` shows `ARCHITECTURE` column. (4) Use `nodeSelector: kubernetes.io/arch: amd64` if you need to pin. (5) Build multi-arch images with `docker buildx build --platform linux/amd64,linux/arm64`. (6) Useful for mixed clusters (ARM-based nodes for cost savings, x86 for specific workloads).

---

## 16. References

| # | Reference | URL |
|---|-----------|-----|
| 1 | Kubernetes Pod Lifecycle | https://kubernetes.io/docs/concepts/workloads/pods/pod-lifecycle/ |
| 2 | Container Runtime Interface (CRI) | https://kubernetes.io/docs/concepts/architecture/cri/ |
| 3 | Container Storage Interface (CSI) | https://kubernetes-csi.github.io/docs/ |
| 4 | Container Network Interface (CNI) | https://www.cni.dev/ |
| 5 | Cilium Documentation | https://docs.cilium.io/ |
| 6 | cgroups v2 in Kubernetes | https://kubernetes.io/docs/concepts/architecture/cgroups/ |
| 7 | Pod Security Standards | https://kubernetes.io/docs/concepts/security/pod-security-standards/ |
| 8 | Kubernetes Resource Management | https://kubernetes.io/docs/concepts/configuration/manage-resources-containers/ |
| 9 | CoreDNS | https://coredns.io/ |
| 10 | OCI Runtime Specification | https://github.com/opencontainers/runtime-spec |
| 11 | containerd | https://containerd.io/ |
| 12 | Kubernetes Networking Model | https://kubernetes.io/docs/concepts/cluster-administration/networking/ |
