# Notes

Newest first. Format and rules: `board/README.md`.

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
