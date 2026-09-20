#pragma once

// Downloads Binance's public historical archives (data.binance.vision).
//
// Binance publishes daily and monthly ZIP archives of aggTrades, trades and
// klines per symbol, each with a SHA-256 CHECKSUM file. This fetcher plans
// the smallest set of archives covering a date range (monthly where a full
// month is covered, daily otherwise), downloads each one streaming to disk,
// verifies the checksum, and skips archives already present and verified.
// The zips are kept as immutable ground truth under
//   <root>/bulk/binance/<SYMBOL>/<dataset>/<archive>.zip
// and read later by the bulk CSV parsers.

#include "tradebot/core/error.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/net/http.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tradebot::market_data::binance {

enum class BulkDataset { agg_trades, trades, klines };

[[nodiscard]] std::string_view to_string(BulkDataset d) noexcept;
[[nodiscard]] Result<BulkDataset> parse_bulk_dataset(std::string_view text);

struct BulkArchive {
    BulkDataset dataset;
    std::string symbol;
    std::string interval;  // klines only, e.g. "1m"
    bool monthly = false;
    Timestamp period_start;  // first day (UTC) covered
    Timestamp period_end;  // exclusive

    [[nodiscard]] std::string file_name() const;  // ETHUSDT-aggTrades-2024-01-01.zip
    [[nodiscard]] std::string url_path() const;  // /data/spot/daily/aggTrades/ETHUSDT/<file>
    [[nodiscard]] std::filesystem::path local_path(const std::filesystem::path& root) const;
};

// Covers [from, to) with whole UTC days. `to` is exclusive; both are
// truncated to day boundaries. Months fully inside the range become one
// monthly archive each.
[[nodiscard]] std::vector<BulkArchive> plan_bulk_archives(BulkDataset dataset,
                                                          const std::string& symbol,
                                                          const std::string& interval,
                                                          Timestamp from, Timestamp to);

enum class FetchOutcome { downloaded, already_present, not_available };

struct BulkFetchStats {
    std::uint64_t downloaded = 0;
    std::uint64_t skipped = 0;
    std::uint64_t not_available = 0;
    std::uint64_t bytes = 0;
};

class BulkFetcher {
public:
    struct Options {
        std::string base_url = "https://data.binance.vision";
        std::filesystem::path root = "data";
        bool verify_existing = true;  // re-hash files already on disk
    };

    BulkFetcher(net::HttpClient& http, Options opts, Logger log);

    [[nodiscard]] Result<FetchOutcome> fetch(const BulkArchive& archive);
    [[nodiscard]] Result<BulkFetchStats> fetch_all(const std::vector<BulkArchive>& archives);

    [[nodiscard]] const BulkFetchStats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] Result<std::string> fetch_checksum(const BulkArchive& archive);
    [[nodiscard]] Result<bool> download_to(const std::string& url,
                                           const std::filesystem::path& dest,
                                           std::uint64_t& bytes);

    net::HttpClient& http_;
    Options opts_;
    Logger log_;
    BulkFetchStats stats_;
};

}  // namespace tradebot::market_data::binance
