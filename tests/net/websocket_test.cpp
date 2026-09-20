#include "tradebot/net/tcp.hpp"
#include "tradebot/net/websocket.hpp"

#include <doctest/doctest.h>

#include <thread>

using namespace tradebot;
using namespace tradebot::net;

namespace {

// Minimal server-side WebSocket implementation for tests: performs the
// upgrade handshake, sends unmasked frames, reads masked client frames.
class TestWsServer {
public:
    TestWsServer() : listener_(*TcpListener::bind_loopback()) {}

    [[nodiscard]] std::uint16_t port() const { return listener_.port(); }

    void accept_and_handshake() {
        conn_ = *listener_.accept(Duration::seconds(5));
        std::string head;
        std::byte buf[4096];
        while (head.find("\r\n\r\n") == std::string::npos) {
            auto n = conn_.read_some(buf);
            REQUIRE(n.has_value());
            head.append(reinterpret_cast<const char*>(buf), *n);
        }
        request_head = head;
        const std::size_t k = head.find("Sec-WebSocket-Key: ");
        REQUIRE(k != std::string::npos);
        const std::string key = head.substr(k + 19, head.find("\r\n", k) - k - 19);
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                           "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                           websocket_accept_key(key) + "\r\n\r\n";
        REQUIRE(conn_.write_all(resp).has_value());
    }

    void send_raw(std::string bytes) { REQUIRE(conn_.write_all(bytes).has_value()); }

    void send_frame(bool fin, WsOpcode op, std::string_view payload) {
        std::string f;
        f.push_back(static_cast<char>((fin ? 0x80 : 0) | static_cast<int>(op)));
        if (payload.size() < 126) {
            f.push_back(static_cast<char>(payload.size()));
        } else if (payload.size() <= 0xFFFF) {
            f.push_back(126);
            f.push_back(static_cast<char>(payload.size() >> 8));
            f.push_back(static_cast<char>(payload.size() & 0xFF));
        } else {
            f.push_back(127);
            for (int s = 56; s >= 0; s -= 8) {
                f.push_back(static_cast<char>((payload.size() >> s) & 0xFF));
            }
        }
        f += payload;
        send_raw(f);
    }

    struct Frame {
        bool fin;
        WsOpcode op;
        std::string payload;
    };

    Frame read_frame() {
        std::byte h[2];
        read_exact(h);
        Frame f{};
        f.fin = (static_cast<std::uint8_t>(h[0]) & 0x80) != 0;
        f.op = static_cast<WsOpcode>(static_cast<std::uint8_t>(h[0]) & 0x0F);
        const bool masked = (static_cast<std::uint8_t>(h[1]) & 0x80) != 0;
        REQUIRE(masked);  // clients must mask
        std::uint64_t len = static_cast<std::uint8_t>(h[1]) & 0x7F;
        if (len == 126) {
            std::byte e[2];
            read_exact(e);
            len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(e[0])) << 8) |
                  static_cast<std::uint8_t>(e[1]);
        } else if (len == 127) {
            std::byte e[8];
            read_exact(e);
            len = 0;
            for (std::byte b : e) {
                len = (len << 8) | static_cast<std::uint8_t>(b);
            }
        }
        std::byte mask[4];
        read_exact(mask);
        f.payload.resize(static_cast<std::size_t>(len));
        read_exact(std::as_writable_bytes(std::span(f.payload.data(), f.payload.size())));
        for (std::size_t i = 0; i < f.payload.size(); ++i) {
            f.payload[i] = static_cast<char>(static_cast<std::uint8_t>(f.payload[i]) ^
                                             static_cast<std::uint8_t>(mask[i % 4]));
        }
        return f;
    }

    void close_tcp() { conn_.close(); }

    std::string request_head;

private:
    void read_exact(std::span<std::byte> out) {
        std::size_t off = 0;
        while (off < out.size()) {
            auto n = conn_.read_some(out.subspan(off));
            REQUIRE(n.has_value());
            REQUIRE(*n > 0);
            off += *n;
        }
    }

    TcpListener listener_;
    TcpSocket conn_;
};

WebSocketClient::Options test_opts() {
    WebSocketClient::Options o;
    o.connect_timeout = Duration::seconds(5);
    o.read_timeout = Duration::seconds(5);
    o.proxy = ProxyConfig{};
    return o;
}

}  // namespace

TEST_CASE("websocket_accept_key matches RFC 6455 example") {
    CHECK(websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("WebSocketClient: handshake, echo, fragmentation, ping, close") {
    TestWsServer server;
    std::thread t([&] {
        server.accept_and_handshake();
        // Echo one client text message.
        auto f = server.read_frame();
        CHECK(f.op == WsOpcode::text);
        CHECK(f.fin);
        server.send_frame(true, WsOpcode::text, "echo:" + f.payload);
        // Fragmented message with an interleaved ping.
        server.send_frame(false, WsOpcode::text, "frag");
        server.send_frame(true, WsOpcode::ping, "p1");
        server.send_frame(false, WsOpcode::continuation, "ment");
        server.send_frame(true, WsOpcode::continuation, "ed");
        // Expect the pong.
        auto pong = server.read_frame();
        CHECK(pong.op == WsOpcode::pong);
        CHECK(pong.payload == "p1");
        // Large binary frame (needs 16-bit length).
        std::string big(70000, 'x');
        server.send_frame(true, WsOpcode::binary, big);
        // Client-initiated close.
        auto c = server.read_frame();
        CHECK(c.op == WsOpcode::close);
        CHECK(c.payload.size() >= 2);
        server.send_frame(true, WsOpcode::close, c.payload.substr(0, 2));
    });

    auto opts = test_opts();
    opts.extra_headers = {{"X-Client", "tradebot"}};
    auto ws = WebSocketClient::connect("ws://127.0.0.1:" + std::to_string(server.port()) +
                                           "/stream?streams=ethusdt@aggTrade",
                                       nullptr, opts);
    REQUIRE_MESSAGE(ws.has_value(), ws.error().message);
    CHECK(ws->is_open());

    REQUIRE(ws->send_text("hello").has_value());
    auto m1 = ws->receive();
    REQUIRE_MESSAGE(m1.has_value(), m1.error().message);
    CHECK(m1->opcode == WsOpcode::text);
    CHECK(m1->payload == "echo:hello");

    auto m2 = ws->receive();
    REQUIRE(m2.has_value());
    CHECK(m2->payload == "fragmented");

    auto m3 = ws->receive();
    REQUIRE(m3.has_value());
    CHECK(m3->opcode == WsOpcode::binary);
    CHECK(m3->payload.size() == 70000);

    REQUIRE(ws->send_close(1000, "bye").has_value());
    auto m4 = ws->receive();
    REQUIRE(m4.has_value());
    CHECK(m4->opcode == WsOpcode::close);
    CHECK(m4->close_code == 1000);
    CHECK_FALSE(ws->is_open());
    t.join();
    CHECK(server.request_head.find("GET /stream?streams=ethusdt@aggTrade HTTP/1.1\r\n") == 0);
    CHECK(server.request_head.find("X-Client: tradebot\r\n") != std::string::npos);
    CHECK(server.request_head.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
}

TEST_CASE("WebSocketClient: server-initiated close and read timeout") {
    TestWsServer server;
    std::thread t([&] {
        server.accept_and_handshake();
        // Say nothing for a while so the client times out, then close.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        server.send_frame(true, WsOpcode::close, std::string("\x03\xe9going away", 12));
        auto c = server.read_frame();
        CHECK(c.op == WsOpcode::close);
    });
    auto opts = test_opts();
    opts.read_timeout = Duration::millis(100);
    auto ws = WebSocketClient::connect("ws://127.0.0.1:" + std::to_string(server.port()) + "/",
                                       nullptr, opts);
    REQUIRE(ws.has_value());
    auto m = ws->receive();
    REQUIRE_FALSE(m.has_value());
    CHECK(m.error().code == ErrorCode::timeout);
    CHECK(ws->is_open());  // a timeout is not a disconnect
    m = ws->receive();
    if (!m.has_value()) {
        // Still within the 300ms silence; wait once more.
        CHECK(m.error().code == ErrorCode::timeout);
        m = ws->receive();
    }
    if (!m.has_value()) {
        CHECK(m.error().code == ErrorCode::timeout);
        m = ws->receive();
    }
    REQUIRE(m.has_value());
    CHECK(m->opcode == WsOpcode::close);
    CHECK(m->close_code == 1001);
    CHECK(m->payload == "going away");
    t.join();
}

TEST_CASE("WebSocketClient: refused upgrade and bad accept key") {
    {
        auto listener = *TcpListener::bind_loopback();
        std::thread t([&] {
            auto conn = *listener.accept(Duration::seconds(5));
            std::byte buf[4096];
            std::string head;
            while (head.find("\r\n\r\n") == std::string::npos) {
                auto n = conn.read_some(buf);
                REQUIRE(n.has_value());
                head.append(reinterpret_cast<const char*>(buf), *n);
            }
            REQUIRE(conn.write_all("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n").has_value());
        });
        auto ws = WebSocketClient::connect("ws://127.0.0.1:" + std::to_string(listener.port()) + "/",
                                           nullptr, test_opts());
        REQUIRE_FALSE(ws.has_value());
        CHECK(ws.error().code == ErrorCode::protocol_error);
        t.join();
    }
    {
        auto listener = *TcpListener::bind_loopback();
        std::thread t([&] {
            auto conn = *listener.accept(Duration::seconds(5));
            std::byte buf[4096];
            std::string head;
            while (head.find("\r\n\r\n") == std::string::npos) {
                auto n = conn.read_some(buf);
                REQUIRE(n.has_value());
                head.append(reinterpret_cast<const char*>(buf), *n);
            }
            REQUIRE(conn.write_all("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                   "Sec-WebSocket-Accept: bogus\r\n\r\n")
                        .has_value());
        });
        auto ws = WebSocketClient::connect("ws://127.0.0.1:" + std::to_string(listener.port()) + "/",
                                           nullptr, test_opts());
        REQUIRE_FALSE(ws.has_value());
        CHECK(ws.error().message.find("Accept mismatch") != std::string::npos);
        t.join();
    }
}

TEST_CASE("WebSocketClient: peer TCP close and protocol violations") {
    {
        TestWsServer server;
        std::thread t([&] {
            server.accept_and_handshake();
            server.close_tcp();
        });
        auto ws = *WebSocketClient::connect("ws://127.0.0.1:" + std::to_string(server.port()) + "/",
                                            nullptr, test_opts());
        auto m = ws.receive();
        REQUIRE_FALSE(m.has_value());
        CHECK(m.error().code == ErrorCode::connection_closed);
        t.join();
    }
    {
        TestWsServer server;
        std::thread t([&] {
            server.accept_and_handshake();
            server.send_frame(true, WsOpcode::continuation, "orphan");
            auto c = server.read_frame();
            CHECK(c.op == WsOpcode::close);
        });
        auto ws = *WebSocketClient::connect("ws://127.0.0.1:" + std::to_string(server.port()) + "/",
                                            nullptr, test_opts());
        auto m = ws.receive();
        REQUIRE_FALSE(m.has_value());
        CHECK(m.error().code == ErrorCode::protocol_error);
        t.join();
    }
    {
        TestWsServer server;
        std::thread t([&] {
            server.accept_and_handshake();
            server.send_frame(true, WsOpcode::text, std::string(2000, 'y'));
            auto c = server.read_frame();
            CHECK(c.op == WsOpcode::close);
        });
        auto opts = test_opts();
        opts.max_message_size = 1000;
        auto ws = *WebSocketClient::connect("ws://127.0.0.1:" + std::to_string(server.port()) + "/",
                                            nullptr, opts);
        auto m = ws.receive();
        REQUIRE_FALSE(m.has_value());
        CHECK(m.error().code == ErrorCode::protocol_error);
        t.join();
    }
}
