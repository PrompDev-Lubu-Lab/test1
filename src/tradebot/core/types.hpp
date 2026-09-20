#pragma once

// Fundamental enumerations and strongly typed identifiers shared by every
// module. Keep this header dependency-free beyond the standard library.

#include "tradebot/core/error.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <string_view>

namespace tradebot {

enum class Side : std::uint8_t { buy, sell };

enum class OrderType : std::uint8_t { market, limit };

enum class TimeInForce : std::uint8_t {
    gtc,  // good till cancelled
    ioc,  // immediate or cancel: fill what you can, cancel the rest
    fok,  // fill or kill: fill entirely or not at all
    post_only,  // rest on the book or reject; never take liquidity
};

enum class OrderStatus : std::uint8_t {
    pending_new,  // sent, not yet acknowledged
    open,  // acknowledged and resting / working
    partially_filled,
    filled,
    pending_cancel,
    cancelled,
    rejected,
    expired,
};

// Whether a fill added or removed liquidity; drives maker/taker fees.
enum class Liquidity : std::uint8_t { maker, taker };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::buy ? Side::sell : Side::buy;
}

[[nodiscard]] constexpr bool is_terminal(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::filled:
        case OrderStatus::cancelled:
        case OrderStatus::rejected:
        case OrderStatus::expired:
            return true;
        case OrderStatus::pending_new:
        case OrderStatus::open:
        case OrderStatus::partially_filled:
        case OrderStatus::pending_cancel:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::buy ? "buy" : "sell";
}

[[nodiscard]] constexpr std::string_view to_string(OrderType t) noexcept {
    return t == OrderType::market ? "market" : "limit";
}

[[nodiscard]] constexpr std::string_view to_string(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::gtc: return "gtc";
        case TimeInForce::ioc: return "ioc";
        case TimeInForce::fok: return "fok";
        case TimeInForce::post_only: return "post_only";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::pending_new: return "pending_new";
        case OrderStatus::open: return "open";
        case OrderStatus::partially_filled: return "partially_filled";
        case OrderStatus::filled: return "filled";
        case OrderStatus::pending_cancel: return "pending_cancel";
        case OrderStatus::cancelled: return "cancelled";
        case OrderStatus::rejected: return "rejected";
        case OrderStatus::expired: return "expired";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view to_string(Liquidity l) noexcept {
    return l == Liquidity::maker ? "maker" : "taker";
}

[[nodiscard]] Result<Side> parse_side(std::string_view text);
[[nodiscard]] Result<OrderType> parse_order_type(std::string_view text);
[[nodiscard]] Result<TimeInForce> parse_time_in_force(std::string_view text);

// Strongly typed integer identifier. Distinct tags make it a compile error
// to pass an OrderId where a TradeId is expected.
template <class Tag, class Rep = std::uint64_t>
class StrongId {
public:
    using rep_type = Rep;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(Rep v) noexcept : value_(v) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != invalid_value; }
    [[nodiscard]] static constexpr StrongId invalid() noexcept { return StrongId{}; }

    friend constexpr auto operator<=>(StrongId, StrongId) noexcept = default;
    friend constexpr bool operator==(StrongId, StrongId) noexcept = default;

    static constexpr Rep invalid_value = static_cast<Rep>(0);

private:
    Rep value_ = invalid_value;
};

struct InstrumentIdTag {};
struct OrderIdTag {};
struct ClientOrderIdTag {};
struct TradeIdTag {};
struct StrategyIdTag {};
struct VenueIdTag {};

using InstrumentId = StrongId<InstrumentIdTag, std::uint32_t>;
using OrderId = StrongId<OrderIdTag>;  // assigned by the venue (real or simulated)
using ClientOrderId = StrongId<ClientOrderIdTag>;  // assigned by us; idempotency key
using TradeId = StrongId<TradeIdTag>;
using StrategyId = StrongId<StrategyIdTag, std::uint32_t>;
using VenueId = StrongId<VenueIdTag, std::uint16_t>;

// Monotonic generator for locally assigned ids. Not thread-safe by design;
// each owning component has its own.
template <class Id>
class IdGenerator {
public:
    constexpr IdGenerator() noexcept = default;
    constexpr explicit IdGenerator(typename Id::rep_type first) noexcept : next_(first) {}
    [[nodiscard]] constexpr Id next() noexcept { return Id{next_++}; }

private:
    typename Id::rep_type next_ = 1;
};

// --- inline implementations -------------------------------------------------

inline Result<Side> parse_side(std::string_view text) {
    if (text == "buy" || text == "BUY" || text == "b") return Side::buy;
    if (text == "sell" || text == "SELL" || text == "s") return Side::sell;
    return make_error(ErrorCode::parse_error, "invalid side: '" + std::string(text) + "'");
}

inline Result<OrderType> parse_order_type(std::string_view text) {
    if (text == "market" || text == "MARKET") return OrderType::market;
    if (text == "limit" || text == "LIMIT") return OrderType::limit;
    return make_error(ErrorCode::parse_error, "invalid order type: '" + std::string(text) + "'");
}

inline Result<TimeInForce> parse_time_in_force(std::string_view text) {
    if (text == "gtc" || text == "GTC") return TimeInForce::gtc;
    if (text == "ioc" || text == "IOC") return TimeInForce::ioc;
    if (text == "fok" || text == "FOK") return TimeInForce::fok;
    if (text == "post_only" || text == "POST_ONLY") return TimeInForce::post_only;
    return make_error(ErrorCode::parse_error,
                      "invalid time in force: '" + std::string(text) + "'");
}

}  // namespace tradebot

template <class Tag, class Rep>
struct std::hash<tradebot::StrongId<Tag, Rep>> {
    std::size_t operator()(tradebot::StrongId<Tag, Rep> id) const noexcept {
        return std::hash<Rep>{}(id.value());
    }
};
