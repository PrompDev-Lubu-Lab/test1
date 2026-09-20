// Failure drills for the live-trading modes. Every venue is an in-process
// fake: the market-data feed (FakeRest + TestWsServer), the private REST
// API (FakeBinanceRest) and the user data stream (a second TestWsServer).

#include "tradebot/live/runtime.hpp"

#include "support/fake_binance_rest.hpp"
#include "support/fake_feed.hpp"
#include "support/ws_test_server.hpp"
#include "tradebot/strategies/baselines.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

using namespace tradebot;
using namespace tradebot::live;
using namespace tradebot::literals;
using test::agg_trade;
using test::FakeBinanceRest;
using test::FakeRest;
using test::TempDir;
using test::wall_now_ms;
namespace fs = std::filesystem;

namespace {

const std::string kKey = "drill-key";
const std::string kSecret = "drill-secret";

std::string mode_config(const fs::path& dir, const FakeRest& feed_rest, std::uint16_t feed_ws, const char* mode,
                        const FakeBinanceRest& venue, std::uint16_t user_ws, const std::string& extra = "") {
    return std::string("[live]\nmode = ") + mode + "\nrest_base = " + venue.base_url() +
           "\nuser_stream_base = ws://127.0.0.1:" + std::to_string(user_ws) +
           "\nproxy = none\nquery_after = 200ms\nreconcile_interval = 300ms\nreconcile_quote_tolerance = 5\n"
           "max_clock_skew = 5s\n"
           "[paper]\nlabel = drill\nsymbol = ETHUSDT\ninitial_cash = 10000\nflush_interval = 1s\narchive_raw = false\n"
           "resume = true\nrun_dir = " + (dir / "run").string() +
           "\n[backtest]\nsample_interval = 1s\n[data]\ndir = " + dir.string() +
           "\n[collector]\nws_base = ws://127.0.0.1:" + std::to_string(feed_ws) + "\nrest_base = " + feed_rest.base() +
           "\nstreams = aggTrade, depth@100ms\nstale_timeout = 5s\n"
           "[exchange]\nlatency = zero\n"
           "[risk]\nmax_price_deviation = 0\nmax_position = 5\nmax_order_notional = 5000\nmax_drawdown = 2000\n"
           "max_daily_loss = 1000\nmax_orders_per_minute = 30\n"
           "[strategy]\nname = buy_and_hold\nlabel = bh\n[strategy.params]\ninterval = 1s\nquantity = 0.25\n" + extra;
}

// Scripts the private REST fake with a healthy account. Balances are
// served from `eth`/`usdt`, which a drill may change as fills happen.
struct VenueScript {
    std::mutex mutex;
    std::string eth = "0";
    std::string usdt = "10000";
    std::string last_client_id;  // newClientOrderId of the last POST /api/v3/order

    void arm(FakeBinanceRest& v) {
        v.on("GET /api/v3/time", [](const FakeBinanceRest::Request&) {
            return FakeBinanceRest::Reply{200, R"({"serverTime":)" + std::to_string(wall_now_ms()) + "}"};
        });
        v.on("GET /api/v3/account", [this](const FakeBinanceRest::Request&) {
            std::lock_guard lock(mutex);
            return FakeBinanceRest::Reply{200, R"({"balances":[{"asset":"ETH","free":")" + eth +
                                                   R"(","locked":"0"},{"asset":"USDT","free":")" + usdt +
                                                   R"(","locked":"0"}]})"};
        });
        v.on("GET /api/v3/openOrders", FakeBinanceRest::Reply{200, "[]"});
        v.on("POST /api/v3/userDataStream", FakeBinanceRest::Reply{200, R"({"listenKey":"lk"})"});
        v.on("PUT /api/v3/userDataStream", FakeBinanceRest::Reply{200, "{}"});
        v.on("POST /api/v3/order", [this](const FakeBinanceRest::Request& r) {
            std::lock_guard lock(mutex);
            auto it = r.params.find("newClientOrderId");
            if (it != r.params.end()) last_client_id = it->second;
            return FakeBinanceRest::Reply{200, R"({"orderId":500,"transactTime":)" + std::to_string(wall_now_ms()) + "}"};
        });
        v.on("DELETE /api/v3/order", FakeBinanceRest::Reply{200, R"({"orderId":500,"status":"CANCELED"})"});
        v.on("GET /api/v3/order",
             FakeBinanceRest::Reply{200, R"({"orderId":500,"status":"NEW","executedQty":"0","cummulativeQuoteQty":"0"})"});
    }
    std::string client_id() {
        std::lock_guard lock(mutex);
        return last_client_id;
    }
    void set_balances(std::string e, std::string u) {
        std::lock_guard lock(mutex);
        eth = std::move(e);
        usdt = std::move(u);
    }
};

std::string fill_report(const std::string& client_id, const char* qty, const char* price, std::int64_t trade_id) {
    return R"({"e":"executionReport","E":1,"s":"ETHUSDT","c":")" + client_id +
           R"(","S":"BUY","o":"MARKET","f":"GTC","q":")" + qty + R"(","p":"0","x":"TRADE","X":"FILLED","r":"NONE","i":500,"l":")" +
           qty + R"(","z":")" + qty + R"(","L":")" + price + R"(","n":"0.75","N":"USDT","T":)" + std::to_string(wall_now_ms()) +
           R"(,"t":)" + std::to_string(trade_id) + R"(,"m":false})";
}

std::size_t count_requests(const FakeBinanceRest& v, const std::string& method, const std::string& path) {
    std::size_t n = 0;
    for (const auto& r : v.requests()) {
        if (r.method == method && r.path == path) ++n;
    }
    return n;
}

struct Drill {
    TempDir tmp;
    FakeRest feed_rest;
    test::TestWsServer feed_ws;
    FakeBinanceRest venue{kKey, kSecret};
    test::TestWsServer user_ws;
    VenueScript script;
    strategy::StrategyRegistry registry;
    std::shared_ptr<MemorySink> sink = std::make_shared<MemorySink>();
    WallClock wall;
    Logger log = Logger::make("drill", sink, wall, LogLevel::debug);

    Drill() {
        strategies::register_baselines(registry);
        script.arm(venue);
    }
    Result<RuntimeSpec> spec(const char* mode, const std::string& extra = "") {
        auto cfg = Config::parse(mode_config(tmp.path, feed_rest, feed_ws.port(), mode, venue, user_ws.port(), extra));
        REQUIRE(cfg.has_value());
        auto s = parse_runtime_spec(*cfg);
        if (s) s->credentials = gateway::BinanceCredentials{kKey, kSecret};
        return s;
    }
    // TRADEBOT_DRILL_LOG=1 prints the runtime log after a drill.
    void dump() const {
        if (std::getenv("TRADEBOT_DRILL_LOG") == nullptr) return;
        for (const auto& e : sink->entries()) std::fprintf(stderr, "[%s] %s\n", e.component.c_str(), e.message.c_str());
    }
    bool logged(std::string_view needle) const {
        for (const auto& e : sink->entries()) {
            if (e.message.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    // Streams trades at 100ms until `done` or `max` iterations.
    void stream_until(const std::function<bool()>& done, int max = 100) {
        for (int i = 0; i < max && !done(); ++i) {
            feed_ws.send_text(agg_trade(i + 1, i % 2 ? "3000.50" : "3000.00", wall_now_ms()));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

}  // namespace

TEST_CASE("preflight: live mode refuses without confirmation and capital limits; testnet needs credentials") {
    Config cfg = *Config::parse("[live]\nmode = live\n[paper]\nsymbol = ETHUSDT\ninitial_cash = 100\n"
                                "[strategy]\nname = buy_and_hold\nlabel = bh\n[strategy.params]\nquantity = 0.01\n");
    auto spec = parse_runtime_spec(cfg);
    REQUIRE_MESSAGE(spec.has_value(), spec.error().message);
    CHECK(spec->mode == TradingMode::live);
    CHECK(spec->run_dir == fs::path("runs") / "live-paper");
    spec->credentials = gateway::BinanceCredentials{};
    auto v = preflight_violations(*spec);
    auto has = [&](std::string_view needle) {
        for (const auto& item : v) {
            if (item.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    CHECK(has("credentials missing"));
    CHECK(has("live.confirm"));
    CHECK(has("live.max_capital"));
    CHECK(has("max_order_notional"));
    CHECK(has("max_drawdown"));
    CHECK(has("max_daily_loss"));
    CHECK(has("max_orders_per_minute"));
    CHECK(has("max_position"));

    // Everything set: clean.
    spec->credentials = gateway::BinanceCredentials{"k", "s"};
    spec->gateway.confirm = std::string(kLiveConfirmation);
    spec->gateway.max_capital = "200"_ntl;
    spec->limits.max_order_notional = "50"_ntl;
    spec->limits.max_position = "0.1"_qty;
    spec->limits.max_drawdown = "20"_ntl;
    spec->limits.max_daily_loss = "10"_ntl;
    spec->limits.max_orders_per_minute = 10;
    CHECK(preflight_violations(*spec).empty());
    // ... but the capital cap is enforced against every notional limit.
    spec->initial_cash = "500"_ntl;
    CHECK(preflight_violations(*spec).size() == 1);
    spec->initial_cash = "100"_ntl;
    spec->gateway.rest_base = "http://api.binance.com";
    CHECK(preflight_violations(*spec).size() == 1);  // plaintext

    // Testnet: no confirmation needed, but never the production venue.
    spec->mode = TradingMode::testnet;
    spec->gateway.rest_base = "https://api.binance.com";
    v = preflight_violations(*spec);
    CHECK(v.size() == 1);
    CHECK(has("production venue"));
    spec->gateway.rest_base = "https://testnet.binance.vision";
    CHECK(preflight_violations(*spec).empty());

    // Paper never needs credentials; run() refuses live mode outright.
    spec->mode = TradingMode::paper;
    spec->credentials = gateway::BinanceCredentials{};
    CHECK(preflight_violations(*spec).empty());
    spec->mode = TradingMode::live;
    strategy::StrategyRegistry registry;
    TradingRuntime runtime(*spec, registry, nullptr, Logger{});
    auto r = runtime.run();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("refusing to start in live mode") != std::string::npos);
    CHECK(r.error().message.find("credentials missing") != std::string::npos);

    // Testnet defaults point at the testnet hosts.
    Config tcfg = *Config::parse("[live]\nmode = testnet\n[paper]\nsymbol = ETHUSDT\ninitial_cash = 100\n[strategy]\nname = buy_and_hold\n"
                                 "label = bh\n[strategy.params]\nquantity = 0.01\n");
    auto tspec = parse_runtime_spec(tcfg);
    REQUIRE(tspec.has_value());
    CHECK(tspec->gateway.rest_base == "https://testnet.binance.vision");
    CHECK(tspec->gateway.user_stream_base == "wss://stream.testnet.binance.vision");
    CHECK(tspec->run_dir == fs::path("runs") / "testnet-paper");
}

TEST_CASE("drill: startup reconciliation mismatch refuses to trade; clock skew refuses to start") {
    Drill d;
    d.script.set_balances("0", "500");  // the account does not hold the configured initial cash
    auto spec = d.spec("testnet");
    REQUIRE_MESSAGE(spec.has_value(), spec.error().message);
    {
        TradingRuntime runtime(*spec, d.registry, nullptr, d.log);
        auto r = runtime.run();
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().message.find("startup reconciliation mismatch") != std::string::npos);
    }
    CHECK(count_requests(d.venue, "GET", "/api/v3/time") == 1);
    CHECK(count_requests(d.venue, "GET", "/api/v3/account") == 2);  // preflight + reconcile
    CHECK(count_requests(d.venue, "POST", "/api/v3/order") == 0);
    for (const auto& r : d.venue.requests()) {
        if (r.path == "/api/v3/time") continue;  // public endpoint
        CHECK(r.key_ok);
        if (r.path == "/api/v3/account") CHECK(r.signature_ok);
    }

    // A venue clock far from ours is refused before any signed call.
    d.venue.on("GET /api/v3/time", [](const FakeBinanceRest::Request&) {
        return FakeBinanceRest::Reply{200, R"({"serverTime":)" + std::to_string(wall_now_ms() - 60'000) + "}"};
    });
    TradingRuntime runtime(*spec, d.registry, nullptr, d.log);
    auto r = runtime.run();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("clock skew") != std::string::npos);
    CHECK(count_requests(d.venue, "GET", "/api/v3/account") == 2);  // no new account call
}

TEST_CASE("drill: testnet order -> REST ack -> user-stream fill -> portfolio, journal, reconciliation") {
    Drill d;
    auto spec = d.spec("testnet");
    REQUIRE_MESSAGE(spec.has_value(), spec.error().message);
    TradingRuntime runtime(*spec, d.registry, nullptr, d.log);

    std::thread user_stream([&] {
        if (!d.user_ws.accept_and_handshake(Duration::seconds(10))) {
            CHECK_MESSAGE(false, "user stream never connected");
            return;
        }
        CHECK(d.user_ws.request_head.find("GET /ws/lk ") != std::string::npos);
        // Wait for the order to be sent, then report the fill on the stream.
        for (int i = 0; i < 100 && d.script.client_id().empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const std::string cid = d.script.client_id();
        if (cid.empty()) return;
        d.script.set_balances("0.25", "9249.00");  // 10000 - 0.25 * 3000 - 0.75 fee
        d.user_ws.send_text(fill_report(cid, "0.25", "3000", 9001));
        d.user_ws.send_text(fill_report(cid, "0.25", "3000", 9001));  // duplicate delivery
    });
    std::thread feed([&] {
        if (!d.feed_ws.accept_and_handshake(Duration::seconds(10))) {
            CHECK_MESSAGE(false, "feed never connected");
            runtime.stop();
            return;
        }
        d.stream_until([&] { return runtime.portfolio() != nullptr && runtime.portfolio()->stats().fills >= 1; });
        // A few more ticks so the periodic reconciliation runs after the fill.
        d.stream_until([&] { return runtime.stats().reconciliations >= 3; }, 30);
        runtime.stop();
        d.user_ws.close_tcp();
    });
    auto r = runtime.run();
    feed.join();
    user_stream.join();
    d.dump();
    REQUIRE_MESSAGE(r.has_value(), r.error().message);

    REQUIRE(runtime.gateway() != nullptr);
    CHECK(runtime.gateway()->stats().sent == 1);
    CHECK(runtime.gateway()->stats().stream_events == 2);
    CHECK(runtime.gateway()->stats().duplicates == 1);
    CHECK(runtime.portfolio()->position(InstrumentId{1}) == "0.25"_qty);
    CHECK(runtime.portfolio()->stats().fills == 1);
    CHECK(runtime.result().fills.size() == 1);
    CHECK(runtime.result().fills[0].price == "3000"_px);
    CHECK(runtime.result().fills[0].fee == "0.75"_ntl);
    CHECK(runtime.journal().appended() >= 2);  // accepted + fill
    CHECK(runtime.stats().reconciliations >= 2);
    CHECK(runtime.stats().reconcile_mismatches == 0);
    CHECK_FALSE(runtime.risk()->tripped());
    REQUIRE(runtime.user_stream() != nullptr);
    CHECK(runtime.user_stream()->stats().reports == 2);
    CHECK(runtime.stats().shutdown_cancels == 0);
    CHECK(count_requests(d.venue, "DELETE", "/api/v3/order") == 0);
    // Every private call carried the key and a valid signature.
    for (const auto& req : d.venue.requests()) {
        if (req.path == "/api/v3/time") continue;
        CHECK(req.key_ok);
        if (req.path == "/api/v3/order") {
            CHECK(req.signature_ok);
            CHECK(req.params.at("symbol") == "ETHUSDT");
            CHECK(req.params.at("type") == "MARKET");
        }
    }
    // Artifacts and state are written like any other run.
    for (const char* f : {"equity.csv", "fills.csv", "orders.csv", "summary.json", "state.json", "journal.jsonl", "heartbeat"}) {
        CHECK_MESSAGE(fs::exists(d.tmp.path / "run" / f), f);
    }
    CHECK(d.logged("startup reconciliation ok"));
    CHECK(d.logged("testnet trading drill started"));
}

TEST_CASE("drill: reconciliation drift trips the kill switch") {
    Drill d;
    auto spec = d.spec("testnet", "[risk]\nflatten_on_trip = false\n");
    REQUIRE(spec.has_value());
    TradingRuntime runtime(*spec, d.registry, nullptr, d.log);
    std::thread user_stream([&] {
        if (!d.user_ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); return; }
        for (int i = 0; i < 100 && d.script.client_id().empty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const std::string cid = d.script.client_id();
        if (cid.empty()) return;
        // The venue keeps reporting the pre-fill balances: our view drifts.
        d.user_ws.send_text(fill_report(cid, "0.25", "3000", 9002));
    });
    std::thread feed([&] {
        if (!d.feed_ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); runtime.stop(); return; }
        d.stream_until([&] { return runtime.risk() != nullptr && runtime.risk()->tripped(); });
        runtime.stop();
        d.user_ws.close_tcp();
    });
    auto r = runtime.run();
    feed.join();
    user_stream.join();
    d.dump();
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    CHECK(runtime.portfolio()->stats().fills == 1);
    CHECK(runtime.risk()->tripped());
    CHECK(runtime.risk()->trip_reason().find("reconciliation mismatch") != std::string::npos);
    CHECK(runtime.stats().reconcile_mismatches >= 1);
}

TEST_CASE("drill: stale feed cancels the working order at the venue; stale orders from a previous run are cancelled at start") {
    Drill d;
    d.venue.on("GET /api/v3/openOrders",
               FakeBinanceRest::Reply{200, R"([{"clientOrderId":"tb7","orderId":7},{"clientOrderId":"web_abc","orderId":8}])"});
    auto spec = d.spec("testnet", "[paper]\nfeed_stale_after = 1s\nauto_rearm = false\n[risk]\nflatten_on_trip = false\n");
    REQUIRE(spec.has_value());
    CHECK(spec->feed_stale_after == Duration::seconds(1));
    TradingRuntime runtime(*spec, d.registry, nullptr, d.log);
    std::thread user_stream([&] {
        if (!d.user_ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); }
        // Never reports anything: the market order stays acknowledged but unfilled.
    });
    std::thread feed([&] {
        if (!d.feed_ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); runtime.stop(); return; }
        d.stream_until([&] { return !d.script.client_id().empty(); });
        // Silence: the feed monitor trips the kill switch, which cancels at the venue.
        for (int i = 0; i < 60 && count_requests(d.venue, "DELETE", "/api/v3/order") < 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        runtime.stop();
        d.user_ws.close_tcp();
    });
    auto r = runtime.run();
    feed.join();
    user_stream.join();
    d.dump();
    REQUIRE_MESSAGE(r.has_value(), r.error().message);

    // Startup: the stale "tb7" order was cancelled, the foreign one left alone.
    CHECK(runtime.stats().startup_cancels == 1);
    CHECK(d.logged("foreign open order 8"));
    std::vector<std::string> cancelled;
    for (const auto& req : d.venue.requests()) {
        if (req.method == "DELETE") cancelled.push_back(req.params.at("origClientOrderId"));
    }
    REQUIRE(cancelled.size() == 2);
    CHECK(cancelled[0] == "tb7");
    CHECK(cancelled[1] == d.script.client_id());
    // The kill switch tripped on staleness and the working order is gone.
    CHECK(runtime.risk()->tripped());
    CHECK(runtime.risk()->stats().kill_switch_trips == 1);
    CHECK(runtime.gateway()->stats().cancels_sent == 1);
    CHECK(runtime.gateway()->open_order_ids().empty());
    CHECK(runtime.portfolio()->stats().fills == 0);
    CHECK(runtime.stats().shutdown_cancels == 0);
    bool cancelled_report = false;
    for (const auto& o : runtime.result().orders) cancelled_report |= o.type == execution::ReportType::cancelled;
    CHECK(cancelled_report);
}

TEST_CASE("drill: shutdown cancels working orders") {
    Drill d;
    auto spec = d.spec("testnet");
    REQUIRE(spec.has_value());
    TradingRuntime runtime(*spec, d.registry, nullptr, d.log);
    std::thread user_stream([&] {
        if (!d.user_ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); }
    });
    std::thread feed([&] {
        if (!d.feed_ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); runtime.stop(); return; }
        d.stream_until([&] { return !d.script.client_id().empty(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let the ack land
        runtime.stop();
        d.user_ws.close_tcp();
    });
    auto r = runtime.run();
    feed.join();
    user_stream.join();
    d.dump();
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    CHECK(runtime.stats().shutdown_cancels == 1);
    CHECK(count_requests(d.venue, "DELETE", "/api/v3/order") == 1);
    CHECK(runtime.gateway()->open_order_ids().empty());
    CHECK(d.logged("cancelling 1 working order(s) on shutdown"));
}

TEST_CASE("drill: shadow mode fills in simulation, exercises the account plumbing, never sends an order") {
    Drill d;
    auto spec = d.spec("shadow");
    REQUIRE_MESSAGE(spec.has_value(), spec.error().message);
    CHECK(spec->mode == TradingMode::shadow);
    TradingRuntime runtime(*spec, d.registry, nullptr, d.log);
    std::thread user_stream([&] {
        if (!d.user_ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); return; }
        // A foreign fill on the account is ignored in shadow mode.
        d.user_ws.send_text(fill_report("tb999", "1", "3000", 1));
    });
    std::thread feed([&] {
        if (!d.feed_ws.accept_and_handshake(Duration::seconds(10))) { CHECK(false); runtime.stop(); return; }
        d.stream_until([&] { return runtime.portfolio() != nullptr && runtime.portfolio()->stats().fills >= 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        runtime.stop();
        d.user_ws.close_tcp();
    });
    auto r = runtime.run();
    feed.join();
    user_stream.join();
    d.dump();
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    CHECK(runtime.gateway() == nullptr);
    CHECK(runtime.stats().shadow_orders == 1);
    CHECK(runtime.portfolio()->position(InstrumentId{1}) == "0.25"_qty);
    CHECK(runtime.result().fills.size() == 1);
    CHECK(runtime.result().fills[0].price == "3001"_px);  // simulated: took the ask
    CHECK(count_requests(d.venue, "POST", "/api/v3/order") == 0);
    CHECK(count_requests(d.venue, "DELETE", "/api/v3/order") == 0);
    CHECK(count_requests(d.venue, "GET", "/api/v3/time") == 1);
    CHECK(count_requests(d.venue, "GET", "/api/v3/account") == 1);
    CHECK(count_requests(d.venue, "POST", "/api/v3/userDataStream") == 1);
    REQUIRE(runtime.user_stream() != nullptr);
    CHECK(runtime.user_stream()->stats().connects == 1);
    CHECK(d.logged("SHADOW would send buy 0.25 market"));
    CHECK(d.logged("SHADOW account event ignored"));
    CHECK(fs::exists(d.tmp.path / "run" / "state.json"));
}
