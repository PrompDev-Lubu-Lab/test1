#include "tradebot/gateway/binance_gateway.hpp"

namespace tradebot::gateway {

using execution::ExecutionReport;
using execution::OrderRequest;
using execution::OrderState;
using execution::ReportType;
using nlohmann::json;

BinanceGateway::BinanceGateway(BinanceRestClient& rest, Instrument instrument, const Clock& clock, Poster post,
                               GatewayOptions opts, Logger log)
    : rest_(rest),
      instrument_(std::move(instrument)),
      clock_(clock),
      post_(std::move(post)),
      opts_(opts),
      log_(std::move(log)) {
    worker_ = std::thread([this] { worker(); });
}

BinanceGateway::~BinanceGateway() { stop(); }

void BinanceGateway::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stop_.exchange(true)) {
            return;
        }
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::size_t BinanceGateway::pending_jobs() const {
    std::lock_guard lock(mutex_);
    return jobs_.size();
}

void BinanceGateway::enqueue(std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void BinanceGateway::worker() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_.load() || !jobs_.empty(); });
            if (jobs_.empty()) {
                return;  // stopping with nothing left
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

// --- client-facing -------------------------------------------------------------

Result<void> BinanceGateway::submit(const OrderRequest& request) {
    if (!request.client_id.is_valid()) {
        return make_error(ErrorCode::invalid_argument, "order needs a valid client id");
    }
    if (orders_.contains(request.client_id)) {
        return make_error(ErrorCode::invalid_argument, "duplicate client order id");
    }
    if (request.instrument != instrument_.id) {
        return make_error(ErrorCode::invalid_argument, "order for an instrument this gateway does not trade");
    }
    if (request.type == OrderType::limit) {
        if (auto v = instrument_.check_order(request.price, request.quantity); !v) {
            return v;
        }
    } else if (!instrument_.is_valid_quantity(request.quantity)) {
        return make_error(ErrorCode::invalid_argument, "quantity violates lot size / minimum");
    }
    ++stats_.submitted;
    Tracked t;
    t.state.request = request;
    t.state.status = OrderStatus::pending_new;
    t.state.created = clock_.now();
    t.state.updated = t.state.created;
    t.last_activity = t.state.created;
    orders_.emplace(request.client_id, std::move(t));

    if (opts_.dry_run) {
        // Shadow mode: record and reject without sending.
        auto& tracked = orders_.at(request.client_id);
        ExecutionReport rep;
        rep.type = ReportType::rejected;
        rep.client_id = request.client_id;
        rep.instrument = request.instrument;
        rep.strategy = request.strategy;
        rep.side = request.side;
        rep.order_type = request.type;
        rep.price = request.type == OrderType::limit ? request.price : Price{};
        rep.time = clock_.now();
        rep.status = OrderStatus::rejected;
        rep.remaining_quantity = request.quantity;
        rep.reason = "dry run: order not sent";
        log_.info("DRY RUN would send {} {} {} @ {}", to_string(request.side), request.quantity.to_string(),
                  to_string(request.type), request.price.to_string());
        apply(tracked, rep, "dryrun");
        return {};
    }

    const ClientOrderId id = request.client_id;
    const OrderRequest copy = request;
    enqueue([this, id, copy] {
        auto resp = rest_.new_order(instrument_.symbol, copy);
        post_([this, id, resp = std::move(resp)]() mutable { handle_rest_ack(id, std::move(resp)); });
    });
    return {};
}

Result<void> BinanceGateway::cancel(ClientOrderId client_id) {
    auto it = orders_.find(client_id);
    if (it == orders_.end()) {
        return make_error(ErrorCode::not_found, "unknown client order id");
    }
    Tracked& t = it->second;
    if (t.state.is_done()) {
        return make_error(ErrorCode::invalid_state, "order is already done");
    }
    t.cancel_requested = true;
    if (t.state.status != OrderStatus::pending_cancel) {
        t.state.status = OrderStatus::pending_cancel;
    }
    if (opts_.dry_run) {
        return {};
    }
    ++stats_.cancels_sent;
    enqueue([this, client_id] {
        auto resp = rest_.cancel_order(instrument_.symbol, client_id);
        post_([this, client_id, resp = std::move(resp)]() mutable { handle_rest_cancel(client_id, std::move(resp)); });
    });
    return {};
}

std::optional<OrderState> BinanceGateway::order(ClientOrderId client_id) const {
    auto it = orders_.find(client_id);
    if (it == orders_.end()) return std::nullopt;
    return it->second.state;
}

// --- state machine ---------------------------------------------------------------

void BinanceGateway::deliver(const ExecutionReport& report) {
    if (listener_ != nullptr) {
        listener_->on_execution_report(report);
    }
}

void BinanceGateway::apply(Tracked& t, ExecutionReport report, const std::string& dedupe_key) {
    if (!t.seen.insert(dedupe_key).second) {
        ++stats_.duplicates;
        return;
    }
    OrderState& s = t.state;
    t.last_activity = clock_.now();
    // Fill the request-derived fields the stream does not carry.
    report.strategy = s.request.strategy;
    report.instrument = s.request.instrument;
    report.order_type = s.request.type;
    if (report.order_id.is_valid()) s.order_id = report.order_id;
    report.order_id = s.order_id;
    switch (report.type) {
        case ReportType::accepted:
            t.acked = true;
            if (s.status == OrderStatus::pending_new) {
                s.status = OrderStatus::open;
            }
            report.status = s.status == OrderStatus::pending_cancel ? OrderStatus::open : s.status;
            break;
        case ReportType::fill:
            if (report.fill) {
                s.filled_quantity += report.fill->quantity;
                s.filled_notional += notional(report.fill->price, report.fill->quantity);
                s.fees += report.fill->fee;
                if (t.cancel_requested) ++stats_.late_fills;
            }
            s.status = s.remaining().is_zero() ? OrderStatus::filled : OrderStatus::partially_filled;
            report.status = s.status;
            report.filled_quantity = s.filled_quantity;
            report.remaining_quantity = s.remaining();
            break;
        case ReportType::cancelled:
            s.status = OrderStatus::cancelled;
            report.status = s.status;
            report.filled_quantity = s.filled_quantity;
            report.remaining_quantity = s.remaining();
            break;
        case ReportType::rejected:
            s.status = OrderStatus::rejected;
            report.status = s.status;
            break;
        case ReportType::expired:
            s.status = OrderStatus::expired;
            report.status = s.status;
            report.filled_quantity = s.filled_quantity;
            report.remaining_quantity = s.remaining();
            break;
        case ReportType::cancel_rejected:
            if (s.status == OrderStatus::pending_cancel) {
                s.status = OrderStatus::open;
            }
            report.status = s.status;
            break;
    }
    s.updated = report.time.nanos_since_epoch() != 0 ? report.time : clock_.now();
    if (report.time.nanos_since_epoch() == 0) report.time = clock_.now();
    report.side = s.request.side;
    report.price = s.request.type == OrderType::limit ? s.request.price : Price{};
    deliver(report);
}

void BinanceGateway::handle_rest_ack(ClientOrderId id, Result<json> response) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    Tracked& t = it->second;
    if (!response) {
        ++stats_.send_failures;
        // The order may still have reached the venue (timeout after send);
        // a query settles it. A venue-side rejection is final.
        auto ve = parse_venue_error(response.error());
        if (ve) {
            ExecutionReport rep;
            rep.type = ReportType::rejected;
            rep.client_id = id;
            rep.status = OrderStatus::rejected;
            rep.remaining_quantity = t.state.request.quantity;
            rep.reason = "venue: " + ve->message;
            log_.warn("order {} rejected by venue: {}", id.value(), ve->message);
            apply(t, rep, "rest-reject");
            return;
        }
        log_.error("order {} send failed ({}); querying", id.value(), response.error().to_string());
        ++stats_.queries;
        enqueue([this, id] {
            auto q = rest_.query_order(instrument_.symbol, id);
            post_([this, id, q = std::move(q)]() mutable { handle_query(id, std::move(q)); });
        });
        return;
    }
    ++stats_.sent;
    const json& j = *response;
    ExecutionReport rep;
    rep.type = ReportType::accepted;
    rep.client_id = id;
    rep.order_id = OrderId{j.value("orderId", std::uint64_t{0})};
    rep.time = Timestamp::from_millis(j.value("transactTime", std::int64_t{0}));
    rep.remaining_quantity = t.state.request.quantity;
    apply(t, rep, "ack");
}

void BinanceGateway::handle_rest_cancel(ClientOrderId id, Result<json> response) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    Tracked& t = it->second;
    if (!response) {
        if (is_unknown_order(response.error())) {
            // Already filled/cancelled/expired at the venue, or never arrived.
            // If we have no ack, the order never existed there: reject.
            if (!t.acked) {
                ExecutionReport rep;
                rep.type = ReportType::rejected;
                rep.client_id = id;
                rep.status = OrderStatus::rejected;
                rep.reason = "venue: unknown order on cancel (never accepted)";
                apply(t, rep, "cancel-unknown-reject");
                return;
            }
            ExecutionReport rep;
            rep.type = ReportType::cancel_rejected;
            rep.client_id = id;
            rep.reason = "venue: unknown order (already done)";
            apply(t, rep, "cancel-unknown");
            ++stats_.queries;
            enqueue([this, id] {
                auto q = rest_.query_order(instrument_.symbol, id);
                post_([this, id, q = std::move(q)]() mutable { handle_query(id, std::move(q)); });
            });
            return;
        }
        log_.error("cancel {} failed: {}", id.value(), response.error().to_string());
        ExecutionReport rep;
        rep.type = ReportType::cancel_rejected;
        rep.client_id = id;
        rep.reason = response.error().message;
        apply(t, rep, "cancel-fail-" + std::to_string(t.seen.size()));
        return;
    }
    // The stream will also report CANCELED; the REST body is authoritative if
    // it arrives first. Dedupe by the venue order id + type.
    ExecutionReport rep;
    rep.type = ReportType::cancelled;
    rep.client_id = id;
    rep.order_id = OrderId{(*response).value("orderId", std::uint64_t{0})};
    apply(t, rep, "cancelled");
}

void BinanceGateway::handle_query(ClientOrderId id, Result<json> response) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    Tracked& t = it->second;
    if (!response) {
        if (is_unknown_order(response.error()) && !t.acked) {
            ExecutionReport rep;
            rep.type = ReportType::rejected;
            rep.client_id = id;
            rep.status = OrderStatus::rejected;
            rep.reason = "venue: order unknown after send failure";
            apply(t, rep, "query-unknown");
        } else {
            log_.error("query {} failed: {}", id.value(), response.error().to_string());
        }
        return;
    }
    const json& j = *response;
    const std::string status = j.value("status", "");
    auto cum = Quantity::parse(j.value("executedQty", "0"));
    if (!cum) return;
    if (!t.acked) {
        ExecutionReport rep;
        rep.type = ReportType::accepted;
        rep.client_id = id;
        rep.order_id = OrderId{j.value("orderId", std::uint64_t{0})};
        rep.remaining_quantity = t.state.request.quantity;
        apply(t, rep, "ack");
    }
    // Fills seen by query but not by the stream: synthesize the difference
    // as one fill at the average price (exec id 0 marks it synthetic).
    if (*cum > t.state.filled_quantity) {
        auto quote = Notional::parse(j.value("cummulativeQuoteQty", "0"));
        const Quantity missing = *cum - t.state.filled_quantity;
        ExecutionReport rep;
        rep.type = ReportType::fill;
        rep.client_id = id;
        execution::Fill f;
        f.quantity = missing;
        f.price = quote && quote->is_positive() ? price_for(*quote, *cum) : t.state.request.price;
        f.liquidity = Liquidity::taker;
        f.exec_id = TradeId{0};
        rep.fill = f;
        log_.warn("order {}: {} filled per query but unseen on stream; synthesizing", id.value(), missing.to_string());
        apply(t, rep, "query-fill-" + cum->to_string());
    }
    if (status == "CANCELED") {
        ExecutionReport rep;
        rep.type = ReportType::cancelled;
        rep.client_id = id;
        apply(t, rep, "cancelled");
    } else if (status == "REJECTED") {
        ExecutionReport rep;
        rep.type = ReportType::rejected;
        rep.client_id = id;
        rep.status = OrderStatus::rejected;
        apply(t, rep, "rest-reject");
    } else if (status == "EXPIRED" || status == "EXPIRED_IN_MATCH") {
        ExecutionReport rep;
        rep.type = ReportType::expired;
        rep.client_id = id;
        apply(t, rep, "expired");
    }
}

void BinanceGateway::on_stream_execution(const ParsedExecution& exec) {
    ParsedExecution copy = exec;
    post_([this, copy = std::move(copy)] {
        ++stats_.stream_events;
        auto it = orders_.find(copy.report.client_id);
        if (it == orders_.end()) {
            log_.warn("stream event for unknown order {} ({})", copy.report.client_id.value(), copy.execution_type);
            return;
        }
        std::string key = copy.execution_type;
        if (copy.report.type == ReportType::fill && copy.report.fill) {
            key += "-" + std::to_string(copy.report.fill->exec_id.value());
        } else if (copy.report.type == ReportType::accepted) {
            key = "ack";
        } else if (copy.report.type == ReportType::cancelled) {
            key = "cancelled";
        }
        apply(it->second, copy.report, key);
    });
}

void BinanceGateway::poll_silent_orders() {
    if (opts_.dry_run) return;
    const Timestamp now = clock_.now();
    for (auto& [id, t] : orders_) {
        if (t.state.is_done()) continue;
        if (now - t.last_activity < opts_.query_after) continue;
        if (t.acked && t.state.status == OrderStatus::open && !t.cancel_requested) {
            continue;  // resting orders are legitimately silent
        }
        t.last_activity = now;
        ++stats_.queries;
        const ClientOrderId cid = id;
        enqueue([this, cid] {
            auto q = rest_.query_order(instrument_.symbol, cid);
            post_([this, cid, q = std::move(q)]() mutable { handle_query(cid, std::move(q)); });
        });
    }
}

// --- reconciliation ------------------------------------------------------------------

Result<BinanceGateway::Reconciliation> BinanceGateway::reconcile(const portfolio::Portfolio& portfolio,
                                                                 Quantity base_tolerance, Notional quote_tolerance) {
    auto balances = rest_.balances();
    if (!balances) {
        return tl::make_unexpected(balances.error());
    }
    Reconciliation r;
    for (const auto& b : *balances) {
        if (b.asset == instrument_.base) r.venue_base = b.free + b.locked;
        if (b.asset == instrument_.quote) r.venue_quote = Notional::from_raw((b.free + b.locked).raw());
    }
    r.expected_base = portfolio.position(instrument_.id);
    r.expected_quote = portfolio.cash();
    r.base_difference = r.venue_base - r.expected_base;
    r.quote_difference = r.venue_quote - r.expected_quote;
    r.within_tolerance = r.base_difference.abs() <= base_tolerance && r.quote_difference.abs() <= quote_tolerance;
    if (!r.within_tolerance) {
        log_.error("reconciliation mismatch: base venue {} vs expected {}, quote venue {} vs expected {}",
                   r.venue_base.to_string(), r.expected_base.to_string(), r.venue_quote.to_string(),
                   r.expected_quote.to_string());
    }
    return r;
}

}  // namespace tradebot::gateway
