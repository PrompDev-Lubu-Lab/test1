#pragma once

// Paper trading: the strategy stack on live market data with simulated
// fills.
//
// The Binance collector runs on its own thread (archiving raw data as it
// goes) and hands every record to the runtime, which parses it and posts
// the event to the LiveScheduler's dispatch thread. There the simulated
// exchange (venue bus) and the portfolio and strategies (client bus) see
// it, exactly as in a backtest. Equity is sampled on a timer, artifacts
// are flushed periodically in the backtest format, and the portfolio
// state is persisted so a restart resumes positions.

#include "tradebot/backtest/backtest.hpp"
#include "tradebot/core/clock.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/execution/simulated_exchange.hpp"
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

struct PaperSpec {
    std::string label = "paper";
    std::filesystem::path data_dir = "data";  // raw capture archive root
    std::filesystem::path run_dir;  // artifacts; defaults to runs/paper-<label>
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
};

[[nodiscard]] Result<PaperSpec> parse_paper_spec(const Config& cfg);

class PaperRuntime {
public:
    PaperRuntime(PaperSpec spec, const strategy::StrategyRegistry& registry,
                 std::shared_ptr<net::TlsContext> tls, Logger log);
    ~PaperRuntime();

    // Builds the stack, restores state, starts the feed thread and runs the
    // dispatch loop until stop() is called (from a signal handler or another
    // thread). Flushes artifacts and state on the way out.
    [[nodiscard]] Result<void> run();
    void stop() noexcept;

    // Writes equity.csv, fills.csv, orders.csv, metrics.csv, summary.json,
    // state.json. Safe to call from the dispatch thread only.
    [[nodiscard]] Result<void> flush();

    [[nodiscard]] const portfolio::Portfolio* portfolio() const noexcept { return portfolio_.get(); }
    [[nodiscard]] const backtest::BacktestResult& result() const noexcept { return result_; }
    [[nodiscard]] const market_data::binance::CollectorStats* collector_stats() const noexcept;
    [[nodiscard]] const risk::RiskManager* risk() const noexcept { return risk_.get(); }
    [[nodiscard]] const FeedHealthMonitor& feed_health() const noexcept { return health_; }
    [[nodiscard]] const Journal& journal() const noexcept { return journal_; }
    [[nodiscard]] const strategy::StrategyRunner* runner() const noexcept { return runner_.get(); }

private:
    class Recorder;
    void on_record(const market_data::RawRecord& record);
    [[nodiscard]] Result<void> restore_state();
    void check_feed(Timestamp now);

    PaperSpec spec_;
    const strategy::StrategyRegistry& registry_;
    std::shared_ptr<net::TlsContext> tls_;
    Logger log_;
    WallClock clock_;
    LiveScheduler scheduler_;
    std::unique_ptr<replay::LatencyModel> latency_;
    replay::Rng rng_;
    std::unique_ptr<execution::SimulatedExchange> exchange_;
    std::unique_ptr<portfolio::Portfolio> portfolio_;
    std::unique_ptr<risk::RiskManager> risk_;
    std::unique_ptr<strategy::StrategyRunner> runner_;
    std::unique_ptr<Recorder> recorder_;
    std::unique_ptr<market_data::RawCaptureWriter> writer_;
    std::unique_ptr<market_data::binance::Collector> collector_;
    std::unique_ptr<market_data::binance::RawRecordProcessor> processor_;
    std::thread feed_thread_;
    std::atomic<bool> stop_{false};
    backtest::BacktestResult result_;
    Timestamp started_;
    FeedHealthMonitor health_{FeedHealthMonitor::Options{}};
    Journal journal_;
    bool tripped_by_feed_ = false;
};

}  // namespace tradebot::live
