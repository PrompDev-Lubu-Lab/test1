#include "tradebot/net/url.hpp"

#include <charconv>

namespace tradebot::net {

Result<Url> Url::parse(std::string_view text) {
    Url u;
    const std::size_t scheme_end = text.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return make_error(ErrorCode::parse_error, "URL missing scheme: '" + std::string(text) + "'");
    }
    u.scheme = std::string(text.substr(0, scheme_end));
    for (char& c : u.scheme) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    std::uint16_t default_port = 0;
    if (u.scheme == "http" || u.scheme == "ws") {
        default_port = 80;
    } else if (u.scheme == "https" || u.scheme == "wss") {
        default_port = 443;
    } else {
        return make_error(ErrorCode::unsupported, "unsupported URL scheme '" + u.scheme + "'");
    }

    std::string_view rest = text.substr(scheme_end + 3);
    const std::size_t path_start = rest.find_first_of("/?");
    std::string_view authority = rest.substr(0, path_start);
    if (authority.empty()) {
        return make_error(ErrorCode::parse_error, "URL missing host: '" + std::string(text) + "'");
    }
    if (authority.find('@') != std::string_view::npos) {
        return make_error(ErrorCode::unsupported, "URL userinfo is not supported");
    }
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        u.host = std::string(authority.substr(0, colon));
        const std::string_view port_text = authority.substr(colon + 1);
        unsigned port = 0;
        const auto [ptr, ec] =
            std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (ec != std::errc{} || ptr != port_text.data() + port_text.size() || port == 0 ||
            port > 65535) {
            return make_error(ErrorCode::parse_error,
                              "invalid URL port: '" + std::string(port_text) + "'");
        }
        u.port = static_cast<std::uint16_t>(port);
    } else {
        u.host = std::string(authority);
        u.port = default_port;
    }
    if (u.host.empty()) {
        return make_error(ErrorCode::parse_error, "URL missing host: '" + std::string(text) + "'");
    }

    if (path_start == std::string_view::npos) {
        u.path = "/";
    } else {
        std::string_view tail = rest.substr(path_start);
        const std::size_t q = tail.find('?');
        u.path = std::string(tail.substr(0, q));
        if (u.path.empty()) {
            u.path = "/";
        }
        if (q != std::string_view::npos) {
            u.query = std::string(tail.substr(q + 1));
        }
    }
    return u;
}

std::string Url::host_header() const {
    const bool default_port = (is_secure() && port == 443) || (!is_secure() && port == 80);
    return default_port ? host : host + ":" + std::to_string(port);
}

std::string Url::to_string() const {
    return scheme + "://" + host_header() + path_and_query();
}

}  // namespace tradebot::net
