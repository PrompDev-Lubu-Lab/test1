#include "tradebot/execution/simulated_exchange.hpp"

#include <algorithm>

namespace tradebot::execution {

using market_data::BookDelta;
using market_data::BookSnapshot;
using market_data::SyncAction;
using market_data::Trade;

SimulatedExchange::SimulatedExchange(Instrument instrument, Scheduler& scheduler,
                                     replay::LatencyModel& latency, replay::Rng& rng,
                                     SimulatedExchangeOptions opts)
    : instrument_(std::move(instrument)),
      scheduler_(scheduler),
      latency_(latency),
      rng_(rng),
      opts_(opts),
      sync_(instrument_.id) {}

// --- client-facing API ------------------------------------------------------

Result<void> SimulatedExchange::submit(const OrderRequest& request) {
    if (!request.client_id.is_valid()) {
        return make_error(ErrorCode::invalid_argument, "order needs a valid client id");
    }
    if (orders_.contains(request.client_id)) {
        return make_error(ErrorCode::invalid_argument,
                          "duplicate client order id " + std::to_string(request.client_id.value()));
    }
    if (request.instrument != instrument_.id) {
        return make_error(ErrorCode::invalid_argument, "order for an instrument this venue does not trade");
    }
    ++stats_.submitted;
    OrderState state;
    state.request = request;
    state.status = OrderStatus::pending_new;
    state.created = scheduler_.now();
    state.updated = state.created;
    orders_.emplace(request.client_id, std::move(state));
    const Timestamp arrival = scheduler_.now() + latency_.order_delay(rng_);
    scheduler_.schedule_at(arrival, [this, request](Timestamp t) { on_order_arrival(request, t); });
    return {};
}

Result<void> SimulatedExchange::cancel(ClientOrderId client_id) {
    auto it = orders_.find(client_id);
    if (it == orders_.end()) {
        return make_error(ErrorCode::not_found,
                          "unknown client order id " + std::to_string(client_id.value()));
    }
    if (it->second.is_done()) {
        return make_error(ErrorCode::invalid_state, "order is already done");
    }
    if (it->second.status != OrderStatus::pending_cancel) {
        it->second.status = OrderStatus::pending_cancel;
    }
    const Timestamp arrival = scheduler_.now() + latency_.order_delay(rng_);
    scheduler_.schedule_at(arrival, [this, client_id](Timestamp t) { on_cancel_arrival(client_id, t); });
    return {};
}

std::optional<OrderState> SimulatedExchange::order(ClientOrderId client_id) const {
    auto it = orders_.find(client_id);
    if (it == orders_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<OrderState> SimulatedExchange::open_orders() const {
    std::vector<OrderState> out;
    for (const auto& [id, o] : orders_) {
        if (!o.is_done()) {
            out.push_back(o);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const OrderState& a, const OrderState& b) { return a.order_id < b.order_id; });
    return out;
}

// --- venue-side processing --------------------------------------------------

Result<void> SimulatedExchange::validate(const OrderRequest& r) const {
    if (r.type == OrderType::limit) {
        return instrument_.check_order(r.price, r.quantity);
    }
    if (!instrument_.is_valid_quantity(r.quantity)) {
        return make_error(ErrorCode::invalid_argument,
                          instrument_.symbol + ": quantity " + r.quantity.to_string() +
                              " violates lot size / minimum");
    }
    if (r.time_in_force == TimeInForce::post_only) {
        return make_error(ErrorCode::invalid_argument, "market orders cannot be post-only");
    }
    return {};
}

void SimulatedExchange::on_order_arrival(OrderRequest request, Timestamp now) {
    auto it = orders_.find(request.client_id);
    if (it == orders_.end()) {
        return;
    }
    OrderState& order = it->second;
    if (order.status == OrderStatus::pending_cancel) {
        // A cancel was requested before the order even arrived; the cancel
        // arrives later and will find the order live.
        order.status = OrderStatus::pending_new;
    }
    if (auto v = validate(request); !v) {
        finish(order, ReportType::rejected, OrderStatus::rejected, now, v.error().message);
        return;
    }
    const auto& book = sync_.book();
    const bool have_book = book_usable();
    const bool have_trades = opts_.fallback_to_trades && last_trade_.has_value();
    order.order_id = order_ids_.next();
    const Side resting_side = opposite(request.side);
    std::optional<Price> best_opposite;
    if (have_book && !book.side(resting_side).empty()) {
        best_opposite = book.side(resting_side).front().price;
    } else if (!have_book && have_trades) {
        best_opposite = *last_trade_;  // trades-only mode: the tape is the touch
    }

    if (request.type == OrderType::market) {
        if (!best_opposite) {
            finish(order, ReportType::rejected, OrderStatus::rejected, now,
                   "no liquidity / book not synchronized");
            return;
        }
        if (have_book && request.time_in_force == TimeInForce::fok) {
            const auto est = book.estimate_fill(request.side, request.quantity);
            if (!est.complete(request.quantity)) {
                finish(order, ReportType::rejected, OrderStatus::rejected, now, "FOK cannot be filled");
                return;
            }
        }
        order.status = OrderStatus::open;
        ++stats_.accepted;
        report(order, ReportType::accepted, now);
        const Quantity filled = have_book ? take(order, std::nullopt, now)
                                          : take_at_last_trade(order, std::nullopt, now);
        if (order.remaining().is_zero()) {
            return;  // finished inside take()
        }
        if (filled.is_zero() || !opts_.allow_partial_market_fills) {
            finish(order, ReportType::expired, OrderStatus::expired, now, "insufficient liquidity");
        } else {
            finish(order, ReportType::expired, OrderStatus::expired, now, "remainder unfilled");
        }
        return;
    }

    // Limit order.
    const bool crosses = best_opposite && (request.side == Side::buy ? request.price >= *best_opposite
                                                                     : request.price <= *best_opposite);
    if (request.time_in_force == TimeInForce::post_only && crosses) {
        finish(order, ReportType::rejected, OrderStatus::rejected, now, "post-only order would take");
        return;
    }
    if (request.time_in_force == TimeInForce::fok) {
        bool fillable = crosses;
        if (have_book) {
            const auto est = book.estimate_fill(request.side, request.quantity, request.price);
            fillable = est.complete(request.quantity);
        }
        if (!fillable) {
            finish(order, ReportType::rejected, OrderStatus::rejected, now, "FOK cannot be filled");
            return;
        }
    }
    order.status = OrderStatus::open;
    ++stats_.accepted;
    report(order, ReportType::accepted, now);
    if (crosses) {
        if (have_book) {
            take(order, request.price, now);
        } else {
            take_at_last_trade(order, request.price, now);
        }
        if (order.remaining().is_zero()) {
            return;
        }
    }
    if (request.time_in_force == TimeInForce::ioc || request.time_in_force == TimeInForce::fok) {
        finish(order, ReportType::expired, OrderStatus::expired, now, "IOC remainder");
        return;
    }
    rest(order);
}

void SimulatedExchange::on_cancel_arrival(ClientOrderId client_id, Timestamp now) {
    auto it = orders_.find(client_id);
    if (it == orders_.end()) {
        return;
    }
    OrderState& order = it->second;
    if (order.is_done() || order.status == OrderStatus::pending_new) {
        // Done, or the order itself has not arrived yet (cancel overtook it,
        // which real venues answer with "unknown order").
        if (!order.is_done()) {
            order.status = OrderStatus::pending_new;
        }
        report(order, ReportType::cancel_rejected, now, std::nullopt, "order not open");
        return;
    }
    unrest(order);
    ++stats_.cancelled;
    finish(order, ReportType::cancelled, OrderStatus::cancelled, now);
}

Quantity SimulatedExchange::take(OrderState& order, std::optional<Price> limit, Timestamp now) {
    const Side resting_side = opposite(order.request.side);
    Quantity total;
    // Copy the levels we will consume, then apply consumption to the book.
    std::vector<market_data::BookLevel> consumed;
    Quantity remaining = order.remaining();
    for (const auto& lvl : sync_.book().side(resting_side)) {
        if (remaining.is_zero()) break;
        if (limit) {
            const bool ok = order.request.side == Side::buy ? lvl.price <= *limit : lvl.price >= *limit;
            if (!ok) break;
        }
        const Quantity qty = std::min(remaining, lvl.quantity);
        consumed.push_back({lvl.price, qty});
        remaining -= qty;
    }
    for (const auto& c : consumed) {
        fill(order, c.price, c.quantity, Liquidity::taker, now);
        total += c.quantity;
        if (opts_.consume_liquidity) {
            const Quantity left = sync_.book().quantity_at(resting_side, c.price) - c.quantity;
            const_cast<market_data::OrderBook&>(sync_.book()).set_level(resting_side, c.price, left);
        }
    }
    return total;
}

bool SimulatedExchange::book_usable() const noexcept {
    if (opts_.require_synced_book) {
        return sync_.synced() && !sync_.book().empty();
    }
    return !sync_.book().empty();
}

Quantity SimulatedExchange::take_at_last_trade(OrderState& order, std::optional<Price> limit,
                                               Timestamp now) {
    if (!last_trade_) {
        return Quantity{};
    }
    // Slip against the order, then snap to the tick grid in the same direction.
    const std::int64_t bps = opts_.trade_slippage_bps;
    Price px = order.request.side == Side::buy ? last_trade_->mul_ratio(10'000 + bps, 10'000)
                                              : last_trade_->mul_ratio(10'000 - bps, 10'000);
    px = px.round_to(instrument_.tick_size,
                     order.request.side == Side::buy ? RoundingMode::up : RoundingMode::down);
    if (limit) {
        // Never worse than the limit; if the slipped price breaches it, fill at the limit.
        if (order.request.side == Side::buy && px > *limit) px = *limit;
        if (order.request.side == Side::sell && px < *limit) px = *limit;
    }
    const Quantity qty = order.remaining();
    fill(order, px, qty, Liquidity::taker, now);
    return qty;
}

void SimulatedExchange::rest(OrderState& order) {
    auto& side_map = order.request.side == Side::buy ? resting_bids_ : resting_asks_;
    Resting r{order.request.client_id, sync_.book().quantity_at(order.request.side, order.request.price)};
    if (opts_.queue_model == QueueModel::optimistic) {
        r.queue_ahead = Quantity{};
    }
    side_map[order.request.price].push_back(r);
}

void SimulatedExchange::unrest(const OrderState& order) {
    auto& side_map = order.request.side == Side::buy ? resting_bids_ : resting_asks_;
    auto it = side_map.find(order.request.price);
    if (it == side_map.end()) return;
    auto& q = it->second;
    q.erase(std::remove_if(q.begin(), q.end(),
                           [&](const Resting& r) { return r.client_id == order.request.client_id; }),
            q.end());
    if (q.empty()) {
        side_map.erase(it);
    }
}

void SimulatedExchange::fill(OrderState& order, Price price, Quantity quantity, Liquidity liquidity,
                             Timestamp now) {
    const Notional value = notional(price, quantity);
    Fill f;
    f.price = price;
    f.quantity = quantity;
    f.fee = opts_.fees.fee_for(liquidity, value);
    f.liquidity = liquidity;
    f.exec_id = exec_ids_.next();
    order.filled_quantity += quantity;
    order.filled_notional += value;
    order.fees += f.fee;
    order.updated = now;
    order.status = order.remaining().is_zero() ? OrderStatus::filled : OrderStatus::partially_filled;
    ++stats_.fills;
    stats_.fees_charged += f.fee;
    if (liquidity == Liquidity::taker) {
        stats_.taker_volume += quantity;
    } else {
        stats_.maker_volume += quantity;
    }
    report(order, ReportType::fill, now, f);
}

void SimulatedExchange::finish(OrderState& order, ReportType type, OrderStatus status, Timestamp now,
                               std::string reason) {
    order.status = status;
    order.updated = now;
    if (type == ReportType::rejected) ++stats_.rejected;
    if (type == ReportType::expired) ++stats_.expired;
    report(order, type, now, std::nullopt, std::move(reason));
}

void SimulatedExchange::report(const OrderState& order, ReportType type, Timestamp now,
                               std::optional<Fill> fill, std::string reason) {
    ExecutionReport rep;
    rep.type = type;
    rep.client_id = order.request.client_id;
    rep.order_id = order.order_id;
    rep.instrument = order.request.instrument;
    rep.strategy = order.request.strategy;
    rep.side = order.request.side;
    rep.order_type = order.request.type;
    rep.price = order.request.type == OrderType::limit ? order.request.price : Price{};
    rep.time = now;
    rep.status = order.status;
    rep.filled_quantity = order.filled_quantity;
    rep.remaining_quantity = order.remaining();
    rep.fill = std::move(fill);
    rep.reason = std::move(reason);
    const Timestamp delivery = now + latency_.ack_delay(rng_);
    scheduler_.schedule_at(delivery, [this, rep = std::move(rep)](Timestamp) {
        if (listener_ != nullptr) {
            listener_->on_execution_report(rep);
        }
    });
}

// --- market-driven matching -------------------------------------------------

void SimulatedExchange::on_book_snapshot(const BookSnapshot& s) {
    sync_.on_snapshot(s);
    if (sync_.synced()) {
        shrink_queues_from_book();
        match_resting_against_book();
    }
}

void SimulatedExchange::on_book_delta(const BookDelta& d) {
    if (sync_.on_delta(d) == SyncAction::applied) {
        shrink_queues_from_book();
        match_resting_against_book();
    }
}

void SimulatedExchange::on_trade(const Trade& t) {
    last_trade_ = t.price;
    match_resting_against_trade(t);
}

void SimulatedExchange::shrink_queues_from_book() {
    const auto& book = sync_.book();
    for (auto& [price, queue] : resting_bids_) {
        const Quantity at = book.quantity_at(Side::buy, price);
        for (auto& r : queue) r.queue_ahead = std::min(r.queue_ahead, at);
    }
    for (auto& [price, queue] : resting_asks_) {
        const Quantity at = book.quantity_at(Side::sell, price);
        for (auto& r : queue) r.queue_ahead = std::min(r.queue_ahead, at);
    }
}

void SimulatedExchange::match_resting_against_trade(const Trade& t) {
    const Timestamp now = scheduler_.now();
    // A sell-aggressor trade hits resting bids at or above its price; a
    // buy-aggressor trade lifts resting asks at or below its price.
    auto process = [&](auto& side_map, Side side) {
        std::vector<Price> emptied;
        for (auto& [price, queue] : side_map) {
            const bool reachable = side == Side::buy ? t.price <= price : t.price >= price;
            if (!reachable) continue;
            const bool through = side == Side::buy ? t.price < price : t.price > price;
            Quantity volume = t.quantity;
            for (auto it = queue.begin(); it != queue.end();) {
                auto oit = orders_.find(it->client_id);
                if (oit == orders_.end() || oit->second.is_done()) {
                    it = queue.erase(it);
                    continue;
                }
                OrderState& order = oit->second;
                Quantity fill_qty;
                if (through) {
                    // Everyone at this price was taken out.
                    fill_qty = order.remaining();
                } else {
                    if (opts_.queue_model == QueueModel::pessimistic) {
                        ++it;
                        continue;
                    }
                    const Quantity ahead = std::min(it->queue_ahead, volume);
                    it->queue_ahead -= ahead;
                    volume -= ahead;
                    fill_qty = std::min(volume, order.remaining());
                    volume -= fill_qty;
                }
                if (fill_qty.is_positive()) {
                    fill(order, price, fill_qty, Liquidity::maker, now);
                }
                if (order.remaining().is_zero()) {
                    it = queue.erase(it);
                } else {
                    ++it;
                }
                if (volume.is_zero() && !through) break;
            }
            if (queue.empty()) emptied.push_back(price);
        }
        for (Price p : emptied) side_map.erase(p);
    };
    if (t.aggressor == Side::sell) {
        process(resting_bids_, Side::buy);
    } else {
        process(resting_asks_, Side::sell);
    }
}

void SimulatedExchange::match_resting_against_book() {
    // If the opposite side of the venue book reaches our price, our order
    // must have been executed (a real book cannot stay crossed).
    const Timestamp now = scheduler_.now();
    const auto& book = sync_.book();
    auto sweep = [&](auto& side_map, Side side, std::optional<market_data::BookLevel> best_opp) {
        if (!best_opp) return;
        std::vector<Price> emptied;
        for (auto& [price, queue] : side_map) {
            const bool crossed = side == Side::buy ? best_opp->price <= price : best_opp->price >= price;
            if (!crossed) continue;
            for (auto& r : queue) {
                auto oit = orders_.find(r.client_id);
                if (oit == orders_.end() || oit->second.is_done()) continue;
                OrderState& order = oit->second;
                fill(order, price, order.remaining(), Liquidity::maker, now);
            }
            emptied.push_back(price);
        }
        for (Price p : emptied) side_map.erase(p);
    };
    sweep(resting_bids_, Side::buy, book.best_ask());
    sweep(resting_asks_, Side::sell, book.best_bid());
}

}  // namespace tradebot::execution
