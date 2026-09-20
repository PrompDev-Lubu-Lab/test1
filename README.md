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
tools/               command-line programs (tradebot-collect, tradebot-fetch, tradebot-ingest)
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
|    10 | Strategy framework         | not started |
|    11 | Initial strategies         | not started |
|    12 | Backtesting                | not started |
|    13 | Performance analytics      | not started |
| 14-23 | Research through live ops  | not started |
