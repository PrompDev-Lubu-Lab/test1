#include "tradebot/storage/event_store.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
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
        path = fs::temp_directory_path() / ("tradebot-estore-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

const InstrumentId kEth{1};
Timestamp ts(const char* iso) { return *Timestamp::parse_iso8601(iso); }

Trade trade_at(Timestamp t, std::uint64_t id) {
    Trade tr;
    tr.instrument = kEth;
    tr.exchange_time = t;
    tr.recv_time = t;
    tr.id = TradeId{id};
    tr.price = "3000"_px;
    tr.quantity = "1"_qty;
    tr.aggressor = Side::buy;
    return tr;
}

Candle candle_at(Timestamp open, Duration interval) {
    Candle c;
    c.instrument = kEth;
    c.open_time = open;
    c.interval = interval;
    c.recv_time = open + interval;
    c.open = c.high = c.low = c.close = "1"_px;
    c.closed = true;
    return c;
}

}  // namespace

TEST_CASE("StorePath: layout") {
    StorePath p{"/d", "binance", "ETHUSDT"};
    CHECK(p.dir() == fs::path("/d/store/binance/ETHUSDT"));
    CHECK(p.file(ts("2024-03-15T13:00:00Z"), StreamKind::trades) ==
          fs::path("/d/store/binance/ETHUSDT/2024-03-15/trades.tbev"));
    CHECK(p.file(ts("2024-03-15"), StreamKind::candles, Duration::minutes(5)) ==
          fs::path("/d/store/binance/ETHUSDT/2024-03-15/candles_5m.tbev"));
    CHECK(StorePath::file_name(StreamKind::book) == "book.tbev");
}

TEST_CASE("EventStoreWriter: routes by day and kind; Catalog and range reader") {
    TempDir tmp;
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    {
        EventStoreWriter w(store, kEth, DataSource::live);
        // Day 1: trades, a ticker, 1m and 5m candles. Day 2: trades only. Day 3: trades.
        REQUIRE(w.write(trade_at(ts("2024-03-15T23:59:59Z"), 1)).has_value());
        REQUIRE(w.write(BookTicker{kEth, ts("2024-03-15T23:59:59.5Z"), 1, "1"_px, "1"_qty, "2"_px, "1"_qty}).has_value());
        REQUIRE(w.write(candle_at(ts("2024-03-15T23:58:00Z"), Duration::minutes(1))).has_value());
        // Candles are keyed by close time (when they become observable).
        REQUIRE(w.write(candle_at(ts("2024-03-15T23:50:00Z"), Duration::minutes(5))).has_value());
        REQUIRE(w.write(trade_at(ts("2024-03-16T00:00:00Z"), 2)).has_value());
        REQUIRE(w.write(trade_at(ts("2024-03-16T12:00:00Z"), 3)).has_value());
        w.note_gap(StreamKind::trades);
        REQUIRE(w.write(trade_at(ts("2024-03-17T00:00:01Z"), 4)).has_value());
        REQUIRE(w.close().has_value());
        CHECK(w.stats().events == 7);
        CHECK(w.stats().files == 6);
    }

    auto cat = Catalog::scan(store);
    REQUIRE_MESSAGE(cat.has_value(), cat.error().message);
    CHECK(cat->broken().empty());
    REQUIRE(cat->files().size() == 6);
    CHECK(cat->files()[0].day == ts("2024-03-15"));
    CHECK(cat->files()[0].header.kind == StreamKind::trades);
    CHECK(cat->files()[0].header.record_count == 1);
    CHECK(cat->files()[3].header.kind == StreamKind::candles);
    CHECK(cat->files()[3].header.candle_interval_ns == Duration::minutes(5).count_nanos());
    auto day2 = cat->select(StreamKind::trades, ts("2024-03-16"), ts("2024-03-17"));
    REQUIRE(day2.size() == 1);
    CHECK(day2[0].header.record_count == 2);
    CHECK(day2[0].header.gap_count == 1);
    CHECK(cat->select(StreamKind::candles, ts("2024-03-15"), ts("2024-03-16"), Duration::minutes(1)).size() == 1);
    CHECK(cat->select(StreamKind::candles, ts("2024-03-15"), ts("2024-03-16"), Duration::hours(1)).empty());
    auto missing = cat->missing_days(StreamKind::tickers, ts("2024-03-15"), ts("2024-03-18"));
    REQUIRE(missing.size() == 2);
    CHECK(missing[0] == ts("2024-03-16"));
    CHECK(cat->to_string().find("trades.tbev") != std::string::npos);

    // Range read across days with filtering.
    EventStoreReader r(store, StreamKind::trades, ts("2024-03-15T23:59:59.5Z"), ts("2024-03-17T00:00:01Z"));
    REQUIRE(r.open().has_value());
    CHECK(r.files().size() == 3);
    std::vector<std::uint64_t> ids;
    MarketEvent ev;
    for (;;) {
        auto more = r.next(ev);
        REQUIRE_MESSAGE(more.has_value(), more.error().message);
        if (!*more) break;
        ids.push_back(std::get<Trade>(ev).id.value());
    }
    CHECK(ids == std::vector<std::uint64_t>{2, 3});  // 1 is before from, 4 is at `to` (exclusive)

    EventStoreReader all(store, StreamKind::trades, Timestamp::epoch(), ts("2100-01-01"));
    REQUIRE(all.open().has_value());
    int n = 0;
    while (true) {
        auto more = all.next(ev);
        REQUIRE(more.has_value());
        if (!*more) break;
        ++n;
    }
    CHECK(n == 4);

    EventStoreReader none(store, StreamKind::book, Timestamp::epoch(), ts("2100-01-01"));
    REQUIRE(none.open().has_value());
    auto more = none.next(ev);
    REQUIRE(more.has_value());
    CHECK_FALSE(*more);

    // A rewrite of one day replaces that day's file only.
    {
        EventStoreWriter w(store, kEth, DataSource::live);
        REQUIRE(w.write(trade_at(ts("2024-03-16T05:00:00Z"), 99)).has_value());
        REQUIRE(w.close().has_value());
    }
    cat = Catalog::scan(store);
    auto d2 = cat->select(StreamKind::trades, ts("2024-03-16"), ts("2024-03-17"));
    REQUIRE(d2.size() == 1);
    CHECK(d2[0].header.record_count == 1);
    CHECK(cat->select(StreamKind::trades, ts("2024-03-15"), ts("2024-03-16"))[0].header.record_count == 1);
}

TEST_CASE("Catalog: empty store and broken files") {
    TempDir tmp;
    StorePath store{tmp.path, "binance", "ETHUSDT"};
    auto cat = Catalog::scan(store);
    REQUIRE(cat.has_value());
    CHECK(cat->files().empty());
    fs::create_directories(store.day_dir(ts("2024-01-01")));
    {
        std::ofstream bad(store.file(ts("2024-01-01"), StreamKind::trades), std::ios::binary);
        bad << "garbage garbage garbage garbage garbage garbage garbage garbage garbage";
    }
    cat = Catalog::scan(store);
    REQUIRE(cat.has_value());
    CHECK(cat->files().empty());
    REQUIRE(cat->broken().size() == 1);
    CHECK(cat->to_string().find("BROKEN") != std::string::npos);
}
