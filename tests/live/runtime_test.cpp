// Integration: TradingRuntime against an in-process fake exchange feed.

#include "tradebot/live/journal.hpp"
#include "tradebot/live/runtime.hpp"

#include "support/fake_feed.hpp"
#include "support/ws_test_server.hpp"
#include "tradebot/net/tcp.hpp"
#include "tradebot/strategies/baselines.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace tradebot;
using namespace tradebot::live;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

using test::agg_trade;
using test::FakeRest;
using test::TempDir;

std::string config_text(const fs::path& dir, const FakeRest& rest, std::uint16_t ws_port, bool resume) {
    return "[paper]\nlabel = test\nsymbol = ETHUSDT\ninitial_cash = 10000\nflush_interval = 1s\narchive_raw = true\nresume = " +
           std::string(resume ? "true" : "false") + "\nrun_dir = " + (dir / "run").string() +
           "\n[backtest]\nsample_interval = 1s\n[data]\ndir = " + dir.string() +
           "\n[collector]\nws_base = ws://127.0.0.1:" + std::to_string(ws_port) + "\nrest_base = " + rest.base() +
           "\nstreams = aggTrade, depth@100ms\nstale_timeout = 2s\n"
           "[exchange]\nlatency = zero\n[risk]\nmax_price_deviation = 0\nmax_position = 5\n"
           "[strategy]\nname = buy_and_hold\nlabel = bh\n[strategy.params]\ninterval = 1s\nquantity = 0.25\n";
}

}  // namespace

TEST_CASE("TradingRuntime: live feed -> simulated fills -> artifacts and state; resume restores state") {
    TempDir tmp;
    FakeRest rest;
    test::TestWsServer ws;
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);

    auto cfg = Config::parse(config_text(tmp.path, rest, ws.port(), false));
    REQUIRE(cfg.has_value());
    auto spec = parse_runtime_spec(*cfg);
    REQUIRE_MESSAGE(spec.has_value(), spec.error().message);
    CHECK(spec->label == "test");
    CHECK(spec->collector.symbol == "ETHUSDT");
    CHECK(spec->collector.streams.size() == 2);
    CHECK(spec->strategies[0].name == "buy_and_hold");
    CHECK(spec->flush_interval == Duration::seconds(1));
    CHECK_FALSE(spec->resume);

    auto sink = std::make_shared<MemorySink>();
    WallClock wall;
    Logger log = Logger::make("paper", sink, wall, LogLevel::debug);
    TradingRuntime runtime(*spec, registry, nullptr, log);

    // The fake exchange: handshake, stream trades once per 100ms for ~3s
    // (enough 1s candles for buy_and_hold to enter), then stop the runtime.
    std::thread server([&] {
        if (!ws.accept_and_handshake(Duration::seconds(10))) {
            CHECK_MESSAGE(false, "feed never connected");
            runtime.stop();
            return;
        }
        const auto now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        };
        for (int i = 0; i < 30; ++i) {
            ws.send_text(agg_trade(i + 1, i % 2 ? "3000.50" : "3000.00", now_ms()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        runtime.stop();
    });
    auto r = runtime.run();
    server.join();
    REQUIRE_MESSAGE(r.has_value(), r.error().message);

    const auto* pf = runtime.portfolio();
    REQUIRE(pf != nullptr);
    CHECK(pf->position(InstrumentId{1}) == "0.25"_qty);
    CHECK(pf->stats().fills == 1);
    CHECK(runtime.result().fills.size() == 1);
    CHECK(runtime.result().fills[0].price == "3001"_px);  // took the ask from the snapshot
    CHECK(pf->equity_curve().size() >= 3);
    REQUIRE(runtime.collector_stats() != nullptr);
    CHECK(runtime.collector_stats()->messages == 30);
    CHECK(runtime.collector_stats()->depth_snapshots == 1);

    const fs::path run = tmp.path / "run";
    for (const char* f : {"config.txt", "equity.csv", "fills.csv", "orders.csv", "summary.json", "state.json", "report.txt"}) {
        CHECK_MESSAGE(fs::exists(run / f), f);
    }
    // Raw archive was written too.
    CHECK(fs::exists(tmp.path / "raw" / "binance" / "ETHUSDT"));
    // Logged the instrument rules from exchange_info.
    bool rules_logged = false;
    for (const auto& e : sink->entries()) rules_logged |= e.message.find("instrument rules") != std::string::npos;
    CHECK(rules_logged);

    // Restart with resume: the position comes back before any fill.
    {
        FakeRest rest2;
        test::TestWsServer ws2;
        auto cfg2 = Config::parse(config_text(tmp.path, rest2, ws2.port(), true));
        auto spec2 = parse_runtime_spec(*cfg2);
        REQUIRE(spec2.has_value());
        TradingRuntime runtime2(*spec2, registry, nullptr, Logger{});
        std::thread server2([&] {
            if (!ws2.accept_and_handshake(Duration::seconds(10))) {
                CHECK_MESSAGE(false, "feed never reconnected");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            runtime2.stop();
        });
        auto r2 = runtime2.run();
        server2.join();
        REQUIRE_MESSAGE(r2.has_value(), r2.error().message);
        CHECK(runtime2.portfolio()->position(InstrumentId{1}) == "0.25"_qty);
        CHECK(runtime2.portfolio()->cash() < "10000"_ntl);
        CHECK(runtime2.result().fills.empty());
    }
}

TEST_CASE("TradingRuntime: chaos - malformed messages, duplicates, disconnects, journal rebuild") {
    TempDir tmp;
    FakeRest rest;
    test::TestWsServer ws;
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    auto cfg = Config::parse(config_text(tmp.path, rest, ws.port(), false) + "[collector]\nstale_timeout = 1s\n");
    // stale_timeout must be re-set after the base config's [collector] section; Config keeps the last value.
    REQUIRE(cfg.has_value());
    auto spec = parse_runtime_spec(*cfg);
    REQUIRE(spec.has_value());
    spec->collector.reconnect_backoff_min = Duration::millis(20);
    spec->collector.reconnect_backoff_max = Duration::millis(50);
    spec->feed_stale_after = Duration::seconds(30);  // do not trip on the short pauses below

    auto sink = std::make_shared<MemorySink>();
    WallClock wall;
    Logger log = Logger::make("paper", sink, wall, LogLevel::debug);
    TradingRuntime runtime(*spec, registry, nullptr, log);

    std::thread server([&] {
        const auto now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        };
        // Connection 1: trades with a malformed message and a duplicate, then a hard TCP drop.
        if (!ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); runtime.stop(); return; }
        for (int i = 0; i < 12; ++i) {
            ws.send_text(agg_trade(i + 1, "3000.00", now_ms()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ws.send_text(R"({"stream":"ethusdt@aggTrade","data":{"e":"aggTrade","broken":true}})");
        ws.send_text(agg_trade(12, "3000.00", now_ms()));  // duplicate id
        ws.send_text("this is not even json");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ws.close_tcp();
        // Connection 2 after reconnect: more trades, then stop.
        if (!ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); runtime.stop(); return; }
        for (int i = 20; i < 30; ++i) {
            ws.send_text(agg_trade(i, "3000.50", now_ms()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        runtime.stop();
    });
    auto r = runtime.run();
    server.join();
    REQUIRE_MESSAGE(r.has_value(), r.error().message);

    REQUIRE(runtime.collector_stats() != nullptr);
    CHECK(runtime.collector_stats()->reconnects == 1);
    CHECK(runtime.collector_stats()->connects == 2);
    CHECK(runtime.collector_stats()->depth_snapshots == 2);  // one per connect
    CHECK(runtime.portfolio()->position(InstrumentId{1}) == "0.25"_qty);
    CHECK(runtime.portfolio()->stats().fills == 1);
    CHECK(runtime.journal().appended() >= 2);  // accepted + fill
    CHECK(runtime.feed_health().state() != FeedState::waiting);
    CHECK(fs::exists(tmp.path / "run" / "heartbeat"));
    CHECK(fs::exists(tmp.path / "run" / "journal.jsonl"));
    bool parse_error_logged = false;
    for (const auto& e : sink->entries()) parse_error_logged |= e.message.find("parse error") != std::string::npos;
    CHECK(parse_error_logged);

    // Journal contents match the portfolio; rebuild from it after losing state.json.
    std::vector<execution::ExecutionReport> reports;
    auto skipped = Journal::replay(tmp.path / "run" / "journal.jsonl", [&](const execution::ExecutionReport& rep) { reports.push_back(rep); });
    REQUIRE(skipped.has_value());
    CHECK(*skipped == 0);
    CHECK(reports.size() == runtime.journal().appended());
    fs::remove(tmp.path / "run" / "state.json");
    {
        FakeRest rest2;
        test::TestWsServer ws2;
        auto cfg2 = Config::parse(config_text(tmp.path, rest2, ws2.port(), true));
        auto spec2 = parse_runtime_spec(*cfg2);
        REQUIRE(spec2.has_value());
        TradingRuntime runtime2(*spec2, registry, nullptr, Logger{});
        std::thread server2([&] {
            if (!ws2.accept_and_handshake(Duration::seconds(10))) { CHECK(false); }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            runtime2.stop();
        });
        auto r2 = runtime2.run();
        server2.join();
        REQUIRE(r2.has_value());
        CHECK(runtime2.portfolio()->position(InstrumentId{1}) == "0.25"_qty);
        CHECK(runtime2.portfolio()->stats().fills == 1);
        // The journal now holds both sessions' reports; a third start replays it idempotently.
    }
}

TEST_CASE("TradingRuntime: stale feed trips the kill switch and re-arms on recovery") {
    TempDir tmp;
    FakeRest rest;
    test::TestWsServer ws;
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);
    auto cfg = Config::parse(config_text(tmp.path, rest, ws.port(), false) + "[paper]\nfeed_stale_after = 1s\n");
    auto spec = parse_runtime_spec(*cfg);
    REQUIRE(spec.has_value());
    CHECK(spec->feed_stale_after == Duration::seconds(1));
    spec->collector.stale_timeout = Duration::seconds(30);  // the collector itself stays connected
    TradingRuntime runtime(*spec, registry, nullptr, Logger{});
    std::atomic<bool> tripped_seen{false};
    std::thread server([&] {
        const auto now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        };
        if (!ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); runtime.stop(); return; }
        ws.send_text(agg_trade(1, "3000.00", now_ms()));
        // Silence for > 1s (plus the 1s check cadence) => stale.
        for (int i = 0; i < 40 && !runtime.risk()->tripped(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        tripped_seen.store(runtime.risk()->tripped());
        // Resume: data flows again => healthy => re-armed.
        for (int i = 2; i < 30 && runtime.risk()->tripped(); ++i) {
            ws.send_text(agg_trade(i, "3000.00", now_ms()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        runtime.stop();
    });
    auto r = runtime.run();
    server.join();
    REQUIRE(r.has_value());
    CHECK(tripped_seen.load());
    CHECK_FALSE(runtime.risk()->tripped());
    CHECK(runtime.risk()->stats().kill_switch_trips == 1);
    CHECK(runtime.feed_health().stale_episodes() == 1);
}
