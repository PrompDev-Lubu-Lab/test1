# Strategy lifecycle

Live trading is an ongoing experiment. A strategy is a hypothesis that
moves through fixed stages, each with an objective gate, and it is retired
as readily as it is promoted. Every stage uses the same binary and the same
strategy code; only the data source and the venue behind the
`ExecutionVenue` interface change.

```
 candidate ──► research ──► paper ──► shadow ──► testnet ──► live ──► monitor
     ▲            │           │          │          │          │        │
     └────────────┴───────────┴──────────┴──────────┴──────────┴────────┘
                         retire (back to candidate, or gone)
```

## 1. Candidate

A new strategy is a class registered in `tradebot::strategies` with named
parameters, a `[strategy.params]` block, and a one-paragraph statement of
the edge it claims and the market condition it needs. Without that
statement there is nothing to falsify later. The random-control baseline
exists so that every candidate is compared against luck.

## 2. Research (Phases 12 to 14, 19)

```sh
tradebot-research sweep --config configs/backtest.conf --metric sharpe
tradebot-research walk-forward --config configs/backtest.conf --train 60d --test 14d
tradebot-research validate --config configs/backtest.conf --train 60d --test 14d
tradebot-research index
```

Gate: `validate` prints GO. The chosen parameters are frozen in
`configs/layers/strategy-<name>.conf` and the winning run directory is
kept as the **reference** for drift detection (`runs/<run_id>`). Every
run is in `runs/index.csv`; nothing is deleted, so the count of things
tried is visible when judging the survivor.

## 3. Paper (Phase 15)

`env-paper.conf`, two weeks minimum, unattended. Gate: backtest-vs-paper
consistency within tolerance and no operational surprises (reconnects,
kill-switch trips, restarts) that the runbook cannot explain.

## 4. Shadow (Phase 21)

`env-shadow.conf` with API keys: simulated fills, real account plumbing,
"would send" log. Gate: the checklist section in
`docs/PRE_LIVE_CHECKLIST.md`.

## 5. Testnet (Phase 21)

`env-testnet.conf`: real order lifecycle on the venue's testnet plus the
failure drills done by hand. Gate: the drills leave a consistent state
every time.

## 6. Live (Phases 20 to 22)

`env-live.conf` with `max_capital`, `confirm`, tiny capital, and someone
watching. The venue can only ever see what the risk gate lets through.

## 7. Monitor (Phase 23)

Three feedback loops run while a strategy is live (or paper, or shadow).

**Operational.** The runtime writes `metrics.prom` and `status.json` on
the heartbeat cadence; `tradebot-healthcheck` and the systemd watchdog
restart a stalled process; `tradebot-monitor status` prints the snapshot.
Point node_exporter's textfile collector at the run directory to scrape
the Prometheus series (`tradebot_equity`, `tradebot_drawdown`,
`tradebot_kill_switch_tripped`, `tradebot_feed_healthy`,
`tradebot_reconcile_mismatches_total`, ...).

**Drift.** `tradebot-monitor drift --run runs/live-x --reference runs/<ref>`
compares the running strategy with its research reference: return
z-score, drawdown ratio, fee drag, trade rate, win rate, rejection rate
and kill-switch trips, each graded ok/warn/alarm. The alarms are
deliberately mechanical; the response is human: a warning means watch, an
alarm means stop and re-validate.

**Re-validation.** `tradebot-monitor revalidate --run ... --reference ...
--config configs/live.conf` adds the kill criteria and a fresh backtest of
the same spec over the observed window (the collector has been archiving
the data the whole time, so the backtest sees exactly what the live run
saw). The decision is keep, watch or retire. The
`tradebot-revalidate@.timer` unit runs it daily; a non-zero exit is the
alarm.

## Retire

A strategy is retired when re-validation says so twice, when the market
condition its statement depends on has demonstrably changed, or when a
better candidate beats it on the same out-of-sample data. Retirement
means stop the instance, keep the run directory, and record the reason
in `runs/index.csv` (the `label` column carries it). Its parameters may
re-enter as a new candidate; its history does not.

## Cadence

| Loop           | How often        | Tool                                   |
|----------------|------------------|----------------------------------------|
| Heartbeat      | seconds          | `tradebot-healthcheck`, watchdog timer |
| Status         | on demand        | `tradebot-monitor status`              |
| Drift          | daily            | `tradebot-monitor drift`               |
| Re-validation  | daily / weekly   | `tradebot-monitor revalidate`          |
| Re-research    | monthly          | `tradebot-research` on the enlarged store |
| Model calibration | after any drift in fees, slippage or fill rate | exchange model parameters, then re-research |

The historical store grows every day the collector runs, so the
re-research step always has more out-of-sample data than the last one.
That is the point of starting the collector on day one.
