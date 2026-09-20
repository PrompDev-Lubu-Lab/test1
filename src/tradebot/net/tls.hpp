#pragma once

#include "tradebot/net/stream.hpp"
#include "tradebot/net/tcp.hpp"

#include <memory>
#include <string>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace tradebot::net {

// Client-side TLS context. One per process is enough; it is safe to share
// between connections.
struct TlsOptions {
    std::string ca_file;  // extra CA bundle; system defaults are always loaded
    bool verify_peer = true;  // false only for tests against self-signed servers
};

class TlsContext {
public:
    using Options = TlsOptions;

    [[nodiscard]] static Result<std::shared_ptr<TlsContext>> create(const Options& opts = Options{});
    ~TlsContext();

    [[nodiscard]] SSL_CTX* native() const noexcept { return ctx_; }
    [[nodiscard]] bool verify_peer() const noexcept { return verify_peer_; }

private:
    TlsContext() = default;
    SSL_CTX* ctx_ = nullptr;
    bool verify_peer_ = true;
};

// TLS session over an owned TcpSocket. Performs SNI and hostname
// verification on handshake.
class TlsStream final : public Stream {
public:
    ~TlsStream() override;
    TlsStream(TlsStream&& o) noexcept;
    TlsStream& operator=(TlsStream&& o) noexcept;
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    [[nodiscard]] static Result<TlsStream> handshake(std::shared_ptr<TlsContext> ctx,
                                                     TcpSocket socket,
                                                     const std::string& server_name);

    [[nodiscard]] Result<std::size_t> read_some(std::span<std::byte> buf) override;
    [[nodiscard]] Result<void> write_all(std::span<const std::byte> data) override;
    using Stream::write_all;
    [[nodiscard]] Result<void> set_timeouts(Duration read_timeout,
                                            Duration write_timeout) override {
        return socket_.set_timeouts(read_timeout, write_timeout);
    }
    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override { return ssl_ != nullptr; }

private:
    TlsStream() = default;
    Result<void> map_error(int rc, const char* what);

    std::shared_ptr<TlsContext> ctx_;
    TcpSocket socket_;
    SSL* ssl_ = nullptr;
};

// Last OpenSSL error queue entry as text (and clears the queue).
std::string openssl_error_text();

}  // namespace tradebot::net
