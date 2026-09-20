#include "tradebot/risk/risk_manager.hpp"

#include <cmath>

namespace tradebot::risk {

using execution::ExecutionReport;
using execution::OrderRequest;
using execution::ReportType;

RiskManager::RiskManager(execution::ExecutionVenue& venue, const portfolio::Portfolio& portfolio,
                         const Clock& clock, RiskLimits limits, Logger log)
    : venue_(venue), portfolio_(portfolio), clock_(clock), limits_(std::move(limits)), log_(std::move(log)) {
    venue_.set_listener(this);
}

Result<void> RiskManager::check(const OrderRequest& r) const {
    if (tripped_) {
        return make_error(ErrorCode::invalid_state, "kill switch tripped: " + trip_reason_);
    }
    if (!limits_.allowed_instruments.empty() && !limits_.allowed_instruments.contains(r.instrument)) {
        return make_error(ErrorCode::invalid_argument, "instrument not allowed");
    }
    if (!r.quantity.is_positive()) {
        return make_error(ErrorCode::invalid_argument, "quantity must be positive");
    }
    if (limits_.max_order_quantity.is_positive() && r.quantity > limits_.max_order_quantity) {
        return make_error(ErrorCode::invalid_argument, "order quantity exceeds max_order_quantity");
    }
    const auto mark = portfolio_.mark(r.instrument);
    const Price ref = r.type == OrderType::limit ? r.price : mark.value_or(Price{});
    if (r.type == OrderType::limit && !r.price.is_positive()) {
        return make_error(ErrorCode::invalid_argument, "limit price must be positive");
    }
    if (ref.is_positive() && limits_.max_order_notional.is_positive() &&
        notional(ref, r.quantity) > limits_.max_order_notional) {
        return make_error(ErrorCode::invalid_argument, "order notional exceeds max_order_notional");
    }
    if (r.type == OrderType::limit && mark && limits_.max_price_deviation > 0.0) {
        const double dev = std::fabs(r.price.to_double() / mark->to_double() - 1.0);
        if (dev > limits_.max_price_deviation) {
            return make_error(ErrorCode::invalid_argument, "limit price too far from mark");
        }
    }
    const auto& exposure = portfolio_.open_exposure(r.strategy);
    if (limits_.max_open_orders > 0 && exposure.open_orders >= limits_.max_open_orders) {
        return make_error(ErrorCode::invalid_state, "max_open_orders reached");
    }
    if (limits_.max_orders_per_minute > 0 && recent_submits_.size() >= limits_.max_orders_per_minute) {
        return make_error(ErrorCode::invalid_state, "max_orders_per_minute reached");
    }
    // Projected position including everything already working.
    const Quantity current = portfolio_.position(r.strategy, r.instrument);
    Quantity projected;
    if (r.side == Side::buy) {
        Quantity open_buys;
        if (ref.is_positive()) {
            open_buys = quantity_for(exposure.buy_notional, ref, RoundingMode::up);
        }
        projected = current + open_buys + r.quantity;
    } else {
        projected = current - exposure.sell_quantity - r.quantity;
        if (!limits_.allow_short && projected.is_negative()) {
            return make_error(ErrorCode::invalid_state, "sell would exceed position (shorting not allowed)");
        }
    }
    if (limits_.max_position.is_positive() && projected.abs() > limits_.max_position) {
        return make_error(ErrorCode::invalid_state, "projected position exceeds max_position");
    }
    if (limits_.max_position_notional.is_positive() && ref.is_positive() &&
        notional(ref, projected.abs()) > limits_.max_position_notional) {
        return make_error(ErrorCode::invalid_state, "projected position exceeds max_position_notional");
    }
    return {};
}

void RiskManager::reject(const OrderRequest& r, const std::string& reason) {
    ++stats_.rejected;
    ++stats_.rejections_by_reason[reason];
    log_.warn("risk rejected order {} ({} {} @ {}): {}", r.client_id.value(), to_string(r.side),
              r.quantity.to_string(), r.price.to_string(), reason);
    if (listener_ == nullptr) {
        return;
    }
    ExecutionReport rep;
    rep.type = ReportType::rejected;
    rep.client_id = r.client_id;
    rep.instrument = r.instrument;
    rep.strategy = r.strategy;
    rep.side = r.side;
    rep.order_type = r.type;
    rep.price = r.type == OrderType::limit ? r.price : Price{};
    rep.time = clock_.now();
    rep.status = OrderStatus::rejected;
    rep.remaining_quantity = r.quantity;
    rep.reason = "risk: " + reason;
    listener_->on_execution_report(rep);
}

Result<void> RiskManager::submit(const OrderRequest& request) {
    ++stats_.checked;
    check_limits();
    const Timestamp now = clock_.now();
    while (!recent_submits_.empty() && now - recent_submits_.front() >= Duration::minutes(1)) {
        recent_submits_.pop_front();
    }
    if (auto c = check(request); !c) {
        reject(request, c.error().message);
        return {};  // rejection is delivered as a report, not an error
    }
    recent_submits_.push_back(now);
    auto r = venue_.submit(request);
    if (!r) {
        reject(request, "venue refused submission: " + r.error().message);
        return {};
    }
    open_.insert(request.client_id);
    return {};
}

Result<void> RiskManager::cancel(ClientOrderId client_id) { return venue_.cancel(client_id); }

std::optional<execution::OrderState> RiskManager::order(ClientOrderId client_id) const {
    return venue_.order(client_id);
}

void RiskManager::on_execution_report(const ExecutionReport& report) {
    if (is_terminal(report.status)) {
        open_.erase(report.client_id);
    }
    if (listener_ != nullptr) {
        listener_->on_execution_report(report);
    }
    if (report.type == ReportType::fill) {
        check_limits();
    }
}

Notional RiskManager::day_start_equity() {
    const Timestamp today = clock_.now().floor_to(Duration::days(1));
    if (!day_ || *day_ != today) {
        day_ = today;
        day_start_equity_ = portfolio_.equity();
    }
    return day_start_equity_;
}

bool RiskManager::check_limits() {
    if (tripped_) {
        return true;
    }
    const Notional start = day_start_equity();
    if (limits_.max_drawdown.is_positive() && portfolio_.drawdown() > limits_.max_drawdown) {
        trip("drawdown " + portfolio_.drawdown().to_string() + " exceeds max_drawdown " +
             limits_.max_drawdown.to_string());
        return true;
    }
    if (limits_.max_daily_loss.is_positive()) {
        const Notional loss = start - portfolio_.equity();
        if (loss > limits_.max_daily_loss) {
            trip("daily loss " + loss.to_string() + " exceeds max_daily_loss " +
                 limits_.max_daily_loss.to_string());
            return true;
        }
    }
    return false;
}

void RiskManager::trip(std::string reason) {
    if (tripped_) {
        return;
    }
    tripped_ = true;
    trip_reason_ = std::move(reason);
    ++stats_.kill_switch_trips;
    log_.error("KILL SWITCH: {}", trip_reason_);
    cancel_all_open();
    if (limits_.flatten_on_trip) {
        flatten_positions();
    }
}

void RiskManager::flatten_positions() {
    // Per strategy so attribution stays right; straight to the venue since
    // the gate is closed.
    for (const auto& [strategy_id, ledger] : portfolio_.strategy_ledgers()) {
        for (const auto& [instrument, pos] : ledger.positions) {
            if (pos.quantity.is_zero()) continue;
            execution::OrderRequest r;
            r.client_id = flatten_ids_.next();
            r.instrument = instrument;
            r.strategy = strategy_id;
            r.side = pos.quantity.is_positive() ? Side::sell : Side::buy;
            r.type = OrderType::market;
            r.time_in_force = TimeInForce::ioc;
            r.quantity = pos.quantity.abs();
            ++stats_.flatten_orders;
            log_.warn("flattening {} {} for strategy {}", to_string(r.side), r.quantity.to_string(), strategy_id.value());
            if (auto s = venue_.submit(r); !s) {
                log_.error("flatten order failed: {}", s.error().to_string());
            } else {
                open_.insert(r.client_id);
            }
        }
    }
}

void RiskManager::cancel_all_open() {
    // Copy: cancellations may synchronously produce reports that mutate open_.
    const std::set<ClientOrderId> open = open_;
    for (ClientOrderId id : open) {
        if (auto state = venue_.order(id); state && !state->is_done()) {
            static_cast<void>(venue_.cancel(id));
        }
    }
}

void RiskManager::reset() {
    tripped_ = false;
    trip_reason_.clear();
    log_.warn("kill switch reset");
}

}  // namespace tradebot::risk
