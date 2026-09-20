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
src/tradebot/strategies/ baseline strategies (buy-and-hold, MA crossover, mean reversion, breakout, random)
src/tradebot/backtest/ backtest spec, wiring, artifacts, sweeps, parallel batch runner
src/tradebot/analytics/ return/risk metrics, round trips, benchmark comparison, reports
src/tradebot/research/ experiment index, sweep ranking, walk-forward evaluation, kill criteria
src/tradebot/live/     wall-clock scheduler, paper-trading runtime, execution journal, feed health, heartbeat
tools/               command-line programs (collect, fetch, ingest, backtest, analyze, research, paper)
configs/             example configuration files
tests/               doctest unit tests, one directory per module
third_party/         vendored header-only dependencies (doctest, tl::expected, nlohmann/json)
docs/                architecture and design notes
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
| 17-23 | Optimization through live ops | not started |
