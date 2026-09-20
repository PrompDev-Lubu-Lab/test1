#include "tradebot/net/tcp.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace tradebot::net {

namespace {

// EAGAIN and EWOULDBLOCK are the same value on Linux but not everywhere.
bool is_would_block(int e) noexcept {
#if EAGAIN == EWOULDBLOCK
    return e == EAGAIN;
#else
    return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

std::string errno_text(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

timeval to_timeval(Duration d) {
    timeval tv{};
    const auto us = d.count_micros();
    tv.tv_sec = us / 1'000'000;
    tv.tv_usec = us % 1'000'000;
    return tv;
}

}  // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

Result<TcpSocket> TcpSocket::connect(const std::string& host, std::uint16_t port,
                                     Duration timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port_text = std::to_string(port);
    if (const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &res); rc != 0) {
        return make_error(ErrorCode::network_error,
                          "getaddrinfo(" + host + "): " + ::gai_strerror(rc));
    }

    std::string last_error = "no addresses";
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) {
            last_error = errno_text("socket");
            continue;
        }
        // Non-blocking connect with poll so the timeout applies to connect too.
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno != EINPROGRESS) {
            last_error = errno_text("connect");
            ::close(fd);
            continue;
        }
        if (rc < 0) {
            pollfd pfd{fd, POLLOUT, 0};
            rc = ::poll(&pfd, 1, static_cast<int>(timeout.count_millis()));
            if (rc == 0) {
                last_error = "connect timed out";
                ::close(fd);
                continue;
            }
            int so_error = 0;
            socklen_t len = sizeof so_error;
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (rc < 0 || so_error != 0) {
                errno = so_error != 0 ? so_error : errno;
                last_error = errno_text("connect");
                ::close(fd);
                continue;
            }
        }
        ::fcntl(fd, F_SETFL, flags);
        ::freeaddrinfo(res);
        TcpSocket sock(fd);
        if (auto r = sock.set_timeouts(timeout, timeout); !r) {
            return tl::make_unexpected(r.error());
        }
        static_cast<void>(sock.set_nodelay(true));
        return sock;
    }
    ::freeaddrinfo(res);
    return make_error(ErrorCode::network_error,
                      "connect to " + host + ":" + port_text + " failed: " + last_error);
}

Result<void> TcpSocket::set_timeouts(Duration read_timeout, Duration write_timeout) {
    const timeval rt = to_timeval(read_timeout);
    const timeval wt = to_timeval(write_timeout);
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt) < 0 ||
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &wt, sizeof wt) < 0) {
        return make_error(ErrorCode::network_error, errno_text("setsockopt(timeout)"));
    }
    return {};
}

Result<void> TcpSocket::set_nodelay(bool on) {
    const int v = on ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v) < 0) {
        return make_error(ErrorCode::network_error, errno_text("setsockopt(TCP_NODELAY)"));
    }
    return {};
}

Result<std::size_t> TcpSocket::read_some(std::span<std::byte> buf) {
    if (fd_ < 0) {
        return make_error(ErrorCode::connection_closed, "read on closed socket");
    }
    for (;;) {
        const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
        if (n >= 0) {
            return static_cast<std::size_t>(n);
        }
        if (errno == EINTR) {
            continue;
        }
        if (is_would_block(errno)) {
            return make_error(ErrorCode::timeout, "socket read timed out");
        }
        return make_error(ErrorCode::network_error, errno_text("recv"));
    }
}

Result<void> TcpSocket::write_all(std::span<const std::byte> data) {
    if (fd_ < 0) {
        return make_error(ErrorCode::connection_closed, "write on closed socket");
    }
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (is_would_block(errno)) {
                return make_error(ErrorCode::timeout, "socket write timed out");
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                return make_error(ErrorCode::connection_closed, errno_text("send"));
            }
            return make_error(ErrorCode::network_error, errno_text("send"));
        }
        off += static_cast<std::size_t>(n);
    }
    return {};
}

void TcpSocket::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// --- TcpListener ------------------------------------------------------------

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& o) noexcept : fd_(o.fd_), port_(o.port_) { o.fd_ = -1; }

TcpListener& TcpListener::operator=(TcpListener&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        port_ = o.port_;
        o.fd_ = -1;
    }
    return *this;
}

Result<TcpListener> TcpListener::bind_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return make_error(ErrorCode::network_error, errno_text("socket"));
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || ::listen(fd, 16) < 0) {
        const std::string err = errno_text("bind/listen");
        ::close(fd);
        return make_error(ErrorCode::network_error, err);
    }
    socklen_t len = sizeof addr;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    TcpListener l;
    l.fd_ = fd;
    l.port_ = ntohs(addr.sin_port);
    return l;
}

Result<TcpSocket> TcpListener::accept(Duration timeout) {
    pollfd pfd{fd_, POLLIN, 0};
    const int rc = ::poll(&pfd, 1, static_cast<int>(timeout.count_millis()));
    if (rc == 0) {
        return make_error(ErrorCode::timeout, "accept timed out");
    }
    if (rc < 0) {
        return make_error(ErrorCode::network_error, errno_text("poll"));
    }
    const int cfd = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (cfd < 0) {
        return make_error(ErrorCode::network_error, errno_text("accept"));
    }
    TcpSocket sock(cfd);
    if (auto r = sock.set_timeouts(timeout, timeout); !r) {
        return tl::make_unexpected(r.error());
    }
    return sock;
}

void TcpListener::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace tradebot::net
