#include "tradebot/backtest/backtest.hpp"

#include "tradebot/market_data/binance/parser.hpp"
#include "tradebot/replay/event_source.hpp"
#include "tradebot/replay/replay_engine.hpp"
#include "tradebot/util/sha256.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>

namespace tradebot::backtest {

namespace fs = std::filesystem;
using execution::ExecutionReport;
using execution::ReportType;

// --- LatencySpec ------------------------------------------------------------

std::unique_ptr<replay::LatencyModel> LatencySpec::make() const {
    switch (kind) {
        case Kind::zero: return std::make_unique<replay::ZeroLatency>();
        case Kind::constant: return std::make_unique<replay::ConstantLatency>(market_data, order, ack);
        case Kind::jitter:
            return std::make_unique<replay::JitterLatency>(replay::JitterLatency::Params{
                .market_data_base = market_data, .market_data_jitter = jitter,
                .order_base = order, .order_jitter = jitter,
                .ack_base = ack, .ack_jitter = jitter});
    }
    return std::make_unique<replay::ZeroLatency>();
}

// --- BacktestSpec -----------------------------------------------------------

std::string BacktestSpec::describe() const {
    std::string s;
    s += "symbol = " + store.symbol + "\n";
    s += "venue = " + store.venue + "\n";
    s += "data_dir = " + store.root.string() + "\n";
    s += "from = " + from.to_iso8601() + "\n";
    s += "to = " + to.to_iso8601() + "\n";
    s += "initial_cash = " + initial_cash.to_string() + "\n";
    s += "seed = " + std::to_string(seed) + "\n";
    s += "sample_interval = " + std::to_string(sample_interval.count_seconds()) + "s\n";
    s += "tick_size = " + instrument.tick_size.to_string() + "\n";
    s += "lot_size = " + instrument.lot_size.to_string() + "\n";
    s += "min_notional = " + instrument.min_notional.to_string() + "\n";
    s += "maker_fee = " + std::to_string(exchange.fees.maker.numerator) + "/" +
         std::to_string(exchange.fees.maker.denominator) + "\n";
    s += "taker_fee = " + std::to_string(exchange.fees.taker.numerator) + "/" +
         std::to_string(exchange.fees.taker.denominator) + "\n";
    s += "queue_model = " + std::to_string(static_cast<int>(exchange.queue_model)) + "\n";
    s += "fallback_to_trades = " + std::string(exchange.fallback_to_trades ? "true" : "false") +
         " slippage_bps=" + std::to_string(exchange.trade_slippage_bps) + "\n";
    s += "latency = " + std::to_string(static_cast<int>(latency.kind)) + " md=" +
         latency.market_data.to_string() + " order=" + latency.order.to_string() + " ack=" +
         latency.ack.to_string() + " jitter=" + latency.jitter.to_string() + "\n";
    s += "max_position = " + limits.max_position.to_string() + "\n";
    s += "max_order_notional = " + limits.max_order_notional.to_string() + "\n";
    s += "max_drawdown = " + limits.max_drawdown.to_string() + "\n";
    s += "max_daily_loss = " + limits.max_daily_loss.to_string() + "\n";
    for (const auto& st : strategies) {
        s += "strategy = " + st.name + " (" + st.label + ")\n";
        for (const auto& k : st.params.keys()) {
            s += "  " + k + " = " + *st.params.get_string(k) + "\n";
        }
    }
    return s;
}

std::string BacktestSpec::derived_run_id() const {
    std::string label = strategies.empty() ? "empty" : strategies.front().label;
    if (strategies.size() > 1) {
        label += "+" + std::to_string(strategies.size() - 1);
    }
    for (char& c : label) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+')) {
            c = '_';
        }
    }
    return label + "_" + from.to_iso8601().substr(0, 10) + "_" + to.to_iso8601().substr(0, 10) + "_" +
           util::sha256_hex(describe()).substr(0, 8);
}

// --- parsing ----------------------------------------------------------------

Result<BacktestSpec> parse_backtest_spec(const Config& cfg) {
    BacktestSpec spec;
    auto fail = [](const Error& e) { return tl::make_unexpected(e); };

    auto symbol = cfg.get_string_or("backtest.symbol", "ETHUSDT");
    auto venue = cfg.get_string_or("data.venue", "binance");
    auto dir = cfg.get_string_or("data.dir", "data");
    if (!symbol || !venue || !dir) return fail(!symbol ? symbol.error() : !venue ? venue.error() : dir.error());
    spec.store = storage::StorePath{*dir, *venue, *symbol};

    auto from = cfg.get_timestamp("backtest.from");
    auto to = cfg.get_timestamp("backtest.to");
    if (!from || !to) return fail(!from ? from.error() : to.error());
    if (*to <= *from) return make_error(ErrorCode::invalid_argument, "backtest.to must be after backtest.from");
    spec.from = *from;
    spec.to = *to;

    auto cash = cfg.get_notional("backtest.initial_cash");
    if (!cash) return fail(cash.error());
    spec.initial_cash = *cash;
    auto seed = cfg.get_int_or("backtest.seed", 1);
    if (!seed) return fail(seed.error());
    spec.seed = static_cast<std::uint64_t>(*seed);
    auto sample = cfg.get_duration_or("backtest.sample_interval", Duration::minutes(1));
    if (!sample) return fail(sample.error());
    spec.sample_interval = *sample;
    auto tickers = cfg.get_bool_or("backtest.load_tickers", false);
    if (!tickers) return fail(tickers.error());
    spec.load_tickers = *tickers;
    if (auto ivs = cfg.get_string("backtest.stored_candles"); ivs) {
        std::string cur;
        for (char c : *ivs + ',') {
            if (c == ',') {
                if (!cur.empty()) {
                    auto d = market_data::binance::parse_kline_interval(cur);
                    if (!d) return fail(d.error());
                    spec.stored_candle_intervals.push_back(*d);
                }
                cur.clear();
            } else if (c != ' ') {
                cur.push_back(c);
            }
        }
    }

    // Instrument
    spec.instrument.id = InstrumentId{1};
    spec.instrument.venue = VenueId{1};
    spec.instrument.symbol = *symbol;
    auto base = cfg.get_string_or("instrument.base", "ETH");
    auto quote = cfg.get_string_or("instrument.quote", "USDT");
    spec.instrument.base = base.value_or("ETH");
    spec.instrument.quote = quote.value_or("USDT");
    auto tick = cfg.get_price("instrument.tick_size");
    auto lot = cfg.get_quantity("instrument.lot_size");
    auto min_qty = cfg.get_quantity("instrument.min_quantity");
    auto min_ntl = cfg.get_notional("instrument.min_notional");
    spec.instrument.tick_size = tick.value_or(Price::from_raw(1'000'000));  // 0.01
    spec.instrument.lot_size = lot.value_or(Quantity::from_raw(10'000));  // 0.0001
    spec.instrument.min_quantity = min_qty.value_or(Quantity::from_raw(10'000));
    spec.instrument.min_notional = min_ntl.value_or(Notional::from_int(5));
    if (auto v = spec.instrument.validate_definition(); !v) return fail(v.error());

    // Exchange
    auto maker = cfg.get_int_or("exchange.maker_fee_bps", 10);
    auto taker = cfg.get_int_or("exchange.taker_fee_bps", 10);
    if (!maker || !taker) return fail(!maker ? maker.error() : taker.error());
    spec.exchange.fees.maker = execution::FeeRate::bps(*maker);
    spec.exchange.fees.taker = execution::FeeRate::bps(*taker);
    auto qm = cfg.get_string_or("exchange.queue_model", "queue");
    if (!qm) return fail(qm.error());
    if (*qm == "optimistic") spec.exchange.queue_model = execution::QueueModel::optimistic;
    else if (*qm == "queue") spec.exchange.queue_model = execution::QueueModel::queue;
    else if (*qm == "pessimistic") spec.exchange.queue_model = execution::QueueModel::pessimistic;
    else return make_error(ErrorCode::parse_error, "exchange.queue_model must be optimistic|queue|pessimistic");
    auto consume = cfg.get_bool_or("exchange.consume_liquidity", true);
    if (!consume) return fail(consume.error());
    spec.exchange.consume_liquidity = *consume;
    auto fallback = cfg.get_bool_or("exchange.fallback_to_trades", true);
    auto slip = cfg.get_int_or("exchange.trade_slippage_bps", 5);
    if (!fallback || !slip) return fail(!fallback ? fallback.error() : slip.error());
    spec.exchange.fallback_to_trades = *fallback;
    spec.exchange.trade_slippage_bps = *slip;

    auto lat = cfg.get_string_or("exchange.latency", "jitter");
    if (!lat) return fail(lat.error());
    if (*lat == "zero") spec.latency.kind = LatencySpec::Kind::zero;
    else if (*lat == "constant") spec.latency.kind = LatencySpec::Kind::constant;
    else if (*lat == "jitter") spec.latency.kind = LatencySpec::Kind::jitter;
    else return make_error(ErrorCode::parse_error, "exchange.latency must be zero|constant|jitter");
    auto md = cfg.get_duration_or("exchange.latency_market_data", spec.latency.market_data);
    auto od = cfg.get_duration_or("exchange.latency_order", spec.latency.order);
    auto ad = cfg.get_duration_or("exchange.latency_ack", spec.latency.ack);
    auto jd = cfg.get_duration_or("exchange.latency_jitter", spec.latency.jitter);
    if (!md || !od || !ad || !jd) return fail(!md ? md.error() : !od ? od.error() : !ad ? ad.error() : jd.error());
    spec.latency.market_data = *md;
    spec.latency.order = *od;
    spec.latency.ack = *ad;
    spec.latency.jitter = *jd;

    // Risk
    auto get_qty = [&](const char* key, Quantity& out) -> Result<void> {
        if (!cfg.contains(key)) return {};
        auto v = cfg.get_quantity(key);
        if (!v) return tl::make_unexpected(v.error());
        out = *v;
        return {};
    };
    auto get_ntl = [&](const char* key, Notional& out) -> Result<void> {
        if (!cfg.contains(key)) return {};
        auto v = cfg.get_notional(key);
        if (!v) return tl::make_unexpected(v.error());
        out = *v;
        return {};
    };
    if (auto r = get_qty("risk.max_order_quantity", spec.limits.max_order_quantity); !r) return fail(r.error());
    if (auto r = get_ntl("risk.max_order_notional", spec.limits.max_order_notional); !r) return fail(r.error());
    if (auto r = get_qty("risk.max_position", spec.limits.max_position); !r) return fail(r.error());
    if (auto r = get_ntl("risk.max_position_notional", spec.limits.max_position_notional); !r) return fail(r.error());
    if (auto r = get_ntl("risk.max_drawdown", spec.limits.max_drawdown); !r) return fail(r.error());
    if (auto r = get_ntl("risk.max_daily_loss", spec.limits.max_daily_loss); !r) return fail(r.error());
    auto dev = cfg.get_double_or("risk.max_price_deviation", 0.05);
    auto moo = cfg.get_int_or("risk.max_open_orders", 0);
    auto mopm = cfg.get_int_or("risk.max_orders_per_minute", 0);
    auto shrt = cfg.get_bool_or("risk.allow_short", false);
    if (!dev || !moo || !mopm || !shrt) return fail(!dev ? dev.error() : !moo ? moo.error() : !mopm ? mopm.error() : shrt.error());
    spec.limits.max_price_deviation = *dev;
    spec.limits.max_open_orders = static_cast<std::uint32_t>(*moo);
    spec.limits.max_orders_per_minute = static_cast<std::uint32_t>(*mopm);
    spec.limits.allow_short = *shrt;

    // Strategy (single) with its params section.
    auto name = cfg.get_string("strategy.name");
    if (!name) return fail(name.error());
    StrategySpec st;
    st.name = *name;
    st.label = cfg.get_string_or("strategy.label", *name).value_or(*name);
    st.params = cfg.section("strategy.params");
    spec.strategies.push_back(std::move(st));
    spec.run_id = cfg.get_string_or("backtest.run_id", "").value_or("");
    return spec;
}

Result<std::vector<BacktestSpec>> expand_sweep(const BacktestSpec& base, const Config& sweep) {
    std::vector<std::pair<std::string, std::vector<std::string>>> axes;
    for (const auto& key : sweep.keys()) {
        std::vector<std::string> values;
        std::string cur;
        for (char c : *sweep.get_string(key) + ',') {
            if (c == ',') {
                if (!cur.empty()) values.push_back(cur);
                cur.clear();
            } else if (c != ' ') {
                cur.push_back(c);
            }
        }
        if (values.empty()) {
            return make_error(ErrorCode::invalid_argument, "sweep key '" + key + "' has no values");
        }
        axes.emplace_back(key, std::move(values));
    }
    if (base.strategies.empty()) {
        return make_error(ErrorCode::invalid_argument, "sweep needs a strategy");
    }
    std::vector<BacktestSpec> out;
    std::vector<std::size_t> idx(axes.size(), 0);
    for (;;) {
        BacktestSpec s = base;
        std::string suffix;
        for (std::size_t i = 0; i < axes.size(); ++i) {
            const auto& [key, values] = axes[i];
            s.strategies.front().params.set(key, values[idx[i]]);
            suffix += "_" + key + "=" + values[idx[i]];
        }
        s.strategies.front().label = base.strategies.front().label + suffix;
        s.run_id.clear();
        out.push_back(std::move(s));
        // Odometer increment.
        std::size_t k = 0;
        for (; k < axes.size(); ++k) {
            if (++idx[k] < axes[k].second.size()) break;
            idx[k] = 0;
        }
        if (k == axes.size()) break;
        if (axes.empty()) break;
    }
    return out;
}

// --- running ----------------------------------------------------------------

namespace {

class Recorder final : public execution::ExecutionListener {
public:
    Recorder(BacktestResult& result, execution::ExecutionListener& next) : result_(result), next_(next) {}
    void on_execution_report(const ExecutionReport& r) override {
        result_.orders.push_back(OrderRecord{r.time, r.strategy, r.client_id, r.type, r.side, r.order_type,
                                             r.price, r.filled_quantity, r.remaining_quantity, r.reason});
        if (r.type == ReportType::fill && r.fill) {
            result_.fills.push_back(FillRecord{r.time, r.strategy, r.client_id, r.side, r.fill->price,
                                               r.fill->quantity, r.fill->fee, r.fill->liquidity});
        }
        next_.on_execution_report(r);
    }

private:
    BacktestResult& result_;
    execution::ExecutionListener& next_;
};

}  // namespace

Result<BacktestResult> run_backtest(const BacktestSpec& spec_in, const strategy::StrategyRegistry& registry,
                                    Logger log) {
    const auto wall_start = std::chrono::steady_clock::now();
    BacktestResult result;
    result.spec = spec_in;
    if (result.spec.run_id.empty()) {
        result.spec.run_id = result.spec.derived_run_id();
    }
    const BacktestSpec& spec = result.spec;

    replay::StoreSelection sel;
    sel.tickers = spec.load_tickers;
    sel.candle_intervals = spec.stored_candle_intervals;
    auto source = replay::open_store_source(spec.store, sel, spec.from, spec.to);
    if (!source) {
        return tl::make_unexpected(source.error());
    }
    SimClock clock(spec.from);
    auto latency = spec.latency.make();
    replay::ReplayEngine engine(**source, clock, *latency, replay::ReplayOptions{.seed = spec.seed, .end_time = spec.to});

    execution::SimulatedExchange exchange(spec.instrument, engine, *latency, engine.rng(), spec.exchange);
    engine.venue_bus().subscribe(exchange);
    portfolio::Portfolio pf(spec.initial_cash);
    engine.bus().subscribe(pf);
    risk::RiskManager risk(exchange, pf, clock, spec.limits, log.child("risk"));
    strategy::StrategyRunner runner(engine, risk, pf, spec.instrument, log.child("strategy"), spec.seed);
    engine.bus().subscribe(runner);
    Recorder recorder(result, runner);
    risk.set_listener(&recorder);

    for (const auto& st : spec.strategies) {
        auto s = registry.create(st.name);
        if (!s) {
            return tl::make_unexpected(s.error());
        }
        const StrategyId id = runner.add(std::move(*s), st.params, st.label);
        result.strategy_labels.emplace_back(id, st.label);
    }

    engine.schedule_every(spec.sample_interval, [&](Timestamp t) {
        pf.sample(t);
        risk.check_limits();
    });

    runner.start();
    pf.sample(clock.now());
    auto run = engine.run();
    runner.stop();
    if (!run) {
        return tl::make_unexpected(run.error());
    }
    pf.sample(clock.now());

    result.equity_curve = pf.equity_curve();
    result.metrics = runner.metrics();
    BacktestSummary& sum = result.summary;
    sum.initial_cash = spec.initial_cash;
    sum.final_equity = pf.equity();
    sum.total_return = ratio(sum.final_equity - spec.initial_cash, spec.initial_cash);
    sum.realized_pnl_net = pf.realized_pnl_net();
    sum.fees = pf.account().fees;
    Notional peak = spec.initial_cash, max_dd;
    for (const auto& s : result.equity_curve) {
        peak = std::max(peak, s.equity);
        max_dd = std::max(max_dd, peak - s.equity);
    }
    sum.max_drawdown = max_dd;
    sum.max_drawdown_fraction = ratio(max_dd, peak.is_positive() ? peak : spec.initial_cash);
    sum.fills = pf.stats().fills;
    sum.orders = runner.stats().orders_submitted;
    sum.rejected = risk.stats().rejected + exchange.stats().rejected;
    sum.volume = pf.stats().volume;
    sum.turnover = pf.stats().turnover;
    sum.events = engine.stats().events_delivered;
    sum.kill_switch_trips = risk.stats().kill_switch_trips;
    sum.wall_time = Duration::from_chrono(std::chrono::steady_clock::now() - wall_start);
    log.info("run {} done: equity {} -> {} ({:+.2f}%), {} fills, {} orders, max dd {}, {} events in {}",
             spec.run_id, spec.initial_cash.to_string(), sum.final_equity.to_string(),
             sum.total_return * 100.0, sum.fills, sum.orders, sum.max_drawdown.to_string(), sum.events,
             sum.wall_time.to_string());
    return result;
}

// --- artifacts --------------------------------------------------------------

Result<void> write_artifacts(const BacktestResult& r, const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return make_error(ErrorCode::io_error, "cannot create " + dir.string() + ": " + ec.message());
    }
    auto open = [&](const char* name) -> Result<std::ofstream> {
        std::ofstream out(dir / name, std::ios::trunc);
        if (!out) return make_error(ErrorCode::io_error, "cannot write " + (dir / name).string());
        return out;
    };

    {
        auto f = open("config.txt");
        if (!f) return tl::make_unexpected(f.error());
        *f << "run_id = " << r.spec.run_id << "\n" << r.spec.describe();
    }
    {
        auto f = open("equity.csv");
        if (!f) return tl::make_unexpected(f.error());
        *f << "time,equity,cash,realized_pnl_net,unrealized_pnl,fees,position,mark\n";
        for (const auto& s : r.equity_curve) {
            *f << s.time.to_iso8601() << ',' << s.equity.to_string() << ',' << s.cash.to_string() << ','
               << s.realized_pnl.to_string() << ',' << s.unrealized_pnl.to_string() << ','
               << s.fees.to_string() << ',' << s.position.to_string() << ',' << s.mark.to_string() << '\n';
        }
    }
    {
        auto f = open("fills.csv");
        if (!f) return tl::make_unexpected(f.error());
        *f << "time,strategy,client_id,side,price,quantity,fee,liquidity\n";
        for (const auto& x : r.fills) {
            *f << x.time.to_iso8601() << ',' << x.strategy.value() << ',' << x.client_id.value() << ','
               << to_string(x.side) << ',' << x.price.to_string() << ',' << x.quantity.to_string() << ','
               << x.fee.to_string() << ',' << to_string(x.liquidity) << '\n';
        }
    }
    {
        auto f = open("orders.csv");
        if (!f) return tl::make_unexpected(f.error());
        *f << "time,strategy,client_id,event,side,type,price,filled,remaining,reason\n";
        for (const auto& o : r.orders) {
            std::string reason = o.reason;
            for (char& c : reason) if (c == ',' || c == '\n') c = ';';
            *f << o.time.to_iso8601() << ',' << o.strategy.value() << ',' << o.client_id.value() << ','
               << to_string(o.type) << ',' << to_string(o.side) << ',' << to_string(o.order_type) << ','
               << o.price.to_string() << ',' << o.filled.to_string() << ',' << o.remaining.to_string() << ','
               << reason << '\n';
        }
    }
    {
        auto f = open("metrics.csv");
        if (!f) return tl::make_unexpected(f.error());
        *f << "time,strategy,name,value\n";
        for (const auto& m : r.metrics) {
            *f << m.time.to_iso8601() << ',' << m.strategy.value() << ',' << m.name << ',' << m.value << '\n';
        }
    }
    {
        auto f = open("summary.json");
        if (!f) return tl::make_unexpected(f.error());
        const auto& s = r.summary;
        nlohmann::json j;
        j["run_id"] = r.spec.run_id;
        j["symbol"] = r.spec.store.symbol;
        j["from"] = r.spec.from.to_iso8601();
        j["to"] = r.spec.to.to_iso8601();
        j["seed"] = r.spec.seed;
        nlohmann::json strategies = nlohmann::json::array();
        for (const auto& [id, label] : r.strategy_labels) {
            strategies.push_back({{"id", id.value()}, {"label", label}});
        }
        j["strategies"] = strategies;
        j["initial_cash"] = s.initial_cash.to_string();
        j["final_equity"] = s.final_equity.to_string();
        j["total_return"] = s.total_return;
        j["realized_pnl_net"] = s.realized_pnl_net.to_string();
        j["fees"] = s.fees.to_string();
        j["max_drawdown"] = s.max_drawdown.to_string();
        j["max_drawdown_fraction"] = s.max_drawdown_fraction;
        j["fills"] = s.fills;
        j["orders"] = s.orders;
        j["rejected"] = s.rejected;
        j["volume"] = s.volume.to_string();
        j["turnover"] = s.turnover.to_string();
        j["events"] = s.events;
        j["kill_switch_trips"] = s.kill_switch_trips;
        j["wall_time_seconds"] = s.wall_time.as_seconds();
        j["equity_samples"] = r.equity_curve.size();
        *f << j.dump(2) << '\n';
    }
    return {};
}

// --- batch ------------------------------------------------------------------

std::vector<Result<BacktestResult>> run_batch(const std::vector<BacktestSpec>& specs,
                                              const strategy::StrategyRegistry& registry, Logger log,
                                              unsigned threads) {
    std::vector<Result<BacktestResult>> results(specs.size(), tl::make_unexpected(Error{ErrorCode::invalid_state, "not run"}));
    if (threads == 0) {
        threads = 1;
    }
    std::mutex mutex;
    std::size_t next = 0;
    auto worker = [&] {
        for (;;) {
            std::size_t i = 0;
            {
                std::lock_guard lock(mutex);
                if (next >= specs.size()) return;
                i = next++;
            }
            auto r = run_backtest(specs[i], registry, log);
            std::lock_guard lock(mutex);
            results[i] = std::move(r);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::min<unsigned>(threads, static_cast<unsigned>(specs.size())); ++t) {
        pool.emplace_back(worker);
    }
    for (auto& t : pool) {
        t.join();
    }
    return results;
}

}  // namespace tradebot::backtest
