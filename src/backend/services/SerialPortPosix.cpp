// POSIX (termios) implementation of ISerialPort. Compiled on non-Windows
// builds (selected in src/backend/CMakeLists.txt). Qt-free.
#include "backend/services/ISerialPort.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace backend::services {

namespace {

speed_t baudConstant(int baud)
{
    switch (baud) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default:     return B115200;
    }
}

int remainingMs(std::chrono::steady_clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return static_cast<int>(std::max<long long>(0, left.count()));
}

// Best-effort COM-number -> device path for Linux USB-serial adapters (the
// dLSP pumps present as CDC/USB serial). Real deployments run on Windows; this
// keeps the Linux backend functional and is bypassed by the path test seam.
std::string devicePathForComPort(int comPort)
{
    return "/dev/ttyUSB" + std::to_string(comPort);
}

class PosixSerialPort final : public ISerialPort {
public:
    ~PosixSerialPort() override { close(); }

    bool open(int comPort, int baudRate) override
    {
        return openPath(devicePathForComPort(comPort), baudRate);
    }

    bool openPath(const std::string& devicePath, int baudRate)
    {
        SerialSettings s;
        s.baudRate = baudRate;
        return openNamed(devicePath, s);
    }

    bool openNamed(const std::string& systemName, const SerialSettings& settings) override
    {
        const std::string devicePath = systemName.rfind('/', 0) == 0 ? systemName : "/dev/" + systemName;
        const int baudRate = settings.baudRate;
        close();
        fd_ = ::open(devicePath.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) {
            setErrno("open " + devicePath);
            return false;
        }

        termios tty{};
        if (::tcgetattr(fd_, &tty) != 0) {
            setErrno("tcgetattr");
            close();
            return false;
        }

        cfmakeraw(&tty); // no canonical processing, no echo
        // Line settings (8N1 default), no flow control, local + receiver enabled.
        tty.c_cflag &= ~static_cast<tcflag_t>(PARENB | PARODD);
        if (settings.parity == 'E') tty.c_cflag |= PARENB;
        if (settings.parity == 'O') tty.c_cflag |= (PARENB | PARODD);
        tty.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);
        if (settings.stopBits == 2) tty.c_cflag |= CSTOPB;
        tty.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
        tty.c_cflag |= settings.dataBits == 5 ? CS5 : settings.dataBits == 6 ? CS6
                     : settings.dataBits == 7 ? CS7 : CS8;
        tty.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);  // no hardware flow control
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY); // no software flow control
        tty.c_cc[VMIN] = 0;  // non-blocking read; timeouts handled via select()
        tty.c_cc[VTIME] = 0;

        const speed_t speed = baudConstant(baudRate);
        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);

        if (::tcsetattr(fd_, TCSANOW, &tty) != 0) {
            setErrno("tcsetattr");
            close();
            return false;
        }
        ::tcflush(fd_, TCIOFLUSH);
        baudRate_ = baudRate;
        error_.clear();
        return true;
    }

    void close() override
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool isOpen() const override { return fd_ >= 0; }

    int write(const std::vector<uint8_t>& data) override
    {
        if (fd_ < 0) {
            error_ = "write on closed port";
            return -1;
        }
        // The port is non-blocking: a full driver queue answers EAGAIN. Wait
        // for room with poll() up to the write deadline instead of retrying
        // for ever (#720).
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kSerialWriteTimeoutMs);
        size_t total = 0;
        while (total < data.size()) {
            const ssize_t n = ::write(fd_, data.data() + total, data.size() - total);
            if (n > 0) {
                total += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                setErrno("write");
                return -1;
            }
            const int left = remainingMs(deadline);
            if (left <= 0) {
                // Nothing may go out late: a partial frame the caller has
                // already given up on is dropped from the driver queue.
                discardOutput();
                systemError_ = ETIMEDOUT;
                error_ = "serial write timed out after " + std::to_string(kSerialWriteTimeoutMs) +
                         " ms (" + std::to_string(total) + " of " + std::to_string(data.size()) +
                         " bytes taken, discarded)";
                return -1;
            }
            pollfd p{};
            p.fd = fd_;
            p.events = POLLOUT;
            const int rc = ::poll(&p, 1, left);
            if (rc < 0 && errno != EINTR) {
                setErrno("poll while writing");
                return -1;
            }
            if (rc > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                systemError_ = EIO;
                error_ = "serial port error while writing (device gone?)";
                return -1;
            }
        }
        lastWriteBytes_ = total;
        return static_cast<int>(total);
    }

    bool waitForBytesWritten(int timeoutMs) override
    {
        if (fd_ < 0) return false;
        const int budgetMs = std::max(0, timeoutMs);
#if defined(TIOCOUTQ)
        // tcdrain() has no timeout and blocks for ever on an adapter that
        // stops draining (#720). Poll the output queue to a deadline instead,
        // as the Win32 port polls COMSTAT.cbOutQue.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
        for (;;) {
            int queued = 0;
            if (::ioctl(fd_, TIOCOUTQ, &queued) != 0) {
                if (errno == EINTR) continue;
                setErrno("TIOCOUTQ while waiting for serial output");
                return false;
            }
            if (queued <= 0) return true;
            if (std::chrono::steady_clock::now() >= deadline) {
                discardOutput();
                systemError_ = ETIMEDOUT;
                error_ = "serial output timed out: " + std::to_string(queued) +
                         " bytes not drained after " + std::to_string(budgetMs) + " ms (discarded)";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#else
        // No TIOCOUTQ on this platform: the queue cannot be read, and
        // tcdrain() has no bound. Give the last write its nominal line time
        // (10 bits per byte), capped at the timeout, and report the drain as
        // unconfirmed rather than block.
        const long long lineMs =
            static_cast<long long>(lastWriteBytes_) * 10 * 1000 / std::max(1, baudRate_) + 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(lineMs, budgetMs)));
        systemError_ = ENOTSUP;
        error_ = "serial output drain cannot be confirmed on this platform (no TIOCOUTQ)";
        return false;
#endif
    }

    bool waitForReadyRead(int timeoutMs) override
    {
        if (fd_ < 0) return false;
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(fd_, &rset);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        const int rc = ::select(fd_ + 1, &rset, nullptr, nullptr, &tv);
        return rc > 0 && FD_ISSET(fd_, &rset);
    }

    std::vector<uint8_t> readAll() override
    {
        std::vector<uint8_t> out;
        if (fd_ < 0) return out;
        uint8_t buf[512];
        for (;;) {
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n > 0) {
                out.insert(out.end(), buf, buf + n);
                if (static_cast<size_t>(n) < sizeof(buf)) break;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                break; // 0 (EOF for now) or EAGAIN (no more data)
            }
        }
        return out;
    }

    std::string lastError() const override { return error_; }
    int lastSystemError() const override { return systemError_; }

private:
    void setErrno(const std::string& what)
    {
        systemError_ = errno;
        error_ = what + ": " + std::strerror(errno);
    }

    // Drop output the driver has not sent yet (best effort: bytes already in
    // the adapter's own FIFO may still go out).
    void discardOutput() { ::tcflush(fd_, TCOFLUSH); }

    int fd_{-1};
    int baudRate_{9600};
    size_t lastWriteBytes_{0};
    std::string error_;
    int systemError_{0};
};

} // namespace

std::unique_ptr<ISerialPort> makePlatformSerialPort()
{
    return std::make_unique<PosixSerialPort>();
}

std::unique_ptr<ISerialPort> makeSerialPortForPathForTesting(const std::string& devicePath,
                                                             int baudRate)
{
    auto port = std::make_unique<PosixSerialPort>();
    if (!port->openPath(devicePath, baudRate)) {
        SPDLOG_WARN("makeSerialPortForPathForTesting: failed to open {}: {}",
                    devicePath, port->lastError());
        return nullptr;
    }
    return port;
}

namespace {
std::string readSysfs(const std::filesystem::path& p)
{
    std::ifstream f(p);
    std::string s;
    if (f) std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}
} // namespace

// sysfs enumeration: every tty with a device link; USB identity read from the
// nearest ancestor that carries idVendor/idProduct.
std::vector<SerialPortInfo> enumerateSerialPorts()
{
    namespace fs = std::filesystem;
    std::vector<SerialPortInfo> ports;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/tty", ec)) {
        const fs::path device = entry.path() / "device";
        if (!fs::exists(device, ec)) continue;
        SerialPortInfo p;
        p.systemName = entry.path().filename().string();
        p.systemLocation = "/dev/" + p.systemName;
        fs::path node = fs::canonical(device, ec);
        for (int depth = 0; depth < 5 && !node.empty() && ec.value() == 0; ++depth) {
            if (fs::exists(node / "idVendor", ec)) {
                p.vendorId = static_cast<uint16_t>(std::strtoul(readSysfs(node / "idVendor").c_str(), nullptr, 16));
                p.productId = static_cast<uint16_t>(std::strtoul(readSysfs(node / "idProduct").c_str(), nullptr, 16));
                p.manufacturer = readSysfs(node / "manufacturer");
                p.description = readSysfs(node / "product");
                p.serialNumber = readSysfs(node / "serial");
                break;
            }
            node = node.parent_path();
        }
        ports.push_back(std::move(p));
    }
    return ports;
}

} // namespace backend::services
