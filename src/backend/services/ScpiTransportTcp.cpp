// Raw-socket SCPI transport (SIGLENT "Socket" service, TCP port 5025) plus the
// transport factory. Portable: WinSock on Windows, BSD sockets elsewhere.
#include "backend/services/ScpiTransport.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace backend::services::scpi {

namespace {

void closeSocket(socket_t s) {
#ifdef _WIN32
    closesocket(s);
#else
    ::close(s);
#endif
}

std::string lastSocketError() {
#ifdef _WIN32
    return "winsock error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

constexpr int kDefaultPort = 5025;
// A bench instrument answers a TCP connect in milliseconds; an unplugged or
// mis-addressed one must not stall a readiness poll for the OS connect
// timeout (tens of seconds), so connects are non-blocking with this bound.
constexpr std::chrono::milliseconds kConnectTimeout{2000};

bool setNonBlocking(socket_t s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

// Block until writable (connect completed) or timeout. 1 ok, 0 timeout, -1 error.
int waitWritable(socket_t s, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    fd_set wset, eset;
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    FD_SET(s, &wset);
    FD_SET(s, &eset);
    timeval tv;
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int r = select(0, nullptr, &wset, &eset, &tv);
    if (r == SOCKET_ERROR) return -1;
    if (r == 0) return 0;
    return FD_ISSET(s, &eset) ? -1 : 1;
#else
    pollfd p{};
    p.fd = s;
    p.events = POLLOUT;
    const int r = ::poll(&p, 1, static_cast<int>(timeout.count()));
    if (r < 0) return -1;
    if (r == 0) return 0;
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0 || err != 0) {
        errno = err;
        return -1;
    }
    return 1;
#endif
}

// connect() bounded by kConnectTimeout; false leaves `why` explaining.
bool connectWithTimeout(socket_t s, const sockaddr* addr, int addrLen, std::string& why) {
    if (!setNonBlocking(s, true)) {
        why = "cannot set non-blocking: " + lastSocketError();
        return false;
    }
    const int rc = ::connect(s, addr, addrLen);
    if (rc != 0) {
#ifdef _WIN32
        const bool inProgress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool inProgress = errno == EINPROGRESS;
#endif
        if (!inProgress) {
            why = lastSocketError();
            return false;
        }
        const int w = waitWritable(s, kConnectTimeout);
        if (w == 0) {
            why = "connect timed out after " + std::to_string(kConnectTimeout.count()) + " ms";
            return false;
        }
        if (w < 0) {
            why = lastSocketError();
            return false;
        }
    }
    if (!setNonBlocking(s, false)) {
        why = "cannot restore blocking mode: " + lastSocketError();
        return false;
    }
    return true;
}

// Block until readable or timeout. Returns 1 readable, 0 timeout, -1 error.
int waitReadable(socket_t s, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int r = select(0, &set, nullptr, nullptr, &tv);
    return r == SOCKET_ERROR ? -1 : r;
#else
    pollfd p{};
    p.fd = s;
    p.events = POLLIN;
    const int r = ::poll(&p, 1, static_cast<int>(timeout.count()));
    if (r < 0) return -1;
    if (r == 0) return 0;
    return 1;
#endif
}

#ifdef _WIN32
struct WinsockInit {
    bool ok{false};
    WinsockInit() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockInit() {
        if (ok) WSACleanup();
    }
};
bool ensureWinsock() {
    static WinsockInit init;
    return init.ok;
}
#endif

class TcpScpiTransport final : public IScpiTransport {
public:
    ~TcpScpiTransport() override { close(); }

    bool open(const std::string& resource, std::string* error) override {
        close();
#ifdef _WIN32
        if (!ensureWinsock()) {
            if (error) *error = "WSAStartup failed";
            return false;
        }
#endif
        std::string host = resource;
        std::string port = std::to_string(kDefaultPort);
        // "host:port" — IPv6 literals are not expected on a bench LAN.
        const auto colon = resource.rfind(':');
        if (colon != std::string::npos && resource.find(':') == colon) {
            host = resource.substr(0, colon);
            port = resource.substr(colon + 1);
        }
        if (host.empty()) {
            if (error) *error = "lan resource must be host[:port]";
            return false;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || !result) {
            if (error) *error = "cannot resolve " + host;
            return false;
        }
        std::string why;
        for (addrinfo* ai = result; ai; ai = ai->ai_next) {
            socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == kInvalidSocket) continue;
            if (connectWithTimeout(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen), why)) {
                int one = 1;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                           sizeof(one));
                socket_ = s;
                break;
            }
            closeSocket(s);
        }
        freeaddrinfo(result);
        if (socket_ == kInvalidSocket) {
            if (error) *error = "connect to " + host + ":" + port + " failed: " + why;
            return false;
        }
        description_ = "lan " + host + ":" + port;
        return true;
    }

    void close() override {
        if (socket_ != kInvalidSocket) {
            closeSocket(socket_);
            socket_ = kInvalidSocket;
        }
        pending_.clear();
    }

    bool isOpen() const override { return socket_ != kInvalidSocket; }

    bool write(std::string_view command, std::string* error) override {
        if (!isOpen()) {
            if (error) *error = "not open";
            return false;
        }
        std::string line(command);
        line.push_back('\n');
        size_t sent = 0;
        while (sent < line.size()) {
#ifdef _WIN32
            const int n = ::send(socket_, line.data() + sent, static_cast<int>(line.size() - sent), 0);
            if (n == SOCKET_ERROR) {
#else
            const ssize_t n = ::send(socket_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
#endif
                if (error) *error = "send failed: " + lastSocketError();
                close();
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool readLine(std::string& reply, std::chrono::milliseconds timeout, std::string* error) override {
        reply.clear();
        if (!isOpen()) {
            if (error) *error = "not open";
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto nl = pending_.find('\n');
            if (nl != std::string::npos) {
                reply = pending_.substr(0, nl);
                pending_.erase(0, nl + 1);
                if (!reply.empty() && reply.back() == '\r') reply.pop_back();
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                if (error) *error = "timeout waiting for reply";
                return false;
            }
            const int ready = waitReadable(
                socket_, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            if (ready < 0) {
                if (error) *error = "poll failed: " + lastSocketError();
                close();
                return false;
            }
            if (ready == 0) {
                if (error) *error = "timeout waiting for reply";
                return false;
            }
            char buf[512];
#ifdef _WIN32
            const int n = ::recv(socket_, buf, sizeof(buf), 0);
            if (n == SOCKET_ERROR) {
#else
            const ssize_t n = ::recv(socket_, buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
#endif
                if (error) *error = "recv failed: " + lastSocketError();
                close();
                return false;
            }
            if (n == 0) {
                if (error) *error = "connection closed by instrument";
                close();
                return false;
            }
            pending_.append(buf, static_cast<size_t>(n));
        }
    }

    std::string describe() const override { return description_; }

private:
    socket_t socket_{kInvalidSocket};
    std::string pending_;
    std::string description_{"lan"};
};

} // namespace

std::unique_ptr<IScpiTransport> makeUsbtmcScpiTransport(); // platform file

std::unique_ptr<IScpiTransport> makeScpiTransport(const std::string& kind) {
    if (kind == "lan" || kind == "tcp" || kind == "socket") return std::make_unique<TcpScpiTransport>();
    if (kind == "usb" || kind == "usbtmc" || kind == "visa") return makeUsbtmcScpiTransport();
    SPDLOG_WARN("ScpiTransport: unknown transport kind '{}'", kind);
    return nullptr;
}

} // namespace backend::services::scpi
