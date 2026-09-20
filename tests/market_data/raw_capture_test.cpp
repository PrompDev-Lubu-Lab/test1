#include "tradebot/market_data/raw_capture.hpp"

#include <doctest/doctest.h>
#include <zlib.h>

#include <filesystem>
#include <fstream>
#include <random>

using namespace tradebot;
using namespace tradebot::market_data;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("tradebot-test-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

Timestamp ts(const char* iso) { return *Timestamp::parse_iso8601(iso); }

}  // namespace

TEST_CASE("raw record: encode/decode round trip") {
    RawRecord r{ts("2024-03-15T12:34:56.123456789Z"), "ethusdt@aggTrade",
                R"({"e":"aggTrade","p":"3000.50","q":"0.1"})"};
    const std::string line = encode_raw_record(r);
    CHECK(line == R"({"t":1710506096123456789,"s":"ethusdt@aggTrade","d":{"e":"aggTrade","p":"3000.50","q":"0.1"}})"
                  "\n");
    auto back = decode_raw_record(line);
    REQUIRE_MESSAGE(back.has_value(), back.error().message);
    CHECK(back->recv_time == r.recv_time);
    CHECK(back->stream == r.stream);
    CHECK(back->payload == r.payload);

    // Non-JSON payload and a stream name needing escapes.
    RawRecord odd{ts("2024-03-15T00:00:00Z"), "weird \"name\"\\x", "plain text, not json"};
    auto back2 = decode_raw_record(encode_raw_record(odd));
    REQUIRE(back2.has_value());
    CHECK(back2->stream == odd.stream);
    CHECK(back2->payload == odd.payload);

    // Array payload.
    RawRecord arr{ts("2024-03-15T00:00:00Z"), "s", "[1,2,3]"};
    CHECK(decode_raw_record(encode_raw_record(arr))->payload == "[1,2,3]");
}

TEST_CASE("raw record: decode rejects malformed lines") {
    CHECK_FALSE(decode_raw_record("").has_value());
    CHECK_FALSE(decode_raw_record("{}").has_value());
    CHECK_FALSE(decode_raw_record(R"({"t":abc,"s":"x","d":{}})").has_value());
    CHECK_FALSE(decode_raw_record(R"({"t":1,"s":"x","d":})").has_value());
    CHECK_FALSE(decode_raw_record(R"({"t":1,"s":"x","d":{}})"
                                  "garbage")
                    .has_value());
    CHECK_FALSE(decode_raw_record(R"({"t":1,"s":x,"d":{}})").has_value());
    CHECK_FALSE(decode_raw_record(R"({"t":1,"s":"x","d":{})").has_value());
}

TEST_CASE("RawCapturePath: layout and hour parsing") {
    RawCapturePath p{"/data", "binance", "ETHUSDT"};
    CHECK(p.dir() == fs::path("/data/raw/binance/ETHUSDT"));
    CHECK(p.file_for_hour(ts("2024-03-15T07:00:00Z")) ==
          fs::path("/data/raw/binance/ETHUSDT/2024-03-15/07.jsonl.gz"));
    CHECK(*RawCapturePath::hour_of("/data/raw/binance/ETHUSDT/2024-03-15/07.jsonl.gz") ==
          ts("2024-03-15T07:00:00Z"));
    CHECK_FALSE(RawCapturePath::hour_of("/data/raw/binance/ETHUSDT/2024-03-15/notes.txt").has_value());
}

TEST_CASE("RawCaptureWriter: rotates hourly, appends on reopen, reads back in order") {
    TempDir tmp;
    RawCapturePath path{tmp.path, "binance", "ETHUSDT"};

    {
        RawCaptureWriter w(path);
        REQUIRE(w.write({ts("2024-03-15T07:59:59Z"), "a", R"({"n":1})"}).has_value());
        REQUIRE(w.write({ts("2024-03-15T07:59:59.5Z"), "a", R"({"n":2})"}).has_value());
        CHECK(w.current_file() == path.file_for_hour(ts("2024-03-15T07:00:00Z")));
        REQUIRE(w.write({ts("2024-03-15T08:00:00Z"), "b", R"({"n":3})"}).has_value());
        CHECK(w.current_file() == path.file_for_hour(ts("2024-03-15T08:00:00Z")));
        REQUIRE(w.flush().has_value());
        CHECK(w.stats().records == 3);
        CHECK(w.stats().rotations == 2);
        REQUIRE(w.close().has_value());
    }
    {
        // Simulated restart within the same hour: must append, not truncate.
        RawCaptureWriter w(path);
        REQUIRE(w.write({ts("2024-03-15T08:30:00Z"), "b", R"({"n":4})"}).has_value());
    }

    auto files = path.files_in_range(ts("2024-03-15T00:00:00Z"), ts("2024-03-16T00:00:00Z"));
    REQUIRE(files.size() == 2);
    CHECK(files[0].filename() == "07.jsonl.gz");
    CHECK(files[1].filename() == "08.jsonl.gz");
    CHECK(path.files_in_range(ts("2024-03-15T08:00:00Z"), ts("2024-03-15T09:00:00Z")).size() == 1);
    CHECK(path.files_in_range(ts("2024-03-15T07:30:00Z"), ts("2024-03-15T07:45:00Z")).size() == 1);
    CHECK(path.files_in_range(ts("2024-03-15T09:00:00Z"), ts("2024-03-15T10:00:00Z")).empty());

    std::vector<std::string> payloads;
    auto count = for_each_raw_record(path, ts("2024-03-15T00:00:00Z"), ts("2024-03-16T00:00:00Z"),
                                     [&](const RawRecord& r) {
                                         payloads.push_back(r.payload);
                                         return true;
                                     });
    REQUIRE_MESSAGE(count.has_value(), count.error().message);
    CHECK(*count == 4);
    CHECK(payloads == std::vector<std::string>{R"({"n":1})", R"({"n":2})", R"({"n":3})", R"({"n":4})"});

    // Range filtering applies at record granularity, and early stop works.
    payloads.clear();
    count = for_each_raw_record(path, ts("2024-03-15T07:59:59.2Z"), ts("2024-03-15T08:10:00Z"),
                                [&](const RawRecord& r) {
                                    payloads.push_back(r.payload);
                                    return payloads.size() < 2;
                                });
    REQUIRE(count.has_value());
    CHECK(*count == 2);
    CHECK(payloads == std::vector<std::string>{R"({"n":2})", R"({"n":3})"});
}

TEST_CASE("RawCaptureReader: skips corrupt lines and a truncated tail") {
    TempDir tmp;
    RawCapturePath path{tmp.path, "v", "S"};
    const fs::path file = path.file_for_hour(ts("2024-01-01T00:00:00Z"));
    {
        RawCaptureWriter w(path);
        REQUIRE(w.write({ts("2024-01-01T00:00:01Z"), "s", R"({"n":1})"}).has_value());
        REQUIRE(w.close().has_value());
    }
    // Append a second gzip member with one good, one bad, and one partial line.
    {
        gzFile gz = gzopen(file.c_str(), "ab");
        REQUIRE(gz != nullptr);
        const std::string extra = "not json at all\n"
                                  R"({"t":1704067202000000000,"s":"s","d":{"n":2}})"
                                  "\n"
                                  R"({"t":1704067203000000000,"s":"s","d":{"n":)";
        gzwrite(gz, extra.data(), static_cast<unsigned>(extra.size()));
        gzclose(gz);
    }
    RawCaptureReader reader(file);
    REQUIRE(reader.open().has_value());
    RawRecord rec;
    std::vector<std::string> got;
    for (;;) {
        auto more = reader.next(rec);
        REQUIRE(more.has_value());
        if (!*more) {
            break;
        }
        got.push_back(rec.payload);
    }
    CHECK(got == std::vector<std::string>{R"({"n":1})", R"({"n":2})"});
    CHECK(reader.skipped_lines() == 2);
}

TEST_CASE("RawCaptureReader: missing file is an io_error") {
    RawCaptureReader reader("/nonexistent/00.jsonl.gz");
    auto r = reader.open();
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::io_error);
}
