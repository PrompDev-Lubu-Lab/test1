#include "tradebot/net/tls.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cerrno>
#include <cstring>

namespace tradebot::net {

std::string openssl_error_text() {
    std::string out;
    unsigned long e = 0;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof buf);
        if (!out.empty()) {
            out += "; ";
        }
        out += buf;
    }
    return out.empty() ? "unknown OpenSSL error" : out;
}

Result<std::shared_ptr<TlsContext>> TlsContext::create(const Options& opts) {
    std::shared_ptr<TlsContext> ctx(new TlsContext());
    ctx->ctx_ = SSL_CTX_new(TLS_client_method());
    if (ctx->ctx_ == nullptr) {
        return make_error(ErrorCode::tls_error, "SSL_CTX_new: " + openssl_error_text());
    }
    SSL_CTX_set_min_proto_version(ctx->ctx_, TLS1_2_VERSION);
    SSL_CTX_set_mode(ctx->ctx_, SSL_MODE_AUTO_RETRY);
    ctx->verify_peer_ = opts.verify_peer;
    if (opts.verify_peer) {
        if (SSL_CTX_set_default_verify_paths(ctx->ctx_) != 1) {
            return make_error(ErrorCode::tls_error,
                              "SSL_CTX_set_default_verify_paths: " + openssl_error_text());
        }
        if (!opts.ca_file.empty() &&
            SSL_CTX_load_verify_locations(ctx->ctx_, opts.ca_file.c_str(), nullptr) != 1) {
            return make_error(ErrorCode::tls_error,
                              "cannot load CA file " + opts.ca_file + ": " + openssl_error_text());
        }
        SSL_CTX_set_verify(ctx->ctx_, SSL_VERIFY_PEER, nullptr);
    } else {
        SSL_CTX_set_verify(ctx->ctx_, SSL_VERIFY_NONE, nullptr);
    }
    return ctx;
}

TlsContext::~TlsContext() {
    if (ctx_ != nullptr) {
        SSL_CTX_free(ctx_);
    }
}

TlsStream::~TlsStream() { close(); }

TlsStream::TlsStream(TlsStream&& o) noexcept
    : ctx_(std::move(o.ctx_)), socket_(std::move(o.socket_)), ssl_(o.ssl_) {
    o.ssl_ = nullptr;
}

TlsStream& TlsStream::operator=(TlsStream&& o) noexcept {
    if (this != &o) {
        close();
        ctx_ = std::move(o.ctx_);
        socket_ = std::move(o.socket_);
        ssl_ = o.ssl_;
        o.ssl_ = nullptr;
    }
    return *this;
}

Result<TlsStream> TlsStream::handshake(std::shared_ptr<TlsContext> ctx, TcpSocket socket,
                                       const std::string& server_name) {
    TlsStream s;
    s.ctx_ = std::move(ctx);
    s.socket_ = std::move(socket);
    s.ssl_ = SSL_new(s.ctx_->native());
    if (s.ssl_ == nullptr) {
        return make_error(ErrorCode::tls_error, "SSL_new: " + openssl_error_text());
    }
    // Equivalent to the SSL_set_tlsext_host_name macro without its C-style cast.
    SSL_ctrl(s.ssl_, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
             const_cast<char*>(server_name.c_str()));
    if (s.ctx_->verify_peer()) {
        SSL_set1_host(s.ssl_, server_name.c_str());
    }
    if (SSL_set_fd(s.ssl_, s.socket_.fd()) != 1) {
        return make_error(ErrorCode::tls_error, "SSL_set_fd: " + openssl_error_text());
    }
    ERR_clear_error();
    const int rc = SSL_connect(s.ssl_);
    if (rc != 1) {
        auto err = s.map_error(rc, "TLS handshake");
        return tl::make_unexpected(err.error());
    }
    if (s.ctx_->verify_peer() && SSL_get_verify_result(s.ssl_) != X509_V_OK) {
        return make_error(ErrorCode::tls_error,
                          std::string("certificate verification failed: ") +
                              X509_verify_cert_error_string(SSL_get_verify_result(s.ssl_)));
    }
    return s;
}

Result<void> TlsStream::map_error(int rc, const char* what) {
    const int ssl_err = SSL_get_error(ssl_, rc);
    switch (ssl_err) {
        case SSL_ERROR_ZERO_RETURN:
            return make_error(ErrorCode::connection_closed, std::string(what) + ": TLS closed");
        case SSL_ERROR_SYSCALL: {
#if EAGAIN == EWOULDBLOCK
            if (errno == EAGAIN) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
                return make_error(ErrorCode::timeout, std::string(what) + ": timed out");
            }
            if (errno == 0 || errno == ECONNRESET || errno == EPIPE) {
                return make_error(ErrorCode::connection_closed,
                                  std::string(what) + ": connection closed");
            }
            return make_error(ErrorCode::network_error,
                              std::string(what) + ": " + std::strerror(errno));
        }
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return make_error(ErrorCode::timeout, std::string(what) + ": timed out");
        default:
            return make_error(ErrorCode::tls_error, std::string(what) + ": " + openssl_error_text());
    }
}

Result<std::size_t> TlsStream::read_some(std::span<std::byte> buf) {
    if (ssl_ == nullptr) {
        return make_error(ErrorCode::connection_closed, "read on closed TLS stream");
    }
    ERR_clear_error();
    errno = 0;
    std::size_t n = 0;
    const int rc = SSL_read_ex(ssl_, buf.data(), buf.size(), &n);
    if (rc == 1) {
        return n;
    }
    auto err = map_error(rc, "TLS read");
    if (err.error().code == ErrorCode::connection_closed) {
        return static_cast<std::size_t>(0);
    }
    return tl::make_unexpected(err.error());
}

Result<void> TlsStream::write_all(std::span<const std::byte> data) {
    if (ssl_ == nullptr) {
        return make_error(ErrorCode::connection_closed, "write on closed TLS stream");
    }
    std::size_t off = 0;
    while (off < data.size()) {
        ERR_clear_error();
        errno = 0;
        std::size_t n = 0;
        const int rc = SSL_write_ex(ssl_, data.data() + off, data.size() - off, &n);
        if (rc != 1) {
            return map_error(rc, "TLS write");
        }
        off += n;
    }
    return {};
}

void TlsStream::close() noexcept {
    if (ssl_ != nullptr) {
        SSL_shutdown(ssl_);  // best effort, one-way
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    socket_.close();
}

}  // namespace tradebot::net
