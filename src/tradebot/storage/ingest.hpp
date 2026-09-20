#pragma once

// Ingest pipelines: raw sources -> validated, normalized events in the store.
//
//   ingest_raw_capture  live capture archives (Phase 2) -> trades, book,
//                       tickers, candles files, with validators counting
//                       gaps and the book synchronizer counting resyncs
//   ingest_bulk_archive bulk zip (aggTrades or klines) -> trades / candles
//
// Both are idempotent per day: a day's file is rewritten from scratch.

#include "tradebot/core/error.hpp"
#include "tradebot/core/log.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/market_data/raw_capture.hpp"
#include "tradebot/storage/event_store.hpp"

#include <filesystem>
#include <string>

namespace tradebot::storage {

struct IngestStats {
    std::uint64_t records_in = 0;
    std::uint64_t events_out = 0;
    std::uint64_t parse_errors = 0;
    std::uint64_t dropped = 0;  // validator drops
    std::uint64_t gaps = 0;
    std::uint64_t resyncs = 0;
};

[[nodiscard]] Result<IngestStats> ingest_raw_capture(const market_data::RawCapturePath& raw,
                                                     const StorePath& store,
                                                     InstrumentId instrument, Timestamp from,
                                                     Timestamp to, Logger log);

[[nodiscard]] Result<IngestStats> ingest_bulk_archive(const std::filesystem::path& zip,
                                                      const StorePath& store,
                                                      InstrumentId instrument, Logger log);

}  // namespace tradebot::storage
