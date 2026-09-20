#include "tradebot/gateway/binance_rest.hpp"

#include "tradebot/gateway/signing.hpp"

#include <charconv>
#include <cstdlib>

namespace tradebot::gateway {

using nlohmann::json;

BinanceCredentials credentials_from_env() {
    BinanceCredentials c;
    if (const char* k = std::getenv("TRADEBOT_BINANCE_API_KEY")) c.api_key = k;
    if (const char* s = std::getenv("TRADEBOT_BINANCE_API_SECRET")) c.api_secret = s;
    return c;
}

std::optional<VenueError> parse_venue_error(const Error& e) {
    // "binance <status> <code>: msg"
    if (e.message.rfind("binance ", 0) != 0) return std::nullopt;
    VenueError v;
    const char* p = e.message.data() + 8;
    const char* end = e.message.data() + e.message.size();
    auto r1 = std::from_chars(p, end, v.http_status);
    if (r1.ec != std::errc{} || r1.ptr >= end || *r1.ptr != ' ') return std::nullopt;
    auto r2 = std::from_chars(r1.ptr + 1, end, v.code);
    if (r2.ec != std::errc{}) return std::nullopt;
    const std::string_view rest(r2.ptr, static_cast<std::size_t>(end - r2.ptr));
    v.message = std::string(rest.rfind(": ", 0) == 0 ? rest.substr(2) : rest);
    return v;
}

bool is_unknown_order(const Error& e) {
    auto v = parse_venue_error(e);
    return v && (v->code == -2011 || v->code == -2013);
}

std::optional<ClientOrderId> BinanceRestClient::parse_client_id(std::string_view text) {
    if (text.size() < 3 || text.substr(0, 2) != "tb") return std::nullopt;
    std::uint64_t v = 0;
    const auto [ptr, ec] = std::from_chars(text.data() + 2, text.data() + text.size(), v);
    if (ec != std::errc{} || ptr != text.data() + text.size()) return std::nullopt;
    return ClientOrderId{v};
}

BinanceRestClient::BinanceRestClient(net::HttpClient& http, BinanceCredentials creds, const Clock& clock,
                                     Options opts)
    : http_(http), creds_(std::move(creds)), clock_(clock), opts_(std::move(opts)) {}

Result<json> BinanceRestClient::parse_response(const net::HttpResponse& resp) {
    auto j = json::parse(resp.body, nullptr, false);
    if (!resp.ok()) {
        // A venue error always carries {"code","msg"}; anything else (a proxy
        // 502, an HTML error page) is a transport failure whose outcome is
        // unknown, so it must not parse as a venue rejection.
        if (j.is_object() && j.contains("code") && j["code"].is_number_integer()) {
            const int code = j.value("code", 0);
            const std::string msg = j.value("msg", "");
            return make_error(resp.status == 429 || resp.status == 418 ? ErrorCode::invalid_state
                                                                       : ErrorCode::network_error,
                              "binance " + std::to_string(resp.status) + " " + std::to_string(code) + ": " + msg);
        }
        return make_error(ErrorCode::network_error, "binance: http " + std::to_string(resp.status) + " with body '" +
                                                        resp.body.substr(0, 200) + "'");
    }
    if (j.is_discarded()) {
        return make_error(ErrorCode::protocol_error, "binance: response is not JSON");
    }
    return j;
}

Result<json> BinanceRestClient::signed_call(const std::string& method, const std::string& path, Params params) {
    if (!creds_.present()) {
        return make_error(ErrorCode::invalid_state, "binance: API credentials not configured");
    }
    const std::string query = signed_query(std::move(params), creds_.api_secret,
                                           clock_.now().millis_since_epoch(), opts_.recv_window_ms);
    const net::Headers headers{{"X-MBX-APIKEY", creds_.api_key},
                               {"Content-Type", "application/x-www-form-urlencoded"}};
    // Binance accepts signed parameters in the query string for every method.
    auto resp = http_.request(method, opts_.base_url + path + "?" + query, headers);
    if (!resp) {
        return tl::make_unexpected(resp.error());
    }
    return parse_response(*resp);
}

Result<json> BinanceRestClient::keyed_call(const std::string& method, const std::string& path, const Params& params) {
    if (creds_.api_key.empty()) {
        return make_error(ErrorCode::invalid_state, "binance: API key not configured");
    }
    const net::Headers headers{{"X-MBX-APIKEY", creds_.api_key}};
    const std::string q = encode_query(params);
    auto resp = http_.request(method, opts_.base_url + path + (q.empty() ? "" : "?" + q), headers);
    if (!resp) {
        return tl::make_unexpected(resp.error());
    }
    return parse_response(*resp);
}

Result<json> BinanceRestClient::new_order(const std::string& symbol, const execution::OrderRequest& r) {
    Params p{{"symbol", symbol},
             {"side", r.side == Side::buy ? "BUY" : "SELL"},
             {"newClientOrderId", client_id_text(r.client_id)},
             {"quantity", r.quantity.to_string()},
             {"newOrderRespType", "ACK"}};
    if (r.type == OrderType::market) {
        p.emplace_back("type", "MARKET");
    } else {
        switch (r.time_in_force) {
            case TimeInForce::post_only:
                p.emplace_back("type", "LIMIT_MAKER");
                break;
            case TimeInForce::ioc:
                p.emplace_back("type", "LIMIT");
                p.emplace_back("timeInForce", "IOC");
                break;
            case TimeInForce::fok:
                p.emplace_back("type", "LIMIT");
                p.emplace_back("timeInForce", "FOK");
                break;
            case TimeInForce::gtc:
                p.emplace_back("type", "LIMIT");
                p.emplace_back("timeInForce", "GTC");
                break;
        }
        p.emplace_back("price", r.price.to_string());
    }
    return signed_call("POST", "/api/v3/order", std::move(p));
}

Result<json> BinanceRestClient::cancel_order(const std::string& symbol, ClientOrderId client_id) {
    return signed_call("DELETE", "/api/v3/order", {{"symbol", symbol}, {"origClientOrderId", client_id_text(client_id)}});
}

Result<json> BinanceRestClient::query_order(const std::string& symbol, ClientOrderId client_id) {
    return signed_call("GET", "/api/v3/order", {{"symbol", symbol}, {"origClientOrderId", client_id_text(client_id)}});
}

Result<json> BinanceRestClient::open_orders(const std::string& symbol) {
    return signed_call("GET", "/api/v3/openOrders", {{"symbol", symbol}});
}

Result<std::vector<Balance>> BinanceRestClient::balances() {
    auto j = signed_call("GET", "/api/v3/account", {{"omitZeroBalances", "true"}});
    if (!j) return tl::make_unexpected(j.error());
    if (!j->contains("balances") || !(*j)["balances"].is_array()) {
        return make_error(ErrorCode::protocol_error, "binance: account response missing balances");
    }
    std::vector<Balance> out;
    for (const auto& b : (*j)["balances"]) {
        auto free = Quantity::parse(b.value("free", "0"));
        auto locked = Quantity::parse(b.value("locked", "0"));
        if (!free || !locked) {
            return make_error(ErrorCode::protocol_error, "binance: malformed balance");
        }
        out.push_back(Balance{b.value("asset", ""), *free, *locked});
    }
    return out;
}

Result<std::string> BinanceRestClient::create_listen_key() {
    auto j = keyed_call("POST", "/api/v3/userDataStream", {});
    if (!j) return tl::make_unexpected(j.error());
    if (!j->contains("listenKey") || !(*j)["listenKey"].is_string()) {
        return make_error(ErrorCode::protocol_error, "binance: no listenKey in response");
    }
    return (*j)["listenKey"].get<std::string>();
}

Result<void> BinanceRestClient::keepalive_listen_key(const std::string& key) {
    auto j = keyed_call("PUT", "/api/v3/userDataStream", {{"listenKey", key}});
    if (!j) return tl::make_unexpected(j.error());
    return {};
}

Result<std::int64_t> BinanceRestClient::server_time_ms() {
    auto resp = http_.get(opts_.base_url + "/api/v3/time");
    if (!resp) return tl::make_unexpected(resp.error());
    auto j = parse_response(*resp);
    if (!j) return tl::make_unexpected(j.error());
    if (!j->contains("serverTime")) {
        return make_error(ErrorCode::protocol_error, "binance: no serverTime");
    }
    return (*j)["serverTime"].get<std::int64_t>();
}

}  // namespace tradebot::gateway
