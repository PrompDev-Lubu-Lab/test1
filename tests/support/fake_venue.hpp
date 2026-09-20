#pragma once

// A recording ExecutionVenue for tests: accepts everything synchronously,
// keeps order state, and lets a test inject fills.

#include "tradebot/execution/types.hpp"

#include <map>
#include <vector>

namespace tradebot::test {

class FakeVenue final : public execution::ExecutionVenue {
public:
    void set_listener(execution::ExecutionListener* l) override { listener_ = l; }

    Result<void> submit(const execution::OrderRequest& r) override {
        submitted.push_back(r);
        execution::OrderState s;
        s.request = r;
        s.order_id = OrderId{next_id_++};
        s.status = OrderStatus::open;
        orders_[r.client_id] = s;
        if (listener_) {
            listener_->on_execution_report(report(s, execution::ReportType::accepted));
        }
        return {};
    }

    Result<void> cancel(ClientOrderId id) override {
        cancelled.push_back(id);
        auto it = orders_.find(id);
        if (it == orders_.end()) {
            return make_error(ErrorCode::not_found, "unknown order");
        }
        it->second.status = OrderStatus::cancelled;
        if (listener_) {
            listener_->on_execution_report(report(it->second, execution::ReportType::cancelled));
        }
        return {};
    }

    std::optional<execution::OrderState> order(ClientOrderId id) const override {
        auto it = orders_.find(id);
        if (it == orders_.end()) return std::nullopt;
        return it->second;
    }

    // Test control: fill an open order (fully by default).
    void fill(ClientOrderId id, Price price, std::optional<Quantity> qty = std::nullopt,
              Notional fee = Notional{}) {
        auto& s = orders_.at(id);
        const Quantity q = qty.value_or(s.remaining());
        s.filled_quantity += q;
        s.filled_notional += notional(price, q);
        s.status = s.remaining().is_zero() ? OrderStatus::filled : OrderStatus::partially_filled;
        auto rep = report(s, execution::ReportType::fill);
        rep.fill = execution::Fill{price, q, fee, Liquidity::taker, TradeId{next_id_++}};
        if (listener_) listener_->on_execution_report(rep);
    }

    std::vector<execution::OrderRequest> submitted;
    std::vector<ClientOrderId> cancelled;

private:
    execution::ExecutionReport report(const execution::OrderState& s, execution::ReportType type) {
        execution::ExecutionReport r;
        r.type = type;
        r.client_id = s.request.client_id;
        r.order_id = s.order_id;
        r.instrument = s.request.instrument;
        r.strategy = s.request.strategy;
        r.side = s.request.side;
        r.order_type = s.request.type;
        r.price = s.request.type == OrderType::limit ? s.request.price : Price{};
        r.status = s.status;
        r.filled_quantity = s.filled_quantity;
        r.remaining_quantity = s.remaining();
        return r;
    }

    execution::ExecutionListener* listener_ = nullptr;
    std::map<ClientOrderId, execution::OrderState> orders_;
    std::uint64_t next_id_ = 1;
};

}  // namespace tradebot::test
