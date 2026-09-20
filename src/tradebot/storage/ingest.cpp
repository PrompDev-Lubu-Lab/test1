#include "tradebot/storage/ingest.hpp"

#include "tradebot/market_data/binance/csv.hpp"
#include "tradebot/market_data/binance/parser.hpp"
#include "tradebot/market_data/binance/raw_processor.hpp"
#include "tradebot/market_data/book_synchronizer.hpp"
#include "tradebot/market_data/validation.hpp"
#include "tradebot/util/zip_reader.hpp"

namespace tradebot::storage {

using namespace market_data;

Result<IngestStats> ingest_raw_capture(const RawCapturePath& raw, const StorePath& store,
                                       InstrumentId instrument, Timestamp from, Timestamp to,
                                       Logger log) {
    IngestStats stats;
    binance::RawRecordProcessor processor(raw.symbol, instrument, VenueId{1});
    EventStoreWriter writer(store, instrument, DataSource::live);
    TradeValidator trade_validator([&](const ValidationIssue& i) {
        if (i.kind == IssueKind::gap) {
            ++stats.gaps;
            writer.note_gap(StreamKind::trades);
        }
        log.debug("trade stream {}: {}", to_string(i.kind), i.detail);
    });
    BookTickerValidator ticker_validator([&](const ValidationIssue& i) {
        log.debug("ticker stream {}: {}", to_string(i.kind), i.detail);
    });
    BookSynchronizer sync(instrument);
    Result<void> failure;

    auto count = for_each_raw_record(raw, from, to, [&](const RawRecord& rec) {
        ++stats.records_in;
        auto ev = processor.process(rec);
        if (!ev) {
            ++stats.parse_errors;
            if (stats.parse_errors <= 10) {
                log.warn("parse error at {}: {}", rec.recv_time.to_iso8601(), ev.error().message);
            }
            return true;
        }
        if (!ev->has_value()) {
            return true;
        }
        MarketEvent& event = **ev;
        bool keep = true;
        if (auto* t = std::get_if<Trade>(&event)) {
            keep = trade_validator.check(*t);
        } else if (auto* bt = std::get_if<BookTicker>(&event)) {
            keep = ticker_validator.check(*bt);
        } else if (auto* s = std::get_if<BookSnapshot>(&event)) {
            // Every snapshot after the first is a resync point (the
            // collector only fetches one on connect, gap or schedule).
            if (sync.stats().snapshots > 0) {
                ++stats.resyncs;
                writer.note_resync();
            }
            if (sync.on_snapshot(*s) == SyncAction::resync_required) {
                ++stats.gaps;
                writer.note_gap(StreamKind::book);
            }
        } else if (auto* d = std::get_if<BookDelta>(&event)) {
            if (sync.on_delta(*d) == SyncAction::resync_required) {
                ++stats.gaps;
                writer.note_gap(StreamKind::book);
            }
        } else if (auto* c = std::get_if<Candle>(&event)) {
            keep = c->closed;  // only completed candles are stored
        }
        if (!keep) {
            ++stats.dropped;
            return true;
        }
        if (auto w = writer.write(event); !w) {
            failure = w;
            return false;
        }
        ++stats.events_out;
        return true;
    });
    if (!count) {
        return tl::make_unexpected(count.error());
    }
    if (!failure) {
        return tl::make_unexpected(failure.error());
    }
    if (auto c = writer.close(); !c) {
        return tl::make_unexpected(c.error());
    }
    log.info("ingested {} records -> {} events ({} parse errors, {} dropped, {} gaps, {} resyncs)",
             stats.records_in, stats.events_out, stats.parse_errors, stats.dropped, stats.gaps,
             stats.resyncs);
    return stats;
}

Result<IngestStats> ingest_bulk_archive(const std::filesystem::path& zip_path, const StorePath& store,
                                        InstrumentId instrument, Logger log) {
    IngestStats stats;
    auto zip = util::ZipReader::open(zip_path);
    if (!zip) {
        return tl::make_unexpected(zip.error());
    }
    if (zip->entries().size() != 1) {
        return make_error(ErrorCode::unsupported,
                          zip_path.string() + ": expected exactly one CSV entry");
    }
    // Infer dataset from the file name: SYMBOL-aggTrades-..., SYMBOL-trades-..., SYMBOL-1m-...
    const std::string name = zip_path.filename().string();
    const std::size_t dash1 = name.find('-');
    const std::size_t dash2 = name.find('-', dash1 + 1);
    if (dash1 == std::string::npos || dash2 == std::string::npos) {
        return make_error(ErrorCode::parse_error, "cannot infer dataset from " + name);
    }
    const std::string dataset = name.substr(dash1 + 1, dash2 - dash1 - 1);
    enum { agg, trd, kln } mode;
    Duration interval;
    if (dataset == "aggTrades") {
        mode = agg;
    } else if (dataset == "trades") {
        mode = trd;
    } else if (auto iv = binance::parse_kline_interval(dataset); iv) {
        mode = kln;
        interval = *iv;
    } else {
        return make_error(ErrorCode::unsupported, "unknown bulk dataset in " + name);
    }

    EventStoreWriter writer(store, instrument, DataSource::bulk);
    TradeValidator validator([&](const ValidationIssue& i) {
        if (i.kind == IssueKind::gap) {
            ++stats.gaps;
            writer.note_gap(StreamKind::trades);
        }
    });
    Result<void> failure;
    auto lines = zip->for_each_line(zip->entries()[0], [&](std::string_view line) {
        if (line.empty()) {
            return true;
        }
        ++stats.records_in;
        if (stats.records_in == 1 && binance::is_csv_header(line)) {
            return true;
        }
        MarketEvent event;
        if (mode == kln) {
            auto c = binance::parse_kline_csv(line, instrument, interval);
            if (!c) {
                ++stats.parse_errors;
                return true;
            }
            event = *c;
        } else {
            auto t = mode == agg ? binance::parse_agg_trade_csv(line, instrument)
                                 : binance::parse_trade_csv(line, instrument);
            if (!t) {
                ++stats.parse_errors;
                return true;
            }
            if (!validator.check(*t)) {
                ++stats.dropped;
                return true;
            }
            event = *t;
        }
        if (auto w = writer.write(event); !w) {
            failure = w;
            return false;
        }
        ++stats.events_out;
        return true;
    });
    if (!lines) {
        return tl::make_unexpected(lines.error());
    }
    if (!failure) {
        return tl::make_unexpected(failure.error());
    }
    if (auto c = writer.close(); !c) {
        return tl::make_unexpected(c.error());
    }
    log.info("{}: {} rows -> {} events ({} parse errors, {} dropped, {} gaps)", name,
             stats.records_in, stats.events_out, stats.parse_errors, stats.dropped, stats.gaps);
    return stats;
}

}  // namespace tradebot::storage
