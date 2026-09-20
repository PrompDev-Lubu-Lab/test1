#include "tradebot/net/http.hpp"
#include "tradebot/net/tcp.hpp"
#include "tradebot/net/tls.hpp"

#include <doctest/doctest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <memory>
#include <thread>

using namespace tradebot;
using namespace tradebot::net;

namespace {

// Generates an in-memory self-signed certificate + key and builds a server
// SSL_CTX from them, so the TLS client path is exercised end to end without
// touching the filesystem or the network.
struct SelfSignedServerCtx {
    SSL_CTX* ctx = nullptr;

    SelfSignedServerCtx() {
        EVP_PKEY* key = EVP_RSA_gen(2048);
        REQUIRE(key != nullptr);
        X509* cert = X509_new();
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        X509_NAME* name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("localhost"), -1, -1,
                                   0);
        X509_set_issuer_name(cert, name);
        REQUIRE(X509_sign(cert, key, EVP_sha256()) > 0);

        ctx = SSL_CTX_new(TLS_server_method());
        REQUIRE(ctx != nullptr);
        REQUIRE(SSL_CTX_use_certificate(ctx, cert) == 1);
        REQUIRE(SSL_CTX_use_PrivateKey(ctx, key) == 1);
        X509_free(cert);
        EVP_PKEY_free(key);
    }
    ~SelfSignedServerCtx() { SSL_CTX_free(ctx); }
};

// Accepts one TLS connection, echoes one record, then responds to an HTTP
// request with a fixed body.
void run_tls_server(SSL_CTX* ctx, TcpListener& listener, bool& handshake_ok) {
    auto conn = listener.accept(Duration::seconds(10));
    REQUIRE(conn.has_value());
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, conn->fd());
    handshake_ok = SSL_accept(ssl) == 1;
    if (handshake_ok) {
        char buf[4096];
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
            const int n = SSL_read(ssl, buf, sizeof buf);
            if (n <= 0) {
                break;
            }
            req.append(buf, static_cast<std::size_t>(n));
        }
        const std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecure";
        SSL_write(ssl, resp.data(), static_cast<int>(resp.size()));
        SSL_shutdown(ssl);
    }
    SSL_free(ssl);
}

}  // namespace

TEST_CASE("TlsStream: handshake and HTTPS GET against self-signed server (no verify)") {
    SelfSignedServerCtx server_ctx;
    auto listener = *TcpListener::bind_loopback();
    bool handshake_ok = false;
    std::thread t([&] { run_tls_server(server_ctx.ctx, listener, handshake_ok); });

    auto tls = TlsContext::create({.verify_peer = false});
    REQUIRE_MESSAGE(tls.has_value(), tls.error().message);
    auto client = *HttpClient::create(*tls, {.timeout = Duration::seconds(5),
                                             .proxy = ProxyConfig{}});
    auto resp = client.get("https://127.0.0.1:" + std::to_string(listener.port()) + "/x");
    t.join();
    REQUIRE_MESSAGE(resp.has_value(), resp.error().message);
    CHECK(handshake_ok);
    CHECK(resp->status == 200);
    CHECK(resp->body == "secure");
}

TEST_CASE("TlsStream: verification rejects a self-signed server") {
    SelfSignedServerCtx server_ctx;
    auto listener = *TcpListener::bind_loopback();
    bool handshake_ok = false;
    std::thread t([&] { run_tls_server(server_ctx.ctx, listener, handshake_ok); });

    auto tls = TlsContext::create();
    REQUIRE_MESSAGE(tls.has_value(), tls.error().message);
    auto client = *HttpClient::create(*tls, {.timeout = Duration::seconds(5),
                                             .proxy = ProxyConfig{}});
    auto resp = client.get("https://localhost:" + std::to_string(listener.port()) + "/x");
    t.join();
    REQUIRE_FALSE(resp.has_value());
    CHECK(resp.error().code == ErrorCode::tls_error);
    CHECK_FALSE(handshake_ok);
}

TEST_CASE("TlsContext: missing CA file is an error") {
    auto tls = TlsContext::create({.ca_file = "/nonexistent/ca.pem"});
    REQUIRE_FALSE(tls.has_value());
    CHECK(tls.error().code == ErrorCode::tls_error);
}

TEST_CASE("TlsStream: handshake against a plaintext peer fails cleanly") {
    auto listener = *TcpListener::bind_loopback();
    std::thread t([&] {
        auto conn = *listener.accept(Duration::seconds(5));
        std::byte buf[512];
        static_cast<void>(conn.read_some(buf));
        static_cast<void>(conn.write_all("not tls at all\r\n"));
    });
    auto tls = *TlsContext::create({.verify_peer = false});
    auto sock = *TcpSocket::connect("127.0.0.1", listener.port(), Duration::seconds(5));
    auto stream = TlsStream::handshake(tls, std::move(sock), "localhost");
    t.join();
    REQUIRE_FALSE(stream.has_value());
    CHECK(stream.error().code == ErrorCode::tls_error);
}
