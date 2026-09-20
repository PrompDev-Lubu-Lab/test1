#pragma once

// Baseline strategies. None is expected to be profitable; they exist to
// exercise the pipeline end to end and to serve as reference points:
//
//   buy_and_hold    the benchmark every active strategy must beat after costs
//   ma_crossover    trend following on fast/slow moving averages of closes
//   mean_reversion  fade z-score extremes of the close vs a rolling mean
//   breakout        buy new N-candle highs, exit on new N-candle lows
//   random          coin-flip entries; if this looks profitable the
//                   simulator is broken
//
// All trade one instrument on closed candles of a configurable interval
// (built from trades by the runner), size orders as a fraction of equity
// or a fixed quantity, and use market orders so fills are unambiguous.
// Parameters come from the strategy's Config section.

#include "tradebot/strategy/indicators.hpp"
#include "tradebot/strategy/strategy.hpp"

#include <memory>
#include <optional>

namespace tradebot::strategies {

// Shared plumbing: candle subscription, sizing, market entry/exit.
class CandleStrategy : public strategy::Strategy {
public:
    void on_start(strategy::StrategyContext& ctx) override;
    void on_candle(const market_data::Candle& c) override;
    void on_execution_report(const execution::ExecutionReport& r) override;

protected:
    // Called for each closed candle of the configured interval.
    virtual void on_closed_candle(const market_data::Candle& c) = 0;

    // Target position helpers (long-only on spot): enter with the configured
    // size, exit everything. Both are no-ops while an order is in flight.
    void enter_long();
    void exit_long();
    [[nodiscard]] bool in_position() const;
    [[nodiscard]] bool order_pending() const noexcept { return pending_; }
    [[nodiscard]] Duration interval() const noexcept { return interval_; }

    // Size of a new long at `reference`; the default uses the fixed
    // quantity or the equity fraction. Subclasses may override (e.g. for
    // volatility-scaled sizing).
    [[nodiscard]] virtual Quantity entry_quantity(Price reference) const;
    [[nodiscard]] Quantity default_entry_quantity(Price reference) const;
    void submit_market(Side side, Quantity quantity);

private:
    Duration interval_ = Duration::minutes(1);
    Quantity fixed_quantity_;  // if set, always this size
    double equity_fraction_ = 0.95;  // else this fraction of equity at the mark
    bool pending_ = false;
    std::optional<ClientOrderId> pending_id_;
};

class BuyAndHold final : public CandleStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "buy_and_hold"; }

protected:
    void on_closed_candle(const market_data::Candle& c) override;
};

class MaCrossover final : public CandleStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "ma_crossover"; }
    void on_start(strategy::StrategyContext& ctx) override;

protected:
    void on_closed_candle(const market_data::Candle& c) override;

private:
    std::unique_ptr<strategy::Sma> fast_;
    std::unique_ptr<strategy::Sma> slow_;
    std::optional<bool> fast_above_;
};

class MeanReversion final : public CandleStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "mean_reversion"; }
    void on_start(strategy::StrategyContext& ctx) override;

protected:
    void on_closed_candle(const market_data::Candle& c) override;

private:
    std::unique_ptr<strategy::RollingStats> stats_;
    double entry_z_ = -2.0;  // buy when z <= entry
    double exit_z_ = 0.0;  // sell when z >= exit
};

class Breakout final : public CandleStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "breakout"; }
    void on_start(strategy::StrategyContext& ctx) override;

protected:
    void on_closed_candle(const market_data::Candle& c) override;

private:
    std::unique_ptr<strategy::RollingExtrema> highs_;  // lookback for entries
    std::unique_ptr<strategy::RollingExtrema> lows_;  // lookback for exits
};

class RandomStrategy final : public CandleStrategy {
public:
    [[nodiscard]] std::string_view name() const noexcept override { return "random"; }
    void on_start(strategy::StrategyContext& ctx) override;

protected:
    void on_closed_candle(const market_data::Candle& c) override;

private:
    double flip_probability_ = 0.1;
};

// Registers every baseline under its name().
void register_baselines(strategy::StrategyRegistry& registry);

}  // namespace tradebot::strategies
