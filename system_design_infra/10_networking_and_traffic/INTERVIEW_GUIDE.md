# Infra-10: Networking & Traffic — Interview Study Guide

Reading Infra Pattern 10: Networking & Traffic — 5 problems, 8 shared components

---

## STEP 1 — ORIENTATION

This pattern covers five interconnected infrastructure problems that together control how traffic flows through a modern cloud platform. The five problems are:

1. **API Gateway Design** — the front door for all external and internal API traffic
2. **DNS at Scale** — how service names resolve to IP addresses across 20,000 pods
3. **Load Balancer Design** — distributing packets and requests across backend fleets
4. **Network Policy Enforcement System** — kernel-level firewall rules per pod
5. **Service Proxy and Sidecar** — transparent L7 proxy injected into every pod

These five systems are not independent. DNS resolves names for the load balancer. The load balancer distributes traffic to pods protected by network policy. Sidecar proxies enforce mTLS on the traffic that passes through the API gateway. Every system ends up sharing the same core infrastructure vocabulary: xDS control plane, eBPF data plane, watch-based config distribution, and two-tier fast-path-plus-smart-path architecture.

The shared components across all five problems are: **two-tier architecture** (fast path + feature-rich path), **watch-based incremental config distribution** (Kubernetes informer or gRPC xDS), **circuit breaker / outlier detection**, **in-memory data structures for O(1) lookup**, **TLS/mTLS termination**, **eBPF kernel-space enforcement**, **distributed rate limiting with local pre-allocation**, and **consistent hashing for session affinity**.

---

## STEP 2 — MENTAL MODEL

### The Core Idea

Every networking and traffic system in a cloud platform is solving the same underlying problem: **how do you route a packet or request to the right destination, enforce rules on it, and do all of that fast enough not to matter?** The word "fast enough" is doing enormous work here. At 1.5 million requests per second with a 1 ms latency budget, you have 1,000 nanoseconds per request. A single Redis round-trip consumes most of that budget. A single iptables chain traversal across 5,000 rules consumes even more.

This forces every system into the same architectural pattern: **move decisions into faster and faster substrates**. The progression is: database (milliseconds) → user-space process (microseconds) → kernel via iptables (tens of microseconds) → kernel via eBPF (single-digit microseconds) → NIC via XDP (sub-microsecond). Every major design decision in this pattern is about how far down this stack you need to push the hot-path logic.

### The Real-World Analogy

Think of a major international airport. The **API gateway** is the check-in counter and security checkpoint — it validates your identity, checks your ticket, enforces rules, and only then lets you through. The **load balancer** is the air traffic control system — it decides which runway and which gate, routing planes (requests) to avoid congestion. The **DNS system** is the signage and announcement system — it translates "Gate B47" into an actual physical location in the terminal. The **network policy** is the set of physical barriers and access cards — the walls and locked doors that prevent someone from the domestic terminal from wandering into international departures regardless of what the signs say. The **service proxy / sidecar** is the personal escort assigned to every passenger — every interaction is supervised, logged, and encrypted, but the passenger (application) never notices the escort is there.

The airport analogy also reveals why this is hard: all five systems must work in real time, at massive scale, with no downtime for reconfiguration, while simultaneously being secure, observable, and cheap to operate.

### Why It Is Hard

Three forces create the difficulty:

**Scale versus latency.** At 20,000 pods each generating 50 DNS queries per second, you have 1 million DNS queries per second. At 150-byte UDP packets, that is 150 MB/s just for DNS. Any synchronous call to an external system on the hot path is immediately disqualifying. Every system solves this by pre-loading all state into in-memory data structures (CoreDNS's ServiceMap, Envoy's cluster table, eBPF hash maps) and receiving updates push-based via watches.

**Consistency versus availability.** When a pod dies, every system in this pattern needs to learn about it quickly. But TTL-based caches, iptables rules, and DNS entries all have stale windows. A misconfigured ndots setting causes 4x query amplification. A 30-second DNS TTL means 30 seconds of traffic hitting a dead pod. A Cilium identity map that doesn't drain fast enough means packets route to an IP that now belongs to a different pod. The "right" TTL is always a tension between staleness and load.

**Security versus transparency.** The sidecar proxy must intercept all pod traffic without the application knowing. iptables REDIRECT achieves this but creates conntrack table pressure. eBPF achieves it more efficiently but requires kernel version 5.15+. mTLS must be enforced everywhere but adding a TLS handshake to every connection adds latency. Short-lived certificates (24h) reduce blast radius but require a CA that can issue 14 certificates per minute reliably.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before diving into design, spend three to four minutes on these questions. What the answer changes is as important as the question itself.

**Q1: Is this system serving external clients, internal service-to-service traffic, or both?**
This changes everything. External traffic needs auth, rate limiting, TLS termination at the edge. Internal east-west traffic needs service discovery, mTLS, circuit breaking. A pure API gateway design and a pure service mesh design are architecturally very different. Mixed traffic (like a load balancer or DNS system) requires careful thought about trust boundaries.

**Q2: What is the scale? How many services, pods, and requests per second?**
The answer determines whether you can use simple approaches or must go to eBPF/kernel-space. 100 services with 1,000 RPS can use basic iptables and polling. 2,000 services with 1.5M RPS requires eBPF, in-memory indexes, and watch-based propagation.

**Q3: What are the consistency requirements? How quickly must a config change propagate?**
Five seconds is the standard target across all five systems. If the interviewer says "immediately" that is a trap — nothing in a distributed system is immediate. Probe what the acceptable staleness window really is, because it determines whether you can use TTL caching or need streaming watches.

**Q4: Are there compliance or multi-tenancy requirements?**
Multi-tenancy changes network policy from "default allow between namespaces" to "explicit whitelist required." Compliance (PCI, SOC 2) adds audit logging, flow log retention, and policy-as-code requirements. These have major architectural implications.

**Q5: What is the bare-metal / cloud split?**
Cloud providers give you managed load balancers, GeoDNS, and security groups for free. Bare-metal requires you to design BGP Anycast, IPVS, eBPF/XDP, and CoreDNS yourself. This pattern assumes bare-metal Kubernetes, which is the harder and more interesting case.

**What changes based on answers:**
- Scale shifts the data plane implementation from iptables → IPVS → eBPF/XDP
- External vs. internal shifts the auth and rate limiting story completely
- Multi-tenancy shifts network policy from optional to mandatory default-deny
- Cloud vs. bare-metal shifts who owns the L4 tier (cloud provider vs. you)

**Red flags to watch for:** An interviewer who says "just use a cloud load balancer" when you are on bare-metal is not engaging with the problem. An answer of "poll every second" for config propagation at 20K pods is a scalability failure. Treating iptables as sufficient for 5,000+ policies is a performance failure.

---

### 3b. Functional Requirements

**Core functional requirements** (what the system absolutely must do):
- Route requests to the correct backend based on configurable rules (path, host, headers)
- Enforce access control (authentication, authorization, network policy)
- Perform health checking and remove unhealthy backends automatically
- Distribute configuration changes to all data plane nodes within seconds
- Observe all traffic with metrics, traces, and logs

**Scope modifiers** (what shifts the design):
- Protocol support: HTTP/1.1 vs. HTTP/2 vs. gRPC vs. TCP/UDP
- Auth mechanism: JWT vs. API key vs. mTLS client certificate vs. OAuth2
- Rate limiting: per-consumer, per-route, global, or none
- Geographic distribution: single datacenter vs. multi-region
- Traffic splitting: canary deployments, A/B testing

**Clear problem statement to open with:**
"Design a system that can route 1.5 million requests per second across 2,000 microservices running in a Kubernetes cluster on bare-metal hardware, with sub-millisecond latency overhead, five-nines availability, and configuration changes that propagate to all nodes in under five seconds."

---

### 3c. Non-Functional Requirements

**Derive NFRs from the problem, do not just recite them.** Walk through this reasoning aloud:

**Latency budget:** If the end-to-end user-facing SLA is 100ms and your backend services take 50ms, you have 50ms left for all the infrastructure between the user and the service. That budget must cover: DNS resolution (~1ms NodeLocal cache hit), load balancer (~1ms L7 or ~50µs L4), network policy enforcement (~50µs eBPF), and sidecar proxy (~1ms per hop × 2 hops). Total infrastructure overhead budget: ~4ms. If any single component uses iptables traversal across 5,000 rules (~2ms), it blows the budget.

**Availability:** 99.999% (five nines) is the standard target. That is 5.26 minutes of downtime per year. DNS failure = total cluster failure. Load balancer failure = no external traffic. These justify HA deployment (3+ replicas), PodDisruptionBudgets, and pod anti-affinity.

**Config propagation:** The agreed target across all five systems is under five seconds from config change to enforcement on all nodes. This drives the use of watch-based distribution (not polling).

**Key trade-offs to mention:**

✅ **eBPF over iptables:** Lower latency and O(1) scaling, but requires kernel 5.15+ and significantly more engineering complexity.

❌ **iptables at scale:** Familiar and proven, but O(n) rule traversal means at 5,000 rules you add ~2ms per first packet, which is unacceptable.

✅ **Short DNS TTLs (5-30 seconds):** Faster reaction to pod churn. 

❌ **Short DNS TTLs:** More cache misses, more CoreDNS load, more upstream DNS traffic.

✅ **Distributed rate limiting via Redis:** Globally accurate limits.

❌ **Distributed rate limiting via Redis:** Every request pays a 1-5ms Redis round-trip. The hybrid approach (local token bucket + Redis for edge cases) resolves this.

✅ **Sidecar mesh (Istio/Envoy):** Full L7 observability, mTLS everywhere, rich traffic management.

❌ **Sidecar mesh:** 20,000 sidecar proxies × 100MB each = 2TB cluster-wide memory overhead. 2ms added per request hop.

---

### 3d. Capacity Estimation

**The formulas and anchor numbers you need to have ready:**

**Traffic sizing:**
- 20,000 pods × 7.5 DNS QPS average = **150,000 DNS QPS** cluster-wide
- 20,000 pods × 50 DNS QPS peak = **1,000,000 DNS QPS** peak
- ndots=5 causes 4× query amplification for external names → peak becomes **4,000,000 QPS** without mitigation
- 2,000 services × 10 replicas = **20,000 backends** for the load balancer
- 2,000 services × 500 average outbound RPS = **1,000,000 east-west RPS** (service mesh)
- 50,000 external API consumers × 10 RPS each = **500,000 external RPS** (API gateway)

**Memory sizing:**
- CoreDNS in-memory cache: 22,000 DNS records × 256 bytes = **~6 MB** (trivially small)
- NodeLocal DNSCache per node: 10,000 records × 256 bytes = **~2.5 MB** per node
- eBPF maps (Cilium): policy map + identity map + conntrack = **~50 MB** per node
- Envoy sidecar: 40 MB base, up to **150 MB** under load per pod; 20,000 sidecars = **~2 TB** cluster-wide
- Redis rate limit counters: 50,000 consumers × 100 routes × 64 bytes = **~320 MB**

**Bandwidth sizing:**
- DNS query traffic at peak: 1M QPS × 512 bytes = **512 MB/s** (4 Gbps) — this is why NodeLocal cache matters
- API gateway ingress: 900K RPS × 2 KB requests = **1.8 GB/s** (14.4 Gbps)
- API gateway egress: 900K RPS × 5 KB responses = **4.5 GB/s** (36 Gbps)
- Total L4 load balancer throughput: ~**112 Gbps** requiring 2× 100G NICs

**Architecture implications from these numbers:**
- DNS at 1M peak QPS cannot go through a single CoreDNS deployment (each pod handles ~30K QPS) → need NodeLocal DNSCache DaemonSet + 10+ CoreDNS replicas
- 14.4 Gbps gateway ingress requires bond interfaces and multiple gateway nodes; one 10G NIC is a hard bottleneck
- 2 TB sidecar memory at scale is a genuine concern and drives the "sidecarless" mesh conversation (Ambient Mesh)
- Rate limit Redis at 320 MB is comfortable — but at 900K RPS with Redis round-trips per request that is 900K Redis ops/second, which is the real bottleneck

**Time to estimate in interview:** 3-4 minutes. Do not over-engineer it. Pick the two or three most architecturally significant numbers and narrate why they matter.

---

### 3e. High-Level Design

**The four to six components that belong on the whiteboard for any Networking & Traffic problem:**

**1. Control Plane** — the "brain" that holds configuration and pushes it to data planes. In Kubernetes this is etcd + the Kubernetes API server + a controller (Istiod, Cilium Operator, CoreDNS). In a standalone API gateway this is PostgreSQL + Route Compiler + xDS server. Always draw this box first.

**2. Data Plane** — the "muscle" that actually processes every packet or request. This is Envoy, eBPF programs, IPVS rules, CoreDNS's in-memory handler. The data plane must have NO synchronous calls to external systems on the hot path.

**3. Config Distribution Channel** — the wire between control plane and data plane. Either gRPC xDS streaming (for Envoy-based systems) or Kubernetes informer watch (for CNI agents and CoreDNS). The key constraint: propagation time < 5 seconds.

**4. Observability Stack** — Prometheus for metrics, OpenTelemetry/Jaeger for traces, Fluentd/Elasticsearch for access logs. Draw this as a sink that all data plane components write to asynchronously.

**5. TLS/Certificate Layer** — where certificates live, how they are distributed, and how they rotate. For a gateway: SDS (Secret Discovery Service) backed by Vault. For a service mesh: Istiod CA issuing SPIFFE SVIDs via SDS.

**6. Health Checker** — active probes (TCP SYN, HTTP GET /healthz, gRPC Health protocol) running on intervals, feeding pass/fail state back to the control plane, which then pushes backend pool updates to data plane nodes.

**Data flow to walk through on the whiteboard:**
1. Client sends request to VIP
2. L4 LB (eBPF/XDP) hashes 5-tuple → selects L7 node → DNAT packet
3. L7 (Envoy) terminates TLS, runs filter chain (Auth → RateLimit → Transform → Route)
4. Envoy selects backend via load balancing algorithm
5. Response traverses reverse path with metrics recorded at each hop

**Config update flow:**
1. Admin updates route in PostgreSQL via Admin API
2. Route Compiler generates new xDS snapshot with incremented version
3. Data plane nodes receive delta over gRPC xDS stream
4. Hot reload applies new config without dropping connections

**Whiteboard order:** Start with the client on the left, draw the VIP/entry point, then the two-tier (L4/L7) structure, then backends on the right. Draw the control plane above everything with arrows down to data plane nodes. Add the observability stack as a horizontal band along the bottom. Certificates and health checker are supporting components that can be boxes within the control plane.

---

### 3f. Deep Dive Areas

Interviewers will probe these three areas most intensively. Present the problem, then the solution, then volunteer the trade-offs without being asked.

**Deep Dive 1: How do you achieve O(1) packet-level performance at scale?**

The problem: at 4.5 million packets per second across 20,000 pods with potentially thousands of network policies, any approach that does O(n) work per packet is disqualifying. Traditional iptables chains are O(n) — 5,000 rules takes approximately 2ms per first packet.

The solution: **eBPF with identity-based policy maps**. Cilium assigns a numeric identity to each unique pod label set. All pods sharing the same labels share an identity (100 pods with app=frontend all have identity 12345). The eBPF program at the pod's veth TC hook does three lookups: (1) LPM trie `cilium_ipcache[src_ip]` → identity (O(log n) for the IP-to-identity step), (2) conntrack hash map for established flows (O(1) fast path — skips policy entirely), (3) policy hash map `cilium_policy_{endpoint_id}[{identity, dport, proto}]` → ALLOW/DENY/REDIRECT (O(1)). Total per-packet overhead for established connections: ~50µs. For first packet of new flow: ~200µs.

The trade-off to volunteer: ✅ This scales regardless of policy count because the hash map lookup is O(1). ❌ It requires a functioning Cilium agent and kernel 5.15+. During Cilium agent restart, eBPF maps persist in kernel (policies continue enforcing), but identity sync may lag. ❌ The identity model means if two services happen to have identical labels, they share an identity and cannot have different policies — label discipline is mandatory.

**Deep Dive 2: Distributed rate limiting — accuracy versus latency**

The problem: rate limiting must be globally accurate (a consumer's limit applies across all 50 gateway nodes), but a centralized Redis call adds 1-5ms RTT to every request. Local-only rate limiting allows burst-through proportional to number of nodes (50 nodes × 1,000 RPS limit = 50,000 RPS actual throughput).

The solution: **hybrid approach**. Each gateway node pre-allocates `global_limit / num_nodes` tokens in a local token bucket. The local bucket handles ~90% of requests at sub-microsecond cost. Only when the local bucket is running low (or on the edge cases) does the node perform a Redis check using a sliding window counter with an atomic Lua script. The Lua script computes `effective_count = prev_window_count × overlap_fraction + INCR(current_window)` atomically, avoiding race conditions. If Redis is unavailable, the system fails open (local-only) with degraded accuracy but no blocking.

The trade-off to volunteer: ✅ Sub-millisecond for ~90% of requests. ✅ Globally accurate under normal conditions. ❌ Brief over-admission during node scale events (local quotas are recalculated from the control plane, but there is a window). ❌ Redis clock skew across nodes can cause window boundary inaccuracy — mitigated by using Redis server time via the TIME command rather than local node time.

**Deep Dive 3: DNS query amplification — ndots and the external lookup problem**

The problem: Kubernetes default `ndots=5` means any hostname with fewer than 5 dots triggers search domain expansion before trying the bare hostname. External names like `api.stripe.com` (2 dots, which is less than 5) generate 4 sequential queries: three NXDOMAIN queries appending cluster search domains, plus the final successful query. At 200,000 external QPS, this becomes 800,000 QPS — 600,000 of which are completely wasted NXDOMAIN queries. At 100 bytes per query this is 60 MB/s of wasted network traffic and unnecessary CoreDNS CPU.

The solution: two complementary optimizations. First, set `ndots=2` in pod dnsConfig. Now `api.stripe.com` has 2 dots which is ≥ ndots, so it tries the bare hostname first — 1 query instead of 4. Second, enable the CoreDNS `autopath` plugin. With autopath, when a query arrives for `api.stripe.com.default.svc.cluster.local`, CoreDNS internally tries all search domains and FQDN in a single pass and returns the winner, so the client only makes 1 round-trip regardless of ndots.

The trade-off to volunteer: ✅ ~75% reduction in DNS query volume by switching ndots=2 alone. ✅ Autopath reduces client round-trips to 1 regardless of ndots. ❌ ndots=2 breaks bare service names that rely on search expansion — but `svc-B` has 0 dots (< 2), so search domains are still tried first for bare names. The subtle failure: `svc-B.default` (1 dot < 2) is fine, but `svc-B.default.svc` (2 dots ≥ 2) would be tried as an absolute name first before search expansion. Teams must understand their service naming patterns before changing ndots. ❌ autopath requires `pods verified` in the kubernetes plugin config, which adds memory overhead (~50 bytes per pod) to store the pod-IP-to-namespace mapping.

---

### 3g. Failure Scenarios

Frame failures at the senior level: not just "what breaks" but "what is the blast radius, how do you detect it, and how do you recover without making it worse."

**DNS failure cascade:** CoreDNS pods are down. NodeLocal DNSCache serves cached entries (up to 30-second TTL). For cache misses, pods get SERVFAIL. Any service that does DNS lookup per request (not using connection reuse) sees ~30s of degradation before NodeLocal cache drains, then a hard failure. The blast radius is total cluster networking if CoreDNS has zero replicas. Mitigation: 3+ CoreDNS replicas with PodDisruptionBudget max-unavailable=1, pod anti-affinity across nodes, and autoscaling. NodeLocal DNSCache provides the survivability buffer.

**Load balancer node failure:** BGP peer adjacency drops. BFD (Bidirectional Forwarding Detection) detects within 1 second. BGP withdraws the failed node's routes. Remaining nodes re-ECMP the traffic. Existing connections on the failed node are reset (TCP RST to clients). Mitigation: consistent hashing for connection affinity means if you have 3 LB nodes and one fails, only 1/3 of connections need to establish new routes (not all connections). The client-side retry handles this.

**Cilium agent restart:** Existing eBPF programs continue to run (maps persist in kernel). In-flight connections continue unaffected. New policy updates pause during the restart window (typically < 5 seconds). If identity map becomes stale, a pod that gets rescheduled to a new IP may not have its new identity in the ipcache yet — the packet arrives with an unknown source identity, defaults to the WORLD identity, and may be dropped if WORLD is denied. Mitigation: Cilium agent restarts are graceful (draining), and ipcache TTLs are short (< 5s).

**API gateway config push failure:** xDS snapshot version mismatch. Data plane nodes ACK the version they received. If a config push fails, the Route Compiler detects the non-ACK and retries with the same version. Data plane nodes continue running the last-successfully-applied config. New traffic routes to old config until recovery. Canary config pushes (push to 1 node first, verify metrics, then roll out) limit blast radius of bad configs.

**mTLS certificate expiry:** If Istiod CA is down for longer than 80% of cert TTL (19.2 hours for 24h certs), pilot-agent fails to rotate. At TTL expiry, Envoy rejects new TLS handshakes — pods cannot establish new connections. Existing long-lived connections continue until they close. Mitigation: Istiod HA (3+ replicas), longer cert TTL (72h buys 57.6 hours of CA outage tolerance), and alerting on cert expiry < 2 hours.

**Senior framing:** In an interview, do not just list failure modes. Say: "The key question is whether this failure is bounded or unbounded. DNS failure is unbounded — it takes down everything. A single LB node failure is bounded — ECMP distributes to surviving nodes. A Cilium agent restart is bounded — eBPF programs keep running. When designing for failures, you want to convert unbounded failures into bounded ones through redundancy, and bounded failures into self-healing ones through automation."

---

## STEP 4 — COMMON COMPONENTS

These eight components appear across two or more of the five problems. Know each deeply.

---

### Component 1: Two-Tier Architecture (Fast Path + Smart Path)

**Why used:** Every networking system needs two kinds of work done: fast, simple decisions (is this packet for this VIP? is this DNS name in cache?) and slow, smart decisions (which backend has fewest connections? does this JWT pass RBAC?). Mixing them forces the smart path's latency onto the fast path. Separation lets each tier be optimized independently.

**How each system uses it:**
- Load balancer: L4 eBPF/XDP tier (<20µs, O(1) hash, 10M+ pps) feeds into L7 Envoy tier (<1ms, content routing, TLS termination)
- DNS: NodeLocal DNSCache DaemonSet at `169.254.25.10` (<0.5ms, 10K record cache) feeds into CoreDNS cluster (<5ms, Kubernetes API lookup)
- API gateway: TLS termination and basic header parsing (fast) feeds into the filter chain — Auth → RateLimit → Transform → CircuitBreak (smart)
- Network policy: eBPF conntrack fast-path (O(1), ~50µs for established flows) feeds into first-packet policy map lookup + optional L7 Envoy redirect
- Service proxy: iptables REDIRECT (kernel, fast) feeds into Envoy sidecar (user-space, smart)

**Key config detail:** The fast tier must be stateless or use in-kernel state (conntrack, eBPF maps). The smart tier handles stateful logic (auth sessions, rate limit counters, circuit breaker state).

**Without it:** Without a fast tier, you run all traffic through the smart tier. Envoy running at a single tier handling 10M pps is impossible — Envoy is a user-space proxy and context-switching alone would consume the entire CPU budget. NodeLocal DNSCache eliminating, alone, reduces CoreDNS load by ~80%.

---

### Component 2: Watch-Based Incremental Config Distribution

**Why used:** Polling for config changes from 20,000 nodes every N seconds is a thundering herd. If 20,000 sidecar proxies all poll every 5 seconds, you get 4,000 requests per second just for config sync. Watch-based distribution inverts this: the server pushes changes to clients, and only when something actually changes.

**How each system uses it:**
- API gateway + service proxy: gRPC **xDS** (Aggregated Discovery Service). Control plane sends delta snapshots (LDS/RDS/CDS/EDS/SDS). Each resource type has a version number. Data plane ACKs received versions. If a node does not ACK, the control plane retries.
- Load balancer + CoreDNS: Kubernetes **informer pattern** (LIST + WATCH). On startup, LIST all relevant resources (Services, EndpointSlices). Then keep a long-poll HTTP WATCH open. The API server pushes Add/Update/Delete events as they happen. Informer caches maintain in-memory indexes with lock-protected updates.
- Network policy: Cilium agent uses the informer pattern to watch NetworkPolicy and CiliumNetworkPolicy CRDs. On delta, it recompiles only the affected eBPF maps.

**Key config detail:** For xDS, always use **delta xDS** (incremental) rather than state-of-the-world xDS at scale. Full state-of-the-world pushes: 20,000 sidecars × 500 KB config = 10 GB per push. Delta xDS sends only what changed.

**Without it:** With polling every 5 seconds from 20,000 nodes, the Kubernetes API server would be overwhelmed. Real-world example: a CoreDNS deployment that polls the K8s API per DNS query adds 1ms per query — catastrophic at 1M QPS.

---

### Component 3: Circuit Breaker / Outlier Detection

**Why used:** A single slow or failing backend can cascade failure across the entire service. If a backend is returning 100% errors but the load balancer keeps sending it traffic, every client request to that backend fails. Circuit breaking removes bad backends from rotation automatically and re-tests them after a timeout.

**How each system uses it:**
- API gateway: error rate tracking per upstream cluster. Circuit opens when error rate exceeds threshold. Half-open probe sends a test request. If successful, circuit closes.
- Load balancer: active health probes (TCP SYN, HTTP GET /healthz, gRPC health protocol) with configurable thresholds (unhealthy after 3 consecutive failures, healthy after 5 consecutive passes). Jitter ±20% on probe interval to avoid probe storms.
- Service proxy (Envoy): **outlier detection** — tracks consecutive 5xx errors per endpoint (`consecutive_5xx: 5`), evaluates every 10 seconds, ejects for base duration of 30 seconds, caps at `max_ejection_percent: 50` (never ejects more than half the pool), and uses exponential backoff on repeat ejections (30s, 60s, 90s, up to 300s cap).
- DNS: upstream resolver failover — if the primary resolver times out, CoreDNS tries the next resolver in the list.

**Key config detail:** `max_ejection_percent: 50` is critical. Without it, a cluster-wide degradation (e.g., a slow downstream database) causes Envoy to eject every backend, leaving 0 healthy — complete failure. Capping at 50% means you always have half the backends serving traffic, even if degraded.

**Without it:** Cascading failure. One bad backend drags down the entire service as all clients wait for timeouts. The circuit breaker converts "upstream latency increases client latency proportionally" to "upstream failures are bounded and self-healing."

---

### Component 4: In-Memory Data Structures for O(1) or O(log n) Lookup

**Why used:** The hot path — the code that runs for every single request or packet — cannot touch a database, network, or even a slow system call. All decisions must be made from data structures loaded into memory or the kernel.

**How each system uses it:**
- API gateway: `HashMap<Host, RadixTrie>` for route matching — O(1) host lookup, O(k) trie walk where k = path depth. 10,000 routes looked up in microseconds.
- Load balancer (Maglev): pre-computed lookup table of size M=65537 (prime). `entry[hash(key) % M]` → backend ID. Single array index access.
- DNS (CoreDNS): `ServiceMap[namespace/name]` → ServiceEntry and `EndpointMap[namespace/name]` → []Endpoint. Pure in-process Go maps. No lock contention except on updates.
- Network policy (Cilium): `cilium_policy_{endpoint_id}` eBPF hash map keyed by `{identity: u32, dport: u16, proto: u8}` → ALLOW/DENY/REDIRECT. Lives in the Linux kernel. `cilium_ipcache` LPM trie maps IP addresses to identity numbers.
- Service proxy: Envoy's in-memory listener/route/cluster/endpoint tables compiled from xDS. O(1) prefix tree lookup.

**Key config detail:** All of these structures are rebuilt or updated asynchronously from the watch-based distribution channel. The hot path only reads (or reads-then-writes for conntrack). Updates happen out-of-band.

**Without it:** Any synchronous database call on the hot path at 1M RPS with 1ms RTT would require 1,000 parallel database connections per node. This is why Kubernetes does not have a "DNS database query" for every DNS lookup — it would be physically impossible.

---

### Component 5: TLS / mTLS Termination

**Why used:** Encryption in transit is a baseline security requirement. TLS termination at the gateway decrypts once and allows inspection, logging, and routing on plaintext internally. mTLS between services adds mutual authentication — both sides verify identity.

**How each system uses it:**
- API gateway: terminates TLS using SNI (Server Name Indication) to select the certificate. SNI field stored in a GIN-indexed array column in PostgreSQL. Certificates are encrypted at rest with KMS-managed keys. Secret Discovery Service (SDS) handles dynamic cert rotation to Envoy data plane nodes.
- Load balancer: L4 tier passes TLS through (no termination); L7 tier (Envoy) terminates. Optionally re-encrypts to upstream (mTLS to backends).
- Service proxy (service mesh): **mandatory mTLS** between all sidecar proxies. SPIFFE X.509 SVIDs issued by Istiod CA (or SPIRE). Format: `spiffe://cluster.local/ns/{namespace}/sa/{service-account}`. Cert TTL 24 hours; rotated at 80% TTL (19.2 hours) with zero connection drops via SDS hot-swap.

**Key config detail:** The UID 1337 exception in iptables is specifically to prevent Envoy's own TLS traffic to backends from being redirected back to itself in a loop. Envoy runs as UID 1337; iptables rules have `-m owner --uid-owner 1337 -j RETURN` to skip interception for this UID.

**Without it:** Without TLS termination at the gateway, you cannot inspect content (for routing or logging), cannot enforce auth, and lose visibility. Without mTLS between services, east-west traffic is plaintext — any node that can observe the network can read inter-service communication. In a multi-tenant cluster this is a critical vulnerability.

---

### Component 6: eBPF / Kernel-Space Enforcement

**Why used:** User-space proxies (Envoy) run at ~1-2ms latency per request. Kernel iptables at 50-200µs per first packet. XDP/eBPF at 5-20µs. For the absolute hot path — L4 packet forwarding and per-packet policy — only kernel-space is fast enough.

**How each system uses it:**
- Load balancer (XDP): NIC driver calls XDP program before kernel network stack. Program parses Ethernet + IP + TCP headers, looks up VIP table, looks up conntrack, selects backend, rewrites destination IP/port (DNAT), and calls `XDP_TX` or `XDP_REDIRECT`. 10M+ packets/second per node, <20µs p99.
- Network policy (Cilium TC hook): eBPF program attached to pod's veth TC (traffic control) ingress and egress. Runs for every packet. Lookup chain: ipcache LPM trie (identity) → conntrack hash map (established?) → policy hash map (verdict). O(1) for established flows.
- Service proxy (planned/Cilium): migration from iptables REDIRECT to eBPF TPROXY at the socket layer. Benefits: no conntrack entries, no iptables rule conflicts, socket-level intercept before packet is sent.

**Key config detail:** eBPF programs have hard constraints: max 512-byte stack, no unbounded loops (verifier rejects them), limited helper functions. This forces clever design — e.g., Maglev hash table pre-computed in user-space, stored in `BPF_MAP_TYPE_ARRAY`, loaded into kernel. The eBPF program just does a single array index.

**Without it:** At 4.5M packets per second cluster-wide, user-space handling would require hundreds of CPU cores just for packet processing. eBPF delivers line-rate processing at a fraction of the CPU cost. This is why Cilium replacing kube-proxy eliminates the iptables/IPVS overhead entirely.

---

### Component 7: Distributed Rate Limiting with Local Pre-allocation

**Why used:** Rate limiting must be globally accurate (consumer's 1,000 RPS limit applies across all 50 gateway nodes, not per-node), but cannot add significant latency to every request. The hybrid approach resolves this tension.

**How it works:**
- Each gateway node holds a local token bucket pre-allocated at `global_limit / num_nodes` tokens, refilling at `global_rate / num_nodes`.
- ~90% of requests are decided locally at sub-microsecond cost.
- When local bucket is low (or for a configurable sampling percentage), the node executes an atomic Lua script against Redis: `effective_count = GET(prev_window) × overlap + INCR(curr_window)`. If `effective_count > limit`, deny.
- If Redis is unavailable: fail open (use local bucket only). Alert on Redis health.

**Key config detail — Lua script math:**
```
current_window = floor(now / window_size)
overlap = 1 - (elapsed / window_size)
effective_count = prev_count × overlap + INCR(curr_window)
```
This gives a smooth rate limit without window boundary spikes (the sliding window counter approach). The fixed window counter approach allows 2× the limit at window boundaries (last second of old window + first second of new window).

**Without it:** Pure local rate limiting allows each of 50 nodes to send `global_limit` requests → 50× over-admission. Pure Redis rate limiting: every request pays 1-5ms → gateway latency dominated by Redis RTT.

---

### Component 8: Consistent Hashing (Maglev + Ring Hash)

**Why used:** When backends are added or removed, **modulo hashing** (hash % N) remaps ~100% of keys. This destroys session affinity and cache locality. Consistent hashing remaps only K/N keys (the theoretically optimal minimum).

**Maglev (O(1) lookup, used for eBPF L4):**
1. Table size M = 65537 (large prime, much larger than N)
2. For each backend B_i: compute `offset_i = hash1(B_i) % M` and `skip_i = hash2(B_i) % (M-1) + 1`
3. Each backend generates a permutation of table slots: `preference[i][j] = (offset_i + j × skip_i) % M`
4. Fill the table greedily: each backend claims preferred empty slots
5. Lookup: `entry[hash(key) % M]` → single array index access O(1)
6. Backend removal: only entries assigned to the removed backend are remapped

**Ring hash (O(log N) lookup, used in Envoy):**
- Place 150-200 virtual nodes per backend on a 2^32 hash ring
- For request key K: `position = hash(K)`, clockwise walk to first virtual node → real backend
- O(log N) via binary search on sorted ring
- Weighted backends: more virtual nodes = more ring slots = proportionally more traffic

**Key config detail:** `minimum_ring_size: 1024` in Envoy's ring_hash config. With too few virtual nodes (< 100 per backend), load distribution has >10% variance.

**Without it:** Modulo hashing during a rolling deployment (10 pods → 9 pods → 10 new pods) causes 90% of sessions to reroute, breaking all stateful connections and defeating cache locality in any memcached-backed tier.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

For each problem, what makes it different from the others, and the unique decisions that come with it.

---

### API Gateway Design

**What is unique:** The API gateway is the only system in this group that explicitly manages **multi-consumer identity** (50,000 API keys, JWT subjects, OAuth2 tokens) and enforces **per-consumer policies** (rate limits, scopes, allowed routes). Every other system in this group works on infrastructure identities (SPIFFE IDs, pod labels, source IPs). The API gateway translates external consumer identity into downstream service identity headers (`X-User-Id`, `X-Consumer-Id`).

**Different decisions:**
- **Auth pipeline is first-class:** JWT validation (with JWKS rotation and bloom filter revocation), API key lookup (prefix index + SHA-256 hash, never plaintext), OAuth2 token introspection (with 200ms timeout and circuit breaking on the IdP), and mTLS client certificates. Other systems do not need this complexity.
- **Plugin/middleware architecture:** The gateway must be extensible without restarts. Wasm plugins for custom logic, native C++ filters for core functionality, `ext_proc` gRPC callout for complex cases. The ordered filter chain (IP Restriction → Auth → RateLimit → Transform → CircuitBreak → Router → Upstream) is specific to the gateway.
- **Route matching is a first-class engineering problem:** Two-phase host hash + radix trie + regex fallback. 10,000+ routes matched in O(1) + O(k). Other systems (DNS, LB) have simpler lookup requirements.
- **xDS configuration is generated, not native:** Unlike service proxy where Istiod translates Kubernetes resources directly to xDS, the gateway has a custom Route Compiler that reads from PostgreSQL and generates xDS snapshots.

**Two-sentence differentiator:** The API gateway is the only system that acts as a trust boundary between external consumers and internal services, translating consumer identity (JWT/API keys) into service-level headers and enforcing per-consumer rate limits. Its defining engineering challenge is the route matching engine and auth pipeline that must each run in under 2ms on the critical path of every external request.

---

### DNS at Scale

**What is unique:** DNS is the only system in this group that is **not configurable by the operator at request time** — you cannot tell a DNS resolver to "use a different algorithm for this query." The configuration lives in ndots, search domain lists, and TTLs set at pod creation time. Changes propagate through stale caches that are outside the operator's direct control. DNS is also the only system here where the default configuration (ndots=5) is **actively harmful at scale**.

**Different decisions:**
- **ndots tuning is a system-level decision that affects all pods:** Changing ndots=5 to ndots=2 saves 600,000 QPS in this scenario. This is not a single-service change; it affects every pod in the cluster and must be tested carefully because some service naming patterns depend on search domain expansion.
- **ExternalDNS controller bridges Kubernetes and external DNS:** No other system in this group has a bi-directional sync between Kubernetes resources and an external managed service (Route53/Cloud DNS). Ownership records (TXT records) prevent conflicting writes.
- **Split-horizon DNS:** Internal and external queries for the same name return different answers. The `internal.example.com` zone is served from a local file; `example.com` is served from Route53. No other system in this group needs this.
- **The data model is dictated by the DNS protocol:** A, AAAA, CNAME, SRV, PTR records have fixed formats. CoreDNS's ServiceMap/EndpointMap must emit valid RFC-compliant DNS responses. The system cannot "optimize" the response format.

**Two-sentence differentiator:** DNS at scale is distinguished by the query amplification problem (ndots=5 turning 1 resolution into 4 queries) and by the fact that its data source (the Kubernetes API) is continuously changing in ways that must propagate to all caches within the TTL window. Unlike the other four systems, DNS's primary failure mode is not a service crash but a misconfiguration that silently degrades every workload simultaneously.

---

### Load Balancer Design

**What is unique:** The load balancer is the only system that must operate at **two completely different performance tiers simultaneously** — L4 at 10 million packets/second with 20µs latency, and L7 at 500,000 RPS with 1ms latency — and make the handoff between them transparent. It is also the only system concerned with **global server load balancing** (GeoDNS, BGP Anycast, multi-datacenter routing).

**Different decisions:**
- **IPVS NAT vs. DR vs. TUN modes:** Direct Server Return (DSR / DR mode) lets backends respond directly to clients, bypassing the LB on the return path. This gives 10× throughput improvement for asymmetric traffic (small requests, large responses). No other system has an equivalent architectural choice.
- **Maglev hashing specifically for eBPF:** Maglev's O(1) lookup via pre-computed array fits eBPF's constraints (no unbounded loops, 512-byte stack). Ring hash is fine for Envoy (user-space), but eBPF cannot do the clockwise ring walk. This is not a performance preference — it is a constraint.
- **BGP ECMP and BFD for HA:** The LB tier achieves HA not through a VIP/VRRP failover but through BGP ECMP — multiple LB nodes all announce the same VIP. BFD (Bidirectional Forwarding Detection) detects node failure in <1 second and withdraws the BGP route. No other system in this group relies on BGP routing.
- **Connection draining is a first-class operation:** When removing a backend, you must drain existing connections gracefully before removing the backend from the pool. This is a unique operational concern not present in the stateless DNS or network policy systems.

**Two-sentence differentiator:** The load balancer is unique in operating simultaneously at kernel-space packet speeds (L4 eBPF/XDP at 10M pps) and user-space request speeds (L7 Envoy at 500K RPS), with BGP ECMP for high availability at the L4 tier and global traffic steering via Anycast. Its hardest engineering problems — consistent hashing with Maglev for eBPF, Direct Server Return to offload return traffic, and connection draining during backend changes — do not appear in any other system in this group.

---

### Network Policy Enforcement System

**What is unique:** Network policy enforcement is the only system in this group that is **purely defensive with zero tolerance for false negatives on the deny side** — a missed allow rule causes application failure, but a missed deny rule is a security incident. It is also the only system where the **policy language is standardized by Kubernetes** (NetworkPolicy API) but implementation quality varies enormously between CNIs.

**Different decisions:**
- **Identity-based model vs. IP-based model:** Cilium's identity model (labels → numeric ID → policy map key) vs. Calico's IP-based model (iptables rules per IP). Identity model scales better: 1,000 pods with same labels = 1 policy entry. IP model: 1,000 pods = 1,000 iptables rules, each time a pod reschedules.
- **Default-deny as a design principle:** No other system starts from "deny everything; explicitly allow what you need." The default-deny cluster-wide policy (`CiliumClusterwideNetworkPolicy` with empty endpoint selector) is the security baseline. Forgetting to allow DNS (UDP/TCP port 53 to kube-dns) breaks all networking silently.
- **Policy-as-code with GitOps:** Policies are YAML in Git. CI validates syntax and checks for conflicts (using OPA conftest). ArgoCD/Flux applies to the cluster. An admission webhook blocks malformed policies at apply time. No other system in this group has this governance requirement built in.
- **L7 policy enforcement via proxy redirect:** When a `CiliumNetworkPolicy` specifies HTTP method/path rules, the eBPF program emits `REDIRECT` verdict (value 4 in the flags byte), directing the packet to a per-node Envoy proxy for L7 inspection. This is a unique hybrid of kernel and user-space enforcement.

**Two-sentence differentiator:** Network policy enforcement is the only system in this group that operates in "default deny" mode and must handle the unique challenge of translating high-level label selectors into kernel-level per-packet decisions across 20,000 pods without degrading throughput. Its identity-based model (collapsing thousands of pods into hundreds of identities) is the key insight that makes eBPF-based enforcement scale where iptables cannot.

---

### Service Proxy and Sidecar

**What is unique:** The sidecar proxy is the only system in this group that operates **inside the pod's network namespace** and must be transparent to the application — the application must not know the proxy exists, must not need to be modified, and must not be affected if the proxy crashes. It is also the only system responsible for **workload identity** (SPIFFE SVIDs), not just infrastructure identity.

**Different decisions:**
- **iptables REDIRECT with UID 1337 exception:** The only mechanism in this group that uses Linux user ID to break a routing loop. Envoy runs as UID 1337; iptables skips interception for this UID to prevent Envoy's outbound connections from being redirected back to itself. This is an Istio-specific convention.
- **xDS resource hierarchy (LDS → RDS → CDS → EDS → SDS):** The service mesh control plane (Istiod) serves all five xDS resource types. No other system in this group uses all five. The xDS dependency chain matters: you need CDS (clusters) before EDS (endpoints), and LDS (listeners) before RDS (routes).
- **SPIFFE/SVID identity model:** Each sidecar has a cryptographic identity (`spiffe://cluster.local/ns/{ns}/sa/{sa}`) embedded in an X.509 certificate. `AuthorizationPolicy` matches on these SPIFFE principals, not on IP addresses or port numbers. This is a much richer identity model than the label-based identity in network policy.
- **Circuit breaking config lives in DestinationRule, not in the gateway:** `outlierDetection`, `connectionPool`, and `loadBalancer` settings are per-upstream configuration in the mesh CRD layer (`DestinationRule`). This is declarative and version-controlled, versus the gateway's imperative API calls.
- **20,000 sidecars × control plane fan-out:** With 20,000 sidecar proxies all maintaining gRPC xDS connections to Istiod, the control plane fan-out is a scaling challenge. Delta xDS is essential — a state-of-the-world push at 20,000 sidecars × 500 KB = 10 GB per push.

**Two-sentence differentiator:** The service proxy/sidecar pattern is unique in requiring the proxy to be invisible to the application (achieved through iptables REDIRECT and UID-based loop prevention) while simultaneously providing the richest identity model of any system in this group (SPIFFE X.509 SVIDs enabling cryptographically verified workload-to-workload authentication). Its scaling challenge is not per-packet performance but control plane fan-out — pushing incremental config to 20,000 sidecar proxies within 5 seconds without saturating the network.

---

## STEP 6 — Q&A BANK

### Tier 1: Surface-Level Questions (Expect These in the First 15 Minutes)

**Q: What is the difference between L4 and L7 load balancing?**
L4 (transport layer) makes routing decisions based on the 5-tuple: source IP, source port, destination IP, destination port, and protocol. It cannot inspect packet content — no HTTP headers, no URL paths. This makes it extremely fast (kernel-space, 10M+ pps) but limited to connection-level decisions. L7 (application layer) terminates the TCP connection, parses the HTTP headers, and makes routing decisions based on content — host header, URL path, query parameters, cookies. This enables content-based routing (send `/api/v2/*` to v2 backends) and TLS termination, at the cost of user-space latency (1ms vs. 20µs). Most production systems need both: L4 as the first tier for raw throughput and L7 as the second tier for intelligent routing.

**Q: How does CoreDNS know about new Kubernetes services?**
CoreDNS uses the Kubernetes informer pattern. On startup, it sends a LIST request to the Kubernetes API server for all Services and EndpointSlices, building in-memory `ServiceMap` and `EndpointMap` data structures. It then keeps a persistent WATCH connection open. When a Service is created, updated, or deleted, the API server pushes an event to CoreDNS's informer callback, which updates the in-memory maps in under a second. There is no polling. DNS queries read directly from these maps, achieving sub-millisecond response times without any external calls.

**Q: What is a sidecar proxy and why is it useful?**
A sidecar proxy is a second container injected into every application pod that intercepts all network traffic in and out of the application. Kubernetes uses an init container (istio-init) to set up iptables rules that redirect all TCP traffic to the sidecar (Envoy) before the application sees it. The application makes a normal HTTP call; the sidecar transparently handles mTLS encryption, load balancing, circuit breaking, retries, and emitting metrics and traces. The application code does not need to be changed. The value is that every service gets the same observability and resilience features automatically, and security policies (like "only service-A can call service-B") are enforced at the infrastructure layer, not in application code.

**Q: Why does Kubernetes use ndots=5 and why is it a problem?**
The `ndots` setting in `/etc/resolv.conf` controls when the resolver tries search domain expansion before treating a hostname as absolute. With `ndots=5`, any hostname with fewer than 5 dots triggers search domain expansion first. Kubernetes sets ndots=5 to ensure short internal names like `svc-B`, `svc-B.default`, and `svc-B.default.svc` (which have 0-3 dots) resolve correctly by trying `svc-B.default.svc.cluster.local` first. The problem is that external names like `api.stripe.com` (2 dots) also trigger search expansion, generating three NXDOMAIN queries before finally resolving the bare hostname. At 200,000 external DNS QPS, this creates 600,000 wasted queries per second.

**Q: What is a circuit breaker in the context of load balancing?**
A circuit breaker monitors the error rate or latency of each backend and temporarily removes ("ejects") backends that are failing. Envoy's outlier detection ejects a backend after 5 consecutive 5xx responses, waits 30 seconds, then sends a probe request to test recovery. The `max_ejection_percent: 50` parameter ensures no more than half the pool is ejected simultaneously — preventing a cascading failure where ejecting too many backends overloads the remaining ones. Without circuit breaking, a failing backend receives traffic proportional to its weight indefinitely, causing every client to wait for timeouts on the fraction of requests that hit the bad backend.

**Q: What is mTLS and why does a service mesh enforce it?**
mTLS (mutual TLS) is standard TLS but with certificate-based authentication on both sides — not just the server presenting a certificate to the client, but also the client presenting a certificate to the server. In a service mesh, every sidecar proxy holds a SPIFFE X.509 certificate that identifies the workload (`spiffe://cluster.local/ns/default/sa/svc-A`). When svc-A calls svc-B, svc-A's sidecar presents its certificate and verifies svc-B's certificate. This means communication is both encrypted and authenticated — any service that cannot present a valid SPIFFE certificate is rejected. This prevents spoofing (a compromised pod cannot impersonate svc-A unless it has svc-A's private key) and eavesdropping.

**Q: How does a Kubernetes NetworkPolicy actually get enforced?**
A NetworkPolicy is a YAML object stored in etcd via the Kubernetes API server. The CNI plugin (Calico or Cilium) runs an agent on every node that watches for NetworkPolicy changes. In Calico (iptables mode), the agent translates policies into iptables rules in the `cali-*` chains, which are evaluated for every packet traversing the FORWARD chain. In Cilium (eBPF mode), the agent compiles policies into eBPF hash maps and attaches eBPF programs to each pod's virtual network interface (veth) at the TC (traffic control) hook. Every packet that arrives at or leaves a pod runs through the eBPF program, which does a hash map lookup to get an ALLOW/DENY/REDIRECT verdict in O(1) time regardless of how many policies exist.

---

### Tier 2: Deep Dive Questions (Expect These After HLD Is Drawn)

**Q: How would you rate-limit without Redis — what are the options and trade-offs?**
Option 1: **Pure local token bucket per node**. Simple, zero latency, but each of N nodes allows the full global limit — over-admission by factor N. Acceptable as a soft limit if the business cost of 2× over-admission is low. Option 2: **Gossip-based counters** (Netflix Concurrency Limits style). Nodes periodically broadcast their local counts; each node maintains a view of total cluster usage. Eventually consistent — brief over-admission during gossip lag (~100ms). Option 3: **Raft-based consensus store (etcd)**. Strong consistency but adds 5-20ms for every counter update — worse than Redis. Option 4: **Accept local-only at the gateway, enforce hard limits at the application service**. The gateway does best-effort rate limiting; the downstream service enforces strict quotas. This is the defense-in-depth approach. The right choice depends on how much over-admission matters. For API monetization (billing based on usage), you need global accuracy and Redis is the correct choice. For DoS protection where 2× burst is acceptable, local token buckets work.

**Q: Walk me through what happens when Istiod goes down in a service mesh.**
Running sidecars continue to function normally — they already have their xDS config loaded in memory and their mTLS certificates in memory. Existing connections continue uninterrupted. New connection establishment also works as long as cert TTLs have not expired. What breaks: new pods cannot get their initial config or certificate (SDS fails), so new pods cannot join the mesh. Dynamic config changes (new VirtualServices, DestinationRules) cannot propagate. Certificate rotation at the 80% TTL mark fails. If Istiod stays down for more than 80% of cert TTL (19.2 hours for 24h certs), sidecars begin failing to rotate and eventually start rejecting new TLS handshakes as their certs expire. Mitigation: Istiod HA (3 replicas minimum), longer cert TTL (72h gives 57.6h outage tolerance), and a monitoring alert for "Istiod unreachable for > 30 minutes."

**Q: Why would you use Maglev hashing instead of ring consistent hashing for eBPF?**
Two reasons: **computational constraints** and **lookup speed**. Ring consistent hashing requires a clockwise walk through a sorted data structure. In user-space (Envoy), this is a binary search over an array — O(log N) with good cache behavior, fast enough. In eBPF, the verifier rejects programs with unbounded loops, and the 512-byte stack prevents maintaining a large sorted array. Maglev solves both: the entire lookup table is pre-computed in user-space and stored in a `BPF_MAP_TYPE_ARRAY`. The eBPF program does exactly one operation: `entry[hash(5-tuple) % M]`. Single array index, O(1), no loops, trivially within eBPF constraints. The trade-off: Maglev requires more memory (65537 entries × 4 bytes = 256KB per table) and rebuilding the table on backend changes (fast in user-space, atomic swap via BPF map updates). Ring hash is simpler to reason about but cannot run in eBPF.

**Q: How does Cilium's identity model scale better than iptables-based enforcement?**
iptables (Calico) works on IP addresses. For a policy "allow frontend to backend," Calico creates iptables rules for every frontend pod IP. If you have 500 frontend pods, that is 500 iptables rules. When a frontend pod reschedules (gets a new IP), you add and remove rules. At scale with 20,000 pods and 500 policies: potentially hundreds of thousands of iptables rules, O(n) per-packet traversal, ~2ms per first packet with 5,000 rules. Cilium's identity model works on label sets. All 500 frontend pods have the same labels and share numeric identity 12345. The eBPF policy map has exactly one entry: `{identity=12345, dport=8080, proto=TCP} → ALLOW`. When a frontend pod reschedules, its new IP is added to `cilium_ipcache` pointing to identity 12345 — the policy map needs no update. 20,000 pods collapse to maybe 1,000 unique label-set identities. The policy map has at most a few thousand entries. Hash map lookup is O(1) regardless of how many entries exist.

**Q: What is EndpointSlice and why does it matter for CoreDNS?**
The legacy `Endpoints` object stores all endpoints for a service in a single Kubernetes object. For a service with 5,000 pods, that is a ~1MB object. Any pod change in that service (scale up, rolling update, pod crash) triggers a full replace of the entire 1MB Endpoints object — the API server writes 1MB, all CoreDNS informers receive 1MB, memory is re-allocated. With EndpointSlices, endpoints are split into slices of up to 100 endpoints each. A service with 5,000 pods has 50 slices. A pod change updates only the affected slice (~10KB instead of 1MB). This reduces API server write load and CoreDNS memory allocation churn by approximately 100x for large services. CoreDNS 1.9+ uses EndpointSlices by default. This is a concrete example of "scale changes the data model."

**Q: How do you handle zero-downtime certificate rotation at 20,000 sidecars?**
Cert rotation uses the SDS (Secret Discovery Service) push mechanism. When a cert nears expiry (at 80% of TTL, so 19.2 hours into a 24h cert), the sidecar's pilot-agent sends a new CSR to Istiod. Istiod signs it and pushes the new cert via the existing SDS gRPC stream. Envoy hot-swaps the certificate: new TLS handshakes use the new cert; existing long-lived connections continue using the old cert until they close naturally. This is atomic from Envoy's perspective — there is no restart, no connection drop, no listener reload. The rate of certificate issuance at 20,000 sidecars / 24h rotation = ~14 certs/minute on average, which is trivial for the Istiod CA. The harder case is root CA rotation — that requires a 48-hour process (add new root to trust bundles, wait for all certs to rotate to new intermediate, remove old root) to avoid breaking existing connections.

**Q: What happens to DNS when a CoreDNS pod is OOMKilled on a large cluster?**
With a large cluster (100K+ endpoints), the CoreDNS informer cache can grow to hundreds of MB. If the pod has insufficient memory limits, it gets OOMKilled. Upon restart, it re-LISTs all resources — another large memory allocation. If the cluster is so large that CoreDNS consistently OOMs on startup, you have a classic bootstrap problem. Solutions: increase memory limits (the right answer), or use EndpointSlices (reduces memory per service by 100x for large services), or use CoreDNS autoscaling with proportional-to-node-count replicas. NodeLocal DNSCache provides an important buffer: during CoreDNS pod restarts, NodeLocal serves cached entries so cluster DNS does not go entirely dark. The practical signal: monitor CoreDNS memory usage as a ratio of `(number of EndpointSlice objects) × (avg size per slice)`.

---

### Tier 3: Staff+ Stress Test Questions (Reason Aloud Under Pressure)

**Q: You have a production incident. DNS resolution is working but latency for external names has spiked from 1ms to 200ms. Walk me through diagnosing this.**
Start with the query path. External name resolution goes: pod → NodeLocal DNSCache → CoreDNS → forward plugin → upstream resolver → authoritative nameserver. 200ms is the signature of an upstream resolver problem. Run `hubble observe` or check CoreDNS `coredns_dns_request_duration_seconds` Prometheus metrics, breaking down by `server` (which CoreDNS pod) and `zone` (which would show `cluster.local` vs. external). If external queries are slow across all CoreDNS pods, the forward plugin's upstream resolvers are the bottleneck. Check the resolver health: are we using the configured resolvers (8.8.8.8, 1.1.1.1), or has `/etc/resolv.conf` on the nodes changed? Check if the network path to the upstream resolvers is experiencing loss (`mtr 8.8.8.8` from a node). Is the NodeLocal cache serving correctly? If NodeLocal cache is broken (check `169.254.25.10` is listening on every node via `ss -ulnp`), queries that should be cached hits are going all the way to the upstream resolver. Also check ndots: if someone recently changed ndots=5 to ndots=2 on a subset of pods, you would see a different mix of cached vs. non-cached external lookups. Finally, check if ExternalDNS recently created hundreds of new records — a large zone sync can temporarily degrade Route53 response times.

**Q: Design a rate limiter that works at 10 million RPS with 99.99% accuracy and adds less than 100 microseconds of latency. Walk me through all the constraints.**
At 10M RPS with <100µs latency, you cannot use Redis. Redis at 1M ops/second is a ceiling, and the RTT alone is 500µs-1ms. You need a purely in-kernel solution. Here is the architecture: Pre-allocate a per-consumer token bucket in an eBPF `BPF_MAP_TYPE_PERCPU_HASH` map (per-CPU buckets eliminate spinlock contention). Each bucket holds `global_limit / (num_nodes × num_cpus)` tokens. The XDP program does an atomic decrement of the per-CPU bucket — this is nanosecond-scale. Periodically (every 10ms), a user-space daemon aggregates all per-CPU, per-node buckets, computes actual cluster-wide consumption, and rebalances token allocations. This gives O(1) per-packet enforcement with accuracy that converges to global accuracy within the rebalancing interval. The constraint: during a 10ms rebalancing interval, you can over-admit by `global_limit × 10ms × (1 - 1/N)` where N is nodes. At 10K global RPS and 10 nodes, that is 100 requests over 10ms. Acceptable for most use cases. For financial-grade accuracy at this scale you need hardware assistance — SmartNIC offload where the rate limit state lives on the NIC's onboard memory and the NIC itself does the enforcement.

**Q: If you had to eliminate the sidecar (and thus iptables REDIRECT and 20,000 Envoy processes) while keeping mTLS and L7 observability, what are your options and what do you give up with each?**
Three approaches exist:

**Option A: Ambient Mesh (Istio's "sidecar-free" mode)**. A per-node "ztunnel" proxy (Rust, extremely lightweight) handles L4 mTLS and observability for all pods on the node. L7 features (HTTP routing, retries, circuit breaking) are handled by a "waypoint proxy" deployed per-namespace or per-service account — only workloads that need L7 features pay the L7 cost. What you give up: the per-pod isolation of sidecar (a buggy ztunnel affects all pods on the node); the rich per-pod traffic stats (you get per-node stats aggregated); and operational simplicity (the waypoint proxy's scaling and placement adds complexity).

**Option B: gRPC proxyless mesh (with gRPC xDS)**. gRPC applications receive xDS config directly (no Envoy). The gRPC library implements load balancing, retries, and circuit breaking. mTLS is handled by gRPC's built-in TLS. What you give up: language support (only gRPC in Go, Java, C++ today), all non-gRPC protocols (REST, Redis), and any L7 features that are not implemented in the gRPC library.

**Option C: CNI-level enforcement (Cilium eBPF with L7 redirect)**. Cilium handles mTLS through kernel-level encryption (WireGuard or eBPF-based encryption between nodes). L7 policy enforcement redirects specific flows to a per-node Envoy proxy. What you give up: per-pod traffic shaping (only node-level), gRPC-stream-level observability (only connection-level), and the VirtualService/DestinationRule traffic management APIs (Cilium has limited L7 policy language compared to Istio).

The real answer for a Staff+ conversation: "The right choice depends on your team's operational maturity and your protocol mix. If 80%+ is gRPC between Go/Java services, proxyless is compelling. If you need L7 policies on arbitrary protocols, Ambient Mesh is the direction Istio is betting on. If you have a strict no-user-space-proxy requirement for latency, Cilium eBPF encryption is the path. I would not recommend a single answer without knowing the workload profile."

**Q: How would the architecture change if request latency requirements went from 1ms to 100 microseconds?**
At 100µs, you have fundamentally changed the constraint. User-space sidecar proxies (Envoy at ~1ms) are completely eliminated from the critical path. iptables REDIRECT (~50µs for conntrack entry creation) is marginal. Here is what the architecture becomes:

L4 load balancing must move entirely to XDP with DPDK or SmartNIC offload. XDP at ~5µs fits the budget; IPVS at ~50µs does not.

Service-to-service communication must bypass any proxy. Either kernel bypass networking (RDMA, DPDK direct NIC access) or at minimum eBPF socket-level load balancing at `connect()` time (Cilium's kube-proxy replacement mode), which avoids DNAT entirely.

DNS cannot be on the critical path. All service IPs must be pre-resolved and cached in the application's memory before requests start. Connection establishment cannot include a DNS lookup.

Rate limiting cannot use Redis or any network call. eBPF per-CPU atomic counters only.

mTLS at 100µs is borderline. TLS 1.3 handshake is 1-2ms — incompatible with 100µs. For existing connections (session resumption), TLS 1.3 0-RTT can add ~50µs. In practice, 100µs end-to-end typically means you need in-memory encryption (IPsec kernel offload or RDMA-level encryption) rather than application-layer TLS.

The honest answer: 100µs end-to-end is a fundamentally different architecture that requires kernel bypass, DPDK, RDMA, and gives up most of the high-level observability that makes Kubernetes valuable. That trade-off is only worth it for HFT, scientific computing, or storage networking workloads.

---

## STEP 7 — MNEMONICS

### Mnemonic 1: "TRACE" — The Five Systems in Decision Order

When a request comes in, it flows through the systems in this order:

- **T**raffic entry (Load Balancer — gets the packet)
- **R**esolution (DNS — turns a name into an IP)
- **A**ccess control (Network Policy — allows or drops the packet)
- **C**ontext enrichment (API Gateway — validates identity, adds headers)
- **E**nforcement of contracts (Service Proxy — mTLS, retries, circuit breaking)

This is not the exact production flow (DNS usually happens before the LB lookup), but it is a useful mental model for explaining which system owns what responsibility.

### Mnemonic 2: "FICKS" — The Six Engineering Moves That Appear in Every System

Every solution to every problem in this pattern uses some combination of:

- **F**ast-path / slow-path split (two-tier architecture)
- **I**n-memory data structures (no database on hot path)
- **C**onsistent hashing (session affinity with minimal disruption on change)
- **K**ernel-space enforcement (eBPF/XDP for anything time-sensitive)
- **S**treaming config propagation (watch/xDS push, not polling)

When you are stuck on a design problem in this pattern, ask: "Which of FICKS am I missing?"

### Opening One-Liner

When the interviewer says "design a [networking] system," start with:

"Before I draw anything, let me make sure I understand the performance envelope — specifically, whether this is on the critical path of a user request, because that determines whether we're in iptables territory, eBPF territory, or XDP territory. At 1ms latency budget you can use Envoy. At 100µs you need the kernel. At 10µs you need XDP or hardware offload. What's the latency SLA?"

This immediately signals that you understand the performance tiers and that design decisions flow from them — which is exactly what FAANG interviewers want to hear.

---

## STEP 8 — CRITIQUE

### What the Source Material Covers Well

The source files are thorough and technically accurate across all five systems. Strengths:

- **eBPF data structures** are covered with actual C code and memory layouts. The Cilium identity model and the two-map approach (ipcache LPM trie + per-endpoint policy hash map) is explained correctly and completely.
- **Maglev hashing** is explained with the actual algorithm — preference list generation, greedy table fill, and O(1) lookup — not just "consistent hashing."
- **ndots amplification** is quantified precisely (600,000 wasted QPS at 200K external QPS with ndots=5).
- **xDS protocol** is covered correctly with the LDS→RDS→CDS→EDS→SDS dependency chain and delta xDS rationale.
- **Capacity numbers** are concrete and internally consistent across all five systems.

### What Is Missing, Shallow, or Potentially Misleading

**Missing: Ambient Mesh (Istio sidecar-free mode)**. The service proxy section does not mention Ambient Mesh, which is the direction Istio is actively moving. Any 2024+ interview will likely ask about this. The guide above covers it in Tier 3 Q&A, but it was not in the source material.

**Missing: eBPF-based traffic interception (Cilium replacing iptables REDIRECT)**. The service proxy section notes "migration planned" but does not explain how Cilium's socket-level interception at the `connect()` syscall works (intercepting before any packet is sent, no DNAT, no conntrack). This is a common interview follow-up.

**Shallow: DNSSEC**. Multiple source files mention "DNSSEC optional" but give no explanation. If your interviewer works on public-facing infrastructure, they may ask about DNSSEC. Know the basics: RRSIG records sign each DNS record set, DNSKEY holds the public key, DS records chain trust to the parent zone. In Kubernetes, DNSSEC is rarely used for internal cluster DNS (it would require signing every record on every pod change) but matters for external zones (Route53 DNSSEC).

**Shallow: Multi-cluster / multi-tenant DNS**. The DNS file mentions "cross-cluster DNS federation" only in passing. Multi-cluster service discovery (Kubernetes MCS API, or Istio multi-cluster with shared control plane) is a real interview topic for senior roles at companies running large Kubernetes fleets.

**Potentially misleading: "fail open" on rate limiting**. The source material says "if Redis is unavailable, fall back to local-only." This is correct for availability, but interviewers may push on whether failing open on rate limiting is acceptable — it depends heavily on the use case. For DoS protection, failing open is fine. For billing/monetization, failing open means you are giving away service for free during Redis outages. Know the distinction.

### Senior Probes That Interviewers Use

These questions distinguish Senior engineers from Staff+ engineers:

1. "Your API gateway is at 900K RPS. Redis for rate limiting adds 5ms per request. How do you design the rate limiting system so that Redis latency does not affect p99 gateway latency?"

2. "You need to enforce that service A can only call service B on GET /api/v1/users, not POST. Walk me through implementing this from scratch — what infrastructure components are involved and what is the enforcement point?"

3. "CoreDNS is using 80% CPU during peak hours. What are the possible causes and how would you diagnose each one?"

4. "An engineer wants to set ndots=0 to eliminate all query amplification. Walk me through what breaks."

5. "Istiod controls 20,000 sidecar proxies. During a config change (a new VirtualService deployed), what is the worst-case latency spike on in-flight requests and why?"

### Common Traps

**Trap 1: Suggesting polling for config distribution.** "The gateway polls etcd every second for config changes." Red flag at Senior+. Always push-based (xDS streaming or informer WATCH). Polling 20,000 nodes × 1/s = 20,000 API server requests per second for config alone.

**Trap 2: Ignoring TTL caching when discussing DNS.** Saying "DNS always returns the current state" is wrong. TTLs cause staleness. NodeLocal adds another 30-second cache layer. Total potential staleness for headless service: 5 seconds (informer propagation) + 30 seconds (NodeLocal TTL) = 35 seconds. This matters when discussing rolling deployments.

**Trap 3: Treating iptables as scalable.** "We use Calico iptables mode with 5,000 policies." Without immediately acknowledging the O(n) per-packet cost and the ~2ms latency at 5,000 rules, this reads as not understanding the performance implications.

**Trap 4: Forgetting the DNS allow rule in network policy.** Every "default deny all" design must explicitly allow UDP/TCP port 53 to kube-dns. Forgetting this means the policy silently breaks all DNS resolution for the affected pods. Interviewers love this gotcha.

**Trap 5: Conflating "circuit breaker" and "rate limiting."** Circuit breaking is about protecting clients from failing upstreams (eject a backend that is returning 5xx). Rate limiting is about protecting upstreams from overloaded clients (reject a consumer that is sending too many requests). They are different problems with different implementations, even though both result in rejecting requests.

---

*End of INTERVIEW_GUIDE.md — Infra Pattern 10: Networking & Traffic*
