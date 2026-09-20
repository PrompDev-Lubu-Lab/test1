#include "tradebot/strategies/baselines.hpp"

#include <random>

namespace tradebot::strategies {

using market_data::Candle;
using strategy::StrategyContext;

namespace {

Duration param_interval(const StrategyContext& ctx) {
    return ctx.params().get_duration_or("interval", Duration::minutes(1)).value_or(Duration::minutes(1));
}

std::size_t param_size(const StrategyContext& ctx, const char* key, std::int64_t def) {
    const auto v = ctx.params().get_int_or(key, def).value_or(def);
    return static_cast<std::size_t>(v < 1 ? 1 : v);
}

}  // namespace

// --- CandleStrategy ---------------------------------------------------------

void CandleStrategy::on_start(StrategyContext& ctx) {
    Strategy::on_start(ctx);
    interval_ = param_interval(ctx);
    if (auto q = ctx.params().get_quantity("quantity"); q) {
        fixed_quantity_ = *q;
    }
    equity_fraction_ = ctx.params().get_double_or("equity_fraction", 0.95).value_or(0.95);
    ctx.request_candles(interval_);
}

void CandleStrategy::on_candle(const Candle& c) {
    if (c.closed && c.interval == interval_) {
        on_closed_candle(c);
    }
}

void CandleStrategy::on_execution_report(const execution::ExecutionReport& r) {
    if (pending_id_ && r.client_id == *pending_id_ && is_terminal(r.status)) {
        pending_ = false;
        pending_id_.reset();
    }
}

bool CandleStrategy::in_position() const { return ctx().position().is_positive(); }

Quantity CandleStrategy::entry_quantity(Price reference) const { return default_entry_quantity(reference); }

void CandleStrategy::submit_market(Side side, Quantity quantity) {
    if (pending_ || !quantity.is_positive()) {
        return;
    }
    auto id = side == Side::buy ? ctx().buy_market(quantity) : ctx().sell_market(quantity);
    if (id) {
        pending_ = true;
        pending_id_ = *id;
    }
}

Quantity CandleStrategy::default_entry_quantity(Price reference) const {
    if (fixed_quantity_.is_positive()) {
        return fixed_quantity_;
    }
    if (!reference.is_positive()) {
        return Quantity{};
    }
    // Size from the account's cash so several strategies cannot all spend
    // the same equity; the risk gate still has the final say.
    const Notional budget =
        ctx().cash().mul_ratio(static_cast<std::int64_t>(equity_fraction_ * 10000), 10000);
    const Quantity q = quantity_for(budget, reference, RoundingMode::down);
    return ctx().instrument().round_quantity(q, RoundingMode::down);
}

void CandleStrategy::enter_long() {
    if (pending_ || in_position()) {
        return;
    }
    const auto ref = ctx().mark().value_or(ctx().last_trade_price().value_or(Price{}));
    const Quantity q = entry_quantity(ref);
    if (!q.is_positive()) {
        return;
    }
    if (auto id = ctx().buy_market(q); id) {
        pending_ = true;
        pending_id_ = *id;
    }
}

void CandleStrategy::exit_long() {
    if (pending_ || !in_position()) {
        return;
    }
    if (auto id = ctx().sell_market(ctx().position()); id) {
        pending_ = true;
        pending_id_ = *id;
    }
}

// --- BuyAndHold ------------------------------------------------------------

void BuyAndHold::on_closed_candle(const Candle&) { enter_long(); }

// --- MaCrossover -----------------------------------------------------------

void MaCrossover::on_start(StrategyContext& ctx) {
    CandleStrategy::on_start(ctx);
    fast_ = std::make_unique<strategy::Sma>(param_size(ctx, "fast", 10));
    slow_ = std::make_unique<strategy::Sma>(param_size(ctx, "slow", 30));
}

void MaCrossover::on_closed_candle(const Candle& c) {
    const double close = c.close.to_double();
    fast_->push(close);
    slow_->push(close);
    if (!slow_->ready()) {
        return;
    }
    const bool above = *fast_->value() > *slow_->value();
    ctx().metric("fast_minus_slow", *fast_->value() - *slow_->value());
    if (fast_above_ && above != *fast_above_) {
        if (above) {
            enter_long();
        } else {
            exit_long();
        }
    }
    fast_above_ = above;
}

// --- MeanReversion ---------------------------------------------------------

void MeanReversion::on_start(StrategyContext& ctx) {
    CandleStrategy::on_start(ctx);
    stats_ = std::make_unique<strategy::RollingStats>(param_size(ctx, "lookback", 50));
    entry_z_ = ctx.params().get_double_or("entry_z", -2.0).value_or(-2.0);
    exit_z_ = ctx.params().get_double_or("exit_z", 0.0).value_or(0.0);
}

void MeanReversion::on_closed_candle(const Candle& c) {
    stats_->push(c.close.to_double());
    const auto z = stats_->zscore();
    if (!z) {
        return;
    }
    ctx().metric("zscore", *z);
    if (*z <= entry_z_) {
        enter_long();
    } else if (*z >= exit_z_) {
        exit_long();
    }
}

// --- Breakout ---------------------------------------------------------------

void Breakout::on_start(StrategyContext& ctx) {
    CandleStrategy::on_start(ctx);
    highs_ = std::make_unique<strategy::RollingExtrema>(param_size(ctx, "entry_lookback", 20));
    lows_ = std::make_unique<strategy::RollingExtrema>(param_size(ctx, "exit_lookback", 10));
}

void Breakout::on_closed_candle(const Candle& c) {
    // Compare this close against the extremes of the *previous* candles.
    const auto prior_high = highs_->max();
    const auto prior_low = lows_->min();
    const double close = c.close.to_double();
    highs_->push(c.high.to_double());
    lows_->push(c.low.to_double());
    if (prior_high && close > *prior_high) {
        enter_long();
    } else if (prior_low && close < *prior_low) {
        exit_long();
    }
}

// --- RandomStrategy -----------------------------------------------------------

void RandomStrategy::on_start(StrategyContext& ctx) {
    CandleStrategy::on_start(ctx);
    flip_probability_ = ctx.params().get_double_or("flip_probability", 0.1).value_or(0.1);
}

void RandomStrategy::on_closed_candle(const Candle&) {
    std::bernoulli_distribution flip(flip_probability_);
    if (!flip(ctx().rng())) {
        return;
    }
    if (in_position()) {
        exit_long();
    } else {
        enter_long();
    }
}

// --- registry -----------------------------------------------------------------

void register_baselines(strategy::StrategyRegistry& registry) {
    registry.add("buy_and_hold", [] { return std::make_unique<BuyAndHold>(); });
    registry.add("ma_crossover", [] { return std::make_unique<MaCrossover>(); });
    registry.add("mean_reversion", [] { return std::make_unique<MeanReversion>(); });
    registry.add("breakout", [] { return std::make_unique<Breakout>(); });
    registry.add("random", [] { return std::make_unique<RandomStrategy>(); });
}

}  // namespace tradebot::strategies
