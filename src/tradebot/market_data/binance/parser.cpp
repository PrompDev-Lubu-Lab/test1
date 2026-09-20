#include "tradebot/market_data/binance/parser.hpp"

#include <nlohmann/json.hpp>

#include <charconv>

namespace tradebot::market_data::binance {

namespace {

using json = nlohmann::json;

Unexpected<Error> bad(std::string what) {
    return make_error(ErrorCode::parse_error, "binance: " + std::move(what));
}

const json* field(const json& obj, const char* key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

Result<std::int64_t> get_int(const json& obj, const char* key) {
    const json* v = field(obj, key);
    if (v == nullptr || !v->is_number_integer()) {
        return bad(std::string("missing or non-integer field '") + key + "'");
    }
    return v->get<std::int64_t>();
}

Result<bool> get_bool(const json& obj, const char* key) {
    const json* v = field(obj, key);
    if (v == nullptr || !v->is_boolean()) {
        return bad(std::string("missing or non-boolean field '") + key + "'");
    }
    return v->get<bool>();
}

Result<std::string_view> get_str(const json& obj, const char* key) {
    const json* v = field(obj, key);
    if (v == nullptr || !v->is_string()) {
        return bad(std::string("missing or non-string field '") + key + "'");
    }
    return std::string_view(v->get_ref<const std::string&>());
}

template <class T>
Result<T> get_decimal(const json& obj, const char* key) {
    auto s = get_str(obj, key);
    if (!s) {
        return tl::make_unexpected(s.error());
    }
    auto d = T::parse(*s);
    if (!d) {
        return bad(std::string("field '") + key + "': " + d.error().message);
    }
    return *d;
}

// Binance timestamps are milliseconds since epoch on the live stream.
Timestamp from_millis_field(std::int64_t ms) { return Timestamp::from_millis(ms); }

Result<std::vector<BookLevel>> parse_levels(const json& arr, const char* what) {
    if (!arr.is_array()) {
        return bad(std::string(what) + " is not an array");
    }
    std::vector<BookLevel> out;
    out.reserve(arr.size());
    for (const auto& lvl : arr) {
        if (!lvl.is_array() || lvl.size() < 2 || !lvl[0].is_string() || !lvl[1].is_string()) {
            return bad(std::string("malformed level in ") + what);
        }
        auto p = Price::parse(lvl[0].get_ref<const std::string&>());
        auto q = Quantity::parse(lvl[1].get_ref<const std::string&>());
        if (!p || !q) {
            return bad(std::string("bad price/quantity in ") + what);
        }
        out.push_back(BookLevel{*p, *q});
    }
    return out;
}

Result<MarketEvent> parse_agg_trade(const json& d, InstrumentId instrument, Timestamp recv) {
    auto id = get_int(d, "a");
    auto price = get_decimal<Price>(d, "p");
    auto qty = get_decimal<Quantity>(d, "q");
    auto first = get_int(d, "f");
    auto last = get_int(d, "l");
    auto time = get_int(d, "T");
    auto maker = get_bool(d, "m");
    for (const auto* e : {id ? nullptr : &id.error(), price ? nullptr : &price.error(),
                          qty ? nullptr : &qty.error(), first ? nullptr : &first.error(),
                          last ? nullptr : &last.error(), time ? nullptr : &time.error(),
                          maker ? nullptr : &maker.error()}) {
        if (e != nullptr) {
            return tl::make_unexpected(*e);
        }
    }
    Trade t;
    t.instrument = instrument;
    t.exchange_time = from_millis_field(*time);
    t.recv_time = recv;
    t.id = TradeId{static_cast<std::uint64_t>(*id)};
    t.price = *price;
    t.quantity = *qty;
    // "m": buyer is the maker => the seller took liquidity.
    t.aggressor = *maker ? Side::sell : Side::buy;
    t.first_trade_id = *first;
    t.last_trade_id = *last;
    return t;
}

Result<MarketEvent> parse_trade(const json& d, InstrumentId instrument, Timestamp recv) {
    auto id = get_int(d, "t");
    auto price = get_decimal<Price>(d, "p");
    auto qty = get_decimal<Quantity>(d, "q");
    auto time = get_int(d, "T");
    auto maker = get_bool(d, "m");
    for (const auto* e : {id ? nullptr : &id.error(), price ? nullptr : &price.error(),
                          qty ? nullptr : &qty.error(), time ? nullptr : &time.error(),
                          maker ? nullptr : &maker.error()}) {
        if (e != nullptr) {
            return tl::make_unexpected(*e);
        }
    }
    Trade t;
    t.instrument = instrument;
    t.exchange_time = from_millis_field(*time);
    t.recv_time = recv;
    t.id = TradeId{static_cast<std::uint64_t>(*id)};
    t.price = *price;
    t.quantity = *qty;
    t.aggressor = *maker ? Side::sell : Side::buy;
    t.first_trade_id = *id;
    t.last_trade_id = *id;
    return t;
}

Result<MarketEvent> parse_depth_update(const json& d, InstrumentId instrument, Timestamp recv) {
    auto first = get_int(d, "U");
    auto final = get_int(d, "u");
    auto time = get_int(d, "E");
    if (!first || !final || !time) {
        return tl::make_unexpected(!first ? first.error() : !final ? final.error() : time.error());
    }
    const json* b = field(d, "b");
    const json* a = field(d, "a");
    if (b == nullptr || a == nullptr) {
        return bad("depthUpdate missing b/a");
    }
    auto bids = parse_levels(*b, "bids");
    auto asks = parse_levels(*a, "asks");
    if (!bids || !asks) {
        return tl::make_unexpected(!bids ? bids.error() : asks.error());
    }
    BookDelta delta;
    delta.instrument = instrument;
    delta.exchange_time = from_millis_field(*time);
    delta.recv_time = recv;
    delta.first_update_id = *first;
    delta.final_update_id = *final;
    delta.bids = std::move(*bids);
    delta.asks = std::move(*asks);
    return delta;
}

Result<MarketEvent> parse_kline(const json& d, InstrumentId instrument, Timestamp recv) {
    const json* k = field(d, "k");
    if (k == nullptr || !k->is_object()) {
        return bad("kline missing 'k'");
    }
    auto open_time = get_int(*k, "t");
    auto interval_text = get_str(*k, "i");
    auto open = get_decimal<Price>(*k, "o");
    auto high = get_decimal<Price>(*k, "h");
    auto low = get_decimal<Price>(*k, "l");
    auto close = get_decimal<Price>(*k, "c");
    auto volume = get_decimal<Quantity>(*k, "v");
    auto quote = get_decimal<Notional>(*k, "q");
    auto taker = get_decimal<Quantity>(*k, "V");
    auto count = get_int(*k, "n");
    auto closed = get_bool(*k, "x");
    for (const auto* e :
         {open_time ? nullptr : &open_time.error(), interval_text ? nullptr : &interval_text.error(),
          open ? nullptr : &open.error(), high ? nullptr : &high.error(),
          low ? nullptr : &low.error(), close ? nullptr : &close.error(),
          volume ? nullptr : &volume.error(), quote ? nullptr : &quote.error(),
          taker ? nullptr : &taker.error(), count ? nullptr : &count.error(),
          closed ? nullptr : &closed.error()}) {
        if (e != nullptr) {
            return tl::make_unexpected(*e);
        }
    }
    auto interval = parse_kline_interval(*interval_text);
    if (!interval) {
        return tl::make_unexpected(interval.error());
    }
    Candle c;
    c.instrument = instrument;
    c.open_time = from_millis_field(*open_time);
    c.interval = *interval;
    c.recv_time = recv;
    c.open = *open;
    c.high = *high;
    c.low = *low;
    c.close = *close;
    c.volume = *volume;
    c.quote_volume = *quote;
    c.taker_buy_volume = *taker;
    c.trade_count = *count;
    c.closed = *closed;
    return c;
}

Result<MarketEvent> parse_book_ticker(const json& d, InstrumentId instrument, Timestamp recv) {
    auto u = get_int(d, "u");
    auto bp = get_decimal<Price>(d, "b");
    auto bq = get_decimal<Quantity>(d, "B");
    auto ap = get_decimal<Price>(d, "a");
    auto aq = get_decimal<Quantity>(d, "A");
    for (const auto* e : {u ? nullptr : &u.error(), bp ? nullptr : &bp.error(),
                          bq ? nullptr : &bq.error(), ap ? nullptr : &ap.error(),
                          aq ? nullptr : &aq.error()}) {
        if (e != nullptr) {
            return tl::make_unexpected(*e);
        }
    }
    BookTicker t;
    t.instrument = instrument;
    t.recv_time = recv;
    t.update_id = *u;
    t.bid_price = *bp;
    t.bid_quantity = *bq;
    t.ask_price = *ap;
    t.ask_quantity = *aq;
    return t;
}

}  // namespace

Result<Duration> parse_kline_interval(std::string_view text) {
    if (text.size() < 2) {
        return bad("bad kline interval '" + std::string(text) + "'");
    }
    std::int64_t n = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size() - 1, n);
    if (ec != std::errc{} || ptr != text.data() + text.size() - 1 || n <= 0) {
        return bad("bad kline interval '" + std::string(text) + "'");
    }
    switch (text.back()) {
        case 's': return Duration::seconds(n);
        case 'm': return Duration::minutes(n);
        case 'h': return Duration::hours(n);
        case 'd': return Duration::days(n);
        case 'w': return Duration::days(7 * n);
        default: return bad("bad kline interval '" + std::string(text) + "'");
    }
}

std::string kline_interval_name(Duration interval) {
    const std::int64_t s = interval.count_seconds();
    if (s % (7 * 86400) == 0) return std::to_string(s / (7 * 86400)) + "w";
    if (s % 86400 == 0) return std::to_string(s / 86400) + "d";
    if (s % 3600 == 0) return std::to_string(s / 3600) + "h";
    if (s % 60 == 0) return std::to_string(s / 60) + "m";
    return std::to_string(s) + "s";
}

Result<MarketEvent> parse_stream_message(std::string_view text, InstrumentId instrument,
                                         Timestamp recv_time) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return bad("message is not a JSON object");
    }
    const json* data = field(root, "data");
    const json& d = (data != nullptr && data->is_object()) ? *data : root;

    if (const json* e = field(d, "e"); e != nullptr && e->is_string()) {
        const auto& type = e->get_ref<const std::string&>();
        if (type == "aggTrade") return parse_agg_trade(d, instrument, recv_time);
        if (type == "trade") return parse_trade(d, instrument, recv_time);
        if (type == "depthUpdate") return parse_depth_update(d, instrument, recv_time);
        if (type == "kline") return parse_kline(d, instrument, recv_time);
        return make_error(ErrorCode::unsupported, "binance: unsupported event type '" + type + "'");
    }
    // bookTicker has no "e": identify by its fields.
    if (field(d, "u") != nullptr && field(d, "b") != nullptr && field(d, "B") != nullptr &&
        field(d, "a") != nullptr && field(d, "A") != nullptr) {
        return parse_book_ticker(d, instrument, recv_time);
    }
    return bad("unrecognized message: " + std::string(text.substr(0, 100)));
}

Result<BookSnapshot> parse_depth_snapshot(std::string_view text, InstrumentId instrument,
                                          Timestamp recv_time) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return bad("depth snapshot is not a JSON object");
    }
    auto last = get_int(root, "lastUpdateId");
    if (!last) {
        return tl::make_unexpected(last.error());
    }
    const json* b = field(root, "bids");
    const json* a = field(root, "asks");
    if (b == nullptr || a == nullptr) {
        return bad("depth snapshot missing bids/asks");
    }
    auto bids = parse_levels(*b, "bids");
    auto asks = parse_levels(*a, "asks");
    if (!bids || !asks) {
        return tl::make_unexpected(!bids ? bids.error() : asks.error());
    }
    BookSnapshot s;
    s.instrument = instrument;
    s.recv_time = recv_time;
    s.last_update_id = *last;
    s.bids = std::move(*bids);
    s.asks = std::move(*asks);
    return s;
}

Result<Instrument> parse_exchange_info(std::string_view text, std::string_view symbol,
                                       InstrumentId instrument, VenueId venue) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return bad("exchangeInfo is not a JSON object");
    }
    const json* symbols = field(root, "symbols");
    if (symbols == nullptr || !symbols->is_array()) {
        return bad("exchangeInfo missing symbols");
    }
    for (const auto& s : *symbols) {
        auto name = get_str(s, "symbol");
        if (!name || *name != symbol) {
            continue;
        }
        Instrument inst;
        inst.id = instrument;
        inst.venue = venue;
        inst.symbol = std::string(symbol);
        auto base = get_str(s, "baseAsset");
        auto quote = get_str(s, "quoteAsset");
        if (!base || !quote) {
            return bad("exchangeInfo symbol missing baseAsset/quoteAsset");
        }
        inst.base = std::string(*base);
        inst.quote = std::string(*quote);
        const json* filters = field(s, "filters");
        if (filters == nullptr || !filters->is_array()) {
            return bad("exchangeInfo symbol missing filters");
        }
        bool have_tick = false, have_lot = false;
        for (const auto& f : *filters) {
            auto type = get_str(f, "filterType");
            if (!type) {
                continue;
            }
            if (*type == "PRICE_FILTER") {
                auto tick = get_decimal<Price>(f, "tickSize");
                if (!tick) return tl::make_unexpected(tick.error());
                inst.tick_size = *tick;
                have_tick = true;
            } else if (*type == "LOT_SIZE") {
                auto step = get_decimal<Quantity>(f, "stepSize");
                auto min_qty = get_decimal<Quantity>(f, "minQty");
                if (!step || !min_qty) return tl::make_unexpected(!step ? step.error() : min_qty.error());
                inst.lot_size = *step;
                inst.min_quantity = *min_qty;
                have_lot = true;
            } else if (*type == "NOTIONAL" || *type == "MIN_NOTIONAL") {
                auto min_notional = get_decimal<Notional>(f, "minNotional");
                if (!min_notional) return tl::make_unexpected(min_notional.error());
                inst.min_notional = *min_notional;
            }
        }
        if (!have_tick || !have_lot) {
            return bad("exchangeInfo symbol missing PRICE_FILTER or LOT_SIZE");
        }
        if (auto v = inst.validate_definition(); !v) {
            return tl::make_unexpected(v.error());
        }
        return inst;
    }
    return make_error(ErrorCode::not_found,
                      "binance: symbol " + std::string(symbol) + " not in exchangeInfo");
}

}  // namespace tradebot::market_data::binance
