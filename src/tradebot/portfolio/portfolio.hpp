#pragma once

// Portfolio: the single source of truth for what we own.
//
// Updated only from execution reports (fills), never from what a strategy
// believes happened. Marks come from market data on the client-side bus.
// Every strategy has its own sub-ledger; the account ledger is the sum.
// Average-cost accounting: reducing a position realizes (price - average
// entry) on the reduced quantity; fees are tracked separately and netted in
// the P&L views.

#include "tradebot/core/fixed_point.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/core/types.hpp"
#include "tradebot/execution/types.hpp"
#include "tradebot/market_data/events.hpp"
#include "tradebot/replay/replay_engine.hpp"

#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace tradebot::portfolio {

struct Position {
    Quantity quantity;  // signed: positive long, negative short
    Price average_entry;  // zero when flat
    Notional realized_pnl;  // gross of fees
    Notional fees;
    Quantity bought;
    Quantity sold;
    std::uint64_t fills = 0;

    [[nodiscard]] bool is_flat() const noexcept { return quantity.is_zero(); }
    [[nodiscard]] Notional unrealized_pnl(Price mark) const;
    [[nodiscard]] Notional market_value(Price mark) const { return notional(mark, quantity); }
};

// Exposure from orders that are live at the venue.
struct OpenExposure {
    Notional buy_notional;  // limit buys: price * remaining; market buys: mark * remaining
    Quantity sell_quantity;
    std::uint32_t open_orders = 0;
};

struct Ledger {
    Notional cash;
    std::map<InstrumentId, Position> positions;
    Notional fees;
    Notional realized_pnl;  // gross of fees, all instruments

    [[nodiscard]] const Position* position(InstrumentId id) const noexcept;
    [[nodiscard]] Position& position_mut(InstrumentId id) { return positions[id]; }
};

struct EquitySample {
    Timestamp time;
    Notional equity;
    Notional cash;
    Notional realized_pnl;  // net of fees
    Notional unrealized_pnl;
    Notional fees;
    Quantity position;  // sum over instruments (meaningful for one instrument)
    Price mark;
};

class Portfolio final : public execution::ExecutionListener, public replay::MarketDataListener {
public:
    explicit Portfolio(Notional initial_cash);

    // --- ExecutionListener ----------------------------------------------
    void on_execution_report(const execution::ExecutionReport& report) override;

    // --- MarketDataListener (client-side bus) ---------------------------
    void on_trade(const market_data::Trade& t) override;
    void on_book_ticker(const market_data::BookTicker& bt) override;
    void on_book_snapshot(const market_data::BookSnapshot& s) override;
    void on_book_delta(const market_data::BookDelta& d) override;
    void on_candle(const market_data::Candle& c) override;

    void set_mark(InstrumentId id, Price mark) {
        marks_[id] = mark;
        track_peak();
    }
    [[nodiscard]] std::optional<Price> mark(InstrumentId id) const;

    // --- account views --------------------------------------------------
    // The account ledger holds exact cash, fees and quantities. Its
    // realized and unrealized P&L are the sums over strategy ledgers, so
    // attribution always adds up (a blended account-level cost basis would
    // not).
    [[nodiscard]] const Ledger& account() const noexcept { return account_; }
    [[nodiscard]] const Ledger& ledger(StrategyId strategy) const;
    [[nodiscard]] const std::unordered_map<StrategyId, Ledger>& strategy_ledgers() const noexcept {
        return ledgers_;
    }
    [[nodiscard]] Notional initial_cash() const noexcept { return initial_cash_; }
    [[nodiscard]] Notional cash() const noexcept { return account_.cash; }
    [[nodiscard]] Quantity position(InstrumentId id) const;
    [[nodiscard]] Quantity position(StrategyId strategy, InstrumentId id) const;
    [[nodiscard]] Notional unrealized_pnl() const;
    [[nodiscard]] Notional unrealized_pnl(StrategyId strategy) const;
    [[nodiscard]] Notional realized_pnl_net() const { return account_.realized_pnl - account_.fees; }
    [[nodiscard]] Notional equity() const;  // cash + market value of positions
    [[nodiscard]] Notional equity(StrategyId strategy) const;
    [[nodiscard]] const OpenExposure& open_exposure(StrategyId strategy) const;
    [[nodiscard]] OpenExposure open_exposure() const;  // account total

    // State persistence (paper/live restarts): the account ledger and the
    // per-strategy ledgers. Open orders are not part of the state.
    struct State {
        Ledger account;
        std::vector<std::pair<StrategyId, Ledger>> strategies;
    };
    [[nodiscard]] State state() const;
    void restore(const State& state);
    [[nodiscard]] std::string state_to_json() const;
    [[nodiscard]] static Result<State> state_from_json(std::string_view json);

    // Appends the current state to the equity curve.
    void sample(Timestamp time);
    [[nodiscard]] const std::vector<EquitySample>& equity_curve() const noexcept { return curve_; }
    [[nodiscard]] EquitySample snapshot(Timestamp time) const;

    // Highest equity seen (via samples or fills) and current drawdown.
    [[nodiscard]] Notional peak_equity() const noexcept { return peak_equity_; }
    [[nodiscard]] Notional drawdown() const;  // peak - equity, >= 0

    struct Stats {
        std::uint64_t fills = 0;
        std::uint64_t duplicate_fills = 0;  // redelivered reports ignored
        Quantity volume;
        Notional turnover;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct OpenOrder {
        StrategyId strategy;
        InstrumentId instrument;
        Side side;
        OrderType type;
        Price price;
        Quantity remaining;
    };
    // Returns the P&L realized by this fill under the ledger's average cost.
    static Notional apply_fill(Ledger& ledger, InstrumentId id, Side side, const execution::Fill& fill);
    void recompute_exposure(StrategyId strategy);
    void track_peak();

    Notional initial_cash_;
    Ledger account_;
    std::unordered_map<StrategyId, Ledger> ledgers_;
    std::unordered_map<StrategyId, OpenExposure> exposures_;
    std::unordered_map<ClientOrderId, OpenOrder> open_orders_;
    std::unordered_map<InstrumentId, Price> marks_;
    std::set<std::pair<std::uint64_t, std::uint64_t>> seen_fills_;  // (client id, exec id)
    std::vector<EquitySample> curve_;
    Notional peak_equity_;
    Stats stats_;
};

}  // namespace tradebot::portfolio
