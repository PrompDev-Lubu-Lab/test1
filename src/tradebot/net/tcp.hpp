#pragma once

#include "tradebot/core/time.hpp"
#include "tradebot/net/stream.hpp"

#include <cstdint>
#include <string>

namespace tradebot::net {

// Blocking TCP socket with per-operation timeouts (SO_RCVTIMEO/SO_SNDTIMEO).
class TcpSocket final : public Stream {
public:
    TcpSocket() = default;
    explicit TcpSocket(int fd) noexcept : fd_(fd) {}
    ~TcpSocket() override;
    TcpSocket(TcpSocket&& o) noexcept;
    TcpSocket& operator=(TcpSocket&& o) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    [[nodiscard]] static Result<TcpSocket> connect(const std::string& host, std::uint16_t port,
                                                   Duration timeout);

    [[nodiscard]] Result<void> set_timeouts(Duration read_timeout,
                                            Duration write_timeout) override;
    [[nodiscard]] Result<void> set_nodelay(bool on);

    [[nodiscard]] Result<std::size_t> read_some(std::span<std::byte> buf) override;
    [[nodiscard]] Result<void> write_all(std::span<const std::byte> data) override;
    using Stream::write_all;
    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override { return fd_ >= 0; }

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_ = -1;
};

// Loopback listener used by tests and by local tooling.
class TcpListener {
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(TcpListener&& o) noexcept;
    TcpListener& operator=(TcpListener&& o) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Binds to 127.0.0.1 on the given port (0 = any free port).
    [[nodiscard]] static Result<TcpListener> bind_loopback(std::uint16_t port = 0);
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] Result<TcpSocket> accept(Duration timeout);
    void close() noexcept;

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

}  // namespace tradebot::net
