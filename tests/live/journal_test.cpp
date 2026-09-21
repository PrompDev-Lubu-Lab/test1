#include "tradebot/live/journal.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>

using namespace tradebot;
using namespace tradebot::execution;
using namespace tradebot::live;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

ExecutionReport sample_report(bool with_fill) {
    ExecutionReport r;
    r.type = with_fill ? ReportType::fill : ReportType::accepted;
    r.client_id = ClientOrderId{42};
    r.order_id = OrderId{7};
    r.instrument = InstrumentId{1};
    r.strategy = StrategyId{3};
    r.side = Side::sell;
    r.order_type = OrderType::limit;
    r.price = "3000.5"_px;
    r.time = *Timestamp::parse_iso8601("2024-03-15T10:00:00.123456789Z");
    r.status = with_fill ? OrderStatus::partially_filled : OrderStatus::open;
    r.filled_quantity = with_fill ? "0.25"_qty : Quantity{};
    r.remaining_quantity = with_fill ? "0.75"_qty : "1"_qty;
    if (with_fill) {
        r.fill = Fill{"3000.5"_px, "0.25"_qty, "0.75"_ntl, Liquidity::maker, TradeId{99}};
    }
    r.reason = with_fill ? "" : "why, not";
    return r;
}

}  // namespace

TEST_CASE("journal: encode/decode round trip") {
    for (bool with_fill : {false, true}) {
        const auto r = sample_report(with_fill);
        auto back = decode_report(encode_report(r));
        REQUIRE_MESSAGE(back.has_value(), back.error().message);
        CHECK(back->type == r.type);
        CHECK(back->client_id == r.client_id);
        CHECK(back->order_id == r.order_id);
        CHECK(back->instrument == r.instrument);
        CHECK(back->strategy == r.strategy);
        CHECK(back->side == r.side);
        CHECK(back->order_type == r.order_type);
        CHECK(back->price == r.price);
        CHECK(back->time == r.time);
        CHECK(back->status == r.status);
        CHECK(back->filled_quantity == r.filled_quantity);
        CHECK(back->remaining_quantity == r.remaining_quantity);
        CHECK(back->reason == r.reason);
        REQUIRE(back->fill.has_value() == with_fill);
        if (with_fill) {
            CHECK(back->fill->price == r.fill->price);
            CHECK(back->fill->quantity == r.fill->quantity);
            CHECK(back->fill->fee == r.fill->fee);
            CHECK(back->fill->liquidity == Liquidity::maker);
            CHECK(back->fill->exec_id == TradeId{99});
        }
    }
    // The encoded time is the ISO-8601 string, never an integer that a JSON
    // client would round.
    {
        const auto line = encode_report(sample_report(false));
        CHECK(line.find("\"time\":\"2024-03-15T10:00:00.123456789Z\"") != std::string::npos);
    }
    // Integer nanoseconds (the pre-2026-09-21 encoding) still decode.
    {
        const auto r = sample_report(false);
        auto line = encode_report(r);
        const auto pos = line.find("\"time\":\"");
        REQUIRE(pos != std::string::npos);
        const auto end = line.find('"', pos + 8);
        line.replace(pos, end + 1 - pos, "\"time\":" + std::to_string(r.time.nanos_since_epoch()));
        auto back = decode_report(line);
        REQUIRE_MESSAGE(back.has_value(), back.error().message);
        CHECK(back->time == r.time);
    }
    CHECK_FALSE(decode_report(R"({"type":"fill","side":"buy","order_type":"limit","status":"open","price":"1","filled":"0","remaining":"1","client_id":1,"time":"yesterday"})").has_value());
    CHECK_FALSE(decode_report("not json").has_value());
    CHECK_FALSE(decode_report(R"({"type":"fill"})").has_value());
    CHECK_FALSE(decode_report(R"({"type":"teleport","side":"buy","order_type":"limit","status":"open","price":"1","filled":"0","remaining":"1","client_id":1,"time":1})").has_value());
}

TEST_CASE("journal: append, replay, skip corrupt lines") {
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("tradebot-journal-" + std::to_string(rd()));
    const fs::path file = dir / "sub" / "journal.jsonl";
    {
        Journal j;
        REQUIRE(j.open(file).has_value());
        REQUIRE(j.append(sample_report(false)).has_value());
        REQUIRE(j.append(sample_report(true), false).has_value());
        CHECK(j.appended() == 2);
        REQUIRE(j.close().has_value());
    }
    {
        std::ofstream out(file, std::ios::app);
        out << "garbage line\n";
    }
    {
        Journal j;  // reopening appends
        REQUIRE(j.open(file).has_value());
        REQUIRE(j.append(sample_report(true)).has_value());
    }
    std::vector<ExecutionReport> seen;
    auto skipped = Journal::replay(file, [&](const ExecutionReport& r) { seen.push_back(r); });
    REQUIRE_MESSAGE(skipped.has_value(), skipped.error().message);
    CHECK(*skipped == 1);
    REQUIRE(seen.size() == 3);
    CHECK(seen[0].type == ReportType::accepted);
    CHECK(seen[1].type == ReportType::fill);
    CHECK(seen[2].fill->exec_id == TradeId{99});
    CHECK_FALSE(Journal::replay(dir / "missing.jsonl", [](const ExecutionReport&) {}).has_value());
    Journal closed;
    CHECK_FALSE(closed.append(sample_report(false)).has_value());
    fs::remove_all(dir);
}
