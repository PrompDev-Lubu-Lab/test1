# Acceptance evidence

Cycle `COLLAB-20260921`. Status must be Pass, Fail or Pending with the environment and evidence. Local fixtures, local signed-token tests, deployed endpoints and real human workflows are separate evidence. No real-market profitability claim is made.

| # | Required outcome | Current state | Evidence needed / owner |
| --- | --- | --- | --- |
| 1 | Invite Ali; unverified login rejected; verification single-use and expiring | Pending | Local signed-token/SQLite flow and single-use/expiry tests pass. Real invite delivery and Ali sign-in pending. |
| 2 | Ten wrong passwords lock login and record audit events | Pending | Local actual-hash login lockout plus concurrent DO storage-harness tests pass; deployed integration pending. |
| 3 | Member cannot call owner-only APIs directly | Pending | Local signed Access member requests denied on enabled owner routes; G2 peer review and deployed checks pending. |
| 4 | Password reset revokes old sessions | Pending | Local reset, replay, stale-proof race and all-session revocation tests pass. Real reset delivery pending. |
| 5 | Avatar rejects non-images/over 2 MB; output 128 px square | Pending | Actual Cloudflare decoder probe: 128 x 128 WebP and invalid/oversize rejection pass; local guarded R2/SQLite tests pass. Real private upload/display pending. |
| 6 | Terms block access and save version/time | Pending | Local handler/SQLite terms gate and immutable version/time receipt pass. Actual browser+D1 flow pending. |
| 7 | Runs UI exactly matches summary/metrics fixtures | Pending | Fable independent JSON/CSV/NDJSON comparison: zero failures;12 API tests pass. Browser visual comparison pending (Chrome blocked local preview). |
| 8 | Stopped heartbeat becomes stale within 45 seconds | Pending | Local atomic heartbeat replacement and45-second stale threshold pass. Real paper-container stop pending server access. |
| 9 | Board app commit visible in git, agent git update visible in app | Pending | Real authorized round trip and expected-SHA conflict test; Astra/Fable |
| 10 | Actual installed desktop updates from v0.1.0 to v0.1.1 | Pending | Signed feed/artifacts, installation, update, relaunch and version evidence per supported platform; Astra + user |
| 11 | Direct outside connection to origin API port fails | Pending | Real server firewall/container/Tunnel evidence and external denial; authorized server operator |

Foundation evidence: API 12 tests, app 6 tests, 13 syntax/source-hash checks, original Git blob provenance, static build; both Ubuntu and Windows platform CI pass at 155cf5c. Worker 60 tests pass, including final cleanup/session cap and avatar code. Wrangler 4.135.0 account Worker dry-run passed (131.13 KiB including avatar code). No platform or account deployment occurred. Test commands are in platform/api/README.md, platform/app/package.json and platform/worker/README.md. No milestone is complete until its applicable rows pass or the user explicitly changes scope.
