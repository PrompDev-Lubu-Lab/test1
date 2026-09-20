#include "tradebot/live/feed_health.hpp"

#include <fstream>

namespace tradebot::live {

void FeedHealthMonitor::seen(Timestamp t, bool trade) {
    ++events_;
    last_event_ = last_event_ ? std::max(*last_event_, t) : t;
    if (trade) {
        last_trade_ = last_trade_ ? std::max(*last_trade_, t) : t;
    }
}

bool FeedHealthMonitor::check(Timestamp now) {
    const FeedState before = state_;
    if (!last_event_) {
        state_ = FeedState::waiting;
    } else if (now - *last_event_ > opts_.stale_after) {
        state_ = FeedState::stale;
    } else {
        state_ = FeedState::healthy;
    }
    if (state_ == FeedState::stale && before != FeedState::stale) {
        ++stale_episodes_;
    }
    return state_ != before;
}

Result<void> write_heartbeat(const std::filesystem::path& file, Timestamp now, std::string_view status) {
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            return make_error(ErrorCode::io_error, "cannot write " + tmp.string());
        }
        out << now.to_iso8601() << ' ' << status << '\n';
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        return make_error(ErrorCode::io_error, "cannot replace " + file.string() + ": " + ec.message());
    }
    return {};
}

std::optional<Duration> heartbeat_age(const std::filesystem::path& file, Timestamp now) {
    std::ifstream in(file);
    std::string iso;
    if (!in || !(in >> iso)) {
        return std::nullopt;
    }
    auto t = Timestamp::parse_iso8601(iso);
    if (!t) {
        return std::nullopt;
    }
    return now - *t;
}

}  // namespace tradebot::live
