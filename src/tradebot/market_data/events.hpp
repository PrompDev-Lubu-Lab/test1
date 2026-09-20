#pragma once

// Normalized, venue-independent market events.
//
// Every feed and every archive is reduced to these types before anything
// downstream (order book, strategies, storage) sees it. Two timestamps are
// carried: exchange_time is the venue's own stamp, recv_time is when the
// message reached us. Live capture orders events by recv_time (what a live
// strategy would have seen); bulk archives only have exchange_time, so
// recv_time equals exchange_time there.

#include "tradebot/core/fixed_point.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/core/types.hpp"

#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

namespace tradebot::market_data {

struct Trade {
    InstrumentId instrument;
    Timestamp exchange_time;
    Timestamp recv_time;
    TradeId id;  // aggregate trade id on Binance aggTrade streams
    Price price;
    Quantity quantity;
    Side aggressor;  // side of the taker (the order that crossed the spread)
    std::int64_t first_trade_id = 0;  // underlying trade range for aggregates
    std::int64_t last_trade_id = 0;
};

struct BookLevel {
    Price price;
    Quantity quantity;  // zero in a delta means "remove this level"
};

struct BookSnapshot {
    InstrumentId instrument;
    Timestamp recv_time;
    std::int64_t last_update_id = 0;
    std::vector<BookLevel> bids;  // best first
    std::vector<BookLevel> asks;  // best first
};

struct BookDelta {
    InstrumentId instrument;
    Timestamp exchange_time;
    Timestamp recv_time;
    std::int64_t first_update_id = 0;
    std::int64_t final_update_id = 0;
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
};

// Top-of-book only; cheaper than a full book for strategies that just need
// the touch.
struct BookTicker {
    InstrumentId instrument;
    Timestamp recv_time;
    std::int64_t update_id = 0;
    Price bid_price;
    Quantity bid_quantity;
    Price ask_price;
    Quantity ask_quantity;
};

struct Candle {
    InstrumentId instrument;
    Timestamp open_time;
    Duration interval;
    Timestamp recv_time;
    Price open;
    Price high;
    Price low;
    Price close;
    Quantity volume;
    Notional quote_volume;
    Quantity taker_buy_volume;
    std::int64_t trade_count = 0;
    bool closed = false;  // false while the interval is still forming

    [[nodiscard]] Timestamp close_time() const noexcept { return open_time + interval; }
};

struct Heartbeat {
    InstrumentId instrument;
    Timestamp recv_time;
};

using MarketEvent = std::variant<Trade, BookSnapshot, BookDelta, BookTicker, Candle, Heartbeat>;

// The time an event is sequenced on. recv_time everywhere, so a live capture
// replays in the order it was observed and bulk data (recv == exchange)
// replays in exchange order.
[[nodiscard]] inline Timestamp event_time(const MarketEvent& e) noexcept {
    return std::visit([](const auto& v) { return v.recv_time; }, e);
}

[[nodiscard]] inline InstrumentId event_instrument(const MarketEvent& e) noexcept {
    return std::visit([](const auto& v) { return v.instrument; }, e);
}

[[nodiscard]] constexpr std::string_view event_type_name(const MarketEvent& e) noexcept {
    switch (e.index()) {
        case 0: return "trade";
        case 1: return "book_snapshot";
        case 2: return "book_delta";
        case 3: return "book_ticker";
        case 4: return "candle";
        case 5: return "heartbeat";
        default: return "?";
    }
}

}  // namespace tradebot::market_data
