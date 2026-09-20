#pragma once

#include "tradebot/core/error.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace tradebot::util {

// Incremental SHA-256 over OpenSSL EVP.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(std::span<const std::byte> data);
    void update(std::string_view text) {
        update(std::as_bytes(std::span(text.data(), text.size())));
    }
    // Lower-case hex digest; the object must not be used afterwards.
    [[nodiscard]] std::string finish_hex();

private:
    void* ctx_;
};

[[nodiscard]] std::string sha256_hex(std::string_view text);
[[nodiscard]] Result<std::string> sha256_hex_of_file(const std::filesystem::path& path);

}  // namespace tradebot::util
