#include "tradebot/market_data/validation.hpp"

namespace tradebot::market_data {

void TradeValidator::report(IssueKind kind, Timestamp time, std::string detail) {
    switch (kind) {
        case IssueKind::duplicate: ++stats_.duplicates; break;
        case IssueKind::gap: ++stats_.gaps; break;
        case IssueKind::time_regression: ++stats_.time_regressions; break;
        case IssueKind::invalid_value: ++stats_.invalid_values; break;
    }
    if (on_issue_) {
        on_issue_(ValidationIssue{kind, time, std::move(detail)});
    }
}

bool TradeValidator::check(const Trade& trade) {
    // Sequence checks first: an invalid trade still occupies its id, so it
    // must advance the sequence or the next good trade looks like a gap.
    const std::uint64_t id = trade.id.value();
    if (last_id_) {
        if (id <= *last_id_) {
            report(IssueKind::duplicate, trade.recv_time,
                   "trade id " + std::to_string(id) + " <= last " + std::to_string(*last_id_));
            ++stats_.dropped;
            return false;
        }
        if (id != *last_id_ + 1) {
            report(IssueKind::gap, trade.recv_time,
                   "trade ids skipped from " + std::to_string(*last_id_) + " to " +
                       std::to_string(id));
        }
    }
    last_id_ = id;
    if (last_time_ && trade.exchange_time < *last_time_) {
        report(IssueKind::time_regression, trade.recv_time,
               "trade " + std::to_string(id) + " exchange_time went backwards by " +
                   (*last_time_ - trade.exchange_time).to_string());
    }
    last_time_ = last_time_ ? std::max(*last_time_, trade.exchange_time) : trade.exchange_time;
    if (!trade.price.is_positive() || !trade.quantity.is_positive()) {
        report(IssueKind::invalid_value, trade.recv_time,
               "trade " + std::to_string(id) + " has non-positive price/quantity");
        ++stats_.dropped;
        return false;
    }
    ++stats_.accepted;
    return true;
}

void TradeValidator::reset() {
    last_id_.reset();
    last_time_.reset();
}

void BookTickerValidator::report(IssueKind kind, Timestamp time, std::string detail) {
    switch (kind) {
        case IssueKind::duplicate: ++stats_.duplicates; break;
        case IssueKind::gap: ++stats_.gaps; break;
        case IssueKind::time_regression: ++stats_.time_regressions; break;
        case IssueKind::invalid_value: ++stats_.invalid_values; break;
    }
    if (on_issue_) {
        on_issue_(ValidationIssue{kind, time, std::move(detail)});
    }
}

bool BookTickerValidator::check(const BookTicker& t) {
    if (!t.bid_price.is_positive() || !t.ask_price.is_positive() || !t.bid_quantity.is_positive() ||
        !t.ask_quantity.is_positive() || t.bid_price >= t.ask_price) {
        report(IssueKind::invalid_value, t.recv_time,
               "book ticker " + std::to_string(t.update_id) + " invalid or crossed: bid " +
                   t.bid_price.to_string() + " ask " + t.ask_price.to_string());
        ++stats_.dropped;
        return false;
    }
    if (last_id_ && t.update_id <= *last_id_) {
        report(IssueKind::duplicate, t.recv_time,
               "book ticker update id " + std::to_string(t.update_id) + " <= last " +
                   std::to_string(*last_id_));
        ++stats_.dropped;
        return false;
    }
    last_id_ = t.update_id;
    ++stats_.accepted;
    return true;
}

void BookTickerValidator::reset() { last_id_.reset(); }

}  // namespace tradebot::market_data
