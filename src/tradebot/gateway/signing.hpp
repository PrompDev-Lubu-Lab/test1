#pragma once

// Request signing for Binance-style APIs: HMAC-SHA256 over the query string,
// hex encoded. Secrets are passed in, never stored here.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tradebot::gateway {

using Params = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] std::string hmac_sha256_hex(std::string_view key, std::string_view message);
[[nodiscard]] std::string url_encode(std::string_view s);
[[nodiscard]] std::string encode_query(const Params& params);
// Appends timestamp (ms) and recvWindow, then signature=HMAC(secret, query).
[[nodiscard]] std::string signed_query(Params params, std::string_view secret, std::int64_t timestamp_ms,
                                       std::int64_t recv_window_ms = 5000);

}  // namespace tradebot::gateway
