#pragma once

// Minimal server-side WebSocket implementation for tests: performs the
// upgrade handshake, sends unmasked frames, reads masked client frames.

#include "tradebot/net/tcp.hpp"
#include "tradebot/net/websocket.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tradebot::test {

class TestWsServer {
public:
    TestWsServer() : listener_(*net::TcpListener::bind_loopback()) {}

    [[nodiscard]] std::uint16_t port() const { return listener_.port(); }

    // Accepts one connection and completes the upgrade. Returns false on
    // accept timeout (used by tests that expect no further connections).
    bool accept_and_handshake(Duration timeout = Duration::seconds(5)) {
        auto conn = listener_.accept(timeout);
        if (!conn) {
            return false;
        }
        conn_ = std::move(*conn);
        std::string head;
        std::byte buf[4096];
        while (head.find("\r\n\r\n") == std::string::npos) {
            auto n = conn_.read_some(buf);
            if (!n || *n == 0) {
                return false;
            }
            head.append(reinterpret_cast<const char*>(buf), *n);
        }
        request_head = head;
        const std::size_t k = head.find("Sec-WebSocket-Key: ");
        if (k == std::string::npos) {
            return false;
        }
        const std::string key = head.substr(k + 19, head.find("\r\n", k) - k - 19);
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                           "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                           net::websocket_accept_key(key) + "\r\n\r\n";
        return conn_.write_all(resp).has_value();
    }

    void send_raw(std::string bytes) { CHECK(conn_.write_all(bytes).has_value()); }

    void send_frame(bool fin, net::WsOpcode op, std::string_view payload) {
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

    void send_text(std::string_view payload) { send_frame(true, net::WsOpcode::text, payload); }

    struct Frame {
        bool ok = false;  // false when the peer closed or an error occurred
        bool fin = false;
        net::WsOpcode op = net::WsOpcode::continuation;
        std::string payload;
    };

    // Never uses fatal assertions: this runs on a test's server thread, where
    // a REQUIRE would terminate the process instead of failing the test.
    Frame read_frame() {
        Frame f{};
        std::byte h[2];
        if (!read_exact(h)) {
            return f;
        }
        f.fin = (static_cast<std::uint8_t>(h[0]) & 0x80) != 0;
        f.op = static_cast<net::WsOpcode>(static_cast<std::uint8_t>(h[0]) & 0x0F);
        const bool masked = (static_cast<std::uint8_t>(h[1]) & 0x80) != 0;
        CHECK(masked);  // clients must mask
        std::uint64_t len = static_cast<std::uint8_t>(h[1]) & 0x7F;
        if (len == 126) {
            std::byte e[2];
            if (!read_exact(e)) {
                return f;
            }
            len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(e[0])) << 8) |
                  static_cast<std::uint8_t>(e[1]);
        } else if (len == 127) {
            std::byte e[8];
            if (!read_exact(e)) {
                return f;
            }
            len = 0;
            for (std::byte b : e) {
                len = (len << 8) | static_cast<std::uint8_t>(b);
            }
        }
        std::byte mask[4];
        if (!read_exact(mask)) {
            return f;
        }
        f.payload.resize(static_cast<std::size_t>(len));
        if (!read_exact(std::as_writable_bytes(std::span(f.payload.data(), f.payload.size())))) {
            return f;
        }
        for (std::size_t i = 0; i < f.payload.size(); ++i) {
            f.payload[i] = static_cast<char>(static_cast<std::uint8_t>(f.payload[i]) ^
                                             static_cast<std::uint8_t>(mask[i % 4]));
        }
        f.ok = true;
        return f;
    }

    void close_tcp() { conn_.close(); }

    std::string request_head;

private:
    bool read_exact(std::span<std::byte> out) {
        std::size_t off = 0;
        while (off < out.size()) {
            auto n = conn_.read_some(out.subspan(off));
            if (!n || *n == 0) {
                return false;
            }
            off += *n;
        }
        return true;
    }

    net::TcpListener listener_;
    net::TcpSocket conn_;
};

}  // namespace tradebot::test
