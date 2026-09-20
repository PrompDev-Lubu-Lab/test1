#include "tradebot/market_data/binance/collector.hpp"

#include "support/ws_test_server.hpp"
#include "tradebot/net/tcp.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <random>
#include <thread>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::market_data::binance;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-collector-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

// Fake Binance REST: serves exchangeInfo and depth with a counter.
class FakeRest {
public:
    FakeRest() : listener_(*net::TcpListener::bind_loopback()) {
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeRest() {
        stop_.store(true);
        thread_.join();
    }
    [[nodiscard]] std::string base() const {
        return "http://127.0.0.1:" + std::to_string(listener_.port());
    }
    std::atomic<int> depth_requests{0};
    std::atomic<int> info_requests{0};
    std::vector<std::string> paths;
    std::mutex mutex;

private:
    void serve() {
        while (!stop_.load()) {
            auto conn = listener_.accept(Duration::millis(100));
            if (!conn) {
                continue;
            }
            std::string req;
            std::byte buf[4096];
            while (req.find("\r\n\r\n") == std::string::npos) {
                auto n = conn->read_some(buf);
                if (!n || *n == 0) {
                    break;
                }
                req.append(reinterpret_cast<const char*>(buf), *n);
            }
            const std::string path = req.substr(4, req.find(' ', 4) - 4);
            {
                std::lock_guard lock(mutex);
                paths.push_back(path);
            }
            std::string body;
            if (path.rfind("/api/v3/depth", 0) == 0) {
                const int n = ++depth_requests;
                body = R"({"lastUpdateId":)" + std::to_string(1000 * n) +
                       R"(,"bids":[["3000.00","1.5"]],"asks":[["3000.10","2.0"]]})";
            } else if (path.rfind("/api/v3/exchangeInfo", 0) == 0) {
                ++info_requests;
                body = R"({"symbols":[{"symbol":"ETHUSDT","filters":[]}]})";
            } else {
                body = "{}";
            }
            static_cast<void>(conn->write_all("HTTP/1.1 200 OK\r\nContent-Length: " +
                                              std::to_string(body.size()) + "\r\n\r\n" + body));
        }
    }
    net::TcpListener listener_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

std::string depth_msg(std::int64_t first, std::int64_t last) {
    return R"({"stream":"ethusdt@depth@100ms","data":{"e":"depthUpdate","E":1,"s":"ETHUSDT","U":)" +
           std::to_string(first) + R"(,"u":)" + std::to_string(last) +
           R"(,"b":[["3000.00","1.0"]],"a":[]}})";
}

std::string trade_msg(int id) {
    return R"({"stream":"ethusdt@aggTrade","data":{"e":"aggTrade","E":1,"s":"ETHUSDT","a":)" +
           std::to_string(id) + R"(,"p":"3000.5","q":"0.1","f":1,"l":1,"T":1,"m":true,"M":true}})";
}

CollectorConfig test_config(const FakeRest& rest, std::uint16_t ws_port) {
    CollectorConfig cc;
    cc.ws_base = "ws://127.0.0.1:" + std::to_string(ws_port);
    cc.rest_base = rest.base();
    cc.symbol = "ETHUSDT";
    cc.streams = {"aggTrade", "depth@100ms"};
    cc.connect_timeout = Duration::seconds(5);
    cc.stale_timeout = Duration::millis(400);
    cc.flush_interval = Duration::millis(10);
    cc.snapshot_interval = Duration::hours(1);
    cc.reconnect_backoff_min = Duration::millis(20);
    cc.reconnect_backoff_max = Duration::millis(50);
    cc.proxy = net::ProxyConfig{};
    return cc;
}

std::vector<RawRecord> read_all(const RawCapturePath& path) {
    std::vector<RawRecord> out;
    auto n = for_each_raw_record(path, Timestamp::epoch(), *Timestamp::parse_iso8601("2100-01-01"),
                                 [&](const RawRecord& r) {
                                     out.push_back(r);
                                     return true;
                                 });
    REQUIRE_MESSAGE(n.has_value(), n.error().message);
    return out;
}

}  // namespace

TEST_CASE("combined_stream_name and depth_update_ids") {
    CHECK(combined_stream_name(trade_msg(1)) == "ethusdt@aggTrade");
    CHECK(combined_stream_name(depth_msg(5, 9)) == "ethusdt@depth@100ms");
    CHECK(combined_stream_name(R"({"e":"aggTrade"})").empty());
    CHECK(combined_stream_name(R"({"stream":"unterminated)").empty());
    std::int64_t a = 0, b = 0;
    CHECK(depth_update_ids(depth_msg(5, 9), a, b));
    CHECK(a == 5);
    CHECK(b == 9);
    CHECK_FALSE(depth_update_ids(trade_msg(1), a, b));
}

TEST_CASE("Collector: stream URL") {
    TempDir tmp;
    RawCaptureWriter writer(RawCapturePath{tmp.path, "binance", "ETHUSDT"});
    SimClock clock;
    CollectorConfig cc;
    cc.streams = {"aggTrade", "depth@100ms"};
    Collector c(cc, writer, nullptr, clock, Logger{});
    CHECK(c.stream_url() == "wss://stream.binance.com:9443/stream?streams=ethusdt@aggTrade/ethusdt@depth@100ms");
}

TEST_CASE("Collector: captures exchange info, snapshot, messages; re-snapshots on gap; stops") {
    TempDir tmp;
    FakeRest rest;
    tradebot::test::TestWsServer ws;
    RawCapturePath path{tmp.path, "binance", "ETHUSDT"};
    SimClock clock(*Timestamp::parse_iso8601("2024-03-15T10:00:00Z"));
    auto sink = std::make_shared<MemorySink>();
    Logger log = Logger::make("test", sink, clock, LogLevel::trace);

    std::atomic<bool> stop{false};
    std::thread server([&] {
        REQUIRE(ws.accept_and_handshake());
        ws.send_text(trade_msg(1));
        ws.send_text(depth_msg(1001, 1005));  // brackets the first snapshot (1000)
        ws.send_text(depth_msg(1006, 1010));
        ws.send_text(depth_msg(1020, 1025));  // gap: expected 1011
        ws.send_text(trade_msg(2));
        // Wait for the client to have asked for the second snapshot, then stop.
        while (rest.depth_requests.load() < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true);
        // Client sends close on stop; read it.
        auto f = ws.read_frame();
        CHECK(f.ok);
        CHECK(f.op == net::WsOpcode::close);
    });

    CollectorStats stats;
    {
        RawCaptureWriter writer(path);
        Collector collector(test_config(rest, ws.port()), writer, nullptr, clock, log);
        auto r = collector.run(stop);
        REQUIRE_MESSAGE(r.has_value(), r.error().message);
        stats = collector.stats();
        REQUIRE(writer.close().has_value());
    }
    server.join();

    CHECK(rest.info_requests.load() == 1);
    CHECK(rest.depth_requests.load() == 2);
    CHECK(stats.messages == 5);
    CHECK(stats.connects == 1);
    CHECK(stats.reconnects == 0);
    CHECK(stats.depth_snapshots == 2);
    CHECK(stats.depth_gaps == 1);
    {
        std::lock_guard lock(rest.mutex);
        CHECK(rest.paths[1] == "/api/v3/depth?symbol=ETHUSDT&limit=1000");
    }

    auto records = read_all(path);
    REQUIRE(records.size() == 8);
    CHECK(records[0].stream == "exchange_info");
    CHECK(records[1].stream == "depth_snapshot");
    CHECK(records[1].payload.find("\"lastUpdateId\":1000") != std::string::npos);
    CHECK(records[2].stream == "ethusdt@aggTrade");
    CHECK(records[2].payload == trade_msg(1));
    CHECK(records[3].stream == "ethusdt@depth@100ms");
    CHECK(records[5].stream == "ethusdt@depth@100ms");
    CHECK(records[6].stream == "depth_snapshot");  // re-sync after the gap
    CHECK(records[6].payload.find("\"lastUpdateId\":2000") != std::string::npos);
    CHECK(records[7].stream == "ethusdt@aggTrade");
    CHECK(ws.request_head.find("GET /stream?streams=ethusdt@aggTrade/ethusdt@depth@100ms HTTP/1.1") == 0);
}

TEST_CASE("Collector: reconnects after server close and after a stale feed") {
    TempDir tmp;
    FakeRest rest;
    tradebot::test::TestWsServer ws;
    RawCapturePath path{tmp.path, "binance", "ETHUSDT"};
    SimClock clock(*Timestamp::parse_iso8601("2024-03-15T10:00:00Z"));
    Logger log;

    std::atomic<bool> stop{false};
    std::thread server([&] {
        // Connection 1: one message, then server closes the TCP socket.
        REQUIRE(ws.accept_and_handshake());
        ws.send_text(trade_msg(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ws.close_tcp();
        // Connection 2: one message, then silence -> client times out (stale).
        REQUIRE(ws.accept_and_handshake());
        ws.send_text(trade_msg(2));
        // Connection 3: message, then a WebSocket close frame.
        REQUIRE(ws.accept_and_handshake());
        ws.send_text(trade_msg(3));
        ws.send_frame(true, net::WsOpcode::close, std::string("\x03\xe9", 2));
        static_cast<void>(ws.read_frame());
        // Connection 4: deliver one more then stop.
        REQUIRE(ws.accept_and_handshake());
        ws.send_text(trade_msg(4));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stop.store(true);
        static_cast<void>(ws.read_frame());
    });

    CollectorStats stats;
    {
        RawCaptureWriter writer(path);
        auto cc = test_config(rest, ws.port());
        cc.streams = {"aggTrade"};  // no depth stream: no snapshots
        Collector collector(cc, writer, nullptr, clock, log);
        auto r = collector.run(stop);
        REQUIRE_MESSAGE(r.has_value(), r.error().message);
        stats = collector.stats();
    }
    server.join();

    CHECK(stats.connects == 4);
    CHECK(stats.reconnects == 3);
    CHECK(stats.stale_timeouts == 1);
    CHECK(stats.depth_snapshots == 0);
    CHECK(stats.messages == 4);
    CHECK(rest.depth_requests.load() == 0);
    CHECK(rest.info_requests.load() == 1);  // exchange info only once

    auto records = read_all(path);
    REQUIRE(records.size() == 5);
    CHECK(records[0].stream == "exchange_info");
    for (int i = 1; i <= 4; ++i) {
        CHECK(records[static_cast<std::size_t>(i)].payload == trade_msg(i));
    }
}

TEST_CASE("Collector: unreachable REST is retried, not fatal; unwritable disk is fatal") {
    TempDir tmp;
    tradebot::test::TestWsServer ws;
    SimClock clock;
    Logger log;
    std::atomic<bool> stop{false};

    // REST base points at a closed port: exchangeInfo fails, collector retries
    // with backoff until stopped.
    {
        auto closed = *net::TcpListener::bind_loopback();
        const auto port = closed.port();
        closed.close();
        RawCaptureWriter writer(RawCapturePath{tmp.path, "binance", "ETHUSDT"});
        CollectorConfig cc;
        cc.ws_base = "ws://127.0.0.1:" + std::to_string(ws.port());
        cc.rest_base = "http://127.0.0.1:" + std::to_string(port);
        cc.connect_timeout = Duration::seconds(1);
        cc.reconnect_backoff_min = Duration::millis(10);
        cc.reconnect_backoff_max = Duration::millis(20);
        cc.proxy = net::ProxyConfig{};
        Collector collector(cc, writer, nullptr, clock, log);
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            stop.store(true);
        });
        auto r = collector.run(stop);
        stopper.join();
        CHECK(r.has_value());
        CHECK(collector.stats().reconnects >= 2);
        CHECK(collector.stats().connects == 0);
    }
    // Unwritable capture root: the first write fails with io_error and run()
    // returns it instead of looping.
    {
        FakeRest rest;
        stop.store(false);
        RawCaptureWriter writer(RawCapturePath{"/proc/tradebot-cannot-write", "binance", "ETHUSDT"});
        CollectorConfig cc;
        cc.ws_base = "ws://127.0.0.1:" + std::to_string(ws.port());
        cc.rest_base = rest.base();
        cc.connect_timeout = Duration::seconds(1);
        cc.proxy = net::ProxyConfig{};
        Collector collector(cc, writer, nullptr, clock, log);
        auto r = collector.run(stop);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ErrorCode::io_error);
    }
}
