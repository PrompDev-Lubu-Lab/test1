# Account Worker

The Worker contains invite-only accounts, verified email, current-role enforcement, signed Cloudflare Access assertions, versioned terms, password recovery and revocable sessions. The production entry point also serves the protected web assets on the same origin. Deployment and account activation are separate states; consult the current deployment checkpoint. Unconfigured bindings fail closed. No exchange credentials, trading endpoint or run-file writes are implemented.

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
| `ASSETS` | Built `../app/dist`; global Worker-first routing is required |
| `DB` | D1 database, apply `migrations/0001_accounts.sql` once to a new staging database |
| `AUTH_LIMITER` | SQLite Durable Object `AuthLimiter`; every request uses `global-auth-v1` |
| `APP_ORIGIN` | Exact HTTPS app origin; Worker also rejects alternate hosts |
| `ACCESS_TEAM_DOMAIN`, `ACCESS_AUDIENCE` | Exact human Access application issuer and audience |
| `TURNSTILE_SITE_KEY`, `TURNSTILE_HOSTNAME` | Public widget key and exact server-verified hostname |
| `TURNSTILE_SECRET`, `RATE_KEY_SECRET` | Worker secrets; rate key must be at least 32 random characters |
| `EMAIL`, `EMAIL_FROM` | Cloudflare Email Sending binding restricted to the approved sender |
| `INVITE_ACCOUNTS` | Private JSON list of exactly two approved `{email,handle}` mappings, handles `deandre` and `ali` |
| `RUN_ORIGIN`, `ORIGIN_CLIENT_ID`, `ORIGIN_CLIENT_SECRET` | Fixed protected Tunnel origin and Worker-only service credentials; never shipped to the client |

Default and preview hosts are disabled. `site.mjs` delegates only `/api` and `/api/*` to the account handler; every static GET/HEAD requires the same signed human Access assertion. The root resolves explicitly to `/index.html`. Use `assets.run_worker_first=true`, `html_handling=none` and `not_found_handling=none`: selective Worker-first paths or an SPA fallback would break the security or 404 contract. The wrapper strips credentials before the asset binding and adds CSP and private no-store headers to responses. Protect the whole custom hostname with the reviewed Access application and verify the actual edge response before inviting anyone. See `../docs/PRODUCTION-HOSTING.md` for the deployment sequence. No paid service enablement or real email is part of local testing.

## Token and mutation rules

Invite tokens are fixed to an email, board handle and role. First redemption binds the observed Access subject without requiring Access and invitation emails to match. The operator-only `scripts/seed-invite.mjs` inserts a 24-hour owner invitation only into a database with no users and no active pending invitation. The raw link is shown once in an interactive terminal after the exact row is confirmed; only its digest enters SQL. There is no public bootstrap endpoint. No real account has been seeded by these changes.

Only password digests and random token digests are stored. Emailed tokens use fragments so they do not appear in HTTP query logs. Verification and reset require Access plus token, with no app cookie. Password change first calls `/auth/reauth`, receiving a session-bound proof valid for 120 seconds, then `/me/password` consumes it. Each request runs at most one KDF.

Email change posts `{email: currentAddress, new_email: requestedAddress, turnstile}`. The current address keys the account quota and must match the session; the destination cannot choose a fresh allowance. The confirmation posts the original current address, token and Turnstile result. Credential changes atomically consume an operation marker, recheck the current account/session version, invalidate other credential proofs and revoke old sessions. Role changes require `expected_version` from the latest owner user listing, and the last verified active owner cannot be removed.

Every mutation requires an exact Origin; session mutations also require `X-CSRF-Token`. A cheap IP/global ingress gate runs before external verification, without touching account counters. After Access verifies, a separate account reservation precedes Turnstile and KDF work. KDF purposes share 10 attempts/IP/10 minutes and 60 KDF reservations/minute globally; recovery has its own IP/account windows. Ordinary writes have separate 120/IP and 120/user windows per 10 minutes, with no login lockout and no reset of counted writes on success. Unadmitted transport/JWT floods and repeated 429 responses do not create D1 audit rows; admitted failed logins do. Valid session revocation remains available when the login quota is exhausted. A user may hold at most 20 active sessions. Maintenance removes up to 1000 expired/used credentials per table every 15 minutes, after a 24-hour grace; this exceeds the maximum admitted creation rate. Audit retention and export are a production policy gate, separate from credential cleanup.

Avatar code uses the Cloudflare Images binding to decode, resize and re-encode PNG/JPEG/WebP to a 128 square WebP, with 2 MiB uploads, animation rejection, decoded pixel bounds and 128 KiB output bounds. Only transformed bytes go to private R2; D1 uses a session/version guard and expected old object key to prevent conflicting saves. Uploads require `AVATAR_READY=verified`, `IMAGES` and private `AVATARS` bindings. A real remote development probe passed PNG/WebP decoding and invalid/oversized rejection; the later production bucket is provisioned but the upload feature flag remains off. Uncertain saves can leave an unreferenced object for later cleanup rather than deleting an object that might already be in use.

The Git-backed board and authenticated WebSocket bridge are implemented behind explicit feature gates. See `../docs/BOARD-ADAPTER.md` and `../docs/LIVE-EVENTS.md` for scope, rollout and evidence. Owner connection settings and human-identity rebinding still return explicit unavailable responses. The browser account flow and real end-to-end acceptance remain gates before M1 completion.

References: [D1 batches](https://developers.cloudflare.com/d1/worker-api/d1-database/), [Email Service](https://developers.cloudflare.com/email-service/), [Access token validation](https://developers.cloudflare.com/cloudflare-one/access-controls/applications/http-apps/authorization-cookie/validating-json/).

## First owner setup (T-031)

Prepare a private, ignored `bootstrap.local.json` containing `INVITE_ACCOUNTS` (the same exact two mappings as the Worker), `APP_ORIGIN`, the exact D1 `database` name, and absolute `wrangler` and `wranglerConfig` paths. Keep this file out of the public repository. Apply the migration to a new staging database first, verify Access and the configured sender, and then run from an interactive operator terminal:

```sh
node scripts/seed-invite.mjs --settings /private/bootstrap.local.json --apply --remote
```

Use `--local` only for an explicitly configured local database. The command refuses redirected/CI output, never seeds a password or Access subject, and refuses a second active invitation or any existing account. Open the one-use link privately and complete normal signup and verification. A timeout or uncertain result requires inspecting the database before retrying; a stored but unconfirmed invitation remains closed until expiry or a deliberate operator recovery. The script's real SQLite tests exercise its SQL and exact-row confirmation; no remote bootstrap has been run.

For an explicitly approved setup email, use the operator email mode instead of printing the invitation. This mode requires `--remote`; its fixed endpoint has no public HTTP handler and cannot be selected from the app:

```sh
node scripts/seed-invite.mjs --settings /private/bootstrap.local.json --apply --remote --email
```

The private settings additionally supply `CLOUDFLARE_ACCOUNT_ID`, `EMAIL_FROM` and preferably the exact `databaseId`. Pass only an existing authorized email-sending access token as the short-lived process environment variable `BOOTSTRAP_EMAIL_API_TOKEN`; never a refresh token or a token in command arguments, source, notes or settings files. The command removes this variable before creating a D1 child. It validates the account, origin, two invitation mappings, sender, sole D1 binding and sender-restricted email binding against the explicitly selected private Wrangler JSON. The operator must also compare those values with the current deployed version and record the reviewed clean source commit before running.

After the guarded insert and exact digest-row confirmation, the command makes one request to Cloudflare Email Sending with redirects disabled. The invitation appears only in the intended email fragment; logs contain a fixed outcome and digest prefix. Provider `email_delivered` means recipient-server acceptance, not verified inbox receipt; `email_queued` is weaker. A rejected, timed-out or uncertain send leaves the active invite intact and does not retry. Inspect provider/quarantine evidence and the exact database row before any deliberate recovery, using the procedure below. Do not delete/recreate the continuing owner identity provider or move the owner to another provider without a reviewed subject-rebinding procedure.

If the first invitation link is lost, inspect the exact unaccepted `deandre` owner-invite row in the private staging database. An authorized operator may expire that exact digest while no users exist, verify one row changed, then rerun the script. Do not delete account rows, expire every invitation, or rerun after an uncertain SQL result without inspecting state. Ordinary completed account setup uses the normal password-reset flow.

Limiter upgrades must preserve active counters through an explicit reviewed migration. Unknown/corrupt persisted state fails closed; it is not silently reset. The v2 storage key was first deployed to the new production namespace on 21 September 2026. Later changes must preserve its active counters; do not treat it as an undeployed namespace.

## Protected desktop downloads

`DOWNLOADS_READY=verified` and a private `RELEASES` R2 binding enable `/api/downloads` and fixed GET/HEAD paths under `/api/updates/windows/x64/`. Current Access, app session and terms apply. `/api/me` reports `authentication_expires_at` as the earlier Access/session expiry for the native update lease. Read `../docs/DESKTOP-RELEASES.md` for object layout, scope and publication gates. The actual local workerd/R2 probe is recorded in `../docs/downloads-runtime-results.json`; installer GET uses a fixed-length stream, GET and HEAD preserve exact Content-Length, and exact bytes were verified. Full downloads only; range requests return416. Provisioning a release bucket does not activate downloads or publish a signed installer.
