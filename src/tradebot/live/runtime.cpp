#include "tradebot/live/runtime.hpp"

#include "tradebot/analytics/analytics.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace tradebot::live {

namespace fs = std::filesystem;
using execution::ExecutionReport;
using execution::OrderRequest;
using execution::ReportType;

// --- modes ---------------------------------------------------------------------

std::string_view to_string(TradingMode mode) noexcept {
    switch (mode) {
        case TradingMode::paper: return "paper";
        case TradingMode::shadow: return "shadow";
        case TradingMode::testnet: return "testnet";
        case TradingMode::live: return "live";
    }
    return "?";
}

Result<TradingMode> parse_trading_mode(std::string_view text) {
    if (text == "paper") return TradingMode::paper;
    if (text == "shadow") return TradingMode::shadow;
    if (text == "testnet") return TradingMode::testnet;
    if (text == "live") return TradingMode::live;
    return make_error(ErrorCode::invalid_argument, "unknown trading mode '" + std::string(text) +
                                                       "' (paper, shadow, testnet, live)");
}

// --- listeners and wrappers ------------------------------------------------------

class TradingRuntime::Recorder final : public execution::ExecutionListener {
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

// Shadow mode: fills come from the simulated exchange; every order that
// the risk gate lets through is logged as what would have been sent.
class TradingRuntime::ShadowVenue final : public execution::ExecutionVenue {
public:
    ShadowVenue(execution::ExecutionVenue& inner, Stats& stats, Logger log)
        : inner_(inner), stats_(stats), log_(std::move(log)) {}
    void set_listener(execution::ExecutionListener* listener) override { inner_.set_listener(listener); }
    [[nodiscard]] Result<void> submit(const OrderRequest& r) override {
        ++stats_.shadow_orders;
        log_.info("SHADOW would send {} {} {} @ {} (client {})", to_string(r.side), r.quantity.to_string(),
                  to_string(r.type), r.price.to_string(), r.client_id.value());
        return inner_.submit(r);
    }
    [[nodiscard]] Result<void> cancel(ClientOrderId id) override {
        log_.info("SHADOW would cancel client {}", id.value());
        return inner_.cancel(id);
    }
    [[nodiscard]] std::optional<execution::OrderState> order(ClientOrderId id) const override {
        return inner_.order(id);
    }

private:
    execution::ExecutionVenue& inner_;
    Stats& stats_;
    Logger log_;
};

// --- spec ----------------------------------------------------------------------

namespace {

Result<std::optional<net::ProxyConfig>> parse_proxy(const Config& cfg, std::string_view key) {
    auto proxy = cfg.get_string(key);
    if (!proxy) return std::optional<net::ProxyConfig>{};
    if (*proxy == "none") return std::optional<net::ProxyConfig>{net::ProxyConfig{}};
    auto url = net::Url::parse(*proxy);
    if (!url) return tl::make_unexpected(url.error());
    return std::optional<net::ProxyConfig>{net::ProxyConfig{url->host, url->port}};
}

}  // namespace

Result<RuntimeSpec> parse_runtime_spec(const Config& cfg) {
    // Reuse the backtest parser for everything it shares, then the paper,
    // live and collector sections.
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
    RuntimeSpec spec;
    auto mode = parse_trading_mode(cfg.get_string_or("live.mode", "paper").value_or("paper"));
    if (!mode) return tl::make_unexpected(mode.error());
    spec.mode = *mode;
    spec.label = cfg.get_string_or("paper.label", "paper").value_or("paper");
    spec.data_dir = base->store.root;
    spec.run_dir = cfg.get_string_or("paper.run_dir", "").value_or("");
    if (spec.run_dir.empty()) {
        spec.run_dir = fs::path("runs") / (std::string(to_string(spec.mode)) + "-" + spec.label);
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
    auto cproxy = parse_proxy(cfg, "collector.proxy");
    if (!cproxy) return tl::make_unexpected(cproxy.error());
    cc.proxy = *cproxy;
    cc.stale_timeout = cfg.get_duration_or("collector.stale_timeout", cc.stale_timeout).value_or(cc.stale_timeout);
    cc.snapshot_interval = cfg.get_duration_or("collector.snapshot_interval", cc.snapshot_interval).value_or(cc.snapshot_interval);
    cc.flush_interval = cfg.get_duration_or("collector.flush_interval", cc.flush_interval).value_or(cc.flush_interval);

    // --- [live] -------------------------------------------------------------------
    auto& g = spec.gateway;
    if (spec.mode == TradingMode::testnet) {
        g.rest_base = "https://testnet.binance.vision";
        g.user_stream_base = "wss://stream.testnet.binance.vision";
    }
    g.rest_base = cfg.get_string_or("live.rest_base", g.rest_base).value_or(g.rest_base);
    g.user_stream_base = cfg.get_string_or("live.user_stream_base", g.user_stream_base).value_or(g.user_stream_base);
    auto gproxy = parse_proxy(cfg, "live.proxy");
    if (!gproxy) return tl::make_unexpected(gproxy.error());
    g.proxy = *gproxy;
    auto recv = cfg.get_duration_or("live.recv_window", g.recv_window);
    auto query_after = cfg.get_duration_or("live.query_after", g.query_after);
    auto reconcile = cfg.get_duration_or("live.reconcile_interval", g.reconcile_interval);
    auto skew = cfg.get_duration_or("live.max_clock_skew", g.max_clock_skew);
    auto trip = cfg.get_bool_or("live.trip_on_reconcile_mismatch", g.trip_on_reconcile_mismatch);
    auto cancel_unknown = cfg.get_bool_or("live.cancel_unknown_orders", g.cancel_unknown_orders);
    auto cancel_stop = cfg.get_bool_or("live.cancel_on_stop", g.cancel_on_stop);
    for (const Error* e : {recv ? nullptr : &recv.error(), query_after ? nullptr : &query_after.error(),
                           reconcile ? nullptr : &reconcile.error(), skew ? nullptr : &skew.error(),
                           trip ? nullptr : &trip.error(), cancel_unknown ? nullptr : &cancel_unknown.error(),
                           cancel_stop ? nullptr : &cancel_stop.error()}) {
        if (e != nullptr) return tl::make_unexpected(*e);
    }
    g.recv_window = *recv;
    g.query_after = *query_after;
    g.reconcile_interval = *reconcile;
    g.max_clock_skew = *skew;
    g.trip_on_reconcile_mismatch = *trip;
    g.cancel_unknown_orders = *cancel_unknown;
    g.cancel_on_stop = *cancel_stop;
    auto streak = cfg.get_int_or("live.reconcile_mismatches_to_trip", g.reconcile_mismatches_to_trip);
    if (!streak) return tl::make_unexpected(streak.error());
    if (*streak < 1) return make_error(ErrorCode::invalid_argument, "live.reconcile_mismatches_to_trip must be >= 1");
    g.reconcile_mismatches_to_trip = static_cast<std::uint32_t>(*streak);
    if (cfg.contains("live.reconcile_base_tolerance")) {
        auto q = cfg.get_quantity("live.reconcile_base_tolerance");
        if (!q) return tl::make_unexpected(q.error());
        g.reconcile_base_tolerance = *q;
    }
    if (cfg.contains("live.reconcile_quote_tolerance")) {
        auto n = cfg.get_notional("live.reconcile_quote_tolerance");
        if (!n) return tl::make_unexpected(n.error());
        g.reconcile_quote_tolerance = *n;
    }
    if (cfg.contains("live.max_capital")) {
        auto n = cfg.get_notional("live.max_capital");
        if (!n) return tl::make_unexpected(n.error());
        g.max_capital = *n;
    }
    g.confirm = cfg.get_string_or("live.confirm", "").value_or("");
    spec.credentials = gateway::credentials_from_env();
    return spec;
}

std::vector<std::string> preflight_violations(const RuntimeSpec& spec) {
    std::vector<std::string> v;
    // (push_back of a std::string rather than emplace_back of a literal: GCC
    // at -O3 flags the latter with a spurious null-dereference warning.)
    auto add = [&v](std::string item) { v.push_back(std::move(item)); };
    if (spec.strategies.empty()) add("no strategy configured");
    if (needs_credentials(spec.mode) && !spec.credentials.present()) {
        add("credentials missing: set TRADEBOT_BINANCE_API_KEY and TRADEBOT_BINANCE_API_SECRET");
    }
    if (spec.mode == TradingMode::testnet && spec.gateway.rest_base.find("api.binance.com") != std::string::npos) {
        add("testnet mode points at the production venue (live.rest_base)");
    }
    if (spec.mode != TradingMode::live) return v;
    const auto& g = spec.gateway;
    const auto& l = spec.limits;
    if (g.confirm != kLiveConfirmation) {
        add(std::string("live.confirm must read exactly: ") + std::string(kLiveConfirmation));
    }
    if (g.rest_base.rfind("https://", 0) != 0) add("live.rest_base must be https");
    if (!g.max_capital.is_positive()) {
        add("live.max_capital must be set: the most this deployment may ever control");
    } else {
        if (spec.initial_cash > g.max_capital) add("initial_cash exceeds live.max_capital");
        if (l.max_order_notional > g.max_capital) add("risk.max_order_notional exceeds live.max_capital");
        if (l.max_daily_loss > g.max_capital) add("risk.max_daily_loss exceeds live.max_capital");
    }
    if (!l.max_order_notional.is_positive()) add("risk.max_order_notional must be set");
    if (!l.max_position.is_positive() && !l.max_position_notional.is_positive()) {
        add("risk.max_position or risk.max_position_notional must be set");
    }
    if (!l.max_drawdown.is_positive()) add("risk.max_drawdown must be set");
    if (!l.max_daily_loss.is_positive()) add("risk.max_daily_loss must be set");
    if (l.max_orders_per_minute == 0) add("risk.max_orders_per_minute must be set");
    if (l.allow_short) add("risk.allow_short is not supported on spot");
    if (!spec.resume) add("paper.resume must be true in live mode (a restart must not forget positions)");
    return v;
}

// --- runtime -------------------------------------------------------------------

TradingRuntime::TradingRuntime(RuntimeSpec spec, const strategy::StrategyRegistry& registry,
                               std::shared_ptr<net::TlsContext> tls, Logger log)
    : spec_(std::move(spec)),
      registry_(registry),
      tls_(std::move(tls)),
      log_(std::move(log)),
      scheduler_(clock_),
      latency_(spec_.latency.make()),
      rng_(spec_.seed) {}

TradingRuntime::~TradingRuntime() {
    stop();
    if (feed_thread_.joinable()) feed_thread_.join();
    if (stream_thread_.joinable()) stream_thread_.join();
    if (gateway_) gateway_->stop();
}

Result<void> TradingRuntime::restore_state() {
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

void TradingRuntime::check_feed(Timestamp now) {
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

const market_data::binance::CollectorStats* TradingRuntime::collector_stats() const noexcept {
    return collector_ ? &collector_->stats() : nullptr;
}

void TradingRuntime::stop() noexcept {
    stop_.store(true);
    scheduler_.stop();
}

void TradingRuntime::on_record(const market_data::RawRecord& record) {
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

// --- venue selection ---------------------------------------------------------------

Result<void> TradingRuntime::build_venue() {
    if (!sends_real_orders(spec_.mode)) {
        exchange_ = std::make_unique<execution::SimulatedExchange>(spec_.instrument, scheduler_, *latency_, rng_,
                                                                   spec_.exchange);
        scheduler_.venue_bus().subscribe(*exchange_);
        venue_ = exchange_.get();
        if (spec_.mode == TradingMode::shadow) {
            shadow_ = std::make_unique<ShadowVenue>(*exchange_, stats_, log_.child("shadow"));
            venue_ = shadow_.get();
        }
    }
    if (!needs_credentials(spec_.mode)) {
        return {};
    }
    auto http = net::HttpClient::create(tls_, {.timeout = Duration::seconds(10), .proxy = spec_.gateway.proxy});
    if (!http) {
        return tl::make_unexpected(http.error());
    }
    http_.emplace(std::move(*http));
    rest_ = std::make_unique<gateway::BinanceRestClient>(
        *http_, spec_.credentials, clock_,
        gateway::BinanceRestOptions{.base_url = spec_.gateway.rest_base,
                                    .recv_window_ms = spec_.gateway.recv_window.count_millis(),
                                    .timeout = Duration::seconds(10),
                                    .proxy = spec_.gateway.proxy});
    if (sends_real_orders(spec_.mode)) {
        gateway_ = std::make_unique<gateway::BinanceGateway>(
            *rest_, spec_.instrument, clock_, [this](std::function<void()> fn) { scheduler_.post(std::move(fn)); },
            gateway::GatewayOptions{.dry_run = false, .query_after = spec_.gateway.query_after},
            log_.child("gateway"));
        venue_ = gateway_.get();
    }
    return {};
}

Result<void> TradingRuntime::venue_preflight() {
    // 1. Clock skew: signed requests are rejected outside recvWindow.
    auto server_ms = rest_->server_time_ms();
    if (!server_ms) {
        return make_error(server_ms.error().code, "venue unreachable: " + server_ms.error().message);
    }
    const std::int64_t skew_ms = clock_.now().millis_since_epoch() - *server_ms;
    log_.info("venue {} reachable; clock skew {} ms", spec_.gateway.rest_base, skew_ms);
    if (std::abs(skew_ms) > spec_.gateway.max_clock_skew.count_millis()) {
        return make_error(ErrorCode::invalid_state, "clock skew " + std::to_string(skew_ms) + " ms exceeds " +
                                                        spec_.gateway.max_clock_skew.to_string() + "; fix NTP");
    }
    // 2. Credentials work and the account is readable.
    auto balances = rest_->balances();
    if (!balances) {
        return make_error(balances.error().code, "cannot read account: " + balances.error().message);
    }
    for (const auto& b : *balances) {
        if (b.asset == spec_.instrument.base || b.asset == spec_.instrument.quote) {
            log_.info("venue balance {}: free {} locked {}", b.asset, b.free.to_string(), b.locked.to_string());
        }
    }
    if (!sends_real_orders(spec_.mode)) {
        return {};
    }
    // 3. Orders on the venue this process does not know about.
    if (auto a = adopt_open_orders(); !a) {
        return a;
    }
    // 4. The portfolio's view must match the venue before trading starts.
    auto rec = gateway_->reconcile(*portfolio_, spec_.gateway.reconcile_base_tolerance,
                                   spec_.gateway.reconcile_quote_tolerance);
    if (!rec) {
        return tl::make_unexpected(rec.error());
    }
    ++stats_.reconciliations;
    if (!rec->within_tolerance) {
        ++stats_.reconcile_mismatches;
        return make_error(ErrorCode::invalid_state,
                          "startup reconciliation mismatch: venue holds " + rec->venue_base.to_string() + " " +
                              spec_.instrument.base + " / " + rec->venue_quote.to_string() + " " +
                              spec_.instrument.quote + ", portfolio expects " + rec->expected_base.to_string() + " / " +
                              rec->expected_quote.to_string() +
                              "; set initial_cash to the account balance, fix state.json, or raise the tolerances");
    }
    log_.info("startup reconciliation ok: {} {} / {} {}", rec->venue_base.to_string(), spec_.instrument.base,
              rec->venue_quote.to_string(), spec_.instrument.quote);
    return {};
}

Result<void> TradingRuntime::adopt_open_orders() {
    auto open = rest_->open_orders(spec_.instrument.symbol);
    if (!open) {
        return make_error(open.error().code, "cannot list open orders: " + open.error().message);
    }
    if (!open->is_array()) {
        return make_error(ErrorCode::protocol_error, "openOrders response is not an array");
    }
    for (const auto& o : *open) {
        const std::string cid = o.value("clientOrderId", "");
        auto ours = gateway::BinanceRestClient::parse_client_id(cid);
        if (!ours) {
            log_.warn("foreign open order {} on {} (client id '{}'); leaving it alone", o.value("orderId", 0LL),
                      spec_.instrument.symbol, cid);
            continue;
        }
        if (!spec_.gateway.cancel_unknown_orders) {
            log_.warn("open order {} from a previous run left working (live.cancel_unknown_orders = false)", cid);
            continue;
        }
        log_.warn("cancelling open order {} left by a previous run", cid);
        auto c = rest_->cancel_order(spec_.instrument.symbol, *ours);
        if (!c && !gateway::is_unknown_order(c.error())) {
            return make_error(c.error().code, "cannot cancel stale order " + cid + ": " + c.error().message);
        }
        ++stats_.startup_cancels;
    }
    return {};
}

void TradingRuntime::on_reconciliation(const gateway::BinanceGateway::Reconciliation& r) {
    ++stats_.reconciliations;
    if (r.within_tolerance) {
        reconcile_mismatch_streak_ = 0;
        log_.debug("reconciliation ok: base {} quote {}", r.venue_base.to_string(), r.venue_quote.to_string());
        return;
    }
    ++stats_.reconcile_mismatches;
    ++reconcile_mismatch_streak_;
    if (spec_.gateway.trip_on_reconcile_mismatch && !risk_->tripped() &&
        reconcile_mismatch_streak_ >= spec_.gateway.reconcile_mismatches_to_trip) {
        risk_->trip("reconciliation mismatch: base off by " + r.base_difference.to_string() + ", quote off by " +
                    r.quote_difference.to_string());
    }
}

void TradingRuntime::shutdown_venue() {
    if (!gateway_) {
        return;
    }
    // Nothing new leaves the process from here on (strategies may still react
    // to the cancels and to the candles closed by the runner's stop).
    risk_->halt("shutting down");
    if (spec_.gateway.cancel_on_stop) {
        const auto open = gateway_->open_order_ids();
        if (!open.empty()) {
            log_.warn("cancelling {} working order(s) on shutdown", open.size());
            risk_->cancel_all_open();
            stats_.shutdown_cancels += open.size();
        }
    }
    // Let in-flight REST replies land so the journal and state are complete.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        scheduler_.run_once(Duration::millis(10));
        const bool quiet = gateway_->pending_jobs() == 0 &&
                           (!spec_.gateway.cancel_on_stop || gateway_->open_order_ids().empty());
        if (quiet) break;
    }
    for (ClientOrderId id : gateway_->open_order_ids()) {
        log_.error("order {} still working at shutdown; it stays on the venue", id.value());
    }
    if (stream_thread_.joinable()) {
        stream_thread_.join();
    }
    gateway_->stop();
}

// --- main loop ---------------------------------------------------------------------

Result<void> TradingRuntime::run() {
    if (auto v = preflight_violations(spec_); !v.empty()) {
        std::string msg = "refusing to start in " + std::string(to_string(spec_.mode)) + " mode:";
        for (const auto& item : v) msg += "\n  - " + item;
        return make_error(ErrorCode::invalid_state, msg);
    }
    started_ = clock_.now();
    result_ = backtest::BacktestResult{};
    result_.spec.run_id = std::string(to_string(spec_.mode)) + "-" + spec_.label;
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
    if (auto b = build_venue(); !b) {
        return b;
    }
    portfolio_ = std::make_unique<portfolio::Portfolio>(spec_.initial_cash);
    scheduler_.bus().subscribe(*portfolio_);
    scheduler_.bus().subscribe(health_);
    health_ = FeedHealthMonitor(FeedHealthMonitor::Options{.stale_after = spec_.feed_stale_after});
    if (spec_.resume) {
        if (auto r = restore_state(); !r) {
            return r;
        }
    }
    if (rest_) {
        if (auto p = venue_preflight(); !p) {
            log_.error("{}", p.error().message);
            return p;
        }
    }
    if (auto j = journal_.open(spec_.run_dir / "journal.jsonl"); !j) {
        return j;
    }
    risk_ = std::make_unique<risk::RiskManager>(*venue_, *portfolio_, clock_, spec_.limits, log_.child("risk"));
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

    // --- user data stream (shadow verifies the plumbing; testnet/live consume it) ---
    if (rest_) {
        gateway::UserStreamOptions uso;
        uso.ws_base = spec_.gateway.user_stream_base;
        uso.proxy = spec_.gateway.proxy;
        user_stream_ = std::make_unique<gateway::UserStream>(
            *rest_, tls_, spec_.instrument.id, uso,
            [this](const gateway::ParsedExecution& exec) {
                if (gateway_) {
                    gateway_->on_stream_execution(exec);
                } else {
                    log_.info("SHADOW account event ignored: {} order {}", exec.execution_type,
                              exec.report.client_id.value());
                }
            },
            log_.child("stream"));
        stream_thread_ = std::thread([this] {
            auto r = user_stream_->run(stop_);
            if (!r) {
                log_.error("user stream stopped: {}", r.error().to_string());
            }
        });
    }

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
        const std::string status = std::string(to_string(spec_.mode)) + " " + std::string(to_string(health_.state())) +
                                   (risk_->tripped() ? " tripped" : " armed");
        if (auto h = write_heartbeat(spec_.run_dir / "heartbeat", t, status); !h) {
            log_.error("heartbeat failed: {}", h.error().to_string());
        }
    });
    scheduler_.schedule_every(Duration::minutes(1), [this](Timestamp) {
        const auto& cs = collector_->stats();
        log_.info("status: {} equity {} position {} msgs {} reconnects {} gaps {} open orders {}", to_string(spec_.mode),
                  portfolio_->equity().to_string(), portfolio_->position(spec_.instrument.id).to_string(), cs.messages,
                  cs.reconnects, cs.depth_gaps, risk_->open_orders());
    });
    if (gateway_) {
        scheduler_.schedule_every(Duration::seconds(1), [this](Timestamp) { gateway_->poll_silent_orders(); });
        scheduler_.schedule_every(spec_.gateway.reconcile_interval, [this](Timestamp) {
            gateway_->reconcile_async(*portfolio_, spec_.gateway.reconcile_base_tolerance,
                                      spec_.gateway.reconcile_quote_tolerance,
                                      [this](Result<gateway::BinanceGateway::Reconciliation> r) {
                                          if (!r) {
                                              log_.warn("reconciliation failed: {}", r.error().to_string());
                                              return;
                                          }
                                          on_reconciliation(*r);
                                      });
        });
    }

    runner_->start();
    portfolio_->sample(clock_.now());
    static_cast<void>(write_heartbeat(spec_.run_dir / "heartbeat", clock_.now(), "starting armed"));
    log_.info("{} trading {} started; artifacts in {}", to_string(spec_.mode), spec_.label, spec_.run_dir.string());
    scheduler_.run();

    // --- shutdown --------------------------------------------------------------
    stop_.store(true);
    if (feed_thread_.joinable()) {
        feed_thread_.join();
    }
    shutdown_venue();
    if (stream_thread_.joinable()) {
        stream_thread_.join();
    }
    runner_->stop();
    portfolio_->sample(clock_.now());
    auto f = flush();
    if (writer_) {
        static_cast<void>(writer_->close());
    }
    static_cast<void>(journal_.close());
    static_cast<void>(write_heartbeat(spec_.run_dir / "heartbeat", clock_.now(), "stopped"));
    log_.info("{} trading stopped: equity {} after {} fills", to_string(spec_.mode), portfolio_->equity().to_string(),
              result_.fills.size());
    return f;
}

Result<void> TradingRuntime::flush() {
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
    sum.rejected = risk_->stats().rejected + (exchange_ ? exchange_->stats().rejected : 0);
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
