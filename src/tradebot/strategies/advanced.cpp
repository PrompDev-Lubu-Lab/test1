#include "tradebot/strategies/advanced.hpp"

#include <algorithm>
#include <cmath>

namespace tradebot::strategies {

using market_data::Candle;
using strategy::StrategyContext;

namespace {

std::size_t size_param(const StrategyContext& ctx, const char* key, std::int64_t def) {
    const auto v = ctx.params().get_int_or(key, def).value_or(def);
    return static_cast<std::size_t>(v < 1 ? 1 : v);
}

double dbl_param(const StrategyContext& ctx, const char* key, double def) {
    return ctx.params().get_double_or(key, def).value_or(def);
}

std::int64_t int_param(const StrategyContext& ctx, const char* key, std::int64_t def) {
    return ctx.params().get_int_or(key, def).value_or(def);
}

Duration dur_param(const StrategyContext& ctx, const char* key, Duration def) {
    return ctx.params().get_duration_or(key, def).value_or(def);
}

Quantity qty_param(const StrategyContext& ctx, const char* key, Quantity def) {
    auto q = ctx.params().get_quantity(key);
    return q ? *q : def;
}

}  // namespace

// --- VolTrend ---------------------------------------------------------------

void VolTrend::on_start(StrategyContext& ctx) {
    CandleStrategy::on_start(ctx);
    trend_ = std::make_unique<strategy::Ema>(size_param(ctx, "trend", 50));
    atr_ = std::make_unique<strategy::Atr>(size_param(ctx, "atr", 14));
    risk_per_trade_ = dbl_param(ctx, "risk_per_trade", 0.01);
    atr_stop_mult_ = dbl_param(ctx, "atr_stop_mult", 2.0);
    max_equity_fraction_ = dbl_param(ctx, "max_equity_fraction", 0.95);
}

Quantity VolTrend::entry_quantity(Price reference) const {
    const auto atr = atr_ ? atr_->value() : std::nullopt;
    if (!atr || *atr <= 0.0 || !reference.is_positive()) {
        return Quantity{};
    }
    // Risk budget / stop distance, capped at a fraction of available cash.
    const double cash = std::max(0.0, ctx().cash().to_double());
    const double budget = cash * risk_per_trade_;
    const double stop_distance = *atr * atr_stop_mult_;
    double qty = budget / stop_distance;
    const double cap = cash * max_equity_fraction_ / reference.to_double();
    qty = std::min(qty, cap);
    return ctx().instrument().round_quantity(Quantity::from_double(qty), RoundingMode::down);
}

void VolTrend::on_closed_candle(const Candle& c) {
    trend_->push(c.close.to_double());
    atr_->push(c);
    if (!trend_->ready() || !atr_->ready()) {
        return;
    }
    const double close = c.close.to_double();
    const bool above = close > *trend_->value();
    ctx().metric("atr", *atr_->value());
    if (in_position()) {
        if (!above || (stop_ && c.close < *stop_)) {
            exit_long();
            stop_.reset();
        }
        return;
    }
    if (above) {
        enter_long();
        if (order_pending()) {
            stop_ = Price::from_double(close - *atr_->value() * atr_stop_mult_);
        }
    }
}

// --- BookImbalance ------------------------------------------------------------

void BookImbalance::on_start(StrategyContext& ctx) {
    Strategy::on_start(ctx);
    levels_ = size_param(ctx, "levels", 5);
    threshold_ = dbl_param(ctx, "threshold", 0.6);
    confirm_ = static_cast<int>(int_param(ctx, "confirm", 3));
    min_interval_ = dur_param(ctx, "min_interval", Duration::seconds(1));
    max_hold_ = dur_param(ctx, "max_hold", Duration::minutes(5));
    quantity_ = qty_param(ctx, "quantity", Quantity::from_raw(10'000'000));  // 0.1
}

void BookImbalance::enter() {
    if (pending_ || ctx().position().is_positive()) return;
    if (auto id = ctx().buy_market(quantity_)) {
        pending_ = *id;
        last_action_ = ctx().now();
    }
}

void BookImbalance::exit() {
    if (pending_ || !ctx().position().is_positive()) return;
    if (auto id = ctx().sell_market(ctx().position())) {
        pending_ = *id;
        last_action_ = ctx().now();
    }
}

void BookImbalance::on_book_update() {
    if (!ctx().book_synced() || ctx().book().empty()) return;
    const double imb = ctx().book().imbalance(levels_);
    ctx().metric("imbalance", imb);
    if (imb >= threshold_) {
        streak_ = streak_ > 0 ? streak_ + 1 : 1;
    } else if (imb <= -threshold_) {
        streak_ = streak_ < 0 ? streak_ - 1 : -1;
    } else {
        streak_ = 0;
    }
    if (ctx().now() - last_action_ < min_interval_) return;
    if (streak_ >= confirm_ && !ctx().position().is_positive()) {
        enter();
    } else if (streak_ <= -confirm_ && ctx().position().is_positive()) {
        exit();
    }
}

void BookImbalance::on_execution_report(const execution::ExecutionReport& r) {
    if (pending_ && r.client_id == *pending_ && is_terminal(r.status)) {
        pending_.reset();
        if (r.type == execution::ReportType::fill || r.status == OrderStatus::filled) {
            if (r.side == Side::buy && ctx().position().is_positive()) {
                entered_at_ = ctx().now();
                hold_timer_ = ctx().schedule_at(ctx().now() + max_hold_, [this](Timestamp) {
                    if (ctx().position().is_positive()) {
                        exit();
                    }
                });
            } else if (r.side == Side::sell) {
                entered_at_.reset();
                if (hold_timer_) ctx().cancel_timer(hold_timer_);
            }
        }
    }
}

// --- MarketMaker ------------------------------------------------------------------

void MarketMaker::on_start(StrategyContext& ctx) {
    Strategy::on_start(ctx);
    half_spread_bps_ = int_param(ctx, "half_spread_bps", 10);
    skew_bps_ = int_param(ctx, "skew_bps", 5);
    quote_size_ = qty_param(ctx, "quote_size", Quantity::from_raw(10'000'000));  // 0.1
    max_inventory_ = qty_param(ctx, "max_inventory", Quantity::from_raw(50'000'000));  // 0.5
    requote_interval_ = dur_param(ctx, "requote_interval", Duration::seconds(1));
    requote_ticks_ = int_param(ctx, "requote_ticks", 2);
    timer_ = ctx.schedule_every(requote_interval_, [this](Timestamp t) { requote(t); });
}

void MarketMaker::on_stop() {
    if (started()) {
        stopping_ = true;
        ctx().cancel_timer(timer_);
        if (quotes_.bid) static_cast<void>(ctx().cancel(*quotes_.bid));
        if (quotes_.ask) static_cast<void>(ctx().cancel(*quotes_.ask));
    }
}

void MarketMaker::on_book_update() {
    if (!ctx().book_synced()) return;
    const auto mid = ctx().book().mid_price();
    if (!mid) return;
    if (!quoted_mid_) {
        requote(ctx().now());
        return;
    }
    const std::int64_t moved = std::llabs(mid->raw() - quoted_mid_->raw()) / ctx().instrument().tick_size.raw();
    if (moved >= requote_ticks_) {
        requote(ctx().now());
    }
}

void MarketMaker::requote(Timestamp now) {
    if (stopping_ || !ctx().book_synced()) return;
    const auto mid = ctx().book().mid_price();
    if (!mid) return;
    const Instrument& inst = ctx().instrument();
    const Quantity inventory = ctx().position();
    // Skew: long inventory lowers both quotes (eager to sell), short raises them.
    const double inv_ratio = max_inventory_.is_positive() ? ratio(inventory, max_inventory_) : 0.0;
    const std::int64_t skew = static_cast<std::int64_t>(std::llround(-inv_ratio * static_cast<double>(skew_bps_)));
    quotes_.bid_price = inst.round_price(mid->mul_ratio(10'000 - half_spread_bps_ + skew, 10'000), RoundingMode::down);
    quotes_.ask_price = inst.round_price(mid->mul_ratio(10'000 + half_spread_bps_ + skew, 10'000), RoundingMode::up);
    quoted_mid_ = *mid;
    last_quote_ = now;
    refresh_side(Side::buy);
    refresh_side(Side::sell);
    ctx().metric("inventory", inventory.to_double());
}

void MarketMaker::refresh_side(Side side) {
    auto& id = side == Side::buy ? quotes_.bid : quotes_.ask;
    const Price target = side == Side::buy ? quotes_.bid_price : quotes_.ask_price;
    if (id) {
        auto state = ctx().open_orders();
        for (const auto& o : state) {
            if (o.request.client_id == *id) {
                if (o.request.price == target) return;  // already there
                static_cast<void>(ctx().cancel(*id));  // replaced on the cancel report
                return;
            }
        }
        id.reset();  // no longer working
    }
    place_side(side);
}

void MarketMaker::place_side(Side side) {
    if (stopping_) return;
    const Quantity inventory = ctx().position();
    if (side == Side::buy) {
        if (inventory >= max_inventory_) return;
        if (auto id = ctx().buy_limit(quote_size_, quotes_.bid_price, TimeInForce::post_only)) quotes_.bid = *id;
    } else {
        if (!inventory.is_positive()) return;  // spot: can only offer what we hold
        const Quantity ask_size = std::min(quote_size_, inventory);
        if (auto id = ctx().sell_limit(ask_size, quotes_.ask_price, TimeInForce::post_only)) quotes_.ask = *id;
    }
}

void MarketMaker::on_execution_report(const execution::ExecutionReport& r) {
    if (!is_terminal(r.status)) return;
    const bool was_bid = quotes_.bid && r.client_id == *quotes_.bid;
    const bool was_ask = quotes_.ask && r.client_id == *quotes_.ask;
    if (was_bid) quotes_.bid.reset();
    if (was_ask) quotes_.ask.reset();
    if (stopping_) return;
    if (r.type == execution::ReportType::fill || r.status == OrderStatus::filled) {
        requote(ctx().now());  // inventory changed: refresh the skew on both sides
    } else if (was_bid || was_ask) {
        // Cancelled / rejected / expired: re-place that side at the current target.
        place_side(was_bid ? Side::buy : Side::sell);
    }
}

// --- SignalEnsemble ------------------------------------------------------------

void SignalEnsemble::on_start(StrategyContext& ctx) {
    CandleStrategy::on_start(ctx);
    fast_ = std::make_unique<strategy::Ema>(size_param(ctx, "fast", 10));
    slow_ = std::make_unique<strategy::Ema>(size_param(ctx, "slow", 30));
    highs_ = std::make_unique<strategy::RollingExtrema>(size_param(ctx, "breakout", 20));
    rsi_ = std::make_unique<strategy::Rsi>(size_param(ctx, "rsi", 14));
    min_votes_ = static_cast<int>(int_param(ctx, "min_votes", 2));
    rsi_buy_ = dbl_param(ctx, "rsi_buy", 55.0);
}

void SignalEnsemble::on_closed_candle(const Candle& c) {
    const double close = c.close.to_double();
    const auto prior_high = highs_->max();
    fast_->push(close);
    slow_->push(close);
    highs_->push(c.high.to_double());
    rsi_->push(close);
    if (!slow_->ready() || !rsi_->ready() || !prior_high) {
        return;
    }
    int votes = 0;
    if (*fast_->value() > *slow_->value()) ++votes;
    if (close > *prior_high) ++votes;
    if (*rsi_->value() > rsi_buy_) ++votes;
    ctx().metric("votes", votes);
    if (votes >= min_votes_) {
        enter_long();
    } else if (votes == 0) {
        exit_long();
    }
}

void register_advanced(strategy::StrategyRegistry& registry) {
    registry.add("vol_trend", [] { return std::make_unique<VolTrend>(); });
    registry.add("book_imbalance", [] { return std::make_unique<BookImbalance>(); });
    registry.add("market_maker", [] { return std::make_unique<MarketMaker>(); });
    registry.add("ensemble", [] { return std::make_unique<SignalEnsemble>(); });
}

}  // namespace tradebot::strategies
