#include "tradebot/net/http.hpp"

#include <charconv>
#include <cstdlib>
#include <cstring>

namespace tradebot::net {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// Buffered reader over a Stream with line and exact-count reads.
class Reader {
public:
    explicit Reader(Stream& s) : stream_(s) {}

    // Reads a line terminated by \n (strips \r\n). Fails on EOF.
    Result<std::string> read_line() {
        for (;;) {
            const std::size_t nl = buf_.find('\n', scan_);
            if (nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                scan_ = 0;
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return line;
            }
            scan_ = buf_.size();
            if (buf_.size() > kMaxLine) {
                return make_error(ErrorCode::protocol_error, "HTTP line too long");
            }
            if (auto r = fill(); !r) {
                return tl::make_unexpected(r.error());
            }
        }
    }

    // Delivers exactly n bytes to sink (from the buffer first).
    Result<void> read_exact(std::size_t n, const BodySink& sink) {
        while (n > 0) {
            if (buf_.empty()) {
                if (auto r = fill(); !r) {
                    return tl::make_unexpected(r.error());
                }
            }
            const std::size_t take = std::min(n, buf_.size());
            if (!deliver(sink, std::string_view(buf_).substr(0, take))) {
                return make_error(ErrorCode::invalid_state, "body sink aborted download");
            }
            buf_.erase(0, take);
            n -= take;
        }
        return {};
    }

    // Delivers everything until EOF.
    Result<void> read_to_eof(const BodySink& sink) {
        for (;;) {
            if (!buf_.empty()) {
                if (!deliver(sink, buf_)) {
                    return make_error(ErrorCode::invalid_state, "body sink aborted download");
                }
                buf_.clear();
            }
            std::byte chunk[16384];
            auto n = stream_.read_some(chunk);
            if (!n) {
                return tl::make_unexpected(n.error());
            }
            if (*n == 0) {
                return {};
            }
            if (!sink(std::span<const std::byte>(chunk, *n))) {
                return make_error(ErrorCode::invalid_state, "body sink aborted download");
            }
        }
    }

private:
    static constexpr std::size_t kMaxLine = 64 * 1024;

    static bool deliver(const BodySink& sink, std::string_view bytes) {
        return sink(std::as_bytes(std::span(bytes.data(), bytes.size())));
    }

    Result<void> fill() {
        std::byte chunk[16384];
        auto n = stream_.read_some(chunk);
        if (!n) {
            return tl::make_unexpected(n.error());
        }
        if (*n == 0) {
            return make_error(ErrorCode::connection_closed, "unexpected EOF in HTTP response");
        }
        buf_.append(reinterpret_cast<const char*>(chunk), *n);
        return {};
    }

    Stream& stream_;
    std::string buf_;
    std::size_t scan_ = 0;
};

Result<HttpResponse> read_response_head(Reader& reader) {
    HttpResponse resp;
    auto status_line = reader.read_line();
    if (!status_line) {
        return tl::make_unexpected(status_line.error());
    }
    // "HTTP/1.1 200 OK"
    if (status_line->size() < 12 || status_line->compare(0, 5, "HTTP/") != 0) {
        return make_error(ErrorCode::protocol_error, "bad HTTP status line: " + *status_line);
    }
    const std::string_view code = std::string_view(*status_line).substr(9, 3);
    if (std::from_chars(code.data(), code.data() + 3, resp.status).ec != std::errc{}) {
        return make_error(ErrorCode::protocol_error, "bad HTTP status code: " + *status_line);
    }
    for (;;) {
        auto line = reader.read_line();
        if (!line) {
            return tl::make_unexpected(line.error());
        }
        if (line->empty()) {
            break;
        }
        const std::size_t colon = line->find(':');
        if (colon == std::string::npos) {
            return make_error(ErrorCode::protocol_error, "bad HTTP header: " + *line);
        }
        resp.headers[to_lower(trim(std::string_view(*line).substr(0, colon)))] =
            std::string(trim(std::string_view(*line).substr(colon + 1)));
    }
    return resp;
}

Result<void> read_body(Reader& reader, const HttpResponse& resp, const BodySink& sink) {
    if (const auto* te = resp.header("transfer-encoding");
        te != nullptr && to_lower(*te).find("chunked") != std::string::npos) {
        for (;;) {
            auto size_line = reader.read_line();
            if (!size_line) {
                return tl::make_unexpected(size_line.error());
            }
            std::string_view size_text = *size_line;
            size_text = size_text.substr(0, size_text.find(';'));
            std::size_t size = 0;
            const auto [ptr, ec] =
                std::from_chars(size_text.data(), size_text.data() + size_text.size(), size, 16);
            if (ec != std::errc{} || ptr == size_text.data()) {
                return make_error(ErrorCode::protocol_error,
                                  "bad chunk size: '" + *size_line + "'");
            }
            if (size == 0) {
                // Trailers until blank line.
                for (;;) {
                    auto t = reader.read_line();
                    if (!t) {
                        return tl::make_unexpected(t.error());
                    }
                    if (t->empty()) {
                        return {};
                    }
                }
            }
            if (auto r = reader.read_exact(size, sink); !r) {
                return r;
            }
            auto crlf = reader.read_line();
            if (!crlf) {
                return tl::make_unexpected(crlf.error());
            }
            if (!crlf->empty()) {
                return make_error(ErrorCode::protocol_error, "missing CRLF after chunk");
            }
        }
    }
    if (const auto* cl = resp.header("content-length"); cl != nullptr) {
        std::size_t len = 0;
        const auto [ptr, ec] = std::from_chars(cl->data(), cl->data() + cl->size(), len);
        if (ec != std::errc{} || ptr != cl->data() + cl->size()) {
            return make_error(ErrorCode::protocol_error, "bad Content-Length: " + *cl);
        }
        return reader.read_exact(len, sink);
    }
    if (resp.status == 204 || resp.status == 304 || (resp.status >= 100 && resp.status < 200)) {
        return {};
    }
    return reader.read_to_eof(sink);
}

}  // namespace

const std::string* HttpResponse::header(std::string_view name) const {
    auto it = headers.find(to_lower(name));
    return it == headers.end() ? nullptr : &it->second;
}

Result<HttpResponse> http_exchange(Stream& stream, const HttpRequest& req, const BodySink& sink) {
    std::string out = req.method + " " + req.url.path_and_query() + " HTTP/1.1\r\n";
    out += "Host: " + req.url.host_header() + "\r\n";
    bool has_connection = false;
    for (const auto& [k, v] : req.headers) {
        out += k + ": " + v + "\r\n";
        if (to_lower(k) == "connection") {
            has_connection = true;
        }
    }
    if (!has_connection) {
        out += "Connection: close\r\n";
    }
    if (!req.body.empty()) {
        out += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    }
    out += "\r\n";
    out += req.body;
    if (auto w = stream.write_all(out); !w) {
        return tl::make_unexpected(w.error());
    }

    Reader reader(stream);
    auto resp = read_response_head(reader);
    if (!resp) {
        return resp;
    }
    if (req.method != "HEAD") {
        if (auto r = read_body(reader, *resp, sink); !r) {
            return tl::make_unexpected(r.error());
        }
    }
    return resp;
}

Result<HttpResponse> http_exchange(Stream& stream, const HttpRequest& req) {
    std::string body;
    auto resp = http_exchange(stream, req, [&](std::span<const std::byte> bytes) {
        body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    });
    if (resp) {
        resp->body = std::move(body);
    }
    return resp;
}

std::optional<ProxyConfig> proxy_from_env() {
    const char* env = std::getenv("HTTPS_PROXY");
    if (env == nullptr || *env == '\0') {
        env = std::getenv("https_proxy");
    }
    if (env == nullptr || *env == '\0') {
        return std::nullopt;
    }
    auto url = Url::parse(env);
    if (!url) {
        return std::nullopt;
    }
    return ProxyConfig{url->host, url->port};
}

Result<std::unique_ptr<Stream>> open_stream(const Url& url, std::shared_ptr<TlsContext> tls,
                                            const ProxyConfig& proxy, Duration timeout) {
    Result<TcpSocket> sock = proxy.enabled() ? TcpSocket::connect(proxy.host, proxy.port, timeout)
                                             : TcpSocket::connect(url.host, url.port, timeout);
    if (!sock) {
        return tl::make_unexpected(sock.error());
    }
    if (proxy.enabled()) {
        const std::string target = url.host + ":" + std::to_string(url.port);
        HttpRequest connect;
        connect.method = "CONNECT";
        connect.url.host = url.host;
        connect.url.port = url.port;
        connect.url.scheme = url.is_secure() ? "https" : "http";
        connect.url.path = target;  // request-target for CONNECT is host:port
        connect.headers = {{"Connection", "keep-alive"}};
        // CONNECT responses have no body; treat as HEAD.
        connect.method = "CONNECT";
        std::string out = "CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\n\r\n";
        if (auto w = sock->write_all(out); !w) {
            return tl::make_unexpected(w.error());
        }
        Reader reader(*sock);
        auto resp = read_response_head(reader);
        if (!resp) {
            return tl::make_unexpected(resp.error());
        }
        if (resp->status != 200) {
            return make_error(ErrorCode::network_error,
                              "proxy refused CONNECT to " + target + ": HTTP " +
                                  std::to_string(resp->status));
        }
    }
    if (!url.is_secure()) {
        return std::unique_ptr<Stream>(new TcpSocket(std::move(*sock)));
    }
    if (!tls) {
        return make_error(ErrorCode::invalid_argument, "TLS context required for " + url.to_string());
    }
    auto tls_stream = TlsStream::handshake(std::move(tls), std::move(*sock), url.host);
    if (!tls_stream) {
        return tl::make_unexpected(tls_stream.error());
    }
    return std::unique_ptr<Stream>(new TlsStream(std::move(*tls_stream)));
}

Result<HttpClient> HttpClient::create(std::shared_ptr<TlsContext> tls, Options opts) {
    HttpClient c;
    c.tls_ = std::move(tls);
    c.proxy_ = opts.proxy ? *opts.proxy : proxy_from_env().value_or(ProxyConfig{});
    c.opts_ = std::move(opts);
    return c;
}

Result<HttpResponse> HttpClient::get(const std::string& url, const Headers& headers) {
    std::string body;
    auto resp = get_streaming(
        url,
        [&](std::span<const std::byte> bytes) {
            body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return true;
        },
        headers);
    if (resp) {
        resp->body = std::move(body);
    }
    return resp;
}

Result<HttpResponse> HttpClient::get_streaming(const std::string& url_text, const BodySink& sink,
                                               const Headers& headers) {
    auto url = Url::parse(url_text);
    if (!url) {
        return tl::make_unexpected(url.error());
    }
    auto stream = open_stream(*url, tls_, proxy_, opts_.timeout);
    if (!stream) {
        return tl::make_unexpected(stream.error());
    }
    HttpRequest req;
    req.url = *url;
    req.headers = headers;
    req.headers.emplace_back("User-Agent", opts_.user_agent);
    req.headers.emplace_back("Accept", "*/*");
    auto resp = http_exchange(**stream, req, sink);
    (*stream)->close();
    return resp;
}

}  // namespace tradebot::net
