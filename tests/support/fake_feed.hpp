#pragma once

// Shared fixtures for runtime tests: a temp directory, a fake Binance
// public REST (exchangeInfo + depth snapshot) for the market-data
// collector, and an aggTrade message builder.

#include "tradebot/core/time.hpp"
#include "tradebot/net/tcp.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

namespace tradebot::test {

namespace fs = std::filesystem;

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

inline std::string agg_trade(std::int64_t id, const char* price, std::int64_t ms) {
    return R"({"stream":"ethusdt@aggTrade","data":{"e":"aggTrade","E":)" + std::to_string(ms) +
           R"(,"s":"ETHUSDT","a":)" + std::to_string(id) + R"(,"p":")" + price +
           R"(","q":"0.5","f":1,"l":1,"T":)" + std::to_string(ms) + R"(,"m":false,"M":true}})";
}

inline std::int64_t wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace tradebot::test
