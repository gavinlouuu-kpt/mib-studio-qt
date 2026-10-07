// USBTMC SCPI transport on Linux: the kernel usbtmc class driver exposes each
// instrument as /dev/usbtmcN and frames the messages, so the device node is a
// plain line channel. "auto" takes the first node present.
#include "backend/services/ScpiTransport.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace backend::services::scpi {

namespace {

class UsbtmcFileTransport final : public IScpiTransport {
public:
    ~UsbtmcFileTransport() override { close(); }

    bool open(const std::string& resource, std::string* error) override {
        close();
        std::string path = resource;
        if (path.empty() || path == "auto") {
            path.clear();
            for (int i = 0; i < 16; ++i) {
                const std::string candidate = "/dev/usbtmc" + std::to_string(i);
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec)) {
                    path = candidate;
                    break;
                }
            }
            if (path.empty()) {
                if (error) *error = "no /dev/usbtmc* device present";
                return false;
            }
        }
        fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            if (error) *error = "open " + path + " failed: " + std::strerror(errno);
            return false;
        }
        description_ = "usb " + path;
        return true;
    }

    void close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool isOpen() const override { return fd_ >= 0; }

    bool write(std::string_view command, std::string* error) override {
        if (fd_ < 0) {
            if (error) *error = "not open";
            return false;
        }
        std::string line(command);
        line.push_back('\n');
        // USBTMC transfers one message per write(): partial writes mean the
        // driver split the message, which the class driver never does.
        const ssize_t n = ::write(fd_, line.data(), line.size());
        if (n < 0 || static_cast<size_t>(n) != line.size()) {
            if (error) *error = std::string("write failed: ") + std::strerror(errno);
            return false;
        }
        return true;
    }

    bool readLine(std::string& reply, std::chrono::milliseconds timeout, std::string* error) override {
        reply.clear();
        if (fd_ < 0) {
            if (error) *error = "not open";
            return false;
        }
        pollfd p{};
        p.fd = fd_;
        p.events = POLLIN;
        const int ready = ::poll(&p, 1, static_cast<int>(timeout.count()));
        if (ready < 0) {
            if (error) *error = std::string("poll failed: ") + std::strerror(errno);
            return false;
        }
        if (ready == 0) {
            if (error) *error = "timeout waiting for reply";
            return false;
        }
        char buf[4096];
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (error) *error = std::string("read failed: ") + std::strerror(errno);
            return false;
        }
        reply.assign(buf, static_cast<size_t>(n));
        while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
        return true;
    }

    std::string describe() const override { return description_; }

private:
    int fd_{-1};
    std::string description_{"usb"};
};

} // namespace

std::unique_ptr<IScpiTransport> makeUsbtmcScpiTransport() {
    return std::make_unique<UsbtmcFileTransport>();
}

} // namespace backend::services::scpi
