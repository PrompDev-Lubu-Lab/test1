#pragma once

// Binance spot message parsers: live WebSocket JSON and REST JSON in, normalized
// events out. Parsing is strict: any missing or malformed field is an error
// rather than a default, because a silently wrong price is worse than a
// dropped message.

#include "tradebot/core/error.hpp"
#include "tradebot/core/instrument.hpp"
#include "tradebot/market_data/events.hpp"

#include <string_view>

namespace tradebot::market_data::binance {

// Parses one WebSocket message. Accepts both the combined-stream wrapper
// {"stream":..,"data":{..}} and a bare payload. Recognizes aggTrade, trade,
// depthUpdate, kline and bookTicker payloads.
[[nodiscard]] Result<MarketEvent> parse_stream_message(std::string_view json,
                                                       InstrumentId instrument,
                                                       Timestamp recv_time);

// REST /api/v3/depth response.
[[nodiscard]] Result<BookSnapshot> parse_depth_snapshot(std::string_view json,
                                                        InstrumentId instrument,
                                                        Timestamp recv_time);

// REST /api/v3/exchangeInfo?symbol=... response -> venue rules for that symbol.
[[nodiscard]] Result<Instrument> parse_exchange_info(std::string_view json, std::string_view symbol,
                                                     InstrumentId instrument, VenueId venue);

// Interval strings as Binance names them: "1m", "5m", "1h", "1d", ...
[[nodiscard]] Result<Duration> parse_kline_interval(std::string_view text);
[[nodiscard]] std::string kline_interval_name(Duration interval);

}  // namespace tradebot::market_data::binance
