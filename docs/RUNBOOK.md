# Runbook

Operational procedures for a deployed tradebot. Every procedure ends with
the system in a state the code can verify: a fresh heartbeat, a portfolio
that reconciles, a journal that replays.

## Layout on a host

| Path                                   | Contents                                         |
|----------------------------------------|--------------------------------------------------|
| `/usr/local/bin/tradebot-*`            | binaries (Release build)                         |
| `/etc/tradebot/<instance>.conf`        | one config per runtime instance (may be layered) |
| `/etc/tradebot/secrets.env`            | `TRADEBOT_BINANCE_API_KEY` / `_SECRET`, mode 0600 |
| `/var/lib/tradebot/data/raw/...`       | raw market-data archive (collector)              |
| `/var/lib/tradebot/runs/<mode>-<label>`| artifacts, `state.json`, `journal.jsonl`, `heartbeat` |

Containers use `/config`, `/data`, `/runs` for the same three things.

## Start, stop, status

```sh
systemctl start tradebot-collect                       # feed archive; keep it running
systemctl start tradebot-live@shadow-ma_1h             # instance = config name
systemctl enable --now tradebot-watchdog@shadow-ma_1h.timer
journalctl -u tradebot-live@shadow-ma_1h -f            # logs
tradebot-healthcheck /var/lib/tradebot/runs/shadow-ma_1h/heartbeat
```

`systemctl stop` sends SIGTERM. The runtime halts the risk gate, cancels
working orders (`cancel_on_stop`), waits up to five seconds for the venue's
replies, flushes artifacts and `state.json`, and writes `stopped` into the
heartbeat. Do not `kill -9` a live instance unless it is hung: the journal
still protects the portfolio, but working orders would stay on the venue
until the next start cancels them.

## Promote a strategy through the environments

1. Research: `tradebot-research judge` and `validate` say go; freeze the
   parameters in `configs/layers/strategy-<name>.conf`.
2. Paper: `env-paper.conf` for two weeks. Compare with the backtest.
3. Shadow: `env-shadow.conf` with API keys. Read the "SHADOW would send" lines.
4. Testnet: `env-testnet.conf` with testnet keys; run the drills in
   `docs/PRE_LIVE_CHECKLIST.md` by hand.
5. Live: `env-live.conf`, tiny capital, `confirm` uncommented, someone watching.

Each step is the same binary and the same strategy file; only the last
config layer changes. `tradebot-live --check` before every start.

## Upgrade the binary

```sh
cmake --build build-release && ./build-release/tests/tradebot_tests
systemctl stop tradebot-live@<instance>            # orders cancelled, state flushed
sudo deploy/install.sh build-release
tradebot-live --config /etc/tradebot/<instance>.conf --check
systemctl start tradebot-live@<instance>
```

The start restores `state.json`, cancels any of our orders still on the
venue, and reconciles against balances before trading. A mismatch refuses
to start; see below.

## Rotate API keys

1. Create the new key on the venue with trading permission only, IP
   restricted, no withdrawals.
2. Stop the instance, edit `secrets.env`, `--check`, start.
3. Delete the old key on the venue. Keys are never written to configs, logs
   or artifacts; only the environment holds them.

## Alarms and what to do

**Heartbeat stale / watchdog restarted the unit.** The dispatch thread
stopped writing for 45 seconds. Look at the last log lines before the gap:
a venue call blocking the dispatch thread is a bug (report it with the
log); a machine stall is not. After the restart, confirm "startup
reconciliation ok" in the log.

**Kill switch tripped.** Read the reason in the log (`KILL SWITCH: ...`).

- *market data stale*: the feed stopped. Working orders were cancelled,
  positions flattened if `flatten_on_trip`. With `auto_rearm`, the gate
  re-arms when data resumes; otherwise restart the instance after the feed
  is back.
- *max drawdown / max daily loss*: the strategy lost what it was allowed
  to. It stays halted until the instance restarts. Decide whether the
  strategy goes back to research before restarting; do not simply raise
  the limit.
- *reconciliation mismatch*: the venue's balances disagree with the
  portfolio for `reconcile_mismatches_to_trip` consecutive rounds. Someone
  else traded on the account, a fill was missed, or a deposit/withdrawal
  happened. Stop the instance, compare `journal.jsonl` with the venue's
  trade history, fix `state.json` (or set `initial_cash` and delete it) and
  start again.

**Startup reconciliation mismatch (refuses to start).** Same causes as
above at start time. The message prints both sides. Fix the portfolio's
view, never the tolerance, unless the difference is dust.

**Clock skew refuses to start.** Fix NTP on the host. The venue rejects
signed requests outside `recv_window` anyway.

**Feed reconnecting in a loop.** Check outbound connectivity to the stream
host and the proxy settings (`collector.proxy`, `HTTPS_PROXY`). The
collector backs off up to a minute between attempts; the runtime keeps
running with the kill switch tripped until data returns.

**Disk full.** The collector's raw archive grows by a few hundred MB per
day per symbol. Move old days out of `data/raw` (they are self-contained
hourly files); the event store under `data/store` can be rebuilt from raw
with `tradebot-ingest`. Run artifacts are small.

## Restore state after a crash

`state.json` is rewritten atomically on every flush. If it is missing or
unreadable, the runtime rebuilds the portfolio by replaying
`journal.jsonl`, which is fsynced before every report is applied. To force
a rebuild, delete `state.json` and start. To audit, replay the journal
against the venue's trade history: every fill carries the venue's trade id.

## What to look at every day

- `tradebot-healthcheck` exit code for every instance.
- The daily `summary.json` numbers against the paper run of the same
  strategy (`tradebot-research validate` consistency check).
- Fees, fill rates and slippage in `fills.csv` against the exchange model's
  assumptions. A drift here goes back into the model, then into research.
