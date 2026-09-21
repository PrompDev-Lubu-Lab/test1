# Acceptance evidence

Cycle `COLLAB-20260921`. Status must be Pass, Fail or Pending with the environment and evidence. Local fixtures, local signed-token tests, deployed endpoints and real human workflows are separate evidence. No real-market profitability claim is made.

| # | Required outcome | Current state | Evidence needed / owner |
| --- | --- | --- | --- |
| 1 | Invite Ali; unverified login rejected; verification single-use and expiring | Pending | Local signed-token/SQLite flow and single-use/expiry tests pass. Real invite delivery and Ali sign-in pending. |
| 2 | Ten wrong passwords lock login and record audit events | Pending | Local actual-hash login lockout plus concurrent DO storage-harness tests pass; deployed integration pending. |
| 3 | Member cannot call owner-only APIs directly | Pending | Local signed Access member requests denied on enabled owner routes; G2 peer re-check passed; deployed checks pending. |
| 4 | Password reset revokes old sessions | Pending | Local reset, replay, stale-proof race and all-session revocation tests pass. Real reset delivery pending. |
| 5 | Avatar rejects non-images/over 2 MB; output 128 px square | Pending | Actual Cloudflare decoder probe: 128 x 128 WebP and invalid/oversize rejection pass; local guarded R2/SQLite tests pass. Real private upload/display pending. |
| 6 | Terms block access and save version/time | Pending | Local handler/SQLite terms gate and immutable version/time receipt pass. Actual browser+D1 flow pending. |
| 7 | Runs UI exactly matches summary/metrics fixtures | Pending | Fable independent JSON/CSV/NDJSON comparison: zero failures;12 API tests pass. Browser visual comparison pending (Chrome blocked local preview). |
| 8 | Stopped heartbeat becomes stale within 45 seconds | Pending | Local atomic replacement, monotonic 45-second stale badge, actual workerd event/revocation probe and expiry regressions pass. Real paper-container stop pending server access. |
| 9 | Board app commit visible in git, agent git update visible in app | Pending | Local adapter/handler/UI conflict, attribution, revocation and idempotency tests pass. Real authorized staging round trip pending; Astra/Fable |
| 10 | Actual installed desktop updates from v0.1.0 to v0.1.1 | Pending | Signed feed/artifacts, installation, update, relaunch and version evidence per supported platform; Astra + user |
| 11 | Direct outside connection to origin API port fails | Pending | Real server firewall/container/Tunnel evidence and external denial; authorized server operator |

Foundation evidence: API 12 tests and independent G3 sign-off. M3/M4 additions: 23 adapter tests, signed-JWT/SQLite board/event route tests, 9 event lifecycle tests and the actual local workerd probe. The Worker full 108-test suite passed before four additional expiry regressions; all nine event tests then passed. App: 39 tests, 23 syntax checks and 13 imported Git-blob hashes pass; static build passes. Wrangler 4.135.0 dry-run is 161.41 KiB, 39.54 KiB gzip. All Ubuntu/Windows platform and existing bot CI passed at d452828; new delivery CI is reported separately. No platform/account/board deployment occurred. Browser visual checks remain unverified because Chrome blocked the local preview. Test commands are in platform/api/README.md, platform/app/package.json and platform/worker/README.md. No milestone is complete until its applicable rows pass or the user explicitly changes scope.
