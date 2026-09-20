#pragma once

// Minimal read-only ZIP archive support: enough for exchange bulk-data
// archives (single deflated CSV per zip). Entries are extracted by
// streaming inflated bytes to a sink so multi-gigabyte CSVs never need to
// fit in memory. Supports the "stored" and "deflate" methods only; ZIP64
// and encryption are rejected with an error.

#include "tradebot/core/error.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace tradebot::util {

struct ZipEntry {
    std::string name;
    std::uint16_t method = 0;  // 0 = stored, 8 = deflate
    std::uint32_t compressed_size = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t local_header_offset = 0;
};

using ByteSink = std::function<bool(std::span<const std::byte>)>;

class ZipReader {
public:
    [[nodiscard]] static Result<ZipReader> open(const std::filesystem::path& path);

    [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const ZipEntry* find(std::string_view name) const noexcept;

    // Streams the entry's uncompressed bytes to sink and verifies the CRC.
    [[nodiscard]] Result<void> extract(const ZipEntry& entry, const ByteSink& sink) const;
    [[nodiscard]] Result<std::string> extract_to_string(const ZipEntry& entry) const;

    // Splits an entry into lines (without the '\n'; a trailing '\r' is
    // stripped) and calls fn for each. Returning false stops early.
    [[nodiscard]] Result<std::uint64_t> for_each_line(
        const ZipEntry& entry, const std::function<bool(std::string_view)>& fn) const;

private:
    std::filesystem::path path_;
    std::vector<ZipEntry> entries_;
};

}  // namespace tradebot::util
