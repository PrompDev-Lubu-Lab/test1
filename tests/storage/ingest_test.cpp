#include "tradebot/storage/ingest.hpp"

#include "support/fixtures.hpp"
#include "tradebot/market_data/raw_capture.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <random>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::storage;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-ingest-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

Timestamp ts(const char* iso) { return *Timestamp::parse_iso8601(iso); }

std::string agg(std::int64_t id, const char* price) {
    return R"({"stream":"ethusdt@aggTrade","data":{"e":"aggTrade","E":1,"s":"ETHUSDT","a":)" +
           std::to_string(id) + R"(,"p":")" + price + R"(","q":"0.1","f":1,"l":1,"T":1710504000000,"m":false,"M":true}})";
}
std::string depth(std::int64_t first, std::int64_t last) {
    return R"({"stream":"ethusdt@depth@100ms","data":{"e":"depthUpdate","E":1710504000000,"s":"ETHUSDT","U":)" +
           std::to_string(first) + R"(,"u":)" + std::to_string(last) + R"(,"b":[["3000","1"]],"a":[["3001","1"]]}})";
}
std::string snapshot(std::int64_t id) {
    return R"({"lastUpdateId":)" + std::to_string(id) + R"(,"bids":[["3000","2"]],"asks":[["3001","2"]]})";
}

std::vector<MarketEvent> read_all(const StorePath& store, StreamKind kind) {
    EventStoreReader r(store, kind, Timestamp::epoch(), ts("2100-01-01"));
    REQUIRE(r.open().has_value());
    std::vector<MarketEvent> out;
    MarketEvent ev;
    for (;;) {
        auto more = r.next(ev);
        REQUIRE_MESSAGE(more.has_value(), more.error().message);
        if (!*more) break;
        out.push_back(ev);
    }
    return out;
}

}  // namespace

TEST_CASE("ingest_raw_capture: validates, synchronizes and stores by kind") {
    TempDir tmp;
    RawCapturePath raw{tmp.path, "binance", "ETHUSDT"};
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    const Timestamp t0 = ts("2024-03-15T12:00:00Z");
    {
        RawCaptureWriter w(raw);
        auto at = [&](int ms) { return t0 + Duration::millis(ms); };
        REQUIRE(w.write({at(0), "exchange_info", R"({"symbols":[{"symbol":"ETHUSDT","baseAsset":"ETH","quoteAsset":"USDT","filters":[{"filterType":"PRICE_FILTER","tickSize":"0.01"},{"filterType":"LOT_SIZE","minQty":"0.0001","stepSize":"0.0001"}]}]})"}).has_value());
        REQUIRE(w.write({at(1), "ethusdt@depth@100ms", depth(98, 100)}).has_value());
        REQUIRE(w.write({at(2), "depth_snapshot", snapshot(100)}).has_value());
        REQUIRE(w.write({at(3), "ethusdt@depth@100ms", depth(101, 103)}).has_value());
        REQUIRE(w.write({at(4), "ethusdt@aggTrade", agg(1, "3000")}).has_value());
        REQUIRE(w.write({at(5), "ethusdt@aggTrade", agg(1, "3000")}).has_value());  // duplicate
        REQUIRE(w.write({at(6), "ethusdt@aggTrade", agg(3, "3001")}).has_value());  // gap
        REQUIRE(w.write({at(7), "ethusdt@aggTrade", R"({"stream":"ethusdt@aggTrade","data":{"e":"aggTrade"}})"}).has_value());  // parse error
        REQUIRE(w.write({at(8), "ethusdt@depth@100ms", depth(110, 112)}).has_value());  // book gap
        REQUIRE(w.write({at(9), "depth_snapshot", snapshot(111)}).has_value());  // resync
        REQUIRE(w.write({at(10), "ethusdt@kline_1m", R"({"stream":"ethusdt@kline_1m","data":{"e":"kline","E":1,"s":"ETHUSDT","k":{"t":1710504000000,"T":1710504059999,"s":"ETHUSDT","i":"1m","f":1,"L":2,"o":"1","c":"1","h":"1","l":"1","v":"1","n":1,"x":false,"q":"1","V":"1","Q":"1","B":"0"}}})"}).has_value());
        REQUIRE(w.write({at(11), "ethusdt@kline_1m", R"({"stream":"ethusdt@kline_1m","data":{"e":"kline","E":1,"s":"ETHUSDT","k":{"t":1710504000000,"T":1710504059999,"s":"ETHUSDT","i":"1m","f":1,"L":2,"o":"1","c":"1","h":"1","l":"1","v":"1","n":1,"x":true,"q":"1","V":"1","Q":"1","B":"0"}}})"}).has_value());
        REQUIRE(w.write({at(12), "ethusdt@bookTicker", R"({"stream":"ethusdt@bookTicker","data":{"u":5,"s":"ETHUSDT","b":"3000","B":"1","a":"3001","A":"1"}})"}).has_value());
        REQUIRE(w.close().has_value());
    }

    auto stats = ingest_raw_capture(raw, store, InstrumentId{1}, ts("2024-03-15"), ts("2024-03-16"), Logger{});
    REQUIRE_MESSAGE(stats.has_value(), stats.error().message);
    CHECK(stats->records_in == 13);
    CHECK(stats->parse_errors == 1);
    CHECK(stats->dropped == 2);  // duplicate trade, unclosed candle
    CHECK(stats->gaps == 2);  // trade id gap + book gap
    CHECK(stats->resyncs == 1);
    CHECK(stats->events_out == 9);  // 3 deltas + 2 snapshots + 2 trades + 1 candle + 1 ticker

    auto trades = read_all(store, StreamKind::trades);
    REQUIRE(trades.size() == 2);
    CHECK(std::get<Trade>(trades[1]).id == TradeId{3});
    auto book = read_all(store, StreamKind::book);
    CHECK(book.size() == 5);
    CHECK(std::holds_alternative<BookDelta>(book[0]));
    CHECK(std::holds_alternative<BookSnapshot>(book[1]));
    CHECK(read_all(store, StreamKind::tickers).size() == 1);
    EventStoreReader candles(store, StreamKind::candles, Timestamp::epoch(), ts("2100-01-01"), Duration::minutes(1));
    REQUIRE(candles.open().has_value());
    MarketEvent ev;
    auto more = candles.next(ev);
    REQUIRE(more.has_value());
    REQUIRE(*more);
    CHECK(std::get<Candle>(ev).closed);

    auto cat = Catalog::scan(store);
    REQUIRE(cat.has_value());
    auto tf = cat->select(StreamKind::trades, ts("2024-03-15"), ts("2024-03-16"));
    REQUIRE(tf.size() == 1);
    CHECK(tf[0].header.gap_count == 1);
    CHECK(tf[0].header.source == DataSource::live);
    auto bf = cat->select(StreamKind::book, ts("2024-03-15"), ts("2024-03-16"));
    REQUIRE(bf.size() == 1);
    CHECK(bf[0].header.gap_count == 1);
    CHECK(bf[0].header.resync_count == 1);
}

TEST_CASE("ingest_bulk_archive: aggTrades fixture -> trades file") {
    TempDir tmp;
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    auto stats = ingest_bulk_archive(test::fixture("ETHUSDT-aggTrades-2024-01-01.zip"), store, InstrumentId{1}, Logger{});
    REQUIRE_MESSAGE(stats.has_value(), stats.error().message);
    CHECK(stats->records_in == 5);
    CHECK(stats->events_out == 5);
    CHECK(stats->parse_errors == 0);
    CHECK(stats->gaps == 0);
    auto trades = read_all(store, StreamKind::trades);
    REQUIRE(trades.size() == 5);
    const auto& first = std::get<Trade>(trades[0]);
    CHECK(first.id == TradeId{100});
    CHECK(first.price == "3000"_px);
    CHECK(first.exchange_time == ts("2024-01-01"));
    CHECK(first.recv_time == first.exchange_time);
    auto cat = Catalog::scan(store);
    REQUIRE(cat->files().size() == 1);
    CHECK(cat->files()[0].header.source == DataSource::bulk);
    CHECK(cat->files()[0].day == ts("2024-01-01"));

    CHECK_FALSE(ingest_bulk_archive(test::fixture("mixed.zip"), store, InstrumentId{1}, Logger{}).has_value());
    CHECK_FALSE(ingest_bulk_archive(tmp.path / "nope.zip", store, InstrumentId{1}, Logger{}).has_value());
}
