#include "tradebot/util/sha256.hpp"

#include <doctest/doctest.h>

#include "support/fixtures.hpp"

using namespace tradebot::util;

TEST_CASE("sha256_hex: known vectors") {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    Sha256 h;
    h.update("a");
    h.update("bc");
    CHECK(h.finish_hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256_hex_of_file: matches the fixture CHECKSUM") {
    const auto zip = tradebot::test::fixture("ETHUSDT-aggTrades-2024-01-01.zip");
    auto hex = sha256_hex_of_file(zip);
    REQUIRE_MESSAGE(hex.has_value(), hex.error().message);
    const std::string checksum = tradebot::test::read_fixture("ETHUSDT-aggTrades-2024-01-01.zip.CHECKSUM");
    CHECK(checksum.substr(0, 64) == *hex);
    CHECK_FALSE(sha256_hex_of_file("/nonexistent").has_value());
}
