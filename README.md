# tradebot

An event-driven cryptocurrency trading research system in C++, initially
focused on ETH. It exists to test trading strategies honestly against real
historical and live market data before any real capital is considered.

The design is described in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
The short version: strategies talk only to abstract market-data and
execution interfaces, so the same compiled strategy runs unchanged against
historical replay, a simulated exchange, live paper trading, and eventually a
real exchange.

No strategy is assumed to be profitable. The pipeline is built to disprove
strategies cheaply.

## Building

Requirements: CMake 3.20+, Ninja, OpenSSL 3 and zlib development headers, and
GCC 13+ or Clang 18+.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Debug builds enable AddressSanitizer and UndefinedBehaviorSanitizer and treat
all warnings as errors. Use `-DCMAKE_BUILD_TYPE=Release` for backtests and
`-DTRADEBOT_ENABLE_SANITIZERS=OFF` if your toolchain lacks the sanitizer
runtime.

## Layout

```
src/tradebot/core/   core types: fixed-point money, time, clock, ids, config, logging
src/tradebot/net/    TCP, TLS (OpenSSL), HTTP/1.1 client, WebSocket client
src/tradebot/util/   SHA-256, streaming zip reader
src/tradebot/market_data/  events, raw capture, Binance collector/fetcher/parsers, order book, candles, validation
src/tradebot/storage/  normalized event store (TBEV files), catalog, ingest pipelines
src/tradebot/replay/   event sources, k-way merge, latency models, replay engine and event bus
src/tradebot/execution/  execution interface (orders, reports, fees) and the simulated exchange
src/tradebot/portfolio/  positions, cash, P&L, exposure, per-strategy ledgers, equity curve
src/tradebot/risk/     pre-trade checks, portfolio limits and kill switch (wraps any venue)
src/tradebot/strategy/ Strategy and StrategyContext interfaces, indicators, multi-strategy runner
src/tradebot/strategies/ baselines (buy-and-hold, MA crossover, mean reversion, breakout, random) and
                     advanced strategies (vol_trend, book_imbalance, market_maker, ensemble)
src/tradebot/backtest/ backtest spec, wiring, artifacts, sweeps, parallel batch runner
src/tradebot/analytics/ return/risk metrics, round trips, benchmark comparison, reports
src/tradebot/research/ experiment index, sweeps, walk-forward, kill criteria, Monte Carlo, cost and
                     parameter sensitivity, regime split, backtest-vs-paper consistency, go/no-go,
                     drift detection and re-validation
src/tradebot/live/     wall-clock scheduler, trading runtime (paper/shadow/testnet/live), journal, feed health,
                     metrics export
src/tradebot/gateway/  Binance spot gateway: request signing, private REST, user data stream, order state machine
tools/               command-line programs (collect, fetch, ingest, backtest, analyze, research, paper, live,
                     healthcheck, monitor, bench)
configs/             example configuration files and layered deployment configs
deploy/              systemd units, install script, secrets template; Dockerfile and compose file at the root
tests/               doctest unit tests, one directory per module
third_party/         vendored header-only dependencies (doctest, tl::expected, nlohmann/json)
docs/                architecture, pre-live checklist, runbook, strategy lifecycle
cmake/               warning and sanitizer configuration
```

## Collecting live data

```sh
cp configs/collect.example.conf configs/collect.conf   # edit symbol / data dir
./build/tools/tradebot-collect --config configs/collect.conf
```

The collector writes every message verbatim, plus periodic order-book
snapshots and the symbol's exchange filters, under
`data/raw/binance/<SYMBOL>/<date>/<hour>.jsonl.gz`. Start it early: data
accumulates while the rest of the system is built.

## Fetching historical data

```sh
./build/tools/tradebot-fetch --symbol ETHUSDT --from 2024-01-01 --to 2024-07-01 \
    --datasets aggTrades,klines --interval 1m
```

Downloads Binance's public daily/monthly archives (monthly where a whole
month is covered), verifies each against its published SHA-256, and keeps
the zips as immutable ground truth under `data/bulk/binance/<SYMBOL>/`.
Re-running is idempotent.

## Building the event store

```sh
./build/tools/tradebot-ingest bulk --symbol ETHUSDT --from 2024-01-01 --to 2024-07-01
./build/tools/tradebot-ingest raw  --symbol ETHUSDT --from 2024-07-01 --to 2024-07-08
./build/tools/tradebot-ingest catalog --symbol ETHUSDT
```

Ingest converts raw sources into validated, normalized events stored as
compressed binary files per instrument, UTC day and stream kind under
`data/store/binance/<SYMBOL>/`. Each file header records counts, time
range and quality flags (sequence gaps, book resyncs), which `catalog`
prints.

## Running a backtest

```sh
cp configs/backtest.example.conf configs/backtest.conf   # edit range, strategy, params
./build/tools/tradebot-backtest --config configs/backtest.conf --out runs
```

Every run writes `runs/<run_id>/` with `config.txt` (the full spec),
`equity.csv`, `fills.csv`, `orders.csv`, `metrics.csv` and `summary.json`.
The run id is derived from the spec, so the same inputs always map to the
same directory, and the same spec and seed always reproduce the same
numbers. A `[sweep]` section expands into a parameter grid run in parallel.

```sh
./build/tools/tradebot-analyze runs/<run_id>
```

prints returns (total, annualized, volatility, Sharpe, Sortino, Calmar, max
drawdown and its duration), a buy-and-hold benchmark on the same marks, and
trade statistics from FIFO round trips (win rate, profit factor,
expectancy, holding time, fee drag), and writes `metrics.json`,
`report.txt` and `round_trips.csv` next to the run's artifacts.

## Researching strategies

```sh
./build/tools/tradebot-research sweep --config configs/backtest.conf --metric sharpe
./build/tools/tradebot-research walk-forward --config configs/backtest.conf --train 60d --test 14d
./build/tools/tradebot-research judge runs/<run_id> --min-trades 30 --min-sharpe 0.5
./build/tools/tradebot-research index
```

`sweep` runs the `[sweep]` grid, ranks the results and appends every run to
`runs/index.csv`. `walk-forward` re-selects parameters on each rolling
train window and reports only out-of-sample results, which are the numbers
that count. `judge` applies pre-agreed kill criteria (minimum trades and
Sharpe, maximum drawdown, minimum profit factor, must beat buy-and-hold).

```sh
./build/tools/tradebot-research validate --config configs/backtest.conf \
    --train 60d --test 14d --paper-dir runs/paper-ma_1h
```

`validate` is the go/no-go gate before any real capital: it runs the
strategy, bootstraps its round trips for confidence intervals on return and
drawdown, reruns it under harsher fee, slippage and latency assumptions,
scores the whole parameter grid (a lone good point is overfit), splits the
run into high- and low-volatility regimes, runs walk-forward when asked,
compares against a paper-trading run over the same window, and prints a
GO or NO-GO checklist.

## Paper trading

```sh
cp configs/paper.example.conf configs/paper.conf   # same strategy/risk sections as a backtest
./build/tools/tradebot-paper --config configs/paper.conf
```

Runs the unchanged strategy against the live Binance feed with simulated
fills (same exchange model as backtests, real market-data latency), archives
the raw feed as it goes, and flushes artifacts in the backtest format plus
`state.json` to `runs/paper-<label>/` every minute. A restart resumes the
portfolio from `state.json`. Because the artifacts match, `tradebot-analyze`
and `tradebot-research judge` compare paper results with backtests directly.

Reliability: every execution report is fsynced to `journal.jsonl` before
the portfolio applies it, and a lost or corrupt `state.json` is rebuilt by
replaying the journal (fills are idempotent by execution id). A feed that
goes silent for `feed_stale_after` trips the kill switch, which cancels
working orders and, with `risk.flatten_on_trip`, market-closes positions;
the switch re-arms when data resumes. A `heartbeat` file is refreshed every
few seconds for external watchdogs. The feed reconnects with backoff on
any disconnect; malformed messages are logged and skipped.

## Live trading modes

```sh
cp configs/live.example.conf configs/live.conf
export TRADEBOT_BINANCE_API_KEY=... TRADEBOT_BINANCE_API_SECRET=...
./build/tools/tradebot-live --config configs/live.conf --check         # preflight only
./build/tools/tradebot-live --config configs/live.conf --mode shadow
./build/tools/tradebot-live --config configs/live.conf --mode testnet
```

`tradebot-live` runs the same runtime as `tradebot-paper` in one of four
modes; only what sits behind the `ExecutionVenue` interface changes.

| Mode      | Fills                | Account access            | Needs                                   |
|-----------|----------------------|---------------------------|-----------------------------------------|
| `paper`   | simulated exchange   | none                      | nothing                                 |
| `shadow`  | simulated exchange   | read-only (clock, balances, user stream); orders logged as "would send" | API keys |
| `testnet` | Binance spot testnet | real orders on the testnet | testnet keys                           |
| `live`    | Binance spot         | real orders, real money   | keys, confirmation phrase, capital limits |

Before connecting, the runtime refuses to start unless the pre-flight
checklist passes: credentials present, clock skew within a second of the
venue, and in live mode an explicit confirmation phrase plus a capital cap
that every notional limit must stay under. At start it cancels open orders
left by a previous run and reconciles the portfolio with the venue's
balances, refusing to trade on a mismatch. While running it re-queries
silent orders, reconciles on a timer (consecutive mismatches trip the kill
switch), and on shutdown it halts the risk gate and cancels working orders
before exiting. The failure drills in `tests/live/live_modes_test.cpp` run
every one of these paths against in-process fakes of the venue;
`docs/PRE_LIVE_CHECKLIST.md` lists what remains to be done by hand.

## Monitoring and re-validation

```sh
./build/tools/tradebot-monitor status runs/shadow-ma_1h                           # status.json + heartbeat age
./build/tools/tradebot-monitor drift --run runs/shadow-ma_1h --reference runs/<run_id>
./build/tools/tradebot-monitor revalidate --run runs/shadow-ma_1h --reference runs/<run_id> --config configs/live.conf
```

Every runtime writes `metrics.prom` (Prometheus text format, for
node_exporter's textfile collector) and `status.json` on the heartbeat
cadence: equity, drawdown, position, fills, rejections, kill-switch state,
feed health, gateway and reconciliation counters. `drift` compares a
running strategy with the research run that approved it (return z-score,
drawdown ratio, fee drag, trade rate, win rate, rejections, kill-switch
trips) and grades each ok, warn or alarm. `revalidate` adds the kill
criteria and a fresh backtest over the observed window, and decides keep,
watch or retire; the systemd timer runs it daily. `docs/LIFECYCLE.md`
describes the whole loop from candidate to retirement.

## Deploying

```sh
docker build -t tradebot .                       # builds Release, runs the tests, ships the tools
cp deploy/env.example deploy/.env                 # API keys for shadow/testnet/live (never committed)
docker compose up -d collector paper              # or shadow
sudo deploy/install.sh build-release              # bare-metal alternative: systemd units
```

Configuration is layered: a base layer (instrument, feed, exchange model),
a strategy layer frozen by research, and an environment layer
(`configs/layers/env-{paper,shadow,testnet,live}.conf`) that sets the mode
and the risk limits. `tradebot-live --config a --config b --config c`
merges them key by key, later files winning, and any `TRADEBOT_SECTION_KEY`
environment variable overrides both. Secrets only ever come from the
environment. `tradebot-healthcheck <run>/heartbeat` backs the container
HEALTHCHECK and the systemd watchdog timer; `docs/RUNBOOK.md` covers
start/stop, promotion between environments, upgrades, key rotation and
every alarm the runtime can raise.

## Live gateway

`tradebot::gateway::BinanceGateway` implements the same `ExecutionVenue`
interface as the simulated exchange, so a strategy, the risk gate and the
portfolio run against it unchanged. It signs requests (HMAC-SHA256) with
`TRADEBOT_BINANCE_API_KEY` / `TRADEBOT_BINANCE_API_SECRET`, sends orders on a
worker thread, and receives acknowledgements, fills and cancels both from
the REST reply and from the user data stream. A per-order state machine
de-duplicates by execution id, so a fill reported by the stream and again by
a later query is applied once. A send whose outcome is unknown (timeout,
proxy error) is resolved by querying the order; a cancel of an order the
venue no longer knows is resolved the same way. Orders that go silent are
re-queried, and `reconcile()` compares venue balances with the portfolio.
`dry_run = true` rejects every order locally without sending anything, and
API keys should be created without withdrawal permission. The gateway is
unit-tested against an in-process fake of the Binance REST API; it has not
been exercised against the real venue from this build environment.

## Performance

`tradebot-bench` measures the hot paths on synthetic data (Release build,
one core):

| Stage                          | Throughput            |
|--------------------------------|-----------------------|
| event store write              | ~1.1M events/s        |
| event store read + merge       | ~3.3M events/s        |
| order book delta application   | ~3.3M deltas/s        |
| replay dispatch (both buses)   | ~11M deliveries/s     |
| full backtest (ma_crossover)   | ~1.5M events/s        |

A day of ETHUSDT trades plus 100 ms depth updates is roughly one to two
million events, so a year backtests in minutes and sweeps parallelize
across cores. Compression (zlib) dominates the read and write paths; the
engine itself is not the bottleneck. Results are bit-identical across
optimizations (the backtest tests check determinism).

## Status

| Phase | Component                  | Status      |
|------:|----------------------------|-------------|
|     1 | Project setup, core types  | done        |
|     2 | Market-data collection     | done        |
|     3 | Market-data processing     | done        |
|     4 | Order-book system          | done        |
|     5 | Historical data storage    | done        |
|     6 | Historical market replay   | done        |
|     7 | Simulated exchange         | done        |
|     8 | Portfolio management       | done        |
|     9 | Risk management            | done        |
|    10 | Strategy framework         | done        |
|    11 | Initial strategies         | done        |
|    12 | Backtesting                | done        |
|    13 | Performance analytics      | done        |
|    14 | Strategy research tooling  | done        |
|    15 | Real-time paper trading    | done        |
|    16 | Reliability and error handling | done    |
|    17 | Performance optimization   | done        |
|    18 | Advanced strategies        | done        |
|    19 | Robust strategy validation | done        |
|    20 | Live trading infrastructure | done       |
|    21 | Final testing (shadow, testnet, live modes, drills) | done |
|    22 | Deployment                 | done        |
|    23 | Continuous monitoring and re-validation | done |
