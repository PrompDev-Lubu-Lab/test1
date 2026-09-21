# Collaboration journal

Record a unique round, sent time, acknowledgement, deliverable and resulting action. A timer wakeup alone is not completion. Never store secrets, private mailbox data or authentication codes here.

| Round | Direction | State | Evidence / action |
| --- | --- | --- | --- |
| COLLAB-20260921-01 | Astra to Fable | Sent, acknowledged, independent delivery received | Claude Message 28 sent the project/access inventory and proposed sole board writer. Message 29 accepted the protocol and delivered live fixtures at board head `142e8e0`. Fable verified `/home/user/test1`, scoped GitHub authority, no server/Cloudflare access. |
| COLLAB-20260921-01 | Integration | Complete, 2026-09-21 02:08 UTC | Merged `142e8e0` into isolated `codex/platform-app`; preserved existing Case Forge checkout. |
| COLLAB-20260921-02 | Astra to Fable | Sent, 2026-09-21 02:09 UTC; awaiting reply | Claude Message 30: Node LTS API, pinned MIT import, clarified `platform/docs/` ownership, requested T-028 execution, lossless journal timestamp contract and a stacked draft PR base. |

The private handoff contains the existing Fable task link and local operational resource inventory. Public board updates are written by Fable during this cycle; Astra sends task evidence and proposed titles, never allocates competing IDs.

| COLLAB-20260921-02 | Fable to Astra | Delivered, integrated | Board heads `a2be84a` then `780dea0` merged. T-028 persisted validation and T-029 ISO journal source fix; legacy NDJSON passthrough accepted. Stacked draft PR approved, polling required for atomic file replacement. |
| COLLAB-20260921-03 | Astra to Fable | Sent, reviewed | Public engineering plan at `62cd7bf` reviewed after operational metadata was removed from the unpublished commit. Full inventory remains task-private. |
| COLLAB-20260921-03 | Fable to Astra | G1 approved with incorporated fixes | `780dea0`: T-018/T-021 done; T-004/T-015/T-027 accepted as API inputs. Seven auth fixes incorporated; RFC 7914 used for SHA-256 vectors and two-step password change preserves one KDF per request. |

### COLLAB-20260921-04

Sent PR #2 at 269f9cf for independent G3 review. Fable compared fixture JSON values, every CSV cell and raw NDJSON: zero fidelity failures. He found startup without status, large journal pagination and research roots above 500 directories; canonical contract now keeps all summaries at /runs and sweep index at /runs/index. CSV cells stay strings. Board/source head dbf20ec merged. T006 done; T023/T025 remain in review. Owner invitation address confirmed privately. Authentication proof expiry reduced to 120 seconds. Linux source-hash CI exposed checkout line-ending conversion; import now hashes original Git blob bytes with conversion disabled.

### COLLAB-20260921-05

Delivered 155cf5c. Fable independently passed 12 API tests and comparisons on fixtures, starting instance and indexed 501-run roots. T-025 done; G3 signed off at board 3b03f1c, merged. He approved journal WebSocket `{kind,instance,bytes}` invalidation; implemented and covered by the large-journal regression. Linux and Windows platform CI both pass. Authentication core now 60 local tests with real signatures/SQLite; remote Images compatibility verified on synthetic PNG/WebP, preview stopped. G2 delivery is next; browser account UI is in progress.

### COLLAB-20260921-06

Fable reviewed authentication/avatars at `79ec1be` and returned conditional G2 approval at `982d72e`, merged. F1 separate write/auth limits, F2 verify Access before account charging, F3 report successful outcomes, F4 owner bootstrap, F5 Access renewal UX and F6 constant-time CSRF are addressed in the next review head. The revised Worker passes 76 tests; the account UI/static app passes 19 tests, 17 syntax checks and all 13 imported asset hashes. The owner script was tested with actual SQLite, and no real invitation was seeded. T-030 permits an isolated `board/staging` branch for the Git-backed board adapter; Fable remains canonical board writer until the mainline planning merge. T-031 tracks first-owner setup. No credentials, staging deployment or real accounts were activated.

### COLLAB-20260921-07

Fable independently rechecked `d452828` and returned no staging code blockers at board `aacd0ab`, merged. Ingress-before-Access, separated write/KDF/recovery allowances, completed outcomes, constant-time CSRF, operator bootstrap and full-navigation Access renewal accepted. T-022 and T-031 are in review pending real staging. All Ubuntu/Windows platform and bot CI checks pass at `d452828`. Fable's N2 lost-link recovery is documented. For N1 the implementation deliberately remains fail closed on unknown limiter state; a future version change must include an explicit reviewed migration rather than silently erase active security counters. No production limiter exists today.

### COLLAB-20260921-08 preparation

M3 freshness/event connection and M4 task/note editing are ready for Fable's independent review. The root/helper review fixed an armed handshake timeout, hard expiry during a pending database check, the close-reason classification and an origin close during authorization. Actual local workerd confirmed upgrade, delivered hint, revocation, read-only denial and clean closure; no deployed event claim. The full Worker 108-test suite passed before four added lifecycle regressions, and the resulting nine-event suite passed. App39 tests,23 syntax checks,13 imported hashes and static build pass. Wrangler dry bundle161.41KiB/39.54KiB gzip. Board branch/token activation, browser QA, real accounts/email and the real paper-stop test remain pending. No canonical board writes were made by the app.

Round08 sent to the existing Fable task after pushing d1d7579. The UI showed the message and Fable responding. Requested independent M3/M4 review and the concrete M5 desktop Access/Turnstile/Origin plan. Reply pending.

Round08 reply received: board5afb23d merged. M3 has no findings. Board B1 requires notes after the preamble; B2 requires capacity for old/new task text; B3 recommends16KiB instructions. Fixed with a board-only64KiB body cap,16KiB instruction cap, unchanged8KiB auth cap and a separate bounded operation digest (regressions exposed the token digest's2048-character limit).25 adapter+12 board UI tests and the signed-handler long-edit transport test pass. Fable updated the board schema for operation metadata. M5 agreed to hardened Electron loading the deployed HTTPS app; the earlier cloudflared desktop path is superseded. Stable npm registry pins are builder26.15.3/updater6.8.9; current v27 site docs describe features absent from those stable pins. Signature and transport enforcement are being prepared against pinned source.


### COLLAB-20260921-09 sent
Delivered board fixes at c00c4c8. Requested Fable review of same-origin protected update feed, per-request human cookie bridge, stable updater constraints and strict Windows signature verification. No signing or publication. M5 source implementation continues.

Round09 reply received and board54ec475 merged. Fable verified B1-B3 and accepted same-origin protected R2 delivery, fresh per-request cookies, current/next certificate allowlists and forward-only recovery. Mainline merge, real accounts and signing remain human gates. M5 now has a hardened HTTPS host, strict pinned updater transport/verifier, a fixed non-elevating installer launcher, protected R2 delivery, Downloads UI and a manually dispatched package-verification workflow. Local adversarial review fixed stale authorization cancellation, native elevation fallbacks and the independent checksum/signature snapshot race. Worker130 full tests plus the new authentication-expiry regression pass; app82 tests,39 syntax checks,13 source hashes and static build pass. The actual local workerd/R2 probe passes18 checks; chunked installer GET omits Content-Length while delivered bytes and HEAD size are exact. No browser/login/signed upgrade/deployment claim.

M5 preparation complete: full Windows NSIS development artifact111566442bytes, native NotSigned confirmed, required executable fuses and ASAR policy/version/private-file exclusion verified. It was not installed or published. Evidence: platform/docs/desktop-review-results.json. Wrangler4.135 local dry bundle173.32KiB/42.42KiB gzip. Account preflight now bounds the native authorization receipt by the earlier signed Access/app-session expiry and20seconds, covered by its signed-handler regression. Awaiting final Fable review on the new M5 head.
