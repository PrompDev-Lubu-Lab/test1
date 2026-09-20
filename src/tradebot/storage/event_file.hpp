#pragma once

// Normalized event files ("TBEV" format, version 1).
//
// One file holds one stream kind (trades, book, tickers, candles) for one
// instrument and one UTC day. Records are packed little-endian into blocks
// of ~256 KiB, each block deflated independently and prefixed with its time
// range, so a reader can stream sequentially and skip whole blocks that end
// before the requested start. The 64-byte header carries counts and the
// time range, patched in on close, so a catalog can be built by reading
// headers alone.
//
// Layout
//   header   : 64 bytes (see FileHeader)
//   blocks   : [u32 raw_len][u32 comp_len][i64 first_time][i64 last_time][comp bytes]...
//   block raw: [u32 rec_len][u8 type][payload]...

#include "tradebot/core/error.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/market_data/events.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tradebot::storage {

enum class StreamKind : std::uint8_t { trades = 1, book = 2, tickers = 3, candles = 4 };
enum class DataSource : std::uint8_t { unknown = 0, live = 1, bulk = 2 };

[[nodiscard]] std::string_view to_string(StreamKind k) noexcept;
[[nodiscard]] Result<StreamKind> parse_stream_kind(std::string_view text);
[[nodiscard]] std::string_view to_string(DataSource s) noexcept;

// Which stream kind an event belongs in.
[[nodiscard]] StreamKind stream_kind_of(const market_data::MarketEvent& event) noexcept;

struct FileHeader {
    static constexpr std::uint16_t kVersion = 1;
    static constexpr std::size_t kSize = 64;

    std::uint16_t version = kVersion;
    StreamKind kind = StreamKind::trades;
    DataSource source = DataSource::unknown;
    std::uint32_t instrument_id = 0;
    std::uint64_t record_count = 0;
    Timestamp first_time;  // valid when record_count > 0
    Timestamp last_time;
    std::uint32_t gap_count = 0;  // validator-reported sequence gaps in this file
    std::uint32_t resync_count = 0;  // book resyncs (snapshots after a gap)
    std::int64_t candle_interval_ns = 0;  // candles only

    [[nodiscard]] std::vector<std::byte> encode() const;
    [[nodiscard]] static Result<FileHeader> decode(std::span<const std::byte> bytes);
};

class EventFileWriter {
public:
    EventFileWriter() = default;
    ~EventFileWriter();
    EventFileWriter(const EventFileWriter&) = delete;
    EventFileWriter& operator=(const EventFileWriter&) = delete;

    // Creates or truncates the file. Events must be written in
    // non-decreasing recv_time order (the replayer relies on it).
    [[nodiscard]] Result<void> open(const std::filesystem::path& path, StreamKind kind,
                                    InstrumentId instrument, DataSource source,
                                    Duration candle_interval = {});

    [[nodiscard]] Result<void> write(const market_data::MarketEvent& event);
    void note_gap() noexcept { ++header_.gap_count; }
    void note_resync() noexcept { ++header_.resync_count; }

    // Flushes the current block and patches the header. Idempotent.
    [[nodiscard]] Result<void> close();

    [[nodiscard]] const FileHeader& header() const noexcept { return header_; }
    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    [[nodiscard]] Result<void> flush_block();

    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
    FileHeader header_;
    std::vector<std::byte> block_;
    Timestamp block_first_;
    Timestamp block_last_;
    std::uint64_t block_records_ = 0;
    static constexpr std::size_t kBlockTarget = 256 * 1024;
};

class EventFileReader {
public:
    EventFileReader() = default;
    ~EventFileReader();
    EventFileReader(const EventFileReader&) = delete;
    EventFileReader& operator=(const EventFileReader&) = delete;

    [[nodiscard]] Result<void> open(const std::filesystem::path& path);

    // Reads only the header (for catalogs).
    [[nodiscard]] static Result<FileHeader> read_header(const std::filesystem::path& path);

    // Skips blocks that end before `from`; events before `from` inside a
    // block are still returned, so callers filter by time as well.
    void seek_from(Timestamp from) noexcept { from_ = from; }

    // False at end of file.
    [[nodiscard]] Result<bool> next(market_data::MarketEvent& out);

    [[nodiscard]] const FileHeader& header() const noexcept { return header_; }

private:
    [[nodiscard]] Result<bool> load_block();

    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
    FileHeader header_;
    std::vector<std::byte> block_;
    std::vector<std::byte> comp_;  // reused compressed-block buffer
    std::size_t pos_ = 0;
    Timestamp from_;
    bool eof_ = false;
};

// Record codec exposed for tests.
void encode_event(const market_data::MarketEvent& event, std::vector<std::byte>& out);
[[nodiscard]] Result<market_data::MarketEvent> decode_event(std::span<const std::byte> record,
                                                            std::size_t& consumed);

}  // namespace tradebot::storage
