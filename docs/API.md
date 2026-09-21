# Run API contract (draft for review, board task T-004)

Two services, one rule: **field names are the field names in the files
the bot writes.** Nothing is renamed or invented in the API layer. If a
screen needs a value the bot does not write, the bot gains a field first
(a C++ change with a test, board task for `deandre-fable`), then the API
exposes it.

| Service | Where | Owns |
|---|---|---|
| **Run API** (`tradebot-api`) | on the server, `runs/` mounted read-only, reachable only through the Cloudflare Tunnel | instances, runs, research files, change events |
| **Worker** | Cloudflare, `api.clawdie.ai` (proposed) | accounts, sessions, roles, avatars, the board, proxying the run API |

The Worker proxies every path below under the same names, after
checking the session and role. The run API itself has no auth and no
public exposure.

Encodings, decided at G3 (COLLAB-20260921-04):

- **JSON files** (`summary.json`, `metrics.json`, `status.json`,
  `state.json`, `validation.json`, `drift.json`) are served with the
  types the bot wrote: money and quantities are decimal strings, counts
  and fractions are numbers, times are ISO-8601 UTC strings, durations
  are the bot's human strings where the file has them.
- **CSV-derived rows** (`equity`, `fills`, `orders`, `metrics`,
  `round_trips`, and `runs/index.csv`) are served with **every cell as
  the source string**, including counts and fractions. The API makes no
  typing decision for CSV; the client converts for plotting only and
  keeps the source string for tables and tooltips.
- **Journal lines** are raw bytes.
- The only derived field anywhere is `heartbeat.age_seconds`.

## 1. Instances (running bots)

An instance is a directory `runs/<mode>-<label>/` that has a `heartbeat`
file. Its id is the directory name.

### `GET /instances`

```json
[
  {
    "id": "paper-ma_1h",
    "mode": "paper",
    "label": "ma_1h",
    "heartbeat": { "time": "2026-09-21T01:00:05.000000000Z", "status": "paper healthy armed", "age_seconds": 3.2 },
    "status": { ...the instance's status.json, see below... }
  }
]
```

**Startup window.** The bot writes `heartbeat` (`starting armed`) before
its first metrics flush, so for up to one heartbeat interval an instance
has no `status.json`. It is still listed, with `status`, `mode` and
`label` all `null`. A malformed `status.json` (no `mode` or `label`) is a
`503 source_incomplete`, not a null.

**Listing pages.** `/runs` and `/instances` return at most `limit` items
(default and maximum 500) starting at item `after` (0-based), with
`X-Next-After` and `X-Has-More`; `X-Result-Warning` is set when the root
holds more than 500 directories. The API refuses a root with more than
5000 directories (`413 too_many_runs`); archive old research roots.

`heartbeat.status` is the raw second field of the `heartbeat` file: at
startup `starting armed`, on the timer `<mode> <feed_state> <armed|tripped>`,
at shutdown `stopped`. `age_seconds` is computed by the API from the
file's time; it is the only derived field in this contract.

### `GET /instances/{id}/status`

`status.json` verbatim, rewritten by the bot every `heartbeat_interval`
(default 5s):

```
time, mode, label, uptime, equity, cash, peak_equity, drawdown,
realized_pnl_net, fees, position, mark (optional), fills, orders,
rejected, open_orders, kill_switch_trips, tripped, halted,
feed { state (waiting|healthy|stale), silence, events, messages, reconnects, depth_gaps, stale_episodes },
dispatched_events, journal_appended,
gateway (testnet/live only) { sent, send_failures, cancels_sent, stream_events, duplicates, late_fills, queries, open_orders, reconciliations, reconcile_mismatches, user_stream_connects }
```

### `GET /instances/{id}/state`

`state.json` verbatim: `version`, `initial_cash`, `account` (ledger),
`strategies` (array of `{id, ledger}`). Positions and cash for the Live
tab come from here.

### `GET /instances/{id}/journal?after=<line>|offset=<byte>&limit=<n>`

Lines of `journal.jsonl`, served as `application/x-ndjson`, raw bytes,
never parsed and re-serialised by the API or the Worker, at most `limit`
lines (default and maximum 10000). An incomplete final line (the bot
mid-append) is not published. Two cursors, mutually exclusive
(`400` if both are given):

- `after=<line>`: 0-based line index. Cheap for small journals; the API
  scans from the start and refuses with `413 cursor_required` once a
  scan would pass 256 MiB.
- `offset=<byte>`: byte position that must sit right after a newline
  (`400` otherwise). The API reads only the requested page in 64 KiB
  chunks, so this works at any journal size. `409 source_changed` when
  `offset` is beyond the file's end (rotated or truncated): restart the
  cursor at 0.

Headers: `X-Next-Offset` always (the byte to pass next; equal to the
request's offset when nothing new is complete), `X-Next-After` only on
line-cursor requests, `X-Has-More`. A single line over 1 MiB or a page
over 16 MiB is `413`. Live clients keep `X-Next-Offset` and poll it when
a `journal` event arrives. CSV routes paginate with `after` and `limit`
only. Each line verbatim: `type` (`accepted|rejected|fill|cancelled|cancel_rejected|expired`),
`client_id`, `order_id`, `instrument`, `strategy`, `side`, `order_type`,
`price`, `time` (ISO-8601 with nanoseconds since T-029; journals written
before 2026-09-21 carry an integer nanoseconds-since-epoch value, which
the bot still reads and the API passes through unchanged as raw NDJSON
bytes), `status`,
`filled`, `remaining`, optional `fill {price, quantity, fee, liquidity, exec_id}`,
optional `reason`.

### `GET /instances/{id}/metrics.prom`

The Prometheus text file, verbatim, for anyone who wants to scrape.

An instance directory also holds the run files below; `/runs/{id}/...`
works with an instance id.

## 2. Runs (finished backtests and any run directory)

A run is any directory under `runs/` with a `summary.json`. Backtest ids
look like `<label>_<from>_<to>_<8 hex>`.

### `GET /runs`

The `summary.json` object of **every** run directory, verbatim, as an
array, whether or not `runs/index.csv` exists. (G3 finding: an index-only
listing hid every run that `tradebot-backtest` or `validate` wrote
without a sweep, because only sweeps append to the index.) Clients
distinguish runs from instances by the directory name and by the
presence of a `heartbeat` in `/instances`. `max_drawdown` on a summary is
money; the fraction is `max_drawdown_fraction`. Round trips are not in a
summary and are shown as unavailable until `metrics.json` is fetched.

### `GET /runs/index`

`runs/index.csv` as rows (every cell a string), or `404` when the file
does not exist. Research metadata only; never the run list.

### `GET /runs/{id}`

```json
{
  "summary": { ...summary.json verbatim... },
  "metrics": { ...metrics.json verbatim, or null if tradebot-analyze has not run... },
  "config": "run_id = ...\n...config.txt verbatim..."
}
```

`summary.json` keys: `run_id, symbol, from, to, seed, strategies[{id,label}],
initial_cash, final_equity, total_return, realized_pnl_net, fees,
max_drawdown, max_drawdown_fraction, fills, orders, rejected, volume,
turnover, events, kill_switch_trips, wall_time_seconds, equity_samples`.

`metrics.json` keys: `run_id`, `returns {samples, span_seconds,
period_seconds, total_return, annualized_return, annualized_volatility,
sharpe, sortino, calmar, max_drawdown, max_drawdown_duration_seconds,
best_period, worst_period, time_in_market}`, `benchmark {same keys}`
(optional), `excess_return`, `trades {fills, round_trips, wins, losses,
win_rate, profit_factor (-1.0 when undefined), gross_profit, gross_loss,
net_pnl, expectancy, average_win, average_loss, largest_win,
largest_loss, average_holding_seconds, total_fees, turnover, fee_drag,
open_quantity}`.

### `GET /runs/{id}/equity`

`equity.csv` as an array of objects with the CSV's own column names:
`time, equity, cash, realized_pnl_net, unrealized_pnl, fees, position, mark`,
every value a string. Optional `?every=N` returns every Nth row for long
runs; the first and last rows are always included. `after` and `limit`
paginate as for the journal.

### `GET /runs/{id}/fills`

`fills.csv` rows: `time, strategy, client_id, side, price, quantity, fee, liquidity`.

### `GET /runs/{id}/orders`

`orders.csv` rows: `time, strategy, client_id, event, side, type, price, filled, remaining, reason`.

### `GET /runs/{id}/metrics`

`metrics.csv` rows: `time, strategy, name, value` (the strategy's own
named telemetry, irregular in time). Optional `?name=zscore`.

### `GET /runs/{id}/round_trips`

`round_trips.csv` rows: `strategy, entry_time, exit_time, direction (long|short),
quantity, entry_price, exit_price, gross_pnl, fees, net_pnl, return, holding_seconds`.

### `GET /runs/{id}/report`

`report.txt` as `text/plain`.

## 3. Research

### `GET /runs/{id}/drift`

`drift.json` if `tradebot-monitor drift` has run against this directory:
`reference, observed, observed_span, observed_samples, status (ok|warn|alarm),
checks[{name, status, expected, observed, score, detail}]`. Check names:
`return, drawdown, fee_drag, trade_rate, win_rate, rejections, kill_switch`.

### `GET /runs/{id}/revalidation`

`revalidation.txt` as `text/plain` (ends with `Decision: keep|watch|retire`).

### `GET /runs/{id}/validation`

`validation.json`, written by `tradebot-research validate` into the run
directory of the base run it validates (T-028). Keys: `run_id`,
`report` (the same object as `metrics.json`), `kill_criteria
{min_round_trips, min_sharpe, max_drawdown, min_profit_factor,
must_beat_benchmark}`, `verdict {pass, failures[]}`, then only the
sections that ran: `monte_carlo {samples, trips, return_p05, return_p50,
return_p95, drawdown_p50, drawdown_p95, probability_negative}`,
`costs` and `stability` (each `{points[{label, total_return, sharpe,
max_drawdown, round_trips}], fraction_positive, median_sharpe,
min_sharpe, max_sharpe}`), `regimes {segments[{from, to, realized_vol,
high_vol, strategy_return, market_return}], high_vol_return,
low_vol_return, high_vol_market, low_vol_market}`, `walk_forward
{windows[{train_from, train_to, test_to, chosen_label, chosen_params{},
in_sample_metric, out_of_sample{metrics.json object}}], oos_total_return,
oos_mean_sharpe, oos_positive_fraction, oos_worst_drawdown,
oos_round_trips}`, `consistency {from, to, backtest_return, paper_return,
backtest_trips, paper_trips, return_gap, consistent}`, and `go_no_go
{go, checks[{name, pass, detail}]}`. Non-finite scores are written as
`0.0`. Sections that did not run are absent, not null.

`tradebot-research sweep|walk-forward|judge --json-out FILE` write the
same shapes (a ranked comparison `{sorted_by, rows[{rank, label, metric,
report}]}`, a walk-forward result, or `{kill_criteria, runs[{run_dir,
run_id, verdict}]}`) wherever asked; the API serves them only when they
sit inside a run directory.

## 4. Events

### `WS /events`

One JSON message per change, pushed by the run API when it sees a file
change:

```json
{ "kind": "heartbeat", "instance": "paper-ma_1h", "time": "...", "status": "paper healthy armed" }
{ "kind": "status",    "instance": "paper-ma_1h", "status": { ...status.json... } }
{ "kind": "state",     "instance": "paper-ma_1h" }
{ "kind": "journal",   "instance": "paper-ma_1h", "bytes": 1834412 }
{ "kind": "run",       "run_id": "...", "event": "created|updated" }
```

The `journal` event is an invalidation hint carrying the file's size at
observation, never a line count (counting lines would mean reading the
whole file). Clients fetch from their saved `X-Next-Offset`. If `bytes`
is smaller than the client's offset the journal was rotated: restart at
0.

The Worker relays this stream to signed-in clients and adds
`{ "kind": "board", "file": "tasks|notes" }` when it commits a board
change.

## 5. Board (Worker only)

Shapes are exactly `board/tasks.json` and `board/notes.md` as defined in
`board/README.md`; the Worker reads and writes them through the GitHub
API and never through the server.

| Endpoint | Body / result |
|---|---|
| `GET /board/tasks` | the `tasks` array |
| `PUT /board/tasks/{id}` | a full task object; the Worker validates the schema, roster handles and status values, appends the log line with the caller's handle, commits `board: <id> <status> (<handle>)` |
| `POST /board/tasks` | a task without `id`; the Worker assigns the next free id |
| `GET /board/notes` | notes parsed into `[{time, from, to[], text}]` newest first |
| `POST /board/notes` | `{to: [...], text}`; the Worker prepends the heading with the caller's handle and time and commits |

Conflict rule (from `board/README.md`): keep both sides, keep every log
line, take the newest status. The Worker retries a rejected commit after
re-reading the file; it never force-pushes.

## 5b. Downloads and updates (Worker only)

Same-origin, session and terms required, GET and HEAD only, `Cache-Control:
private, no-store`, no Range support. Served from private R2 by the
Worker, never from a public bucket.

| Endpoint | Returns |
|---|---|
| `GET /downloads` | the release catalog: `{version, file, size, sha512, released_at, url, notes?}` |
| `GET /updates/windows/x64/latest.yml` | electron-updater's manifest for the one complete Windows x64 installer |
| `GET /updates/windows/x64/clawdie-platform-<version>-win-x64.exe` | the installer bytes; `HEAD` gives the exact size |

`GET /me` also returns `authentication_expires_at`, the earlier of the
Access assertion's expiry and the app session's expiry, which the desktop
updater uses to bound a download.

## 6. Accounts (Worker only)

Defined by Astra's M1 design note (board task T-018), not here. This
document only fixes that every run and board path above is served under
the same names by the Worker after a session check, and that role
`member` may read everything and write the board, while `owner` may also
manage users and the allowlist.

## 7. Errors

`400` for a bad id, path or query, `404` for an unknown instance, run or
artifact, `405` for anything but GET and HEAD on the run API, `409` on a
board conflict that could not be merged (with both versions in the
body), `413` for an artifact over the read limit, `503` when a source
file is incomplete or changing (retry) or when the tunnel to the server
is down (the board and accounts keep working). Error bodies are
`{ "error": "<short code>", "detail": "<sentence>" }`. Every response
carries `X-Data-Source: synthetic-fixtures | bot-artifacts` so a preview
can never be mistaken for the bot.
