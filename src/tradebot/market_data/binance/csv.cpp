#include "tradebot/market_data/binance/csv.hpp"

#include <array>
#include <charconv>

namespace tradebot::market_data::binance {

namespace {

Unexpected<Error> bad(std::string_view line, std::string what) {
    return make_error(ErrorCode::parse_error,
                      "binance csv: " + std::move(what) + " in '" + std::string(line.substr(0, 80)) + "'");
}

// Splits into at most N fields; returns the count found.
template <std::size_t N>
std::size_t split(std::string_view line, std::array<std::string_view, N>& out) {
    std::size_t n = 0;
    std::size_t start = 0;
    while (n < N) {
        const std::size_t comma = line.find(',', start);
        out[n++] = line.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                      : comma - start);
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return n;
}

bool parse_i64(std::string_view s, std::int64_t& out) {
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

bool parse_bool(std::string_view s, bool& out) {
    if (s == "true" || s == "True" || s == "TRUE" || s == "1") {
        out = true;
        return true;
    }
    if (s == "false" || s == "False" || s == "FALSE" || s == "0") {
        out = false;
        return true;
    }
    return false;
}

}  // namespace

Timestamp epoch_from_ms_or_us(std::int64_t value) noexcept {
    // 1e14 ms is year 5138; 1e14 us is 1973. Anything above is microseconds.
    return value >= 100'000'000'000'000 ? Timestamp::from_micros(value)
                                       : Timestamp::from_millis(value);
}

bool is_csv_header(std::string_view line) noexcept {
    return !line.empty() && !(line.front() >= '0' && line.front() <= '9');
}

Result<Trade> parse_agg_trade_csv(std::string_view line, InstrumentId instrument) {
    std::array<std::string_view, 8> f{};
    if (split(line, f) < 7) {
        return bad(line, "expected at least 7 fields");
    }
    std::int64_t id = 0, first = 0, last = 0, time = 0;
    bool maker = false;
    if (!parse_i64(f[0], id) || !parse_i64(f[3], first) || !parse_i64(f[4], last) ||
        !parse_i64(f[5], time) || !parse_bool(f[6], maker)) {
        return bad(line, "bad integer/boolean field");
    }
    auto price = Price::parse(f[1]);
    auto qty = Quantity::parse(f[2]);
    if (!price || !qty) {
        return bad(line, "bad price/quantity");
    }
    Trade t;
    t.instrument = instrument;
    t.exchange_time = epoch_from_ms_or_us(time);
    t.recv_time = t.exchange_time;
    t.id = TradeId{static_cast<std::uint64_t>(id)};
    t.price = *price;
    t.quantity = *qty;
    t.aggressor = maker ? Side::sell : Side::buy;
    t.first_trade_id = first;
    t.last_trade_id = last;
    return t;
}

Result<Trade> parse_trade_csv(std::string_view line, InstrumentId instrument) {
    std::array<std::string_view, 7> f{};
    if (split(line, f) < 6) {
        return bad(line, "expected at least 6 fields");
    }
    std::int64_t id = 0, time = 0;
    bool maker = false;
    if (!parse_i64(f[0], id) || !parse_i64(f[4], time) || !parse_bool(f[5], maker)) {
        return bad(line, "bad integer/boolean field");
    }
    auto price = Price::parse(f[1]);
    auto qty = Quantity::parse(f[2]);
    if (!price || !qty) {
        return bad(line, "bad price/quantity");
    }
    Trade t;
    t.instrument = instrument;
    t.exchange_time = epoch_from_ms_or_us(time);
    t.recv_time = t.exchange_time;
    t.id = TradeId{static_cast<std::uint64_t>(id)};
    t.price = *price;
    t.quantity = *qty;
    t.aggressor = maker ? Side::sell : Side::buy;
    t.first_trade_id = id;
    t.last_trade_id = id;
    return t;
}

Result<Candle> parse_kline_csv(std::string_view line, InstrumentId instrument, Duration interval) {
    std::array<std::string_view, 12> f{};
    if (split(line, f) < 11) {
        return bad(line, "expected at least 11 fields");
    }
    std::int64_t open_time = 0, count = 0;
    if (!parse_i64(f[0], open_time) || !parse_i64(f[8], count)) {
        return bad(line, "bad integer field");
    }
    auto open = Price::parse(f[1]);
    auto high = Price::parse(f[2]);
    auto low = Price::parse(f[3]);
    auto close = Price::parse(f[4]);
    auto volume = Quantity::parse(f[5]);
    auto quote = Notional::parse(f[7]);
    auto taker = Quantity::parse(f[9]);
    if (!open || !high || !low || !close || !volume || !quote || !taker) {
        return bad(line, "bad decimal field");
    }
    Candle c;
    c.instrument = instrument;
    c.open_time = epoch_from_ms_or_us(open_time);
    c.interval = interval;
    c.recv_time = c.open_time + interval;  // known only once the interval closed
    c.open = *open;
    c.high = *high;
    c.low = *low;
    c.close = *close;
    c.volume = *volume;
    c.quote_volume = *quote;
    c.taker_buy_volume = *taker;
    c.trade_count = count;
    c.closed = true;
    return c;
}

}  // namespace tradebot::market_data::binance
