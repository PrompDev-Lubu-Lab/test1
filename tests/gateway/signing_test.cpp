#include "tradebot/gateway/signing.hpp"

#include <doctest/doctest.h>

using namespace tradebot::gateway;

TEST_CASE("hmac_sha256_hex: RFC 4231 test vector") {
    // Test case 2 from RFC 4231: key "Jefe", data "what do ya want for nothing?"
    CHECK(hmac_sha256_hex("Jefe", "what do ya want for nothing?") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("hmac_sha256_hex: Binance-style query (cross-checked with `openssl dgst -sha256 -hmac`)") {
    const std::string secret = "NhqPtmdSJYdKjVHjA7PZj4Yge3FbwGEYcdsQfYq5MvIU3jScnkT5T0Tn5Ct8lu9k";
    const std::string query =
        "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";
    CHECK(hmac_sha256_hex(secret, query) == "97ec4fbb440129d92c4703ff637f570598036232026ea0b5567999b9f32469e6");
}

TEST_CASE("url_encode and encode_query") {
    CHECK(url_encode("abc-_.~123") == "abc-_.~123");
    CHECK(url_encode("a b&c=d/") == "a%20b%26c%3Dd%2F");
    CHECK(encode_query({{"symbol", "ETHUSDT"}, {"q", "1.5"}, {"note", "a b"}}) == "symbol=ETHUSDT&q=1.5&note=a%20b");
    CHECK(encode_query({}).empty());
}

TEST_CASE("signed_query appends recvWindow, timestamp and signature") {
    const std::string q = signed_query({{"symbol", "ETHUSDT"}}, "secret", 1700000000000, 5000);
    CHECK(q.find("symbol=ETHUSDT&recvWindow=5000&timestamp=1700000000000&signature=") == 0);
    const std::string sig = q.substr(q.find("signature=") + 10);
    CHECK(sig.size() == 64);
    CHECK(sig == hmac_sha256_hex("secret", "symbol=ETHUSDT&recvWindow=5000&timestamp=1700000000000"));
}
