#include "tradebot/live/journal.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <unistd.h>

namespace tradebot::live {

using execution::ExecutionReport;
using execution::ReportType;

std::string encode_report(const ExecutionReport& r) {
    nlohmann::json j;
    j["type"] = std::string(to_string(r.type));
    j["client_id"] = r.client_id.value();
    j["order_id"] = r.order_id.value();
    j["instrument"] = r.instrument.value();
    j["strategy"] = r.strategy.value();
    j["side"] = std::string(to_string(r.side));
    j["order_type"] = std::string(to_string(r.order_type));
    j["price"] = r.price.to_string();
    j["time"] = r.time.nanos_since_epoch();
    j["status"] = std::string(to_string(r.status));
    j["filled"] = r.filled_quantity.to_string();
    j["remaining"] = r.remaining_quantity.to_string();
    if (r.fill) {
        j["fill"] = {{"price", r.fill->price.to_string()},
                     {"quantity", r.fill->quantity.to_string()},
                     {"fee", r.fill->fee.to_string()},
                     {"liquidity", std::string(to_string(r.fill->liquidity))},
                     {"exec_id", r.fill->exec_id.value()}};
    }
    if (!r.reason.empty()) {
        j["reason"] = r.reason;
    }
    return j.dump();
}

namespace {

Result<ReportType> parse_report_type(std::string_view s) {
    if (s == "accepted") return ReportType::accepted;
    if (s == "rejected") return ReportType::rejected;
    if (s == "fill") return ReportType::fill;
    if (s == "cancelled") return ReportType::cancelled;
    if (s == "cancel_rejected") return ReportType::cancel_rejected;
    if (s == "expired") return ReportType::expired;
    return make_error(ErrorCode::parse_error, "bad report type");
}

Result<OrderStatus> parse_status(std::string_view s) {
    for (OrderStatus st : {OrderStatus::pending_new, OrderStatus::open, OrderStatus::partially_filled,
                           OrderStatus::filled, OrderStatus::pending_cancel, OrderStatus::cancelled,
                           OrderStatus::rejected, OrderStatus::expired}) {
        if (to_string(st) == s) return st;
    }
    return make_error(ErrorCode::parse_error, "bad order status");
}

}  // namespace

Result<ExecutionReport> decode_report(std::string_view line) {
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return make_error(ErrorCode::parse_error, "journal line is not a JSON object");
    }
    auto str = [&](const char* key) -> Result<std::string> {
        if (!j.contains(key) || !j[key].is_string()) {
            return make_error(ErrorCode::parse_error, std::string("journal line missing '") + key + "'");
        }
        return j[key].get<std::string>();
    };
    ExecutionReport r;
    auto type = str("type"); if (!type) return tl::make_unexpected(type.error());
    auto t = parse_report_type(*type); if (!t) return tl::make_unexpected(t.error());
    r.type = *t;
    auto side = str("side"); if (!side) return tl::make_unexpected(side.error());
    auto sd = parse_side(*side); if (!sd) return tl::make_unexpected(sd.error());
    r.side = *sd;
    auto ot = str("order_type"); if (!ot) return tl::make_unexpected(ot.error());
    auto oty = parse_order_type(*ot); if (!oty) return tl::make_unexpected(oty.error());
    r.order_type = *oty;
    auto status = str("status"); if (!status) return tl::make_unexpected(status.error());
    auto st = parse_status(*status); if (!st) return tl::make_unexpected(st.error());
    r.status = *st;
    auto price = str("price"); if (!price) return tl::make_unexpected(price.error());
    auto px = Price::parse(*price); if (!px) return tl::make_unexpected(px.error());
    r.price = *px;
    auto filled = str("filled"); if (!filled) return tl::make_unexpected(filled.error());
    auto fq = Quantity::parse(*filled); if (!fq) return tl::make_unexpected(fq.error());
    r.filled_quantity = *fq;
    auto remaining = str("remaining"); if (!remaining) return tl::make_unexpected(remaining.error());
    auto rq = Quantity::parse(*remaining); if (!rq) return tl::make_unexpected(rq.error());
    r.remaining_quantity = *rq;
    if (!j.contains("client_id") || !j["client_id"].is_number_unsigned() || !j.contains("time") ||
        !j["time"].is_number_integer()) {
        return make_error(ErrorCode::parse_error, "journal line missing client_id/time");
    }
    r.client_id = ClientOrderId{j["client_id"].get<std::uint64_t>()};
    r.order_id = OrderId{j.value("order_id", std::uint64_t{0})};
    r.instrument = InstrumentId{j.value("instrument", std::uint32_t{0})};
    r.strategy = StrategyId{j.value("strategy", std::uint32_t{0})};
    r.time = Timestamp::from_nanos(j["time"].get<std::int64_t>());
    r.reason = j.value("reason", std::string{});
    if (j.contains("fill") && j["fill"].is_object()) {
        const auto& f = j["fill"];
        execution::Fill fill;
        auto fp = Price::parse(f.value("price", ""));
        auto fqty = Quantity::parse(f.value("quantity", ""));
        auto fee = Notional::parse(f.value("fee", ""));
        if (!fp || !fqty || !fee) {
            return make_error(ErrorCode::parse_error, "journal fill malformed");
        }
        fill.price = *fp;
        fill.quantity = *fqty;
        fill.fee = *fee;
        fill.liquidity = f.value("liquidity", "taker") == "maker" ? Liquidity::maker : Liquidity::taker;
        fill.exec_id = TradeId{f.value("exec_id", std::uint64_t{0})};
        r.fill = fill;
    }
    return r;
}

Journal::~Journal() { static_cast<void>(close()); }

Result<void> Journal::open(const std::filesystem::path& path) {
    if (auto c = close(); !c) return c;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    file_ = std::fopen(path.c_str(), "ab");
    if (file_ == nullptr) {
        return make_error(ErrorCode::io_error, "cannot open journal " + path.string() + ": " + std::strerror(errno));
    }
    path_ = path;
    return {};
}

Result<void> Journal::append(const ExecutionReport& report, bool sync) {
    if (file_ == nullptr) {
        return make_error(ErrorCode::invalid_state, "journal not open");
    }
    const std::string line = encode_report(report) + "\n";
    if (std::fwrite(line.data(), 1, line.size(), file_) != line.size() || std::fflush(file_) != 0) {
        return make_error(ErrorCode::io_error, "journal write failed: " + std::string(std::strerror(errno)));
    }
    if (sync && ::fsync(::fileno(file_)) != 0) {
        return make_error(ErrorCode::io_error, "journal fsync failed: " + std::string(std::strerror(errno)));
    }
    ++appended_;
    return {};
}

Result<void> Journal::close() {
    if (file_ == nullptr) return {};
    const int rc = std::fclose(file_);
    file_ = nullptr;
    if (rc != 0) {
        return make_error(ErrorCode::io_error, "journal close failed");
    }
    return {};
}

Result<std::uint64_t> Journal::replay(const std::filesystem::path& path,
                                      const std::function<void(const ExecutionReport&)>& handler) {
    std::ifstream in(path);
    if (!in) {
        return make_error(ErrorCode::io_error, "cannot open journal " + path.string());
    }
    std::uint64_t skipped = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto r = decode_report(line);
        if (!r) {
            ++skipped;
            continue;
        }
        handler(*r);
    }
    return skipped;
}

}  // namespace tradebot::live
