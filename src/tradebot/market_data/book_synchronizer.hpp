#pragma once

// Keeps an OrderBook consistent with a snapshot + incremental delta feed
// using Binance's documented procedure:
//
//   1. Buffer deltas until a snapshot arrives.
//   2. Drop buffered deltas whose final id u <= snapshot.lastUpdateId.
//   3. The first applied delta must satisfy U <= lastUpdateId + 1 <= u.
//   4. Every following delta must have U == previous u + 1.
//
// Any violation of 3 or 4 means data was lost: the book is cleared, the
// synchronizer reports that a fresh snapshot is needed, and it goes back
// to buffering. The live collector and the replayer both provide snapshots
// on demand (the collector fetches one on every gap it detects itself, so
// in replay a snapshot normally follows within a second).

#include "tradebot/market_data/events.hpp"
#include "tradebot/market_data/order_book.hpp"

#include <cstdint>
#include <deque>

namespace tradebot::market_data {

enum class SyncState : std::uint8_t {
    awaiting_snapshot,  // buffering deltas, book is empty
    synced,  // book is live
};

// What the caller should do after feeding an event.
enum class SyncAction : std::uint8_t {
    none,  // buffered or dropped
    applied,  // book changed
    resync_required,  // gap detected: book cleared, feed a new snapshot
};

struct SyncStats {
    std::uint64_t snapshots = 0;
    std::uint64_t deltas_applied = 0;
    std::uint64_t deltas_dropped = 0;  // stale (u <= lastUpdateId) or overflow
    std::uint64_t gaps = 0;
    std::uint64_t buffer_overflows = 0;
};

class BookSynchronizer {
public:
    explicit BookSynchronizer(InstrumentId instrument, std::size_t max_buffer = 20'000);

    SyncAction on_snapshot(const BookSnapshot& snapshot);
    SyncAction on_delta(const BookDelta& delta);

    [[nodiscard]] SyncState state() const noexcept { return state_; }
    [[nodiscard]] bool synced() const noexcept { return state_ == SyncState::synced; }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const SyncStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }

private:
    SyncAction apply_first(const BookDelta& delta);
    void desync();

    OrderBook book_;
    SyncState state_ = SyncState::awaiting_snapshot;
    std::deque<BookDelta> buffer_;
    std::size_t max_buffer_;
    std::int64_t last_final_id_ = -1;
    bool first_after_snapshot_ = false;
    SyncStats stats_;
};

}  // namespace tradebot::market_data
