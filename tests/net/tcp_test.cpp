#include "tradebot/net/tcp.hpp"

#include <doctest/doctest.h>

#include <thread>

using namespace tradebot;
using namespace tradebot::net;

TEST_CASE("TcpSocket: loopback round trip and timeout") {
    auto listener = TcpListener::bind_loopback();
    REQUIRE(listener.has_value());
    const auto port = listener->port();
    CHECK(port != 0);

    std::thread server([&] {
        auto conn = listener->accept(Duration::seconds(5));
        REQUIRE(conn.has_value());
        std::byte buf[64];
        auto n = conn->read_some(buf);
        REQUIRE(n.has_value());
        REQUIRE(conn->write_all(std::span<const std::byte>(buf, *n)).has_value());
        // Then go quiet so the client read times out, then close.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    });

    auto client = TcpSocket::connect("127.0.0.1", port, Duration::seconds(5));
    REQUIRE(client.has_value());
    REQUIRE(client->write_all("hello").has_value());
    std::byte buf[64];
    auto n = client->read_some(buf);
    REQUIRE(n.has_value());
    CHECK(std::string_view(reinterpret_cast<const char*>(buf), *n) == "hello");

    REQUIRE(client->set_timeouts(Duration::millis(50), Duration::millis(50)).has_value());
    auto t = client->read_some(buf);
    REQUIRE_FALSE(t.has_value());
    CHECK(t.error().code == ErrorCode::timeout);

    server.join();
    REQUIRE(client->set_timeouts(Duration::seconds(2), Duration::seconds(2)).has_value());
    auto eof = client->read_some(buf);
    REQUIRE(eof.has_value());
    CHECK(*eof == 0);
    client->close();
    CHECK_FALSE(client->is_open());
    auto after = client->read_some(buf);
    CHECK(after.error().code == ErrorCode::connection_closed);
}

TEST_CASE("TcpSocket: connection refused") {
    auto listener = TcpListener::bind_loopback();
    REQUIRE(listener.has_value());
    const auto port = listener->port();
    listener->close();
    auto client = TcpSocket::connect("127.0.0.1", port, Duration::seconds(2));
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().code == ErrorCode::network_error);
}

TEST_CASE("TcpSocket: bad hostname") {
    auto client = TcpSocket::connect("no.such.host.invalid", 80, Duration::seconds(2));
    REQUIRE_FALSE(client.has_value());
    CHECK(client.error().code == ErrorCode::network_error);
}
