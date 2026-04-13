# Pattern 16: Auth & Security — Interview Guide

Reading Pattern 16: Auth & Security — 4 problems, 8 shared components

---

## STEP 1 — PATTERN OVERVIEW

Auth & Security covers four closely related but distinct system design problems that come up constantly at FAANG and FAANG-adjacent companies. They share a common philosophy but each has a sharp differentiator that interviewers are specifically probing for.

**The four problems:**

1. **Auth Service** — The core identity backbone. Handles email/password login, MFA (TOTP + WebAuthn), refresh token rotation with theft detection, and session management. The heart of "who are you and prove it."

2. **OAuth2 Provider (Authorization Server)** — The delegation layer. Lets users grant third-party apps access to their data without giving those apps their password. Runs Authorization Code + PKCE, Client Credentials, Device Flow, and OpenID Connect.

3. **Single Sign-On (SSO) Service** — The enterprise bridge. Accepts SAML 2.0 assertions and OIDC tokens from external corporate identity providers (Okta, Azure AD, Ping Identity) and translates them into platform sessions. The only one of the four that talks to systems you don't control.

4. **API Key Management** — The machine credential layer. Issues, validates, rotates, and revokes long-lived developer credentials. The distinguishing concern is sub-millisecond validation at high throughput and immediate revocation propagation.

**Why these four are grouped together:** They all solve the same meta-problem — "how does a system know who is making a request and what they're allowed to do?" — but through different trust models. Auth Service handles direct human trust (you know your password). OAuth2 handles delegated trust (you authorized this app). SSO handles federated trust (your employer's IdP vouches for you). API keys handle long-lived machine trust (you hold this secret credential).

**What interviewers are really testing:** Your ability to separate authentication from authorization, your fluency with token lifecycle (issuance, validation, rotation, revocation), and your instinct for the security properties that make each approach safe at scale.

---

## STEP 2 — MENTAL MODEL

**Core idea:** Every auth system is fundamentally a factory that issues proof-of-identity artifacts and a verifier that checks those artifacts — the entire engineering challenge is making the factory unforgeable, making the verifier stateless and fast, and making revocation immediate without breaking the stateless property.

**Real-world analogy:** Think of a concert venue. The ticket office (Auth Service) verifies your ID and issues a wristband. The wristband is checked at the door (JWT validation) — the bouncer doesn't need to call the ticket office for every person, they just look at the wristband (stateless). But if someone reports a stolen wristband, the venue needs a way to invalidate it fast (revocation). The twist: wristbands expire in 15 minutes (access token TTL), and you can get a new one from a special exchange booth by showing a different longer-lived pass (refresh token). If someone steals your long-lived pass and uses it, you both show up at the exchange booth — that's the reuse detection moment.

OAuth2 is the same venue but now you're letting a friend get a wristband on your behalf — you authorized them with a specific scope (they can only enter the main hall, not backstage). SSO is when your corporate badge (from a completely different company's system) is accepted at the door — the venue trusts your employer's assertion. API keys are like a permanent backstage pass that machines hold — they never expire unless revoked, and the security question becomes "what happens if that pass ends up on GitHub."

**Why it's genuinely hard:**

First, there is a fundamental tension between statelessness and revocation. JWTs are stateless — any service can verify them with a public key, no network call needed. But statelessness means you can't "take back" a JWT once issued without introducing state (a blocklist), which partially defeats the purpose. Short TTLs (15 minutes) are the primary mitigation, but enterprise customers ask for longer sessions. Getting this balance right under scale is non-trivial.

Second, security is asymmetric. The defender has to get every property right simultaneously (hashing, entropy, timing, rotation, revocation, audit). The attacker only needs to find one gap. The systems files include things like dummy Argon2id computation to prevent timing-based user enumeration — most candidates forget this and interviewers at senior levels specifically probe it.

Third, token theft is a latent failure. Unlike a service outage (immediately obvious), a stolen refresh token silently drains access for 90 days. The theft may have happened weeks ago. Family-based reuse detection is the mechanism that makes the failure visible — but only when the attacker and legitimate user both try to use the token, which requires understanding the token state machine deeply.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these before drawing anything. They meaningfully change the design.

**Question 1: Is this a first-party auth system (for your own users) or a third-party auth provider (for developers' apps)?**
What changes: First-party means you own the user database, you do password hashing, you issue short-lived JWTs. Third-party means you are an OAuth2 Provider — you delegate user authentication to an Auth Service and focus on the delegation protocol, scopes, and consent.

**Question 2: What client types need to be supported — browsers, mobile apps, server-side apps, IoT/CLI devices?**
What changes: Browser clients need HttpOnly cookies for refresh tokens (localStorage is XSS-vulnerable). Mobile apps need PKCE (no server-side client secret is safe on a mobile device). IoT/CLI devices without a browser need Device Authorization Flow (RFC 8628). Getting this wrong means recommending implicit flow (deprecated, insecure) or missing PKCE entirely.

**Question 3: Do enterprise customers need SSO with their corporate identity provider?**
What changes: If yes, you need SAML 2.0 and/or OIDC federation, JIT provisioning, attribute mapping, and Single Logout. This is an entirely separate service concern from your core auth system. Without this question, a candidate designing "auth" for an enterprise SaaS product will miss 40% of the scope.

**Question 4: What are the scale expectations — DAU, peak logins per second, and what's the read/write ratio for token validation vs. token issuance?**
What changes: Token validation (JWT verify) is 10-100x higher volume than token issuance but is stateless and CPU-bound. The auth service itself handles only issuance/refresh (~6K RPS in the example). The validation path is completely separate architecture. Conflating them leads to drastically over-engineering the auth service.

**Question 5: What are the compliance and audit requirements — SOC 2, HIPAA, PCI, 7-year log retention?**
What changes: Determines whether you need immutable audit logs (append-only Kafka → S3 Parquet), HSM-based key storage (AWS KMS / HashiCorp Vault rather than application secrets), and specific password policy constraints (FIPS-compliant algorithms like PBKDF2 instead of Argon2id in some government contexts).

**Red flags in requirements:** Watch for "we need to be able to retrieve a user's password for support purposes" — that means they want reversible encryption, which is never acceptable. Also watch for "users log in with phone number + SMS OTP" as primary factor without MFA — SMS OTP alone is not MFA, it's a single factor delivered over an insecure channel. Gently correct both.

---

### 3b. Functional Requirements

**Core (non-negotiable):**
- User registration with email/password; password stored as Argon2id hash with HSM pepper; never plaintext or reversible.
- Login: verify credentials, issue short-lived JWT access token (15-minute TTL) and long-lived opaque refresh token (90-day TTL).
- Refresh token rotation: each use issues a new refresh token and invalidates the old one; reuse of an invalidated token (theft signal) revokes the entire token family.
- Token revocation: explicit logout revokes the refresh token family; admin-initiated suspension blocks all token validation.
- MFA: TOTP (RFC 6238) via authenticator apps and WebAuthn (FIDO2) hardware/biometric keys.
- Account lockout after configurable failed attempts; rate limiting per IP and per account; CAPTCHA at threshold.
- Password reset via time-limited, single-use, cryptographically random token sent to verified email.
- Audit log: every auth event recorded with timestamp, IP, user agent.

**Scope boundary (say this explicitly in interview):** OAuth2 delegation flows, SAML/SSO federation, and API key issuance are separate systems. This service handles direct human authentication only.

**Clear statement for the board:** "I'm designing an authentication service that creates and manages user identities and issues short-lived cryptographic credentials (JWTs) that downstream services can verify without calling back to this service. All long-lived secrets are stored hashed or encrypted; no raw credentials ever touch the database."

---

### 3c. Non-Functional Requirements

**Latency:** p99 login < 300 ms. The 200 ms budget for Argon2id is intentional — it's the cost of memory-hard password hashing. p99 token refresh < 20 ms (Redis read + JWT sign). p99 JWT validation < 1 ms (in-process JWKS cache + RSA verify, no network call).

**Availability:** 99.99% uptime (< 52 min/year). Auth is a hard dependency — if auth is down, users can't log in to anything.

**Throughput:** ~6K RPS for login + refresh at the auth service level. 100K+ RPS for JWT validation — but this is handled stateless at the API gateway, not by the auth service.

**Trade-offs to state upfront:**
- ✅ Short JWT TTL (15 min) means fast revocation propagation but forces clients to refresh frequently.
- ❌ Short JWT TTL means more load on the refresh endpoint; mitigated by the fact that refresh is cheap (Redis lookup + sign, ~5 ms).
- ✅ Memory-hard password hashing (Argon2id, 200 ms) makes offline cracking economically infeasible.
- ❌ Argon2id at 200 ms × 1,447 peak login RPS = 37 auth pods just for hashing compute. This is the primary scaling driver.
- ✅ Refresh token reuse detection catches token theft.
- ❌ A legitimate client with a dropped network connection retrying the refresh will look like a reuse event. Mitigate with a short grace window or by including the new token in the error response.

**Security-specific NFR (often forgotten):** OWASP ASVS Level 2 compliance. SOC 2 Type II scope. Secrets in HSM/KMS. This matters because it drives the architecture decision to never store raw tokens and to use KMS for the signing key.

---

### 3d. Capacity Estimation

**Formula approach — always do this in order: users → events → RPS → storage → bandwidth.**

**Anchor numbers (memorize these):**
- 500M registered users, 10% DAU = 50M active users/day
- 50% of active users log in fresh (new login) = 25M logins/day; the rest use refresh tokens
- 20 API calls per active user per day = 1B JWT validations/day
- Peak multiplier: 5x for login (morning rush), 10x for validation

**RPS calculations:**
| Event | Daily | Peak Multiplier | Peak RPS |
|---|---|---|---|
| Login (Argon2id verify) | 25M | 5x | ~1,450 RPS |
| Token Refresh | 75M | 5x | ~4,340 RPS |
| JWT Validation (gateway) | 1B | 10x | ~115,000 RPS |

**Architecture implication (say this explicitly):** JWT validation at 115K RPS is completely stateless — handled in-process at the API gateway using a cached RSA public key. The auth service itself only handles 6K RPS. These are different scaling problems. The auth service scales for compute (Argon2id) not for throughput.

**Storage:**
- User credentials: 500M × 512 bytes = 256 GB (fits on a single large PostgreSQL instance)
- Active refresh tokens: ~100M × 256 bytes = 25.6 GB
- Audit log hot (90 days): ~4.6 TB; cold (7 years): ~65 TB

**Time budget for Argon2id:** 1,447 login RPS × 200 ms/hash = requires 290 concurrent Argon2id computations. At 4 cores/pod, that's ~73 pods. Mention this — interviewers at Staff+ levels specifically probe whether you know the compute is the constraint, not the DB.

---

### 3e. High-Level Design

**Draw in this order on the whiteboard:**

1. **Client** (browser/mobile) on the left.
2. **API Gateway / Load Balancer** — TLS termination, IP rate limiting, JWT validation using JWKS cache for all non-auth endpoints.
3. **Auth Service (stateless pods)** — receives only `/auth/*` routes. Horizontally scalable. All logic here: login handler, refresh handler, MFA handler, revocation handler.
4. **Backing stores** (right side of board): User Store (PostgreSQL), Refresh Token Store (Redis + PostgreSQL), MFA Store (PostgreSQL), Rate Limiter (Redis).
5. **JWKS Endpoint** served from CDN — public key only, no secrets.
6. **KMS / HSM** — RSA signing private key, Argon2id pepper. Never in application memory long-term.
7. **Audit Log** — Kafka → ClickHouse (hot) + S3 Parquet (cold).

**Data flow for login with MFA (narrate while drawing arrows):**
1. Client POSTs credentials to API Gateway.
2. Gateway does IP rate limit check in Redis; if exceeded → 429.
3. Auth Service: look up user by normalized email in PostgreSQL.
4. If user not found: run dummy Argon2id anyway to prevent timing enumeration, return 401.
5. Check account lock state. If locked → 423 with retry-after.
6. Argon2id verify (fetches pepper from KMS, ~200 ms).
7. Wrong password: increment `failed_attempts` in Redis (atomic INCR with TTL). If threshold → lock account.
8. Correct password, MFA enrolled: return `{mfa_required: true, mfa_session_token: <5-min signed JWT>}`.
9. Client POSTs TOTP code + mfa_session_token.
10. Auth Service decrypts TOTP secret from DB (AES-256-GCM, key in KMS), verifies code, checks Redis replay cache.
11. Success: issue RS256 JWT (15-min TTL) + opaque refresh token (CSPRNG 32 bytes, stored as SHA-256 hash in Redis + PostgreSQL). Set refresh token in HttpOnly Secure SameSite=Strict cookie.

**Key decisions to state proactively (before interviewer asks):**
- "I'm using RS256 not HS256 because resource servers can verify with the public key without sharing a secret."
- "Refresh tokens are stored as SHA-256 hashes — a DB breach exposes only irreversible digests."
- "JWKS is served from CDN with max-age=3600 so JWT validation never hits the auth service."

---

### 3f. Deep Dive Areas

**Deep Dive 1: Refresh Token Rotation with Reuse Detection**

This is the most frequently probed auth deep dive at FAANG.

The problem: refresh tokens are long-lived (90 days). If stolen (XSS from localStorage, malware, MitM), an attacker silently maintains access by refreshing. Rotation alone (issue new token on each use) only partially helps — if the attacker refreshes first, the legitimate user silently loses their session without knowing why.

The solution — token families:
- Every refresh token belongs to a family (a UUID).
- On each use: mark old token `used_at`, issue new token with same `family_id`.
- If a token with `used_at IS NOT NULL` is presented again: **REUSE DETECTED**. Revoke all tokens in the entire family. Force re-login. Send security alert.

The critical implementation detail: the rotation must happen inside a `SERIALIZABLE` transaction with `SELECT ... FOR UPDATE` on the refresh token row. Without this, two concurrent legitimate refresh requests (e.g., mobile app + web tab both refreshing simultaneously) can trigger a false reuse detection. The serializable transaction ensures only one succeeds; the other gets a clean "token already rotated" error.

**Trade-offs to state unprompted:**
- ✅ Detects token theft; limits blast radius to one family.
- ❌ Legitimate user loses session on detection (they have to re-login). Accepted trade-off for security.
- ✅ No client changes required — works with standard OAuth2/cookie patterns.
- ❌ Network drop after server issues new token but before client receives it causes false positive. Mitigation: 5-second grace window where the old token's reuse is tolerated if the new token has not been used yet.

**Deep Dive 2: Stateless JWT Validation vs. Immediate Revocation**

The fundamental tension: JWTs are stateless (fast, no DB call) but can't be instantly revoked.

Three approaches:
1. **Short TTL (15 minutes)**: revocation latency bounded by TTL. Simple. ✅ No state needed. ❌ Refresh churn every 15 minutes.
2. **JTI blocklist in Redis**: maintain a set of revoked JWT IDs with TTL = remaining token lifetime. At 50K active revoked tokens × 64 bytes = 3.2 MB in Redis — negligible. ✅ Immediate revocation. ❌ Every token validation needs a Redis check (adds ~1 ms).
3. **Generation numbers**: per-user counter in Redis. Include generation in JWT. Increment to invalidate all tokens for a user at once. ✅ Immediate full revocation for security incidents. ❌ Cannot selectively revoke one session; all sessions revoked.

Production answer: use short TTL as the primary mechanism plus JTI blocklist for security-critical revocations (account compromise, admin suspension). Generation numbers for mass "revoke all sessions" operations (password change, suspected breach).

**Deep Dive 3: Argon2id with HSM Pepper**

Why Argon2id and not bcrypt? bcrypt uses a fixed 4 KB of memory regardless of cost factor — a modern GPU can run 1,000+ bcrypt verifications per second in parallel. Argon2id with m=19,456 KiB requires ~19 MB per computation. A GPU with 4 GB VRAM can only run ~200 concurrent Argon2id computations, not millions. The attacker's cracking speed drops from thousands to tens per second per GPU.

Why pepper? The pepper is a 32-byte secret stored in HSM/KMS, concatenated with the password before hashing. If only the database is exfiltrated (SQL injection, insider threat), the attacker can't crack passwords because they don't know the pepper. A full compromise (DB + application secrets) is required. The pepper is versioned so it can be rotated without forcing all users to reset passwords — rotate on next successful login.

What if KMS is down? The pepper can be cached in application memory with a short TTL (5 minutes). KMS is a transient outage risk, not a constant dependency. The security trade-off: pepper is hidden from the database but visible to the running application — in-memory cache doesn't materially weaken that property.

---

### 3g. Failure Scenarios

**How seniors frame failure scenarios:** Don't just list what breaks. For each failure, state: what the user experiences, how you detect it, what the auto-remediation is, and what the residual risk is.

| Failure | User Experience | Detection | Auto-Remediation | Residual Risk |
|---|---|---|---|---|
| Auth Service pod crash | In-flight requests fail; next request goes to healthy pod | LB health check (15s) | K8s auto-restart; LB routes around | Brief 503 for in-flight requests |
| PostgreSQL primary failure | Login fails for ~30s | Patroni health check (<10s) | Patroni promotes standby (<30s failover) | Refresh tokens in PostgreSQL: if Redis evicted them, cache miss → PostgreSQL → during failover, refresh fails |
| Redis cluster node failure | Rate limits disabled; token validation slower | Redis Cluster health check | Auto-promote replica; fallback to PostgreSQL for token validation | Brief rate limit bypass window; attackers aware of this can exploit it |
| KMS unavailability | Cannot verify passwords (no pepper) | HTTP 503 from KMS | Use cached pepper (5-min TTL); circuit break after 30s | 5-min window where logins still work on cache; after that, fail closed (503) |
| JWKS endpoint unavailable | New API gateway instances can't fetch public key | Synthetic monitor on JWKS URL | CDN serves cached JWKS; existing in-process caches valid for up to 1 hour | Cold-start gateway instances (fresh deploys) fail to validate until CDN serves cached version |
| Refresh token reuse detection false positive | User gets logged out unexpectedly (re-login required) | Spike in `auth_reuse_detection_total` metric | Security alert sent; user re-logs in | Poor UX; may indicate real attack, may be network issue — audit log required to distinguish |
| Account lockout storm (DDoS) | Legitimate users locked out en masse | Spike in 423 responses | Require CAPTCHA instead of lockout if >1000 distinct accounts locked in 1 minute | DDoS achieves its goal (disruption) but cannot gain access; notify ops immediately |
| Clock skew on TOTP | TOTP codes rejected even though entered correctly | Increase in TOTP failure rate | NTP sync alert; ±1 window handles up to 30s drift | Users get locked out if skew exceeds 90s; requires NTP enforcement on all nodes |

**Senior framing to use verbally:** "My general posture here is fail secure but fail gracefully. For the password verification path, if KMS is down and the pepper cache expires, I prefer 503 over accepting unchecked logins — but I give a 5-minute cache grace to absorb transient hiccups. For audit logging, I do the opposite — I fail open, buffering events in memory, because losing a login event is better than blocking a login."

---

## STEP 4 — COMMON COMPONENTS

Every component below appears across multiple problems in this pattern. Know why each is chosen, its key configuration, and what breaks without it.

---

### Stateless API Layer + Durable Backend

**Why used:** Separating compute from state enables horizontal scaling. When auth pods are stateless, you can add and remove them freely under load without sticky sessions or migration concerns. All persistent state lives in backing stores that are independently scaled and replicated.

**Key config:** Auth Service pods have no local state. They read from Redis (hot path) and PostgreSQL (durable). Kubernetes HPA scales on CPU > 70% or custom metric (login RPS).

**Without it:** You'd need sticky sessions, which means losing a pod loses active sessions. You can't scale or deploy without downtime windows.

---

### CSPRNG Token Generation (32-byte OS randomness)

**Why used:** Tokens must be unpredictable. Predictable tokens (sequential IDs, weak PRNGs) can be enumerated or guessed by attackers. The OS CSPRNG (/dev/urandom, getrandom()) is seeded from hardware entropy and produces outputs that are computationally indistinguishable from random.

**Key config:** `CSPRNG(32 bytes) → base64url encode → 43-character string`. 32 bytes = 256 bits of entropy. With SHA-256 lookup speed of ~10^9/s, brute-force takes 10^68 years. This is why SHA-256 is safe for token storage (but NOT for passwords, which have only ~28 bits of entropy).

**Without it:** Tokens generated from weak PRNGs (RAND(), UUID v1 which encodes MAC + timestamp) have predictable structure. Attackers can enumerate valid tokens.

---

### SHA-256 Hashing for Storage (Never Store Raw Tokens)

**Why used:** If the database is compromised, stored SHA-256 hashes of tokens cannot be reversed. An attacker with the hash database cannot reconstruct the original token (unlike a password hash, the token has 256 bits of entropy, so SHA-256 is one-way in practice even though it's a fast hash).

**Key config:** `token_hash = SHA256(raw_token)` stored in database. Raw token only in HTTPS transport and client memory. Lookup: hash the incoming token, query by hash. Important: include the full token including prefix in the hash to prevent prefix-stripping attacks.

**Without it:** A single SQL injection or DBA access exposes all active tokens. Every user's session is compromised in one breach.

---

### Redis for Hot-Path Token/Key Validation

**Why used:** PostgreSQL cannot serve 100K+ reads/second at sub-millisecond latency for the token validation hot path. Redis GET by key is O(1) and sub-millisecond. It also natively supports TTL (tokens expire automatically) and atomic operations (INCR for rate limiting, GETDEL for single-use code enforcement).

**Key config:** Write-through pattern: every token issuance writes to both Redis and PostgreSQL simultaneously. Redis is the read path; PostgreSQL is the durable source of truth. On Redis eviction (memory pressure): fall back to PostgreSQL lookup and re-warm Redis. Redis Cluster for fault tolerance, not necessarily for capacity (the active token set is often only tens of GBs).

**Without it:** Every API call that requires token validation hits PostgreSQL. At 100K+ RPS this destroys your database.

---

### KMS / HSM for Long-Term Secret Storage

**Why used:** Secrets stored in application code or environment variables are one `git log` or `env dump` away from exposure. KMS (AWS KMS, HashiCorp Vault) provides hardware-backed secret storage, audit logging of every key use, automatic rotation, and key access control policies. The RSA private signing key for JWTs, the Argon2id pepper, and TOTP encryption keys (AES-256-GCM data keys) all live here.

**Key config:** The private signing key is ideally never loaded into application memory — signing is a KMS API call. In practice, for performance, the key can be fetched and cached in-process for short periods. Pepper: cached in application memory with a short TTL to tolerate transient KMS outages. TOTP secrets: encrypted with a KMS data key using AES-256-GCM envelope encryption.

**Without it:** A single application memory dump, heap snapshot, or environment variable leak exposes the signing key (compromises all JWTs), the pepper (compromises all password hashes), and all TOTP secrets (compromises all MFA enrollments simultaneously).

---

### JWT RS256 with JWKS for Stateless Verification

**Why used:** RS256 (RSA asymmetric signing) means only the auth server has the private key (signs tokens) while any resource server has the public key (verifies tokens). This decouples verification from the auth server — resource servers can validate tokens locally with no network call. JWKS (JSON Web Key Sets) is the standard format for publishing public keys with key IDs (`kid`) to support key rotation.

**Key config:** Access token TTL: 15 minutes (auth service), 1 hour (OAuth2 provider — resource servers cache introspection results). JWKS served from CDN with `Cache-Control: max-age=3600`. Key rotation: publish new key alongside old key, wait for old tokens to expire (one TTL), then start signing with new key. Remove old key after another TTL cycle so in-process caches drain.

**Without it:** Either every API call needs a network round-trip to the auth server for validation (bottleneck at 100K+ RPS), or you use HS256 (symmetric) where every service needing validation must hold the signing secret (massive key distribution problem).

---

### Refresh Token Rotation with Family-Based Reuse Detection

**Why used:** Long-lived tokens (90-day refresh tokens) are a liability if stolen. Rotation makes each token single-use — the stolen token becomes invalid after either the attacker or the legitimate user uses it. Family tracking detects when both parties try to use the token (the stolen-one-used-then-legitimate-one-also-used pattern) and revokes the entire chain.

**Key config:** `refresh_token_families (family_id, user_id, invalidated_at)` + `refresh_tokens (token_hash, family_id, used_at, replaced_by, revoked_at)`. Token exchange runs in a `SERIALIZABLE` PostgreSQL transaction with `SELECT ... FOR UPDATE` on the token row. If `used_at IS NOT NULL` on the presented token → reuse detected → `UPDATE refresh_token_families SET invalidated_at = NOW()` → revoke entire family.

**Without it:** A stolen refresh token is valid for 90 days with no detection mechanism. The user has no way of knowing their session has been hijacked.

---

### Audit Log via Kafka → ClickHouse + S3

**Why used:** Auth events are the most security-sensitive log stream in the system. They need to be: (1) non-blocking (auth should never fail because the audit log is slow), (2) durable (no events dropped even during downstream outages), (3) queryable fast (incident response needs instant lookups), and (4) retained long-term (7-year compliance requirement). Kafka provides the durable non-blocking buffer. ClickHouse provides fast analytical queries on hot data. S3 Parquet provides cheap immutable cold retention.

**Key config:** Auth service emits structured JSON events to Kafka (fire-and-forget, async). Kafka replication factor = 3, `min.insync.replicas = 2`. Kafka consumer writes to ClickHouse (90-day hot) and S3 (7-year cold). Every event includes: `actor_id, actor_ip, target_entity_id, event_type, timestamp, outcome`.

**Without it:** Either audit logging blocks auth (catastrophic for availability), or you lose events under load, which is a compliance violation and destroys your incident response capability.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### Auth Service

**What makes it unique:**
- The only problem that does password hashing. Argon2id with m=19,456 KiB, t=2, p=1 is the OWASP-recommended algorithm. The 200 ms hash time is intentional — it's the defense against offline GPU cracking.
- The HSM pepper is a second layer: even a full database dump is insufficient for offline cracking without the application secret.
- TOTP secrets encrypted with AES-256-GCM (KMS data key) in the database — a DB breach alone doesn't compromise MFA.
- WebAuthn/FIDO2 is the only phishing-resistant MFA — the authenticator signs the RP origin, so a phishing domain's stolen assertion is cryptographically useless.
- The dummy hash guard (running Argon2id even for non-existent users) is the kind of detail most candidates forget; interviewers specifically probe for it.
- MFA session token (short-lived 5-minute JWT between password verify and TOTP verify) is an important state management detail.

**Different decisions vs. the others:**
- Uses Argon2id for password storage (the only problem that needs slow/memory-hard hashing).
- Refresh token TTL is 90 days (vs. 30 days in OAuth2 provider); longer because users expect persistent sessions on their own apps.
- No consent screen — this service owns the user relationship directly.

**Two-sentence differentiator:** Auth Service is the only problem in this group that owns user credentials directly and bears responsibility for password storage security. Its uniqueness is Argon2id with HSM pepper (defeating GPU-cracking even after DB breach) combined with FIDO2 WebAuthn MFA (defeating phishing attacks) and TOTP replay prevention (Redis TTL on used code hashes).

---

### OAuth2 Provider (Authorization Server)

**What makes it unique:**
- The only problem with a **consent screen** and the concept of "delegated authorization" — users grant third-party apps access to their data within specific scopes.
- **PKCE is mandatory** for all public clients (mobile, SPA). PKCE prevents authorization code interception attacks where a malicious app on the same device intercepts the redirect URI. The code verifier is generated on-device and never transmitted until the token exchange — even if the authorization code is intercepted, it's useless without the verifier.
- **Authorization code must be single-use** — implemented via Redis `GETDEL` (atomic get + delete), ensuring no two concurrent requests can exchange the same code.
- **Device Authorization Flow (RFC 8628)** — for TVs and CLIs that can't open a browser. The device gets a `user_code` (8 characters, human-typeable like "WDJB-MJHT"), the user approves on a separate browser, and the device polls until approved. The polling interval enforcement (`slow_down` error) prevents abuse.
- **Token introspection (RFC 7662)** — resource servers validate opaque tokens by calling the introspection endpoint. This is the 57K RPS dominant load. Introspection results are cached in Redis with TTL = remaining token lifetime.
- Access token TTL is 1 hour (vs. 15 minutes in Auth Service) because resource servers cache introspection results — the effective revocation latency is bounded by the cache TTL.

**Different decisions vs. the others:**
- Delegates user authentication to Auth Service via internal gRPC. OAuth2 Provider doesn't own the credential.
- Access tokens are optionally opaque (not always JWTs) to support introspection-based validation.
- Must handle M2M (Client Credentials flow) where there is no user at all.

**Two-sentence differentiator:** OAuth2 Provider is the delegation protocol layer, not a credential storage system — it accepts authenticated user context from the Auth Service and issues scoped, third-party-specific tokens based on user consent. Its uniqueness is mandatory PKCE (cryptographic code-verifier binding defeating code interception), RFC 8628 device flow for input-constrained clients, consent persistence per (user_id, client_id), and introspection caching to serve the 57K RPS resource-server validation load.

---

### Single Sign-On (SSO) Service

**What makes it unique:**
- The **only problem that interacts with systems you don't control** — external corporate identity providers (Okta, Azure AD, Ping Identity) that have their own protocols, certificate rotations, and trust models.
- **XML Signature Wrapping (XSW) attack prevention** is the most critical security detail here. An attacker can wrap a valid signature around unsigned malicious content, and a naive XML parser processes the unsigned wrapper first. The defense: verify the signature, then extract content from the signed element — never the reverse. Most XML libraries are vulnerable by default; this requires explicit hardening.
- **SAML assertion replay prevention** — SAML assertions have a `NotOnOrAfter` validity window. An intercepted assertion can be replayed within that window. Defense: store assertion IDs in Redis with TTL = NotOnOrAfter minus current time. On each assertion: check Redis before accepting.
- **JIT (Just-in-Time) Provisioning** — creating or updating platform user accounts on first SSO login. This requires an atomic upsert: `INSERT INTO identity_federations ... ON CONFLICT DO UPDATE` — ensuring no race condition if two requests for the same new user arrive simultaneously.
- **IdP discovery by email domain** — map `alice@enterprise-corp.com` → the right IdP configuration. Cached in Redis. The alternative (user picks their IdP from a list) is terrible UX.
- **Single Logout (SLO)** — propagating logout to all federated sessions. Back-channel (server-to-server HTTP POST for SAML) vs. front-channel (browser iframes). SLO is inherently unreliable because you're calling external services; implement best-effort with timeout.

**Different decisions vs. the others:**
- Does not store user passwords at all — delegates authentication entirely to the external IdP.
- Attribute mapping is configuration-driven (JSONB), not code-driven — enterprise customers have wildly different SAML attribute schemas.
- OIDC client secrets for external IdPs are stored encrypted with KMS — they're third-party credentials the platform must protect.

**Two-sentence differentiator:** SSO is the enterprise federation bridge — it accepts identity assertions from external corporate IdPs it doesn't control, validates them against known security attacks (XML Signature Wrapping, replay), and translates the external identity into a platform session. Its uniqueness is the SAML XSW attack prevention (signature-first, content-second extraction), multi-protocol support (SAML 2.0 + OIDC), JSONB-driven attribute mapping across heterogeneous IdP schemas, and JIT provisioning via atomic upsert.

---

### API Key Management

**What makes it unique:**
- **Prefixed key format** — `sk_live_<43 random chars>`. The prefix serves two purposes: it encodes the environment (live/test/dev) so keys can't be used across environments, and it enables secret scanning. GitHub, GitLab, GitGuardian all run regex scanners looking for `sk_live_[A-Za-z0-9\-_]{43}` patterns. When your key format is distinctive and registered with GitHub's Secret Scanning Partner Program, leaked keys are auto-detected and you receive a webhook.
- **SHA-256 for storage, not Argon2id** — this is explicitly different from passwords. Passwords have ~28 bits of entropy (a "strong" password). API keys have 256 bits of CSPRNG entropy. SHA-256 is one-way in practice for 256-bit inputs even at GPU cracking speeds (10^9 hashes/sec × all atoms in universe × age of universe < 2^256). Using Argon2id would add 200 ms × 5,787 RPS = completely unacceptable overhead for no security gain.
- **24-hour rotation overlap window** — when a key is rotated, both the old and new key are valid simultaneously for 24 hours (configurable). This is a deployment grace period: developers can update their applications to use the new key without a midnight cutover that causes downtime.
- **Immediate revocation via Redis DEL** — unlike JWT revocation (bounded by TTL), API key revocation is instantaneous. On revocation: delete the Redis entry synchronously in the same transaction commit hook. Propagation to all regions < 500 ms via Redis replication.
- **Leakage detection worker** — background worker that receives webhooks from GitHub/GitGuardian when a key matching the prefix pattern is found in public code. Auto-suspends the key (recoverable) and notifies the owner.
- **Usage tracking** — per-key request count, error count, bytes transferred, last_used_at. Implemented as async Redis INCR (fire-and-forget on hot path) with a background aggregation job writing to PostgreSQL daily summaries.

**Different decisions vs. the others:**
- No user session concept — API keys are long-lived credentials, not session-bound tokens.
- No rotation family concept — rotation creates a sibling key with overlap window, not a chain.
- Scope enforcement is at the key level, not the user level — the key grants specific endpoints, independent of who created it.

**Two-sentence differentiator:** API Key Management is the machine credential lifecycle service — it issues high-entropy developer credentials, validates them at sub-millisecond latency on every API call, and enables safe rotation via 24-hour overlap windows without deployment coordination. Its uniqueness is the prefixed CSPRNG key format enabling GitHub Secret Scanning partner integration, SHA-256 storage (safe at 256-bit entropy, avoiding Argon2id's 200 ms overhead), immediate Redis DEL revocation (<500 ms global propagation), and async leakage detection via webhook integration.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (interviewers ask in first 10 minutes)

**Q1: Why use RS256 instead of HS256 for JWT signing?**

RS256 uses an asymmetric RSA key pair. The auth server signs tokens with the private key, which it keeps in KMS and never shares. Any resource server that needs to verify tokens uses the public key, which is published openly in the JWKS endpoint. This means you can add new resource servers without distributing secrets. HS256 uses a shared secret — every service that verifies tokens must possess the secret, creating a key distribution problem. If that shared secret leaks from any one of those services, every JWT in the system is forgeable. At microservice scale with dozens of services verifying tokens, HS256 is operationally and security-wise untenable.

**Q2: What is a refresh token and why do we need it alongside the access token?**

Access tokens are short-lived (15 minutes) to bound the revocation window — if stolen, they're useless quickly. But short-lived tokens can't sustain long user sessions (you'd have to log in every 15 minutes). The refresh token is a long-lived credential (90 days) that allows the client to silently obtain a new access token without user interaction. The key security property is that the refresh token is stored server-side as a hash (validatable) and is delivered exclusively via HttpOnly cookie (not accessible to JavaScript), while the access token is a short-lived artifact that can be in memory. The two tokens have different security profiles: access token is broadly used on every API call; refresh token is narrowly used only at the `/token/refresh` endpoint.

**Q3: Why store password hashes and not encrypted passwords?**

Encryption is reversible — if you have the decryption key, you can recover the password. This means any breach that includes both the database and the encryption key (a common scenario: application servers hold keys and databases) exposes all passwords. Hashing is one-way: given the hash, there is no algorithm to recover the original password (for a sufficiently strong hash function and sufficient input entropy). The purpose of storing passwords is never to retrieve them; it is only to verify them. Hashing is the correct tool for a verification-only credential.

**Q4: What is PKCE and why is it required for mobile apps?**

PKCE (Proof Key for Code Exchange, RFC 7636) solves the authorization code interception problem for public clients. A mobile app registers a custom URI scheme (e.g., `myapp://callback`) to receive the authorization code redirect. A malicious app on the same device can register the same scheme and intercept the code. Without PKCE, the attacker can exchange that code for tokens. With PKCE, the legitimate app generates a random `code_verifier` before the flow starts and sends a `code_challenge = SHA256(code_verifier)` to the authorization endpoint. At token exchange time, it sends the original `code_verifier`. The server verifies that `SHA256(code_verifier) == stored code_challenge`. An interceptor who captured only the code has no `code_verifier` (it was never transmitted over the redirect URL) and cannot complete the exchange.

**Q5: How do you prevent user enumeration on the login endpoint?**

Two things: First, always return the same error message ("invalid credentials") regardless of whether the email doesn't exist or the password is wrong. Never distinguish between "user not found" and "wrong password" in the response. Second, for timing attacks — if the user doesn't exist, you skip the Argon2id computation (very fast), but if the user exists with a wrong password, you run the full ~200 ms hash. An attacker can measure response times to determine which emails exist. Defense: if the user is not found, run a dummy Argon2id computation against a pre-stored dummy hash before returning. This makes the response time indistinguishable from a valid-user-wrong-password case.

**Q6: What goes wrong if you store refresh tokens in localStorage instead of HttpOnly cookies?**

localStorage is accessible via JavaScript. Any XSS vulnerability — a single unescaped user-provided string rendered in the DOM — allows an attacker's injected script to call `localStorage.getItem('refresh_token')` and exfiltrate the token to their server. The attacker then has a 90-day credential. HttpOnly cookies are not accessible to JavaScript at all, even from the same origin. `SameSite=Strict` prevents CSRF attacks from making the browser send the cookie to your auth endpoint from a different origin. `Path=/auth/v1/token` restricts the cookie to the refresh endpoint only, so it's not sent to every API call.

**Q7: What is the difference between authentication and authorization?**

Authentication is proving identity: "I am Alice, and I can prove it by knowing this password and possessing this TOTP device." Authorization is determining what an authenticated identity is permitted to do: "Alice is authenticated, and she is allowed to read her own profile but not edit other users' profiles." These are separate concerns. JWT access tokens handle both: the `sub` claim is the authentication result, and the `scope` claim encodes the authorization permissions. OAuth2 specifically separates these by having the auth server authenticate and the resource server authorize based on the scopes in the token.

---

### Tier 2 — Deep Dive Questions (after HLD is on the board)

**Q1: How does refresh token reuse detection work at the implementation level, and what are the failure modes?**

The implementation requires a `SERIALIZABLE` PostgreSQL transaction with `SELECT ... FOR UPDATE` on the token row. The algorithm: look up the token by SHA-256 hash; if `used_at IS NOT NULL`, this token was already rotated — reuse detected, revoke entire family; if `used_at IS NULL` and not expired, mark `used_at = NOW()`, issue a new token in the same family, commit. The serializable isolation ensures two concurrent refresh calls can't both pass the `used_at IS NULL` check simultaneously.

There are two failure modes worth mentioning. First, false positives from dropped networks: the server issues a new token but the client's HTTP connection drops before receiving it. The client retries with the old token, which now has `used_at` set. Mitigation: implement a 5-second grace window where reuse of a just-rotated token is tolerated if the replacement hasn't been used. Second, at very high concurrency, serializable transactions can abort with `ERROR: could not serialize access due to concurrent update`. The client must implement retry logic with exponential backoff for refresh endpoint calls.

**Q2: Why is token introspection at 57K RPS the dominant load in the OAuth2 Provider, and how do you handle it?**

Resource servers call the introspection endpoint to validate opaque access tokens on every API request. With 500 million introspection calls per day and 10x peak factor, you get 57K RPS. This is 10x higher than token issuance. The mitigation: cache introspection results in Redis on the resource server side with TTL = remaining token lifetime. The first time a resource server sees a token, it calls introspection (20 ms); subsequent calls for the same token within its 1-hour lifetime are served from local Redis cache (<1 ms). The introspection endpoint itself needs Redis caching: compute `token_hash = SHA256(token)`, `GET redis:token:{hash}` → serialized active/inactive + metadata. The active token set for 20M active users (20M × 512 bytes = 10 GB) fits comfortably in a Redis cluster.

**Q3: Explain XML Signature Wrapping attacks and how you prevent them in SAML.**

In a SAML assertion, the IdP signs an XML element — let's say `<Assertion ID="abc">`. The signature covers the content under `ID="abc"`. An XML Signature Wrapping attack works by injecting an unsigned element with a different ID at a position in the document tree that a naive parser reads first: `<Assertion ID="evil">evil-content</Assertion>` is injected before the signed element. Many XML parsers traverse the document tree and find the first `<Assertion>` element, which is the unsigned malicious one. The signature is then verified against the correctly-signed second assertion — and it passes. The result: the signature is valid but the content being acted upon is the attacker's.

Prevention requires an explicit order of operations: (1) Find the signed element by locating the `<Signature>` element and reading its `Reference URI` attribute (e.g., `#abc`). (2) Verify the signature on the element with that exact ID. (3) Extract assertions from the verified element only. Never use the first element found in the tree — always use the element pointed to by the signature reference. Use a hardened XML library (like `xmlsec`) that implements this correctly rather than writing your own XML parsing.

**Q4: Why do we use SHA-256 for API key storage but Argon2id for passwords, when both are "stored secrets"?**

This is an entropy argument. A user's password has approximately 28 bits of entropy for a "strong" password (humans are predictable). An attacker who steals a hash database can run a dictionary attack: try common passwords, their variants, and brute-force combinations at GPU speed (~10^10 SHA-256 operations per second on a modern GPU). Argon2id's memory-hardness reduces this to ~50 operations per second per GPU — making the attack economically infeasible.

An API key generated from `CSPRNG(32 bytes)` has 256 bits of entropy. Even at SHA-256 speed (10^10/s), exhaustively searching 2^256 values takes 10^68 years. There is no dictionary to attack. The entropy gap makes the hash algorithm choice irrelevant for security — what matters is the entropy of the input, not the speed of the hash. Using Argon2id for API key validation would add 200 ms × 5,787 RPS of unnecessary compute overhead with zero security benefit.

**Q5: How do you handle JIT provisioning race conditions in SSO when the same user logs in simultaneously from two browsers?**

Two SAML assertions arrive for the same IdP subject at nearly the same moment. Both trigger the JIT provisioner. Without concurrency protection, you'd create duplicate user accounts or duplicate federation records.

The solution is a single PostgreSQL `INSERT ... ON CONFLICT DO UPDATE` statement (upsert): `INSERT INTO identity_federations (platform_user_id, idp_id, external_subject, last_login_at) VALUES (new_uuid, ?, ?, NOW()) ON CONFLICT (idp_id, external_subject) DO UPDATE SET last_login_at = NOW() RETURNING id, platform_user_id`. The `UNIQUE(idp_id, external_subject)` constraint ensures PostgreSQL's row-level locking handles the race condition atomically — one insert wins, the other gets the UPDATE path. Both requests end up with the same `platform_user_id` and the user is provisioned exactly once.

**Q6: How would you implement immediate API key revocation with sub-500 ms global propagation across multiple regions?**

The primary mechanism: on revocation, synchronously delete the Redis entry for the key hash (`DEL apikey:{hash}`) before returning 204 to the revocation caller. This is the fast path.

For multi-region: each region has its own Redis cluster serving local API gateway nodes. On revocation at the management API (which writes to the primary PostgreSQL in one region), also publish a `key_revoked:{hash}` message to a global pub/sub (Redis Pub/Sub, Kafka, or a purpose-built message bus with multi-region replication). Each region's local Redis subscriber receives this and executes `DEL apikey:{hash}` locally. With a well-configured replication path, this is under 200 ms globally. The remaining < 500 ms budget covers processing latency.

The fallback: on a Redis cache miss (either natural expiry or missed invalidation event), the gateway falls back to PostgreSQL. If the PostgreSQL record shows `status = 'revoked'`, return 401 and do not re-cache. This ensures eventual correctness even if the pub/sub message is lost.

**Q7: What is the security implication of including a `jti` (JWT ID) in every access token?**

The `jti` enables targeted revocation without waiting for the token to expire. Without `jti`, you can only revoke all tokens for a user (by incrementing a generation counter) — you can't revoke one specific session. With `jti`, you can maintain a Redis blocklist: on logout or admin-initiated revocation, add the `jti` to a Redis set with TTL = remaining token lifetime. Every token validation includes a Redis `SISMEMBER jwt_blocklist {jti}` check. At any given time, the blocklist contains only active revocations — tokens that have expired are naturally pruned by TTL. The storage overhead is ~64 bytes per revoked token × number of actively revoked tokens — at 50,000 active revocations, that's 3.2 MB in Redis. The performance overhead is one additional Redis command per validation (on the hot path), which adds ~0.5 ms.

---

### Tier 3 — Staff+ Stress Test Questions (think aloud; no single right answer)

**Q1: Design a key rotation mechanism for the RSA signing key used to sign JWTs without causing any service disruption.**

"Let me think through this. The problem is: the private key is in KMS, the public key is published in JWKS, and thousands of service instances are caching that JWKS in-process with a 1-hour TTL. If I simply swap the key, any in-flight JWT signed with the old key will fail validation on instances that have already picked up the new JWKS.

The correct procedure has to account for two TTLs: the JWT access token TTL (15 minutes) and the JWKS cache TTL (1 hour).

Step 1: Generate the new RSA key pair in KMS.
Step 2: Publish both the old and new public keys in the JWKS endpoint simultaneously. The JWKS JSON now contains two entries, each with distinct `kid` values. Instances that refresh their JWKS cache now know both keys.
Step 3: Wait at least one JWKS cache TTL (1 hour) so all instances have refreshed and know the new key.
Step 4: Start signing new access tokens with the new private key (new `kid` in JWT header).
Step 5: Continue publishing both public keys for another access token TTL (15 minutes). This covers any in-flight tokens signed with the old key that were issued just before Step 4.
Step 6: Remove the old public key from JWKS.
Step 7: Wait another JWKS TTL (1 hour) for caches to drain the old key. Now no instance will attempt to validate with the old public key.

Edge case: a service instance that hasn't refreshed its JWKS cache sees a `kid` it doesn't recognize. It should trigger an immediate on-demand JWKS refresh rather than rejecting the token. This on-demand refresh on unknown `kid` is a safety mechanism that prevents disruption from cache staleness.

Total rotation time: approximately 2.5 hours for a fully safe rotation with zero disruption."

**Q2: Your auth service is under a credential stuffing attack — 500,000 login attempts/minute from 50,000 distinct IPs using breached username/password pairs. How do you respond without blocking legitimate users?**

"This is the hardest operational scenario in auth — the attacker is deliberately spreading across IPs to evade simple IP rate limiting.

Immediate response (under the attack):
The existing per-IP rate limit (10 requests/IP/minute) is already slowing them down, but 50,000 IPs × 10 = 500,000 RPS is still bypassing it. So I need additional signals.

Behavioral signals I can use:
- User agent diversity: botnets often use a small set of user agent strings. A sudden spike in login attempts from a user agent you've never seen before (or from user agents known for automation) is a signal.
- Geographic impossibility: if a user's last login was in New York 5 minutes ago and a new attempt is from Singapore, that's suspicious regardless of rate limits.
- Credential corpus matching: I can hash incoming passwords and check against HaveIBeenPwned k-anonymity API. If a high percentage of incoming credentials match known breach corpora, this confirms a credential stuffing attack.

Response mechanisms in order of aggressiveness:
1. CAPTCHA challenge at 5 failed attempts per account per hour (already in place). This slows the attack but doesn't stop it for accounts the attacker hasn't failed on yet.
2. Require email-verified MFA enrollment for any account successfully logging in from a new device (fingerprint change), even on first successful login during the attack window.
3. Temporarily require CAPTCHA for all login attempts (not just after failures) — significant UX impact but buys time.
4. IP reputation filtering: purchase a commercial threat intelligence feed (Cloudflare, Recorded Future) that identifies known botnet IPs. Block or heavily throttle.
5. For accounts that get successfully logged into: send an immediate suspicious activity email with a one-click session revocation link.
6. Emergency communication to security team: this is also a data signal — the attacker has a breach corpus. If they're succeeding at a measurable rate, affected users need proactive notification.

The metric I'd watch: `auth_login_requests_total{result="success"} / auth_login_requests_total{result="failure"}`. During a credential stuffing attack, the success rate drops to near zero (the attacker is trying many invalid combinations). But if it stays above 1%, users with reused passwords are being compromised. That's the escalation trigger."

**Q3: An enterprise customer wants their employees' sessions on your platform to be immediately terminated when the employee is deactivated in Okta. How do you architect this?**

"This is the 'near-real-time IdP lifecycle event' problem, and it has no perfect solution — only trade-offs.

The fundamental issue: once a user has a valid platform session (JWT + refresh token), that session is independent of the IdP. Okta deactivating the user doesn't automatically invalidate anything in our system. SAML and OIDC are one-directional at login time; there's no built-in push mechanism for account deactivation.

Option 1: Rely on OIDC Back-Channel Logout.
OIDC defines a back-channel logout specification where the IdP can POST a `logout_token` JWT to a registered callback URL when a user's session ends. We implement the `/sso/oidc/backchannel-logout` endpoint. On receipt of a valid logout_token: look up all active platform sessions for the `sub` in the token, revoke all refresh tokens in those families, and add all active JTIs to the JWT blocklist. This is instant but requires Okta to support back-channel logout (it does) and to be configured with our callback URL. Residual risk: if the logout_token POST fails or is delayed, the session persists.

Option 2: Short-lived access tokens + SCIM provisioning.
Use SCIM (System for Cross-domain Identity Management) — Okta can push user lifecycle events (deactivate, reactivate) to our SCIM endpoint. On SCIM deactivate event: set the user's platform account to `disabled` state immediately. Disabled accounts fail all token validation (we check account state on every refresh). But active access tokens remain valid until expiry (15 minutes for us). Acceptable for most enterprise use cases; not acceptable for high-security scenarios.

Option 3: Extremely short-lived sessions (5 minutes) with OIDC `prompt=login` or re-validation.
Force SSO users to re-validate against the IdP frequently. Short session TTL means deactivated employees lose access within 5 minutes. High operational overhead for users; only appropriate for high-security environments.

My recommendation for most enterprise customers: Option 1 (OIDC back-channel logout) as the primary mechanism, with Option 2 (SCIM) as the safety net for cases where back-channel logout is missed. Document to the customer that there is a residual window up to the access token TTL (15 minutes) for already-active requests. If they need sub-minute eviction, Option 3 is the only option and comes with significant UX cost."

**Q4: How would you design an auth system that handles 500 million users across 5 geographic regions with data residency requirements (EU data must stay in EU, US data must stay in US)?**

"Data residency turns a single-region auth design into a multi-region problem with hard constraints, not just availability concerns.

The core challenge: user credentials (Argon2id hashes, refresh tokens) for EU users must never be read by US-region infrastructure. But the signing key for JWTs is usually single: you can't have EU-signed tokens that US resource servers can't verify.

Partition the credential stores by data residency region. Each region (EU, US, APAC, etc.) has its own PostgreSQL cluster, its own Redis cluster, and its own set of auth service pods. User accounts are assigned to a region at registration time based on their declared location and cannot move (or can only move via an explicit data migration with user consent).

For routing: a global entry point (Anycast DNS, Cloudflare) routes users to their home region. If a user in the EU tries to log in, the request goes to the EU auth cluster. This is determined by the email domain (enterprise) or explicitly stored user-region mapping (consumer).

For JWT signing: you have two options. First, each region has its own RSA signing key and its own JWKS endpoint. Resource servers in the EU use the EU JWKS; resource servers in the US use the US JWKS. This works if services and their users are co-located (EU services serve EU users, US services serve US users). Second, a single global signing key, with the private key replicated to regional KMS instances (each region's KMS has a copy). The public key is published from a single global JWKS endpoint. This violates the spirit of data residency if the signing key itself is considered sensitive data; consult your legal team.

Refresh token reuse detection: since refresh tokens are region-local, a user moving between regions (EU → US trip) would experience a forced re-login because their EU refresh token isn't known to the US region's Redis. This is acceptable behavior for a residency-compliant system; communicate it clearly as a product limitation.

Edge case: an EU user accessing a US-hosted resource server. The US resource server needs to validate a JWT signed by the EU JWKS. If the JWT's `kid` points to the EU key and the US resource server has only the US JWKS cached, it will fail validation. Mitigation: make the JWKS endpoint aware of both regional public keys (public keys are safe to share globally — only private keys are residency-sensitive). The resource server can check both regional JWKS when the `kid` doesn't match its cached set."

---

## STEP 7 — MNEMONICS

### Mnemonic 1: The CRAVE-F checklist for every auth token

When designing or reviewing any token in auth, run through CRAVE-F:

- **C — Cryptographic randomness**: Generated from CSPRNG (32 bytes minimum). Never sequential, timestamp-based, or deterministic.
- **R — Revocable**: Is there a mechanism to invalidate it before expiry? JWT: JTI blocklist or short TTL. Refresh: family revocation. API key: Redis DEL.
- **A — Audited**: Every issuance, use, and revocation logged to Kafka/Audit store with actor + IP + outcome.
- **V — Validated correctly**: Signature verified (RS256 via JWKS), claims checked (exp, iss, aud), replay prevented (jti, TOTP window).
- **E — Entropy preserved**: Never stored raw. Always SHA-256 hash in database (for tokens) or Argon2id (for passwords).
- **F — Fingerprinted for abuse**: Is there rate limiting? Is reuse detection in place? Is the format recognizable to secret scanners?

### Mnemonic 2: The "Token Ladder" for session architecture

From most ephemeral to most permanent:

```
TOTP code (30 seconds)
  → MFA session token (5 minutes)
    → Access token / JWT (15 minutes)
      → Authorization code (10 minutes, OAuth2)
        → Refresh token (90 days, auth service)
          → OAuth2 refresh token (30 days)
            → API key (months to years)
```

Each rung up the ladder has more entropy, longer life, and needs stronger protection. The ladder also tells you what to revoke on a security incident: revoke from the bottom up (API keys first for machine access, then refresh tokens for human sessions).

### Opening one-liner for any auth problem

"Before I draw anything, let me orient: auth systems have two completely different performance profiles. Token issuance is write-heavy, compute-bound (Argon2id takes 200ms by design), and happens at hundreds of RPS. Token validation is read-heavy, stateless, and happens at 100K+ RPS — it must never touch the auth service. I'll design these paths completely independently."

This line immediately signals to the interviewer that you understand the system holistically and won't conflate the two paths.

---

## STEP 8 — CRITIQUE

### What the source material covers well

The source material is exceptionally thorough on several critical areas:

- **Argon2id rationale** is the best explanation I've seen in a study guide. The comparison to bcrypt (fixed 4 KB memory vs. Argon2id's 19 MiB) and the quantitative GPU cracking rate comparison (1,000 bcrypt/s vs. 50 Argon2id/s per GPU) is exactly the depth interviewers at Staff+ levels probe for.
- **Refresh token rotation algorithm** is precise: the SERIALIZABLE transaction + SELECT FOR UPDATE + `used_at IS NOT NULL` check covers the concurrent refresh race condition that most candidates miss entirely.
- **PKCE flow** is spelled out completely. The distinction between S256 and plain, and the explanation of what attack PKCE prevents (code interception on same-device malicious apps), is correct and complete.
- **XML Signature Wrapping** prevention is specifically called out with the correct fix (signature-first, content-second extraction). Most SSO material glosses over XSW entirely; this is a real interview question at companies that have been attacked this way.
- **Capacity math** is correct and usable. The "auth service handles 6K RPS, JWT validation handles 115K RPS — these are separate" insight is the kind of statement that earns points in interviews.
- **SHA-256 vs. Argon2id for API keys** is the clearest treatment of this distinction I've encountered. The entropy argument (256-bit CSPRNG token vs. 28-bit password entropy) is the right frame.

### What is missing, shallow, or could mislead

**Missing: DPoP (Demonstrating Proof of Possession)** — sender-constraining tokens to a specific key. The source materials mention it briefly as "sender-constrained tokens" but don't explain how DPoP works. At Staff+ levels in 2025-2026, interviewers increasingly ask about DPoP as the next step beyond refresh token rotation. DPoP binds an access token to a client's asymmetric key via a proof-of-possession JWT sent with every request. Even a stolen token is useless without the private key. Know this concept exists and what problem it solves.

**Missing: Passkeys / WebAuthn for primary authentication (passwordless)** — the source material covers WebAuthn as a second factor only. In 2026, the trend is WebAuthn as the first factor (passkeys replacing passwords entirely). If an interviewer asks "how would you add passkeys to your auth service," the answer is: passkey registration generates a resident credential bound to the RP origin + user; login sends an authenticatorAssertionResponse that the server verifies using the stored public key; no password hash needed. The existing WebAuthn data model in the source (credential_id, public_key, sign_count, aaguid) supports this — you just remove the password step.

**Missing: Token binding at the TLS layer (mTLS)** — mutual TLS authentication for service-to-service calls, and using client certificates to bind OAuth2 tokens to a specific TLS client certificate (RFC 8705). This comes up in fintech/compliance interviews. The short answer: in high-assurance environments, client_credentials tokens are bound to the client's TLS certificate fingerprint, making stolen tokens unusable from a different network endpoint.

**Shallow: Key rotation operational runbook** — the source describes the 2.5-hour JWKS rotation procedure correctly, but doesn't mention what to do if you discover the signing key has been compromised and need emergency rotation (you can't wait 2.5 hours). Emergency rotation: immediately stop accepting tokens with the old `kid` (add to a per-kid blocklist), publish new key, send forced re-authentication to all users. The source's description assumes a planned, non-emergency rotation.

**Shallow: Multi-tenant considerations in SSO** — the source covers multi-IdP for enterprise customers, but doesn't deeply address the tenant isolation problem: ensuring Tenant A's IdP configuration cannot be used to authenticate users into Tenant B's context. The IdP configuration data model has `tenant_id`, but the interview follow-up "how do you prevent one tenant's IdP from being used for another tenant" requires explicit discussion of the validation: SAML `AudienceRestriction` must contain the correct SP entity ID (which is tenant-specific), and the redirect URI in OIDC must match the tenant's registered URI.

**Potentially misleading: The claim that refresh tokens should have 90-day TTL universally.** This is the auth service default, but OAuth2 Provider refresh tokens are 30 days. More importantly, the "correct" TTL depends entirely on the threat model and UX requirements. For a banking app: 24 hours is standard. For a consumer social app: 90 days or longer. The source's specific numbers are good anchors for interview math but shouldn't be presented as universal defaults.

### Senior probes to watch for (questions interviewers ask to separate senior from staff+)

1. "What happens if Redis loses the refresh token cache and the PostgreSQL has it, but Redis has a stale expired-at?" Answer: PostgreSQL is authoritative. On cache miss, always re-read from PostgreSQL and re-warm Redis. Never use Redis as the source of truth for revocation; Redis can say "valid" when the token is revoked if the DEL event was lost. This is why revocation writes to PostgreSQL first and then DELs from Redis — never the other way.

2. "Your JWKS endpoint caches for 1 hour but you need to do emergency key revocation. What breaks?" Answer: Services with a recently refreshed in-process JWKS cache will continue accepting tokens signed with the compromised key for up to 1 hour. Mitigations: (a) Implement an emergency JWKS cache-bust endpoint (authenticated, admin-only) that sends a signal to all service instances to immediately refresh their JWKS. (b) Add the old `kid` to a per-kid blocklist checked alongside the JTI blocklist. (c) Accept the 1-hour window as an acceptable operational risk for the compromise scenario and focus on containing the blast radius other ways (session revocation, forcing re-authentication).

3. "How do you handle a SAML assertion signed with an RSA-SHA1 signature when SHA-1 is deprecated?" Answer: This is a real operational problem. SHA-1 is cryptographically broken since 2017 (SHAttered attack). The correct response: accept RSA-SHA1 from IdPs that cannot upgrade (ADFS on older Windows Server), log a deprecation warning, and have a per-IdP flag that enforces minimum `RSA-SHA256`. Work with the enterprise customer's IT team to upgrade. For new IdP integrations: require RSA-SHA256 or better. Never silently downgrade signature requirements.

### Common traps in interviews

**Trap 1: Designing the JWT validation path as part of the auth service.** If you put JWT validation inside the auth service and route every API call through it, you've made the auth service the bottleneck for 100K+ RPS and turned a stateless operation into a network round-trip. The validation must happen at the API gateway layer using in-process JWKS cache.

**Trap 2: Recommending OAuth2 implicit flow.** If asked "how should a SPA authenticate with OAuth2," the answer is Authorization Code + PKCE (with no client secret). Implicit flow was deprecated in RFC 9700 and is actively harmful (tokens in URL fragments, no refresh token support, no PKCE). Many study guides still mention implicit flow — don't use it in interviews.

**Trap 3: Returning different HTTP status codes for "user not found" vs. "wrong password."** This enables user enumeration. Always return 401 with the same body regardless of which check failed.

**Trap 4: Forgetting that Single Logout (SLO) is best-effort.** Many candidates design SLO as if it's guaranteed. In practice, back-channel logout calls to external IdPs can time out, fail, or be silently dropped. The correct design: attempt SLO, log the outcome, but don't block the user's logout from the platform on SLO failure. The platform session must always be revocable independently of what the IdP does.

**Trap 5: Conflating the OAuth2 Provider's consent screen with the login page.** The OAuth2 Provider doesn't handle passwords — it delegates to the Auth Service. The consent screen says "do you want to allow App X to access your profile?" It's shown after the user is already authenticated, not as part of authentication. Drawing these as the same step is a protocol misunderstanding.

---

*End of Pattern 16: Auth & Security Interview Guide*
