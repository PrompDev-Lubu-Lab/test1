#pragma once

// Minimal HTTP/1.1 client: enough for exchange REST endpoints and bulk
// archive downloads. Supports Content-Length and chunked bodies, streaming
// the body to a callback (so multi-hundred-megabyte archives never sit in
// memory), and tunnelling through an HTTP CONNECT proxy.

#include "tradebot/core/error.hpp"
#include "tradebot/core/time.hpp"
#include "tradebot/net/stream.hpp"
#include "tradebot/net/tls.hpp"
#include "tradebot/net/url.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tradebot::net {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;

    [[nodiscard]] bool ok() const noexcept { return status >= 200 && status < 300; }
    [[nodiscard]] const std::string* header(std::string_view name) const;
};

// Body bytes are delivered incrementally; return false to abort.
using BodySink = std::function<bool(std::span<const std::byte>)>;

struct HttpRequest {
    std::string method = "GET";
    Url url;
    Headers headers;
    std::string body;
};

// Sends one request on an already-connected stream and reads the response.
// Header-only variant returns the parsed headers and streams the body.
[[nodiscard]] Result<HttpResponse> http_exchange(Stream& stream, const HttpRequest& req,
                                                 const BodySink& sink);
[[nodiscard]] Result<HttpResponse> http_exchange(Stream& stream, const HttpRequest& req);

struct ProxyConfig {
    std::string host;
    std::uint16_t port = 0;
    [[nodiscard]] bool enabled() const noexcept { return !host.empty(); }
};

// Reads HTTPS_PROXY / https_proxy (a plain http://host:port URL).
[[nodiscard]] std::optional<ProxyConfig> proxy_from_env();

// Opens a TCP (+TLS when the URL is secure) connection to the URL's host,
// via the proxy when one is configured. Shared by HTTP and WebSocket.
[[nodiscard]] Result<std::unique_ptr<Stream>> open_stream(const Url& url,
                                                          std::shared_ptr<TlsContext> tls,
                                                          const ProxyConfig& proxy,
                                                          Duration timeout);

struct HttpClientOptions {
    Duration timeout = Duration::seconds(30);
    std::optional<ProxyConfig> proxy;  // nullopt = read from environment
    std::string user_agent = "tradebot/0.1";
};

class HttpClient {
public:
    using Options = HttpClientOptions;

    [[nodiscard]] static Result<HttpClient> create(std::shared_ptr<TlsContext> tls,
                                                   Options opts = Options{});

    // One connection per request; exchange REST calls are infrequent enough
    // that keep-alive is not worth the state.
    [[nodiscard]] Result<HttpResponse> get(const std::string& url, const Headers& headers = {});
    [[nodiscard]] Result<HttpResponse> get_streaming(const std::string& url, const BodySink& sink,
                                                     const Headers& headers = {});

private:
    HttpClient() = default;
    std::shared_ptr<TlsContext> tls_;
    Options opts_;
    ProxyConfig proxy_;
};

}  // namespace tradebot::net
