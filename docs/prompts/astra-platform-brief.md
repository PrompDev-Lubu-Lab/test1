# Brief for GPT-6 Astra Max: build the Tradebot app for two people

You are `deandre-gpt6` on this project's roster. You built Case Forge, and
this app reuses Case Forge's design, shell and setup flow. You have
Cloudflare, GitHub and MCP access. Read this whole brief before doing
anything, then ask the three questions at the end, then work through the
milestones in order.

## 1. What this is

Repository: `PrompDev-Lubu-Lab/test1`. It contains `tradebot`, a C++
cryptocurrency trading research system (ETHUSDT on Binance). It runs as
Docker services on a server: a market-data collector, a paper trader, and
later shadow, testnet and live. It writes its results as files
(`runs/<name>/equity.csv`, `fills.csv`, `orders.csv`, `summary.json`,
`status.json`, `heartbeat`, `journal.jsonl`). It has no user interface.

You are building the interface: a desktop app and web app called
**Tradebot**, for exactly two people, DeAndre and Ali, hosted on the
Clawdies domain behind Cloudflare, with accounts, email verification,
roles, profile pictures, a task board for humans and agents, and a
download-and-auto-update flow like Case Forge has.

Read these files first, in this order. They are the source of truth and
this brief does not repeat them:

1. `docs/PLATFORM_PLAN.md` on branch `claude/cool-sagan-7klcfy`: the
   simulation explainer, the tabs and charts, the run API contract, the
   hosting architecture, the desktop app and release flow, the board.
2. `board/README.md`, `board/tasks.json`, `board/notes.md` on the same
   branch: the roster, task schema and the protocol you must follow.
3. `docs/ARCHITECTURE.md`, `README.md`, `docs/RUNBOOK.md`,
   `docker-compose.yml`, `Dockerfile` on the default branch: what the bot
   is and how it runs.
4. Pull request #1 (`https://github.com/PrompDev-Lubu-Lab/test1/pull/1`):
   the bot itself, under review. Do not modify anything in it.

## 2. Who does what

| Actor           | Owns                                                              |
|-----------------|-------------------------------------------------------------------|
| `deandre-gpt6` (you) | everything under `platform/`, Cloudflare configuration, GitHub Actions for the app, the desktop build and release flow |
| `deandre-fable` (Claude) | the C++ bot under `src/`, `tools/`, `tests/`, its docs, and the board's protocol |
| `deandre`, `ali` | decisions, accounts, secrets, server access, merges              |

If you need something from the bot (a field it does not write, a new
CLI flag, a compose change), do not change the C++. Create a task on the
board assigned to `deandre-fable` with exact field names and why, post a
note, and continue with what does not depend on it. You may also message
the Claude session over MCP with the task id; keep the board as the
record either way.

## 3. Hard rules

These are not preferences. If a rule blocks a feature, stop and ask.

- **No trading logic in the app.** The app displays, it never decides.
- **The app never writes under `runs/` or `data/`** and never commits
  market data or run artifacts. Both are gitignored; keep it that way.
- **The app cannot enable live mode, edit risk limits, uncomment
  `confirm`, or hold exchange API keys.** Those are human edits to config
  files on the server. The app may show them read-only.
- **Secrets only in Cloudflare secrets, GitHub Actions secrets, or the
  server's env file.** Never in the repository, never in the app bundle,
  never in a log.
- **Invite-only.** Exactly two accounts can exist unless an owner adds an
  email to the allowlist. No open signup page.
- **Work on a branch.** Create `platform/app` from
  `claude/cool-sagan-7klcfy` (it carries the plan and the board). Open
  pull requests into `main` if it exists, otherwise `baseline`. Never
  push to the default branch directly. Never rewrite history on a branch
  you did not create.
- **Use the board.** Before starting, set your tasks to `in_progress`;
  when done, `review` with what was delivered. Post a note at the end of
  every working session. Board commits are separate from code commits.
- **Follow the repository's CI.** The C++ CI must stay green; your app
  gets its own workflow and must not slow the existing one.

## 4. What to build

### 4.1 Reuse Case Forge

Case Forge's shell is the app's shell: the top bar (logo, current-item
switcher, search, AI button, clock and weather, icon buttons), the tab
strip, the scene background with the Weather & scene control, the
three-step setup wizard, the terms step, the plan cards, the version
badge and "Reset app setup" in the footer. Keep its typography, colors,
spacing, components and build tooling. Change the content, not the
design. Where Case Forge says "case", Tradebot says "instance" (a
running bot: `paper-ma_1h`, `shadow-ma_1h`) or "run" (a finished
backtest).

Tab strip, in this order: **Overview · Live · Runs · Research · Tasks ·
Notes · Downloads · Settings**. Contents per tab are in
`docs/PLATFORM_PLAN.md` section 2. Charts use uPlot; everything else is
Case Forge's own components.

Setup wizard, three steps like Case Forge:

1. **Account**: sign in, or create your account from an invite.
2. **Connect**: the API base URL (`https://api.<domain>`, prefilled) and
   which roster handle this person is (`deandre` or `ali`; agents never
   sign in here).
3. **Terms**: plain-language terms for this app. Must state: nothing in
   the system is known to be profitable; the app shows results and never
   sends orders; live trading is enabled only by a person editing
   configuration on the server; the user's own risk. Accepted on this
   device, recorded server-side with a timestamp and terms version.

### 4.2 Accounts, login, permissions, profile picture

Replace Case Forge's PIN with real accounts. Cloudflare is the platform.

- **Storage**: Cloudflare D1 (users, invites, sessions, email
  verification tokens, password reset tokens, terms acceptances, avatars
  metadata, audit log). Avatars in R2.
- **API**: a Cloudflare Worker at `api.<domain>` that (a) serves auth and
  profile endpoints from D1, (b) proxies the run API on the server through
  the Cloudflare Tunnel, and (c) reads and writes the board through the
  GitHub API (commits to `board/` on the branch the app is configured
  for). Nothing else in the app talks to GitHub.
- **Signup**: invite-only. An owner adds an email to the allowlist (or
  the two emails are seeded). The invitee opens the app, enters that
  email, sets a password, receives a verification email with a one-time
  link (expires in 30 minutes), and cannot sign in until verified.
- **Verification and reset email**: Cloudflare does not send arbitrary
  outbound mail by itself. Use Cloudflare's Email Service send binding
  from the Worker if the account has it; otherwise call Resend (free
  tier) from the Worker with the API key as a Worker secret, sending from
  a subdomain of the Clawdies domain with SPF and DKIM set in Cloudflare
  DNS. Say which one you used in your report.
- **Passwords**: minimum 12 characters, checked against a breached-password
  list is optional, never stored in clear. Hash with PBKDF2-SHA256 at
  600k iterations via WebCrypto (Workers have no argon2); a per-user
  salt; upgrade the hash on next login if the parameters change.
- **Sessions**: HttpOnly, Secure, SameSite=Strict cookie for the web app;
  a bearer token stored in the OS keychain for the desktop app. Seven-day
  expiry, rotated on login, revocable from Settings ("Sign out
  everywhere").
- **Protection**: Cloudflare Turnstile on signup, login and reset. A
  Cloudflare rate-limiting rule on `/auth/*` (10 attempts per 10 minutes
  per IP, then a temporary lock). Every auth event in the audit log.
- **Perimeter**: Cloudflare Access in front of `api.<domain>` and
  `app.<domain>` as a second gate: email one-time-code for the two humans,
  a service token for each agent that needs the API. Access gets people
  to the door; the app's own accounts decide what they can do inside.
- **Roles**: `owner` (DeAndre) and `member` (Ali). Owner can invite,
  remove a user, change roles, change the API URL, and edit the
  allowlist. Member can do everything else. Nobody can change bot
  configuration from the app. Roles are checked in the Worker on every
  request, not only in the UI.
- **Profile**: display name, roster handle, email (change requires
  re-verification), password change (requires current password), a
  small profile picture. The picture is uploaded through the Worker,
  limited to 2 MB, resized server-side to 128 px square (use Cloudflare
  Images if available, otherwise a WASM resizer in the Worker), stored
  in R2, served from `api.<domain>/avatars/<id>` with caching. Shown in
  the top bar and next to the person's name on tasks and notes.

### 4.3 The run API on the server

Per `docs/PLATFORM_PLAN.md` section 3. A small service under
`platform/api/` (TypeScript on Bun is the recommendation) that mounts the
`runs` volume read-only, serves the endpoints listed there, watches
`heartbeat` and `status.json` and pushes changes over WebSocket. Field
names are the field names in the files; invent nothing. Add it to
`docker-compose.yml` as service `api` with `cloudflared` beside it. The
Worker at `api.<domain>` proxies to it through the tunnel; the service
itself is never exposed.

### 4.4 The board in the app

Tasks tab: table and kanban, filters by assignee, status, area; the
assignee picker is limited to the five roster handles; due-date picker;
per-task log; the assigner's and assignee's avatars where they are
humans, a fixed agent icon where they are agents. Notes tab: newest
first, recipient chips, task links, a compose box. Every write goes
through the Worker, which commits to `board/tasks.json` or
`board/notes.md` with the person's handle in the commit message. Git is
the store; the app is a view. Follow the merge rule in
`board/README.md`: never discard the other side of a board conflict.

### 4.5 Desktop app, downloads and updates

Case Forge's packaging and updater are the model; reuse the same
mechanism if Case Forge is Tauri or Electron, otherwise Tauri 2. Signed
auto-updater checking `https://updates.<domain>/latest.json`, private
signing key only in GitHub secrets. Builds for macOS (arm64 and x64),
Windows and Linux.

Release workflow on a `v*` tag: build installers, upload them to an R2
bucket served at `updates.<domain>`, write `latest.json`, create a GitHub
Release, append a note to `board/notes.md` with the download links, and
message the Claude session over MCP with the version and links. The
Downloads tab reads the same manifest and release list so a fresh
install is one click from inside the web app.

### 4.6 Web app

Same code, built for the browser, deployed to Cloudflare Pages at
`app.<domain>` from GitHub Actions on every push to `main`, with preview
deployments per pull request.

## 5. Milestones and order

Do them in order. Each milestone ends with a pull request, its board
tasks set to `review`, a note on the board, and an MCP message to the
Claude session with the PR link and what to test.

| Milestone | Deliverable                                                                 | Board tasks        |
|-----------|-----------------------------------------------------------------------------|--------------------|
| M1        | Worker + D1 + accounts: invite, signup, verification email, login, reset, roles, avatar, terms acceptance. Case Forge shell with the setup wizard and empty tabs, deployed to Pages behind Access. Two real accounts working. | new tasks you create |
| M2        | Run API service on the server behind the tunnel; Runs tab showing the fixture backtest (T-003's run directory) with equity, drawdown, fills, metrics grid. | T-004, T-005, T-006, T-007 |
| M3        | Overview and Live tabs on the running paper instance with WebSocket updates; Research tab from `runs/index.csv`, `drift.json`, `revalidation.txt`. | T-007, T-009, T-010 |
| M4        | Tasks and Notes tabs writing to `board/` through the Worker.                | T-008              |
| M5        | Desktop builds, signed updater, release workflow, Downloads tab; first tagged release with download links posted to the board. | T-011, T-012       |

Create the M1 tasks on the board yourself before starting (auth, email,
roles, avatar, shell, Pages deploy), assigned to `deandre-gpt6`, with
`assigned_by: deandre`.

## 6. Acceptance tests you must run and report

- Ali's invite: owner adds email; Ali signs up; unverified login is
  refused; verification link works once and expires; second use fails.
- Wrong password ten times triggers the lock; audit log shows it.
- Member cannot reach owner endpoints (test with the API directly, not
  only the UI).
- Password reset end to end; old sessions are revoked after reset.
- Avatar upload: 2 MB limit enforced, non-image rejected, result is
  128 px, visible in the top bar and on a task.
- Terms: cannot enter the workspace without accepting; acceptance
  recorded with version and time.
- Runs tab renders the fixture run with numbers that match its
  `summary.json` and `metrics.json` exactly (no rounding drift).
- Live tab shows heartbeat age changing; stopping the paper container
  turns the instance stale within 45 seconds.
- A task edited in the app appears as a commit on `board/tasks.json`
  with the person's handle; an agent's git edit appears in the app.
- Update flow: install v0.1.0, tag v0.1.1, the running app offers the
  update, installs it, relaunches on v0.1.1; the board has the note.
- Direct access to the server's API port from the internet fails.

## 7. Report format

At the end of each milestone, one note on the board and one MCP message
to the Claude session, both with: PR link, what was built, what was
tested (the list above, pass or fail), what is deployed where (URLs),
what you need from a human, and any board task you created for
`deandre-fable`. Keep it under 300 words. Do not paste code into notes.

## 8. Ask these before you start

1. Which hostnames on the Clawdies domain: the plan assumes
   `app.<domain>`, `api.<domain>`, `updates.<domain>`.
2. Where the Case Forge source lives (repository or folder) and whether
   it is Tauri or Electron.
3. Which email sender to use: Cloudflare Email Service if the account
   has it, otherwise Resend, and the two emails to seed as owner and
   member.

Then begin with M1.
