#include "tradebot/strategy/runner.hpp"

#include <algorithm>

namespace tradebot::strategy {

using execution::ExecutionReport;
using execution::OrderRequest;

class StrategyRunner::Context final : public StrategyContext {
public:
    Context(StrategyRunner& runner, Slot& slot, Config params, Logger log, std::uint64_t seed)
        : runner_(runner), slot_(slot), params_(std::move(params)), log_(std::move(log)), rng_(seed) {}

    [[nodiscard]] StrategyId id() const noexcept override { return slot_.id; }
    [[nodiscard]] const Config& params() const noexcept override { return params_; }
    [[nodiscard]] Logger& log() noexcept override { return log_; }
    [[nodiscard]] replay::Rng& rng() noexcept override { return rng_; }

    [[nodiscard]] Timestamp now() const noexcept override { return runner_.scheduler_.now(); }
    TimerId schedule_every(Duration interval, TimerCallback cb) override {
        return runner_.scheduler_.schedule_every(interval, std::move(cb));
    }
    TimerId schedule_at(Timestamp at, TimerCallback cb) override {
        return runner_.scheduler_.schedule_at(at, std::move(cb));
    }
    void cancel_timer(TimerId id) override { runner_.scheduler_.cancel(id); }

    [[nodiscard]] const Instrument& instrument() const noexcept override { return runner_.instrument_; }
    [[nodiscard]] const market_data::OrderBook& book() const noexcept override { return runner_.sync_.book(); }
    [[nodiscard]] bool book_synced() const noexcept override { return runner_.sync_.synced(); }
    [[nodiscard]] std::optional<Price> last_trade_price() const noexcept override { return runner_.last_trade_; }
    [[nodiscard]] std::optional<Price> mark() const noexcept override {
        return runner_.portfolio_.mark(runner_.instrument_.id);
    }
    void request_candles(Duration interval) override {
        Strategy* strategy = slot_.strategy.get();
        slot_.aggregators.push_back(std::make_unique<market_data::CandleAggregator>(
            runner_.instrument_.id,
            market_data::CandleAggregator::Options{.intervals = {interval}, .fill_gaps = true},
            [strategy](const market_data::Candle& c) { strategy->on_candle(c); }));
    }

    [[nodiscard]] Quantity position() const override {
        return runner_.portfolio_.position(slot_.id, runner_.instrument_.id);
    }
    [[nodiscard]] Notional cash() const override { return runner_.portfolio_.cash(); }
    [[nodiscard]] Notional equity() const override { return runner_.portfolio_.equity(slot_.id); }
    [[nodiscard]] std::vector<execution::OrderState> open_orders() const override {
        std::vector<execution::OrderState> out;
        for (const auto& [cid, owner] : runner_.order_owner_) {
            if (owner != slot_.id) continue;
            if (auto s = runner_.venue_.order(cid); s && !s->is_done()) {
                out.push_back(*s);
            }
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return a.request.client_id < b.request.client_id;
        });
        return out;
    }

    [[nodiscard]] Result<ClientOrderId> submit(const OrderIntent& intent) override {
        OrderRequest r;
        r.client_id = runner_.client_ids_.next();
        r.instrument = runner_.instrument_.id;
        r.strategy = slot_.id;
        r.side = intent.side;
        r.type = intent.type;
        r.time_in_force = intent.time_in_force;
        r.price = intent.type == OrderType::limit ? intent.price : Price{};
        r.quantity = intent.quantity;
        runner_.order_owner_[r.client_id] = slot_.id;
        ++runner_.stats_.orders_submitted;
        if (auto s = runner_.venue_.submit(r); !s) {
            runner_.order_owner_.erase(r.client_id);
            return tl::make_unexpected(s.error());
        }
        return r.client_id;
    }
    [[nodiscard]] Result<void> cancel(ClientOrderId id) override {
        auto it = runner_.order_owner_.find(id);
        if (it == runner_.order_owner_.end() || it->second != slot_.id) {
            return make_error(ErrorCode::not_found, "order does not belong to this strategy");
        }
        return runner_.venue_.cancel(id);
    }
    void cancel_all() override {
        for (const auto& o : open_orders()) {
            static_cast<void>(runner_.venue_.cancel(o.request.client_id));
        }
    }
    void metric(std::string_view name, double value) override {
        runner_.metrics_.push_back(MetricSample{now(), slot_.id, std::string(name), value});
    }

private:
    StrategyRunner& runner_;
    Slot& slot_;
    Config params_;
    Logger log_;
    replay::Rng rng_;
};

StrategyRunner::StrategyRunner(Scheduler& scheduler, execution::ExecutionVenue& venue,
                               portfolio::Portfolio& portfolio, Instrument instrument, Logger log,
                               std::uint64_t seed)
    : scheduler_(scheduler),
      venue_(venue),
      portfolio_(portfolio),
      instrument_(std::move(instrument)),
      log_(std::move(log)),
      seed_(seed),
      sync_(instrument_.id) {
    venue_.set_listener(this);
}

StrategyRunner::~StrategyRunner() { stop(); }

StrategyId StrategyRunner::add(std::unique_ptr<Strategy> strategy, Config params, std::string label) {
    const StrategyId id{static_cast<std::uint32_t>(slots_.size() + 1)};
    auto slot = std::make_unique<Slot>();
    slot->id = id;
    slot->label = label.empty() ? std::string(strategy->name()) + "#" + std::to_string(id.value()) : label;
    slot->strategy = std::move(strategy);
    slot->context = std::make_unique<Context>(*this, *slot, std::move(params), log_.child(slot->label),
                                              seed_ ^ (0x9E3779B97F4A7C15ULL * id.value()));
    slots_.push_back(std::move(slot));
    return id;
}

void StrategyRunner::start() {
    if (started_) return;
    started_ = true;
    for (auto& s : slots_) {
        log_.info("starting strategy {}", s->label);
        s->strategy->on_start(*s->context);
    }
}

void StrategyRunner::stop() {
    if (!started_) return;
    started_ = false;
    for (auto& s : slots_) {
        for (auto& agg : s->aggregators) {
            agg->advance_to(scheduler_.now());
        }
        s->strategy->on_stop();
    }
}

void StrategyRunner::on_trade(const market_data::Trade& t) {
    ++stats_.trades;
    last_trade_ = t.price;
    for (auto& s : slots_) {
        for (auto& agg : s->aggregators) {
            agg->on_trade(t);
        }
        s->strategy->on_trade(t);
    }
}

void StrategyRunner::on_book_snapshot(const market_data::BookSnapshot& snap) {
    sync_.on_snapshot(snap);
    if (sync_.synced()) {
        ++stats_.book_updates;
        for (auto& s : slots_) s->strategy->on_book_update();
    }
}

void StrategyRunner::on_book_delta(const market_data::BookDelta& d) {
    if (sync_.on_delta(d) == market_data::SyncAction::applied) {
        ++stats_.book_updates;
        for (auto& s : slots_) s->strategy->on_book_update();
    }
}

void StrategyRunner::on_book_ticker(const market_data::BookTicker& bt) {
    for (auto& s : slots_) s->strategy->on_book_ticker(bt);
}

void StrategyRunner::on_candle(const market_data::Candle& c) {
    ++stats_.candles;
    for (auto& s : slots_) s->strategy->on_candle(c);
}

void StrategyRunner::on_execution_report(const ExecutionReport& report) {
    ++stats_.reports;
    portfolio_.on_execution_report(report);  // positions first, then the owner sees them
    auto it = order_owner_.find(report.client_id);
    if (it == order_owner_.end()) {
        log_.warn("execution report for unknown order {}", report.client_id.value());
        return;
    }
    for (auto& s : slots_) {
        if (s->id == it->second) {
            s->strategy->on_execution_report(report);
            break;
        }
    }
    if (is_terminal(report.status)) {
        order_owner_.erase(it);
    }
}

std::vector<StrategyId> StrategyRunner::strategy_ids() const {
    std::vector<StrategyId> out;
    for (const auto& s : slots_) out.push_back(s->id);
    return out;
}

std::string StrategyRunner::label(StrategyId id) const {
    for (const auto& s : slots_) {
        if (s->id == id) return s->label;
    }
    return {};
}

Strategy* StrategyRunner::strategy(StrategyId id) const {
    for (const auto& s : slots_) {
        if (s->id == id) return s->strategy.get();
    }
    return nullptr;
}

}  // namespace tradebot::strategy
