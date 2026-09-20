#pragma once

#include "tradebot/core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tradebot::net {

struct Url {
    std::string scheme;  // http, https, ws, wss
    std::string host;
    std::uint16_t port = 0;  // resolved default if absent in text
    std::string path;  // always starts with '/'
    std::string query;  // without leading '?'

    [[nodiscard]] bool is_secure() const noexcept { return scheme == "https" || scheme == "wss"; }
    [[nodiscard]] std::string path_and_query() const {
        return query.empty() ? path : path + "?" + query;
    }
    [[nodiscard]] std::string host_header() const;
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] static Result<Url> parse(std::string_view text);
};

}  // namespace tradebot::net
