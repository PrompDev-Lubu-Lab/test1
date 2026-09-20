#include "tradebot/net/http.hpp"
#include "tradebot/net/tcp.hpp"

#include <doctest/doctest.h>

#include <thread>

using namespace tradebot;
using namespace tradebot::net;

namespace {

// Serves one connection: reads the request head, then writes `response`.
struct OneShotServer {
    TcpListener listener;
    std::string received;
    std::thread thread;

    explicit OneShotServer(std::string response) {
        listener = *TcpListener::bind_loopback();
        thread = std::thread([this, response = std::move(response)] {
            auto conn = listener.accept(Duration::seconds(5));
            REQUIRE(conn.has_value());
            std::byte buf[4096];
            while (received.find("\r\n\r\n") == std::string::npos) {
                auto n = conn->read_some(buf);
                REQUIRE(n.has_value());
                REQUIRE(*n > 0);
                received.append(reinterpret_cast<const char*>(buf), *n);
            }
            REQUIRE(conn->write_all(response).has_value());
        });
    }
    ~OneShotServer() { thread.join(); }
    [[nodiscard]] std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(listener.port()) + path;
    }
};

}  // namespace

TEST_CASE("HttpClient: GET with Content-Length") {
    OneShotServer server(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 13\r\n\r\n"
        "{\"pong\":true}");
    auto client = HttpClient::create(nullptr, {.timeout = Duration::seconds(5),
                                               .proxy = ProxyConfig{}});
    REQUIRE(client.has_value());
    auto resp = client->get(server.url("/api/v3/ping?x=1"), {{"X-Test", "yes"}});
    REQUIRE_MESSAGE(resp.has_value(), resp.error().message);
    CHECK(resp->status == 200);
    CHECK(resp->ok());
    CHECK(resp->body == "{\"pong\":true}");
    REQUIRE(resp->header("Content-Type") != nullptr);
    CHECK(*resp->header("content-type") == "application/json");
    server.thread.join();
    server.thread = std::thread([] {});
    CHECK(server.received.find("GET /api/v3/ping?x=1 HTTP/1.1\r\n") == 0);
    CHECK(server.received.find("Host: 127.0.0.1:" + std::to_string(server.listener.port()) +
                               "\r\n") != std::string::npos);
    CHECK(server.received.find("X-Test: yes\r\n") != std::string::npos);
    CHECK(server.received.find("User-Agent: tradebot/0.1\r\n") != std::string::npos);
    CHECK(server.received.find("Connection: close\r\n") != std::string::npos);
}

TEST_CASE("HttpClient: chunked body streamed to sink") {
    OneShotServer server(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n"
        "7;ext=1\r\n, world\r\n"
        "0\r\nX-Trailer: t\r\n\r\n");
    auto client = *HttpClient::create(nullptr, {.timeout = Duration::seconds(5),
                                                .proxy = ProxyConfig{}});
    std::string got;
    int calls = 0;
    auto resp = client.get_streaming(server.url("/"), [&](std::span<const std::byte> b) {
        got.append(reinterpret_cast<const char*>(b.data()), b.size());
        ++calls;
        return true;
    });
    REQUIRE_MESSAGE(resp.has_value(), resp.error().message);
    CHECK(got == "hello, world");
    CHECK(calls >= 2);
    CHECK(resp->body.empty());
}

TEST_CASE("HttpClient: body to EOF when no length given") {
    OneShotServer server("HTTP/1.1 500 Internal Server Error\r\n\r\nboom");
    auto client = *HttpClient::create(nullptr, {.timeout = Duration::seconds(5),
                                                .proxy = ProxyConfig{}});
    auto resp = client.get(server.url("/"));
    REQUIRE(resp.has_value());
    CHECK(resp->status == 500);
    CHECK_FALSE(resp->ok());
    CHECK(resp->body == "boom");
}

TEST_CASE("HttpClient: 204 has no body") {
    OneShotServer server("HTTP/1.1 204 No Content\r\n\r\n");
    auto client = *HttpClient::create(nullptr, {.timeout = Duration::seconds(5),
                                                .proxy = ProxyConfig{}});
    auto resp = client.get(server.url("/"));
    REQUIRE(resp.has_value());
    CHECK(resp->status == 204);
    CHECK(resp->body.empty());
}

TEST_CASE("HttpClient: malformed responses are protocol errors") {
    OneShotServer server("garbage\r\n\r\n");
    auto client = *HttpClient::create(nullptr, {.timeout = Duration::seconds(5),
                                                .proxy = ProxyConfig{}});
    auto resp = client.get(server.url("/"));
    REQUIRE_FALSE(resp.has_value());
    CHECK(resp.error().code == ErrorCode::protocol_error);
}

TEST_CASE("HttpClient: sink abort stops the download") {
    OneShotServer server("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789");
    auto client = *HttpClient::create(nullptr, {.timeout = Duration::seconds(5),
                                                .proxy = ProxyConfig{}});
    auto resp = client.get_streaming(server.url("/"), [](std::span<const std::byte>) {
        return false;
    });
    REQUIRE_FALSE(resp.has_value());
    CHECK(resp.error().code == ErrorCode::invalid_state);
}

TEST_CASE("HttpClient: https without TLS context is rejected up front") {
    auto client = *HttpClient::create(nullptr, {.timeout = Duration::seconds(1),
                                                .proxy = ProxyConfig{}});
    // Never connects: the URL is loopback on a closed port, but the TLS
    // check only happens after connect, so use the listener trick.
    auto listener = *TcpListener::bind_loopback();
    std::thread t([&] { static_cast<void>(listener.accept(Duration::seconds(2))); });
    auto resp = client.get("https://127.0.0.1:" + std::to_string(listener.port()) + "/");
    REQUIRE_FALSE(resp.has_value());
    CHECK(resp.error().code == ErrorCode::invalid_argument);
    t.join();
}

TEST_CASE("HttpClient: CONNECT proxy tunnel") {
    // The "proxy" accepts CONNECT and then behaves as the origin server.
    auto listener = *TcpListener::bind_loopback();
    std::string received;
    std::thread proxy([&] {
        auto conn = *listener.accept(Duration::seconds(5));
        std::byte buf[4096];
        while (received.find("\r\n\r\n") == std::string::npos) {
            auto n = conn.read_some(buf);
            REQUIRE(n.has_value());
            received.append(reinterpret_cast<const char*>(buf), *n);
        }
        REQUIRE(conn.write_all("HTTP/1.1 200 Connection established\r\n\r\n").has_value());
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
            auto n = conn.read_some(buf);
            REQUIRE(n.has_value());
            req.append(reinterpret_cast<const char*>(buf), *n);
        }
        REQUIRE(req.find("GET /via-proxy HTTP/1.1\r\nHost: origin.example:8080\r\n") == 0);
        REQUIRE(conn.write_all("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok").has_value());
    });
    auto client = *HttpClient::create(
        nullptr, {.timeout = Duration::seconds(5),
                  .proxy = ProxyConfig{"127.0.0.1", listener.port()}});
    auto resp = client.get("http://origin.example:8080/via-proxy");
    proxy.join();
    REQUIRE_MESSAGE(resp.has_value(), resp.error().message);
    CHECK(resp->body == "ok");
    CHECK(received.find("CONNECT origin.example:8080 HTTP/1.1\r\n") == 0);
}

TEST_CASE("proxy_from_env parses HTTPS_PROXY") {
    setenv("HTTPS_PROXY", "http://127.0.0.1:3128", 1);
    auto p = proxy_from_env();
    REQUIRE(p.has_value());
    CHECK(p->host == "127.0.0.1");
    CHECK(p->port == 3128);
    setenv("HTTPS_PROXY", "", 1);
    unsetenv("https_proxy");
    CHECK_FALSE(proxy_from_env().has_value());
    unsetenv("HTTPS_PROXY");
}
