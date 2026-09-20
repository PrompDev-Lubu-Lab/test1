#pragma once

// The normalized event store: a directory of TBEV files partitioned by
// venue, symbol, UTC day and stream kind, plus a catalog built from their
// headers.
//
//   <root>/store/<venue>/<symbol>/<YYYY-MM-DD>/<kind>[_<interval>].tbev
//
// EventStoreWriter routes events to the right day file and rolls files at
// day boundaries. EventStoreReader streams one kind over a time range
// across days. Catalog::scan lists what exists, with counts and quality
// flags, so research code can check coverage before running.

#include "tradebot/core/error.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/market_data/events.hpp"
#include "tradebot/storage/event_file.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tradebot::storage {

struct StorePath {
    std::filesystem::path root;
    std::string venue;
    std::string symbol;

    [[nodiscard]] std::filesystem::path dir() const;
    [[nodiscard]] std::filesystem::path day_dir(Timestamp day) const;
    [[nodiscard]] std::filesystem::path file(Timestamp day, StreamKind kind,
                                            Duration candle_interval = {}) const;
    [[nodiscard]] static std::string file_name(StreamKind kind, Duration candle_interval = {});
};

class EventStoreWriter {
public:
    EventStoreWriter(StorePath path, InstrumentId instrument, DataSource source);
    ~EventStoreWriter();

    // Events of any kind, in non-decreasing time order per kind. Candles of
    // different intervals go to different files.
    [[nodiscard]] Result<void> write(const market_data::MarketEvent& event);
    void note_gap(StreamKind kind);
    void note_resync();
    [[nodiscard]] Result<void> close();

    struct Stats {
        std::uint64_t events = 0;
        std::uint64_t files = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Key {
        Timestamp day;
        StreamKind kind;
        std::int64_t interval_ns;
        auto operator<=>(const Key&) const = default;
    };
    [[nodiscard]] Result<EventFileWriter*> writer_for(Key key);

    StorePath path_;
    InstrumentId instrument_;
    DataSource source_;
    std::map<Key, std::unique_ptr<EventFileWriter>> open_;
    Stats stats_;
};

struct FileSummary {
    std::filesystem::path path;
    Timestamp day;
    FileHeader header;
};

class Catalog {
public:
    // Reads every file header under the symbol directory. Files that fail
    // to parse are reported in `broken` rather than aborting the scan.
    [[nodiscard]] static Result<Catalog> scan(const StorePath& path);

    [[nodiscard]] const std::vector<FileSummary>& files() const noexcept { return files_; }
    [[nodiscard]] const std::vector<std::filesystem::path>& broken() const noexcept { return broken_; }

    // Files of a kind whose day intersects [from, to), in day order.
    [[nodiscard]] std::vector<FileSummary> select(StreamKind kind, Timestamp from, Timestamp to,
                                                  Duration candle_interval = {}) const;

    // Days in [from, to) with no file of this kind.
    [[nodiscard]] std::vector<Timestamp> missing_days(StreamKind kind, Timestamp from,
                                                      Timestamp to,
                                                      Duration candle_interval = {}) const;

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<FileSummary> files_;
    std::vector<std::filesystem::path> broken_;
};

// Streams one stream kind over [from, to) across day files.
class EventStoreReader {
public:
    EventStoreReader(StorePath path, StreamKind kind, Timestamp from, Timestamp to,
                     Duration candle_interval = {});

    [[nodiscard]] Result<void> open();
    // False when the range is exhausted. Events are filtered to [from, to).
    [[nodiscard]] Result<bool> next(market_data::MarketEvent& out);

    [[nodiscard]] const std::vector<FileSummary>& files() const noexcept { return files_; }

private:
    [[nodiscard]] Result<bool> open_next_file();

    StorePath path_;
    StreamKind kind_;
    Timestamp from_;
    Timestamp to_;
    Duration interval_;
    std::vector<FileSummary> files_;
    std::size_t next_file_ = 0;
    std::unique_ptr<EventFileReader> reader_;
};

}  // namespace tradebot::storage
