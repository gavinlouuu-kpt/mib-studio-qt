// serial_port_posix_loopback_test
//
// Exercises the real POSIX termios ISerialPort transport (epic #246) against a
// pseudo-terminal loopback — no hardware. Opens a pty master, points the
// SerialPortPosix impl at the slave via the path test seam, and round-trips
// bytes both directions plus verifies the read-timeout path. POSIX-only; on
// Windows the test is a no-op pass (the Win32 impl has no path seam).

#include "backend/services/ISerialPort.h"

#include "support/assert.h"

#include <cstdint>
#include <vector>

#ifndef _WIN32
#include <cstdlib>   // posix_openpt, grantpt, unlockpt, ptsname
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>

namespace {

// Read up to `want` bytes from a raw fd, waiting up to timeoutMs total.
std::vector<uint8_t> readFd(int fd, size_t want, int timeoutMs)
{
    std::vector<uint8_t> out;
    for (int waited = 0; out.size() < want && waited < timeoutMs; waited += 20) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        timeval tv{0, 20 * 1000};
        if (::select(fd + 1, &r, nullptr, nullptr, &tv) > 0 && FD_ISSET(fd, &r)) {
            uint8_t buf[256];
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) out.insert(out.end(), buf, buf + n);
        }
    }
    return out;
}

// Pull from the ISerialPort until `want` bytes arrive or the budget elapses.
std::vector<uint8_t> readPort(backend::services::ISerialPort& port, size_t want, int budgetMs)
{
    std::vector<uint8_t> out;
    for (int waited = 0; out.size() < want && waited < budgetMs; waited += 50) {
        if (port.waitForReadyRead(50)) {
            const auto chunk = port.readAll();
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
    }
    return out;
}

} // namespace
#endif

int main()
{
#ifndef _WIN32
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    MIB_REQUIRE(master >= 0, "posix_openpt allocates a pty master");
    MIB_REQUIRE(::grantpt(master) == 0 && ::unlockpt(master) == 0, "grantpt/unlockpt succeed");
    const char* slavePath = ::ptsname(master);
    MIB_REQUIRE(slavePath != nullptr, "ptsname returns the slave device path");

    auto port = backend::services::makeSerialPortForPathForTesting(slavePath, 115200);
    MIB_REQUIRE(port != nullptr && port->isOpen(), "SerialPortPosix opens the pty slave");

    // A Modbus-shaped frame (includes 0x00 and control-range bytes; raw termios
    // must pass them through unmodified).
    const std::vector<uint8_t> frame{0x01, 0x03, 0x00, 0x6A, 0x00, 0x02, 0xA5, 0x5A};

    // master -> port
    MIB_REQUIRE(::write(master, frame.data(), frame.size()) == static_cast<ssize_t>(frame.size()),
                "write to pty master");
    const auto got = readPort(*port, frame.size(), 1000);
    MIB_EXPECT(got == frame, "master->port bytes arrive intact through termios");

    // port -> master
    MIB_EXPECT(port->write(frame) == static_cast<int>(frame.size()), "port writes full frame");
    port->waitForBytesWritten(1000);
    const auto back = readFd(master, frame.size(), 1000);
    MIB_EXPECT(back == frame, "port->master bytes arrive intact through termios");

    // Idle read must time out (and not block forever).
    MIB_EXPECT(!port->waitForReadyRead(50), "waitForReadyRead times out when no data");

    port->close();
    MIB_EXPECT(!port->isOpen(), "close() releases the fd");
    ::close(master);

    if (mib::test::exitCode() == 0) {
        std::printf("SerialPortPosix pty loopback verified\n");
    }
    return mib::test::exitCode();
#else
    std::printf("serial_port_posix_loopback_test: skipped on Windows\n");
    return 0;
#endif
}
