#include "tradebot/market_data/candle_aggregator.hpp"

#include <algorithm>

namespace tradebot::market_data {

CandleAggregator::CandleAggregator(InstrumentId instrument, Options opts, CandleHandler on_closed)
    : instrument_(instrument), opts_(std::move(opts)), on_closed_(std::move(on_closed)) {
    for (Duration d : opts_.intervals) {
        series_.push_back(Series{d, std::nullopt, std::nullopt});
    }
}

void CandleAggregator::open_bucket(Series& s, Timestamp open_time, Timestamp recv_time) {
    Candle c;
    c.instrument = instrument_;
    c.open_time = open_time;
    c.interval = s.interval;
    c.recv_time = recv_time;
    c.closed = false;
    if (s.last_close) {
        c.open = c.high = c.low = c.close = *s.last_close;
    }
    s.current = c;
}

// Closes the current bucket and, if fill_gaps is on, emits flat candles for
// every empty bucket up to (not including) the one containing `until`.
void CandleAggregator::close_and_roll(Series& s, Timestamp until) {
    if (!s.current) {
        return;
    }
    Candle& c = *s.current;
    if (until < c.close_time()) {
        return;  // still forming
    }
    c.closed = true;
    c.recv_time = c.close_time();
    s.last_close = c.close;
    on_closed_(c);
    Timestamp next_open = c.close_time();
    s.current.reset();
    if (opts_.fill_gaps) {
        const Timestamp target_open = until.floor_to(s.interval);
        while (next_open < target_open) {
            Candle gap;
            gap.instrument = instrument_;
            gap.open_time = next_open;
            gap.interval = s.interval;
            gap.recv_time = next_open + s.interval;
            gap.open = gap.high = gap.low = gap.close = *s.last_close;
            gap.closed = true;
            on_closed_(gap);
            next_open += s.interval;
        }
    }
}

void CandleAggregator::on_trade(const Trade& trade) {
    for (Series& s : series_) {
        const Timestamp bucket = trade.exchange_time.floor_to(s.interval);
        if (s.current && bucket < s.current->open_time) {
            ++late_trades_;
            continue;
        }
        if (s.current && bucket > s.current->open_time) {
            close_and_roll(s, trade.exchange_time);
        }
        if (!s.current) {
            open_bucket(s, bucket, trade.recv_time);
            s.current->open = s.current->high = s.current->low = s.current->close = trade.price;
        }
        Candle& c = *s.current;
        c.high = std::max(c.high, trade.price);
        c.low = std::min(c.low, trade.price);
        c.close = trade.price;
        c.volume += trade.quantity;
        c.quote_volume += notional(trade.price, trade.quantity);
        if (trade.aggressor == Side::buy) {
            c.taker_buy_volume += trade.quantity;
        }
        ++c.trade_count;
        c.recv_time = trade.recv_time;
    }
}

void CandleAggregator::advance_to(Timestamp now) {
    for (Series& s : series_) {
        close_and_roll(s, now);
    }
}

std::optional<Candle> CandleAggregator::current(Duration interval) const {
    for (const Series& s : series_) {
        if (s.interval == interval) {
            return s.current;
        }
    }
    return std::nullopt;
}

}  // namespace tradebot::market_data
