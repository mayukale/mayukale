# Infra Pattern 12: Security & Access — Interview Study Guide

Reading Infra Pattern 12: Security & Access — 5 problems, 6 shared components

---

## STEP 1 — PATTERN OVERVIEW

This pattern covers the security and access control layer of a cloud infrastructure platform. It groups five tightly related but distinct design problems that all answer variations of the same question: "how do you prove who you are and control what you're allowed to do, at infrastructure scale, without slowing everything down?"

The five problems are:

1. **API Key Management System** — Generate, validate, rotate, and revoke programmatic API keys for services and pipelines. 50,000 active keys, 100,000 validations per second, leaked-key detection in under 5 minutes.

2. **Certificate Lifecycle Management** — Manage a PKI hierarchy for a bare-metal Kubernetes cluster. 500 nodes, 50,000 pods, all service-to-service traffic uses mTLS, 5,000 SVID issuances per second, zero-downtime certificate rotation.

3. **RBAC Authorization System** — Enterprise role-based access control spanning Kubernetes, OpenStack, job schedulers, and API gateways. 500,000 authorization decisions per second, sub-millisecond cached latency, role hierarchy and object-level relationship-based access.

4. **Secrets Management System** — HashiCorp Vault-compatible central secrets store. 100,000 secret paths, 200,000 active leases, dynamic short-lived database credentials, encryption as a service, HSM-backed auto-unseal.

5. **Zero Trust Network Design** — BeyondCorp-style elimination of implicit trust based on network location. Identity-based micro-segmentation via eBPF, mTLS everywhere, context-aware proxy for human access to kubectl, SSH, and databases. No VPN.

**Why these five problems belong together:** They are not independent. Zero trust is the architectural philosophy that makes the others necessary. Certificates are the cryptographic foundation for mTLS and SPIFFE identity. Secrets management stores the credentials everything else needs. RBAC decides what an identity is allowed to do. API keys are the programmatic access mechanism that sits on top of all of it. An interviewer who asks you to design any one of these may immediately pivot to how it connects to the others, because the real skill being tested is whether you understand the whole security posture, not just isolated components.

**6 shared components across all five systems:**
- Envoy + ext_authz as the on-wire enforcement point
- Cache-first validation (sub-millisecond hot path)
- Deny-by-default security model
- Audit logging with zero-loss guarantee (Kafka → Elasticsearch → S3)
- Kubernetes integration via admission webhooks and CRDs
- Policy propagation via Kafka CDC (under 5 seconds end-to-end)

---

## STEP 2 — MENTAL MODEL

**The core idea:** Every access decision in a distributed system must answer three questions in under a millisecond, without calling a central database every time — "Is this identity real?", "Is this identity allowed to do this action?", and "Has this identity been revoked since I last checked?" The entire design space of this pattern is about answering those three questions fast, correctly, and with a complete audit trail.

**The real-world analogy:** Think of a large concert venue with 50,000 people moving through 100 gates simultaneously. Each gate guard has a laminated list of approved ticket types (the in-process cache). If a ticket looks unusual, the guard calls the central ticket office (the authoritative database). There is a PA system that instantly broadcasts when a ticket is reported stolen (Kafka revocation fanout). The head of security can revoke any ticket remotely and all gates know within 5 seconds (Redis pub/sub + Kafka dual path). The PA system itself has a backup recording (audit log) of every announcement ever made. The venue has no external wall — every door requires a valid ticket, not just the front entrance (zero trust, deny by default). Every gate guard speaks the same language for ticket formats (Envoy ext_authz as the common enforcement point).

**Why this is genuinely hard:**

The difficulty is not any single piece — it is the combination of three competing requirements that all pull in different directions.

First, **latency vs. freshness**. The only way to validate at 100,000 requests per second without melting your database is to cache aggressively. But a cached "valid" entry for a revoked key is a security hole. You need sub-millisecond response and under 5-second revocation propagation simultaneously. Most naive solutions choose one and sacrifice the other.

Second, **availability vs. correctness**. Your authorization system is on the critical path of every API call. If it goes down, the platform goes down. But if you make it fail-open to preserve availability, you lose the entire security guarantee. The answer — fail-closed with graceful degradation and defense-in-depth caching — is not obvious.

Third, **scale vs. consistency**. At 500 nodes and 50,000 pods, you cannot have a single central policy store that every node calls synchronously. But policy changes (like a new RBAC binding or a revoked certificate) must reach all nodes quickly. The solution involves a multi-tier cache hierarchy, Kafka CDC for propagation, and carefully designed TTLs that bound the inconsistency window to an acceptable SLA.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Before drawing anything on the whiteboard, ask these:

**Question 1: What type of credential are we issuing and who consumes it?**
This immediately differentiates the five problems. API keys are for programmatic machine-to-machine access by services and pipelines. Certificates are for mTLS workload identity (Kubernetes pods and nodes). Secrets are for storing and distributing any credential (passwords, tokens, certificates). RBAC is about what you can do after you authenticate. Zero trust is the network-level enforcement layer. If the interviewer says "a service needs to call another service securely," that is probably certificates + RBAC + secrets all at once.

What this changes: if the consumer is a human engineer, you need device trust, MFA, and a context-aware access proxy. If the consumer is a CI/CD pipeline, you need AppRole or OIDC workload identity and no human in the loop. If the consumer is a Kubernetes pod, you need SPIFFE/SPIRE. These are different architectures.

**Question 2: What does "revocation" mean in this context, and how fast must it happen?**
For a leaked API key, you need revocation to propagate to all gateway nodes in under 5 seconds — this drives a dual path of Redis pub/sub (fast, best-effort) and Kafka (reliable, slightly slower). For a certificate on a pod that is being decommissioned normally, TTL expiry is fine (24 hours). For a compromised CA, you need to re-issue every certificate signed by that CA in under one hour. These are wildly different propagation requirements that dictate completely different architectures.

What this changes: drives the entire revocation design. Short-TTL certificates can rely on natural expiry rather than a revocation list. High-volume API keys need a distributed cache invalidation mechanism. A compromised CA requires a full emergency re-issuance runbook, not just a revocation record.

**Question 3: What is the authorization decision throughput and latency requirement?**
500,000 authorization decisions per second at sub-millisecond latency cannot be served from a relational database. This forces an in-process OPA cache, a Redis cluster tier, and a MySQL authoritative tier — a three-tier cache hierarchy. 10,000 secret reads per second can be served from Vault with a Vault Agent sidecar. 100,000 key validations per second need Redis. Knowing the numbers before designing prevents you from proposing a centralized SQL lookup on every request.

What this changes: determines whether you need in-process caching (OPA + LRU), a Redis cluster, or both. Also determines whether you need write-through or write-back cache invalidation.

**Question 4: Is this multi-region or single-region? What are the consistency requirements across regions?**
Single-region simplifies everything — Redis pub/sub handles revocation fanout. Multi-region requires Kafka MirrorMaker to replicate revocation events, and you accept some delay (100-500ms extra) for cross-region propagation. For global API key revocation (leaked key), you need to decide: do you accept a 1-second window where the key still works in a remote region, or do you need instant global revocation? The answer changes whether you add a globally-replicated Redis tier.

What this changes: cross-region revocation design, whether you use Redis Geo-replication, and how you define "revocation SLA" in your NFRs.

**Question 5 (context-dependent): Are we building this from scratch or integrating with existing platform primitives?**
A greenfield system on Kubernetes uses cert-manager, SPIFFE/SPIRE, OPA/Gatekeeper, and Vault as off-the-shelf components — you are designing the integration and configuration, not reinventing the wheel. A brownfield system with legacy LDAP and no service mesh requires bridging between old and new identity models. This question tells you whether to spend time on architecture vs. component selection.

**Red flags that indicate a misunderstanding:** If the interviewer or candidate suggests storing API key plaintext (as opposed to SHA-256 hash), using bcrypt for API keys (appropriate for passwords, not for high-entropy keys on the hot path), using a single root CA that is also the issuing CA (no offline root protection), or putting the authorization service behind a synchronous network call on every API request without caching, these all indicate a misunderstanding of the core constraints. Push back immediately and explain why.

---

### 3b. Functional Requirements

**Core requirement statement:** The system must issue verifiable credentials to identities, validate those credentials on every access request with sub-millisecond latency, enforce deny-by-default access policies, propagate revocations within seconds, and produce a complete tamper-evident audit log of every access decision.

**The non-negotiable core for any problem in this pattern:**
- Generate credentials using a cryptographically secure random source (CSPRNG)
- Never store credential plaintext after initial issuance
- Validate at the edge (gateway/sidecar), not in the business logic
- Revoke with propagation to all enforcement points within 5 seconds
- Log every allow and every deny decision
- Fail closed on validation service unavailability (deny, not allow)

**Problem-specific functional requirements:**

API Key Management specifically requires: scoped permissions per key (namespace and resource type), per-key rate limiting, rotation with a configurable grace period (both old and new key accepted simultaneously), and automated leak detection with auto-revocation.

Certificate Lifecycle specifically requires: PKI hierarchy with offline root CA, automated cert-manager CRD workflow, SPIFFE/SPIRE workload identity SVIDs, mTLS enforcement, zero-downtime rotation, CRL/OCSP for revocation, and emergency mass re-issuance on CA compromise.

RBAC specifically requires: role inheritance (parent-child), scoped bindings (cluster, namespace, resource instance), Kubernetes-native integration via Role/ClusterRole objects, Zanzibar-style relationship tuples for object-level access, and emergency break-glass access.

Secrets Management specifically requires: pluggable auth methods (Kubernetes SA JWT, AppRole, LDAP, OIDC), static KV secrets with versioning, dynamic credentials with auto-revocation on lease expiry, Transit encryption as a service, Vault Agent sidecar for transparent secret injection, and seal/unseal with Shamir's Secret Sharing.

Zero Trust specifically requires: identity on every flow (no implicit trust from network location), eBPF-based micro-segmentation, context-aware access proxy for human access, device trust verification, continuous verification (re-evaluate on context change), and complete network flow audit.

---

### 3c. Non-Functional Requirements

Derive these from the stated scale, not from thin air. The numbers are interdependent.

**Availability:** 99.99% for the data plane (the path that validates credentials on every API call — this is on the critical path, so if it goes down, the platform goes down). The authorization/validation service has no tolerance for downtime. Control plane (policy management, certificate issuance) can tolerate 99.9% — a brief inability to issue new certificates is not an outage as long as existing short-lived certs are still valid.

**Latency:** The two-tier requirement is key. Cached decisions must be under 1 millisecond p99 (this is what the in-process LRU and Redis tiers are designed for). Cold (cache miss) decisions must be under 5 milliseconds p99 (database lookup). Any slower and you will add visible latency to every API call. For eBPF-level network policy enforcement, the target is under 0.1 milliseconds per packet — this is why Cilium with eBPF is chosen over iptables, which can add 1-2ms.

**Throughput:** These are the anchor numbers to memorize. 100,000 API key validations per second. 500,000 RBAC decisions per second. 10,000,000+ network policy evaluations per second (eBPF, kernel level). 10,000 Vault secret reads per second. 5,000 SPIFFE SVID issuances per second during pod churn. Each number dictates a different caching strategy.

**Propagation SLA (the most commonly forgotten NFR):** Revocations must reach all enforcement nodes within 5 seconds. Policy changes (new RBAC binding, new network policy) must reach all nodes within 5 seconds. This is a binding constraint that eliminates pure polling-based approaches and forces event-driven propagation.

**Trade-offs to call out unprompted:**

✅ Short-lived certificates (24-hour TTL) significantly reduce the impact of certificate compromise — at worst you have a 24-hour window. ❌ But they increase issuance volume by 365x compared to 1-year certificates. At 50,000 pods with 2 restarts per day, you need 100,000 issuances per day — Vault PKI handles this fine but it must be designed for.

✅ In-process OPA caching eliminates the network hop for 99%+ of authorization decisions, getting latency under 1ms. ❌ But it creates a staleness window: policy changes take up to 60 seconds to reach cached decisions. You must decide whether 60 seconds of stale RBAC is acceptable (usually yes) or whether you need sub-second consistency (much harder).

✅ Redis pub/sub gives sub-second revocation propagation to all connected nodes. ❌ But Redis pub/sub is fire-and-forget — if a node is disconnected when the message is published, it never receives it. This is why you pair it with a Kafka consumer for guaranteed delivery, creating a dual-path design.

✅ Deny-by-default is the correct security posture — no implicit permissions. ❌ But it means you must explicitly authorize everything, which creates significant operational overhead during initial rollout and requires good tooling for debugging access denials.

---

### 3d. Capacity Estimation

**The formula to internalize:**

For hot-path validation: `Validations/sec × Request size × Cache hit rate ÷ (1 - cache hit rate) = Database QPS`

For example: 100,000 API key validations per second, 99% cache hit rate, 5ms database read:
- Redis handles 99,000 requests per second at under 1ms
- Database handles 1,000 requests per second at 5ms — well within MySQL capacity

For revocation: `Revocations/day ÷ 86,400 × Nodes = Kafka messages/sec` (extremely small — 200 revocations per day across 50 nodes = essentially zero bandwidth)

**Anchor numbers to memorize for this pattern:**

| System | Scale | Throughput | Key Latency |
|--------|-------|-----------|-------------|
| API Key Management | 50,000 active keys | 100K validations/sec | <1ms cached, <5ms cold |
| Certificate Lifecycle | 50K pods, 500 nodes | 5K SVIDs/sec | <500ms Vault PKI, <100ms SPIRE |
| RBAC Authorization | 50K roles, 200K bindings | 500K decisions/sec | <1ms cached, <5ms cold |
| Secrets Management | 100K paths, 200K leases | 10K reads/sec | <10ms server, <1ms Agent cache |
| Zero Trust | 1M+ mTLS connections | 10M+ eBPF evals/sec | <0.1ms eBPF, <10ms mTLS handshake |

**Architecture implications of the numbers:**

100,000 API key validations per second means each validation must use a hash lookup, not bcrypt or any slow hash. At 1ms per Redis roundtrip, you can serve 100K requests per second per gateway node with adequate Redis cluster capacity. But bcrypt at 100ms per hash would need 10,000 CPU cores — impossible.

500,000 RBAC decisions per second means every node needs an embedded OPA instance with an in-process LRU, not a remote authorization service. A remote call at 1ms still adds 500 seconds of cumulative latency per second of traffic — that is a 0.5ms overhead per request that is visible to end users at scale.

**Storage math:**

API key usage data: 4 billion events per day × 50 bytes = 200 GB per day, 18 TB for 90-day hot retention. This is why usage data goes to Elasticsearch with time-partitioned indices, not MySQL.

RBAC audit log: 20 billion decisions per day × 200 bytes = 4 TB per day. You cannot store 90 days of this in Elasticsearch economically — you keep 90 days hot and archive to S3 for 7 years for compliance.

**Time to spend on estimation in an interview:** 5-8 minutes. Present the key numbers, derive the hot-path QPS to justify caching, derive the storage size to justify your storage selection, and move on. Do not spend 20 minutes on arithmetic — that is not what is being evaluated.

---

### 3e. High-Level Design

**The canonical HLD for a request validation flow (works for 4 of 5 problems):**

```
Client → Envoy Gateway (ext_authz) → Cache (Redis / LRU) → [on miss] Validation Service → Authoritative Store (MySQL) → Allow/Deny → Audit (Kafka)
```

**For certificates specifically:**

```
Pod startup → cert-manager CRD / SPIRE Agent → Vault PKI Engine / SPIRE Server → Signed Certificate → Pod TLS Store
                                                       ↑
                                              Intermediate CA (Vault)
                                                       ↑
                                               Root CA (offline, HSM)
```

**The 4-6 components every answer needs:**

1. **Identity Enforcement Point** — Envoy ext_authz filter (API keys, RBAC), Envoy sidecar + Istio AuthorizationPolicy (mTLS), Cilium eBPF (network policy). This is the place where access decisions are enforced, not merely evaluated.

2. **Hot Path Cache** — Redis cluster for distributed cache (sub-1ms), in-process LRU for zero-network-hop (OPA embedded). Without this, your throughput collapses by 3 orders of magnitude.

3. **Authoritative Store** — MySQL 8.0 for metadata, permissions, audit. Vault Raft for secrets. etcd for Kubernetes-native resources. This is the source of truth but should never be on the hot path at scale.

4. **Event Bus** — Kafka for revocation fanout, CDC policy propagation, audit stream. This is what makes revocation fast and audit loss-proof simultaneously.

5. **Credential Issuer** — Key Management Service (API keys), Vault PKI + SPIRE Server (certificates), Vault Raft cluster (secrets). The credential issuer is never on the validation hot path.

6. **Audit Sink** — Elasticsearch (90-day hot search) → S3 (7-year compliance archive). Every allow and every deny must land here within seconds.

**Data flow for the most important scenario (revocation):**

Admin revokes credential → Credential service writes revoked status to MySQL → Publishes event to Kafka `*.revocations` topic → All gateway/enforcement nodes consume event via Kafka consumer → Immediately delete the cached entry from Redis → Also publish to Redis pub/sub channel for nodes with active connections → Periodic MySQL sync every 60 seconds as defense-in-depth backstop.

**Key decisions to explain unprompted:**

Why SHA-256 and not bcrypt for API key hashing? Because API keys have 238 bits of entropy — brute-force is computationally infeasible regardless of hash speed. bcrypt adds 100ms per hash; at 100,000 validations per second, that is physically impossible to compute.

Why dual-path revocation (Redis pub/sub AND Kafka)? Redis pub/sub is fast (under 1 second) but fire-and-forget — a disconnected node misses the message. Kafka is guaranteed delivery but takes up to 2 seconds. Both together gives you fast-by-default and correct-always.

Why Vault Raft instead of Vault+Consul? Vault Raft (integrated storage since Vault 1.4) eliminates a runtime dependency on Consul, reducing failure modes. A 5-node Raft cluster with 3-of-5 quorum gives HA without needing to manage a separate Consul cluster.

Why offline root CA? The root CA is used once a year to sign new intermediate CAs. Keeping it air-gapped and HSM-protected means a compromise of the online infrastructure cannot compromise the root of trust. If the online intermediate CA is compromised, you revoke it and re-sign a new one from the offline root without needing to re-issue the root.

**Whiteboard order:** Draw the client and the enforcement point first (shows you understand the hot path). Then draw the cache tier. Then the authoritative store. Then the event bus connecting them. Finally add the credential issuer, the audit sink, and the monitoring layer. This order tells a coherent story rather than jumping between layers.

---

### 3f. Deep Dive Areas

**Deep Dive 1: Sub-millisecond authorization at 500,000 decisions per second**

**The problem:** A relational database can serve approximately 5,000-10,000 QPS at 5ms latency. You need 500,000 decisions per second. The math does not work without caching. But caching introduces staleness — a revoked permission that is still cached for 60 seconds is a security risk.

**The solution:** Three-tier cache hierarchy.

Tier 1 is the in-process LRU cache, embedded in the OPA sidecar running on each node. 100,000 entries (approximately 100MB of memory), 60-second TTL. Latency is zero network hops — pure memory lookup, under 0.1ms. This handles 99%+ of traffic. The OPA engine is embedded as a Go library, so policy evaluation itself is in-process.

Tier 2 is the Redis cluster. This serves cache misses from tier 1 and provides cross-node consistency — if a role binding changes, we can invalidate the Redis entry and all nodes that miss their LRU will get the updated answer from Redis before hitting MySQL. Latency is under 1ms.

Tier 3 is MySQL. Only cache misses from both tiers hit the database — approximately 0.1% of traffic. Latency is under 5ms and the load is manageable (500 QPS on MySQL is trivial).

**The critical invariant:** Cache invalidation happens via Kafka CDC. When a role binding changes in MySQL, Debezium captures the change and publishes to a Kafka topic. Every OPA sidecar and every Redis tier has a consumer that invalidates the relevant cache entries. The maximum staleness window is the TTL (60 seconds) minus the time the event took to arrive (under 5 seconds) — so at worst 55 seconds of stale policy. For security-critical changes (revocation, break-glass), you publish a force-invalidation event that wipes the cache entry regardless of TTL.

**Trade-offs to call out unprompted:**

✅ Three-tier cache makes 500K decisions/sec feasible with a small MySQL cluster. ❌ 60-second staleness means a newly granted permission takes up to 60 seconds to take effect. For most RBAC changes this is acceptable; for revocation of sensitive access, you must publish a force-invalidation event.

✅ In-process OPA means policy evaluation never adds a network hop. ❌ Each node has a copy of the full policy bundle (~100MB RAM). At 500 nodes, that is 50GB of replicated policy data. Acceptable for this scale but must be measured.

---

**Deep Dive 2: Zero-downtime certificate rotation at 50,000 pods**

**The problem:** When a certificate is about to expire, the service must switch to a new certificate without dropping any in-flight connections. This is especially hard when both the client and server must present certificates — if one side rotates first and the other still only trusts the old certificate, you get a mTLS handshake failure and a hard outage.

**The solution:** Proactive renewal at 67% of TTL, dual-certificate trust window, and SPIRE's transparent rotation model.

SPIRE agents renew SVIDs at 16 hours into a 24-hour TTL (67% of lifetime). When the new SVID is delivered via the Workload API, the pod's Envoy sidecar adds the new certificate to its trust store while keeping the old one valid. During the overlap window (the remaining 8 hours), both the old and the new certificate are accepted in TLS handshakes. Since all pods across the cluster are renewing proactively before expiry — not all simultaneously at hour 24 — there is never a moment where all certificates expire at once.

cert-manager uses the same principle for Kubernetes-managed certificates: `renewBefore: 8h` on a 24-hour certificate means cert-manager requests a new certificate 8 hours before expiry, stores it alongside the old one in the k8s Secret, and only switches the active pointer once the new certificate is confirmed valid.

For ACME/Let's Encrypt public certificates (90-day TTL), cert-manager renews at 30 days before expiry — a 60-day valid window gives ample time for retry if Let's Encrypt is temporarily unavailable.

**Trade-offs to call out unprompted:**

✅ 24-hour certificate TTL dramatically limits the blast radius of a compromised certificate — at most 24 hours of exposure before natural expiry. ❌ But 50,000 pods × 2 restarts per day = 100,000 certificate issuances per day. Vault PKI must be provisioned to handle 5,000 issuances per second during peak pod churn (rolling deployments). This is entirely achievable but must be in your capacity plan.

✅ Separate intermediate CAs per trust domain (infra, workload, ingress) means a compromised workload intermediate only requires re-issuing workload certificates, not node certificates or public-facing TLS certificates. ❌ Operational complexity: you must maintain separate Vault PKI mounts, separate monitoring, and separate renewal pipelines for each CA.

---

**Deep Dive 3: Vault seal/unseal and the cold start problem**

**The problem:** Vault encrypts all stored data with a master key. On startup, Vault is sealed — it cannot serve any requests until it knows the master key. If Vault needs to restart (OS reboot, crash, upgrade), every pod that needs secrets is blocked until Vault unseals. In a bare-metal environment without a cloud KMS, auto-unseal requires creative engineering.

**The solution:** Shamir's Secret Sharing with HSM-backed auto-unseal.

In development, the master key is split into N=5 shares with a 3-of-5 threshold. Any 3 key holders can unseal by entering their share. In production, this human ceremony is replaced by auto-unseal: an HSM (connected via PKCS#11) or a separate "seal Vault" cluster stores the unseal key and Vault contacts it programmatically on startup. No human is needed for restarts.

With a 5-node Raft cluster (1 active + 4 standby), a node failure triggers leader election within 30 seconds. The new leader is already running and does not need to unseal — Raft replication ensures it has a copy of the master key in memory. Only a full cluster restart requires re-unsealing. The 5-node quorum tolerates 2 simultaneous node failures without losing availability.

Dynamic secrets are the key scaling technique: instead of storing a MySQL password that must be rotated manually, Vault's Database engine creates a temporary MySQL user on request, grants minimum required privileges, and automatically drops that user when the lease expires. The result is that a compromised container's database credential expires automatically — the attacker's access self-terminates.

**Trade-offs to call out unprompted:**

✅ Dynamic secrets mean leaked credentials are self-expiring and individually traceable. ❌ If Vault is unavailable, services cannot get new credentials. Applications must handle the case where their database credentials expire and Vault is temporarily unreachable — Vault Agent caching buys a buffer equal to the lease TTL.

✅ HSM-backed auto-unseal means a Vault cluster can restart without human intervention. ❌ The HSM becomes a single point of failure for the unseal path. You mitigate this with HSM clustering (active-passive pair) or a backup manual unseal procedure for break-glass scenarios.

---

### 3g. Failure Scenarios

**Failure 1: Redis cluster is unavailable**

Every system in this pattern has a Redis dependency. When Redis goes down:
- API key validation falls back to MySQL (5ms instead of <1ms, volume drops to what MySQL can handle — about 5,000-10,000 validations per second instead of 100,000). Rate limiting degrades because you cannot do atomic sliding window operations without Redis — you fail open on rate limiting (allow traffic through, accept some burst abuse).
- RBAC decisions fall back from Redis tier to MySQL cold lookups. LRU still works for cached entries. Newly unseen subjects hit MySQL directly.
- Revocation pub/sub stops working — Kafka consumer path continues to deliver revocation events, so revocations still propagate, just without the sub-second Redis pub/sub fast path.

Senior framing: "Redis is not a single point of failure for correctness — it is a performance optimization and latency accelerator. MySQL is always the authoritative source. Redis loss degrades performance significantly but does not cause incorrect decisions. We monitor Redis availability and have automatic alerts for latency spikes above 5ms on the validation path."

**Failure 2: A CA private key is compromised**

This is the nightmare scenario for certificate management. If an intermediate CA's private key leaks, every certificate signed by that CA is untrustworthy.

The response procedure: immediately revoke the intermediate CA in Vault (this stops new issuances from that CA), publish an updated CRL, update the OCSP responder, issue a new intermediate CA from the offline root CA (this requires a formal key ceremony), update all trust bundles to include the new intermediate CA, and trigger emergency mass re-issuance of all affected leaf certificates. At 50,000 pods, re-issuing all workload SVIDs should take under 1 hour (Vault PKI can do 5,000 issuances per second).

The reason separate intermediate CAs per domain (infra/workload/ingress) exist is precisely for this scenario. Compromising the workload intermediate does not require re-issuing node certificates or public-facing TLS certificates.

Senior framing: "The blast radius of intermediate CA compromise is bounded by the trust domain. We never let the root CA go online, which means root compromise is not a realistic operational risk — it would require physical access to the air-gapped HSM. Intermediate CAs are designed to be replaceable."

**Failure 3: A race condition between key rotation and revocation**

During the grace period of an API key rotation, both the old and new key are valid. If the old key is simultaneously flagged as leaked by the GitHub scanner, the leak response system must revoke it immediately — even though it is technically in "grace period" status, not "active" status. The grace period exception must be overridden by the revocation.

The system handles this by treating "leaked" status as an override that trumps all other status values. The validation logic checks: if `status = 'leaked'` OR `status = 'revoked'`, deny — regardless of whether a grace period is in effect.

**Failure 4: A network partition isolates a cluster of gateway nodes**

The isolated nodes cannot receive Kafka revocation events or Redis pub/sub messages. Their in-process caches continue serving decisions based on last-known state. After cache TTL expires (30-60 seconds), cache misses fall back to local MySQL reads (if MySQL is accessible in the partition) or allow fallback (if not). In the worst case, revocation takes up to the TTL duration plus the partition duration to take effect on isolated nodes.

Senior framing: "This is a CAP theorem reality. We choose AP — availability plus partition tolerance — over strict consistency for the hot path. The revocation SLA is 5 seconds for 99.9% of nodes under normal conditions. For the extreme case of a network partition, we document the 60-second worst-case TTL in our security documentation. For immediate revocation (leaked key), we use the force-delete path that also sets a bloom filter flag that prevents any cache entry from being created for the revoked key hash."

---

## STEP 4 — COMMON COMPONENTS

Every problem in this pattern shares these building blocks. Know each one cold.

---

### Envoy + ext_authz Filter

**Why it's used:** Envoy is the universal enforcement point. Rather than putting authorization logic in every service (which would create N different implementations), you put it at the proxy layer once. The ext_authz filter intercepts every request before it reaches the upstream service and calls an external authorization service. If authorization fails, Envoy returns 401/403 without ever touching the upstream. This gives you a single, auditable point of enforcement that services cannot bypass.

**Key configuration:** The ext_authz filter calls the authorization service with the full request context (headers, path, method, source IP). For API keys, it hashes the key and calls the Key Validation Service. For RBAC, it calls the OPA sidecar. For mTLS/certificate validation, validation happens inline in the TLS handshake itself without a separate ext_authz call. The filter must have a timeout (typically 5ms for the hot path) and a `failure_mode_deny: true` setting — if the authorization service times out, Envoy denies the request, not allows it.

**What happens without it:** Without a centralized enforcement point, every service independently implements authorization. In practice, some services skip it, some implement it incorrectly, and you have no way to audit cross-service access decisions uniformly. This is how breaches propagate — a compromised service can freely call other services that do not enforce authorization.

---

### Cache-First Validation (Two-Tier: In-Process LRU + Redis Cluster)

**Why it's used:** The validation throughput requirements (100K to 500K decisions per second) exceed what any relational database can serve synchronously. The only way to achieve sub-millisecond latency at this scale is to serve the vast majority of requests from in-memory caches at the enforcement point.

**Key configuration:** In-process LRU (OPA/RBAC: 100,000 entries, 60-second TTL; API keys: the Redis client itself acts as the in-process tier via pipelining). Redis cluster for cross-node consistency and cache miss handling. MySQL for cold lookups. The cache key must be stable: for RBAC, it is `hash(subject_type + subject_name + action + resource_type + scope)`. For API keys, it is `hex(SHA-256(key))`. Never include mutable context (timestamps, request IDs) in the cache key. Cache invalidation triggers on Kafka CDC events — when MySQL changes, the event consumer calls Redis DEL and the in-process LRU entry will naturally expire on next TTL cycle or be explicitly evicted.

**What happens without it:** At 100,000 API key validations per second hitting MySQL directly, you would need approximately 100 MySQL replicas to serve 5,000 QPS each. The cost and operational complexity are untenable. More importantly, at 5ms per MySQL read, the total validation latency would be 5ms — added to every API call, this creates visible user-facing slowness.

---

### Deny-by-Default Security Model

**Why it's used:** Implicit trust is the root cause of most security breaches in distributed systems. If a service can call another service freely because "they're both inside the network perimeter," one compromised service becomes a pivot point for attacking the entire platform. Deny-by-default means every flow — service to service, human to service, workload to database — must carry a valid credential that is explicitly authorized. Nothing works by default.

**Key configuration:** In OPA Rego: `default allow := false`. In Cilium: `CiliumNetworkPolicy` default-deny on all pods — every namespace gets a default deny-all policy and services must explicitly whitelist their allowed inbound and outbound connections. In Vault: every secret path requires an explicit policy capability (`read`, `write`, `list`). In API key management: an unrecognized key hash returns 401 immediately, not 403 — "who are you?" before "what are you allowed to do?"

**What happens without it:** Any service that gets a valid network connection can make arbitrary API calls to other services. A compromised pod can query the database, call the secrets manager, and exfiltrate credentials. The blast radius of a single compromise becomes the entire platform. In zero trust terminology, this is "implicit trust based on network location" — exactly the model that zero trust architecture is designed to replace.

---

### Audit Logging with Zero-Loss Guarantee

**Why it's used:** Security audit logs are evidence, not metrics. They must be complete — a single dropped allow decision could mean a compliance violation. A single dropped deny decision could mean an undetected attack that was invisible in the audit trail. Zero-loss guarantee requires durable, at-least-once delivery semantics with idempotent consumers.

**Key configuration:** Every decision emits an event to Kafka with replication factor 3 and `acks=all` (the producer only considers the write complete when all 3 replicas acknowledge). This makes Kafka the durable write-ahead log for audit events. Kafka consumers write to Elasticsearch (90-day hot retention for search and dashboards) and S3/HDFS (7-year cold retention for compliance). The Elasticsearch index is time-partitioned — new indices per day allow efficient deletion of old data without expensive DELETEs. The Kafka topic for audit events uses a separate consumer group from the revocation topic, so audit consumer lag does not block revocation processing.

**What happens without it:** Without durable ordered delivery, audit events written directly to Elasticsearch can be dropped during network partitions or Elasticsearch backpressure. A compliance audit that reveals gaps in the audit log is a regulatory problem. An incomplete audit trail during a security incident means you cannot reconstruct what happened, which can turn a security incident into a legal liability.

---

### Kubernetes Integration via CRDs and Admission Webhooks

**Why it's used:** Kubernetes is the control plane for the entire workload fleet. Integrating security controls at the Kubernetes API layer means policies are enforced before a workload even starts, not just after it is running. cert-manager Certificate CRDs allow certificate lifecycle to be declared and managed like any other Kubernetes resource. OPA/Gatekeeper admission webhooks block non-compliant workloads at the `kubectl apply` or `helm install` step rather than discovering the violation in production.

**Key configuration:** cert-manager: `Certificate` CRD declaratively specifies `duration`, `renewBefore`, `issuerRef`, and `secretName`. The cert-manager controller watches all Certificate objects and manages the full lifecycle — CSR generation, Vault submission, secret storage, renewal scheduling. OPA/Gatekeeper: `ConstraintTemplate` defines the Rego policy; `Constraint` instances apply it to specific namespaces or resource types. The webhook is registered with `failurePolicy: Fail` for production namespaces (deny if the webhook is unavailable) and `failurePolicy: Ignore` for system namespaces (do not block system operations).

**What happens without it:** Without Kubernetes integration, security policies are enforced only after workloads are running — a pod that mounts a broad Vault policy or uses `automountServiceAccountToken: true` is only detected by manual audit, not at admission time. cert-manager without CRD integration requires manual certificate renewal scripts, which fail silently when the certificate is already expired.

---

### Policy Propagation via Kafka CDC (under 5 seconds)

**Why it's used:** Policy changes — a new role binding, a revoked credential, a new network policy — must reach all enforcement nodes within seconds. Pure polling (nodes checking MySQL every N seconds) has a worst-case propagation time equal to N and creates O(nodes × policies) QPS on MySQL. Event-driven propagation via Kafka CDC means MySQL only needs to handle the initial write, and all nodes receive the change within the Kafka delivery latency (under 1 second for intra-datacenter).

**Key configuration:** Debezium captures MySQL row changes via binlog replication and publishes them to Kafka topics named by table (e.g., `platform.rbac.role_bindings`). OPA sidecars have a Kafka consumer watching these topics and update their in-process policy bundle incrementally. For API key revocations, the Key Management Service both publishes to Kafka (`key.revocations`) and issues a Redis pub/sub message for sub-second delivery to nodes with active connections. Kafka consumer groups allow multiple consumers (OPA sidecars, Redis invalidators, audit log processors) to independently consume the same events without interfering.

**What happens without it:** Without event-driven propagation, every node must poll MySQL for changes. At 500 nodes polling every 5 seconds: 100 QPS on MySQL just for polling, plus the 5-second staleness window is a hard lower bound. For revocation propagation, 5 seconds is already at the edge of the SLA. Any MySQL latency spike would push revocation outside the SLA, which is a security breach (a revoked credential that works for 10 seconds instead of 5).

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

What makes each problem unique, and what would you draw or explain differently for each one.

---

### API Key Management System

**The unique architectural element** is the key format design. The `plat_{env}_{40-char-base62}` prefix structure is not just aesthetics — it enables GitHub secret scanning to detect leaked keys via regex (`plat_(live|test|ci)_[A-Za-z0-9]{40}`). Without a recognizable prefix, automated leak detection requires hashing every string in a scanned file and checking against a database. The prefix turns leak detection from an O(file_size × database_size) problem into an O(file_size) regex scan.

The other unique element is **rate limiting per key with global consistency**. A key with a 100 requests-per-second limit must be enforced globally across 50+ gateway nodes — you cannot just count per node (that would give 100 req/sec × 50 nodes = 5,000 effective rate limit). The solution is a Redis sliding window Lua script that atomically increments a counter with the current timestamp, removes entries older than the window size, and either allows or denies. This is a single Redis roundtrip per request (0.5ms), enforcing a globally consistent rate limit.

**Two-sentence differentiator:** API Key Management is distinguished by its focus on the credential lifecycle (never store plaintext, only SHA-256 hash), automated leak detection via prefix-based regex scanning, and the unique dual-requirement of sub-millisecond validation throughput combined with sub-5-second revocation propagation to all gateway nodes. Unlike certificates which have a fixed TTL, API keys can live indefinitely and require explicit revocation, making the revocation propagation problem more critical.

---

### Certificate Lifecycle Management

**The unique architectural element** is the PKI hierarchy with the offline root CA. Every other system in this pattern manages credentials that can be rotated quickly if compromised. A certificate authority's key compromise is categorically different — if the root CA is compromised, every certificate ever signed by that CA (or by any intermediate it signed) must be treated as potentially compromised. The offline, air-gapped, HSM-protected root CA is the architectural answer: it is used once a year, the key never touches a network-connected system, and compromise requires physical access to both the HSM and the air-gapped machine.

The other unique element is **SPIFFE/SPIRE for workload identity**. API keys are manually issued to known services. Certificates via cert-manager require a Kubernetes CRD per service. SPIRE does something categorically different: it attests workload identity at runtime based on cryptographically verifiable facts (this pod is running in namespace X with service account Y, on node Z that has a valid PSAT token). The SPIFFE ID (`spiffe://cluster.local/ns/myapp/sa/myapp-sa`) is issued automatically without any manual registration, scaling to 50,000 pods without operational overhead.

**Two-sentence differentiator:** Certificate Lifecycle Management is unique in its management of a PKI trust hierarchy — the offline root CA, the intermediate CAs per trust domain, and the automated issuance pipeline via cert-manager and SPIRE — because the failure mode (CA key compromise) requires mass re-issuance of potentially all certificates in the platform. The SPIFFE/SPIRE workload identity model solves the "which pod is this?" problem cryptographically and at scale in a way that API keys and secrets management cannot.

---

### RBAC Authorization System

**The unique architectural element** is the combination of three access control models in one system: classic RBAC (role bindings to subjects), ABAC (attribute-based conditions on permissions, like "only in environment:prod namespaces"), and ReBAC (Zanzibar-style relationship tuples for object-level access like "user X is owner of project Y"). Most systems need all three but implement them inconsistently across different subsystems. The unified OPA Rego evaluation engine can express all three models in a single policy language.

The other unique element is **role inheritance**. The `parent_role_id` pointer on the roles table allows `platform-admin` to automatically inherit all permissions of `namespace-admin`. This is powerful but dangerous — adding a permission to a parent role implicitly grants it to all child roles. The OPA evaluation must resolve the full ancestor chain, and any new permission on a parent role must be explicitly audited.

**Two-sentence differentiator:** RBAC Authorization is unique in the breadth of access models it must unify — Kubernetes-native RBAC (Role/ClusterRole objects), platform-level RBAC with role inheritance, ABAC conditions for fine-grained attribute matching, and ReBAC for object-level ownership relationships — all evaluated at 500,000 decisions per second with sub-millisecond latency. The Kubernetes sync controller (bidirectional sync between platform RBAC and k8s Role/RoleBinding objects) is the key operational complexity this problem introduces that none of the other four problems have.

---

### Secrets Management System

**The unique architectural element** is dynamic secret generation. Every other system in this pattern manages static credentials — API keys, certificates, and role bindings all have a fixed value that changes only on explicit rotation. Vault's dynamic secret generation creates a new credential on every request, issues it with a TTL, and automatically destroys it when the TTL expires. For MySQL credentials, this means an attacker who compromises a pod's environment variables has a credential that self-destructs — the maximum value of the stolen credential is bounded by the TTL.

The other unique element is **the seal/unseal problem**. Vault must be able to restart without human intervention in a production environment, but restarting means the master key must be recoverable. The HSM-backed auto-unseal solves this: the HSM holds the unseal key and Vault contacts it over PKCS#11 on startup. The fallback (Shamir's Secret Sharing with 3-of-5 key shares) is the break-glass procedure for HSM failure.

**Two-sentence differentiator:** Secrets Management is unique in its Transit encryption-as-a-service capability (applications encrypt data through Vault without ever knowing the encryption key) and its dynamic credential generation (short-lived, auto-revoking credentials that never need manual rotation), both of which require a stateful, HA backend (Vault Raft) rather than the stateless validation path that the other four problems rely on. The seal/unseal mechanism is a Secrets Management-specific operational challenge with no analog in the other four problems.

---

### Zero Trust Network Design

**The unique architectural element** is Cilium's eBPF identity model. Traditional network policies use IP addresses to identify endpoints — `allow 10.0.1.0/24 to port 8080`. In a Kubernetes cluster where pods have ephemeral IPs that change on every restart, IP-based policies are operationally unmaintainable and provide no real security (anyone can spoof an IP). Cilium derives a numeric identity from the pod's immutable labels and enforces that identity at the kernel level using eBPF maps. The policy `allow pods with {app=api-server} to accept connections from pods with {app=frontend}` is enforced at under 0.1ms per packet, survives any IP change, and cannot be bypassed by the application layer.

The other unique element is the **context-aware access proxy for human access**. The other four problems are about machine-to-machine access — services calling services. Zero trust must also solve how human engineers access production infrastructure (kubectl, SSH, database consoles) without a VPN. The BeyondCorp model: device trust verification (is this laptop OS-patched, disk-encrypted, no malware?) + OIDC authentication with MFA + context evaluation (is this engineer on-call? Is it business hours? Is their risk score elevated?) → access decision → session certificate issued for the duration of the access. No persistent VPN tunnels, no static SSH keys.

**Two-sentence differentiator:** Zero Trust Network Design is unique in that it operates at the network layer (packets and connections) rather than the application layer (API calls), using eBPF maps for kernel-level enforcement at 10 million policy evaluations per second — several orders of magnitude higher throughput than any application-layer authorization system. It is also the only problem in this pattern that must handle human access (device trust, OIDC + MFA, context-aware proxy, session recording) in addition to machine-to-machine access.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (2-4 sentences each)

**Q: Why do you store only the SHA-256 hash of an API key and not the key itself?**

**KEY PHRASE: the key is a shared secret — if you store it, you own the risk of your storage being compromised.** If an attacker breaches your database and finds key hashes, they still cannot use them because they need the original key value to authenticate. The plaintext key is shown to the user exactly once at creation and is never stored anywhere in the system. This is the same model used by GitHub (personal access tokens), Stripe (API keys), and Twilio — industry best practice for high-entropy credentials.

**Q: What is the difference between RBAC, ABAC, and ReBAC, and when do you use each?**

**KEY PHRASE: RBAC asks "what role does this identity have?", ABAC asks "what attributes does this request have?", ReBAC asks "what is this identity's relationship to this specific object?"** RBAC is fast (role binding lookup, O(1) with caching) and works well for broad access patterns like "service account job-scheduler can create pods in the batch namespace." ABAC adds conditions like "only in namespaces labeled env=prod" — useful for label-based restrictions that vary per resource. ReBAC is needed when access depends on ownership — "user Alice can edit this specific project" without giving Alice edit access to all projects. Most enterprise platforms need all three.

**Q: Why use short-lived certificates (24-hour TTL) instead of 1-year certificates?**

**KEY PHRASE: short TTL is the most effective revocation mechanism.** If a workload's certificate is compromised, the attacker's window is bounded by the TTL — at most 24 hours before the certificate naturally expires. With 1-year certificates, a compromised certificate is valid for up to 364 more days and requires CRL/OCSP for revocation, which has its own reliability concerns. The trade-off is higher issuance volume, but Vault PKI can handle 5,000 issuances per second — more than enough for 50,000 pods with daily rotation.

**Q: What is the purpose of the offline root CA, and why does it matter?**

**KEY PHRASE: an offline root CA cannot be compromised online because it is never online.** The root CA's only job is to sign intermediate CAs, which happens once a year in a formal ceremony. By keeping the root CA's private key in an air-gapped HSM, you ensure that an attacker who fully compromises your online infrastructure — every Vault node, every Kubernetes cluster, every network device — still cannot forge a root CA signature. The online intermediate CAs can be revoked and replaced if compromised, because the offline root is intact.

**Q: What is Shamir's Secret Sharing and why does Vault use it for the unseal process?**

**KEY PHRASE: Shamir's Secret Sharing splits the master key so no single person can unseal Vault alone.** Vault encrypts all stored data with a 256-bit master key. This master key is split into N shares (typically 5) using Shamir's Secret Sharing — a cryptographic scheme where any threshold T shares (typically 3) can reconstruct the key, but T-1 shares reveal nothing about the key. This means 3 of 5 key custodians must cooperate to unseal Vault, preventing a single compromised or malicious employee from accessing all secrets. In production, auto-unseal via HSM eliminates the human ceremony for routine restarts while preserving the break-glass manual procedure for HSM failures.

**Q: Why does zero trust use "never VPN, always identity proxy" instead of traditional VPN?**

**KEY PHRASE: a VPN grants access to a network segment; an identity proxy grants access to a specific resource for a specific identity at a specific time.** With a VPN, compromising one engineer's laptop gives an attacker access to the entire network segment the VPN terminates on. With an identity-aware access proxy, access to each resource (kubectl, SSH to specific node, database console) is independently gated on identity + device trust + context. Session recording provides forensic evidence. Time-bounded session certificates mean a compromised session token expires quickly. The principle of least privilege is enforced at the connection level, not just the application level.

**Q: What does "fail closed" mean for an authorization system, and when would you fail open?**

**KEY PHRASE: fail closed means "deny on uncertainty"; fail open means "allow on uncertainty" — for security-critical systems, you always fail closed.** If the OPA sidecar crashes, Envoy must reject the request rather than forward it (fail closed). This is the `failure_mode_deny: true` Envoy setting. Failing open "temporarily" to maintain availability is a security anti-pattern — attackers can intentionally overload the authorization service to open the gates. The only exception is for Kubernetes system components (kubelet, scheduler) where Gatekeeper admission webhooks use `failurePolicy: Ignore` — blocking system operations to enforce admission policies would cause a cluster outage, which is worse than briefly allowing a non-compliant resource.

---

### Tier 2 — Deep Dive Questions (with trade-offs)

**Q: How does revocation propagation work when you have 50+ gateway nodes, and what is the worst-case delay?**

**KEY PHRASE: dual-path revocation (Redis pub/sub fast path + Kafka reliable path) with a MySQL periodic sync backstop.** When a key is revoked, the Key Management Service atomically marks it revoked in MySQL and publishes to both Redis pub/sub (sub-second delivery to connected nodes) and Kafka `key.revocations` (guaranteed delivery, under 2 seconds). Gateway nodes listening on Redis pub/sub immediately delete the cached entry and add the key hash to a local in-memory blacklist. Kafka consumers on each gateway node process the same event as a reliability guarantee — if a node was restarted and missed the pub/sub message, it will still get the Kafka message when it reconnects.

The worst case: a gateway node is network-partitioned at the moment of revocation, misses both pub/sub and Kafka, and its Redis cache has a 5-minute TTL. The worst-case delay is 5 minutes. We document this in the revocation SLA. For emergency revocations (leaked key), we set the Redis TTL to 30 seconds and use a bloom filter to prevent re-caching of revoked key hashes.

Trade-offs: ✅ Dual-path gives both speed and reliability. ❌ The dual publication (Redis + Kafka) creates a brief window of inconsistency between the two systems, but since both result in the same action (delete cache entry), the outcome is idempotent. The 60-second MySQL sync is the belt-and-suspenders backstop.

**Q: How would you design the OPA policy evaluation to handle 500,000 authorization decisions per second at under 1 millisecond?**

**KEY PHRASE: embed OPA as a Go library in the authorization sidecar so policy evaluation has zero network hops.** The authorization sidecar runs on each Kubernetes node (or as a process per gateway node). It contains the OPA Go library, a full copy of the policy bundle (roles, bindings, relationship tuples — approximately 300MB of data), and a 100,000-entry LRU cache. Envoy's ext_authz calls into this sidecar process, which does an in-process LRU lookup first (sub-0.1ms), falls back to OPA evaluation on miss (0.5-2ms for Rego evaluation against in-memory data), and falls back to Redis on cold data miss (under 1ms). MySQL is only consulted for data not in the Raft-replicated bundle.

The bundle is updated via Kafka CDC — when a role binding changes in MySQL, the change is published to Kafka, the sidecar's consumer updates the in-memory data, and the relevant LRU entries are invalidated. This gives under 5-second policy propagation with sub-millisecond evaluation latency.

Trade-offs: ✅ In-process evaluation eliminates the entire network latency from the critical path. ❌ Replicating the full policy bundle to 500 nodes means 500 × 300MB = 150GB of replicated policy data across the fleet. This is acceptable but must be measured. Also, partial bundle updates via Kafka CDC are complex to implement correctly — you must handle out-of-order events and ensure consistency of the in-memory policy data structure.

**Q: How would you handle the case where a Vault cluster needs to be restored from backup after a catastrophic failure of all 5 nodes?**

**KEY PHRASE: Raft snapshots plus encrypted backup, verified regularly, with a documented recovery procedure that is tested quarterly.** Vault's Raft integrated storage supports automated snapshots (`vault operator raft snapshot save`). In production, snapshots are taken every 15 minutes and written to encrypted S3 storage. Recovery procedure: provision 5 new nodes, initialize a new Vault cluster from the snapshot, unseal using the HSM (auto-unseal) or Shamir key shares (break-glass), and verify all secret paths, leases, and policies are intact.

The harder problem is that all active Vault tokens and leases are invalidated when restoring from a snapshot taken 15 minutes ago — every Vault Agent sidecar will need to re-authenticate and get new tokens, and all dynamic credentials will need to be re-issued. This causes a 15-30 minute disruption as the fleet reconnects. We mitigate this with a Vault recovery runbook that pre-warms the most critical secret paths after restoration before bringing traffic back.

Trade-offs: ✅ Raft + S3 backups provide a recovery point objective (RPO) of 15 minutes. ❌ Recovery time objective (RTO) for a full cluster failure is 30-60 minutes due to the re-authentication cascade. For most secret paths this is acceptable; for critical infrastructure secrets (database credentials, TLS certificates), we maintain a local Vault Agent cache that buffers for the duration of the outage.

**Q: How does SPIFFE workload attestation work, and how do you prevent a compromised node from impersonating a legitimate workload?**

**KEY PHRASE: SPIRE uses two-factor attestation — the node proves its identity, and the workload proves its identity separately, and neither can fake the other.** Node attestation: the SPIRE Agent on a node presents its Kubernetes Projected Service Account Token (PSAT) to the SPIRE Server. The SPIRE Server verifies this token with the Kubernetes TokenReview API — the token is cryptographically signed by the Kubernetes API server and audience-bound to the SPIRE Server. Only a legitimate Kubernetes node can get a valid PSAT.

Workload attestation: within a node, the SPIRE Agent determines a workload's identity by querying the kubelet API — "what pod is running with this process ID, in which namespace, with which service account?" The answer comes from the Kubernetes control plane, not from the workload itself. A compromised container cannot claim to be running in a different namespace or under a different service account, because that answer comes from the kubelet, not the container.

The combination: even if an attacker compromises one container, they can only get an SVID for that container's identity (namespace X, service account Y). They cannot claim to be any other workload. The SVID is a short-lived X.509 certificate (24-hour TTL), so a compromised SVID expires quickly.

**Q: What is the blast radius if an engineer's laptop is compromised in a zero trust model versus a VPN model?**

**KEY PHRASE: in zero trust, a compromised laptop gives access only to what that engineer was currently authorized for, with a complete audit trail; in a VPN model, it gives access to the entire network segment.** In the zero trust model: the attacker can use the engineer's active session — they have the engineer's OIDC token and device certificate. But the session has a bounded TTL (typically 8 hours). Each access type (kubectl, SSH, database) is gated by the context-aware proxy, which can detect anomalous behavior (unusual time, unusual target, unusual volume) and terminate the session. The attacker has no persistent access — when the session expires, they need to re-authenticate (which requires MFA on the engineer's hardware key, which the attacker may not have).

In the VPN model: the attacker has full access to the internal network segment. They can scan for open ports, attempt lateral movement to any internal service, exfiltrate data from services that do not enforce authentication between internal clients, and do all of this with minimal audit trail (network flows may be logged, but application-layer activity may not be).

**Q: How would you design the audit log system to guarantee zero loss of authorization decisions at 500,000 events per second?**

**KEY PHRASE: make the audit write synchronous from the application's perspective but asynchronous from the database's perspective, with Kafka as the durable write-ahead log.** The application (OPA sidecar, API gateway) writes every decision to Kafka synchronously — the write is considered complete when Kafka acknowledges receipt with `acks=all` (all replicas). This adds approximately 0.5ms to each decision. Kafka then asynchronously delivers to Elasticsearch (hot store) and S3 (cold archive). If Elasticsearch is slow, Kafka buffers events in the `authorization.audit` topic. If Elasticsearch fails entirely, the events remain in Kafka and are replayed when Elasticsearch recovers.

The key invariant: Kafka is the commit point, not Elasticsearch or S3. We never lose an event that Kafka has acknowledged. Kafka's retention for the audit topic is set to 7 days — enough time for Elasticsearch to recover from any failure mode. Elasticsearch uses time-partitioned indices (one per day) to allow efficient archival to S3 after the 90-day hot retention period without expensive DELETE operations.

Trade-offs: ✅ Kafka provides at-least-once delivery with durability. ❌ At 500,000 events per second × 200 bytes = 100MB/sec write throughput on Kafka. This requires a Kafka cluster sized for this throughput (approximately 10-15 brokers with 3x replication = 300MB/sec throughput capacity). The Elasticsearch ingest cluster must also handle this volume — approximately 10 data nodes with 50GB/node/day ingest capacity.

**Q: How would you implement dynamic MySQL credential generation, and what happens when the lease expires?**

**KEY PHRASE: Vault creates a real MySQL user on demand, grants minimum required privileges, and drops that user when the TTL expires — zero manual rotation required.** When an application requests `GET /v1/database/creds/myapp-role`, Vault connects to MySQL using its own vault-admin credentials, generates a random username (`v-k8s-myapp-<8-hex>`) and a 32-character CSPRNG password, executes `CREATE USER` and `GRANT` statements (scoped to the minimum necessary tables and verbs), and returns the credentials to the application. The lease TTL is 1 hour, renewable up to 24 hours.

When the lease expires (or the application explicitly calls `PUT /v1/sys/leases/revoke`), Vault automatically executes `DROP USER` on MySQL. The credential is gone from MySQL — no manual cleanup, no rotation script, no risk of a forgotten service account with stale credentials. This is why dynamic secrets are categorically safer than static passwords: a leaked dynamic credential is useless after the TTL, and you can identify exactly which entity requested it from the audit log.

Trade-offs: ✅ Zero-rotation-overhead, automatically bounded credential lifetime. ❌ If Vault is unavailable when a lease expires, it cannot execute the `DROP USER` — the MySQL user persists until Vault reconnects and processes the expired lease queue. We monitor the lease expiry backlog and alert if it exceeds 5 minutes. We also set a MySQL account expiry (`CREATE USER ... PASSWORD EXPIRE INTERVAL 25 HOUR`) as a defense-in-depth backstop independent of Vault.

---

### Tier 3 — Staff+ Stress Test Questions (reason aloud)

**Q: You are the on-call engineer at 2 AM. GitHub secret scanning has just detected a production API key committed to a public repository 6 hours ago. Walk me through your response.**

**KEY PHRASE: speed of revocation, identification of blast radius, and evidence preservation — in that order.**

First, trigger immediate revocation. The GitHub scanning webhook should have already auto-revoked the key and notified the owner. Verify in the audit log that the revocation actually propagated to all gateway nodes within 5 seconds. If any node still shows the key as active 30 seconds after revocation, that is a critical finding that needs to be diagnosed separately.

Second, assess the blast radius. Query the usage audit log for the 6-hour exposure window: which endpoints were called, from which IP addresses, at what volume, and what resources were accessed. Does the usage pattern match the legitimate service that owned the key, or do you see calls from unexpected IPs, at unusual hours, or accessing resources outside the key's normal scope? A key with read-only access to a specific namespace is much lower severity than a key with cluster-wide admin permissions.

Third, preserve evidence. Snapshot the Elasticsearch audit records for the 6-hour window. Export to immutable S3 before any log rotation could affect them. Create a security incident ticket linking the GitHub commit, the revocation timestamp, and the audit log export.

Fourth, assess secondary compromise. If the usage logs show the key was used by an external attacker, consider: did they use it to read secrets (if so, assume those secrets are compromised and rotate them)? Did they create any resources (pods, jobs, VMs) that are still running? Did they create new credentials that would survive the key revocation?

The meta-lesson I would raise in the interview: the 6-hour exposure window was only possible because the key was never auto-revoked on the initial commit. This should trigger a design review — our GitHub secret scanning webhook was clearly not operating or not connected to the auto-revocation pipeline during those 6 hours.

---

**Q: A senior engineer suggests eliminating the offline root CA because it adds operational complexity — "we have to schedule a ceremony every time we need a new intermediate CA." How do you respond?**

**KEY PHRASE: the operational inconvenience of the offline root CA is exactly proportional to the security it provides — do not optimize away the friction that is protecting you.**

Start by agreeing that the ceremony is operationally expensive. Scheduling 3-5 key custodians, setting up the air-gapped machine, following the procedure correctly, and re-distributing the new intermediate CA — this might take half a day and happen once a year. That is real overhead.

Then explain what an online root CA costs you. If the root CA's private key is stored in Vault (as the engineer is probably proposing), a Vault cluster compromise gives an attacker the ability to generate valid intermediate CAs with arbitrary validity periods and names. They could create an intermediate CA for `corp.com` that is trusted by every certificate chain in the platform, sign arbitrary leaf certificates for any domain, and mount a man-in-the-middle attack against every mTLS connection in the fleet. The blast radius is total: you must regenerate every certificate in the platform, update every trust bundle in every application, and possibly notify users if any user-facing certificates were involved.

Counter the "it's too complex" argument with "we have made it less complex." The ceremony is documented as a runbook, version-controlled, and rehearsed annually. We have tooling to generate the ceremony procedure for the specific intermediate CAs we need to sign. The ceremony happens once a year — that is two hours of operational overhead per year in exchange for protecting the root of trust for the entire platform's PKI. That is a good trade.

If pushed further: "What is the SLA for intermediate CA rotation if a certificate compromise requires it?" The honest answer is that with an offline root CA, it is 4-8 hours (ceremony setup + execution). That is acceptable for a P1 security incident. With an online root CA, it is 5 minutes — but you have accepted the risk that the root itself can be compromised online.

---

**Q: The platform is growing from 500 nodes to 5,000 nodes over the next 18 months. Which components in this security pattern will fail first, and how do you evolve the architecture?**

**KEY PHRASE: identify the components that scale linearly with nodes (bad) versus logarithmically or not at all (good), and fix the linear ones.**

The components that scale linearly with nodes and will fail first:

**RBAC audit log storage** is the first failure. At 500,000 decisions per second now, 5,000 nodes implies 5,000,000 decisions per second at the same per-node rate. That is 5 million events per second × 200 bytes = 1 GB/sec of audit log data. At 90-day hot retention, that is 8 PB in Elasticsearch — untenable. Solution: implement audit log sampling for low-risk, repetitive decisions (e.g., a read-only service making the same GET pods call 10,000 times a minute can be sampled at 1%) while maintaining 100% logging for sensitive operations (write, delete, cluster-admin, any deny).

**SPIRE Server** is the second. A single SPIRE Server cluster handling 5,000 SVIDs per second at 500 nodes becomes 50,000 SVIDs per second at 5,000 nodes during peak pod churn — right at the edge of a single SPIRE Server's capacity. Solution: federate multiple SPIRE Server deployments (one per 1,000 nodes) with cross-trust-domain federation. The SPIFFE federation model supports this natively.

**OPA policy bundle replication** becomes expensive. The full policy bundle replicated to 5,000 nodes at 300MB each = 1.5 TB of replicated data. Each policy change must be delivered to 5,000 consumers within 5 seconds — Kafka can handle this (it is designed for millions of consumers) but the initial bundle load for new nodes must be optimized. Solution: hierarchical bundle distribution (per-cluster bundle caches) and incremental bundle updates via Kafka rather than full-bundle reloads.

**Vault dynamic credential issuance** against a single MySQL cluster. At 5,000 nodes with 50 pods per node, Vault is managing 250,000 pods' worth of MySQL credentials. Dynamic credential creation/revocation against a single MySQL cluster becomes a bottleneck. Solution: partition the Database engine across multiple Vault mount paths, each pointing to a read replica of MySQL (or a separate MySQL cluster per service tier).

The components that do NOT fail: Cilium eBPF (scales within a single node — the 10x more nodes does not increase per-node eBPF load), Redis pub/sub for revocation fanout (10x more consumers on a Kafka topic is trivial for Kafka), cert-manager (scales with Kubernetes API server, not node count directly).

The meta-lesson: security infrastructure tends to have audit and policy data that scales as O(nodes × requests), which grows faster than compute workloads. Plan for this by architecting the audit pipeline to be lossy-by-design for low-risk events and durable for high-risk events.

---

**Q: An engineer proposes using Kubernetes RBAC as the only authorization system for the entire platform, removing the custom RBAC service. Is this a good idea?**

**KEY PHRASE: Kubernetes RBAC is excellent at what it does and terrible at what it does not do — know the boundary.**

Kubernetes RBAC has genuine strengths: it is evaluated in-process in the API server (no network hop), it has a 15-year battle-tested implementation, it integrates with every Kubernetes ecosystem tool natively, and kubectl provides transparent visibility into effective permissions. For Kubernetes resource access, there is no better choice.

The gaps that make it insufficient as the sole authorization system:

First, Kubernetes RBAC has no role hierarchy. You cannot say "namespace-admin inherits all permissions of namespace-reader." You must manually duplicate permissions across roles or manage a combinatorial explosion of role bindings. At 200,000 bindings, manual management is not operationally feasible.

Second, it has no ReBAC. There is no way to say "user Alice can edit this specific project resource (project ID 12345) but no other projects." You would have to create a per-project Role and RoleBinding for every user × project combination — O(users × projects) objects, which becomes millions for a large platform.

Third, Kubernetes RBAC only covers Kubernetes resources. OpenStack Keystone resources (VM creation, network management, storage), job scheduler resources (batch job submission, queue management), and message broker resources (Kafka topic access) are outside the Kubernetes API entirely. You need a unified authorization system that spans all these planes, not separate authorization systems per control plane that cannot share roles or provide unified audit.

Fourth, Kubernetes RBAC's audit log is per-cluster. Cross-cluster audit correlation (this service account made a suspicious sequence of API calls across 5 clusters) requires extracting kube-apiserver audit logs from each cluster and correlating them externally — no first-class support.

My recommendation: keep Kubernetes RBAC for Kubernetes resource access (it is best-in-class for that), sync platform RBAC bindings to k8s Role/RoleBinding objects via a controller, and use OPA/Gatekeeper for admission-time policy enforcement that RBAC cannot express. Remove the custom RBAC service only if the platform's authorization needs fit entirely within Kubernetes resources.

---

## STEP 7 — MNEMONICS

### The Five-System Memory Hook: **"ACRES"**

- **A**PI Key Management — programmatic access with scoped permissions
- **C**ertificate Lifecycle Management — PKI + mTLS + SPIFFE workload identity
- **R**BAC Authorization — who can do what, at sub-millisecond speed
- **E**ncryption + Secrets Management (Vault) — store and distribute all credentials
- **S**ecurity Network (Zero Trust) — no implicit trust, identity on every packet

### The Validation Path Mnemonic: **"C-R-M-A-K"**

Every access decision flows through these five layers. Memorize the order:
- **C** — Check the in-process **C**ache (LRU / OPA memory / Vault Agent)
- **R** — **R**edis cluster if cache miss (sub-1ms distributed lookup)
- **M** — **M**ySQL authoritative store if Redis miss (under 5ms cold path)
- **A** — **A**udit the decision (Kafka → Elasticsearch → S3, async, zero loss)
- **K** — **K**afka for propagating changes back (CDC, policy updates, revocations)

### Opening one-liner for any question in this pattern:

"Before I draw anything, I want to establish that every problem in this domain requires answering three questions in under a millisecond: is this identity real, is it authorized for this action, and has it been revoked since I last checked — and the architecture is entirely about serving those three answers at scale without querying a database on every request."

---

## STEP 8 — CRITIQUE

### Well-Covered Areas

The source material is exceptionally strong in the following areas:

**Concrete NFRs with derivation.** The latency targets (under 1ms cached, under 5ms cold, under 0.1ms eBPF), throughput targets (100K, 500K, 10M evaluations per second), and propagation SLAs (under 5 seconds) are consistently applied across all five problems and have clear justifications. This is more rigorous than most real interview guides.

**Data model depth.** Full SQL schemas for each problem, including indexing strategy and partitioning rationale, are provided. This is valuable because interviewers at Staff+ level frequently probe the data model specifically.

**Failure mode coverage.** The source material covers the interesting failure modes (Redis unavailable, Kafka consumer lag, CA key compromise, network partition) with specific handling strategies per component. Most interview guides stop at "it's eventually consistent" without explaining the specific degraded behavior.

**Cross-problem integration.** The explicit mapping of how all five problems connect to each other — SPIFFE is used by zero trust and certificate management; Vault is used by secrets and certificate management; OPA appears in RBAC and zero trust — is well-documented and rare in standalone problem guides.

### Missing or Shallow Coverage

**Compliance and regulatory requirements** are mentioned briefly in the context of 7-year audit retention, but there is no coverage of SOC2, PCI-DSS, or HIPAA-specific requirements that might drive design choices (e.g., PCI-DSS requires 1-year audit retention of authentication events, HIPAA requires encryption at rest for all PHI-adjacent data). An interviewer from a financial services or healthcare background will probe this.

**Multi-tenancy isolation** is largely absent. In a platform with multiple internal teams (or external customers in a SaaS context), how do you ensure that Team A's secrets cannot be read by Team B's service account? The source material covers namespace-scoped RBAC but does not discuss Vault namespace isolation (Vault's enterprise feature), Kubernetes network policy boundaries between tenant namespaces, or the audit requirements for cross-tenant access attempts.

**Certificate transparency (CT) logs** for public-facing certificates are not mentioned. Let's Encrypt and all major CAs submit certificates to Certificate Transparency logs. An interviewer working on public-facing infrastructure may ask whether you monitor CT logs for unauthorized certificate issuance for your domains.

**The Kubernetes API server as a security boundary** is underexplored. The kube-apiserver itself is an attack target — authenticated API calls can exfiltrate cluster configuration, secret references, and workload specifications. The source material covers how to authorize calls to the API server but not how to secure the API server itself (audit logging, anonymous auth disabled, insecure port disabled, RBAC-only authorization mode).

**Secret sprawl and orphaned secrets** are real operational problems not covered. What happens when a team creates 50 Vault paths for a project that is subsequently cancelled? How do you audit and clean up unused secret paths? How do you detect a Vault path that has not been accessed in 90 days and might be a forgotten credential?

### Senior Probes to Anticipate

Interviewers at the L6/Staff+ level specifically probe:

**"How do you debug an unexpected deny in production without compromising security?"** The answer requires: OPA decision traces accessible via a debug endpoint (with audit), the `kubectl auth can-i` equivalent for the platform RBAC, and an `explain=true` flag on the authorization API that returns the full evaluation path without logging sensitive request data.

**"What is your rollout strategy for a new OPA policy that might break existing workflows?"** This requires: dry-run mode (evaluate but do not enforce), canary deployment (1% of nodes enforce the new policy, 99% get audit-only mode), metric diff (deny rate before vs. after new policy), and a rollback mechanism (revert to previous policy bundle version in under 60 seconds).

**"How do you handle the cold start problem when a new pod starts — it needs its Vault token to get its secrets, but it needs its secrets to start?"** This is the bootstrapping problem. Answer: SPIFFE/SPIRE handles this — the pod gets its X.509 SVID from the SPIRE Agent on the same node before the pod's init containers run. The SVID is the Vault authentication credential (Kubernetes auth method verifies the SVID). The SVID is available from the moment the pod is scheduled, before the pod's containers start. There is no circular dependency.

**"How would you detect a service account that has been granted too many permissions over time due to accumulated role bindings?"** This requires: a periodic job that computes "effective permissions" for every service account and compares it against "actually used permissions" (from the audit log over 90 days). Any permission that has never been used in 90 days is flagged for removal. This is the "permission drift" problem that most authorization systems accumulate without active governance.

### Common Traps

**Trap 1: Proposing bcrypt for API key hashing.** Bcrypt is correct for passwords (low entropy, brute-force viable). API keys have 238 bits of entropy — brute-force is impossible regardless of hash function. bcrypt adds 100ms per hash, which is physically impossible to compute at 100,000 validations per second. SHA-256 is the correct choice for high-entropy credentials on the hot path.

**Trap 2: Using only Redis TTL for revocation (no event-driven invalidation).** A 5-minute Redis TTL means a revoked key works for up to 5 minutes after revocation. For routine rotation this is acceptable, but for a leaked key this is unacceptable. The correct design is TTL as a backstop plus event-driven immediate invalidation via Redis pub/sub and Kafka.

**Trap 3: Putting the authorization service on a synchronous remote call path for every request.** Even at 1ms per call, adding a synchronous authorization service call to every API request adds 1ms of latency uniformly. At high RPS, this creates a single point of contention. The correct approach is to push evaluation to the edge (in-process OPA on each node) and use the remote service only for cache misses and policy updates.

**Trap 4: Forgetting that deny decisions must also be audited.** Most candidates remember to audit successful accesses. Audit logs of denials are often more valuable for security monitoring — an unusual pattern of denials can indicate a probing attack or a misconfigured service before it becomes an incident.

**Trap 5: Proposing a single root CA that is also the issuing CA.** Skipping the intermediate CA layer to simplify operations is a common suggestion. The cost is that the root CA must be online to sign leaf certificates, which means it can be compromised. This is a well-known PKI anti-pattern. Push back hard: the root CA must be offline, period.

**Trap 6: Assuming RBAC is sufficient without ReBAC for object-level access.** RBAC can express "Alice has edit access to all projects." It cannot express "Alice has edit access to only project ID 12345." If the interviewer asks about multi-tenancy or ownership-based access, the answer requires relationship-based access control (Zanzibar-style tuples). Missing this shows a gap in authorization system design knowledge.

---

*End of Infra-12 Security & Access Interview Guide*
