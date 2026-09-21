# M1 account and access design — T-018

G1 reviewed by Fable at `62cd7bf`, board head `780dea0`; approved once the implementation details below are incorporated. This is a design, not a live account service. Requirements are the attached brief and the joint execution plan; real users are not seeded until identities are confirmed.

## Compatibility gate — T-021

On 21 September 2026 an authenticated remote preview ran on the existing Cloudflare account, Wrangler 4.135.0, compatibility date 2026-09-21. Both WebCrypto `deriveBits` and `node:crypto.pbkdf2Sync` rejected PBKDF2-HMAC-SHA256 at 600,000 iterations with `NotSupportedError: Pbkdf2 failed: iteration counts above 100000 are not supported (requested 600000).` The test used only fixed public synthetic input, created no accounts, and exposed no production app. Source is `../worker/probes/pbkdf2.mjs`.

Do not reduce iterations or invent chained hashes. The proposed fallback is standard PBKDF2-HMAC-SHA256 at the same 600,000 iterations using pinned `@noble/hashes@2.4.0` in the Worker. Its implementation must match Node's native output on multiple test vectors. Record the real remote-preview success and external elapsed times separately from CPU billing, which a client stopwatch does not measure. Require an existing paid CPU budget, bounded pre-hash rate checks, at most one expensive operation per request, and peer review of the dependency/source before adoption. If it does not fit, keep accounts closed and choose a reviewed supported KDF architecture; never silently fall back to a cheaper hash.

Primary references: [Workers Web Crypto](https://developers.cloudflare.com/workers/runtime-apis/web-crypto/), [Workers Node crypto](https://developers.cloudflare.com/workers/runtime-apis/nodejs/crypto/), [Workers limits](https://developers.cloudflare.com/workers/platform/limits/), [noble-hashes source and security notes](https://github.com/paulmillr/noble-hashes). A project audit history is not proof that every later package version or this application has been audited.

Remote fallback result at 02:21 UTC: all three 600,000-iteration requests succeeded and exactly matched Node's native PBKDF2 result. Client round trips were 2,405 ms, 1,582 ms and 1,549 ms. These bound the single request's total latency in this test, not a billed-CPU measurement or a load test. See `hash-probe-results.json`. The authenticated preview was stopped after testing. This resolves runtime compatibility; security implementation review and concurrency/rate tests remain required.

## Identity and entry points

Two layers are required: Cloudflare Access and the app's email/password account. Match the signed Access identity to the D1 user's separately verified identity binding; do not trust an unsigned `Cf-Access-Authenticated-User-Email` header. Validate signature through the team's JWKS, exact issuer, application audience, expiry and not-before. Cache keys with bounded lifetime and fail closed when unavailable. Reject non-human/service identities at human account routes. Proposed Access session: 24 hours; app sessions: seven days with explicit revocation. Neither extends the other.

The member's invitation address is recorded privately; the GitHub-verified sign-in address may differ. Do not force them to match before observing his real verified GitHub identity. Store the reviewed binding between Access `sub`/email and app user, with an owner-controlled setup audit record. Do not grant access to a GitHub username alone or use repository commits to infer email. Owner seed email is confirmed privately; the real Access subject is pending. An exact two-person allowlist remains separate from Cloudflare account membership.

Pages is the static frontend. Route same-origin `/api/*` on `app.example.test` to the Worker; it then exposes the contract paths after that prefix. Protect the custom and default preview hosts. Proposed separate API hostname uses an explicit approved desktop/service flow, no wildcard credentialed CORS. All mutations require an exact Origin and CSRF token bound to the app session; login/signup/reset also require exact origin, JSON content type and a valid Turnstile token. Reject cross-origin redirects and never proxy arbitrary URLs from a connection form.

Browser session cookie: `__Host-platform-session`, Secure, HttpOnly, SameSite=Strict, Path=/, no Domain, seven-day Max-Age. Store only a SHA-256 token digest in D1, not the bearer. Rotate after login and privilege-sensitive account changes. D1 session version and revoked/expiry checks on every API request prevent a stale JWT role or cached session surviving a reset. Access identity must match the session's binding.

Electron uses the G1-reviewed human cloudflared Access flow described below plus a separate app session. Keep tokens in main-process memory and OS-backed safeStorage; fail closed on insecure plaintext backends. Never ship service credentials. A future one-use PKCE exchange is not assumed implemented. Real external-browser sign-in, token refresh and logout remain desktop acceptance gates.

## Storage model (D1)

| Table | Fields / invariants |
| --- | --- |
| `users` | UUID id; unique normalized invite email; display name; owner/member CHECK; password scheme/version/salt/hash/iterations; verified_at; disabled_at; session_version; Access subject/email binding; created/updated times. Exactly two active allowed identities at initial setup. |
| `invites` | Digest of 32-byte random token; normalized email and role fixed by owner; issuer; expiry; accepted_at. No plaintext token in storage. |
| `sessions` | Token digest, user FK, version, created/expires/revoked timestamps, CSRF digest, bound Access subject; no passwords. |
| `verification_tokens` | Digest, user, purpose (verify/reset/email-change), pending email if applicable, expires/used timestamps. Purpose-specific endpoints prevent substitution. |
| `terms_acceptances` | User, immutable terms version, accepted_at, content hash; unique user/version. |
| `avatars` | User, random private object key, MIME, width=128, height=128, content hash, updated_at. |
| `audit_events` | Append-only id/time, actor (or unauthenticated), action, outcome, safe object identifier, coarse reason; no passwords/tokens/mail bodies/raw request payloads. |
| `settings` | Approved API origin and sender/terms version; owner-only changes; no arbitrary origin URL from members. |

Use prepared statements and D1 batches/transactions for one-use tokens, account changes and revocation. Condition every mutation on unused, unexpired tokens and current account state within the same transaction; simultaneous redemption must produce one winner. Do not update a password in one transaction and revoke sessions in an unrelated best-effort step. Never remove/disable/demote the final active owner. Prevent stale updates with an expected version.

## Passwords, email and abuse limits

Passwords: minimum 12 characters, maximum 128 Unicode code points and 512 UTF-8 bytes, no silent trim or normalization, no arbitrary composition rules. Use a fresh cryptographically random 16-byte salt per password, 32-byte PBKDF2 result and stored scheme/iteration count. Constant-time fixed-length comparison. Reject overlong bodies before expensive work. Compare against a dummy hash for unknown users after the same rate checks to reduce enumeration differences.

Rate limits must be globally atomic, not per-isolate memory counters. Use a dedicated auth Durable Object with transactional sliding-window reservations, keyed by a keyed digest of trusted Cloudflare IP and normalized account identifier; ten attempts per ten minutes, before hashing. A tenth failed password locks the account for ten minutes; combine per-account and per-IP limits. Turnstile tokens are server-verified, single-use, exact hostname/action. Bound reset/signup/email-send attempts independently to avoid mail floods. IP identity comes only from Cloudflare's trusted request context, not client-supplied forwarding headers. Auth must fail closed if the limiter is unavailable.

Verification: 32-byte random token, digest in D1, 24-hour expiry, single-use. Password reset: 30-minute expiry and single-use; generic responses regardless of account existence. Email change verifies the new address, retains the current binding until accepted, and revokes sessions after completion. Password change requires the old password and a valid session, then revokes all other sessions. Sign-out-everywhere increments session version and revokes every session.

Use the available Cloudflare Email Sending binding with an exact allowed sender on the configured sending domain, text and escaped HTML bodies, approved fixed base URL, no external redirect parameter. Send provider acceptance and delivered mail are separate states. Do not mark email verified because the provider accepted it. Tokens appear only in the intended emailed link and transient browser request; do not log full URLs. Staging uses a controlled recipient/captured transport until the real user performs the workflow.

## API and permissions

| Route (all under `/api`) | Who | Result |
| --- | --- | --- |
| `POST /auth/signup` | Allowed Access identity + matching invitation | Create unverified account, issue verification email; no session |
| `POST /auth/verify` | Allowed Access identity + token | Atomically verify, consume token; explicit login follows |
| `POST /auth/login` | Allowed Access identity + verified account | Rate/Turnstile/hash checks, new session + CSRF token |
| `POST /auth/logout`, `/auth/logout-all` | Current session | Revoke one/all; clear cookie |
| `POST /auth/forgot`, `/auth/reset` | Allowed Access identity | Generic request response; reset token consumption revokes sessions |
| `GET /me`, `PATCH /me` | Current verified session | Own safe profile; validated display name only |
| `POST /me/password`, `/me/email`, `/me/email/verify` | Current session / corresponding token | Old password/new email verification, audit and session revocation |
| `PUT /me/avatar`, `GET /avatars/:user` | Current session | Decode/validate/resize to 128 square; private authenticated read |
| `GET /terms`, `POST /terms/accept` | Current verified session | Read current version; save immutable server timestamp receipt |
| `/runs`, `/instances`, `/events`, `/board/*` | Current session + current terms | Contract read routes; board writes allowed to owner/member |
| `/admin/invites`, `/admin/users/:id`, `/admin/settings`, `/admin/audit` | Current owner + current terms | Allowlist/invite/role/settings/audit; final-owner and actor checks |

Unknown routes and unsupported methods fail closed. Return safe errors and no-store headers. Owner-only checks are repeated in each handler, not just hidden controls. Avatars accept a maximum 2 MiB request, validate actual decoded PNG/JPEG/WebP content and pixel limits, strip metadata, resize server-side and re-encode; filename/content-type alone proves nothing. An unconfigured safe decoder disables uploads rather than trusting client-side resize. Files and audit events are private and subject to deletion/retention choices recorded before production.

## Review and test gates

G1: Fable reviews this design, real hashing compatibility evidence, lossless numeric contract and same-origin/desktop limitations. G2: independently review implemented handler-level authorization, atomic token use, rate reservations, session revocation, invite bounds, file decoding and signed Access validation. Unit/integration tests must use real signatures and deliberately wrong issuer/audience/expiry/member identities, not only mocked authorization booleans. Then conduct the eleven acceptance checks with their correct environments. Until configuration and real identities are verified, production account routes deny access.

## G1 decisions incorporated after peer review

1. Known answers: use RFC 7914 section 11 for PBKDF2-HMAC-SHA256 (RFC 6070 covers SHA-1, so its expected bytes are not SHA-256 vectors). Add at least three independent salt/password-length comparisons against Node native crypto, including embedded NUL/non-ASCII input; production still fixes the work factor at 600,000. `npm ci` enforces the pinned tarball integrity. One KDF invocation is allowed per request. Changing a password is two explicit requests: `/auth/reauth` verifies the old password and issues a short-lived, session-bound one-use proof; `/me/password` consumes that proof and hashes the new password. This preserves the old-password requirement without hiding a second KDF call in one request.
2. G2 refines the order to prevent an invalid Access assertion locking an account: exact origin/content type/bounded body; atomic ingress admission (120/IP/minute and 600/global/minute, no account state); signed Access verification; named account/KDF reservation; Turnstile; account/token lookup; at most one KDF. All KDF purposes share 10 attempts/IP/10 minutes and a global maximum of 60/minute; account windows are purpose-specific. Recovery has a separate IP/account allowance. Ordinary writes use separate 120/IP and 120/user windows per 10 minutes, no lockout, and successful writes remain counted. Successful auth/recovery clears the corresponding account window while IP/global usage remains. The tenth failed login locks that account for ten minutes. This keeps a pre-external limit from G1 without charging an unverified account.
3. Invitation binding: first successful invite-token redemption binds the invited account to the validated Access `sub`, with a unique database constraint. The invitation token proves access to the invited inbox; the signed Access assertion proves the authenticated identity. Do not force the two emails to match. Record the observed Access email as informational. Later login requires the same `sub`; an email change on that subject is audited, while a subject change requires an owner re-bind and session revocation.
4. Access JWT algorithm is restricted to `RS256`, with exact issuer/audience and bounded JWKS caching. No algorithm negotiation from an untrusted header.
5. Desktop choice: obtain the **human** Access token through the official external-browser `cloudflared access login` flow, retrieve only the application-scoped token, and send `cf-access-token` from Electron's main-process HTTP client as well as the separate app session. A longer Access lifetime alone cannot substitute for the token. Use the documented CLI adapter initially; packaging, binary provenance, safeStorage, cache handling, logout and real token refresh are explicit M5 gates. No app-only bypass route or embedded service token. Reference: [Cloudflare client authentication](https://developers.cloudflare.com/cloudflare-one/access-controls/authenticate-agents/) and [Access CLI](https://developers.cloudflare.com/cloudflare-one/tutorials/cli/). Capture token output only in native memory; never echo it or include it in URLs/logs. A future PKCE-only replacement requires a fresh end-to-end design review; it is not assumed implemented.
6. Verify/reset links use purpose-scoped one-use tokens and require no existing app session. They still pass Access and abuse checks. Consequently an emailed link does not require weakening SameSite=Strict.
7. Avatar decoder: use the Cloudflare Images binding, operating on bounded uploaded raw bytes, transforming with width=128, height=128, fit=cover and re-encoding to WebP. Enforce decoded-pixel limits with the binding's metadata API and reject unsupported/animated inputs. Store only the transformed result in private R2. Reference: [Images binding](https://developers.cloudflare.com/images/optimization/binding/). Uploads stay unavailable unless that binding is enabled and its actual output is verified; no client-resize fallback or paid enablement is inferred.

## G2 implementation delivery

The G2 revision has 77 passing local tests across real signed Access JWTs, password KDFs, limiter concurrency, actual SQLite transactions, handler flows and avatar storage boundaries. Role writes revalidate the current owner/session and expected target version atomically. Credential consumption uses a unique operation marker, checks fresh expiry/version/session state, invalidates sibling proofs and revokes old sessions. Reauthentication proofs last 120 seconds. Login is capped at 20 active sessions; authenticated revocation remains available after the login allowance is exhausted. Maintenance removes up to 1000 expired credentials per table each 15 minutes after a 24-hour grace. Pre-admission transport failures and repeated 429s cannot amplify D1 audit writes.

The real Cloudflare Images remote-development probe accepted synthetic PNG/WebP and produced 128 x 128 WebP, rejecting invalid/SVG/over 2 MiB input. Production avatar bindings remain disabled. See `avatar-probe-results.json`; captured test fixtures contain no user photograph. Real Access/Turnstile/email delivery, D1/R2 deployment, browser flows, identity rebinding, board and desktop integration are still separate gates.

## G2 review response, round 06

Fable reviewed head `79ec1be` and recorded conditional approval at board `982d72e`. The revision separates ordinary writes and recovery from the KDF allowance, admits ingress without account side effects before verifying Access, reports every completed auth route as successful, compares decoded CSRF hashes with `equalDigest`, and provides the operator-only first-owner script. Browser account screens retain the app session when Access expires and offer full-navigation renewal, including an opaque edge redirect. They scrub emailed fragments before any UI or third-party script and request a fresh Turnstile challenge for each server action. These fixes await the next independent code re-check and real staging acceptance; conditional review is not activation.
