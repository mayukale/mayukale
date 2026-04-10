# System Design: Authentication Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **User Registration** — create accounts with email/password; validate email uniqueness; enforce password policy (minimum length 12, complexity rules).
2. **Password Hashing** — store passwords using Argon2id; never store plaintext or reversibly encrypted passwords.
3. **Login** — authenticate via email + password; return short-lived access token and long-lived refresh token.
4. **Token Issuance** — issue signed JWT access tokens (RS256, 15-minute TTL) and opaque refresh tokens (90-day TTL, rotating on use).
5. **Refresh Token Rotation** — each use of a refresh token issues a new refresh token and invalidates the old one; detect reuse of invalidated tokens (reuse detection).
6. **Token Revocation** — support explicit logout (revoke refresh token family); support admin-initiated account suspension that blocks all token validation.
7. **Multi-Factor Authentication (MFA)** — support TOTP (RFC 6238) via authenticator apps and WebAuthn (FIDO2) hardware/biometric keys as second factors.
8. **Account Lockout** — lock account after N consecutive failed login attempts (configurable, default 10); use exponential backoff + lockout with unlock via email.
9. **Brute Force Protection** — rate-limit login attempts per IP and per account; use CAPTCHA challenge after threshold.
10. **Password Reset** — time-limited, single-use, cryptographically random reset tokens sent to verified email.
11. **Session Management** — list active sessions per user; allow selective session revocation.
12. **Audit Log** — record every authentication event (login, logout, token refresh, MFA enrollment, password change) with timestamp, IP, user-agent.

### Non-Functional Requirements

1. **Latency** — p99 login latency < 300 ms (including bcrypt/Argon2 computation time budget of ~200 ms).
2. **Availability** — 99.99% uptime (< 52 min/year downtime); auth is a critical dependency for all services.
3. **Throughput** — handle 50,000 login requests/second at peak; 500,000 token validation requests/second (JWT validation is stateless and CPU-bound).
4. **Security** — OWASP ASVS Level 2 compliance; SOC 2 Type II scope; secrets stored in HSM or secrets manager.
5. **Scalability** — horizontally scalable stateless token validation; stateful refresh token store must support sharding.
6. **Consistency** — refresh token revocation must be strongly consistent (no stale reads that allow reuse after revocation).
7. **Durability** — refresh token store and audit logs must survive single-AZ failure.

### Out of Scope

- OAuth2 provider flows (authorization code, client credentials) — covered in `oauth2_provider.md`.
- SSO / SAML / OIDC identity brokering — covered in `sso.md`.
- API key issuance — covered in `api_key_management.md`.
- User profile storage beyond authentication attributes.
- Email delivery infrastructure (assumed external SMTP/transactional email service).

---

## 2. Users & Scale

### User Types

| Type | Description | Auth Patterns |
|---|---|---|
| End User (Consumer) | Human logging in via web/mobile | Password + MFA, refresh rotation |
| End User (Enterprise) | Human behind SSO/IdP | Delegated; token exchange |
| Service Account | Machine-to-machine | Client credentials (out of scope here) |
| Admin | Internal operators | Password + MFA + IP allowlist |

### Traffic Estimates

**Assumptions:**
- 500 million total registered users.
- Daily Active Users (DAU) = 10% of registered = 50 million.
- Average sessions per DAU per day = 2 (new login or refresh).
- Login events = 50M × 0.5 (new logins, rest are token refreshes) = 25M/day.
- Refresh events = 50M × 1.5 = 75M/day.
- Token validation (JWT verify) = every API call; assume 20 API calls per active user per day = 50M × 20 = 1B/day.

| Event | Daily Volume | Peak Multiplier | Peak RPS |
|---|---|---|---|
| Login (password verify) | 25,000,000 | 5× (morning rush) | 25M × 5 / 86,400 = **1,447 RPS** |
| Token Refresh | 75,000,000 | 5× | 75M × 5 / 86,400 = **4,340 RPS** |
| JWT Token Validation | 1,000,000,000 | 10× | 1B × 10 / 86,400 = **115,741 RPS** |
| Registration | 500,000 | 3× | 500K × 3 / 86,400 = **17 RPS** |
| Password Reset | 1,000,000 | 3× | 1M × 3 / 86,400 = **35 RPS** |
| MFA Verify | 12,500,000 (50% of logins) | 5× | **723 RPS** |

**Note:** JWT validation (115K RPS) is handled stateless at API gateway layer using the public key; the auth service itself handles login/refresh only (~6K RPS peak).

### Latency Requirements

| Operation | Target p50 | Target p99 | Constraint |
|---|---|---|---|
| Login (password hash + token issue) | 150 ms | 300 ms | Argon2id ~100–200 ms intentional |
| Token Refresh | 5 ms | 20 ms | DB read + sign |
| JWT Validation (gateway) | < 1 ms | 5 ms | In-memory JWKS cache + RSA verify |
| Registration | 100 ms | 250 ms | Hash + DB write |
| Password Reset Token Send | 200 ms | 500 ms | Hash + email dispatch |

### Storage Estimates

| Data | Record Size | Total Records | Storage |
|---|---|---|---|
| User credential record | 512 bytes | 500M | 500M × 512B = **256 GB** |
| Refresh token record | 256 bytes | 2 tokens/active user × 50M DAU × 90-day retention = 9B entries → pruned to active ~100M | 100M × 256B = **25.6 GB** |
| Token family/reuse detection state | 128 bytes | 100M active families | 100M × 128B = **12.8 GB** |
| MFA enrollment (TOTP/WebAuthn) | 256 bytes | 200M enrolled users | 200M × 256B = **51.2 GB** |
| Audit log (hot tier, 90 days) | 512 bytes/event | 100M events/day × 90 days = 9B | 9B × 512B = **4.6 TB** |
| Audit log (cold tier, 7 years) | 256 bytes compressed | ~255B events | **~65 TB** |

**Total operational storage: ~350 GB (credentials + tokens + MFA). Audit log tiered separately.**

### Bandwidth Estimates

| Flow | Payload Size | Daily Requests | Daily Bandwidth |
|---|---|---|---|
| Login request (inbound) | 1 KB | 25M | 25 GB |
| Login response (access + refresh token) | 2 KB | 25M | 50 GB |
| Refresh request/response | 1.5 KB | 75M | 112.5 GB |
| JWKS endpoint (public key, cached) | 2 KB | 1M fetches (cached for 1h) | 2 GB |
| **Total per day** | — | — | **~190 GB/day (~17.8 Gbps peak at 10×)** |

---

## 3. High-Level Architecture

```
                          ┌─────────────────────────────────────────────────────┐
                          │                   Clients                           │
                          │  Browser / Mobile App / Server-to-Server            │
                          └──────────────┬──────────────────────────────────────┘
                                         │ HTTPS
                          ┌──────────────▼──────────────────────────────────────┐
                          │              API Gateway / Load Balancer             │
                          │  TLS termination, IP rate limiting, DDoS shield     │
                          │  JWT validation (JWKS cache) for protected routes   │
                          └──┬───────────────────────┬───────────────────────┬──┘
                             │ /auth/*                │ /validate (internal)  │
                ┌────────────▼──────────┐             │                       │
                │   Auth Service        │    ┌────────▼─────────┐            │
                │   (Stateless pods,    │    │ Token Validator  │            │
                │    N replicas)        │    │ (Sidecar or lib) │            │
                │                       │    └──────────────────┘            │
                │  ┌─────────────────┐  │                                    │
                │  │ Login Handler   │  │    ┌──────────────────────────┐   │
                │  │ Refresh Handler │  │    │   JWKS Endpoint (CDN)    │   │
                │  │ Registration    │  │    │   /auth/v1/jwks.json     │◄──┘
                │  │ MFA Handler     │  │    └──────────────────────────┘
                │  │ Revocation      │  │
                │  └────────┬────────┘  │
                └───────────┼───────────┘
                            │
          ┌─────────────────┼──────────────────────────────────────┐
          │                 │                                        │
┌─────────▼──────┐  ┌───────▼──────────┐  ┌───────────────────┐  │
│  User Store    │  │ Refresh Token    │  │   MFA Store        │  │
│  (PostgreSQL,  │  │ Store            │  │   (PostgreSQL,     │  │
│   primary DB)  │  │ (Redis Cluster + │  │    TOTP secrets,   │  │
│                │  │  PostgreSQL for  │  │    WebAuthn creds) │  │
│  - credentials │  │  durability)     │  └───────────────────┘  │
│  - account     │  │                  │                           │
│    state       │  │  - token hash    │  ┌───────────────────┐  │
└────────────────┘  │  - family id     │  │  Audit Log Store  │  │
                    │  - expiry        │  │  (Kafka → S3 /    │  │
                    └──────────────────┘  │   ClickHouse)     │  │
                                          └───────────────────┘  │
                                                                   │
                    ┌─────────────────────────────────────────────┘
                    │
          ┌─────────▼──────────┐     ┌────────────────────┐
          │  Rate Limiter      │     │  HSM / Secrets Mgr │
          │  (Redis, token     │     │  (AWS KMS / HashiC │
          │   bucket per IP    │     │   Vault)            │
          │   + per account)   │     │  - RSA private key  │
          └────────────────────┘     │  - Argon2 pepper   │
                                     └────────────────────┘
```

**Component Roles:**

- **API Gateway / Load Balancer**: Terminates TLS, applies coarse IP-level rate limits, validates JWT signatures using cached JWKS for all non-auth endpoints, routes `/auth/*` to Auth Service.
- **Auth Service (stateless pods)**: Contains all authentication business logic. Stateless itself — all state is in backing stores. Horizontally scalable. Responsible for login, registration, MFA verify, refresh rotation, revocation.
- **Token Validator**: A shared library (or sidecar) that validates JWT signatures + claims locally using JWKS public key. No network call per request; JWKS cached in-process with 1-hour TTL and background refresh.
- **JWKS Endpoint**: Serves RS256 public keys at a well-known URL. Served from CDN with aggressive caching (Cache-Control: max-age=3600). Enables stateless JWT validation across all services.
- **User Store (PostgreSQL)**: Canonical source of truth for user accounts, credential hashes, account state (active/locked/disabled).
- **Refresh Token Store (Redis + PostgreSQL)**: Redis for fast O(1) lookup of token validity; PostgreSQL as durable backing store. Redis is write-through; evictions trigger revalidation from PostgreSQL.
- **MFA Store (PostgreSQL)**: TOTP encrypted secrets (AES-256-GCM, key in KMS), WebAuthn credential IDs and public keys. Stored per-user.
- **Rate Limiter (Redis)**: Token bucket algorithm per IP and per account. Backed by Redis atomic increment with TTL. Shared across all Auth Service pods.
- **HSM / Secrets Manager**: AWS KMS or HashiCorp Vault stores RSA private signing key (never loaded in application memory directly; signing is a KMS API call or Vault transit call in sensitive deployments) and Argon2 pepper.
- **Audit Log Store**: Auth Service emits structured events to Kafka. A consumer writes to ClickHouse (analytics, real-time queries) and archives to S3 Parquet (7-year retention, compliance).

**Primary Use-Case Data Flow (Login with MFA):**

```
1. Client POST /auth/v1/login {email, password}
2. API Gateway applies IP rate limit check (Redis). If exceeded → 429.
3. Auth Service receives request.
4. Lookup user record by email (PostgreSQL read). If not found → constant-time 
   response to prevent user enumeration.
5. Check account lock state. If locked → 423 with retry-after.
6. Verify Argon2id(password + pepper) against stored hash (CPU-bound, ~100ms).
7. If mismatch → increment failed_attempts counter (Redis atomic incr with TTL).
   If failed_attempts >= threshold → lock account + emit audit event.
8. If password correct AND MFA enrolled → return {mfa_required: true, 
   mfa_session_token: <short-lived 5-min signed token>}.
9. Client POST /auth/v1/mfa/verify {mfa_session_token, totp_code}
10. Auth Service validates mfa_session_token, verifies TOTP code (RFC 6238, 
    ±1 window, stored used-codes in Redis to prevent replay).
11. On success:
    a. Generate access token: JWT RS256, {sub, iat, exp=+15min, jti, amr=["pwd","otp"]}
    b. Generate refresh token: 32-byte CSPRNG → URL-safe base64 = 43 chars; 
       compute SHA-256 hash for storage.
    c. Store refresh token: {hash, user_id, family_id, issued_at, expires_at, 
       replaced_by=null} in Redis + PostgreSQL.
    d. Reset failed_attempts counter.
    e. Emit audit event to Kafka.
12. Return {access_token, refresh_token, expires_in: 900} to client.
    Set-Cookie: refresh_token=<value>; HttpOnly; Secure; SameSite=Strict
```

---

## 4. Data Model

### Entities & Schema

```sql
-- =============================================
-- Users table: core account and credential data
-- =============================================
CREATE TABLE users (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    email               VARCHAR(320)    NOT NULL,
    email_verified      BOOLEAN         NOT NULL DEFAULT FALSE,
    email_normalized    VARCHAR(320)    NOT NULL,          -- lowercase, trimmed
    password_hash       VARCHAR(512)    NOT NULL,          -- Argon2id output, PHC string format
    password_changed_at TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    account_state       VARCHAR(20)     NOT NULL DEFAULT 'active'
                            CHECK (account_state IN ('active','locked','disabled','pending_verification')),
    failed_attempts     SMALLINT        NOT NULL DEFAULT 0,
    locked_until        TIMESTAMPTZ,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    CONSTRAINT uq_email_normalized UNIQUE (email_normalized)
);

CREATE INDEX idx_users_email_normalized ON users (email_normalized);

-- =============================================
-- MFA enrollments
-- =============================================
CREATE TABLE mfa_enrollments (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id             UUID            NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    method              VARCHAR(20)     NOT NULL CHECK (method IN ('totp','webauthn','backup_code')),
    is_primary          BOOLEAN         NOT NULL DEFAULT FALSE,
    is_verified         BOOLEAN         NOT NULL DEFAULT FALSE,
    -- TOTP fields (secret encrypted with AES-256-GCM, key in KMS)
    totp_encrypted_secret   BYTEA,
    totp_key_id         VARCHAR(128),                     -- KMS key reference
    -- WebAuthn fields
    webauthn_credential_id  BYTEA,                        -- credential ID from authenticator
    webauthn_public_key     BYTEA,                        -- COSE-encoded public key
    webauthn_sign_count     BIGINT      DEFAULT 0,        -- for clone detection
    webauthn_aaguid         UUID,                         -- authenticator model ID
    webauthn_transports     TEXT[],                       -- ["usb","nfc","ble","internal"]
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    last_used_at        TIMESTAMPTZ
);

CREATE INDEX idx_mfa_user_id ON mfa_enrollments (user_id);
CREATE UNIQUE INDEX idx_mfa_webauthn_cred_id ON mfa_enrollments (webauthn_credential_id) 
    WHERE webauthn_credential_id IS NOT NULL;

-- =============================================
-- Refresh token families (for reuse detection)
-- =============================================
CREATE TABLE refresh_token_families (
    id              UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID            NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    invalidated_at  TIMESTAMPTZ,                          -- set when family is revoked
    invalidation_reason VARCHAR(50)                       -- 'logout','reuse_detected','admin','expired'
);

CREATE INDEX idx_rtf_user_id ON refresh_token_families (user_id);

-- =============================================
-- Refresh tokens
-- =============================================
CREATE TABLE refresh_tokens (
    id              UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    family_id       UUID            NOT NULL REFERENCES refresh_token_families(id),
    user_id         UUID            NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash      CHAR(64)        NOT NULL,             -- SHA-256 hex of raw token
    issued_at       TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at      TIMESTAMPTZ     NOT NULL,
    used_at         TIMESTAMPTZ,                          -- set when rotated
    replaced_by     UUID            REFERENCES refresh_tokens(id),
    revoked_at      TIMESTAMPTZ,
    revocation_reason VARCHAR(50),
    ip_address      INET,
    user_agent_hash CHAR(64)                              -- SHA-256 of user-agent string
);

CREATE UNIQUE INDEX idx_rt_token_hash ON refresh_tokens (token_hash);
CREATE INDEX idx_rt_family_id ON refresh_tokens (family_id);
CREATE INDEX idx_rt_user_id_expires ON refresh_tokens (user_id, expires_at);

-- =============================================
-- TOTP used-codes (replay prevention)
-- =============================================
CREATE TABLE totp_used_codes (
    user_id         UUID            NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    code_hash       CHAR(64)        NOT NULL,             -- SHA-256 of 6-digit code + timestamp_window
    used_at         TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    PRIMARY KEY (user_id, code_hash)
);

-- TTL-managed: purge rows where used_at < NOW() - INTERVAL '2 minutes'
CREATE INDEX idx_totp_used_cleanup ON totp_used_codes (used_at);

-- =============================================
-- Password reset tokens
-- =============================================
CREATE TABLE password_reset_tokens (
    id              UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID            NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash      CHAR(64)        NOT NULL,             -- SHA-256 of raw token
    created_at      TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at      TIMESTAMPTZ     NOT NULL,             -- 1-hour TTL
    used_at         TIMESTAMPTZ,
    ip_address      INET
);

CREATE UNIQUE INDEX idx_prt_token_hash ON password_reset_tokens (token_hash);
CREATE INDEX idx_prt_user_id ON password_reset_tokens (user_id);

-- =============================================
-- Audit log (write-optimized, append-only)
-- =============================================
CREATE TABLE audit_events (
    id              UUID            DEFAULT gen_random_uuid(),
    user_id         UUID,                                 -- NULL for anonymous events
    event_type      VARCHAR(50)     NOT NULL,             -- 'login_success','login_failure',...
    event_timestamp TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    ip_address      INET,
    user_agent      TEXT,
    metadata        JSONB,                                -- event-specific fields
    PRIMARY KEY (event_timestamp, id)                     -- for TimescaleDB hypertable partitioning
) PARTITION BY RANGE (event_timestamp);

-- Partition monthly; automated partition creation via pg_partman
CREATE INDEX idx_ae_user_id_ts ON audit_events (user_id, event_timestamp DESC);
```

### Database Choice

**Options Considered:**

| Database | Pros | Cons | Verdict |
|---|---|---|---|
| PostgreSQL | ACID, row-level locking, mature ecosystem, excellent JSONB support, pg_partman for audit table partitioning, rich indexing | Vertical scaling ceiling; sharding requires external tooling (Citus) | **Selected for User Store, MFA, Refresh Tokens** |
| MySQL / InnoDB | Wide hosting support, good replication | Weaker JSONB, less flexible indexing, `utf8mb4` confusion | Not selected |
| CockroachDB | Distributed ACID, auto-sharding | Higher latency (consensus overhead), operational complexity | Viable at very large scale; add if PostgreSQL sharding becomes bottleneck |
| MongoDB | Flexible schema | No multi-document ACID prior to 4.0; schema flexibility unnecessary here | Not selected |
| Redis | Sub-millisecond reads, atomic operations, TTL native | Not durable by default; data loss on crash without AOF/RDB + replication | **Selected for rate limiting, TOTP replay cache, refresh token hot path** |
| DynamoDB | Auto-scaling, managed | Eventual consistency on GSIs, expensive for complex queries, vendor lock-in | Viable alternative to Redis for token store |

**Selected: PostgreSQL (primary) + Redis (cache/rate-limit layer)**

Justification:
- PostgreSQL's `SERIALIZABLE` isolation level is used for the refresh token rotation transaction (read current token → verify unused → write replacement → mark used), preventing TOCTOU races.
- `gen_random_uuid()` uses `pgcrypto`'s CSPRNG, ensuring unpredictable primary keys.
- Redis atomic `INCR` + `EXPIRE` provides lock-free, low-latency rate counter increments without distributed coordination overhead.
- Redis key-value TTL natively handles refresh token expiry cache eviction without a background sweep job.

---

## 5. API Design

All endpoints use HTTPS. Auth endpoints return JSON. Tokens returned in response body AND optionally as `HttpOnly; Secure; SameSite=Strict` cookies for browser clients. Response headers include `X-Request-ID` for tracing.

---

### POST /auth/v1/register

**Auth:** None  
**Rate Limit:** 5 requests/IP/minute; 1 request/email/hour

```
Request:
  Content-Type: application/json
  {
    "email": "user@example.com",
    "password": "CorrectHorseBatteryStaple42!",
    "display_name": "Alice Smith"    // optional
  }

Response 201 Created:
  {
    "user_id": "uuid",
    "email": "user@example.com",
    "message": "Verification email sent."
  }

Response 400 Bad Request:
  { "error": "validation_error", "details": ["password_too_short", "email_invalid"] }

Response 409 Conflict:
  { "error": "email_already_registered" }
  // IMPORTANT: Return 409 only after sending a notification to the existing email
  // ("someone tried to register with your email") to prevent silent account takeover
  // while also not exposing which emails are registered via 200/400 difference.
```

---

### POST /auth/v1/login

**Auth:** None  
**Rate Limit:** 10 requests/IP/minute; 10 requests/account/15 minutes (sliding window)

```
Request:
  {
    "email": "user@example.com",
    "password": "CorrectHorseBatteryStaple42!",
    "device_fingerprint": "base64-string"   // optional, for session binding
  }

Response 200 OK (no MFA enrolled):
  {
    "access_token": "eyJ...",
    "token_type": "Bearer",
    "expires_in": 900,
    "refresh_token": "dGhpcyBpcyBhIHJlZnJlc2g...",
    "scope": "openid profile"
  }
  Set-Cookie: refresh_token=dGhpcyBpcyBhIHJlZnJlc2g...; HttpOnly; Secure; SameSite=Strict; Path=/auth/v1/token; Max-Age=7776000

Response 200 OK (MFA required):
  {
    "mfa_required": true,
    "mfa_session_token": "eyJ...",   // short-lived JWT, 5-minute TTL, signed HS256 with server secret
    "mfa_methods": ["totp", "webauthn"]
  }

Response 401 Unauthorized:
  { "error": "invalid_credentials" }
  // Always use this regardless of whether email exists (no enumeration)

Response 423 Locked:
  { "error": "account_locked", "retry_after": 1744238400 }

Response 429 Too Many Requests:
  { "error": "rate_limit_exceeded", "retry_after": 60 }
```

---

### POST /auth/v1/mfa/verify

**Auth:** `mfa_session_token` in request body  
**Rate Limit:** 5 requests/mfa_session_token (token is invalidated after 5 failures)

```
Request:
  {
    "mfa_session_token": "eyJ...",
    "method": "totp",
    "code": "847291"
  }

  // For WebAuthn:
  {
    "mfa_session_token": "eyJ...",
    "method": "webauthn",
    "assertion": { ...WebAuthn AuthenticatorAssertionResponse... }
  }

Response 200 OK:
  {
    "access_token": "eyJ...",
    "token_type": "Bearer",
    "expires_in": 900,
    "refresh_token": "dGhpcyBpcyBhIHJlZnJlc2g..."
  }

Response 401 Unauthorized:
  { "error": "invalid_mfa_code" }

Response 400 Bad Request:
  { "error": "mfa_session_expired" }
```

---

### POST /auth/v1/token/refresh

**Auth:** Refresh token (HttpOnly cookie or request body)  
**Rate Limit:** 20 requests/user/minute

```
Request (cookie-based, preferred for browsers):
  Cookie: refresh_token=dGhpcyBpcyBhIHJlZnJlc2g...

  // OR body-based for mobile:
  { "refresh_token": "dGhpcyBpcyBhIHJlZnJlc2g..." }

Response 200 OK:
  {
    "access_token": "eyJ...",
    "token_type": "Bearer",
    "expires_in": 900,
    "refresh_token": "new-token-replacing-old..."
  }
  Set-Cookie: refresh_token=new-token...; HttpOnly; Secure; SameSite=Strict; ...

Response 401 Unauthorized (token not found / already used):
  { "error": "invalid_refresh_token" }
  // If reuse detected → revoke entire family, return:
  { "error": "refresh_token_reuse_detected", "action": "all_sessions_revoked" }

Response 401 Unauthorized (token expired):
  { "error": "refresh_token_expired" }
```

---

### POST /auth/v1/logout

**Auth:** Bearer access token (Authorization header) + refresh token (cookie or body)  
**Rate Limit:** 10 requests/user/minute

```
Request:
  Authorization: Bearer eyJ...
  { "all_devices": false }   // true = revoke all refresh token families for this user

Response 204 No Content
Set-Cookie: refresh_token=; HttpOnly; Secure; SameSite=Strict; Max-Age=0; Path=/auth/v1/token

Response 401 Unauthorized:
  { "error": "invalid_token" }
```

---

### GET /auth/v1/sessions

**Auth:** Bearer access token  
**Pagination:** cursor-based (`?cursor=<opaque>&limit=20`)

```
Response 200 OK:
  {
    "sessions": [
      {
        "session_id": "family_id_uuid",
        "created_at": "2026-04-01T10:00:00Z",
        "last_used_at": "2026-04-09T08:30:00Z",
        "ip_address": "203.0.113.42",
        "user_agent_summary": "Chrome 124 / macOS",
        "is_current": true
      }
    ],
    "next_cursor": null
  }
```

---

### DELETE /auth/v1/sessions/{session_id}

**Auth:** Bearer access token  
**Rate Limit:** 10 requests/user/minute

```
Response 204 No Content   // session revoked
Response 404 Not Found    // session not owned by authenticated user
```

---

### GET /auth/v1/.well-known/jwks.json

**Auth:** None  
**Cache:** `Cache-Control: public, max-age=3600, stale-while-revalidate=600`

```
Response 200 OK:
  {
    "keys": [
      {
        "kty": "RSA",
        "use": "sig",
        "alg": "RS256",
        "kid": "2026-04-01",
        "n": "...",
        "e": "AQAB"
      }
    ]
  }
```

---

### POST /auth/v1/password/forgot

**Auth:** None  
**Rate Limit:** 3 requests/IP/hour; 3 requests/email/hour

```
Request:
  { "email": "user@example.com" }

Response 202 Accepted:
  { "message": "If this email is registered, a reset link has been sent." }
  // Always return 202 regardless of whether email exists (no enumeration)
```

---

### POST /auth/v1/password/reset

**Auth:** Reset token in request body  
**Rate Limit:** 5 requests/token

```
Request:
  {
    "token": "raw-reset-token",
    "new_password": "NewCorrectHorse99!"
  }

Response 200 OK:
  { "message": "Password updated. All sessions revoked." }

Response 400 Bad Request:
  { "error": "token_expired" | "token_already_used" | "password_too_weak" }
```

---

## 6. Deep Dive: Core Components

### 6.1 Refresh Token Rotation with Reuse Detection

**Problem it solves:**  
Refresh tokens are long-lived (90 days) and stored on client devices. If a refresh token is stolen (e.g., XSS extracting from localStorage, malware, or MitM on non-HTTPS), an attacker can silently maintain access by refreshing before the legitimate user does. Rotation alone (issue new token on each use) helps because the stolen token becomes invalid after one use — but only if we detect when both the attacker and the legitimate user try to use it.

**Approaches Compared:**

| Approach | How It Works | Pros | Cons |
|---|---|---|---|
| No rotation (static refresh token) | Same token valid for 90 days | Simple | Stolen token valid indefinitely |
| Rotation without reuse detection | New token on each refresh; old token invalid | Stolen token invalidated after legitimate use | If attacker refreshes first, legitimate user silently loses session |
| Rotation with reuse detection (selected) | Family tracking; reuse of revoked token in family → revoke entire family | Detects theft; alerts both parties; limits blast radius | Legitimate user loses session on detection (UX trade-off accepted for security) |
| Sender-constrained tokens (mTLS / DPoP) | Token bound to client's TLS cert or proof-of-possession key | Stolen token unusable without private key | Complex client implementation; not universally supported |

**Selected: Rotation with reuse detection (family-based)**

Reasoning: Sender-constrained tokens are the strongest approach but require client-side key management (DPoP) or mTLS which is impractical for browser clients today. Family-based reuse detection is a well-established, widely deployed pattern (used by Auth0, Google, GitHub) that requires no client changes and catches the most common theft scenarios.

**Implementation Detail:**

```
Data structures:
  refresh_token_families: { family_id, user_id, created_at, invalidated_at }
  refresh_tokens: { token_id, family_id, token_hash, used_at, replaced_by, revoked_at }

Algorithm: IssueRefreshToken(user_id, family_id=null)
  1. If family_id is null → create new family: family_id = new UUID
  2. raw_token = CSPRNG(32 bytes)  → base64url encode → 43-char string
  3. token_hash = SHA-256(raw_token)  [stored; raw token sent to client]
  4. INSERT refresh_tokens(family_id, user_id, token_hash, expires_at=NOW()+90d)
  5. Return raw_token

Algorithm: UseRefreshToken(raw_token)
  BEGIN SERIALIZABLE TRANSACTION
    1. token_hash = SHA-256(raw_token)
    2. token = SELECT * FROM refresh_tokens WHERE token_hash = ? FOR UPDATE
    3. If not found → ABORT → return INVALID
    4. family = SELECT * FROM refresh_token_families WHERE id = token.family_id
    5. If family.invalidated_at IS NOT NULL → ABORT → return INVALID (already revoked)
    6. If token.used_at IS NOT NULL:
       // REUSE DETECTED: this token was already rotated; someone is replaying old token
       UPDATE refresh_token_families SET invalidated_at=NOW(), 
              invalidation_reason='reuse_detected' WHERE id = family.id
       // Revoke ALL tokens in this family (including current valid one)
       UPDATE refresh_tokens SET revoked_at=NOW() WHERE family_id = family.id
       Emit security alert event (audit log + notify user via email)
       COMMIT
       return REUSE_DETECTED (force re-login)
    7. If token.expires_at < NOW() → ABORT → return EXPIRED
    8. Mark old token used: UPDATE refresh_tokens SET used_at=NOW() WHERE id=token.id
    9. new_token = IssueRefreshToken(token.user_id, token.family_id)
       // sets replaced_by on old token: UPDATE refresh_tokens SET replaced_by=new_token.id
    10. Issue new access token JWT
    COMMIT
  11. Return { new_access_token, new_refresh_token }

Reuse window concern: In distributed deployments, two legitimate concurrent refresh calls 
could theoretically trigger false reuse detection. Mitigation: SERIALIZABLE transaction 
with SELECT FOR UPDATE ensures only one succeeds; the other gets "token already used" and 
should retry with the new token it receives in-band (include new token in error response 
when safe) or re-authenticate.
```

**Interviewer Q&As:**

Q: What if the network drops after the server issues a new refresh token but before the client receives it?  
A: The client still holds the old token (used_at is now set server-side). On retry with the old token, the server will detect reuse and revoke the family. This is a false positive. Mitigation: include the new refresh token in the "reuse detected" response body so the client can recover it. Alternatively, implement a short "grace window" (5 seconds) where the previous token's reuse is tolerated if the replacement token has not been used yet — this reduces false positives from network failures at the cost of a tiny replay window.

Q: Why SHA-256 for token hashing instead of bcrypt?  
A: Bcrypt is appropriate for password hashing because passwords have low entropy and must resist offline dictionary attacks even if the hash database is leaked. Refresh tokens are 32 bytes from a CSPRNG (~256 bits of entropy) — an attacker who gets the hash database has no feasible way to reverse a 256-bit random value regardless of hash speed. SHA-256 lookup is O(1) and does not bottleneck the token refresh hot path. Using bcrypt here would add ~200ms per refresh for no security gain.

Q: How do you handle the 90-day token expiry at scale? Do you run a batch job?  
A: Three mechanisms work together: (1) Redis TTL auto-evicts the hot-path cache entry at expiry; (2) PostgreSQL `expires_at` is checked in every token lookup query — no batch job needed for correctness; (3) A background cleanup job runs nightly during off-peak hours using a cursor-based DELETE with a LIMIT (e.g., `DELETE FROM refresh_tokens WHERE expires_at < NOW() AND id > $cursor LIMIT 1000`) to avoid lock contention. This keeps the table size bounded.

Q: What is the security implication of storing the refresh token hash in PostgreSQL vs. the raw token?  
A: The raw token in the database would allow any engineer with DB read access or any SQL injection to extract and replay tokens. The hash means the database contains only irreversible digests. An attacker who exfiltrates the database cannot derive the original token values (given the high entropy of the tokens). The raw token only ever exists in transport (HTTPS) and on the client device.

Q: Should refresh tokens be stored in HttpOnly cookies or localStorage on the browser?  
A: HttpOnly cookies are strictly preferable. `localStorage` is accessible via JavaScript, meaning XSS vulnerabilities can exfiltrate refresh tokens. HttpOnly cookies are not accessible from JavaScript at all. `SameSite=Strict` further prevents CSRF attacks from triggering token refresh on the user's behalf. The `Path=/auth/v1/token` restriction means the cookie is only sent to the token refresh endpoint, minimizing exposure. The only downside is that cookie-based tokens require explicit CORS handling for cross-origin SPAs, which is a manageable engineering concern.

---

### 6.2 Argon2id Password Hashing with Pepper

**Problem it solves:**  
User passwords are low-entropy secrets. If the `users` table is compromised via SQL injection or insider access, raw or weakly hashed passwords can be cracked offline at GPU scale. The hashing function must be memory-hard (to defeat GPU/ASIC parallelism) and must include a server-side secret (pepper) so that even a full database dump is insufficient for offline cracking.

**Approaches Compared:**

| Algorithm | Memory-Hard | Time Cost Tunable | GPU Resistance | Salt Support | Notes |
|---|---|---|---|---|---|
| MD5 / SHA-1 | No | No | Poor | Manual | Never acceptable |
| bcrypt | No (only 4 KB memory) | Yes (cost factor) | Moderate | Built-in | Widely deployed; 72-byte password limit |
| scrypt | Yes | Yes (N, r, p) | Good | Built-in | Less audited; hard to tune safely |
| PBKDF2 | No | Yes (iterations) | Poor (GPU-friendly) | Built-in | Required for FIPS; otherwise weak |
| **Argon2id** | **Yes** | **Yes (m, t, p)** | **Excellent** | **Built-in** | **Winner of PHC 2015; OWASP recommended** |

**Selected: Argon2id**

Justification: Argon2id is the hybrid variant (combines Argon2i's data-independent memory access with Argon2d's data-dependent access), providing the best resistance to both side-channel attacks (relevant in cloud multi-tenant environments) and GPU-based offline cracking. It is the OWASP ASVS recommended algorithm and the RFC 9106 standard.

**Recommended Parameters (per OWASP 2023 guidance):**
- Memory: `m = 19456` (19 MiB) — fills a significant fraction of a GPU's per-core shared memory
- Iterations: `t = 2`
- Parallelism: `p = 1`
- Output length: 32 bytes
- Salt: 16 bytes CSPRNG per password (built-in to PHC string format)
- Resulting PHC string: `$argon2id$v=19$m=19456,t=2,p=1$<salt_b64>$<hash_b64>`

These parameters yield ~100 ms hash time on a single modern CPU core — fast enough for user experience, slow enough to make offline cracking economically infeasible (attacker can compute ~10 hashes/second/GPU vs. 100B MD5/second/GPU).

**Pepper:**

```
Concept:
  stored_value = Argon2id(password + pepper, salt, m, t, p)
  pepper = 32-byte secret stored in HSM/KMS, NOT in the database

Why: If only the DB is compromised (not the application secrets), the attacker 
cannot crack passwords because they don't know the pepper. The pepper acts as a 
second factor for the hashing computation.

Pepper rotation:
  Cannot re-hash without the old pepper.
  Approach: Store pepper_version with each hash.
  On login (success), if pepper_version < current_version:
    re-hash with new pepper and update record.
  This allows gradual rotation across active users on their next login.

Implementation:
  function HashPassword(password, pepper_version=current):
    pepper = KMS.GetSecret(f"auth-pepper-v{pepper_version}")
    salt = CSPRNG(16)
    hash = Argon2id(password=password+pepper, salt=salt, m=19456, t=2, p=1)
    return f"$argon2id$v=19$m=19456,t=2,p=1$pv={pepper_version}${b64(salt)}${b64(hash)}"

  function VerifyPassword(password, stored_hash):
    pepper_version = parse_pepper_version(stored_hash)
    pepper = KMS.GetSecret(f"auth-pepper-v{pepper_version}")
    return Argon2id_verify(password + pepper, stored_hash)
```

**Interviewer Q&As:**

Q: Why Argon2id over bcrypt if bcrypt is widely considered "good enough"?  
A: bcrypt's memory usage is fixed at 4 KB regardless of cost factor — this makes it vulnerable to GPU cracking where each GPU core has its own 4 KB memory. Argon2id with 19 MiB requires memory that exceeds the per-core cache on modern GPUs, forcing serial execution on GPU hardware and reducing the attacker's parallelism advantage by orders of magnitude. With bcrypt at cost 12, a modern GPU can crack ~1,000 hashes/second. With Argon2id at 19 MiB, that drops to ~50-100 hashes/second on the same hardware. Additionally, bcrypt truncates passwords at 72 bytes, which is a surprising limitation that could affect long passphrase users.

Q: What happens if the KMS is unavailable during login?  
A: The auth service cannot verify passwords without the pepper (the hash cannot be verified without it). Options: (1) Fail closed — return 503 during KMS outage. This is the secure default. (2) Cache the pepper in-process memory for a short TTL (e.g., 5 minutes) with the understanding that this trades availability for a brief exposure window if memory is dumped. Most deployments use option (2) with a short TTL to tolerate transient KMS hiccups. The pepper is not a secret that needs to be hidden from the application process — it needs to be hidden from the database. In-memory caching is acceptable. KMS is designed for 99.999%+ availability with regional redundancy.

Q: How do you handle the case where a user has a hash from an older algorithm (e.g., MD5 from a legacy migration)?  
A: Detect hash algorithm from the stored string prefix (MD5 hashes don't start with `$argon2id$`). On successful login with any algorithm: immediately re-hash the password with Argon2id and update the stored hash. This is called "on-the-fly migration." Never store MD5 hashes alongside Argon2id — migrate all new logins. For users who never log in, a forced password reset email campaign may be necessary after some deprecation period.

Q: How do you prevent timing attacks on the email lookup before the password hash verification?  
A: Two mitigations: (1) If the user is not found, still execute a dummy Argon2id computation (using a pre-computed hash of a fake password) to make the response time indistinguishable from a valid-user-wrong-password response. (2) Always return the same error message ("invalid credentials") regardless of whether the email exists or the password is wrong. Without the dummy hash, an attacker could enumerate valid emails by measuring response times: found users take ~100ms (hash compute), not-found users take ~1ms (immediate return).

Q: What is the password strength policy and why those specific rules?  
A: The policy is: minimum 12 characters, no maximum (avoid bcrypt truncation issues), check against HaveIBeenPwned (HIBP) k-anonymity API (send first 5 chars of SHA-1, receive matches, check locally), block the top 10,000 commonly used passwords. NIST SP 800-63B (2017) explicitly recommends against forced complexity rules (uppercase + number + symbol requirements) because they lead to predictable substitution patterns (P@ssw0rd!) and frequent password reuse. Length and breach-corpus checking provide stronger real-world security. Complexity is optional but encouraged via a visual strength meter, not enforcement.

---

### 6.3 TOTP MFA Implementation

**Problem it solves:**  
Passwords alone are susceptible to phishing, credential stuffing, and database breaches. TOTP (Time-based One-Time Passwords, RFC 6238) adds a "something you have" factor that is not transmittable by a phisher capturing a password form (the code expires in 30 seconds and is derived from a secret only the authenticator app holds).

**Approaches Compared:**

| Method | Phishing Resistant | UX | Implementation Complexity | Recovery Risk |
|---|---|---|---|---|
| SMS OTP | No (SIM swap, SS7) | Medium | Low | High |
| Email OTP | No (email account hijack) | Medium | Low | Medium |
| TOTP (RFC 6238) | Partial (real-time phishing possible) | Good | Medium | Requires backup codes |
| **WebAuthn/FIDO2** | **Yes (origin-bound)** | **Excellent (biometric)** | **High** | **Complex** |
| Push notifications | No (MFA fatigue attacks) | Good | High (push infra) | Medium |

**Selected: TOTP as default + WebAuthn as premium option**

TOTP is selected as the primary option because: it works offline, requires no server-side infrastructure beyond a secret and time, is universally supported (Google Authenticator, Authy, 1Password), and is free of dependency on SMS carriers or push notification services. WebAuthn is offered for users who enroll a hardware key (YubiKey) or platform authenticator (Face ID, Touch ID) and provides phishing-resistant authentication.

**TOTP Implementation:**

```
Enrollment:
  1. Generate TOTP secret: secret = CSPRNG(20 bytes) → base32 encode (160-bit secret)
     Why 20 bytes: RFC 6238 recommends SHA-1 HMAC which takes any key length; 
     20 bytes = 160 bits = same as SHA-1 output length, convention-aligned.
  2. Encrypt secret: encrypted_secret = AES-256-GCM(secret, kms_data_key)
  3. Store in mfa_enrollments(totp_encrypted_secret, totp_key_id)
  4. Return to client: otpauth://totp/ServiceName:user@example.com?secret=BASE32&issuer=ServiceName
  5. Client scans QR code into authenticator app
  6. Verify enrollment: client submits first code to confirm setup worked

Verification:
  function VerifyTOTP(user_id, submitted_code):
    secret = AES_GCM_decrypt(KMS.GetKey(totp_key_id), totp_encrypted_secret)
    
    // Allow ±1 window (±30 seconds) to account for clock skew
    current_window = floor(unix_timestamp() / 30)
    for window_offset in [-1, 0, 1]:
      expected_code = HOTP(secret, counter=current_window + window_offset)
      if expected_code == submitted_code:
        // Replay prevention: reject if this (window, code) combo already used
        code_key = SHA256(user_id + window + code)
        if Redis.EXISTS(code_key): return REPLAY_DETECTED
        Redis.SET(code_key, 1, EX=90)  // 90s TTL covers ±1 window
        return SUCCESS
    return INVALID

  function HOTP(secret, counter):
    hmac = HMAC-SHA1(key=secret, message=counter_as_8_byte_big_endian)
    offset = hmac[19] & 0x0F
    code = (hmac[offset:offset+4] as uint32 & 0x7FFFFFFF) % 1_000_000
    return zero_padded_6_digit_string(code)
```

**Backup Codes:**  
Generate 10 single-use codes (8 characters each, alphanumeric, CSPRNG). Store SHA-256 hashes. Present to user once at enrollment with instruction to store securely. Each code is single-use (mark `used_at` on consumption).

**Interviewer Q&As:**

Q: What is the real-time phishing vulnerability of TOTP and how do you mitigate it?  
A: A sophisticated attacker can run a real-time proxy phishing site: user submits credentials + TOTP to fake site, fake site immediately relays to real site, gets access within the 30-second window. Mitigation: (1) WebAuthn/FIDO2 is origin-bound and completely immune — the authenticator cryptographically signs the RP origin, so a phishing domain gets a signature that is useless on the real domain. (2) For TOTP, implement login anomaly detection (new device/IP triggers additional verification), short session binding, and employee training. The fundamental protection TOTP provides is against offline credential database attacks and automated credential stuffing, not real-time phishing.

Q: How do you handle MFA recovery if a user loses their authenticator device?  
A: Recovery options in order of security: (1) Pre-enrolled backup codes (strongest — cryptographically random, generated at MFA setup time). (2) Alternative second factor (e.g., if user enrolled both TOTP and WebAuthn, use the other). (3) Account recovery flow requiring identity verification: submit government ID + selfie to support team, which reviews manually and revokes all MFA + initiates re-enrollment. There is no automated "forgot MFA" flow that bypasses the second factor — that would defeat the purpose of MFA. Recovery codes are the designed escape hatch.

Q: Why is SMS OTP not recommended as a primary MFA method?  
A: Three attack vectors: (1) SIM swapping — attacker social-engineers the carrier into transferring the victim's phone number to attacker's SIM. No technical defense possible from the relying party side. (2) SS7 protocol vulnerabilities — the telephony signaling protocol has known vulnerabilities allowing interception of SMS messages by nation-state actors or sophisticated criminals with SS7 access. (3) SIM cloning — physical access to phone + specialized hardware. Additionally, SMS OTP is vulnerable to real-time phishing and malware on the phone. NIST SP 800-63B deprecated SMS OTP as a "restricted authenticator" in 2017 citing these risks.

Q: How do you prevent TOTP enrollment being abused if the user's account is already compromised?  
A: Require re-authentication (password re-entry or current session MFA) before enrolling a new MFA method. Never allow MFA enrollment from a session that was created via a password reset link without explicit re-authentication. Additionally: send a push notification / email to all enrolled email addresses when a new MFA factor is enrolled, allowing the legitimate user to detect and revoke unauthorized enrollment. WebAuthn enrollment additionally requires physical presence (user gesture on the authenticator device) which prevents remote enrollment by a session hijacker.

Q: What is the difference between TOTP and HOTP, and when would you use HOTP?  
A: HOTP (RFC 4226) is counter-based: codes are derived from an incrementing counter. The authenticator and server must stay synchronized on the counter. TOTP (RFC 6238) extends HOTP by deriving the counter from the current Unix timestamp divided by the time step (30 seconds). TOTP is preferred for end-user MFA because: (1) no counter synchronization complexity, (2) codes expire automatically, (3) a stolen TOTP code is only valid for at most 90 seconds (±1 window). HOTP is used in hardware tokens that don't have a clock (e.g., some YubiKey OTP modes, OATH event-based tokens). The counter-drift problem in HOTP (server must accept N codes ahead) can allow wider replay windows.

---

## 7. Scaling

### Horizontal Scaling

**Auth Service (stateless):** Deploy behind a load balancer. All state is in backing stores. Add pods as CPU/memory thresholds are crossed. Kubernetes HPA on CPU > 70% or custom metric (auth_requests_per_second > threshold). No sticky sessions required.

**Token Validation (JWT):** Completely stateless — each API gateway node validates tokens locally using the cached RSA public key from JWKS. JWKS is cached in-process with 1-hour TTL and a background goroutine/thread refreshes it. No network hop per token validation. This path scales linearly with API gateway instances.

**JWKS Endpoint:** Served from CDN (CloudFront/Fastly). Cache-Control: max-age=3600. Key rotation publishes new key with `kid` and old key is kept for 15 minutes (access token TTL) to drain in-flight tokens. CDN serves 99%+ of JWKS requests without hitting origin.

### Database Sharding

**User Store (PostgreSQL):**
- Initial deployment: primary + 2 read replicas. Read replicas handle login lookups (SELECT by email) with replication lag < 1 second (acceptable; password verification is not time-critical).
- Sharding strategy: shard by `user_id` (UUID) using consistent hashing. UUIDs are uniformly distributed. Use Citus (PostgreSQL sharding extension) or PgBouncer routing to shard nodes.
- 500M users at 512 bytes = 256 GB — fits on a single large PostgreSQL instance (1 TB NVMe). Sharding is a future concern, not day-one requirement.

**Refresh Token Store (Redis + PostgreSQL):**
- Redis Cluster: 3 primary + 3 replica nodes. Shard by `token_hash` prefix. Redis Cluster handles automatic resharding.
- PostgreSQL: same shard key as user_id (tokens belong to users; queries are always by user_id or token_hash).

### Replication

| Store | Strategy | RPO | RTO |
|---|---|---|---|
| User Store (PostgreSQL) | Synchronous streaming replication to 1 standby; async to 1 replica | 0 (synchronous standby) | < 30s (auto-failover via Patroni) |
| Refresh Token Store (PostgreSQL) | Async replication to 1 standby | < 5s | < 60s |
| Redis | Redis Cluster with 1 replica per shard; AOF enabled | < 1s | < 10s (replica promoted) |
| Audit Log (Kafka) | Replication factor 3, min.insync.replicas=2 | 0 (synchronous acks) | N/A (Kafka is log; consumers replay) |

### Caching

| Data | Cache | TTL | Invalidation |
|---|---|---|---|
| JWKS public keys | In-process (per service instance) | 1 hour | Background refresh |
| Rate limit counters | Redis | Per-window TTL (60s) | Atomic INCR+EXPIRE |
| Refresh token validity | Redis (hot path) | Until token expiry | Explicit DEL on revocation |
| Account lock state | Redis | Lock duration | DEL on unlock |
| TOTP replay prevention codes | Redis | 90 seconds | TTL-based eviction |

### Interviewer Q&As on Scaling

Q: How do you handle JWT token revocation at scale if JWTs are stateless?  
A: JWTs are intentionally short-lived (15 minutes) to bound the revocation gap. For immediate revocation (account suspension, security incident): maintain a JWT blocklist in Redis keyed by `jti` (JWT ID) with TTL equal to the remaining token lifetime. At 50,000 active revoked tokens × 64 bytes each = 3.2 MB in Redis — negligible. The API gateway checks the blocklist on every request. Alternatively, use "generation numbers": store a `token_generation` counter per user in Redis; include the generation in the JWT. Increment the counter to invalidate all current JWTs for a user without per-token storage. The trade-off: generation numbers invalidate ALL tokens for a user simultaneously (no selective revocation), while per-`jti` blocklisting allows targeted revocation.

Q: At what scale does PostgreSQL for refresh tokens become a bottleneck, and what's the migration path?  
A: At 4,340 refresh RPS peak, each requiring 1 write + 1 read from PostgreSQL: a single well-tuned PostgreSQL instance on NVMe can handle ~50,000 simple transactional TPS. Redis handles the hot path (95%+ of reads). PostgreSQL sees primarily writes and durable reads. Bottleneck likely appears around 50,000+ RPS. Migration path: (1) First, ensure all reads go through Redis (write-through cache). (2) Shard PostgreSQL by `user_id` using Citus or application-level routing. (3) Long term: consider Cassandra for the refresh token write path — it's append-optimized and naturally handles high write throughput with tunable consistency. The trade-off is losing SERIALIZABLE transactions, requiring application-level reuse detection logic.

Q: How do you prevent the JWKS cache from causing availability issues during key rotation?  
A: Key rotation procedure: (1) Generate new RSA key pair, publish new key in JWKS with new `kid` alongside old key. (2) Wait 15 minutes (access token TTL) — all in-flight tokens signed with old key will expire. (3) Start signing new access tokens with new key. (4) Wait another 1 hour (JWKS cache TTL) — all services have refreshed JWKS and know the new public key. (5) Remove old public key from JWKS. If a service validates a token signed with an old `kid` not in its cached JWKS: trigger an immediate JWKS refresh (on-demand cache invalidation via cache-key-based refresh). This is why `kid` in the JWT header is critical — it lets the validator know exactly which key to use.

Q: How do you scale Argon2id computation across pods without each pod being a bottleneck?  
A: Each Argon2id computation takes ~100 ms and uses ~19 MiB of memory. A single pod with 4 CPU cores can handle 4 concurrent Argon2id computations = 40 logins/second. At 1,447 peak login RPS, we need: 1,447 / 40 = ~37 pods. Each pod needs: 4 CPU cores + 19 MiB × 4 concurrent = 76 MiB for active hashes + overhead = ~2 GB RAM per pod. 37 pods × 4 CPU = 148 CPU cores dedicated to login. This is the primary scaling driver for the auth service. Optimization: use an async task queue (separate "password verification worker" pool) to avoid blocking HTTP handler threads. The HTTP handler submits a verification job and long-polls or uses a callback, keeping the HTTP connection pool healthy even under burst load.

Q: How do you handle database connection pool exhaustion during traffic spikes?  
A: Several layers: (1) PgBouncer connection pooling in transaction mode: multiple application threads share a small pool of actual PostgreSQL connections. Each transaction holds a connection only for its duration. (2) Auth service pods have connection pool limits (e.g., max 20 connections per pod, 37 pods = 740 logical connections → PgBouncer serves from a pool of 100 actual PostgreSQL connections). (3) Circuit breaker on the DB connection pool: if pool exhausted for > 500ms, return 503 to clients rather than queuing indefinitely. (4) Read replicas offload SELECT queries for non-critical paths (session listing, audit log reads). (5) Redis handles the hot path (rate limit, token validity) without touching PostgreSQL.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| Auth Service pod crash | Login/refresh unavailable for in-flight requests | LB health check (15s) | Pod auto-restart (Kubernetes); LB routes around; 3+ replicas ensures no full outage |
| PostgreSQL primary failure | Logins fail (cannot read credential hash) | Patroni health check (< 10s) | Patroni auto-promotes standby; < 30s failover; read replicas serve reads during promotion |
| Redis cluster node failure | Rate limits disabled; token cache miss | Redis Cluster health check | Redis Cluster auto-promotes replica; fallback: bypass cache and read PostgreSQL (slower but correct) |
| KMS/HSM unavailability | Cannot verify passwords (no pepper) or decrypt TOTP secrets | HTTP 503 from KMS | In-process pepper cache (5 min TTL) tolerates transient outages; circuit break after 30s |
| JWKS endpoint unavailable | New API gateway instances cannot fetch public key | Synthetic monitor | CDN serves cached JWKS; existing in-process caches serve for up to 1 hour |
| Kafka unavailable | Audit log events lost | Kafka health check | Auth service buffers events in local memory queue (up to 10,000 events); retries with backoff; fails open (auth continues, audit may lag) |
| Clock skew > 90 seconds | TOTP codes rejected | NTP monitoring alert | NTP sync enforced on all nodes; alert if drift > 1 second; the ±1 TOTP window handles up to 30s drift |
| Refresh token store corruption | Users cannot refresh; forced re-login | Data integrity check | Restore from PostgreSQL (authoritative source); Redis is cache layer only |
| Account lockout storm (DDoS) | Mass lockouts of legitimate users | Spike in 423 responses | Lock accounts by IP first; implement CAPTCHA at lower threshold; require CAPTCHA, not lockout, if >1000 distinct accounts locked in 1 minute |

### Retries and Idempotency

- **Refresh token endpoint**: Idempotent by design for network retries within the grace window. If the client retries with the original token within 5 seconds of a timeout and the new token has not been used yet, return the same new token (look up `replaced_by` from the old token record).
- **Registration endpoint**: Idempotent on email: if re-submitted with same email during the "pending verification" state, re-send verification email rather than creating a duplicate record.
- **Password reset request**: Idempotent — if an unexpired reset token exists for the user, either return the same expiry time or issue a new token and invalidate the old one (prefer the latter to avoid token accumulation).
- **Audit log writes**: Kafka producer uses idempotent producer (`enable.idempotence=true`). Duplicate events filtered at consumer by `event_id` deduplication in ClickHouse.

### Circuit Breaker

```
Implemented per downstream dependency (PostgreSQL, Redis, KMS):

State machine: CLOSED → OPEN → HALF_OPEN → CLOSED

CLOSED: requests pass through normally
  Failure counter increments on error.
  If failure_count > 5 in last 10s → transition to OPEN.

OPEN: requests fail immediately (fast fail, return 503)
  After 30s → transition to HALF_OPEN.

HALF_OPEN: allow 1 probe request.
  If success → CLOSED (reset counters).
  If failure → back to OPEN.

Specific behaviors:
  - KMS circuit open: use cached pepper if available; reject logins if cache expired.
  - Redis circuit open: bypass rate limiting (risk accepted: attackers may get through
    during outage); fall back to PostgreSQL for token validation (slower, correct).
  - PostgreSQL circuit open: return 503 for login/refresh; JWT validation 
    (stateless) unaffected.
```

---

## 9. Monitoring & Observability

### Metrics

| Metric | Type | Alert Threshold | Meaning |
|---|---|---|---|
| `auth_login_requests_total{result="success|failure|locked|rate_limited"}` | Counter | Failure rate > 30% → warn | Login health |
| `auth_login_duration_seconds` | Histogram (p50/p95/p99) | p99 > 500ms → warn | Latency SLO |
| `auth_password_hash_duration_seconds` | Histogram | p99 > 400ms → warn | Argon2id timing |
| `auth_refresh_requests_total{result}` | Counter | Failure rate > 5% → alert | Refresh health |
| `auth_reuse_detection_total` | Counter | Any > 0 in 1 min → alert | Security incident signal |
| `auth_mfa_verify_total{method,result}` | Counter | Failure rate > 50% → alert | MFA health |
| `auth_account_lockouts_total` | Counter | > 1000/min → alert (DDoS) | Brute force detection |
| `auth_token_revocations_total{reason}` | Counter | Spike in 'admin' reason → alert | Incident response |
| `auth_rate_limit_hits_total{dimension="ip|account"}` | Counter | IP hits > 10,000/min → alert | DDoS / spray attack |
| `auth_db_connection_pool_available` | Gauge | < 20% → alert | Pool exhaustion |
| `auth_redis_command_duration_seconds` | Histogram | p99 > 10ms → warn | Redis latency |
| `auth_kms_call_duration_seconds` | Histogram | p99 > 100ms → warn | KMS latency |
| `auth_jwks_cache_hit_ratio` | Gauge | < 0.99 → warn | JWKS cache effectiveness |

### Distributed Tracing

- **Instrumentation**: OpenTelemetry SDK injected into Auth Service. Every inbound request creates a trace span.
- **Trace spans**: `login_handler` → `db_user_lookup` → `argon2_verify` → `mfa_verify` → `token_issue` → `db_token_write` → `audit_emit`.
- **Propagation**: W3C `traceparent` header propagated to PostgreSQL via `application_name` session variable and to Kafka via message headers. Enables end-to-end trace: client request → auth service → DB query → Kafka message.
- **Sampling**: 100% sampling for error traces; 1% for success traces (auth is high-volume). Tail-based sampling captures full traces for p99 outliers.
- **Backend**: Jaeger or AWS X-Ray. Retention: 7 days for detail traces, 30 days for aggregated.

### Logging

- **Format**: Structured JSON, per-line. Fields: `timestamp`, `level`, `service`, `trace_id`, `span_id`, `user_id` (masked: first 8 chars of UUID), `event`, `duration_ms`, `ip_address`, `result`.
- **Security logging rules**:
  - Never log passwords, tokens, secrets, PII in plain text.
  - Refresh tokens in logs: log only `SHA256(token)[0:16]` (first 16 hex chars of hash) — enough to correlate logs without exposing the token.
  - IP addresses: log in full for security analysis; mask last octet for non-security dashboards (GDPR compliance).
- **Log levels**: ERROR for unexpected failures (DB down, KMS error); WARN for security events (lockout, rate limit, reuse detection); INFO for normal auth events (login success/failure, refresh, logout).
- **Log pipeline**: Fluentd → Kafka → Elasticsearch (7-day hot) + S3 (90-day warm, 7-year cold). Kibana for search; CloudWatch Insights for operations.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen Approach | Alternative | Trade-off |
|---|---|---|---|
| Token format (access token) | JWT (RS256, stateless) | Opaque token with introspection | JWT: fast stateless validation at gateway scale; opaque: easier revocation but requires network call per request |
| Refresh token storage | SHA-256 hash in DB | Opaque lookup by raw token in DB | Hash: DB breach doesn't expose tokens; raw: simpler but catastrophic if leaked |
| Password hashing | Argon2id + pepper | bcrypt cost=12 | Argon2id: memory-hard, defeats GPU; pepper: layer-2 protection; bcrypt simpler but weaker against modern GPUs |
| Refresh token store hot path | Redis (cache) + PostgreSQL (durable) | Redis only | Redis+PG: durable with fast hot path; Redis only: data loss risk on crash despite persistence config |
| MFA primary method | TOTP | SMS OTP | TOTP: no carrier dependency, offline, free; SMS: better UX but SIM-swap vulnerable |
| Token revocation for access tokens | Short TTL (15 min) + jti blocklist | Long TTL + active introspection per request | Short TTL bounds revocation gap without network hop; blocklist handles immediate revocation for small % of cases |
| Reuse detection response | Revoke entire token family | Revoke only the reused token | Family revocation: breaks attacker's session; higher legitimate user impact; single-token revocation leaves attacker's newer token valid |
| Brute force protection | Rate limit (Redis) + exponential lockout | CAPTCHA only | Rate limit + lockout: automated defense; CAPTCHA: better UX but solvable by humans/CAPTCHA farms |
| Cookie vs localStorage for refresh token | HttpOnly cookie | localStorage | Cookie: XSS-immune; localStorage: simpler for mobile/native apps (which use secure storage anyway) |
| Audit log storage | Kafka → ClickHouse + S3 | Write directly to PostgreSQL | Kafka: decoupled, non-blocking, durable; direct PG write: simpler but auth-critical path would be slowed by audit writes |

---

## 11. Follow-up Interview Questions

**Q1: How would you implement "remember me" / "stay logged in" functionality securely?**  
A: "Remember me" extends the refresh token TTL (e.g., 90 days vs 1 day for standard session) and marks the token family with a `long_lived=true` flag. The access token TTL remains 15 minutes regardless. On the server side, store the device binding (user-agent hash, IP range) and alert the user if the long-lived token is used from a significantly different context. Never extend access token TTL for "remember me" — that defeats the purpose of short-lived access tokens.

**Q2: How do you design the account lockout to be resistant to denial-of-service abuse?**  
A: A naive lockout that locks on 10 failures allows an attacker to lock any account by intentionally failing 10 times. Mitigations: (1) Implement CAPTCHA at 5 failures (before lockout), making automated lockout attacks expensive. (2) Use progressive delay instead of full lockout: 1s after 5 failures, 10s after 7, 60s after 9, lockout after 10. (3) Only apply lockout from suspicious IPs (new IP for account + failure pattern) — allow trusted IPs (recognized device) a higher threshold. (4) Offer real-time notification to the user's email on lockout so the legitimate user can self-unlock via email link, reducing support burden.

**Q3: What is the difference between `sub` and `jti` in a JWT and how are both used?**  
A: `sub` (subject) is the user identifier — who the token represents (e.g., user UUID). It is not unique across tokens; multiple tokens for the same user share the same `sub`. `jti` (JWT ID) is a unique identifier for the specific token instance — used for revocation (add `jti` to blocklist) and for idempotency/logging correlation. `jti` should be a CSPRNG-generated UUID or random string to prevent prediction.

**Q4: How do you handle cross-device logout (logout from all devices)?**  
A: Two approaches: (1) Token family revocation — revoke all refresh token families for the user. Active access tokens (up to 15 min) remain valid but cannot be refreshed; effectively dead within 15 minutes. (2) User generation counter — store a `session_generation` integer per user in Redis. Embed the generation in the JWT. On "logout all," increment the counter. Token validation checks JWT generation against current generation; mismatched tokens rejected immediately. Option 2 provides immediate logout for all access tokens without per-token storage. Combine both: increment generation (immediate access token invalidation) + revoke all refresh token families.

**Q5: How would you implement account takeover detection?**  
A: Behavioral signals: (1) Login from new country/IP not seen in past 90 days → require MFA re-verification even if session is within TTL. (2) Login from impossible travel: two logins from cities 5,000km apart within 1 hour → alert + require MFA. (3) Password change followed by email change within 10 minutes → suspicious; require email verification with link to old address. (4) Bulk API calls after login from a new IP (scraping/data exfiltration pattern) → rate limit + alert. (5) Login at unusual hour (3 AM local time based on timezone inference from IP) → soft challenge. Implement as an async risk scoring pipeline (ML model or rule engine) that runs after login succeeds and can step up auth or trigger alerts.

**Q6: How do you rotate the RSA signing key without service disruption?**  
A: See Section 7, Q3 for the procedure. Key: dual-key publication in JWKS, use `kid` to distinguish keys, 15-minute overlap (access token TTL), 1-hour cache drain period. Automate with a script that: (1) generates new key pair, (2) updates KMS/Vault, (3) publishes to JWKS, (4) triggers JWKS cache invalidation, (5) waits and validates, (6) removes old key. Key rotation should happen every 90 days per NIST recommendation.

**Q7: What attack does PKCE protect against and is it relevant to the auth service?**  
A: PKCE (Proof Key for Code Exchange) protects against the authorization code interception attack in public clients (mobile apps) where an attacker intercepts the authorization code from the redirect URI. It is relevant to the OAuth2 provider (covered in `oauth2_provider.md`), not the direct login flow of the auth service. The auth service's direct login endpoint is not susceptible to this attack since there is no redirect/code exchange step.

**Q8: How do you design the auth service to be testable and support staging environments?**  
A: (1) Configurable Argon2id parameters per environment: staging uses very low cost (m=64, t=1) so tests run fast. (2) KMS abstraction interface with a local (in-memory) implementation for testing. (3) Deterministic CSPRNG injection for test scenarios. (4) All time-dependent logic (token expiry, TOTP) injectable via a clock interface. (5) Synthetic user accounts with known passwords in staging, auto-pruned after 24 hours. (6) Separate Redis namespace per environment (key prefix) to prevent staging from polluting production rate limit counters.

**Q9: What is credential stuffing and how does the auth service defend against it?**  
A: Credential stuffing is using large databases of leaked username/password pairs (from other breaches) to automate login attempts at scale. Defenses: (1) Per-account rate limiting — even with valid credentials, lockout after 10 failures limits the blast radius. (2) CAPTCHA after N failures per IP — adds cost to automation. (3) IP reputation scoring — block or challenge IPs from known bot networks, hosting providers, and Tor exit nodes using a threat intelligence feed. (4) Device fingerprinting — require MFA for logins from unrecognized devices. (5) Check submitted passwords against HaveIBeenPwned at registration — prevents users from using compromised passwords. (6) Anomaly detection — login from a residential IP block using a credential pair in a sequence matching a known leaked database is a red flag even on first attempt.

**Q10: How do you handle multi-tenancy in the auth service if you serve multiple product lines?**  
A: Add a `tenant_id` column to all tables. JWT includes `tid` (tenant ID) claim. JWKS can be tenant-specific (different signing keys per tenant for isolation) or shared with tenant claim validation. Rate limits applied per `(tenant_id, ip)` pair. Tenant-specific policies: some tenants may require MFA enforcement, different password policies, or custom lockout thresholds. Tenant isolation at the DB level: row-level security policies in PostgreSQL enforce that queries from tenant A cannot read tenant B's data even if the application has a bug. Separate databases for enterprise tenants with strict data residency requirements.

**Q11: What is the "not before" (`nbf`) claim in JWT and when would you use it?**  
A: `nbf` specifies the time before which the token must not be accepted. Use cases: (1) Issue a token for a future operation (scheduled access) — e.g., a token for a maintenance window that starts in 1 hour. (2) Clock skew tolerance — ensure the token is not used immediately after issuance if there is a known distribution delay. In practice, `nbf` is rarely used in standard access token flows; `iat` and `exp` are sufficient. Include `nbf = iat` as a safe default to explicitly state "valid from issuance."

**Q12: How do you prevent email enumeration through the login endpoint?**  
A: (1) Return the same error message and HTTP status code for "email not found" and "wrong password" cases. (2) Ensure the same response time for both cases by executing a dummy Argon2id hash when the email is not found. (3) Use `202 Accepted` for password reset requests regardless of whether the email exists. (4) On registration, if the email already exists, send a notification to that email rather than returning 409 immediately — though 409 may be returned after the notification is sent to inform the registering client. (5) Rate-limit registration probing (limit attempts per IP).

**Q13: How would you implement a "suspicious login" email notification without blocking the auth flow?**  
A: Emit a login event to Kafka with the relevant metadata (IP, user-agent, geolocation, is_new_device). A separate notification consumer service subscribes to login events, applies risk rules (new country, new device, impossible travel), and sends transactional email if rules trigger. This is fully asynchronous — does not add latency to the login path. The auth service does not wait for the notification to send. If risk is high enough to block login (step-up auth required), the risk score can be computed synchronously pre-login using a lightweight rule check, with expensive ML scoring done async.

**Q14: What is the minimum viable auth service for an MVP, and what security shortcuts are acceptable?**  
A: MVP: (1) Argon2id password hashing — non-negotiable from day 1, trivial to implement, catastrophic to retrofit. (2) HTTPS only — non-negotiable. (3) JWT access tokens + opaque refresh tokens — basic rotation is a few hours of work. (4) Rate limiting on login endpoint — Redis counter, 1 hour of work, prevents the most common attacks. Acceptable deferrals: MFA (add after launch), WebAuthn (complex, defer), key rotation automation (manual initially), advanced anomaly detection, full audit pipeline (log to files initially). Never acceptable to defer: password hashing strength, HTTPS, input validation, SQL injection prevention.

**Q15: How does the auth service interact with a downstream authorization service?**  
A: The auth service is responsible for authentication only (who are you?). Authorization (are you allowed to do X?) is a separate concern. The JWT access token carries claims: `sub` (user ID), `roles` or `permissions` (scopes), `tenant_id`, `amr` (authentication method reference — e.g., ["pwd","otp"] means password + OTP). Downstream services extract these claims from the validated JWT to make authorization decisions. A separate authorization service (e.g., OPA, Casbin, or custom RBAC service) evaluates policies against these claims. The auth service does not need to know about specific resource permissions — it only asserts the user's identity and granted scopes.

---

## 12. References & Further Reading

- **NIST SP 800-63B** — Digital Identity Guidelines: Authentication and Lifecycle Management. https://pages.nist.gov/800-63-3/sp800-63b.html
- **RFC 9106** — Argon2 Memory-Hard Function for Password Hashing and Proof-of-Work Applications. https://www.rfc-editor.org/rfc/rfc9106
- **RFC 6238** — TOTP: Time-Based One-Time Password Algorithm. https://www.rfc-editor.org/rfc/rfc6238
- **RFC 7519** — JSON Web Token (JWT). https://www.rfc-editor.org/rfc/rfc7519
- **RFC 7517** — JSON Web Key (JWK). https://www.rfc-editor.org/rfc/rfc7517
- **OWASP ASVS 4.0** — Application Security Verification Standard. https://owasp.org/www-project-application-security-verification-standard/
- **OWASP Authentication Cheat Sheet**. https://cheatsheetseries.owasp.org/cheatsheets/Authentication_Cheat_Sheet.html
- **OWASP Password Storage Cheat Sheet**. https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html
- **WebAuthn Specification (W3C)**. https://www.w3.org/TR/webauthn-2/
- **Auth0 — Refresh Token Rotation**. https://auth0.com/docs/secure/tokens/refresh-tokens/refresh-token-rotation
- **Troy Hunt — HaveIBeenPwned Pwned Passwords API**. https://haveibeenpwned.com/API/v3#PwnedPasswords
- **Dropbox Tech Blog — zxcvbn: realistic password strength estimation**. https://dropbox.tech/security/zxcvbn-realistic-password-strength-estimation
- **Google Cloud — Best practices for application authentication**. https://cloud.google.com/docs/authentication/best-practices-applications
- **Scott Helme — Security Headers**. https://securityheaders.com
- **Philippe De Ryck — Pragmatic Web Security (JWT Deep Dive)**. https://pragmaticwebsecurity.com
