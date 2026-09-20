// tradebot-ingest: build the normalized event store from raw sources.
//
//   tradebot-ingest raw  --symbol ETHUSDT --from 2024-03-01 --to 2024-03-08 [--data-dir data]
//   tradebot-ingest bulk --symbol ETHUSDT --from 2024-01-01 --to 2024-03-01 [--data-dir data]
//   tradebot-ingest catalog --symbol ETHUSDT [--data-dir data]
//
// "raw" converts live capture archives; "bulk" converts every downloaded
// archive whose period intersects the range; "catalog" prints what the
// store contains.

#include "tradebot/core/log.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/market_data/raw_capture.hpp"
#include "tradebot/storage/event_store.hpp"
#include "tradebot/storage/ingest.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s raw|bulk|catalog --symbol SYMBOL [--from YYYY-MM-DD --to YYYY-MM-DD]\n"
                 "          [--data-dir DIR] [--venue binance] [--log-level L]\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace tradebot;
    using namespace tradebot::storage;

    if (argc < 2) {
        return usage(argv[0]);
    }
    const std::string mode = argv[1];
    std::string symbol = "ETHUSDT", from_text, to_text, data_dir = "data", venue = "binance";
    std::string log_level_text = "info";
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return usage(argv[0]);
        }
        const std::string val = argv[++i];
        if (arg == "--symbol") symbol = val;
        else if (arg == "--from") from_text = val;
        else if (arg == "--to") to_text = val;
        else if (arg == "--data-dir") data_dir = val;
        else if (arg == "--venue") venue = val;
        else if (arg == "--log-level") log_level_text = val;
        else return usage(argv[0]);
    }
    auto level = parse_log_level(log_level_text);
    if (!level) {
        return usage(argv[0]);
    }
    Logger log = Logger::stderr_logger("ingest", *level);
    const StorePath store{data_dir, venue, symbol};
    const InstrumentId instrument{1};

    if (mode == "catalog") {
        auto cat = Catalog::scan(store);
        if (!cat) {
            log.error("{}", cat.error().to_string());
            return 1;
        }
        std::fputs(cat->to_string().c_str(), stdout);
        std::printf("%zu files, %zu broken\n", cat->files().size(), cat->broken().size());
        return 0;
    }

    auto from = Timestamp::parse_iso8601(from_text);
    auto to = Timestamp::parse_iso8601(to_text);
    if (!from || !to) {
        std::fprintf(stderr, "--from and --to are required (YYYY-MM-DD)\n");
        return 1;
    }

    if (mode == "raw") {
        market_data::RawCapturePath raw{data_dir, venue, symbol};
        auto stats = ingest_raw_capture(raw, store, instrument, *from, *to, log);
        if (!stats) {
            log.error("{}", stats.error().to_string());
            return 1;
        }
        return 0;
    }
    if (mode == "bulk") {
        const fs::path bulk_dir = fs::path(data_dir) / "bulk" / venue / symbol;
        std::vector<fs::path> zips;
        std::error_code ec;
        for (const auto& ds : fs::directory_iterator(bulk_dir, ec)) {
            if (!ds.is_directory()) continue;
            for (const auto& f : fs::directory_iterator(ds.path(), ec)) {
                if (f.path().extension() != ".zip") continue;
                // SYMBOL-<dataset>-<YYYY-MM[-DD]>.zip: the period follows the second dash.
                const std::string stem = f.path().stem().string();
                const std::size_t d1 = stem.find('-');
                const std::size_t d2 = d1 == std::string::npos ? d1 : stem.find('-', d1 + 1);
                if (d2 == std::string::npos) continue;
                std::string period = stem.substr(d2 + 1);
                const bool monthly = period.size() == 7;
                if (monthly) period += "-01";
                auto start = Timestamp::parse_iso8601(period);
                if (!start) continue;
                const Timestamp end = *start + Duration::days(monthly ? 31 : 1);
                if (end <= *from || *start >= *to) continue;
                zips.push_back(f.path());
            }
        }
        std::sort(zips.begin(), zips.end());
        if (zips.empty()) {
            log.warn("no archives under {} intersect the range", bulk_dir.string());
        }
        for (const auto& z : zips) {
            auto stats = ingest_bulk_archive(z, store, instrument, log);
            if (!stats) {
                log.error("{}: {}", z.string(), stats.error().to_string());
                return 1;
            }
        }
        return 0;
    }
    return usage(argv[0]);
}
