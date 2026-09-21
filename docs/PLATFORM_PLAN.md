# Platform plan: simulation, dashboard, hosting, desktop app, board

This document is the plan for everything that sits around the trading
bot: how its simulation works (so the numbers it shows can be trusted),
how those numbers get visualized, how the system gets hosted on the
Clawdies domain behind Cloudflare, how a downloadable app with automatic
updates is produced, and how humans and agents coordinate the work
through the board in `board/`.

The bot itself (phases 1 to 23 in `docs/ARCHITECTURE.md`) is finished
and under review. Nothing here changes its code; the platform reads what
the bot already writes.

## 1. How the simulation works

A backtest is a replay of recorded market events through the same
strategy, risk and portfolio code that runs live, with a simulated
exchange deciding fills. The whole run is one function,
`run_backtest` in `src/tradebot/backtest/backtest.cpp`, and it wires
these pieces in this order:

```
 data/store/<venue>/<SYMBOL>/<day>/<kind>      (trades, book deltas, candles)
        │  StoreEventSource per kind, MergedEventSource across kinds
        ▼
 ReplayEngine  ── SimClock (time only moves forward, driven by events)
   │  venue bus: events at their own time ──────► SimulatedExchange
   │  client bus: events after market-data delay ► StrategyRunner, Portfolio
   ▼
 Strategy ──intent──► RiskManager ──approved──► SimulatedExchange
        ▲                                             │ execution reports
        └────────── Portfolio ◄───────────────────────┘ (also written to
                                                         the run directory)
```

### 1.1 Replay

Stored events (trades, order-book snapshots and deltas, candles) are
read from the event store and merged into one stream in strict time
order. Ties break by source, then by arrival order, so the same inputs
always produce the same sequence. Time is a simulated clock that only
the event stream advances; reading the wall clock anywhere in this path
is a bug by design.

Two buses leave the replay engine. The venue bus delivers each event at
its own timestamp; the simulated exchange listens there, because the
exchange knows the market instantly. The client bus delivers the same
event after a market-data delay; strategies and the portfolio listen
there, because that is what a real client would have seen. Timers fire
on the same simulated clock, aligned to interval boundaries.

### 1.2 Latency

A latency model supplies three delays: market data to the client, order
to the venue, and acknowledgement back. The default is jittered
(a base plus a uniform random extra, drawn from the run's one seeded
generator). A cancel that overtakes its own order is rejected with
"order not open", exactly as a venue would. Latency is configured in
`[exchange]` (`latency`, `latency_order`, `latency_ack`,
`latency_jitter`).

### 1.3 The simulated exchange

The exchange keeps its own copy of the order book from the venue-side
feed and decides every fill from it.

- **Market and marketable limit orders** walk the opposite side of the
  book level by level and fill at each level's price, paying taker
  fees. The consumed quantity is removed from the book until the feed's
  next update restores it, which approximates the transient impact of
  our own order.
- **Resting limit orders** join a first-in-first-out queue at their
  price. The quantity ahead of us is the book's quantity at that price
  when we arrive. A market trade through our price fills us; a trade at
  our price first consumes the queue ahead, then fills us. Book
  reductions shrink the queue ahead (treated as cancellations).
  Three queue models exist: optimistic (fill as soon as the price
  trades), queue (the default above), pessimistic (fill only when the
  market trades through the price). Resting fills pay maker fees.
- **When the book is unusable** (not yet synced), a fallback fills at
  the last trade price plus a configurable slippage in basis points,
  rounded against us to the tick. Results from this path are optimistic
  on liquidity and are flagged as such.
- **Venue rules** are checked on arrival: tick size, lot size, minimum
  quantity, minimum notional. Time in force covers GTC, IOC, FOK and
  post-only, each with the real rejection reasons.
- **Fees** are exact rationals in basis points (default 10 maker and
  10 taker) and round up, so the venue never rounds in our favour.
- **Self-trade prevention** is implicit: our resting orders are not in
  the venue's book, so our aggressive orders cannot hit them.

Not modelled: rate-limit rejections, venue outages, volume-based fee
tiers.

### 1.4 Risk and portfolio in the loop

The risk manager implements the same venue interface as the exchange
and wraps it, so the strategy cannot tell it is talking to the gate.
Every order passes pre-trade checks (order size and notional, price
deviation from the mark, position and position notional, open orders,
orders per minute, allowed instruments); a failed check produces a
rejection report identical in shape to a venue rejection. Portfolio
limits (max drawdown, daily loss) are checked on each sample; a breach
trips the kill switch, which cancels everything, optionally flattens,
and refuses new orders. Backtests therefore reflect what would really
have been allowed.

The portfolio is updated only from execution reports, never from what a
strategy believes. It keeps average-cost positions, realized and
unrealized P&L, fees, and per-strategy sub-ledgers, and it is sampled
onto the equity curve every `sample_interval` (default one minute).

### 1.5 What the strategy sees

A strategy receives `on_trade`, `on_book_update`, `on_book_ticker`,
`on_candle`, `on_execution_report` and timers, and can only act through
its context: read the book, its position, cash and equity, submit and
cancel orders, schedule timers, log and emit named metrics. It never
sees a socket, a clock implementation, a venue type or a file. The same
compiled strategy runs in backtest, paper, shadow, testnet and live.

### 1.6 Determinism

One seed (`backtest.seed`, default 1) feeds one random generator shared
by the replay engine and the exchange; each strategy gets its own
generator derived from that seed and its id. Money is fixed-point, not
floating point. The run id is derived from a hash of the full
specification (symbol, dates, fees, latency, limits, every strategy
parameter), so the same specification always lands in the same run
directory with the same numbers. Parallel sweeps run whole
specifications on separate threads with separate engines, so parallelism
does not perturb any single run. The only non-deterministic field in the
outputs is `wall_time_seconds`.

### 1.7 The four live modes

Paper, shadow, testnet and live differ only in what sits behind the
venue interface, and in the clock, which is the wall clock.

| Mode    | Venue behind the interface                     | Credentials | Real orders |
|---------|------------------------------------------------|-------------|-------------|
| paper   | simulated exchange fed by the live book         | no          | no          |
| shadow  | logs "SHADOW would send", then simulated venue  | read-only   | no          |
| testnet | Binance spot testnet gateway                    | testnet     | yes (fake)  |
| live    | Binance spot gateway                            | live        | yes         |

Live mode refuses to start unless the config carries the literal
confirmation sentence, `max_capital` is set and every risk limit fits
inside it, `resume` is on, and the account reconciles against the
portfolio at startup. Keys come only from the environment, never from
config files.

### 1.8 What a run writes

Every run directory, backtest or live, has the same six files, which is
what lets one dashboard show all modes.

| File           | Contents                                                                 |
|----------------|--------------------------------------------------------------------------|
| `config.txt`   | the run id and the full specification                                    |
| `equity.csv`   | `time,equity,cash,realized_pnl_net,unrealized_pnl,fees,position,mark` per sample |
| `fills.csv`    | `time,strategy,client_id,side,price,quantity,fee,liquidity`              |
| `orders.csv`   | `time,strategy,client_id,event,side,type,price,filled,remaining,reason`  |
| `metrics.csv`  | `time,strategy,name,value`, the strategy's own named telemetry           |
| `summary.json` | run id, dates, seed, strategies, initial and final equity, return, fees, drawdown, counts |

`tradebot-analyze` adds `metrics.json` (returns block: total and
annualized return, volatility, Sharpe, Sortino, Calmar, max drawdown
and duration, time in market; benchmark block; trades block: win rate,
profit factor, expectancy, average and largest win and loss, holding
time, turnover, fee drag), `round_trips.csv` and a human `report.txt`.

Live directories add the streaming surface, rewritten atomically on the
heartbeat cadence (default five seconds):

| File            | Contents                                                              |
|-----------------|-----------------------------------------------------------------------|
| `status.json`   | equity, cash, drawdown, position, mark, open orders, kill-switch and halt flags, feed state and silence, counters, gateway counters |
| `metrics.prom`  | the same as Prometheus gauges and counters, labelled by mode and label |
| `heartbeat`     | one line, `<time> <mode> <feed state> <armed|tripped>`                |
| `state.json`    | the portfolio, for resume after restart                               |
| `journal.jsonl` | every execution report, fsynced before it is applied; the audit trail |

Research adds `runs/index.csv` (one row per run with return, Sharpe,
drawdown, round trips, benchmark and parameters) and, when
`tradebot-monitor` runs, `drift.json` and `revalidation.txt` with a
`keep`, `watch` or `retire` decision.

Monetary values in JSON are decimal strings (exact); counts and
fractions are numbers. A dashboard renders the strings and never does
arithmetic on prices.

### 1.9 Research on top of the simulation

`tradebot-research` runs sweeps (a grid of parameters in parallel),
walk-forward windows (choose on a train window, score on the following
test window, roll forward), Monte Carlo resampling of round trips,
cost sensitivity (fees 0 to 40 bps, slippage 0 to 20 bps, extra latency
up to 500 ms), parameter-neighbourhood stability, a regime split by
realized volatility, and a backtest-versus-paper consistency check. The
`validate` command combines them into a go/no-go with six named checks:
kill criteria, Monte Carlo p05 return above zero, survives higher costs,
parameter neighbourhood, walk-forward out-of-sample, backtest vs paper.
Only a GO moves a strategy to paper.

## 2. Visualizing it

The bot already writes everything a dashboard needs; nothing in the C++
code changes for visualization. The rule is the same as the bot's own:
one artifact schema for backtest, paper, shadow and live, so one set of
screens shows all of them.

### 2.1 The app's tabs

The shell is the tab layout from DeAndre's existing app (the one with the
Downloads tab). Its design system is reused as is; only the content of
each tab is new.

| Tab       | Job                                                        | Reads                                            |
|-----------|------------------------------------------------------------|--------------------------------------------------|
| Overview  | every running instance at a glance: mode, strategy, heartbeat age, today's P&L, kill-switch state | `heartbeat`, `status.json`, `summary.json` |
| Live      | one instance in depth: equity, positions, open orders, risk-limit headroom, feed health, last fills | `status.json`, `state.json`, `journal.jsonl`, `fills.csv` |
| Runs      | any finished run (backtest or paper period): equity vs buy-and-hold, drawdown, fills on price, metrics grid, run config | run directory artifacts                          |
| Research  | sweeps, walk-forward, Monte Carlo, go/no-go verdicts, drift | `runs/index.csv`, `drift.json`, `revalidation.txt` |
| Tasks     | the board's task list (section 5)                          | `board/tasks.json`                               |
| Notes     | the board's message feed (section 5)                       | `board/notes.md`                                 |
| Downloads | installers for the desktop app, release notes, update channel | release manifest (section 4.4)                   |
| Settings  | API endpoint, identity (which roster handle you are), theme | local                                            |

### 2.2 The charts

- **Equity curve with benchmark.** Strategy equity and buy-and-hold on
  one axis, fees shown as a second faint line (equity before fees), so
  fee drag is visible rather than a number.
- **Underwater plot.** Drawdown from the running peak, filled below zero.
  Max drawdown and its duration are read straight off it.
- **Price with fills.** Mid price with buy and sell markers sized by
  quantity, and rejected orders as hollow markers. This is the chart that
  catches a broken strategy fastest.
- **Metrics grid.** Total and annualized return, Sharpe, Sortino, Calmar,
  max drawdown, win rate, profit factor, expectancy, turnover, fee drag,
  fill rate, average slippage. Same grid in every mode, so a backtest and
  its paper run sit side by side.
- **Risk headroom bars** (Live only). For each limit in `[risk]`: current
  value against the limit. Position, order notional, open orders, orders
  per minute, drawdown, daily loss.
- **Research views.** Parameter heatmaps (metric over two parameters),
  walk-forward bars (train vs test per window), Monte Carlo fan
  (bootstrapped equity band), and the go/no-go card with each criterion
  as pass or fail.

Time series use uPlot (small, fast, handles a million points). Everything
else is the design system's own components. Money is rendered from the
fixed-point strings the bot writes; the dashboard never does arithmetic on
prices.

### 2.3 Live updates

The Live and Overview tabs subscribe to a WebSocket from the run API
(section 3). The API watches the heartbeat and `state.json` and pushes a
message on change, so the app never polls files.

## 3. The run API

The bot writes files; the app reads HTTP. Between them sits one small
service, `tradebot-api`, that mounts the `runs` volume read-only and the
repository's `board/` directory read-write.

| Endpoint                          | Returns                                              |
|-----------------------------------|------------------------------------------------------|
| `GET /instances`                  | running instances with heartbeat age and mode        |
| `GET /instances/{id}/state`       | `state.json` as JSON                                 |
| `GET /runs`                       | `runs/index.csv` as JSON                             |
| `GET /runs/{id}`                  | `summary.json` plus the run config                   |
| `GET /runs/{id}/equity`           | equity curve rows                                    |
| `GET /runs/{id}/fills`            | fills rows                                           |
| `GET /runs/{id}/orders`           | order log rows                                       |
| `GET /research/...`               | research reports                                     |
| `WS /events`                      | heartbeat and state change notifications             |

The board endpoints live in the Cloudflare Worker, not on the server:

| Endpoint (Worker)                 | Returns                                              |
|-----------------------------------|------------------------------------------------------|
| `GET /board/tasks`, `PUT /board/tasks/{id}` | the task list; a `PUT` commits to `board/` through the GitHub API |
| `GET /board/notes`, `POST /board/notes`     | the message feed; a `POST` appends and commits the same way |

So the API has two halves. The **server run API** is read-only: it
mounts `runs/` and serves files as JSON. The **Cloudflare Worker** at
the API hostname owns accounts, sessions, roles, avatars, the board
(reads and writes through the GitHub API, committing with the person's
handle), and proxies the run API through the tunnel. Git stays the
board's store and keeps working when the server is down.

Field names in the API are the field names in the artifact files. The
API invents nothing; if a screen needs a number the bot does not write,
the bot gains a field first (a small C++ change with a test), then the
API exposes it. The API contains no trading logic and never writes under
`runs/`.

Implementation: TypeScript on Bun (single binary, built-in WebSocket and
file watching) is the recommendation; Python with FastAPI is the
fallback if that is what the existing app's backend uses. It runs as one
more service in `docker-compose.yml`.

## 4. Going live and hosted

### 4.1 Recommendation

Yes to Cloudflare, yes to the Clawdies domain, yes to the existing
server, with one boundary made explicit: **the bot runs on the server;
Cloudflare fronts it.** Cloudflare Workers cannot host the bot (it needs
a persistent process holding WebSocket connections to Binance for weeks,
and it is a native binary), so Cloudflare's role is network, identity,
static hosting and file distribution.

```
                  ┌────────────────────── Cloudflare ──────────────────────┐
  team ──HTTPS──► │ DNS  ·  Access (who may see it)  ·  Pages (web app)    │
  desktop app ──► │ R2 + updates.clawdie.ai (installers, latest.yml)       │
                  └───────────────────────┬────────────────────────────────┘
                                          │ Cloudflare Tunnel (outbound only)
                  ┌───────────────────────▼───────── the server ───────────┐
                  │ docker compose:                                        │
                  │   collector   paper   shadow   (later: testnet, live)  │
                  │   api  (reads runs/, serves the app, edits board/)     │
                  │   cloudflared                                          │
                  │ volumes: data (raw archive), runs (artifacts)          │
                  └────────────────────────────────────────────────────────┘
                                          │
                                   Binance (public feed; private API for shadow+)
```

Nothing on the server listens on a public port. `cloudflared` opens the
tunnel outbound; Cloudflare Access sits in front of `api.clawdie.ai` and
`app.clawdie.ai` and allows the five roster identities: the two humans by
the verified email their GitHub sign-in asserts, with the GitHub login
method required (never a one-time code to a mailbox this system hosts),
and a service token per agent. The bot's own processes
are unreachable from the internet.

Hostnames: `app.clawdie.ai` is decided. `api.clawdie.ai` and
`updates.clawdie.ai` are proposed until DeAndre confirms. Access adds a
second sign-in on top of the app's own accounts; keep it, because it is
what keeps the API unreachable even if app auth has a bug, and set its
session length deliberately (long, so the two humans rarely see it).

### 4.2 Steps to hosted

1. **Server prep.** Docker and compose on the server. Clone the
   repository. `cp deploy/env.example deploy/.env` (empty keys are fine
   for collector and paper).
2. **Start collecting.** `docker compose up -d collector`. From this
   moment the historical store grows, which every later step needs.
   (T-002)
3. **Paper.** `docker compose up -d paper`. Two weeks minimum
   unattended. (T-013)
4. **Run API.** Add the `api` service; confirm `GET /instances` shows the
   paper instance. (T-005)
5. **Tunnel and Access.** `cloudflared` service, one tunnel, two
   hostnames, one Access policy. (T-009)
6. **Web app on Pages.** Build from `platform/app`, custom domain
   `app.clawdie.ai`, API base `https://api.clawdie.ai`. (T-010)
7. **Desktop app and releases.** Electron shell, updater, release workflow
   (section 4.4). (T-011, T-012)

Steps 4 to 7 do not touch the bot and can run in parallel with step 3's
two weeks.

### 4.3 Steps to live trading

These are the gates already written into the bot and its checklist; the
platform work above only makes them visible.

| Stage   | Config layer         | Gate                                                       | Who decides |
|---------|----------------------|------------------------------------------------------------|-------------|
| research| `configs/backtest.*` | `tradebot-research validate` prints GO; parameters frozen  | human       |
| paper   | `env-paper.conf`     | 14 days; backtest-vs-paper consistency within tolerance     | human       |
| shadow  | `env-shadow.conf`    | every "would send" line is an order you would send by hand | human       |
| testnet | `env-testnet.conf`   | failure drills in `docs/PRE_LIVE_CHECKLIST.md` pass        | human       |
| live    | `env-live.conf`      | tiny capital, `confirm` line uncommented by a person       | human       |

Each stage is the same binary and the same strategy file; only the last
config layer changes. No agent may uncomment `confirm`, add API keys, or
raise a risk limit. Those three edits are human-only and the board's
task instructions say so wherever they come up.

### 4.4 The downloadable app and its update link

The desktop app is the web app packaged with **Electron**, exactly as
Case Forge is: the same electron-builder packaging and electron-updater
mechanism, so the design and the update flow are reused rather than
rebuilt. electron-updater's generic provider reads a per-platform
`latest.yml` manifest from `updates.clawdie.ai` on R2. The flow DeAndre's
other project has:

1. A `v*` tag is pushed.
2. GitHub Actions builds installers (macOS arm64 and x64 `.dmg`, Windows
   `.msi`, Linux `.AppImage` and `.deb`), signs them with the updater key
   (private key lives only in GitHub secrets), and uploads them to a
   Cloudflare R2 bucket served at `updates.clawdie.ai`.
3. The workflow writes the `latest.yml` manifests (version, notes,
   per-platform file and checksum) to the same bucket, creates a GitHub Release, and appends a
   note to `board/notes.md` with the download links.
4. Running apps check the manifest on launch and every few hours, show
   "Update available", download, verify the signature, and relaunch.
5. The Downloads tab reads the same manifests and the release list,
   so a fresh install is one click from inside any browser.

Installers are stored in R2, never in git.

**Signing.** electron-updater on macOS only applies updates to a signed
and notarized build, which needs an Apple Developer Program membership
(paid, human-owned). Windows needs a code-signing certificate or ships
unsigned with SmartScreen warnings. Linux needs nothing. Until DeAndre
decides (T-020), releases are unsigned development builds, the Downloads
tab labels them so, and macOS users reinstall by hand.

Session tokens in the desktop app are kept with Electron's `safeStorage`,
which uses the operating system's credential store.

### 4.5 What to decide up front

- **The domain.** Decided: `app.clawdie.ai`. Proposed and awaiting
  confirmation: `api.clawdie.ai`, `updates.clawdie.ai`.
- **Where the existing design lives.** Resolved: Case Forge's source is
  available, and it is Electron with electron-builder and
  electron-updater. Open: fork the Case Forge repository, or extract its
  renderer into a shared package (Astra proposes).
- **Owner email** for the first account, and the email sender for
  verification mails (Cloudflare Email Service if the account has it,
  otherwise Resend from the Worker). Ali's mailbox at ali@clawdie.ai is
  a separate mail-provider matter and does not gate the app.
- **Server access for agents.** Whether agents may deploy (a deploy key
  and a compose-pull workflow), or humans run every `docker compose`.
  Default in the plan: humans deploy the bot, agents deploy the app.

## 5. Tasks and notes: the board

The board answers two questions: who is doing what, and what has been
said. It is a directory in the repository, `board/`, because git is the
one store every actor already has, with attribution and history built
in. The app renders it; it does not own it.

### 5.1 Roster

| Handle          | Who                             |
|-----------------|---------------------------------|
| `deandre`       | DeAndre                         |
| `deandre-fable` | DeAndre's Claude Fable session  |
| `deandre-gpt6`  | DeAndre's GPT-6 session         |
| `ali`           | Ali                             |
| `ali-fable`     | Ali's Claude Fable session      |

Only these five may create or assign tasks. Adding an actor is a one-row
edit to `board/README.md`.

### 5.2 Tasks

A task has an id, a title, a status (`todo`, `in_progress`, `blocked`,
`review`, `done`, `dropped`), a priority, `assigned_by`, `assigned_to`,
an `assigned_on` date, an optional `due` date, an area, dependencies, an
`instructions` field written so a fresh agent needs no other context,
and an append-only log. The full schema is in `board/README.md`; the
live list is `board/tasks.json`, seeded with the fourteen work items of
this plan.

The Tasks tab shows the list as a table or a kanban, filtered by
assignee, status and area, with a roster-limited assignee picker and a
due-date picker. Saving a change commits to `board/` through the
Worker and the GitHub API,
so a human's edit in the app and an agent's edit in git are the same
kind of edit.

### 5.3 Telling an agent to work a task

The instruction to any agent is one line: "check the board" or "do
T-007". The agent's protocol (in `board/README.md`, and pointed to from
`CLAUDE.md` so Claude sessions load it automatically):

1. Pull, read `board/tasks.json`, find tasks assigned to its handle.
2. Check dependencies; if blocked, say so in the log and stop.
3. Mark `in_progress`, commit, push.
4. Do the work; commit it separately.
5. Mark `review` or `done` with what was delivered; post a note if
   others need to know.
6. Never delete, never edit another actor's lines.

GPT-6 sessions get the same protocol by being pointed at
`board/README.md` in their first message.

### 5.4 Notes

`board/notes.md` is the message board: newest first, one heading per
note with time, author handle and recipients, plain text underneath,
tasks referenced by id. A note that asks for work also creates a task,
so nothing lives only in conversation. The Notes tab is a feed with
recipient chips and task links, and a compose box that appends and
commits.

### 5.5 Fixtures

`runs/` is never committed. A small **synthetic** run directory, produced
by the same code the backtest tests use, may live under
`platform/fixtures/` for the app's unit tests, because it is generated
data, not a run artifact. Real runs (T-003) are shared as a tarball in
R2 and referenced by run id on the board.

## 6. Plan and order of work

| Step | Task(s)      | Depends on | Owner suggestion |
|------|--------------|------------|------------------|
| Merge the bot PR                                 | T-001 | –        | `deandre`       |
| Collector running on the server                  | T-002 | –        | `deandre`       |
| First backtest, run directory kept as a fixture  | T-003 | –        | `deandre-fable` |
| API contract, then the API service               | T-004, T-005 | T-003 | any agent |
| Import the existing design, tab shell            | T-006 | design location from `deandre` | `deandre` + agent |
| Runs and Live tabs                               | T-007 | T-005, T-006 | any agent |
| Tasks and Notes tabs                             | T-008 | T-005, T-006 | any agent |
| Tunnel, Access, DNS                              | T-009 | T-005    | `deandre`       |
| Pages deploy                                     | T-010 | T-007, T-009 | any agent   |
| Electron app and updater                         | T-011 | T-007    | any agent       |
| Release pipeline with download link              | T-012 | T-011    | any agent       |
| Two weeks of paper, consistency check            | T-013 | T-002, T-003 | `deandre-fable` |
| Shadow go/no-go                                  | T-014 | T-013    | `deandre`       |

Three tracks run at once: the bot's data and paper track (T-002, T-003,
T-013), the platform track (T-004 to T-012), and the human decisions
(T-001, T-006's design location, T-009's Cloudflare account, T-014).
The platform track is where the agents can go fastest; nothing in it
waits on market data beyond the one fixture run.
