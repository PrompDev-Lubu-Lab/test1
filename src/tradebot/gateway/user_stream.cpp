#include "tradebot/gateway/user_stream.hpp"

#include "tradebot/net/websocket.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>

namespace tradebot::gateway {

using execution::ExecutionReport;
using execution::ReportType;
using nlohmann::json;

namespace {

Result<OrderStatus> map_status(std::string_view x) {
    if (x == "NEW") return OrderStatus::open;
    if (x == "PARTIALLY_FILLED") return OrderStatus::partially_filled;
    if (x == "FILLED") return OrderStatus::filled;
    if (x == "CANCELED" || x == "PENDING_CANCEL") return OrderStatus::cancelled;
    if (x == "REJECTED") return OrderStatus::rejected;
    if (x == "EXPIRED" || x == "EXPIRED_IN_MATCH") return OrderStatus::expired;
    return make_error(ErrorCode::parse_error, "binance: unknown order status '" + std::string(x) + "'");
}

Result<ReportType> map_type(std::string_view exec_type, OrderStatus status) {
    if (exec_type == "NEW") return ReportType::accepted;
    if (exec_type == "TRADE") return ReportType::fill;
    if (exec_type == "CANCELED") return ReportType::cancelled;
    if (exec_type == "REJECTED") return ReportType::rejected;
    if (exec_type == "EXPIRED" || exec_type == "TRADE_PREVENTION") return ReportType::expired;
    if (exec_type == "REPLACED") return status == OrderStatus::cancelled ? ReportType::cancelled : ReportType::accepted;
    return make_error(ErrorCode::parse_error, "binance: unknown execution type '" + std::string(exec_type) + "'");
}

}  // namespace

Result<std::optional<ParsedExecution>> parse_user_event(std::string_view text, InstrumentId instrument) {
    auto j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return make_error(ErrorCode::parse_error, "binance user event is not a JSON object");
    }
    if (j.value("e", "") != "executionReport") {
        return std::optional<ParsedExecution>{};
    }
    auto str = [&](const char* k) -> Result<std::string> {
        if (!j.contains(k) || !j[k].is_string()) {
            return make_error(ErrorCode::parse_error, std::string("executionReport missing '") + k + "'");
        }
        return j[k].get<std::string>();
    };
    ParsedExecution out;
    auto c = str("c"); if (!c) return tl::make_unexpected(c.error());
    auto S = str("S"); if (!S) return tl::make_unexpected(S.error());
    auto o = str("o"); if (!o) return tl::make_unexpected(o.error());
    auto x = str("x"); if (!x) return tl::make_unexpected(x.error());
    auto X = str("X"); if (!X) return tl::make_unexpected(X.error());
    auto q = str("q"); if (!q) return tl::make_unexpected(q.error());
    auto p = str("p"); if (!p) return tl::make_unexpected(p.error());
    auto z = str("z"); if (!z) return tl::make_unexpected(z.error());
    auto l = str("l"); if (!l) return tl::make_unexpected(l.error());
    auto L = str("L"); if (!L) return tl::make_unexpected(L.error());
    auto n = str("n"); if (!n) return tl::make_unexpected(n.error());
    // For cancels the original client id is in "C"; "c" holds the cancel request's id.
    std::string client_text = *c;
    if (*x == "CANCELED" && j.contains("C") && j["C"].is_string() && !j["C"].get<std::string>().empty()) {
        client_text = j["C"].get<std::string>();
    }
    auto client_id = BinanceRestClient::parse_client_id(client_text);
    ExecutionReport& r = out.report;
    r.client_id = client_id.value_or(ClientOrderId{});
    r.order_id = OrderId{j.value("i", std::uint64_t{0})};
    r.instrument = instrument;
    r.side = *S == "BUY" ? Side::buy : Side::sell;
    r.order_type = *o == "MARKET" ? OrderType::market : OrderType::limit;
    auto price = Price::parse(*p);
    auto qty = Quantity::parse(*q);
    auto cum = Quantity::parse(*z);
    auto last_qty = Quantity::parse(*l);
    auto last_px = Price::parse(*L);
    auto fee = Notional::parse(*n);
    if (!price || !qty || !cum || !last_qty || !last_px || !fee) {
        return make_error(ErrorCode::parse_error, "executionReport has a malformed decimal");
    }
    r.price = *price;
    r.time = Timestamp::from_millis(j.value("T", j.value("E", std::int64_t{0})));
    auto status = map_status(*X);
    if (!status) return tl::make_unexpected(status.error());
    r.status = *status;
    auto type = map_type(*x, *status);
    if (!type) return tl::make_unexpected(type.error());
    r.type = *type;
    r.filled_quantity = *cum;
    r.remaining_quantity = *qty - *cum;
    if (r.type == ReportType::fill) {
        execution::Fill f;
        f.price = *last_px;
        f.quantity = *last_qty;
        f.fee = *fee;
        f.liquidity = j.value("m", false) ? Liquidity::maker : Liquidity::taker;
        f.exec_id = TradeId{static_cast<std::uint64_t>(j.value("t", std::int64_t{0}))};
        r.fill = f;
    }
    if (j.contains("r") && j["r"].is_string() && j["r"].get<std::string>() != "NONE") {
        r.reason = j["r"].get<std::string>();
    }
    out.execution_type = *x;
    out.symbol = j.value("s", "");
    if (j.contains("N") && j["N"].is_string()) out.commission_asset = j["N"].get<std::string>();
    out.is_maker = j.value("m", false);
    if (!client_id) {
        out.report.client_id = ClientOrderId{};  // not ours; caller decides
    }
    return std::optional<ParsedExecution>(std::move(out));
}

UserStream::UserStream(BinanceRestClient& rest, std::shared_ptr<net::TlsContext> tls, InstrumentId instrument,
                       Options opts, Handler handler, Logger log)
    : rest_(rest), tls_(std::move(tls)), instrument_(instrument), opts_(std::move(opts)),
      handler_(std::move(handler)), log_(std::move(log)) {}

Result<void> UserStream::run_connection(const std::atomic<bool>& stop) {
    auto key = rest_.create_listen_key();
    if (!key) {
        return tl::make_unexpected(key.error());
    }
    net::WebSocketOptions ws_opts;
    ws_opts.read_timeout = opts_.stale_timeout;
    ws_opts.proxy = opts_.proxy;
    auto ws = net::WebSocketClient::connect(opts_.ws_base + "/ws/" + *key, tls_, ws_opts);
    if (!ws) {
        return tl::make_unexpected(ws.error());
    }
    ++stats_.connects;
    log_.info("user stream connected");
    auto last_keepalive = std::chrono::steady_clock::now();
    while (!stop.load()) {
        auto msg = ws->receive();
        if (stop.load()) break;
        if (!msg) {
            if (msg.error().code == ErrorCode::timeout) {
                // Quiet account: keep the key alive and keep listening.
                if (std::chrono::steady_clock::now() - last_keepalive >= opts_.keepalive_interval.to_chrono()) {
                    if (auto k = rest_.keepalive_listen_key(*key); !k) {
                        return tl::make_unexpected(k.error());
                    }
                    last_keepalive = std::chrono::steady_clock::now();
                }
                continue;
            }
            return tl::make_unexpected(msg.error());
        }
        if (msg->opcode == net::WsOpcode::close) {
            return make_error(ErrorCode::connection_closed, "user stream closed by server");
        }
        auto parsed = parse_user_event(msg->payload, instrument_);
        if (!parsed) {
            ++stats_.errors;
            log_.warn("user stream: {}", parsed.error().message);
            continue;
        }
        if (!parsed->has_value() || !(*parsed)->report.client_id.is_valid()) {
            ++stats_.ignored;
            continue;
        }
        ++stats_.reports;
        handler_(**parsed);
        if (std::chrono::steady_clock::now() - last_keepalive >= opts_.keepalive_interval.to_chrono()) {
            if (auto k = rest_.keepalive_listen_key(*key); !k) {
                log_.warn("listenKey keepalive failed: {}", k.error().to_string());
            }
            last_keepalive = std::chrono::steady_clock::now();
        }
    }
    static_cast<void>(ws->send_close());
    return {};
}

Result<void> UserStream::run(const std::atomic<bool>& stop) {
    while (!stop.load()) {
        auto r = run_connection(stop);
        if (stop.load()) break;
        if (!r) {
            ++stats_.errors;
            log_.warn("user stream ended: {}; reconnecting in {}", r.error().to_string(),
                      opts_.reconnect_backoff.to_string());
            const auto deadline = std::chrono::steady_clock::now() + opts_.reconnect_backoff.to_chrono();
            while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }
    return {};
}

}  // namespace tradebot::gateway
