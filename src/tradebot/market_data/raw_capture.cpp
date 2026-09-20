#include "tradebot/market_data/raw_capture.hpp"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

namespace tradebot::market_data {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kExtension = ".jsonl.gz";

std::string day_dir_name(Timestamp t) {
    return t.to_iso8601().substr(0, 10);  // YYYY-MM-DD
}

std::string hour_file_name(Timestamp t) {
    return t.to_iso8601().substr(11, 2) + std::string(kExtension);  // HH
}

// Parses a JSON string literal starting at text[pos] == '"'. Returns the
// index just past the closing quote and the decoded value.
bool parse_json_string(std::string_view text, std::size_t& pos, std::string& out) {
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }
    const std::size_t start = pos;
    ++pos;
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '"') {
            ++pos;
            // Decode with the JSON library so escapes are handled exactly.
            auto j = nlohmann::json::parse(text.substr(start, pos - start), nullptr, false);
            if (!j.is_string()) {
                return false;
            }
            out = j.get<std::string>();
            return true;
        }
        if (c == '\\') {
            ++pos;
        }
        ++pos;
    }
    return false;
}

}  // namespace

// --- RawCapturePath ---------------------------------------------------------

fs::path RawCapturePath::dir() const { return root / "raw" / venue / symbol; }

fs::path RawCapturePath::file_for_hour(Timestamp hour_start) const {
    return dir() / day_dir_name(hour_start) / hour_file_name(hour_start);
}

Result<Timestamp> RawCapturePath::hour_of(const fs::path& file) {
    const std::string name = file.filename().string();
    const std::string day = file.parent_path().filename().string();
    if (name.size() != 2 + kExtension.size() || name.compare(2, std::string::npos, kExtension) != 0 ||
        day.size() != 10) {
        return make_error(ErrorCode::parse_error, "not a raw capture file: " + file.string());
    }
    return Timestamp::parse_iso8601(day + "T" + name.substr(0, 2) + ":00:00Z");
}

std::vector<fs::path> RawCapturePath::files_in_range(Timestamp from, Timestamp to) const {
    std::vector<std::pair<Timestamp, fs::path>> found;
    std::error_code ec;
    for (const auto& day_entry : fs::directory_iterator(dir(), ec)) {
        if (!day_entry.is_directory()) {
            continue;
        }
        for (const auto& hour_entry : fs::directory_iterator(day_entry.path(), ec)) {
            auto hour = hour_of(hour_entry.path());
            if (!hour) {
                continue;
            }
            if (*hour + Duration::hours(1) > from && *hour < to) {
                found.emplace_back(*hour, hour_entry.path());
            }
        }
    }
    std::sort(found.begin(), found.end());
    std::vector<fs::path> out;
    out.reserve(found.size());
    for (auto& [_, p] : found) {
        out.push_back(std::move(p));
    }
    return out;
}

// --- encoding ---------------------------------------------------------------

std::string encode_raw_record(const RawRecord& record) {
    std::string line = "{\"t\":";
    line += std::to_string(record.recv_time.nanos_since_epoch());
    line += ",\"s\":";
    line += nlohmann::json(record.stream).dump();
    line += ",\"d\":";
    const bool looks_like_json =
        !record.payload.empty() && (record.payload.front() == '{' || record.payload.front() == '[' ||
                                    record.payload.front() == '"');
    if (looks_like_json) {
        line += record.payload;
    } else {
        line += nlohmann::json(record.payload).dump();
    }
    line += "}\n";
    return line;
}

Result<RawRecord> decode_raw_record(std::string_view line) {
    auto fail = [&] {
        return make_error(ErrorCode::parse_error,
                          "malformed raw record: " + std::string(line.substr(0, 80)));
    };
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    static constexpr std::string_view kPrefix = "{\"t\":";
    if (line.substr(0, kPrefix.size()) != kPrefix || line.back() != '}') {
        return fail();
    }
    std::size_t pos = kPrefix.size();
    std::int64_t t = 0;
    const auto [ptr, ec] = std::from_chars(line.data() + pos, line.data() + line.size(), t);
    if (ec != std::errc{}) {
        return fail();
    }
    pos = static_cast<std::size_t>(ptr - line.data());
    static constexpr std::string_view kStreamKey = ",\"s\":";
    if (line.substr(pos, kStreamKey.size()) != kStreamKey) {
        return fail();
    }
    pos += kStreamKey.size();
    RawRecord rec;
    rec.recv_time = Timestamp::from_nanos(t);
    if (!parse_json_string(line, pos, rec.stream)) {
        return fail();
    }
    static constexpr std::string_view kDataKey = ",\"d\":";
    if (line.substr(pos, kDataKey.size()) != kDataKey) {
        return fail();
    }
    pos += kDataKey.size();
    rec.payload = std::string(line.substr(pos, line.size() - pos - 1));
    if (rec.payload.empty()) {
        return fail();
    }
    if (rec.payload.front() != '{' && rec.payload.front() != '[') {
        // Stored as a JSON string: decode it back to the original text.
        auto j = nlohmann::json::parse(rec.payload, nullptr, false);
        if (!j.is_string()) {
            return fail();
        }
        rec.payload = j.get<std::string>();
    } else if (!nlohmann::json::accept(rec.payload)) {
        // Structural check only (no DOM): catches truncated writes.
        return fail();
    }
    return rec;
}

// --- RawCaptureWriter -------------------------------------------------------

RawCaptureWriter::RawCaptureWriter(RawCapturePath path) : path_(std::move(path)) {}

RawCaptureWriter::~RawCaptureWriter() { static_cast<void>(close()); }

Result<void> RawCaptureWriter::open_for(Timestamp hour_start) {
    if (auto r = close(); !r) {
        return r;
    }
    const fs::path file = path_.file_for_hour(hour_start);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) {
        return make_error(ErrorCode::io_error,
                          "cannot create " + file.parent_path().string() + ": " + ec.message());
    }
    file_ = gzopen(file.c_str(), "ab");
    if (file_ == nullptr) {
        return make_error(ErrorCode::io_error, "cannot open " + file.string() + " for append");
    }
    current_file_ = file;
    current_hour_ = hour_start;
    has_hour_ = true;
    ++stats_.rotations;
    return {};
}

Result<void> RawCaptureWriter::write(const RawRecord& record) {
    const Timestamp hour = record.recv_time.floor_to(Duration::hours(1));
    if (!has_hour_ || hour != current_hour_) {
        if (auto r = open_for(hour); !r) {
            return r;
        }
    }
    const std::string line = encode_raw_record(record);
    const int n = gzwrite(file_, line.data(), static_cast<unsigned>(line.size()));
    if (n <= 0 || static_cast<std::size_t>(n) != line.size()) {
        int errnum = 0;
        const char* msg = gzerror(file_, &errnum);
        return make_error(ErrorCode::io_error,
                          "gzwrite to " + current_file_.string() + " failed: " + msg);
    }
    ++stats_.records;
    stats_.bytes += line.size();
    return {};
}

Result<void> RawCaptureWriter::flush() {
    if (file_ == nullptr) {
        return {};
    }
    if (gzflush(file_, Z_SYNC_FLUSH) != Z_OK) {
        return make_error(ErrorCode::io_error, "gzflush of " + current_file_.string() + " failed");
    }
    return {};
}

Result<void> RawCaptureWriter::close() {
    if (file_ == nullptr) {
        return {};
    }
    const int rc = gzclose(file_);
    file_ = nullptr;
    has_hour_ = false;
    if (rc != Z_OK) {
        return make_error(ErrorCode::io_error, "gzclose of " + current_file_.string() + " failed");
    }
    return {};
}

// --- RawCaptureReader -------------------------------------------------------

RawCaptureReader::RawCaptureReader(fs::path file) : file_(std::move(file)) {}

RawCaptureReader::~RawCaptureReader() {
    if (gz_ != nullptr) {
        gzclose(gz_);
    }
}

Result<void> RawCaptureReader::open() {
    gz_ = gzopen(file_.c_str(), "rb");
    if (gz_ == nullptr) {
        return make_error(ErrorCode::io_error, "cannot open " + file_.string());
    }
    gzbuffer(gz_, 256 * 1024);
    return {};
}

Result<bool> RawCaptureReader::next(RawRecord& out) {
    if (gz_ == nullptr) {
        return make_error(ErrorCode::invalid_state, "reader not open");
    }
    for (;;) {
        const std::size_t nl = buffer_.find('\n', pos_);
        if (nl != std::string::npos) {
            const std::string_view line(buffer_.data() + pos_, nl - pos_);
            pos_ = nl + 1;
            auto rec = decode_raw_record(line);
            if (!rec) {
                ++skipped_;
                continue;
            }
            out = std::move(*rec);
            return true;
        }
        if (eof_) {
            if (pos_ < buffer_.size()) {
                // Trailing partial line (crash mid-write): count and drop.
                ++skipped_;
                pos_ = buffer_.size();
            }
            return false;
        }
        buffer_.erase(0, pos_);
        pos_ = 0;
        char chunk[64 * 1024];
        const int n = gzread(gz_, chunk, sizeof chunk);
        if (n < 0) {
            int errnum = 0;
            const char* msg = gzerror(gz_, &errnum);
            return make_error(ErrorCode::io_error, "gzread of " + file_.string() + " failed: " + msg);
        }
        if (n == 0) {
            eof_ = true;
            continue;
        }
        buffer_.append(chunk, static_cast<std::size_t>(n));
    }
}

Result<std::uint64_t> for_each_raw_record(const RawCapturePath& path, Timestamp from, Timestamp to,
                                          const std::function<bool(const RawRecord&)>& fn) {
    std::uint64_t count = 0;
    for (const fs::path& file : path.files_in_range(from, to)) {
        RawCaptureReader reader(file);
        if (auto r = reader.open(); !r) {
            return tl::make_unexpected(r.error());
        }
        RawRecord rec;
        for (;;) {
            auto more = reader.next(rec);
            if (!more) {
                return tl::make_unexpected(more.error());
            }
            if (!*more) {
                break;
            }
            if (rec.recv_time < from || rec.recv_time >= to) {
                continue;
            }
            ++count;
            if (!fn(rec)) {
                return count;
            }
        }
    }
    return count;
}

}  // namespace tradebot::market_data
