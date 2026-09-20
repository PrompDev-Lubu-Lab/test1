#pragma once

// Builds candles of one or more intervals from a trade stream, incrementally.
//
// Identical code serves replay and live: feed trades in time order, collect
// closed candles as they complete. Buckets are aligned to the interval from
// the Unix epoch, matching how venues define their own klines. Gaps with no
// trades produce flat zero-volume candles (open=high=low=close=previous
// close) so downstream series are regular; this can be disabled.

#include "tradebot/core/time.hpp"
#include "tradebot/market_data/events.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace tradebot::market_data {

class CandleAggregator {
public:
    struct Options {
        std::vector<Duration> intervals = {Duration::minutes(1)};
        bool fill_gaps = true;
    };

    using CandleHandler = std::function<void(const Candle&)>;

    CandleAggregator(InstrumentId instrument, Options opts, CandleHandler on_closed);

    // Trades must arrive in non-decreasing exchange_time order; an
    // out-of-order trade is applied to the current bucket if it belongs
    // there and otherwise ignored (counted in late_trades()).
    void on_trade(const Trade& trade);

    // Closes every bucket that ended at or before `now` (call on timers in
    // live mode so quiet periods still emit candles).
    void advance_to(Timestamp now);

    // The candle currently forming for an interval, if any.
    [[nodiscard]] std::optional<Candle> current(Duration interval) const;

    [[nodiscard]] std::uint64_t late_trades() const noexcept { return late_trades_; }

private:
    struct Series {
        Duration interval;
        std::optional<Candle> current;
        std::optional<Price> last_close;
    };

    void close_and_roll(Series& s, Timestamp until);
    void open_bucket(Series& s, Timestamp open_time, Timestamp recv_time);

    InstrumentId instrument_;
    Options opts_;
    CandleHandler on_closed_;
    std::vector<Series> series_;
    std::uint64_t late_trades_ = 0;
};

}  // namespace tradebot::market_data
