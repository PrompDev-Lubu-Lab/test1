#include "tradebot/storage/event_file.hpp"

#include <zlib.h>

#include <cstring>

namespace tradebot::storage {

using market_data::BookDelta;
using market_data::BookLevel;
using market_data::BookSnapshot;
using market_data::BookTicker;
using market_data::Candle;
using market_data::Heartbeat;
using market_data::MarketEvent;
using market_data::Trade;

namespace {

constexpr char kMagic[4] = {'T', 'B', 'E', 'V'};
constexpr std::size_t kBlockHeaderSize = 4 + 4 + 8 + 8;

// --- little-endian byte helpers ---------------------------------------------

class Writer {
public:
    explicit Writer(std::vector<std::byte>& out) : out_(out) {}
    void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) { raw(&v, 2); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }
    void i64(std::int64_t v) { raw(&v, 8); }
    void time(Timestamp t) { i64(t.nanos_since_epoch()); }
    void fixed(std::int64_t raw_value) { i64(raw_value); }
    void level(const BookLevel& l) {
        fixed(l.price.raw());
        fixed(l.quantity.raw());
    }

private:
    void raw(const void* p, std::size_t n) {
        // Host is little-endian on every platform we target; assert once.
        static_assert(std::endian::native == std::endian::little);
        const auto* b = static_cast<const std::byte*>(p);
        out_.insert(out_.end(), b, b + n);
    }
    std::vector<std::byte>& out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> in) : in_(in) {}
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
    std::uint8_t u8() {
        std::uint8_t v = 0;
        raw(&v, 1);
        return v;
    }
    std::uint16_t u16() {
        std::uint16_t v = 0;
        raw(&v, 2);
        return v;
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        raw(&v, 4);
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t v = 0;
        raw(&v, 8);
        return v;
    }
    std::int64_t i64() {
        std::int64_t v = 0;
        raw(&v, 8);
        return v;
    }
    Timestamp time() { return Timestamp::from_nanos(i64()); }
    BookLevel level() {
        BookLevel l;
        l.price = Price::from_raw(i64());
        l.quantity = Quantity::from_raw(i64());
        return l;
    }
    void skip(std::size_t n) {
        if (pos_ + n > in_.size()) {
            ok_ = false;
            pos_ = in_.size();
        } else {
            pos_ += n;
        }
    }

private:
    void raw(void* p, std::size_t n) {
        if (pos_ + n > in_.size()) {
            ok_ = false;
            std::memset(p, 0, n);
            return;
        }
        std::memcpy(p, in_.data() + pos_, n);
        pos_ += n;
    }
    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

enum class RecordType : std::uint8_t {
    trade = 1,
    book_snapshot = 2,
    book_delta = 3,
    book_ticker = 4,
    candle = 5,
    heartbeat = 6,
};

// zlib sizes: uLong is size_t-sized on Linux but not everywhere; a helper
// avoids a cast that some platforms need and others call useless.
uLong zsize(std::size_t n) noexcept { return n; }

Unexpected<Error> corrupt(const std::string& what) {
    return make_error(ErrorCode::parse_error, "event file: " + what);
}

Unexpected<Error> io(const std::filesystem::path& p, const char* what) {
    return make_error(ErrorCode::io_error, p.string() + ": " + what);
}

}  // namespace

// --- names ------------------------------------------------------------------

std::string_view to_string(StreamKind k) noexcept {
    switch (k) {
        case StreamKind::trades: return "trades";
        case StreamKind::book: return "book";
        case StreamKind::tickers: return "tickers";
        case StreamKind::candles: return "candles";
    }
    return "?";
}

Result<StreamKind> parse_stream_kind(std::string_view text) {
    if (text == "trades") return StreamKind::trades;
    if (text == "book") return StreamKind::book;
    if (text == "tickers") return StreamKind::tickers;
    if (text == "candles") return StreamKind::candles;
    return make_error(ErrorCode::parse_error, "unknown stream kind '" + std::string(text) + "'");
}

std::string_view to_string(DataSource s) noexcept {
    switch (s) {
        case DataSource::unknown: return "unknown";
        case DataSource::live: return "live";
        case DataSource::bulk: return "bulk";
    }
    return "?";
}

StreamKind stream_kind_of(const MarketEvent& event) noexcept {
    switch (event.index()) {
        case 0: return StreamKind::trades;
        case 1:
        case 2: return StreamKind::book;
        case 3: return StreamKind::tickers;
        case 4: return StreamKind::candles;
        default: return StreamKind::trades;  // heartbeats ride along with trades
    }
}

// --- header -----------------------------------------------------------------

std::vector<std::byte> FileHeader::encode() const {
    std::vector<std::byte> out;
    out.reserve(kSize);
    Writer w(out);
    for (char c : kMagic) w.u8(static_cast<std::uint8_t>(c));
    w.u16(version);
    w.u8(static_cast<std::uint8_t>(kind));
    w.u8(static_cast<std::uint8_t>(source));
    w.u32(instrument_id);
    w.u64(record_count);
    w.time(first_time);
    w.time(last_time);
    w.u32(gap_count);
    w.u32(resync_count);
    w.i64(candle_interval_ns);
    out.resize(kSize, std::byte{0});
    return out;
}

Result<FileHeader> FileHeader::decode(std::span<const std::byte> bytes) {
    if (bytes.size() < kSize) {
        return corrupt("header too short");
    }
    Reader r(bytes);
    for (char c : kMagic) {
        if (r.u8() != static_cast<std::uint8_t>(c)) {
            return corrupt("bad magic");
        }
    }
    FileHeader h;
    h.version = r.u16();
    if (h.version != kVersion) {
        return make_error(ErrorCode::unsupported,
                          "event file version " + std::to_string(h.version) + " not supported");
    }
    h.kind = static_cast<StreamKind>(r.u8());
    h.source = static_cast<DataSource>(r.u8());
    h.instrument_id = r.u32();
    h.record_count = r.u64();
    h.first_time = r.time();
    h.last_time = r.time();
    h.gap_count = r.u32();
    h.resync_count = r.u32();
    h.candle_interval_ns = r.i64();
    if (static_cast<std::uint8_t>(h.kind) < 1 || static_cast<std::uint8_t>(h.kind) > 4) {
        return corrupt("bad stream kind");
    }
    return h;
}

// --- record codec -----------------------------------------------------------

void encode_event(const MarketEvent& event, std::vector<std::byte>& out) {
    const std::size_t start = out.size();
    Writer w(out);
    w.u32(0);  // length placeholder
    std::visit(
        [&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, Trade>) {
                w.u8(static_cast<std::uint8_t>(RecordType::trade));
                w.u32(e.instrument.value());
                w.time(e.exchange_time);
                w.time(e.recv_time);
                w.u64(e.id.value());
                w.fixed(e.price.raw());
                w.fixed(e.quantity.raw());
                w.u8(static_cast<std::uint8_t>(e.aggressor));
                w.i64(e.first_trade_id);
                w.i64(e.last_trade_id);
            } else if constexpr (std::is_same_v<T, BookSnapshot>) {
                w.u8(static_cast<std::uint8_t>(RecordType::book_snapshot));
                w.u32(e.instrument.value());
                w.time(e.recv_time);
                w.i64(e.last_update_id);
                w.u32(static_cast<std::uint32_t>(e.bids.size()));
                w.u32(static_cast<std::uint32_t>(e.asks.size()));
                for (const auto& l : e.bids) w.level(l);
                for (const auto& l : e.asks) w.level(l);
            } else if constexpr (std::is_same_v<T, BookDelta>) {
                w.u8(static_cast<std::uint8_t>(RecordType::book_delta));
                w.u32(e.instrument.value());
                w.time(e.exchange_time);
                w.time(e.recv_time);
                w.i64(e.first_update_id);
                w.i64(e.final_update_id);
                w.u32(static_cast<std::uint32_t>(e.bids.size()));
                w.u32(static_cast<std::uint32_t>(e.asks.size()));
                for (const auto& l : e.bids) w.level(l);
                for (const auto& l : e.asks) w.level(l);
            } else if constexpr (std::is_same_v<T, BookTicker>) {
                w.u8(static_cast<std::uint8_t>(RecordType::book_ticker));
                w.u32(e.instrument.value());
                w.time(e.recv_time);
                w.i64(e.update_id);
                w.fixed(e.bid_price.raw());
                w.fixed(e.bid_quantity.raw());
                w.fixed(e.ask_price.raw());
                w.fixed(e.ask_quantity.raw());
            } else if constexpr (std::is_same_v<T, Candle>) {
                w.u8(static_cast<std::uint8_t>(RecordType::candle));
                w.u32(e.instrument.value());
                w.time(e.open_time);
                w.i64(e.interval.count_nanos());
                w.time(e.recv_time);
                w.fixed(e.open.raw());
                w.fixed(e.high.raw());
                w.fixed(e.low.raw());
                w.fixed(e.close.raw());
                w.fixed(e.volume.raw());
                w.fixed(e.quote_volume.raw());
                w.fixed(e.taker_buy_volume.raw());
                w.i64(e.trade_count);
                w.u8(e.closed ? 1 : 0);
            } else {
                w.u8(static_cast<std::uint8_t>(RecordType::heartbeat));
                w.u32(e.instrument.value());
                w.time(e.recv_time);
            }
        },
        event);
    const auto len = static_cast<std::uint32_t>(out.size() - start - 4);
    std::memcpy(out.data() + start, &len, 4);
}

Result<MarketEvent> decode_event(std::span<const std::byte> bytes, std::size_t& consumed) {
    Reader r(bytes);
    const std::uint32_t len = r.u32();
    if (!r.ok() || 4 + static_cast<std::size_t>(len) > bytes.size() || len < 5) {
        return corrupt("bad record length");
    }
    consumed = 4 + len;
    const auto type = static_cast<RecordType>(r.u8());
    const InstrumentId instrument{r.u32()};
    MarketEvent out;
    switch (type) {
        case RecordType::trade: {
            Trade t;
            t.instrument = instrument;
            t.exchange_time = r.time();
            t.recv_time = r.time();
            t.id = TradeId{r.u64()};
            t.price = Price::from_raw(r.i64());
            t.quantity = Quantity::from_raw(r.i64());
            t.aggressor = static_cast<Side>(r.u8());
            t.first_trade_id = r.i64();
            t.last_trade_id = r.i64();
            out = t;
            break;
        }
        case RecordType::book_snapshot: {
            BookSnapshot s;
            s.instrument = instrument;
            s.recv_time = r.time();
            s.last_update_id = r.i64();
            const std::uint32_t nb = r.u32();
            const std::uint32_t na = r.u32();
            if (!r.ok() || (static_cast<std::size_t>(nb) + na) * 16 > len) {
                return corrupt("bad snapshot level count");
            }
            s.bids.reserve(nb);
            s.asks.reserve(na);
            for (std::uint32_t i = 0; i < nb; ++i) s.bids.push_back(r.level());
            for (std::uint32_t i = 0; i < na; ++i) s.asks.push_back(r.level());
            out = std::move(s);
            break;
        }
        case RecordType::book_delta: {
            BookDelta d;
            d.instrument = instrument;
            d.exchange_time = r.time();
            d.recv_time = r.time();
            d.first_update_id = r.i64();
            d.final_update_id = r.i64();
            const std::uint32_t nb = r.u32();
            const std::uint32_t na = r.u32();
            if (!r.ok() || (static_cast<std::size_t>(nb) + na) * 16 > len) {
                return corrupt("bad delta level count");
            }
            d.bids.reserve(nb);
            d.asks.reserve(na);
            for (std::uint32_t i = 0; i < nb; ++i) d.bids.push_back(r.level());
            for (std::uint32_t i = 0; i < na; ++i) d.asks.push_back(r.level());
            out = std::move(d);
            break;
        }
        case RecordType::book_ticker: {
            BookTicker t;
            t.instrument = instrument;
            t.recv_time = r.time();
            t.update_id = r.i64();
            t.bid_price = Price::from_raw(r.i64());
            t.bid_quantity = Quantity::from_raw(r.i64());
            t.ask_price = Price::from_raw(r.i64());
            t.ask_quantity = Quantity::from_raw(r.i64());
            out = t;
            break;
        }
        case RecordType::candle: {
            Candle c;
            c.instrument = instrument;
            c.open_time = r.time();
            c.interval = Duration::nanos(r.i64());
            c.recv_time = r.time();
            c.open = Price::from_raw(r.i64());
            c.high = Price::from_raw(r.i64());
            c.low = Price::from_raw(r.i64());
            c.close = Price::from_raw(r.i64());
            c.volume = Quantity::from_raw(r.i64());
            c.quote_volume = Notional::from_raw(r.i64());
            c.taker_buy_volume = Quantity::from_raw(r.i64());
            c.trade_count = r.i64();
            c.closed = r.u8() != 0;
            out = c;
            break;
        }
        case RecordType::heartbeat: {
            Heartbeat h;
            h.instrument = instrument;
            h.recv_time = r.time();
            out = h;
            break;
        }
        default:
            return corrupt("unknown record type " + std::to_string(static_cast<int>(type)));
    }
    if (!r.ok() || r.pos() != consumed) {
        return corrupt("record length mismatch");
    }
    return out;
}

// --- writer -----------------------------------------------------------------

EventFileWriter::~EventFileWriter() { static_cast<void>(close()); }

Result<void> EventFileWriter::open(const std::filesystem::path& path, StreamKind kind,
                                   InstrumentId instrument, DataSource source,
                                   Duration candle_interval) {
    if (auto r = close(); !r) {
        return r;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr) {
        return io(path, "cannot create");
    }
    path_ = path;
    header_ = FileHeader{};
    header_.kind = kind;
    header_.source = source;
    header_.instrument_id = instrument.value();
    header_.candle_interval_ns = candle_interval.count_nanos();
    block_.clear();
    block_records_ = 0;
    const auto hdr = header_.encode();
    if (std::fwrite(hdr.data(), 1, hdr.size(), file_) != hdr.size()) {
        return io(path, "cannot write header");
    }
    return {};
}

Result<void> EventFileWriter::write(const MarketEvent& event) {
    if (file_ == nullptr) {
        return make_error(ErrorCode::invalid_state, "event file writer not open");
    }
    const Timestamp t = market_data::event_time(event);
    if (block_records_ == 0) {
        block_first_ = t;
    }
    block_last_ = t;
    encode_event(event, block_);
    ++block_records_;
    if (header_.record_count == 0) {
        header_.first_time = t;
    }
    header_.last_time = t;
    ++header_.record_count;
    if (block_.size() >= kBlockTarget) {
        return flush_block();
    }
    return {};
}

Result<void> EventFileWriter::flush_block() {
    if (block_.empty()) {
        return {};
    }
    uLongf comp_len = compressBound(zsize(block_.size()));
    std::vector<std::byte> comp(comp_len);
    // Z_BEST_SPEED: ~3x faster than the default level for ~6% larger files
    // on real market data; ingest throughput matters more than disk here.
    if (compress2(reinterpret_cast<Bytef*>(comp.data()), &comp_len,
                  reinterpret_cast<const Bytef*>(block_.data()), zsize(block_.size()),
                  Z_BEST_SPEED) != Z_OK) {
        return io(path_, "compress failed");
    }
    std::vector<std::byte> hdr;
    Writer w(hdr);
    w.u32(static_cast<std::uint32_t>(block_.size()));
    w.u32(static_cast<std::uint32_t>(comp_len));
    w.time(block_first_);
    w.time(block_last_);
    if (std::fwrite(hdr.data(), 1, hdr.size(), file_) != hdr.size() ||
        std::fwrite(comp.data(), 1, comp_len, file_) != comp_len) {
        return io(path_, "write failed");
    }
    block_.clear();
    block_records_ = 0;
    return {};
}

Result<void> EventFileWriter::close() {
    if (file_ == nullptr) {
        return {};
    }
    auto r = flush_block();
    if (r) {
        const auto hdr = header_.encode();
        if (std::fseek(file_, 0, SEEK_SET) != 0 ||
            std::fwrite(hdr.data(), 1, hdr.size(), file_) != hdr.size()) {
            r = io(path_, "cannot patch header");
        }
    }
    if (std::fclose(file_) != 0 && r) {
        r = io(path_, "close failed");
    }
    file_ = nullptr;
    return r;
}

// --- reader -----------------------------------------------------------------

EventFileReader::~EventFileReader() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

Result<FileHeader> EventFileReader::read_header(const std::filesystem::path& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return io(path, "cannot open");
    }
    std::byte buf[FileHeader::kSize];
    const std::size_t n = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    if (n != sizeof buf) {
        return io(path, "truncated header");
    }
    return FileHeader::decode(buf);
}

Result<void> EventFileReader::open(const std::filesystem::path& path) {
    auto hdr = read_header(path);
    if (!hdr) {
        return tl::make_unexpected(hdr.error());
    }
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
        return io(path, "cannot open");
    }
    std::fseek(file_, static_cast<long>(FileHeader::kSize), SEEK_SET);
    path_ = path;
    header_ = *hdr;
    block_.clear();
    pos_ = 0;
    eof_ = false;
    return {};
}

Result<bool> EventFileReader::load_block() {
    for (;;) {
        std::byte hdr[kBlockHeaderSize];
        const std::size_t n = std::fread(hdr, 1, sizeof hdr, file_);
        if (n == 0) {
            eof_ = true;
            return false;
        }
        if (n != sizeof hdr) {
            return corrupt("truncated block header in " + path_.string());
        }
        Reader r(hdr);
        const std::uint32_t raw_len = r.u32();
        const std::uint32_t comp_len = r.u32();
        r.time();  // first
        const Timestamp last = r.time();
        if (last < from_) {
            if (std::fseek(file_, static_cast<long>(comp_len), SEEK_CUR) != 0) {
                return corrupt("cannot skip block in " + path_.string());
            }
            continue;
        }
        comp_.resize(comp_len);
        if (std::fread(comp_.data(), 1, comp_len, file_) != comp_len) {
            return corrupt("truncated block in " + path_.string());
        }
        block_.resize(raw_len);
        uLongf out_len = raw_len;
        if (uncompress(reinterpret_cast<Bytef*>(block_.data()), &out_len,
                       reinterpret_cast<const Bytef*>(comp_.data()), comp_len) != Z_OK ||
            out_len != raw_len) {
            return corrupt("bad block in " + path_.string());
        }
        pos_ = 0;
        return true;
    }
}

Result<bool> EventFileReader::next(MarketEvent& out) {
    if (file_ == nullptr) {
        return make_error(ErrorCode::invalid_state, "event file reader not open");
    }
    while (pos_ >= block_.size()) {
        if (eof_) {
            return false;
        }
        auto loaded = load_block();
        if (!loaded) {
            return tl::make_unexpected(loaded.error());
        }
        if (!*loaded) {
            return false;
        }
    }
    std::size_t consumed = 0;
    auto ev = decode_event(std::span(block_).subspan(pos_), consumed);
    if (!ev) {
        return tl::make_unexpected(ev.error());
    }
    pos_ += consumed;
    out = std::move(*ev);
    return true;
}

}  // namespace tradebot::storage
