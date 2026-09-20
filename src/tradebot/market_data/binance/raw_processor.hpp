#pragma once

// Turns raw capture records (Phase 2) into normalized events (Phase 3).
// Stream names recorded by the collector select the parser; exchange_info
// records update the Instrument definition, which callers can query.

#include "tradebot/core/error.hpp"
#include "tradebot/core/instrument.hpp"
#include "tradebot/market_data/events.hpp"
#include "tradebot/market_data/raw_capture.hpp"

#include <optional>
#include <string>

namespace tradebot::market_data::binance {

class RawRecordProcessor {
public:
    struct Stats {
        std::uint64_t records = 0;
        std::uint64_t events = 0;
        std::uint64_t ignored = 0;  // records that carry no market event
        std::uint64_t errors = 0;
    };

    RawRecordProcessor(std::string symbol, InstrumentId instrument, VenueId venue);

    // Returns an event, or nullopt for records that are not events (e.g.
    // exchange_info, which updates instrument() instead). Parse failures
    // are errors; the caller decides whether to log and continue.
    [[nodiscard]] Result<std::optional<MarketEvent>> process(const RawRecord& record);

    [[nodiscard]] const std::optional<Instrument>& instrument() const noexcept { return instrument_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    std::string symbol_;
    InstrumentId instrument_id_;
    VenueId venue_;
    std::optional<Instrument> instrument_;
    Stats stats_;
};

}  // namespace tradebot::market_data::binance
