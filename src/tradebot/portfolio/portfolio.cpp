#include "tradebot/portfolio/portfolio.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace tradebot::portfolio {

using execution::ExecutionReport;
using execution::Fill;
using execution::ReportType;

Notional Position::unrealized_pnl(Price mark) const {
    if (quantity.is_zero()) {
        return Notional{};
    }
    // (mark - average_entry) * quantity, signed by direction.
    return notional(mark, quantity) - notional(average_entry, quantity);
}

const Position* Ledger::position(InstrumentId id) const noexcept {
    auto it = positions.find(id);
    return it == positions.end() ? nullptr : &it->second;
}

Portfolio::Portfolio(Notional initial_cash) : initial_cash_(initial_cash) {
    account_.cash = initial_cash;
    peak_equity_ = initial_cash;
}

// --- fills ------------------------------------------------------------------

Notional Portfolio::apply_fill(Ledger& ledger, InstrumentId id, Side side, const Fill& fill) {
    Position& pos = ledger.position_mut(id);
    const Notional value = notional(fill.price, fill.quantity);
    const Quantity signed_qty = side == Side::buy ? fill.quantity : -fill.quantity;

    if (side == Side::buy) {
        ledger.cash -= value;
        pos.bought += fill.quantity;
    } else {
        ledger.cash += value;
        pos.sold += fill.quantity;
    }
    ledger.cash -= fill.fee;
    ledger.fees += fill.fee;
    pos.fees += fill.fee;
    ++pos.fills;

    const bool same_direction = pos.quantity.is_zero() ||
                                (pos.quantity.is_positive() == signed_qty.is_positive());
    if (same_direction) {
        // Adding to (or opening) a position: weighted average entry.
        const Quantity new_qty = pos.quantity + signed_qty;
        const Notional cost = notional(pos.average_entry, pos.quantity.abs()) + value;
        pos.average_entry = price_for(cost, new_qty.abs());
        pos.quantity = new_qty;
        return Notional{};
    }
    // Reducing (possibly flipping): realize on the overlap.
    const Quantity closing = std::min(pos.quantity.abs(), fill.quantity);
    Notional realized = notional(fill.price, closing) - notional(pos.average_entry, closing);
    if (pos.quantity.is_negative()) {
        realized = -realized;  // short: profit when buying back lower
    }
    pos.realized_pnl += realized;
    ledger.realized_pnl += realized;
    const Quantity leftover = fill.quantity - closing;
    pos.quantity += signed_qty;
    if (pos.quantity.is_zero()) {
        pos.average_entry = Price{};
    } else if (leftover.is_positive()) {
        pos.average_entry = fill.price;  // flipped: new position opened at this fill
    }
    return realized;
}

void Portfolio::on_execution_report(const ExecutionReport& r) {
    switch (r.type) {
        case ReportType::accepted: {
            open_orders_[r.client_id] =
                OpenOrder{r.strategy, r.instrument, r.side, r.order_type, r.price, r.remaining_quantity};
            break;
        }
        case ReportType::fill: {
            if (r.fill && !seen_fills_.insert({r.client_id.value(), r.fill->exec_id.value()}).second) {
                // A venue (or a replayed journal) redelivered this fill.
                ++stats_.duplicate_fills;
                break;
            }
            if (r.fill) {
                // Account-level realized P&L is attributed from the strategy
                // ledger's cost basis, not the account's blended one.
                const Notional blended = apply_fill(account_, r.instrument, r.side, *r.fill);
                const Notional attributed = apply_fill(ledgers_[r.strategy], r.instrument, r.side, *r.fill);
                account_.realized_pnl += attributed - blended;
                account_.position_mut(r.instrument).realized_pnl += attributed - blended;
                ++stats_.fills;
                stats_.volume += r.fill->quantity;
                stats_.turnover += notional(r.fill->price, r.fill->quantity);
            }
            if (auto it = open_orders_.find(r.client_id); it != open_orders_.end()) {
                it->second.remaining = r.remaining_quantity;
            }
            track_peak();
            break;
        }
        case ReportType::rejected:
        case ReportType::cancelled:
        case ReportType::expired:
            open_orders_.erase(r.client_id);
            break;
        case ReportType::cancel_rejected:
            break;
    }
    if (is_terminal(r.status)) {
        open_orders_.erase(r.client_id);
    }
    recompute_exposure(r.strategy);
}

void Portfolio::recompute_exposure(StrategyId strategy) {
    OpenExposure e;
    for (const auto& [id, o] : open_orders_) {
        if (o.strategy != strategy || o.remaining.is_zero()) continue;
        ++e.open_orders;
        if (o.side == Side::buy) {
            const Price px = o.price.is_positive() ? o.price : mark(o.instrument).value_or(Price{});
            e.buy_notional += notional(px, o.remaining);
        } else {
            e.sell_quantity += o.remaining;
        }
    }
    exposures_[strategy] = e;
}

// --- marks ------------------------------------------------------------------

void Portfolio::on_trade(const market_data::Trade& t) {
    marks_[t.instrument] = t.price;
    track_peak();
}

void Portfolio::on_book_ticker(const market_data::BookTicker& bt) {
    marks_[bt.instrument] = Price::from_raw((bt.bid_price.raw() + bt.ask_price.raw()) / 2);
    track_peak();
}

void Portfolio::on_book_snapshot(const market_data::BookSnapshot& s) {
    if (!s.bids.empty() && !s.asks.empty()) {
        marks_[s.instrument] = Price::from_raw((s.bids.front().price.raw() + s.asks.front().price.raw()) / 2);
    }
}

void Portfolio::on_book_delta(const market_data::BookDelta&) {
    // Deltas alone do not give a mark; trades and tickers do.
}

void Portfolio::on_candle(const market_data::Candle& c) {
    if (c.closed) {
        marks_[c.instrument] = c.close;
        track_peak();
    }
}

std::optional<Price> Portfolio::mark(InstrumentId id) const {
    auto it = marks_.find(id);
    if (it == marks_.end()) return std::nullopt;
    return it->second;
}

// --- views ------------------------------------------------------------------

const Ledger& Portfolio::ledger(StrategyId strategy) const {
    static const Ledger kEmpty{};
    auto it = ledgers_.find(strategy);
    return it == ledgers_.end() ? kEmpty : it->second;
}

Quantity Portfolio::position(InstrumentId id) const {
    const Position* p = account_.position(id);
    return p ? p->quantity : Quantity{};
}

Quantity Portfolio::position(StrategyId strategy, InstrumentId id) const {
    const Position* p = ledger(strategy).position(id);
    return p ? p->quantity : Quantity{};
}

namespace {
Notional unrealized_of(const Ledger& l, const std::unordered_map<InstrumentId, Price>& marks) {
    Notional total;
    for (const auto& [id, pos] : l.positions) {
        auto m = marks.find(id);
        if (m != marks.end()) {
            total += pos.unrealized_pnl(m->second);
        }
    }
    return total;
}
Notional market_value_of(const Ledger& l, const std::unordered_map<InstrumentId, Price>& marks) {
    Notional total;
    for (const auto& [id, pos] : l.positions) {
        auto m = marks.find(id);
        if (m != marks.end()) {
            total += pos.market_value(m->second);
        } else if (!pos.is_flat()) {
            total += pos.market_value(pos.average_entry);  // no mark yet: at cost
        }
    }
    return total;
}
}  // namespace

Notional Portfolio::unrealized_pnl() const {
    Notional total;
    for (const auto& [s, l] : ledgers_) {
        total += unrealized_of(l, marks_);
    }
    return total;
}
Notional Portfolio::unrealized_pnl(StrategyId s) const { return unrealized_of(ledger(s), marks_); }
Notional Portfolio::equity() const { return account_.cash + market_value_of(account_, marks_); }
Notional Portfolio::equity(StrategyId s) const {
    const Ledger& l = ledger(s);
    return l.cash + market_value_of(l, marks_);
}

const OpenExposure& Portfolio::open_exposure(StrategyId strategy) const {
    static const OpenExposure kNone{};
    auto it = exposures_.find(strategy);
    return it == exposures_.end() ? kNone : it->second;
}

OpenExposure Portfolio::open_exposure() const {
    OpenExposure total;
    for (const auto& [s, e] : exposures_) {
        total.buy_notional += e.buy_notional;
        total.sell_quantity += e.sell_quantity;
        total.open_orders += e.open_orders;
    }
    return total;
}

// --- persistence ------------------------------------------------------------

namespace {

nlohmann::json ledger_to_json(const Ledger& l) {
    nlohmann::json positions = nlohmann::json::array();
    for (const auto& [id, p] : l.positions) {
        positions.push_back({{"instrument", id.value()},
                             {"quantity", p.quantity.to_string()},
                             {"average_entry", p.average_entry.to_string()},
                             {"realized_pnl", p.realized_pnl.to_string()},
                             {"fees", p.fees.to_string()},
                             {"bought", p.bought.to_string()},
                             {"sold", p.sold.to_string()},
                             {"fills", p.fills}});
    }
    return {{"cash", l.cash.to_string()},
            {"fees", l.fees.to_string()},
            {"realized_pnl", l.realized_pnl.to_string()},
            {"positions", positions}};
}

Result<Ledger> ledger_from_json(const nlohmann::json& j) {
    if (!j.is_object()) {
        return make_error(ErrorCode::parse_error, "ledger is not an object");
    }
    Ledger l;
    auto dec = [&](const char* key, auto& out) -> Result<void> {
        using T = std::decay_t<decltype(out)>;
        if (!j.contains(key) || !j[key].is_string()) {
            return make_error(ErrorCode::parse_error, std::string("ledger missing '") + key + "'");
        }
        auto v = T::parse(j[key].get_ref<const std::string&>());
        if (!v) return tl::make_unexpected(v.error());
        out = *v;
        return {};
    };
    if (auto r = dec("cash", l.cash); !r) return tl::make_unexpected(r.error());
    if (auto r = dec("fees", l.fees); !r) return tl::make_unexpected(r.error());
    if (auto r = dec("realized_pnl", l.realized_pnl); !r) return tl::make_unexpected(r.error());
    if (j.contains("positions") && j["positions"].is_array()) {
        for (const auto& pj : j["positions"]) {
            Position p;
            auto pd = [&](const char* key, auto& out) -> Result<void> {
                using T = std::decay_t<decltype(out)>;
                if (!pj.contains(key) || !pj[key].is_string()) {
                    return make_error(ErrorCode::parse_error, std::string("position missing '") + key + "'");
                }
                auto v = T::parse(pj[key].get_ref<const std::string&>());
                if (!v) return tl::make_unexpected(v.error());
                out = *v;
                return {};
            };
            if (auto r = pd("quantity", p.quantity); !r) return tl::make_unexpected(r.error());
            if (auto r = pd("average_entry", p.average_entry); !r) return tl::make_unexpected(r.error());
            if (auto r = pd("realized_pnl", p.realized_pnl); !r) return tl::make_unexpected(r.error());
            if (auto r = pd("fees", p.fees); !r) return tl::make_unexpected(r.error());
            if (auto r = pd("bought", p.bought); !r) return tl::make_unexpected(r.error());
            if (auto r = pd("sold", p.sold); !r) return tl::make_unexpected(r.error());
            p.fills = pj.value("fills", std::uint64_t{0});
            if (!pj.contains("instrument") || !pj["instrument"].is_number_unsigned()) {
                return make_error(ErrorCode::parse_error, "position missing instrument");
            }
            l.positions[InstrumentId{pj["instrument"].get<std::uint32_t>()}] = p;
        }
    }
    return l;
}

}  // namespace

Portfolio::State Portfolio::state() const {
    State s;
    s.account = account_;
    for (const auto& [id, l] : ledgers_) {
        s.strategies.emplace_back(id, l);
    }
    std::sort(s.strategies.begin(), s.strategies.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return s;
}

void Portfolio::restore(const State& s) {
    account_ = s.account;
    ledgers_.clear();
    for (const auto& [id, l] : s.strategies) {
        ledgers_[id] = l;
    }
    exposures_.clear();
    open_orders_.clear();
    peak_equity_ = std::max(peak_equity_, equity());
}

std::string Portfolio::state_to_json() const {
    const State s = state();
    nlohmann::json j;
    j["version"] = 1;
    j["initial_cash"] = initial_cash_.to_string();
    j["account"] = ledger_to_json(s.account);
    nlohmann::json strategies = nlohmann::json::array();
    for (const auto& [id, l] : s.strategies) {
        strategies.push_back({{"id", id.value()}, {"ledger", ledger_to_json(l)}});
    }
    j["strategies"] = strategies;
    return j.dump(2);
}

Result<Portfolio::State> Portfolio::state_from_json(std::string_view text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("version", 0) != 1) {
        return make_error(ErrorCode::parse_error, "portfolio state: not a version 1 document");
    }
    State s;
    auto account = ledger_from_json(j["account"]);
    if (!account) return tl::make_unexpected(account.error());
    s.account = std::move(*account);
    if (j.contains("strategies") && j["strategies"].is_array()) {
        for (const auto& sj : j["strategies"]) {
            auto l = ledger_from_json(sj["ledger"]);
            if (!l) return tl::make_unexpected(l.error());
            s.strategies.emplace_back(StrategyId{sj.value("id", std::uint32_t{0})}, std::move(*l));
        }
    }
    return s;
}

EquitySample Portfolio::snapshot(Timestamp time) const {
    EquitySample s;
    s.time = time;
    s.equity = equity();
    s.cash = account_.cash;
    s.realized_pnl = realized_pnl_net();
    s.unrealized_pnl = unrealized_pnl();
    s.fees = account_.fees;
    for (const auto& [id, pos] : account_.positions) {
        s.position += pos.quantity;
    }
    // The mark is recorded even when flat so benchmarks can use the curve.
    if (!marks_.empty()) {
        s.mark = marks_.begin()->second;
    }
    return s;
}

void Portfolio::sample(Timestamp time) {
    curve_.push_back(snapshot(time));
    track_peak();
}

void Portfolio::track_peak() { peak_equity_ = std::max(peak_equity_, equity()); }

Notional Portfolio::drawdown() const {
    const Notional dd = peak_equity_ - equity();
    return dd.is_negative() ? Notional{} : dd;
}

}  // namespace tradebot::portfolio
