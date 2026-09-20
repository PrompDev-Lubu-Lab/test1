#pragma once

// TradingRuntime: the strategy stack on live market data, in one of four
// modes that differ only in what sits behind the ExecutionVenue interface.
//
//   paper    simulated fills (SimulatedExchange); no credentials needed.
//   shadow   simulated fills, plus the live plumbing is exercised: the
//            venue's clock, balances and user stream are checked and every
//            order the risk gate accepts is logged as "would send". Nothing
//            reaches the real account.
//   testnet  real orders through BinanceGateway against the Binance spot
//            testnet (testnet keys).
//   live     real orders against the real venue. Refuses to start unless
//            the config carries an explicit confirmation and the capital
//            limits in the pre-live checklist are set.
//
// The Binance collector runs on its own thread (archiving raw data as it
// goes) and hands every record to the runtime, which parses it and posts
// the event to the LiveScheduler's dispatch thread. There the venue (venue
// bus) and the portfolio and strategies (client bus) see it, exactly as in
// a backtest. Equity is sampled on a timer, artifacts are flushed
// periodically in the backtest format, and the portfolio state is persisted
// so a restart resumes positions. In testnet/live mode the user data stream
// runs on a third thread, orders that go silent are re-queried, and the
// portfolio is reconciled against venue balances on a timer.

#include "tradebot/backtest/backtest.hpp"
#include "tradebot/core/clock.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/execution/simulated_exchange.hpp"
#include "tradebot/gateway/binance_gateway.hpp"
#include "tradebot/gateway/binance_rest.hpp"
#include "tradebot/gateway/user_stream.hpp"
#include "tradebot/live/feed_health.hpp"
#include "tradebot/live/journal.hpp"
#include "tradebot/live/live_scheduler.hpp"
#include "tradebot/market_data/binance/collector.hpp"
#include "tradebot/market_data/binance/raw_processor.hpp"
#include "tradebot/market_data/raw_capture.hpp"
#include "tradebot/net/tls.hpp"
#include "tradebot/portfolio/portfolio.hpp"
#include "tradebot/risk/risk_manager.hpp"
#include "tradebot/strategy/runner.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

namespace tradebot::live {

enum class TradingMode { paper, shadow, testnet, live };
[[nodiscard]] std::string_view to_string(TradingMode mode) noexcept;
[[nodiscard]] Result<TradingMode> parse_trading_mode(std::string_view text);
[[nodiscard]] constexpr bool sends_real_orders(TradingMode mode) noexcept {
    return mode == TradingMode::testnet || mode == TradingMode::live;
}
[[nodiscard]] constexpr bool needs_credentials(TradingMode mode) noexcept { return mode != TradingMode::paper; }

// The phrase [live] confirm must equal for mode = live.
inline constexpr std::string_view kLiveConfirmation = "I understand this sends real orders with real money";

struct GatewaySpec {
    std::string rest_base = "https://api.binance.com";
    std::string user_stream_base = "wss://stream.binance.com:9443";
    std::optional<net::ProxyConfig> proxy;  // nullopt = environment
    Duration recv_window = Duration::seconds(5);
    Duration query_after = Duration::seconds(5);
    Duration reconcile_interval = Duration::minutes(1);
    Quantity reconcile_base_tolerance;  // zero = must match exactly
    Notional reconcile_quote_tolerance;
    bool trip_on_reconcile_mismatch = true;  // consecutive mismatches trip the kill switch
    std::uint32_t reconcile_mismatches_to_trip = 2;  // one mismatch can be a fill in flight
    bool cancel_unknown_orders = true;  // at start, cancel our (tb*) open orders the runtime does not know
    bool cancel_on_stop = true;  // cancel working orders on shutdown
    Duration max_clock_skew = Duration::seconds(1);
    std::string confirm;  // must equal kLiveConfirmation for mode = live
    Notional max_capital;  // live: initial cash and per-order notional must not exceed this
};

struct RuntimeSpec {
    TradingMode mode = TradingMode::paper;
    std::string label = "paper";
    std::filesystem::path data_dir = "data";  // raw capture archive root
    std::filesystem::path run_dir;  // artifacts; defaults to runs/<mode>-<label>
    market_data::binance::CollectorConfig collector;
    Instrument instrument;  // rules; refreshed from exchange_info when received
    std::vector<backtest::StrategySpec> strategies;
    Notional initial_cash;
    execution::SimulatedExchangeOptions exchange;
    backtest::LatencySpec latency;  // market-data delay is ignored (data is live)
    risk::RiskLimits limits;
    std::uint64_t seed = 1;
    Duration sample_interval = Duration::minutes(1);
    Duration flush_interval = Duration::minutes(1);
    bool archive_raw = true;
    bool resume = true;  // restore portfolio state from run_dir/state.json (or the journal)
    Duration feed_stale_after = Duration::seconds(15);  // trip the kill switch after this silence
    bool auto_rearm = true;  // reset the kill switch when the feed recovers from staleness
    Duration heartbeat_interval = Duration::seconds(5);
    GatewaySpec gateway;
    gateway::BinanceCredentials credentials;  // from the environment; never from the config file
};

// Reads [live] (mode and gateway settings), [paper] (runtime settings; the
// section name is historical), [collector], and the backtest sections.
[[nodiscard]] Result<RuntimeSpec> parse_runtime_spec(const Config& cfg);

// The pre-live checklist that the code itself enforces. Returns every
// violated item; empty means the spec may run in the given mode.
[[nodiscard]] std::vector<std::string> preflight_violations(const RuntimeSpec& spec);

class TradingRuntime {
public:
    TradingRuntime(RuntimeSpec spec, const strategy::StrategyRegistry& registry,
                   std::shared_ptr<net::TlsContext> tls, Logger log);
    ~TradingRuntime();

    // Builds the stack, restores state, starts the feed thread and runs the
    // dispatch loop until stop() is called (from a signal handler or another
    // thread). Flushes artifacts and state on the way out.
    [[nodiscard]] Result<void> run();
    void stop() noexcept;

    // Writes equity.csv, fills.csv, orders.csv, metrics.csv, summary.json,
    // state.json. Safe to call from the dispatch thread only.
    [[nodiscard]] Result<void> flush();

    [[nodiscard]] TradingMode mode() const noexcept { return spec_.mode; }
    [[nodiscard]] const portfolio::Portfolio* portfolio() const noexcept { return portfolio_.get(); }
    [[nodiscard]] const backtest::BacktestResult& result() const noexcept { return result_; }
    [[nodiscard]] const market_data::binance::CollectorStats* collector_stats() const noexcept;
    [[nodiscard]] const risk::RiskManager* risk() const noexcept { return risk_.get(); }
    [[nodiscard]] const FeedHealthMonitor& feed_health() const noexcept { return health_; }
    [[nodiscard]] const Journal& journal() const noexcept { return journal_; }
    [[nodiscard]] const strategy::StrategyRunner* runner() const noexcept { return runner_.get(); }
    [[nodiscard]] const gateway::BinanceGateway* gateway() const noexcept { return gateway_.get(); }
    [[nodiscard]] const gateway::UserStream* user_stream() const noexcept { return user_stream_.get(); }

    struct Stats {
        std::uint64_t shadow_orders = 0;  // shadow mode: orders that would have been sent
        std::uint64_t reconciliations = 0;
        std::uint64_t reconcile_mismatches = 0;
        std::uint64_t startup_cancels = 0;  // unknown open orders cancelled at start
        std::uint64_t shutdown_cancels = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    class Recorder;
    class ShadowVenue;
    void on_record(const market_data::RawRecord& record);
    [[nodiscard]] Result<void> restore_state();
    void check_feed(Timestamp now);
    [[nodiscard]] Result<void> build_venue();
    [[nodiscard]] Result<void> venue_preflight();
    [[nodiscard]] Result<void> adopt_open_orders();
    void on_reconciliation(const gateway::BinanceGateway::Reconciliation& r);
    void shutdown_venue();

    RuntimeSpec spec_;
    const strategy::StrategyRegistry& registry_;
    std::shared_ptr<net::TlsContext> tls_;
    Logger log_;
    WallClock clock_;
    LiveScheduler scheduler_;
    std::unique_ptr<replay::LatencyModel> latency_;
    replay::Rng rng_;
    std::unique_ptr<execution::SimulatedExchange> exchange_;
    std::unique_ptr<ShadowVenue> shadow_;
    std::optional<net::HttpClient> http_;
    std::unique_ptr<gateway::BinanceRestClient> rest_;
    std::unique_ptr<gateway::BinanceGateway> gateway_;
    std::unique_ptr<gateway::UserStream> user_stream_;
    execution::ExecutionVenue* venue_ = nullptr;
    std::unique_ptr<portfolio::Portfolio> portfolio_;
    std::unique_ptr<risk::RiskManager> risk_;
    std::unique_ptr<strategy::StrategyRunner> runner_;
    std::unique_ptr<Recorder> recorder_;
    std::unique_ptr<market_data::RawCaptureWriter> writer_;
    std::unique_ptr<market_data::binance::Collector> collector_;
    std::unique_ptr<market_data::binance::RawRecordProcessor> processor_;
    std::thread feed_thread_;
    std::thread stream_thread_;
    std::atomic<bool> stop_{false};
    backtest::BacktestResult result_;
    Timestamp started_;
    FeedHealthMonitor health_{FeedHealthMonitor::Options{}};
    Journal journal_;
    bool tripped_by_feed_ = false;
    std::uint32_t reconcile_mismatch_streak_ = 0;
    Stats stats_;
};

}  // namespace tradebot::live
