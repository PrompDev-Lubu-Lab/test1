#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef TRADEBOT_TEST_FIXTURES_DIR
#error "TRADEBOT_TEST_FIXTURES_DIR must be defined by the build"
#endif

namespace tradebot::test {

inline std::filesystem::path fixture(const std::string& name) {
    return std::filesystem::path(TRADEBOT_TEST_FIXTURES_DIR) / name;
}

inline std::string read_fixture(const std::string& name) {
    std::ifstream in(fixture(name), std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace tradebot::test
