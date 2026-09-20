#include "tradebot/storage/event_store.hpp"

#include "tradebot/market_data/binance/parser.hpp"

#include <algorithm>
#include <format>

namespace tradebot::storage {

namespace fs = std::filesystem;
using market_data::MarketEvent;

namespace {

Timestamp day_of(Timestamp t) { return t.floor_to(Duration::days(1)); }

std::string day_name(Timestamp day) { return day.to_iso8601().substr(0, 10); }

std::int64_t interval_of(const MarketEvent& e) {
    if (const auto* c = std::get_if<market_data::Candle>(&e)) {
        return c->interval.count_nanos();
    }
    return 0;
}

}  // namespace

// --- StorePath --------------------------------------------------------------

fs::path StorePath::dir() const { return root / "store" / venue / symbol; }

fs::path StorePath::day_dir(Timestamp day) const { return dir() / day_name(day_of(day)); }

std::string StorePath::file_name(StreamKind kind, Duration candle_interval) {
    std::string name(to_string(kind));
    if (kind == StreamKind::candles && !candle_interval.is_zero()) {
        name += "_" + market_data::binance::kline_interval_name(candle_interval);
    }
    return name + ".tbev";
}

fs::path StorePath::file(Timestamp day, StreamKind kind, Duration candle_interval) const {
    return day_dir(day) / file_name(kind, candle_interval);
}

// --- EventStoreWriter -------------------------------------------------------

EventStoreWriter::EventStoreWriter(StorePath path, InstrumentId instrument, DataSource source)
    : path_(std::move(path)), instrument_(instrument), source_(source) {}

EventStoreWriter::~EventStoreWriter() { static_cast<void>(close()); }

Result<EventFileWriter*> EventStoreWriter::writer_for(Key key) {
    auto it = open_.find(key);
    if (it != open_.end()) {
        return it->second.get();
    }
    // A new day for this kind: close the previous day's file of the same
    // kind so at most one file per kind is open.
    for (auto o = open_.begin(); o != open_.end();) {
        if (o->first.kind == key.kind && o->first.interval_ns == key.interval_ns) {
            if (auto r = o->second->close(); !r) {
                return tl::make_unexpected(r.error());
            }
            o = open_.erase(o);
        } else {
            ++o;
        }
    }
    auto w = std::make_unique<EventFileWriter>();
    const fs::path file = path_.file(key.day, key.kind, Duration::nanos(key.interval_ns));
    if (auto r = w->open(file, key.kind, instrument_, source_, Duration::nanos(key.interval_ns)); !r) {
        return tl::make_unexpected(r.error());
    }
    ++stats_.files;
    auto* ptr = w.get();
    open_.emplace(key, std::move(w));
    return ptr;
}

Result<void> EventStoreWriter::write(const MarketEvent& event) {
    const Key key{day_of(market_data::event_time(event)), stream_kind_of(event), interval_of(event)};
    auto w = writer_for(key);
    if (!w) {
        return tl::make_unexpected(w.error());
    }
    if (auto r = (*w)->write(event); !r) {
        return r;
    }
    ++stats_.events;
    return {};
}

void EventStoreWriter::note_gap(StreamKind kind) {
    for (auto& [key, w] : open_) {
        if (key.kind == kind) {
            w->note_gap();
        }
    }
}

void EventStoreWriter::note_resync() {
    for (auto& [key, w] : open_) {
        if (key.kind == StreamKind::book) {
            w->note_resync();
        }
    }
}

Result<void> EventStoreWriter::close() {
    Result<void> result;
    for (auto& [key, w] : open_) {
        if (auto r = w->close(); !r && result) {
            result = r;
        }
    }
    open_.clear();
    return result;
}

// --- Catalog ----------------------------------------------------------------

Result<Catalog> Catalog::scan(const StorePath& path) {
    Catalog cat;
    std::error_code ec;
    if (!fs::exists(path.dir(), ec)) {
        return cat;  // nothing stored yet is not an error
    }
    for (const auto& day_entry : fs::directory_iterator(path.dir(), ec)) {
        if (!day_entry.is_directory()) {
            continue;
        }
        auto day = Timestamp::parse_iso8601(day_entry.path().filename().string());
        if (!day) {
            continue;
        }
        for (const auto& f : fs::directory_iterator(day_entry.path(), ec)) {
            if (f.path().extension() != ".tbev") {
                continue;
            }
            auto hdr = EventFileReader::read_header(f.path());
            if (!hdr) {
                cat.broken_.push_back(f.path());
                continue;
            }
            cat.files_.push_back(FileSummary{f.path(), *day, *hdr});
        }
    }
    std::sort(cat.files_.begin(), cat.files_.end(), [](const FileSummary& a, const FileSummary& b) {
        if (a.day != b.day) return a.day < b.day;
        if (a.header.kind != b.header.kind) return a.header.kind < b.header.kind;
        return a.header.candle_interval_ns < b.header.candle_interval_ns;
    });
    return cat;
}

std::vector<FileSummary> Catalog::select(StreamKind kind, Timestamp from, Timestamp to,
                                         Duration candle_interval) const {
    std::vector<FileSummary> out;
    for (const auto& f : files_) {
        if (f.header.kind != kind) continue;
        if (kind == StreamKind::candles && f.header.candle_interval_ns != candle_interval.count_nanos()) continue;
        if (f.day + Duration::days(1) <= from || f.day >= to) continue;
        out.push_back(f);
    }
    return out;
}

std::vector<Timestamp> Catalog::missing_days(StreamKind kind, Timestamp from, Timestamp to,
                                             Duration candle_interval) const {
    const auto present = select(kind, from, to, candle_interval);
    std::vector<Timestamp> missing;
    for (Timestamp day = day_of(from); day < to; day += Duration::days(1)) {
        const bool have = std::any_of(present.begin(), present.end(),
                                      [&](const FileSummary& f) { return f.day == day; });
        if (!have) {
            missing.push_back(day);
        }
    }
    return missing;
}

std::string Catalog::to_string() const {
    std::string out;
    for (const auto& f : files_) {
        const auto& h = f.header;
        out += std::format("{} {:<12} {:>10} records  {} .. {}  gaps={} resyncs={} source={}\n",
                           day_name(f.day), f.path.filename().string(), h.record_count,
                           h.record_count ? h.first_time.to_iso8601().substr(11, 12) : "-",
                           h.record_count ? h.last_time.to_iso8601().substr(11, 12) : "-",
                           h.gap_count, h.resync_count, storage::to_string(h.source));
    }
    for (const auto& b : broken_) {
        out += "BROKEN " + b.string() + "\n";
    }
    return out;
}

// --- EventStoreReader -------------------------------------------------------

EventStoreReader::EventStoreReader(StorePath path, StreamKind kind, Timestamp from, Timestamp to,
                                   Duration candle_interval)
    : path_(std::move(path)), kind_(kind), from_(from), to_(to), interval_(candle_interval) {}

Result<void> EventStoreReader::open() {
    auto cat = Catalog::scan(path_);
    if (!cat) {
        return tl::make_unexpected(cat.error());
    }
    files_ = cat->select(kind_, from_, to_, interval_);
    next_file_ = 0;
    reader_.reset();
    return {};
}

Result<bool> EventStoreReader::open_next_file() {
    while (next_file_ < files_.size()) {
        const FileSummary& f = files_[next_file_++];
        if (f.header.record_count == 0 || f.header.last_time < from_ || f.header.first_time >= to_) {
            continue;
        }
        auto r = std::make_unique<EventFileReader>();
        if (auto o = r->open(f.path); !o) {
            return tl::make_unexpected(o.error());
        }
        r->seek_from(from_);
        reader_ = std::move(r);
        return true;
    }
    return false;
}

Result<bool> EventStoreReader::next(MarketEvent& out) {
    for (;;) {
        if (!reader_) {
            auto opened = open_next_file();
            if (!opened) {
                return tl::make_unexpected(opened.error());
            }
            if (!*opened) {
                return false;
            }
        }
        auto more = reader_->next(out);
        if (!more) {
            return tl::make_unexpected(more.error());
        }
        if (!*more) {
            reader_.reset();
            continue;
        }
        const Timestamp t = market_data::event_time(out);
        if (t < from_) {
            continue;
        }
        if (t >= to_) {
            // Files are time-ordered; nothing later in this file qualifies,
            // and later files start at later days.
            reader_.reset();
            next_file_ = files_.size();
            return false;
        }
        return true;
    }
}

}  // namespace tradebot::storage
