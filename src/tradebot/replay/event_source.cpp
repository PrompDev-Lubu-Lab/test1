#include "tradebot/replay/event_source.hpp"

namespace tradebot::replay {

using market_data::MarketEvent;

Result<bool> VectorEventSource::next(MarketEvent& out) {
    if (pos_ >= events_.size()) {
        return false;
    }
    out = events_[pos_++];
    return true;
}

StoreEventSource::StoreEventSource(storage::StorePath path, storage::StreamKind kind,
                                   Timestamp from, Timestamp to, Duration candle_interval)
    : reader_(std::move(path), kind, from, to, candle_interval) {}

Result<void> StoreEventSource::open() {
    if (auto r = reader_.open(); !r) {
        return r;
    }
    opened_ = true;
    return {};
}

Result<bool> StoreEventSource::next(MarketEvent& out) {
    if (!opened_) {
        if (auto r = open(); !r) {
            return tl::make_unexpected(r.error());
        }
    }
    return reader_.next(out);
}

void MergedEventSource::add(std::unique_ptr<EventSource> source) {
    sources_.push_back(std::move(source));
    heads_.emplace_back();
    primed_ = false;
}

Result<void> MergedEventSource::refill(std::size_t i) {
    MarketEvent ev;
    auto more = sources_[i]->next(ev);
    if (!more) {
        return tl::make_unexpected(more.error());
    }
    if (*more) {
        queue_.push(Head{market_data::event_time(ev), i, seq_++});
        heads_[i] = std::move(ev);
    } else {
        heads_[i].reset();
    }
    return {};
}

Result<bool> MergedEventSource::next(MarketEvent& out) {
    if (!primed_) {
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            if (!heads_[i]) {
                if (auto r = refill(i); !r) {
                    return tl::make_unexpected(r.error());
                }
            }
        }
        primed_ = true;
    }
    if (queue_.empty()) {
        return false;
    }
    const Head head = queue_.top();
    queue_.pop();
    out = std::move(*heads_[head.source]);
    heads_[head.source].reset();
    if (auto r = refill(head.source); !r) {
        return tl::make_unexpected(r.error());
    }
    return true;
}

Result<std::unique_ptr<EventSource>> open_store_source(const storage::StorePath& path,
                                                       const StoreSelection& sel, Timestamp from,
                                                       Timestamp to) {
    auto merged = std::make_unique<MergedEventSource>();
    auto add = [&](storage::StreamKind kind, Duration interval) -> Result<void> {
        auto src = std::make_unique<StoreEventSource>(path, kind, from, to, interval);
        if (auto r = src->open(); !r) {
            return r;
        }
        merged->add(std::move(src));
        return {};
    };
    // Order of addition is the tie-break order: book state before trades at
    // the same instant, trades before tickers, candles last.
    if (sel.book) {
        if (auto r = add(storage::StreamKind::book, {}); !r) return tl::make_unexpected(r.error());
    }
    if (sel.trades) {
        if (auto r = add(storage::StreamKind::trades, {}); !r) return tl::make_unexpected(r.error());
    }
    if (sel.tickers) {
        if (auto r = add(storage::StreamKind::tickers, {}); !r) return tl::make_unexpected(r.error());
    }
    for (Duration iv : sel.candle_intervals) {
        if (auto r = add(storage::StreamKind::candles, iv); !r) return tl::make_unexpected(r.error());
    }
    return std::unique_ptr<EventSource>(std::move(merged));
}

}  // namespace tradebot::replay
