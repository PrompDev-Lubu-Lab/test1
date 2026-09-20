#pragma once

// Raw market-data capture.
//
// Everything received from a venue is written to disk exactly as received,
// tagged with the local receive time and the stream it arrived on, before
// any parsing. These archives are the ground truth: every normalized
// dataset can be rebuilt from them, and a bug in a parser never costs data.
//
// Layout:  <root>/raw/<venue>/<symbol>/<YYYY-MM-DD>/<HH>.jsonl.gz
// Record:  {"t":<recv_ns>,"s":"<stream>","d":<payload>}\n
//
// Files rotate on the UTC hour of the receive time. Reopening an existing
// hour appends a new gzip member, which every gzip reader handles, so a
// collector restart never overwrites data.

#include "tradebot/core/clock.hpp"
#include "tradebot/core/error.hpp"
#include "tradebot/core/time.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

typedef struct gzFile_s* gzFile;

namespace tradebot::market_data {

struct RawRecord {
    Timestamp recv_time;
    std::string stream;  // e.g. "ethusdt@aggTrade", "depth_snapshot"
    std::string payload;  // a complete JSON value, verbatim
};

struct RawCapturePath {
    std::filesystem::path root;
    std::string venue;
    std::string symbol;

    [[nodiscard]] std::filesystem::path dir() const;
    [[nodiscard]] std::filesystem::path file_for_hour(Timestamp hour_start) const;
    // All hour files whose hour intersects [from, to), in time order.
    [[nodiscard]] std::vector<std::filesystem::path> files_in_range(Timestamp from,
                                                                    Timestamp to) const;
    // Parses "<dir>/YYYY-MM-DD/HH.jsonl.gz" back into its hour start.
    [[nodiscard]] static Result<Timestamp> hour_of(const std::filesystem::path& file);
};

class RawCaptureWriter {
public:
    struct Stats {
        std::uint64_t records = 0;
        std::uint64_t bytes = 0;  // uncompressed
        std::uint64_t rotations = 0;
    };

    explicit RawCaptureWriter(RawCapturePath path);
    ~RawCaptureWriter();
    RawCaptureWriter(const RawCaptureWriter&) = delete;
    RawCaptureWriter& operator=(const RawCaptureWriter&) = delete;

    [[nodiscard]] Result<void> write(const RawRecord& record);

    // Pushes buffered data through zlib and to the OS. Called periodically
    // by the collector so a crash loses at most the flush interval.
    [[nodiscard]] Result<void> flush();
    [[nodiscard]] Result<void> close();

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::filesystem::path current_file() const { return current_file_; }

private:
    Result<void> open_for(Timestamp hour_start);

    RawCapturePath path_;
    gzFile file_ = nullptr;
    std::filesystem::path current_file_;
    Timestamp current_hour_;
    bool has_hour_ = false;
    Stats stats_;
};

class RawCaptureReader {
public:
    explicit RawCaptureReader(std::filesystem::path file);
    ~RawCaptureReader();
    RawCaptureReader(const RawCaptureReader&) = delete;
    RawCaptureReader& operator=(const RawCaptureReader&) = delete;

    [[nodiscard]] Result<void> open();

    // Reads the next record. Returns false at end of file. A malformed line
    // is skipped and counted, not fatal: one bad byte must not hide an hour
    // of data.
    [[nodiscard]] Result<bool> next(RawRecord& out);

    [[nodiscard]] std::uint64_t skipped_lines() const noexcept { return skipped_; }

private:
    std::filesystem::path file_;
    gzFile gz_ = nullptr;
    std::string buffer_;
    std::size_t pos_ = 0;
    bool eof_ = false;
    std::uint64_t skipped_ = 0;
};

// Serialization helpers exposed for tests and tooling.
[[nodiscard]] std::string encode_raw_record(const RawRecord& record);
[[nodiscard]] Result<RawRecord> decode_raw_record(std::string_view line);

// Streams all records in [from, to) across hour files in order.
[[nodiscard]] Result<std::uint64_t> for_each_raw_record(
    const RawCapturePath& path, Timestamp from, Timestamp to,
    const std::function<bool(const RawRecord&)>& fn);

}  // namespace tradebot::market_data
