# System Design: OAuth2 Authorization Server (Provider)

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Authorization Code Flow with PKCE** — issue authorization codes to public and confidential clients; exchange codes for access + refresh tokens; PKCE mandatory for all public clients (RFC 7636).
2. **Client Credentials Flow** — issue access tokens to confidential machine-to-machine clients authenticating with client_id + client_secret (no user involvement).
3. **Device Authorization Flow** — issue device codes and user codes for input-constrained devices (smart TVs, CLIs); user authorizes on a separate browser (RFC 8628).
4. **Token Refresh** — issue new access tokens from valid refresh tokens; rotation with reuse detection (same as auth service, see `auth_service.md`).
5. **Token Introspection** — allow resource servers to validate opaque tokens and retrieve their metadata (RFC 7662).
6. **Token Revocation** — allow clients to revoke access or refresh tokens (RFC 7009).
7. **Scope Management** — define, register, and enforce fine-grained scopes (e.g., `read:profile`, `write:posts`); display human-readable scope descriptions on consent screen.
8. **Consent Screen** — display requested scopes to users; allow users to grant/deny; persist consent decisions per user+client pair to avoid re-prompting.
9. **Client Registration** — register OAuth2 clients with redirect URIs, allowed grant types, allowed scopes, client type (public vs confidential); support dynamic client registration (RFC 7591) for trusted partners.
10. **OpenID Connect (OIDC)** — issue ID tokens (JWTs with user identity claims) alongside access tokens when `openid` scope requested; expose UserInfo endpoint; publish discovery document at `/.well-known/openid-configuration`.
11. **Metadata / Discovery** — RFC 8414 Authorization Server Metadata endpoint.
12. **Implicit Flow (deprecated)** — recognize and reject implicit flow requests; return helpful error directing clients to use PKCE instead.

### Non-Functional Requirements

1. **Availability** — 99.99% uptime; OAuth2 provider is a dependency for all third-party integrations.
2. **Latency** — authorization endpoint: p99 < 200 ms (redirect); token endpoint: p99 < 100 ms; introspection: p99 < 20 ms.
3. **Throughput** — 10,000 token requests/second peak; 50,000 introspection requests/second.
4. **Security** — RFC 6749, RFC 7636 (PKCE), RFC 7009, RFC 7662, RFC 8707 (Resource Indicators) compliance; OWASP OAuth2 threat model mitigated.
5. **Scalability** — horizontally scalable token and authorization endpoints; authorization code store and token store must be low-latency.
6. **Auditability** — all authorization grants, token issuances, revocations, and introspection calls must be logged with correlation IDs.

### Out of Scope

- End-user authentication (login/password/MFA) — delegated to the Auth Service (`auth_service.md`).
- SAML federation — covered in `sso.md`.
- API key issuance — covered in `api_key_management.md`.
- Billing/rate limiting based on OAuth2 client subscription tiers (assumed to be handled by API gateway layer).

---

## 2. Users & Scale

### User Types

| Type | Description | Flows Used |
|---|---|---|
| End User | Human granting access to a third-party app | Authorization Code + PKCE |
| Third-Party Developer | Registers apps, manages client credentials | Client Registration API |
| Resource Server | Backend service validating tokens | Token Introspection |
| Machine Client (M2M) | Service-to-service without user context | Client Credentials |
| Device User | Using smart TV or CLI that can't open browser | Device Authorization Flow |
| Admin | Platform operator managing clients, scopes | Admin API |

### Traffic Estimates

**Assumptions:**
- Platform has 200M end users.
- 5,000 registered OAuth2 client applications.
- 20M active users per day interact with at least one third-party app.
- Average 3 OAuth2 sessions initiated per active third-party user per day.
- Token lifetime: access token 1 hour, refresh token 30 days (OAuth2 provider typically uses longer-lived access tokens than the internal auth service since resource servers implement introspection caching).
- M2M clients: 500 M2M clients, each calling token endpoint every 55 minutes (just before 1h expiry): 500 × 24 × (60/55) ≈ 13,000 token requests/day from M2M.

| Event | Daily Volume | Peak Multiplier | Peak RPS |
|---|---|---|---|
| Authorization Endpoint (user redirect) | 60,000,000 | 3× | 60M × 3 / 86,400 = **2,083 RPS** |
| Token Exchange (code → tokens) | 58,000,000 (95% of auth codes used) | 3× | **2,014 RPS** |
| Token Refresh | 40,000,000 | 5× | 40M × 5 / 86,400 = **2,315 RPS** |
| Client Credentials Token | 13,000 | 2× | < 1 RPS (negligible) |
| Token Introspection | 500,000,000 (25 resource calls/user/day) | 10× | 500M × 10 / 86,400 = **57,870 RPS** |
| Token Revocation | 20,000,000 | 3× | **694 RPS** |
| UserInfo Endpoint | 200,000,000 | 5× | **11,574 RPS** |
| JWKS / Discovery | 1,000,000 (CDN-cached) | 1× | **12 RPS** (origin; CDN serves rest) |

**Key insight:** Token introspection at 57K RPS is the dominant load. This must be served from an in-memory cache. The token endpoint itself is ~5K RPS peak.

### Latency Requirements

| Endpoint | Target p50 | Target p99 | Notes |
|---|---|---|---|
| Authorization redirect | 50 ms | 200 ms | Includes consent check DB lookup |
| Token exchange | 20 ms | 100 ms | Code lookup + DB writes + JWT sign |
| Token introspection | 2 ms | 20 ms | Redis cache hit |
| UserInfo | 10 ms | 50 ms | Cache hit on user attributes |
| Token revocation | 10 ms | 50 ms | DB write + cache invalidation |
| Discovery / JWKS | 1 ms | 5 ms | Static or CDN-cached |

### Storage Estimates

| Data | Record Size | Volume | Storage |
|---|---|---|---|
| Client registrations | 2 KB | 5,000 clients | 10 MB (trivial) |
| Scope definitions | 512 bytes | 500 scopes | 256 KB (trivial) |
| Authorization codes (TTL 10 min) | 512 bytes | Peak concurrent = 2,083 RPS × 600s = 1.25M active | 625 MB |
| Access tokens (opaque, 1h TTL) | 512 bytes | 20M active | 10 GB |
| Refresh tokens (30d TTL) | 512 bytes | 20M active users × 5 clients avg = 100M | 51 GB |
| Consent grants | 256 bytes | 20M users × 5 clients = 100M | 25.6 GB |
| Token families | 128 bytes | 100M | 12.8 GB |
| Audit log (hot, 90 days) | 512 bytes | 800M events/day × 90 = 72B | ~36 TB |

### Bandwidth Estimates

| Flow | Payload | Daily Volume | Daily Bandwidth |
|---|---|---|---|
| Authorization redirect (inbound) | 2 KB | 60M | 120 GB |
| Token response (access + refresh + id_token) | 3 KB | 98M | 294 GB |
| Introspection response | 512 bytes | 500M | 256 GB |
| UserInfo response | 1 KB | 200M | 200 GB |
| **Total/day** | | | **~870 GB (~800 Mbps avg; ~8 Gbps peak at 10×)** |

---

## 3. High-Level Architecture

```
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │                         Third-Party Client Applications                      │
  │          Web Apps / Mobile Apps / SPAs / M2M Services / IoT Devices          │
  └─────────────┬──────────────────────────────────────────────────┬─────────────┘
                │ 1. Authorization Request                          │ 5. API Calls with
                │    (browser redirect)                             │    Access Token
                ▼                                                   ▼
  ┌─────────────────────────────┐              ┌────────────────────────────────────┐
  │   CDN / Edge (static)       │              │    Resource Servers (3rd party     │
  │   /.well-known/openid-conf  │              │    or first-party APIs)            │
  │   /oauth2/v1/jwks.json      │              │    → call /oauth2/v1/introspect    │
  └─────────────────────────────┘              └─────────────────┬──────────────────┘
                                                                  │
  ┌───────────────────────────────────────────────────────────────▼────────────────┐
  │                     API Gateway / Load Balancer                                │
  │   TLS termination, DDoS mitigation, rate limiting, routing                     │
  └───┬──────────────────────────┬──────────────────────┬──────────────────────────┘
      │ /oauth2/v1/authorize      │ /oauth2/v1/token      │ /oauth2/v1/introspect
      │ /oauth2/v1/userinfo       │ /oauth2/v1/revoke     │
      ▼                           ▼                        ▼
  ┌──────────────────────────────────────────────────────────────┐
  │                  OAuth2 Authorization Server                  │
  │              (Stateless pods, horizontally scaled)            │
  │                                                               │
  │  ┌─────────────────────┐  ┌──────────────────────────────┐   │
  │  │ Authorization        │  │ Token Endpoint               │   │
  │  │ Endpoint Handler     │  │ - Code Exchange              │   │
  │  │ - PKCE validation    │  │ - Client Credentials         │   │
  │  │ - Consent check      │  │ - Refresh Token              │   │
  │  │ - Code issuance      │  │ - Device Code Exchange       │   │
  │  └─────────────────────┘  └──────────────────────────────┘   │
  │                                                               │
  │  ┌─────────────────────┐  ┌──────────────────────────────┐   │
  │  │ Introspection        │  │ UserInfo / OIDC              │   │
  │  │ Endpoint             │  │ Endpoint                     │   │
  │  │ - Redis cache lookup │  │ - JWT ID token issuance      │   │
  │  │ - Auth server only   │  │ - Attribute mapping          │   │
  │  └─────────────────────┘  └──────────────────────────────┘   │
  └───────┬───────────────────────────────────┬──────────────────┘
          │                                   │
  ┌───────▼───────────────────────┐  ┌────────▼────────────────────────────────┐
  │   Authorization Code Store    │  │   Token Store                           │
  │   (Redis, TTL=600s)           │  │   (Redis hot + PostgreSQL durable)      │
  │   - code_hash → {client_id,   │  │   - access_token_hash → metadata        │
  │     user_id, scope, pkce}     │  │   - refresh_token_hash → metadata       │
  └───────────────────────────────┘  │   - token families                      │
                                     └─────────────────────────────────────────┘
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │                         Backing Stores                                       │
  │                                                                              │
  │  ┌──────────────────┐  ┌─────────────────┐  ┌────────────────────────────┐ │
  │  │ Client Registry  │  │ Consent Store   │  │ Auth Service               │ │
  │  │ (PostgreSQL)     │  │ (PostgreSQL,    │  │ (Internal RPC / gRPC)      │ │
  │  │ - client metadata│  │  cached Redis)  │  │ - Delegates user login     │ │
  │  │ - secret hashes  │  │                 │  │ - Returns authenticated    │ │
  │  │ - redirect URIs  │  │                 │  │   user context             │ │
  │  └──────────────────┘  └─────────────────┘  └────────────────────────────┘ │
  │                                                                              │
  │  ┌──────────────────────────────────────────────────────────────────────┐   │
  │  │ Audit Log (Kafka → ClickHouse / S3)                                  │   │
  │  └──────────────────────────────────────────────────────────────────────┘   │
  └─────────────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────────────┐
  │                        Device Authorization Flow (side path)                 │
  │   Device → POST /device_authorization → {device_code, user_code, verify_uri}│
  │   User → Browser → verify_uri?user_code=XXXX → login + consent              │
  │   Device → Poll POST /token until success or expired                         │
  └──────────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

- **OAuth2 Authorization Server (stateless pods)**: Core business logic for all OAuth2/OIDC flows. Stateless — all session and token state is external. Horizontally scalable.
- **Authorization Code Store (Redis)**: Short-lived (10-minute TTL) store for authorization codes. Codes are hashed before storage. Single-use enforced by atomically deleting the code on first exchange attempt.
- **Token Store (Redis + PostgreSQL)**: Redis serves the introspection hot path (57K RPS). PostgreSQL is the durable source of truth. Write-through: every token issuance writes to both. Redis entries have TTLs matching token expiry.
- **Client Registry (PostgreSQL)**: Client metadata read-heavy, write-rarely. Cached in Redis (1-hour TTL). Contains client_secret hashes, allowed redirect URIs, allowed scopes, grant types.
- **Consent Store (PostgreSQL + Redis)**: Stores user consent decisions per (user_id, client_id, scope) tuple. Cached in Redis to serve the authorization endpoint fast path without DB lookup.
- **Auth Service (internal gRPC)**: The OAuth2 server delegates user authentication to the Auth Service. The authorization endpoint presents a login form (or redirects to the Auth Service login page) and receives a validated user context back.

**Primary Use-Case Data Flow (Authorization Code + PKCE):**

```
1.  Client generates code_verifier = CSPRNG(32-96 bytes) → base64url
    code_challenge = BASE64URL(SHA256(code_verifier))
    code_challenge_method = "S256"

2.  Client redirects browser:
    GET /oauth2/v1/authorize
      ?response_type=code
      &client_id=abc123
      &redirect_uri=https://app.example.com/callback
      &scope=openid+profile+read:posts
      &state=random-csrf-token
      &code_challenge=<hash>
      &code_challenge_method=S256

3.  OAuth2 Server validates:
    a. client_id exists and is active
    b. redirect_uri exactly matches one registered redirect URI for client
    c. requested scopes are a subset of client's allowed scopes
    d. code_challenge present (required for public clients; recommended for all)
    e. No implicit response_type (rejected)

4.  If user not authenticated → redirect to Auth Service login
    Auth Service returns authenticated user context (user_id, amr, auth_time)

5.  Check consent store for (user_id, client_id, scope_set)
    If not consented → render consent screen with scope descriptions
    User clicks "Allow" → persist consent grant to DB

6.  Generate authorization code:
    raw_code = CSPRNG(32 bytes) → base64url (43 chars)
    code_hash = SHA256(raw_code)
    Store in Redis: code_hash → {client_id, user_id, scope, pkce_challenge, 
                                  pkce_method, redirect_uri, nonce, auth_time}
    TTL = 600 seconds (10 minutes)

7.  Redirect to redirect_uri?code=<raw_code>&state=<client_state>

8.  Client back-channel POST /oauth2/v1/token:
    grant_type=authorization_code
    &code=<raw_code>
    &redirect_uri=https://app.example.com/callback
    &client_id=abc123           (public client; no secret)
    &code_verifier=<original>

9.  OAuth2 Server:
    a. Compute code_hash = SHA256(code)
    b. Fetch from Redis by code_hash → get stored data (ATOMIC: DEL on fetch)
    c. If not found → 400 invalid_grant (code used or expired)
    d. Verify redirect_uri matches stored redirect_uri
    e. Verify PKCE: SHA256(code_verifier) == stored code_challenge ✓
    f. Issue access token (JWT or opaque), refresh token, ID token (if openid scope)
    g. Write tokens to Redis + PostgreSQL
    h. Return token response

10. Client uses access token to call resource server APIs.
11. Resource server calls POST /oauth2/v1/introspect {token: <access_token>}
    OAuth2 Server returns {active: true, sub, scope, exp, client_id, ...}
```

---

## 4. Data Model

### Entities & Schema

```sql
-- =============================================
-- OAuth2 Client Registrations
-- =============================================
CREATE TABLE oauth2_clients (
    id                      UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    client_id               VARCHAR(64)     NOT NULL UNIQUE,          -- public identifier
    client_secret_hash      VARCHAR(128),                             -- NULL for public clients
    client_type             VARCHAR(20)     NOT NULL CHECK (client_type IN ('public','confidential')),
    client_name             VARCHAR(255)    NOT NULL,
    client_uri              VARCHAR(2048),
    logo_uri                VARCHAR(2048),
    -- Allowed redirect URIs (exact match required)
    redirect_uris           TEXT[]          NOT NULL,
    -- Allowed OAuth2 grant types
    grant_types             TEXT[]          NOT NULL
        DEFAULT ARRAY['authorization_code'],
    -- Allowed scopes (subset of platform scopes)
    allowed_scopes          TEXT[]          NOT NULL,
    -- Token configuration
    access_token_ttl        INTEGER         NOT NULL DEFAULT 3600,    -- seconds
    refresh_token_ttl       INTEGER         NOT NULL DEFAULT 2592000, -- 30 days
    refresh_token_rotation  BOOLEAN         NOT NULL DEFAULT TRUE,
    -- PKCE requirements
    require_pkce            BOOLEAN         NOT NULL DEFAULT TRUE,
    -- Consent behavior
    skip_consent            BOOLEAN         NOT NULL DEFAULT FALSE,   -- for first-party clients
    -- Status
    is_active               BOOLEAN         NOT NULL DEFAULT TRUE,
    created_at              TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at              TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    owner_user_id           UUID,                                     -- developer who registered
    -- Contacts for notifications
    contacts                TEXT[]
);

CREATE INDEX idx_clients_client_id ON oauth2_clients (client_id);

-- =============================================
-- Platform Scope Definitions
-- =============================================
CREATE TABLE oauth2_scopes (
    name                    VARCHAR(128)    PRIMARY KEY,              -- e.g. 'read:profile'
    display_name            VARCHAR(255)    NOT NULL,                 -- e.g. 'View your profile'
    description             TEXT,
    is_default              BOOLEAN         NOT NULL DEFAULT FALSE,   -- included if no scope param
    is_openid               BOOLEAN         NOT NULL DEFAULT FALSE,   -- part of OIDC scope set
    requires_mfa            BOOLEAN         NOT NULL DEFAULT FALSE,   -- high-privilege scopes
    created_at              TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- =============================================
-- Authorization Codes (also mirrored in Redis)
-- =============================================
CREATE TABLE authorization_codes (
    code_hash               CHAR(64)        PRIMARY KEY,              -- SHA-256 hex
    client_id               VARCHAR(64)     NOT NULL REFERENCES oauth2_clients(client_id),
    user_id                 UUID            NOT NULL,
    scope                   TEXT            NOT NULL,                 -- space-separated
    redirect_uri            VARCHAR(2048)   NOT NULL,
    pkce_challenge          VARCHAR(128),
    pkce_method             VARCHAR(10)     CHECK (pkce_method IN ('S256','plain')),
    nonce                   VARCHAR(256),                             -- OIDC nonce
    auth_time               TIMESTAMPTZ     NOT NULL,
    amr                     TEXT[],                                   -- authentication methods
    created_at              TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at              TIMESTAMPTZ     NOT NULL,                 -- +10 minutes
    used_at                 TIMESTAMPTZ
);

CREATE INDEX idx_ac_expires ON authorization_codes (expires_at);

-- =============================================
-- Access Tokens (opaque; JWT metadata stored for introspection)
-- =============================================
CREATE TABLE access_tokens (
    token_hash              CHAR(64)        PRIMARY KEY,              -- SHA-256 of opaque token
    -- For JWT access tokens: store the jti instead
    jti                     UUID            UNIQUE,
    client_id               VARCHAR(64)     NOT NULL REFERENCES oauth2_clients(client_id),
    user_id                 UUID,                                     -- NULL for client_credentials
    scope                   TEXT            NOT NULL,
    issued_at               TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at              TIMESTAMPTZ     NOT NULL,
    revoked_at              TIMESTAMPTZ,
    grant_type              VARCHAR(30)     NOT NULL,
    ip_address              INET,
    -- Resource Indicators (RFC 8707)
    audiences               TEXT[]
);

CREATE INDEX idx_at_expires ON access_tokens (expires_at);
CREATE INDEX idx_at_user_id ON access_tokens (user_id, expires_at DESC);
CREATE INDEX idx_at_client_id ON access_tokens (client_id);

-- =============================================
-- Refresh Tokens and Token Families
-- =============================================
CREATE TABLE oauth2_token_families (
    id                      UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    client_id               VARCHAR(64)     NOT NULL REFERENCES oauth2_clients(client_id),
    user_id                 UUID,
    created_at              TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    invalidated_at          TIMESTAMPTZ,
    invalidation_reason     VARCHAR(50)
);

CREATE TABLE oauth2_refresh_tokens (
    id                      UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    family_id               UUID            NOT NULL REFERENCES oauth2_token_families(id),
    client_id               VARCHAR(64)     NOT NULL,
    user_id                 UUID,
    token_hash              CHAR(64)        NOT NULL UNIQUE,
    scope                   TEXT            NOT NULL,
    issued_at               TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at              TIMESTAMPTZ     NOT NULL,
    used_at                 TIMESTAMPTZ,
    replaced_by             UUID            REFERENCES oauth2_refresh_tokens(id),
    revoked_at              TIMESTAMPTZ
);

CREATE INDEX idx_ort_family ON oauth2_refresh_tokens (family_id);
CREATE INDEX idx_ort_user_client ON oauth2_refresh_tokens (user_id, client_id);

-- =============================================
-- Consent Grants
-- =============================================
CREATE TABLE consent_grants (
    id                      UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id                 UUID            NOT NULL,
    client_id               VARCHAR(64)     NOT NULL REFERENCES oauth2_clients(client_id),
    scope                   TEXT            NOT NULL,                 -- granted scopes
    granted_at              TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at              TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at              TIMESTAMPTZ,                              -- NULL = indefinite
    UNIQUE (user_id, client_id)
);

CREATE INDEX idx_cg_user_id ON consent_grants (user_id);

-- =============================================
-- Device Authorization Codes (RFC 8628)
-- =============================================
CREATE TABLE device_authorization_codes (
    device_code_hash        CHAR(64)        PRIMARY KEY,
    user_code               VARCHAR(8)      NOT NULL UNIQUE,          -- e.g. "WDJB-MJHT"
    client_id               VARCHAR(64)     NOT NULL,
    scope                   TEXT            NOT NULL,
    is_approved             BOOLEAN         NOT NULL DEFAULT FALSE,
    approved_user_id        UUID,
    expires_at              TIMESTAMPTZ     NOT NULL,
    created_at              TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    last_polled_at          TIMESTAMPTZ,
    interval_seconds        INTEGER         NOT NULL DEFAULT 5
);

CREATE UNIQUE INDEX idx_dac_user_code ON device_authorization_codes (upper(user_code));

-- =============================================
-- Audit Log
-- =============================================
CREATE TABLE oauth2_audit_events (
    id                      UUID            DEFAULT gen_random_uuid(),
    event_type              VARCHAR(60)     NOT NULL,
    event_timestamp         TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    client_id               VARCHAR(64),
    user_id                 UUID,
    grant_type              VARCHAR(30),
    scope                   TEXT,
    ip_address              INET,
    metadata                JSONB,
    PRIMARY KEY (event_timestamp, id)
) PARTITION BY RANGE (event_timestamp);
```

### Database Choice

| Database | Fit | Reasoning |
|---|---|---|
| **PostgreSQL** | **Selected** | ACID for authorization code exchange (single-use enforcement), consent grants, client registry. JSONB for flexible audit metadata. Mature, well-understood |
| **Redis** | **Selected (cache + code store)** | Sub-millisecond introspection lookups, TTL-native for short-lived authorization codes and access tokens, atomic DEL for single-use code enforcement |
| DynamoDB | Alternative | Auto-scaling token store; eventual consistency on GSIs is a concern for introspection; viable but higher complexity |
| Cassandra | Alternative (writes) | High write throughput for token audit; overkill for primary token store at this scale |
| MongoDB | Not selected | No advantage over PostgreSQL; weaker ACID guarantees |

**Authorization code single-use enforcement in Redis:**
```
Redis MULTI/EXEC:
  GETDEL oauth2:code:<code_hash>
If result is nil → code already used or expired → reject
If result is data → proceed (atomic: code deleted in same operation)
```
This atomic `GETDEL` (Redis 6.2+) or `MULTI/GET/DEL/EXEC` ensures no two concurrent requests can exchange the same code.

---

## 5. API Design

Base URL: `https://auth.platform.example.com/oauth2/v1`

All endpoints use HTTPS. Client authentication uses HTTP Basic Auth or `client_id`/`client_secret` in POST body (per RFC 6749 §2.3; Basic Auth preferred for confidential clients).

---

### GET /oauth2/v1/authorize

**Auth:** None (user not yet authenticated; auth happens here)  
**Rate Limit:** 100 requests/IP/minute

```
Query Parameters:
  response_type=code                    (required; "token" rejected)
  client_id=<client_id>                 (required)
  redirect_uri=<uri>                    (required; must match registered URI exactly)
  scope=openid+profile+read:posts       (optional; defaults to client's default scopes)
  state=<random-value>                  (required; CSRF protection)
  code_challenge=<S256-challenge>       (required for public clients; strongly recommended)
  code_challenge_method=S256            (required with code_challenge; "plain" rejected)
  nonce=<random>                        (required when scope includes "openid")
  prompt=none|login|consent|select_account  (optional OIDC parameter)
  max_age=<seconds>                     (optional; max auth age for OIDC)
  login_hint=user@example.com           (optional; pre-fill login form)
  ui_locales=en-US                      (optional)

Response (redirect):
  302 → https://app.example.com/callback?code=<raw_code>&state=<client_state>

Error redirect:
  302 → https://app.example.com/callback?error=access_denied&error_description=...&state=...

Error codes: invalid_request | unauthorized_client | access_denied | 
             unsupported_response_type | invalid_scope | server_error | 
             temporarily_unavailable

Special case (redirect_uri invalid or client_id invalid):
  Return 400 Bad Request directly — do NOT redirect (to prevent open redirect attacks)
```

---

### POST /oauth2/v1/token

**Auth:** HTTP Basic (confidential clients: Authorization: Basic base64(client_id:client_secret)); public clients: client_id in body  
**Rate Limit:** 100 requests/client/minute; 1,000 requests/IP/minute

```
--- Authorization Code Exchange ---
Request:
  Content-Type: application/x-www-form-urlencoded
  grant_type=authorization_code
  &code=<raw_code>
  &redirect_uri=https://app.example.com/callback
  &client_id=abc123
  &code_verifier=<original-code-verifier>

Response 200 OK:
  Content-Type: application/json
  Cache-Control: no-store
  Pragma: no-cache
  {
    "access_token": "eyJ...",              // JWT (RS256) or opaque 32-byte b64url
    "token_type": "Bearer",
    "expires_in": 3600,
    "refresh_token": "dGhpcyBpcyBhIHJlZnJlc2g...",
    "scope": "openid profile read:posts",
    "id_token": "eyJ..."                   // present when scope includes "openid"
  }

--- Client Credentials ---
Request:
  grant_type=client_credentials
  &scope=service:write
  Authorization: Basic base64(client_id:client_secret)

Response 200 OK:
  {
    "access_token": "eyJ...",
    "token_type": "Bearer",
    "expires_in": 3600,
    "scope": "service:write"
    // No refresh_token for client_credentials
  }

--- Refresh Token ---
Request:
  grant_type=refresh_token
  &refresh_token=<token>
  &scope=read:posts         // optional: narrow scope; cannot expand

Response 200 OK:
  { "access_token": "...", "expires_in": 3600, "refresh_token": "...", ... }

--- Device Code Exchange (polling) ---
Request:
  grant_type=urn:ietf:params:oauth:grant-type:device_code
  &device_code=<raw_device_code>
  &client_id=abc123

Response 200 OK (approved):
  { "access_token": "...", "expires_in": 3600, "refresh_token": "...", ... }

Response 400 (still pending):
  { "error": "authorization_pending", "error_description": "User has not yet approved." }

Response 400 (polling too fast):
  { "error": "slow_down", "error_description": "Reduce polling interval." }

Response 400 (expired):
  { "error": "expired_token" }

--- Error Response (all grant types) ---
Response 400 Bad Request:
  { "error": "invalid_grant", "error_description": "Authorization code expired or already used." }

Error codes: invalid_request | invalid_client | invalid_grant | unauthorized_client |
             unsupported_grant_type | invalid_scope
```

---

### POST /oauth2/v1/introspect

**Auth:** HTTP Basic (resource server client credentials)  
**Rate Limit:** 10,000 requests/client/minute

```
Request:
  Content-Type: application/x-www-form-urlencoded
  token=<access_or_refresh_token>
  &token_type_hint=access_token     // optional: access_token | refresh_token

Response 200 OK (active token):
  {
    "active": true,
    "sub": "user-uuid",
    "client_id": "abc123",
    "scope": "read:posts profile",
    "exp": 1744241600,
    "iat": 1744238000,
    "iss": "https://auth.platform.example.com",
    "aud": ["https://api.platform.example.com"],
    "jti": "uuid",
    "username": "alice@example.com",
    "token_type": "access_token",
    "amr": ["pwd", "otp"]
  }

Response 200 OK (inactive/revoked/expired token):
  { "active": false }

Response 401 Unauthorized (bad resource server credentials):
  WWW-Authenticate: Basic realm="oauth2-introspection"
  { "error": "invalid_client" }
```

---

### POST /oauth2/v1/revoke

**Auth:** HTTP Basic (client credentials) or Bearer (user token to revoke own tokens)  
**Rate Limit:** 100 requests/client/minute

```
Request:
  Content-Type: application/x-www-form-urlencoded
  token=<token_to_revoke>
  &token_type_hint=refresh_token     // optional

Response 200 OK (always, even if token not found — per RFC 7009)
  {}

Side effects:
  - Access token: mark revoked in DB + add to Redis revocation set (TTL = remaining lifetime)
  - Refresh token: mark revoked; revoke entire token family
  - Propagate revocation to resource servers via webhook or pub/sub (optional)
```

---

### POST /oauth2/v1/device_authorization

**Auth:** HTTP Basic (confidential clients) or client_id in body (public clients)  
**Rate Limit:** 10 requests/client/minute

```
Request:
  client_id=abc123
  &scope=read:profile

Response 200 OK:
  {
    "device_code": "GmRhmhcxhwAzkoEqiMEg_DnyEysNkuNhszIySk9eS",
    "user_code": "WDJB-MJHT",
    "verification_uri": "https://auth.platform.example.com/device",
    "verification_uri_complete": "https://auth.platform.example.com/device?user_code=WDJB-MJHT",
    "expires_in": 1800,
    "interval": 5
  }
```

---

### GET /oauth2/v1/userinfo

**Auth:** Bearer access token (must include `openid` scope)  
**Rate Limit:** 1,000 requests/client/minute

```
Response 200 OK:
  {
    "sub": "user-uuid",
    "name": "Alice Smith",
    "email": "alice@example.com",
    "email_verified": true,
    "picture": "https://cdn.example.com/avatars/alice.jpg",
    "updated_at": 1744000000
    // Additional claims based on granted scopes
  }

Response 401 Unauthorized:
  WWW-Authenticate: Bearer realm="userinfo", error="invalid_token"
```

---

### GET /oauth2/v1/.well-known/openid-configuration

**Auth:** None  
**Cache:** `Cache-Control: public, max-age=86400`

```
Response 200 OK:
  {
    "issuer": "https://auth.platform.example.com",
    "authorization_endpoint": "https://auth.platform.example.com/oauth2/v1/authorize",
    "token_endpoint": "https://auth.platform.example.com/oauth2/v1/token",
    "userinfo_endpoint": "https://auth.platform.example.com/oauth2/v1/userinfo",
    "jwks_uri": "https://auth.platform.example.com/oauth2/v1/jwks.json",
    "revocation_endpoint": "https://auth.platform.example.com/oauth2/v1/revoke",
    "introspection_endpoint": "https://auth.platform.example.com/oauth2/v1/introspect",
    "device_authorization_endpoint": "https://auth.platform.example.com/oauth2/v1/device_authorization",
    "scopes_supported": ["openid","profile","email","read:posts","write:posts"],
    "response_types_supported": ["code"],
    "grant_types_supported": ["authorization_code","client_credentials","refresh_token","urn:ietf:params:oauth:grant-type:device_code"],
    "code_challenge_methods_supported": ["S256"],
    "token_endpoint_auth_methods_supported": ["client_secret_basic","client_secret_post","private_key_jwt"],
    "subject_types_supported": ["public","pairwise"],
    "id_token_signing_alg_values_supported": ["RS256"],
    "claims_supported": ["sub","iss","aud","exp","iat","nonce","auth_time","amr","name","email","email_verified"],
    "request_parameter_supported": true,
    "require_pkce": true
  }
```

---

## 6. Deep Dive: Core Components

### 6.1 Authorization Code Flow with PKCE

**Problem it solves:**  
The authorization code flow with PKCE addresses two attack vectors: (1) **Authorization code interception**: an attacker operating a malicious app on the same device can register the same custom URI scheme (e.g., `myapp://callback`) and intercept the authorization code. Without PKCE, they could exchange it for tokens. (2) **Cross-Site Request Forgery on the redirect**: `state` parameter prevents CSRF, but PKCE adds a cryptographically bound proof that the entity receiving the code is the same one that initiated the authorization request.

**Approaches Compared:**

| Approach | Security | Client Complexity | Notes |
|---|---|---|---|
| Plain redirect code (no PKCE, no state) | Weak | Low | Vulnerable to CSRF and code interception |
| With `state` only | Moderate | Low | CSRF protection only; code still stealable |
| PKCE (S256, code_challenge) | Strong | Medium | Cryptographic binding; recommended for all clients |
| PKCE with `plain` method | Weak | Low | Code verifier == challenge; interception still works |
| mTLS client auth (RFC 8705) | Strongest | High | Certificate-bound tokens; complex PKI management |
| **PKCE S256 (selected)** | **Strong** | **Medium** | **Standard; no cert management** |

**Selected: PKCE with S256 (mandatory for public clients, strongly recommended for confidential)**

Reasoning: S256 is the only secure PKCE method. `plain` is explicitly discouraged by RFC 7636 (use only if S256 is not possible, which it always is). S256 ensures the interceptor who captures the authorization code cannot exchange it without also knowing the `code_verifier`, which was never transmitted.

**Implementation Detail:**

```
Client-side (must be on device):
  function StartAuthFlow():
    code_verifier = base64url(CSPRNG(32 bytes))  // 43 chars, URL-safe
    // Must be 43-128 chars, unreserved chars only [A-Za-z0-9\-._~]
    code_challenge = base64url(SHA256(ASCII(code_verifier)))
    state = base64url(CSPRNG(16 bytes))
    nonce = base64url(CSPRNG(16 bytes))  // for OIDC
    
    // Persist {state, code_verifier, nonce} in session/secure storage
    // code_verifier MUST NOT be transmitted until token exchange
    
    redirect_url = build_url({
      response_type: "code",
      client_id: CLIENT_ID,
      redirect_uri: REDIRECT_URI,
      scope: "openid profile read:posts",
      state: state,
      code_challenge: code_challenge,
      code_challenge_method: "S256",
      nonce: nonce
    })
    browser.navigate(redirect_url)

  function HandleCallback(callback_url):
    params = parse(callback_url)
    if params.error: handle_error(params.error)
    if params.state != session.state: reject("CSRF mismatch")
    
    POST /oauth2/v1/token {
      grant_type: "authorization_code",
      code: params.code,
      redirect_uri: REDIRECT_URI,
      client_id: CLIENT_ID,
      code_verifier: session.code_verifier  // sent here, only once
    }

Server-side verification:
  function ExchangeCode(request):
    code_hash = SHA256(request.code)
    stored = Redis.GETDEL(f"oauth2:code:{code_hash}")  // atomic get + delete
    if stored is nil: return error("invalid_grant")
    
    // Verify client
    if stored.client_id != request.client_id: return error("invalid_grant")
    if stored.redirect_uri != request.redirect_uri: return error("invalid_grant")
    
    // Verify PKCE
    if stored.pkce_method == "S256":
      expected_challenge = base64url(SHA256(request.code_verifier))
      if expected_challenge != stored.pkce_challenge:
        return error("invalid_grant", "PKCE verification failed")
    
    // Check code not expired (belt-and-suspenders; Redis TTL handles this)
    if stored.expires_at < now(): return error("invalid_grant", "Code expired")
    
    // Issue tokens
    access_token = IssueAccessToken(stored.user_id, stored.client_id, stored.scope)
    refresh_token = IssueRefreshToken(stored.user_id, stored.client_id, stored.scope)
    id_token = null
    if "openid" in stored.scope:
      id_token = IssueIDToken(stored.user_id, stored.client_id, stored.nonce, stored.auth_time)
    
    return { access_token, refresh_token, id_token, expires_in }
```

**Interviewer Q&As:**

Q: What is the difference between `state` and PKCE and do you need both?  
A: Yes, both serve distinct purposes. `state` is an opaque value the client sets, includes in the authorization request, and verifies in the callback — it protects against CSRF (a malicious site tricking the user's browser into sending a crafted callback). PKCE binds the code exchange to the specific client instance that initiated the request via a cryptographic proof (code_verifier/code_challenge) — it protects against code interception by a different app on the same device. Remove either one and you lose protection against a specific attack class.

Q: Why is the implicit flow deprecated?  
A: The implicit flow returns the access token directly in the redirect URI fragment (`#access_token=...`) without an intermediate code. Problems: (1) The access token appears in browser history, referrer headers, and server logs. (2) No way to authenticate the client (no code exchange step). (3) Token injection attacks — an attacker who intercepts or forges a redirect can inject a token from a different issuer. RFC 9700 (OAuth 2.0 Security Best Current Practice) explicitly prohibits implicit flow. PKCE-based authorization code flow provides equivalent functionality for SPAs with better security.

Q: What happens if two requests race to exchange the same authorization code?  
A: The Redis `GETDEL` command atomically retrieves and deletes the code in a single operation (Redis 6.2+; or use `MULTI/GET/DEL/EXEC` for older Redis). Only one of the two racing requests will get a non-nil result; the other gets nil and receives `invalid_grant`. This is correct and safe behavior. The first requester to reach Redis wins. In practice, this race only happens if an attacker replays the code — the legitimate client has no reason to send the code twice.

Q: How do you handle redirect URI matching to prevent open redirect vulnerabilities?  
A: Exact string matching against the registered redirect URIs. Do not perform prefix matching, wildcard matching, or URL parsing for comparison (these have all been exploited). Compare the byte-exact string. Registered URIs are stored in the client registration and validated at registration time: (1) must use HTTPS (except `localhost` for development clients); (2) no URL fragments; (3) for native apps, custom URI schemes must be registered. If redirect_uri is invalid or missing, return 400 directly (do not redirect with an error — redirecting would enable open redirect attacks using the OAuth provider as a trusted middleman).

Q: What is the authorization code timeout and why is 10 minutes the standard?  
A: RFC 6749 recommends a "very short lifetime" for authorization codes. 10 minutes is the industry convention (used by Google, GitHub, Auth0). The rationale: (1) The code exchange should happen immediately after the redirect (milliseconds to seconds); a 10-minute window accommodates slow networks, paused browser sessions, and development/debugging. (2) The shorter the window, the smaller the attack surface for intercepted codes. (3) Codes can only be used once (GETDEL enforces this), so extending the window doesn't increase per-request risk, only the brute-force attack window. Some providers use 5 minutes; 10 is a reasonable balance.

---

### 6.2 Token Introspection at Scale

**Problem it solves:**  
When access tokens are opaque (random strings, not JWTs), resource servers have no way to validate them locally. They must call the authorization server to ask "is this token valid and what does it authorize?" This introspection path is the highest-volume endpoint (57K RPS peak) and must not add latency to every API call at resource servers.

**Approaches Compared:**

| Approach | Latency | Network Dependency | Revocation Freshness | Complexity |
|---|---|---|---|---|
| Opaque token + sync introspection per call | High (20-50ms) | Yes (coupling) | Immediate | Low |
| JWT (self-contained) + local verify | < 1ms | No | Bounded by token TTL | Medium (JWKS rotation) |
| Opaque token + cached introspection | Low (2ms in cache) | Only on miss/expiry | Bounded by cache TTL | Medium |
| JWT + revocation list check | < 2ms | Redis only | Immediate for listed tokens | Medium |
| **Opaque + Redis cache (selected)** | **2ms cache hit** | **Redis only** | **TTL-bounded (1 min)** | **Medium** |

**Selected: Opaque access token + aggressive Redis caching at introspection endpoint**

Justification: Opaque tokens allow immediate server-side revocation (the token entry is deleted from Redis; next introspection returns `active: false`). JWTs with short TTL are equivalent for access tokens but have the disadvantage of leaking metadata to resource servers (JWT payload is base64-decodable without verification). Opaque tokens give the authorization server full control over what metadata is disclosed via introspection.

**Implementation Detail:**

```
Token issuance:
  function IssueAccessToken(user_id, client_id, scope, audiences):
    raw = CSPRNG(32 bytes) → base64url  // 43-char opaque token
    token_hash = SHA256(raw)
    
    token_data = {
      "active": true,
      "sub": user_id,
      "client_id": client_id,
      "scope": scope,
      "iss": ISSUER_URL,
      "iat": now(),
      "exp": now() + access_token_ttl,
      "jti": UUID(),
      "aud": audiences,
      "token_type": "access_token"
    }
    
    // Write to Redis (primary lookup path)
    Redis.SETEX(
      key=f"oauth2:at:{token_hash}",
      ttl=access_token_ttl,
      value=JSON.serialize(token_data)
    )
    // Write to PostgreSQL (durable audit + session listing)
    DB.INSERT INTO access_tokens(token_hash, jti, client_id, user_id, scope, ...)
    
    return raw

Introspection (hot path):
  function Introspect(token, token_type_hint, requesting_client_id):
    token_hash = SHA256(token)
    
    // Try Redis first (< 1ms)
    cached = Redis.GET(f"oauth2:at:{token_hash}")
    if cached is nil:
      // Cache miss: check PostgreSQL (revoked/expired tokens not in Redis)
      row = DB.SELECT FROM access_tokens WHERE token_hash=?
      if row is nil OR row.revoked_at IS NOT NULL OR row.expires_at < now():
        return {"active": false}
      // Warm Redis cache
      Redis.SETEX(f"oauth2:at:{token_hash}", remaining_ttl, serialize(row))
      cached = serialize(row)
    
    token_data = JSON.parse(cached)
    if token_data.exp < now():
      return {"active": false}
    
    // Audience check: ensure requesting_client is allowed to introspect this token
    // (prevent client A from discovering info about client B's tokens)
    // Only the issuing client or explicitly listed audiences may introspect
    if requesting_client_id not in [token_data.client_id] + token_data.aud:
      return {"active": false}  // do not 403; return inactive to prevent oracle attacks
    
    return token_data

Revocation propagation:
  function RevokeToken(token):
    token_hash = SHA256(token)
    // Atomic: delete from Redis (immediate effect for active introspectors)
    Redis.DEL(f"oauth2:at:{token_hash}")
    // Mark revoked in PostgreSQL (durable)
    DB.UPDATE access_tokens SET revoked_at=now() WHERE token_hash=?
    // Publish to pub/sub so resource servers with local caches can invalidate
    PubSub.PUBLISH("oauth2:token_revoked", {token_hash: token_hash})
```

**Resource server caching:**
Resource servers may cache introspection results locally (in-process) for a short TTL (e.g., 30 seconds to 2 minutes) to reduce load on the introspection endpoint. The trade-off: up to 2 minutes of stale positive results after revocation. Mitigate: resource servers subscribe to the `oauth2:token_revoked` pub/sub channel and evict local cache entries immediately on revocation signal. This provides near-immediate revocation propagation while keeping the introspection call rate manageable.

**Interviewer Q&As:**

Q: When should a resource server use introspection vs. local JWT verification?  
A: Local JWT verification (using JWKS) is preferable for high-throughput, latency-sensitive resource servers: it requires zero network calls per request and scales infinitely. Use introspection when: (1) Tokens are opaque (not JWTs); (2) Immediate revocation is required (e.g., financial transactions where a stolen token must be killed instantly, not just "within 15 minutes"); (3) The resource server needs metadata not embeddable in the JWT (e.g., real-time account state). In practice, many large platforms use JWTs for access tokens with short TTLs (15-60 minutes) and accept the bounded revocation delay, reserving introspection for high-security operations.

Q: How do you protect the introspection endpoint from being used as a token enumeration oracle?  
A: Four protections: (1) Require authentication for introspection (resource server must present its own client credentials) — prevents public enumeration. (2) Return `{"active": false}` for any invalid, expired, or unauthorized token — never return 404 or different error codes that distinguish "token not found" from "token revoked." (3) Audience validation — only the token's intended audience can introspect it. (4) Rate limit per introspecting client. Even with these controls, the token search space (32-byte CSPRNG = 256 bits of entropy) makes brute-force enumeration infeasible (2^256 possibilities).

Q: What is the `token_type_hint` parameter in introspection and how is it used?  
A: `token_type_hint` (`access_token` or `refresh_token`) is an optimization hint to help the server look in the right store first, since access tokens and refresh tokens are stored differently. It is not authoritative: if the token is not found under the hinted type, the server should search other token stores. The client does not need to provide it; the server must handle both token types regardless. Providing the hint reduces average lookup latency from two Redis lookups to one.

Q: How does introspection caching interact with token revocation SLAs?  
A: It creates a bounded inconsistency window. If a resource server caches an introspection result for 60 seconds, a revoked token can still be used at that resource server for up to 60 seconds after revocation. The SLA becomes: "tokens are revoked within max(cache_ttl) seconds across all resource servers." Design choices to tighten this: (1) Shorter cache TTL (30 seconds) — increases introspection load. (2) Push-based invalidation (pub/sub on revocation) — near-instant propagation, complexity of maintaining subscriber registry. (3) Accept the window for non-critical APIs; use introspection without caching for high-security endpoints (payment processing, account deletion). Document the SLA explicitly.

Q: How do you support resource servers in multiple regions without centralized introspection being a latency bottleneck?  
A: Deploy read replicas of the Redis token store in each region. Writes (token issuance, revocation) go to the primary region and replicate asynchronously (~50-100ms cross-region). Introspection reads from the local region's replica. Revocation propagation lag is bounded by replication lag (< 200ms). For immediate revocation, publish revocation events to a global pub/sub (e.g., Redis Pub/Sub with cross-region forwarding or a dedicated message bus) so regional caches invalidate within 500ms. This means the revocation SLA is "globally revoked within 500ms" — acceptable for most use cases.

---

### 6.3 Scope Design and Enforcement

**Problem it solves:**  
OAuth2 scopes define the permissions a client is requesting on behalf of a user. Poorly designed scopes either grant too much access (violating least-privilege) or are so granular that the consent screen becomes incomprehensible. Scope enforcement must be consistent across all resource servers and must prevent scope escalation during token refresh.

**Approaches Compared:**

| Scope Model | Granularity | UX | Enforcement Complexity |
|---|---|---|---|
| Single-scope ("full access") | None | Simple | Trivial but insecure |
| Coarse resource-level (`read`, `write`) | Low | Simple | Simple |
| **Resource:action scopes (`read:profile`, `write:posts`)** | **Medium** | **Moderate** | **Medium** |
| Fine-grained per-object (`read:post:<id>`) | High | Complex | Complex; impractical in OAuth2 |
| Dynamic scopes (Authz policies) | Highest | Hard | Complex (e.g., OPA) |

**Selected: Resource:action scope naming convention**

Reasoning: Provides sufficient granularity for most use cases (a photo app needs `read:photos` but not `read:contacts`), displays cleanly in consent UI ("View your photos"), and maps directly to API endpoint authorization checks. Object-level scopes belong in the resource server's authorization layer, not the OAuth2 scope system.

**Implementation Detail:**

```
Scope validation at authorization request:
  function ValidateRequestedScopes(client_id, requested_scopes):
    client = ClientRegistry.get(client_id)
    requested = set(requested_scopes.split(" "))
    allowed = set(client.allowed_scopes)
    
    // Requested scopes must be a subset of client's allowed scopes
    invalid = requested - allowed
    if invalid: return error("invalid_scope", f"Not allowed: {invalid}")
    
    // Check if any requested scope requires MFA
    high_privilege = [s for s in requested if Scopes[s].requires_mfa]
    if high_privilege and amr_does_not_include_mfa(session):
      return step_up_mfa_required(session, high_privilege)
    
    return requested  // may be narrowed by user on consent screen

Consent screen display:
  function BuildConsentPage(client, requested_scopes, user):
    scope_details = [Scopes[s] for s in requested_scopes]
    existing_consent = ConsentStore.get(user.id, client.client_id)
    
    // Only show scopes not already consented
    new_scopes = requested_scopes - existing_consent.granted_scopes
    if not new_scopes and client.skip_consent is False:
      // All scopes already consented → skip consent screen
      return auto_approve(existing_consent)
    
    return render_consent_html(client, scope_details, new_scopes)

Scope enforcement at token refresh:
  function RefreshToken(refresh_token, requested_scope=None):
    stored_scope = refresh_token_record.scope
    
    if requested_scope is not None:
      requested = set(requested_scope.split(" "))
      granted = set(stored_scope.split(" "))
      
      // Cannot request more than what was originally granted
      if not requested.issubset(granted):
        return error("invalid_scope", "Cannot expand scope via refresh")
      
      // Issue new access token with narrowed scope
      effective_scope = " ".join(requested)
    else:
      effective_scope = stored_scope
    
    // Refresh token retains original full scope (user can still expand later)
    return IssueAccessToken(user_id, client_id, effective_scope)
```

**Interviewer Q&As:**

Q: How do you handle scope changes when a new version of an app requests additional scopes?  
A: Check whether the new requested scope is a superset of previously consented scopes. If so, trigger a new consent screen showing only the newly requested scopes (incremental consent). The consent record is updated to include the new scopes. The original refresh token family is preserved; a new family is created for the extended scope. If the user denies the additional scopes, they can continue using the app with the previously granted scopes (graceful degradation — the app should handle `insufficient_scope` errors from resource servers).

Q: What is the `offline_access` scope and when is it appropriate?  
A: `offline_access` is an OIDC scope that signals the client wants a refresh token (for long-term access without user presence). Without this scope, some implementations do not issue a refresh token. It should be: (1) Only granted to confidential clients or public clients with PKCE; (2) Displayed prominently on the consent screen ("Allow this app to access your account while you're not using it"); (3) Not available to implicit flow clients (since they cannot securely store refresh tokens). Granting offline_access without user awareness is a privacy concern — users should understand the app can access their data indefinitely until they explicitly revoke.

Q: How do you prevent a malicious client from requesting more scopes than it needs and tricking users into consenting?  
A: Three controls: (1) Client registration limits allowed scopes — a malicious client that registered with `read:profile` cannot request `write:posts`. (2) Consent screen displays human-readable scope descriptions in plain language, making it obvious what the app is asking for. (3) App Store / client approval workflow: clients must go through an approval process before being allowed to request sensitive scopes (similar to Apple App Store privacy review or Google OAuth verification). Particularly sensitive scopes (e.g., `read:email_content`, `admin:*`) require manual review and are not grantable to arbitrary clients.

---

## 7. Scaling

### Horizontal Scaling

**OAuth2 Server (stateless pods):** All state in Redis + PostgreSQL. Deploy N pods behind load balancer. Auto-scale on CPU > 60% or token endpoint RPS > threshold. No sticky sessions.

**Introspection hot path (57K RPS):** Served by Redis in ~1ms. Redis Cluster with 3 primary nodes handles 57K GET operations/second easily (Redis single node handles 100K+ simple commands/second). OAuth2 Server pods act as thin proxies to Redis for introspection.

**Authorization code store (Redis TTL):** Small dataset (< 1 GB active at any time). Single Redis primary with replica is sufficient. The code store is write-once, read-once — extremely low contention.

**Client Registry (PostgreSQL, heavily read):** Cache in Redis for 1 hour. Client records rarely change. Cache invalidation: on client update, pub/sub message to all pods to evict local in-process cache. 5,000 clients × 2 KB = 10 MB — entire client registry fits in in-process memory of every pod.

### DB Sharding Strategy

**Token Store:** Shard by `token_hash` prefix (already a uniformly distributed SHA-256 value). Even distribution across shards guaranteed. No hot shard problem.

**Consent Grants:** Shard by `user_id`. Queries are always by `(user_id, client_id)`. Access tokens and refresh tokens also shard by `user_id` for session listing queries.

### Replication

| Store | Replication | RPO | RTO |
|---|---|---|---|
| Client Registry (PostgreSQL) | Synchronous to 1 standby | 0 | < 30s (Patroni) |
| Token Store (PostgreSQL) | Async to 1 replica | < 5s | < 60s |
| Token Store (Redis) | Redis Cluster replica per shard | < 1s | < 10s |
| Consent Store (PostgreSQL) | Async to 1 replica | < 5s | < 60s |

### Caching

| Data | Cache Key | TTL | Invalidation |
|---|---|---|---|
| Client registrations | `oauth2:client:<client_id>` | 1 hour | Pub/sub on update |
| Access token introspection | `oauth2:at:<token_hash>` | Remaining token lifetime | DEL on revocation |
| Refresh token validity | `oauth2:rt:<token_hash>` | Remaining lifetime | DEL on revocation |
| Consent grants | `oauth2:consent:<user_id>:<client_id>` | 1 hour | DEL on revocation/update |
| Scope definitions | In-process (all pods) | 24 hours | Restart or pub/sub |

### Interviewer Q&As on Scaling

Q: How do you handle a thundering herd on the token endpoint if a major client has all users simultaneously refresh their tokens?  
A: Multiple mitigations: (1) Client-level rate limiting (1,000 token requests/client/minute) prevents any single client from overwhelming the system. (2) Jitter on client-side refresh scheduling: clients should refresh at `expires_at - random(0, 300)` seconds rather than exactly at expiry. (3) Token endpoint pods auto-scale and are stateless. (4) Redis Pipeline batching: if multiple tokens need to be written, batch the Redis writes in a pipeline to reduce round trips. (5) For predictable spikes (e.g., daily 9 AM login storm), pre-scale pods via scheduled HPA.

Q: What is the impact on the system if the consent store goes down?  
A: If the consent store is unavailable: (1) For clients with `skip_consent=true` (first-party): authorization proceeds without consent check — no impact. (2) For third-party clients: the authorization server can fail open (allow auth, re-show consent on next request — user sees consent every login) or fail closed (return `temporarily_unavailable`). The safer choice depends on the security posture: financial apps should fail closed; consumer apps might prefer fail open with a "you'll be prompted for consent again next time" note. With 1-hour Redis caching of consent grants, a PostgreSQL consent store outage is tolerable for up to 1 hour for users with cached consent.

Q: How do you scale the device authorization flow polling?  
A: Device code polling is at `interval=5` seconds. A device code expires after 1800 seconds. In the worst case, one device generates 1800/5 = 360 poll requests. With 100,000 active device flows: 100K × 360 / 1800 = 20,000 polls/second. Scale with: (1) Store device code state in Redis with the `is_approved` flag; polling is a simple Redis GET — sub-millisecond. (2) Return `slow_down` error if a device polls faster than the specified interval (track `last_polled_at` in Redis). (3) Cap the maximum number of concurrent active device flows per client.

Q: How do you handle multi-region deployments for the authorization flow?  
A: The authorization flow involves a browser redirect and a back-channel token exchange. Both must hit the same region for the authorization code lookup to work. Options: (1) GeoDNS routes users to nearest region; ensure the authorization code is in that region's Redis. (2) Use a global Redis cluster (e.g., Redis Enterprise with active-active geo-replication) — the code is readable from any region. (3) Embed the region in the state parameter and enforce that the callback hits the same region's token endpoint. Option 3 is the simplest without global Redis.

Q: What is the right TTL for access tokens in an OAuth2 provider vs. an internal auth service?  
A: Internal auth service access tokens can be shorter (15 minutes) because they are validated inline and the internal revocation blocklist is checked by the gateway. OAuth2 provider access tokens for third-party resource servers are typically longer (1 hour) because resource servers may implement local introspection caching and the introspection endpoint adds latency. The trade-off: longer TTL = larger revocation window. For sensitive resources (financial, health), use shorter TTLs (15-30 minutes) and require fresh token exchange. For read-only, low-sensitivity scopes, 1-hour TTL with 2-minute introspection cache is acceptable.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Mitigation |
|---|---|---|
| Redis token store down | Introspection returns inactive; active tokens appear revoked | Redis Cluster auto-failover (< 10s); fall back to PostgreSQL for introspection (slower) |
| PostgreSQL down | New token issuances and revocations fail; introspection served from Redis | Patroni auto-failover (< 30s); Redis continues serving existing tokens |
| Auth Service (login) down | Authorization flow fails at login step | Auth Service has its own HA; fallback: return `temporarily_unavailable` with Retry-After |
| Code store Redis node failure | Authorization codes issued to failed node inaccessible | Redis Cluster replica promotes; codes issued during failover window may be lost (user must re-initiate flow) |
| Clock skew between pods | OIDC `auth_time` and `iat` claims inconsistent | NTP sync enforced; use monotonic clock for internal durations |
| Certificate expiry (TLS) | HTTPS connections fail | Automated cert renewal (Let's Encrypt ACME / AWS ACM); alert 30 days before expiry |
| Malformed token in introspection | Returns `{"active": false}` correctly | SHA-256 of any input is valid; no panic |
| PKCE bypass attempt | Attacker submits code without verifier | Server rejects: `pkce_challenge` is present; `code_verifier` is required and verified |

### Idempotency

- **Authorization code exchange**: Not idempotent — codes are single-use by design. Retry means re-initiating the auth flow.
- **Token revocation**: Idempotent — RFC 7009 requires 200 OK even if token not found.
- **Token introspection**: Naturally idempotent (read-only).
- **Client registration**: Idempotent by `client_id`; duplicate registration with same client_id returns existing record.

### Circuit Breaker

Wrap calls to: Auth Service (user login delegation), PostgreSQL (token writes), external email/notification services (consent notifications). Pattern identical to auth service (see `auth_service.md` §8). OAuth2 server fails safely: return `server_error` or `temporarily_unavailable` to clients (not an open failure that could leak tokens or bypass security checks).

---

## 9. Monitoring & Observability

### Metrics

| Metric | Alert |
|---|---|
| `oauth2_authorization_requests_total{result}` | Error rate > 5% |
| `oauth2_token_exchange_duration_seconds` p99 | > 200ms |
| `oauth2_introspection_cache_hit_ratio` | < 0.95 |
| `oauth2_token_revocations_total` | Spike > 10× baseline |
| `oauth2_pkce_failures_total` | Any > 0 per minute |
| `oauth2_invalid_redirect_uri_total` | Any sustained > 0 |
| `oauth2_refresh_token_reuse_detections` | Any > 0 |
| `oauth2_device_flow_pending_count` | > 100K |
| `oauth2_consent_denials_total{client_id}` | > 20% denial rate for a client |
| `oauth2_client_credentials_errors_total` | > 10% error rate |

### Distributed Tracing

- Every authorization request carries a `traceparent` header through redirect flows (embedded in `state` parameter encoding for browser flows where headers aren't preserved across redirects).
- Token endpoint spans: `validate_client` → `lookup_code` → `verify_pkce` → `issue_tokens` → `write_tokens`.
- Introspection spans labeled with cache hit/miss.

### Logging

- Never log client_secret, code_verifier, raw tokens, or PKCE challenges.
- Log `SHA256(token)[0:16]` for correlation.
- Consent decisions logged with user_id (masked), client_id, scope, and outcome.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Alternative | Rationale |
|---|---|---|---|
| PKCE mandatory vs. optional | Mandatory for all clients | Optional for confidential clients | RFC 9700 recommends mandatory; confidential clients also benefit |
| Access token format | Opaque (random + introspection) | JWT (self-contained) | Opaque: immediate revocation, no metadata leakage; JWT: no introspection call |
| Introspection caching | Redis (server-side) + resource server caching | No caching | Cache reduces 57K RPS to ~3K cache-miss RPS; 60s inconsistency window acceptable |
| Single-use authorization code | Redis GETDEL (atomic) | DB row with used_at flag | GETDEL is O(1) and atomic; DB approach has TOCTOU risk without serializable transaction |
| Scope model | resource:action namespacing | Flat string scopes | resource:action is readable, extensible, maps to API design naturally |
| Device flow polling | Redis GET (stateless) | Long-polling (SSE) | Redis polling is simpler; SSE adds persistent connection complexity |
| Consent persistence | Per (user, client) tuple | Re-prompt every flow | Persist: better UX; re-prompt: clearer user control; persisted with revocation mechanism is balanced |
| Implicit flow | Rejected with helpful error | Supported (deprecated) | Security: implicit was deprecated; redirect users to PKCE flow |

---

## 11. Follow-up Interview Questions

**Q1: What is the difference between OAuth2 and OIDC?**  
A: OAuth2 is an authorization framework — it defines how an application can obtain limited access to a user's resources without sharing credentials. It says nothing about who the user is. OpenID Connect (OIDC) is an identity layer on top of OAuth2 that adds authentication: it introduces the ID token (a JWT with identity claims like `sub`, `email`, `name`), the UserInfo endpoint, and standardized discovery. OAuth2 answers "what can this app access?"; OIDC answers "who is the user?" Use OIDC when you need user identity information; use plain OAuth2 for API authorization without identity.

**Q2: How do you prevent a confused deputy attack where a client misuses a token intended for a different service?**  
A: Use Resource Indicators (RFC 8707). The client specifies `resource=https://api.example.com` in the authorization request. The access token's `aud` claim is set to that resource URI. The resource server validates that the token's `aud` matches its own URI before accepting it. This prevents client A from obtaining a token for resource B and using it at resource C. Without audience validation, any resource server that accepts the issuer's tokens would accept tokens intended for any other resource.

**Q3: What is DPoP (Demonstrating Proof-of-Possession) and when would you implement it?**  
A: DPoP (RFC 9449) binds an access token to a client's proof-of-possession key. The client generates an asymmetric key pair; the public key is included in the token request; the server embeds a `cnf` (confirmation) claim in the token referencing the public key. On each API call, the client includes a DPoP proof header signed with the private key. Resource servers verify the signature matches the `cnf` claim, proving the caller possesses the private key. A stolen DPoP-bound token is useless without the private key. Implement for financial APIs (FAPI 2.0), sensitive admin APIs, or anywhere stolen bearer tokens are a significant threat. Complexity: client must manage key pairs and generate proofs per request.

**Q4: How do you implement pairwise subject identifiers for privacy?**  
A: OIDC supports pairwise `sub` values (opaque per-client user identifiers) in addition to public (consistent) `sub`. With pairwise subjects, the same user gets a different `sub` value at each client application, preventing correlation across clients. Implementation: `pairwise_sub = HMAC-SHA256(sector_identifier_uri + user_id, pairwise_secret)`. The `sector_identifier_uri` groups related clients (e.g., same company's multiple apps share a `sub`). Useful for privacy-sensitive platforms where preventing user tracking across unrelated applications is required.

**Q5: What is the Token Exchange grant type (RFC 8693) and when is it used?**  
A: Token Exchange allows a service to obtain a token on behalf of a user by presenting the user's token as a delegation credential. Use cases: (1) A backend service receives a user's token and needs to call another downstream service on the user's behalf — it exchanges the user token for a narrower-scoped token for the downstream service (impersonation). (2) Cross-domain identity propagation: user authenticates at IdP A, token is exchanged for a token from IdP B that trusts A (federation). Request: `grant_type=urn:ietf:params:oauth:grant-type:token-exchange`, `subject_token=<user_token>`, `requested_token_type=access_token`, `audience=https://downstream.service`. The authorization server validates the subject token and the requesting client's permission to exchange on behalf of that user.

**Q6: How do you handle token binding to prevent token theft from TLS session logs?**  
A: TLS Token Binding (RFC 8471) was designed for this but was abandoned by most browsers. Alternative: DPoP (see Q3). For server-to-server: mTLS (RFC 8705) — the client presents a TLS certificate; the token includes the certificate thumbprint (`x5t#S256` claim); resource servers verify the presented certificate matches the token binding. This prevents a stolen token from being used from a different TLS connection. mTLS is practical for M2M scenarios; DPoP is the emerging standard for browser and mobile clients.

**Q7: What is the security implication of including user claims directly in the access token vs. only in the ID token?**  
A: Access tokens are opaque credentials for API access and are sent to every resource server. If user PII (email, name, phone) is embedded in the access token JWT, it is exposed to every resource server the token is presented to. ID tokens are only intended for the client application (the relying party) that initiated the authorization flow — they should never be sent to resource servers. Best practice: access tokens include only authorization-relevant claims (`sub`, `scope`, `client_id`, `aud`, `exp`). Resource servers that need user attributes call the UserInfo endpoint or receive user claims from their own user profile store using the `sub` as a key. This minimizes PII exposure.

**Q8: How do you test the security of your OAuth2 implementation?**  
A: Automated testing layers: (1) Unit tests: PKCE verification logic, scope validation, redirect URI matching (include fuzz tests with malformed URIs). (2) Integration tests: complete authorization code flows with test clients; verify single-use codes; verify expired code rejection; verify reuse detection triggers. (3) Security-specific tests: open redirect attempts (redirect_uri manipulation), PKCE bypass (omitting code_verifier), state omission (CSRF simulation), implicit flow rejection, scope escalation in refresh, audience confusion. (4) Penetration testing: hire external pentesters with the OWASP OAuth2 threat model checklist. (5) RFC compliance testing: use open-source OIDC conformance suite (OpenID Foundation's conformance tests).

**Q9: How does the authorization server handle prompt=none (silent authentication)?**  
A: `prompt=none` requests that the authorization server not show any UI (no login page, no consent screen). If the user is already authenticated and has consented to the requested scopes, the authorization server redirects immediately with an authorization code. If not authenticated or not consented, it redirects with `error=login_required` or `error=consent_required`. Use case: SPAs checking whether a user is still logged in without disrupting the UI. Security: ensure `prompt=none` does not bypass consent for new scopes — if the user has never consented to a scope, return `consent_required` even with `prompt=none`.

**Q10: What is the back-channel logout specification and how is it different from front-channel logout?**  
A: Both are OIDC session management specifications. Front-channel logout (via iframes): the authorization server redirects the browser to each registered logout URI, causing the browser to hit each relying party's logout endpoint. Simple but requires browser participation (tab must be open, iframes allowed). Back-channel logout (RFC 9099): the authorization server sends a signed HTTP POST to each registered back-channel logout URI with a "logout token" (JWT with `sid` or `sub`) directly from server to server, without browser involvement. More reliable: works even if the user closed the browser tab. Trade-off: requires each relying party to expose an HTTPS server endpoint and validate the logout token. Covered in more detail in `sso.md`.

**Q11: How do you prevent authorization code injection attacks?**  
A: Authorization code injection: an attacker obtains a valid authorization code (possibly from their own account at a victim IdP) and injects it into a victim's session by sending them a crafted callback URL. Mitigations: (1) `state` parameter: client verifies that the state in the callback matches what was stored in session before initiating the flow — if the victim didn't start this flow, `state` won't match. (2) PKCE: the code is bound to a `code_challenge` that only the original initiating client can satisfy. (3) OIDC `nonce`: for ID tokens, include a nonce in the authorization request; verify the nonce in the returned ID token. Together, these three mechanisms make code injection attacks infeasible.

**Q12: How do you handle the case where a client's redirect_uri changes (e.g., domain migration)?**  
A: Strict policy: clients must update their registered redirect URIs through the client management API before changing their callback URL in production. The update process: (1) Client adds the new redirect URI to their registered list (both old and new now registered). (2) Client deploys the code change pointing to the new URI. (3) After confirming all traffic uses the new URI, client removes the old URI from registration. This blue-green approach ensures zero downtime during migration. Emergency protocol: client contacts platform support with proof of domain ownership; support updates the redirect URI directly. Always require exact domain match; never allow wildcards (e.g., `*.example.com`) in redirect URIs.

**Q13: What is the Phantom Token pattern?**  
A: A pattern for combining opaque tokens (external, sent to third-party clients) with JWTs (internal, used between microservices). Flow: (1) Client presents an opaque access token to an API gateway. (2) API gateway calls the authorization server's introspection endpoint to validate the opaque token and receive its metadata. (3) The API gateway creates an internal JWT containing the token's claims and forwards it to backend microservices. (4) Backend microservices validate the internal JWT locally (stateless, fast). Benefits: clients/third-party services never see JWTs (no metadata leakage from JWT payload); internal services get fast JWT validation; introspection call is cached at the gateway layer.

**Q14: What are the security considerations for the `client_secret` in a confidential client?**  
A: (1) Generate with CSPRNG, minimum 256 bits entropy. (2) Store hash (same approach as API key storage in `api_key_management.md`) — client_secret_hash in the database, never plaintext. (3) Transmit only over HTTPS; never in URLs (only in Authorization header Basic or POST body). (4) Support rotation: old secret and new secret both valid for a transition period (30 days); after rotation, old secret invalid. (5) Scope the secret to specific environments (staging secret ≠ production secret). (6) Alert if the same client_id is used from more than N distinct IP addresses/hour (indicates secret compromise). (7) For mobile/native clients that bundle a "client_secret," this is not truly confidential — use public client type with PKCE only.

**Q15: How do you implement the user-facing application management page (like Google's "Third-party apps with account access")?**  
A: The page shows all consent grants for the user. Implementation: (1) Query `consent_grants` table by user_id, join with `oauth2_clients` for app name/logo. (2) Also query `oauth2_token_families` for last_used_at (aggregate from refresh tokens). Display: app name, logo, granted scopes (human-readable), last used date, "Revoke access" button. Revoke action: (1) DELETE consent grant; (2) Revoke all active token families for (user_id, client_id); (3) Revoke all active access tokens for (user_id, client_id) by adding jti values to revocation set with TTL. Emit audit event. This gives users full control over third-party access and is required for GDPR compliance.

---

## 12. References & Further Reading

- **RFC 6749** — The OAuth 2.0 Authorization Framework. https://www.rfc-editor.org/rfc/rfc6749
- **RFC 7636** — Proof Key for Code Exchange (PKCE) by OAuth Public Clients. https://www.rfc-editor.org/rfc/rfc7636
- **RFC 7662** — OAuth 2.0 Token Introspection. https://www.rfc-editor.org/rfc/rfc7662
- **RFC 7009** — OAuth 2.0 Token Revocation. https://www.rfc-editor.org/rfc/rfc7009
- **RFC 8628** — OAuth 2.0 Device Authorization Grant. https://www.rfc-editor.org/rfc/rfc8628
- **RFC 8693** — OAuth 2.0 Token Exchange. https://www.rfc-editor.org/rfc/rfc8693
- **RFC 8707** — Resource Indicators for OAuth 2.0. https://www.rfc-editor.org/rfc/rfc8707
- **RFC 9449** — OAuth 2.0 Demonstrating Proof of Possession (DPoP). https://www.rfc-editor.org/rfc/rfc9449
- **RFC 9700** — OAuth 2.0 Security Best Current Practice. https://www.rfc-editor.org/rfc/rfc9700
- **OpenID Connect Core 1.0**. https://openid.net/specs/openid-connect-core-1_0.html
- **OpenID Connect Discovery 1.0**. https://openid.net/specs/openid-connect-discovery-1_0.html
- **RFC 8414** — OAuth 2.0 Authorization Server Metadata. https://www.rfc-editor.org/rfc/rfc8414
- **RFC 8705** — OAuth 2.0 Mutual-TLS Client Authentication and Certificate-Bound Access Tokens. https://www.rfc-editor.org/rfc/rfc8705
- **OWASP OAuth2 Cheat Sheet**. https://cheatsheetseries.owasp.org/cheatsheets/OAuth2_Cheat_Sheet.html
- **Auth0 — OAuth2 Threat Model**. https://auth0.com/docs/secure/security-guidance/prevent-threats
- **Okta Developer Blog — OAuth2 Best Practices**. https://developer.okta.com/blog/2019/10/21/illustrated-guide-to-oauth-and-oidc
- **Financial-grade API (FAPI) Security Profile**. https://openid.net/wg/fapi/
