# Problem-Specific Design — Auth & Security (16_auth_security)

## Auth Service

### Unique Functional Requirements
- Email/password login → JWT access token (RS256, 15 min) + opaque refresh token (90 days)
- Refresh token rotation: issue new token on each use; reuse detection invalidates entire family
- MFA: TOTP (RFC 6238) + WebAuthn (FIDO2)
- Account lockout after 10 failed attempts; brute-force rate limiting + CAPTCHA
- Single-use password reset tokens; selective session revocation

### Unique Components / Services
- **Argon2id Password Hasher**: m=19456 KiB, t=2, p=1, output 32 bytes, salt 16 bytes CSPRNG; pepper (32-byte HSM secret) included in hash; ~100–200 ms/hash; PHC format: `$argon2id$v=19$m=19456,t=2,p=1$<salt_b64>$<hash_b64>`; pepper rotation via version tracking
- **Token Issuer**: RS256 JWT access token (sub, iat, exp=+15 min, jti, amr=["pwd","otp"]); refresh token = CSPRNG(32 bytes) → base64url (43 chars); stored as SHA-256(raw_token)
- **Refresh Token Store (Write-Through)**: Redis for O(1) lookup (hot path); PostgreSQL for durability; family-based reuse detection in SERIALIZABLE transaction with `SELECT...FOR UPDATE`
- **TOTP MFA (RFC 6238)**: Secret = CSPRNG(20 bytes) → base32 (160-bit); AES-256-GCM encrypted in DB; HMAC-SHA1 with 30-second steps; ±1 window tolerance (3 windows total); replay prevention via `SHA256(user_id+window+code)` in Redis, 90-second TTL
- **WebAuthn/FIDO2**: credential_id (BYTEA), public key (COSE-encoded BYTEA), sign_count (clone detection), transports TEXT[], aaguid UUID; ECDSA P-256 or RSA signature verification
- **Dummy Hash Guard**: for non-existent user login attempts, perform full Argon2id computation to prevent timing-based user enumeration (~100 ms constant response)

### Unique Data Model
- **users**: id, email_normalized (unique lowercase), password_hash (Argon2id PHC string, 512 chars), account_state (active/locked/disabled/pending_verification), failed_attempts, locked_until
- **refresh_token_families**: id, user_id, created_at, invalidated_at, invalidation_reason
- **refresh_tokens**: id, family_id, token_hash CHAR(64) (SHA-256 hex), expires_at (+90 days), used_at, replaced_by, revoked_at
- **mfa_enrollments**: id, user_id, method (totp/webauthn/backup_code), totp_encrypted_secret (AES-256-GCM), totp_key_id (KMS ref), webauthn_credential_id, webauthn_public_key, webauthn_sign_count

### Algorithms

**Refresh Token Rotation with Reuse Detection:**
```sql
-- SERIALIZABLE transaction:
SELECT * FROM refresh_tokens WHERE token_hash = $1 FOR UPDATE
-- If token.used_at IS NOT NULL → REUSE DETECTED:
UPDATE refresh_token_families SET invalidated_at=NOW(), invalidation_reason='reuse_detected'
UPDATE refresh_tokens SET revoked_at=NOW() WHERE family_id=family.id
-- emit security alert (audit + email)
-- Otherwise mark old token used, issue new token with same family_id
```

### Key Differentiator
Auth Service's uniqueness is its **Argon2id with HSM pepper + token family reuse detection**: OWASP-recommended Argon2id (m=19456, t=2, p=1) with 32-byte pepper in HSM ensures DB breach + code breach combined are insufficient for offline cracking; refresh token family invalidation on any reuse detects concurrent token theft; TOTP AES-256-GCM encrypted secrets in DB with KMS key; WebAuthn FIDO2 support for phishing-resistant MFA.

---

## OAuth2 Provider

### Unique Functional Requirements
- Authorization Code Flow with mandatory PKCE (RFC 7636) for all public clients
- Client Credentials Flow (M2M); Device Authorization Flow (RFC 8628) for input-constrained devices
- Token introspection (RFC 7662), token revocation (RFC 7009), scope management
- Consent persistence per (user_id, client_id, scope_set)
- OpenID Connect ID tokens; RFC 8414 discovery endpoint
- Access token TTL 3600 s (1 hr, longer than auth_service 15-min JWT because resource servers implement introspection caching)

### Unique Components / Services
- **PKCE Validator**: `code_challenge = BASE64URL(SHA256(code_verifier))`; `code_verifier = CSPRNG(32–96 bytes) → base64url`; `code_challenge_method=S256` required (`plain` discouraged); prevents authorization code interception attacks in public clients
- **Authorization Code Store (Redis)**: single-use; stored as hash → `{client_id, user_id, scope, pkce_challenge, redirect_uri, nonce, auth_time}`; TTL 600 s; atomically consumed via `GETDEL oauth2:code:{hash}`
- **Consent Store**: `consent_grants(user_id, client_id)` UNIQUE constraint; cached in Redis (1-hr TTL); users can revoke; skip_consent flag per client
- **Device Authorization (RFC 8628)**: device_code = CSPRNG(32 bytes) → SHA-256 stored; user_code = 8-char alphanumeric hyphenated (e.g., "WDJB-MJHT"); poll interval = 5 s; TTL 600 s; device polls `POST /token` until approval or expiry
- **Token Introspection (RFC 7662)**: resource servers POST access_token to introspection endpoint; response cached in Redis with TTL = token remaining lifetime; reduces load on token store

### Unique Data Model
- **oauth2_clients**: client_id (VARCHAR 64), client_secret_hash (NULL for public clients), client_type (public/confidential), redirect_uris TEXT[] (exact match), grant_types TEXT[], allowed_scopes TEXT[], access_token_ttl (3600s), refresh_token_ttl (2592000s = 30 days), require_pkce (default TRUE), skip_consent
- **authorization_codes**: code_hash CHAR(64), client_id, user_id, scope, redirect_uri, pkce_challenge, pkce_method (S256/plain), nonce, auth_time, amr TEXT[], expires_at (+10 min)
- **oauth2_refresh_tokens**: same family-based rotation as auth_service; adds client_id field
- **consent_grants**: user_id, client_id, scope (space-separated), granted_at, expires_at (NULL = indefinite); UNIQUE(user_id, client_id)
- **device_authorization_codes**: device_code_hash, user_code (VARCHAR 8 UNIQUE), client_id, scope, is_approved, approved_user_id, expires_at, interval_seconds=5

### Key Differentiator
OAuth2 Provider's uniqueness is its **PKCE-mandatory authorization code flow + RFC 8628 device flow**: PKCE (code_verifier CSPRNG → BASE64URL(SHA256) → code_challenge) prevents authorization code interception attacks; RFC 8628 device authorization for TVs/CLIs without browser redirect capability; consent persistence per (user_id, client_id) avoids re-prompting; access token TTL 1 hr (vs 15 min in auth_service) because resource servers cache introspection results.

---

## Single Sign-On (SSO)

### Unique Functional Requirements
- SAML 2.0 SP-Initiated SSO: accept assertions from enterprise IdPs (Okta, Azure AD, Ping, ADFS)
- OIDC Relying Party: authorization code flow with PKCE to external OIDC IdPs
- IdP discovery by email domain; JIT (Just-in-Time) provisioning
- Single Logout (SLO): back-channel + front-channel propagation
- Attribute mapping from IdP schemas to platform schema (JSONB config)
- XML Signature Wrapping (XSW) attack prevention
- SAML assertion replay cache; assertionID uniqueness per (idp_id, assertion_id)
- 500 distinct enterprise IdPs; 10,000 SSO logins/day

### Unique Components / Services
- **SAML AuthnRequest Generator**: Deflate XML → base64url → `SAMLRequest` query parameter; signed with SP private key (RSA-SHA256); stores `{requestID → {idp_id, created_at, relay_state}}` in Redis (TTL 10 min); InResponseTo validation prevents assertion injection
- **SAML Assertion Validator (XSW Prevention)**: (1) Verify XML structure first; (2) find IdP signing cert from `idp_signing_certificates` table; (3) verify RSA-SHA256 digital signature; (4) validate InResponseTo = stored requestID; (5) Conditions.NotBefore ≤ now() ≤ Conditions.NotOnOrAfter; (6) AudienceRestriction contains SP EntityID; (7) check assertionID NOT IN replay cache; (8) store assertionID in Redis with TTL = NotOnOrAfter − now()
- **JIT Provisioner**: on first SSO login, lookup `identity_federations` by (idp_id, external_subject); if absent → create platform user + federation record; if present → update attributes; auto-sync groups if `jit_group_sync=TRUE`
- **Attribute Mapper**: JSONB rules per IdP translate attribute names: `{"email": "urn:oid:0.9.2342.19200300.100.1.3", "first_name": "givenname"}` — admin-configurable without code changes
- **SLO Handler**: back-channel POST to IdP's SLO URL (SAML) or RP-Initiated logout (OIDC); terminates all `sso_sessions` for the user+idp pair

### Unique Data Model
- **identity_providers**: protocol (saml2/oidc/wsfed), saml_entity_id, saml_sso_url, saml_slo_url, saml_metadata_xml (cached), oidc_issuer, oidc_client_id, oidc_client_secret_enc (AES-256-GCM + KMS key_id), attribute_mapping JSONB, jit_provisioning_enabled, required_amr TEXT[]
- **idp_signing_certificates**: certificate_pem TEXT, fingerprint_sha256 CHAR(64), is_active, valid_from, valid_until (multiple certs per IdP for rotation overlap)
- **identity_federations**: platform_user_id, idp_id, external_subject (VARCHAR 1024 — NameID or OIDC sub); UNIQUE(idp_id, external_subject)
- **sso_sessions**: platform_session_id, idp_session_index (SAML), idp_name_id, oidc_id_token (for id_token_hint), oidc_sid; terminated_at, termination_reason
- **saml_assertion_ids**: assertion_id, idp_id, received_at, not_on_or_after; PRIMARY KEY (assertion_id, idp_id)

### Key Differentiator
SSO's uniqueness is its **multi-protocol SP (SAML 2.0 + OIDC) with XSW prevention and JIT provisioning**: XML Signature Wrapping attack prevention (validate signature → extract content, never the reverse), SAML assertion replay cache (Redis TTL = NotOnOrAfter − now()), OIDC client_secret encrypted with KMS, configuration-driven attribute mapping (JSONB) supporting arbitrary IdP schemas, and Single Logout back-channel propagation — the only problem that bridges enterprise identity providers to the platform's native auth system.

---

## API Key Management

### Unique Functional Requirements
- 256-bit entropy minimum; CSPRNG(32 bytes); format: `sk_{live|test|dev}_{43 random chars}`
- SHA-256 for storage (not Argon2id — fast hash safe given 256-bit entropy; no brute-force risk)
- Key rotation with 24-hour overlap window (both old and new keys valid during deployment)
- Immediate revocation with < 500 ms global propagation (Redis DEL on revocation)
- GitHub Secret Scanning partner integration: regex `sk_live_[A-Za-z0-9\-_]{43}` detectable by scanners
- Scoped permissions: `read:data`, `write:records`; environment segregation (production/staging/development)
- Usage tracking: request count, error count, bytes transferred, last_used_at

### Unique Components / Services
- **Key Generator**: `raw_key = f"sk_{env}_{base64url(CSPRNG(32))}"` (total 51 chars); `key_hash = SHA256(raw_key)` (64-char hex); `key_prefix = sk_{env}_{first 8 random chars}` for identification (reveals 48 bits of 256 → remaining 2^208 ≈ 10^62 combinations)
- **Validation Hot Path (Redis)**: `GET apikey:{hash}` → serialized JSON {key_id, scopes, status, expires_at, environment, owner_id}; write-through from PostgreSQL on creation; DEL on revocation (sub-500 ms propagation); native TTL for expiring keys; P99 < 5 ms end-to-end
- **Rotation Manager**: on rotate → generate new key → set old key status=active with `rotation_overlap_ends_at = NOW() + 24h`; background job auto-revokes old key after overlap window; `rotated_from` chain in DB
- **Leakage Detection Worker**: webhook from GitHub/GitLab/GitGuardian/Trufflehog; on detection → auto-suspend key + notify owner + log `suspected_leak_url` and `suspected_leak_at`; async to avoid blocking API
- **Usage Aggregator**: Redis INCR `usage:{key_id}:requests:{day}` (fire-and-forget in request handler); background job every 5 min aggregates to `api_key_usage_daily` PostgreSQL table

### Unique Data Model
- **api_keys**: key_hash CHAR(64) UNIQUE (SHA-256 hex), key_prefix VARCHAR(20) (first 8 random chars for display), environment (production/staging/development), scopes TEXT[], status (active/revoked/suspended/expired), expires_at (optional), rotated_from UUID (FK, rotation chain), rotation_overlap_ends_at, suspected_leak_url, suspected_leak_at
- **api_key_usage_daily**: (key_id, usage_date) PK; request_count, error_count, bytes_transferred
- **api_key_policies**: tenant_id, max_keys_per_user (20), max_keys_per_team (100), forced_rotation_days, require_expiry
- **Indexes**: `idx_ak_key_hash ON api_keys (key_hash)` (hot path); `idx_ak_expires ON api_keys (expires_at) WHERE expires_at IS NOT NULL`

### Algorithms

**Why SHA-256 Instead of Argon2id for API Keys:**
- Argon2id is needed for passwords because passwords have low entropy (~28 bits for a strong password) — slow hash prevents offline cracking
- API keys have 256-bit CSPRNG entropy; brute-force at 10^9/s takes 1.16 × 10^68 years — fast hash (SHA-256) is safe
- Argon2id at 100–200 ms × 5,787 RPS = unacceptable latency overhead; SHA-256 at < 1 μs is negligible

**Key Format (Why Prefix is Safe to Expose):**
- Revealing `sk_live_AbCd1234` exposes 48 bits of a 256-bit key → 2^208 ≈ 10^62 remaining possibilities; infeasible to brute-force
- Prefix enables secret scanning regex (e.g., GitHub code scanning), used by Stripe, AWS, GitHub for same reason

### Key Differentiator
API Key Management's uniqueness is its **prefixed CSPRNG key format with write-through Redis invalidation**: `sk_{env}_{43 random chars}` format (prefix enables secret scanning integrations with GitHub/GitLab/GitGuardian), SHA-256 storage (fast hash safe at 256-bit entropy, unlike passwords requiring Argon2id), immediate revocation via Redis DEL (< 500 ms propagation), 24-hour rotation overlap window for safe deployment, and async leakage detection via webhook integration with GitHub Secret Scanning partner program — designed as a standalone API key lifecycle service, not a session token system.
