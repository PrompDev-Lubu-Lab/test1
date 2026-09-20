#pragma once

// Binance user data stream: executionReport events -> ExecutionReports.
//
// The parser is pure (testable with recorded messages); the stream client
// owns the listenKey lifecycle (create, keepalive every 30 minutes,
// reconnect with a fresh key) on its own thread and hands parsed reports
// to a handler. Only our own orders (client ids of the form "tb<n>") are
// forwarded; anything else on the account is ignored but counted.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/error.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/execution/types.hpp"
#include "tradebot/gateway/binance_rest.hpp"
#include "tradebot/net/tls.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace tradebot::gateway {

struct ParsedExecution {
    execution::ExecutionReport report;
    std::string execution_type;  // NEW, CANCELED, REJECTED, TRADE, EXPIRED, ...
    std::string symbol;
    std::string commission_asset;
    bool is_maker = false;
};

// Returns nullopt for events that are not executionReports; errors for
// malformed executionReports. `instrument` is stamped on the report.
[[nodiscard]] Result<std::optional<ParsedExecution>> parse_user_event(std::string_view json, InstrumentId instrument);

struct UserStreamOptions {
    std::string ws_base = "wss://stream.binance.com:9443";
    Duration keepalive_interval = Duration::minutes(30);
    Duration read_timeout = Duration::seconds(5);  // socket read timeout; the loop keeps the key alive on quiet accounts
    Duration reconnect_backoff = Duration::seconds(2);
    std::optional<net::ProxyConfig> proxy;
};

class UserStream {
public:
    using Options = UserStreamOptions;
    using Handler = std::function<void(const ParsedExecution&)>;

    UserStream(BinanceRestClient& rest, std::shared_ptr<net::TlsContext> tls, InstrumentId instrument,
               Options opts, Handler handler, Logger log);

    // Blocks until `stop` is set; reconnects on any failure.
    [[nodiscard]] Result<void> run(const std::atomic<bool>& stop);

    struct Stats {
        std::uint64_t connects = 0;
        std::uint64_t reports = 0;
        std::uint64_t ignored = 0;
        std::uint64_t errors = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] Result<void> run_connection(const std::atomic<bool>& stop);

    BinanceRestClient& rest_;
    std::shared_ptr<net::TlsContext> tls_;
    InstrumentId instrument_;
    Options opts_;
    Handler handler_;
    Logger log_;
    Stats stats_;
};

}  // namespace tradebot::gateway
