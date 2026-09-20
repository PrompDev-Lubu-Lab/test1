#pragma once

// Binance spot private REST: the handful of signed calls live trading needs.
// Every call returns the parsed JSON body or a typed error carrying the
// venue's code and message, so the gateway can distinguish "unknown order"
// from "rate limited" from "your clock is off".

#include "tradebot/core/clock.hpp"
#include "tradebot/core/error.hpp"
#include "tradebot/execution/types.hpp"
#include "tradebot/gateway/signing.hpp"
#include "tradebot/net/http.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace tradebot::gateway {

struct BinanceCredentials {
    std::string api_key;
    std::string api_secret;
    [[nodiscard]] bool present() const noexcept { return !api_key.empty() && !api_secret.empty(); }
};

// Reads TRADEBOT_BINANCE_API_KEY / TRADEBOT_BINANCE_API_SECRET.
[[nodiscard]] BinanceCredentials credentials_from_env();

struct VenueError {
    int http_status = 0;
    int code = 0;  // Binance error code (e.g. -2011 unknown order), 0 if none
    std::string message;
};

// Venue errors are encoded in Error::message as "binance <status> <code>: msg";
// parse_venue_error recovers the structure.
[[nodiscard]] std::optional<VenueError> parse_venue_error(const Error& e);
[[nodiscard]] bool is_unknown_order(const Error& e);

struct Balance {
    std::string asset;
    Quantity free;
    Quantity locked;
};

struct BinanceRestOptions {
    std::string base_url = "https://api.binance.com";
    std::int64_t recv_window_ms = 5000;
    Duration timeout = Duration::seconds(10);
    std::optional<net::ProxyConfig> proxy;
};

class BinanceRestClient {
public:
    using Options = BinanceRestOptions;

    BinanceRestClient(net::HttpClient& http, BinanceCredentials creds, const Clock& clock,
                      Options opts = Options{});

    // Orders. `symbol` is the venue symbol; client ids are formatted "tb<id>".
    [[nodiscard]] Result<nlohmann::json> new_order(const std::string& symbol, const execution::OrderRequest& r);
    [[nodiscard]] Result<nlohmann::json> cancel_order(const std::string& symbol, ClientOrderId client_id);
    [[nodiscard]] Result<nlohmann::json> query_order(const std::string& symbol, ClientOrderId client_id);
    [[nodiscard]] Result<nlohmann::json> open_orders(const std::string& symbol);
    // Account.
    [[nodiscard]] Result<std::vector<Balance>> balances();
    // User data stream.
    [[nodiscard]] Result<std::string> create_listen_key();
    [[nodiscard]] Result<void> keepalive_listen_key(const std::string& key);
    // Public: server time (for clock-skew checks).
    [[nodiscard]] Result<std::int64_t> server_time_ms();

    [[nodiscard]] static std::string client_id_text(ClientOrderId id) { return "tb" + std::to_string(id.value()); }
    [[nodiscard]] static std::optional<ClientOrderId> parse_client_id(std::string_view text);

private:
    [[nodiscard]] Result<nlohmann::json> signed_call(const std::string& method, const std::string& path,
                                                     Params params);
    [[nodiscard]] Result<nlohmann::json> keyed_call(const std::string& method, const std::string& path,
                                                    const Params& params);
    [[nodiscard]] static Result<nlohmann::json> parse_response(const net::HttpResponse& resp);

    net::HttpClient& http_;
    BinanceCredentials creds_;
    const Clock& clock_;
    Options opts_;
};

}  // namespace tradebot::gateway
