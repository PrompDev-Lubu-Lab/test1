#pragma once

// A fake signed Binance REST server for tests: verifies the API key header
// and the HMAC signature, records requests, and answers from a scripted
// table of responses keyed by "METHOD path".

#include "tradebot/gateway/signing.hpp"
#include "tradebot/net/tcp.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tradebot::test {

class FakeBinanceRest {
public:
    struct Request {
        std::string method;
        std::string path;
        std::string query;  // without signature
        std::map<std::string, std::string> params;
        bool key_ok = false;
        bool signature_ok = false;
    };
    struct Reply {
        int status = 200;
        std::string body;
    };
    using Responder = std::function<Reply(const Request&)>;

    explicit FakeBinanceRest(std::string api_key = "key", std::string secret = "secret")
        : api_key_(std::move(api_key)), secret_(std::move(secret)), listener_(*net::TcpListener::bind_loopback()) {
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeBinanceRest() {
        stop_.store(true);
        thread_.join();
    }

    [[nodiscard]] std::string base_url() const { return "http://127.0.0.1:" + std::to_string(listener_.port()); }

    // Scripted replies: "METHOD /path" -> responder. Unknown -> 404.
    void on(const std::string& key, Responder r) {
        std::lock_guard lock(mutex_);
        responders_[key] = std::move(r);
    }
    void on(const std::string& key, Reply reply) {
        on(key, [reply](const Request&) { return reply; });
    }
    [[nodiscard]] std::vector<Request> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    static std::map<std::string, std::string> parse_params(const std::string& q) {
        std::map<std::string, std::string> out;
        std::size_t start = 0;
        while (start < q.size()) {
            std::size_t amp = q.find('&', start);
            if (amp == std::string::npos) amp = q.size();
            const std::string kv = q.substr(start, amp - start);
            const std::size_t eq = kv.find('=');
            if (eq != std::string::npos) out[kv.substr(0, eq)] = kv.substr(eq + 1);
            start = amp + 1;
        }
        return out;
    }

    void serve() {
        while (!stop_.load()) {
            auto conn = listener_.accept(Duration::millis(50));
            if (!conn) continue;
            std::string raw;
            std::byte buf[8192];
            while (raw.find("\r\n\r\n") == std::string::npos) {
                auto n = conn->read_some(buf);
                if (!n || *n == 0) break;
                raw.append(reinterpret_cast<const char*>(buf), *n);
            }
            Request req;
            const std::size_t sp1 = raw.find(' ');
            const std::size_t sp2 = raw.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) continue;
            req.method = raw.substr(0, sp1);
            std::string target = raw.substr(sp1 + 1, sp2 - sp1 - 1);
            const std::size_t qm = target.find('?');
            req.path = target.substr(0, qm);
            std::string query = qm == std::string::npos ? "" : target.substr(qm + 1);
            req.key_ok = raw.find("X-MBX-APIKEY: " + api_key_ + "\r\n") != std::string::npos;
            const std::size_t sig = query.find("&signature=");
            if (sig != std::string::npos) {
                req.query = query.substr(0, sig);
                req.signature_ok = query.substr(sig + 11) == gateway::hmac_sha256_hex(secret_, req.query);
            } else {
                req.query = query;
            }
            req.params = parse_params(req.query);
            Reply reply{404, R"({"code":-1000,"msg":"no route"})"};
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(req);
                auto it = responders_.find(req.method + " " + req.path);
                if (it != responders_.end()) reply = it->second(req);
            }
            static_cast<void>(conn->write_all("HTTP/1.1 " + std::to_string(reply.status) + " X\r\nContent-Length: " +
                                              std::to_string(reply.body.size()) + "\r\n\r\n" + reply.body));
        }
    }

    std::string api_key_;
    std::string secret_;
    net::TcpListener listener_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::map<std::string, Responder> responders_;
    std::vector<Request> requests_;
};

}  // namespace tradebot::test
