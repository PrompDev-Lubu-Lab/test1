#include "tradebot/replay/event_source.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <random>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::replay;
using namespace tradebot::storage;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-replay-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

const InstrumentId kEth{1};

Trade trade(std::int64_t ms, std::uint64_t id) {
    Trade t;
    t.instrument = kEth;
    t.exchange_time = Timestamp::from_millis(ms);
    t.recv_time = Timestamp::from_millis(ms);
    t.id = TradeId{id};
    t.price = "1"_px;
    t.quantity = "1"_qty;
    t.aggressor = Side::buy;
    return t;
}

BookTicker ticker(std::int64_t ms, std::int64_t id) {
    return BookTicker{kEth, Timestamp::from_millis(ms), id, "1"_px, "1"_qty, "2"_px, "1"_qty};
}

std::vector<MarketEvent> drain(EventSource& src) {
    std::vector<MarketEvent> out;
    MarketEvent ev;
    for (;;) {
        auto more = src.next(ev);
        REQUIRE_MESSAGE(more.has_value(), more.error().message);
        if (!*more) break;
        out.push_back(ev);
    }
    return out;
}

}  // namespace

TEST_CASE("MergedEventSource: time order with deterministic tie-breaks") {
    auto a = std::make_unique<VectorEventSource>(
        std::vector<MarketEvent>{trade(1, 1), trade(3, 2), trade(3, 3), trade(7, 4)});
    auto b = std::make_unique<VectorEventSource>(
        std::vector<MarketEvent>{ticker(0, 10), ticker(3, 11), ticker(5, 12)});
    auto c = std::make_unique<VectorEventSource>(std::vector<MarketEvent>{});
    MergedEventSource merged;
    merged.add(std::move(a));
    merged.add(std::move(b));
    merged.add(std::move(c));
    CHECK(merged.source_count() == 3);
    auto out = drain(merged);
    REQUIRE(out.size() == 7);
    std::vector<std::int64_t> times;
    for (const auto& e : out) times.push_back(event_time(e).millis_since_epoch());
    CHECK(times == std::vector<std::int64_t>{0, 1, 3, 3, 3, 5, 7});
    // At t=3: source 0's two trades (in their order) before source 1's ticker.
    CHECK(std::get<Trade>(out[2]).id == TradeId{2});
    CHECK(std::get<Trade>(out[3]).id == TradeId{3});
    CHECK(std::holds_alternative<BookTicker>(out[4]));
    // Exhausted source keeps returning false.
    MarketEvent ev;
    CHECK_FALSE(*merged.next(ev));
    MergedEventSource empty;
    CHECK_FALSE(*empty.next(ev));
}

TEST_CASE("open_store_source: merges kinds from the store over a range") {
    TempDir tmp;
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    const Timestamp day = *Timestamp::parse_iso8601("2024-03-15");
    {
        EventStoreWriter w(store, kEth, DataSource::live);
        auto at = [&](int s) { return (day + Duration::seconds(s)).millis_since_epoch(); };
        REQUIRE(w.write(BookSnapshot{kEth, day + Duration::seconds(1), 5, {{"1"_px, "1"_qty}}, {{"2"_px, "1"_qty}}}).has_value());
        REQUIRE(w.write(trade(at(1), 1)).has_value());  // same instant as snapshot: book wins
        REQUIRE(w.write(trade(at(2), 2)).has_value());
        REQUIRE(w.write(ticker(at(2), 7)).has_value());
        REQUIRE(w.write(trade(at(90000), 3)).has_value());  // next day
        REQUIRE(w.close().has_value());
    }
    StoreSelection sel;
    sel.tickers = true;
    auto src = open_store_source(store, sel, day, day + Duration::days(2));
    REQUIRE_MESSAGE(src.has_value(), src.error().message);
    auto out = drain(**src);
    REQUIRE(out.size() == 5);
    CHECK(std::holds_alternative<BookSnapshot>(out[0]));
    CHECK(std::holds_alternative<Trade>(out[1]));
    CHECK(std::holds_alternative<Trade>(out[2]));
    CHECK(std::holds_alternative<BookTicker>(out[3]));
    CHECK(std::get<Trade>(out[4]).id == TradeId{3});

    // Range restricts; missing kinds are simply absent.
    StoreSelection only_trades;
    only_trades.book = false;
    only_trades.candle_intervals = {Duration::minutes(1)};
    src = open_store_source(store, only_trades, day + Duration::seconds(2), day + Duration::days(1));
    REQUIRE(src.has_value());
    out = drain(**src);
    REQUIRE(out.size() == 1);
    CHECK(std::get<Trade>(out[0]).id == TradeId{2});
}
