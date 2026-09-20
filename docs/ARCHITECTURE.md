# ETH Trading Bot — Architecture and Build Roadmap

This document is the high-level plan for a C++ cryptocurrency trading system,
initially focused on ETH. It describes **what** gets built, **what each
component is responsible for**, and **how the pieces connect**, in the order
they should be built. It intentionally avoids implementation detail.

## Guiding principles

- **Strategies are exchange-agnostic.** A strategy talks to abstract
  interfaces (market data in, orders out, fills/positions back). The same
  compiled strategy runs against the historical replay, the simulated
  exchange, a live paper-trading feed, and eventually a real exchange.
- **No strategy is assumed profitable.** The system exists to *disprove*
  strategies cheaply. Every stage of the pipeline is built to make results
  honest: realistic fills, fees, latency, slippage, and out-of-sample checks.
- **Event-driven core.** Everything is an event on a timeline: ticks, book
  updates, order submissions, fills, timer ticks. Backtesting, paper trading,
  and live trading differ only in *where the events come from* and *how fast
  the clock runs*.
- **Correctness before speed, then speed.** Deterministic replay and
  reproducible results come first. Optimization happens after the system is
  proven correct, guided by measurement.
- **Modularity.** Each component has one job and a narrow interface, so it can
  be replaced or tested in isolation.

## Two-phase research workflow

The workflow the whole system serves:

1. **Historical phase.** Run many candidate strategies through the backtester
   against stored real ETH market data. Rank them with robust, out-of-sample
   metrics. Discard most.
2. **Forward phase.** Run the survivors in real-time paper trading against the
   live market for an extended period. Keep only those whose forward results
   are consistent with their backtests. Only then is real capital considered.

## System overview

```
                       ┌───────────────────────────────────────────┐
                       │                Strategies                 │
                       │  (identical code in every environment)    │
                       └───────▲───────────────────────┬───────────┘
                    market events              order intents
                               │                       │
        ┌──────────────────────┴───────┐   ┌───────────▼─────────────────┐
        │   Strategy Framework / Bus   │   │  Risk Manager (gatekeeper)  │
        └──────────────────────▲───────┘   └───────────┬─────────────────┘
                               │                       │ approved orders
       ┌───────────────────────┼───────────────────────▼──────────────────┐
       │                Execution Interface (abstract)                    │
       ├─────────────────────┬──────────────────────┬─────────────────────┤
       │ Simulated Exchange  │  Paper-Trading Sim   │  Live Exchange      │
       │ (replay clock)      │  (wall clock, real   │  Gateway (real      │
       │                     │   data, fake fills)  │   orders)           │
       └─────────▲───────────┴───────────▲──────────┴──────────▲──────────┘
                 │                       │                     │
       ┌─────────┴───────────┐  ┌────────┴─────────────────────┴──────────┐
       │ Historical Replay   │  │ Live Market-Data Collector + Processor  │
       │ (from storage)      │  │ (websocket → normalized events)         │
       └─────────▲───────────┘  └────────┬────────────────────────────────┘
                 │                       │
       ┌─────────┴───────────────────────▼───────────┐
       │        Historical Data Storage               │
       └──────────────────────────────────────────────┘

       Portfolio ◄── fills ──  (any execution venue)  ──► Analytics / Logs
```

The key seam is the **Execution Interface** plus the **Market-Data Interface**.
Everything above them is venue-independent. Everything below them is a
pluggable backend.

---

## Phase-by-phase roadmap

### 1. Project setup

**Goal:** a repeatable, testable C++ foundation before any trading code exists.

- Build system, compiler warnings as errors, sanitizers in debug builds,
  a unit-test framework, and continuous integration that runs the tests.
- A clear module layout mirroring the components below (core types, market
  data, storage, exchange, portfolio, risk, strategy, backtest, analytics,
  live, tools).
- Core shared types defined once: instrument/symbol, price and quantity
  representations (fixed-point or scaled integers, never raw floating point
  for money), timestamps with a single clock convention, side, order types,
  time-in-force, and unique IDs.
- Configuration and structured logging from day one, so every later
  component is observable and reproducible.
- Coding standards: value semantics for events, explicit ownership, no
  hidden global state, everything deterministic given the same inputs.

**Connects to:** everything. This is the substrate.

### 2. Market-data collection

**Goal:** get real ETH market data into the system, live and historically.

- A **collector** that connects to an exchange's public feeds and receives
  raw trades, order-book snapshots, and incremental book updates (and,
  optionally, candles and funding/mark data if perpetuals are ever in
  scope).
- A **historical fetcher** for backfilling past data via REST or bulk
  downloads.
- Raw data is captured *as received*, with receive timestamps, before any
  interpretation. Raw capture is the ground truth that everything else can
  be rebuilt from.
- Reconnection, sequence-gap detection, and snapshot resynchronization
  belong here, since a corrupted feed poisons every downstream result.

**Connects to:** feeds raw messages to the processor (3) and raw archives to
storage (5).

### 3. Market-data processing

**Goal:** turn raw exchange messages into clean, normalized, exchange-neutral
events.

- Parsers per exchange that produce a common event set: `Trade`,
  `BookSnapshot`, `BookDelta`, `Candle`, `Heartbeat`.
- Normalization: symbol mapping, unit scaling, timestamp alignment, side
  conventions.
- Validation: monotonic sequence numbers, sane prices/sizes, duplicate
  suppression, gap flags.
- Derived data that strategies commonly need: candle aggregation across
  timeframes, rolling statistics, volume profiles. These are computed
  incrementally from the event stream so the same code serves replay and
  live.

**Connects to:** consumes from the collector (2) or the replayer (6); emits
normalized events onto the event bus consumed by the order book (4),
strategies (10), and storage (5).

### 4. Order-book system

**Goal:** maintain an accurate local limit order book from snapshots and
deltas, and expose it to everything that needs market state.

- Reconstructs the full book (or top-N levels) from snapshot + deltas,
  handles resync when gaps are detected.
- Provides queries: best bid/ask, mid, spread, depth at a level, cumulative
  liquidity to a size, imbalance metrics.
- Designed as a pure data structure driven by events, so it is identical in
  backtest and live.
- This same book is later used by the simulated exchange (7) to decide how
  a simulated order would have filled, which is what makes execution
  realistic instead of "fill at last price."

**Connects to:** driven by processed events (3); read by strategies (10),
the simulated exchange (7), and risk (9).

### 5. Historical data storage

**Goal:** durable, fast, append-only storage of everything the system has
seen, so any past period can be replayed exactly.

- Two tiers: **raw archives** (compressed message logs, immutable) and
  **normalized columnar stores** (trades, book events, candles) optimized
  for sequential time-range reads.
- A catalog/index describing what data exists for which symbol, venue,
  and time range, with data-quality flags (gaps, resyncs).
- Versioned schemas so old data remains readable as the format evolves.
- Tools for ingest, verification, and backfill.

**Connects to:** written by collector (2) and processor (3); read by the
replayer (6) and analytics (13).

### 6. Historical market replay

**Goal:** replay stored data as a stream of events indistinguishable to
consumers from a live feed.

- A **replay clock**: a simulated time source that advances with the events.
  Strategies and the simulated exchange never read wall-clock time; they
  read *the* clock, which is either simulated or real.
- Supports as-fast-as-possible replay for backtests and paced replay for
  visual debugging.
- Multi-stream merging (trades + book + candles) in strict timestamp order,
  with deterministic tie-breaking.
- Optional injected **latency model**: events reach the strategy slightly
  after they "happened," and orders reach the exchange slightly after they
  are sent. This is essential for honesty about what a live system would
  actually have seen.

**Connects to:** reads storage (5); emits normalized events into the same
pipeline as (3); drives the simulated exchange (7) and strategies (10).

### 7. Simulated exchange

**Goal:** a matching engine that behaves like a real exchange did at that
moment in history, so backtests approximate real fills rather than
fantasy fills.

- Implements the abstract **Execution Interface**: submit, amend, cancel,
  query orders; emits acknowledgements, rejections, partial and full fills.
- Uses the reconstructed order book (4) and trade stream to determine
  fills: market orders walk the book and consume liquidity (with slippage);
  limit orders are filled only when the historical market trades through
  or at their price, with a configurable queue-position model.
- Applies exchange rules: fees (maker/taker), tick and lot sizes, minimum
  notional, order types, time-in-force, self-trade prevention.
- Models latency (order-to-ack, ack-to-fill) and occasional rejections.
- Models the *impact* of your own order if it is large relative to book
  depth, so strategies cannot pretend infinite liquidity.
- Fully deterministic given the same input stream and seed.

**Connects to:** consumes market events (3/6) and approved orders from risk
(9); emits execution reports to portfolio (8), strategies (10), and
analytics (13).

### 8. Portfolio management

**Goal:** the single source of truth for what you own and what it is worth.

- Tracks cash balances, positions, average entry price, realized and
  unrealized P&L, fees paid, and open-order exposure.
- Updated only from execution reports (fills), never from what a strategy
  *thinks* happened.
- Marks positions to market from the current book/last price.
- Supports multiple strategies sharing one account with per-strategy
  sub-ledgers, so you can run many candidates side by side and attribute
  results.
- Reconciliation hooks: in live trading, periodically compares internal
  state to the exchange's reported state and flags discrepancies.

**Connects to:** fed by execution reports (7/15/20); read by risk (9),
strategies (10), and analytics (13).

### 9. Risk management

**Goal:** a mandatory gatekeeper between strategy intent and the exchange.
Strategies *propose*; risk *disposes*.

- Pre-trade checks on every order: max position size, max order size, max
  notional, price sanity (fat-finger bands), max open orders, rate limits,
  allowed instruments, trading-hours or kill-switch state.
- Portfolio-level limits: max drawdown, daily loss limit, max leverage or
  exposure, concentration.
- A **kill switch** that flattens and halts, triggerable manually or by
  breach of limits, feed staleness, or reconciliation failure.
- Runs identically in backtest and live. A strategy that violates risk in
  backtest is rejected there too, so backtests reflect what would really
  have been allowed.

**Connects to:** sits on the order path between strategies (10) and any
execution backend (7/15/20); reads portfolio (8) and market state (4).

### 10. Strategy framework

**Goal:** the abstraction that makes strategies portable across every
environment.

- A strategy is an object that receives events (`onTrade`, `onBook`,
  `onCandle`, `onTimer`, `onFill`, `onOrderUpdate`) and can call a narrow
  **Strategy Context** API: read market state, read its own positions and
  orders, submit/cancel orders, schedule timers, log, and emit metrics.
- The context is the *only* door to the outside world. The strategy never
  touches sockets, clocks, files, or exchange-specific types.
- Strategy lifecycle: configure (from parameters), start, run, stop, with
  parameters externalized so the same strategy can be swept.
- Indicator library (moving averages, volatility, order-flow measures, etc.)
  implemented as incremental, stateful calculators shared by all
  strategies.
- Multi-strategy runner that hosts many strategies on one event stream with
  isolated state and per-strategy ledgers.

**Connects to:** receives events from the bus (3/6/15); sends order intents
to risk (9); reads portfolio (8) and book (4).

### 11. Initial trading strategies

**Goal:** simple, well-understood baselines to exercise the whole pipeline
and serve as reference points. None are expected to be profitable.

- Buy-and-hold (the benchmark everything must beat after costs).
- Moving-average crossover / trend following.
- Mean reversion on a z-score of price.
- Simple breakout.
- A random strategy, as a sanity control: if it appears profitable, the
  simulator is broken.

**Connects to:** implemented against the framework (10); run by the
backtester (12).

### 12. Backtesting

**Goal:** an engine that wires replay, simulated exchange, portfolio, risk,
and strategies into a single deterministic run, and records everything.

- Run specification: symbol, time range, data sources, strategy +
  parameters, exchange model settings (fees, latency, slippage), initial
  capital, random seed.
- Produces a complete artifact: trade log, order log, equity curve,
  positions over time, rejected orders, and the run configuration, so any
  result can be reproduced and audited.
- Batch runner for parameter sweeps and multi-period runs, with parallel
  execution across cores.
- Guardrails against look-ahead bias: strategies only ever see events up to
  the simulated "now."

**Connects to:** orchestrates (6), (7), (8), (9), (10); emits artifacts to
analytics (13).

### 13. Performance analytics

**Goal:** turn run artifacts into honest, comparable numbers.

- Return metrics: total and annualized return, volatility, Sharpe, Sortino,
  Calmar, max drawdown and duration, win rate, profit factor, expectancy,
  average holding time, turnover, fee drag.
- Trade-level analysis: per-trade P&L distribution, MAE/MFE, slippage vs.
  intended price.
- Comparison against benchmarks (buy-and-hold, random).
- Reports and plots exported for review; a common schema so backtest, paper,
  and live results are compared with identical metrics.

**Connects to:** consumes artifacts from (12), (15), (20); feeds research
(14) and validation (19).

### 14. Strategy research and experimentation

**Goal:** a disciplined workflow for generating and testing many ideas
without fooling yourself.

- Parameter sweeps and grid/random searches over the batch backtester.
- Walk-forward and rolling-window evaluation built in from the start.
- Experiment tracking: every run has an ID, config, data version, code
  version, and results, stored so results are never re-derived from memory.
- Notebooks or scripts (a scripting layer may call the C++ engine) for
  exploration, while the engine itself stays in C++.
- A short list of "kill criteria" agreed in advance: a strategy is dropped
  if it fails them, no matter how good one backtest looks.

**Connects to:** drives (12); consumes (13); produces candidates for (15).

### 15. Real-time paper trading

**Goal:** run surviving strategies against the *live* market with simulated
fills, using the exact same strategy code.

- Live collector (2) + processor (3) + order book (4) feed the strategy in
  real time on the wall clock.
- The simulated exchange (7) runs in "live mode": it fills simulated orders
  against the live book and trade stream, with the same fee/latency/slippage
  models.
- Persistent state: positions and results survive restarts.
- Results flow into the same analytics (13) so forward performance is
  directly compared with the backtest for the same period.
- This is the second research phase: strategies must hold up here for a
  meaningful duration before anything else is considered.

**Connects to:** the first time (2)/(3)/(4) run live against (7)/(8)/(9)/(10).

### 16. Reliability and error handling

**Goal:** make the long-running processes trustworthy.

- Feed health: heartbeats, staleness detection, automatic reconnect and
  resync, alerting.
- Crash safety: journaling of orders and fills so state can be rebuilt;
  idempotent order submission with client order IDs.
- Clear failure policy: on uncertainty (unknown order state, stale data),
  the system goes *safe* (cancel, halt) rather than *guess*.
- Structured error types, no silent catch-and-continue, and a watchdog
  process.
- Comprehensive tests: unit, property-based tests for the book and matching
  engine, deterministic replay regression tests, and chaos tests that
  inject disconnects and malformed messages.

**Connects to:** cross-cutting across (2)–(15), hardened before live.

### 17. Performance optimization

**Goal:** make backtests fast enough to run thousands of experiments and
the live path fast enough that latency modelling is realistic.

- Profile first; optimize the measured hot paths: data decoding, book
  updates, event dispatch, storage reads.
- Techniques as needed: cache-friendly layouts, preallocation and
  arena/pool allocation, avoiding dynamic dispatch in the inner loop,
  lock-free queues between threads, memory-mapped data files, parallel
  backtests across cores.
- Benchmarks checked in and run in CI to catch regressions.
- Determinism is preserved: optimization must not change results.

**Connects to:** cross-cutting; especially (3), (4), (6), (7), (12).

### 18. Advanced trading strategies

**Goal:** with a proven, fast pipeline, research more sophisticated ideas.

- Order-flow and microstructure signals (imbalance, trade aggressiveness).
- Volatility-regime-aware position sizing.
- Market-making style strategies (which stress the queue-position model and
  fee assumptions).
- Multi-timeframe and ensemble approaches.
- Optional statistical/ML models, trained strictly on past data with
  time-respecting splits.

**Connects to:** built on (10); evaluated via (12)–(15).

### 19. Robust strategy validation

**Goal:** separate real edge from noise and overfitting before risking money.

- Out-of-sample and walk-forward testing as a hard requirement.
- Sensitivity analysis: performance across neighboring parameters,
  different fee/slippage/latency assumptions, and different market regimes.
- Monte Carlo on trade sequences and bootstrapped confidence intervals.
- Multiple-comparison awareness: the more strategies tested, the higher the
  bar for the winner.
- Backtest vs. paper-trading consistency check for the same period.
- A formal go/no-go checklist that a strategy must pass.

**Connects to:** consumes (13), (14), (15); gates entry to (20).

### 20. Live trading infrastructure

**Goal:** a real exchange backend behind the *same* Execution Interface.

- Authenticated REST/websocket gateway: order entry, cancels, private fills,
  balances, and positions.
- Order-state machine that reconciles what we sent with what the exchange
  acknowledges, handling every edge case (partial fills, late fills after
  cancel, rejected amends).
- Continuous reconciliation of portfolio (8) against exchange state.
- Secrets management, separate API keys with minimal permissions, and a
  read-only mode.
- Small position sizes, explicit whitelists, and the risk kill switch (9)
  wired to real cancellation.

**Connects to:** plugs into the Execution Interface; strategies and risk are
unchanged.

### 21. Final testing

**Goal:** prove the live path end-to-end before real capital.

- Exchange testnet/sandbox runs with the full live stack.
- Shadow mode: live strategy generates orders that are logged but not
  sent, compared with what paper trading would have done.
- Failure drills: kill the feed, kill the process, revoke the API key,
  simulate exchange downtime; verify safe behavior every time.
- Tiny-capital live run with hard limits, compared daily against paper
  results.

### 22. Deployment

**Goal:** run the system reliably and reproducibly.

- Containerized builds, pinned dependencies, immutable release artifacts.
- Separate deployments for collector, paper trader, and live trader so a
  crash in one does not take out the others.
- Configuration per environment, secrets injected at runtime.
- Time synchronization, log shipping, metrics export, alerting.
- Documented runbooks: start, stop, flatten, roll back.

### 23. Continuous monitoring and strategy development

**Goal:** treat live trading as an ongoing experiment, not a finish line.

- Dashboards: feed health, latency, P&L, exposure, risk-limit headroom, and
  live vs. expected performance.
- Automatic drift detection: if live performance diverges from paper and
  backtest expectations, reduce size or halt.
- Continuous data collection keeps enlarging the historical store, so
  strategies are periodically re-validated on newer data.
- The research loop (14) → (19) → (15) → (20) runs continuously; strategies
  are retired as readily as they are promoted.

---

## How the pieces fit: the three run modes

| Concern           | Backtest                 | Paper trading                 | Live                          |
|-------------------|--------------------------|-------------------------------|-------------------------------|
| Market data       | Replay from storage (6)  | Live collector (2/3)          | Live collector (2/3)          |
| Clock             | Simulated                | Wall clock                    | Wall clock                    |
| Order book        | Same (4)                 | Same (4)                      | Same (4)                      |
| Execution backend | Simulated exchange (7)   | Simulated exchange, live mode | Exchange gateway (20)         |
| Portfolio / Risk  | Same (8/9)               | Same (8/9)                    | Same (8/9) + reconciliation   |
| Strategy          | Same binary code (10)    | Same                          | Same                          |
| Analytics         | Same (13)                | Same                          | Same                          |

Only the rows "Market data," "Clock," and "Execution backend" change. That is
the whole point of the architecture.

## Suggested build order (summary)

1. Foundation and core types.
2. Collect and store real ETH data (start capturing early; data accrues while
   you build the rest).
3. Processor + order book, validated against captured data.
4. Replay + simulated exchange + portfolio + risk.
5. Strategy framework + baseline strategies + backtester + analytics.
6. Research tooling and the first bulk experiments (historical phase).
7. Paper trading (forward phase), reliability hardening, optimization.
8. Advanced strategies, rigorous validation.
9. Live gateway, final testing, deployment, monitoring.
