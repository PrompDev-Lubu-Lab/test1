#pragma once

// RFC 6455 WebSocket client over any Stream. Blocking, single-threaded:
// the collector owns one connection per feed and drives it from its own
// loop. Control frames (ping/pong/close) are handled inside receive() so
// callers only see data messages and a final close.

#include "tradebot/core/error.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/net/http.hpp"
#include "tradebot/net/stream.hpp"
#include "tradebot/net/tls.hpp"
#include "tradebot/net/url.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tradebot::net {

enum class WsOpcode : std::uint8_t {
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA,
};

struct WsMessage {
    WsOpcode opcode = WsOpcode::text;  // text, binary, or close
    std::string payload;
    std::uint16_t close_code = 0;  // set when opcode == close
};

struct WebSocketOptions {
    Duration connect_timeout = Duration::seconds(10);
    Duration read_timeout = Duration::seconds(30);  // surfaces as ErrorCode::timeout
    std::size_t max_message_size = 16 * 1024 * 1024;
    std::optional<ProxyConfig> proxy;  // nullopt = read from environment
    Headers extra_headers;
};

class WebSocketClient {
public:
    using Options = WebSocketOptions;

    WebSocketClient() = default;
    ~WebSocketClient();
    WebSocketClient(WebSocketClient&&) noexcept = default;
    WebSocketClient& operator=(WebSocketClient&&) noexcept = default;

    // Opens the connection and completes the upgrade handshake.
    [[nodiscard]] static Result<WebSocketClient> connect(const std::string& url,
                                                         std::shared_ptr<TlsContext> tls,
                                                         Options opts = Options{});

    // Performs the handshake over an already-open stream (used by tests and
    // by callers that manage their own transport).
    [[nodiscard]] static Result<WebSocketClient> upgrade(std::unique_ptr<Stream> stream,
                                                         const Url& url, Options opts = Options{});

    [[nodiscard]] Result<void> send_text(std::string_view text);
    [[nodiscard]] Result<void> send_binary(std::span<const std::byte> data);
    [[nodiscard]] Result<void> send_ping(std::string_view payload = {});
    // Sends a close frame; the peer's close is then read by receive().
    [[nodiscard]] Result<void> send_close(std::uint16_t code = 1000, std::string_view reason = {});

    // Blocks for the next data message. Pings are answered transparently. A
    // close frame from the peer is acknowledged and returned with
    // opcode == close, after which the connection is closed.
    [[nodiscard]] Result<WsMessage> receive();

    [[nodiscard]] bool is_open() const noexcept { return stream_ && stream_->is_open(); }
    void close() noexcept;

private:
    struct Frame {
        bool fin = false;
        WsOpcode opcode = WsOpcode::continuation;
        std::string payload;
    };

    Result<void> send_frame(WsOpcode opcode, std::span<const std::byte> payload);
    Result<Frame> read_frame();
    Result<void> read_exact(std::span<std::byte> out);

    std::unique_ptr<Stream> stream_;
    Options opts_;
    std::string buffer_;  // unread bytes from the stream
    bool close_sent_ = false;
};

// Exposed for tests: Sec-WebSocket-Accept derivation per RFC 6455 §4.2.2.
[[nodiscard]] std::string websocket_accept_key(std::string_view client_key);

}  // namespace tradebot::net
