#include "tradebot/util/zip_reader.hpp"

#include <doctest/doctest.h>

#include "support/fixtures.hpp"

#include <fstream>
#include <random>

using namespace tradebot;
using namespace tradebot::util;

TEST_CASE("ZipReader: lists entries and extracts stored, deflated and empty entries") {
    auto zip = ZipReader::open(test::fixture("mixed.zip"));
    REQUIRE_MESSAGE(zip.has_value(), zip.error().message);
    REQUIRE(zip->entries().size() == 3);
    CHECK(zip->entries()[0].name == "stored.txt");
    CHECK(zip->entries()[0].method == 0);
    CHECK(zip->entries()[1].name == "dir/deflated.txt");
    CHECK(zip->entries()[1].method == 8);
    CHECK(zip->entries()[1].uncompressed_size == 100003);
    CHECK(zip->find("empty.txt") != nullptr);
    CHECK(zip->find("nope") == nullptr);

    CHECK(*zip->extract_to_string(*zip->find("stored.txt")) == "stored bytes");
    auto big = zip->extract_to_string(*zip->find("dir/deflated.txt"));
    REQUIRE(big.has_value());
    CHECK(big->size() == 100003);
    CHECK(big->substr(100000) == "END");
    CHECK(big->find_first_not_of('x') == 100000);
    CHECK(zip->extract_to_string(*zip->find("empty.txt"))->empty());

    // Streaming delivers chunks and honours abort.
    std::size_t calls = 0;
    auto r = zip->extract(*zip->find("dir/deflated.txt"), [&](std::span<const std::byte>) {
        ++calls;
        return false;
    });
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::invalid_state);
    CHECK(calls == 1);
}

TEST_CASE("ZipReader: for_each_line on a Binance-style CSV") {
    auto zip = ZipReader::open(test::fixture("ETHUSDT-aggTrades-2024-01-01.zip"));
    REQUIRE(zip.has_value());
    REQUIRE(zip->entries().size() == 1);
    CHECK(zip->entries()[0].name == "ETHUSDT-aggTrades-2024-01-01.csv");
    std::vector<std::string> lines;
    auto n = zip->for_each_line(zip->entries()[0], [&](std::string_view line) {
        lines.emplace_back(line);
        return true;
    });
    REQUIRE_MESSAGE(n.has_value(), n.error().message);
    CHECK(*n == 5);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "100,3000.00,0.1000,200,200,1704067200000,false,true");
    CHECK(lines[4] == "104,3002.00,0.5000,204,204,1704067204000,false,true");

    // Early stop.
    lines.clear();
    n = zip->for_each_line(zip->entries()[0], [&](std::string_view line) {
        lines.emplace_back(line);
        return lines.size() < 2;
    });
    REQUIRE(n.has_value());
    CHECK(*n == 2);
}

TEST_CASE("ZipReader: rejects non-zip and corrupt files") {
    std::random_device rd;
    const auto tmp = std::filesystem::temp_directory_path() / ("tb-zip-" + std::to_string(rd()));
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "this is not a zip file, but it is long enough to be scanned for a signature";
    }
    auto r = ZipReader::open(tmp);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::parse_error);

    // Corrupt the compressed payload of a real archive: CRC must catch it.
    std::string bytes = test::read_fixture("ETHUSDT-aggTrades-2024-01-01.zip");
    // First entry's data begins after its 30-byte local header, name and extra.
    const auto u16 = [&](std::size_t i) {
        return static_cast<std::size_t>(static_cast<unsigned char>(bytes[i])) |
               (static_cast<std::size_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
    };
    const std::size_t data_offset = 30u + u16(26) + u16(28);
    bytes[data_offset + 5] = static_cast<char>(bytes[data_offset + 5] ^ 0x55);
    {
        std::ofstream out(tmp, std::ios::binary);
        out << bytes;
    }
    auto zip = ZipReader::open(tmp);
    REQUIRE(zip.has_value());
    auto data = zip->extract_to_string(zip->entries()[0]);
    CHECK_FALSE(data.has_value());
    std::filesystem::remove(tmp);

    CHECK_FALSE(ZipReader::open("/nonexistent.zip").has_value());
}
