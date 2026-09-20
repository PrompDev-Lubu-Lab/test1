#include "tradebot/util/zip_reader.hpp"

#include <zlib.h>

#include <cstring>
#include <fstream>

namespace tradebot::util {

namespace {

constexpr std::uint32_t kEocdSignature = 0x06054b50;
constexpr std::uint32_t kCentralSignature = 0x02014b50;
constexpr std::uint32_t kLocalSignature = 0x04034b50;
constexpr std::uint16_t kMethodStored = 0;
constexpr std::uint16_t kMethodDeflate = 8;

std::uint16_t rd16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t rd32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

Result<void> read_at(std::ifstream& in, std::uint64_t offset, std::span<unsigned char> out,
                     const std::filesystem::path& path) {
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (static_cast<std::size_t>(in.gcount()) != out.size()) {
        return make_error(ErrorCode::io_error, "short read in " + path.string());
    }
    return {};
}

}  // namespace

Result<ZipReader> ZipReader::open(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return make_error(ErrorCode::io_error, "cannot open " + path.string());
    }
    const auto file_size = static_cast<std::uint64_t>(in.tellg());
    if (file_size < 22) {
        return make_error(ErrorCode::parse_error, path.string() + " is too small to be a zip");
    }

    // Find the End Of Central Directory record: scan backwards over at
    // most 64KiB of comment.
    const std::uint64_t tail_size = std::min<std::uint64_t>(file_size, 22 + 65535);
    std::vector<unsigned char> tail(tail_size);
    if (auto r = read_at(in, file_size - tail_size, tail, path); !r) {
        return tl::make_unexpected(r.error());
    }
    std::int64_t eocd = -1;
    for (std::int64_t i = static_cast<std::int64_t>(tail_size) - 22; i >= 0; --i) {
        if (rd32(tail.data() + i) == kEocdSignature) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        return make_error(ErrorCode::parse_error, path.string() + ": no zip end-of-central-directory");
    }
    const unsigned char* e = tail.data() + eocd;
    const std::uint16_t entry_count = rd16(e + 10);
    const std::uint32_t cd_size = rd32(e + 12);
    const std::uint32_t cd_offset = rd32(e + 16);
    if (entry_count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_offset == 0xFFFFFFFF) {
        return make_error(ErrorCode::unsupported, path.string() + ": ZIP64 archives are not supported");
    }

    std::vector<unsigned char> cd(cd_size);
    if (auto r = read_at(in, cd_offset, cd, path); !r) {
        return tl::make_unexpected(r.error());
    }
    ZipReader reader;
    reader.path_ = path;
    std::size_t pos = 0;
    for (std::uint16_t i = 0; i < entry_count; ++i) {
        if (pos + 46 > cd.size() || rd32(cd.data() + pos) != kCentralSignature) {
            return make_error(ErrorCode::parse_error, path.string() + ": corrupt central directory");
        }
        const unsigned char* h = cd.data() + pos;
        ZipEntry entry;
        const std::uint16_t flags = rd16(h + 8);
        entry.method = rd16(h + 10);
        entry.crc32 = rd32(h + 16);
        entry.compressed_size = rd32(h + 20);
        entry.uncompressed_size = rd32(h + 24);
        const std::uint16_t name_len = rd16(h + 28);
        const std::uint16_t extra_len = rd16(h + 30);
        const std::uint16_t comment_len = rd16(h + 32);
        entry.local_header_offset = rd32(h + 42);
        if (pos + 46 + name_len > cd.size()) {
            return make_error(ErrorCode::parse_error, path.string() + ": corrupt central directory");
        }
        entry.name.assign(reinterpret_cast<const char*>(h + 46), name_len);
        if ((flags & 0x1) != 0) {
            return make_error(ErrorCode::unsupported,
                              path.string() + ": encrypted entry " + entry.name);
        }
        if (entry.method != kMethodStored && entry.method != kMethodDeflate) {
            return make_error(ErrorCode::unsupported, path.string() + ": entry " + entry.name +
                                                          " uses unsupported compression method " +
                                                          std::to_string(entry.method));
        }
        reader.entries_.push_back(std::move(entry));
        pos += 46u + name_len + extra_len + comment_len;
    }
    return reader;
}

const ZipEntry* ZipReader::find(std::string_view name) const noexcept {
    for (const auto& e : entries_) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

Result<void> ZipReader::extract(const ZipEntry& entry, const ByteSink& sink) const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return make_error(ErrorCode::io_error, "cannot open " + path_.string());
    }
    unsigned char lh[30];
    if (auto r = read_at(in, entry.local_header_offset, lh, path_); !r) {
        return r;
    }
    if (rd32(lh) != kLocalSignature) {
        return make_error(ErrorCode::parse_error,
                          path_.string() + ": bad local header for " + entry.name);
    }
    const std::uint64_t data_offset =
        entry.local_header_offset + 30u + rd16(lh + 26) + rd16(lh + 28);
    in.seekg(static_cast<std::streamoff>(data_offset));

    std::uint32_t crc = static_cast<std::uint32_t>(crc32(0L, Z_NULL, 0));
    std::uint64_t produced = 0;
    auto emit = [&](const unsigned char* data, std::size_t n) -> bool {
        crc = static_cast<std::uint32_t>(crc32(crc, data, static_cast<uInt>(n)));
        produced += n;
        return sink(std::as_bytes(std::span(reinterpret_cast<const std::byte*>(data), n)));
    };

    std::vector<unsigned char> in_buf(256 * 1024);
    std::uint64_t remaining = entry.compressed_size;

    if (entry.method == kMethodStored) {
        while (remaining > 0) {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, in_buf.size()));
            in.read(reinterpret_cast<char*>(in_buf.data()), static_cast<std::streamsize>(want));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (got == 0) {
                return make_error(ErrorCode::io_error, path_.string() + ": truncated entry " + entry.name);
            }
            if (!emit(in_buf.data(), got)) {
                return make_error(ErrorCode::invalid_state, "sink aborted extraction");
            }
            remaining -= got;
        }
    } else {
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {  // raw deflate
            return make_error(ErrorCode::io_error, "inflateInit2 failed");
        }
        std::vector<unsigned char> out_buf(256 * 1024);
        int rc = Z_OK;
        while (rc != Z_STREAM_END) {
            if (zs.avail_in == 0) {
                if (remaining == 0) {
                    inflateEnd(&zs);
                    return make_error(ErrorCode::parse_error,
                                      path_.string() + ": deflate stream ended early in " + entry.name);
                }
                const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, in_buf.size()));
                in.read(reinterpret_cast<char*>(in_buf.data()), static_cast<std::streamsize>(want));
                const auto got = static_cast<std::size_t>(in.gcount());
                if (got == 0) {
                    inflateEnd(&zs);
                    return make_error(ErrorCode::io_error, path_.string() + ": truncated entry " + entry.name);
                }
                remaining -= got;
                zs.next_in = in_buf.data();
                zs.avail_in = static_cast<uInt>(got);
            }
            do {
                zs.next_out = out_buf.data();
                zs.avail_out = static_cast<uInt>(out_buf.size());
                rc = inflate(&zs, Z_NO_FLUSH);
                if (rc != Z_OK && rc != Z_STREAM_END) {
                    inflateEnd(&zs);
                    return make_error(ErrorCode::parse_error,
                                      path_.string() + ": inflate error in " + entry.name + ": " +
                                          (zs.msg ? zs.msg : "unknown"));
                }
                const std::size_t n = out_buf.size() - zs.avail_out;
                if (n > 0 && !emit(out_buf.data(), n)) {
                    inflateEnd(&zs);
                    return make_error(ErrorCode::invalid_state, "sink aborted extraction");
                }
            } while (zs.avail_out == 0 && rc != Z_STREAM_END);
        }
        inflateEnd(&zs);
    }

    if (produced != entry.uncompressed_size) {
        return make_error(ErrorCode::parse_error,
                          path_.string() + ": size mismatch in " + entry.name);
    }
    if (crc != entry.crc32) {
        return make_error(ErrorCode::parse_error, path_.string() + ": CRC mismatch in " + entry.name);
    }
    return {};
}

Result<std::string> ZipReader::extract_to_string(const ZipEntry& entry) const {
    std::string out;
    out.reserve(entry.uncompressed_size);
    auto r = extract(entry, [&](std::span<const std::byte> bytes) {
        out.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    });
    if (!r) {
        return tl::make_unexpected(r.error());
    }
    return out;
}

Result<std::uint64_t> ZipReader::for_each_line(
    const ZipEntry& entry, const std::function<bool(std::string_view)>& fn) const {
    std::string carry;
    std::uint64_t lines = 0;
    bool stopped = false;
    auto r = extract(entry, [&](std::span<const std::byte> bytes) {
        std::string_view chunk(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = chunk.find('\n', start);
            if (nl == std::string_view::npos) {
                carry.append(chunk.substr(start));
                return true;
            }
            std::string_view line;
            if (carry.empty()) {
                line = chunk.substr(start, nl - start);
            } else {
                carry.append(chunk.substr(start, nl - start));
                line = carry;
            }
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            ++lines;
            if (!fn(line)) {
                stopped = true;
                return false;
            }
            carry.clear();
            start = nl + 1;
        }
    });
    if (!r) {
        if (stopped) {
            return lines;
        }
        return tl::make_unexpected(r.error());
    }
    if (!carry.empty()) {
        std::string_view line = carry;
        if (line.back() == '\r') {
            line.remove_suffix(1);
        }
        ++lines;
        static_cast<void>(fn(line));
    }
    return lines;
}

}  // namespace tradebot::util
