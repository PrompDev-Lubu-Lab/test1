// tradebot-bench: throughput benchmarks for the hot paths.
//
//   tradebot-bench [--events N] [--quick]
//
// Generates a synthetic day of trades and book deltas, then measures event
// file write/read, order-book delta application, replay dispatch and a
// full backtest. Prints events per second for each stage. Used for
// tracking regressions; run in CI with --quick as a smoke test.

#include "tradebot/backtest/backtest.hpp"
#include "tradebot/market_data/order_book.hpp"
#include "tradebot/replay/event_source.hpp"
#include "tradebot/replay/replay_engine.hpp"
#include "tradebot/storage/event_store.hpp"
#include "tradebot/strategies/baselines.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

using namespace tradebot;
using namespace tradebot::market_data;
namespace fs = std::filesystem;

namespace {

const InstrumentId kEth{1};
const Timestamp kDay = *Timestamp::parse_iso8601("2024-03-15");

struct Timer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    [[nodiscard]] double seconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
};

void report(const char* name, std::uint64_t events, double seconds) {
    std::printf("%-36s %12llu events %9.3f s %14.0f events/s\n", name, static_cast<unsigned long long>(events),
                seconds, seconds > 0 ? static_cast<double>(events) / seconds : 0.0);
}

// Synthetic feed: a snapshot, then per step one delta touching 4 levels and
// one trade, prices random-walking around 3000.
std::vector<MarketEvent> synthetic(std::size_t steps, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> noise(0.0, 0.2);
    std::uniform_int_distribution<int> depth(0, 19);
    std::uniform_real_distribution<double> qty(0.0, 5.0);
    std::vector<MarketEvent> out;
    out.reserve(steps * 2 + 1);
    double mid = 3000.0;
    BookSnapshot snap;
    snap.instrument = kEth;
    snap.recv_time = kDay;
    snap.last_update_id = 1;
    for (int i = 0; i < 20; ++i) {
        snap.bids.push_back({Price::from_double(mid - 0.01 * (i + 1)).round_to(Price::from_raw(1'000'000), RoundingMode::down), Quantity::from_double(1.0 + i)});
        snap.asks.push_back({Price::from_double(mid + 0.01 * (i + 1)).round_to(Price::from_raw(1'000'000), RoundingMode::up), Quantity::from_double(1.0 + i)});
    }
    out.push_back(snap);
    std::int64_t update_id = 1;
    for (std::size_t s = 0; s < steps; ++s) {
        mid += noise(rng);
        const Timestamp t = kDay + Duration::millis(static_cast<std::int64_t>(s) * 100);
        BookDelta d;
        d.instrument = kEth;
        d.exchange_time = t;
        d.recv_time = t;
        d.first_update_id = update_id + 1;
        d.final_update_id = update_id + 4;
        update_id += 4;
        for (int k = 0; k < 2; ++k) {
            const int lvl = depth(rng);
            d.bids.push_back({Price::from_double(mid - 0.01 * (lvl + 1)).round_to(Price::from_raw(1'000'000), RoundingMode::down), Quantity::from_double(qty(rng))});
            d.asks.push_back({Price::from_double(mid + 0.01 * (lvl + 1)).round_to(Price::from_raw(1'000'000), RoundingMode::up), Quantity::from_double(qty(rng))});
        }
        out.push_back(std::move(d));
        Trade tr;
        tr.instrument = kEth;
        tr.exchange_time = t + Duration::millis(50);
        tr.recv_time = tr.exchange_time;
        tr.id = TradeId{s + 1};
        tr.price = Price::from_double(mid).round_to(Price::from_raw(1'000'000), RoundingMode::nearest);
        tr.quantity = Quantity::from_double(0.1 + qty(rng) / 10.0);
        tr.aggressor = (s % 2) ? Side::buy : Side::sell;
        out.push_back(tr);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t steps = 500'000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--quick") steps = 20'000;
        else if (a == "--events" && i + 1 < argc) steps = std::stoul(argv[++i]);
    }
    std::random_device rd;
    const fs::path tmp = fs::temp_directory_path() / ("tradebot-bench-" + std::to_string(rd()));
    fs::create_directories(tmp);
    const storage::StorePath store{tmp, "binance", "ETHUSDT"};

    std::printf("tradebot-bench: %zu steps (%zu events)\n\n", steps, steps * 2 + 1);
    auto events = synthetic(steps, 1);

    // 1. Event file write.
    {
        Timer t;
        storage::EventStoreWriter w(store, kEth, storage::DataSource::bulk);
        for (const auto& e : events) {
            if (auto r = w.write(e); !r) {
                std::fprintf(stderr, "write: %s\n", r.error().to_string().c_str());
                return 1;
            }
        }
        static_cast<void>(w.close());
        report("event store write", events.size(), t.seconds());
        std::uintmax_t bytes = 0;
        for (const auto& f : fs::recursive_directory_iterator(store.dir())) {
            if (f.is_regular_file()) bytes += f.file_size();
        }
        std::printf("  store size %.1f MB (%.1f bytes/event)\n", static_cast<double>(bytes) / 1e6,
                    static_cast<double>(bytes) / static_cast<double>(events.size()));
    }
    // 2. Event file read.
    {
        Timer t;
        auto src = replay::open_store_source(store, replay::StoreSelection{}, kDay, kDay + Duration::days(1));
        if (!src) return 1;
        MarketEvent ev;
        std::uint64_t n = 0;
        for (;;) {
            auto more = (*src)->next(ev);
            if (!more || !*more) break;
            ++n;
        }
        report("event store read + merge", n, t.seconds());
    }
    // 3. Order book updates.
    {
        Timer t;
        OrderBook book(kEth);
        std::uint64_t n = 0;
        for (const auto& e : events) {
            if (const auto* s = std::get_if<BookSnapshot>(&e)) book.apply(*s);
            else if (const auto* d = std::get_if<BookDelta>(&e)) { book.apply(*d); ++n; }
        }
        report("order book deltas", n, t.seconds());
    }
    // 4. Replay dispatch (venue + client bus, one subscriber each).
    {
        Timer t;
        replay::VectorEventSource src(events);
        SimClock clock(kDay);
        replay::JitterLatency latency({});
        replay::ReplayEngine engine(src, clock, latency, {.seed = 1});
        std::uint64_t seen = 0;
        engine.bus().subscribe([&](const MarketEvent&) { ++seen; });
        engine.venue_bus().subscribe([&](const MarketEvent&) { ++seen; });
        static_cast<void>(engine.run());
        report("replay dispatch (2 buses)", seen, t.seconds());
    }
    // 5. Full backtest.
    {
        strategy::StrategyRegistry registry;
        strategies::register_baselines(registry);
        auto cfg = Config::parse("[backtest]\nsymbol = ETHUSDT\nfrom = 2024-03-15\nto = 2024-03-16\ninitial_cash = 10000\n"
                                 "sample_interval = 1m\n[data]\ndir = " + tmp.string() +
                                 "\n[risk]\nmax_price_deviation = 0\n[strategy]\nname = ma_crossover\n[strategy.params]\ninterval = 1m\nfast = 5\nslow = 20\nquantity = 0.5\n");
        auto spec = backtest::parse_backtest_spec(*cfg);
        if (!spec) return 1;
        Timer t;
        auto result = backtest::run_backtest(*spec, registry, Logger{});
        if (!result) {
            std::fprintf(stderr, "backtest: %s\n", result.error().to_string().c_str());
            return 1;
        }
        report("full backtest (ma_crossover)", result->summary.events, t.seconds());
        std::printf("  fills %llu, orders %llu, final equity %s\n",
                    static_cast<unsigned long long>(result->summary.fills),
                    static_cast<unsigned long long>(result->summary.orders),
                    result->summary.final_equity.to_string().c_str());
    }
    fs::remove_all(tmp);
    return 0;
}
