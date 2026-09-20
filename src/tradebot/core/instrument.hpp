#pragma once

// Static description of a tradeable instrument and the venue rules that
// every order must satisfy. The simulated exchange enforces these exactly as
// a real venue would, so a strategy cannot pass in backtest with an order
// that live trading would reject.

#include "tradebot/core/error.hpp"
#include "tradebot/core/fixed_point.hpp"
#include "tradebot/core/types.hpp"

#include <string>

namespace tradebot {

struct Instrument {
    InstrumentId id;
    VenueId venue;
    std::string symbol;  // venue's native symbol, e.g. "ETHUSDT"
    std::string base;  // e.g. "ETH"
    std::string quote;  // e.g. "USDT"
    Price tick_size;  // minimum price increment
    Quantity lot_size;  // minimum quantity increment
    Quantity min_quantity;  // smallest allowed order size
    Notional min_notional;  // smallest allowed price * quantity

    [[nodiscard]] Result<void> validate_definition() const {
        if (!tick_size.is_positive()) {
            return make_error(ErrorCode::invalid_argument, symbol + ": tick_size must be > 0");
        }
        if (!lot_size.is_positive()) {
            return make_error(ErrorCode::invalid_argument, symbol + ": lot_size must be > 0");
        }
        if (min_quantity.is_negative() || min_notional.is_negative()) {
            return make_error(ErrorCode::invalid_argument,
                              symbol + ": minimums must be >= 0");
        }
        return {};
    }

    [[nodiscard]] bool is_valid_price(Price p) const noexcept {
        return p.is_positive() && p.is_multiple_of(tick_size);
    }

    [[nodiscard]] bool is_valid_quantity(Quantity q) const noexcept {
        return q.is_positive() && q >= min_quantity && q.is_multiple_of(lot_size);
    }

    // Full order-level check a venue applies before accepting an order.
    [[nodiscard]] Result<void> check_order(Price p, Quantity q) const {
        if (!is_valid_price(p)) {
            return make_error(ErrorCode::invalid_argument,
                              symbol + ": price " + p.to_string() + " is not a multiple of tick " +
                                  tick_size.to_string());
        }
        if (!is_valid_quantity(q)) {
            return make_error(ErrorCode::invalid_argument,
                              symbol + ": quantity " + q.to_string() +
                                  " violates lot size / minimum");
        }
        if (notional(p, q) < min_notional) {
            return make_error(ErrorCode::invalid_argument,
                              symbol + ": notional below minimum " + min_notional.to_string());
        }
        return {};
    }

    [[nodiscard]] Price round_price(Price p, RoundingMode mode) const {
        return p.round_to(tick_size, mode);
    }

    [[nodiscard]] Quantity round_quantity(Quantity q, RoundingMode mode) const {
        return q.round_to(lot_size, mode);
    }
};

}  // namespace tradebot
