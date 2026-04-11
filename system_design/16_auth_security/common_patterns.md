# Common Patterns — Auth & Security (16_auth_security)

## Common Components

### Stateless API Layer + Durable Backend
- All four systems split into a stateless API tier (horizontally scalable) and a durable persistent backend (PostgreSQL + Redis)
- auth_service: stateless auth pods → PostgreSQL (users, tokens) + Redis (refresh token cache, rate limits)
- oauth2_provider: stateless OAuth2 server → PostgreSQL (clients, codes, tokens) + Redis (authorization code hot cache)
- sso: stateless SSO pods → PostgreSQL (IdP config, federation, sessions) + Redis (SAML replay cache, OIDC JWKS cache)
- api_key_management: stateless key service → PostgreSQL (key metadata) + Redis Cluster (key validation hot path)

### Cryptographic Token Generation (CSPRNG 32 bytes)
- All four generate secrets/tokens using OS CSPRNG, never deterministic algorithms
- auth_service: refresh token = `CSPRNG(32 bytes) → base64url` (43 chars); stored as SHA-256 hash
- oauth2_provider: authorization code = `CSPRNG(32 bytes) → base64url`; refresh token = same pattern
- sso: SAML AuthnRequest ID = UUID (CSPRNG-based); relay state = CSPRNG token
- api_key_management: API key = `CSPRNG(32 bytes) → base64url` (43 chars); stored as SHA-256 hash; prefix `sk_{env}_`

### SHA-256 Hashing for Storage (Never Store Raw Tokens)
- All four store SHA-256 hashes of secrets, never the raw values — database breach cannot expose usable credentials
- auth_service: `token_hash = SHA-256(raw_refresh_token)` stored in refresh_tokens table
- oauth2_provider: `code_hash = SHA-256(raw_code)` in authorization_codes; `token_hash` in oauth2_refresh_tokens
- sso: assertion IDs stored directly (not a secret); session tokens hashed
- api_key_management: `key_hash = SHA-256(full_raw_key)` in api_keys table; `key_prefix` (first 8 chars) for human identification

### Redis for Hot-Path Token/Key Validation
- All four use Redis as the sub-millisecond validation cache to avoid PostgreSQL reads per request
- auth_service: refresh token store in Redis (write-through from PostgreSQL); rate limit counters per IP + account
- oauth2_provider: authorization code in Redis (`GETDEL oauth2:code:{hash}` for single-use atomicity); access token introspection cache
- sso: SAML assertion replay cache `saml:assert:{assertion_id}` with TTL = NotOnOrAfter − now(); OIDC JWKS cache (1-hr TTL); IdP domain lookup cache
- api_key_management: `apikey:{hash}` → serialized key metadata; write-through on creation; immediate DEL on revocation; native TTL for expiring keys

### KMS / HSM for Long-Term Secret Storage
- All four store cryptographic secrets in HSM or KMS (AWS KMS / HashiCorp Vault), never in the database or application code
- auth_service: RSA RS256 signing private key (JWKS); Argon2id pepper (32 bytes); TOTP encryption key (AES-256-GCM for encrypted TOTP secrets)
- oauth2_provider: RS256 signing key (shared with auth_service for ID token issuance); client_secret_hash stored in DB (plaintext secret only shown once at registration)
- sso: SP (Service Provider) SAML signing key; OIDC client secrets encrypted via KMS (`oidc_client_secret_enc` BYTEA + `oidc_client_secret_key_id`)
- api_key_management: no long-term KMS usage (SHA-256 for key storage is safe given 256-bit entropy); audit log encryption uses KMS

### JWT RS256 with JWKS for Stateless Verification
- All four issue JWT access tokens signed with RS256; resource servers verify via cached JWKS endpoint (no per-request network call to auth server)
- auth_service: access token JWT RS256, TTL 900 s (15 min); `kid` header for key rotation; JWKS at `/.well-known/jwks.json`, CDN-cached max-age=3600
- oauth2_provider: access token JWT RS256, TTL 3600 s (1 hr); ID token (OIDC) JWT RS256 with nonce; discovery at `/.well-known/openid-configuration`
- sso: platform session token JWT RS256 after IdP authentication; `amr` claim carries IdP authentication methods
- api_key_management: not JWT-based (opaque API key); internal services get JWT access token after validating API key

### Refresh Token Rotation with Reuse / Family Detection
- auth_service and oauth2_provider implement token family-based rotation: each refresh token has a family_id; if a used token is reused, the entire family is invalidated and a security alert sent
- Pattern: `SERIALIZABLE` transaction + `SELECT ... FOR UPDATE` on refresh_tokens row; if `used_at IS NOT NULL` → reuse detected → invalidate family
- Data structures: `refresh_token_families` (family_id, user_id, invalidated_at, invalidation_reason) + `refresh_tokens` (token_hash, family_id, used_at, replaced_by, revoked_at)

### Audit Log (Kafka → ClickHouse + S3)
- All four emit all security events (acquire, release, create, use, revoke, suspend) to Kafka → ClickHouse (30-day hot query) + S3 Parquet (7-year cold retention)
- Fields always include: actor_id, actor_ip, target_entity_id, event_type, timestamp, outcome

## Common Databases

### PostgreSQL
- All four; primary source of truth for users, tokens, keys, configurations; ACID transactions; row-level locking (`SELECT...FOR UPDATE`) for token operations; SERIALIZABLE isolation for reuse detection

### Redis
- All four; hot-path token/key validation cache; rate limiting counters; replay prevention (SAML, TOTP); session state; write-through from PostgreSQL

### Kafka + S3
- All four; asynchronous audit logging; decouples security event emission from main request path; S3 for 7-year compliance retention

## Common Communication Patterns

### gRPC for Internal Service-to-Service Auth
- auth_service ↔ oauth2_provider: OAuth2 delegates user authentication to Auth Service via gRPC
- sso → Auth Service via gRPC to create platform session after IdP assertion validation
- api_key_management: validation library calls key service internally via gRPC on cache miss

### Constant-Time Comparison and Dummy Work
- All four avoid timing oracle attacks by performing constant-time operations
- auth_service: dummy Argon2id hash computation for non-existent users (prevents user enumeration via timing)
- oauth2_provider: constant-time HMAC comparison for client_secret validation
- api_key_management: SHA-256 is constant-time for equal-length inputs; no early exit

## Common Scalability Techniques

### In-Process JWKS Cache (1-hour TTL)
- All JWT-issuing systems (auth_service, oauth2_provider, sso) cache JWKS in-process on gateway pods; avoids network call per request for JWT signature verification; key rotation uses `kid` to drain in-flight tokens with old key (overlap = access token TTL)

### Rate Limiting Per IP + Per Account (Redis Token Bucket)
- auth_service, oauth2_provider, sso all implement rate limiting at both IP level (bot protection) and account level (credential stuffing prevention); Redis INCR + EXPIRE per time window

## Common Deep Dive Questions

### Why not store the raw refresh token in the database?
Answer: If the database is compromised, attackers cannot reconstruct usable tokens from SHA-256 hashes (SHA-256 is one-way and computationally infeasible to reverse for 43-char CSPRNG tokens). The raw token is only in application memory during the request, not persisted anywhere. This is the same reason passwords are not stored plaintext — defense in depth against database breaches.
Present in: auth_service, oauth2_provider, api_key_management

### What is the difference between RS256 and HS256 for JWT signing?
Answer: RS256 uses RSA asymmetric key pair: the private key (stored in KMS, never shared) signs tokens; the public key (published in JWKS) verifies tokens. Any resource server can verify without having the signing key. HS256 uses a shared secret — every service that verifies must have the secret, creating a key distribution problem and a larger blast radius if the secret leaks. RS256 is required for multi-service architectures where resource servers are separate from the token issuer.
Present in: auth_service, oauth2_provider, sso

## Common NFRs

- **Token issue latency**: P99 < 100 ms (login/token endpoint)
- **Token validation latency**: P99 < 1 ms (JWKS-cached signature verification, in-process)
- **API key validation latency**: P99 < 5 ms (Redis GET)
- **Availability**: 99.99% for token validation path; 99.9% for management operations
- **Security**: all secrets stored hashed or encrypted; no plaintext credentials in database
- **Audit**: every security event logged with actor, timestamp, outcome; 7-year retention
