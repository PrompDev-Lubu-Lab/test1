#include "tradebot/market_data/binance/bulk_fetcher.hpp"

#include "support/fake_http_server.hpp"
#include "support/fixtures.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>

using namespace tradebot;
using namespace tradebot::market_data::binance;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-bulk-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

Timestamp day(const char* iso) { return *Timestamp::parse_iso8601(iso); }

}  // namespace

TEST_CASE("BulkArchive: names, URL paths and local paths") {
    BulkArchive daily{BulkDataset::agg_trades, "ETHUSDT", "", false, day("2024-01-05"),
                      day("2024-01-06")};
    CHECK(daily.file_name() == "ETHUSDT-aggTrades-2024-01-05.zip");
    CHECK(daily.url_path() == "/data/spot/daily/aggTrades/ETHUSDT/ETHUSDT-aggTrades-2024-01-05.zip");
    CHECK(daily.local_path("/d") ==
          fs::path("/d/bulk/binance/ETHUSDT/aggTrades/ETHUSDT-aggTrades-2024-01-05.zip"));

    BulkArchive monthly{BulkDataset::klines, "ETHUSDT", "1m", true, day("2024-02-01"),
                        day("2024-03-01")};
    CHECK(monthly.file_name() == "ETHUSDT-1m-2024-02.zip");
    CHECK(monthly.url_path() == "/data/spot/monthly/klines/ETHUSDT/1m/ETHUSDT-1m-2024-02.zip");
    CHECK(monthly.local_path("/d") ==
          fs::path("/d/bulk/binance/ETHUSDT/klines_1m/ETHUSDT-1m-2024-02.zip"));

    BulkArchive trades{BulkDataset::trades, "BTCUSDT", "", true, day("2023-12-01"), day("2024-01-01")};
    CHECK(trades.url_path() == "/data/spot/monthly/trades/BTCUSDT/BTCUSDT-trades-2023-12.zip");

    CHECK(*parse_bulk_dataset("aggTrades") == BulkDataset::agg_trades);
    CHECK(*parse_bulk_dataset("klines") == BulkDataset::klines);
    CHECK_FALSE(parse_bulk_dataset("candles").has_value());
}

TEST_CASE("plan_bulk_archives: monthly for whole months, daily for the rest") {
    // Jan 30 .. Apr 3: 2 daily (Jan 30, 31), Feb monthly, Mar monthly, 2 daily (Apr 1, 2).
    auto plan = plan_bulk_archives(BulkDataset::agg_trades, "ETHUSDT", "", day("2024-01-30"),
                                   day("2024-04-03"));
    REQUIRE(plan.size() == 6);
    CHECK(plan[0].file_name() == "ETHUSDT-aggTrades-2024-01-30.zip");
    CHECK(plan[1].file_name() == "ETHUSDT-aggTrades-2024-01-31.zip");
    CHECK(plan[2].file_name() == "ETHUSDT-aggTrades-2024-02.zip");
    CHECK(plan[2].monthly);
    CHECK(plan[2].period_end == day("2024-03-01"));
    CHECK(plan[3].file_name() == "ETHUSDT-aggTrades-2024-03.zip");
    CHECK(plan[4].file_name() == "ETHUSDT-aggTrades-2024-04-01.zip");
    CHECK(plan[5].file_name() == "ETHUSDT-aggTrades-2024-04-02.zip");

    // Exactly one month.
    plan = plan_bulk_archives(BulkDataset::klines, "ETHUSDT", "1m", day("2023-12-01"),
                              day("2024-01-01"));
    REQUIRE(plan.size() == 1);
    CHECK(plan[0].file_name() == "ETHUSDT-1m-2023-12.zip");

    // Year boundary, partial: Dec 31 daily then Jan monthly.
    plan = plan_bulk_archives(BulkDataset::trades, "X", "", day("2023-12-31"), day("2024-02-01"));
    REQUIRE(plan.size() == 2);
    CHECK_FALSE(plan[0].monthly);
    CHECK(plan[1].monthly);

    // Sub-day times are truncated; empty range yields nothing.
    plan = plan_bulk_archives(BulkDataset::trades, "X", "", day("2024-01-01T05:00:00Z"),
                              day("2024-01-02T23:00:00Z"));
    REQUIRE(plan.size() == 1);
    CHECK(plan_bulk_archives(BulkDataset::trades, "X", "", day("2024-01-02"), day("2024-01-01")).empty());
}

TEST_CASE("BulkFetcher: download, verify, skip when present, re-download when corrupt") {
    TempDir tmp;
    test::FakeHttpServer server;
    const std::string zip = test::read_fixture("ETHUSDT-aggTrades-2024-01-01.zip");
    const std::string checksum = test::read_fixture("ETHUSDT-aggTrades-2024-01-01.zip.CHECKSUM");
    const std::string path = "/data/spot/daily/aggTrades/ETHUSDT/ETHUSDT-aggTrades-2024-01-01.zip";
    server.route(path, {200, zip});
    server.route(path + ".CHECKSUM", {200, checksum});

    auto http = *net::HttpClient::create(nullptr, {.timeout = Duration::seconds(5),
                                                   .proxy = net::ProxyConfig{}});
    BulkFetcher fetcher(http, {.base_url = server.base_url(), .root = tmp.path}, Logger{});
    const BulkArchive archive{BulkDataset::agg_trades, "ETHUSDT", "", false, day("2024-01-01"),
                              day("2024-01-02")};
    const fs::path local = archive.local_path(tmp.path);

    auto r = fetcher.fetch(archive);
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    CHECK(*r == FetchOutcome::downloaded);
    REQUIRE(fs::exists(local));
    CHECK(fs::file_size(local) == zip.size());
    CHECK_FALSE(fs::exists(local.string() + ".part"));
    CHECK(server.request_count(path) == 1);

    // Second fetch: checksum still requested, archive not re-downloaded.
    r = fetcher.fetch(archive);
    REQUIRE(r.has_value());
    CHECK(*r == FetchOutcome::already_present);
    CHECK(server.request_count(path) == 1);
    CHECK(server.request_count(path + ".CHECKSUM") == 2);

    // Corrupt the local file: it must be replaced.
    {
        std::ofstream out(local, std::ios::binary | std::ios::trunc);
        out << "garbage";
    }
    r = fetcher.fetch(archive);
    REQUIRE(r.has_value());
    CHECK(*r == FetchOutcome::downloaded);
    CHECK(fs::file_size(local) == zip.size());
    CHECK(server.request_count(path) == 2);

    const auto& s = fetcher.stats();
    CHECK(s.downloaded == 2);
    CHECK(s.skipped == 1);
    CHECK(s.not_available == 0);
    CHECK(s.bytes == 2 * zip.size());
}

TEST_CASE("BulkFetcher: not available, checksum mismatch, server errors") {
    TempDir tmp;
    test::FakeHttpServer server;
    auto http = *net::HttpClient::create(nullptr, {.timeout = Duration::seconds(5),
                                                   .proxy = net::ProxyConfig{}});
    BulkFetcher fetcher(http, {.base_url = server.base_url(), .root = tmp.path}, Logger{});

    // No CHECKSUM -> not available, no archive request made.
    const BulkArchive missing{BulkDataset::agg_trades, "ETHUSDT", "", false, day("2024-01-02"),
                              day("2024-01-03")};
    auto r = fetcher.fetch(missing);
    REQUIRE(r.has_value());
    CHECK(*r == FetchOutcome::not_available);
    CHECK(server.request_count(missing.url_path()) == 0);

    // CHECKSUM that does not match the served bytes -> error, file removed.
    const std::string bad_path = "/data/spot/daily/aggTrades/ETHUSDT/ETHUSDT-aggTrades-2024-01-03.zip";
    server.route(bad_path, {200, test::read_fixture("bad.zip")});
    server.route(bad_path + ".CHECKSUM", {200, test::read_fixture("bad.zip.CHECKSUM")});
    const BulkArchive bad{BulkDataset::agg_trades, "ETHUSDT", "", false, day("2024-01-03"),
                          day("2024-01-04")};
    r = fetcher.fetch(bad);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::io_error);
    CHECK_FALSE(fs::exists(bad.local_path(tmp.path)));
    CHECK_FALSE(fs::exists(bad.local_path(tmp.path).string() + ".part"));

    // Malformed CHECKSUM body.
    const std::string weird_path = "/data/spot/daily/aggTrades/ETHUSDT/ETHUSDT-aggTrades-2024-01-04.zip";
    server.route(weird_path + ".CHECKSUM", {200, "not a checksum"});
    const BulkArchive weird{BulkDataset::agg_trades, "ETHUSDT", "", false, day("2024-01-04"),
                            day("2024-01-05")};
    r = fetcher.fetch(weird);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::parse_error);

    // Server error on CHECKSUM.
    const std::string err_path = "/data/spot/daily/aggTrades/ETHUSDT/ETHUSDT-aggTrades-2024-01-05.zip";
    server.route(err_path + ".CHECKSUM", {503, "down"});
    const BulkArchive err{BulkDataset::agg_trades, "ETHUSDT", "", false, day("2024-01-05"),
                          day("2024-01-06")};
    r = fetcher.fetch(err);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::network_error);

    // fetch_all stops at the first hard error.
    auto all = fetcher.fetch_all({missing, err, weird});
    REQUIRE_FALSE(all.has_value());
    CHECK(fetcher.stats().not_available == 2);
}
