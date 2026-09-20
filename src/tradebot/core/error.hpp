#pragma once

#include <tl/expected.hpp>

#include <string>
#include <string_view>

namespace tradebot {

enum class ErrorCode {
    invalid_argument,
    parse_error,
    not_found,
    overflow,
    io_error,
    invalid_state,
    unsupported,
    timeout,
    connection_closed,
    network_error,
    tls_error,
    protocol_error,
};

std::string_view to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code;
    std::string message;

    [[nodiscard]] std::string to_string() const {
        return std::string(tradebot::to_string(code)) + ": " + message;
    }
};

// tl::expected is API-compatible with std::expected and, unlike libstdc++'s
// std::expected, is usable from Clang as well as GCC.
template <class T>
using Result = tl::expected<T, Error>;

template <class E>
using Unexpected = tl::unexpected<E>;

inline Unexpected<Error> make_error(ErrorCode code, std::string message) {
    return tl::make_unexpected(Error{code, std::move(message)});
}

inline std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::invalid_argument: return "invalid_argument";
        case ErrorCode::parse_error: return "parse_error";
        case ErrorCode::not_found: return "not_found";
        case ErrorCode::overflow: return "overflow";
        case ErrorCode::io_error: return "io_error";
        case ErrorCode::invalid_state: return "invalid_state";
        case ErrorCode::unsupported: return "unsupported";
        case ErrorCode::timeout: return "timeout";
        case ErrorCode::connection_closed: return "connection_closed";
        case ErrorCode::network_error: return "network_error";
        case ErrorCode::tls_error: return "tls_error";
        case ErrorCode::protocol_error: return "protocol_error";
    }
    return "unknown";
}

}  // namespace tradebot
