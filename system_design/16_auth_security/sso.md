# System Design: Single Sign-On (SSO) Service

---

## 1. Requirement Clarifications

### Functional Requirements

1. **OIDC-Based SSO (OpenID Connect)** — act as an OpenID Connect Relying Party (RP) when integrating with external Identity Providers (IdPs); also act as an OIDC Provider (OP) for downstream Service Providers (SPs) within the platform.
2. **SAML 2.0 SP-Initiated SSO** — support SAML 2.0 Web Browser SSO Profile as a Service Provider; accept SAML Assertions from enterprise IdPs (Okta, Azure AD, Ping Identity, OneLogin, ADFS); SP-initiated and IdP-initiated flows.
3. **Identity Provider Discovery** — determine which IdP to route a user to based on their email domain (IdP discovery); support multiple IdPs per tenant; support "home realm discovery."
4. **Session Federation** — when a user authenticates via an external IdP, create a local platform session (issued by the platform's Auth Service); synchronize session lifetime with IdP session where possible.
5. **Single Logout (SLO)** — propagate logout to all federated sessions: both front-channel (browser-based iframe) and back-channel (HTTP POST) logout flows; SAML SLO and OIDC RP-Initiated Logout.
6. **Attribute Mapping** — translate identity attributes from IdP format to platform's user profile format (e.g., map `http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress` from SAML to platform's `email` field).
7. **Just-in-Time (JIT) Provisioning** — automatically create or update user accounts in the platform on first SSO login based on IdP-provided attributes (and configured mapping rules).
8. **Trust Establishment** — manage IdP-SP trust: exchange SAML metadata or OIDC client registrations; store and rotate IdP certificates/keys; validate IdP signatures.
9. **MFA Pass-Through / Enforcement** — if the enterprise IdP performed MFA, propagate the `amr` (Authentication Method Reference) claim to downstream platform tokens; optionally enforce that IdP MFA was performed before granting access.
10. **Audit Logging** — record all SSO events: login success/failure, IdP used, user provisioned/updated, SLO triggered, attribute mapping applied.
11. **Admin Configuration** — enterprise admins configure their IdP integration: upload SAML metadata XML or enter OIDC discovery URL; configure attribute mappings; test the integration; view SSO audit log.

### Non-Functional Requirements

1. **Availability** — 99.99% uptime; SSO is a dependency for all enterprise customers whose employees cannot log in without it.
2. **Latency** — SSO login round-trip p99 < 2 seconds (including browser redirects; dominated by IdP latency, not platform processing); platform processing < 100 ms p99.
3. **Throughput** — 5,000 SSO logins/second peak; 50,000 session validation requests/second.
4. **Security** — full SAML 2.0 security profile compliance; validate all signatures; prevent XML Signature Wrapping attacks; prevent replay attacks; enforce TLS everywhere.
5. **Scalability** — stateless platform SSO pods; IdP configuration cached; horizontally scalable.
6. **Compliance** — support for attribute-based access control (ABAC) from IdP-provided groups/roles for enterprise compliance requirements.
7. **Protocol Support** — SAML 2.0, OIDC 1.0, WS-Federation (read-only support; do not issue WS-Fed tokens).

### Out of Scope

- Acting as an IdP for SAML to external parties (the platform is an SP, not a SAML IdP, in enterprise federation scenarios).
- Password/MFA-based login — handled by Auth Service (`auth_service.md`).
- OAuth2 Provider flows — handled by `oauth2_provider.md`.
- Directory Services (LDAP/AD) direct integration (assumed to be via SAML/OIDC from the IdP).

---

## 2. Users & Scale

### User Types

| Type | Description | Primary Flows |
|---|---|---|
| Enterprise Employee | Human logging in via company IdP (Okta, Azure AD) | SAML SSO, OIDC SSO, SLO |
| IT Administrator | Configures IdP integration, attribute mappings | Admin config API |
| Platform Service | Validates federated sessions | Session validation |
| External IdP | Sends SAML assertions or OIDC tokens | SAML POST, OIDC token endpoint |

### Traffic Estimates

**Assumptions:**
- Platform has 5,000 enterprise customers.
- Average 200 employees per customer = 1,000,000 enterprise users total.
- 60% of enterprise users do SSO login per day = 600,000 SSO logins/day.
- Each login involves: 1 IdP discovery lookup + 1 SAML/OIDC redirect + 1 assertion validation + 1 platform session creation.
- Session validation: 20 API calls per user per day = 600K × 20 = 12M/day (mostly JWT validation; ~10% require SSO session check = 1.2M SSO session checks/day).
- SLO events: 600,000/day (every login has a corresponding logout eventually).
- JIT provisioning: 2% of logins result in new user creation = 12,000/day.

| Event | Daily Volume | Peak Multiplier | Peak RPS |
|---|---|---|---|
| SSO Login (SAML assertion or OIDC token validation) | 600,000 | 5× | 600K × 5 / 86,400 = **34.7 RPS** |
| IdP Discovery Lookup | 600,000 | 5× | **34.7 RPS** |
| Platform Session Creation (post-SSO) | 600,000 | 5× | **34.7 RPS** |
| Session Validation (SSO session check) | 1,200,000 | 10× | 1.2M × 10 / 86,400 = **138.9 RPS** |
| SLO Events | 600,000 | 3× | **20.8 RPS** |
| JIT Provisioning | 12,000 | 2× | **0.3 RPS** |
| Admin Configuration Reads | 50,000 | 2× | **1.2 RPS** |

**Design note:** SSO is much lower volume than the Auth Service (600K vs 25M logins/day). The complexity is in the protocol implementation and multi-IdP support, not scale. Platform processing latency must be minimal since IdP round-trip already adds 500ms-1s.

### Latency Requirements

| Operation | Target p50 | Target p99 | Constraint |
|---|---|---|---|
| IdP discovery lookup | 1 ms | 5 ms | Redis cache hit |
| SAML assertion validation | 5 ms | 30 ms | XML parse + signature verify |
| OIDC token validation | 2 ms | 10 ms | JWT signature verify (cached JWKS) |
| JIT provisioning | 20 ms | 100 ms | DB read + conditional write |
| Platform session creation | 5 ms | 20 ms | Token sign + cache write |
| SLO propagation (async) | 100 ms | 2000 ms | Network calls to SP/RP endpoints |

### Storage Estimates

| Data | Record Size | Volume | Storage |
|---|---|---|---|
| IdP configurations | 10 KB (includes SAML metadata XML) | 5,000 IdPs | 50 MB |
| IdP signing certificates (PEM) | 4 KB | 5,000 (avg 1-2 certs/IdP for rotation) | 20 MB |
| User → IdP mapping (identity federation records) | 256 bytes | 1,000,000 users | 256 MB |
| SAML assertion replay cache (5 min window) | 64 bytes | Peak concurrent: 34.7 RPS × 300s = 10K | 640 KB (negligible) |
| Active SSO sessions | 512 bytes | 600K concurrent active | 307 MB |
| Attribute mapping rules | 1 KB | 5,000 configs × avg 10 rules = 50K rules | 50 MB |
| Audit log (hot, 90 days) | 512 bytes/event | 2.4M events/day × 90 = 216M | 111 GB |

### Bandwidth Estimates

| Flow | Payload | Daily Requests | Daily Bandwidth |
|---|---|---|---|
| SAML AuthnRequest (outbound to IdP) | 2 KB | 400K (SAML logins) | 800 MB |
| SAML Assertion (inbound from IdP) | 8 KB (signed XML) | 400K | 3.2 GB |
| OIDC token response (from IdP) | 4 KB | 200K | 800 MB |
| SLO requests/responses | 2 KB | 600K | 1.2 GB |
| **Total/day** | | | **~6 GB/day (~560 Mbps peak)** |

---

## 3. High-Level Architecture

```
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │                    Enterprise Users (Employees)                              │
  │                         Browser-based SSO Login                              │
  └──────────────────────┬──────────────────────────────────────────────────────┘
                         │ 1. Access platform URL
                         ▼
  ┌──────────────────────────────────────────────────────────────────────────────┐
  │                  Platform (Service Provider)                                  │
  │              https://app.platform.example.com                                │
  │                                                                              │
  │  ┌──────────────────────────────────────────────────────────────────────┐   │
  │  │                         SSO Service                                  │   │
  │  │                   (Stateless pods, N replicas)                       │   │
  │  │                                                                      │   │
  │  │   ┌──────────────────┐   ┌──────────────────┐   ┌────────────────┐  │   │
  │  │   │ IdP Discovery    │   │ SAML Handler     │   │ OIDC Handler   │  │   │
  │  │   │ - Email domain   │   │ - AuthnRequest   │   │ - Auth Code    │  │   │
  │  │   │   lookup         │   │   generation     │   │   initiation   │  │   │
  │  │   │ - Tenant lookup  │   │ - Assertion      │   │ - Token        │  │   │
  │  │   │                  │   │   validation     │   │   exchange     │  │   │
  │  │   └──────────────────┘   │ - Signature      │   │ - JWKS fetch   │  │   │
  │  │                          │   verification   │   └────────────────┘  │   │
  │  │   ┌──────────────────┐   └──────────────────┘                       │   │
  │  │   │ SLO Handler      │                          ┌────────────────┐  │   │
  │  │   │ - SAML SLO       │   ┌──────────────────┐   │ JIT Provisioner│  │   │
  │  │   │   (front/back)   │   │ Attribute Mapper │   │ - User create  │  │   │
  │  │   │ - OIDC RP-Init   │   │ - Claim transform│   │ - User update  │  │   │
  │  │   │   Logout         │   │ - Group mapping  │   │ - Role sync    │  │   │
  │  │   └──────────────────┘   └──────────────────┘   └────────────────┘  │   │
  │  └──────────────────────────────────────────────────────────────────────┘   │
  │                                                                              │
  │  ┌──────────────────┐  ┌───────────────────┐  ┌──────────────────────────┐  │
  │  │ IdP Config Store │  │ Federation Store  │  │ Auth Service (internal)  │  │
  │  │ (PostgreSQL,     │  │ (PostgreSQL)      │  │ - Issues platform tokens │  │
  │  │  Redis cache)    │  │ - identity links  │  │ - Validates sessions     │  │
  │  │                  │  │ - SSO sessions    │  └──────────────────────────┘  │
  │  │ - SAML metadata  │  └───────────────────┘                               │
  │  │ - OIDC config    │                            ┌──────────────────────┐   │
  │  │ - cert store     │  ┌───────────────────┐    │ Audit Log            │   │
  │  │ - attr mappings  │  │ Replay Cache      │    │ (Kafka → ClickHouse) │   │
  │  └──────────────────┘  │ (Redis, TTL 5min) │    └──────────────────────┘   │
  │                         │ - SAML assertion  │                               │
  │                         │   IDs             │                               │
  │                         └───────────────────┘                               │
  └──────────────────────────────────────────────────────────────────────────────┘
                │ 2. SAML AuthnRequest (redirect) or OIDC authorization request
                │ 5. SAML POST Assertion (back-channel) or OIDC callback
                ▼
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │                    Enterprise Identity Providers                             │
  │    Okta / Azure AD / Ping Identity / OneLogin / ADFS / Google Workspace     │
  │                                                                              │
  │   Each IdP has:                                                              │
  │   - A SAML SSO URL or OIDC authorization endpoint                            │
  │   - A signing certificate or JWKS URL                                        │
  │   - The platform's SP metadata (registered)                                  │
  └─────────────────────────────────────────────────────────────────────────────┘

  ┌─────────────────────────────────────────────────────────────────────────────┐
  │                    SLO Propagation Flow                                      │
  │                                                                              │
  │   User initiates logout at platform (or IdP):                                │
  │                                                                              │
  │   SAML SLO (back-channel):                                                   │
  │   Platform → HTTP POST {SAML LogoutRequest} → IdP SLO endpoint               │
  │   IdP → HTTP POST {SAML LogoutResponse} → Platform SLO endpoint              │
  │   IdP → {notify all other SPs with active sessions for this user}            │
  │                                                                              │
  │   OIDC RP-Initiated Logout:                                                  │
  │   Platform → Redirect browser → IdP /end_session endpoint                   │
  │             {id_token_hint, post_logout_redirect_uri}                        │
  │                                                                              │
  │   Back-Channel Logout (OIDC):                                                │
  │   IdP → HTTP POST {logout_token JWT} → Platform back-channel logout URI      │
  │   Platform → Revoke platform session for {sub, sid} in logout_token          │
  └─────────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

- **SSO Service (stateless pods)**: Orchestrates all SSO protocol flows. Stateless — all state in backing stores. SAML and OIDC handlers are protocol-specific but share common IdP config and session management.
- **IdP Config Store (PostgreSQL + Redis)**: Stores all IdP configurations: SAML metadata (entity ID, SSO URL, signing certificate), OIDC config (client_id, client_secret, discovery URL, JWKS). Redis cache with 1-hour TTL serves the hot path (IdP lookup on every login).
- **Federation Store (PostgreSQL)**: Links platform user accounts to external IdP subject identifiers (`nameId` in SAML, `sub` in OIDC). Enables the same person to have consistent platform account across SSO sessions.
- **Replay Cache (Redis)**: Stores SAML assertion IDs with 5-minute TTL. Prevents replay attacks where an intercepted assertion is resubmitted. Redis TTL handles natural expiry.
- **JIT Provisioner**: Creates/updates platform user accounts from IdP-provided attributes. Runs synchronously in the SSO login flow (fast: read-then-conditional-write).
- **Attribute Mapper**: Translates IdP-specific claim names and formats to platform's canonical user schema. Configuration-driven (admin-defined rules).
- **Auth Service (internal gRPC)**: The SSO Service calls Auth Service to create a platform session after successful IdP authentication. SSO Service handles protocol; Auth Service handles token issuance.
- **SLO Handler**: Manages logout propagation. Back-channel logout calls are made server-to-server (HTTP POST); front-channel uses browser iframes.

**Primary Use-Case Data Flow (SAML SP-Initiated SSO):**

```
1.  User navigates to https://app.platform.example.com
    Not logged in → redirected to /login

2.  User enters email: alice@enterprise-corp.com
    Or: enterprise URL directly https://app.platform.example.com/login/enterprise-corp.com

3.  IdP Discovery:
    a. Extract email domain: "enterprise-corp.com"
    b. Redis GET "sso:domain:enterprise-corp.com" → {idp_id, protocol}
    c. If miss: PostgreSQL lookup → warm Redis cache

4.  Generate SAML AuthnRequest:
    a. requestID = Random UUID (store for validation)
    b. AuthnRequest XML:
       <samlp:AuthnRequest
         ID="_requestID"
         Version="2.0"
         IssueInstant="2026-04-09T10:00:00Z"
         AssertionConsumerServiceURL="https://platform.example.com/sso/saml/acs"
         Destination="https://idp.enterprise-corp.com/sso/saml"
         ProtocolBinding="urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST"
         ForceAuthn="false"
         IsPassive="false">
         <saml:Issuer>https://platform.example.com/sso/saml/metadata</saml:Issuer>
         <samlp:NameIDPolicy Format="urn:oasis:names:tc:SAML:1.1:nameid-format:emailAddress"/>
         <samlp:RequestedAuthnContext Comparison="minimum">
           <saml:AuthnContextClassRef>
             urn:oasis:names:tc:SAML:2.0:ac:classes:PasswordProtectedTransport
           </saml:AuthnContextClassRef>
         </samlp:RequestedAuthnContext>
       </samlp:AuthnRequest>
    c. Deflate + base64 → SAMLRequest parameter
    d. Sign using platform's SP private key (RSA-SHA256)
    e. Store {requestID → {idp_id, created_at, relay_state}} in Redis (TTL 10 min)
    f. Redirect browser to IdP SSO URL with SAMLRequest + RelayState

5.  IdP authenticates the user (username/password, MFA, etc.)
    IdP generates signed SAML Assertion.

6.  IdP POSTs to platform ACS (Assertion Consumer Service):
    POST https://platform.example.com/sso/saml/acs
    SAMLResponse=<base64(XML)>&RelayState=<original_state>

7.  Platform SSO Service processes assertion:
    a. Base64 decode + XML parse SAMLResponse
    b. Validate XML structure (prevent XML Signature Wrapping)
    c. Find signing certificate for IdP from IdP config store
    d. Verify XML digital signature (RSA-SHA256 or RSA-SHA512)
    e. Validate:
       - InResponseTo == stored requestID (prevents injection)
       - Conditions.NotBefore <= now() <= Conditions.NotOnOrAfter
       - AudienceRestriction.Audience contains platform's SP EntityID
       - AssertionID not in replay cache (prevent replay)
       - IssuerEntityID matches expected IdP entity ID
    f. Store AssertionID in Redis replay cache (TTL = NotOnOrAfter - now())
    g. Extract NameID (subject) and attributes

8.  Attribute Mapping:
    Raw SAML attributes → Platform canonical schema
    e.g., "urn:oid:0.9.2342.19200300.100.1.3" → "email"
         "http://schemas.xmlsoap.org/claims/Group" → platform groups

9.  JIT Provisioning:
    a. Look up existing federation record: SELECT * FROM identity_federations
       WHERE idp_id=? AND external_subject=?
    b. If found: UPDATE user attributes if changed; return user_id
    c. If not found:
       - Check if email already exists in platform (link accounts if policy allows)
       - Create new platform user with provisioned attributes
       - Create identity_federation record linking platform_user_id ↔ external_subject
       - Emit provisioning audit event

10. Create platform session (call Auth Service gRPC):
    a. Issue platform access token (JWT, 1h TTL) and refresh token
    b. amr includes IdP's authentication methods if available
    c. Store SSO session: {platform_session_id, idp_session_id (NameID), idp_id, user_id}

11. Redirect user to original destination (from RelayState).
    Set platform session cookies.
```

---

## 4. Data Model

### Entities & Schema

```sql
-- =============================================
-- Identity Provider Configurations
-- =============================================
CREATE TABLE identity_providers (
    id                          UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    tenant_id                   UUID            NOT NULL,
    
    -- Identification
    display_name                VARCHAR(255)    NOT NULL,             -- "Acme Corp Okta"
    protocol                    VARCHAR(20)     NOT NULL 
                                    CHECK (protocol IN ('saml2','oidc','wsfed')),
    is_active                   BOOLEAN         NOT NULL DEFAULT TRUE,
    
    -- SAML 2.0 Configuration
    saml_entity_id              VARCHAR(1024),                        -- IdP entity ID (SP uses this to verify issuer)
    saml_sso_url                VARCHAR(2048),                        -- IdP SSO endpoint for AuthnRequest redirect
    saml_slo_url                VARCHAR(2048),                        -- IdP SLO endpoint
    saml_name_id_format         VARCHAR(256)    DEFAULT 'urn:oasis:names:tc:SAML:1.1:nameid-format:emailAddress',
    saml_sign_requests          BOOLEAN         NOT NULL DEFAULT TRUE,
    saml_metadata_url           VARCHAR(2048),                        -- IdP metadata URL (for auto-refresh)
    saml_metadata_xml           TEXT,                                 -- Cached/uploaded IdP metadata
    
    -- OIDC Configuration
    oidc_issuer                 VARCHAR(1024),                        -- e.g. "https://accounts.google.com"
    oidc_discovery_url          VARCHAR(2048),                        -- /.well-known/openid-configuration
    oidc_client_id              VARCHAR(512),
    oidc_client_secret_enc      BYTEA,                                -- AES-256-GCM encrypted
    oidc_client_secret_key_id   VARCHAR(128),                         -- KMS key reference
    oidc_scopes                 TEXT[]          DEFAULT ARRAY['openid','profile','email'],
    oidc_response_type          VARCHAR(50)     DEFAULT 'code',
    oidc_pkce_enabled           BOOLEAN         NOT NULL DEFAULT TRUE,
    oidc_cached_jwks            JSONB,                                -- cached IdP JWKS
    oidc_jwks_cached_at         TIMESTAMPTZ,
    
    -- Attribute mapping config
    attribute_mapping           JSONB           NOT NULL DEFAULT '{}',
    -- Example:
    -- {"email": "http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress",
    --  "first_name": "givenname",
    --  "last_name": "sn",
    --  "groups": "memberOf"}
    
    -- JIT provisioning config
    jit_provisioning_enabled    BOOLEAN         NOT NULL DEFAULT TRUE,
    jit_default_role            VARCHAR(100),
    jit_group_sync              BOOLEAN         NOT NULL DEFAULT FALSE,
    
    -- MFA enforcement
    require_mfa_from_idp        BOOLEAN         NOT NULL DEFAULT FALSE,
    required_amr                TEXT[],                               -- e.g. ['mfa','otp']
    
    -- Metadata
    created_at                  TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    updated_at                  TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    created_by_user_id          UUID
);

CREATE INDEX idx_idp_tenant ON identity_providers (tenant_id, is_active);

-- =============================================
-- IdP Signing Certificates (SAML)
-- =============================================
CREATE TABLE idp_signing_certificates (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    idp_id              UUID            NOT NULL REFERENCES identity_providers(id) ON DELETE CASCADE,
    certificate_pem     TEXT            NOT NULL,                     -- X.509 PEM
    fingerprint_sha256  CHAR(64)        NOT NULL,                     -- for display/verification
    is_active           BOOLEAN         NOT NULL DEFAULT TRUE,
    valid_from          TIMESTAMPTZ,
    valid_until         TIMESTAMPTZ,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

CREATE INDEX idx_cert_idp ON idp_signing_certificates (idp_id, is_active);

-- =============================================
-- SP Signing Keys (used to sign AuthnRequests)
-- =============================================
CREATE TABLE sp_signing_keys (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    key_id_label        VARCHAR(64)     NOT NULL UNIQUE,              -- e.g. "2026-Q1"
    private_key_ref     VARCHAR(256),                                 -- KMS/Vault key reference
    certificate_pem     TEXT            NOT NULL,                     -- X.509 PEM, public key
    is_active           BOOLEAN         NOT NULL DEFAULT TRUE,
    valid_from          TIMESTAMPTZ     NOT NULL,
    valid_until         TIMESTAMPTZ,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW()
);

-- =============================================
-- Email domain → IdP routing
-- =============================================
CREATE TABLE email_domain_idp_mappings (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    email_domain        VARCHAR(255)    NOT NULL,                     -- "enterprise-corp.com"
    idp_id              UUID            NOT NULL REFERENCES identity_providers(id),
    is_primary          BOOLEAN         NOT NULL DEFAULT TRUE,
    priority            INTEGER         NOT NULL DEFAULT 0,
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    UNIQUE (email_domain, idp_id)
);

CREATE INDEX idx_eddm_domain ON email_domain_idp_mappings (email_domain, is_primary);

-- =============================================
-- Identity Federations: link platform users to IdP subjects
-- =============================================
CREATE TABLE identity_federations (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    platform_user_id    UUID            NOT NULL,                     -- FK to users.id
    idp_id              UUID            NOT NULL REFERENCES identity_providers(id),
    external_subject    VARCHAR(1024)   NOT NULL,                     -- NameID or OIDC sub
    external_subject_format VARCHAR(256),                             -- SAML NameID format
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    last_login_at       TIMESTAMPTZ,
    UNIQUE (idp_id, external_subject)
);

CREATE INDEX idx_if_user ON identity_federations (platform_user_id);
CREATE INDEX idx_if_idp_subject ON identity_federations (idp_id, external_subject);

-- =============================================
-- Active SSO Sessions (for SLO tracking)
-- =============================================
CREATE TABLE sso_sessions (
    id                  UUID            PRIMARY KEY DEFAULT gen_random_uuid(),
    platform_user_id    UUID            NOT NULL,
    idp_id              UUID            NOT NULL REFERENCES identity_providers(id),
    platform_session_id UUID            NOT NULL,                     -- auth service session
    
    -- IdP session identifiers (needed for SLO)
    idp_session_index   VARCHAR(256),                                 -- SAML SessionIndex
    idp_name_id         VARCHAR(1024),                                -- SAML NameID (for SLO)
    idp_name_id_format  VARCHAR(256),
    
    -- OIDC session
    oidc_id_token       TEXT,                                         -- stored for id_token_hint in RP-initiated logout
    oidc_sid            VARCHAR(256),                                 -- OIDC session ID from IdP
    
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    expires_at          TIMESTAMPTZ     NOT NULL,
    terminated_at       TIMESTAMPTZ,
    termination_reason  VARCHAR(50)
);

CREATE INDEX idx_ssos_user ON sso_sessions (platform_user_id, terminated_at);
CREATE INDEX idx_ssos_platform_session ON sso_sessions (platform_session_id);
CREATE INDEX idx_ssos_idp_name_id ON sso_sessions (idp_id, idp_name_id)
    WHERE terminated_at IS NULL;

-- =============================================
-- SAML Assertion Replay Prevention (also mirrored in Redis with TTL)
-- =============================================
CREATE TABLE saml_assertion_ids (
    assertion_id        VARCHAR(256)    NOT NULL,
    idp_id              UUID            NOT NULL,
    received_at         TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    not_on_or_after     TIMESTAMPTZ     NOT NULL,
    PRIMARY KEY (idp_id, assertion_id)
);

CREATE INDEX idx_sai_cleanup ON saml_assertion_ids (not_on_or_after);

-- =============================================
-- SSO Audit Log
-- =============================================
CREATE TABLE sso_audit_events (
    id                  UUID            DEFAULT gen_random_uuid(),
    event_type          VARCHAR(60)     NOT NULL,
    -- event_types: sso_login_success, sso_login_failure, sso_jit_provisioned,
    --              sso_jit_updated, sso_logout, slo_initiated, slo_completed,
    --              idp_config_created, idp_config_updated, saml_replay_detected,
    --              signature_validation_failed, attribute_mapping_error
    event_timestamp     TIMESTAMPTZ     NOT NULL DEFAULT NOW(),
    tenant_id           UUID,
    idp_id              UUID,
    platform_user_id    UUID,
    idp_external_subject VARCHAR(1024),
    protocol            VARCHAR(20),
    ip_address          INET,
    user_agent          TEXT,
    metadata            JSONB,
    PRIMARY KEY (event_timestamp, id)
) PARTITION BY RANGE (event_timestamp);

CREATE INDEX idx_sae_user ON sso_audit_events (platform_user_id, event_timestamp DESC);
CREATE INDEX idx_sae_tenant ON sso_audit_events (tenant_id, event_timestamp DESC);
```

### Database Choice

| Database | Fit | Reasoning |
|---|---|---|
| **PostgreSQL** | **Selected (all persistent data)** | ACID for JIT provisioning (upsert user + create federation record in one transaction); JSONB for attribute mappings and metadata; mature, well-understood |
| **Redis** | **Selected (discovery cache, replay cache, session cache)** | Sub-millisecond IdP discovery lookup; TTL-native for SAML replay prevention; session hot path |
| MongoDB | Not selected | Flexible schema is not needed; ACID is required for JIT provisioning transactions |
| DynamoDB | Alternative | Auto-scaling; eventual consistency on GSIs is concern for federation record lookups |

**JIT provisioning atomicity in PostgreSQL:**
```sql
-- Atomic upsert: link IdP subject to platform user
INSERT INTO identity_federations (platform_user_id, idp_id, external_subject, last_login_at)
VALUES (?, ?, ?, NOW())
ON CONFLICT (idp_id, external_subject)
DO UPDATE SET last_login_at = NOW()
RETURNING id, platform_user_id;
```

---

## 5. API Design

### Inbound SSO Endpoints (for IdP and Browser)

---

### GET /sso/saml/metadata

**Auth:** None  
**Cache:** `Cache-Control: public, max-age=86400`

```
Response 200 OK:
  Content-Type: application/samlmetadata+xml

  <md:EntityDescriptor xmlns:md="urn:oasis:names:tc:SAML:2.0:metadata"
    entityID="https://platform.example.com/sso/saml/metadata">
    <md:SPSSODescriptor
      AuthnRequestsSigned="true"
      WantAssertionsSigned="true"
      protocolSupportEnumeration="urn:oasis:names:tc:SAML:2.0:protocol">
      
      <md:KeyDescriptor use="signing">
        <ds:KeyInfo>
          <ds:X509Data><ds:X509Certificate>MIIBkD...</ds:X509Certificate></ds:X509Data>
        </ds:KeyInfo>
      </md:KeyDescriptor>
      
      <md:SingleLogoutService
        Binding="urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST"
        Location="https://platform.example.com/sso/saml/slo"/>
      
      <md:AssertionConsumerService
        Binding="urn:oasis:names:tc:SAML:2.0:bindings:HTTP-POST"
        Location="https://platform.example.com/sso/saml/acs"
        index="1" isDefault="true"/>
    </md:SPSSODescriptor>
  </md:EntityDescriptor>
```

---

### GET /sso/login

**Auth:** None — initiates SSO flow  
**Rate Limit:** 50 requests/IP/minute

```
Query Parameters:
  email=alice@enterprise-corp.com     // for email-based discovery
  // OR
  tenant_slug=enterprise-corp         // for direct tenant routing
  // OR
  idp_hint=<idp_id>                   // for direct IdP selection

Response: Redirect to IdP SSO URL (SAML redirect or OIDC authorization request)
  302 → https://idp.enterprise-corp.com/sso/saml?SAMLRequest=<deflated_base64>&RelayState=<state>

Response 400 (no IdP found):
  { "error": "idp_not_found", "message": "No SSO provider configured for this email domain." }
```

---

### POST /sso/saml/acs

**Auth:** SAML assertion (self-contained signature)  
**Rate Limit:** 100 requests/IP/minute

```
Request (from browser, POST from IdP):
  Content-Type: application/x-www-form-urlencoded
  SAMLResponse=<base64_encoded_saml_assertion>&RelayState=<original_relay_state>

Response (success):
  302 → <original_destination_url>
  Set-Cookie: platform_session=<token>; HttpOnly; Secure; SameSite=Lax

Response (failure):
  302 → /login?error=sso_failed&reason=<human-readable-reason>
  // Reason must not leak internal details: "Authentication failed" only

  Internal reasons (logged, not shown to user):
    - signature_invalid
    - assertion_expired
    - replay_detected
    - audience_mismatch
    - issuer_mismatch
    - missing_required_attributes
    - jit_provisioning_failed
```

---

### GET /sso/oidc/callback

**Auth:** OIDC authorization code (self-contained in callback URL)  
**Rate Limit:** 100 requests/IP/minute

```
Query Parameters (from IdP redirect):
  code=<authorization_code>
  state=<original_state>
  // OR (error case):
  error=access_denied&error_description=...

Response (success): same as SAML ACS success
Response (failure): same pattern as SAML ACS failure
```

---

### POST /sso/saml/slo

**Auth:** SAML LogoutRequest signature  
**Rate Limit:** 20 requests/IP/minute

```
Request (IdP-initiated SLO via HTTP-POST binding):
  SAMLRequest=<LogoutRequest>&RelayState=<state>
  // OR LogoutResponse (SP receives response to its own SLO request)

Response (SP sends response back to IdP or redirects browser):
  302 → IdP SLO response URL with SAMLResponse
```

---

### POST /sso/oidc/backchannel-logout

**Auth:** Logout token (signed JWT from IdP)  
**Rate Limit:** 100 requests/IP/minute

```
Request:
  Content-Type: application/x-www-form-urlencoded
  logout_token=<signed_JWT>

  // Logout token contains:
  // { iss, aud, iat, jti, sub OR sid, events: {"http://schemas.openid.net/event/backchannel-logout":{}} }

Response 200 OK (success)
Response 400 Bad Request (invalid token)
```

---

### POST /sso/logout (User-Initiated)

**Auth:** Platform session (Bearer token or cookie)  
**Rate Limit:** 10 requests/user/minute

```
Request:
  { "all_devices": false }  // if true, terminate all SSO sessions for user

Response 200 OK:
  {
    "slo_status": "initiated",
    "redirect_url": "https://idp.enterprise-corp.com/logout?id_token_hint=...&post_logout_redirect_uri=..."
    // Client should navigate to redirect_url for front-channel OIDC logout
    // OR platform handles back-channel SLO automatically for SAML
  }
```

---

### POST /v1/admin/idps (Admin — Create IdP Configuration)

**Auth:** Bearer token (admin scope required: `admin:sso`)  
**Rate Limit:** 10 requests/user/hour

```
Request:
  {
    "display_name": "Acme Corp Okta",
    "protocol": "saml2",
    "saml_metadata_url": "https://acme.okta.com/app/abc123/sso/saml/metadata",
    // OR
    "saml_metadata_xml": "<EntityDescriptor>...</EntityDescriptor>",
    "attribute_mapping": {
      "email": "http://schemas.xmlsoap.org/ws/2005/05/identity/claims/emailaddress",
      "first_name": "firstName",
      "last_name": "lastName",
      "groups": "groups"
    },
    "jit_provisioning_enabled": true,
    "require_mfa_from_idp": false
  }

Response 201 Created:
  {
    "idp_id": "uuid",
    "sp_metadata_url": "https://platform.example.com/sso/saml/metadata",
    "acs_url": "https://platform.example.com/sso/saml/acs",
    "entity_id": "https://platform.example.com/sso/saml/metadata",
    "test_url": "https://platform.example.com/sso/test/{idp_id}"
  }
```

---

### POST /v1/admin/idps/{idp_id}/domains

**Auth:** Bearer token (admin scope)

```
Request:
  { "email_domain": "enterprise-corp.com" }

Response 201 Created:
  { "domain": "enterprise-corp.com", "idp_id": "uuid", "status": "active" }

Response 409 Conflict:
  { "error": "domain_already_claimed", "claimed_by_tenant": "other-tenant-uuid" }
  // Domain ownership: only one tenant can claim a given email domain
```

---

## 6. Deep Dive: Core Components

### 6.1 SAML 2.0 Assertion Validation

**Problem it solves:**  
A SAML assertion is an XML document signed by the IdP. The SSO Service must validate this assertion without being tricked by: (1) XML Signature Wrapping (XSW) attacks — where an attacker wraps a valid signature around malicious content; (2) Replay attacks — reusing an intercepted assertion after it has already been used; (3) Audience confusion — using an assertion intended for SP-A at SP-B; (4) Time-based attacks — using an expired or not-yet-valid assertion.

**XML Signature Wrapping (XSW) Attack — Most Critical Vulnerability:**

```
Normal signed XML structure:
  <Response>
    <Assertion ID="signed-id">       ← Signature covers this element
      <Subject><NameID>alice</NameID></Subject>
      <Signature>...</Signature>
    </Assertion>
  </Response>

XSW attack (wrapping):
  <Response>
    <Assertion ID="evil-id">          ← UNSIGNED wrapper; parser sees this first
      <Subject><NameID>admin</NameID></Subject>
    </Assertion>
    <hidden_element>
      <Assertion ID="signed-id">     ← Signature is valid for this element
        <Subject><NameID>alice</NameID></Subject>
        <Signature>...</Signature>
      </Assertion>
    </hidden_element>
  </Response>

Attack: XML parser resolves ID reference for signature validation to the signed element,
        but business logic reads the first Assertion element (unsigned, malicious).
```

**Validation Algorithm (secure SAML assertion processing):**

```
function ValidateSAMLAssertion(raw_saml_response_b64, expected_idp_id, request_id):
  
  // Step 1: Decode and parse XML
  xml_bytes = base64_decode(raw_saml_response_b64)
  
  // CRITICAL: Use a security-hardened XML parser
  // - Disable external entity (XXE) resolution: FEATURE_SECURE_PROCESSING=true
  // - Disable DTD processing
  // - Limit XML nesting depth (prevent billion laughs attack)
  doc = XML.parse(xml_bytes, 
    external_entities=False, 
    dtd=False,
    max_depth=50)
  
  // Step 2: Find and validate the signature BEFORE reading any assertion content
  // This is the key to preventing XSW attacks
  
  // Find all Signature elements in the document
  signatures = doc.findall("//ds:Signature")
  if not signatures: raise InvalidAssertion("No signature found")
  
  // Verify each signature using IdP's public key
  idp_certs = IDPConfigStore.get_active_certificates(expected_idp_id)
  signed_ids = set()
  for sig in signatures:
    referenced_id = sig.parent_element.get_attribute("ID")
    for cert in idp_certs:
      if xmldsig.verify(sig, cert):
        signed_ids.add(referenced_id)
        break
    else:
      raise InvalidSignature("Signature verification failed")
  
  // Step 3: Extract Assertion — ONLY from the signed element, not any wrapper
  // The Assertion we process MUST be the element whose ID is in signed_ids
  assertion = doc.find_by_id(one_of(signed_ids))
  if not assertion: raise InvalidAssertion("Signed element not found")
  
  // Verify the signed element IS the Assertion (not a Response wrapping an Assertion)
  if assertion.tag != "saml:Assertion":
    // Either the Response is signed (valid) and contains an Assertion
    // OR this is an XSW attempt
    // Rule: if Response is signed, the contained Assertion must be within the signed subtree
    assertion = assertion.find("saml:Assertion")
    if not assertion: raise InvalidAssertion("No Assertion in signed Response")
  
  // Step 4: Extract and validate assertion content
  
  // 4a. Issuer validation
  issuer = assertion.findtext("saml:Issuer")
  expected_entity_id = IDPConfigStore.get_entity_id(expected_idp_id)
  if issuer != expected_entity_id:
    raise InvalidIssuer(f"Expected {expected_entity_id}, got {issuer}")
  
  // 4b. Replay prevention
  assertion_id = assertion.get_attribute("ID")
  if Redis.EXISTS(f"saml:replay:{expected_idp_id}:{assertion_id}"):
    raise ReplayAttack(f"Assertion ID {assertion_id} already used")
  
  // 4c. Time conditions
  conditions = assertion.find("saml:Conditions")
  not_before = conditions.get("NotBefore")   // allow 2-minute clock skew
  not_on_or_after = conditions.get("NotOnOrAfter")
  if not_before and parse_datetime(not_before) > now() + timedelta(minutes=2):
    raise InvalidAssertion("Assertion not yet valid")
  if not_on_or_after and parse_datetime(not_on_or_after) < now() - timedelta(minutes=2):
    raise InvalidAssertion("Assertion expired")
  
  // 4d. Audience restriction
  audience_restriction = conditions.find("saml:AudienceRestriction/saml:Audience")
  sp_entity_id = config.SP_ENTITY_ID  // "https://platform.example.com/sso/saml/metadata"
  if audience_restriction.text != sp_entity_id:
    raise InvalidAudience(f"Not addressed to {sp_entity_id}")
  
  // 4e. In-response-to (for SP-initiated flows)
  if request_id:
    subject_conf = assertion.find("saml:Subject/saml:SubjectConfirmation")
    in_response_to = subject_conf.find("saml:SubjectConfirmationData").get("InResponseTo")
    stored_request = Redis.GET(f"saml:request:{in_response_to}")
    if not stored_request or in_response_to != request_id:
      raise InvalidAssertion("InResponseTo mismatch: possible injection attack")
  
  // Step 5: Store assertion ID to prevent replay
  ttl = max(0, (parse_datetime(not_on_or_after) - now()).total_seconds())
  Redis.SETEX(f"saml:replay:{expected_idp_id}:{assertion_id}", int(ttl) + 120, "1")
  
  // Step 6: Return validated attributes
  name_id = assertion.findtext("saml:Subject/saml:NameID")
  attributes = extract_attributes(assertion)  // map to platform schema
  
  return SAMLSubject(name_id=name_id, attributes=attributes)
```

**Interviewer Q&As:**

Q: What is XML Signature Wrapping and what is the definitive defense?  
A: XSW is an attack that exploits the difference between how the XML signature library validates signatures (by ID reference) and how the application extracts data (by document position/XPath). The attacker inserts a malicious unsigned element and preserves the valid signed element elsewhere in the document. The signature library validates the signed element correctly; the application code reads the malicious element. The definitive defense is: **after signature validation, extract assertion data only from the specific element that was signed, identified by its `ID` attribute.** Do not re-traverse the document by XPath or position to find the assertion. Implementations like python-saml and OneLogin use this approach. Never use `doc.find("//saml:Assertion")` — always use `doc.find_by_id(signed_element_id)`.

Q: How do you handle IdP certificate rotation without downtime?  
A: IdPs periodically rotate their signing certificates (typically 1-3 year cycles). The SSO service must accept assertions signed with either the old or new certificate during the rotation window. Implementation: store multiple active certificates per IdP (the `idp_signing_certificates` table supports multiple active certs). During validation: try each active certificate until one verifies successfully. Monitoring: set an alert when any IdP certificate's `valid_until` is within 60 days. Admin notification: alert the enterprise customer's IT admin when their IdP certificate is about to expire and provide instructions for updating the metadata. Automation: if the IdP published its metadata at a URL (`saml_metadata_url`), poll it daily and automatically import new certificates.

Q: What is the difference between SP-initiated and IdP-initiated SSO, and which is more secure?  
A: SP-initiated: the user starts at the Service Provider (platform), which generates an AuthnRequest, and the login flow is tied to that specific request. The assertion's `InResponseTo` field references the SP's AuthnRequest ID, which was stored in the SP's session/Redis. IdP-initiated: the user starts at the IdP (e.g., clicks the app tile in their Okta dashboard), and the IdP sends an assertion directly to the SP's ACS URL without a prior AuthnRequest. IdP-initiated flow is inherently less secure because: (1) There is no `InResponseTo` to validate — a man-in-the-middle or phisher can replay an assertion to the SP. (2) The SP has no state to verify the assertion is in response to a legitimate request. Some SPs disable IdP-initiated SSO entirely. If supporting IdP-initiated SSO: strictly validate all other assertion fields (audience, time, replay), add IP-based anomaly detection, and consider requiring a short-lived one-time token in the RelayState.

Q: How do you prevent the billion laughs / XML bomb attack in SAML parsing?  
A: XML bomb attacks use recursive entity expansion to cause exponential memory growth during parsing. SAML parsers must: (1) Disable DTD (Document Type Definition) processing entirely — DTDs are the mechanism for entity expansion. (2) Disable external entity resolution (XXE). (3) Set a maximum entity expansion limit. (4) Set a maximum document size (e.g., 1 MB — typical SAML assertions are 5-20 KB). All major XML parsing libraries have options for this: Java's SAXParserFactory.setFeature("http://xml.org/sax/features/external-general-entities", false); Python's defusedxml library handles all of these safely and should be used instead of the standard library's `xml.etree` for parsing untrusted XML.

Q: What clock skew tolerance should you allow in SAML assertion validation?  
A: The `NotBefore` and `NotOnOrAfter` conditions in SAML assertions assume synchronized clocks. In practice, IdP and SP clocks may drift by seconds to minutes. Industry standard is ±2 to ±5 minutes of tolerance. The SAML 2.0 specification says the conditions "SHOULD" be validated within the time constraints. Google's SAML SP allows ±5 minutes. The security trade-off: a larger tolerance window means an expired assertion can be replayed for longer. Combined with the replay cache (which catches re-use of the same assertion ID within the tolerance window), ±2 minutes is a reasonable balance. Enforce NTP on all SSO Service pods to keep local clock drift to < 1 second.

---

### 6.2 SAML vs OIDC: Protocol Selection and Trade-offs

**Problem it solves:**  
Enterprise customers use different IdP technologies. Okta and Azure AD support both SAML 2.0 and OIDC. Legacy systems (ADFS pre-2019) support only SAML or WS-Federation. The SSO Service must support both protocols, and IT admins need guidance on which to use for new integrations.

**Protocol Comparison:**

| Attribute | SAML 2.0 | OIDC / OAuth2 |
|---|---|---|
| Year introduced | 2005 | 2014 |
| Token format | Signed XML (verbose) | JWT (compact) |
| Transport | Browser redirect (HTTP redirect/POST bindings) | Browser redirect + back-channel HTTP |
| Signature algorithm | XMLDSig (RSA-SHA1/SHA256) | JOSE (RS256, ES256) |
| Key distribution | X.509 certificates in metadata XML | JWKS endpoint |
| Session information | SessionIndex in Assertion | `sid` claim in ID token |
| Logout | SAML SLO (complex, brittle) | OIDC back-channel logout (cleaner) |
| Mobile/SPA support | Poor (requires browser form POST) | Excellent (PKCE code flow) |
| Adoption among legacy enterprise | Dominant (required for ADFS, some legacy Okta configs) | Growing; preferred for new integrations |
| Complexity of implementation | High (XML parsing, XSW attack surface) | Medium (JWT-based, well-tested libraries) |
| Attribute format | XML SAML Attribute elements | JSON claims |
| Standard compliance libraries | OpenSAML (Java), python-saml, ruby-saml | Widely available in all languages |

**Decision Matrix for New Integrations:**

| Scenario | Recommended Protocol |
|---|---|
| Enterprise using Okta/Azure AD/Ping | OIDC (simpler, modern, better SLO) |
| Legacy ADFS (Windows Server < 2016) | SAML 2.0 (ADFS has limited OIDC support) |
| Mobile/SPA client | OIDC with PKCE |
| Requirement for rich attribute assertions | SAML (XML allows arbitrary attribute schema) |
| Strict backward compatibility requirement | SAML 2.0 |
| New greenfield enterprise integration | OIDC |

**OIDC SSO Flow Implementation:**

```
OIDC SP-Initiated Flow:

function InitiateOIDCSSO(idp_id, original_destination):
  config = IDPConfigStore.get_oidc_config(idp_id)
  
  // Generate PKCE and state
  code_verifier = base64url(CSPRNG(32 bytes))
  code_challenge = base64url(SHA256(code_verifier))
  state = base64url(CSPRNG(16 bytes))
  nonce = base64url(CSPRNG(16 bytes))
  
  // Store OIDC state (TTL 10 min)
  Redis.SETEX(f"sso:oidc:{state}", 600, JSON.encode({
    idp_id, code_verifier, nonce, original_destination
  }))
  
  // Build authorization URL
  params = {
    response_type: "code",
    client_id: config.oidc_client_id,
    redirect_uri: "https://platform.example.com/sso/oidc/callback",
    scope: "openid profile email " + config.additional_scopes,
    state: state,
    nonce: nonce,
    code_challenge: code_challenge,
    code_challenge_method: "S256"
  }
  return redirect(config.authorization_endpoint + "?" + url_encode(params))

function HandleOIDCCallback(code, state):
  // Retrieve stored OIDC state
  stored = Redis.GETDEL(f"sso:oidc:{state}")
  if not stored: raise InvalidState("state parameter mismatch or expired")
  
  config = IDPConfigStore.get_oidc_config(stored.idp_id)
  
  // Exchange code for tokens (back-channel)
  token_response = HTTP.POST(config.token_endpoint, {
    grant_type: "authorization_code",
    code: code,
    redirect_uri: "https://platform.example.com/sso/oidc/callback",
    client_id: config.oidc_client_id,
    client_secret: decrypt(config.oidc_client_secret_enc),
    code_verifier: stored.code_verifier
  })
  
  // Validate ID token
  id_token_claims = ValidateIDToken(token_response.id_token, config, stored.nonce)
  
  // Extract user attributes
  // For additional attributes: call UserInfo endpoint if needed
  userinfo = HTTP.GET(config.userinfo_endpoint,
    headers={"Authorization": f"Bearer {token_response.access_token}"})
  
  attributes = merge(id_token_claims, userinfo)
  
  // Continue with JIT provisioning + platform session creation
  return ProcessSSOLogin(stored.idp_id, id_token_claims.sub, attributes, token_response.id_token)

function ValidateIDToken(id_token, config, expected_nonce):
  // Fetch JWKS from IdP (cached)
  jwks = IDPConfigStore.get_oidc_jwks(config.idp_id)  // Redis cache, 1h TTL
  
  // Validate JWT
  header = jwt.decode_header(id_token)
  key = jwks.find_by_kid(header.kid)
  claims = jwt.verify(id_token, key, algorithms=["RS256", "ES256"])
  
  // Standard OIDC ID token validations
  assert claims.iss == config.oidc_issuer
  assert config.oidc_client_id in claims.aud
  assert claims.exp > now()
  assert claims.iat >= now() - timedelta(minutes=5)  // clock skew tolerance
  assert claims.nonce == expected_nonce  // prevent replay
  
  return claims
```

**Interviewer Q&As:**

Q: How does OIDC back-channel logout work and how does it differ from SAML SLO?  
A: OIDC back-channel logout (RFC 9099): the IdP sends a signed HTTP POST to the SP's registered `backchannel_logout_uri` with a "logout token" (a JWT containing `sub` or `sid`, the event type, and standard JWT claims). The SP validates the logout token, finds the matching platform session, and terminates it. No browser involvement required. SAML SLO (Single Logout): the IdP sends a `LogoutRequest` XML message via the browser (HTTP-Redirect or HTTP-POST binding) to the SP's SLO endpoint. The SP terminates the session and sends a `LogoutResponse` back via the browser. OIDC back-channel logout is more reliable (works even if user closed the browser tab) and simpler to implement (JWT validation vs XML). SAML SLO is notoriously fragile: it requires browser mediation, iframes in some implementations, and often fails if the browser blocks third-party cookies.

Q: What is the `nonce` claim in OIDC and why is it critical for SSO security?  
A: The `nonce` is a cryptographically random value included in the OIDC authorization request and echoed back in the ID token. Its purpose: prevent replay attacks on the ID token. Without a nonce: an attacker who captures an ID token (e.g., from a logging system or via a malicious relying party) could replay it to a different session. With the nonce: the SP stores the nonce in the session before redirecting to the IdP; on callback, the SP verifies the nonce in the ID token matches the stored nonce. Since the nonce is bound to a specific session (stored in the SP's session store), a replayed ID token with the same nonce will not match any active session's nonce.

Q: How do you handle the case where a user's email changes at the IdP?  
A: The `external_subject` (SAML NameID or OIDC `sub`) is the stable, persistent identifier — not the email. If a user's email changes but their `sub` stays the same (as it should in modern IdPs), the SSO Service looks up the federation record by `(idp_id, external_subject)`, finds the linked platform account, and updates the email. If the IdP reuses NameIDs (common with email-format NameIDs in legacy SAML configurations): the SSO Service cannot distinguish "email changed for existing user" from "new user with recycled email." Mitigation: (1) Prefer persistent (opaque, non-email-based) NameID format. (2) Use `urn:oasis:names:tc:SAML:2.0:nameid-format:persistent` in the NameIDPolicy. (3) If only email-format NameIDs are available, implement a grace period and require admin confirmation for account linking when an email-format NameID appears on a new account.

Q: How does SSO work for mobile apps that cannot use browser redirects?  
A: For mobile apps, the recommended approach is OIDC with PKCE using the system browser (not an embedded WebView): (1) App calls `ASWebAuthenticationSession` (iOS) or `Custom Tabs` (Android) — a system-level browser that shares cookies with the main browser but is sandboxed. (2) OIDC flow proceeds normally with PKCE. (3) App registers a custom URI scheme (`com.example.app://callback`) as the redirect URI. (4) After IdP authentication, the system browser calls back the app via the custom scheme with the authorization code. (5) App exchanges code for tokens using PKCE in a back-channel request. Never use an embedded WebView (WKWebView, Android WebView) for SSO — the IdP cannot detect whether it's a legitimate browser or a malicious app intercepting credentials. RFC 8252 (OAuth for Native Apps) documents this pattern.

Q: When would you recommend a customer use SAML instead of OIDC for their SSO integration?  
A: Recommend SAML when: (1) The IdP is ADFS (Active Directory Federation Services) on Windows Server 2012/2016 — ADFS has poor OIDC support pre-2019 versions. (2) The enterprise has an existing, working SAML configuration they don't want to migrate. (3) The enterprise requires rich attribute assertions with custom XML schemas that OIDC claims cannot easily represent. (4) Compliance requirements mandate SAML (rare, but some healthcare/government contracts specify SAML 2.0). Recommend OIDC when: (1) Setting up a new integration from scratch. (2) The IdP is modern (Okta, Azure AD, Ping, Google Workspace — all support OIDC well). (3) Mobile or SPA applications are involved. (4) Simpler ongoing maintenance is a priority (OIDC certificate rotation is automated via JWKS; SAML requires manual certificate exchange).

---

### 6.3 Identity Provider Discovery and Session Federation

**Problem it solves:**  
When a user arrives at the platform login page, the SSO Service must determine (1) which IdP to route them to, and (2) whether they already have an active federated session that can be reused without requiring them to re-authenticate at the IdP.

**Discovery Approaches Compared:**

| Method | UX | Accuracy | Complexity |
|---|---|---|---|
| Manual IdP selection (dropdown) | Poor (user must know IdP) | High | Low |
| Email domain lookup | Good | High | Low |
| Tenant-scoped URL (`/login/acme-corp`) | Excellent | Exact | Low |
| Identifier-First login | Excellent | High | Medium |
| **Hybrid: tenant URL + email domain fallback** | **Excellent** | **High** | **Low** |

**Selected: Tenant URL as primary, email domain as secondary, identifier-first as UX pattern**

Implementation:

```
IdP Discovery Algorithm:

function DiscoverIdP(request):
  // Priority 1: Tenant-scoped URL
  // e.g., https://app.platform.example.com/login/acme-corp
  if tenant_slug = extract_tenant_from_url(request.path):
    idp = IDPConfigStore.get_primary_idp_for_tenant(tenant_slug)
    if idp: return idp
  
  // Priority 2: Email domain from identifier-first input
  // User has entered their email in the "email" field
  if email = request.params.get("email"):
    domain = email.split("@")[1].lower()
    
    // Cache: Redis GET "sso:domain:{domain}"
    cached_idp_id = Redis.GET(f"sso:domain:{domain}")
    if cached_idp_id:
      return IDPConfigStore.get(cached_idp_id)
    
    // DB lookup: exact domain match
    mapping = DB.SELECT FROM email_domain_idp_mappings 
      WHERE email_domain=domain 
      ORDER BY priority DESC
      LIMIT 1
    
    if mapping:
      // Warm cache
      Redis.SETEX(f"sso:domain:{domain}", 3600, mapping.idp_id)
      return IDPConfigStore.get(mapping.idp_id)
  
  // Priority 3: No IdP found → show login form (email/password)
  return None

// Note: Wildcard domain matching (*.enterprise-corp.com → enterprise-corp.com IdP)
// is implemented at DB level with a separate "domain_suffix" field and checked if 
// exact match fails.
```

**Session Federation (avoiding unnecessary re-authentication):**

```
Problem: User logs into Platform App A via SSO. They navigate to Platform App B.
         Should they have to authenticate at the IdP again?

Solution: Platform session federation using a shared platform session.

Implementation:
  1. All Platform services share the same session via the Auth Service.
  2. The platform access token (JWT) from App A is valid for App B (same issuer, 
     same audience = the platform).
  3. SSO session is stored in `sso_sessions` table. Any platform service can check:
     "Is there an active SSO session for this user?" and use it without redirecting to IdP.

  function CheckExistingSession(user_id, required_amr):
    // Check if user has a valid platform session
    session = AuthService.ValidateSession(request.access_token)
    if session.valid:
      if required_amr and not session.amr.satisfies(required_amr):
        // Step-up auth required (e.g., endpoint needs MFA but session only has password)
        return require_step_up(session, required_amr)
      return session  // re-use existing session

    // No valid session → initiate SSO
    idp = DiscoverIdP(request)
    if idp:
      return InitiateSSOFlow(idp, original_destination=request.path)
    else:
      return ShowLoginForm()

  // Prompt=none for OIDC (silent re-auth):
  // If the IdP supports it, attempt a prompt=none OIDC request to check if 
  // the user has an active IdP session. If yes, get tokens without user interaction.
  // If no: show the login prompt.
  // This enables "seamless SSO" without requiring a new user interaction.
```

**Interviewer Q&As:**

Q: How do you handle IdP discovery when a user's company has multiple email domains mapping to the same IdP?  
A: Store multiple rows in `email_domain_idp_mappings` for the same `idp_id`: `acme.com → idp_X` and `acme-corp.com → idp_X`. The discovery lookup finds the correct IdP for both domains. The admin can register all their email domains in the SSO configuration UI. Additional concern: domain squatting — an attacker registers `acme-corp.net` and tries to claim it for a malicious IdP. Mitigation: domain ownership verification at registration time (DNS TXT record challenge: `platform-verify=<token>`, similar to SSL certificate domain validation).

Q: What is "home realm discovery" and how is it different from IdP discovery?  
A: Home realm discovery is the more general term; it refers to determining which IdP (or "realm") a user belongs to. IdP discovery based on email domain is the most common implementation of home realm discovery. In WS-Federation, the "whr" (Wantsrealmhint) parameter explicitly specifies the realm. In SAML, the `IDPHint` parameter is sometimes used. In OIDC, the `login_hint` parameter (containing the email) helps the IdP skip the user discovery step on their end. The platform's discovery mechanism is a superset: it must work even when the user has not entered their email yet (using URL-based tenant routing as the first step).

Q: How do you handle SP-to-SP SSO (both apps on the same platform use the same IdP)?  
A: Since both apps share the same platform Auth Service and the same session cookies (scoped to `platform.example.com`), an authenticated session at App A is valid at App B automatically (same first-party cookies, same JWT issuer). The user does not need to re-authenticate. The SSO flow is only triggered when there is no valid platform session. This is sometimes called "transparent SSO" or "platform-level SSO" — the IdP integration is only needed for the first login, after which the platform's own session system handles cross-app navigation.

Q: What happens when the IdP is down and a user tries to log in via SSO?  
A: The IdP is an external dependency outside the platform's control. Behaviors: (1) SAML SP-initiated: the user is redirected to the IdP's SSO URL; if the IdP is down, they see an IdP error page (nothing the platform can do). (2) OIDC: the authorization request redirects to the IdP; same outcome. Mitigations: (1) Detect IdP unavailability proactively: ping the IdP's health/metadata URL every 30 seconds; if 3 consecutive failures, switch to "IdP degraded" mode and show a warning to users: "Your company's single sign-on is experiencing issues. Contact your IT administrator." (2) Emergency access: platform admins can temporarily enable email/password login for SSO users (only if the enterprise customer has this option enabled and a platform password was provisioned). (3) Never cache SAML assertions as a "bypass" — this would create a security vulnerability.

Q: How do you implement forced re-authentication (step-up) via SSO?  
A: Some operations (e.g., deleting an account, accessing financial data) should require fresh authentication even if the user has an active session. For OIDC: redirect to IdP with `max_age=0` (require fresh authentication regardless of IdP session) and `prompt=login` (force re-authentication UI even if IdP session is valid). For SAML: send AuthnRequest with `ForceAuthn="true"`. The IdP will require the user to re-authenticate. After fresh IdP authentication, the new assertion/ID token will have a recent `auth_time` value. The platform verifies `auth_time` is within the last N seconds (e.g., 5 minutes) before granting access to the sensitive operation.

---

## 7. Scaling

### Horizontal Scaling

**SSO Service (stateless pods):** Low RPS (~35 RPS at peak). 2-3 pods with auto-scaling suffice. CPU-intensive operations are XML parsing and signature verification (~5 ms/request). A single 4-core pod handles ~200 SAML validations/second — far more than needed. Scale for availability (N+1 redundancy), not capacity.

**IdP Config Cache (Redis):** 5,000 IdP configs × 10 KB = 50 MB — entire IdP config fits in a single Redis instance's memory. Redis GET per login = 35 RPS — trivial. Use Redis Cluster for fault tolerance, not capacity.

**Replay Cache (Redis):** Short-lived entries (5-minute TTL). 35 RPS × 300s = 10,500 active entries × 64 bytes = ~672 KB. Completely trivial. Redis handles this with a single instance.

**Session Store (PostgreSQL):** 600K concurrent SSO sessions × 512 bytes = 307 MB. PostgreSQL comfortably handles this. Partition `sso_sessions` by month for efficient cleanup.

**SLO Propagation (async workers):** SLO calls to IdP back-channel endpoints are async. Each SLO generates 1-2 HTTP calls to external IdP endpoints. At 21 RPS peak SLO events × average 500ms per external call = ~10 concurrent HTTP connections. Managed by a small async worker pool.

### Caching

| Data | Cache Key | TTL | Invalidation |
|---|---|---|---|
| IdP config | `sso:idp:{idp_id}` | 1 hour | Pub/sub on config update |
| Email domain → IdP mapping | `sso:domain:{domain}` | 1 hour | Pub/sub on mapping update |
| OIDC JWKS (IdP's keys) | `sso:oidc:jwks:{idp_id}` | 1 hour | Background refresh |
| SAML assertion replay | `saml:replay:{idp_id}:{assertion_id}` | NotOnOrAfter TTL | Natural expiry |
| OIDC state | `sso:oidc:{state}` | 10 minutes | Natural expiry or GETDEL |
| SAML request ID | `saml:request:{request_id}` | 10 minutes | Natural expiry |

### Multi-Region

**SSO Service:** Deploy in each region. IdP config and federation data in PostgreSQL with async replication (1-5s lag). For login events: the 10-minute TTL for SAML/OIDC state means a user who starts a flow in region A must complete it in region A (the state is in that region's Redis). Solution: include region in RelayState; API gateway routes callbacks to the correct region based on RelayState.

**SLO propagation cross-region:** Publish SLO events to a global message bus; regional workers consume and process. Ensure platform sessions are terminated in all regions within 500ms.

### Interviewer Q&As on Scaling

Q: The SAML protocol requires browser-mediated redirects. How does this interact with load balancing?  
A: SAML SP-initiated SSO: (1) User's browser hits any SSO pod (no affinity needed). (2) The pod stores the RequestID in Redis (shared across pods). (3) The SAML callback (ACS POST) can hit any pod; it fetches the RequestID from Redis to validate. Redis is the shared state store — no sticky sessions needed. For OIDC: same pattern with the `state` stored in Redis. The only concern is if Redis becomes unavailable during the flow window — the user gets an error and must restart the login flow (a minor UX issue for a brief infrastructure incident).

Q: How do you test SSO integration without a production IdP?  
A: (1) Use a local SAML IdP for development: SimpleSAMLphp (PHP) or samltest.id (public test IdP). (2) Use a test OIDC IdP: Keycloak (self-hosted) or Dex (lightweight, Go-based). (3) Create test users in the test IdP matching production email domain patterns. (4) Implement an "SSO Test" endpoint: `GET /sso/test/{idp_id}` that initiates the SSO flow and logs all assertion validation steps, returning a diagnostic report. This is exposed only in non-production environments and to admins in production. (5) Capture and replay production SAML assertions in a test environment (after sanitizing PII) to reproduce assertion parsing issues.

Q: How would you scale this system to 1,000 enterprise customers each with 10,000 employees (10M SSO users)?  
A: Current 1M users → 10M users = 10× scale. Peak SSO login RPS: 34.7 × 10 = 347 RPS. Still low; 10-20 SSO pods handle this easily. The real scale challenge: (1) IdP configurations: 1,000 → 10,000 configs; still 100 MB in Redis, trivial. (2) Federation records: 10M rows in PostgreSQL; 2.56 GB; partition by `idp_id`; index on `(idp_id, external_subject)` keeps lookups O(log n). (3) SSO sessions: 6M concurrent; 3 GB; PostgreSQL handles with proper partitioning. (4) SAML assertion XML parsing: CPU-intensive at higher scale; scale SSO pods horizontally. The SSO service architecture scales linearly without fundamental design changes up to this level.

Q: What is the performance impact of XML digital signature verification vs JWT signature verification?  
A: SAML uses XML Digital Signatures (XMLDSig): RSA-SHA256 verification over an XML-canonicalized document. The canonicalization step (XML-C14N) is CPU-intensive: ~3-5 ms for typical SAML assertions on modern hardware. Total SAML validation: ~5-10 ms. OIDC uses JWT with JSON Web Signatures: RS256 verification = RSA-2048 signature verify over a SHA-256 hash of the header.payload string. This takes ~0.5-1 ms. OIDC token validation is ~10× faster than SAML assertion validation. At 347 peak RPS: SAML pods need: 347 × 10 ms = 3.47 CPU-seconds/second = ~4 CPU cores dedicated to SAML parsing. OIDC pods: 347 × 1 ms = 0.347 CPU-seconds/second = < 1 CPU core. Not a bottleneck at this scale; relevant at 10,000+ RPS.

Q: How do you handle SLO for a user who is logged into 10 different SPs (applications) simultaneously?  
A: The platform is a single SP — SLO at the IdP level revokes the user's IdP session and notifies all registered SPs. The platform receives one SLO notification (back-channel POST) from the IdP and internally revokes all platform sessions for that user (across all platform apps). The internal session revocation is handled by the Auth Service: revoke all refresh token families for the user (see `auth_service.md`). For external SPs registered with the same IdP: those SPs receive their own SLO notifications directly from the IdP. The platform is not responsible for propagating logout to other SPs — that is the IdP's responsibility. The platform only needs to handle SLO for its own sessions.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation |
|---|---|---|---|
| IdP unavailable | New SSO logins fail; existing platform sessions unaffected | IdP health check ping (30s interval) | Show "SSO degraded" warning; optional fallback to email/password if configured |
| SSO Service pod crash | In-flight logins fail (user sees error page) | Kubernetes pod health check | Pod auto-restart; user retries flow (SAML/OIDC is stateless per flow, just retry) |
| Redis (state store) down | SAML/OIDC flow state lost; in-flight logins fail | Health check | Circuit breaker: SSO new logins return 503; existing platform sessions unaffected (JWT validation doesn't use SSO Redis) |
| PostgreSQL down | JIT provisioning fails; federation record lookup fails | Health check | Patroni failover (< 30s); brief SSO outage for new logins; existing sessions valid via JWT |
| SAML certificate expired (IdP's cert) | SAML signature validation fails; all SSO logins fail | Certificate expiry monitoring (alert 60 days before) | Contact IdP admin to renew; support multiple active certs during transition |
| XML signature wrapping attack | Attacker logs in as another user | Signature validation catches it; audit alert on failed assertions | Defense-in-depth: (1) Strict XSW validation, (2) Anomaly detection on assertion processing errors, (3) Alert on any signature failure spike |
| Replay attack | Reuse of captured SAML assertion | Replay cache detects duplicate assertion ID | Redis TTL handles; backup: PostgreSQL assertion ID table |
| JIT provisioning race (concurrent logins) | Duplicate user accounts created | `UNIQUE(idp_id, external_subject)` constraint | PostgreSQL ON CONFLICT handles atomically; one insert succeeds, other is treated as update |
| SLO back-channel call failure | IdP session persists after platform logout | SLO delivery monitoring | Retry with exponential backoff (3 retries); accept eventual consistency for SLO; log failures for manual remediation |

### Idempotency

- **SAML assertion processing**: The replay cache (Redis SETEX) ensures assertion IDs are only processed once. If the ACS POST is retried by the browser (rare), the second processing is rejected as a replay. The user must restart the flow.
- **JIT Provisioning**: `INSERT ... ON CONFLICT DO UPDATE` — idempotent upsert.
- **SSO Session creation**: Idempotent by `(platform_user_id, idp_id, idp_session_index)` — duplicate creation returns existing session.
- **SLO processing**: Idempotent — terminating an already-terminated session is a no-op.

### Circuit Breaker

- Wrap all external IdP HTTP calls (token exchange for OIDC, SLO back-channel).
- If an IdP's token endpoint fails 5 times in 30 seconds → circuit open for that IdP for 60 seconds → return SSO_UNAVAILABLE.
- Auth Service circuit: if Auth Service is unavailable, SSO cannot create platform sessions → fail with 503 (do not create sessions outside Auth Service).

---

## 9. Monitoring & Observability

### Metrics

| Metric | Alert |
|---|---|
| `sso_logins_total{protocol, idp_id, result}` | Failure rate > 10% per IdP → alert |
| `sso_login_duration_seconds` p99 | > 3s → warn |
| `saml_signature_failures_total{idp_id}` | Any > 0 → alert (potential attack) |
| `saml_replay_detections_total` | Any > 0 → alert |
| `sso_jit_provisions_total{action="create|update"}` | Spike in creates → check for bulk provisioning |
| `sso_logout_total{result}` | SLO failure rate > 20% → alert |
| `sso_idp_certificate_expiry_days` | < 60 days → warn; < 14 days → alert |
| `sso_discovery_failures_total` | > 1% of logins → alert |
| `sso_session_active_count` | Sudden drop (> 50%) → alert |
| `sso_oidc_jwks_refresh_failures_total` | Any → warn |

### Distributed Tracing

- End-to-end SSO flow spans: `idp_discovery` → `authn_request_generate` → [IdP round-trip, measured as gap] → `assertion_validate` → `attribute_map` → `jit_provision` → `session_create`.
- Mark the "IdP round-trip" span with `peer.service=<idp_name>` for per-IdP latency dashboards.
- SLO propagation spans: `slo_receive` → `session_terminate` → `slo_back_channel_send`.

### Logging

- Never log SAML assertions, OIDC tokens, or raw IdP credentials in logs.
- Log: event type, idp_id, external_subject (masked: first 8 chars), attribute_mapping result, JIT action taken, session_id (not session token), duration_ms.
- Security events (signature failures, replay detections) logged at WARN level with full sanitized context.
- All authentication events include `trace_id` for cross-service correlation.

---

## 10. Trade-offs & Design Decisions Summary

| Decision | Chosen | Alternative | Rationale |
|---|---|---|---|
| SAML assertion validation approach | Validate signature first, then extract data from signed element | Parse first, then validate | Signing-first is the only XSW-safe approach |
| SAML assertion ID storage | Redis (primary) + PostgreSQL (backup) | Redis only | Redis for performance; PG for durability across Redis restarts |
| IdP discovery | Email domain + tenant URL | Manual IdP selection | Email domain enables seamless UX; tenant URL is fastest path |
| OIDC IdP client secret storage | AES-256-GCM encrypted (KMS key) | Plaintext in DB | Encrypted: DB breach doesn't expose client secrets |
| JIT provisioning | Synchronous in login flow | Async background | Sync: user has account immediately on first login; async: faster login but user may hit errors if account not ready |
| SLO implementation | Back-channel (OIDC) + HTTP-POST binding (SAML) | Front-channel only (iframes) | Back-channel is more reliable (works without browser); front-channel only fails if browser tab is closed |
| Session federation | Shared platform session (JWT) | Per-SP sessions | Shared session: seamless cross-app UX; per-SP: more isolation but worse UX |
| Clock skew tolerance | ±2 minutes | ±5 minutes | ±2: tighter security window; combined with replay cache prevents reuse even within tolerance |
| Protocol preference | OIDC for new integrations | SAML as default | OIDC: modern, better tooling, easier certificate rotation; SAML: legacy compat |
| Domain ownership verification | DNS TXT record challenge | None | Prevents domain squatting attacks on IdP discovery |

---

## 11. Follow-up Interview Questions

**Q1: What is the difference between an IdP, SP, and RP in SSO contexts?**  
A: Identity Provider (IdP): the service that authenticates the user and asserts their identity (e.g., Okta, Azure AD, Google Workspace). Service Provider (SP): in SAML terminology, the application that the user wants to access; it trusts the IdP and consumes SAML assertions (e.g., the platform). Relying Party (RP): the equivalent concept in OIDC — an application that relies on the OIDC Provider (OP) for identity. OP (OpenID Provider): the OIDC equivalent of an IdP — issues ID tokens (e.g., Google's OIDC endpoint). The platform SSO Service acts as an SP (for SAML) and RP (for OIDC) when integrating with enterprise IdPs. When the platform acts as an identity source for its own downstream microservices, it acts as an OP/IdP.

**Q2: How does Azure AD / Entra ID handle multi-tenant SSO and what does that mean for your platform?**  
A: Azure AD supports multi-tenant app registrations where a single app registration can accept sign-ins from any Azure AD tenant. In this case: (1) The `iss` (issuer) claim in the ID token is `https://sts.windows.net/{tenant_id}/` — the tenant ID is part of the issuer. (2) Your platform must validate that the tenant_id in the issuer is the expected tenant for the platform customer. (3) For SAML: Azure AD's Entity ID includes the tenant ID. The platform must store the expected tenant ID per enterprise customer configuration and validate it in every assertion. Failure to do this creates a cross-tenant authentication vulnerability: a user from Tenant A could authenticate to a platform account configured for Tenant B.

**Q3: What is SCIM and how does it complement SSO?**  
A: SCIM (System for Cross-domain Identity Management, RFC 7642-7644) is a standard protocol for user provisioning and deprovisioning between an IdP and SPs. While SSO handles authentication (login), SCIM handles identity lifecycle management: (1) Provisioning: when a new employee is added to the company's IdP, SCIM automatically creates their platform account (without requiring them to log in first). (2) Updates: name, role, or group changes in the IdP are immediately reflected in the platform. (3) Deprovisioning: when an employee leaves, SCIM triggers account deactivation in the platform within seconds. Compared to JIT provisioning: SCIM is proactive (accounts exist before first login); JIT is reactive (account created at first login). SCIM is preferred for large enterprises where IT needs to manage accounts centrally before employees start using the platform.

**Q4: How do you handle group/role mapping from IdP attributes to platform permissions?**  
A: The IdP sends group membership as SAML attributes (e.g., `memberOf: ["Engineering", "Platform-Admins"]`) or OIDC claims (e.g., `groups: ["engineering", "platform_admins"]`). The SSO Service's attribute mapper translates these to platform roles using admin-configured rules: `"Platform-Admins" → platform_role="admin"`. JIT provisioning applies these roles to the user account. If `jit_group_sync=true`, roles are updated on every SSO login (not just first login) so role changes in the IdP are automatically reflected in the platform. Security consideration: never map IdP groups to platform `super_admin` or billing roles without additional confirmation — an IdP compromise could grant escalated privileges. Require explicit admin approval for sensitive role mappings.

**Q5: What is attribute-based access control (ABAC) in the SSO context?**  
A: ABAC is an access control model where access decisions are based on attributes (properties) of the user, resource, and environment, rather than static role assignments. In SSO, the IdP provides rich attribute sets: department, clearance level, location, employment status. The platform can use these attributes in authorization policies: "Allow access to financial dashboard only if `department=Finance` AND `clearance=confidential`." Implementation: attributes from the SSO assertion are included in the platform's JWT claims (or accessible via the UserInfo endpoint). The authorization layer evaluates policies against these claims. ABAC via SSO attributes enables dynamic, policy-driven access control that stays synchronized with the enterprise's HR system through the IdP.

**Q6: How would you design SSO for a microservices architecture where each service needs to verify the user's identity?**  
A: The SSO flow creates a platform-level session managed by the Auth Service. After SSO completes, the Auth Service issues a JWT access token. This JWT is passed in `Authorization: Bearer <token>` headers to all microservices. Each microservice validates the JWT locally (using the platform's public key from JWKS — see `auth_service.md`). The SSO context (which IdP was used, `amr`, `auth_time`) is embedded as claims in the JWT. Microservices do not need to know about SAML or OIDC — they only deal with the platform's own JWT format. The SSO Service and Auth Service are a boundary between external identity protocols and the internal service mesh.

**Q7: What is "federation metadata" and why is it important for operational maintenance?**  
A: SAML federation metadata is an XML document that describes an entity's SSO configuration: its Entity ID, signing certificates, SSO/SLO endpoint URLs, and NameID formats. Both IdPs and SPs publish metadata. Importance: (1) Automated trust establishment: rather than manually copying certificate PEM files, both parties exchange metadata URLs, and the SSO software automatically imports the trust configuration. (2) Certificate rotation: when an IdP updates their signing certificate, they publish the new certificate in their metadata. The SP's metadata consumer detects the change and imports the new certificate without manual intervention. (3) Configuration validation: metadata is signed by the entity that published it, preventing tampered configurations. The platform should poll IdP metadata URLs daily and alert on unexpected changes (certificate replacement without prior notice could indicate IdP compromise).

**Q8: How do you handle the scenario where a user has accounts at two different companies that both use your platform, each with their own IdP?**  
A: The `identity_federations` table links `(idp_id, external_subject)` to a `platform_user_id`. A single platform user can have multiple identity federation records: one for their personal email (direct login) and one or more for enterprise IdPs. The user selects their login method: "Sign in with email" or "Sign in with [Company] SSO." Each IdP provides a separate `external_subject` (different `sub` values across IdPs). If the same person is a customer of two enterprise companies using different IdPs: they would have two separate platform accounts (one per company) unless the platform supports explicit account linking. Account linking: verify ownership of both accounts (authenticate with each in the same session) before linking their identity federation records.

**Q9: What is WS-Federation and do you need to support it?**  
A: WS-Federation is an older identity federation protocol based on SOAP/WS-Security, primarily used by ADFS (Active Directory Federation Services) and older .NET applications. It predates SAML 2.0's browser-based SSO profiles and OIDC. Support requirement: if your enterprise customers have legacy ADFS deployments that have not been migrated to SAML 2.0 or OIDC, WS-Federation may be required. However, as of 2024, virtually all enterprises that can support ADFS can also configure SAML 2.0 via ADFS. Recommendation: support SAML 2.0 and OIDC as the primary protocols; offer WS-Federation read-only support (accept WS-Fed passive sign-in tokens, translate to platform sessions) only if a specific enterprise customer requires it. Do not invest in WS-Federation issuing capability — the protocol is effectively deprecated.

**Q10: How do you implement "Just-In-Time" deprovisioning (automatic account disabling when user leaves the company)?**  
A: Pure JIT deprovisioning: not possible. JIT provisioning is triggered by the user's login event; if the user never logs in again after leaving, there is no trigger. Solutions: (1) SCIM deprovisioning: the IdP sends a SCIM DELETE request when the user is removed from the IdP directory. Immediate and proactive. (2) Session TTL: if the user's session expires, they must re-authenticate via SSO. If their IdP account is disabled, the SSO authentication fails → the platform session expires → the user is effectively locked out within the session TTL duration (e.g., 1 day). (3) IdP-initiated SLO: when the user is deleted from the IdP, the IdP sends a SLO notification to all registered SPs. The platform receives the SLO, terminates the platform session, and can optionally mark the account as `suspended_pending_review`. SCIM + SLO together provide near-real-time deprovisioning.

**Q11: How do you validate that an incoming SAML assertion is not a phishing-grade forgery using a valid but stolen certificate?**  
A: The trust chain in SAML works as follows: the platform trusts a specific IdP's signing certificate (registered in `idp_signing_certificates`). An attacker who steals the IdP's signing certificate could forge arbitrary assertions. Mitigations: (1) The IdP signing certificate is stored and managed by the IdP (Okta, Azure AD) on HSMs — extremely difficult to steal. (2) If a certificate leak is suspected: the enterprise admin updates the IdP configuration (rotates to a new certificate); the platform immediately trusts only the new certificate. (3) Network-level: SAML assertions arrive via HTTPS browser POST from the user's browser (not directly from the IdP server). Even with a stolen certificate, the attacker would need to construct a malicious page that posts to the platform's ACS URL — this is possible but requires exploiting the user's browser. PKCE-equivalent protections: the `InResponseTo` binding (assertion must reference a valid pending request) prevents arbitrary injection.

**Q12: What are the compliance implications of storing IdP-provided user attributes?**  
A: GDPR/CCPA considerations: (1) User consent: the enterprise (data controller) has already obtained consent from employees for using the IdP and SSO. The platform (data processor) processes attributes under the enterprise's data processing agreement (DPA). (2) Data minimization: only store attributes necessary for the platform's function. Do not store all SAML attributes if only email and name are needed. (3) Right to erasure: when an enterprise customer offboards, delete all identity federation records and JIT-provisioned user data for that tenant. (4) Data residency: some enterprises require user data (including SSO attributes) to remain in specific geographic regions. Design the IdP config store to support per-tenant data residency (deploy in the appropriate region, use region-specific PostgreSQL instance). (5) Audit log: store who accessed/processed the attributes and for what purpose — required for GDPR accountability principle.

**Q13: How do you handle SSO for CLI tools and developer tools that don't have a browser?**  
A: Options in order of preference: (1) Device Authorization Flow (OIDC, RFC 8628): the CLI prompts the user to visit a URL and enter a short code (`XXXX-YYYY`); the user authenticates in their browser (full SSO flow); the CLI polls until approved. This is the same flow as casting to a Chromecast. (2) Personal Access Tokens: the user creates a PAT in the developer dashboard (authenticated via full SSO) and provides it to the CLI. The PAT is a long-lived credential for the CLI. (3) OS-level SSO integration: on macOS/Windows with Azure AD or Okta integrated at the OS level, the CLI can use the system's SSO credential provider (similar to `aws sso login` in AWS CLI). For security: option 1 is preferred (modern, time-limited tokens); option 2 is simplest (PAT management). Never prompt for username/password in CLI tools for SSO-only enterprises — this bypasses the IdP MFA entirely.

**Q14: What is the security risk of "IdP-initiated" SSO compared to "SP-initiated"?**  
A: IdP-initiated: the user clicks an app tile in their IdP portal; the IdP sends a SAML assertion directly to the SP's ACS URL without a prior AuthnRequest from the SP. Security risks: (1) No `InResponseTo` binding: there is no pending request ID to validate, so the assertion cannot be cryptographically tied to a specific session. (2) Replay attacks: an attacker who captures the browser POST (e.g., via XSS on the IdP portal) can replay the assertion at the ACS URL within the `NotOnOrAfter` window. (3) Open redirect risk: the RelayState parameter (contains the post-login destination URL) in IdP-initiated flow is provided by the IdP; malicious RelayState values could redirect the user after login. Mitigation: (1) Validate RelayState as an allowlisted URL path. (2) Use the SAML assertion replay cache to prevent reuse. (3) Add IP-based and user-agent-based anomaly detection for IdP-initiated logins. (4) Consider disabling IdP-initiated SSO entirely for high-security environments.

**Q15: How would you implement SSO for a platform that serves both consumer users (no SSO) and enterprise users (mandatory SSO)?**  
A: The login flow branches based on user type discovery: (1) If the user visits via a tenant-scoped URL or enters an email with a corporate domain: route to enterprise SSO. (2) If the user enters a non-corporate email or explicitly chooses "Personal login": route to password/OAuth2 login. Platform account structure: both consumer and enterprise users have the same `users` table structure. Enterprise users have an `identity_federation` record linking to their IdP. The difference is in the authentication method: enterprise accounts have `sso_required=true` flag. If `sso_required=true` and a user attempts password login: block the attempt with "Your account requires single sign-on. Please use your company's login portal." The platform's Auth Service enforces this flag. SSO-required users still have a platform password hash (for emergency access via admin) but it is not exposed as a login option.

---

## 12. References & Further Reading

- **SAML 2.0 Core Specification** (OASIS). https://docs.oasis-open.org/security/saml/v2.0/saml-core-2.0-os.pdf
- **SAML 2.0 Web Browser SSO Profile** (OASIS). https://docs.oasis-open.org/security/saml/v2.0/saml-profiles-2.0-os.pdf
- **OpenID Connect Core 1.0**. https://openid.net/specs/openid-connect-core-1_0.html
- **RFC 9099** — OpenID Connect Back-Channel Logout 1.0. https://openid.net/specs/openid-connect-backchannel-1_0.html
- **RFC 8628** — OAuth 2.0 Device Authorization Grant (for CLI/TV SSO). https://www.rfc-editor.org/rfc/rfc8628
- **RFC 7522** — SAML 2.0 Bearer Assertion Profile for OAuth 2.0. https://www.rfc-editor.org/rfc/rfc7522
- **RFC 8252** — OAuth 2.0 for Native Apps (PKCE for mobile SSO). https://www.rfc-editor.org/rfc/rfc8252
- **SCIM Protocol** (RFC 7642-7644) — Cross-domain Identity Management. https://www.rfc-editor.org/rfc/rfc7642
- **XML Signature Wrapping Attacks — Academic Paper** (Juraj Somorovsky et al., 2012). https://www.nds.rub.de/media/nds/attachments/files/2012/06/signature_wrapping_ccs2012.pdf
- **OWASP SAML Security Cheat Sheet**. https://cheatsheetseries.owasp.org/cheatsheets/SAML_Security_Cheat_Sheet.html
- **defusedxml — Python XML Parsing Hardening**. https://github.com/tiran/defusedxml
- **Microsoft Identity Platform — SAML and OIDC Documentation**. https://learn.microsoft.com/en-us/azure/active-directory/develop/
- **Okta Developer — SAML and OIDC Integration Guides**. https://developer.okta.com/docs/concepts/saml/
- **NIST SP 800-63C** — Digital Identity Guidelines: Federation and Assertions. https://pages.nist.gov/800-63-3/sp800-63c.html
- **OpenSAML Library** (Java reference implementation). https://wiki.shibboleth.net/confluence/display/OS30/Home
- **python-saml** (python-saml by OneLogin). https://github.com/onelogin/python-saml
- **Shibboleth Identity Provider** (reference SAML IdP/SP implementation). https://www.shibboleth.net/products/identity-provider/
