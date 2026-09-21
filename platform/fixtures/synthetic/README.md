# Synthetic fixture runs

**These are not market data and not real results.** The store behind them
is generated: one book snapshot and one trade per minute for two days
(2024-03-15 to 2024-03-17), with the mid price following a sine wave with a
12-hour period around 3000. It exists so the app's Runs tab, parsers and
tests can be built against files in exactly the format the bot writes,
before the real sample run (board task T-003) is available.

| Directory | Strategy | Note |
|---|---|---|
| `ma_synthetic_2024-03-15_2024-03-17_78e97a61` | `ma_crossover` (15m candles, fast 4, slow 12, quantity 1) | 7 fills, crosses the sine wave several times |
| `bah_synthetic_2024-03-15_2024-03-17_f2e66a79` | `buy_and_hold` (quantity 1) | 1 fill; the benchmark |

The `ma_synthetic_…` directory also holds `validation.json`, written by
`tradebot-research validate --config ma_synthetic_validate.conf --out runs
--samples 200 --train 1d --test 12h --min-trades 3` (the same spec plus a
`[sweep]` block, so the run id is unchanged). It has every section:
`monte_carlo`, `costs` (45 points), `stability` (3 points), `regimes`,
`walk_forward` (2 windows) and `go_no_go`. Its numbers are meaningless as
research (a sine wave is trivially predictable) and exist only to give the
Research tab a complete file to render.

Each directory holds the six files every run writes (`config.txt`,
`equity.csv`, `fills.csv`, `orders.csv`, `metrics.csv`, `summary.json`) plus
the three `tradebot-analyze` adds (`metrics.json`, `report.txt`,
`round_trips.csv`). Field names and value encodings are documented in
`docs/PLATFORM_PLAN.md` section 1.8. Monetary values in JSON are strings.

## Reproduce

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DTRADEBOT_ENABLE_SANITIZERS=OFF
cmake --build build-release
LIBS=$(find build-release -name '*.a' | tr '\n' ' ')
g++ -std=c++23 -O2 -I src -isystem third_party platform/fixtures/synthetic/generate_store.cpp \
    -o /tmp/gen_store -Wl,--start-group $LIBS -Wl,--end-group -lz -lssl -lcrypto -lpthread
mkdir -p /tmp/fixture && cd /tmp/fixture && /tmp/gen_store data
cp $REPO/platform/fixtures/synthetic/*.conf .
$REPO/build-release/tools/tradebot-backtest --config ma_synthetic.conf --out runs
$REPO/build-release/tools/tradebot-backtest --config bah_synthetic.conf --out runs
$REPO/build-release/tools/tradebot-analyze runs/*
```

The run ids are derived from the specification, so a reproduction lands in
directories with the same names and identical numbers except
`wall_time_seconds` in `summary.json`.

## Live-surface fixture: `paper-ma_synthetic/`

**Also synthetic.** Written by the bot's own writers (`write_metrics`,
`write_heartbeat`, `Portfolio::state_to_json`, `Journal::append`) from a
small scripted sequence, so the field names and encodings are exactly what
a paper instance produces, but no process ran and no market was watched.

| File | What it shows |
|---|---|
| `heartbeat` | `2026-09-21T01:15:00Z paper healthy armed` |
| `status.json`, `metrics.prom` | a healthy, armed paper instance 75 minutes in: long 1 ETH at average 3000, marked 3012.50, 2 fills, 1 rejection |
| `state.json` | the portfolio behind that status, account and per-strategy ledgers |
| `journal.jsonl` | six reports: accepted, two fills (partial then full), accepted, cancelled, rejected with a reason. `time` is integer nanoseconds. |
| `sample-stale-tripped/` | the same instance 20 minutes later after the feed went stale and the kill switch tripped: `heartbeat` says `paper stale tripped`, `status.json` has `feed.state = "stale"`, `tripped = true` |

Reproduce with `generate_live.cpp`, compiled the same way as
`generate_store.cpp` in the section above:

```sh
g++ -std=c++23 -O2 -I src -isystem third_party platform/fixtures/synthetic/generate_live.cpp \
    -o /tmp/gen_live -Wl,--start-group $LIBS -Wl,--end-group -lz -lssl -lcrypto -lpthread
/tmp/gen_live platform/fixtures/synthetic/paper-ma_synthetic
```
