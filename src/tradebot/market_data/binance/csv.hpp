#pragma once

// Parsers for Binance bulk archive CSV rows (data.binance.vision). Rows
// have no header in older archives and a header row in newer ones; both are
// accepted. Timestamps are milliseconds historically and microseconds since
// 2025; the unit is inferred from magnitude.

#include "tradebot/core/error.hpp"
#include "tradebot/market_data/events.hpp"

#include <string_view>

namespace tradebot::market_data::binance {

// True for the optional header row ("agg_trade_id,price,...").
[[nodiscard]] bool is_csv_header(std::string_view line) noexcept;

// agg_trade_id,price,quantity,first_trade_id,last_trade_id,transact_time,is_buyer_maker,is_best_match
[[nodiscard]] Result<Trade> parse_agg_trade_csv(std::string_view line, InstrumentId instrument);

// trade_id,price,qty,quote_qty,time,is_buyer_maker,is_best_match
[[nodiscard]] Result<Trade> parse_trade_csv(std::string_view line, InstrumentId instrument);

// open_time,open,high,low,close,volume,close_time,quote_volume,count,
// taker_buy_volume,taker_buy_quote_volume,ignore
[[nodiscard]] Result<Candle> parse_kline_csv(std::string_view line, InstrumentId instrument,
                                             Duration interval);

// Exposed for tests: epoch value in ms or us -> Timestamp.
[[nodiscard]] Timestamp epoch_from_ms_or_us(std::int64_t value) noexcept;

}  // namespace tradebot::market_data::binance
