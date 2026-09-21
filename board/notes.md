# Notes

Newest first. Format and rules: `board/README.md`.

## 2026-09-21 06:40 UTC · deandre-fable → deandre-gpt6, deandre, ali
COLLAB-20260921-11. M5 follow-ups M1 to M6 accepted on b3b1d7c. The
joint source checkpoint is closed: M1 accounts, M2 read API, M3 events,
M4 board adapter and M5 desktop are reviewed and in review status on
the board, with nothing deployed, merged, signed or published. What
remains is human: merge PR #1 and create main, hostnames, owner seed,
Access application and Turnstile keys, board token, server access,
signing. The coordination timer stays quiet until one of those changes.

## 2026-09-21 06:10 UTC · deandre-fable → deandre-gpt6, deandre, ali
COLLAB-20260921-10. M5 desktop source reviewed on 0e91a9e: passes with
seven non-blocking findings on T-011 (Content-Length via a fixed-length
stream, publisher pin is the certificate CN, verifier budget, internal
updater members, pinned actions, sign-in message, min-version gate
pending). T-011 in review, T-012 in progress. docs/API.md gains the
downloads and update routes; the plan's desktop section now matches.
Checkpoint for DeAndre: every remaining gate is human: PR #1 merge and
main, hostnames, owner seed run, Access application and Turnstile keys,
board token, server access, signing. Agents continue on findings only;
the timer stays quiet until one of those changes.

## 2026-09-21 05:30 UTC · deandre-fable → deandre-gpt6, deandre
COLLAB-20260921-09. Board adapter fixes B1 to B3 verified on c00c4c8;
T-030 stays in review until the staging round trip. M5 design accepted
with rules recorded on T-011 and T-012: same-origin feed through the
Worker, request-time cookie bridge limited to the exact feed path,
strict Authenticode with a thumbprint allowlist and re-check before
install, monotonic versions, forward-only rollback, minimum-version as
a gate not a revocation. Signing and hostnames stay human-gated.

## 2026-09-21 05:05 UTC · deandre-fable → deandre-gpt6, deandre
COLLAB-20260921-08. M4 board adapter (T-030) passes review with two
fixes before the staging round trip: note insertion position and the
board request body limit. M3 event transport reviewed, no findings.
Desktop plan settled on T-011: Electron loads the deployed web app in a
hardened window, so Access, Turnstile, cookies and CSRF work unchanged
and nothing secret ships. board/README.md documents the two fields the
app adds to log lines and the note marker line.

## 2026-09-21 04:40 UTC · deandre-fable → deandre-gpt6, deandre
COLLAB-20260921-07. G2 re-check on d452828 passes: F1, F2, F3, F6
resolved, F4 delivered as T-031 (in review, unrun), F5 handled in the
app. The ingress-before-Access order is confirmed. No code blockers
remain for staging; what remains is human-gated (Access application,
Turnstile keys, email sender, owner seed, hostnames) plus real
acceptance rows 1 to 6. T-022 moves to review. Two non-blocking nits
logged on T-022.

## 2026-09-21 04:10 UTC · deandre-fable → deandre-gpt6, deandre
COLLAB-20260921-06. G2 on the account Worker (PR #2 head 79ec1be)
passes conditionally: findings F1 to F10 on T-022, with F1 (shared IP
window between logins and writes), F2 (reserve before Access check) and
F4 (no owner bootstrap, now T-031) required before any staging
activation. M4 board adapter approved as T-030 with a throwaway
board/staging branch for tests; the canonical board stays here until
T-017 lands on main. Nothing is deployed; no account exists.

## 2026-09-21 03:05 UTC · deandre-fable → deandre-gpt6, deandre, ali
COLLAB-20260921-04 milestone progress. PR #2 (draft, stacked on this
branch for review only) carries the Case Forge shell import (T-006 done,
T-023 in review), the read-only run API (T-025 in review) and the joint
plan and auth design (T-018 done; G1 fixes incorporated). G3 ran here:
the independent comparison passes with 0 failures; three boundary
findings (startup window on /instances, journal over 16 MiB, run count
over 500) and one contract correction (/runs always lists every run;
index moves to /runs/index) are on T-025. Owner email confirmed
privately (T-020). Still human-gated: PR #1 merge, main branch,
hostnames, server access, signing, Ali's real sign-in.

## 2026-09-21 02:20 UTC · deandre-fable → deandre-gpt6, deandre, ali
COLLAB-20260921-01 round 1. Protocol accepted: Fable is the sole board
writer and task-id allocator on claude/cool-sagan-7klcfy for this cycle;
Astra merges (never rebases) this branch into codex/platform-app before
every PR (T-026). Reserved T-022 to T-025 (M1 auth, shell, Access, run
API) with root owner deandre-gpt6; T-005/007/008/010/011/012 reassigned
to deandre-gpt6. Fable's independent lane now: T-027 (synthetic live
fixture) then T-028 (research JSON output). Electron signing decision
added to T-020. Board head after this note is the merge point.

## 2026-09-21 01:10 UTC · deandre-fable → all
Coordination pass with deandre-gpt6 applied. Corrections: Electron with
electron-builder and electron-updater replaces Tauri everywhere; the
Cloudflare Worker owns board writes via the GitHub API and the server run
API is read-only; app.clawdie.ai is decided, api. and updates. are
proposed (T-020). New tasks T-015 to T-021. T-003 now carries the exact
commands and is blocked on a machine that can reach Binance (T-002).
T-015 (synthetic fixture) is in progress here. Mailbox approach for
ali@clawdie.ai is in T-019.

## 2026-09-20 14:00 UTC · deandre-fable → all
Board opened. The trading bot (phases 1 to 23) is complete on the default
branch and is under review as PR #1:
https://github.com/PrompDev-Lubu-Lab/test1/pull/1 (T-001). The platform plan
(simulation explainer, dashboard, hosting on Cloudflare plus the server,
desktop app with auto-update, this board) is in `docs/PLATFORM_PLAN.md`.
Tasks T-001 to T-014 are the plan broken into work items; most are
unclaimed. `deandre` and `ali`: assign or claim from the Tasks list.
