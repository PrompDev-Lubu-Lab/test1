// Writes the same two-day synthetic ETHUSDT store the backtest tests use:
// a one-level book snapshot and one trade per minute, mid price following a
// sine wave with a 12-hour period around 3000. Usage: gen_store DATA_DIR
#include "tradebot/storage/event_store.hpp"
#include "tradebot/market_data/events.hpp"
#include <cmath>
#include <cstdio>
using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::storage;
using namespace tradebot::literals;
int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: gen_store DATA_DIR\n"); return 2; }
    const InstrumentId eth{1};
    const Timestamp day = *Timestamp::parse_iso8601("2024-03-15");
    EventStoreWriter w(StorePath{argv[1], "binance", "ETHUSDT"}, eth, DataSource::bulk);
    std::uint64_t id = 1;
    const int minutes = 2 * 24 * 60;
    for (int i = 0; i < minutes; ++i) {
        const double mid = 3000.0 + 100.0 * std::sin(2.0 * 3.14159265358979 * i / 720.0);
        const Timestamp t = day + Duration::minutes(i);
        BookSnapshot s;
        s.instrument = eth; s.recv_time = t; s.last_update_id = 1000 + i;
        s.bids = {{Price::from_double(mid - 0.5).round_to("0.01"_px, RoundingMode::down), "500"_qty}};
        s.asks = {{Price::from_double(mid + 0.5).round_to("0.01"_px, RoundingMode::up), "500"_qty}};
        if (!w.write(s)) { std::fprintf(stderr, "write snapshot failed\n"); return 1; }
        Trade tr;
        tr.instrument = eth; tr.exchange_time = t + Duration::seconds(1); tr.recv_time = tr.exchange_time;
        tr.id = TradeId{id++};
        tr.price = Price::from_double(mid).round_to("0.01"_px, RoundingMode::nearest);
        tr.quantity = "1"_qty; tr.aggressor = Side::buy;
        if (!w.write(tr)) { std::fprintf(stderr, "write trade failed\n"); return 1; }
    }
    if (!w.close()) { std::fprintf(stderr, "close failed\n"); return 1; }
    std::printf("wrote %d minutes\n", minutes);
    return 0;
}
