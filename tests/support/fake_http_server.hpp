#pragma once

// Tiny in-process HTTP/1.1 server for tests: one request per connection,
// routes matched by exact path, everything else 404. Runs on its own
// thread until destroyed.

#include "tradebot/core/time.hpp"
#include "tradebot/net/tcp.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tradebot::test {

class FakeHttpServer {
public:
    struct Response {
        int status = 200;
        std::string body;
    };

    FakeHttpServer() : listener_(*net::TcpListener::bind_loopback()) {
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeHttpServer() {
        stop_.store(true);
        thread_.join();
    }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(listener_.port());
    }

    void route(std::string path, Response response) {
        std::lock_guard lock(mutex_);
        routes_[std::move(path)] = std::move(response);
    }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    [[nodiscard]] std::size_t request_count(const std::string& path) const {
        std::lock_guard lock(mutex_);
        std::size_t n = 0;
        for (const auto& r : requests_) {
            n += r == path;
        }
        return n;
    }

private:
    void serve() {
        while (!stop_.load()) {
            auto conn = listener_.accept(Duration::millis(50));
            if (!conn) {
                continue;
            }
            std::string req;
            std::byte buf[8192];
            while (req.find("\r\n\r\n") == std::string::npos) {
                auto n = conn->read_some(buf);
                if (!n || *n == 0) {
                    break;
                }
                req.append(reinterpret_cast<const char*>(buf), *n);
            }
            const std::size_t sp1 = req.find(' ');
            const std::size_t sp2 = req.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                continue;
            }
            const std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
            Response resp{404, "not found"};
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(path);
                if (auto it = routes_.find(path); it != routes_.end()) {
                    resp = it->second;
                }
            }
            static_cast<void>(conn->write_all("HTTP/1.1 " + std::to_string(resp.status) +
                                              " X\r\nContent-Length: " +
                                              std::to_string(resp.body.size()) + "\r\n\r\n" +
                                              resp.body));
        }
    }

    net::TcpListener listener_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::map<std::string, Response> routes_;
    std::vector<std::string> requests_;
};

}  // namespace tradebot::test
