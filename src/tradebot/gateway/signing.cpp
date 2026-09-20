#include "tradebot/gateway/signing.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace tradebot::gateway {

std::string hmac_sha256_hex(std::string_view key, std::string_view message) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest, &len);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

std::string url_encode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string encode_query(const Params& params) {
    std::string q;
    for (const auto& [k, v] : params) {
        if (!q.empty()) q.push_back('&');
        q += url_encode(k);
        q.push_back('=');
        q += url_encode(v);
    }
    return q;
}

std::string signed_query(Params params, std::string_view secret, std::int64_t timestamp_ms,
                         std::int64_t recv_window_ms) {
    params.emplace_back("recvWindow", std::to_string(recv_window_ms));
    params.emplace_back("timestamp", std::to_string(timestamp_ms));
    const std::string q = encode_query(params);
    return q + "&signature=" + hmac_sha256_hex(secret, q);
}

}  // namespace tradebot::gateway
