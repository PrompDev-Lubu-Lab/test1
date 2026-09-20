#include "tradebot/net/tcp.hpp"
#include "tradebot/net/websocket.hpp"

#include "support/ws_test_server.hpp"

#include <doctest/doctest.h>

#include <thread>

using namespace tradebot;
using namespace tradebot::net;

namespace {

using tradebot::test::TestWsServer;

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
        CHECK(f.ok);
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
