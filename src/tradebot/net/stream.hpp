#pragma once

// Byte-stream abstraction shared by TCP and TLS connections. HTTP and
// WebSocket are written against Stream so they can be tested over plaintext
// loopback sockets and run over TLS in production without any changes.

#include "tradebot/core/error.hpp"
#include "tradebot/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tradebot::net {

class Stream {
public:
    virtual ~Stream() = default;

    // Reads up to buf.size() bytes. Returns 0 only on orderly EOF. A
    // receive timeout surfaces as ErrorCode::timeout so callers can treat
    // silence as feed staleness rather than as a broken connection.
    [[nodiscard]] virtual Result<std::size_t> read_some(std::span<std::byte> buf) = 0;

    // Writes the entire buffer or fails.
    [[nodiscard]] virtual Result<void> write_all(std::span<const std::byte> data) = 0;

    [[nodiscard]] Result<void> write_all(std::string_view text) {
        return write_all(std::as_bytes(std::span(text.data(), text.size())));
    }

    // Per-operation timeouts; a read that exceeds read_timeout fails with
    // ErrorCode::timeout.
    [[nodiscard]] virtual Result<void> set_timeouts(Duration read_timeout,
                                                    Duration write_timeout) = 0;

    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool is_open() const noexcept = 0;
};

}  // namespace tradebot::net
