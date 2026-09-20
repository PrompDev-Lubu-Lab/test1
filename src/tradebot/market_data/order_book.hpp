#pragma once

// Level-2 limit order book reconstructed from snapshots and deltas.
//
// A pure data structure: it applies whatever it is given and answers
// queries. Sequencing rules (which deltas are valid after which snapshot)
// live in BookSynchronizer so this class is identical in replay and live.
// Levels are kept in vectors sorted best-first; L2 books have at most a few
// thousand levels and this is far faster than a map for the read-heavy
// access pattern of strategies and the matching engine.

#include "tradebot/core/fixed_point.hpp"
#include "tradebot/core/types.hpp"
#include "tradebot/market_data/events.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tradebot::market_data {

// Result of walking one side of the book with a given quantity.
struct FillEstimate {
    Quantity filled;  // may be less than requested if the book runs out
    Notional cost;  // sum of price * quantity over the levels consumed
    Price worst_price;  // last level touched (zero if nothing filled)
    std::size_t levels = 0;  // number of levels touched

    [[nodiscard]] bool complete(Quantity requested) const noexcept { return filled == requested; }
    [[nodiscard]] Price average_price() const {
        return filled.is_zero() ? Price{} : price_for(cost, filled);
    }
};

class OrderBook {
public:
    OrderBook() = default;
    explicit OrderBook(InstrumentId instrument) : instrument_(instrument) {}

    void apply(const BookSnapshot& snapshot);
    void apply(const BookDelta& delta);
    // Sets one level directly (quantity zero removes). Exposed for tests and
    // for venues whose feeds deliver single-level updates.
    void set_level(Side side, Price price, Quantity quantity);
    void clear();

    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }
    [[nodiscard]] std::int64_t last_update_id() const noexcept { return last_update_id_; }
    [[nodiscard]] Timestamp last_update_time() const noexcept { return last_update_time_; }
    [[nodiscard]] bool empty() const noexcept { return bids_.empty() && asks_.empty(); }

    // Best-first views.
    [[nodiscard]] std::span<const BookLevel> bids() const noexcept { return bids_; }
    [[nodiscard]] std::span<const BookLevel> asks() const noexcept { return asks_; }
    [[nodiscard]] std::span<const BookLevel> side(Side s) const noexcept {
        return s == Side::buy ? bids() : asks();
    }

    [[nodiscard]] std::optional<BookLevel> best_bid() const noexcept;
    [[nodiscard]] std::optional<BookLevel> best_ask() const noexcept;
    [[nodiscard]] std::optional<Price> mid_price() const noexcept;
    [[nodiscard]] std::optional<Price> spread() const noexcept;
    [[nodiscard]] bool is_crossed() const noexcept;  // bid >= ask: feed is broken

    // Quantity resting at exactly this price on the side (zero if none).
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const noexcept;

    // Total quantity on the side at prices no worse than `limit` (bids at or
    // above, asks at or below).
    [[nodiscard]] Quantity liquidity_within(Side side, Price limit) const noexcept;

    // Walks the side an aggressor would hit: a buy consumes asks, a sell
    // consumes bids. Optional limit stops at prices worse than it.
    [[nodiscard]] FillEstimate estimate_fill(Side aggressor, Quantity quantity,
                                             std::optional<Price> limit = std::nullopt) const;

    // (bid_qty - ask_qty) / (bid_qty + ask_qty) over the top `levels`; in
    // [-1, 1], NaN-free (zero when both sides are empty).
    [[nodiscard]] double imbalance(std::size_t levels) const noexcept;

    // Consistency check used by tests and by the synchronizer after apply.
    [[nodiscard]] bool is_sorted() const noexcept;

private:
    static void set_level_on(std::vector<BookLevel>& levels, bool descending, Price price,
                             Quantity quantity);

    InstrumentId instrument_;
    std::vector<BookLevel> bids_;  // price descending
    std::vector<BookLevel> asks_;  // price ascending
    std::int64_t last_update_id_ = 0;
    Timestamp last_update_time_;
};

}  // namespace tradebot::market_data
