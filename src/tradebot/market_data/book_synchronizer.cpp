#include "tradebot/market_data/book_synchronizer.hpp"

namespace tradebot::market_data {

BookSynchronizer::BookSynchronizer(InstrumentId instrument, std::size_t max_buffer)
    : book_(instrument), max_buffer_(max_buffer) {}

void BookSynchronizer::desync() {
    book_.clear();
    state_ = SyncState::awaiting_snapshot;
    first_after_snapshot_ = false;
    last_final_id_ = -1;
    buffer_.clear();
}

SyncAction BookSynchronizer::apply_first(const BookDelta& delta) {
    // Rule 3: U <= lastUpdateId + 1 <= u.
    const std::int64_t next = book_.last_update_id() + 1;
    if (delta.first_update_id > next) {
        ++stats_.gaps;
        desync();
        return SyncAction::resync_required;
    }
    book_.apply(delta);
    last_final_id_ = delta.final_update_id;
    first_after_snapshot_ = false;
    ++stats_.deltas_applied;
    return SyncAction::applied;
}

SyncAction BookSynchronizer::on_snapshot(const BookSnapshot& snapshot) {
    ++stats_.snapshots;
    book_.apply(snapshot);
    state_ = SyncState::synced;
    first_after_snapshot_ = true;
    last_final_id_ = snapshot.last_update_id;

    // Drain the buffer.
    std::deque<BookDelta> pending;
    pending.swap(buffer_);
    for (const auto& d : pending) {
        if (d.final_update_id <= snapshot.last_update_id) {
            ++stats_.deltas_dropped;  // Rule 2: already in the snapshot
            continue;
        }
        const SyncAction a = on_delta(d);
        if (a == SyncAction::resync_required) {
            return a;
        }
    }
    return SyncAction::applied;
}

SyncAction BookSynchronizer::on_delta(const BookDelta& delta) {
    if (state_ != SyncState::synced) {
        if (buffer_.size() >= max_buffer_) {
            ++stats_.buffer_overflows;
            ++stats_.deltas_dropped;
            buffer_.pop_front();
        }
        buffer_.push_back(delta);
        return SyncAction::none;
    }
    if (delta.final_update_id <= last_final_id_) {
        ++stats_.deltas_dropped;  // stale duplicate
        return SyncAction::none;
    }
    if (first_after_snapshot_) {
        return apply_first(delta);
    }
    // Rule 4: strictly contiguous.
    if (delta.first_update_id != last_final_id_ + 1) {
        ++stats_.gaps;
        desync();
        return SyncAction::resync_required;
    }
    book_.apply(delta);
    last_final_id_ = delta.final_update_id;
    ++stats_.deltas_applied;
    return SyncAction::applied;
}

}  // namespace tradebot::market_data
