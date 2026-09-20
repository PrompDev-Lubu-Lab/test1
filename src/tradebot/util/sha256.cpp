#include "tradebot/util/sha256.hpp"

#include <openssl/evp.h>

#include <fstream>

namespace tradebot::util {

Sha256::Sha256() : ctx_(EVP_MD_CTX_new()) {
    EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_), EVP_sha256(), nullptr);
}

Sha256::~Sha256() { EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_)); }

void Sha256::update(std::span<const std::byte> data) {
    EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), data.data(), data.size());
}

std::string Sha256::finish_hex() {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx_), digest, &len);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

std::string sha256_hex(std::string_view text) {
    Sha256 h;
    h.update(text);
    return h.finish_hex();
}

Result<std::string> sha256_hex_of_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return make_error(ErrorCode::io_error, "cannot open " + path.string());
    }
    Sha256 h;
    char buf[64 * 1024];
    while (in) {
        in.read(buf, sizeof buf);
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n > 0) {
            h.update(std::as_bytes(std::span(buf, n)));
        }
    }
    return h.finish_hex();
}

}  // namespace tradebot::util
