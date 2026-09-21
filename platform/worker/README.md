# Account Worker

The Worker contains invite-only accounts, verified email, current-role enforcement, signed Cloudflare Access assertions, versioned terms, password recovery and revocable sessions. It is not deployed. Unconfigured bindings fail closed. No exchange credentials, trading endpoint or run-file writes are implemented.

Run the checks with Node 24 LTS:

```sh
npm ci --ignore-scripts
npm test
```

The tests use actual RS256 signatures, PBKDF2-HMAC-SHA256 at 600,000 iterations, Node SQLite executing the D1 statements, and an explicitly serialized storage harness for the limiter. They are local integration evidence, not proof of a real Access login, D1 deployment, delivered email or browser acceptance. The earlier authenticated remote hash probe is recorded separately in `../docs/hash-probe-results.json`.

## Bindings and deployment

`wrangler.example.jsonc` is a closed template with example identifiers. Copy it to an ignored private configuration and resolve the inventory there. Do not replace public examples with actual account IDs, addresses or access metadata.

| Binding or setting | Purpose |
| --- | --- |
| `DB` | D1 database, apply `migrations/0001_accounts.sql` once to a new staging database |
| `AUTH_LIMITER` | SQLite Durable Object `AuthLimiter`; every request uses `global-auth-v1` |
| `APP_ORIGIN` | Exact HTTPS app origin; Worker also rejects alternate hosts |
| `ACCESS_TEAM_DOMAIN`, `ACCESS_AUDIENCE` | Exact human Access application issuer and audience |
| `TURNSTILE_SITE_KEY`, `TURNSTILE_HOSTNAME` | Public widget key and exact server-verified hostname |
| `TURNSTILE_SECRET`, `RATE_KEY_SECRET` | Worker secrets; rate key must be at least 32 random characters |
| `EMAIL`, `EMAIL_FROM` | Cloudflare Email Sending binding restricted to the approved sender |
| `INVITE_ACCOUNTS` | Private JSON list of exactly two approved `{email,handle}` mappings, handles `deandre` and `ali` |
| `RUN_ORIGIN`, `ORIGIN_CLIENT_ID`, `ORIGIN_CLIENT_SECRET` | Fixed protected Tunnel origin and Worker-only service credentials; never shipped to the client |

Default and preview hosts are disabled. Deployment must protect the app and `/api/*` with the reviewed Access policy and test the Pages/Worker route precedence. The app must not be publicly activated merely because a bundle dry-run passes. No paid service enablement or real email is part of local testing.

## Token and mutation rules

Invite tokens are fixed to an email, board handle and role. First redemption binds the observed Access subject without requiring Access and invitation emails to match. An operator must seed the first owner invitation in a controlled setup transaction; there is no public bootstrap endpoint. No real account has been seeded by these changes.

Only password digests and random token digests are stored. Emailed tokens use fragments so they do not appear in HTTP query logs. Verification and reset require Access plus token, with no app cookie. Password change first calls `/auth/reauth`, receiving a session-bound proof valid for 120 seconds, then `/me/password` consumes it. Each request runs at most one KDF.

Email change posts `{email: currentAddress, new_email: requestedAddress, turnstile}`. The current address keys the account quota and must match the session; the destination cannot choose a fresh allowance. The confirmation posts the original current address, token and Turnstile result. Credential changes atomically consume an operation marker, recheck the current account/session version, invalidate other credential proofs and revoke old sessions. Role changes require `expected_version` from the latest owner user listing, and the last verified active owner cannot be removed.

Every mutation requires an exact Origin; session mutations also require `X-CSRF-Token`. Auth reservations precede Turnstile/JWKS network calls and KDF work. Unadmitted transport/JWT floods and repeated 429 responses do not create D1 audit rows; admitted failed logins do. Valid session revocation remains available when the login quota is exhausted. A user may hold at most 20 active sessions. Maintenance removes up to 1000 expired/used credentials per table every 15 minutes, after a 24-hour grace; this exceeds the maximum admitted creation rate. Audit retention and export are a production policy gate, separate from credential cleanup.

Avatar code uses the Cloudflare Images binding to decode, resize and re-encode PNG/JPEG/WebP to a 128 square WebP, with 2 MiB uploads, animation rejection, decoded pixel bounds and 128 KiB output bounds. Only transformed bytes go to private R2; D1 uses a session/version guard and expected old object key to prevent conflicting saves. Uploads require `AVATAR_READY=verified`, `IMAGES` and private `AVATARS` bindings. A real remote development probe passed PNG/WebP decoding and invalid/oversized rejection; no production flag or bucket was activated. Uncertain saves can leave an unreferenced object for later cleanup rather than deleting an object that might already be in use.

The Git-backed board, owner connection settings, human-identity rebinding and authenticated WebSocket sessions are not enabled yet. They return explicit unavailable responses. The browser account flow and real end-to-end acceptance remain gates before M1 completion.

References: [D1 batches](https://developers.cloudflare.com/d1/worker-api/d1-database/), [Email Service](https://developers.cloudflare.com/email-service/), [Access token validation](https://developers.cloudflare.com/cloudflare-one/access-controls/applications/http-apps/authorization-cookie/validating-json/).
