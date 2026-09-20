#include "tradebot/market_data/binance/raw_processor.hpp"

#include "tradebot/market_data/binance/parser.hpp"

namespace tradebot::market_data::binance {

RawRecordProcessor::RawRecordProcessor(std::string symbol, InstrumentId instrument, VenueId venue)
    : symbol_(std::move(symbol)), instrument_id_(instrument), venue_(venue) {}

Result<std::optional<MarketEvent>> RawRecordProcessor::process(const RawRecord& record) {
    ++stats_.records;
    if (record.stream == "exchange_info") {
        auto inst = parse_exchange_info(record.payload, symbol_, instrument_id_, venue_);
        if (!inst) {
            ++stats_.errors;
            return tl::make_unexpected(inst.error());
        }
        instrument_ = std::move(*inst);
        ++stats_.ignored;
        return std::nullopt;
    }
    if (record.stream == "depth_snapshot") {
        auto snap = parse_depth_snapshot(record.payload, instrument_id_, record.recv_time);
        if (!snap) {
            ++stats_.errors;
            return tl::make_unexpected(snap.error());
        }
        ++stats_.events;
        return MarketEvent{std::move(*snap)};
    }
    auto ev = parse_stream_message(record.payload, instrument_id_, record.recv_time);
    if (!ev) {
        if (ev.error().code == ErrorCode::unsupported) {
            ++stats_.ignored;
            return std::nullopt;
        }
        ++stats_.errors;
        return tl::make_unexpected(ev.error());
    }
    ++stats_.events;
    return std::move(*ev);
}

}  // namespace tradebot::market_data::binance
