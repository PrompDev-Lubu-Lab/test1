#pragma once

// Sources of time-ordered market events for replay.
//
// EventSource is the pull interface the replay engine consumes. Concrete
// sources read the event store, wrap in-memory vectors (tests), or merge
// several sources into one stream with deterministic ordering.

#include "tradebot/core/error.hpp"
#include "tradebot/market_data/events.hpp"
#include "tradebot/storage/event_store.hpp"

#include <memory>
#include <optional>
#include <queue>
#include <vector>

namespace tradebot::replay {

class EventSource {
public:
    virtual ~EventSource() = default;
    // False when exhausted. Events must come out in non-decreasing
    // event_time order.
    [[nodiscard]] virtual Result<bool> next(market_data::MarketEvent& out) = 0;
};

class VectorEventSource final : public EventSource {
public:
    explicit VectorEventSource(std::vector<market_data::MarketEvent> events)
        : events_(std::move(events)) {}
    [[nodiscard]] Result<bool> next(market_data::MarketEvent& out) override;

private:
    std::vector<market_data::MarketEvent> events_;
    std::size_t pos_ = 0;
};

class StoreEventSource final : public EventSource {
public:
    StoreEventSource(storage::StorePath path, storage::StreamKind kind, Timestamp from,
                     Timestamp to, Duration candle_interval = {});
    [[nodiscard]] Result<void> open();
    [[nodiscard]] Result<bool> next(market_data::MarketEvent& out) override;

private:
    storage::EventStoreReader reader_;
    bool opened_ = false;
};

// K-way merge by event_time. Ties are broken by source index (the order
// sources were added), then by arrival order within a source, so a replay
// over the same inputs always produces the same sequence.
class MergedEventSource final : public EventSource {
public:
    void add(std::unique_ptr<EventSource> source);
    [[nodiscard]] Result<bool> next(market_data::MarketEvent& out) override;
    [[nodiscard]] std::size_t source_count() const noexcept { return sources_.size(); }

private:
    struct Head {
        Timestamp time;
        std::size_t source;
        std::uint64_t seq;
        bool operator>(const Head& o) const noexcept {
            if (time != o.time) return time > o.time;
            if (source != o.source) return source > o.source;
            return seq > o.seq;
        }
    };
    [[nodiscard]] Result<void> refill(std::size_t source);

    std::vector<std::unique_ptr<EventSource>> sources_;
    std::vector<std::optional<market_data::MarketEvent>> heads_;
    std::priority_queue<Head, std::vector<Head>, std::greater<>> queue_;
    std::uint64_t seq_ = 0;
    bool primed_ = false;
};

// Convenience: every stored kind for an instrument over a range, merged.
struct StoreSelection {
    bool trades = true;
    bool book = true;
    bool tickers = false;
    std::vector<Duration> candle_intervals;  // e.g. {1m}
};

[[nodiscard]] Result<std::unique_ptr<EventSource>> open_store_source(const storage::StorePath& path,
                                                                     const StoreSelection& sel,
                                                                     Timestamp from, Timestamp to);

}  // namespace tradebot::replay
