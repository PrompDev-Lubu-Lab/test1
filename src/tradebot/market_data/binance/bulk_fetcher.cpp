#include "tradebot/market_data/binance/bulk_fetcher.hpp"

#include "tradebot/util/sha256.hpp"

#include <format>
#include <fstream>

namespace tradebot::market_data::binance {

namespace fs = std::filesystem;

namespace {

std::string day_text(Timestamp t) { return t.to_iso8601().substr(0, 10); }
std::string month_text(Timestamp t) { return t.to_iso8601().substr(0, 7); }

Timestamp start_of_month(Timestamp t) {
    return *Timestamp::parse_iso8601(month_text(t) + "-01");
}

Timestamp next_month(Timestamp month_start) {
    const std::string m = month_text(month_start);
    int year = std::stoi(m.substr(0, 4));
    int month = std::stoi(m.substr(5, 2));
    if (++month > 12) {
        month = 1;
        ++year;
    }
    return *Timestamp::parse_iso8601(std::format("{:04}-{:02}-01", year, month));
}

}  // namespace

std::string_view to_string(BulkDataset d) noexcept {
    switch (d) {
        case BulkDataset::agg_trades: return "aggTrades";
        case BulkDataset::trades: return "trades";
        case BulkDataset::klines: return "klines";
    }
    return "?";
}

Result<BulkDataset> parse_bulk_dataset(std::string_view text) {
    if (text == "aggTrades" || text == "aggtrades" || text == "agg_trades") {
        return BulkDataset::agg_trades;
    }
    if (text == "trades") {
        return BulkDataset::trades;
    }
    if (text == "klines") {
        return BulkDataset::klines;
    }
    return make_error(ErrorCode::parse_error, "unknown bulk dataset '" + std::string(text) + "'");
}

std::string BulkArchive::file_name() const {
    std::string name = symbol + "-";
    name += dataset == BulkDataset::klines ? interval : std::string(to_string(dataset));
    name += "-";
    name += monthly ? month_text(period_start) : day_text(period_start);
    name += ".zip";
    return name;
}

std::string BulkArchive::url_path() const {
    std::string p = "/data/spot/";
    p += monthly ? "monthly/" : "daily/";
    p += to_string(dataset);
    p += "/" + symbol + "/";
    if (dataset == BulkDataset::klines) {
        p += interval + "/";
    }
    p += file_name();
    return p;
}

fs::path BulkArchive::local_path(const fs::path& root) const {
    std::string ds(to_string(dataset));
    if (dataset == BulkDataset::klines) {
        ds += "_" + interval;
    }
    return root / "bulk" / "binance" / symbol / ds / file_name();
}

std::vector<BulkArchive> plan_bulk_archives(BulkDataset dataset, const std::string& symbol,
                                            const std::string& interval, Timestamp from,
                                            Timestamp to) {
    std::vector<BulkArchive> out;
    const Duration day = Duration::days(1);
    Timestamp cursor = from.floor_to(day);
    const Timestamp end = to.floor_to(day);
    while (cursor < end) {
        BulkArchive a{dataset, symbol, interval, false, cursor, cursor + day};
        const Timestamp month_start = start_of_month(cursor);
        const Timestamp month_end = next_month(month_start);
        if (cursor == month_start && month_end <= end) {
            a.monthly = true;
            a.period_end = month_end;
            cursor = month_end;
        } else {
            cursor = cursor + day;
        }
        out.push_back(std::move(a));
    }
    return out;
}

BulkFetcher::BulkFetcher(net::HttpClient& http, Options opts, Logger log)
    : http_(http), opts_(std::move(opts)), log_(std::move(log)) {}

Result<std::string> BulkFetcher::fetch_checksum(const BulkArchive& archive) {
    auto resp = http_.get(opts_.base_url + archive.url_path() + ".CHECKSUM");
    if (!resp) {
        return tl::make_unexpected(resp.error());
    }
    if (resp->status == 404) {
        return make_error(ErrorCode::not_found, archive.file_name() + " is not available");
    }
    if (!resp->ok()) {
        return make_error(ErrorCode::network_error,
                          "CHECKSUM for " + archive.file_name() + ": HTTP " +
                              std::to_string(resp->status));
    }
    // "<64 hex>  <file name>\n"
    const std::string& body = resp->body;
    if (body.size() < 64) {
        return make_error(ErrorCode::parse_error, "malformed CHECKSUM for " + archive.file_name());
    }
    std::string hex = body.substr(0, 64);
    for (char& c : hex) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return make_error(ErrorCode::parse_error,
                              "malformed CHECKSUM for " + archive.file_name());
        }
    }
    return hex;
}

Result<bool> BulkFetcher::download_to(const std::string& url, const fs::path& dest,
                                      std::uint64_t& bytes) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        return make_error(ErrorCode::io_error, "cannot create " + dest.parent_path().string());
    }
    const fs::path tmp = dest.string() + ".part";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        return make_error(ErrorCode::io_error, "cannot write " + tmp.string());
    }
    bytes = 0;
    bool write_failed = false;
    auto resp = http_.get_streaming(url, [&](std::span<const std::byte> chunk) {
        out.write(reinterpret_cast<const char*>(chunk.data()),
                  static_cast<std::streamsize>(chunk.size()));
        bytes += chunk.size();
        if (!out) {
            write_failed = true;
            return false;
        }
        return true;
    });
    out.close();
    if (write_failed) {
        fs::remove(tmp, ec);
        return make_error(ErrorCode::io_error, "write failed for " + tmp.string());
    }
    if (!resp) {
        fs::remove(tmp, ec);
        return tl::make_unexpected(resp.error());
    }
    if (resp->status == 404) {
        fs::remove(tmp, ec);
        return false;
    }
    if (!resp->ok()) {
        fs::remove(tmp, ec);
        return make_error(ErrorCode::network_error,
                          url + ": HTTP " + std::to_string(resp->status));
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        return make_error(ErrorCode::io_error, "cannot rename " + tmp.string() + ": " + ec.message());
    }
    return true;
}

Result<FetchOutcome> BulkFetcher::fetch(const BulkArchive& archive) {
    const fs::path dest = archive.local_path(opts_.root);

    auto expected = fetch_checksum(archive);
    if (!expected) {
        if (expected.error().code == ErrorCode::not_found) {
            ++stats_.not_available;
            log_.warn("{}: not available", archive.file_name());
            return FetchOutcome::not_available;
        }
        return tl::make_unexpected(expected.error());
    }

    if (fs::exists(dest)) {
        if (!opts_.verify_existing) {
            ++stats_.skipped;
            return FetchOutcome::already_present;
        }
        auto actual = util::sha256_hex_of_file(dest);
        if (actual && *actual == *expected) {
            ++stats_.skipped;
            log_.debug("{}: already present and verified", archive.file_name());
            return FetchOutcome::already_present;
        }
        log_.warn("{}: existing file fails checksum; re-downloading", archive.file_name());
    }

    std::uint64_t bytes = 0;
    auto ok = download_to(opts_.base_url + archive.url_path(), dest, bytes);
    if (!ok) {
        return tl::make_unexpected(ok.error());
    }
    if (!*ok) {
        ++stats_.not_available;
        log_.warn("{}: archive missing although CHECKSUM exists", archive.file_name());
        return FetchOutcome::not_available;
    }
    auto actual = util::sha256_hex_of_file(dest);
    if (!actual) {
        return tl::make_unexpected(actual.error());
    }
    if (*actual != *expected) {
        std::error_code ec;
        fs::remove(dest, ec);
        return make_error(ErrorCode::io_error,
                          archive.file_name() + ": checksum mismatch after download (expected " +
                              *expected + ", got " + *actual + ")");
    }
    ++stats_.downloaded;
    stats_.bytes += bytes;
    log_.info("{}: downloaded {} bytes, checksum ok", archive.file_name(), bytes);
    return FetchOutcome::downloaded;
}

Result<BulkFetchStats> BulkFetcher::fetch_all(const std::vector<BulkArchive>& archives) {
    for (const auto& a : archives) {
        if (auto r = fetch(a); !r) {
            return tl::make_unexpected(r.error());
        }
    }
    return stats_;
}

}  // namespace tradebot::market_data::binance
