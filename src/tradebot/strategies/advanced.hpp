#pragma once

// Advanced strategies. Like the baselines, none is assumed profitable; they
// exercise parts of the pipeline the baselines do not touch: the order
// book, resting post-only quotes (queue model and maker fees), volatility
// scaled sizing and signal combination.
//
//   vol_trend        EMA trend filter with ATR-scaled position size and an
//                    ATR stop: risk_per_trade of equity per position
//   book_imbalance   enters when top-of-book imbalance stays one-sided,
//                    exits on the opposite reading or after max_hold
//   market_maker     two-sided post-only quotes around the mid with an
//                    inventory skew; requotes when the mid moves
//   ensemble         votes across an EMA trend, a breakout and RSI

#include "tradebot/strategies/baselines.hpp"
#include "tradebot/strategy/indicators.hpp"
#include "tradebot/strategy/strategy.hpp"

#include <deque>
#include <memory>
#include <optional>

namespace tradebot::strategies {

class VolTrend final : public CandleStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "vol_trend"; }
    void on_start(strategy::StrategyContext& ctx) override;

protected:
    void on_closed_candle(const market_data::Candle& c) override;
    [[nodiscard]] Quantity entry_quantity(Price reference) const override;

private:
    std::unique_ptr<strategy::Ema> trend_;
    std::unique_ptr<strategy::Atr> atr_;
    double risk_per_trade_ = 0.01;  // fraction of equity lost if the stop is hit
    double atr_stop_mult_ = 2.0;
    double max_equity_fraction_ = 0.95;
    std::optional<Price> stop_;
};

class BookImbalance final : public strategy::Strategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "book_imbalance"; }
    void on_start(strategy::StrategyContext& ctx) override;
    void on_book_update() override;
    void on_execution_report(const execution::ExecutionReport& r) override;

private:
    void enter();
    void exit();

    std::size_t levels_ = 5;
    double threshold_ = 0.6;
    int confirm_ = 3;  // consecutive one-sided readings before acting
    Duration min_interval_ = Duration::seconds(1);
    Duration max_hold_ = Duration::minutes(5);
    Quantity quantity_;
    int streak_ = 0;
    Timestamp last_action_;
    std::optional<Timestamp> entered_at_;
    std::optional<ClientOrderId> pending_;
    TimerId hold_timer_ = 0;
};

class MarketMaker final : public strategy::Strategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "market_maker"; }
    void on_start(strategy::StrategyContext& ctx) override;
    void on_stop() override;
    void on_book_update() override;
    void on_execution_report(const execution::ExecutionReport& r) override;

    struct Quotes {
        std::optional<ClientOrderId> bid;
        std::optional<ClientOrderId> ask;
        Price bid_price;
        Price ask_price;
    };
    [[nodiscard]] const Quotes& quotes() const noexcept { return quotes_; }

private:
    void requote(Timestamp now);
    // Places one side at its target if nothing is working there. A working
    // quote at a different price is cancelled first and replaced when the
    // cancellation is confirmed, so the risk gate never sees both.
    void refresh_side(Side side);
    void place_side(Side side);

    std::int64_t half_spread_bps_ = 10;
    std::int64_t skew_bps_ = 5;  // per unit of inventory / max_inventory
    Quantity quote_size_;
    Quantity max_inventory_;
    Duration requote_interval_ = Duration::seconds(1);
    std::int64_t requote_ticks_ = 2;  // requote when the mid moved this many ticks
    Quotes quotes_;
    std::optional<Price> quoted_mid_;
    Timestamp last_quote_;
    TimerId timer_ = 0;
    bool stopping_ = false;
};

class SignalEnsemble final : public CandleStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "ensemble"; }
    void on_start(strategy::StrategyContext& ctx) override;

protected:
    void on_closed_candle(const market_data::Candle& c) override;

private:
    std::unique_ptr<strategy::Ema> fast_;
    std::unique_ptr<strategy::Ema> slow_;
    std::unique_ptr<strategy::RollingExtrema> highs_;
    std::unique_ptr<strategy::Rsi> rsi_;
    int min_votes_ = 2;
    double rsi_buy_ = 55.0;
};

void register_advanced(strategy::StrategyRegistry& registry);

}  // namespace tradebot::strategies
