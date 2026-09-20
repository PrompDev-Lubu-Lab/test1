#pragma once

// Risk manager: the mandatory gate between strategies and any venue.
//
// It implements ExecutionVenue and wraps the real one, so a strategy cannot
// tell whether it is talking to the venue or the gate. Every order passes
// pre-trade checks against the limits and the current portfolio; a failed
// check is answered with a synthesized rejection report, exactly like a
// venue rejection. Portfolio-level limits (drawdown, daily loss) trip a
// kill switch that cancels everything open and refuses new orders until
// explicitly reset. The same object runs in backtests and live, so a
// strategy that breaches limits fails in research, not in production.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/fixed_point.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/execution/types.hpp"
#include "tradebot/portfolio/portfolio.hpp"

#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace tradebot::risk {

struct RiskLimits {
    // Per order
    Quantity max_order_quantity;  // zero = unlimited
    Notional max_order_notional;  // zero = unlimited
    double max_price_deviation = 0.05;  // limit price vs mark, fraction; <= 0 disables
    // Per strategy / account
    Quantity max_position;  // absolute, projected including open orders; zero = unlimited
    Notional max_position_notional;  // at mark; zero = unlimited
    std::uint32_t max_open_orders = 0;  // zero = unlimited
    std::uint32_t max_orders_per_minute = 0;  // zero = unlimited
    bool allow_short = false;  // spot: selling more than you hold is rejected
    // Kill switch
    Notional max_drawdown;  // from peak equity; zero = disabled
    Notional max_daily_loss;  // from equity at UTC day start; zero = disabled
    std::set<InstrumentId> allowed_instruments;  // empty = all
    // On trip, after cancelling every working order, send market orders to
    // close every position (per strategy) straight to the venue.
    bool flatten_on_trip = false;
};

struct RiskStats {
    std::uint64_t checked = 0;
    std::uint64_t rejected = 0;
    std::uint64_t kill_switch_trips = 0;
    std::uint64_t flatten_orders = 0;
    std::map<std::string, std::uint64_t> rejections_by_reason;
};

class RiskManager final : public execution::ExecutionVenue, public execution::ExecutionListener {
public:
    RiskManager(execution::ExecutionVenue& venue, const portfolio::Portfolio& portfolio,
                const Clock& clock, RiskLimits limits, Logger log = {});

    // --- ExecutionVenue (strategy-facing) ---------------------------------
    void set_listener(execution::ExecutionListener* listener) override { listener_ = listener; }
    [[nodiscard]] Result<void> submit(const execution::OrderRequest& request) override;
    [[nodiscard]] Result<void> cancel(ClientOrderId client_id) override;
    [[nodiscard]] std::optional<execution::OrderState> order(ClientOrderId client_id) const override;

    // --- ExecutionListener (venue-facing) ---------------------------------
    void on_execution_report(const execution::ExecutionReport& report) override;

    // Pre-trade checks only; no side effects. Exposed for tests and tools.
    [[nodiscard]] Result<void> check(const execution::OrderRequest& request) const;

    // Evaluates drawdown / daily loss and trips the kill switch on breach.
    // Call on marks or a timer. Returns true if tripped (now or before).
    bool check_limits();
    void trip(std::string reason);
    void reset();
    [[nodiscard]] bool tripped() const noexcept { return tripped_; }
    // Refuses new orders without cancelling or flattening anything (orderly
    // shutdown). Not cleared by reset().
    void halt(std::string reason);
    [[nodiscard]] bool halted() const noexcept { return !halt_reason_.empty(); }
    [[nodiscard]] const std::string& trip_reason() const noexcept { return trip_reason_; }

    // Cancels every order accepted downstream and not yet done (also part of
    // a trip). Exposed for orderly shutdown.
    void cancel_all_open();
    [[nodiscard]] std::size_t open_orders() const noexcept { return open_.size(); }

    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }
    void set_limits(RiskLimits limits) { limits_ = std::move(limits); }
    [[nodiscard]] const RiskStats& stats() const noexcept { return stats_; }

private:
    void reject(const execution::OrderRequest& request, const std::string& reason);
    void flatten_positions();
    [[nodiscard]] Notional day_start_equity();

    execution::ExecutionVenue& venue_;
    const portfolio::Portfolio& portfolio_;
    const Clock& clock_;
    RiskLimits limits_;
    Logger log_;
    execution::ExecutionListener* listener_ = nullptr;
    bool tripped_ = false;
    std::string trip_reason_;
    std::string halt_reason_;
    std::deque<Timestamp> recent_submits_;
    std::set<ClientOrderId> open_;  // orders accepted downstream and not yet done
    std::optional<Timestamp> day_;
    Notional day_start_equity_;
    IdGenerator<ClientOrderId> flatten_ids_{0x7FFF'0000'0000'0000ULL};  // distinct from strategy ids
    RiskStats stats_;
};

}  // namespace tradebot::risk
