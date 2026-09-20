// Integration: PaperRuntime against an in-process fake exchange feed.

#include "tradebot/live/paper_runtime.hpp"

#include "support/ws_test_server.hpp"
#include "tradebot/net/tcp.hpp"
#include "tradebot/strategies/baselines.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace tradebot;
using namespace tradebot::live;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-paper-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

// Fake Binance REST serving exchangeInfo and depth snapshots.
class FakeRest {
public:
    FakeRest() : listener_(*net::TcpListener::bind_loopback()) {
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeRest() {
        stop_.store(true);
        thread_.join();
    }
    [[nodiscard]] std::string base() const { return "http://127.0.0.1:" + std::to_string(listener_.port()); }

private:
    void serve() {
        while (!stop_.load()) {
            auto conn = listener_.accept(Duration::millis(50));
            if (!conn) continue;
            std::string req;
            std::byte buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                auto n = conn->read_some(buf);
                if (!n || *n == 0) break;
                req.append(reinterpret_cast<const char*>(buf), *n);
            }
            std::string body = "{}";
            if (req.find("/api/v3/depth") != std::string::npos) {
                body = R"({"lastUpdateId":100,"bids":[["3000.00","50"],["2999.00","50"]],"asks":[["3001.00","50"],["3002.00","50"]]})";
            } else if (req.find("/api/v3/exchangeInfo") != std::string::npos) {
                body = R"({"symbols":[{"symbol":"ETHUSDT","baseAsset":"ETH","quoteAsset":"USDT","filters":[
                    {"filterType":"PRICE_FILTER","tickSize":"0.01"},{"filterType":"LOT_SIZE","minQty":"0.0001","stepSize":"0.0001"},
                    {"filterType":"NOTIONAL","minNotional":"5"}]}]})";
            }
            static_cast<void>(conn->write_all("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                              "\r\n\r\n" + body));
        }
    }
    net::TcpListener listener_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

std::string agg_trade(std::int64_t id, const char* price, std::int64_t ms) {
    return R"({"stream":"ethusdt@aggTrade","data":{"e":"aggTrade","E":)" + std::to_string(ms) +
           R"(,"s":"ETHUSDT","a":)" + std::to_string(id) + R"(,"p":")" + price +
           R"(","q":"0.5","f":1,"l":1,"T":)" + std::to_string(ms) + R"(,"m":false,"M":true}})";
}

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

TEST_CASE("PaperRuntime: live feed -> simulated fills -> artifacts and state; resume restores state") {
    TempDir tmp;
    FakeRest rest;
    test::TestWsServer ws;
    strategy::StrategyRegistry registry;
    strategies::register_baselines(registry);

    auto cfg = Config::parse(config_text(tmp.path, rest, ws.port(), false));
    REQUIRE(cfg.has_value());
    auto spec = parse_paper_spec(*cfg);
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
    PaperRuntime runtime(*spec, registry, nullptr, log);

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
        auto spec2 = parse_paper_spec(*cfg2);
        REQUIRE(spec2.has_value());
        PaperRuntime runtime2(*spec2, registry, nullptr, Logger{});
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
