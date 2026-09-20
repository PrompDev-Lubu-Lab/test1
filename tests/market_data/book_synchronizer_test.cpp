#include "tradebot/market_data/book_synchronizer.hpp"

#include <doctest/doctest.h>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::literals;

namespace {

const InstrumentId kEth{1};

BookSnapshot snapshot(std::int64_t id) {
    BookSnapshot s;
    s.instrument = kEth;
    s.recv_time = Timestamp::from_seconds(1);
    s.last_update_id = id;
    s.bids = {{"100"_px, "1"_qty}};
    s.asks = {{"101"_px, "1"_qty}};
    return s;
}

BookDelta delta(std::int64_t first, std::int64_t last, const char* bid_qty = "1") {
    BookDelta d;
    d.instrument = kEth;
    d.exchange_time = Timestamp::from_seconds(2);
    d.recv_time = Timestamp::from_seconds(2);
    d.first_update_id = first;
    d.final_update_id = last;
    d.bids = {{"100"_px, *Quantity::parse(bid_qty)}};
    return d;
}

}  // namespace

TEST_CASE("BookSynchronizer: buffers before snapshot, drops stale, applies bracketed and contiguous") {
    BookSynchronizer sync(kEth);
    CHECK_FALSE(sync.synced());

    CHECK(sync.on_delta(delta(90, 95, "2")) == SyncAction::none);  // stale: u <= 100
    CHECK(sync.on_delta(delta(96, 100, "3")) == SyncAction::none);  // stale: u == 100
    CHECK(sync.on_delta(delta(98, 103, "4")) == SyncAction::none);  // brackets 101
    CHECK(sync.on_delta(delta(104, 110, "5")) == SyncAction::none);  // contiguous
    CHECK(sync.buffered() == 4);

    CHECK(sync.on_snapshot(snapshot(100)) == SyncAction::applied);
    CHECK(sync.synced());
    CHECK(sync.buffered() == 0);
    CHECK(sync.book().last_update_id() == 110);
    CHECK(sync.book().quantity_at(Side::buy, "100"_px) == "5"_qty);
    CHECK(sync.stats().deltas_dropped == 2);
    CHECK(sync.stats().deltas_applied == 2);
    CHECK(sync.stats().gaps == 0);

    // Live continuation.
    CHECK(sync.on_delta(delta(111, 111, "6")) == SyncAction::applied);
    CHECK(sync.on_delta(delta(111, 111, "7")) == SyncAction::none);  // duplicate
    CHECK(sync.book().quantity_at(Side::buy, "100"_px) == "6"_qty);

    // Gap: 113 != 112.
    CHECK(sync.on_delta(delta(113, 114, "8")) == SyncAction::resync_required);
    CHECK_FALSE(sync.synced());
    CHECK(sync.book().empty());
    CHECK(sync.stats().gaps == 1);

    // Deltas buffer again until the next snapshot; snapshot after the gap.
    CHECK(sync.on_delta(delta(115, 116, "9")) == SyncAction::none);
    CHECK(sync.on_snapshot(snapshot(115)) == SyncAction::applied);
    CHECK(sync.synced());
    CHECK(sync.book().last_update_id() == 116);
    CHECK(sync.book().quantity_at(Side::buy, "100"_px) == "9"_qty);
}

TEST_CASE("BookSynchronizer: snapshot with empty buffer, first delta must bracket") {
    BookSynchronizer sync(kEth);
    CHECK(sync.on_snapshot(snapshot(50)) == SyncAction::applied);
    CHECK(sync.synced());
    CHECK(sync.on_delta(delta(40, 50)) == SyncAction::none);  // stale
    CHECK(sync.on_delta(delta(53, 55)) == SyncAction::resync_required);  // U > 51: gap
    CHECK(sync.stats().gaps == 1);

    CHECK(sync.on_snapshot(snapshot(60)) == SyncAction::applied);
    CHECK(sync.on_delta(delta(58, 62)) == SyncAction::applied);  // U <= 61 <= u
    CHECK(sync.on_delta(delta(63, 63)) == SyncAction::applied);
    CHECK(sync.book().last_update_id() == 63);
}

TEST_CASE("BookSynchronizer: buffered events that do not reach the snapshot cause resync") {
    BookSynchronizer sync(kEth);
    CHECK(sync.on_delta(delta(1, 5)) == SyncAction::none);
    CHECK(sync.on_delta(delta(6, 9)) == SyncAction::none);
    // Snapshot at 20: all buffered are stale (dropped); synced, waiting for first delta.
    CHECK(sync.on_snapshot(snapshot(20)) == SyncAction::applied);
    CHECK(sync.synced());
    CHECK(sync.stats().deltas_dropped == 2);
    // Snapshot at 3 with buffer starting at 1..5 (brackets 4) then 6..9 contiguous.
    BookSynchronizer sync2(kEth);
    CHECK(sync2.on_delta(delta(1, 5)) == SyncAction::none);
    CHECK(sync2.on_delta(delta(6, 9)) == SyncAction::none);
    CHECK(sync2.on_delta(delta(12, 13)) == SyncAction::none);  // gap in buffer
    CHECK(sync2.on_snapshot(snapshot(3)) == SyncAction::resync_required);
    CHECK_FALSE(sync2.synced());
    CHECK(sync2.stats().deltas_applied == 2);
    CHECK(sync2.stats().gaps == 1);
}

TEST_CASE("BookSynchronizer: buffer overflow drops oldest") {
    BookSynchronizer sync(kEth, 3);
    for (int i = 1; i <= 5; ++i) {
        CHECK(sync.on_delta(delta(i, i)) == SyncAction::none);
    }
    CHECK(sync.buffered() == 3);
    CHECK(sync.stats().buffer_overflows == 2);
    // Snapshot at 2: remaining buffer is 3,4,5 -> 3 brackets 3.
    CHECK(sync.on_snapshot(snapshot(2)) == SyncAction::applied);
    CHECK(sync.book().last_update_id() == 5);
}
