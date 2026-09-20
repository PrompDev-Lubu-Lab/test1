#include "tradebot/net/websocket.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstring>

namespace tradebot::net {

namespace {

std::string base64_encode(std::span<const unsigned char> in) {
    std::string out;
    out.resize(4 * ((in.size() + 2) / 3) + 1);
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), in.data(),
                                  static_cast<int>(in.size()));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::string random_key() {
    unsigned char bytes[16];
    RAND_bytes(bytes, sizeof bytes);
    return base64_encode(bytes);
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

constexpr std::uint16_t kCloseNormal = 1000;
constexpr std::uint16_t kCloseProtocolError = 1002;
constexpr std::uint16_t kCloseTooBig = 1009;

}  // namespace

std::string websocket_accept_key(std::string_view client_key) {
    static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string input(client_key);
    input += kGuid;
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
    return base64_encode(digest);
}

WebSocketClient::~WebSocketClient() { close(); }

Result<WebSocketClient> WebSocketClient::connect(const std::string& url_text,
                                                 std::shared_ptr<TlsContext> tls, Options opts) {
    auto url = Url::parse(url_text);
    if (!url) {
        return tl::make_unexpected(url.error());
    }
    if (url->scheme != "ws" && url->scheme != "wss") {
        return make_error(ErrorCode::invalid_argument, "not a WebSocket URL: " + url_text);
    }
    const ProxyConfig proxy = opts.proxy ? *opts.proxy : proxy_from_env(url->host).value_or(ProxyConfig{});
    auto stream = open_stream(*url, std::move(tls), proxy, opts.connect_timeout);
    if (!stream) {
        return tl::make_unexpected(stream.error());
    }
    auto ws = upgrade(std::move(*stream), *url, std::move(opts));
    if (ws) {
        if (auto r = ws->stream_->set_timeouts(ws->opts_.read_timeout, ws->opts_.connect_timeout);
            !r) {
            return tl::make_unexpected(r.error());
        }
    }
    return ws;
}

Result<WebSocketClient> WebSocketClient::upgrade(std::unique_ptr<Stream> stream, const Url& url,
                                                 Options opts) {
    const std::string key = random_key();
    std::string req = "GET " + url.path_and_query() + " HTTP/1.1\r\n";
    req += "Host: " + url.host_header() + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    for (const auto& [k, v] : opts.extra_headers) {
        req += k + ": " + v + "\r\n";
    }
    req += "\r\n";
    if (auto w = stream->write_all(req); !w) {
        return tl::make_unexpected(w.error());
    }

    // Read the response head byte-wise into a buffer until the blank line;
    // anything after it is the start of the first frame.
    WebSocketClient ws;
    ws.stream_ = std::move(stream);
    ws.opts_ = std::move(opts);
    std::string head;
    for (;;) {
        const std::size_t end = head.find("\r\n\r\n");
        if (end != std::string::npos) {
            ws.buffer_ = head.substr(end + 4);
            head.resize(end);
            break;
        }
        if (head.size() > 64 * 1024) {
            return make_error(ErrorCode::protocol_error, "WebSocket handshake response too large");
        }
        std::byte chunk[4096];
        auto n = ws.stream_->read_some(chunk);
        if (!n) {
            return tl::make_unexpected(n.error());
        }
        if (*n == 0) {
            return make_error(ErrorCode::connection_closed,
                              "connection closed during WebSocket handshake");
        }
        head.append(reinterpret_cast<const char*>(chunk), *n);
    }

    // Parse status + headers.
    const std::size_t line_end = head.find("\r\n");
    const std::string status_line = head.substr(0, line_end);
    if (status_line.size() < 12 || status_line.compare(9, 3, "101") != 0) {
        return make_error(ErrorCode::protocol_error,
                          "WebSocket upgrade refused: " + status_line);
    }
    std::string accept;
    bool upgrade_ok = false;
    std::size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
        std::size_t next = head.find("\r\n", pos);
        if (next == std::string::npos) {
            next = head.size();
        }
        const std::string_view line(head.data() + pos, next - pos);
        pos = next + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string name = to_lower(line.substr(0, colon));
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) {
            value.remove_suffix(1);
        }
        if (name == "sec-websocket-accept") {
            accept = std::string(value);
        } else if (name == "upgrade" && to_lower(value) == "websocket") {
            upgrade_ok = true;
        }
    }
    if (!upgrade_ok) {
        return make_error(ErrorCode::protocol_error, "missing Upgrade: websocket header");
    }
    if (accept != websocket_accept_key(key)) {
        return make_error(ErrorCode::protocol_error, "Sec-WebSocket-Accept mismatch");
    }
    return ws;
}

Result<void> WebSocketClient::send_frame(WsOpcode opcode, std::span<const std::byte> payload) {
    if (!is_open()) {
        return make_error(ErrorCode::connection_closed, "WebSocket is closed");
    }
    std::vector<std::byte> frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<std::byte>(0x80 | static_cast<std::uint8_t>(opcode)));
    const std::size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<std::byte>(0x80 | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<std::byte>(0x80 | 126));
        frame.push_back(static_cast<std::byte>((len >> 8) & 0xFF));
        frame.push_back(static_cast<std::byte>(len & 0xFF));
    } else {
        frame.push_back(static_cast<std::byte>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(len) >> shift) & 0xFF));
        }
    }
    unsigned char mask[4];
    RAND_bytes(mask, sizeof mask);
    for (unsigned char m : mask) {
        frame.push_back(static_cast<std::byte>(m));
    }
    const std::size_t start = frame.size();
    frame.resize(start + len);
    for (std::size_t i = 0; i < len; ++i) {
        frame[start + i] = payload[i] ^ static_cast<std::byte>(mask[i % 4]);
    }
    return stream_->write_all(frame);
}

Result<void> WebSocketClient::send_text(std::string_view text) {
    return send_frame(WsOpcode::text, std::as_bytes(std::span(text.data(), text.size())));
}

Result<void> WebSocketClient::send_binary(std::span<const std::byte> data) {
    return send_frame(WsOpcode::binary, data);
}

Result<void> WebSocketClient::send_ping(std::string_view payload) {
    return send_frame(WsOpcode::ping, std::as_bytes(std::span(payload.data(), payload.size())));
}

Result<void> WebSocketClient::send_close(std::uint16_t code, std::string_view reason) {
    if (close_sent_) {
        return {};
    }
    close_sent_ = true;
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload += reason.substr(0, 123);
    return send_frame(WsOpcode::close, std::as_bytes(std::span(payload.data(), payload.size())));
}

Result<void> WebSocketClient::read_exact(std::span<std::byte> out) {
    std::size_t off = 0;
    if (!buffer_.empty()) {
        const std::size_t take = std::min(out.size(), buffer_.size());
        std::memcpy(out.data(), buffer_.data(), take);
        buffer_.erase(0, take);
        off = take;
    }
    while (off < out.size()) {
        auto n = stream_->read_some(out.subspan(off));
        if (!n) {
            return tl::make_unexpected(n.error());
        }
        if (*n == 0) {
            return make_error(ErrorCode::connection_closed, "WebSocket peer closed connection");
        }
        off += *n;
    }
    return {};
}

Result<WebSocketClient::Frame> WebSocketClient::read_frame() {
    std::byte hdr[2];
    if (auto r = read_exact(hdr); !r) {
        return tl::make_unexpected(r.error());
    }
    Frame f;
    const auto b0 = static_cast<std::uint8_t>(hdr[0]);
    const auto b1 = static_cast<std::uint8_t>(hdr[1]);
    f.fin = (b0 & 0x80) != 0;
    if ((b0 & 0x70) != 0) {
        return make_error(ErrorCode::protocol_error, "WebSocket RSV bits set (no extensions negotiated)");
    }
    f.opcode = static_cast<WsOpcode>(b0 & 0x0F);
    const bool masked = (b1 & 0x80) != 0;
    std::uint64_t len = b1 & 0x7F;
    if (len == 126) {
        std::byte ext[2];
        if (auto r = read_exact(ext); !r) {
            return tl::make_unexpected(r.error());
        }
        len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(ext[0])) << 8) |
              static_cast<std::uint8_t>(ext[1]);
    } else if (len == 127) {
        std::byte ext[8];
        if (auto r = read_exact(ext); !r) {
            return tl::make_unexpected(r.error());
        }
        len = 0;
        for (std::byte b : ext) {
            len = (len << 8) | static_cast<std::uint8_t>(b);
        }
    }
    if (len > opts_.max_message_size) {
        static_cast<void>(send_close(kCloseTooBig, "frame too large"));
        return make_error(ErrorCode::protocol_error, "WebSocket frame exceeds max_message_size");
    }
    std::byte mask[4]{};
    if (masked) {
        // Servers must not mask; tolerate it but unmask correctly.
        if (auto r = read_exact(mask); !r) {
            return tl::make_unexpected(r.error());
        }
    }
    f.payload.resize(static_cast<std::size_t>(len));
    if (len > 0) {
        if (auto r = read_exact(std::as_writable_bytes(std::span(f.payload.data(), f.payload.size())));
            !r) {
            return tl::make_unexpected(r.error());
        }
        if (masked) {
            for (std::size_t i = 0; i < f.payload.size(); ++i) {
                f.payload[i] = static_cast<char>(static_cast<std::uint8_t>(f.payload[i]) ^
                                                 static_cast<std::uint8_t>(mask[i % 4]));
            }
        }
    }
    return f;
}

Result<WsMessage> WebSocketClient::receive() {
    if (!is_open()) {
        return make_error(ErrorCode::connection_closed, "WebSocket is closed");
    }
    WsMessage msg;
    bool in_message = false;
    for (;;) {
        auto frame = read_frame();
        if (!frame) {
            return tl::make_unexpected(frame.error());
        }
        switch (frame->opcode) {
            case WsOpcode::ping:
                if (auto r = send_frame(WsOpcode::pong, std::as_bytes(std::span(
                                                            frame->payload.data(),
                                                            frame->payload.size())));
                    !r) {
                    return tl::make_unexpected(r.error());
                }
                continue;
            case WsOpcode::pong:
                continue;
            case WsOpcode::close: {
                msg.opcode = WsOpcode::close;
                if (frame->payload.size() >= 2) {
                    msg.close_code = static_cast<std::uint16_t>(
                        (static_cast<std::uint8_t>(frame->payload[0]) << 8) |
                        static_cast<std::uint8_t>(frame->payload[1]));
                    msg.payload = frame->payload.substr(2);
                } else {
                    msg.close_code = kCloseNormal;
                }
                static_cast<void>(send_close(msg.close_code));
                close();
                return msg;
            }
            case WsOpcode::text:
            case WsOpcode::binary:
                if (in_message) {
                    static_cast<void>(send_close(kCloseProtocolError));
                    return make_error(ErrorCode::protocol_error,
                                      "new data frame while a fragmented message is in progress");
                }
                in_message = true;
                msg.opcode = frame->opcode;
                msg.payload = std::move(frame->payload);
                break;
            case WsOpcode::continuation:
                if (!in_message) {
                    static_cast<void>(send_close(kCloseProtocolError));
                    return make_error(ErrorCode::protocol_error,
                                      "continuation frame without a message in progress");
                }
                msg.payload += frame->payload;
                break;
            default:
                static_cast<void>(send_close(kCloseProtocolError));
                return make_error(ErrorCode::protocol_error, "unknown WebSocket opcode");
        }
        if (msg.payload.size() > opts_.max_message_size) {
            static_cast<void>(send_close(kCloseTooBig));
            return make_error(ErrorCode::protocol_error, "WebSocket message exceeds max_message_size");
        }
        if (frame->fin) {
            return msg;
        }
    }
}

void WebSocketClient::close() noexcept {
    if (stream_) {
        stream_->close();
    }
}

}  // namespace tradebot::net
