# Pre-live checklist

Nothing in this repository is known to be profitable. This list exists so
that the first real order is sent by a process that has already survived
every failure the code can simulate, with an amount of money whose total
loss is acceptable. Work through the sections in order; each one is a gate
for the next.

The items marked **enforced** are checked by `tradebot-live --check` and
again by the runtime before it connects. The rest are yours.

## 1. Research gate

- [ ] The strategy passed `tradebot-research judge` on out-of-sample data
      and `tradebot-research validate` (Monte Carlo, cost and latency
      sensitivity, parameter stability, regime split) with a go decision.
- [ ] It beat the random-control baseline by a margin that survives the
      cost-sensitivity sweep.
- [ ] Its parameters are frozen in a config file under version control.

## 2. Paper gate (`mode = paper`)

- [ ] Ran for at least two weeks unattended; no crash, no unexplained
      restart, heartbeat never older than `heartbeat_interval` × 3.
- [ ] Backtest-vs-paper consistency check (`tradebot-research validate`)
      on the same period shows the same trades and returns within the
      cost and latency tolerances.
- [ ] Kill switch tripped and re-armed at least once (pull the network
      cable) and the run resumed correctly from `state.json`.

## 3. Shadow gate (`mode = shadow`)

Shadow runs simulated fills but connects to the real account read-only.

- [ ] Preflight passes: venue reachable, clock skew under
      `live.max_clock_skew`, balances readable, user data stream connects.
- [ ] Every "SHADOW would send" line in the log is an order you would have
      sent by hand: right side, size, price, symbol.
- [ ] The API key has trading permission only: no withdrawals, no futures,
      no margin. IP restriction enabled.

## 4. Testnet gate (`mode = testnet`)

- [ ] Testnet keys in the environment; `live.rest_base` is not the
      production venue (**enforced**).
- [ ] At least one full order lifecycle observed: ack on REST, fill on the
      user stream, portfolio and `journal.jsonl` agree, periodic
      reconciliation reports no mismatch.
- [ ] Failure drills passed by hand, each ending in a consistent state:
  - kill the process with a working order: on restart the order is
    cancelled at start (`startup_cancels`) and the position matches;
  - stop the process normally with a working order: it is cancelled
    (`shutdown_cancels`);
  - block the market-data feed: the kill switch trips, working orders are
    cancelled, positions are flattened if `risk.flatten_on_trip`;
  - change a balance on the account behind the bot's back: reconciliation
    trips the kill switch after `reconcile_mismatches_to_trip` rounds;
  - delete `state.json`: the portfolio is rebuilt from the journal.

## 5. Live gate (`mode = live`)

- [ ] `live.confirm` carries the exact confirmation phrase (**enforced**).
- [ ] `live.max_capital` is set to the most this deployment may ever
      control, and `initial_cash`, `risk.max_order_notional` and
      `risk.max_daily_loss` are at or below it (**enforced**).
- [ ] `risk.max_position` (or `max_position_notional`), `max_drawdown`,
      `max_daily_loss` and `max_orders_per_minute` are set;
      `allow_short` is off; `resume` is on (**enforced**).
- [ ] `initial_cash` equals the quote balance on the account and the base
      balance is zero (or the tolerances cover it), otherwise the startup
      reconciliation refuses to trade.
- [ ] `TRADEBOT_BINANCE_API_KEY` / `_SECRET` are production keys with
      trading permission only, stored outside the repository.
- [ ] The amount is tiny: an amount whose complete loss changes nothing.
- [ ] Someone is watching the first session end to end and knows how to
      stop it (`SIGTERM`; working orders are cancelled on the way out).
- [ ] The monitoring from Phase 23 is running against this deployment.

## After the first live day

- [ ] `journal.jsonl` and the venue's trade history agree line by line.
- [ ] Fees, slippage and fill rates match what the paper run assumed; if
      not, feed the measured numbers back into the exchange model and
      re-run the research gate before adding capital.
