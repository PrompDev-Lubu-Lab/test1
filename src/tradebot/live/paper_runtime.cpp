#include "tradebot/live/paper_runtime.hpp"

#include "tradebot/analytics/analytics.hpp"

#include <fstream>
#include <sstream>

namespace tradebot::live {

namespace fs = std::filesystem;
using execution::ExecutionReport;
using execution::ReportType;

class PaperRuntime::Recorder final : public execution::ExecutionListener {
public:
    Recorder(backtest::BacktestResult& result, execution::ExecutionListener& next, Journal& journal, Logger log)
        : result_(result), next_(next), journal_(journal), log_(std::move(log)) {}
    void on_execution_report(const ExecutionReport& r) override {
        // Journal first: if the process dies after this line, replaying the
        // journal reproduces exactly what the portfolio is about to apply.
        if (auto j = journal_.append(r); !j) {
            log_.error("journal append failed: {}", j.error().to_string());
        }
        result_.orders.push_back(backtest::OrderRecord{r.time, r.strategy, r.client_id, r.type, r.side, r.order_type,
                                                       r.price, r.filled_quantity, r.remaining_quantity, r.reason});
        if (r.type == ReportType::fill && r.fill) {
            result_.fills.push_back(backtest::FillRecord{r.time, r.strategy, r.client_id, r.side, r.fill->price,
                                                         r.fill->quantity, r.fill->fee, r.fill->liquidity});
            log_.info("FILL {} {} @ {} fee {} ({})", to_string(r.side), r.fill->quantity.to_string(),
                      r.fill->price.to_string(), r.fill->fee.to_string(), to_string(r.fill->liquidity));
        } else if (r.type == ReportType::rejected) {
            log_.warn("REJECTED order {}: {}", r.client_id.value(), r.reason);
        }
        next_.on_execution_report(r);
    }

private:
    backtest::BacktestResult& result_;
    execution::ExecutionListener& next_;
    Journal& journal_;
    Logger log_;
};

Result<PaperSpec> parse_paper_spec(const Config& cfg) {
    // Reuse the backtest parser for everything it shares, then the paper
    // and collector sections.
    Config bt = cfg;
    if (!bt.contains("backtest.from")) bt.set("backtest.from", "2000-01-01");
    if (!bt.contains("backtest.to")) bt.set("backtest.to", "2100-01-01");
    if (!bt.contains("backtest.initial_cash") && cfg.contains("paper.initial_cash")) {
        bt.set("backtest.initial_cash", *cfg.get_string("paper.initial_cash"));
    }
    if (!bt.contains("backtest.symbol") && cfg.contains("paper.symbol")) {
        bt.set("backtest.symbol", *cfg.get_string("paper.symbol"));
    }
    auto base = backtest::parse_backtest_spec(bt);
    if (!base) {
        return tl::make_unexpected(base.error());
    }
    PaperSpec spec;
    spec.label = cfg.get_string_or("paper.label", "paper").value_or("paper");
    spec.data_dir = base->store.root;
    spec.run_dir = cfg.get_string_or("paper.run_dir", "").value_or("");
    if (spec.run_dir.empty()) {
        spec.run_dir = fs::path("runs") / ("paper-" + spec.label);
    }
    spec.instrument = base->instrument;
    spec.strategies = base->strategies;
    spec.initial_cash = base->initial_cash;
    spec.exchange = base->exchange;
    spec.latency = base->latency;
    spec.limits = base->limits;
    spec.seed = base->seed;
    spec.sample_interval = base->sample_interval;
    auto flush = cfg.get_duration_or("paper.flush_interval", Duration::minutes(1));
    auto archive = cfg.get_bool_or("paper.archive_raw", true);
    auto resume = cfg.get_bool_or("paper.resume", true);
    if (!flush || !archive || !resume) {
        return tl::make_unexpected(!flush ? flush.error() : !archive ? archive.error() : resume.error());
    }
    spec.flush_interval = *flush;
    spec.archive_raw = *archive;
    spec.resume = *resume;
    auto stale = cfg.get_duration_or("paper.feed_stale_after", Duration::seconds(15));
    auto rearm = cfg.get_bool_or("paper.auto_rearm", true);
    auto hb = cfg.get_duration_or("paper.heartbeat_interval", Duration::seconds(5));
    auto flatten = cfg.get_bool_or("risk.flatten_on_trip", false);
    if (!stale || !rearm || !hb || !flatten) {
        return tl::make_unexpected(!stale ? stale.error() : !rearm ? rearm.error() : !hb ? hb.error() : flatten.error());
    }
    spec.feed_stale_after = *stale;
    spec.auto_rearm = *rearm;
    spec.heartbeat_interval = *hb;
    spec.limits.flatten_on_trip = *flatten;

    auto& cc = spec.collector;
    cc.symbol = base->store.symbol;
    cc.ws_base = cfg.get_string_or("collector.ws_base", cc.ws_base).value_or(cc.ws_base);
    cc.rest_base = cfg.get_string_or("collector.rest_base", cc.rest_base).value_or(cc.rest_base);
    if (auto streams = cfg.get_string("collector.streams"); streams) {
        cc.streams.clear();
        std::string cur;
        for (char c : *streams + ',') {
            if (c == ',') {
                if (!cur.empty()) cc.streams.push_back(cur);
                cur.clear();
            } else if (c != ' ') {
                cur.push_back(c);
            }
        }
    }
    if (auto proxy = cfg.get_string("collector.proxy"); proxy) {
        if (*proxy == "none") {
            cc.proxy = net::ProxyConfig{};
        } else {
            auto url = net::Url::parse(*proxy);
            if (!url) return tl::make_unexpected(url.error());
            cc.proxy = net::ProxyConfig{url->host, url->port};
        }
    }
    cc.stale_timeout = cfg.get_duration_or("collector.stale_timeout", cc.stale_timeout).value_or(cc.stale_timeout);
    cc.snapshot_interval = cfg.get_duration_or("collector.snapshot_interval", cc.snapshot_interval).value_or(cc.snapshot_interval);
    cc.flush_interval = cfg.get_duration_or("collector.flush_interval", cc.flush_interval).value_or(cc.flush_interval);
    return spec;
}

PaperRuntime::PaperRuntime(PaperSpec spec, const strategy::StrategyRegistry& registry,
                           std::shared_ptr<net::TlsContext> tls, Logger log)
    : spec_(std::move(spec)),
      registry_(registry),
      tls_(std::move(tls)),
      log_(std::move(log)),
      scheduler_(clock_),
      latency_(spec_.latency.make()),
      rng_(spec_.seed) {}

PaperRuntime::~PaperRuntime() {
    stop();
    if (feed_thread_.joinable()) {
        feed_thread_.join();
    }
}

Result<void> PaperRuntime::restore_state() {
    const fs::path state_file = spec_.run_dir / "state.json";
    const fs::path journal_file = spec_.run_dir / "journal.jsonl";
    std::ifstream in(state_file);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        auto state = portfolio::Portfolio::state_from_json(ss.str());
        if (state) {
            portfolio_->restore(*state);
            log_.info("restored state.json: cash {} equity {}", portfolio_->cash().to_string(),
                      portfolio_->equity().to_string());
            return {};
        }
        log_.error("state.json unreadable ({}); rebuilding from the journal", state.error().message);
    }
    if (fs::exists(journal_file)) {
        std::uint64_t reports = 0;
        auto skipped = Journal::replay(journal_file, [&](const execution::ExecutionReport& r) {
            portfolio_->on_execution_report(r);
            ++reports;
        });
        if (!skipped) {
            return tl::make_unexpected(skipped.error());
        }
        log_.info("rebuilt state from journal: {} reports ({} corrupt lines skipped), cash {} equity {}", reports,
                  *skipped, portfolio_->cash().to_string(), portfolio_->equity().to_string());
    }
    return {};
}

void PaperRuntime::check_feed(Timestamp now) {
    const bool changed = health_.check(now);
    if (!changed) {
        return;
    }
    if (health_.state() == FeedState::stale) {
        log_.error("feed stale for {}; going safe", health_.silence(now).to_string());
        risk_->trip("market data stale for " + health_.silence(now).to_string());
        tripped_by_feed_ = true;
    } else if (health_.state() == FeedState::healthy && tripped_by_feed_ && risk_->tripped() && spec_.auto_rearm) {
        log_.warn("feed recovered; re-arming the kill switch");
        risk_->reset();
        tripped_by_feed_ = false;
    }
}

const market_data::binance::CollectorStats* PaperRuntime::collector_stats() const noexcept {
    return collector_ ? &collector_->stats() : nullptr;
}

void PaperRuntime::stop() noexcept {
    stop_.store(true);
    scheduler_.stop();
}

void PaperRuntime::on_record(const market_data::RawRecord& record) {
    // Feed thread: parse here (cheap, off the dispatch thread), post the event.
    auto ev = processor_->process(record);
    if (!ev) {
        log_.warn("parse error on {}: {}", record.stream, ev.error().message);
        return;
    }
    if (!ev->has_value()) {
        if (record.stream == "exchange_info" && processor_->instrument()) {
            const Instrument inst = *processor_->instrument();
            scheduler_.post([this, inst] {
                log_.info("instrument rules from exchange: tick {} lot {} min notional {}",
                          inst.tick_size.to_string(), inst.lot_size.to_string(), inst.min_notional.to_string());
                spec_.instrument = inst;
            });
        }
        return;
    }
    scheduler_.post_event(std::move(**ev));
}

Result<void> PaperRuntime::run() {
    started_ = clock_.now();
    result_ = backtest::BacktestResult{};
    result_.spec.run_id = "paper-" + spec_.label;
    result_.spec.store = storage::StorePath{spec_.data_dir, "binance", spec_.collector.symbol};
    result_.spec.instrument = spec_.instrument;
    result_.spec.from = started_;
    result_.spec.strategies = spec_.strategies;
    result_.spec.initial_cash = spec_.initial_cash;
    result_.spec.exchange = spec_.exchange;
    result_.spec.latency = spec_.latency;
    result_.spec.limits = spec_.limits;
    result_.spec.seed = spec_.seed;
    result_.spec.sample_interval = spec_.sample_interval;

    std::error_code ec;
    fs::create_directories(spec_.run_dir, ec);
    if (ec) {
        return make_error(ErrorCode::io_error, "cannot create " + spec_.run_dir.string());
    }

    // --- stack ---------------------------------------------------------------
    exchange_ = std::make_unique<execution::SimulatedExchange>(spec_.instrument, scheduler_, *latency_, rng_,
                                                               spec_.exchange);
    scheduler_.venue_bus().subscribe(*exchange_);
    portfolio_ = std::make_unique<portfolio::Portfolio>(spec_.initial_cash);
    scheduler_.bus().subscribe(*portfolio_);
    scheduler_.bus().subscribe(health_);
    health_ = FeedHealthMonitor(FeedHealthMonitor::Options{.stale_after = spec_.feed_stale_after});
    if (spec_.resume) {
        if (auto r = restore_state(); !r) {
            return r;
        }
    }
    if (auto j = journal_.open(spec_.run_dir / "journal.jsonl"); !j) {
        return j;
    }
    risk_ = std::make_unique<risk::RiskManager>(*exchange_, *portfolio_, clock_, spec_.limits, log_.child("risk"));
    runner_ = std::make_unique<strategy::StrategyRunner>(scheduler_, *risk_, *portfolio_, spec_.instrument,
                                                         log_.child("strategy"), spec_.seed);
    scheduler_.bus().subscribe(*runner_);
    recorder_ = std::make_unique<Recorder>(result_, *runner_, journal_, log_.child("exec"));
    risk_->set_listener(recorder_.get());
    for (const auto& st : spec_.strategies) {
        auto s = registry_.create(st.name);
        if (!s) {
            return tl::make_unexpected(s.error());
        }
        const StrategyId id = runner_->add(std::move(*s), st.params, st.label);
        result_.strategy_labels.emplace_back(id, st.label);
    }

    // --- feed ----------------------------------------------------------------
    processor_ = std::make_unique<market_data::binance::RawRecordProcessor>(spec_.collector.symbol, spec_.instrument.id,
                                                                            VenueId{1});
    if (spec_.archive_raw) {
        writer_ = std::make_unique<market_data::RawCaptureWriter>(
            market_data::RawCapturePath{spec_.data_dir, "binance", spec_.collector.symbol});
    }
    collector_ = std::make_unique<market_data::binance::Collector>(
        spec_.collector, writer_.get(), [this](const market_data::RawRecord& r) { on_record(r); }, tls_, clock_,
        log_.child("feed"));
    stop_.store(false);
    feed_thread_ = std::thread([this] {
        auto r = collector_->run(stop_);
        if (!r) {
            log_.error("feed stopped: {}", r.error().to_string());
            scheduler_.post([this] { risk_->trip("market data feed failed"); });
        }
    });

    // --- timers ----------------------------------------------------------------
    scheduler_.schedule_every(spec_.sample_interval, [this](Timestamp t) {
        portfolio_->sample(t);
        risk_->check_limits();
    });
    scheduler_.schedule_every(spec_.flush_interval, [this](Timestamp) {
        if (auto f = flush(); !f) {
            log_.error("flush failed: {}", f.error().to_string());
        }
    });
    scheduler_.schedule_every(Duration::seconds(1), [this](Timestamp t) { check_feed(t); });
    scheduler_.schedule_every(spec_.heartbeat_interval, [this](Timestamp t) {
        const std::string status = std::string(to_string(health_.state())) + (risk_->tripped() ? " tripped" : " armed");
        if (auto h = write_heartbeat(spec_.run_dir / "heartbeat", t, status); !h) {
            log_.error("heartbeat failed: {}", h.error().to_string());
        }
    });
    scheduler_.schedule_every(Duration::minutes(1), [this](Timestamp) {
        const auto& cs = collector_->stats();
        log_.info("status: equity {} position {} msgs {} reconnects {} gaps {} book {}", portfolio_->equity().to_string(),
                  portfolio_->position(spec_.instrument.id).to_string(), cs.messages, cs.reconnects, cs.depth_gaps,
                  exchange_->book().empty() ? "empty" : "ok");
    });

    runner_->start();
    portfolio_->sample(clock_.now());
    static_cast<void>(write_heartbeat(spec_.run_dir / "heartbeat", clock_.now(), "starting armed"));
    log_.info("paper trading {} started; artifacts in {}", spec_.label, spec_.run_dir.string());
    scheduler_.run();

    // --- shutdown --------------------------------------------------------------
    stop_.store(true);
    if (feed_thread_.joinable()) {
        feed_thread_.join();
    }
    runner_->stop();
    portfolio_->sample(clock_.now());
    auto f = flush();
    if (writer_) {
        static_cast<void>(writer_->close());
    }
    static_cast<void>(journal_.close());
    static_cast<void>(write_heartbeat(spec_.run_dir / "heartbeat", clock_.now(), "stopped"));
    log_.info("paper trading stopped: equity {} after {} fills", portfolio_->equity().to_string(), result_.fills.size());
    return f;
}

Result<void> PaperRuntime::flush() {
    result_.spec.to = clock_.now();
    result_.equity_curve = portfolio_->equity_curve();
    result_.metrics = runner_->metrics();
    auto& sum = result_.summary;
    sum.initial_cash = spec_.initial_cash;
    sum.final_equity = portfolio_->equity();
    sum.total_return = spec_.initial_cash.is_positive() ? ratio(sum.final_equity - spec_.initial_cash, spec_.initial_cash) : 0.0;
    sum.realized_pnl_net = portfolio_->realized_pnl_net();
    sum.fees = portfolio_->account().fees;
    sum.max_drawdown = portfolio_->drawdown();
    sum.fills = portfolio_->stats().fills;
    sum.orders = runner_->stats().orders_submitted;
    sum.rejected = risk_->stats().rejected + exchange_->stats().rejected;
    sum.volume = portfolio_->stats().volume;
    sum.turnover = portfolio_->stats().turnover;
    sum.events = scheduler_.stats().events;
    sum.kill_switch_trips = risk_->stats().kill_switch_trips;
    sum.wall_time = clock_.now() - started_;
    if (auto w = backtest::write_artifacts(result_, spec_.run_dir); !w) {
        return w;
    }
    if (result_.equity_curve.size() >= 2) {
        static_cast<void>(analytics::write_report(analytics::analyze(result_), spec_.run_dir));
    }
    std::ofstream out(spec_.run_dir / "state.json.tmp", std::ios::trunc);
    if (!out) {
        return make_error(ErrorCode::io_error, "cannot write state.json");
    }
    out << portfolio_->state_to_json() << '\n';
    out.close();
    std::error_code ec;
    fs::rename(spec_.run_dir / "state.json.tmp", spec_.run_dir / "state.json", ec);
    if (ec) {
        return make_error(ErrorCode::io_error, "cannot replace state.json: " + ec.message());
    }
    return {};
}

}  // namespace tradebot::live
