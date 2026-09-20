#include "tradebot/market_data/order_book.hpp"

#include <algorithm>

namespace tradebot::market_data {

namespace {

// Comparator producing the "better-first" order for a side.
struct BetterThan {
    bool descending;
    bool operator()(Price a, Price b) const noexcept { return descending ? a > b : a < b; }
};

}  // namespace

void OrderBook::set_level_on(std::vector<BookLevel>& levels, bool descending, Price price,
                             Quantity quantity) {
    const BetterThan better{descending};
    auto it = std::lower_bound(levels.begin(), levels.end(), price,
                               [&](const BookLevel& l, Price p) { return better(l.price, p); });
    if (it != levels.end() && it->price == price) {
        if (quantity.is_zero()) {
            levels.erase(it);
        } else {
            it->quantity = quantity;
        }
    } else if (!quantity.is_zero()) {
        levels.insert(it, BookLevel{price, quantity});
    }
}

void OrderBook::apply(const BookSnapshot& snapshot) {
    instrument_ = snapshot.instrument;
    bids_.clear();
    asks_.clear();
    for (const auto& l : snapshot.bids) {
        set_level_on(bids_, true, l.price, l.quantity);
    }
    for (const auto& l : snapshot.asks) {
        set_level_on(asks_, false, l.price, l.quantity);
    }
    last_update_id_ = snapshot.last_update_id;
    last_update_time_ = snapshot.recv_time;
}

void OrderBook::apply(const BookDelta& delta) {
    for (const auto& l : delta.bids) {
        set_level_on(bids_, true, l.price, l.quantity);
    }
    for (const auto& l : delta.asks) {
        set_level_on(asks_, false, l.price, l.quantity);
    }
    last_update_id_ = delta.final_update_id;
    last_update_time_ = delta.recv_time;
}

void OrderBook::set_level(Side side, Price price, Quantity quantity) {
    if (side == Side::buy) {
        set_level_on(bids_, true, price, quantity);
    } else {
        set_level_on(asks_, false, price, quantity);
    }
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    last_update_id_ = 0;
    last_update_time_ = Timestamp{};
}

std::optional<BookLevel> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.front();
}

std::optional<BookLevel> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.front();
}

std::optional<Price> OrderBook::mid_price() const noexcept {
    if (bids_.empty() || asks_.empty()) {
        return std::nullopt;
    }
    return Price::from_raw((bids_.front().price.raw() + asks_.front().price.raw()) / 2);
}

std::optional<Price> OrderBook::spread() const noexcept {
    if (bids_.empty() || asks_.empty()) {
        return std::nullopt;
    }
    return asks_.front().price - bids_.front().price;
}

bool OrderBook::is_crossed() const noexcept {
    return !bids_.empty() && !asks_.empty() && bids_.front().price >= asks_.front().price;
}

Quantity OrderBook::quantity_at(Side s, Price price) const noexcept {
    const auto levels = side(s);
    const BetterThan better{s == Side::buy};
    auto it = std::lower_bound(levels.begin(), levels.end(), price,
                               [&](const BookLevel& l, Price p) { return better(l.price, p); });
    if (it != levels.end() && it->price == price) {
        return it->quantity;
    }
    return Quantity{};
}

Quantity OrderBook::liquidity_within(Side s, Price limit) const noexcept {
    Quantity total;
    for (const auto& l : side(s)) {
        const bool within = s == Side::buy ? l.price >= limit : l.price <= limit;
        if (!within) {
            break;
        }
        total += l.quantity;
    }
    return total;
}

FillEstimate OrderBook::estimate_fill(Side aggressor, Quantity quantity,
                                      std::optional<Price> limit) const {
    FillEstimate est;
    const Side resting = opposite(aggressor);
    Quantity remaining = quantity;
    for (const auto& l : side(resting)) {
        if (remaining.is_zero()) {
            break;
        }
        if (limit) {
            const bool acceptable = aggressor == Side::buy ? l.price <= *limit : l.price >= *limit;
            if (!acceptable) {
                break;
            }
        }
        const Quantity take = std::min(remaining, l.quantity);
        est.filled += take;
        est.cost += notional(l.price, take);
        est.worst_price = l.price;
        ++est.levels;
        remaining -= take;
    }
    return est;
}

double OrderBook::imbalance(std::size_t levels) const noexcept {
    Quantity bid_qty, ask_qty;
    for (std::size_t i = 0; i < levels && i < bids_.size(); ++i) {
        bid_qty += bids_[i].quantity;
    }
    for (std::size_t i = 0; i < levels && i < asks_.size(); ++i) {
        ask_qty += asks_[i].quantity;
    }
    const auto total = static_cast<double>(bid_qty.raw()) + static_cast<double>(ask_qty.raw());
    if (total <= 0.0) {
        return 0.0;
    }
    return (static_cast<double>(bid_qty.raw()) - static_cast<double>(ask_qty.raw())) / total;
}

bool OrderBook::is_sorted() const noexcept {
    for (std::size_t i = 1; i < bids_.size(); ++i) {
        if (!(bids_[i - 1].price > bids_[i].price)) {
            return false;
        }
    }
    for (std::size_t i = 1; i < asks_.size(); ++i) {
        if (!(asks_[i - 1].price < asks_[i].price)) {
            return false;
        }
    }
    for (const auto& l : bids_) {
        if (!l.quantity.is_positive()) return false;
    }
    for (const auto& l : asks_) {
        if (!l.quantity.is_positive()) return false;
    }
    return true;
}

}  // namespace tradebot::market_data
