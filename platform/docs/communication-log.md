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

Round10 sent and visibly received in the existing Fable task: M5 head0e91a9e, concrete code/security review requested, all local/runtime/package evidence and remaining real gates supplied. Fable is responding.


### COLLAB-20260921-10 reviewed and integrated

Fable approved M5 at `0e91a9e` for staging with no blockers; canonical board/docs `0511a17` is merged. T-011 is in review and T-012 in progress. All platform, gcc/clang Debug/Release and Docker CI checks passed on that source head. M1 fixed the response to use native FixedLengthStream; the real workerd/R2 probe now requires and observes exact Content-Length on installer/manifest GET and HEAD while all18 checks still pass. M2 documents the signer simple/common name in the release runbook and settings README. M3 raises only the bounded signature process timeout to30seconds; its21 focused tests pass and the independent20second account receipt remains enforced. M4 adds an actual installed updater6.8.9 contract test for config/cache/file members. M5 pins both packaging jobs to official GitHub Actions commit SHAs. M6 distinguishes an unconfirmed sign-in from download/signature failure. The revised app passes84 tests plus39 syntax and13 imported-hash checks;13 focused Worker download tests pass. No signed build, installer launch, account seed or deployment took place. Minimum-version compatibility policy and other native platforms remain follow-up work; neither is a claim of server-side client revocation.


### COLLAB-20260921-11 accepted; joint source checkpoint closed

Round11 delivered `b3b1d7c` once in the existing Fable task. Fable independently rechecked the six exact follow-ups, accepted all and posted canonical board `041e59d`, now merged. M1 accounts, M2 read API, M3 events, M4 board and M5 Windows desktop source have peer-reviewed checkpoints. T-011/T-012 retain real activation/acceptance work; no full production milestone is claimed complete. Platform CI passed on Windows and Ubuntu at `b3b1d7c`; compiler/Docker checks were still running at the last read. All seven kinds of checks passed on preceding `0e91a9e`.

No more acknowledgement-only peer messages are needed. The existing five-minute heartbeat remains active and deduplicates this round; it should continue only on an actionable change and pause after three unchanged cycles when only human actions remain. Pending passkey handoff, optional hostname/server details, mainline merge, board credential and signing decisions remain recorded privately. Real Access/Turnstile/resources, seed, email, board and installed-upgrade checks are downstream of those gates. Minimum-version compatibility and other native platforms remain explicit later scope.


### COLLAB-20260921-12 and 13: single-origin deployment review

Fable accepted a single approved app origin for web/API/protected updates and recorded deployment sequencing. G4 reviewed `f462cc8` and passed the Worker-first static wrapper with15 focused tests. Both board checkpoints were merged. The source is deployed with private storage and optional features disabled. Worker146 tests and14 exact-head CI checks pass. Seven live unauthenticated paths redirect to Access; ten local workerd paths deny at the host gate, which is not independent JWT proof. Actual signed-in browser and installed desktop checks remain pending.

### COLLAB-20260921-14: deployed owner setup, in progress

Fable board `0e24e8c` records deployment separately from usability and accepts a continuing owner OTP identity subject to preserving its identity-provider configuration. The owner explicitly approved the exact-email Access rule and one setup email; the saved rule and real login page are verified. The proposed operator-only emailed bootstrap keeps the token in memory and the intended email only, validates the exact deployed mappings/sender, inserts before sending and never retries ambiguous delivery. Existing email-sending authority and settings were verified privately. Source review and actual email delivery are still pending; no invite or account activation is claimed here.

### COLLAB-20260921-14 accepted; round 15 review closed

Fable accepted all seven owner-bootstrap source conditions at `5d3d084` in board commit `536cfd5`. The 17 focused bootstrap tests passed. A read-only GitHub snapshot during this close-out confirmed all 14 checks completed successfully at the exact reviewed source `5d3d084f260856f62de7231ac0d4a8e659011244`. The operator bootstrap remains unrun. These source checks do not prove invitation receipt or human signup.

Round 15 at board `ced19e8` records the final evidence and closes the review rounds with no reply expected. Both board commits were merged into `codex/platform-app` as `c023f65`; the incoming diff changed only `board/notes.md` and `board/tasks.json`. No acknowledgement-only message was sent. Future coordination is limited to actionable recovery or CI changes under the existing heartbeat policy.

The deployed runtime remains source `f462cc8`, Worker version `e8516426-648e-4437-a0f7-983bb4fd7be8`. The approved exact owner Access rule is live; authenticated application use is still pending. The last reported read-only D1 check found zero users and zero invites. The approved owner invitation has not been sent while private mail delivery recovery remains open. Those live states were not rechecked during this documentation close-out. The Windows development package remains unsigned, uninstalled and unpublished. All eleven real-workflow acceptance rows remain Pending. No deployment, mainline merge or PR #1 merge occurred in this close-out.


### COLLAB-20260921-16 through 18: live bootstrap recovery and delivery

The earlier unrun snapshot is superseded. A first remote attempt stored an invite digest but stopped before sending because file-import results did not contain confirmation rows. Round 16 reviewed separate exact-digest confirmation at `199e086`. Its real attempt also stopped before any send because the installed Wrangler file importer prefixes JSON with progress output. Harmless read-only probes established the exact behavior; strict parsing was retained.

Round 17 passed `ea30244`: both the atomic guarded insert and separate exact-digest read use the command path; the child log level is pinned, both child environments omit the email token, and no temporary SQL file is created. All 23 focused tests pass, including actual subprocess argv/environment/parser tests. The default production executor independently passed two remote SELECT 1 queries using installed Wrangler. All 14 CI checks passed at the exact reviewed code commit.

Each unsent orphan was expired individually under full digest, identity, timestamp, unaccepted/unexpired and zero-users guards. Each recovery required exactly one changed row and a subsequent zero-users/zero-active-invites read. The reviewed CLI then confirmed one new invitation and made exactly one email request. Cloudflare reported queued; separate intended-mailbox metadata confirmed actual receipt at 2026-09-21 13:41:47 UTC. The invitation expires at 2026-09-22 13:41:27 UTC, 24 hours after creation. No invitation token or email body was printed or committed.

Round 18 recorded delivery at canonical board `49dae95`, now integrated, and closed the review. The final read-only account count is zero users, three total invitation rows, one active invitation and two expired unaccepted rows. Human Access PIN, app password, terms and signup remain pending, followed by authenticated browser acceptance. Member access, all eleven complete acceptance workflows and signed installed upgrade remain unverified. Runtime source remains `f462cc8`; the Windows development installer remains unsigned, uninstalled and unpublished. No further acknowledgement-only message or email retry is expected.


### COLLAB-20260921-19: real onboarding regression and repair

The owner confirmed they used the invitation email button. After Cloudflare Access the app showed the bare sign-in screen; a subsequent password attempt returned a generic security-service error. A read-only D1 audit recorded `turnstile_unavailable` and zero users. The delivered invitation remains active and unaccepted. This is an onboarding defect, recorded by Fable as T-034 at `eec4e79`; M1 returns to in progress.

The repair resolves a valid email-link token through a protected, rate-limited context endpoint, pre-fills its exact email as read-only and keeps the token and address only in page memory. The bare sign-in view explains how to reopen the original invitation after Access. The working display name is now Clawdie through the existing single constant; this is placeholder cleanup, not a claim that the final brand was approved. Bounded Turnstile diagnostics distinguish rejected tokens from provider/configuration failures while preserving authentication. The existing exact-widget secret was reapplied in the encrypted Worker binding at 13:57 UTC; active configuration version `12d50efe-c6c6-471c-a916-b583ecbc002e` is verified, but successful live authentication remains unproven. No underlying widget key was rotated.

The first complete repair passes 94 app tests and 173 Worker tests, plus 40 JavaScript syntax checks and 13 imported asset hashes. Fable's design conditions are being applied before final source review and deployment. No user password was entered or changed by an agent, and no invitation was resent or consumed. All eleven complete real-workflow acceptance rows remain Pending.

The review refinements are complete: link-context audit stores only a validated kind, outcome and safe reason after admission; rejected Turnstile tokens return a fresh-challenge 403, while configuration, outage and malformed responses remain safely categorized 503s. The full Worker suite now passes 176/176. The production-config dry build succeeds at 183.00 KiB (44.56 KiB gzip) with 35 static assets. Board-only `eec4e79` is integrated. The source awaits final peer review and has not been deployed.


### COLLAB-20260921-20: source passed; deployment next

Fable passed `b2021bd` with no blockers and recorded board `70607d0`. All 14 exact-source CI checks passed. The two small frontend follow-ups are also complete: every Turnstile503 category gets the friendly security-check availability message while token403 retains fresh-check guidance, and a scoped app-owned footer rule removes observed narrow-screen clipping. The 31 focused account/UI tests,40 syntax checks,13 imported hashes and static build pass. Local synthetic browser QA observed Clawdie in the title/header/footer, the read-only confirmed email and immediate fragment scrubbing. None of these checks claims successful production signup. The original delivered invitation is retained for the human retry after deployment.


### COLLAB-20260921-21: repair deployed; human retry and bot CI finding

Source `7051615` is deployed as Worker version `a5b1c69b-f46d-420f-a689-f5a2a42b5e59` at 2026-09-21 14:20:58 UTC and independently read back at100%. Both encrypted secret bindings remain; workers.dev and preview URLs remain disabled. Seven unauthenticated HTTP paths returned403, and a fresh real Chrome navigation reached the expected Cloudflare Access email-code login. This proves deployed protection, not authenticated app acceptance. D1 still has zero users and one active original invitation, with no new auth failure since the reported attempt. The user was asked to reopen that same invitation in their existing browser and complete password, verification, sign-in and terms. No invitation was resent or consumed by an agent.

Final source CI has13 passed and one failed gcc/Debug bot job; all four platform jobs passed. The failed assertion is `tests/gateway/binance_gateway_test.cpp:144`, expected cancelled status5 but observed2, in the venue-rejection/cancel/late-fill test. The job reports220 of221 cases passed. C++ was unchanged by the onboarding repair; timing is only an unconfirmed hypothesis. The exact job evidence was handed to Fable in round21/message36 for the C++ owner to record and investigate; no root C++ edit, silent retry or PR1 merge occurred. T-034 remains pending the actual user flow.


### Round21 completed: genuine gateway bug fixed and reviewed source integrated

At the scheduled14:41 UTC follow-up, Fable supplied fix `1b03b7b` and separate canonical board `f995276`. The failure was reproduced deterministically before the fix: late fills revived cancelled orders, and stale cancellation could overwrite a filled order. Terminal state is now preserved while late-fill quantities and fees still update. Two regression tests force both orderings. Fable reported warning-free full gcc/clang Debug sanitizer suites, gcc Release and a clang nonsanitizer fallback, plus30 gateway repeats under each sanitizer compiler. An independent GitHub read verified all five compiler/Docker checks passed at `f995276`.

Root reviewed the exact incoming five files (three Fable-authored C++/test files and two board files) and integrated them as `540045a` into the platform feature branch. No C++ source was edited in the platform lane, no PR1/mainline merge occurred and no bot was deployed. T-035 remains in review until it lands through T-017. Combined-branch CI follows this substantive source fix; the original failed7051615 result is retained as history, even though later documentation-only CI passed. Onboarding deployment remains7051615 and T-034 still requires the user's actual signup flow.


### Integrated source CI completed

All 14 checks at `fb78fcf0598e2002ff0f3b26c851b557990a7bee` completed successfully: four platform checks, eight compiler checks and two Docker checks across push/PR workflows. Verified through the exact-commit GitHub API at 2026-09-21T14:55:44.850080+00:00. This is the reviewed source integration result; the deployed app remains `7051615` and the production bot is unchanged. The original failing bot assertion remains documented above. Round22/message38 handed this evidence to the sole board writer. Fable completed it at message39 and canonical board `f27fe3f`, now reviewed and integrated. T-034 real signup and T-035 mainline landing remain pending. No peer action remains and no acknowledgement-only round is needed.
