# Read-only event connection

The bot-origin `/events` contract remains unchanged. The authenticated Worker connects to that fixed origin with separate service credentials, and the browser opens only same-origin `wss://<app-host>/api/events` with its existing cookies. `EVENTS_READY=verified` is required in addition to the protected origin settings. Local synthetic preview does not open an authenticated event socket.

The Worker validates Access, app session, terms, exact Origin and a connection reservation before the upgrade. It rechecks the session after the origin handshake and before returning 101. The ten-second handshake timeout is canceled after the response; leaving it armed was found to terminate an otherwise healthy socket in workerd. The accepted upstream must still be OPEN after authorization.

A burst produces at most one `{"kind":"refresh"}` notice per second, at most 300 notices over a five-minute connection. No raw status, identity, decimal value or legacy journal integer crosses this notice channel. The app coalesces notices into at most one data refresh per five seconds and reads the source again over authenticated HTTP. The Worker has no documented bufferedAmount control; the fixed, small output and lifetime bound the amount it sends. The runtime's inbound message limit is not claimed to be reduced by this application.

Every 15 seconds a bounded five-second authorization check tests the app session and terms again. A hard timer closes the socket independently of pending database work. Access expiry closes with 4403, app-session expiry/revocation with 4401, normal five-minute rotation with 4408, and an unavailable check with 1011. Client data frames close with 1008 and are never forwarded. Binary origin frames close with 1003. Closing either side cleans up both sides and timers.

The browser stops on sign-out, account boundaries and hidden tabs. Reconnects use bounded exponential backoff and first recheck the HTTP app session. Access denial offers full navigation to renew the outer login; app-session expiry goes to sign-in. The client never places tokens in URLs or stores service credentials. A delayed session read cannot restore local account state after it has been cleared.

Instance reads also poll every five seconds in visible Overview/Live views. Heartbeat age advances locally using a monotonic clock even if subsequent requests fail; at 45 seconds the badge becomes stale. Synthetic snapshots remain explicitly frozen. This local aging behavior is covered by tests; stopping a real paper container remains acceptance test 8.

For a static deployment, build with the exact approved HTTPS `APP_ORIGIN` environment value. The build adds only that host's WSS origin to the generated Content Security Policy, covering browsers that do not include WebSockets in connect-src self. The checked-in template remains portable and contains no production domain.

## Runtime evidence

`events-runtime-results.json` records an actual local workerd upgrade, delivered notice, session-revocation close, read-only client denial and normal close. The probe uses two synthetic Workers with an in-process service binding; no production Access/D1/origin or deployed event route is involved. Run `node scripts/probe-events-runtime.mjs` from the Worker package with `PROBE_RUNTIME_ROOT` set to an explicitly installed pinned Miniflare/esbuild toolchain. The local probe entry point must never be deployed. Nine targeted tests also cover bounded authorization, an upstream close during authorization and hard expiry during a pending check.

Reference: [Cloudflare WebSockets runtime](https://developers.cloudflare.com/workers/runtime-apis/websockets/).
