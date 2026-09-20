// tradebot-fetch: download Binance historical archives for a symbol.
//
//   tradebot-fetch --symbol ETHUSDT --from 2024-01-01 --to 2024-03-01
//                  [--datasets aggTrades,klines] [--interval 1m] [--data-dir data]
//
// Archives are stored as immutable zips under <data-dir>/bulk/binance/.

#include "tradebot/core/log.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/market_data/binance/bulk_fetcher.hpp"
#include "tradebot/net/http.hpp"
#include "tradebot/net/tls.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s --symbol SYMBOL --from YYYY-MM-DD --to YYYY-MM-DD\n"
                 "          [--datasets aggTrades,trades,klines] [--interval 1m]\n"
                 "          [--data-dir DIR] [--base-url URL] [--log-level L]\n",
                 argv0);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace tradebot;
    using namespace tradebot::market_data::binance;

    std::string symbol = "ETHUSDT", from_text, to_text, datasets_text = "aggTrades,klines";
    std::string interval = "1m", data_dir = "data", base_url = "https://data.binance.vision";
    std::string log_level_text = "info";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            return usage(argv[0]);
        }
        const std::string val = argv[++i];
        if (arg == "--symbol") symbol = val;
        else if (arg == "--from") from_text = val;
        else if (arg == "--to") to_text = val;
        else if (arg == "--datasets") datasets_text = val;
        else if (arg == "--interval") interval = val;
        else if (arg == "--data-dir") data_dir = val;
        else if (arg == "--base-url") base_url = val;
        else if (arg == "--log-level") log_level_text = val;
        else return usage(argv[0]);
    }
    if (from_text.empty() || to_text.empty()) {
        return usage(argv[0]);
    }
    auto level = parse_log_level(log_level_text);
    auto from = Timestamp::parse_iso8601(from_text);
    auto to = Timestamp::parse_iso8601(to_text);
    if (!level || !from || !to) {
        std::fprintf(stderr, "invalid --log-level, --from or --to\n");
        return 1;
    }
    Logger log = Logger::stderr_logger("fetch", *level);

    std::vector<BulkDataset> datasets;
    std::string cur;
    for (char c : datasets_text + ',') {
        if (c == ',') {
            if (!cur.empty()) {
                auto d = parse_bulk_dataset(cur);
                if (!d) {
                    log.error("{}", d.error().to_string());
                    return 1;
                }
                datasets.push_back(*d);
            }
            cur.clear();
        } else if (c != ' ') {
            cur.push_back(c);
        }
    }

    auto tls = net::TlsContext::create();
    if (!tls) {
        log.error("{}", tls.error().to_string());
        return 1;
    }
    auto http = net::HttpClient::create(*tls, {.timeout = Duration::minutes(10)});
    if (!http) {
        log.error("{}", http.error().to_string());
        return 1;
    }
    BulkFetcher fetcher(*http, {.base_url = base_url, .root = data_dir}, log);

    for (BulkDataset d : datasets) {
        const auto plan = plan_bulk_archives(d, symbol, interval, *from, *to);
        log.info("{}: {} archives to check for {} [{}, {})", to_string(d), plan.size(), symbol,
                 from_text, to_text);
        auto r = fetcher.fetch_all(plan);
        if (!r) {
            log.error("{}", r.error().to_string());
            return 1;
        }
    }
    const auto& s = fetcher.stats();
    log.info("done: downloaded={} already_present={} not_available={} bytes={}", s.downloaded,
             s.skipped, s.not_available, s.bytes);
    return 0;
}
