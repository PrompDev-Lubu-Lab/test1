#include "tradebot/market_data/binance/collector.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <thread>

namespace tradebot::market_data::binance {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// Finds `"key":` at top level-ish (first occurrence) and parses an integer.
bool find_int_field(std::string_view text, std::string_view key, std::int64_t& out) noexcept {
    const std::string needle = "\"" + std::string(key) + "\":";
    const std::size_t pos = text.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }
    const char* begin = text.data() + pos + needle.size();
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr != begin;
}

}  // namespace

std::string_view combined_stream_name(std::string_view message) noexcept {
    static constexpr std::string_view kPrefix = "{\"stream\":\"";
    if (message.substr(0, kPrefix.size()) != kPrefix) {
        return {};
    }
    const std::size_t start = kPrefix.size();
    const std::size_t end = message.find('"', start);
    if (end == std::string_view::npos) {
        return {};
    }
    return message.substr(start, end - start);
}

bool depth_update_ids(std::string_view message, std::int64_t& first_id,
                      std::int64_t& final_id) noexcept {
    return find_int_field(message, "U", first_id) && find_int_field(message, "u", final_id);
}

Collector::Collector(CollectorConfig config, RawCaptureWriter* writer, RecordHandler handler,
                     std::shared_ptr<net::TlsContext> tls, const Clock& clock, Logger log)
    : config_(std::move(config)),
      writer_(writer),
      handler_(std::move(handler)),
      tls_(std::move(tls)),
      clock_(clock),
      log_(std::move(log)),
      symbol_lower_(to_lower(config_.symbol)) {
    for (const auto& s : config_.streams) {
        if (s.rfind("depth", 0) == 0) {
            depth_stream_name_ = symbol_lower_ + "@" + s;
        }
    }
}

std::string Collector::stream_url() const {
    std::string url = config_.ws_base + "/stream?streams=";
    for (std::size_t i = 0; i < config_.streams.size(); ++i) {
        if (i > 0) {
            url += '/';
        }
        url += symbol_lower_ + "@" + config_.streams[i];
    }
    return url;
}

Result<void> Collector::emit(const RawRecord& record) {
    if (writer_ != nullptr) {
        if (auto w = writer_->write(record); !w) {
            return w;
        }
    }
    if (handler_) {
        handler_(record);
    }
    return {};
}

Result<void> Collector::flush_writer() {
    if (writer_ == nullptr) {
        return {};
    }
    return writer_->flush();
}

void Collector::interruptible_sleep(Duration d, const std::atomic<bool>& stop) const {
    const auto deadline = std::chrono::steady_clock::now() + d.to_chrono();
    while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

Result<void> Collector::capture_exchange_info() {
    auto http = net::HttpClient::create(tls_, {.timeout = config_.connect_timeout,
                                               .proxy = config_.proxy});
    if (!http) {
        return tl::make_unexpected(http.error());
    }
    const std::string url = config_.rest_base + "/api/v3/exchangeInfo?symbol=" + config_.symbol;
    auto resp = http->get(url);
    if (!resp) {
        return tl::make_unexpected(resp.error());
    }
    if (!resp->ok()) {
        return make_error(ErrorCode::network_error,
                          "exchangeInfo returned HTTP " + std::to_string(resp->status) + ": " +
                              resp->body.substr(0, 200));
    }
    if (!nlohmann::json::accept(resp->body)) {
        return make_error(ErrorCode::protocol_error, "exchangeInfo body is not JSON");
    }
    if (auto w = emit({clock_.now(), "exchange_info", resp->body}); !w) {
        return w;
    }
    exchange_info_captured_ = true;
    log_.info("captured exchange_info for {}", config_.symbol);
    return {};
}

Result<void> Collector::capture_depth_snapshot(std::string_view reason) {
    auto http = net::HttpClient::create(tls_, {.timeout = config_.connect_timeout,
                                               .proxy = config_.proxy});
    if (!http) {
        return tl::make_unexpected(http.error());
    }
    const std::string url = config_.rest_base + "/api/v3/depth?symbol=" + config_.symbol +
                            "&limit=" + std::to_string(config_.depth_snapshot_limit);
    auto resp = http->get(url);
    if (!resp) {
        return tl::make_unexpected(resp.error());
    }
    if (!resp->ok()) {
        return make_error(ErrorCode::network_error,
                          "depth snapshot returned HTTP " + std::to_string(resp->status) + ": " +
                              resp->body.substr(0, 200));
    }
    std::int64_t last_update_id = 0;
    if (!nlohmann::json::accept(resp->body) ||
        !find_int_field(resp->body, "lastUpdateId", last_update_id)) {
        return make_error(ErrorCode::protocol_error, "depth snapshot body is not a valid snapshot");
    }
    if (auto w = emit({clock_.now(), "depth_snapshot", resp->body}); !w) {
        return w;
    }
    ++stats_.depth_snapshots;
    log_.info("captured depth snapshot ({}), lastUpdateId={}, {} bytes", reason, last_update_id,
              resp->body.size());
    return {};
}

Result<void> Collector::handle_message(std::string_view message, Timestamp recv_time) {
    const std::string_view stream = combined_stream_name(message);
    RawRecord rec{recv_time, std::string(stream.empty() ? "unknown" : stream),
                  std::string(message)};
    if (auto w = emit(rec); !w) {
        return w;
    }
    ++stats_.messages;
    stats_.bytes += message.size();

    if (!depth_stream_name_.empty() && stream == depth_stream_name_) {
        std::int64_t first = 0, final = 0;
        if (depth_update_ids(message, first, final)) {
            if (last_depth_update_id_ >= 0 && first != last_depth_update_id_ + 1) {
                ++stats_.depth_gaps;
                log_.warn("depth sequence gap: expected U={}, got U={} (u={}); re-snapshotting",
                          last_depth_update_id_ + 1, first, final);
                last_depth_update_id_ = final;
                return capture_depth_snapshot("sequence gap");
            }
            last_depth_update_id_ = final;
        } else {
            log_.warn("depth message without U/u fields: {}", message.substr(0, 120));
        }
    }
    return {};
}

Result<void> Collector::run_connection(const std::atomic<bool>& stop) {
    if (config_.fetch_exchange_info && !exchange_info_captured_) {
        if (auto r = capture_exchange_info(); !r) {
            return r;
        }
    }

    net::WebSocketOptions ws_opts;
    ws_opts.connect_timeout = config_.connect_timeout;
    ws_opts.read_timeout = config_.stale_timeout;
    ws_opts.proxy = config_.proxy;
    const std::string url = stream_url();
    log_.info("connecting to {}", url);
    auto ws = net::WebSocketClient::connect(url, tls_, ws_opts);
    if (!ws) {
        return tl::make_unexpected(ws.error());
    }
    ++stats_.connects;
    const auto connected_at = std::chrono::steady_clock::now();
    log_.info("connected");

    // Snapshot after the stream is open so the processor can bracket its
    // lastUpdateId with buffered depth events (Binance's documented order).
    last_depth_update_id_ = -1;
    if (!depth_stream_name_.empty()) {
        if (auto r = capture_depth_snapshot("connect"); !r) {
            return r;
        }
    }

    auto last_flush = std::chrono::steady_clock::now();
    auto last_snapshot = last_flush;
    while (!stop.load()) {
        auto msg = ws->receive();
        if (stop.load()) {
            // Stopped while blocked in receive: leave politely.
            break;
        }
        if (!msg) {
            if (msg.error().code == ErrorCode::timeout) {
                ++stats_.stale_timeouts;
                log_.warn("no message for {}; treating feed as stale",
                          config_.stale_timeout.to_string());
                return make_error(ErrorCode::timeout, "feed stale");
            }
            return tl::make_unexpected(msg.error());
        }
        const Timestamp recv_time = clock_.now();
        if (msg->opcode == net::WsOpcode::close) {
            log_.warn("server closed connection: code={} reason='{}'", msg->close_code,
                      msg->payload);
            return make_error(ErrorCode::connection_closed, "server closed WebSocket");
        }
        if (auto r = handle_message(msg->payload, recv_time); !r) {
            return r;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush >= config_.flush_interval.to_chrono()) {
            if (auto r = flush_writer(); !r) {
                return r;
            }
            last_flush = now;
        }
        if (!depth_stream_name_.empty() &&
            now - last_snapshot >= config_.snapshot_interval.to_chrono()) {
            if (auto r = capture_depth_snapshot("periodic"); !r) {
                return r;
            }
            last_snapshot = now;
        }
        if (now - connected_at >= config_.max_connection_age.to_chrono()) {
            log_.info("connection reached max age; reconnecting");
            static_cast<void>(ws->send_close());
            return make_error(ErrorCode::connection_closed, "max connection age");
        }
    }
    if (ws->is_open()) {
        static_cast<void>(ws->send_close());
    }
    return flush_writer();
}

Result<void> Collector::run(const std::atomic<bool>& stop) {
    Duration backoff = config_.reconnect_backoff_min;
    while (!stop.load()) {
        auto r = run_connection(stop);
        if (stop.load()) {
            break;
        }
        if (r) {
            // A clean return without stop only happens on stop; treat as loop.
            continue;
        }
        const ErrorCode code = r.error().code;
        if (code == ErrorCode::io_error) {
            // Disk problems are not something a reconnect fixes.
            log_.error("fatal: {}", r.error().to_string());
            return r;
        }
        ++stats_.errors;
        ++stats_.reconnects;
        static_cast<void>(flush_writer());
        log_.warn("connection ended: {}; reconnecting in {}", r.error().to_string(),
                  backoff.to_string());
        interruptible_sleep(backoff, stop);
        backoff = std::min(backoff * 2, config_.reconnect_backoff_max);
        if (stats_.messages > 0 && code == ErrorCode::connection_closed) {
            // A long-lived connection that ended normally restarts fast.
            backoff = config_.reconnect_backoff_min;
        }
    }
    return flush_writer();
}

}  // namespace tradebot::market_data::binance
