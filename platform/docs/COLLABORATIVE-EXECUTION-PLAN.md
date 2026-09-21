# Collaborative platform execution plan

Public engineering edition. Exact operational locations, deployed resource identifiers and pending identity setup are kept only in the private handoff.

Prepared jointly by Astra (`deandre-gpt6`) and Fable (`deandre-fable`), cycle `COLLAB-20260921`. This is a working plan with evidence, dependencies and owners. Read `coordination-state.json`, `communication-log.md` and `ACCEPTANCE.md` for current execution state. An open checkbox is not a delivered feature.

## 1. Intended result and boundaries

Build the invite-only web and Electron companion to the existing trading bot, using the Case Forge shell and scene. The product has Overview, Live, Runs, Research, Tasks, Notes, Downloads and Settings. The bot remains the only trading engine and the only writer of run data. The platform reads its files and lets approved people collaborate through the GitHub-backed board.

DeAndre requested a collaborative plan, repeated agent messages, a timer and execution in this task. The attached brief supplies requirements; quoted recommendations in it do not approve purchases, merges, bot deployment or live trading. No agent changes exchange credentials, risk limits or live-mode configuration. Two weeks of paper trading remains a real elapsed-time gate.

The frontend hostname is selected privately; API and update hostnames remain proposed until confirmed. The display name is one `APP_NAME` constant with the supplied placeholder; Cloudflare resource names and Electron application IDs use valid separate slugs. The member invitation address is recorded privately. The owner seed email is confirmed privately; activation is pending.

## 2. Verified starting point

| Item | Evidence at this cycle | Implication |
| --- | --- | --- |
| Bot repository | `PrompDev-Lubu-Lab/test1`, public | Source, board and non-sensitive design notes are public. Do not commit operational secrets or personal data. |
| Bot PR | [PR #1](https://github.com/PrompDev-Lubu-Lab/test1/pull/1), open, mergeable, head `53d5b12f5e6227c2ca4eeae308bef328300553d0`, green gcc/clang Debug/Release and Docker checks | Tests pass; this does not prove a real market run or authorize merging. |
| Branches | Default `claude/crypto-trading-bot-arch-p8iygb`; `baseline`; planning `claude/cool-sagan-7klcfy`; no `main` yet | Keep platform review diff separate; retarget only after the human mainline decision. |
| Platform checkout | Isolated branch `codex/platform-app`, planning/board head `142e8e0` merged | Original Case Forge checkout is preserved. |
| Case Forge source | Public MIT repository `CaseForgeHq/family-court-strategist`, clean snapshot `c8bea2a9fe3ad3a1702f7915f2712a604cc57cc9` | Electron, plain HTML/CSS/JS; import selected presentation assets with licences and a hash manifest. |
| Fixtures | T-015 two synthetic backtests; T-027 live-surface files emitted by the bot's writers | Enables contract/UI development. No synthetic result may be described as market performance. |
| Real backtest | T-003 blocked on Fable's Binance network access | Needs the agreed collector/backtest commands on an authorized machine. |
| Account readiness | Real member sign-in and verification remain unverified | Continue local work; human identity setup is a separate gate. |

## 3. People, agents and access

| Actor | Scope | Review boundary |
| --- | --- | --- |
| Human owner | Deployment/merge/signing decisions, live/shadow go/no-go | Uses existing authorized accounts; never shares passwords in messages |
| Astra | Platform code, app CI, account/access architecture and packaging | Existing project authority; no C++ or bot run-data writes |
| Fable | C++, bot docs, fixtures and board protocol; sole board writer for this cycle | Scoped repository access; does not need Cloudflare administration or server keys |
| Invited member | App member role and permitted board collaboration after verified sign-in | No platform administration by default |
| Local helpers | Bounded disjoint source paths under Astra's board handle | No separate board IDs or production credentials |

Repository access, Cloudflare administration, Access sign-in and app role are separate states. Use existing identities. Any future service identity requires a defined audience, lifetime and least-privilege scope; do not embed service credentials in the desktop app. The private handoff records actual machine/account/session locations.

## 4. Locations and source ownership

| Logical location | Purpose / owner |
| --- | --- |
| Repository root `docs/PLATFORM_PLAN.md`, `docs/API.md`, `CLAUDE.md` | Fable's canonical bot/platform-contract documentation; received by merge |
| `board/README.md`, `board/tasks.json`, `board/notes.md` | Shared protocol, tasks and append-only notes; Fable writes during this cycle |
| `platform/docs/` | Astra's design, joint execution plan, acceptance evidence and coordination state |
| `platform/app/` | Shared web presentation, pinned Case Forge asset provenance, minimal Electron host |
| `platform/api/` | Read-only Node API; run/instance file adapter and contract tests |
| `platform/worker/` | Account/role/terms/profile/board/proxy code, D1 migrations, bounded compatibility probes |
| `platform/fixtures/synthetic/` | Fable's generated files and reproduction code; never edited by the app |
| `platform/deploy/` | App API container and tunnel examples, no production server values or bot rewrites |
| `.github/workflows/platform-ci.yml` | App checks. Existing C++ CI remains intact. |
| Task-private sibling `astra-fable-handoff.md` and `operations-inventory.md` | Full absolute Windows folders, account/zone/resource IDs, current browser task and operational pending actions; kept outside the public repo |

The public source is [test1](https://github.com/PrompDev-Lubu-Lab/test1). Fable's cloud path is `/home/user/test1`. Absolute operator paths are recorded privately so public source remains portable. The real bot server host, SSH alias, working directory, volume paths and backup destination are **unknown**; examples are explicitly proposed and must not be presented as deployed locations.

Local development uses loopback only: app `127.0.0.1:8787`, read API `127.0.0.1:8788`, temporary authenticated Worker probe `127.0.0.1:8790`. All synthetic previews are marked in the UI. An account gate remains closed outside the explicit development mode.

## 5. Cloudflare layout and traffic

Browser → Access → Pages frontend → same-origin `/api/` Worker → account/board services or protected Tunnel → read-only Node adapter → bot files.

The public design uses `app.example.test`, optional `api.example.test`, and optional `updates.example.test` as placeholders. The selected domain and deployed inventory are private operator configuration. Host-only cookies and CSRF checks use the same-origin API route. Verify Pages/Worker route precedence in staging and protect default/preview hostnames as well as the production hostname. Disable unneeded public Worker URLs; JWT enforcement still rejects bypasses.

| Resource | Responsibility | Rollout condition |
| --- | --- | --- |
| Access application | Exactly two verified human identities; proposed 24-hour session | Confirm identity binding and login method; separate from other applications |
| Pages frontend | Shared static web UI | Preview/prod separation and protected hostnames |
| Worker | Auth, profiles, terms, board commits, read API proxy | Deny when issuer/audience/approved origins or required bindings are absent |
| D1 | Accounts, invites, sessions, receipts and audit | Separate preview/prod databases and reviewed migrations |
| Private R2 | Avatars and authorized release assets | Public bucket access disabled; serve through authorized routes |
| Turnstile | Signup/login/reset abuse controls | Exact configured hostnames/actions, test keys only in tests |
| Transactional email binding | Verification/reset mail | Inspect available sender capability; real delivery is a separate test |
| Tunnel | Outbound connector to read-only API container | Confirm server host, deployment authority and firewall; no published API port |

Secrets are stored only in Cloudflare/GitHub Secrets. Use named bindings for Access issuer/audience, Turnstile, scoped GitHub board writes, origin service authorization and platform-specific release signing. Values, resource IDs, account identities and unrelated mailbox routing do not belong in this public plan. Existing unrelated DNS/mail services remain outside platform changes.

## 6. Data and permission design

The Node adapter exposes only documented GET/HEAD run and instance routes plus a read-only event stream. It opens approved filenames under configured roots, rejects traversal/symlinks and unsupported methods, bounds file sizes/rows/subscriptions, and uses container read-only mounts. No order endpoint, arbitrary command runner, filesystem writer or exchange SDK exists in the platform.

Exact decimal strings pass through unchanged. Raw JSON transport avoids rounding nanosecond journal numbers; the UI must use lossless parsing or display the original token. Plots may convert values only to visual coordinates, with exact original values in tables/tooltips. Never synthesize research metrics, strategy outcomes or a healthy live status when a file is absent. Show unavailable/stale with the actual source and timestamp.

The Worker validates Access JWT signature, issuer, audience, expiry and intended human identity before app authentication. Owner/member roles come from current D1 state on each request, never from client payloads. An unverified account cannot log in. Terms version/time is recorded before data access. Session/reset/token/rate/audit details are in `AUTH-DESIGN.md`; that design must pass Fable's review before account code (G1).

Board commits happen through the Worker and GitHub API. The API token is limited to this repository and necessary content; fixed branch/file allowlists prohibit arbitrary git writes. Resolve the actor from the verified session. Fetch expected blob/commit SHA, validate roster/schema/task transition and append-only log, then commit atomically. On a conflict, return 409 with current safe state and refresh; never force-push or silently replace another update. Serialize ID allocation. Until the canonical board moves to main, use the agreed planning branch deliberately. Bot availability is not a dependency for board and accounts.

## 7. Messages, timer and integration protocol

The active timer is the existing task's **Astra and Fable platform collaboration** heartbeat, every five minutes. It continues this task; it does not create more tasks. Active work may exchange messages sooner when a deliverable or review decision is ready.

1. Read `coordination-state.json` and the private handoff; check the current board and branch heads.
2. Read the existing Fable task. Preserve user drafts. If Fable is implementing, let him finish instead of interrupting for status.
3. Process each response once using round ID + message marker + commit SHA. Record `sent`, `acknowledged`, `delivered`, `reviewed`, `integrated` separately.
4. Send one actionable message containing round, task IDs, branch/head, evidence, explicit next action and exact code/doc location. No secrets; no acknowledgement-only loops.
5. Fable allocates task IDs and writes `board/` on `claude/cool-sagan-7klcfy`. Astra proposes task titles and supplies evidence. All other board actors wait or coordinate before touching that branch.
6. Astra fetches and merges the quoted board head into `codex/platform-app` at least once each working session and before each PR. Never rebase/force-push shared work. Review `git status` before edits and merge; resolve meaningful conflicts with the owning agent.
7. Validate the next unblocked deliverable, record a commit/test result, request the relevant peer review, then continue.
8. Notify DeAndre only for meaningful progress, failure, completion or an actual required human action. Pause the heartbeat at completion, or after three unchanged cycles where only human actions remain and there is no independent work. Record pending actions once.

Local compute review recommended two active local agents. Run one bounded helper alongside Astra and keep heavyweight builds sequential. Fable's sandbox can run its independent C++ checks. Additional agents do not justify duplicated file ownership or extra production credentials.

## 8. Milestones and dependency order

| Milestone | Tasks / owner | Concrete output | Review and end condition |
| --- | --- | --- | --- |
| M1: design, accounts, shell | T-018/021/022/023/024, Astra; Fable reviews security | Joint plan, measured compatible password hashing, D1/Worker accounts, three-step Account/Connect/Terms wizard, Case Forge shell/scene/footer, eight tabs, protected two-person staging | G1 design then G2 auth/session review; tests 1–6; PR + board note + peer message. Missing real identity/sign-in is reported pending. |
| M2: run API and Runs | T-004/005/015/025/027, shared contract; Astra implementation | Node read-only file service, synthetic fixture fidelity, exact summary/equity/fill/order/metric views, tunnel deployment recipe | G3 Fable verifies keys/decimal strings/journal precision. Test 7. Real data separately depends on T-003. PR/note/message. |
| M3: Live and Research | T-007/028, Astra UI/API; Fable bot JSON writers | Status/feed/state/journal stream, stale heartbeat detection, persisted research results when present | Test 8 local controlled clock + later actual paper-container stop; no guessed research outputs. PR/note/message. |
| M4: Tasks and Notes | T-008, Astra; Fable protocol review | Worker GitHub-backed board, role-controlled writes, filters/logs, conflict handling and agent messages | Test 9 both app-to-git and git-to-app with concurrent conflict test. PR/note/message. |
| M5: Downloads, desktop and updates | T-010/011/012, Astra; human signing decisions | Protected web deployment, shared Electron build, Windows/macOS/Linux pipeline, verified artifacts/update feed | Tests 10–11 and remaining real acceptance. PR/note/message. Unsigned dev builds clearly labelled. |

Immediate work in this collaborative cycle: integrate Fable's fixtures/board; deliver this plan/design; resolve the hashing compatibility gate; implement and test local shell/API; have Fable deliver/review research contracts; open reviewable scoped draft PRs. Continue through all unblocked milestones. Do not mark an entire milestone done because its fixture preview works.

The bot collector (T-002), real backtest (T-003), PR #1 merge (T-001), creation/default of main (T-016), plan/board PR (T-017), paper run (T-013) and shadow decision (T-014) retain their owners and independent gates. When a task's board dependency is still `review`, ask Fable to approve it or record a separate independent implementation task; do not falsify dependency completion.

## 9. Pull requests, CI, release and rollback

Until main exists, propose a draft platform PR with planning branch as its base, so reviewers see app changes without repeating the bot PR. Fable must acknowledge that stacking arrangement. Record the exact base/head and pending mainline dependency. After T-017/mainline integration, retarget with a verified diff and green checks. Human performs merges. Each milestone retains its own review/evidence checkpoint even if later development is stacked.

CI uses pinned lockfiles and an explicit Node LTS matrix; local Windows Node may differ and must be reported. Test API contracts/path boundaries/read-only behavior; Worker security decisions; build static assets; validate source/notice manifest and Electron config. Existing four-way C++ checks plus Docker remain required for final mainline PRs. A unit mock is not a live auth, mail, tunnel or installer test.

Electron keeps context isolation and sandboxing, disables renderer Node integration, restricts navigation/external links/IPC, and stores session tokens through supported OS credential storage only. Reject insecure plaintext fallbacks. Fable round08 revised desktop login to a hardened BrowserWindow loading the approved HTTPS web app, with normal human Access/Turnstile/Origin and browser sessions; no cloudflared token adapter, custom PKCE endpoint or shared service token. Private update downloads must support a documented per-session request transport without secrets in manifest URLs.

Electron-builder/updater uses its native platform manifests (for example Windows `latest.yml`); any catalogue JSON is separate metadata. Windows certificate signing, macOS Developer ID/notarization and Linux artifact checks are different mechanisms. A single invented cross-platform signing key is insufficient. No paid membership/certificate purchase is authorized. Actual v0.1.0 → v0.1.1 update, signature verification and relaunch are M5 evidence, not inferred from a successful build.

Rollback: keep previous Worker version and Pages deployment; apply forward-compatible D1 migrations and back up before destructive schema changes; retain versioned release artifacts/manifests; disable a faulty Access allow policy or pause deployment without changing domain-wide mail/DNS; restore a board change through an ordinary reviewed commit. Never roll back by deleting run data, changing bot configuration or force-pushing shared history.

## 10. Remaining human inputs

Complete the pending human authentication step recorded privately. The platform owner email is confirmed privately. Confirm API/update hostnames; provide the authorized server SSH alias/host and working directory if agent deployment is desired; choose production signing options when a build is ready. The plan can advance on fixtures and local builds while these are pending. No passwords or secret values should be pasted into the chat.

Read `ACCEPTANCE.md` for the eleven individual completion gates and `operations-inventory.md` in the private parent folder for exact operational locations.
