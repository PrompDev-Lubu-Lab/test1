#include "tradebot/storage/event_file.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>

using namespace tradebot;
using namespace tradebot::market_data;
using namespace tradebot::storage;
using namespace tradebot::literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-store-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

const InstrumentId kEth{1};

Trade make_trade(std::int64_t ms, std::uint64_t id) {
    Trade t;
    t.instrument = kEth;
    t.exchange_time = Timestamp::from_millis(ms);
    t.recv_time = Timestamp::from_millis(ms + 2);
    t.id = TradeId{id};
    t.price = Price::from_raw(300000000000 + static_cast<std::int64_t>(id));
    t.quantity = "0.123"_qty;
    t.aggressor = id % 2 ? Side::buy : Side::sell;
    t.first_trade_id = static_cast<std::int64_t>(id * 3);
    t.last_trade_id = static_cast<std::int64_t>(id * 3 + 2);
    return t;
}

bool same(const Trade& a, const Trade& b) {
    return a.instrument == b.instrument && a.exchange_time == b.exchange_time &&
           a.recv_time == b.recv_time && a.id == b.id && a.price == b.price &&
           a.quantity == b.quantity && a.aggressor == b.aggressor &&
           a.first_trade_id == b.first_trade_id && a.last_trade_id == b.last_trade_id;
}

}  // namespace

TEST_CASE("event codec: every event type round-trips") {
    std::vector<MarketEvent> events;
    events.push_back(make_trade(1000, 7));
    BookSnapshot s{kEth, Timestamp::from_millis(1001), 99, {{"100"_px, "1"_qty}, {"99"_px, "2"_qty}}, {{"101"_px, "3"_qty}}};
    events.push_back(s);
    BookDelta d{kEth, Timestamp::from_millis(1002), Timestamp::from_millis(1003), 100, 105, {{"100"_px, "0"_qty}}, {}};
    events.push_back(d);
    BookTicker bt{kEth, Timestamp::from_millis(1004), 555, "100"_px, "1.5"_qty, "100.01"_px, "2.5"_qty};
    events.push_back(bt);
    Candle c;
    c.instrument = kEth;
    c.open_time = Timestamp::from_seconds(60);
    c.interval = Duration::minutes(1);
    c.recv_time = Timestamp::from_seconds(120);
    c.open = "1"_px; c.high = "2"_px; c.low = "0.5"_px; c.close = "1.5"_px;
    c.volume = "10"_qty; c.quote_volume = "12.5"_ntl; c.taker_buy_volume = "4"_qty;
    c.trade_count = 42; c.closed = true;
    events.push_back(c);
    events.push_back(Heartbeat{kEth, Timestamp::from_millis(1005)});

    std::vector<std::byte> buf;
    for (const auto& e : events) {
        encode_event(e, buf);
    }
    std::size_t pos = 0;
    std::vector<MarketEvent> back;
    while (pos < buf.size()) {
        std::size_t consumed = 0;
        auto ev = decode_event(std::span(buf).subspan(pos), consumed);
        REQUIRE_MESSAGE(ev.has_value(), ev.error().message);
        pos += consumed;
        back.push_back(*ev);
    }
    REQUIRE(back.size() == 6);
    CHECK(same(std::get<Trade>(back[0]), std::get<Trade>(events[0])));
    const auto& s2 = std::get<BookSnapshot>(back[1]);
    CHECK(s2.last_update_id == 99);
    REQUIRE(s2.bids.size() == 2);
    CHECK(s2.bids[1].price == "99"_px);
    CHECK(s2.asks[0].quantity == "3"_qty);
    const auto& d2 = std::get<BookDelta>(back[2]);
    CHECK(d2.first_update_id == 100);
    CHECK(d2.final_update_id == 105);
    CHECK(d2.bids[0].quantity.is_zero());
    CHECK(d2.asks.empty());
    const auto& bt2 = std::get<BookTicker>(back[3]);
    CHECK(bt2.update_id == 555);
    CHECK(bt2.ask_quantity == "2.5"_qty);
    const auto& c2 = std::get<Candle>(back[4]);
    CHECK(c2.interval == Duration::minutes(1));
    CHECK(c2.quote_volume == "12.5"_ntl);
    CHECK(c2.trade_count == 42);
    CHECK(c2.closed);
    CHECK(std::get<Heartbeat>(back[5]).recv_time == Timestamp::from_millis(1005));

    // Truncated / corrupt input is rejected, not misread.
    std::size_t consumed = 0;
    CHECK_FALSE(decode_event(std::span(buf).subspan(0, 3), consumed).has_value());
    CHECK_FALSE(decode_event(std::span(buf).subspan(0, 20), consumed).has_value());
    std::vector<std::byte> bad = buf;
    bad[4] = std::byte{99};  // unknown record type
    CHECK_FALSE(decode_event(bad, consumed).has_value());
}

TEST_CASE("FileHeader: encode/decode and version check") {
    FileHeader h;
    h.kind = StreamKind::candles;
    h.source = DataSource::bulk;
    h.instrument_id = 42;
    h.record_count = 12345;
    h.first_time = Timestamp::from_seconds(1);
    h.last_time = Timestamp::from_seconds(2);
    h.gap_count = 3;
    h.resync_count = 4;
    h.candle_interval_ns = Duration::minutes(5).count_nanos();
    auto bytes = h.encode();
    REQUIRE(bytes.size() == FileHeader::kSize);
    auto back = FileHeader::decode(bytes);
    REQUIRE(back.has_value());
    CHECK(back->kind == StreamKind::candles);
    CHECK(back->source == DataSource::bulk);
    CHECK(back->instrument_id == 42);
    CHECK(back->record_count == 12345);
    CHECK(back->last_time == Timestamp::from_seconds(2));
    CHECK(back->gap_count == 3);
    CHECK(back->resync_count == 4);
    CHECK(back->candle_interval_ns == Duration::minutes(5).count_nanos());

    bytes[0] = std::byte{'X'};
    CHECK_FALSE(FileHeader::decode(bytes).has_value());
    bytes = h.encode();
    bytes[4] = std::byte{9};  // version 9
    auto v = FileHeader::decode(bytes);
    REQUIRE_FALSE(v.has_value());
    CHECK(v.error().code == ErrorCode::unsupported);
    CHECK(*parse_stream_kind("book") == StreamKind::book);
    CHECK_FALSE(parse_stream_kind("nope").has_value());
}

TEST_CASE("EventFileWriter/Reader: many blocks, header patched, block skipping") {
    TempDir tmp;
    const fs::path file = tmp.path / "trades.tbev";
    constexpr int kCount = 50'000;  // ~3 MB raw => several blocks
    {
        EventFileWriter w;
        REQUIRE(w.open(file, StreamKind::trades, kEth, DataSource::live).has_value());
        for (int i = 0; i < kCount; ++i) {
            REQUIRE(w.write(make_trade(1'000'000 + i * 10, static_cast<std::uint64_t>(i))).has_value());
        }
        w.note_gap();
        REQUIRE(w.close().has_value());
        CHECK(w.header().record_count == kCount);
        REQUIRE(w.close().has_value());  // idempotent
    }
    auto hdr = EventFileReader::read_header(file);
    REQUIRE(hdr.has_value());
    CHECK(hdr->record_count == kCount);
    CHECK(hdr->kind == StreamKind::trades);
    CHECK(hdr->source == DataSource::live);
    CHECK(hdr->instrument_id == 1);
    CHECK(hdr->gap_count == 1);
    CHECK(hdr->first_time == Timestamp::from_millis(1'000'002));
    CHECK(hdr->last_time == Timestamp::from_millis(1'000'000 + (kCount - 1) * 10 + 2));
    CHECK(fs::file_size(file) < static_cast<std::uintmax_t>(kCount) * 30);  // compressed

    // Full read.
    {
        EventFileReader r;
        REQUIRE(r.open(file).has_value());
        MarketEvent ev;
        int n = 0;
        for (;;) {
            auto more = r.next(ev);
            REQUIRE(more.has_value());
            if (!*more) break;
            REQUIRE(same(std::get<Trade>(ev), make_trade(1'000'000 + n * 10, static_cast<std::uint64_t>(n))));
            ++n;
        }
        CHECK(n == kCount);
    }
    // Read from the middle: whole blocks before `from` are skipped.
    {
        EventFileReader r;
        REQUIRE(r.open(file).has_value());
        const Timestamp from = Timestamp::from_millis(1'000'000 + 40'000 * 10);
        r.seek_from(from);
        MarketEvent ev;
        int n = 0;
        Timestamp first;
        for (;;) {
            auto more = r.next(ev);
            REQUIRE(more.has_value());
            if (!*more) break;
            if (n == 0) first = event_time(ev);
            ++n;
        }
        CHECK(n < kCount);
        CHECK(n >= 10'000);
        CHECK(first <= from);
    }
    // Truncated file: the header still reads, the body errors cleanly.
    {
        const fs::path cut = tmp.path / "cut.tbev";
        std::string bytes(fs::file_size(file), '\0');
        std::ifstream in(file, std::ios::binary);
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::ofstream out(cut, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
        out.close();
        EventFileReader r;
        REQUIRE(r.open(cut).has_value());
        MarketEvent ev;
        bool failed = false;
        for (;;) {
            auto more = r.next(ev);
            if (!more) {
                failed = true;
                CHECK(more.error().code == ErrorCode::parse_error);
                break;
            }
            if (!*more) break;
        }
        CHECK(failed);
    }
    CHECK_FALSE(EventFileReader::read_header(tmp.path / "missing.tbev").has_value());
}

TEST_CASE("EventFileWriter: empty file has zero records and reads as empty") {
    TempDir tmp;
    const fs::path file = tmp.path / "empty.tbev";
    {
        EventFileWriter w;
        REQUIRE(w.open(file, StreamKind::book, kEth, DataSource::live).has_value());
        REQUIRE(w.close().has_value());
    }
    EventFileReader r;
    REQUIRE(r.open(file).has_value());
    CHECK(r.header().record_count == 0);
    MarketEvent ev;
    auto more = r.next(ev);
    REQUIRE(more.has_value());
    CHECK_FALSE(*more);
}
