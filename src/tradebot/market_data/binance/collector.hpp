#pragma once

// Binance spot market-data collector.
//
// Subscribes to a combined WebSocket stream for one symbol and writes every
// message verbatim to a RawCaptureWriter. It also captures the pieces a
// later order-book rebuild needs and that the stream alone does not carry:
//
//   - exchange_info   the symbol's filters (tick size, lot size, ...) once
//                     at start-up
//   - depth_snapshot  a full REST order-book snapshot after every connect,
//                     whenever a sequence gap is seen in depth updates, and
//                     periodically so replay can start from any hour
//
// The collector is deliberately single-threaded and blocking. Its only job
// is to keep the connection alive and the disk current; all interpretation
// happens later in the processor.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/market_data/raw_capture.hpp"
#include "tradebot/net/http.hpp"
#include "tradebot/net/tls.hpp"
#include "tradebot/net/websocket.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tradebot::market_data::binance {

struct CollectorConfig {
    std::string ws_base = "wss://stream.binance.com:9443";
    std::string rest_base = "https://api.binance.com";
    std::string symbol = "ETHUSDT";
    // Stream suffixes; the symbol is prepended in lower case.
    std::vector<std::string> streams = {"aggTrade", "trade", "depth@100ms", "kline_1m",
                                        "bookTicker"};
    int depth_snapshot_limit = 1000;
    bool fetch_exchange_info = true;

    Duration connect_timeout = Duration::seconds(10);
    Duration stale_timeout = Duration::seconds(15);  // no message for this long => reconnect
    Duration flush_interval = Duration::seconds(1);
    Duration snapshot_interval = Duration::hours(1);
    Duration max_connection_age = Duration::hours(23);  // Binance drops at 24h
    Duration reconnect_backoff_min = Duration::seconds(1);
    Duration reconnect_backoff_max = Duration::seconds(60);
    std::optional<net::ProxyConfig> proxy;  // nullopt = environment
};

struct CollectorStats {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::uint64_t connects = 0;
    std::uint64_t reconnects = 0;
    std::uint64_t depth_snapshots = 0;
    std::uint64_t depth_gaps = 0;
    std::uint64_t stale_timeouts = 0;
    std::uint64_t errors = 0;
};

// Receives every captured record (after it has been written, if a writer is
// attached). Used by the paper-trading runtime to feed live events.
using RecordHandler = std::function<void(const RawRecord&)>;

class Collector {
public:
    // `writer` may be null (no archive), `handler` may be empty (no
    // consumer); at least one should be set for the collector to be useful.
    Collector(CollectorConfig config, RawCaptureWriter* writer, RecordHandler handler,
              std::shared_ptr<net::TlsContext> tls, const Clock& clock, Logger log);
    Collector(CollectorConfig config, RawCaptureWriter& writer, std::shared_ptr<net::TlsContext> tls,
              const Clock& clock, Logger log)
        : Collector(std::move(config), &writer, RecordHandler{}, std::move(tls), clock, std::move(log)) {}

    // Runs until `stop` becomes true. Returns an error only for unrecoverable
    // problems (e.g. the capture directory is unwritable); network failures
    // are retried with backoff forever.
    [[nodiscard]] Result<void> run(const std::atomic<bool>& stop);

    // One connection lifetime: connect, snapshot, stream until an error, the
    // stop flag, staleness or max age. Exposed for tests.
    [[nodiscard]] Result<void> run_connection(const std::atomic<bool>& stop);

    [[nodiscard]] const CollectorStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::string stream_url() const;

private:
    [[nodiscard]] Result<void> emit(const RawRecord& record);
    [[nodiscard]] Result<void> flush_writer();
    [[nodiscard]] Result<void> capture_exchange_info();
    [[nodiscard]] Result<void> capture_depth_snapshot(std::string_view reason);
    [[nodiscard]] Result<void> handle_message(std::string_view message, Timestamp recv_time);
    void interruptible_sleep(Duration d, const std::atomic<bool>& stop) const;

    CollectorConfig config_;
    RawCaptureWriter* writer_;
    RecordHandler handler_;
    std::shared_ptr<net::TlsContext> tls_;
    const Clock& clock_;
    Logger log_;
    CollectorStats stats_;

    std::string symbol_lower_;
    std::string depth_stream_name_;
    std::int64_t last_depth_update_id_ = -1;  // "u" of the last depth event
    bool exchange_info_captured_ = false;
};

// Extracts the "stream" name from a combined-stream message
// {"stream":"ethusdt@aggTrade","data":{...}} without a full JSON parse.
// Returns an empty view for anything else.
[[nodiscard]] std::string_view combined_stream_name(std::string_view message) noexcept;

// Extracts integer fields "U" and "u" from a depthUpdate payload. Returns
// false if either is missing.
[[nodiscard]] bool depth_update_ids(std::string_view message, std::int64_t& first_id,
                                    std::int64_t& final_id) noexcept;

}  // namespace tradebot::market_data::binance
